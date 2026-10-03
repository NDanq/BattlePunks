#include "Public/GameCore/Prefab.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/WorldPackage.h"
#include "Private/GameCore/PrefabInternal.h"

#if WITH_EDITOR
#include <filesystem>

#include "pugixml.hpp"

#include "Private/GameCore/XmlComponentArchive.h"
#endif

/*
 * The prefab asset, its blobs, capture, both file formats and the registry.
 * Spawning is World's (World.cpp). See Prefab.h and DESIGN.md.
 */

#if WITH_EDITOR
using namespace GameCore::XmlText;
#endif

namespace GameCore
{
namespace
{
    constexpr uint32_t TAG_PREFAB = EngineCore::MakeArchiveTag('P','R','F','B');
    constexpr uint32_t TAG_DATA   = EngineCore::MakeArchiveTag('D','A','T','A');

    constexpr uint16_t PREFAB_CHUNK_VERSION = 1;

    /* An EntityID field in a blob: a local id, 0 for unset. Saving looks the
     * id up in ToLocal (absent: stored unset, counted); loading in ToLive. */
    class BlobRemapper final : public IEntityRemapper
    {
    public:
        const LiveIdMap*  ToLocal = nullptr;
        const LocalIdMap* ToLive  = nullptr;
        uint32_t          Dropped = 0;

        void RemapEntity(EngineCore::Archive& Ar, EntityID& InOut) override
        {
            uint32_t Local = PREFAB_NO_LOCAL_ID;
            if(Ar.IsSaving() && InOut.IsSet())
            {
                const auto Found = ToLocal ? ToLocal->find(InOut.Pack()) : LiveIdMap::const_iterator();
                if(ToLocal && Found != ToLocal->end()) { Local = Found->second; }
                else                                   { ++Dropped; }
            }
            Ar << Local;
            if(!Ar.IsLoading() || Ar.HasError()) { return; }

            InOut = EntityID();
            if(Local != PREFAB_NO_LOCAL_ID && ToLive)
            {
                const auto Found = ToLive->find(Local);
                if(Found != ToLive->end()) { InOut = Found->second; }
            }
        }
    };

    std::string FileStem(const std::string& InPath)
    {
        const size_t Slash = InPath.find_last_of("/\\");
        std::string Name = Slash == std::string::npos ? InPath : InPath.substr(Slash + 1);
        const size_t Dot = Name.find('.');
        return Dot == std::string::npos ? Name : Name.substr(0, Dot);
    }

    /* Root first, then each entity after its parent, siblings keeping their
     * order in InOutEntities. False (with why) on no root, two roots, a
     * missing parent, a duplicate or zero id, or a cycle. */
    bool NormalizeTree(std::vector<PrefabEntityData>& InOutEntities, std::string& OutReason)
    {
        if(InOutEntities.empty())
        {
            OutReason = "it has no entities";
            return false;
        }

        std::unordered_map<uint32_t, size_t> ById;
        size_t Root = SIZE_MAX;
        for(size_t Index = 0; Index < InOutEntities.size(); ++Index)
        {
            const PrefabEntityData& E = InOutEntities[Index];
            if(E.LocalId == PREFAB_NO_LOCAL_ID)
            {
                OutReason = "an entity has id 0";
                return false;
            }
            if(!ById.emplace(E.LocalId, Index).second)
            {
                OutReason = "id " + std::to_string(E.LocalId) + " is used twice";
                return false;
            }
            if(E.ParentId == PREFAB_NO_LOCAL_ID)
            {
                if(Root != SIZE_MAX)
                {
                    OutReason = "it has more than one root";
                    return false;
                }
                Root = Index;
            }
        }
        if(Root == SIZE_MAX)
        {
            OutReason = "it has no root (an entity without a Parent)";
            return false;
        }

        std::unordered_map<uint32_t, std::vector<size_t>> Children;
        for(size_t Index = 0; Index < InOutEntities.size(); ++Index)
        {
            const PrefabEntityData& E = InOutEntities[Index];
            if(E.ParentId == PREFAB_NO_LOCAL_ID) { continue; }
            if(ById.count(E.ParentId) == 0)
            {
                OutReason = "entity " + std::to_string(E.LocalId) + " names parent " + std::to_string(E.ParentId)
                          + ", which is not in it";
                return false;
            }
            Children[E.ParentId].push_back(Index);
        }

        /* Pre-order from the root; whatever it never reaches is in a cycle. */
        std::vector<size_t> Order;
        Order.reserve(InOutEntities.size());
        std::vector<size_t> Stack{ Root };
        while(!Stack.empty())
        {
            const size_t Index = Stack.back();
            Stack.pop_back();
            Order.push_back(Index);
            const auto Found = Children.find(InOutEntities[Index].LocalId);
            if(Found != Children.end())
            {
                for(auto It = Found->second.rbegin(); It != Found->second.rend(); ++It) { Stack.push_back(*It); }
            }
        }
        if(Order.size() != InOutEntities.size())
        {
            OutReason = "some entities are parented in a cycle";
            return false;
        }

        std::vector<PrefabEntityData> Sorted;
        Sorted.reserve(Order.size());
        for(const size_t Index : Order) { Sorted.push_back(std::move(InOutEntities[Index])); }
        InOutEntities = std::move(Sorted);
        return true;
    }

    uint32_t HighestId(const std::vector<PrefabEntityData>& InEntities)
    {
        uint32_t Highest = 0;
        for(const PrefabEntityData& E : InEntities) { Highest = std::max(Highest, E.LocalId); }
        return Highest;
    }

    void SortComponents(std::vector<PrefabComponentData>& InOut)
    {
        std::sort(InOut.begin(), InOut.end(),
                  [](const PrefabComponentData& L, const PrefabComponentData& R) { return L.TypeName < R.TypeName; });
    }

    uint64_t Mix(uint64_t InValue)
    {
        /* SplitMix64's finaliser. */
        InValue += 0x9E3779B97F4A7C15ull;
        InValue = (InValue ^ (InValue >> 30)) * 0xBF58476D1CE4E5B9ull;
        InValue = (InValue ^ (InValue >> 27)) * 0x94D049BB133111EBull;
        return InValue ^ (InValue >> 31);
    }
}

// === Tick modes ==============================================================

const char* PrefabTickModeName(const EPrefabTickMode InMode)
{
    switch(InMode)
    {
        case EPrefabTickMode::Tick:       return "Tick";
        case EPrefabTickMode::EventsOnly: return "EventsOnly";
        case EPrefabTickMode::Off:        return "Off";
    }
    return "Unknown";
}

bool ParsePrefabTickMode(const char* InName, EPrefabTickMode& OutMode)
{
    if(!InName) { return false; }
    for(const EPrefabTickMode Mode : { EPrefabTickMode::Tick, EPrefabTickMode::EventsOnly, EPrefabTickMode::Off })
    {
        if(std::strcmp(InName, PrefabTickModeName(Mode)) == 0)
        {
            OutMode = Mode;
            return true;
        }
    }
    return false;
}

// === PrefabAsset =============================================================

PrefabAsset::PrefabAsset() : D(new Data()) {}

PrefabAsset::PrefabAsset(const PrefabAsset& InOther) : D(InOther.D)
{
    ++D->RefCount;
}

PrefabAsset& PrefabAsset::operator=(const PrefabAsset& InOther)
{
    if(D != InOther.D)
    {
        ++InOther.D->RefCount;
        if(--D->RefCount == 0) { delete D; }
        D = InOther.D;
    }
    return *this;
}

PrefabAsset::~PrefabAsset()
{
    if(--D->RefCount == 0) { delete D; }
}

PrefabAsset::Data& PrefabAccess::Write(PrefabAsset& InAsset)
{
    if(InAsset.D->RefCount > 1)
    {
        PrefabAsset::Data* Copy = new PrefabAsset::Data(*InAsset.D);
        Copy->RefCount = 1;
        --InAsset.D->RefCount;
        InAsset.D = Copy;
    }
    return *InAsset.D;
}

bool        PrefabAsset::IsEmpty() const        { return D->Entities.empty(); }
const char* PrefabAsset::GetName() const        { return D->Name.c_str(); }
void        PrefabAsset::SetName(const char* InName)       { PrefabAccess::Write(*this).Name = InName ? InName : ""; }
const char* PrefabAsset::GetScriptPath() const  { return D->ScriptPath.c_str(); }
void        PrefabAsset::SetScriptPath(const char* InPath) { PrefabAccess::Write(*this).ScriptPath = InPath ? InPath : ""; }
EPrefabTickMode PrefabAsset::GetTickMode() const { return D->TickMode; }
void        PrefabAsset::SetTickMode(const EPrefabTickMode InMode) { PrefabAccess::Write(*this).TickMode = InMode; }

size_t PrefabAsset::GetEntityCount() const { return D->Entities.size(); }

uint32_t PrefabAsset::GetEntityLocalId(const size_t InIndex) const
{
    return InIndex < D->Entities.size() ? D->Entities[InIndex].LocalId : PREFAB_NO_LOCAL_ID;
}

uint32_t PrefabAsset::GetEntityParentId(const size_t InIndex) const
{
    return InIndex < D->Entities.size() ? D->Entities[InIndex].ParentId : PREFAB_NO_LOCAL_ID;
}

const char* PrefabAsset::GetEntityName(const size_t InIndex) const
{
    return InIndex < D->Entities.size() ? D->Entities[InIndex].Name.c_str() : "";
}

size_t PrefabAsset::GetEntityComponentCount(const size_t InIndex) const
{
    return InIndex < D->Entities.size() ? D->Entities[InIndex].Components.size() : 0;
}

const char* PrefabAsset::GetEntityComponentType(const size_t InIndex, const size_t InComponent) const
{
    if(InIndex >= D->Entities.size() || InComponent >= D->Entities[InIndex].Components.size()) { return ""; }
    return D->Entities[InIndex].Components[InComponent].TypeName.c_str();
}

uint32_t PrefabAsset::GetRootLocalId() const
{
    return D->Entities.empty() ? PREFAB_NO_LOCAL_ID : D->Entities.front().LocalId;
}

uint32_t PrefabAsset::FindLocalId(const char* InName) const
{
    if(!InName || InName[0] == '\0') { return PREFAB_NO_LOCAL_ID; }
    for(const PrefabEntityData& E : D->Entities)
    {
        if(E.Name == InName) { return E.LocalId; }
    }
    return PREFAB_NO_LOCAL_ID;
}

size_t PrefabAsset::FindEntityIndex(const uint32_t InLocalId) const
{
    for(size_t Index = 0; Index < D->Entities.size(); ++Index)
    {
        if(D->Entities[Index].LocalId == InLocalId) { return Index; }
    }
    return SIZE_MAX;
}

uint32_t PrefabAsset::GetNextLocalId() const { return D->NextLocalId; }

// === Blobs ===================================================================

bool ComponentDefaults(const ComponentTypeID InType, std::vector<uint8_t>& OutBytes)
{
    const ComponentTypeInfo*       Info       = GetComponentType(InType);
    const ComponentSerializerInfo* Serializer = FindComponentSerializer(InType);
    if(!Info || !Serializer) { return false; }
    OutBytes.assign(Info->SizeBytes, 0);
    if(Serializer->DefaultsFn) { Serializer->DefaultsFn(OutBytes.data()); }
    return true;
}

bool WriteComponentBlob(const ComponentTypeID InType, void* InComponent, const LiveIdMap& InToLocal,
                        std::vector<uint8_t>& OutBlob, uint32_t& OutDropped)
{
    const ComponentSerializerInfo* Serializer = FindComponentSerializer(InType);
    if(!Serializer) { return false; }

    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
    uint16_t Version = Serializer->Version;
    Ar.OpenChunk(TAG_DATA, Version);
    BlobRemapper Remap;
    Remap.ToLocal = &InToLocal;
    BinaryComponentArchive ComponentAr(Ar, Remap);
    Serializer->Fn(ComponentAr, InComponent);
    Ar.CloseChunk();

    OutDropped += Remap.Dropped;
    OutBlob = Ar.GetBytes();
    return !Ar.HasError();
}

bool ReadComponentBlob(const ComponentTypeID InType, const std::vector<uint8_t>& InBlob, const LocalIdMap& InToLive,
                       void* InComponent)
{
    const ComponentSerializerInfo* Serializer = FindComponentSerializer(InType);
    if(!Serializer) { return false; }

    EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBlob);
    uint16_t Version = 0;
    if(!Ar.OpenChunk(TAG_DATA, Version)) { return false; }
    BlobRemapper Remap;
    Remap.ToLive = &InToLive;
    BinaryComponentArchive ComponentAr(Ar, Remap);
    Serializer->Fn(ComponentAr, InComponent);
    Ar.CloseChunk();
    return !Ar.HasError();
}

bool ValidatePrefabTree(const PrefabAsset::Data& InData, std::string& OutReason)
{
    std::vector<PrefabEntityData> Copy = InData.Entities;
    if(!NormalizeTree(Copy, OutReason)) { return false; }
    for(size_t Index = 0; Index < Copy.size(); ++Index)
    {
        if(Copy[Index].LocalId != InData.Entities[Index].LocalId)
        {
            OutReason = "its entities are not in tree order";
            return false;
        }
    }
    return true;
}

EntityGuid DerivePrefabEntityGuid(const EntityGuid& InInstanceGuid, const uint32_t InLocalId)
{
    EntityGuid Result;
    Result.Hi = Mix(InInstanceGuid.Hi ^ Mix(uint64_t(InLocalId)));
    Result.Lo = Mix(InInstanceGuid.Lo ^ Mix(uint64_t(InLocalId) + 0x632BE59BD9B4E019ull));
    if(!Result.IsSet()) { Result.Lo = 1; }
    return Result;
}

// === Capture =================================================================

bool CapturePrefab(World& InWorld, const EntityID InRoot, PrefabAsset& InOutAsset, const bool bInAdoptNew)
{
    if(!InWorld.IsAlive(InRoot))
    {
        return false;
    }

    /* Pre-order: every parent before its children, siblings in order. */
    std::vector<EntityID> Order;
    std::vector<EntityID> Stack{ InRoot };
    while(!Stack.empty())
    {
        const EntityID Current = Stack.back();
        Stack.pop_back();
        Order.push_back(Current);
        std::vector<EntityID> Children;
        for(EntityID Child = InWorld.GetEntity(Current).GetFirstChild(); Child.IsSet(); Child = InWorld.GetEntity(Child).GetNextSibling())
        {
            Children.push_back(Child);
        }
        for(auto It = Children.rbegin(); It != Children.rend(); ++It) { Stack.push_back(*It); }
    }

    /* Ids: an entity spawned from an instance of this same prefab -- the
     * root's instance -- keeps its own; everything else gets a new one. */
    const PrefabInstanceID Instance = InWorld.GetEntity(InRoot).GetInstance();
    PrefabAsset::Data& D = PrefabAccess::Write(InOutAsset);
    uint32_t Next = D.NextLocalId;

    std::unordered_set<uint32_t> Used;
    LiveIdMap ToLocal;
    std::vector<uint32_t> Ids(Order.size(), PREFAB_NO_LOCAL_ID);
    for(size_t Index = 0; Index < Order.size(); ++Index)
    {
        const Entity E = InWorld.GetEntity(Order[Index]);
        const uint32_t Kept = E.GetPrefabLocalId();
        if(Instance.IsSet() && E.GetInstance() == Instance && Kept != PREFAB_NO_LOCAL_ID && Used.insert(Kept).second)
        {
            Ids[Index] = Kept;
            Next = std::max(Next, Kept + 1);
        }
    }
    for(size_t Index = 0; Index < Order.size(); ++Index)
    {
        if(Ids[Index] == PREFAB_NO_LOCAL_ID)
        {
            while(Used.count(Next) != 0) { ++Next; }
            Ids[Index] = Next++;
            Used.insert(Ids[Index]);
        }
        ToLocal[Order[Index].Pack()] = Ids[Index];
    }

    const ComponentTypeID TransformType = TransformComponent::StaticType().ID;
    std::vector<PrefabEntityData> Entities;
    uint32_t Dropped = 0;
    std::vector<ComponentTypeID> WarnedTypes;
    for(size_t Index = 0; Index < Order.size(); ++Index)
    {
        const Entity E = InWorld.GetEntity(Order[Index]);
        PrefabEntityData Data;
        Data.LocalId  = Ids[Index];
        Data.ParentId = Index == 0 ? PREFAB_NO_LOCAL_ID : ToLocal[E.GetParent().Pack()];
        Data.Name     = E.GetName();

        for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
        {
            void* Component = E.GetComponentByType(Type);
            if(!Component) { continue; }
            const ComponentTypeInfo* Info = GetComponentType(Type);
            if(!Info) { continue; }
            if(!FindComponentSerializer(Type))
            {
                if(std::find(WarnedTypes.begin(), WarnedTypes.end(), Type) == WarnedTypes.end())
                {
                    WarnedTypes.push_back(Type);
                    GAMECORE_LOG_WARN(std::string("CapturePrefab: no serializer registered for '") + Info->Name
                                      + "'; components of that type are left out of the prefab")
                }
                continue;
            }

            PrefabComponentData Captured;
            Captured.TypeName = Info->Name;
            if(Index == 0 && Type == TransformType)
            {
                /* The root stands where its instance is placed: identity here. */
                TransformComponent Placed = *static_cast<const TransformComponent*>(Component);
                Placed.SetPose(TransformPose());
                WriteComponentBlob(Type, &Placed, ToLocal, Captured.Blob, Dropped);
            }
            else
            {
                WriteComponentBlob(Type, Component, ToLocal, Captured.Blob, Dropped);
            }
            Data.Components.push_back(std::move(Captured));
        }
        SortComponents(Data.Components);
        Entities.push_back(std::move(Data));
    }

    if(Dropped > 0)
    {
        GAMECORE_LOG_WARN("CapturePrefab: " + std::to_string(Dropped) + " entity reference(s) point outside '"
                          + InWorld.GetEntity(InRoot).GetName() + "' and its children; they are stored unset")
    }

    D.Entities    = std::move(Entities);
    D.NextLocalId = std::max(Next, HighestId(D.Entities) + 1);

    if(bInAdoptNew && Instance.IsSet())
    {
        for(size_t Index = 1; Index < Order.size(); ++Index)
        {
            if(InWorld.GetEntity(Order[Index]).GetInstance() != Instance)
            {
                InWorld.AdoptIntoInstance(Instance, Order[Index], Ids[Index]);
            }
        }
    }
    return true;
}

// === The cooked form =========================================================

bool SerializePrefab(EngineCore::Archive& Ar, PrefabAsset& InOutAsset)
{
    if(Ar.IsSaving())
    {
        const PrefabAsset::Data& D = PrefabAccess::Read(InOutAsset);
        uint16_t Version = PREFAB_CHUNK_VERSION;
        Ar.OpenChunk(TAG_PREFAB, Version);

        std::string Name   = D.Name;
        std::string Script = D.ScriptPath;
        uint8_t     Mode   = uint8_t(D.TickMode);
        uint32_t    NextId = D.NextLocalId;
        uint32_t    Count  = uint32_t(D.Entities.size());
        Ar << Name << Script << Mode << NextId << Count;

        for(const PrefabEntityData& E : D.Entities)
        {
            uint32_t    LocalId  = E.LocalId;
            uint32_t    ParentId = E.ParentId;
            std::string EntityName = E.Name;
            uint32_t    ComponentCount = uint32_t(E.Components.size());
            Ar << LocalId << ParentId << EntityName << ComponentCount;
            for(const PrefabComponentData& C : E.Components)
            {
                std::string TypeName = C.TypeName;
                uint32_t    Size     = uint32_t(C.Blob.size());
                Ar << TypeName << Size;
                std::vector<uint8_t> Bytes = C.Blob;
                if(Size > 0) { Ar.SerializeBytes(Bytes.data(), Size); }
            }
        }
        Ar.CloseChunk();
        return !Ar.HasError();
    }

    uint16_t Version = 0;
    if(!Ar.OpenChunk(TAG_PREFAB, Version))
    {
        Ar.SetError("expected a PRFB chunk");
        return false;
    }
    if(Version > PREFAB_CHUNK_VERSION)
    {
        GAMECORE_LOG_WARN("SerializePrefab: the prefab chunk is version " + std::to_string(Version)
                          + "; this build understands " + std::to_string(PREFAB_CHUNK_VERSION))
        Ar.SetError("prefab chunk is newer than this build can read");
        Ar.CloseChunk();
        return false;
    }

    PrefabAsset::Data Loaded;
    uint8_t  Mode  = 0;
    uint32_t Count = 0;
    Ar << Loaded.Name << Loaded.ScriptPath << Mode << Loaded.NextLocalId << Count;
    if(Ar.HasError() || Mode > uint8_t(EPrefabTickMode::Off))
    {
        Ar.SetError("unreadable prefab header");
        Ar.CloseChunk();
        return false;
    }
    Loaded.TickMode = EPrefabTickMode(Mode);

    for(uint32_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
    {
        PrefabEntityData E;
        uint32_t ComponentCount = 0;
        Ar << E.LocalId << E.ParentId << E.Name << ComponentCount;
        for(uint32_t Component = 0; Component < ComponentCount && !Ar.HasError(); ++Component)
        {
            PrefabComponentData C;
            uint32_t Size = 0;
            Ar << C.TypeName << Size;
            if(Ar.HasError() || Size > Ar.GetSize() - Ar.GetCursor())
            {
                Ar.SetError("a component blob runs past the end of the prefab");
                break;
            }
            C.Blob.resize(Size);
            if(Size > 0) { Ar.SerializeBytes(C.Blob.data(), Size); }
            E.Components.push_back(std::move(C));
        }
        Loaded.Entities.push_back(std::move(E));
    }
    Ar.CloseChunk();
    if(Ar.HasError()) { return false; }

    std::string Reason;
    if(!NormalizeTree(Loaded.Entities, Reason))
    {
        GAMECORE_LOG_ERROR("SerializePrefab: the prefab is not a valid tree: " + Reason)
        Ar.SetError("not a valid prefab tree");
        return false;
    }
    Loaded.NextLocalId = std::max(Loaded.NextLocalId, HighestId(Loaded.Entities) + 1);

    PrefabAsset::Data& D = PrefabAccess::Write(InOutAsset);
    Loaded.RefCount = D.RefCount;
    D = std::move(Loaded);
    return true;
}

// === The text form ===========================================================

#if WITH_EDITOR
namespace
{
    constexpr int TEXT_FORMAT = 1;

    /* A prefab names entities by local id. While converting, an EntityID
     * field holds a stand-in, EntityID(localId, 0): it never meets a World. */
    class PrefabRefs final : public XmlEntityRefs
    {
    public:
        explicit PrefabRefs(const std::unordered_set<uint32_t>& InIds) : Ids(InIds) {}

        const char* GetAttributeName() const override { return "Entity"; }
        std::string Write(const EntityID InEntity) const override
        {
            return InEntity.IsSet() ? std::to_string(InEntity.Index) : std::string();
        }
        bool Read(const char* InText, EntityID& OutEntity) const override
        {
            uint32_t Id = 0;
            if(!ParseNumber(InText, Id) || Ids.count(Id) == 0) { return false; }
            OutEntity = EntityID(Id, 0);
            return true;
        }
        const char* GetScopeName() const override { return "this prefab"; }

    private:
        const std::unordered_set<uint32_t>& Ids;
    };

    struct StandIns
    {
        std::unordered_set<uint32_t> Ids;
        LiveIdMap  ToLocal;   // stand-in -> id
        LocalIdMap ToLive;    // id -> stand-in

        explicit StandIns(const std::vector<PrefabEntityData>& InEntities)
        {
            for(const PrefabEntityData& E : InEntities)
            {
                Ids.insert(E.LocalId);
                ToLocal[EntityID(E.LocalId, 0).Pack()] = E.LocalId;
                ToLive[E.LocalId] = EntityID(E.LocalId, 0);
            }
        }
    };

    bool RenameReplacing(const std::string& InFrom, const std::string& InTo, std::string& OutError)
    {
        std::error_code Error;
        std::filesystem::rename(std::filesystem::u8path(InFrom), std::filesystem::u8path(InTo), Error);
        if(Error)
        {
            OutError = Error.message();
            return false;
        }
        return true;
    }

    std::string ParentDirectory(const std::string& InPath)
    {
        const size_t Slash = InPath.find_last_of("/\\");
        return Slash == std::string::npos ? std::string() : InPath.substr(0, Slash);
    }

    /* Writes InText to "<path>.tmp", then over InPath. */
    bool WriteReplacing(const std::string& InPath, const std::string& InText, const char* InWhat)
    {
        const std::string Parent = ParentDirectory(InPath);
        if(!Parent.empty() && !EngineCore::appCreateDirectories(Parent))
        {
            GAMECORE_LOG_ERROR(std::string(InWhat) + ": could not create '" + Parent + "'")
            return false;
        }
        const std::string Temp = InPath + ".tmp";
        std::string Error;
        if(!WriteWholeFile(Temp, InText) || !RenameReplacing(Temp, InPath, Error))
        {
            EngineCore::appFileDelete(Temp);
            GAMECORE_LOG_ERROR(std::string(InWhat) + ": could not write '" + InPath + "'" + (Error.empty() ? "" : " (" + Error + ")"))
            return false;
        }
        return true;
    }

    std::string BuildPrefabText(const PrefabAsset::Data& InData)
    {
        const StandIns Stand(InData.Entities);
        const PrefabRefs Refs(Stand.Ids);

        pugi::xml_document Doc;
        AppendDeclaration(Doc);
        pugi::xml_node Root = Doc.append_child("Prefab");
        Root.append_attribute("Format") = TEXT_FORMAT;
        Root.append_attribute("Name")   = InData.Name.c_str();
        Root.append_attribute("NextId") = unsigned(InData.NextLocalId);
        if(!InData.ScriptPath.empty())
        {
            Root.append_child("Script").append_attribute("Path") = InData.ScriptPath.c_str();
        }
        Root.append_child("Tick").append_attribute("Mode") = PrefabTickModeName(InData.TickMode);

        for(const PrefabEntityData& E : InData.Entities)
        {
            pugi::xml_node Element = Root.append_child("Entity");
            Element.append_attribute("Id")   = unsigned(E.LocalId);
            Element.append_attribute("Name") = E.Name.c_str();
            if(E.ParentId != PREFAB_NO_LOCAL_ID)
            {
                Element.append_attribute("Parent") = unsigned(E.ParentId);
            }

            for(const PrefabComponentData& C : E.Components)
            {
                const ComponentTypeInfo*       Info       = FindComponentType(C.TypeName.c_str());
                const ComponentSerializerInfo* Serializer = Info ? FindComponentSerializer(Info->ID) : nullptr;
                std::vector<uint8_t> Bytes;
                if(!Info || !Serializer || !ComponentDefaults(Info->ID, Bytes) || !ReadComponentBlob(Info->ID, C.Blob, Stand.ToLive, Bytes.data()))
                {
                    GAMECORE_LOG_WARN("SavePrefabFile: entity " + std::to_string(E.LocalId) + ": '" + C.TypeName
                                      + "' cannot be written as text by this build (" + (Info ? "no serializer" : "type not registered")
                                      + "); it is left out")
                    continue;
                }
                pugi::xml_node Component = Element.append_child("Component");
                Component.append_attribute("Type")    = C.TypeName.c_str();
                Component.append_attribute("Version") = unsigned(Serializer->Version);
                XmlComponentArchive Ar(Component, false, Serializer->Version, Refs,
                                       "entity " + std::to_string(E.LocalId) + " " + C.TypeName, "Prefab");
                Serializer->Fn(Ar, Bytes.data());
            }
        }
        return DocumentText(Doc);
    }

    bool LoadTextPrefab(const std::string& InPath, PrefabAsset::Data& OutData)
    {
        std::string Text;
        if(!ReadWholeFile(InPath, Text))
        {
            GAMECORE_LOG_ERROR("Prefab: could not read '" + InPath + "'")
            return false;
        }
        pugi::xml_document Doc;
        if(!ParseDocument(Text, Doc, InPath, "Prefab"))
        {
            return false;
        }
        const pugi::xml_node Root = Doc.child("Prefab");
        if(!Root)
        {
            GAMECORE_LOG_ERROR("Prefab: '" + InPath + "' has no <Prefab> element")
            return false;
        }
        const int Format = Root.attribute("Format").as_int(0);
        if(Format != TEXT_FORMAT)
        {
            GAMECORE_LOG_ERROR("Prefab: '" + InPath + "' is text format " + std::to_string(Format)
                               + "; this build reads format " + std::to_string(TEXT_FORMAT))
            return false;
        }

        OutData.Name        = Root.attribute("Name").as_string();
        OutData.NextLocalId = Root.attribute("NextId").as_uint(1);
        OutData.ScriptPath  = Root.child("Script").attribute("Path").as_string();
        if(const pugi::xml_node Tick = Root.child("Tick"))
        {
            if(!ParsePrefabTickMode(Tick.attribute("Mode").as_string(), OutData.TickMode))
            {
                GAMECORE_LOG_WARN("Prefab: '" + InPath + "' has an unknown tick mode '" + Tick.attribute("Mode").as_string()
                                  + "'; using Tick")
                OutData.TickMode = EPrefabTickMode::Tick;
            }
        }

        /* The tree first, so references can be checked against every id. */
        std::vector<std::pair<PrefabEntityData, pugi::xml_node>> Read;
        for(const pugi::xml_node Item : Root.children("Entity"))
        {
            PrefabEntityData E;
            E.LocalId  = Item.attribute("Id").as_uint(PREFAB_NO_LOCAL_ID);
            E.ParentId = Item.attribute("Parent").as_uint(PREFAB_NO_LOCAL_ID);
            E.Name     = Item.attribute("Name").as_string();
            Read.emplace_back(std::move(E), Item);
        }
        std::vector<PrefabEntityData> Tree;
        for(const auto& Each : Read) { Tree.push_back(Each.first); }
        const StandIns Stand(Tree);
        const PrefabRefs Refs(Stand.Ids);

        for(auto& [E, Item] : Read)
        {
            for(const pugi::xml_node Element : Item.children("Component"))
            {
                const char* TypeName = Element.attribute("Type").as_string();
                const ComponentTypeInfo*       Info       = FindComponentType(TypeName);
                const ComponentSerializerInfo* Serializer = Info ? FindComponentSerializer(Info->ID) : nullptr;
                std::vector<uint8_t> Bytes;
                if(!Info || !Serializer || !ComponentDefaults(Info->ID, Bytes))
                {
                    GAMECORE_LOG_WARN("Prefab: '" + InPath + "' entity " + std::to_string(E.LocalId) + ": skipping component '"
                                      + TypeName + "' -- " + (Info ? "no serializer registered" : "type not registered in this build"))
                    continue;
                }
                const uint16_t Version = uint16_t(Element.attribute("Version").as_uint(Serializer->Version));
                XmlComponentArchive Ar(Element, true, Version, Refs,
                                       "entity " + std::to_string(E.LocalId) + " " + TypeName, "Prefab");
                Serializer->Fn(Ar, Bytes.data());

                PrefabComponentData C;
                C.TypeName = Info->Name;
                uint32_t Dropped = 0;
                WriteComponentBlob(Info->ID, Bytes.data(), Stand.ToLocal, C.Blob, Dropped);
                E.Components.push_back(std::move(C));
            }
            SortComponents(E.Components);
            OutData.Entities.push_back(std::move(E));
        }

        std::string Reason;
        if(!NormalizeTree(OutData.Entities, Reason))
        {
            GAMECORE_LOG_ERROR("Prefab: '" + InPath + "' is not a valid prefab: " + Reason)
            return false;
        }
        OutData.NextLocalId = std::max(OutData.NextLocalId, HighestId(OutData.Entities) + 1);
        return true;
    }
}
#endif // WITH_EDITOR

// === Files ===================================================================

bool LoadPrefabFile(const char* InPath, PrefabAsset& OutAsset)
{
    if(!InPath || InPath[0] == '\0') { return false; }

    switch(DetectWorldFileFormat(InPath))
    {
        case EWorldFileFormat::Binary:
        {
            EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
            if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
            {
                GAMECORE_LOG_ERROR(std::string("LoadPrefabFile: could not read '") + InPath + "'")
                return false;
            }
            PrefabAsset Loaded;
            if(!SerializePrefab(Ar, Loaded) || Ar.HasError())
            {
                GAMECORE_LOG_ERROR(std::string("LoadPrefabFile: '") + InPath + "' is not a readable prefab")
                return false;
            }
            if(Loaded.GetName()[0] == '\0') { Loaded.SetName(FileStem(InPath).c_str()); }
            OutAsset = Loaded;
            return true;
        }

        case EWorldFileFormat::Text:
        {
#if WITH_EDITOR
            PrefabAsset Loaded;
            if(!LoadTextPrefab(InPath, PrefabAccess::Write(Loaded)))
            {
                return false;
            }
            if(Loaded.GetName()[0] == '\0') { Loaded.SetName(FileStem(InPath).c_str()); }
            OutAsset = Loaded;
            return true;
#else
            GAMECORE_LOG_ERROR(std::string("LoadPrefabFile: '") + InPath
                               + "' is an editor (text) prefab; a game build loads cooked prefabs -- cook it first")
            return false;
#endif
        }

        case EWorldFileFormat::Unknown:
            break;
    }
    GAMECORE_LOG_ERROR(std::string("LoadPrefabFile: could not read '") + InPath + "'")
    return false;
}

#if WITH_EDITOR
bool SavePrefabFile(const char* InPath, const PrefabAsset& InAsset)
{
    if(!InPath || InPath[0] == '\0') { return false; }
    std::string Reason;
    if(!ValidatePrefabTree(PrefabAccess::Read(InAsset), Reason))
    {
        GAMECORE_LOG_ERROR(std::string("SavePrefabFile: refusing to save '") + InPath + "': " + Reason)
        return false;
    }
    const std::string Text = BuildPrefabText(PrefabAccess::Read(InAsset));
    std::string Existing;
    if(ReadWholeFile(InPath, Existing) && Existing == Text)
    {
        return true;   // unchanged: no write, no new timestamp
    }
    return WriteReplacing(InPath, Text, "SavePrefabFile");
}

bool CookPrefabFile(const char* InSourcePath, const char* InCookedPath)
{
    if(!InSourcePath || !InCookedPath) { return false; }

    PrefabAsset Asset;
    if(!LoadPrefabFile(InSourcePath, Asset))
    {
        GAMECORE_LOG_ERROR(std::string("CookPrefabFile: could not load '") + InSourcePath + "'")
        return false;
    }
    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
    if(!SerializePrefab(Ar, Asset) || Ar.HasError())
    {
        GAMECORE_LOG_ERROR(std::string("CookPrefabFile: could not serialize '") + InSourcePath + "'")
        return false;
    }
    const std::vector<uint8_t>& Bytes = Ar.GetBytes();
    if(!WriteReplacing(InCookedPath, std::string(reinterpret_cast<const char*>(Bytes.data()), Bytes.size()), "CookPrefabFile"))
    {
        return false;
    }
    GAMECORE_LOG(std::string("Prefab: cooked '") + InSourcePath + "' -> '" + InCookedPath + "' ("
                 + std::to_string(Bytes.size()) + " bytes, " + std::to_string(Asset.GetEntityCount()) + " entities)")
    return true;
}
#endif // WITH_EDITOR

// === The registry ============================================================

namespace
{
    /* Node-based, so a value's address survives other paths loading. */
    std::unordered_map<std::string, PrefabAsset>& Registry()
    {
        static std::unordered_map<std::string, PrefabAsset> Cache;
        return Cache;
    }
}

const PrefabAsset* FindOrLoadPrefab(const char* InPath)
{
    if(!InPath || InPath[0] == '\0') { return nullptr; }
    auto& Cache = Registry();
    const auto Found = Cache.find(InPath);
    if(Found != Cache.end()) { return &Found->second; }

    PrefabAsset Loaded;
    if(!LoadPrefabFile(InPath, Loaded))
    {
        return nullptr;
    }
    return &Cache.emplace(InPath, Loaded).first->second;
}

bool ReloadPrefab(const char* InPath)
{
    if(!InPath || InPath[0] == '\0') { return false; }
    PrefabAsset Loaded;
    if(!LoadPrefabFile(InPath, Loaded))
    {
        return false;
    }
    auto& Cache = Registry();
    const auto Found = Cache.find(InPath);
    if(Found == Cache.end()) { Cache.emplace(InPath, Loaded); }
    else                     { Found->second = Loaded; }
    return true;
}

void ClearPrefabCache()
{
    Registry().clear();
}
}

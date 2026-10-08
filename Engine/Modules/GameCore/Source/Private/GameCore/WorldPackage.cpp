#include "Public/GameCore/WorldPackage.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldSerializer.h"
#include "Public/Core/CoreConfig.h"

#if WITH_UNCOOKED && !WITH_EDITOR
#include <filesystem>

/* A Client or Server build cooks a text world or prefab when it first loads
 * it (WorldPackage.h, ResolveContentWorld); off, it reads the text itself. */
NIMBLE_CONFIG_BOOL(GContentCookOnLoad, "Content", "CookOnLoad", true, nullptr);
#endif

#if WITH_UNCOOKED
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "pugixml.hpp"

#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/Prefab.h"
#include "Private/GameCore/PrefabInternal.h"
#include "Private/GameCore/XmlComponentArchive.h"
#endif

#if WITH_UNCOOKED
using namespace GameCore::XmlText;
#endif

/* Format detection and binary load in every build; the text package (save,
 * load, cook) in every build but Shipping (WITH_UNCOOKED). See WorldPackage.h. */

namespace GameCore
{
namespace
{
    bool LoadBinaryWorld(const std::string& InPath, World& InOutWorld)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        return SerializeWorld(Ar, InOutWorld) && !Ar.HasError();
    }

#if WITH_UNCOOKED
    /* The one text format: loose entities and prefab instances. */
    constexpr int         TEXT_FORMAT        = 1;
    constexpr const char* ENTITY_EXTENSION   = ".nmentity";
    constexpr const char* INSTANCE_EXTENSION = ".nmprefabinst";
    constexpr const char* TEMP_SUFFIX      = ".tmp";

    using GuidMap = std::unordered_map<EntityGuid, EntityID, EntityGuidHash>;
    using GuidSet = std::unordered_set<EntityGuid, EntityGuidHash>;

    /* A world names entities by GUID. Saving: the target's, in InWorld. */
    class WorldSavingRefs final : public XmlEntityRefs
    {
    public:
        explicit WorldSavingRefs(World& InWorld) : W(InWorld) {}
        const char* GetAttributeName() const override { return "Guid"; }
        std::string Write(const EntityID InEntity) const override
        {
            const EntityGuid Target = ::GameCore::Entity(&W, InEntity).GetGuid();
            return Target.IsSet() ? GuidText(Target) : std::string();
        }
        bool Read(const char*, EntityID&) const override { return false; }
        const char* GetScopeName() const override { return "this world"; }
    private:
        World& W;
    };

    /* Loading: only against the entities this load created. */
    class WorldLoadingRefs final : public XmlEntityRefs
    {
    public:
        explicit WorldLoadingRefs(const GuidMap& InLoaded) : Loaded(InLoaded) {}
        const char* GetAttributeName() const override { return "Guid"; }
        std::string Write(EntityID) const override { return std::string(); }
        bool Read(const char* InText, EntityID& OutEntity) const override
        {
            EntityGuid Target;
            const auto Found = EntityGuid::Parse(InText, Target) ? Loaded.find(Target) : Loaded.end();
            if(Found == Loaded.end()) { return false; }
            OutEntity = Found->second;
            return true;
        }
        const char* GetScopeName() const override { return "this world"; }
    private:
        const GuidMap& Loaded;
    };

    // === Paths and files ====================================================
    // Core's appFile*; std::filesystem only for the replacing rename.

    bool EndsWith(const std::string& InText, const char* InSuffix)
    {
        const size_t Length = std::strlen(InSuffix);
        return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
    }

    /* "Levels/Sandbox.nmworld" -> "Levels/Sandbox". */
    std::string EntityFolderFor(const std::string& InWorldPath)
    {
        const size_t Slash = InWorldPath.find_last_of("/\\");
        const size_t Dot   = InWorldPath.find_last_of('.');
        if(Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash))
        {
            return InWorldPath + "_Entities";   // no extension to strip; never the world file's own name
        }
        return InWorldPath.substr(0, Dot);
    }

    std::string EntityFileFor(const std::string& InFolder, const EntityGuid& InGuid)
    {
        char Text[EntityGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return InFolder + "/" + Text + ENTITY_EXTENSION;
    }

    std::string InstanceFileFor(const std::string& InFolder, const EntityGuid& InGuid)
    {
        char Text[EntityGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return InFolder + "/" + Text + INSTANCE_EXTENSION;
    }


    /* Replaces InTo if it exists (MoveFileEx with REPLACE_EXISTING on
     * Windows, rename(2) elsewhere). */
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


    // === Saving =============================================================

    struct CollectContext
    {
        std::vector<EntityID>* Entities;
    };

    /* <Entity Guid Name> <Component>*, components sorted by type
     * NAME (ids are assignment order and differ run to run). */
    std::string BuildEntityText(World& InWorld, const EntityID InEntity, std::vector<ComponentTypeID>& InOutWarnedTypes)
    {
        const ::GameCore::Entity Handle = InWorld.GetEntity(InEntity);
        const std::string GuidString = GuidText(Handle.GetGuid());

        pugi::xml_document Doc;
        AppendDeclaration(Doc);
        pugi::xml_node Root = Doc.append_child("Entity");
        Root.append_attribute("Guid") = GuidString.c_str();
        Root.append_attribute("Name") = Handle.GetName();
        /* Only when not dynamic, so a dynamic entity's file reads as it always has. */
        if(Handle.GetMobility() != EMobility::Dynamic)
        {
            Root.append_attribute("Mobility") = MobilityName(Handle.GetMobility());
        }
        /* Only when set, so a root entity's file reads as it always has. */
        if(const EntityID Parent = Handle.GetParent(); Parent.IsSet())
        {
            Root.append_attribute("Parent") = GuidText(InWorld.GetEntity(Parent).GetGuid()).c_str();
        }

        struct Present
        {
            const char*                    Name;
            ComponentTypeID                Type;
            const ComponentSerializerInfo* Serializer;
        };
        std::vector<Present> Components;
        for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
        {
            const ComponentPool* Pool = InWorld.FindPool(Type);
            if(!Pool || !Pool->Has(InEntity)) { continue; }

            const ComponentTypeInfo*       Info       = GetComponentType(Type);
            const ComponentSerializerInfo* Serializer = FindComponentSerializer(Type);
            if(!Info) { continue; }
            if(!Serializer)
            {
                /* Skipped, not fatal -- once per type per save. */
                if(std::find(InOutWarnedTypes.begin(), InOutWarnedTypes.end(), Type) == InOutWarnedTypes.end())
                {
                    InOutWarnedTypes.push_back(Type);
                    GAMECORE_LOG_WARN(std::string("WorldPackage: no serializer registered for '") + Info->Name
                                      + "'; components of that type are not saved")
                }
                continue;
            }
            Components.push_back({ Info->Name, Type, Serializer });
        }
        std::sort(Components.begin(), Components.end(),
                  [](const Present& L, const Present& R) { return std::strcmp(L.Name, R.Name) < 0; });

        for(const Present& Component : Components)
        {
            pugi::xml_node Element = Root.append_child("Component");
            Element.append_attribute("Type")    = Component.Name;
            Element.append_attribute("Version") = unsigned(Component.Serializer->Version);

            const WorldSavingRefs Refs(InWorld);
            XmlComponentArchive Ar(Element, false, Component.Serializer->Version, Refs,
                                   GuidString + " " + Component.Name, "WorldPackage");
            Component.Serializer->Fn(Ar, Handle.GetComponentByType(Component.Type));
        }

        return DocumentText(Doc);
    }

    // === Prefab instances ===================================================
    // <Name>/<instance-guid>.nmprefabinst: the prefab's path and only what this
    // instance changed. See DESIGN.md's WorldPackage notes.

    /* Same name, same attributes in the same order, same children. */
    bool SameElement(const pugi::xml_node InA, const pugi::xml_node InB)
    {
        if(std::strcmp(InA.name(), InB.name()) != 0) { return false; }
        pugi::xml_attribute A = InA.first_attribute(), B = InB.first_attribute();
        for(; A && B; A = A.next_attribute(), B = B.next_attribute())
        {
            if(std::strcmp(A.name(), B.name()) != 0 || std::strcmp(A.value(), B.value()) != 0) { return false; }
        }
        if(A || B) { return false; }
        pugi::xml_node ChildA = InA.first_child(), ChildB = InB.first_child();
        for(; ChildA && ChildB; ChildA = ChildA.next_sibling(), ChildB = ChildB.next_sibling())
        {
            if(!SameElement(ChildA, ChildB)) { return false; }
        }
        return !ChildA && !ChildB;
    }

    /* An instance is saved as one if it came from a prefab file; its
     * entities are those whose template that prefab still has. */
    bool IsSavedAsInstance(World& InWorld, const PrefabInstanceID InInstance)
    {
        return InWorld.IsInstanceAlive(InInstance) && PrefabInstance(&InWorld, InInstance).GetPrefabPath()[0] != '\0';
    }

    bool IsSavedInInstance(World& InWorld, const EntityID InEntity)
    {
        const Entity Handle = InWorld.GetEntity(InEntity);
        const PrefabInstanceID Instance = Handle.GetInstance();
        if(!IsSavedAsInstance(InWorld, Instance)) { return false; }
        const PrefabAsset* Prefab = PrefabInstance(&InWorld, Instance).GetPrefab();
        return Prefab && Prefab->FindEntityIndex(Handle.GetPrefabLocalId()) != SIZE_MAX;
    }

    /*
     * <Instance Guid Name Prefab [TickMode] [Parent]>
     *   <Entity Id [Name]> <Component Type Version>changed fields</Component>
     *                      <RemovedComponent Type/> </Entity>
     *   <Removed Entity/>
     * </Instance>
     *
     * Each component is written twice into a scratch document -- as the
     * prefab has it (references mapped to this instance's entities) and as
     * it is -- and only the field elements that differ are kept. The root's
     * placement is just its Transform differing from the prefab's identity.
     */
    std::string BuildInstanceText(World& InWorld, const PrefabInstanceID InInstance, std::vector<ComponentTypeID>& InOutWarnedTypes)
    {
        const PrefabInstance Instance(&InWorld, InInstance);
        const PrefabAsset* Prefab = Instance.GetPrefab();
        const PrefabAsset::Data& Data = PrefabAccess::Read(*Prefab);
        const std::string GuidString = GuidText(Instance.GetGuid());
        const WorldSavingRefs Refs(InWorld);

        pugi::xml_document Doc;
        AppendDeclaration(Doc);
        pugi::xml_node Root = Doc.append_child("Instance");
        Root.append_attribute("Guid")   = GuidString.c_str();
        Root.append_attribute("Name")   = Instance.GetName();
        Root.append_attribute("Prefab") = Instance.GetPrefabPath();
        if(Instance.GetTickMode() != Data.TickMode)
        {
            Root.append_attribute("TickMode") = PrefabTickModeName(Instance.GetTickMode());
        }
        if(const EntityID Parent = Instance.GetRoot().GetParent(); Parent.IsSet())
        {
            Root.append_attribute("Parent") = GuidText(InWorld.GetEntity(Parent).GetGuid()).c_str();
        }

        LocalIdMap ToLive;
        for(const PrefabEntityData& Template : Data.Entities)
        {
            ToLive[Template.LocalId] = Instance.FindEntity(Template.LocalId).GetID();
        }

        pugi::xml_document Scratch;
        for(const PrefabEntityData& Template : Data.Entities)
        {
            const Entity Member = InWorld.GetEntity(ToLive[Template.LocalId]);
            if(!Member.IsAlive())
            {
                Root.append_child("Removed").append_attribute("Entity") = unsigned(Template.LocalId);
                continue;
            }

            pugi::xml_node Element = Root.append_child("Entity");
            Element.append_attribute("Id") = unsigned(Template.LocalId);
            if(Template.Name != Member.GetName())
            {
                Element.append_attribute("Name") = Member.GetName();
            }
            if(Template.Mobility != Member.GetMobility())
            {
                Element.append_attribute("Mobility") = MobilityName(Member.GetMobility());
            }

            /* Every type either side has, by name. */
            std::vector<std::string> Names;
            for(const PrefabComponentData& Component : Template.Components)
            {
                if(FindComponentType(Component.TypeName.c_str())) { Names.push_back(Component.TypeName); }
            }
            for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
            {
                const ComponentTypeInfo* Info = GetComponentType(Type);
                if(!Info || !Member.HasComponentByType(Type)) { continue; }
                if(!FindComponentSerializer(Type))
                {
                    if(std::find(InOutWarnedTypes.begin(), InOutWarnedTypes.end(), Type) == InOutWarnedTypes.end())
                    {
                        InOutWarnedTypes.push_back(Type);
                        GAMECORE_LOG_WARN(std::string("WorldPackage: no serializer registered for '") + Info->Name
                                          + "'; components of that type are not saved")
                    }
                    continue;
                }
                if(std::find(Names.begin(), Names.end(), Info->Name) == Names.end()) { Names.push_back(Info->Name); }
            }
            std::sort(Names.begin(), Names.end());

            for(const std::string& Name : Names)
            {
                const ComponentTypeInfo*       Info       = FindComponentType(Name.c_str());
                const ComponentSerializerInfo* Serializer = FindComponentSerializer(Info->ID);
                void* Live = Member.GetComponentByType(Info->ID);
                const auto TemplateComponent = std::find_if(Template.Components.begin(), Template.Components.end(),
                    [&Name](const PrefabComponentData& C) { return C.TypeName == Name; });
                const bool bInTemplate = TemplateComponent != Template.Components.end();

                if(!Live)
                {
                    Element.append_child("RemovedComponent").append_attribute("Type") = Name.c_str();
                    continue;
                }
                if(!Serializer) { continue; }

                const std::string Context = GuidString + " entity " + std::to_string(Template.LocalId) + " " + Name;
                pugi::xml_node Now = Scratch.append_child("Now");
                XmlComponentArchive NowAr(Now, false, Serializer->Version, Refs, Context, "WorldPackage");
                Serializer->Fn(NowAr, Live);

                pugi::xml_node Was = Scratch.append_child("Was");
                std::vector<uint8_t> Bytes;
                const bool bHaveTemplate = bInTemplate && ComponentDefaults(Info->ID, Bytes)
                                        && ReadComponentBlob(Info->ID, TemplateComponent->Blob, ToLive, Bytes.data());
                if(bHaveTemplate)
                {
                    XmlComponentArchive WasAr(Was, false, Serializer->Version, Refs, Context, "WorldPackage");
                    Serializer->Fn(WasAr, Bytes.data());
                }

                /* Added on this instance: all of it. Otherwise only what changed. */
                pugi::xml_node Changed;
                for(pugi::xml_node Field = Now.first_child(); Field; Field = Field.next_sibling())
                {
                    const pugi::xml_node Before = bHaveTemplate ? Was.child(Field.name()) : pugi::xml_node();
                    if(Before && SameElement(Before, Field)) { continue; }
                    if(!Changed)
                    {
                        Changed = Element.append_child("Component");
                        Changed.append_attribute("Type")    = Name.c_str();
                        Changed.append_attribute("Version") = unsigned(Serializer->Version);
                    }
                    Changed.append_copy(Field);
                }
                Scratch.remove_child(Now);
                Scratch.remove_child(Was);
            }

            /* Nothing differs: no element at all. */
            if(!Element.first_child() && !Element.attribute("Name") && !Element.attribute("Mobility"))
            {
                Root.remove_child(Element);
            }
        }
        return DocumentText(Doc);
    }

    // === Loading ============================================================

    struct PendingEntity
    {
        EntityID                             Id;
        std::string                          GuidString;
        std::unique_ptr<pugi::xml_document>  Doc;
    };

    struct PendingInstance
    {
        PrefabInstanceID                     Id;
        std::string                          File;
        std::unique_ptr<pugi::xml_document>  Doc;
    };

    /* Spawns the instance a .nmprefabinst names, with its saved GUID, and
     * adds its entities to InOutLoaded so every reference can find them.
     * Its overrides come later (ApplyInstanceOverrides). */
    bool SpawnSavedInstance(World& InWorld, const std::string& InFolder, const EntityGuid& InGuid,
                            GuidMap& InOutLoaded, std::vector<PendingInstance>& OutPending)
    {
        PendingInstance Entry;
        Entry.File = InstanceFileFor(InFolder, InGuid);
        std::string Text;
        Entry.Doc = std::make_unique<pugi::xml_document>();
        if(!ReadWholeFile(Entry.File, Text))
        {
            GAMECORE_LOG_WARN("WorldPackage: instance file '" + Entry.File + "' is missing; skipping that instance")
            return false;
        }
        if(!ParseDocument(Text, *Entry.Doc, Entry.File, "WorldPackage")) { return false; }
        const pugi::xml_node Root = Entry.Doc->child("Instance");
        if(!Root)
        {
            GAMECORE_LOG_WARN("WorldPackage: '" + Entry.File + "' has no <Instance> element; skipping it")
            return false;
        }

        const char* PrefabPath = Root.attribute("Prefab").as_string();
        const PrefabInstance Spawned = InWorld.SpawnPrefab(PrefabPath, TransformPose(), Root.attribute("Name").as_string(), InGuid);
        if(!Spawned.IsValid())
        {
            GAMECORE_LOG_WARN("WorldPackage: '" + Entry.File + "': prefab '" + PrefabPath
                              + "' could not be spawned; skipping that instance")
            return false;
        }
        if(Spawned.GetGuid() != InGuid)
        {
            GAMECORE_LOG_WARN("WorldPackage: '" + Entry.File + "' has a GUID already in use; the instance got a fresh one")
        }

        const PrefabAsset* Prefab = Spawned.GetPrefab();
        for(size_t Index = 0; Prefab && Index < Prefab->GetEntityCount(); ++Index)
        {
            const Entity Member = Spawned.FindEntity(Prefab->GetEntityLocalId(Index));
            if(Member.IsAlive()) { InOutLoaded[Member.GetGuid()] = Member.GetID(); }
        }
        Entry.Id = Spawned.GetID();
        OutPending.push_back(std::move(Entry));
        return true;
    }

    /* Tick mode, then each <Entity>'s name, script and components: a
     * component it lacks is added from its defaults, and either way the
     * saved fields are loaded over it -- a field not written keeps the
     * prefab's value. <RemovedComponent> removes one. */
    void ApplyInstanceOverrides(World& InWorld, const PendingInstance& InEntry, const GuidMap& InLoaded)
    {
        const PrefabInstance Instance(&InWorld, InEntry.Id);
        const pugi::xml_node Root = InEntry.Doc->child("Instance");

        if(const pugi::xml_attribute Mode = Root.attribute("TickMode"))
        {
            EPrefabTickMode Parsed;
            if(ParsePrefabTickMode(Mode.as_string(), Parsed)) { Instance.SetTickMode(Parsed); }
            else { GAMECORE_LOG_WARN("WorldPackage: '" + InEntry.File + "' has an unknown tick mode '" + Mode.as_string() + "'") }
        }

        const WorldLoadingRefs Refs(InLoaded);
        for(const pugi::xml_node Item : Root.children("Entity"))
        {
            const uint32_t LocalId = Item.attribute("Id").as_uint(PREFAB_NO_LOCAL_ID);
            const Entity Member = Instance.FindEntity(LocalId);
            if(!Member.IsAlive())
            {
                GAMECORE_LOG_WARN("WorldPackage: '" + InEntry.File + "' changes entity " + std::to_string(LocalId)
                                  + ", which the prefab no longer has; ignoring that")
                continue;
            }
            if(const pugi::xml_attribute Name = Item.attribute("Name")) { Member.SetName(Name.as_string()); }
            if(const pugi::xml_attribute Mobility = Item.attribute("Mobility"))
            {
                EMobility Parsed;
                if(ParseMobility(Mobility.as_string(), Parsed)) { Member.SetMobility(Parsed); }
                else { GAMECORE_LOG_WARN("WorldPackage: '" + InEntry.File + "' has an unknown mobility '" + Mobility.as_string() + "'") }
            }
            for(const pugi::xml_node Removed : Item.children("RemovedComponent"))
            {
                if(const ComponentTypeInfo* Info = FindComponentType(Removed.attribute("Type").as_string()))
                {
                    Member.RemoveComponentByType(Info->ID);
                }
            }
            for(const pugi::xml_node Element : Item.children("Component"))
            {
                const char* TypeName = Element.attribute("Type").as_string();
                const ComponentTypeInfo*       Info       = FindComponentType(TypeName);
                const ComponentSerializerInfo* Serializer = Info ? FindComponentSerializer(Info->ID) : nullptr;
                if(!Info || !Serializer)
                {
                    GAMECORE_LOG_WARN("WorldPackage: '" + InEntry.File + "' entity " + std::to_string(LocalId) + ": skipping component '"
                                      + TypeName + "' -- " + (Info ? "no serializer registered" : "type not registered in this build"))
                    continue;
                }
                void* Component = Member.GetComponentByType(Info->ID);
                if(!Component)
                {
                    std::vector<uint8_t> Defaults;
                    ComponentDefaults(Info->ID, Defaults);
                    Component = Member.AddComponentByType(Info->ID, Defaults.data());
                    if(!Component) { continue; }
                }
                const uint16_t Version = uint16_t(Element.attribute("Version").as_uint(Serializer->Version));
                XmlComponentArchive Ar(Element, true, Version, Refs,
                                       InEntry.File + " entity " + std::to_string(LocalId) + " " + TypeName, "WorldPackage");
                Serializer->Fn(Ar, Component);
            }
        }
    }

    /* The manifest's <Settings>: only what differs from the defaults, and
     * nothing at all for a world with none, so its manifest stays as it was. */
    void AppendSettings(pugi::xml_node InRoot, const WorldSettings& InSettings)
    {
        const WorldLightingSettings Defaults;
        const WorldLightingSettings& Lighting = InSettings.Lighting;
        if(Lighting == Defaults) { return; }

        /* Numbers as entity files write them: shortest round-trip, any locale. */
        pugi::xml_node Element = InRoot.append_child("Settings").append_child("Lighting");
        const auto Put = [&Element](const char* InName, const std::string& InText) { Element.append_attribute(InName) = InText.c_str(); };
        if(Lighting.bBakedLighting != Defaults.bBakedLighting)   { Put("BakedLighting",   Lighting.bBakedLighting ? "true" : "false"); }
        if(Lighting.TexelsPerUnit != Defaults.TexelsPerUnit)     { Put("TexelsPerUnit",   FormatNumber(Lighting.TexelsPerUnit)); }
        if(Lighting.Bounces != Defaults.Bounces)                 { Put("Bounces",         FormatNumber(Lighting.Bounces)); }
        if(Lighting.SamplesPerTexel != Defaults.SamplesPerTexel) { Put("SamplesPerTexel", FormatNumber(Lighting.SamplesPerTexel)); }
        if(Lighting.ProbeSpacing != Defaults.ProbeSpacing)       { Put("ProbeSpacing",    FormatNumber(Lighting.ProbeSpacing)); }
        if(Lighting.MaxAtlasSize != Defaults.MaxAtlasSize)       { Put("MaxAtlasSize",    FormatNumber(Lighting.MaxAtlasSize)); }
        if(Lighting.BakedData != Defaults.BakedData)             { Put("BakedData",       Lighting.BakedData); }
    }

    void ReadSettings(const pugi::xml_node InRoot, WorldSettings& OutSettings)
    {
        const pugi::xml_node Element = InRoot.child("Settings").child("Lighting");
        WorldLightingSettings& Lighting = OutSettings.Lighting;
        Lighting = WorldLightingSettings();
        Lighting.bBakedLighting  = Element.attribute("BakedLighting").as_bool(Lighting.bBakedLighting);
        ParseNumber(Element.attribute("TexelsPerUnit").as_string(), Lighting.TexelsPerUnit);
        ParseNumber(Element.attribute("Bounces").as_string(), Lighting.Bounces);
        ParseNumber(Element.attribute("SamplesPerTexel").as_string(), Lighting.SamplesPerTexel);
        ParseNumber(Element.attribute("ProbeSpacing").as_string(), Lighting.ProbeSpacing);
        ParseNumber(Element.attribute("MaxAtlasSize").as_string(), Lighting.MaxAtlasSize);
        Lighting.BakedData       = Element.attribute("BakedData").as_string("");
        Lighting.Clamp();
    }

    bool LoadTextWorld(const std::string& InPath, World& InOutWorld)
    {
        std::string Text;
        if(!ReadWholeFile(InPath, Text))
        {
            GAMECORE_LOG_ERROR("WorldPackage: could not read '" + InPath + "'")
            return false;
        }
        pugi::xml_document WorldDoc;
        if(!ParseDocument(Text, WorldDoc, InPath, "WorldPackage"))
        {
            return false;
        }

        const pugi::xml_node Root = WorldDoc.child("World");
        if(!Root)
        {
            GAMECORE_LOG_ERROR("WorldPackage: '" + InPath + "' has no <World> element")
            return false;
        }
        const int Format = Root.attribute("Format").as_int(0);
        if(Format != TEXT_FORMAT)
        {
            GAMECORE_LOG_ERROR("WorldPackage: '" + InPath + "' is text format " + std::to_string(Format)
                               + "; this build reads format " + std::to_string(TEXT_FORMAT))
            return false;
        }

        ReadSettings(Root, InOutWorld.GetSettings());

        const std::string Folder = EntityFolderFor(InPath);

        /* Pass 1: all entities -- loose ones, and each instance's, spawned
         * whole -- before any loose component, so references resolve. In
         * manifest order, which is slot order. */
        std::vector<PendingEntity> Pending;
        std::vector<PendingInstance> PendingInstances;
        GuidMap Loaded;
        GuidSet Listed;
        GuidSet ListedInstances;
        for(const pugi::xml_node Item : Root.children())
        {
            if(std::strcmp(Item.name(), "Instance") == 0)
            {
                EntityGuid Guid;
                if(!EntityGuid::Parse(Item.attribute("Guid").as_string(), Guid) || !Guid.IsSet()
                   || !ListedInstances.insert(Guid).second)
                {
                    GAMECORE_LOG_WARN("WorldPackage: '" + InPath + "' lists an instance with an unreadable or repeated GUID '"
                                      + Item.attribute("Guid").as_string() + "'; skipping it")
                    continue;
                }
                SpawnSavedInstance(InOutWorld, Folder, Guid, Loaded, PendingInstances);
                continue;
            }
            if(std::strcmp(Item.name(), "Entity") != 0) { continue; }

            const char* GuidAttr = Item.attribute("Guid").as_string();
            EntityGuid Guid;
            if(!EntityGuid::Parse(GuidAttr, Guid) || !Guid.IsSet())
            {
                GAMECORE_LOG_WARN("WorldPackage: '" + InPath + "' lists an entity with an unreadable GUID '"
                                  + GuidAttr + "'; skipping it")
                continue;
            }
            if(!Listed.insert(Guid).second)
            {
                GAMECORE_LOG_WARN("WorldPackage: '" + InPath + "' lists entity " + GuidAttr + " twice; loading it once")
                continue;
            }

            const std::string File = EntityFileFor(Folder, Guid);
            std::string EntityText;
            auto Doc = std::make_unique<pugi::xml_document>();
            if(!ReadWholeFile(File, EntityText))
            {
                GAMECORE_LOG_WARN("WorldPackage: entity file '" + File + "' is missing; skipping that entity")
                continue;
            }
            if(!ParseDocument(EntityText, *Doc, File, "WorldPackage"))
            {
                continue;
            }
            const pugi::xml_node EntityRoot = Doc->child("Entity");
            if(!EntityRoot)
            {
                GAMECORE_LOG_WARN("WorldPackage: '" + File + "' has no <Entity> element; skipping it")
                continue;
            }

            EntityGuid FileGuid;
            if(!EntityGuid::Parse(EntityRoot.attribute("Guid").as_string(), FileGuid) || FileGuid != Guid)
            {
                GAMECORE_LOG_WARN("WorldPackage: '" + File + "' names a different GUID inside; using the manifest's")
            }

            const ::GameCore::Entity Created = InOutWorld.CreateEntity(EntityRoot.attribute("Name").as_string(), Guid);
            Loaded[Guid] = Created.GetID();
            if(const pugi::xml_attribute Mobility = EntityRoot.attribute("Mobility"))
            {
                EMobility Parsed;
                if(ParseMobility(Mobility.as_string(), Parsed)) { Created.SetMobility(Parsed); }
                else { GAMECORE_LOG_WARN("WorldPackage: '" + File + "' has an unknown mobility '" + Mobility.as_string() + "'") }
            }

            PendingEntity Entry;
            Entry.Id         = Created.GetID();
            Entry.GuidString = GuidAttr;
            Entry.Doc        = std::move(Doc);
            Pending.push_back(std::move(Entry));
        }

        /* Pass 2: components. */
        for(const PendingEntity& Entry : Pending)
        {
            const ::GameCore::Entity Handle(&InOutWorld, Entry.Id);
            const pugi::xml_node EntityRoot = Entry.Doc->child("Entity");

            for(const pugi::xml_node Element : EntityRoot.children("Component"))
            {
                const char* TypeName = Element.attribute("Type").as_string();
                const ComponentTypeInfo*       Info       = FindComponentType(TypeName);
                const ComponentSerializerInfo* Serializer = Info ? FindComponentSerializer(Info->ID) : nullptr;
                if(!Info || !Serializer)
                {
                    GAMECORE_LOG_WARN("WorldPackage: entity " + Entry.GuidString + ": skipping component '" + TypeName
                                      + "' -- " + (Info ? "no serializer registered" : "type not registered in this build"))
                    continue;
                }

                /* Default-constructed, not zeroed (DESIGN.md). */
                std::vector<uint8_t> Init(Info->SizeBytes, 0);
                if(Serializer->DefaultsFn)
                {
                    Serializer->DefaultsFn(Init.data());
                }
                void* Component = Handle.AddComponentByType(Info->ID, Init.data());
                if(!Component)
                {
                    continue;   // AddComponentByType warned (a duplicate)
                }

                const uint16_t Version = uint16_t(Element.attribute("Version").as_uint(Serializer->Version));
                const WorldLoadingRefs Refs(Loaded);
                XmlComponentArchive Ar(Element, true, Version, Refs, Entry.GuidString + " " + TypeName, "WorldPackage");
                Serializer->Fn(Ar, Component);
            }
        }

        /* Pass 2, instances: what each one changed over its prefab. */
        for(const PendingInstance& Entry : PendingInstances)
        {
            if(InOutWorld.IsInstanceAlive(Entry.Id)) { ApplyInstanceOverrides(InOutWorld, Entry, Loaded); }
        }

        /* Pass 3: parents, once every entity has its components -- the
         * saved Transforms are already local, so nothing moves. Manifest
         * order is the child order. An instance's own tree came with it;
         * only its root can hang from something else. */
        for(const PendingInstance& Entry : PendingInstances)
        {
            const PrefabInstance Instance(&InOutWorld, Entry.Id);
            const char* ParentText = Entry.Doc->child("Instance").attribute("Parent").as_string();
            if(!Instance.IsAlive() || ParentText[0] == '\0') { continue; }
            EntityGuid ParentGuid;
            const auto Found = EntityGuid::Parse(ParentText, ParentGuid) ? Loaded.find(ParentGuid) : Loaded.end();
            if(Found == Loaded.end())
            {
                GAMECORE_LOG_WARN("WorldPackage: '" + Entry.File + "' names parent '" + ParentText
                                  + "', which is not in this world; leaving it unparented")
                continue;
            }
            Instance.GetRoot().SetParent(Found->second, false);
        }
        for(const PendingEntity& Entry : Pending)
        {
            const char* ParentText = Entry.Doc->child("Entity").attribute("Parent").as_string();
            if(ParentText[0] == '\0') { continue; }

            EntityGuid ParentGuid;
            const auto Found = EntityGuid::Parse(ParentText, ParentGuid) ? Loaded.find(ParentGuid) : Loaded.end();
            if(Found == Loaded.end())
            {
                GAMECORE_LOG_WARN("WorldPackage: entity " + Entry.GuidString + " names parent '" + ParentText
                                  + "', which is not in this world; leaving it unparented")
                continue;
            }
            InOutWorld.GetEntity(Entry.Id).SetParent(Found->second, false);
        }

        /* Last: the prefab's entities this instance had lost. Destroyed, not
         * skipped at spawn, so a loose child under one goes with it as it did. */
        for(const PendingInstance& Entry : PendingInstances)
        {
            const PrefabInstance Instance(&InOutWorld, Entry.Id);
            for(const pugi::xml_node Removed : Entry.Doc->child("Instance").children("Removed"))
            {
                Instance.FindEntity(Removed.attribute("Entity").as_uint(PREFAB_NO_LOCAL_ID)).Destroy();
            }
        }

        /* Files the manifest doesn't list are ignored. */
        std::vector<std::string> Names;
        if(EngineCore::appDirectoryList(Folder, Names))
        {
            size_t Unlisted = 0;
            for(const std::string& Name : Names)
            {
                for(const auto& [Extension, Set] : { std::pair<const char*, const GuidSet*>{ ENTITY_EXTENSION, &Listed },
                                                     std::pair<const char*, const GuidSet*>{ INSTANCE_EXTENSION, &ListedInstances } })
                {
                    EntityGuid Guid;
                    if(EndsWith(Name, Extension)
                       && EntityGuid::Parse(Name.substr(0, Name.size() - std::strlen(Extension)).c_str(), Guid)
                       && Set->count(Guid) == 0)
                    {
                        ++Unlisted;
                    }
                }
            }
            if(Unlisted > 0)
            {
                GAMECORE_LOG_WARN("WorldPackage: " + std::to_string(Unlisted) + " entity file(s) in '" + Folder
                                  + "' are not listed in '" + InPath + "' and were ignored")
            }
        }

        InOutWorld.RefreshSpatialIndex();
        return true;
    }
#endif // WITH_UNCOOKED
}

#if WITH_UNCOOKED
PrefabInstanceID RefreshPrefabInstance(World& InWorld, const PrefabInstanceID InInstance)
{
    if(InWorld.IsTicking() || !IsSavedAsInstance(InWorld, InInstance))
    {
        return InInstance;
    }
    const PrefabInstance Old(&InWorld, InInstance);
    const std::string Path = Old.GetPrefabPath();
    const std::string Name = Old.GetName();
    const EntityGuid  Guid = Old.GetGuid();

    /* What a level file would keep, as one: the overrides against the
     * version it was spawned from. */
    std::vector<ComponentTypeID> Warned;
    PendingInstance Entry;
    Entry.File = "refreshing an instance of '" + Path + "'";
    Entry.Doc  = std::make_unique<pugi::xml_document>();
    if(!ParseDocument(BuildInstanceText(InWorld, InInstance, Warned), *Entry.Doc, Entry.File, "WorldPackage"))
    {
        return InInstance;
    }

    /* Loose entities hanging from members, and the root's own parent, are
     * let go -- local Transforms as they are -- and hung back afterwards. */
    struct Hung { EntityID Child; EntityGuid Parent; };
    std::vector<Hung> Loose;
    std::vector<EntityID> Stack{ Old.GetRoot().GetID() };
    while(!Stack.empty())
    {
        const Entity Member = InWorld.GetEntity(Stack.back());
        Stack.pop_back();
        for(EntityID Child = Member.GetFirstChild(); Child.IsSet(); Child = InWorld.GetEntity(Child).GetNextSibling())
        {
            if(InWorld.GetEntity(Child).GetInstance() == InInstance) { Stack.push_back(Child); }
            else                                                    { Loose.push_back({ Child, Member.GetGuid() }); }
        }
    }
    for(const Hung& Each : Loose) { InWorld.GetEntity(Each.Child).SetParent(EntityID(), false); }
    const EntityID RootParent = Old.GetRoot().GetParent();
    if(RootParent.IsSet()) { Old.GetRoot().SetParent(EntityID(), false); }

    InWorld.DestroyPrefabInstance(InInstance);   // immediate: not ticking
    const PrefabInstance New = InWorld.SpawnPrefab(Path.c_str(), TransformPose(), Name.c_str(), Guid);
    if(!New.IsValid())
    {
        GAMECORE_LOG_WARN("RefreshPrefabInstance: '" + Path + "' could not be spawned again; the instance is gone")
        return PrefabInstanceID();
    }

    Entry.Id = New.GetID();
    GuidMap Loaded;
    struct Collect { World* W; GuidMap* Map; } Ctx{ &InWorld, &Loaded };
    InWorld.ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        const auto& C = *static_cast<Collect*>(InUserData);
        (*C.Map)[C.W->GetEntity(InEntity).GetGuid()] = InEntity;
    }, &Ctx);
    ApplyInstanceOverrides(InWorld, Entry, Loaded);

    if(InWorld.IsAlive(RootParent)) { New.GetRoot().SetParent(RootParent, false); }
    for(const Hung& Each : Loose)
    {
        const EntityID Parent = InWorld.FindEntityByGuid(Each.Parent);
        if(Parent.IsSet() && InWorld.IsAlive(Each.Child)) { InWorld.GetEntity(Each.Child).SetParent(Parent, false); }
    }
    for(const pugi::xml_node Removed : Entry.Doc->child("Instance").children("Removed"))
    {
        New.FindEntity(Removed.attribute("Entity").as_uint(PREFAB_NO_LOCAL_ID)).Destroy();
    }
    return New.GetID();
}
#endif // WITH_UNCOOKED

EWorldFileFormat DetectWorldFileFormat(const char* InPath)
{
    if(!InPath) { return EWorldFileFormat::Unknown; }
    EngineCore::FileHandle Handle = EngineCore::appFileOpenRead(InPath);
    if(!Handle) { return EWorldFileFormat::Unknown; }

    uint8_t Head[64];
    const size_t Count = EngineCore::appFileRead(Handle, Head, sizeof(Head));
    EngineCore::appFileClose(Handle);

    size_t Index = 0;
    if(Count >= 3 && Head[0] == 0xEF && Head[1] == 0xBB && Head[2] == 0xBF) { Index = 3; }   // UTF-8 BOM
    while(Index < Count && (Head[Index] == ' ' || Head[Index] == '\t' || Head[Index] == '\r' || Head[Index] == '\n'))
    {
        ++Index;
    }
    if(Index < Count && Head[Index] == '<') { return EWorldFileFormat::Text; }
    return Count > 0 ? EWorldFileFormat::Binary : EWorldFileFormat::Unknown;
}

bool LoadWorldFile(const char* InPath, World& InOutWorld, EWorldFileFormat* OutFormat)
{
    CORE_CHECK(!InOutWorld.IsTicking(), "LoadWorldFile called while the World is ticking -- see WorldPackage.h")
    if(!InPath || InOutWorld.IsTicking()) { return false; }

    const EWorldFileFormat Format = DetectWorldFileFormat(InPath);
    if(OutFormat) { *OutFormat = Format; }

    switch(Format)
    {
        case EWorldFileFormat::Binary:
            return LoadBinaryWorld(InPath, InOutWorld);

        case EWorldFileFormat::Text:
#if WITH_UNCOOKED
            return LoadTextWorld(InPath, InOutWorld);
#else
            GAMECORE_LOG_ERROR(std::string("LoadWorldFile: '") + InPath
                               + "' is a text world; a Shipping build loads only cooked worlds -- cook it first")
            return false;
#endif

        case EWorldFileFormat::Unknown:
            break;
    }
    GAMECORE_LOG_ERROR(std::string("LoadWorldFile: could not read '") + InPath + "'")
    return false;
}

#if WITH_UNCOOKED
bool SaveWorldPackage(const char* InPath, World& InWorld)
{
    CORE_CHECK(!InWorld.IsTicking(), "SaveWorldPackage called while the World is ticking -- see WorldPackage.h")
    if(!InPath || InPath[0] == '\0' || InWorld.IsTicking()) { return false; }

    const std::string WorldPath(InPath);
    const std::string Folder = EntityFolderFor(WorldPath);

    struct OutFile
    {
        std::string Path;
        std::string Text;
    };

    // --- 1. Everything in memory: the entities in slot order, the manifest last.
    std::vector<EntityID> Entities;
    InWorld.ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        static_cast<std::vector<EntityID>*>(InUserData)->push_back(InEntity);
    }, &Entities);

    std::vector<OutFile>         Files;
    std::vector<ComponentTypeID> WarnedTypes;
    GuidSet                      Keep;

    pugi::xml_document Manifest;
    AppendDeclaration(Manifest);
    pugi::xml_node ManifestRoot = Manifest.append_child("World");
    pugi::xml_attribute FormatAttribute = ManifestRoot.append_attribute("Format");
    AppendSettings(ManifestRoot, InWorld.GetSettings());

    /* In slot order. An instance's entities are one <Instance>, listed where
     * its root is; everything else -- loose entities, and members of an
     * instance that cannot be saved as one -- is an <Entity>. */
    std::vector<PrefabInstanceID> Flattened;
    for(const EntityID Id : Entities)
    {
        const Entity Handle = InWorld.GetEntity(Id);
        const PrefabInstanceID Instance = Handle.GetInstance();
        if(IsSavedInInstance(InWorld, Id))
        {
            if(PrefabInstance(&InWorld, Instance).GetRoot() == Handle)
            {
                const EntityGuid Guid = PrefabInstance(&InWorld, Instance).GetGuid();
                Keep.insert(Guid);
                ManifestRoot.append_child("Instance").append_attribute("Guid") = GuidText(Guid).c_str();
                Files.push_back({ InstanceFileFor(Folder, Guid), BuildInstanceText(InWorld, Instance, WarnedTypes) });
            }
            continue;
        }
        if(Instance.IsSet() && !IsSavedAsInstance(InWorld, Instance)
           && std::find(Flattened.begin(), Flattened.end(), Instance) == Flattened.end())
        {
            Flattened.push_back(Instance);
            GAMECORE_LOG_WARN(std::string("SaveWorldPackage: prefab instance '") + PrefabInstance(&InWorld, Instance).GetName()
                              + "' was not spawned from a prefab file; its entities are saved as plain entities")
        }

        const EntityGuid Guid = Handle.GetGuid();
        Keep.insert(Guid);
        ManifestRoot.append_child("Entity").append_attribute("Guid") = GuidText(Guid).c_str();
        Files.push_back({ EntityFileFor(Folder, Guid), BuildEntityText(InWorld, Id, WarnedTypes) });
    }
    FormatAttribute = TEXT_FORMAT;
    Files.push_back({ WorldPath, DocumentText(Manifest) });

    if(!EngineCore::appCreateDirectories(Folder))
    {
        GAMECORE_LOG_ERROR("SaveWorldPackage: could not create '" + Folder + "'")
        return false;
    }

    // --- 2. Changed files to temporaries; any failure leaves the old world intact.
    std::vector<const OutFile*> Changed;
    for(const OutFile& File : Files)
    {
        std::string Existing;
        if(ReadWholeFile(File.Path, Existing) && Existing == File.Text)
        {
            continue;   // unchanged: leave timestamp and VCS state alone
        }
        if(!WriteWholeFile(File.Path + TEMP_SUFFIX, File.Text))
        {
            GAMECORE_LOG_ERROR("SaveWorldPackage: could not write '" + File.Path + TEMP_SUFFIX
                               + "'; nothing was changed")
            EngineCore::appFileDelete(File.Path + TEMP_SUFFIX);
            for(const OutFile* Written : Changed)
            {
                EngineCore::appFileDelete(Written->Path + TEMP_SUFFIX);
            }
            return false;
        }
        Changed.push_back(&File);
    }

    // --- 3. Commit: rename over the targets, the manifest (pushed last) last.
    for(size_t Index = 0; Index < Changed.size(); ++Index)
    {
        std::string Error;
        if(!RenameReplacing(Changed[Index]->Path + TEMP_SUFFIX, Changed[Index]->Path, Error))
        {
            GAMECORE_LOG_ERROR("SaveWorldPackage: could not replace '" + Changed[Index]->Path + "' (" + Error + "); "
                               + std::to_string(Index) + " of " + std::to_string(Changed.size())
                               + " changed file(s) were already committed and the manifest was "
                               + "not updated -- save again once the file is free")
            for(size_t Rest = Index; Rest < Changed.size(); ++Rest)
            {
                EngineCore::appFileDelete(Changed[Rest]->Path + TEMP_SUFFIX);
            }
            return false;
        }
    }

    // --- 4. Clean up: unlisted entity files and stale temporaries, in the folder only.
    std::vector<std::string> Names;
    if(EngineCore::appDirectoryList(Folder, Names))
    {
        size_t Removed = 0;
        for(const std::string& Name : Names)
        {
            const std::string Full = Folder + "/" + Name;
            for(const char* Extension : { ENTITY_EXTENSION, INSTANCE_EXTENSION })
            {
                if(EndsWith(Name, (std::string(Extension) + TEMP_SUFFIX).c_str()))
                {
                    EngineCore::appFileDelete(Full);
                    break;
                }
                EntityGuid Guid;
                if(EndsWith(Name, Extension)
                   && EntityGuid::Parse(Name.substr(0, Name.size() - std::strlen(Extension)).c_str(), Guid)
                   && Keep.count(Guid) == 0)
                {
                    if(EngineCore::appFileDelete(Full)) { ++Removed; }
                    else { GAMECORE_LOG_WARN("SaveWorldPackage: could not remove deleted entity's file '" + Full + "'") }
                    break;
                }
            }
        }
        if(Removed > 0)
        {
            GAMECORE_LOG("WorldPackage: removed " + std::to_string(Removed) + " deleted entit"
                         + (Removed == 1 ? "y's file" : "ies' files") + " from '" + Folder + "'")
        }
    }

    return true;
}

bool CookWorldFile(const char* InSourcePath, const char* InCookedPath)
{
    if(!InSourcePath || !InCookedPath) { return false; }

    World Scratch;
    if(!LoadWorldFile(InSourcePath, Scratch))
    {
        GAMECORE_LOG_ERROR(std::string("CookWorldFile: could not load '") + InSourcePath + "'")
        return false;
    }

    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
    if(!SerializeWorld(Ar, Scratch) || Ar.HasError())
    {
        GAMECORE_LOG_ERROR(std::string("CookWorldFile: could not serialize '") + InSourcePath + "'")
        return false;
    }

    const std::string Cooked(InCookedPath);
    const std::string Parent = ParentDirectory(Cooked);
    if(!Parent.empty() && !EngineCore::appCreateDirectories(Parent))
    {
        GAMECORE_LOG_ERROR("CookWorldFile: could not create '" + Parent + "'")
        return false;
    }

    const std::vector<uint8_t>& Bytes = Ar.GetBytes();
    const std::string Temp = Cooked + TEMP_SUFFIX;
    std::string Error;
    if(!WriteWholeFile(Temp, std::string(reinterpret_cast<const char*>(Bytes.data()), Bytes.size()))
       || !RenameReplacing(Temp, Cooked, Error))
    {
        EngineCore::appFileDelete(Temp);
        GAMECORE_LOG_ERROR("CookWorldFile: could not write '" + Cooked + "'" + (Error.empty() ? "" : " (" + Error + ")"))
        return false;
    }

    GAMECORE_LOG(std::string("WorldPackage: cooked '") + InSourcePath + "' -> '" + Cooked + "' ("
                 + std::to_string(Bytes.size()) + " bytes, " + std::to_string(Scratch.GetEntityCount()) + " entities)")
    return true;
}
#endif // WITH_UNCOOKED

// === Content by source path ==================================================

namespace
{
#if WITH_UNCOOKED && !WITH_EDITOR
    namespace fs = std::filesystem;


    /* The newest modification of InFile and of every file under InFolder;
     * false if InFile is missing. */
    bool NewestTime(const fs::path& InFile, const fs::path* InFolder, fs::file_time_type& OutNewest)
    {
        std::error_code Error;
        OutNewest = fs::last_write_time(InFile, Error);
        if(Error) { return false; }
        if(InFolder && fs::is_directory(*InFolder, Error))
        {
            for(const fs::directory_entry& Entry : fs::recursive_directory_iterator(*InFolder, Error))
            {
                const fs::file_time_type Time = Entry.last_write_time(Error);
                if(!Error && Time > OutNewest) { OutNewest = Time; }
            }
        }
        return true;
    }

    /* InSource (text) cooked into Intermediate/Cooked/<InSource> if that is
     * missing or older; the path to read: the cooked one, or InSource if the
     * cook fails. InFolder: a world's entity folder, part of its source. */
    std::string CookOnLoad(const std::string& InSource, const fs::path* InFolder,
                           bool (*InCook)(const char*, const char*), const char* InWhat)
    {
        const fs::path Source = fs::u8path(InSource);
        fs::file_time_type SourceTime;
        if(!NewestTime(Source, InFolder, SourceTime)) { return InSource; }   // no source: let the load report it

        const fs::path Cooked = fs::u8path("Intermediate/Cooked") / Source.relative_path();
        std::error_code Error;
        const fs::file_time_type CookedTime = fs::last_write_time(Cooked, Error);
        const auto CookedU8 = Cooked.generic_u8string();
        const std::string CookedPath(CookedU8.begin(), CookedU8.end());
        if(!Error && CookedTime >= SourceTime) { return CookedPath; }

        fs::create_directories(Cooked.parent_path(), Error);
        if(!InCook(InSource.c_str(), CookedPath.c_str()))
        {
            GAMECORE_LOG_WARN(std::string("Content: could not cook ") + InWhat + " '" + InSource + "'; reading the text source")
            return InSource;
        }
        GAMECORE_LOG(std::string("Content: cooked ") + InWhat + " '" + InSource + "' for this build")
        return CookedPath;
    }
#endif
}

std::string ResolveContentWorld(const char* InSourcePath)
{
    const std::string Source = InSourcePath ? InSourcePath : "";
#if WITH_EDITOR
    return Source;
#elif WITH_UNCOOKED
    if(DetectWorldFileFormat(Source.c_str()) != EWorldFileFormat::Text || !GContentCookOnLoad.GetBool()) { return Source; }
    const fs::path Folder = fs::u8path(EntityFolderFor(Source));
    return CookOnLoad(Source, &Folder, &CookWorldFile, "world");
#else
    /* Shipping: the cooked world is staged at its source path (from <project>/Cooked/). */
    return Source;
#endif
}

std::string ResolveContentPrefab(const char* InSourcePath)
{
    const std::string Source = InSourcePath ? InSourcePath : "";
#if WITH_UNCOOKED && !WITH_EDITOR
    if(DetectWorldFileFormat(Source.c_str()) == EWorldFileFormat::Text && GContentCookOnLoad.GetBool())
    {
        return CookOnLoad(Source, nullptr, &CookPrefabFile, "prefab");
    }
#endif
    /* Editor: the source. Shipping: the cooked prefab is staged at its source path. */
    return Source;
}

bool LoadContentWorld(const char* InSourcePath, World& InOutWorld)
{
    const std::string Path = ResolveContentWorld(InSourcePath);
    if(!LoadWorldFile(Path.c_str(), InOutWorld))
    {
        GAMECORE_LOG_ERROR(std::string("LoadContentWorld: could not load '") + (InSourcePath ? InSourcePath : "")
                           + "'" + (Path != (InSourcePath ? InSourcePath : "") ? " (read as '" + Path + "')" : std::string()))
        return false;
    }
    return true;
}
}

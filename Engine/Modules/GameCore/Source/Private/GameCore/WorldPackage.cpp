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

#if WITH_EDITOR
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
#include "Private/GameCore/XmlComponentArchive.h"
#endif

#if WITH_EDITOR
using namespace GameCore::XmlText;
#endif

/* Format detection and binary load in every build; the text package (save,
 * load, cook) in editor builds only. See WorldPackage.h and DESIGN.md. */

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

#if WITH_EDITOR
    constexpr int         TEXT_FORMAT      = 1;
    constexpr const char* ENTITY_EXTENSION = ".nmentity";
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

    /* <Entity Guid Name> [<Script>] <Component>*, components sorted by type
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
        /* Only when set, so a root entity's file reads as it always has. */
        if(const EntityID Parent = Handle.GetParent(); Parent.IsSet())
        {
            Root.append_attribute("Parent") = GuidText(InWorld.GetEntity(Parent).GetGuid()).c_str();
        }

        if(Handle.HasScript())
        {
            pugi::xml_node Script = Root.append_child("Script");
            Script.append_attribute("Path") = Handle.GetScriptPath();
            const EntityScriptState* State = Handle.GetScriptState();
            Script.append_attribute("CullOutsideActiveRegion") = (State && State->bCullOutsideActiveRegion) ? "true" : "false";
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

    // === Loading ============================================================

    struct PendingEntity
    {
        EntityID                             Id;
        std::string                          GuidString;
        std::unique_ptr<pugi::xml_document>  Doc;
    };

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

        const std::string Folder = EntityFolderFor(InPath);

        /* Pass 1: all entities before any component, so forward references resolve. */
        std::vector<PendingEntity> Pending;
        GuidMap Loaded;
        GuidSet Listed;
        for(const pugi::xml_node Item : Root.children("Entity"))
        {
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

            PendingEntity Entry;
            Entry.Id         = Created.GetID();
            Entry.GuidString = GuidAttr;
            Entry.Doc        = std::move(Doc);
            Pending.push_back(std::move(Entry));
        }

        /* Pass 2: scripts and components. */
        for(const PendingEntity& Entry : Pending)
        {
            const ::GameCore::Entity Handle(&InOutWorld, Entry.Id);
            const pugi::xml_node EntityRoot = Entry.Doc->child("Entity");

            if(const pugi::xml_node Script = EntityRoot.child("Script"))
            {
                const char* ScriptPath = Script.attribute("Path").as_string();
                if(ScriptPath[0] != '\0' && Handle.SetScript(ScriptPath))
                {
                    if(EntityScriptState* State = Handle.GetScriptState())
                    {
                        State->bCullOutsideActiveRegion = Script.attribute("CullOutsideActiveRegion").as_bool() ? 1 : 0;
                    }
                }
            }

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

        /* Pass 3: parents, once every entity has its components -- the
         * saved Transforms are already local, so nothing moves. Manifest
         * order is the child order. */
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

        /* Files the manifest doesn't list are ignored. */
        std::vector<std::string> Names;
        if(EngineCore::appDirectoryList(Folder, Names))
        {
            size_t Unlisted = 0;
            for(const std::string& Name : Names)
            {
                EntityGuid Guid;
                if(EndsWith(Name, ENTITY_EXTENSION)
                   && EntityGuid::Parse(Name.substr(0, Name.size() - std::strlen(ENTITY_EXTENSION)).c_str(), Guid)
                   && Listed.count(Guid) == 0)
                {
                    ++Unlisted;
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
#endif // WITH_EDITOR
}

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
#if WITH_EDITOR
            return LoadTextWorld(InPath, InOutWorld);
#else
            GAMECORE_LOG_ERROR(std::string("LoadWorldFile: '") + InPath
                               + "' is an editor (text) world; a game build loads cooked worlds -- cook it first")
            return false;
#endif

        case EWorldFileFormat::Unknown:
            break;
    }
    GAMECORE_LOG_ERROR(std::string("LoadWorldFile: could not read '") + InPath + "'")
    return false;
}

#if WITH_EDITOR
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
    ManifestRoot.append_attribute("Format") = TEXT_FORMAT;

    for(const EntityID Id : Entities)
    {
        const EntityGuid Guid = InWorld.GetEntity(Id).GetGuid();
        Keep.insert(Guid);
        ManifestRoot.append_child("Entity").append_attribute("Guid") = GuidText(Guid).c_str();
        Files.push_back({ EntityFileFor(Folder, Guid), BuildEntityText(InWorld, Id, WarnedTypes) });
    }
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
            if(EndsWith(Name, (std::string(ENTITY_EXTENSION) + TEMP_SUFFIX).c_str()))
            {
                EngineCore::appFileDelete(Full);
                continue;
            }
            EntityGuid Guid;
            if(EndsWith(Name, ENTITY_EXTENSION)
               && EntityGuid::Parse(Name.substr(0, Name.size() - std::strlen(ENTITY_EXTENSION)).c_str(), Guid)
               && Keep.count(Guid) == 0)
            {
                if(EngineCore::appFileDelete(Full)) { ++Removed; }
                else { GAMECORE_LOG_WARN("SaveWorldPackage: could not remove deleted entity's file '" + Full + "'") }
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
#endif // WITH_EDITOR
}

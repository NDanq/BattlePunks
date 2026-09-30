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
#endif

/*
 * World files: format detection and the binary load in every build; the text
 * package (XML manifest + one .nmentity per entity), its save, its load and
 * cooking in editor builds only -- pugixml is not compiled into a game. The
 * layout and the save's commit order are documented in WorldPackage.h and
 * DESIGN.md.
 */

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

    // === Paths and files ====================================================
    // Core's appFile* for reading and writing (UTF-8 paths); std::filesystem
    // only for the one thing Core has no call for, a replacing rename.

    bool EndsWith(const std::string& InText, const char* InSuffix)
    {
        const size_t Length = std::strlen(InSuffix);
        return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
    }

    /* "Levels/Sandbox.nmworld" -> "Levels/Sandbox": the extension off the
     * file name only, so a dot in a directory name is left alone. */
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

    bool ReadWholeFile(const std::string& InPath, std::string& OutText)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(InPath, Bytes))
        {
            return false;
        }
        OutText.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        return true;
    }

    bool WriteWholeFile(const std::string& InPath, const std::string& InText)
    {
        EngineCore::FileHandle Handle = EngineCore::appFileOpenWrite(InPath);
        if(!Handle)
        {
            return false;
        }
        const size_t Written = InText.empty() ? 0 : EngineCore::appFileWrite(Handle, InText.data(), InText.size());
        EngineCore::appFileClose(Handle);
        return Written == InText.size();
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

    // === Text values ========================================================
    // std::to_chars' shortest round-trip form: exact on reload, the same text
    // for the same value every save (so an untouched entity diffs as nothing),
    // and locale-independent in both directions.

    template<typename T>
    std::string FormatNumber(const T InValue)
    {
        char Buffer[64];
        const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), InValue);
        return std::string(Buffer, Result.ptr);
    }

    template<typename T>
    bool ParseNumber(const char* InText, T& OutValue)
    {
        if(!InText || InText[0] == '\0') { return false; }
        const char* End = InText + std::strlen(InText);
        T Parsed{};
        const std::from_chars_result Result = std::from_chars(InText, End, Parsed);
        if(Result.ec != std::errc() || Result.ptr != End) { return false; }
        OutValue = Parsed;
        return true;
    }

    bool ParseBool(const char* InText, bool& OutValue)
    {
        if(!InText) { return false; }
        if(std::strcmp(InText, "true") == 0)  { OutValue = true;  return true; }
        if(std::strcmp(InText, "false") == 0) { OutValue = false; return true; }
        return false;
    }

    /* Letters, digits and '_', not starting with a digit -- a safe subset of
     * XML's element names that every serializer's field names fit. */
    bool IsElementName(const char* InName)
    {
        if(!InName || InName[0] == '\0' || (InName[0] >= '0' && InName[0] <= '9')) { return false; }
        for(const char* Ch = InName; *Ch; ++Ch)
        {
            const bool bOk = (*Ch >= 'a' && *Ch <= 'z') || (*Ch >= 'A' && *Ch <= 'Z')
                          || (*Ch >= '0' && *Ch <= '9') || *Ch == '_';
            if(!bOk) { return false; }
        }
        return true;
    }

    std::string GuidText(const EntityGuid& InGuid)
    {
        char Text[EntityGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return Text;
    }

    struct StringWriter final : pugi::xml_writer
    {
        std::string Text;
        void write(const void* InData, const size_t InSize) override
        {
            Text.append(static_cast<const char*>(InData), InSize);
        }
    };

    /* Two-space indent, '\n' line endings, UTF-8 -- fixed, so the bytes depend
     * on the data alone. */
    std::string DocumentText(const pugi::xml_document& InDoc)
    {
        StringWriter Writer;
        InDoc.save(Writer, "  ", pugi::format_indent, pugi::encoding_utf8);
        return Writer.Text;
    }

    pugi::xml_node AppendDeclaration(pugi::xml_document& InDoc)
    {
        pugi::xml_node Declaration = InDoc.append_child(pugi::node_declaration);
        Declaration.append_attribute("version")  = "1.0";
        Declaration.append_attribute("encoding") = "UTF-8";
        return Declaration;
    }

    bool ParseDocument(const std::string& InText, pugi::xml_document& OutDoc, const std::string& InPath)
    {
        const pugi::xml_parse_result Result = OutDoc.load_buffer(InText.data(), InText.size(),
                                                                 pugi::parse_default, pugi::encoding_utf8);
        if(!Result)
        {
            GAMECORE_LOG_ERROR("WorldPackage: '" + InPath + "' is not valid XML: " + Result.description()
                               + " (at byte " + std::to_string(Result.offset) + ")")
            return false;
        }
        return true;
    }

    // === The text ComponentArchive ==========================================

    /*
     * One element per field, named for the field, its value in attributes --
     * <Width Value="32"/>, <Position X="0" Y="360" Z="0"/>. Saving appends to
     * the <Component> element; loading reads it by name, so a missing field
     * keeps its default (the loader starts every component from its defaults)
     * and an element nothing asks for is ignored.
     */
    class XmlComponentArchive final : public ComponentArchive
    {
    public:
        /* Saving: entity references are written as their target's GUID. */
        XmlComponentArchive(const pugi::xml_node InNode, const uint16_t InVersion, World& InWorld, std::string InContext)
            : Node(InNode), bLoading(false), Version(InVersion), SavingWorld(&InWorld), Context(std::move(InContext)) {}

        /* Loading: entity references resolve through the GUIDs of the entities
         * this load created -- never through the World, which may already hold
         * other entities. */
        XmlComponentArchive(const pugi::xml_node InNode, const uint16_t InVersion, const GuidMap& InLoaded, std::string InContext)
            : Node(InNode), bLoading(true), Version(InVersion), Loaded(&InLoaded), Context(std::move(InContext)) {}

        bool     IsLoading() const override { return bLoading; }
        uint16_t GetVersion() const override { return Version; }

        using ComponentArchive::Field;

        void Field(const char* InName, float& InOut) override    { Number(InName, InOut); }
        void Field(const char* InName, double& InOut) override   { Number(InName, InOut); }
        void Field(const char* InName, int32_t& InOut) override  { Number(InName, InOut); }
        void Field(const char* InName, uint32_t& InOut) override { Number(InName, InOut); }

        void Field(const char* InName, uint8_t& InOut) override
        {
            uint32_t Wide = InOut;
            Number(InName, Wide);
            if(bLoading)
            {
                if(Wide > 255) { Unreadable(InName); return; }
                InOut = uint8_t(Wide);
            }
        }

        void Field(const char* InName, bool& InOut) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            if(!bLoading)
            {
                Element.append_attribute("Value") = InOut ? "true" : "false";
                return;
            }
            if(!ParseBool(Element.attribute("Value").as_string(nullptr), InOut)) { Unreadable(InName); }
        }

        void Flag(const char* InName, uint8_t& InOut) override
        {
            bool Value = InOut != 0;
            Field(InName, Value);
            if(bLoading) { InOut = Value ? 1 : 0; }
        }

        void Field(const char* InName, EngineCore::Vector3& InOut) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            float X = float(InOut.GetX()), Y = float(InOut.GetY()), Z = float(InOut.GetZ());
            if(!bLoading)
            {
                Put(Element, "X", X); Put(Element, "Y", Y); Put(Element, "Z", Z);
                return;
            }
            if(Get(Element, "X", X) && Get(Element, "Y", Y) && Get(Element, "Z", Z))
            {
                InOut.SetX(X); InOut.SetY(Y); InOut.SetZ(Z);
            }
            else
            {
                Unreadable(InName);
            }
        }

        void Field(const char* InName, AABB& InOut) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            float Values[6] = { float(InOut.Min.GetX()), float(InOut.Min.GetY()), float(InOut.Min.GetZ()),
                                float(InOut.Max.GetX()), float(InOut.Max.GetY()), float(InOut.Max.GetZ()) };
            static const char* const Names[6] = { "MinX", "MinY", "MinZ", "MaxX", "MaxY", "MaxZ" };
            if(!bLoading)
            {
                for(int Index = 0; Index < 6; ++Index) { Put(Element, Names[Index], Values[Index]); }
                return;
            }
            for(int Index = 0; Index < 6; ++Index)
            {
                if(!Get(Element, Names[Index], Values[Index])) { Unreadable(InName); return; }
            }
            InOut.Min.SetX(Values[0]); InOut.Min.SetY(Values[1]); InOut.Min.SetZ(Values[2]);
            InOut.Max.SetX(Values[3]); InOut.Max.SetY(Values[4]); InOut.Max.SetZ(Values[5]);
        }

        void Field(const char* InName, EngineCore::Color& InOut) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            uint32_t Channels[4] = { InOut.R, InOut.G, InOut.B, InOut.A };
            static const char* const Names[4] = { "R", "G", "B", "A" };
            if(!bLoading)
            {
                for(int Index = 0; Index < 4; ++Index) { Put(Element, Names[Index], Channels[Index]); }
                return;
            }
            for(int Index = 0; Index < 4; ++Index)
            {
                if(!Get(Element, Names[Index], Channels[Index]) || Channels[Index] > 255) { Unreadable(InName); return; }
            }
            InOut.R = uint8_t(Channels[0]); InOut.G = uint8_t(Channels[1]);
            InOut.B = uint8_t(Channels[2]); InOut.A = uint8_t(Channels[3]);
        }

        void FixedString(const char* InName, char* InOutBuffer, const size_t InCapacity) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element || InCapacity == 0) { return; }
            if(!bLoading)
            {
                Element.append_attribute("Value") = InOutBuffer;
                return;
            }
            const pugi::xml_attribute Value = Element.attribute("Value");
            if(!Value) { Unreadable(InName); return; }
            std::strncpy(InOutBuffer, Value.as_string(), InCapacity - 1);
            InOutBuffer[InCapacity - 1] = '\0';
        }

        /* Guid="" for an unset reference or one to an entity that no longer
         * exists. */
        void Entity(const char* InName, EntityID& InOut) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            if(!bLoading)
            {
                const EntityGuid Target = ::GameCore::Entity(SavingWorld, InOut).GetGuid();
                Element.append_attribute("Guid") = Target.IsSet() ? GuidText(Target).c_str() : "";
                return;
            }

            const char* Text = Element.attribute("Guid").as_string();
            if(Text[0] == '\0')
            {
                InOut = InvalidEntityID();
                return;
            }
            EntityGuid Target;
            const auto Found = EntityGuid::Parse(Text, Target) ? Loaded->find(Target) : Loaded->end();
            if(Found == Loaded->end())
            {
                GAMECORE_LOG_WARN("WorldPackage: " + Context + ": '" + InName + "' refers to entity " + Text
                                  + ", which this world does not contain; leaving it unset")
                InOut = InvalidEntityID();
                return;
            }
            InOut = Found->second;
        }

    private:
        /* Saving: the new element (null, with an error, for a name XML cannot
         * hold). Loading: the element, or null if absent -- the default stays. */
        pugi::xml_node Begin(const char* InName)
        {
            if(bLoading)
            {
                return Node.child(InName);
            }
            if(!IsElementName(InName))
            {
                GAMECORE_LOG_ERROR("WorldPackage: " + Context + ": field name '" + (InName ? InName : "")
                                   + "' is not a valid XML element name; the field is not saved")
                return pugi::xml_node();
            }
            return Node.append_child(InName);
        }

        template<typename T>
        void Number(const char* InName, T& InOut)
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            if(!bLoading)
            {
                Put(Element, "Value", InOut);
                return;
            }
            if(!Get(Element, "Value", InOut)) { Unreadable(InName); }
        }

        template<typename T>
        static void Put(pugi::xml_node InElement, const char* InAttribute, const T InValue)
        {
            InElement.append_attribute(InAttribute) = FormatNumber(InValue).c_str();
        }

        template<typename T>
        static bool Get(const pugi::xml_node InElement, const char* InAttribute, T& OutValue)
        {
            return ParseNumber(InElement.attribute(InAttribute).as_string(nullptr), OutValue);
        }

        void Unreadable(const char* InName) const
        {
            GAMECORE_LOG_WARN("WorldPackage: " + Context + ": field '" + InName
                              + "' has a missing or unreadable value; keeping its default")
        }

        pugi::xml_node Node;
        bool           bLoading    = false;
        uint16_t       Version     = 0;
        World*         SavingWorld = nullptr;
        const GuidMap* Loaded      = nullptr;
        std::string    Context;
    };

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

            XmlComponentArchive Ar(Element, Component.Serializer->Version, InWorld,
                                   GuidString + " " + Component.Name);
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
        if(!ParseDocument(Text, WorldDoc, InPath))
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

        /* Pass 1: every listed entity, with its GUID and name, before any
         * component -- so a reference to an entity listed later resolves. */
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
            if(!ParseDocument(EntityText, *Doc, File))
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

                /* Default-constructed, NOT zeroed -- see DESIGN.md -- so a
                 * field the file lacks keeps its real default. */
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
                XmlComponentArchive Ar(Element, Version, Loaded, Entry.GuidString + " " + TypeName);
                Serializer->Fn(Ar, Component);
            }
        }

        /* Files in the folder the manifest does not list are ignored -- a
         * crashed save's leftovers, or one merged in without its manifest line. */
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
            continue;   // unchanged: not rewritten, so its timestamp and VCS state stay put
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

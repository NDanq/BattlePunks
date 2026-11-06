#ifndef GAMECORE_XMLCOMPONENTARCHIVE_H
#define GAMECORE_XMLCOMPONENTARCHIVE_H

#if WITH_UNCOOKED

#include <charconv>
#include <cstring>
#include <string>
#include <vector>

#include "pugixml.hpp"

#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * The editor's XML text, shared by the text world (WorldPackage.cpp) and the
 * text prefab (Prefab.cpp): whole-file IO, exact number formatting, a fixed
 * document layout, and the ComponentArchive that writes one element per
 * field. Editor builds only, like pugixml itself. Private to GameCore.
 *
 * Entity references are the one thing the two formats spell differently --
 * a world names an entity by its GUID, a prefab by its prefab-local id -- so
 * the archive asks an XmlEntityRefs for them.
 */
namespace GameCore
{
namespace XmlText
{
    // === Files ==============================================================
    // Core's appFile*.

    inline bool ReadWholeFile(const std::string& InPath, std::string& OutText)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(InPath, Bytes))
        {
            return false;
        }
        OutText.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        return true;
    }

    inline bool WriteWholeFile(const std::string& InPath, const std::string& InText)
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

    // === Text values ========================================================
    // std::to_chars' shortest round-trip form: exact, stable across saves,
    // locale-independent.

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

    inline bool ParseBool(const char* InText, bool& OutValue)
    {
        if(!InText) { return false; }
        if(std::strcmp(InText, "true") == 0)  { OutValue = true;  return true; }
        if(std::strcmp(InText, "false") == 0) { OutValue = false; return true; }
        return false;
    }

    /* [A-Za-z_][A-Za-z0-9_]*: a safe subset of XML element names. */
    inline bool IsElementName(const char* InName)
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

    inline std::string GuidText(const EntityGuid& InGuid)
    {
        char Text[EntityGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return Text;
    }

    // === Documents ==========================================================

    struct StringWriter final : pugi::xml_writer
    {
        std::string Text;
        void write(const void* InData, const size_t InSize) override
        {
            Text.append(static_cast<const char*>(InData), InSize);
        }
    };

    /* Fixed formatting, so the bytes depend on the data alone. */
    inline std::string DocumentText(const pugi::xml_document& InDoc)
    {
        StringWriter Writer;
        InDoc.save(Writer, "  ", pugi::format_indent, pugi::encoding_utf8);
        return Writer.Text;
    }

    inline pugi::xml_node AppendDeclaration(pugi::xml_document& InDoc)
    {
        pugi::xml_node Declaration = InDoc.append_child(pugi::node_declaration);
        Declaration.append_attribute("version")  = "1.0";
        Declaration.append_attribute("encoding") = "UTF-8";
        return Declaration;
    }

    /* InLogPrefix names the caller in the error: "WorldPackage", "Prefab". */
    inline bool ParseDocument(const std::string& InText, pugi::xml_document& OutDoc, const std::string& InPath,
                              const char* InLogPrefix)
    {
        const pugi::xml_parse_result Result = OutDoc.load_buffer(InText.data(), InText.size(),
                                                                 pugi::parse_default, pugi::encoding_utf8);
        if(!Result)
        {
            GAMECORE_LOG_ERROR(std::string(InLogPrefix) + ": '" + InPath + "' is not valid XML: " + Result.description()
                               + " (at byte " + std::to_string(Result.offset) + ")")
            return false;
        }
        return true;
    }

    // === Entity references ==================================================

    /* How an archive spells an EntityID field: <Target Guid="..."/> in a
     * world, <Target Entity="2"/> in a prefab. "" is always an unset id. */
    class XmlEntityRefs
    {
    public:
        virtual ~XmlEntityRefs() = default;

        virtual const char* GetAttributeName() const = 0;

        /* Saving: InEntity's text, "" if it is unset or has none. */
        virtual std::string Write(EntityID InEntity) const = 0;

        /* Loading non-empty text. False if it names nothing this load knows;
         * the archive warns and leaves the field unset. */
        virtual bool Read(const char* InText, EntityID& OutEntity) const = 0;

        /* For that warning: "this world", "this prefab". */
        virtual const char* GetScopeName() const = 0;
    };

    // === The text ComponentArchive ==========================================

    /* One element per field, value in attributes: <Width Value="32"/>,
     * <Position X="0" Y="360" Z="0"/>. Loaded by name; missing fields keep
     * defaults, unknown elements are ignored. */
    class XmlComponentArchive final : public ComponentArchive
    {
    public:
        /* InContext and InLogPrefix only label warnings. */
        XmlComponentArchive(const pugi::xml_node InNode, const bool bInLoading, const uint16_t InVersion,
                            const XmlEntityRefs& InRefs, std::string InContext, const char* InLogPrefix)
            : Node(InNode), bLoading(bInLoading), Version(InVersion), Refs(InRefs)
            , Context(std::move(InContext)), LogPrefix(InLogPrefix) {}

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

        /* "" for an unset reference or one to an entity that no longer
         * exists. */
        void Entity(const char* InName, EntityID& InOut) override
        {
            pugi::xml_node Element = Begin(InName);
            if(!Element) { return; }
            if(!bLoading)
            {
                Element.append_attribute(Refs.GetAttributeName()) = Refs.Write(InOut).c_str();
                return;
            }

            const char* Text = Element.attribute(Refs.GetAttributeName()).as_string();
            if(Text[0] == '\0')
            {
                InOut = InvalidEntityID();
                return;
            }
            if(!Refs.Read(Text, InOut))
            {
                GAMECORE_LOG_WARN(std::string(LogPrefix) + ": " + Context + ": '" + InName + "' refers to entity " + Text
                                  + ", which " + Refs.GetScopeName() + " does not contain; leaving it unset")
                InOut = InvalidEntityID();
            }
        }

    private:
        /* Save: the new element (null on an invalid name). Load: the element,
         * or null if absent. */
        pugi::xml_node Begin(const char* InName)
        {
            if(bLoading)
            {
                return Node.child(InName);
            }
            if(!IsElementName(InName))
            {
                GAMECORE_LOG_ERROR(std::string(LogPrefix) + ": " + Context + ": field name '" + (InName ? InName : "")
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
            GAMECORE_LOG_WARN(std::string(LogPrefix) + ": " + Context + ": field '" + InName
                              + "' has a missing or unreadable value; keeping its default")
        }

        pugi::xml_node       Node;
        bool                 bLoading = false;
        uint16_t             Version  = 0;
        const XmlEntityRefs& Refs;
        std::string          Context;
        const char*          LogPrefix;
    };
}
}

#endif // WITH_UNCOOKED

#endif //GAMECORE_XMLCOMPONENTARCHIVE_H

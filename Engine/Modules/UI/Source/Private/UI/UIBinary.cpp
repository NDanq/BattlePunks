#include "Public/UI/UIFile.h"

#include <cctype>
#include <cstring>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreFileProvider.h"

/*
 * The cooked .nmui: "NMUI", a version, then chunks (CoreArchive) -- SETT the
 * root's settings, WIDG the widgets, RSRV the reserved sections -- each a
 * list of strings. Text, not packed values: a property's text is what every
 * build parses already, and a property this build does not know stays as it
 * was written rather than breaking the file.
 */
namespace UI
{
    namespace
    {
        constexpr char     MAGIC[4]       = { 'N', 'M', 'U', 'I' };
        constexpr uint32_t BINARY_VERSION = 1;

        constexpr uint32_t Tag(const char InA, const char InB, const char InC, const char InD)
        {
            return uint32_t(uint8_t(InA)) | (uint32_t(uint8_t(InB)) << 8) | (uint32_t(uint8_t(InC)) << 16) | (uint32_t(uint8_t(InD)) << 24);
        }
        constexpr uint32_t TAG_SETTINGS = Tag('S', 'E', 'T', 'T');
        constexpr uint32_t TAG_WIDGETS  = Tag('W', 'I', 'D', 'G');
        constexpr uint32_t TAG_RESERVED = Tag('R', 'S', 'R', 'V');

        void SerializeTexts(EngineCore::Archive& Ar, std::vector<UIPropertyText>& InOutTexts)
        {
            uint32_t Count = uint32_t(InOutTexts.size());
            Ar << Count;
            if(Ar.IsLoading())
            {
                if(Count > 100000) { Ar.SetError("a property list is impossibly long"); return; }
                InOutTexts.resize(Count);
            }
            for(UIPropertyText& Each : InOutTexts)
            {
                Ar << Each.Name;
                Ar << Each.Value;
            }
        }
    }

    EUIFileFormat DetectUIFileFormat(const std::vector<uint8_t>& InBytes)
    {
        if(InBytes.size() >= 4 && std::memcmp(InBytes.data(), MAGIC, 4) == 0) return EUIFileFormat::Binary;
        size_t At = 0;
        if(InBytes.size() >= 3 && InBytes[0] == 0xEF && InBytes[1] == 0xBB && InBytes[2] == 0xBF) At = 3;   // a UTF-8 BOM
        while(At < InBytes.size() && std::isspace(InBytes[At])) ++At;
        return At < InBytes.size() && InBytes[At] == '<' ? EUIFileFormat::Text : EUIFileFormat::Unknown;
    }

    std::vector<uint8_t> WriteUIBinary(const UIDocumentAsset& InAsset)
    {
        UIDocumentAsset Asset = InAsset;
        Asset.ApplyStyles();

        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        char Magic[4];
        std::memcpy(Magic, MAGIC, 4);
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = BINARY_VERSION;
        Ar << Version;

        uint16_t ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_SETTINGS, ChunkVersion))
        {
            std::vector<UIPropertyText> Settings = Asset.Settings.ToText();
            SerializeTexts(Ar, Settings);
            Ar.CloseChunk();
        }
        ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_WIDGETS, ChunkVersion))
        {
            uint32_t Count = uint32_t(Asset.Widgets.size());
            Ar << Count;
            for(UIWidgetDesc& Each : Asset.Widgets)
            {
                Ar << Each.Type;
                Ar << Each.Parent;
                SerializeTexts(Ar, Each.Properties);
            }
            Ar.CloseChunk();
        }
        ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_RESERVED, ChunkVersion))
        {
            uint32_t Count = uint32_t(Asset.Reserved.size());
            Ar << Count;
            for(std::string& Each : Asset.Reserved) { Ar << Each; }
            Ar.CloseChunk();
        }
        return Ar.GetBytes();
    }

    bool ReadUIBinary(const std::vector<uint8_t>& InBytes, UIDocumentAsset& OutAsset, std::string* OutError)
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) *OutError = InWhy; return false; };
        if(DetectUIFileFormat(InBytes) != EUIFileFormat::Binary) return Fail("not a cooked .nmui");

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        char Magic[4];
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = 0;
        Ar << Version;
        if(Version == 0 || Version > BINARY_VERSION)
        {
            return Fail("cooked by a newer build (version " + std::to_string(Version) + ")");
        }

        UIDocumentAsset Asset;
        while(!Ar.HasError() && !Ar.IsAtEnd())
        {
            uint16_t ChunkVersion = 0;
            if(Ar.OpenChunk(TAG_SETTINGS, ChunkVersion))
            {
                std::vector<UIPropertyText> Settings;
                SerializeTexts(Ar, Settings);
                for(const UIPropertyText& Each : Settings) { Asset.Settings.SetFromText(Each.Name, Each.Value); }
                Ar.CloseChunk();
            }
            else if(Ar.OpenChunk(TAG_WIDGETS, ChunkVersion))
            {
                uint32_t Count = 0;
                Ar << Count;
                if(Count > 100000) { return Fail("an impossible widget count"); }
                Asset.Widgets.resize(Count);
                for(UIWidgetDesc& Each : Asset.Widgets)
                {
                    Ar << Each.Type;
                    Ar << Each.Parent;
                    SerializeTexts(Ar, Each.Properties);
                    if(Ar.HasError()) break;
                }
                Ar.CloseChunk();
            }
            else if(Ar.OpenChunk(TAG_RESERVED, ChunkVersion))
            {
                uint32_t Count = 0;
                Ar << Count;
                if(Count > 10000) { return Fail("an impossible reserved count"); }
                Asset.Reserved.resize(Count);
                for(std::string& Each : Asset.Reserved) { Ar << Each; }
                Ar.CloseChunk();
            }
            else
            {
                /* A chunk a newer build added: stepped over. */
                const uint32_t Unknown = Ar.PeekChunkTag();
                if(Unknown == 0 || !Ar.OpenChunk(Unknown, ChunkVersion)) break;
                Ar.CloseChunk();
            }
        }
        if(Ar.HasError()) return Fail("the cooked file is damaged");
        OutAsset = std::move(Asset);
        return true;
    }

    bool ReadUIDocument(const std::vector<uint8_t>& InBytes, UIDocumentAsset& OutAsset, std::string* OutError)
    {
        switch(DetectUIFileFormat(InBytes))
        {
            case EUIFileFormat::Binary:
                return ReadUIBinary(InBytes, OutAsset, OutError);
            case EUIFileFormat::Text:
#if WITH_EDITOR
                return ParseUIXml(std::string(InBytes.begin(), InBytes.end()), OutAsset, OutError);
#else
                if(OutError) *OutError = "is XML; a game reads only the cooked form ('cook ui' in the editor)";
                return false;
#endif
            case EUIFileFormat::Unknown:
                break;
        }
        if(OutError) *OutError = "is neither a .nmui document nor a cooked one";
        return false;
    }

    bool LoadUIDocument(const std::string& InPath, UIDocumentAsset& OutAsset, std::string* OutError)
    {
        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        std::vector<uint8_t> Bytes;
        if(!Provider || !Provider->ReadAll(InPath, Bytes))
        {
            if(OutError) *OutError = "cannot be read";
            return false;
        }
        return ReadUIDocument(Bytes, OutAsset, OutError);
    }
}

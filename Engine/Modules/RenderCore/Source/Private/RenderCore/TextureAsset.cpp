#include "Public/RenderCore/TextureAsset.h"

#include <mutex>
#include <unordered_map>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_TEXTURE = EngineCore::MakeArchiveTag('N','T','E','X');
        constexpr uint32_t TAG_HEADER  = EngineCore::MakeArchiveTag('T','H','D','R');
        constexpr uint32_t TAG_PIXELS  = EngineCore::MakeArchiveTag('T','P','I','X');

        /* THDR's pixel format byte. Only RGBA8 so far. */
        constexpr uint8_t FORMAT_RGBA8 = 0;

        /* THDR. False (archive failed) for unknown values. */
        bool SerializeHeader(EngineCore::Archive& Ar, TextureHeader& InOutHeader)
        {
            int32_t Width  = InOutHeader.Width;
            int32_t Height = InOutHeader.Height;
            uint8_t Format = FORMAT_RGBA8;
            uint8_t Filter = uint8_t(InOutHeader.Filter);
            uint8_t Wrap   = uint8_t(InOutHeader.Wrap);
            Ar << Width << Height << Format << InOutHeader.bSRGB << Filter << Wrap;

            if(Ar.IsLoading())
            {
                if(Width <= 0 || Height <= 0 || Width > TEXTURE_MAX_DIMENSION || Height > TEXTURE_MAX_DIMENSION)
                {
                    Ar.SetError("texture: implausible size");
                    return false;
                }
                if(Format != FORMAT_RGBA8 || Filter > uint8_t(ETextureFilter::Linear) || Wrap > uint8_t(ETextureWrap::Mirror))
                {
                    Ar.SetError("texture: unknown format, filter or wrap");
                    return false;
                }
                InOutHeader.Width  = Width;
                InOutHeader.Height = Height;
                InOutHeader.Filter = ETextureFilter(Filter);
                InOutHeader.Wrap   = ETextureWrap(Wrap);
            }
            return !Ar.HasError();
        }
    }

    bool SerializeTexture(EngineCore::Archive& Ar, TextureData& InOutTexture)
    {
        if(!OpenRequiredChunk(Ar, TAG_TEXTURE, 1, "not a texture (no NTEX chunk)")) { return false; }

        TextureHeader Header;
        if(Ar.IsSaving())
        {
            if(InOutTexture.Pixels.IsNull() || InOutTexture.Pixels.GetFormat() != EngineCore::EImageFormat::RGBA8)
            {
                Ar.SetError("texture: pixels must be RGBA8");
                Ar.CloseChunk();
                return false;
            }
            Header.Width  = InOutTexture.Pixels.GetWidth();
            Header.Height = InOutTexture.Pixels.GetHeight();
            Header.bSRGB  = InOutTexture.bSRGB;
            Header.Filter = InOutTexture.Filter;
            Header.Wrap   = InOutTexture.Wrap;
        }

        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "texture: no THDR chunk"))
        {
            SerializeHeader(Ar, Header);
            Ar.CloseChunk();
        }

        if(Ar.IsLoading() && !Ar.HasError())
        {
            InOutTexture.bSRGB  = Header.bSRGB;
            InOutTexture.Filter = Header.Filter;
            InOutTexture.Wrap   = Header.Wrap;
            if(CanHoldElements(Ar, uint64_t(Header.Width) * uint64_t(Header.Height), 4, "texture: pixels run past the end of the file"))
            {
                InOutTexture.Pixels = EngineCore::Image(Header.Width, Header.Height, EngineCore::EImageFormat::RGBA8);
            }
        }

        /* Raw bytes: RGBA8 has no byte order; rows are tightly packed. */
        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_PIXELS, 1, "texture: no TPIX chunk"))
        {
            Ar.SerializeBytes(InOutTexture.Pixels.GetData(), InOutTexture.Pixels.GetSizeBytes());
            Ar.CloseChunk();
        }

        if(!Ar.HasError())
        {
            SerializeImportInfo(Ar, InOutTexture.ImportInfo);
        }

        Ar.CloseChunk();
        return !Ar.HasError();
    }

    bool LoadTextureFile(const std::string& InPath, TextureData& OutTexture)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutTexture = TextureData();
        if(!SerializeTexture(Ar, OutTexture))
        {
            RENDERCORE_LOG_WARN("cannot load texture " + InPath)
            OutTexture = TextureData();
            return false;
        }
        return true;
    }

    bool LoadTextureHeader(const std::string& InPath, TextureHeader& OutHeader)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        if(!OpenRequiredChunk(Ar, TAG_TEXTURE, 1, "not a texture (no NTEX chunk)")) { return false; }
        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "texture: no THDR chunk"))
        {
            SerializeHeader(Ar, OutHeader);
            Ar.CloseChunk();
        }
        Ar.CloseChunk();
        return !Ar.HasError();
    }

#if WITH_EDITOR
    bool SaveTextureFile(const std::string& InPath, TextureData& InTexture)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeTexture(Ar, InTexture))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    // === TextureAssetCache ====================================================

    namespace
    {
        struct TextureCacheState
        {
            std::mutex                                                          Lock;
            std::unordered_map<std::string, std::shared_ptr<const TextureData>> Textures;   // null: failed
        };

        /* Function-local, so it exists before any static initialiser asks. */
        TextureCacheState& GetTextureCache()
        {
            static TextureCacheState State;
            return State;
        }
    }

    std::shared_ptr<const TextureData> TextureAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty())
        {
            return nullptr;
        }

        TextureCacheState& Cache = GetTextureCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Textures.find(InPath);
            if(Found != Cache.Textures.end())
            {
                return Found->second;
            }
        }

        /* Loaded outside the lock; if two threads race, the first stored wins. */
        auto Loaded = std::make_shared<TextureData>();
        std::shared_ptr<const TextureData> Result;
        if(LoadTextureFile(InPath, *Loaded))
        {
            Result = std::move(Loaded);
        }
        else
        {
            RENDERCORE_LOG_WARN("texture '" + InPath + "' cannot be loaded; a material sampling it reads transparent black")
        }

        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Textures.emplace(InPath, Result).first->second;
    }

    void TextureAssetCache::Invalidate(const std::string& InPath)
    {
        TextureCacheState& Cache = GetTextureCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(InPath.empty())
        {
            Cache.Textures.clear();
        }
        else
        {
            Cache.Textures.erase(InPath);
        }
    }
}

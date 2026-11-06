#include "Public/RenderCore/RenderTargetAsset.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_TARGET = EngineCore::MakeArchiveTag('N','R','T','A');
        constexpr uint32_t TAG_HEADER = EngineCore::MakeArchiveTag('R','T','H','D');
        constexpr uint32_t TAG_PIXELS = EngineCore::MakeArchiveTag('R','T','P','X');

        const char* const TRANSIENT_PREFIX = "transient://";

        bool EndsWith(const std::string& InText, const char* InSuffix)
        {
            const size_t Length = std::char_traits<char>::length(InSuffix);
            return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
        }

        /* RTHD. False (archive failed) for an implausible one. */
        bool SerializeDesc(EngineCore::Archive& Ar, RenderTargetDesc& InOutDesc)
        {
            uint8_t  Kind   = uint8_t(InOutDesc.Kind);
            uint32_t Width  = InOutDesc.Width;
            uint32_t Height = InOutDesc.Height;
            uint8_t  Filter = uint8_t(InOutDesc.Filter);
            uint8_t  Wrap   = uint8_t(InOutDesc.Wrap);
            uint8_t  Clear[4] = { InOutDesc.Clear.R, InOutDesc.Clear.G, InOutDesc.Clear.B, InOutDesc.Clear.A };
            Ar << Kind << Width << Height << Filter << Wrap << Clear[0] << Clear[1] << Clear[2] << Clear[3];
            if(Ar.IsLoading())
            {
                if(Kind > uint8_t(ERenderTargetKind::Cube) || Width == 0 || Height == 0 || Width > RENDER_TARGET_MAX_SIZE
                   || Height > RENDER_TARGET_MAX_SIZE || Filter > uint8_t(ETextureFilter::Linear) || Wrap > uint8_t(ETextureWrap::Mirror))
                {
                    Ar.SetError("render target: implausible settings");
                    return false;
                }
                InOutDesc.Kind   = ERenderTargetKind(Kind);
                InOutDesc.Width  = Width;
                InOutDesc.Height = InOutDesc.Kind == ERenderTargetKind::Cube ? Width : Height;
                InOutDesc.Filter = ETextureFilter(Filter);
                InOutDesc.Wrap   = ETextureWrap(Wrap);
                InOutDesc.Clear  = EngineCore::Color(Clear[0], Clear[1], Clear[2], Clear[3]);
            }
            return !Ar.HasError();
        }
    }

    bool KindOfRenderTargetPath(const std::string& InPath, ERenderTargetKind& OutKind)
    {
        if(EndsWith(InPath, ".nmrt2d")) { OutKind = ERenderTargetKind::Tex2D; return true; }
        if(EndsWith(InPath, ".nmrt3d")) { OutKind = ERenderTargetKind::Cube;  return true; }
        return false;
    }

    bool IsRenderTargetPath(const std::string& InPath)
    {
        ERenderTargetKind Kind;
        return KindOfRenderTargetPath(InPath, Kind) || InPath.rfind(TRANSIENT_PREFIX, 0) == 0;
    }

    bool SerializeRenderTarget(EngineCore::Archive& Ar, RenderTargetData& InOutTarget)
    {
        if(!OpenRequiredChunk(Ar, TAG_TARGET, 1, "not a render target (no NRTA chunk)")) { return false; }

        if(Ar.IsSaving())
        {
            uint16_t Version = 1;
            Ar.OpenChunk(TAG_HEADER, Version);
            SerializeDesc(Ar, InOutTarget.Desc);
            Ar.CloseChunk();
            if(!InOutTarget.Baked.IsNull())
            {
                if(InOutTarget.Baked.GetFormat() != EngineCore::EImageFormat::RGBA8)
                {
                    Ar.SetError("render target: baked pixels must be RGBA8");
                    Ar.CloseChunk();
                    return false;
                }
                int32_t Width  = InOutTarget.Baked.GetWidth();
                int32_t Height = InOutTarget.Baked.GetHeight();
                Version = 1;
                Ar.OpenChunk(TAG_PIXELS, Version);
                Ar << Width << Height;
                for(int32_t Row = 0; Row < Height; ++Row)
                {
                    Ar.SerializeBytes(InOutTarget.Baked.GetData() + InOutTarget.Baked.GetStride() * size_t(Row), size_t(Width) * 4);
                }
                Ar.CloseChunk();
            }
            Ar.CloseChunk();
            return !Ar.HasError();
        }

        InOutTarget = RenderTargetData();
        if(!OpenRequiredChunk(Ar, TAG_HEADER, 1, "render target: no RTHD chunk") || !SerializeDesc(Ar, InOutTarget.Desc))
        {
            return false;
        }
        Ar.CloseChunk();

        /* The optional bake, sized as the target's texture is. */
        uint16_t Version = 0;
        if(Ar.PeekChunkTag() == TAG_PIXELS && Ar.OpenChunk(TAG_PIXELS, Version))
        {
            int32_t Width = 0, Height = 0;
            Ar << Width << Height;
            if(Width != int32_t(RenderTargetTextureWidth(InOutTarget.Desc)) || Height != int32_t(RenderTargetTextureHeight(InOutTarget.Desc)))
            {
                Ar.SetError("render target: baked pixels do not fit its size");
                return false;
            }
            if(!CanHoldElements(Ar, uint64_t(Width) * uint64_t(Height), 4, "render target: baked pixels run past the end of the file"))
            {
                return false;
            }
            EngineCore::Image Pixels(Width, Height, EngineCore::EImageFormat::RGBA8);
            for(int32_t Row = 0; Row < Height; ++Row)
            {
                Ar.SerializeBytes(Pixels.GetData() + Pixels.GetStride() * size_t(Row), size_t(Width) * 4);
            }
            InOutTarget.Baked = std::move(Pixels);
            Ar.CloseChunk();
        }
        Ar.CloseChunk();
        return !Ar.HasError();
    }

    bool LoadRenderTargetFile(const std::string& InPath, RenderTargetData& OutTarget)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        if(!SerializeRenderTarget(Ar, OutTarget))
        {
            RENDERCORE_LOG_WARN("cannot load render target " + InPath)
            OutTarget = RenderTargetData();
            return false;
        }
        /* The extension decides the kind: a renamed file still means what it says. */
        ERenderTargetKind Kind;
        if(KindOfRenderTargetPath(InPath, Kind) && Kind != OutTarget.Desc.Kind)
        {
            RENDERCORE_LOG_WARN("render target " + InPath + " is stored as the other kind; using its extension's")
            OutTarget.Desc.Kind = Kind;
            if(Kind == ERenderTargetKind::Cube) { OutTarget.Desc.Height = OutTarget.Desc.Width; }
            OutTarget.Baked = EngineCore::Image();
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveRenderTargetFile(const std::string& InPath, RenderTargetData& InTarget)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeRenderTarget(Ar, InTarget))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    // === RenderTargetAssetCache ===============================================

    namespace
    {
        struct RenderTargetCacheState
        {
            std::mutex                                                               Lock;
            std::unordered_map<std::string, std::shared_ptr<const RenderTargetData>> Targets;   // null: failed
        };

        RenderTargetCacheState& GetRenderTargetCache()
        {
            static RenderTargetCacheState State;
            return State;
        }
    }

    std::shared_ptr<const RenderTargetData> RenderTargetAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty())
        {
            return nullptr;
        }
        RenderTargetCacheState& Cache = GetRenderTargetCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Targets.find(InPath);
            if(Found != Cache.Targets.end())
            {
                return Found->second;
            }
        }
        if(InPath.rfind(TRANSIENT_PREFIX, 0) == 0)
        {
            return nullptr;   // never made, or destroyed: nothing to load
        }

        auto Loaded = std::make_shared<RenderTargetData>();
        std::shared_ptr<const RenderTargetData> Result;
        if(LoadRenderTargetFile(InPath, *Loaded))
        {
            Result = std::move(Loaded);
        }
        else
        {
            RENDERCORE_LOG_WARN("render target '" + InPath + "' cannot be loaded; a material sampling it reads transparent black")
        }
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Targets.emplace(InPath, Result).first->second;
    }

    void RenderTargetAssetCache::Invalidate(const std::string& InPath)
    {
        RenderTargetCacheState& Cache = GetRenderTargetCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(!InPath.empty())
        {
            Cache.Targets.erase(InPath);
            return;
        }
        for(auto It = Cache.Targets.begin(); It != Cache.Targets.end();)
        {
            It = It->first.rfind(TRANSIENT_PREFIX, 0) == 0 ? std::next(It) : Cache.Targets.erase(It);
        }
    }

    std::string RenderTargetAssetCache::CreateTransient(const std::string& InName, const RenderTargetDesc& InDesc)
    {
        auto Data = std::make_shared<RenderTargetData>();
        Data->Desc        = InDesc;
        Data->Desc.Width  = std::clamp<uint32_t>(InDesc.Width, 1u, RENDER_TARGET_MAX_SIZE);
        Data->Desc.Height = InDesc.Kind == ERenderTargetKind::Cube ? Data->Desc.Width
                                                                   : std::clamp<uint32_t>(InDesc.Height, 1u, RENDER_TARGET_MAX_SIZE);
        const std::string Path = TRANSIENT_PREFIX + InName;
        RenderTargetCacheState& Cache = GetRenderTargetCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Targets[Path] = std::move(Data);
        return Path;
    }

    void RenderTargetAssetCache::DestroyTransient(const std::string& InName)
    {
        RenderTargetCacheState& Cache = GetRenderTargetCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(!InName.empty())
        {
            Cache.Targets.erase(TRANSIENT_PREFIX + InName);
            return;
        }
        for(auto It = Cache.Targets.begin(); It != Cache.Targets.end();)
        {
            It = It->first.rfind(TRANSIENT_PREFIX, 0) == 0 ? Cache.Targets.erase(It) : std::next(It);
        }
    }
}

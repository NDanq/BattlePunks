#include "Public/RenderCore/TextureAsset.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/TextureCompression.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_TEXTURE = EngineCore::MakeArchiveTag('N','T','E','X');
        constexpr uint32_t TAG_HEADER  = EngineCore::MakeArchiveTag('T','H','D','R');
        constexpr uint32_t TAG_PIXELS  = EngineCore::MakeArchiveTag('T','P','I','X');
        constexpr uint32_t TAG_MIPS    = EngineCore::MakeArchiveTag('T','M','I','P');
        constexpr uint16_t HEADER_VERSION = 2;

        /* THDR's pixel format byte: ETexturePixelFormat. */
        constexpr uint8_t FORMAT_MAX = uint8_t(ETexturePixelFormat::RGBE8);

        /* THDR. False (archive failed) for unknown values. */
        bool SerializeHeader(EngineCore::Archive& Ar, TextureHeader& InOutHeader)
        {
            int32_t Width  = InOutHeader.Width;
            int32_t Height = InOutHeader.Height;
            uint8_t Format = uint8_t(InOutHeader.Format);
            uint8_t Filter = uint8_t(InOutHeader.Filter);
            uint8_t Wrap   = uint8_t(InOutHeader.Wrap);
            uint8_t MipGen      = uint8_t(InOutHeader.MipGen);
            uint8_t Compression = uint8_t(InOutHeader.Compression);
            uint8_t MipCount    = uint8_t(std::clamp(InOutHeader.MipCount, 1, 255));
            Ar << Width << Height << Format << InOutHeader.bSRGB << Filter << Wrap;
            if(Ar.GetChunkVersion() >= 2)
            {
                Ar << MipGen << Compression << MipCount;
            }
            else
            {
                MipGen = 0; Compression = 0; MipCount = 1;
            }

            if(Ar.IsLoading())
            {
                if(Width <= 0 || Height <= 0 || Width > TEXTURE_MAX_DIMENSION || Height > TEXTURE_MAX_DIMENSION)
                {
                    Ar.SetError("texture: implausible size");
                    return false;
                }
                if(Format > FORMAT_MAX || Filter > uint8_t(ETextureFilter::Linear) || Wrap > uint8_t(ETextureWrap::Mirror))
                {
                    Ar.SetError("texture: unknown format, filter or wrap");
                    return false;
                }
                if(MipGen > uint8_t(ETextureMipGen::Kaiser) || Compression > uint8_t(ETextureCompression::BC7)
                   || MipCount < 1 || int(MipCount) > FullMipCount(Width, Height))
                {
                    Ar.SetError("texture: unknown mip generation or compression, or too many mips");
                    return false;
                }
                InOutHeader.Format      = ETexturePixelFormat(Format);
                InOutHeader.Width       = Width;
                InOutHeader.Height      = Height;
                InOutHeader.Filter      = ETextureFilter(Filter);
                InOutHeader.Wrap        = ETextureWrap(Wrap);
                InOutHeader.MipGen      = ETextureMipGen(MipGen);
                InOutHeader.Compression = ETextureCompression(Compression);
                InOutHeader.MipCount    = MipCount;
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
            Header.Format = InOutTexture.Format;
            Header.bSRGB  = InOutTexture.bSRGB;
            Header.Filter = InOutTexture.Filter;
            Header.Wrap   = InOutTexture.Wrap;
            Header.MipGen      = InOutTexture.MipGen;
            Header.Compression = InOutTexture.Compression;
            Header.MipCount    = InOutTexture.GetMipCount();
            int Expected = Header.Width, ExpectedH = Header.Height;
            for(const EngineCore::Image& Level : InOutTexture.Mips)
            {
                Expected = std::max(Expected / 2, 1); ExpectedH = std::max(ExpectedH / 2, 1);
                if(Level.GetWidth() != Expected || Level.GetHeight() != ExpectedH || Level.GetFormat() != EngineCore::EImageFormat::RGBA8)
                {
                    Ar.SetError("texture: a mip is not half the last, or not RGBA8");
                    Ar.CloseChunk();
                    return false;
                }
            }
        }

        if(OpenRequiredChunk(Ar, TAG_HEADER, HEADER_VERSION, "texture: no THDR chunk"))
        {
            SerializeHeader(Ar, Header);
            Ar.CloseChunk();
        }

        if(Ar.IsLoading() && !Ar.HasError())
        {
            InOutTexture.Format = Header.Format;
            InOutTexture.bSRGB  = Header.bSRGB;
            InOutTexture.Filter = Header.Filter;
            InOutTexture.Wrap   = Header.Wrap;
            InOutTexture.MipGen      = Header.MipGen;
            InOutTexture.Compression = Header.Compression;
            InOutTexture.Mips.clear();
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

        /* The rest of the chain, if any: each level's size follows from the last. */
        if(!Ar.HasError() && Header.MipCount > 1)
        {
            if(OpenRequiredChunk(Ar, TAG_MIPS, 1, "texture: THDR counts mips but there is no TMIP chunk"))
            {
                int Width = Header.Width, Height = Header.Height;
                for(int Level = 1; Level < Header.MipCount && !Ar.HasError(); ++Level)
                {
                    Width  = std::max(Width / 2, 1);
                    Height = std::max(Height / 2, 1);
                    if(Ar.IsLoading())
                    {
                        if(!CanHoldElements(Ar, uint64_t(Width) * uint64_t(Height), 4, "texture: mips run past the end of the file")) { break; }
                        InOutTexture.Mips.emplace_back(Width, Height, EngineCore::EImageFormat::RGBA8);
                    }
                    EngineCore::Image& Mip = InOutTexture.Mips[size_t(Level - 1)];
                    Ar.SerializeBytes(Mip.GetData(), Mip.GetSizeBytes());
                }
                Ar.CloseChunk();
            }
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
        if(OpenRequiredChunk(Ar, TAG_HEADER, HEADER_VERSION, "texture: no THDR chunk"))
        {
            SerializeHeader(Ar, OutHeader);
            Ar.CloseChunk();
        }
        Ar.CloseChunk();
        return !Ar.HasError();
    }

    const EngineCore::Image* TextureData::GetMip(const int InLevel) const
    {
        if(InLevel == 0) { return &Pixels; }
        return InLevel > 0 && size_t(InLevel) <= Mips.size() ? &Mips[size_t(InLevel - 1)] : nullptr;
    }

    const char* GetTextureMipGenName(const ETextureMipGen InMipGen)
    {
        switch(InMipGen)
        {
            case ETextureMipGen::None:   return "None";
            case ETextureMipGen::Box:    return "Box";
            case ETextureMipGen::Kaiser: return "Kaiser";
        }
        return "";
    }

    const char* GetTextureCompressionName(const ETextureCompression InCompression)
    {
        switch(InCompression)
        {
            case ETextureCompression::None: return "None";
            case ETextureCompression::BC1:  return "BC1";
            case ETextureCompression::BC3:  return "BC3";
            case ETextureCompression::BC4:  return "BC4";
            case ETextureCompression::BC5:  return "BC5";
            case ETextureCompression::BC7:  return "BC7";
        }
        return "";
    }

    int FullMipCount(int InWidth, int InHeight)
    {
        int Count = 1;
        while(InWidth > 1 || InHeight > 1)
        {
            InWidth  = std::max(InWidth / 2, 1);
            InHeight = std::max(InHeight / 2, 1);
            ++Count;
        }
        return Count;
    }

    namespace
    {
        /* A level as linear floats, RGBA. */
        struct FloatLevel
        {
            int                Width  = 0;
            int                Height = 0;
            std::vector<float> Texels;

            const float* At(const int InX, const int InY) const { return &Texels[(size_t(InY) * size_t(Width) + size_t(InX)) * 4]; }
        };

        float SrgbToLinear(const float InValue)
        {
            return InValue <= 0.04045f ? InValue / 12.92f : std::pow((InValue + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(const float InValue)
        {
            return InValue <= 0.0031308f ? InValue * 12.92f : 1.055f * std::pow(InValue, 1.0f / 2.4f) - 0.055f;
        }

        FloatLevel ToFloat(const EngineCore::Image& InImage, const bool bInSRGB)
        {
            float Table[256];
            for(int Each = 0; Each < 256; ++Each) { Table[Each] = bInSRGB ? SrgbToLinear(float(Each) / 255.0f) : float(Each) / 255.0f; }
            FloatLevel Out;
            Out.Width  = InImage.GetWidth();
            Out.Height = InImage.GetHeight();
            Out.Texels.resize(size_t(Out.Width) * size_t(Out.Height) * 4);
            for(int Y = 0; Y < Out.Height; ++Y)
            {
                const uint8_t* Row = InImage.GetScanline(Y);
                float*         Dst = &Out.Texels[size_t(Y) * size_t(Out.Width) * 4];
                for(int X = 0; X < Out.Width * 4; X += 4)
                {
                    Dst[X + 0] = Table[Row[X + 0]];
                    Dst[X + 1] = Table[Row[X + 1]];
                    Dst[X + 2] = Table[Row[X + 2]];
                    Dst[X + 3] = float(Row[X + 3]) / 255.0f;
                }
            }
            return Out;
        }

        EngineCore::Image ToImage(const FloatLevel& InLevel, const bool bInSRGB)
        {
            EngineCore::Image Out(InLevel.Width, InLevel.Height, EngineCore::EImageFormat::RGBA8);
            const auto Quantise = [](const float InValue) { return uint8_t(std::lround(std::clamp(InValue, 0.0f, 1.0f) * 255.0f)); };
            for(int Y = 0; Y < InLevel.Height; ++Y)
            {
                uint8_t* Row = Out.GetScanline(Y);
                for(int X = 0; X < InLevel.Width; ++X)
                {
                    const float* Src = InLevel.At(X, Y);
                    for(int Channel = 0; Channel < 3; ++Channel)
                    {
                        const float Value = std::clamp(Src[Channel], 0.0f, 1.0f);
                        Row[X * 4 + Channel] = Quantise(bInSRGB ? LinearToSrgb(Value) : Value);
                    }
                    Row[X * 4 + 3] = Quantise(Src[3]);
                }
            }
            return Out;
        }

        /* InIndex into [0, InSize) by the texture's wrap. */
        int Address(int InIndex, const int InSize, const ETextureWrap InWrap)
        {
            if(InIndex >= 0 && InIndex < InSize) { return InIndex; }
            switch(InWrap)
            {
                case ETextureWrap::Repeat:
                    InIndex %= InSize;
                    return InIndex < 0 ? InIndex + InSize : InIndex;
                case ETextureWrap::Mirror:
                {
                    const int Period = InSize * 2;
                    InIndex %= Period;
                    if(InIndex < 0) { InIndex += Period; }
                    return InIndex < InSize ? InIndex : Period - 1 - InIndex;
                }
                case ETextureWrap::Clamp:
                    break;
            }
            return std::clamp(InIndex, 0, InSize - 1);
        }

        double BesselI0(const double InX)
        {
            double Sum = 1.0, Term = 1.0;
            for(int K = 1; K < 32; ++K)
            {
                Term *= (InX / (2.0 * K)) * (InX / (2.0 * K));
                Sum  += Term;
                if(Term < Sum * 1e-12) { break; }
            }
            return Sum;
        }

        /*
         * The weights for one output texel from the input row: a box takes
         * its two texels (one where the axis is already 1); Kaiser takes six,
         * a Kaiser-windowed sinc (alpha 4) at half the input's frequency.
         * Offsets are input texels from 2 * output.
         */
        struct Kernel
        {
            int   First = 0;
            int   Count = 0;
            float Weights[6] = {};
        };

        Kernel MakeKernel(const ETextureMipGen InMipGen, const bool bInHalves)
        {
            Kernel Out;
            if(!bInHalves)
            {
                Out.First = 0; Out.Count = 1; Out.Weights[0] = 1.0f;   // an axis already 1 wide
                return Out;
            }
            if(InMipGen == ETextureMipGen::Box)
            {
                Out.First = 0; Out.Count = 2; Out.Weights[0] = Out.Weights[1] = 0.5f;
                return Out;
            }
            constexpr double Pi = 3.14159265358979323846, Alpha = 4.0, Support = 3.0;   // in input texels
            Out.First = -2;
            Out.Count = 6;
            double Total = 0.0;
            for(int Tap = 0; Tap < 6; ++Tap)
            {
                const double Distance = (Out.First + Tap + 0.5) - 1.0;   // from the output texel's centre, in input texels
                const double T        = Distance / 2.0;                  // in output texels
                const double Sinc     = T == 0.0 ? 1.0 : std::sin(Pi * T) / (Pi * T);
                const double Ratio    = Distance / Support;
                const double Window   = std::abs(Ratio) >= 1.0 ? 0.0 : BesselI0(Alpha * std::sqrt(1.0 - Ratio * Ratio)) / BesselI0(Alpha);
                Out.Weights[Tap] = float(Sinc * Window);
                Total += Out.Weights[Tap];
            }
            for(int Tap = 0; Tap < 6; ++Tap) { Out.Weights[Tap] = float(Out.Weights[Tap] / Total); }
            return Out;
        }

        /* InLevel at half size, separably: across, then down. */
        FloatLevel Downsample(const FloatLevel& InLevel, const ETextureMipGen InMipGen, const ETextureWrap InWrap)
        {
            const int    Width   = std::max(InLevel.Width / 2, 1);
            const int    Height  = std::max(InLevel.Height / 2, 1);
            const Kernel AcrossK = MakeKernel(InMipGen, InLevel.Width > 1);
            const Kernel DownK   = MakeKernel(InMipGen, InLevel.Height > 1);
            const int    StepX   = InLevel.Width > 1 ? 2 : 1;
            const int    StepY   = InLevel.Height > 1 ? 2 : 1;

            FloatLevel Across;
            Across.Width  = Width;
            Across.Height = InLevel.Height;
            Across.Texels.assign(size_t(Width) * size_t(InLevel.Height) * 4, 0.0f);
            for(int Y = 0; Y < InLevel.Height; ++Y)
            {
                for(int X = 0; X < Width; ++X)
                {
                    float* Dst = &Across.Texels[(size_t(Y) * size_t(Width) + size_t(X)) * 4];
                    for(int Tap = 0; Tap < AcrossK.Count; ++Tap)
                    {
                        const float* Src = InLevel.At(Address(X * StepX + AcrossK.First + Tap, InLevel.Width, InWrap), Y);
                        for(int Channel = 0; Channel < 4; ++Channel) { Dst[Channel] += Src[Channel] * AcrossK.Weights[Tap]; }
                    }
                }
            }

            FloatLevel Out;
            Out.Width  = Width;
            Out.Height = Height;
            Out.Texels.assign(size_t(Width) * size_t(Height) * 4, 0.0f);
            for(int Y = 0; Y < Height; ++Y)
            {
                for(int X = 0; X < Width; ++X)
                {
                    float* Dst = &Out.Texels[(size_t(Y) * size_t(Width) + size_t(X)) * 4];
                    for(int Tap = 0; Tap < DownK.Count; ++Tap)
                    {
                        const float* Src = Across.At(X, Address(Y * StepY + DownK.First + Tap, InLevel.Height, InWrap));
                        for(int Channel = 0; Channel < 4; ++Channel) { Dst[Channel] += Src[Channel] * DownK.Weights[Tap]; }
                    }
                }
            }
            return Out;
        }
    }

    void DecodeRgbe(const uint8_t* InTexel, float (&OutRgb)[3])
    {
        if(InTexel[3] == 0) { OutRgb[0] = OutRgb[1] = OutRgb[2] = 0.0f; return; }
        const float Scale = std::ldexp(1.0f, int(InTexel[3]) - 136);   // 2^(e - 128) / 256
        for(int Channel = 0; Channel < 3; ++Channel) { OutRgb[Channel] = (float(InTexel[Channel]) + 0.5f) * Scale; }
    }

    void EncodeRgbe(const float (&InRgb)[3], uint8_t* OutTexel)
    {
        const float Largest = std::max({ InRgb[0], InRgb[1], InRgb[2] });
        if(Largest < 1e-32f) { OutTexel[0] = OutTexel[1] = OutTexel[2] = OutTexel[3] = 0; return; }
        int Exponent = 0;
        const float Mantissa = std::frexp(Largest, &Exponent) * 256.0f / Largest;   // Largest = m * 2^e, m in [0.5, 1)
        for(int Channel = 0; Channel < 3; ++Channel)
        {
            OutTexel[Channel] = uint8_t(std::clamp(int(std::max(InRgb[Channel], 0.0f) * Mantissa), 0, 255));
        }
        OutTexel[3] = uint8_t(std::clamp(Exponent + 128, 0, 255));
    }

    void BuildTextureMips(TextureData& InOutTexture)
    {
        InOutTexture.Mips.clear();
        /* RGBE8 is decoded by its shader, point sampled: no chain. */
        if(InOutTexture.MipGen == ETextureMipGen::None || InOutTexture.Pixels.IsNull()
           || InOutTexture.Format != ETexturePixelFormat::RGBA8) { return; }
        const EngineCore::Image Source = InOutTexture.Pixels.ConvertedTo(EngineCore::EImageFormat::RGBA8);
        FloatLevel Level = ToFloat(Source, InOutTexture.bSRGB);
        while(Level.Width > 1 || Level.Height > 1)
        {
            Level = Downsample(Level, InOutTexture.MipGen, InOutTexture.Wrap);   // each from the last, in float
            InOutTexture.Mips.push_back(ToImage(Level, InOutTexture.bSRGB));
        }
    }

    uint64_t TextureGpuBytes(const TextureData& InTexture)
    {
        uint64_t Bytes = InTexture.Pixels.GetSizeBytes();
        for(const EngineCore::Image& Level : InTexture.Mips) { Bytes += Level.GetSizeBytes(); }
        return Bytes;
    }

    uint64_t TextureCompressedBytes(int InWidth, int InHeight, const int InMipCount, const ETextureCompression InCompression)
    {
        uint64_t Bytes = 0;
        for(int Level = 0; Level < std::max(InMipCount, 1); ++Level)
        {
            if(InCompression == ETextureCompression::None)
            {
                Bytes += uint64_t(InWidth) * uint64_t(InHeight) * 4;
            }
            else
            {
                const uint64_t Blocks = uint64_t((InWidth + 3) / 4) * uint64_t((InHeight + 3) / 4);
                const bool     bHalf  = InCompression == ETextureCompression::BC1 || InCompression == ETextureCompression::BC4;
                Bytes += Blocks * (bHalf ? 8 : 16);
            }
            InWidth  = std::max(InWidth / 2, 1);
            InHeight = std::max(InHeight / 2, 1);
        }
        return Bytes;
    }

    namespace
    {
        ITextureCompressor* GCompressor = nullptr;
    }

    void SetTextureCompressor(ITextureCompressor* InCompressor) { GCompressor = InCompressor; }
    ITextureCompressor* GetTextureCompressor() { return GCompressor; }

    bool IsTextureCompressionAvailable(const ETextureCompression InCompression)
    {
        return InCompression == ETextureCompression::None || (GCompressor && GCompressor->Supports(InCompression));
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

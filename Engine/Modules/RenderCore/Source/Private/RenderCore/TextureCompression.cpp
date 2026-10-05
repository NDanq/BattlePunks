#include "Public/RenderCore/TextureCompression.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "Public/Core/CoreJob.h"

#include "bc7decomp.h"
#include "rgbcx.h"
#if WITH_EDITOR
#include "bc7enc.h"
#endif

namespace RenderCore
{
    namespace
    {
        /* The 4x4 texels of block (InBlockX, InBlockY), RGBA8, row by row: past
         * the right or bottom edge, the edge texel again. */
        void GatherBlock(const EngineCore::Image& InLevel, const int InBlockX, const int InBlockY, uint8_t (&OutTexels)[64])
        {
            for(int Y = 0; Y < 4; ++Y)
            {
                const uint8_t* Row = InLevel.GetScanline(std::min(InBlockY * 4 + Y, InLevel.GetHeight() - 1));
                for(int X = 0; X < 4; ++X)
                {
                    const int Column = std::min(InBlockX * 4 + X, InLevel.GetWidth() - 1);
                    std::memcpy(&OutTexels[(Y * 4 + X) * 4], &Row[Column * 4], 4);
                }
            }
        }

        /* rgbcx's tables: once, before any block is encoded or decoded. */
        void InitRgbcx()
        {
            static std::once_flag Once;
            std::call_once(Once, [] { rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); });
        }

#if WITH_EDITOR
        /*
         * bc7enc_rdo: rgbcx for BC1-BC5, bc7enc for BC7. Quality over speed
         * where it is cheap -- BC7 tries every mode-1 partition -- since this
         * runs at import and save, not at load. sRGB colour is weighed
         * perceptually; linear data (masks, normals) channel for channel.
         */
        class Bc7EncCompressor final : public ITextureCompressor
        {
        public:
            const char* GetName() const override { return "bc7enc_rdo"; }

            bool Supports(const ETextureCompression InCompression) const override
            {
                return InCompression != ETextureCompression::None;
            }

            bool Compress(const EngineCore::Image& InLevel, const bool bInSRGB, const ETextureCompression InCompression,
                          std::vector<uint8_t>& OutBlocks, std::string& OutError) override
            {
                if(InLevel.IsNull() || InLevel.GetFormat() != EngineCore::EImageFormat::RGBA8)
                {
                    OutError = "bc7enc_rdo: the level is not RGBA8";
                    return false;
                }
                if(!Supports(InCompression))
                {
                    OutError = "bc7enc_rdo: nothing to encode for None";
                    return false;
                }
                Init();

                bc7enc_compress_block_params Bc7;
                bc7enc_compress_block_params_init(&Bc7);
                if(!bInSRGB) { bc7enc_compress_block_params_init_linear_weights(&Bc7); }

                const int      BlocksX    = (InLevel.GetWidth() + 3) / 4;
                const int      BlocksY    = (InLevel.GetHeight() + 3) / 4;
                const size_t   BlockBytes = size_t(TextureBlockBytes(InCompression));
                OutBlocks.assign(size_t(BlocksX) * size_t(BlocksY) * BlockBytes, 0);

                /* A row of blocks at a time. */
                const auto EncodeRows = [&](const uint32_t InBegin, const uint32_t InEnd)
                {
                    uint8_t Texels[64];
                    for(uint32_t BlockY = InBegin; BlockY < InEnd; ++BlockY)
                    {
                        for(int BlockX = 0; BlockX < BlocksX; ++BlockX)
                        {
                            GatherBlock(InLevel, BlockX, int(BlockY), Texels);
                            uint8_t* Out = &OutBlocks[(size_t(BlockY) * size_t(BlocksX) + size_t(BlockX)) * BlockBytes];
                            switch(InCompression)
                            {
                                /* No 3-colour blocks: their black would read transparent where BC1 is RGBA. */
                                case ETextureCompression::BC1: rgbcx::encode_bc1(BC1_LEVEL, Out, Texels, false, false); break;
                                case ETextureCompression::BC3: rgbcx::encode_bc3_hq(BC1_LEVEL, Out, Texels); break;
                                case ETextureCompression::BC4: rgbcx::encode_bc4_hq(Out, Texels); break;
                                case ETextureCompression::BC5: rgbcx::encode_bc5_hq(Out, Texels); break;
                                case ETextureCompression::BC7: bc7enc_compress_block(Out, Texels, &Bc7); break;
                                case ETextureCompression::None: break;
                            }
                        }
                    }
                };
                if(EngineCore::JobSystem* Jobs = EngineCore::GetJobSystem())
                {
                    Jobs->ParallelFor(uint32_t(BlocksY), 4, EncodeRows);
                }
                else
                {
                    EncodeRows(0, uint32_t(BlocksY));
                }
                return true;
            }

        private:
            /* rgbcx's level, 0..18: a little short of its slowest. */
            static constexpr uint32_t BC1_LEVEL = 16;

            static void Init()
            {
                InitRgbcx();
                static std::once_flag Once;
                std::call_once(Once, [] { bc7enc_compress_block_init(); });
            }
        };

        Bc7EncCompressor GBuiltInCompressor;
        ITextureCompressor* GCompressor = &GBuiltInCompressor;
#else
        ITextureCompressor* GCompressor = nullptr;
#endif
    }

    void SetTextureCompressor(ITextureCompressor* InCompressor) { GCompressor = InCompressor; }
    ITextureCompressor* GetTextureCompressor() { return GCompressor; }

    bool IsTextureCompressionAvailable(const ETextureCompression InCompression)
    {
        return InCompression == ETextureCompression::None || (GCompressor && GCompressor->Supports(InCompression));
    }

    int TextureBlockBytes(const ETextureCompression InCompression)
    {
        switch(InCompression)
        {
            case ETextureCompression::None: return 0;
            case ETextureCompression::BC1:
            case ETextureCompression::BC4:  return 8;
            case ETextureCompression::BC3:
            case ETextureCompression::BC5:
            case ETextureCompression::BC7:  return 16;
        }
        return 0;
    }

    bool TextureBlocksMatch(const TextureData& InTexture)
    {
        if(InTexture.Blocks.empty() || int(InTexture.Blocks.size()) != InTexture.GetMipCount()
           || InTexture.Compression == ETextureCompression::None) { return false; }
        for(int Level = 0; Level < InTexture.GetMipCount(); ++Level)
        {
            const EngineCore::Image* Mip  = InTexture.GetMip(Level);
            const TextureBlockLevel& Each = InTexture.Blocks[size_t(Level)];
            if(!Mip || Mip->GetWidth() != Each.Width || Mip->GetHeight() != Each.Height
               || Each.Bytes.size() != TextureCompressedBytes(Each.Width, Each.Height, 1, InTexture.Compression))
            {
                return false;
            }
        }
        return true;
    }

    bool CompressTexture(TextureData& InOutTexture, std::string& OutError)
    {
        InOutTexture.Blocks.clear();
        if(InOutTexture.Compression == ETextureCompression::None || InOutTexture.Format != ETexturePixelFormat::RGBA8)
        {
            return true;
        }
        if(!IsTextureCompressionAvailable(InOutTexture.Compression))
        {
            OutError = std::string("no encoder for ") + GetTextureCompressionName(InOutTexture.Compression);
            return false;
        }

        std::vector<TextureBlockLevel> Blocks(size_t(InOutTexture.GetMipCount()));
        for(int Level = 0; Level < InOutTexture.GetMipCount(); ++Level)
        {
            const EngineCore::Image& Mip = *InOutTexture.GetMip(Level);
            TextureBlockLevel& Each = Blocks[size_t(Level)];
            Each.Width  = Mip.GetWidth();
            Each.Height = Mip.GetHeight();
            if(!GCompressor->Compress(Mip.ConvertedTo(EngineCore::EImageFormat::RGBA8), InOutTexture.bSRGB,
                                      InOutTexture.Compression, Each.Bytes, OutError))
            {
                return false;
            }
            if(Each.Bytes.size() != TextureCompressedBytes(Each.Width, Each.Height, 1, InOutTexture.Compression))
            {
                OutError = std::string(GCompressor->GetName()) + " gave the wrong number of bytes";
                return false;
            }
        }
        InOutTexture.Blocks = std::move(Blocks);
        return true;
    }

    bool DecodeTextureBlocks(const TextureBlockLevel& InLevel, const ETextureCompression InCompression, EngineCore::Image& OutImage)
    {
        if(InCompression == ETextureCompression::None || InLevel.Width <= 0 || InLevel.Height <= 0
           || InLevel.Bytes.size() != TextureCompressedBytes(InLevel.Width, InLevel.Height, 1, InCompression))
        {
            return false;
        }
        InitRgbcx();

        const int    BlocksX    = (InLevel.Width + 3) / 4;
        const int    BlocksY    = (InLevel.Height + 3) / 4;
        const size_t BlockBytes = size_t(TextureBlockBytes(InCompression));
        OutImage = EngineCore::Image(InLevel.Width, InLevel.Height, EngineCore::EImageFormat::RGBA8);
        for(int BlockY = 0; BlockY < BlocksY; ++BlockY)
        {
            for(int BlockX = 0; BlockX < BlocksX; ++BlockX)
            {
                const uint8_t* Block = &InLevel.Bytes[(size_t(BlockY) * size_t(BlocksX) + size_t(BlockX)) * BlockBytes];
                /* As the GPU reads them: what a format lacks is 0, alpha 1. */
                uint8_t Texels[64] = {};
                for(int Each = 3; Each < 64; Each += 4) { Texels[Each] = 255; }
                switch(InCompression)
                {
                    case ETextureCompression::BC1: rgbcx::unpack_bc1(Block, Texels, false); break;
                    case ETextureCompression::BC3: rgbcx::unpack_bc3(Block, Texels); break;
                    case ETextureCompression::BC4: rgbcx::unpack_bc4(Block, Texels); break;
                    case ETextureCompression::BC5: rgbcx::unpack_bc5(Block, Texels); break;
                    case ETextureCompression::BC7:
                        if(!bc7decomp::unpack_bc7(Block, reinterpret_cast<bc7decomp::color_rgba*>(Texels))) { return false; }
                        break;
                    case ETextureCompression::None: break;
                }
                /* Only the texels inside the level. */
                for(int Y = 0; Y < 4 && BlockY * 4 + Y < InLevel.Height; ++Y)
                {
                    const int Columns = std::min(4, InLevel.Width - BlockX * 4);
                    std::memcpy(OutImage.GetScanline(BlockY * 4 + Y) + BlockX * 16, &Texels[Y * 16], size_t(Columns) * 4);
                }
            }
        }
        return true;
    }
}

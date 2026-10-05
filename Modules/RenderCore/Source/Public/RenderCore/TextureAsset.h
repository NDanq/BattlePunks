#ifndef RENDERCORE_TEXTUREASSET_H
#define RENDERCORE_TEXTUREASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreImage.h"
#include "Public/RenderCore/AssetImportInfo.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

/*
 * .nmtexture -- a pre-decoded 2D texture:
 *
 *   NTEX v1                the file
 *     THDR v3              width, height, pixel format (RGBA8, or RGBE8 for HDR), sRGB, filter, wrap;
 *                          v2: mip generation, compression, mip count; v3: whether TBLK follows
 *     TPIX v1              level 0: tightly packed rows (CoreImage.h)
 *     TMIP v1 (optional)   levels 1..n-1, each half the last (at least 1),
 *                          tightly packed, one after another
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *     TBLK v1 (optional)   the compression again, then every level as its 4x4
 *                          blocks (TextureCompression.h), level 0 first
 *
 * The RGBA8 pixels are the source and always kept, so a setting changed in
 * the editor encodes again from them, not from the blocks. TBLK is last so a
 * build from before it reads the file as RGBA8. Without TBLK, compression
 * is a setting only: the texture is drawn as RGBA8. A v1 file reads as no
 * mips, no compression; v2 as no blocks.
 */
namespace RenderCore
{
    /* Stored as numbers: GDI's own enums never reach the file. */
    enum class ETextureFilter : uint8_t { Nearest = 0, Linear = 1 };
    enum class ETextureWrap   : uint8_t { Repeat = 0, Clamp = 1, Mirror = 2 };

    /* How the mip chain is made from level 0 (BuildTextureMips): none, a
     * 2x2 average, or a sharper windowed-sinc (Kaiser) downsample. */
    enum class ETextureMipGen : uint8_t { None = 0, Box = 1, Kaiser = 2 };

    /* GPU block compression the texture asks for: what TextureData::Blocks
     * are, when there are any (TextureCompression.h). */
    enum class ETextureCompression : uint8_t { None = 0, BC1 = 1, BC3 = 2, BC4 = 3, BC5 = 4, BC7 = 5 };

    /* How a texel's four bytes read. RGBE8: Radiance's shared exponent, HDR
     * colour = rgb / 256 * 2^(e - 128) (DecodeRgbe) -- what a sky's HDRI is
     * stored as; shaders decode it (IBLCommon.glsl), so it is sampled point. */
    enum class ETexturePixelFormat : uint8_t { RGBA8 = 0, RGBE8 = 1 };

    /* One RGBE8 texel to linear RGB, and back (the nearest). */
    RENDERCORE_API void DecodeRgbe(const uint8_t* InTexel, float (&OutRgb)[3]);
    RENDERCORE_API void EncodeRgbe(const float (&InRgb)[3], uint8_t* OutTexel);

    enum : int { TEXTURE_MAX_DIMENSION = 16384 };

    /* One level as its compression's 4x4 blocks, row by row; a partial
     * block at the right or bottom edge repeats the edge texels. */
    struct TextureBlockLevel
    {
        int                  Width  = 0;   // in texels
        int                  Height = 0;
        std::vector<uint8_t> Bytes;
    };

    /* What LoadTextureFile reads: everything, or what the GPU is given -- the
     * blocks instead of the pixels when the file has them (Pixels and Mips
     * then stay empty). */
    enum class ETextureLoad : uint8_t { Everything = 0, ForGpu = 1 };

    struct TextureData
    {
        EngineCore::Image Pixels;            // RGBA8 bytes (RGBE8's too)
        ETexturePixelFormat Format = ETexturePixelFormat::RGBA8;
        bool              bSRGB  = true;     // colour data; false for masks, normals
        ETextureFilter    Filter = ETextureFilter::Linear;
        ETextureWrap      Wrap   = ETextureWrap::Repeat;
        ETextureMipGen      MipGen      = ETextureMipGen::None;
        ETextureCompression Compression = ETextureCompression::None;
        std::vector<EngineCore::Image> Mips;   // levels 1.. (level 0 is Pixels); empty with MipGen None
        /* Every level encoded as Compression, what the GPU is given; empty
         * if it is None or nothing encoded them. Made from Pixels and Mips
         * (CompressTexture), so whatever changes those clears these. */
        std::vector<TextureBlockLevel> Blocks;
        AssetImportInfo   ImportInfo;

        /* Levels in all, level 0 included. */
        int GetMipCount() const { return 1 + int(Mips.size()); }
        /* Level InLevel (0: Pixels); null past the last. */
        RENDERCORE_API const EngineCore::Image* GetMip(int InLevel) const;
    };

    struct TextureHeader
    {
        int            Width  = 0;
        int            Height = 0;
        ETexturePixelFormat Format = ETexturePixelFormat::RGBA8;
        bool           bSRGB  = true;
        ETextureFilter Filter = ETextureFilter::Linear;
        ETextureWrap   Wrap   = ETextureWrap::Repeat;
        ETextureMipGen      MipGen      = ETextureMipGen::None;
        ETextureCompression Compression = ETextureCompression::None;
        int                 MipCount    = 1;
        bool                bBlocks     = false;   // TBLK follows
    };

    /* Loads everything. Saving needs Blocks empty or matching: one per level,
     * Compression's size. */
    RENDERCORE_API bool SerializeTexture(EngineCore::Archive& Ar, TextureData& InOutTexture);
    RENDERCORE_API bool LoadTextureFile(const std::string& InPath, TextureData& OutTexture,
                                        ETextureLoad InLoad = ETextureLoad::Everything);
    RENDERCORE_API bool LoadTextureHeader(const std::string& InPath, TextureHeader& OutHeader);

    /* "None", "Box", "Kaiser"; "None", "BC1", ... */
    RENDERCORE_API const char* GetTextureMipGenName(ETextureMipGen InMipGen);
    RENDERCORE_API const char* GetTextureCompressionName(ETextureCompression InCompression);

    /* Levels a full chain of InWidth x InHeight has, down to 1x1. */
    RENDERCORE_API int FullMipCount(int InWidth, int InHeight);

    /*
     * Rebuilds InOutTexture.Mips from Pixels by its MipGen (cleared for
     * None), down to 1x1. An sRGB texture is filtered in linear light; alpha
     * is filtered linearly either way. Clears Blocks.
     */
    RENDERCORE_API void BuildTextureMips(TextureData& InOutTexture);

    /* Bytes the texture takes on the GPU as stored: its blocks, or RGBA8,
     * every level. */
    RENDERCORE_API uint64_t TextureGpuBytes(const TextureData& InTexture);
    /* ... and what it would take with InCompression's blocks (4x4, BC1 and
     * BC4 8 bytes a block, the rest 16), every level. */
    RENDERCORE_API uint64_t TextureCompressedBytes(int InWidth, int InHeight, int InMipCount, ETextureCompression InCompression);

#if WITH_EDITOR
    /* Encodes the blocks first when Compression asks for them and Blocks
     * do not match (CompressTexture); false, logged, if that fails. */
    RENDERCORE_API bool SaveTextureFile(const std::string& InPath, TextureData& InTexture);
#endif

    /*
     * Loaded textures by path, as MeshAssetCache is for meshes: shared by
     * the game and render threads, thread-safe. Loaded ForGpu: a texture
     * with blocks has no Pixels here. A failed load is warned once
     * and remembered until Invalidate, which is how a reimport reaches the
     * renderer (it rebuilds a GPU texture on a new pointer).
     */
    class RENDERCORE_API TextureAssetCache
    {
    public:
        /* The texture, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<const TextureData> Find(const std::string& InPath);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);
    };
}

#endif //RENDERCORE_TEXTUREASSET_H

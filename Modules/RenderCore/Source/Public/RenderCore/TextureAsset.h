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
 *     THDR v2              width, height, pixel format, sRGB, filter, wrap;
 *                          v2: mip generation, compression, mip count
 *     TPIX v1              level 0: tightly packed rows (CoreImage.h)
 *     TMIP v1 (optional)   levels 1..n-1, each half the last (at least 1),
 *                          tightly packed, one after another
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *
 * RGBA8 only. Compression is a setting stored for the encoder to come
 * (TextureCompression.h): the pixels stay RGBA8 until one is registered.
 * A v1 file reads as no mips, no compression.
 */
namespace RenderCore
{
    /* Stored as numbers: GDI's own enums never reach the file. */
    enum class ETextureFilter : uint8_t { Nearest = 0, Linear = 1 };
    enum class ETextureWrap   : uint8_t { Repeat = 0, Clamp = 1, Mirror = 2 };

    /* How the mip chain is made from level 0 (BuildTextureMips): none, a
     * 2x2 average, or a sharper windowed-sinc (Kaiser) downsample. */
    enum class ETextureMipGen : uint8_t { None = 0, Box = 1, Kaiser = 2 };

    /* GPU block compression the texture asks for. Stored only: see
     * TextureCompression.h. */
    enum class ETextureCompression : uint8_t { None = 0, BC1 = 1, BC3 = 2, BC4 = 3, BC5 = 4, BC7 = 5 };

    enum : int { TEXTURE_MAX_DIMENSION = 16384 };

    struct TextureData
    {
        EngineCore::Image Pixels;            // RGBA8
        bool              bSRGB  = true;     // colour data; false for masks, normals
        ETextureFilter    Filter = ETextureFilter::Linear;
        ETextureWrap      Wrap   = ETextureWrap::Repeat;
        ETextureMipGen      MipGen      = ETextureMipGen::None;
        ETextureCompression Compression = ETextureCompression::None;
        std::vector<EngineCore::Image> Mips;   // levels 1.. (level 0 is Pixels); empty with MipGen None
        AssetImportInfo   ImportInfo;

        /* Levels in all, level 0 included. */
        int GetMipCount() const { return 1 + int(Mips.size()); }
        /* Level InLevel (0: Pixels); null past the last. */
        const EngineCore::Image* GetMip(int InLevel) const;
    };

    struct TextureHeader
    {
        int            Width  = 0;
        int            Height = 0;
        bool           bSRGB  = true;
        ETextureFilter Filter = ETextureFilter::Linear;
        ETextureWrap   Wrap   = ETextureWrap::Repeat;
        ETextureMipGen      MipGen      = ETextureMipGen::None;
        ETextureCompression Compression = ETextureCompression::None;
        int                 MipCount    = 1;
    };

    RENDERCORE_API bool SerializeTexture(EngineCore::Archive& Ar, TextureData& InOutTexture);
    RENDERCORE_API bool LoadTextureFile(const std::string& InPath, TextureData& OutTexture);
    RENDERCORE_API bool LoadTextureHeader(const std::string& InPath, TextureHeader& OutHeader);

    /* "None", "Box", "Kaiser"; "None", "BC1", ... */
    RENDERCORE_API const char* GetTextureMipGenName(ETextureMipGen InMipGen);
    RENDERCORE_API const char* GetTextureCompressionName(ETextureCompression InCompression);

    /* Levels a full chain of InWidth x InHeight has, down to 1x1. */
    RENDERCORE_API int FullMipCount(int InWidth, int InHeight);

    /*
     * Rebuilds InOutTexture.Mips from Pixels by its MipGen (cleared for
     * None), down to 1x1. An sRGB texture is filtered in linear light; alpha
     * is filtered linearly either way.
     */
    RENDERCORE_API void BuildTextureMips(TextureData& InOutTexture);

    /* Bytes the texture takes on the GPU as stored: RGBA8, every level. */
    RENDERCORE_API uint64_t TextureGpuBytes(const TextureData& InTexture);
    /* ... and what it would take with InCompression's blocks (4x4, BC1 and
     * BC4 8 bytes a block, the rest 16), every level. */
    RENDERCORE_API uint64_t TextureCompressedBytes(int InWidth, int InHeight, int InMipCount, ETextureCompression InCompression);

#if WITH_EDITOR
    RENDERCORE_API bool SaveTextureFile(const std::string& InPath, TextureData& InTexture);
#endif

    /*
     * Loaded textures by path, as MeshAssetCache is for meshes: shared by
     * the game and render threads, thread-safe. A failed load is warned once
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

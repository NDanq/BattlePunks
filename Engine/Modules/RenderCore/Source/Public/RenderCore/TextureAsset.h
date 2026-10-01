#ifndef RENDERCORE_TEXTUREASSET_H
#define RENDERCORE_TEXTUREASSET_H

#include <cstdint>
#include <string>

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
 *     THDR v1              width, height, pixel format, sRGB, filter, wrap
 *     TPIX v1              tightly packed rows (CoreImage.h)
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *
 * RGBA8 only. Materials cannot sample textures yet.
 */
namespace RenderCore
{
    /* Stored as numbers: GDI's own enums never reach the file. */
    enum class ETextureFilter : uint8_t { Nearest = 0, Linear = 1 };
    enum class ETextureWrap   : uint8_t { Repeat = 0, Clamp = 1, Mirror = 2 };

    enum : int { TEXTURE_MAX_DIMENSION = 16384 };

    struct TextureData
    {
        EngineCore::Image Pixels;            // RGBA8
        bool              bSRGB  = true;     // colour data; false for masks, normals
        ETextureFilter    Filter = ETextureFilter::Linear;
        ETextureWrap      Wrap   = ETextureWrap::Repeat;
        AssetImportInfo   ImportInfo;
    };

    struct TextureHeader
    {
        int            Width  = 0;
        int            Height = 0;
        bool           bSRGB  = true;
        ETextureFilter Filter = ETextureFilter::Linear;
        ETextureWrap   Wrap   = ETextureWrap::Repeat;
    };

    RENDERCORE_API bool SerializeTexture(EngineCore::Archive& Ar, TextureData& InOutTexture);
    RENDERCORE_API bool LoadTextureFile(const std::string& InPath, TextureData& OutTexture);
    RENDERCORE_API bool LoadTextureHeader(const std::string& InPath, TextureHeader& OutHeader);

#if WITH_EDITOR
    RENDERCORE_API bool SaveTextureFile(const std::string& InPath, TextureData& InTexture);
#endif
}

#endif //RENDERCORE_TEXTUREASSET_H

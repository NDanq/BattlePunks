#ifndef RENDERCORE_TEXTURECOMPRESSION_H
#define RENDERCORE_TEXTURECOMPRESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/TextureAsset.h"

/*
 * GPU block compression: a stub until NVIDIA Texture Tools is integrated.
 *
 * A texture records the compression it asks for (TextureData::Compression,
 * saved in THDR v2), and the Texture Viewer edits it. An encoder registered
 * here is what will turn each level into blocks; none is registered yet, so
 * every texture is still stored and uploaded as RGBA8, and the viewer says
 * so. Integrating NVTT means: implement ITextureCompressor over it, register
 * it from the editor at start-up, store the blocks in the file (a new pixel
 * format in THDR), and give GDI compressed formats.
 */
namespace RenderCore
{
    class ITextureCompressor
    {
    public:
        virtual ~ITextureCompressor() = default;

        /* "NVIDIA Texture Tools 3.2", for the viewer. */
        virtual const char* GetName() const = 0;

        /* Whether InCompression can be encoded. */
        virtual bool Supports(ETextureCompression InCompression) const = 0;

        /* InLevel (RGBA8) as InCompression's 4x4 blocks, row by row. */
        virtual bool Compress(const EngineCore::Image& InLevel, bool bInSRGB, ETextureCompression InCompression,
                              std::vector<uint8_t>& OutBlocks, std::string& OutError) = 0;
    };

    /* The encoder, or null (today). Not owned; set once at start-up. */
    RENDERCORE_API void                SetTextureCompressor(ITextureCompressor* InCompressor);
    RENDERCORE_API ITextureCompressor* GetTextureCompressor();

    /* Whether InCompression would be honoured now: None always; a block
     * format only with an encoder that supports it. */
    RENDERCORE_API bool IsTextureCompressionAvailable(ETextureCompression InCompression);
}

#endif //RENDERCORE_TEXTURECOMPRESSION_H

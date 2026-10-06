#ifndef RENDERCORE_TEXTURECOMPRESSION_H
#define RENDERCORE_TEXTURECOMPRESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/TextureAsset.h"

/*
 * GPU block compression.
 *
 * A texture records the compression it asks for (TextureData::Compression),
 * and the Texture Viewer edits it. An editor build encodes it on save with
 * the built-in encoder, bc7enc_rdo (External/), into TextureData::Blocks,
 * which TBLK stores beside the RGBA8 source. The renderer uploads the blocks
 * as they are where the backend samples the format, and decodes them to
 * RGBA8 where it does not, so the image is the same either way.
 *
 * The formats, all UNORM, read as their RGBA8 source would -- sRGB is the
 * shader's business, as it is for RGBA8:
 *
 *   BC1   RGB, alpha 1                            8 bytes a block
 *   BC3   RGBA                                    16
 *   BC4   red; reads (r, 0, 0, 1)                 8
 *   BC5   red and green; reads (r, g, 0, 1)       16
 *   BC7   RGBA, the best quality                  16
 *
 * An RGBE8 texture (an HDRI) is never compressed.
 */
namespace RenderCore
{
    class ITextureCompressor
    {
    public:
        virtual ~ITextureCompressor() = default;

        /* "bc7enc_rdo", for the viewer. */
        virtual const char* GetName() const = 0;

        /* Whether InCompression can be encoded. */
        virtual bool Supports(ETextureCompression InCompression) const = 0;

        /* InLevel (RGBA8) as InCompression's 4x4 blocks, row by row. Called
         * from any thread, several at once. */
        virtual bool Compress(const EngineCore::Image& InLevel, bool bInSRGB, ETextureCompression InCompression,
                              std::vector<uint8_t>& OutBlocks, std::string& OutError) = 0;
    };

    /* The encoder: bc7enc_rdo in an editor build, null in a game's, until
     * set. Not owned; set once at start-up, or null to have none. */
    RENDERCORE_API void                SetTextureCompressor(ITextureCompressor* InCompressor);
    RENDERCORE_API ITextureCompressor* GetTextureCompressor();

    /* Whether InCompression would be honoured now: None always; a block
     * format only with an encoder that supports it. */
    RENDERCORE_API bool IsTextureCompressionAvailable(ETextureCompression InCompression);

    /* Bytes in one 4x4 block of InCompression; 0 for None. */
    RENDERCORE_API int TextureBlockBytes(ETextureCompression InCompression);

    /* Whether InTexture.Blocks are its levels in its compression: one per
     * level, each the right size. False with none. */
    RENDERCORE_API bool TextureBlocksMatch(const TextureData& InTexture);

    /*
     * Encodes every level of InOutTexture into Blocks with the encoder, the
     * blocks of a level in parallel (EngineCore's job system, if there is
     * one). Clears them for None or RGBE8. False, with Blocks cleared, if no
     * encoder supports the compression or it fails.
     */
    RENDERCORE_API bool CompressTexture(TextureData& InOutTexture, std::string& OutError);

    /* InLevel's blocks back to RGBA8, as the GPU would sample them. False if
     * the bytes are the wrong size or a block is malformed. */
    RENDERCORE_API bool DecodeTextureBlocks(const TextureBlockLevel& InLevel, ETextureCompression InCompression,
                                            EngineCore::Image& OutImage);
}

#endif //RENDERCORE_TEXTURECOMPRESSION_H

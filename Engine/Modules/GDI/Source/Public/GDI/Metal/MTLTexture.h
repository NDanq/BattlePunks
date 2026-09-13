#ifndef CORECLIENT_MTLTEXTURE_H
#define CORECLIENT_MTLTEXTURE_H

#include "Public/GDI/GDITexture.h"
#include "Public/GDI/Metal/MTLForward.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class MTLDevice;

/**
 * Metal GDITextureResource implementation -- the counterpart of VKTextureResource.
 *
 * Owns:
 *   - MTLTexture      -- MTLPixelFormatRGBA8Unorm, storageModeShared
 *   - MTLSamplerState -- built from the EGDITextureBorder / EGDITextureFilter pair
 *
 * Sampled textures use SHARED storage, which removes the whole Vulkan staging
 * dance: pixels go up with -replaceRegion: and come back with -getBytes:, no
 * staging buffer, no transient command buffer, no layout transitions. (Render
 * targets are the exception -- they are PRIVATE and read back through a blit
 * encoder; see MTLRenderTarget.)
 *
 * Qt pixel data is converted to Format_RGBA8888 before upload so it maps
 * byte-for-byte onto MTLPixelFormatRGBA8Unorm, exactly as the Vulkan path does.
 *
 * Deferred destruction has no Metal counterpart: a committed MTLCommandBuffer
 * retains every resource it references, so releasing a texture immediately after
 * the draw that used it (the flood-fill / Restore paths do exactly that) is safe.
 *
 * All operations are render-thread-only.
 */
class MTLTextureResource : public GDITextureResource
{
public:
    /** Standalone texture from image data. */
    MTLTextureResource(MTLDevice*        InDevice,
                        const EngineCore::Image&     InImage,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    /** Parented texture seeded with initial pixel data. */
    MTLTextureResource(MTLDevice*        InDevice,
                        GDITexture2D*     InParent,
                        const EngineCore::Image&     InImage,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    /** Parented texture with no initial data (caller must call Update later). */
    MTLTextureResource(MTLDevice*        InDevice,
                        GDITexture2D*     InParent,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    /**
     * Adopting wrapper: references an externally-owned MTLTexture (a
     * MTLRenderTarget's color texture) and builds only a sampler. Does NOT own or
     * release the texture -- used by MTLRenderTarget::ToGDITexture2D so the render
     * target can be sampled by the layer / blit shaders.
     */
    MTLTextureResource(MTLDevice*        InDevice,
                        MTLTextureRef     InExternalTexture,
                        GDITexture2D*     InParent,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    ~MTLTextureResource();

    /** Re-upload pixel data, recreating the MTLTexture if the dimensions changed. */
    void Update(const EngineCore::Image& InImage) override;

    /** Release the sampler (and the texture, unless it is borrowed). */
    void Release() override;

    /**
     * Synchronous host readback. Returns a EngineCore::Image::Format_RGBA8888 copy of the
     * texture data, or a null EngineCore::Image if the texture has not been populated.
     */
    EngineCore::Image ToImage() const override;

    // Accessors used by MetalDynamicGDI::BindDrawState.
    MTLTextureRef      GetTexture() const { return Texture; }
    MTLSamplerStateRef GetSampler() const { return Sampler; }

private:
    void AllocateTexture();
    void DestroyTexture();
    void BuildSampler();
    void UploadImageData(const EngineCore::Image& InImage);

    MTLDevice*          Device       = nullptr;

    MTLTextureRef       Texture      = nullptr;
    MTLSamplerStateRef  Sampler      = nullptr;

    /** False for the adopting constructor: the texture belongs to the render target. */
    bool                bOwnsTexture = true;

    // Saved for sampler / texture recreation on Update() with changed dimensions.
    EGDITextureBorder   StoredBorder = GDI_TB_Repeat;
    EGDITextureFilter   StoredFilter = GDI_TF_Nearest;
};

#endif //CORECLIENT_MTLTEXTURE_H

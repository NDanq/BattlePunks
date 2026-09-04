#ifndef CORECLIENT_QMTLTEXTURE_H
#define CORECLIENT_QMTLTEXTURE_H

#include "Public/GDI/GDITexture.h"
#include "Public/GDI/Metal/MTLForward.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class QMTLDevice;

/**
 * Metal GDITextureResource implementation -- the counterpart of QVKTextureResource.
 *
 * Owns:
 *   - MTLTexture      -- MTLPixelFormatRGBA8Unorm, storageModeShared
 *   - MTLSamplerState -- built from the EGDITextureBorder / EGDITextureFilter pair
 *
 * Sampled textures use SHARED storage, which removes the whole Vulkan staging
 * dance: pixels go up with -replaceRegion: and come back with -getBytes:, no
 * staging buffer, no transient command buffer, no layout transitions. (Render
 * targets are the exception -- they are PRIVATE and read back through a blit
 * encoder; see QMTLRenderTarget.)
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
class QMTLTextureResource : public GDITextureResource
{
public:
    /** Standalone texture from image data. */
    QMTLTextureResource(QMTLDevice*       InDevice,
                        const EngineCore::Image&     InImage,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    /** Parented texture seeded with initial pixel data. */
    QMTLTextureResource(QMTLDevice*       InDevice,
                        GDITexture2D*     InParent,
                        const EngineCore::Image&     InImage,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    /** Parented texture with no initial data (caller must call Update later). */
    QMTLTextureResource(QMTLDevice*       InDevice,
                        GDITexture2D*     InParent,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    /**
     * Adopting wrapper: references an externally-owned MTLTexture (a
     * QMTLRenderTarget's color texture) and builds only a sampler. Does NOT own or
     * release the texture -- used by QMTLRenderTarget::ToGDITexture2D so the render
     * target can be sampled by the layer / blit shaders.
     */
    QMTLTextureResource(QMTLDevice*       InDevice,
                        MTLTextureRef     InExternalTexture,
                        GDITexture2D*     InParent,
                        EGDITextureBorder InBorder,
                        EGDITextureFilter InFilter);

    ~QMTLTextureResource();

    /** Re-upload pixel data, recreating the MTLTexture if the dimensions changed. */
    void Update(const EngineCore::Image& InImage) override;

    /** Release the sampler (and the texture, unless it is borrowed). */
    void Release() override;

    /**
     * Synchronous host readback. Returns a EngineCore::Image::Format_RGBA8888 copy of the
     * texture data, or a null EngineCore::Image if the texture has not been populated.
     */
    EngineCore::Image ToImage() const override;

    // Accessors used by QMetalDynamicGDI::BindDrawState.
    MTLTextureRef      GetTexture() const { return Texture; }
    MTLSamplerStateRef GetSampler() const { return Sampler; }

private:
    void AllocateTexture();
    void DestroyTexture();
    void BuildSampler();
    void UploadImageData(const EngineCore::Image& InImage);

    QMTLDevice*         Device       = nullptr;

    MTLTextureRef       Texture      = nullptr;
    MTLSamplerStateRef  Sampler      = nullptr;

    /** False for the adopting constructor: the texture belongs to the render target. */
    bool                bOwnsTexture = true;

    // Saved for sampler / texture recreation on Update() with changed dimensions.
    EGDITextureBorder   StoredBorder = GDI_TB_Repeat;
    EGDITextureFilter   StoredFilter = GDI_TF_Nearest;
};

#endif //CORECLIENT_QMTLTEXTURE_H

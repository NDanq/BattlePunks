#ifndef CORECLIENT_MTLRENDERTARGET_H
#define CORECLIENT_MTLRENDERTARGET_H


#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/Metal/MTLForward.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class MTLDevice;

/**
 * Metal render target -- a single RGBA8Unorm color texture, no depth attachment
 * (matching VKRenderTarget).
 *
 * Storage is PRIVATE: the texture is written as a color attachment and sampled by
 * the layer / blit shaders, never touched by the CPU directly. Readback
 * (ToImage) therefore goes through a blit encoder into a shared MTLBuffer, which
 * is the Metal equivalent of Vulkan's vkCmdCopyImageToBuffer + staging buffer.
 *
 * There is no layout tracking: Metal has no image layouts, so the whole
 * GetCurrentLayout/SetCurrentLayout/barrier apparatus of the Vulkan backend
 * simply does not exist here. Hazard tracking between passes inside one command
 * buffer is the driver's job.
 */
class MTLRenderTarget : public GDIRenderTarget
{
public:
    /* InHalfFloat: MTLPixelFormatRGBA16Float (HDR), else RGBA8Unorm. */
    MTLRenderTarget(MTLDevice* InDevice, const std::string& InName, uint InWidth, uint InHeight, bool InHalfFloat = false);
    ~MTLRenderTarget();

    void Bind() override;
    void Release() override;
    void Resize(const uint InWidth, const uint InHeight) override;
    bool IsValid() override;
    EngineCore::Image ToImage() override;
    GDITexture2D* ToGDITexture2D() override;
    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    MTLTextureRef  GetTexture() const { return Texture; }
    const std::string& GetName() const    { return Name; }

    /** True once at least one render pass has written this target. Drives the
     *  pass load action (Clear on the first use, Load thereafter) so a
     *  never-rendered target doesn't display uninitialized memory. */
    bool HasContent() const        { return bHasContent; }
    void SetHasContent(bool bIn)   { bHasContent = bIn; }

protected:
    void Allocate() override;

private:
    void DestroyTexture();

    MTLDevice*      Device         = nullptr;
    std::string         Name;
    MTLTextureRef   Texture        = nullptr;
    bool            bHasContent    = false;
    bool            bHalfFloat     = false;

    /**
     * Lazily-created GDITexture2D that adopts this RT's texture so the render
     * target can be sampled by the layer / blit shaders (ToGDITexture2D).
     * Invalidated on Resize because the underlying texture is recreated.
     */
    GDITexture2D*   TextureWrapper = nullptr;
};

#endif //CORECLIENT_MTLRENDERTARGET_H

#ifndef CORECLIENT_VKTEXTURE_H
#define CORECLIENT_VKTEXTURE_H

#include <vector>

#include <vulkan/vulkan.h>

#include "Public/GDI/GDITexture.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class VKDevice;

/**
 * Vulkan GDITextureResource implementation.
 *
 * Owns:
 *   - VkImage         — device-local, VK_FORMAT_R8G8B8A8_UNORM (or a BC format,
 *                       the compressed constructor), TILING_OPTIMAL
 *   - VkDeviceMemory  — backing memory (device-local)
 *   - VkImageView     — 2D view for sampling / descriptor writes
 *   - VkSampler       — sampler built from the EGDITextureBorder / EGDITextureFilter pair
 *
 * Pixel uploads (InitResource / Update) go through a host-coherent staging
 * buffer + a one-shot transient command buffer, submitted synchronously.
 * Qt pixel data is converted to Format_RGBA8888 before upload so it maps
 * directly onto VK_FORMAT_R8G8B8A8_UNORM byte-for-byte.
 *
 * ToImage() performs a synchronous readback via a staging buffer.
 *
 * All operations are render-thread-only (Phase 2).
 * Phase 3 will add async staging queue + VMA migration.
 */
class VKTextureResource : public GDITextureResource
{
public:
    /** Standalone texture from image data. */
    VKTextureResource(VKDevice*           InDevice,
                       const EngineCore::Image&       InImage,
                       EGDITextureBorder   InBorder,
                       EGDITextureFilter   InFilter);

    /** Parented texture seeded with initial pixel data. */
    VKTextureResource(VKDevice*           InDevice,
                       GDITexture2D*       InParent,
                       const EngineCore::Image&       InImage,
                       EGDITextureBorder   InBorder,
                       EGDITextureFilter   InFilter);

    /** Standalone texture with its mip chain: InLevels[0] full size, each
     *  next half the last; sampled trilinear with a Linear filter. */
    VKTextureResource(VKDevice*                InDevice,
                       const EngineCore::Image* InLevels,
                       uint32_t                 InCount,
                       EGDITextureBorder        InBorder,
                       EGDITextureFilter        InFilter);

    /** Standalone block-compressed texture with its mip chain, as the one
     *  above: InFormat a BC format the device samples (SupportsSampledFormat). */
    VKTextureResource(VKDevice*                 InDevice,
                       VkFormat                  InFormat,
                       const GDICompressedLevel* InLevels,
                       uint32_t                  InCount,
                       EGDITextureBorder         InBorder,
                       EGDITextureFilter         InFilter);

    /** Parented texture with no initial data (caller must call Update later). */
    VKTextureResource(VKDevice*           InDevice,
                       GDITexture2D*       InParent,
                       EGDITextureBorder   InBorder,
                       EGDITextureFilter   InFilter);

    /**
     * Adopting wrapper: references an externally-owned VkImageView (e.g. a
     * VKRenderTarget's color image) and builds only a sampler.  Does NOT own
     * or destroy the image/view — used by VKRenderTarget::ToGDITexture2D so the
     * render target can be sampled by the layer/blit shaders.
     */
    VKTextureResource(VKDevice*           InDevice,
                       VkImageView         InExternalView,
                       GDITexture2D*       InParent,
                       EGDITextureBorder   InBorder,
                       EGDITextureFilter   InFilter);

    ~VKTextureResource();

    /**
     * Re-upload pixel data.  If the incoming image has different dimensions
     * (or the image has mips or blocks), the VkImage is recreated as RGBA8.
     * Otherwise only the pixels are re-uploaded.
     */
    void Update(const EngineCore::Image& InImage) override;

    /** Destroy all Vulkan resources. */
    void Release() override;

    /**
     * Synchronous device-to-host readback.
     * Returns a EngineCore::Image::Format_RGBA8888 copy of the texture data,
     * or a null EngineCore::Image if the texture has not yet been populated
     * or is block-compressed.
     */
    EngineCore::Image ToImage() const override;

    // Accessors used by the Phase-2 shader / descriptor binding system.
    inline VkImageView GetImageView() const { return ImageView; }
    inline VkSampler   GetSampler()   const { return Sampler; }

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Allocate VkImage + memory + view for the current Width × Height. */
    void AllocateImage();

    /** Destroy view, image, and memory.  Safe to call when already null. */
    void DestroyImage();

    /** Create (or recreate) the VkSampler from stored border/filter settings. */
    void BuildSampler();

    /**
     * Upload pixel data via a staging buffer + transient command buffer.
     * Leaves the image in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL on completion.
     * @param InImage Must be non-null and have valid Width × Height.
     */
    void UploadImageData(const EngineCore::Image& InImage);

    /** Every level of a mip chain in one staging copy (MipLevels of them). */
    void UploadMipChain(const EngineCore::Image* InLevels, uint32_t InCount);

    /** Copies InTotal staged bytes into the image by InRegions, one per
     *  level, leaving it SHADER_READ_ONLY. */
    void UploadRegions(const uint8_t* const* InSources, const VkDeviceSize* InSizes,
                       std::vector<VkBufferImageCopy>& InRegions, VkDeviceSize InTotal);

    // -----------------------------------------------------------------------
    // Decode helpers
    // -----------------------------------------------------------------------
    static VkSamplerAddressMode DecodeAddressMode(EGDITextureBorder InBorder);
    static VkFilter             DecodeFilter(EGDITextureFilter InFilter);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    VKDevice*           Device          = nullptr;

    VkImage             Image           = VK_NULL_HANDLE;
    VkDeviceMemory      ImageMemory     = VK_NULL_HANDLE;
    VkImageView         ImageView       = VK_NULL_HANDLE;
    VkSampler           Sampler         = VK_NULL_HANDLE;

    // When false the Image/ImageView/memory are borrowed (adopting ctor) and
    // must not be destroyed by this resource — only the Sampler is owned.
    bool                bOwnsImage      = true;

    static constexpr VkFormat kRGBA8    = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat            Format          = kRGBA8;   // the image's; a BC one from the compressed constructor
    VkImageLayout       CurrentLayout   = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t            MipLevels       = 1;   // the image's levels; Update() goes back to one

    // Saved for sampler / image recreation on Update() with changed dimensions.
    EGDITextureBorder   StoredBorder    = GDI_TB_Repeat;
    EGDITextureFilter   StoredFilter    = GDI_TF_Nearest;
};

#endif //CORECLIENT_VKTEXTURE_H

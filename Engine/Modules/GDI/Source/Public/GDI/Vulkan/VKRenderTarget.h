#ifndef CORECLIENT_VKRENDERTARGET_H
#define CORECLIENT_VKRENDERTARGET_H

#include <vulkan/vulkan.h>

#include "Public/GDI/GDIRenderTarget.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class VKDevice;

/**
 * A color image (R8G8B8A8_UNORM) and a depth attachment (D32_SFLOAT, starts
 * UNDEFINED so its first pass clears it; sampleable through ToDepthTexture2D,
 * in SHADER_READ_ONLY_OPTIMAL whenever it is not the bound depth). Raw
 * vkAllocateMemory; Phase 2 will migrate to VMA.
 */
class VKRenderTarget : public GDIRenderTarget
{
public:
    VKRenderTarget(VKDevice* InDevice, const std::string& InName, uint InWidth, uint InHeight);
    ~VKRenderTarget();

    void Bind() override;
    void Release() override;
    void Resize(const uint InWidth, const uint InHeight) override;
    bool IsValid() override;
    EngineCore::Image ToImage() override;
    GDITexture2D* ToGDITexture2D() override;
    GDITexture2D* ToDepthTexture2D() override;
    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    inline VkImage      GetImage() const        { return Image; }
    inline VkImageView  GetImageView() const    { return ImageView; }
    inline VkFormat     GetFormat() const       { return Format; }
    inline VkImageLayout GetCurrentLayout() const { return CurrentLayout; }
    inline void         SetCurrentLayout(VkImageLayout InLayout) { CurrentLayout = InLayout; }

    inline bool          HasDepth() const        { return DepthView != VK_NULL_HANDLE; }
    inline VkImage       GetDepthImage() const   { return DepthImage; }
    inline VkImageView   GetDepthView() const    { return DepthView; }
    inline VkFormat      GetDepthFormat() const  { return HasDepth() ? DepthFormat : VK_FORMAT_UNDEFINED; }
    inline VkImageLayout GetDepthLayout() const  { return DepthLayout; }
    inline void          SetDepthLayout(VkImageLayout InLayout) { DepthLayout = InLayout; }

protected:
    void Allocate() override;

private:
    void DestroyImage();

    /* On failure the target just lacks depth (logged). */
    void AllocateDepth();

    VKDevice*       Device          = nullptr;
    std::string         Name;
    VkFormat        Format          = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage         Image           = VK_NULL_HANDLE;
    VkDeviceMemory  ImageMemory     = VK_NULL_HANDLE;
    VkImageView     ImageView       = VK_NULL_HANDLE;
    VkImageLayout   CurrentLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    VkFormat        DepthFormat     = VK_FORMAT_D32_SFLOAT;
    VkImage         DepthImage      = VK_NULL_HANDLE;
    VkDeviceMemory  DepthMemory     = VK_NULL_HANDLE;
    VkImageView     DepthView       = VK_NULL_HANDLE;
    VkImageLayout   DepthLayout     = VK_IMAGE_LAYOUT_UNDEFINED;

    /**
     * Lazily-created GDITexture2D that adopts this RT's image view so the render
     * target can be sampled by the layer / blit shaders (ToGDITexture2D).
     * Invalidated on Resize because the underlying image view is recreated.
     */
    GDITexture2D*   TextureWrapper  = nullptr;

    /* Likewise for the depth view, point-filtered. */
    GDITexture2D*   DepthWrapper    = nullptr;
};

#endif //CORECLIENT_VKRENDERTARGET_H

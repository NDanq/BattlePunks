#ifndef CORECLIENT_QVKRENDERTARGET_H
#define CORECLIENT_QVKRENDERTARGET_H

#include <vulkan/vulkan.h>

#include "Public/GDI/GDIRenderTarget.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class QVKDevice;

/**
 * Phase 1 render target: a single color image (R8G8B8A8_UNORM).
 * No depth attachment yet. Memory is allocated via raw vkAllocateMemory;
 * Phase 2 will migrate to VMA.
 */
class QVKRenderTarget : public GDIRenderTarget
{
public:
    QVKRenderTarget(QVKDevice* InDevice, const std::string& InName, uint InWidth, uint InHeight);
    ~QVKRenderTarget();

    void Bind() override;
    void Release() override;
    void Resize(const uint InWidth, const uint InHeight) override;
    bool IsValid() override;
    EngineCore::Image ToImage() override;
    GDITexture2D* ToGDITexture2D() override;
    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    inline VkImage      GetImage() const        { return Image; }
    inline VkImageView  GetImageView() const    { return ImageView; }
    inline VkFormat     GetFormat() const       { return Format; }
    inline VkImageLayout GetCurrentLayout() const { return CurrentLayout; }
    inline void         SetCurrentLayout(VkImageLayout InLayout) { CurrentLayout = InLayout; }

protected:
    void Allocate() override;

private:
    void DestroyImage();

    QVKDevice*      Device          = nullptr;
    std::string         Name;
    VkFormat        Format          = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage         Image           = VK_NULL_HANDLE;
    VkDeviceMemory  ImageMemory     = VK_NULL_HANDLE;
    VkImageView     ImageView       = VK_NULL_HANDLE;
    VkImageLayout   CurrentLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    /**
     * Lazily-created GDITexture2D that adopts this RT's image view so the render
     * target can be sampled by the layer / blit shaders (ToGDITexture2D).
     * Invalidated on Resize because the underlying image view is recreated.
     */
    GDITexture2D*   TextureWrapper  = nullptr;
};

#endif //CORECLIENT_QVKRENDERTARGET_H

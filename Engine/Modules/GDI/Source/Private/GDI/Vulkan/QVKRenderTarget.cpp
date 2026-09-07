#include "Public/GDI/Vulkan/QVKRenderTarget.h"
#include "Public/GDI/Vulkan/QVKDevice.h"
#include "Public/GDI/Vulkan/QVulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/QVKTexture.h"
#include "Private/GDI/Vulkan/QVKTransientOps.h"
#include "Public/GDI/GDITexture.h"

#include <cstring>
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// Global GDI driver (declared in GDI.cpp) — used to route synchronous readbacks
// and render-target resolves through the active command buffer / device.
extern GDIDynamicDriver* GDynamicGDI;
using namespace VKOps;

QVKRenderTarget::QVKRenderTarget(QVKDevice* InDevice, const std::string& InName, uint InWidth, uint InHeight)
    : GDIRenderTarget(InWidth, InHeight),
      Device(InDevice),
      Name(InName)
{
    Allocate();
}

QVKRenderTarget::~QVKRenderTarget()
{
    if(auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI))
    {
        GDI->UnregisterRenderTarget(this);
    }
    DestroyImage();
}

void QVKRenderTarget::Allocate()
{
    if(!Device || !Device->IsValid()) return;
    if(Width == 0 || Height == 0) return;

    VkImageCreateInfo ImageInfo{};
    ImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ImageInfo.imageType     = VK_IMAGE_TYPE_2D;
    ImageInfo.format        = Format;
    ImageInfo.extent.width  = Width;
    ImageInfo.extent.height = Height;
    ImageInfo.extent.depth  = 1;
    ImageInfo.mipLevels     = 1;
    ImageInfo.arrayLayers   = 1;
    ImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    ImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ImageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT      // for blit-to-swapchain
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT      // for clears via copy
                            | VK_IMAGE_USAGE_SAMPLED_BIT;          // Phase 2 will sample as a texture
    ImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if(vkCreateImage(Device->GetDevice(), &ImageInfo, nullptr, &Image) != VK_SUCCESS)
    {
        GDI_LOG("[QVKRenderTarget] vkCreateImage failed for " + Name)
        return;
    }

    VkMemoryRequirements MemReqs{};
    vkGetImageMemoryRequirements(Device->GetDevice(), Image, &MemReqs);

    const uint32_t MemTypeIdx = Device->FindMemoryType(MemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(MemTypeIdx == UINT32_MAX)
    {
        GDI_LOG("[QVKRenderTarget] No suitable memory type for RT " + Name)
        DestroyImage();
        return;
    }

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReqs.size;
    AllocInfo.memoryTypeIndex = MemTypeIdx;

    if(vkAllocateMemory(Device->GetDevice(), &AllocInfo, nullptr, &ImageMemory) != VK_SUCCESS)
    {
        GDI_LOG("[QVKRenderTarget] vkAllocateMemory failed for " + Name)
        DestroyImage();
        return;
    }
    vkBindImageMemory(Device->GetDevice(), Image, ImageMemory, 0);

    VkImageViewCreateInfo ViewInfo{};
    ViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ViewInfo.image                           = Image;
    ViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    ViewInfo.format                          = Format;
    ViewInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ViewInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ViewInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ViewInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    ViewInfo.subresourceRange.baseMipLevel   = 0;
    ViewInfo.subresourceRange.levelCount     = 1;
    ViewInfo.subresourceRange.baseArrayLayer = 0;
    ViewInfo.subresourceRange.layerCount     = 1;

    if(vkCreateImageView(Device->GetDevice(), &ViewInfo, nullptr, &ImageView) != VK_SUCCESS)
    {
        GDI_LOG("[QVKRenderTarget] vkCreateImageView failed for " + Name)
        DestroyImage();
        return;
    }

#ifdef QT_DEBUG
    // Name the image so validation-layer messages identify which RT they refer to.
    {
        static auto SetObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(Device->GetInstance(), "vkSetDebugUtilsObjectNameEXT"));
        if(SetObjectName)
        {
            const std::string NameUtf8 = std::string("RT:") + Name;
            VkDebugUtilsObjectNameInfoEXT NameInfo{};
            NameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            NameInfo.objectType   = VK_OBJECT_TYPE_IMAGE;
            NameInfo.objectHandle = reinterpret_cast<uint64_t>(Image);
            NameInfo.pObjectName  = NameUtf8.c_str();
            SetObjectName(Device->GetDevice(), &NameInfo);
        }
    }
#endif

    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void QVKRenderTarget::DestroyImage()
{
    if(!Device || !Device->IsValid()) return;
    VkDevice Dev = Device->GetDevice();

    // The sample-wrapper borrows ImageView; tear it down before the view is freed.
    if(TextureWrapper)
    {
        if(TextureWrapper->Resource)
        {
            TextureWrapper->Resource->Release();
            delete TextureWrapper->Resource;
            TextureWrapper->Resource = nullptr;
        }
        delete TextureWrapper;
        TextureWrapper = nullptr;
    }

    if(ImageView   != VK_NULL_HANDLE) { vkDestroyImageView(Dev, ImageView, nullptr);   ImageView = VK_NULL_HANDLE; }
    if(Image       != VK_NULL_HANDLE) { vkDestroyImage(Dev, Image, nullptr);           Image = VK_NULL_HANDLE; }
    if(ImageMemory != VK_NULL_HANDLE) { vkFreeMemory(Dev, ImageMemory, nullptr);       ImageMemory = VK_NULL_HANDLE; }
    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void QVKRenderTarget::Bind()
{
    // Layout transition is performed by QVulkanDynamicGDI::SetRenderTarget so it can
    // record into the active command buffer. Nothing to do here in Phase 1.
}

void QVKRenderTarget::Release()
{
    DestroyImage();
}

void QVKRenderTarget::Resize(const uint InWidth, const uint InHeight)
{
    // This RT may be the render thread's *active* target with an open dynamic-rendering
    // region recorded in the current (not-yet-submitted) command buffer. Flush that
    // buffer first (close the region + submit, WITHOUT re-binding this RT) so the
    // recorded vkCmdEndRendering completes on the still-valid image before we destroy
    // it — otherwise EndRendering executes against a freed VkImage.
    if(auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI))
    {
        // Tell the UI-thread present to skip until a full frame is re-rendered into
        // the new targets (cleared in EndRendering). Prevents the present from racing
        // the half-recreated RTs (stale layout / destroyed image).
        GDI->SetResizing(true);
        GDI->FlushForReadback(/*bResumeActiveRegion*/ false);
    }

    // Then block until ALL submitted work (incl. the UI-thread present sampling this
    // RT) is done, so the image/view aren't destroyed while still in use.
    if(Device) Device->WaitIdle();

    GDIRenderTarget::Resize(InWidth, InHeight);
    DestroyImage();
    Allocate();
}

bool QVKRenderTarget::IsValid()
{
    return Image != VK_NULL_HANDLE && ImageView != VK_NULL_HANDLE;
}

EngineCore::Image QVKRenderTarget::ToImage()
{
    if(!Device || !Device->IsValid() || Image == VK_NULL_HANDLE) return EngineCore::Image();
    if(Width == 0 || Height == 0) return EngineCore::Image();

    // Submit & finish any in-flight draws targeting this RT so the readback sees them.
    if(auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI))
        GDI->FlushForReadback();

    VkDevice Dev = Device->GetDevice();
    const VkDeviceSize DataSize = VkDeviceSize(Width) * VkDeviceSize(Height) * 4u;

    VkBuffer       Buf = VK_NULL_HANDLE;
    VkDeviceMemory Mem = VK_NULL_HANDLE;
    if(!AllocStagingBuffer(Device, DataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buf, Mem))
        return EngineCore::Image();

    VkCommandPool   Pool = VK_NULL_HANDLE;
    VkCommandBuffer Cmd  = VK_NULL_HANDLE;
    if(!BeginTransientCmd(Device, Pool, Cmd))
    {
        vkDestroyBuffer(Dev, Buf, nullptr);
        vkFreeMemory(Dev, Mem, nullptr);
        return EngineCore::Image();
    }

    const VkImageLayout Cur = CurrentLayout;
    RecordBarrier(Cmd, MakeImageBarrier(
        Image, Cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT));

    VkBufferImageCopy Region{};
    Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Region.imageSubresource.layerCount = 1;
    Region.imageExtent = { Width, Height, 1 };
    vkCmdCopyImageToBuffer(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Buf, 1, &Region);

    RecordBarrier(Cmd, MakeImageBarrier(
        Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Cur,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));

    EndAndSubmitTransientCmd(Device, Pool, Cmd);

    void* Mapped = nullptr;
    vkMapMemory(Dev, Mem, 0, DataSize, 0, &Mapped);
    // The render thread draws with the same GL-convention matrices as the OpenGL
    // backend, so RT memory is row-identical to a GL framebuffer (row 0 = visual
    // bottom). Mirror vertically exactly like QOpenGLRenderTarget::ToImage so
    // both backends return an identically-oriented EngineCore::Image.
    EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    std::memcpy(Result.GetData(), Mapped, size_t(DataSize));
    vkUnmapMemory(Dev, Mem);
    Result.FlipVertical();

    vkDestroyBuffer(Dev, Buf, nullptr);
    vkFreeMemory(Dev, Mem, nullptr);
    return Result;
}

GDITexture2D* QVKRenderTarget::ToGDITexture2D()
{
    if(ImageView == VK_NULL_HANDLE) return nullptr;

    // Lazily wrap the RT's color image view in a GDITexture2D so the layer / blit
    // shaders can sample it. The adopting QVKTextureResource borrows the view (it
    // is not destroyed by the texture). Invalidated in DestroyImage on resize.
    if(!TextureWrapper)
    {
        TextureWrapper         = new GDITexture2D();
        TextureWrapper->Width  = Width;
        TextureWrapper->Height = Height;
        TextureWrapper->Resource =
            new QVKTextureResource(Device, ImageView, TextureWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Linear);
    }
    return TextureWrapper;
}

void QVKRenderTarget::ResolveRenderTarget(GDIRenderTarget* Dest)
{
    auto* DstRT = dynamic_cast<QVKRenderTarget*>(Dest);
    if(!DstRT) return;
    if(auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI))
        GDI->RecordResolve(this, DstRT);
}

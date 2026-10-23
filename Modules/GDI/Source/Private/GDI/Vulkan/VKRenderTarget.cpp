#include "Public/GDI/Vulkan/VKRenderTarget.h"
#include "Public/GDI/GDIHalf.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/VKTexture.h"
#include "Private/GDI/Vulkan/VKTransientOps.h"
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

namespace
{
    /* Bytes a texel of InFormat reads back as. */
    VkDeviceSize ReadbackTexelBytes(const VkFormat InFormat)
    {
        return InFormat == VK_FORMAT_R32G32B32A32_SFLOAT ? 16u : InFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    }

    /* InTexels of InFormat, as read back, into OutImage's RGBA8. */
    void ReadbackToBytes(const VkFormat InFormat, const void* InMapped, EngineCore::Image& OutImage, const size_t InTexels)
    {
        if(InFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
        {
            GDIHalvesToBytes(static_cast<const uint16_t*>(InMapped), OutImage.GetData(), InTexels * 4u);
        }
        else if(InFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        {
            GDIRgb10A2ToBytes(static_cast<const uint32_t*>(InMapped), OutImage.GetData(), InTexels);
        }
        else if(InFormat == VK_FORMAT_R32G32B32A32_SFLOAT)
        {
            GDIFloatsToBytes(static_cast<const float*>(InMapped), OutImage.GetData(), InTexels * 4u);
        }
        else if(InFormat == VK_FORMAT_R32_SFLOAT)
        {
            GDIR32FToBytes(static_cast<const float*>(InMapped), OutImage.GetData(), InTexels);
        }
        else
        {
            std::memcpy(OutImage.GetData(), InMapped, InTexels * 4u);
        }
    }
}

VKRenderTarget::VKRenderTarget(VKDevice* InDevice, const std::string& InName, uint InWidth, uint InHeight, VkFormat InFormat)
    : GDIRenderTarget(InWidth, InHeight),
      Device(InDevice),
      Name(InName),
      Format(InFormat)
{
    Allocate();
}

VKRenderTarget::~VKRenderTarget()
{
    if(auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI))
    {
        GDI->UnregisterRenderTarget(this);
    }
    DestroyImage();
}

void VKRenderTarget::Allocate()
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
        GDI_LOG("[VKRenderTarget] vkCreateImage failed for " + Name)
        return;
    }

    VkMemoryRequirements MemReqs{};
    vkGetImageMemoryRequirements(Device->GetDevice(), Image, &MemReqs);

    const uint32_t MemTypeIdx = Device->FindMemoryType(MemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(MemTypeIdx == UINT32_MAX)
    {
        GDI_LOG("[VKRenderTarget] No suitable memory type for RT " + Name)
        DestroyImage();
        return;
    }

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReqs.size;
    AllocInfo.memoryTypeIndex = MemTypeIdx;

    if(vkAllocateMemory(Device->GetDevice(), &AllocInfo, nullptr, &ImageMemory) != VK_SUCCESS)
    {
        GDI_LOG("[VKRenderTarget] vkAllocateMemory failed for " + Name)
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
        GDI_LOG("[VKRenderTarget] vkCreateImageView failed for " + Name)
        DestroyImage();
        return;
    }

    // Name the image so captures and validation-layer messages identify which RT it is.
    if(VulkanGDIContext::HasDebugUtils() && GDIShaderDebugEnabled())
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

    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    AllocateDepth();
}

void VKRenderTarget::AllocateDepth()
{
    VkDevice Dev = Device->GetDevice();

    VkImageCreateInfo ImageInfo{};
    ImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ImageInfo.imageType     = VK_IMAGE_TYPE_2D;
    ImageInfo.format        = DepthFormat;
    ImageInfo.extent        = { Width, Height, 1 };
    ImageInfo.mipLevels     = 1;
    ImageInfo.arrayLayers   = 1;
    ImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    ImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ImageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT;                // ToDepthTexture2D
    ImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if(vkCreateImage(Dev, &ImageInfo, nullptr, &DepthImage) != VK_SUCCESS)
    {
        GDI_LOG("[VKRenderTarget] no depth image for " + Name + "; it draws without a depth test")
        DepthImage = VK_NULL_HANDLE;
        return;
    }

    VkMemoryRequirements MemReqs{};
    vkGetImageMemoryRequirements(Dev, DepthImage, &MemReqs);
    const uint32_t MemTypeIdx = Device->FindMemoryType(MemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReqs.size;
    AllocInfo.memoryTypeIndex = MemTypeIdx;

    if(MemTypeIdx == UINT32_MAX || vkAllocateMemory(Dev, &AllocInfo, nullptr, &DepthMemory) != VK_SUCCESS)
    {
        GDI_LOG("[VKRenderTarget] no depth memory for " + Name + "; it draws without a depth test")
        vkDestroyImage(Dev, DepthImage, nullptr);
        DepthImage  = VK_NULL_HANDLE;
        DepthMemory = VK_NULL_HANDLE;
        return;
    }
    vkBindImageMemory(Dev, DepthImage, DepthMemory, 0);

    VkImageViewCreateInfo ViewInfo{};
    ViewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ViewInfo.image                       = DepthImage;
    ViewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    ViewInfo.format                      = DepthFormat;
    ViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ViewInfo.subresourceRange.levelCount = 1;
    ViewInfo.subresourceRange.layerCount = 1;

    if(vkCreateImageView(Dev, &ViewInfo, nullptr, &DepthView) != VK_SUCCESS)
    {
        GDI_LOG("[VKRenderTarget] no depth view for " + Name + "; it draws without a depth test")
        vkDestroyImage(Dev, DepthImage, nullptr);
        vkFreeMemory(Dev, DepthMemory, nullptr);
        DepthImage  = VK_NULL_HANDLE;
        DepthMemory = VK_NULL_HANDLE;
        DepthView   = VK_NULL_HANDLE;
        return;
    }
    DepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VKRenderTarget::DestroyImage()
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
    if(DepthWrapper)
    {
        if(DepthWrapper->Resource)
        {
            DepthWrapper->Resource->Release();
            delete DepthWrapper->Resource;
            DepthWrapper->Resource = nullptr;
        }
        delete DepthWrapper;
        DepthWrapper = nullptr;
    }

    if(ImageView   != VK_NULL_HANDLE) { vkDestroyImageView(Dev, ImageView, nullptr);   ImageView = VK_NULL_HANDLE; }
    if(Image       != VK_NULL_HANDLE) { vkDestroyImage(Dev, Image, nullptr);           Image = VK_NULL_HANDLE; }
    if(ImageMemory != VK_NULL_HANDLE) { vkFreeMemory(Dev, ImageMemory, nullptr);       ImageMemory = VK_NULL_HANDLE; }
    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if(DepthView   != VK_NULL_HANDLE) { vkDestroyImageView(Dev, DepthView, nullptr);   DepthView = VK_NULL_HANDLE; }
    if(DepthImage  != VK_NULL_HANDLE) { vkDestroyImage(Dev, DepthImage, nullptr);      DepthImage = VK_NULL_HANDLE; }
    if(DepthMemory != VK_NULL_HANDLE) { vkFreeMemory(Dev, DepthMemory, nullptr);       DepthMemory = VK_NULL_HANDLE; }
    DepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VKRenderTarget::Bind()
{
    // Layout transition is performed by VulkanDynamicGDI::SetRenderTarget so it can
    // record into the active command buffer. Nothing to do here in Phase 1.
}

void VKRenderTarget::Release()
{
    // Submitted frames (and the UI-thread present) may still use the images:
    // wait for them, as Resize does. Releases are rare -- teardown, mostly.
    if(Device && Device->IsValid() && Image != VK_NULL_HANDLE) Device->WaitIdle();
    DestroyImage();
}

void VKRenderTarget::Resize(const uint InWidth, const uint InHeight)
{
    // This RT may be the render thread's *active* target with an open dynamic-rendering
    // region recorded in the current (not-yet-submitted) command buffer. Flush that
    // buffer first (close the region + submit, WITHOUT re-binding this RT) so the
    // recorded vkCmdEndRendering completes on the still-valid image before we destroy
    // it — otherwise EndRendering executes against a freed VkImage.
    if(auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI))
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

bool VKRenderTarget::IsValid()
{
    return Image != VK_NULL_HANDLE && ImageView != VK_NULL_HANDLE;
}

EngineCore::Image VKRenderTarget::ToImage()
{
    if(!Device || !Device->IsValid() || Image == VK_NULL_HANDLE) return EngineCore::Image();
    if(Width == 0 || Height == 0) return EngineCore::Image();

    // Submit & finish any in-flight draws targeting this RT so the readback sees them.
    if(auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI))
        GDI->FlushForReadback();

    VkDevice Dev = Device->GetDevice();
    const VkDeviceSize DataSize = VkDeviceSize(Width) * VkDeviceSize(Height) * ReadbackTexelBytes(Format);

    VkBuffer       Buf = VK_NULL_HANDLE;
    VkDeviceMemory Mem = VK_NULL_HANDLE;
    if(!AllocStagingBuffer(Device, DataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buf, Mem, true))
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
    // RT memory is top-down (row 0 = visual top, VulkanDynamicGDI::SetViewport),
    // which is EngineCore::Image's own order: the rows copy as they are.
    EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    ReadbackToBytes(Format, Mapped, Result, size_t(Width) * size_t(Height));
    vkUnmapMemory(Dev, Mem);

    vkDestroyBuffer(Dev, Buf, nullptr);
    vkFreeMemory(Dev, Mem, nullptr);
    return Result;
}

namespace
{
    /* VKRenderTarget::BeginReadback's: a staging buffer the frame copies into. */
    class VKReadback final : public GDIReadback
    {
    public:
        VKReadback(VKDevice* InDevice, VkBuffer InBuffer, VkDeviceMemory InMemory, uint InWidth, uint InHeight, VkFormat InFormat, uint64_t InDoneAt)
            : Device(InDevice), Buffer(InBuffer), Memory(InMemory), Width(InWidth), Height(InHeight), Format(InFormat), DoneAt(InDoneAt) {}

        ~VKReadback() override
        {
            /* Still being copied into: freed once the GPU is past it. */
            auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI);
            if(GDI && !GDI->IsSubmissionDone(DoneAt))
            {
                GDI->DeferDestroyBuffer(Buffer, Memory);
                return;
            }
            if(Device && Device->IsValid())
            {
                vkDestroyBuffer(Device->GetDevice(), Buffer, nullptr);
                vkFreeMemory(Device->GetDevice(), Memory, nullptr);
            }
        }

        bool IsReady() override
        {
            auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI);
            return !GDI || GDI->IsSubmissionDone(DoneAt);
        }

        EngineCore::Image Take() override
        {
            if(bTaken || !IsReady() || !Device || !Device->IsValid()) return EngineCore::Image();
            bTaken = true;

            const VkDeviceSize DataSize = VkDeviceSize(Width) * VkDeviceSize(Height) * ReadbackTexelBytes(Format);
            void* Mapped = nullptr;
            if(vkMapMemory(Device->GetDevice(), Memory, 0, DataSize, 0, &Mapped) != VK_SUCCESS) return EngineCore::Image();
            // RT memory is top-down, EngineCore::Image's own order (ToImage).
            EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
            ReadbackToBytes(Format, Mapped, Result, size_t(Width) * size_t(Height));
            vkUnmapMemory(Device->GetDevice(), Memory);
            return Result;
        }

    private:
        VKDevice*      Device;
        VkBuffer       Buffer;
        VkDeviceMemory Memory;
        uint           Width, Height;
        VkFormat       Format;
        uint64_t       DoneAt;
        bool           bTaken = false;
    };
}

GDIReadback* VKRenderTarget::BeginReadback()
{
    auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI);
    if(!GDI || !Device || !Device->IsValid() || Image == VK_NULL_HANDLE || Width == 0 || Height == 0)
        return GDIRenderTarget::BeginReadback();

    const VkDeviceSize DataSize = VkDeviceSize(Width) * VkDeviceSize(Height) * ReadbackTexelBytes(Format);
    VkBuffer       Buf = VK_NULL_HANDLE;
    VkDeviceMemory Mem = VK_NULL_HANDLE;
    if(!AllocStagingBuffer(Device, DataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buf, Mem, true))
        return GDIRenderTarget::BeginReadback();

    uint64_t DoneAt = 0;
    if(!GDI->RecordReadback(this, Buf, DoneAt))
    {
        // No frame recording: the synchronous path.
        vkDestroyBuffer(Device->GetDevice(), Buf, nullptr);
        vkFreeMemory(Device->GetDevice(), Mem, nullptr);
        return GDIRenderTarget::BeginReadback();
    }
    return new VKReadback(Device, Buf, Mem, Width, Height, Format, DoneAt);
}

GDITexture2D* VKRenderTarget::ToGDITexture2D()
{
    if(ImageView == VK_NULL_HANDLE) return nullptr;

    // Lazily wrap the RT's color image view in a GDITexture2D so the layer / blit
    // shaders can sample it. The adopting VKTextureResource borrows the view (it
    // is not destroyed by the texture). Invalidated in DestroyImage on resize.
    if(!TextureWrapper)
    {
        TextureWrapper         = new GDITexture2D();
        TextureWrapper->Width  = Width;
        TextureWrapper->Height = Height;
        TextureWrapper->Resource =
            new VKTextureResource(Device, ImageView, TextureWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Linear);
    }
    return TextureWrapper;
}

GDITexture2D* VKRenderTarget::ToDepthTexture2D()
{
    if(DepthView == VK_NULL_HANDLE) return nullptr;

    // As ToGDITexture2D, over the depth view. Point-filtered: linear filtering
    // of D32_SFLOAT is optional, and depth is read texel by texel anyway.
    if(!DepthWrapper)
    {
        DepthWrapper         = new GDITexture2D();
        DepthWrapper->Width  = Width;
        DepthWrapper->Height = Height;
        DepthWrapper->Resource =
            new VKTextureResource(Device, DepthView, DepthWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Nearest);
    }
    return DepthWrapper;
}

void VKRenderTarget::ResolveRenderTarget(GDIRenderTarget* Dest)
{
    auto* DstRT = dynamic_cast<VKRenderTarget*>(Dest);
    if(!DstRT) return;
    if(auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI))
        GDI->RecordResolve(this, DstRT);
}

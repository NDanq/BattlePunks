#include "Public/GDI/Vulkan/VKTexture.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Private/GDI/Vulkan/VKTransientOps.h"

#include <cstring>
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// Transient-command / staging helpers now live in the shared VKTransientOps.h
// (reused by VKRenderTarget for render-target readback / resolve).
using namespace VKOps;

// Global GDI driver (declared in GDI.cpp) — used to defer Vulkan object
// destruction until the in-flight frame that may reference them has completed.
extern GDIDynamicDriver* GDynamicGDI;

// During GDIDynamicDriver teardown the dynamic_cast yields null (the derived
// object is already destructed) — fall back to immediate destruction, which is
// safe there because VulkanDynamicGDI::Destroy() has already waited idle.
static VulkanDynamicGDI* GetVKDriver()
{
    return dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI);
}

// ---------------------------------------------------------------------------
//  VKTextureResource — constructors / destructor
// ---------------------------------------------------------------------------

VKTextureResource::VKTextureResource(VKDevice*         InDevice,
                                         const EngineCore::Image&     InImage,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InImage, 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    // Width and Height are set by the GDITextureResource(EngineCore::Image) constructor.
    AllocateImage();
    BuildSampler();
    if(!InImage.IsNull())
    {
        UploadImageData(InImage);
        bInitialized = true;
    }
}

VKTextureResource::VKTextureResource(VKDevice*         InDevice,
                                         GDITexture2D*     InParent,
                                         const EngineCore::Image&     InImage,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InImage, 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    AllocateImage();
    BuildSampler();
    if(!InImage.IsNull())
    {
        UploadImageData(InImage);
        bInitialized = true;
    }
}

VKTextureResource::VKTextureResource(VKDevice*                InDevice,
                                     const EngineCore::Image* InLevels,
                                     uint32_t                 InCount,
                                     EGDITextureBorder        InBorder,
                                     EGDITextureFilter        InFilter)
    : GDITextureResource(InLevels[0], 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    MipLevels = InCount > 0 ? InCount : 1;
    AllocateImage();
    BuildSampler();
    if(!InLevels[0].IsNull())
    {
        UploadMipChain(InLevels, MipLevels);
        bInitialized = true;
    }
}

VKTextureResource::VKTextureResource(VKDevice*                 InDevice,
                                     const VkFormat            InFormat,
                                     const GDICompressedLevel* InLevels,
                                     const uint32_t            InCount,
                                     const EGDITextureBorder   InBorder,
                                     const EGDITextureFilter   InFilter)
    : GDITextureResource()
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    Width     = int(InLevels[0].Width);
    Height    = int(InLevels[0].Height);
    Format    = InFormat;
    MipLevels = InCount > 0 ? InCount : 1;
    AllocateImage();
    BuildSampler();
    if(Image == VK_NULL_HANDLE) return;

    /* The blocks as they are, each level after the last; a block is 8 or 16
     * bytes, so every offset stays a multiple of the texel block size. */
    std::vector<const uint8_t*>    Sources(MipLevels);
    std::vector<VkDeviceSize>      Sizes(MipLevels);
    std::vector<VkBufferImageCopy> Regions(MipLevels);
    VkDeviceSize Total = 0;
    for(uint32_t Level = 0; Level < MipLevels; ++Level)
    {
        VkBufferImageCopy& Region = Regions[Level];
        Region = {};
        Region.bufferOffset                = Total;
        Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Region.imageSubresource.mipLevel   = Level;
        Region.imageSubresource.layerCount = 1;
        Region.imageExtent                 = { InLevels[Level].Width, InLevels[Level].Height, 1u };
        Sources[Level] = InLevels[Level].Data;
        Sizes[Level]   = VkDeviceSize(InLevels[Level].Bytes);
        Total = (Total + Sizes[Level] + 15u) & ~VkDeviceSize(15u);
    }
    UploadRegions(Sources.data(), Sizes.data(), Regions, Total);
    bInitialized = CurrentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VKTextureResource::VKTextureResource(VKDevice*         InDevice,
                                         GDITexture2D*     InParent,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    if(InParent)
    {
        Width  = static_cast<int>(InParent->Width);
        Height = static_cast<int>(InParent->Height);
    }
    // Allocate image with uninitialized content.
    // Caller must call Update() before sampling.
    AllocateImage();
    BuildSampler();
    // bInitialized remains false — no pixel data yet.
}

VKTextureResource::VKTextureResource(VKDevice*         InDevice,
                                         VkImageView       InExternalView,
                                         GDITexture2D*     InParent,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    if(InParent)
    {
        Width  = static_cast<int>(InParent->Width);
        Height = static_cast<int>(InParent->Height);
    }
    // Borrow the external image view (owned by the render target); build only a sampler.
    ImageView     = InExternalView;
    bOwnsImage    = false;
    CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    BuildSampler();
    bInitialized  = (ImageView != VK_NULL_HANDLE);
}

VKTextureResource::~VKTextureResource()
{
    Release();
}

// ---------------------------------------------------------------------------
//  GDITextureResource overrides
// ---------------------------------------------------------------------------

void VKTextureResource::Update(const EngineCore::Image& InImage)
{
    if(!Device || !Device->IsValid() || InImage.IsNull()) return;

    // If dimensions changed (or it had mips or blocks, which one RGBA8 image cannot refill), recreate the image.
    if(InImage.GetWidth() != Width || InImage.GetHeight() != Height || MipLevels != 1 || Format != kRGBA8)
    {
        Width  = InImage.GetWidth();
        Height = InImage.GetHeight();
        const bool bHadMips = MipLevels != 1;
        MipLevels = 1;
        Format    = kRGBA8;
        DestroyImage();
        AllocateImage();
        if(bHadMips) { BuildSampler(); }
    }

    UploadImageData(InImage);
    bInitialized = true;
}

void VKTextureResource::Release()
{
    if(!Device || !Device->IsValid()) return;

    // Borrowed (adopting) resources do not own the image/view/memory — those
    // belong to the render target and are freed by it. The sampler is always ours.
    const VkSampler      S = Sampler;
    const VkImageView    V = bOwnsImage ? ImageView   : VK_NULL_HANDLE;
    const VkImage        I = bOwnsImage ? Image       : VK_NULL_HANDLE;
    const VkDeviceMemory M = bOwnsImage ? ImageMemory : VK_NULL_HANDLE;

    Sampler = VK_NULL_HANDLE;
    ImageView = VK_NULL_HANDLE;
    if(bOwnsImage) { Image = VK_NULL_HANDLE; ImageMemory = VK_NULL_HANDLE; }

    // GL semantics allow releasing a texture right after the draw that uses it
    // (flood fill / Restore / selection overlay do exactly that). The frame command
    // buffer referencing these handles may still be recording or in flight, so
    // destruction must be deferred until the next frame-fence wait.
    if(VulkanDynamicGDI* GDI = GetVKDriver())
    {
        GDI->DeferDestroyImage(I, M, V, S);
    }
    else
    {
        VkDevice Dev = Device->GetDevice();
        if(S != VK_NULL_HANDLE) vkDestroySampler(Dev, S, nullptr);
        if(V != VK_NULL_HANDLE) vkDestroyImageView(Dev, V, nullptr);
        if(I != VK_NULL_HANDLE) vkDestroyImage(Dev, I, nullptr);
        if(M != VK_NULL_HANDLE) vkFreeMemory(Dev, M, nullptr);
    }

    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bInitialized  = false;
}

EngineCore::Image VKTextureResource::ToImage() const
{
    if(!bInitialized || !Device || !Device->IsValid() || Image == VK_NULL_HANDLE || Format != kRGBA8)
        return EngineCore::Image();

    if(Width <= 0 || Height <= 0) return EngineCore::Image();

    const VkDeviceSize ImageDataSize =
        static_cast<VkDeviceSize>(Width) * static_cast<VkDeviceSize>(Height) * 4u;

    // --- Readback staging buffer ---
    VkBuffer       ReadbackBuf = VK_NULL_HANDLE;
    VkDeviceMemory ReadbackMem = VK_NULL_HANDLE;

    if(!AllocStagingBuffer(Device, ImageDataSize,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            ReadbackBuf, ReadbackMem))
    {
        return EngineCore::Image();
    }

    // --- Transient command buffer ---
    VkCommandPool   ReadPool = VK_NULL_HANDLE;
    VkCommandBuffer ReadCmd  = VK_NULL_HANDLE;
    if(!BeginTransientCmd(Device, ReadPool, ReadCmd))
    {
        vkDestroyBuffer(Device->GetDevice(), ReadbackBuf, nullptr);
        vkFreeMemory(Device->GetDevice(), ReadbackMem, nullptr);
        return EngineCore::Image();
    }

    // Transition: SHADER_READ_ONLY → TRANSFER_SRC
    const VkImageMemoryBarrier2 ToSrc = MakeImageBarrier(
        Image, CurrentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,            VK_ACCESS_2_TRANSFER_READ_BIT);
    RecordBarrier(ReadCmd, ToSrc);

    // Copy image → buffer
    VkBufferImageCopy Region{};
    Region.bufferOffset                    = 0;
    Region.bufferRowLength                 = 0;   // tightly packed
    Region.bufferImageHeight               = 0;
    Region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    Region.imageSubresource.mipLevel       = 0;
    Region.imageSubresource.baseArrayLayer = 0;
    Region.imageSubresource.layerCount     = 1;
    Region.imageOffset                     = { 0, 0, 0 };
    Region.imageExtent                     = { static_cast<uint32_t>(Width),
                                               static_cast<uint32_t>(Height), 1u };

    vkCmdCopyImageToBuffer(ReadCmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ReadbackBuf, 1, &Region);

    // Transition back: TRANSFER_SRC → original layout
    const VkImageMemoryBarrier2 BackToShader = MakeImageBarrier(
        Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CurrentLayout,
        VK_PIPELINE_STAGE_2_COPY_BIT,            VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    RecordBarrier(ReadCmd, BackToShader);

    EndAndSubmitTransientCmd(Device, ReadPool, ReadCmd);

    // --- Map and copy to EngineCore::Image ---
    void* Mapped = nullptr;
    vkMapMemory(Device->GetDevice(), ReadbackMem, 0, ImageDataSize, 0, &Mapped);

    EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    std::memcpy(Result.GetData(), Mapped, static_cast<size_t>(ImageDataSize));

    vkUnmapMemory(Device->GetDevice(), ReadbackMem);

    vkDestroyBuffer(Device->GetDevice(), ReadbackBuf, nullptr);
    vkFreeMemory(Device->GetDevice(), ReadbackMem, nullptr);

    return Result;
}

// ---------------------------------------------------------------------------
//  Private helpers
// ---------------------------------------------------------------------------

void VKTextureResource::AllocateImage()
{
    if(!Device || !Device->IsValid() || Width <= 0 || Height <= 0) return;

    VkImageCreateInfo ImageInfo{};
    ImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ImageInfo.imageType     = VK_IMAGE_TYPE_2D;
    ImageInfo.format        = Format;
    ImageInfo.extent        = { static_cast<uint32_t>(Width),
                                 static_cast<uint32_t>(Height), 1u };
    ImageInfo.mipLevels     = MipLevels;
    ImageInfo.arrayLayers   = 1;
    ImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    ImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ImageInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT        // sampled by shaders
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT   // receives staging uploads
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // for ToImage() readback
    ImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if(vkCreateImage(Device->GetDevice(), &ImageInfo, nullptr, &Image) != VK_SUCCESS)
    {
        GDI_LOG("[VKTexture] vkCreateImage failed.")
        return;
    }

    VkMemoryRequirements MemReqs{};
    vkGetImageMemoryRequirements(Device->GetDevice(), Image, &MemReqs);

    const uint32_t MemTypeIdx =
        Device->FindMemoryType(MemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if(MemTypeIdx == UINT32_MAX)
    {
        GDI_LOG("[VKTexture] No device-local memory type for texture image.")
        vkDestroyImage(Device->GetDevice(), Image, nullptr);
        Image = VK_NULL_HANDLE;
        return;
    }

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReqs.size;
    AllocInfo.memoryTypeIndex = MemTypeIdx;

    if(vkAllocateMemory(Device->GetDevice(), &AllocInfo, nullptr, &ImageMemory) != VK_SUCCESS)
    {
        GDI_LOG("[VKTexture] vkAllocateMemory failed for texture.")
        vkDestroyImage(Device->GetDevice(), Image, nullptr);
        Image = VK_NULL_HANDLE;
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
    ViewInfo.subresourceRange.levelCount     = MipLevels;
    ViewInfo.subresourceRange.baseArrayLayer = 0;
    ViewInfo.subresourceRange.layerCount     = 1;

    if(vkCreateImageView(Device->GetDevice(), &ViewInfo, nullptr, &ImageView) != VK_SUCCESS)
    {
        GDI_LOG("[VKTexture] vkCreateImageView failed.")
        vkDestroyImage(Device->GetDevice(), Image, nullptr);   Image = VK_NULL_HANDLE;
        vkFreeMemory(Device->GetDevice(), ImageMemory, nullptr); ImageMemory = VK_NULL_HANDLE;
        return;
    }

    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ApplyDebugName();
}

void VKTextureResource::DestroyImage()
{
    if(!Device || !Device->IsValid()) return;

    const VkImageView    V = ImageView;
    const VkImage        I = Image;
    const VkDeviceMemory M = ImageMemory;
    ImageView = VK_NULL_HANDLE; Image = VK_NULL_HANDLE; ImageMemory = VK_NULL_HANDLE;

    // Deferred like Release(): Update() recreates the image mid-frame while the
    // old one may still be referenced by recorded draws.
    if(VulkanDynamicGDI* GDI = GetVKDriver())
    {
        GDI->DeferDestroyImage(I, M, V, VK_NULL_HANDLE);
    }
    else
    {
        VkDevice Dev = Device->GetDevice();
        if(V != VK_NULL_HANDLE) vkDestroyImageView(Dev, V, nullptr);
        if(I != VK_NULL_HANDLE) vkDestroyImage(Dev, I, nullptr);
        if(M != VK_NULL_HANDLE) vkFreeMemory(Dev, M, nullptr);
    }
    CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VKTextureResource::BuildSampler()
{
    if(!Device || !Device->IsValid()) return;

    // Destroy any existing sampler before rebuilding.
    if(Sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(Device->GetDevice(), Sampler, nullptr);
        Sampler = VK_NULL_HANDLE;
    }

    VkSamplerCreateInfo SamplerInfo{};
    SamplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    SamplerInfo.magFilter               = DecodeFilter(StoredFilter);
    SamplerInfo.minFilter               = DecodeFilter(StoredFilter);
    SamplerInfo.mipmapMode              = (StoredFilter == GDI_TF_Linear)
                                              ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                              : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    SamplerInfo.addressModeU            = DecodeAddressMode(StoredBorder);
    SamplerInfo.addressModeV            = DecodeAddressMode(StoredBorder);
    SamplerInfo.addressModeW            = DecodeAddressMode(StoredBorder);
    SamplerInfo.anisotropyEnable        = VK_FALSE;
    SamplerInfo.maxAnisotropy           = 1.0f;
    SamplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    SamplerInfo.unnormalizedCoordinates = VK_FALSE;
    SamplerInfo.compareEnable           = VK_FALSE;
    SamplerInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
    SamplerInfo.minLod                  = 0.0f;
    SamplerInfo.maxLod                  = float(MipLevels - 1);   // 0 without a mip chain

    if(vkCreateSampler(Device->GetDevice(), &SamplerInfo, nullptr, &Sampler) != VK_SUCCESS)
    {
        GDI_LOG("[VKTexture] vkCreateSampler failed.")
    }
}

void VKTextureResource::UploadImageData(const EngineCore::Image& InImage)
{
    if(!Device || !Device->IsValid() || Image == VK_NULL_HANDLE) return;

    // Convert to RGBA8888 — maps byte-for-byte onto VK_FORMAT_R8G8B8A8_UNORM.
    const EngineCore::Image Formatted = InImage.ConvertedTo(EngineCore::EImageFormat::RGBA8);

    const VkDeviceSize ImageDataSize =
        static_cast<VkDeviceSize>(Formatted.GetWidth())
      * static_cast<VkDeviceSize>(Formatted.GetHeight()) * 4u;

    // --- Staging buffer ---
    VkBuffer       StagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory StagingMem = VK_NULL_HANDLE;

    if(!AllocStagingBuffer(Device, ImageDataSize,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            StagingBuf, StagingMem))
    {
        return;
    }

    void* Mapped = nullptr;
    vkMapMemory(Device->GetDevice(), StagingMem, 0, ImageDataSize, 0, &Mapped);
    std::memcpy(Mapped, Formatted.GetData(), static_cast<size_t>(ImageDataSize));
    vkUnmapMemory(Device->GetDevice(), StagingMem);

    // --- Transient command buffer ---
    VkCommandPool   UploadPool = VK_NULL_HANDLE;
    VkCommandBuffer UploadCmd  = VK_NULL_HANDLE;
    if(!BeginTransientCmd(Device, UploadPool, UploadCmd))
    {
        vkDestroyBuffer(Device->GetDevice(), StagingBuf, nullptr);
        vkFreeMemory(Device->GetDevice(), StagingMem, nullptr);
        return;
    }

    // Transition: CurrentLayout → TRANSFER_DST_OPTIMAL
    const VkImageMemoryBarrier2 ToTransfer = MakeImageBarrier(
        Image, CurrentLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COPY_BIT,        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    RecordBarrier(UploadCmd, ToTransfer);

    // Copy staged data into the image
    VkBufferImageCopy Region{};
    Region.bufferOffset                    = 0;
    Region.bufferRowLength                 = 0;
    Region.bufferImageHeight               = 0;
    Region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    Region.imageSubresource.mipLevel       = 0;
    Region.imageSubresource.baseArrayLayer = 0;
    Region.imageSubresource.layerCount     = 1;
    Region.imageOffset                     = { 0, 0, 0 };
    Region.imageExtent                     = { static_cast<uint32_t>(Formatted.GetWidth()),
                                               static_cast<uint32_t>(Formatted.GetHeight()), 1u };

    vkCmdCopyBufferToImage(UploadCmd, StagingBuf, Image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);

    // Transition: TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    const VkImageMemoryBarrier2 ToShader = MakeImageBarrier(
        Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,            VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    RecordBarrier(UploadCmd, ToShader);

    EndAndSubmitTransientCmd(Device, UploadPool, UploadCmd);

    // Staging cleanup
    vkDestroyBuffer(Device->GetDevice(), StagingBuf, nullptr);
    vkFreeMemory(Device->GetDevice(), StagingMem, nullptr);

    CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VKTextureResource::UploadMipChain(const EngineCore::Image* InLevels, const uint32_t InCount)
{
    if(!Device || !Device->IsValid() || Image == VK_NULL_HANDLE || InCount == 0) return;

    // Each level as RGBA8, packed one after another in one staging buffer.
    std::vector<EngineCore::Image> Formatted;
    Formatted.reserve(InCount);
    std::vector<VkBufferImageCopy> Regions(InCount);
    VkDeviceSize Total = 0;
    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        Formatted.push_back(InLevels[Level].ConvertedTo(EngineCore::EImageFormat::RGBA8));
        VkBufferImageCopy& Region = Regions[Level];
        Region = {};
        Region.bufferOffset                    = Total;
        Region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        Region.imageSubresource.mipLevel       = Level;
        Region.imageSubresource.layerCount     = 1;
        Region.imageExtent                     = { static_cast<uint32_t>(Formatted.back().GetWidth()),
                                                   static_cast<uint32_t>(Formatted.back().GetHeight()), 1u };
        Total += static_cast<VkDeviceSize>(Formatted.back().GetSizeBytes());
        Total  = (Total + 3u) & ~VkDeviceSize(3u);   // bufferOffset: a multiple of the texel size
    }

    std::vector<const uint8_t*> Sources(InCount);
    std::vector<VkDeviceSize>   Sizes(InCount);
    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        Sources[Level] = Formatted[Level].GetData();
        Sizes[Level]   = VkDeviceSize(Formatted[Level].GetSizeBytes());
    }
    UploadRegions(Sources.data(), Sizes.data(), Regions, Total);
}

void VKTextureResource::UploadRegions(const uint8_t* const* InSources, const VkDeviceSize* InSizes,
                                      std::vector<VkBufferImageCopy>& InRegions, const VkDeviceSize InTotal)
{
    const uint32_t Count = uint32_t(InRegions.size());

    VkBuffer       StagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory StagingMem = VK_NULL_HANDLE;
    if(!AllocStagingBuffer(Device, InTotal, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, StagingBuf, StagingMem))
    {
        return;
    }
    uint8_t* Mapped = nullptr;
    vkMapMemory(Device->GetDevice(), StagingMem, 0, InTotal, 0, reinterpret_cast<void**>(&Mapped));
    for(uint32_t Level = 0; Level < Count; ++Level)
    {
        std::memcpy(Mapped + InRegions[Level].bufferOffset, InSources[Level], size_t(InSizes[Level]));
    }
    vkUnmapMemory(Device->GetDevice(), StagingMem);

    VkCommandPool   UploadPool = VK_NULL_HANDLE;
    VkCommandBuffer UploadCmd  = VK_NULL_HANDLE;
    if(!BeginTransientCmd(Device, UploadPool, UploadCmd))
    {
        vkDestroyBuffer(Device->GetDevice(), StagingBuf, nullptr);
        vkFreeMemory(Device->GetDevice(), StagingMem, nullptr);
        return;
    }

    VkImageMemoryBarrier2 ToTransfer = MakeImageBarrier(
        Image, CurrentLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COPY_BIT,        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ToTransfer.subresourceRange.levelCount = Count;
    RecordBarrier(UploadCmd, ToTransfer);

    vkCmdCopyBufferToImage(UploadCmd, StagingBuf, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Count, InRegions.data());

    VkImageMemoryBarrier2 ToShader = MakeImageBarrier(
        Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,            VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    ToShader.subresourceRange.levelCount = Count;
    RecordBarrier(UploadCmd, ToShader);

    EndAndSubmitTransientCmd(Device, UploadPool, UploadCmd);

    vkDestroyBuffer(Device->GetDevice(), StagingBuf, nullptr);
    vkFreeMemory(Device->GetDevice(), StagingMem, nullptr);

    CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ---------------------------------------------------------------------------
//  Decode helpers
// ---------------------------------------------------------------------------

VkSamplerAddressMode VKTextureResource::DecodeAddressMode(EGDITextureBorder InBorder)
{
    switch(InBorder)
    {
        case GDI_TB_Repeat:          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case GDI_TB_Mirrored_Repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case GDI_TB_Clamp_To_Edge:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case GDI_TB_Clamp_To_Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: break;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkFilter VKTextureResource::DecodeFilter(EGDITextureFilter InFilter)
{
    switch(InFilter)
    {
        case GDI_TF_Nearest: return VK_FILTER_NEAREST;
        case GDI_TF_Linear:  return VK_FILTER_LINEAR;
        default: break;
    }
    return VK_FILTER_NEAREST;
}

void VKTextureResource::SetDebugName(const std::string& InName)
{
    GDITextureResource::SetDebugName(InName);
    ApplyDebugName();
}

void VKTextureResource::ApplyDebugName() const
{
    if(!Device || DebugName.empty())
    {
        return;
    }
    Device->SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(Image), DebugName);
    Device->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(ImageView), DebugName + " View");
}

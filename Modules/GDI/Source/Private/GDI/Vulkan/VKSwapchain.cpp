#include "Public/GDI/Vulkan/VKSwapchain.h"
#include "Public/GDI/GDIPlatform.h"
#include "Public/GDI/GDI.h"
#include <mutex>
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VKRenderTarget.h"


#include <algorithm>
#include <vector>
#include <cstring>
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
#include "Public/Core/CoreFileProvider.h"

VKSwapchain::~VKSwapchain()
{
    Shutdown();
}

bool VKSwapchain::Initialize(VkInstance InInstance, VKDevice* InDevice, EngineCore::WindowHandle InWindow)
{
    Instance = InInstance;
    Device   = InDevice;
    Window   = InWindow;
    if(Instance == VK_NULL_HANDLE || !Device || !Window.IsValid()) return false;

    // Surface through the platform seam, replacing
    // QVulkanInstance::surfaceForWindow.
    const VkResult SurfaceResult = GDIPlatform::appVulkanCreateSurface(Instance, Window, &Surface);
    if(SurfaceResult != VK_SUCCESS || Surface == VK_NULL_HANDLE)
    {
        GDI_LOG("[VKSwapchain] appVulkanCreateSurface failed (" + ::GDILogDetail::GDINum(SurfaceResult) + ").")
        return false;
    }

    // Verify the chosen graphics queue family also supports present.
    VkBool32 PresentSupport = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(Device->GetPhysicalDevice(),
                                         Device->GetGraphicsQueueFamily(),
                                         Surface, &PresentSupport);
    if(!PresentSupport)
    {
        GDI_LOG("[VKSwapchain] Graphics queue family does not support present on this surface.")
        return false;
    }

    if(!CreateSwapchain())          return false;
    if(!CreateUICommandResources()) return false;
    if(!CreateBlitPipeline())       return false;
    return true;
}

void VKSwapchain::Shutdown()
{
    if(Device && Device->IsValid())
    {
        Device->WaitIdle();
        DestroyBlitPipeline();
        DestroyUICommandResources();
        DestroySwapchain();
    }
    if(Instance != VK_NULL_HANDLE && Surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(Instance, Surface, nullptr);
        Surface = VK_NULL_HANDLE;
    }
    Instance = VK_NULL_HANDLE;
    Device   = nullptr;
    Window   = EngineCore::WindowHandle();
}

bool VKSwapchain::Recreate()
{
    if(!Device || !Surface) return false;
    Device->WaitIdle();

    // Recreate the per-frame ImageAvailable semaphore: frames skipped during a resize
    // (acquire returning OUT_OF_DATE) can leave it in an inconsistent signaled/pending
    // state; the WaitIdle above guarantees it's no longer in use, so a fresh unsignaled
    // one avoids "semaphore signaled but may still be in use" hazards. (InFlightFence is
    // left as-is — signaled after WaitIdle so the next present's wait passes. The
    // per-image BlitFinished semaphores are recreated by DestroySwapchain/CreateSwapchain.)
    VkDevice Dev = Device->GetDevice();
    if(ImageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(Dev, ImageAvailable, nullptr);
    VkSemaphoreCreateInfo SemInfo{};
    SemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(Dev, &SemInfo, nullptr, &ImageAvailable);

    DestroySwapchain();
    return CreateSwapchain();
}

bool VKSwapchain::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR Caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Device->GetPhysicalDevice(), Surface, &Caps);

    // Pick a B8G8R8A8_UNORM SRGB format if available, else first.
    uint32_t FormatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(Device->GetPhysicalDevice(), Surface, &FormatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> Formats(FormatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(Device->GetPhysicalDevice(), Surface, &FormatCount, Formats.data());

    VkSurfaceFormatKHR Chosen = Formats[0];
    for(const auto& F : Formats)
    {
        if(F.format == VK_FORMAT_B8G8R8A8_UNORM && F.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            Chosen = F;
            break;
        }
    }
    SurfaceFormat = Chosen.format;
    ColorSpace    = Chosen.colorSpace;

    // FIFO (vsync) is the guaranteed fallback, but it queues presents up to the full
    // swapchain depth: during a brush stroke the UI thread presents every finished
    // render frame, and each queued present waits its own vblank — 3-4 frames of
    // stroke latency at 60Hz. MAILBOX replaces the pending image with the newest
    // frame (no queue buildup, no tearing); IMMEDIATE skips the queue entirely
    // (composited windowed mode doesn't tear). With VSync off (GDIGetVSync), prefer
    // MAILBOX > IMMEDIATE > FIFO. With it on, FIFO is the point: its acquire and
    // fence waits are what hold the game loop to the display's refresh.
    uint32_t PresentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(Device->GetPhysicalDevice(), Surface, &PresentModeCount, nullptr);
    std::vector<VkPresentModeKHR> PresentModes(PresentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(Device->GetPhysicalDevice(), Surface, &PresentModeCount, PresentModes.data());

    bVSync = GDIGetVSync();
    VkPresentModeKHR PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for(const VkPresentModeKHR Mode : PresentModes)
    {
        if(bVSync) break;
        if(Mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            PresentMode = Mode;
            break;
        }
        if(Mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
            PresentMode = Mode;
        }
    }
    GDI_LOG(std::string("[VKSwapchain] Present mode: ")
          + (PresentMode == VK_PRESENT_MODE_MAILBOX_KHR   ? "MAILBOX"
           : PresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                          : "FIFO")
          + (bVSync ? " (VSync on)" : " (VSync off)"))

    // Choose extent.
    if(Caps.currentExtent.width != UINT32_MAX)
    {
        Extent = Caps.currentExtent;
    }
    else
    {
        // Physical pixels straight from the platform; GetClientRect needs no
        // devicePixelRatio the way QWindow::width() did.
        uint32_t PixelWidth = 0, PixelHeight = 0;
        GDIPlatform::appGetWindowPixelSize(Window, PixelWidth, PixelHeight);
        Extent.width  = std::clamp<uint32_t>(PixelWidth,  Caps.minImageExtent.width,  Caps.maxImageExtent.width);
        Extent.height = std::clamp<uint32_t>(PixelHeight, Caps.minImageExtent.height, Caps.maxImageExtent.height);
    }

    uint32_t ImageCount = Caps.minImageCount + 1;
    if(Caps.maxImageCount > 0 && ImageCount > Caps.maxImageCount) ImageCount = Caps.maxImageCount;

    VkSwapchainCreateInfoKHR CreateInfo{};
    CreateInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    CreateInfo.surface          = Surface;
    CreateInfo.minImageCount    = ImageCount;
    CreateInfo.imageFormat      = SurfaceFormat;
    CreateInfo.imageColorSpace  = ColorSpace;
    CreateInfo.imageExtent      = Extent;
    CreateInfo.imageArrayLayers = 1;
    CreateInfo.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CreateInfo.preTransform     = Caps.currentTransform;
    CreateInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    CreateInfo.presentMode      = PresentMode;
    CreateInfo.clipped          = VK_TRUE;
    CreateInfo.oldSwapchain     = VK_NULL_HANDLE;

    if(vkCreateSwapchainKHR(Device->GetDevice(), &CreateInfo, nullptr, &Swapchain) != VK_SUCCESS)
    {
        GDI_LOG("[VKSwapchain] vkCreateSwapchainKHR failed.")
        return false;
    }

    uint32_t ActualImageCount = 0;
    vkGetSwapchainImagesKHR(Device->GetDevice(), Swapchain, &ActualImageCount, nullptr);
    SwapchainImages.resize(ActualImageCount);
    vkGetSwapchainImagesKHR(Device->GetDevice(), Swapchain, &ActualImageCount, SwapchainImages.data());

    SwapchainImageViews.resize(ActualImageCount);
    for(uint32_t i = 0; i < ActualImageCount; ++i)
    {
        VkImageViewCreateInfo ViewInfo{};
        ViewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ViewInfo.image    = SwapchainImages[i];
        ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ViewInfo.format   = SurfaceFormat;
        ViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ViewInfo.subresourceRange.levelCount = 1;
        ViewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(Device->GetDevice(), &ViewInfo, nullptr, &SwapchainImageViews[i]);
    }

    // One present-wait (BlitFinished) semaphore per swapchain image.
    BlitFinishedPerImage.resize(ActualImageCount);
    VkSemaphoreCreateInfo BFInfo{};
    BFInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for(uint32_t i = 0; i < ActualImageCount; ++i)
        vkCreateSemaphore(Device->GetDevice(), &BFInfo, nullptr, &BlitFinishedPerImage[i]);

    return true;
}

void VKSwapchain::DestroySwapchain()
{
    if(!Device || !Device->IsValid()) return;
    VkDevice Dev = Device->GetDevice();
    for(VkImageView V : SwapchainImageViews)
    {
        if(V != VK_NULL_HANDLE) vkDestroyImageView(Dev, V, nullptr);
    }
    SwapchainImageViews.clear();
    SwapchainImages.clear();

    for(VkSemaphore S : BlitFinishedPerImage)
    {
        if(S != VK_NULL_HANDLE) vkDestroySemaphore(Dev, S, nullptr);
    }
    BlitFinishedPerImage.clear();
    if(Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(Dev, Swapchain, nullptr);
        Swapchain = VK_NULL_HANDLE;
    }
}

bool VKSwapchain::CreateUICommandResources()
{
    VkCommandPoolCreateInfo PoolInfo{};
    PoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    PoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    PoolInfo.queueFamilyIndex = Device->GetGraphicsQueueFamily();
    if(vkCreateCommandPool(Device->GetDevice(), &PoolInfo, nullptr, &UICommandPool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo AllocInfo{};
    AllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    AllocInfo.commandPool        = UICommandPool;
    AllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    AllocInfo.commandBufferCount = 1;
    if(vkAllocateCommandBuffers(Device->GetDevice(), &AllocInfo, &UICommandBuffer) != VK_SUCCESS)
        return false;

    // ImageAvailable is per-frame (1 frame in flight); BlitFinished is per-image and
    // lives with the swapchain (see CreateSwapchain).
    VkSemaphoreCreateInfo SemInfo{};
    SemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(Device->GetDevice(), &SemInfo, nullptr, &ImageAvailable);

    VkFenceCreateInfo FenceInfo{};
    FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(Device->GetDevice(), &FenceInfo, nullptr, &InFlightFence);
    return true;
}

void VKSwapchain::DestroyUICommandResources()
{
    if(!Device || !Device->IsValid()) return;
    VkDevice Dev = Device->GetDevice();
    if(InFlightFence  != VK_NULL_HANDLE) { vkDestroyFence(Dev, InFlightFence, nullptr); InFlightFence = VK_NULL_HANDLE; }
    if(ImageAvailable != VK_NULL_HANDLE) { vkDestroySemaphore(Dev, ImageAvailable, nullptr); ImageAvailable = VK_NULL_HANDLE; }
    if(UICommandPool  != VK_NULL_HANDLE) { vkDestroyCommandPool(Dev, UICommandPool, nullptr); UICommandPool = VK_NULL_HANDLE; }
    UICommandBuffer = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Screen-quad blit pipeline
// ---------------------------------------------------------------------------

VkShaderModule VKSwapchain::LoadShaderModule(const std::string& SpvResourcePath) const
{
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(Provider == nullptr)
    {
        GDI_LOG("[VKSwapchain] No file provider installed for: " + SpvResourcePath)
        return VK_NULL_HANDLE;
    }

    std::vector<uint8_t> Bytes;
    if(!Provider->ReadAll(SpvResourcePath, Bytes))
    {
        GDI_LOG("[VKSwapchain] Failed to open SPIR-V resource: " + SpvResourcePath)
        return VK_NULL_HANDLE;
    }
    if(Bytes.empty() || (Bytes.size() % 4 != 0))
    {
        GDI_LOG("[VKSwapchain] Invalid SPIR-V size for: " + SpvResourcePath)
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo Info{};
    Info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    Info.codeSize = static_cast<size_t>(Bytes.size());
    Info.pCode    = reinterpret_cast<const uint32_t*>(Bytes.data());

    VkShaderModule Module = VK_NULL_HANDLE;
    if(vkCreateShaderModule(Device->GetDevice(), &Info, nullptr, &Module) != VK_SUCCESS)
    {
        GDI_LOG("[VKSwapchain] vkCreateShaderModule failed for: " + SpvResourcePath)
        return VK_NULL_HANDLE;
    }
    return Module;
}

bool VKSwapchain::CreateBlitPipeline()
{
    VkDevice Dev = Device->GetDevice();

    BlitVS = LoadShaderModule("Shaders/Cooked/Vulkan/Screen.vsh.spv");
    BlitFS = LoadShaderModule("Shaders/Cooked/Vulkan/Screen.fsh.spv");
    if(BlitVS == VK_NULL_HANDLE || BlitFS == VK_NULL_HANDLE) return false;

    // Sampler for reading the offscreen RT.
    VkSamplerCreateInfo SamplerInfo{};
    SamplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    SamplerInfo.magFilter    = VK_FILTER_LINEAR;
    SamplerInfo.minFilter    = VK_FILTER_LINEAR;
    SamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    SamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(Dev, &SamplerInfo, nullptr, &BlitSampler) != VK_SUCCESS) return false;

    // Descriptor set layout: just the combined image/sampler at binding 2
    // (Screen.fsh: SAMPLER2D(2, ScreenTexture); the Common.glsl UBOs at 0/1 are
    // unused by both stages and stripped from the SPIR-V, so they need no binding).
    VkDescriptorSetLayoutBinding SamplerBinding{};
    SamplerBinding.binding         = 2;
    SamplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    SamplerBinding.descriptorCount = 1;
    SamplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo SetLayoutInfo{};
    SetLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    SetLayoutInfo.bindingCount = 1;
    SetLayoutInfo.pBindings    = &SamplerBinding;
    if(vkCreateDescriptorSetLayout(Dev, &SetLayoutInfo, nullptr, &BlitSetLayout) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo PipeLayoutInfo{};
    PipeLayoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    PipeLayoutInfo.setLayoutCount = 1;
    PipeLayoutInfo.pSetLayouts    = &BlitSetLayout;
    if(vkCreatePipelineLayout(Dev, &PipeLayoutInfo, nullptr, &BlitPipelineLayout) != VK_SUCCESS) return false;

    // Descriptor pool + a single set (re-written each frame; one frame in flight).
    VkDescriptorPoolSize PoolSize{};
    PoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    PoolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo PoolInfo{};
    PoolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    PoolInfo.maxSets       = 1;
    PoolInfo.poolSizeCount = 1;
    PoolInfo.pPoolSizes    = &PoolSize;
    if(vkCreateDescriptorPool(Dev, &PoolInfo, nullptr, &BlitDescPool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo SetAlloc{};
    SetAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    SetAlloc.descriptorPool     = BlitDescPool;
    SetAlloc.descriptorSetCount = 1;
    SetAlloc.pSetLayouts        = &BlitSetLayout;
    if(vkAllocateDescriptorSets(Dev, &SetAlloc, &BlitDescSet) != VK_SUCCESS) return false;

    // Full-screen NDC quad: 6 interleaved verts {vec3 position, vec2 texcoord}.
    // The offscreen RT is stored top-down (row 0 = visual top) and this blit's
    // viewport is positive, so Vulkan NDC (-1,-1) is the top-left of the
    // swapchain image: v = 0 there copies the rows as they are.
    const float QuadVerts[] = {
        // pos.x pos.y pos.z   u    v
        -1.f, -1.f, 0.f,      0.f, 0.f,
        -1.f,  1.f, 0.f,      0.f, 1.f,
         1.f,  1.f, 0.f,      1.f, 1.f,
         1.f,  1.f, 0.f,      1.f, 1.f,
         1.f, -1.f, 0.f,      1.f, 0.f,
        -1.f, -1.f, 0.f,      0.f, 0.f,
    };
    const VkDeviceSize QuadSize = sizeof(QuadVerts);

    VkBufferCreateInfo BufInfo{};
    BufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufInfo.size        = QuadSize;
    BufInfo.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    BufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(Dev, &BufInfo, nullptr, &QuadVertexBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements MemReq{};
    vkGetBufferMemoryRequirements(Dev, QuadVertexBuffer, &MemReq);

    VkMemoryAllocateInfo MemAlloc{};
    MemAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    MemAlloc.allocationSize  = MemReq.size;
    MemAlloc.memoryTypeIndex = Device->FindMemoryType(MemReq.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(MemAlloc.memoryTypeIndex == UINT32_MAX) return false;
    if(vkAllocateMemory(Dev, &MemAlloc, nullptr, &QuadVertexMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(Dev, QuadVertexBuffer, QuadVertexMemory, 0);

    void* Mapped = nullptr;
    vkMapMemory(Dev, QuadVertexMemory, 0, QuadSize, 0, &Mapped);
    std::memcpy(Mapped, QuadVerts, static_cast<size_t>(QuadSize));
    vkUnmapMemory(Dev, QuadVertexMemory);

    // Graphics pipeline (dynamic rendering, swapchain color format).
    VkPipelineShaderStageCreateInfo Stages[2]{};
    Stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    Stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    Stages[0].module = BlitVS;
    Stages[0].pName  = "main";
    Stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    Stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    Stages[1].module = BlitFS;
    Stages[1].pName  = "main";

    VkVertexInputBindingDescription VtxBinding{};
    VtxBinding.binding   = 0;
    VtxBinding.stride    = 5 * sizeof(float);
    VtxBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription VtxAttribs[2]{};
    VtxAttribs[0].location = 0;
    VtxAttribs[0].binding  = 0;
    VtxAttribs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    VtxAttribs[0].offset   = 0;
    VtxAttribs[1].location = 1;
    VtxAttribs[1].binding  = 0;
    VtxAttribs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    VtxAttribs[1].offset   = 3 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo VtxInput{};
    VtxInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VtxInput.vertexBindingDescriptionCount   = 1;
    VtxInput.pVertexBindingDescriptions      = &VtxBinding;
    VtxInput.vertexAttributeDescriptionCount = 2;
    VtxInput.pVertexAttributeDescriptions    = VtxAttribs;

    VkPipelineInputAssemblyStateCreateInfo InputAsm{};
    InputAsm.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    InputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo Viewport{};
    Viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    Viewport.viewportCount = 1;
    Viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo Raster{};
    Raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    Raster.polygonMode = VK_POLYGON_MODE_FILL;
    Raster.cullMode    = VK_CULL_MODE_NONE;
    Raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    Raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo Multisample{};
    Multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState BlendAtt{};
    BlendAtt.blendEnable    = VK_FALSE;
    BlendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo Blend{};
    Blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    Blend.attachmentCount = 1;
    Blend.pAttachments    = &BlendAtt;

    const VkDynamicState DynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo Dynamic{};
    Dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    Dynamic.dynamicStateCount = 2;
    Dynamic.pDynamicStates    = DynStates;

    VkPipelineRenderingCreateInfo RenderingInfo{};
    RenderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    RenderingInfo.colorAttachmentCount    = 1;
    RenderingInfo.pColorAttachmentFormats = &SurfaceFormat;

    VkGraphicsPipelineCreateInfo PipeInfo{};
    PipeInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    PipeInfo.pNext               = &RenderingInfo;
    PipeInfo.stageCount          = 2;
    PipeInfo.pStages             = Stages;
    PipeInfo.pVertexInputState   = &VtxInput;
    PipeInfo.pInputAssemblyState = &InputAsm;
    PipeInfo.pViewportState      = &Viewport;
    PipeInfo.pRasterizationState = &Raster;
    PipeInfo.pMultisampleState   = &Multisample;
    PipeInfo.pColorBlendState    = &Blend;
    PipeInfo.pDynamicState       = &Dynamic;
    PipeInfo.layout              = BlitPipelineLayout;
    PipeInfo.renderPass          = VK_NULL_HANDLE;   // dynamic rendering

    if(vkCreateGraphicsPipelines(Dev, VK_NULL_HANDLE, 1, &PipeInfo, nullptr, &BlitPipeline) != VK_SUCCESS)
    {
        GDI_LOG("[VKSwapchain] vkCreateGraphicsPipelines (blit) failed.")
        return false;
    }
    return true;
}

void VKSwapchain::DestroyBlitPipeline()
{
    if(!Device || !Device->IsValid()) return;
    VkDevice Dev = Device->GetDevice();

    if(BlitPipeline       != VK_NULL_HANDLE) { vkDestroyPipeline(Dev, BlitPipeline, nullptr); BlitPipeline = VK_NULL_HANDLE; }
    if(BlitPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(Dev, BlitPipelineLayout, nullptr); BlitPipelineLayout = VK_NULL_HANDLE; }
    if(BlitSetLayout      != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(Dev, BlitSetLayout, nullptr); BlitSetLayout = VK_NULL_HANDLE; }
    if(BlitDescPool       != VK_NULL_HANDLE) { vkDestroyDescriptorPool(Dev, BlitDescPool, nullptr); BlitDescPool = VK_NULL_HANDLE; BlitDescSet = VK_NULL_HANDLE; }
    if(BlitSampler        != VK_NULL_HANDLE) { vkDestroySampler(Dev, BlitSampler, nullptr); BlitSampler = VK_NULL_HANDLE; }
    if(QuadVertexBuffer   != VK_NULL_HANDLE) { vkDestroyBuffer(Dev, QuadVertexBuffer, nullptr); QuadVertexBuffer = VK_NULL_HANDLE; }
    if(QuadVertexMemory   != VK_NULL_HANDLE) { vkFreeMemory(Dev, QuadVertexMemory, nullptr); QuadVertexMemory = VK_NULL_HANDLE; }
    if(BlitVS             != VK_NULL_HANDLE) { vkDestroyShaderModule(Dev, BlitVS, nullptr); BlitVS = VK_NULL_HANDLE; }
    if(BlitFS             != VK_NULL_HANDLE) { vkDestroyShaderModule(Dev, BlitFS, nullptr); BlitFS = VK_NULL_HANDLE; }
}

void VKSwapchain::TransitionImageLayout(VkCommandBuffer Cmd,
                                         VkImage Image,
                                         VkImageLayout OldLayout, VkImageLayout NewLayout,
                                         VkAccessFlags2 SrcAccess, VkAccessFlags2 DstAccess,
                                         VkPipelineStageFlags2 SrcStage, VkPipelineStageFlags2 DstStage)
{
    VkImageMemoryBarrier2 Barrier{};
    Barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    Barrier.srcStageMask  = SrcStage;
    Barrier.srcAccessMask = SrcAccess;
    Barrier.dstStageMask  = DstStage;
    Barrier.dstAccessMask = DstAccess;
    Barrier.oldLayout     = OldLayout;
    Barrier.newLayout     = NewLayout;
    Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    Barrier.image         = Image;
    Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Barrier.subresourceRange.levelCount = 1;
    Barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo Dep{};
    Dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    Dep.imageMemoryBarrierCount = 1;
    Dep.pImageMemoryBarriers    = &Barrier;
    vkCmdPipelineBarrier2(Cmd, &Dep);
}

void VKSwapchain::PresentFrame(VKRenderTarget* InMainRT, VkSemaphore InRenderTimeline, const std::atomic<uint64_t>* InPublishedValue)
{
    if(!IsValid() || !InMainRT || !InMainRT->IsValid()) return;

    // Only present an RT whose tracked layout says it is in its idle, sampleable
    // state. If the render thread is mid-frame (RT re-bound as color attachment) or
    // the RT has never been rendered to (UNDEFINED), skip — a fresh present is
    // triggered again after the frame completes (RendererFinished). Reading the
    // tracked layout cross-thread is a benign stale-read at worst; crucially we do
    // NOT transition or overwrite it here — doing so poisoned the render thread's
    // layout tracking and broke every subsequent RT-sampling draw (brush strokes).
    if(InMainRT->GetCurrentLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;

    // The present mode is fixed at creation: a VSync change rebuilds the chain.
    // Recreate idles the device, which a settings toggle can afford.
    if(GDIGetVSync() != bVSync && !Recreate()) return;

    VkDevice Dev = Device->GetDevice();
    vkWaitForFences(Dev, 1, &InFlightFence, VK_TRUE, UINT64_MAX);

    uint32_t ImageIndex = 0;
    VkResult Acquire = vkAcquireNextImageKHR(Dev, Swapchain, UINT64_MAX, ImageAvailable, VK_NULL_HANDLE, &ImageIndex);
    if(Acquire == VK_ERROR_OUT_OF_DATE_KHR)
    {
        Recreate();
        return;
    }
    if(Acquire != VK_SUCCESS && Acquire != VK_SUBOPTIMAL_KHR)
    {
        return;
    }

    vkResetFences(Dev, 1, &InFlightFence);
    vkResetCommandPool(Dev, UICommandPool, 0);

    /*
     * If the blit pipeline failed to build -- a missing or unreadable
     * Screen.vsh/fsh SPIR-V resource is the realistic cause -- BlitDescSet is
     * null, and handing that to vkUpdateDescriptorSets faults inside the
     * driver with no usable diagnostic. Fail visibly here instead.
     */
    if(BlitDescSet == VK_NULL_HANDLE)
    {
        GDI_LOG_ERROR("[VKSwapchain] Blit descriptor set is null - the blit pipeline failed to build; skipping present.")
        return;
    }

    // Point the blit descriptor set at the offscreen RT view. Safe to update now:
    // the InFlightFence wait above guarantees the previous present has finished.
    VkDescriptorImageInfo ImageInfo{};
    ImageInfo.sampler     = BlitSampler;
    ImageInfo.imageView   = InMainRT->GetImageView();
    ImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet Write{};
    Write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    Write.dstSet          = BlitDescSet;
    Write.dstBinding      = 2;
    Write.descriptorCount = 1;
    Write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    Write.pImageInfo      = &ImageInfo;
    vkUpdateDescriptorSets(Dev, 1, &Write, 0, nullptr);

    VkCommandBufferBeginInfo BeginInfo{};
    BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(UICommandBuffer, &BeginInfo);

    // (The offscreen RT layout was verified SHADER_READ_ONLY_OPTIMAL above; the
    // render thread owns all transitions of it.)

    // Transition swapchain image: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    TransitionImageLayout(UICommandBuffer, SwapchainImages[ImageIndex],
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

    // Dynamic-rendering pass: draw the full-screen quad sampling the offscreen RT.
    VkRenderingAttachmentInfo ColorAtt{};
    ColorAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ColorAtt.imageView   = SwapchainImageViews[ImageIndex];
    ColorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ColorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // quad covers the whole target
    ColorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo Rendering{};
    Rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    Rendering.renderArea.offset    = { 0, 0 };
    Rendering.renderArea.extent    = Extent;
    Rendering.layerCount           = 1;
    Rendering.colorAttachmentCount = 1;
    Rendering.pColorAttachments    = &ColorAtt;

    vkCmdBeginRendering(UICommandBuffer, &Rendering);

    VkViewport VP{};
    VP.x = 0.f; VP.y = 0.f;
    VP.width  = float(Extent.width);
    VP.height = float(Extent.height);
    VP.minDepth = 0.f; VP.maxDepth = 1.f;
    vkCmdSetViewport(UICommandBuffer, 0, 1, &VP);

    VkRect2D Scissor{};
    Scissor.offset = { 0, 0 };
    Scissor.extent = Extent;
    vkCmdSetScissor(UICommandBuffer, 0, 1, &Scissor);

    vkCmdBindPipeline(UICommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, BlitPipeline);
    vkCmdBindDescriptorSets(UICommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            BlitPipelineLayout, 0, 1, &BlitDescSet, 0, nullptr);
    const VkDeviceSize Offset = 0;
    vkCmdBindVertexBuffers(UICommandBuffer, 0, 1, &QuadVertexBuffer, &Offset);
    vkCmdDraw(UICommandBuffer, 6, 1, 0, 0);

    vkCmdEndRendering(UICommandBuffer);

    // Transition swapchain image to PRESENT_SRC_KHR.
    TransitionImageLayout(UICommandBuffer, SwapchainImages[ImageIndex],
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(UICommandBuffer);

    // Submit waiting on ImageAvailable (@ color-attachment-output, the swapchain write)
    // and the RenderTimeline (@ fragment-shader, where we sample the RT), if available.
    VkSemaphore          WaitSems[2]   = { ImageAvailable, InRenderTimeline };
    VkPipelineStageFlags WaitStages[2] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };
    const uint32_t WaitCount = (InRenderTimeline != VK_NULL_HANDLE) ? 2u : 1u;

    // Wait values: ImageAvailable is binary (value ignored); the timeline waits for
    // the latest render submit. BlitFinished is binary (signal value ignored).
    // WaitValues[1] is patched under the queue mutex below.
    uint64_t       WaitValues[2]   = { 0, 0 };
    const uint64_t SignalValues[1] = { 0 };

    VkTimelineSemaphoreSubmitInfo TimelineSubmit{};
    TimelineSubmit.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    TimelineSubmit.waitSemaphoreValueCount   = WaitCount;
    TimelineSubmit.pWaitSemaphoreValues      = WaitValues;
    TimelineSubmit.signalSemaphoreValueCount = 1;
    TimelineSubmit.pSignalSemaphoreValues    = SignalValues;

    VkSubmitInfo Submit{};
    Submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    Submit.pNext                = (InRenderTimeline != VK_NULL_HANDLE) ? &TimelineSubmit : nullptr;
    Submit.waitSemaphoreCount   = WaitCount;
    Submit.pWaitSemaphores      = WaitSems;
    Submit.pWaitDstStageMask    = WaitStages;
    Submit.commandBufferCount   = 1;
    Submit.pCommandBuffers      = &UICommandBuffer;
    Submit.signalSemaphoreCount = 1;
    // Per-image present-wait semaphore: safe to reuse only when image ImageIndex is
    // re-acquired, which can't happen before the previous present of it completed.
    VkSemaphore PresentWait     = BlitFinishedPerImage[ImageIndex];
    Submit.pSignalSemaphores    = &PresentWait;

    // Load the published render-timeline value and submit atomically w.r.t. the
    // render thread's publish+submit (EndRendering does both under this mutex).
    // Two possible orders, both safe:
    //   - this present submits BEFORE the next render frame: that frame's
    //     ALL_COMMANDS layout barrier orders it after our sampling;
    //   - this present submits AFTER it: we loaded the newer timeline value and
    //     wait for the whole frame to complete before sampling.
    {
        std::lock_guard<std::mutex> QueueLock(Device->GetQueueMutex());
        WaitValues[1] = InPublishedValue ? InPublishedValue->load() : 0;
        Device->QueueSubmitLocked(1, &Submit, InFlightFence);
    }

    VkPresentInfoKHR Present{};
    Present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    Present.waitSemaphoreCount = 1;
    Present.pWaitSemaphores    = &PresentWait;
    Present.swapchainCount     = 1;
    Present.pSwapchains        = &Swapchain;
    Present.pImageIndices      = &ImageIndex;
    VkResult PR = Device->QueuePresent(&Present);
    if(PR == VK_ERROR_OUT_OF_DATE_KHR || PR == VK_SUBOPTIMAL_KHR)
    {
        Recreate();
    }
}

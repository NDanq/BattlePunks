#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include <vector>
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VKCommandManager.h"
#include "Public/GDI/Vulkan/VKSwapchain.h"
#include "Public/GDI/Vulkan/VKRenderTarget.h"
#include "Public/GDI/Vulkan/VKSync.h"
#include "Public/GDI/Vulkan/VKMesh.h"
#include "Public/GDI/Vulkan/VKPipelineCache.h"
#include "Public/GDI/Vulkan/VKDescriptorAllocator.h"
#include "Public/GDI/Vulkan/VKShader.h"
#include "Public/GDI/Vulkan/VKTexture.h"
#include "Public/GDI/Vulkan/VKMesh.h"
#include "Private/GDI/Vulkan/VKTransientOps.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDIView.h"

#include <cstring>

#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
#include <unordered_map>
#include "Public/Core/CoreString.h"

// At global scope, not inside a module namespace, so these are wider than the
// usual directive -- contained only because nothing includes a .cpp.
using namespace EngineCore;
using namespace VKOps;

// External global declared in GDI.h.
extern GDISync* GSync;

#pragma region VulkanGDIContext

VulkanGDIContext::VulkanGDIContext()
{
    VKContext = GetSharedVkInstance();
    ContextType = GDIContext::VulkanContext;
}

VulkanGDIContext::~VulkanGDIContext()
{
    // The shared instance lives for the application lifetime; do NOT destroy it here.
    VKContext = nullptr;
}

/*
 * Creates the process-wide VkInstance.
 *
 * GDI owns this now; it used to be a QVulkanInstance that Qt created and GDI
 * borrowed. The direction is reversed because a renderer should not depend on
 * a UI toolkit to open its device -- the Qt bridge ADOPTS this instance
 * (QVulkanInstance::setVkInstance) so the host's window can still be told
 * about it, which Qt requires before the window is created.
 *
 * Configuration is unchanged from the QVulkanInstance version: API 1.3, the
 * platform surface extensions, and validation + debug-utils in debug builds.
 */
namespace
{
    bool bDebugUtils = false;   // GetSharedVkInstance enabled VK_EXT_debug_utils
}

VkInstance VulkanGDIContext::GetSharedVkInstance()
{
    static VkInstance SharedInstance = VK_NULL_HANDLE;
    if(SharedInstance != VK_NULL_HANDLE)
    {
        return SharedInstance;
    }

    VkApplicationInfo AppInfo{};
    AppInfo.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    AppInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> Extensions = GDIPlatform::appVulkanRequiredInstanceExtensions();
    std::vector<const char*> Layers;

#ifdef QT_DEBUG
    Layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    /* Debug utils on their own, without validation: draw-event labels and
     * object names for RenderDoc and the like. Only if offered -- asking for
     * an extension the loader lacks fails vkCreateInstance outright. */
    bDebugUtils = false;
#if !(defined(NIMBLE_SHIPPING) && NIMBLE_SHIPPING)
    {
        uint32_t Count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &Count, nullptr);
        std::vector<VkExtensionProperties> Available(Count);
        vkEnumerateInstanceExtensionProperties(nullptr, &Count, Available.data());
        for(const VkExtensionProperties& Each : Available)
        {
            if(std::strcmp(Each.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
            {
                Extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                bDebugUtils = true;
                break;
            }
        }
    }
#endif

    VkInstanceCreateInfo Info{};
    Info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    Info.pApplicationInfo        = &AppInfo;
    Info.enabledExtensionCount   = uint32_t(Extensions.size());
    Info.ppEnabledExtensionNames = Extensions.empty() ? nullptr : Extensions.data();
    Info.enabledLayerCount       = uint32_t(Layers.size());
    Info.ppEnabledLayerNames     = Layers.empty() ? nullptr : Layers.data();

    const VkResult Result = vkCreateInstance(&Info, nullptr, &SharedInstance);
    if(Result != VK_SUCCESS)
    {
        GDI_LOG_ERROR("[VulkanGDIContext] vkCreateInstance failed (" + ::GDILogDetail::GDINum(Result) + ").")
        SharedInstance = VK_NULL_HANDLE;
        bDebugUtils    = false;
    }

    return SharedInstance;
}

bool VulkanGDIContext::HasDebugUtils()
{
    return bDebugUtils;
}


#pragma endregion

#pragma region VKCurrentStateData

VKCurrentStateData::VKCurrentStateData()
{
    BlendState        = new VKBlendState;
    DepthStencilState = new VKDepthStencilState;
}

VKCurrentStateData::~VKCurrentStateData()
{
    delete BlendState;        BlendState = nullptr;
    delete DepthStencilState; DepthStencilState = nullptr;
}

#pragma endregion

#pragma region VulkanDynamicGDI

VulkanDynamicGDI::VulkanDynamicGDI(WindowHandle InWindow)
    : GDIDynamicDriver(InWindow)
{
}

VulkanDynamicGDI::~VulkanDynamicGDI()
{
    Destroy();
}

void VulkanDynamicGDI::AssignContext(GDIContext* InContext)
{
    Context = InContext;
}

void VulkanDynamicGDI::Allocate()
{
    if(bInitialized) return;

    SetThreadID(appThreadID());

    auto* VKCtx = static_cast<VulkanGDIContext*>(Context);
    if(!VKCtx || !VKCtx->VKContext)
    {
        GDI_LOG("[VulkanDynamicGDI] No VulkanGDIContext assigned.")
        return;
    }

    DeviceObj = new VKDevice();
    if(!DeviceObj->Initialize(VKCtx->VKContext))
    {
        GDI_LOG("[VulkanDynamicGDI] Device init failed.")
        return;
    }

    CommandManager = new VKCommandManager();
    if(!CommandManager->Initialize(DeviceObj))
    {
        GDI_LOG("[VulkanDynamicGDI] CommandManager init failed.")
        return;
    }

    // Bind GSync to the device so the render thread can wait on submit completions.
    if(auto* Sync = dynamic_cast<VKSync*>(GSync))
    {
        Sync->Bind(DeviceObj);
    }

    // RenderFinished timeline semaphore: monotonic value signaled by EndRendering,
    // awaited by the UI-thread present. Robust to render/present running on
    // independent triggers (no binary signal/wait desync).
    VkSemaphoreTypeCreateInfo TimelineType{};
    TimelineType.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    TimelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    TimelineType.initialValue  = 0;

    VkSemaphoreCreateInfo SemInfo{};
    SemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    SemInfo.pNext = &TimelineType;
    vkCreateSemaphore(DeviceObj->GetDevice(), &SemInfo, nullptr, &RenderTimeline);

    CreateTimestampPool();

    // Swapchains are created on the UI thread, one per window, by the first
    // present to it (FindOrCreateSwapchain).

    PipelineCache = new VKPipelineCache();
    if(!PipelineCache->Initialize(DeviceObj))
    {
        GDI_LOG("[VulkanDynamicGDI] PipelineCache init failed.")
        return;
    }

    DescAlloc = new VKDescriptorAllocator();
    if(!DescAlloc->Initialize(DeviceObj))
    {
        GDI_LOG("[VulkanDynamicGDI] DescriptorAllocator init failed.")
        return;
    }

    // Create 1×1 transparent fallback texture for unbound sampler descriptor slots.
    {
        // Zero-initialised by construction, i.e. transparent black --
        // the same thing QImage::fill(Qt::transparent) produced.
        Image NullImg(1, 1, EImageFormat::RGBA8);
        NullTexture = new VKTextureResource(DeviceObj, NullImg, GDI_TB_Repeat, GDI_TF_Nearest);
    }

    CurrentStateData = new VKCurrentStateData();

    bInitialized = true;
    GDI_LOG("[VulkanDynamicGDI] Allocate complete.")
}

void VulkanDynamicGDI::Destroy()
{
    /* BEFORE the early-out, and before DeviceObj is deleted below: cached
     * shaders and textures hold a raw Device pointer and must be released
     * while it is still alive -- see ~GDIDynamicDriver. Idempotent, so the
     * !bInitialized path below still runs it exactly once. */
    ReleaseCachedResources();

    if(!bInitialized) return;

    if(DeviceObj && DeviceObj->IsValid())
    {
        DeviceObj->WaitIdle();
    }
    DrainDeferredDestruction();

    if(GSync)
    {
        GSync->DestroySync();
    }

    delete CurrentStateData; CurrentStateData = nullptr;

    if(ScreenMesh)   { ScreenMesh->Release();   delete ScreenMesh;   ScreenMesh   = nullptr; }
    if(NullTexture)  { NullTexture->Release();  delete NullTexture;  NullTexture  = nullptr; }

    for(auto& Entry : Swapchains) { Entry.second->Shutdown(); delete Entry.second; }
    Swapchains.clear();
    if(CommandManager)  { CommandManager->Shutdown();   delete CommandManager;  CommandManager = nullptr; }
    if(DescAlloc)       { DescAlloc->Shutdown();        delete DescAlloc;       DescAlloc = nullptr; }
    if(PipelineCache)   { PipelineCache->Shutdown();    delete PipelineCache;   PipelineCache = nullptr; }

    if(DeviceObj && DeviceObj->IsValid() && RenderTimeline != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(DeviceObj->GetDevice(), RenderTimeline, nullptr);
        RenderTimeline = VK_NULL_HANDLE;
    }
    if(DeviceObj && DeviceObj->IsValid() && TimestampPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(DeviceObj->GetDevice(), TimestampPool, nullptr);
        TimestampPool = VK_NULL_HANDLE;
    }

    if(DeviceObj)
    {
        DeviceObj->Shutdown();
        delete DeviceObj;
        DeviceObj = nullptr;
    }

    bInitialized = false;
}

void VulkanDynamicGDI::SwapBuffers()
{
    // No-op on render thread. Present is performed by UI thread via PresentToWindow.
}

void VulkanDynamicGDI::Flush()
{
    // No-op: EndRendering already submits and signals the fence/semaphore.
}

void VulkanDynamicGDI::PresentToWindow(WindowHandle InWindow)
{
    // Find the main RT — the renderer's RenderTargetManager owns it; we don't have a direct handle here,
    // so the swapchain takes the most recently bound RT. In Phase 1 the renderer always rebinds Main right
    // before EndRendering, so CurrentStateData->ActiveRenderTarget is the main RT.
    PresentToSwapchain(InWindow, CurrentStateData ? CurrentStateData->ActiveRenderTarget : nullptr);
}

void VulkanDynamicGDI::PresentRenderTargetToWindow(WindowHandle InWindow, GDIRenderTarget* InSource)
{
    PresentToSwapchain(InWindow, static_cast<VKRenderTarget*>(InSource));
}

void VulkanDynamicGDI::PresentToSwapchain(WindowHandle InWindow, VKRenderTarget* InRT)
{
    if(!bInitialized || !InWindow.IsValid()) return;

    // Skip while render targets are being resized — they're mid-recreation on the
    // render thread (inconsistent layout/contents). EndRendering clears this once a
    // full frame has been rendered into the new targets.
    if(bResizing.load()) return;

    // No frame was rendered yet — don't present.
    if(!InRT) return;

    VKSwapchain* Swapchain = FindOrCreateSwapchain(InWindow);
    if(!Swapchain) return;

    // Wait for the most recent render-thread submit (idempotent: if no new render
    // happened — e.g. an expose-triggered present — the value is already reached).
    // PresentFrame loads the published value itself, under the queue mutex, so the
    // wait value and the submit are atomic w.r.t. the render thread's publish+submit.
    Swapchain->PresentFrame(InRT, RenderTimeline, &RenderTimelinePublished);
}

VKSwapchain* VulkanDynamicGDI::FindOrCreateSwapchain(WindowHandle InWindow)
{
    for(const auto& Entry : Swapchains)
    {
        if(Entry.first == InWindow.Native) return Entry.second;
    }

    auto* VKCtx = static_cast<VulkanGDIContext*>(Context);
    if(!VKCtx) return nullptr;

    auto* Swapchain = new VKSwapchain();
    if(!Swapchain->Initialize(VKCtx->VKContext, DeviceObj, InWindow))
    {
        GDI_LOG("[VulkanDynamicGDI] Swapchain initialize failed.")
        delete Swapchain;
        return nullptr;
    }
    Swapchains.emplace_back(InWindow.Native, Swapchain);
    return Swapchain;
}

void VulkanDynamicGDI::ReleaseWindow(WindowHandle InWindow)
{
    for(auto It = Swapchains.begin(); It != Swapchains.end(); ++It)
    {
        if(It->first == InWindow.Native)
        {
            It->second->Shutdown();   // waits for the device to go idle first
            delete It->second;
            Swapchains.erase(It);
            return;
        }
    }
}

GDIRenderTarget* VulkanDynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    if(!DeviceObj) return nullptr;
    auto* RT = new VKRenderTarget(DeviceObj, Name, InWidth, InHeight);
    RegisterRenderTarget(RT);
    return RT;
}

GDIRenderTarget* VulkanDynamicGDI::CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat)
{
    if(!DeviceObj) return nullptr;
    const VkFormat Format = InFormat == GDI_RTF_RGBA16F ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    auto* RT = new VKRenderTarget(DeviceObj, Name, InWidth, InHeight, Format);
    RegisterRenderTarget(RT);
    return RT;
}

bool VulkanDynamicGDI::SupportsRenderTargetFormat(const EGDIRenderTargetFormat InFormat) const
{
    /* Half float colour attachments with blending are required of every Vulkan device. */
    return InFormat == GDI_RTF_RGBA8 || InFormat == GDI_RTF_RGBA16F;
}

void VulkanDynamicGDI::SetRenderTarget(GDIRenderTarget* InRenderTarget)
{
    SetRenderTargets(&InRenderTarget, 1, InRenderTarget);
}

void VulkanDynamicGDI::SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom)
{
    if(!CurrentStateData || !InColors || InCount == 0) return;
    if(CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;
    if(InCount > GDI_MAX_COLOR_TARGETS) InCount = GDI_MAX_COLOR_TARGETS;

    VKRenderTarget* Colors[GDI_MAX_COLOR_TARGETS] = {};
    for(uint Index = 0; Index < InCount; ++Index)
    {
        Colors[Index] = dynamic_cast<VKRenderTarget*>(InColors[Index]);
        if(!Colors[Index] || !Colors[Index]->IsValid()) return;
        if(Colors[Index]->GetWidth() != Colors[0]->GetWidth() || Colors[Index]->GetHeight() != Colors[0]->GetHeight())
        {
            GDI_LOG("[VulkanDynamicGDI] SetRenderTargets: targets of different sizes; nothing bound")
            return;
        }
    }
    VKRenderTarget* DepthRT = dynamic_cast<VKRenderTarget*>(InDepthFrom);
    if(DepthRT && (!DepthRT->HasDepth() || DepthRT->GetWidth() != Colors[0]->GetWidth()
                   || DepthRT->GetHeight() != Colors[0]->GetHeight()))
    {
        DepthRT = nullptr;
    }
    const auto IsBoundColor = [&](const VKRenderTarget* InRT)
    {
        for(uint Index = 0; Index < InCount; ++Index) { if(Colors[Index] == InRT) { return true; } }
        return false;
    };

    EndActiveRenderingRegion();

    // Transition EVERY render target still in COLOR_ATTACHMENT layout (except those
    // being bound) to SHADER_READ_ONLY_OPTIMAL so it can be sampled by the upcoming
    // pass. The sampler descriptors written by BindDrawState always declare
    // SHADER_READ_ONLY, and layers/stroke buffers are sampled in arbitrary order —
    // transitioning only the immediately-previous RT is not enough. Depth likewise:
    // every depth image but the one being attached becomes sampleable.
    std::vector<VkImageMemoryBarrier2> Barriers;
    for(VKRenderTarget* RT : LiveRenderTargets)
    {
        if(!RT->IsValid()) continue;

        if(!IsBoundColor(RT) && RT->GetCurrentLayout() == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            VkImageMemoryBarrier2 ToRead{};
            ToRead.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ToRead.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            ToRead.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ToRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            ToRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ToRead.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ToRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ToRead.image         = RT->GetImage();
            ToRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ToRead.subresourceRange.levelCount = 1;
            ToRead.subresourceRange.layerCount = 1;
            Barriers.push_back(ToRead);
            RT->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        if(RT != DepthRT && RT->HasDepth() && RT->GetDepthLayout() == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
        {
            VkImageMemoryBarrier2 ToRead{};
            ToRead.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ToRead.srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            ToRead.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ToRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            ToRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ToRead.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            ToRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ToRead.image         = RT->GetDepthImage();
            ToRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            ToRead.subresourceRange.levelCount = 1;
            ToRead.subresourceRange.layerCount = 1;
            Barriers.push_back(ToRead);
            RT->SetDepthLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    // Transition the new RTs to COLOR_ATTACHMENT_OPTIMAL. srcStage/srcAccess must cover
    // prior reads AND writes: the UI-thread present blit samples this image
    // (fragment-shader read), so this write-after-read barrier is what orders a
    // subsequently-submitted frame against an earlier-submitted present.
    for(uint Index = 0; Index < InCount; ++Index)
    {
        VKRenderTarget* RT = Colors[Index];
        VkImageMemoryBarrier2 Barrier{};
        Barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        Barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        Barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        Barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        Barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        Barrier.oldLayout     = RT->GetCurrentLayout();
        Barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.image         = RT->GetImage();
        Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Barrier.subresourceRange.levelCount = 1;
        Barrier.subresourceRange.layerCount = 1;
        Barriers.push_back(Barrier);
        RT->SetCurrentLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    // Depth: from UNDEFINED (and cleared) on first use, else after the previous
    // pass's writes or a pass that sampled it.
    const bool bDepthIsFresh = DepthRT && DepthRT->GetDepthLayout() == VK_IMAGE_LAYOUT_UNDEFINED;
    if(DepthRT)
    {
        VkImageMemoryBarrier2 DepthBarrier{};
        DepthBarrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        DepthBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                                   | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        DepthBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        DepthBarrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        DepthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        DepthBarrier.oldLayout     = DepthRT->GetDepthLayout();
        DepthBarrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        DepthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        DepthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        DepthBarrier.image         = DepthRT->GetDepthImage();
        DepthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        DepthBarrier.subresourceRange.levelCount = 1;
        DepthBarrier.subresourceRange.layerCount = 1;
        Barriers.push_back(DepthBarrier);
        DepthRT->SetDepthLayout(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    }

    VkDependencyInfo Dep{};
    Dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    Dep.imageMemoryBarrierCount = uint32_t(Barriers.size());
    Dep.pImageMemoryBarriers    = Barriers.data();
    vkCmdPipelineBarrier2(CurrentStateData->CurrentCmd, &Dep);

    // Begin dynamic rendering region.
    VkRenderingAttachmentInfo ColorAttachments[GDI_MAX_COLOR_TARGETS]{};
    for(uint Index = 0; Index < InCount; ++Index)
    {
        VkRenderingAttachmentInfo& ColorAttachment = ColorAttachments[Index];
        ColorAttachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ColorAttachment.imageView   = Colors[Index]->GetImageView();
        ColorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ColorAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        ColorAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingInfo RenderingInfo{};
    RenderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    RenderingInfo.renderArea.offset    = { 0, 0 };
    RenderingInfo.renderArea.extent    = { Colors[0]->GetWidth(), Colors[0]->GetHeight() };
    RenderingInfo.layerCount           = 1;
    RenderingInfo.colorAttachmentCount = InCount;
    RenderingInfo.pColorAttachments    = ColorAttachments;

    VkRenderingAttachmentInfo DepthAttachment{};
    if(DepthRT)
    {
        DepthAttachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        DepthAttachment.imageView   = DepthRT->GetDepthView();
        DepthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        DepthAttachment.loadOp      = bDepthIsFresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        DepthAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        DepthAttachment.clearValue.depthStencil.depth = 1.0f;
        RenderingInfo.pDepthAttachment = &DepthAttachment;
    }
    vkCmdBeginRendering(CurrentStateData->CurrentCmd, &RenderingInfo);

    CurrentStateData->ActiveRenderTarget = Colors[0];
    for(uint Index = 0; Index < GDI_MAX_COLOR_TARGETS; ++Index)
    {
        CurrentStateData->ActiveColors[Index] = Index < InCount ? Colors[Index] : nullptr;
    }
    CurrentStateData->ActiveColorCount = InCount;
    CurrentStateData->ActiveDepth      = DepthRT;
    CurrentStateData->bRenderingActive = true;
}

void VulkanDynamicGDI::ReleaseRenderTarget(GDIRenderTarget* InRenderTarget)
{
    if(InRenderTarget) InRenderTarget->Release();
}

void VulkanDynamicGDI::DeferDestroyImage(VkImage InImage, VkDeviceMemory InMemory,
                                          VkImageView InView, VkSampler InSampler)
{
    if(InImage == VK_NULL_HANDLE && InMemory == VK_NULL_HANDLE &&
       InView == VK_NULL_HANDLE && InSampler == VK_NULL_HANDLE) return;
    DeferredImages.push_back({ InImage, InMemory, InView, InSampler });
}

void VulkanDynamicGDI::DeferDestroyBuffer(VkBuffer InBuffer, VkDeviceMemory InMemory)
{
    if(InBuffer == VK_NULL_HANDLE && InMemory == VK_NULL_HANDLE) return;
    DeferredBuffers.push_back({ InBuffer, InMemory });
}

void VulkanDynamicGDI::DrainDeferredDestruction()
{
    if(!DeviceObj || !DeviceObj->IsValid()) { DeferredImages.clear(); DeferredBuffers.clear(); return; }
    VkDevice Dev = DeviceObj->GetDevice();

    for(const DeferredImage& D : DeferredImages)
    {
        if(D.Sampler != VK_NULL_HANDLE) vkDestroySampler(Dev, D.Sampler, nullptr);
        if(D.View    != VK_NULL_HANDLE) vkDestroyImageView(Dev, D.View, nullptr);
        if(D.Image   != VK_NULL_HANDLE) vkDestroyImage(Dev, D.Image, nullptr);
        if(D.Memory  != VK_NULL_HANDLE) vkFreeMemory(Dev, D.Memory, nullptr);
    }
    DeferredImages.clear();

    for(const DeferredBuffer& D : DeferredBuffers)
    {
        if(D.Buffer != VK_NULL_HANDLE) vkDestroyBuffer(Dev, D.Buffer, nullptr);
        if(D.Memory != VK_NULL_HANDLE) vkFreeMemory(Dev, D.Memory, nullptr);
    }
    DeferredBuffers.clear();
}

void VulkanDynamicGDI::EndActiveRenderingRegion()
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive) return;
    if(CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;
    vkCmdEndRendering(CurrentStateData->CurrentCmd);
    CurrentStateData->bRenderingActive = false;
}

void VulkanDynamicGDI::FlushForReadback(bool bResumeActiveRegion)
{
    if(!CommandManager || !CurrentStateData || !DeviceObj) return;
    if(CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;   // no frame recording

    VKRenderTarget*  RT          = CurrentStateData->ActiveRenderTarget;
    const bool       bWasActive  = CurrentStateData->bRenderingActive;
    GDIRenderTarget* Colors[GDI_MAX_COLOR_TARGETS] = {};
    const uint32_t   ColorCount  = CurrentStateData->ActiveColorCount;
    for(uint32_t Index = 0; Index < ColorCount; ++Index) { Colors[Index] = CurrentStateData->ActiveColors[Index]; }
    GDIRenderTarget* Depth       = CurrentStateData->ActiveDepth;

    EndActiveRenderingRegion();
    CommandManager->EndFrame();

    // Submit the in-progress command buffer signaling the command manager's FrameFence.
    // The BeginFrame() below waits that fence before reusing the buffer, so by the time
    // this returns the flushed GPU work is complete (required for the readback / resize).
    VkCommandBuffer Cmd = CurrentStateData->CurrentCmd;
    VkSubmitInfo Submit{};
    Submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    Submit.commandBufferCount = 1;
    Submit.pCommandBuffers    = &Cmd;
    DeviceObj->QueueSubmit(1, &Submit, CommandManager->GetFrameFence());

    // Resume recording into a fresh command buffer (BeginFrame waits FrameFence —
    // the flushed submit is complete, so deferred releases are safe to destroy too).
    VkCommandBuffer NewCmd = CommandManager->BeginFrame();
    DrainDeferredDestruction();
    CurrentStateData->CurrentCmd         = NewCmd;
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->ActiveColorCount   = 0;
    CurrentStateData->ActiveDepth        = nullptr;
    CurrentStateData->bRenderingActive   = false;
    CurrentStateData->DebugLabelDepth    = 0;

    // Restore dynamic state (a fresh command buffer has none).
    vkCmdSetViewport(NewCmd, 0, 1, &CurrentStateData->ViewportRect);
    vkCmdSetScissor (NewCmd, 0, 1, &CurrentStateData->ScissorRect);

    // Re-open the rendering region on the RT that was active, so the frame
    // continues exactly as before the flush (loadOp=LOAD preserves contents).
    // Skipped for resize, where RT is about to be destroyed.
    if(bResumeActiveRegion && bWasActive && RT && ColorCount > 0)
        SetRenderTargets(Colors, ColorCount, Depth);
}

void VulkanDynamicGDI::RecordResolve(VKRenderTarget* Src, VKRenderTarget* Dst)
{
    if(!Src || !Dst || !CurrentStateData || !DeviceObj) return;
    // Self-resolve is a no-op (the OpenGL path self-blits an FBO onto itself; in
    // Vulkan a same-image vkCmdCopyImage over the full extent is invalid and the
    // double barrier would corrupt layout tracking).
    if(Src == Dst) return;
    if(CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;
    if(!Src->IsValid() || !Dst->IsValid()) return;

    VkCommandBuffer Cmd = CurrentStateData->CurrentCmd;

    // A copy cannot happen inside a dynamic-rendering region.
    EndActiveRenderingRegion();
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->ActiveColorCount   = 0;
    CurrentStateData->ActiveDepth        = nullptr;

    // Transition Src → TRANSFER_SRC, Dst → TRANSFER_DST from their tracked layouts.
    RecordBarrier(Cmd, MakeImageBarrier(
        Src->GetImage(), Src->GetCurrentLayout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT));
    RecordBarrier(Cmd, MakeImageBarrier(
        Dst->GetImage(), Dst->GetCurrentLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_WRITE_BIT));

    VkImageCopy Copy{};
    Copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Copy.srcSubresource.layerCount = 1;
    Copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Copy.dstSubresource.layerCount = 1;
    Copy.extent = { Src->GetWidth(), Src->GetHeight(), 1 };
    vkCmdCopyImage(Cmd,
                   Src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   Dst->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &Copy);

    // Leave both in SHADER_READ_ONLY_OPTIMAL — the canonical idle layout that the
    // next SetRenderTarget / sampler bind expects.
    RecordBarrier(Cmd, MakeImageBarrier(
        Src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
    RecordBarrier(Cmd, MakeImageBarrier(
        Dst->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));

    Src->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Dst->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

#pragma endregion

#pragma region Shader resource

GDIShaderResource* VulkanDynamicGDI::CreateShaderResource(EShaderType InType, const std::string& InSrcPath)
{
    if(!DeviceObj) return nullptr;
    const std::string SpvPath = ProcessShaderFromSource(InSrcPath, ERenderPlatform::ERP_Vulkan);
    return new VKShaderResource(DeviceObj, InType, SpvPath);
}

void VulkanDynamicGDI::SetBoundShader(GDIShader* In_VS, GDIShader* In_PS)
{
    BoundVertexShader = In_VS;
    BoundPixelShader  = In_PS;
    if(!CurrentStateData) return;

    auto ExtractModule = [](GDIShader* Shader) -> VkShaderModule
    {
        if(!Shader || !Shader->Resource) return VK_NULL_HANDLE;
        return static_cast<VKShaderResource*>(Shader->Resource)->GetShaderModule();
    };
    CurrentStateData->BoundVSModule = ExtractModule(In_VS);
    CurrentStateData->BoundFSModule = ExtractModule(In_PS);
}

#pragma endregion

#pragma region Mesh creation

GDIMesh* VulkanDynamicGDI::CreateQuadMesh(const float* VertData, const float* TexData,
                                             const int InNumVerts, const int InNumTexCoords,
                                             bool bIsInstanced)
{
    if(!DeviceObj || !CurrentStateData) return nullptr;

    GDIMesh::ORenderData RenderData;

    const size_t VertArraySize = sizeof(float) * static_cast<size_t>(InNumVerts);
    RenderData.Position = static_cast<float*>(malloc(VertArraySize));
    memcpy(RenderData.Position, VertData, VertArraySize);

    const size_t TexArraySize = sizeof(float) * static_cast<size_t>(InNumTexCoords);
    RenderData.TexCoord = static_cast<float*>(malloc(TexArraySize));
    memcpy(RenderData.TexCoord, TexData, TexArraySize);

    RenderData.NumVertices = static_cast<uint>(InNumVerts);

    auto* Mesh = new VKMesh(DeviceObj, CurrentStateData);
    Mesh->SetRenderData(RenderData);
    Mesh->SetSupportsInstanced(bIsInstanced);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* VulkanDynamicGDI::CreateMesh(const GDIMeshDesc& InDesc)
{
    if(!DeviceObj || !CurrentStateData) return nullptr;

    GDIMesh::ORenderData RenderData;
    if(!GDIMakeIndexedRenderData(InDesc, RenderData)) return nullptr;

    auto* Mesh = new VKMesh(DeviceObj, CurrentStateData);
    Mesh->SetRenderData(RenderData);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* VulkanDynamicGDI::CreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    if(!DeviceObj || !CurrentStateData) return nullptr;

    auto* Mesh = new VKMesh(DeviceObj, CurrentStateData);
    if(!Mesh->InitDynamic(InMaxVertices, InMaxIndices))
    {
        delete Mesh;
        return nullptr;
    }
    return Mesh;
}

GDIMesh* VulkanDynamicGDI::CreateQuadMesh(const float& InWidth, const float& InHeight,
                                             bool bIsInstanced)
{
    const float HalfWidth  = InWidth  * 0.5f;
    const float HalfHeight = InHeight * 0.5f;

    const float VertQuadData[] = {
        -HalfWidth, -HalfHeight, 0.f,
        -HalfWidth,  HalfHeight, 0.f,
         HalfWidth,  HalfHeight, 0.f,
         HalfWidth,  HalfHeight, 0.f,
         HalfWidth, -HalfHeight, 0.f,
        -HalfWidth, -HalfHeight, 0.f
    };
    const float VertTexData[] = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
        1.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f
    };

    return CreateQuadMesh(VertQuadData, VertTexData, 18, 12, bIsInstanced);
}

#pragma endregion

#pragma region Texture creation

GDITextureResource* VulkanDynamicGDI::CreateTextureResource(
        const Image& InImage,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new VKTextureResource(DeviceObj, InImage, InBorder, InFilter);
}

GDITextureResource* VulkanDynamicGDI::CreateTextureResource(
        GDITexture2D* InParent,
        const Image& InImage,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new VKTextureResource(DeviceObj, InParent, InImage, InBorder, InFilter);
}

GDITextureResource* VulkanDynamicGDI::CreateTextureResourceMips(
        const Image* InLevels,
        const uint32_t InCount,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj || !InLevels || InCount == 0) return nullptr;
    return new VKTextureResource(DeviceObj, InLevels, InCount, InBorder, InFilter);
}

GDITextureResource* VulkanDynamicGDI::CreateTextureResource(
        GDITexture2D* InParent,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new VKTextureResource(DeviceObj, InParent, InBorder, InFilter);
}

#pragma endregion

#pragma region Core draw path

// ---------------------------------------------------------------------------
// UBO packing helpers
// ---------------------------------------------------------------------------

/** Pack a scalar value from Scalars into a raw byte buffer at Offset (returns new offset). */
static uint32_t PackScalar(const VKUniformValue& V, uint8_t* Dst, uint32_t Offset, uint32_t MaxBytes)
{
    uint32_t Size = 0;
    const void* Ptr = nullptr;

    // Temporary float for INT conversion.
    float IntAsFloat = 0.f;

    switch(V.Tag)
    {
        case VKUniformValue::INT:
            IntAsFloat = static_cast<float>(V.Data.i);
            Size = 4;  Ptr = &IntAsFloat;         break;
        case VKUniformValue::FLOAT:
            Size = 4;  Ptr = &V.Data.f;           break;
        case VKUniformValue::VEC2:
            Size = 8;  Ptr = V.Data.v;            break;
        case VKUniformValue::VEC3:
            Size = 12; Ptr = V.Data.v;            break;
        case VKUniformValue::VEC4:
            Size = 16; Ptr = V.Data.v;            break;
        case VKUniformValue::MAT:
            Size = 64; Ptr = V.Data.v;            break;
        default:
            return Offset;
    }
    if(Offset + Size > MaxBytes) return Offset;
    memcpy(Dst + Offset, Ptr, Size);
    return Offset + Size;
}

/**
 * Pack ViewParameters struct into a 160-byte std140 UBO buffer:
 *   Offset   0: mat4  Projection       (64 bytes)
 *   Offset  64: mat4  View             (64 bytes)
 *   Offset 128: vec3  MouseCoords      (12 bytes + 4 pad = 16 bytes)
 *   Offset 144: vec4  Resolution       (16 bytes)
 */
static void PackViewParamsUBO(const std::unordered_map<std::string, VKUniformValue>& S, void* Dst)
{
    float* D = static_cast<float*>(Dst);
    memset(D, 0, 160);

    auto Write = [&](const std::string& Name, float* Out, int Count)
    {
        auto It = S.find(Name);
        if(It != S.end())
            memcpy(Out, It->second.Data.v, static_cast<size_t>(Count) * sizeof(float));
    };

    Write("ViewParameters.Projection",  D + 0,  16);  // offset   0, 64 bytes
    Write("ViewParameters.View",        D + 16, 16);  // offset  64, 64 bytes
    Write("ViewParameters.MouseCoords", D + 32,  3);  // offset 128, 12 bytes (+ 4 pad)
    Write("ViewParameters.Resolution",  D + 36,  4);  // offset 144, 16 bytes

    // GL→Vulkan clip-space correction on the projection: the camera builds a GL
    // projection (NDC z in [-1,1]) but Vulkan clips z outside [0,1], which would
    // discard geometry entirely. Remap z' = 0.5*z + 0.5*w, i.e. pre-multiply by
    //   [1 0  0   0 ]
    //   [0 1  0   0 ]
    //   [0 0 0.5 0.5]
    //   [0 0  0   1 ]
    // The floats here are column-major (D[Col*4 + Row]); only row 2 changes.
    // Y is intentionally NOT flipped — the render targets keep GL-identical row
    // order and the swapchain blit flips V at present time instead.
    for(int Col = 0; Col < 4; ++Col)
    {
        const float Z = D[Col * 4 + 2];
        const float W = D[Col * 4 + 3];
        D[Col * 4 + 2] = 0.5f * Z + 0.5f * W;
    }
}

/**
 * Pack Transform struct into an 80-byte std140 UBO buffer:
 *   Offset  0: vec3  PositionWorldSpace (12 bytes + 4 pad = 16 bytes)
 *   Offset 16: mat4  Model              (64 bytes)
 */
static void PackTransformUBO(const std::unordered_map<std::string, VKUniformValue>& S, void* Dst)
{
    float* D = static_cast<float*>(Dst);
    memset(D, 0, 80);

    auto Write = [&](const std::string& Name, float* Out, int Count)
    {
        auto It = S.find(Name);
        if(It != S.end())
            memcpy(Out, It->second.Data.v, static_cast<size_t>(Count) * sizeof(float));
    };

    Write("Transform.PositionWorldSpace", D + 0, 3);  // offset  0, 12 bytes (+ 4 pad)
    Write("Transform.Model",              D + 4, 16); // offset 16, 64 bytes
}

/**
 * Pack all non-core (non-ViewParameters, non-Transform) scalars from S into Dst.
 * Returns the number of bytes written (used as the UBO range).
 * QHash iteration is unordered — for single-scalar custom UBOs this is deterministic.
 */
static uint32_t PackCustomUBO(const std::unordered_map<std::string, VKUniformValue>& S, void* Dst, uint32_t MaxBytes)
{
    uint8_t* D = static_cast<uint8_t*>(Dst);
    memset(D, 0, MaxBytes);
    uint32_t Offset = 0;
    for(auto It = S.begin(); It != S.end(); ++It)
    {
        const std::string& N = It->first;
        if(StartsWith(N, "ViewParameters.") || StartsWith(N, "Transform.")) continue;
        Offset = PackScalar(It->second, D, Offset, MaxBytes);
    }
    return (Offset > 0) ? Offset : 1u; // minimum 1 byte range for vkUpdateDescriptorSets
}

// ---------------------------------------------------------------------------
// ExecuteDrawCalls — core Vulkan draw implementation
// ---------------------------------------------------------------------------

bool VulkanDynamicGDI::BindDrawState(GDIMesh* InMesh, bool bIsInstanced)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive) return false;
    if(CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return false;
    if(!InMesh || !PipelineCache || !DescAlloc || !DeviceObj) return false;
    if(CurrentStateData->BoundVSModule == VK_NULL_HANDLE ||
       CurrentStateData->BoundFSModule == VK_NULL_HANDLE) return false;

    // ----------------------------------------------------------------
    // 1. Gather shader resources
    // ----------------------------------------------------------------
    VKShaderResource* PSRes = nullptr;
    VKShaderResource* VSRes = nullptr;
    if(BoundPixelShader && BoundPixelShader->Resource)
        PSRes = static_cast<VKShaderResource*>(BoundPixelShader->Resource);
    if(BoundVertexShader && BoundVertexShader->Resource)
        VSRes = static_cast<VKShaderResource*>(BoundVertexShader->Resource);

    // For core UBO packing we prefer PS (both VS and PS have identical core data).
    const std::unordered_map<std::string, VKUniformValue>& CoreScalars =
        PSRes ? PSRes->GetScalars() : (VSRes ? VSRes->GetScalars() : std::unordered_map<std::string,VKUniformValue>());

    // ----------------------------------------------------------------
    // 2. Determine descriptor layout group from PS uniforms
    // ----------------------------------------------------------------
    bool bHasCustomUBO     = false;
    bool bHasCustomSampler = false;
    if(PSRes)
    {
        for(auto It = PSRes->GetScalars().begin();
            It != PSRes->GetScalars().end(); ++It)
        {
            const std::string& N = It->first;
            if(!StartsWith(N, "ViewParameters.") && !StartsWith(N, "Transform."))
            {
                bHasCustomUBO = true;
                break;
            }
        }
        bHasCustomSampler = !PSRes->GetTextures().empty();
    }

    EDescriptorLayoutGroup Group = DLG_Base;
    if     ( bHasCustomUBO && !bHasCustomSampler) Group = DLG_CustomUBO;
    else if(!bHasCustomUBO &&  bHasCustomSampler) Group = DLG_Sampler;
    else if( bHasCustomUBO &&  bHasCustomSampler) Group = DLG_UBOAndSampler;
    if(IsMaterialDraw()) Group = DLG_Material;

    // ----------------------------------------------------------------
    // 3. Build pipeline key
    // ----------------------------------------------------------------
    VKPipelineCache::PipelineKey Key{};
    Key.VertexShader    = CurrentStateData->BoundVSModule;
    Key.FragmentShader  = CurrentStateData->BoundFSModule;
    Key.VertexLayoutID  = InMesh->IsIndexed() ? 2u : (bIsInstanced ? 1u : 0u);
    Key.DescLayoutGroup = Group;

    if(CurrentStateData->BlendState)
    {
        const VkPipelineColorBlendAttachmentState& BA =
            CurrentStateData->BlendState->ColorAttachment;
        VKPipelineCache::ApplyBlendState(Key,
            BA.blendEnable, BA.srcColorBlendFactor,
            BA.dstColorBlendFactor,
            BA.srcAlphaBlendFactor, BA.dstAlphaBlendFactor,
            BA.colorWriteMask,
            BA.colorBlendOp, BA.alphaBlendOp);
    }
    if(CurrentStateData->DepthStencilState)
    {
        const VkPipelineDepthStencilStateCreateInfo& DS =
            CurrentStateData->DepthStencilState->StateInfo;
        VKPipelineCache::ApplyDepthStencilState(Key,
            DS.depthTestEnable, DS.depthWriteEnable, DS.stencilTestEnable,
            DS.depthCompareOp);
    }
    if(CurrentStateData->ActiveRenderTarget)
    {
        Key.RTFormat    = CurrentStateData->ActiveRenderTarget->GetFormat();
        Key.ColorCount  = CurrentStateData->ActiveColorCount > 0 ? CurrentStateData->ActiveColorCount : 1u;
        Key.DepthFormat = CurrentStateData->ActiveDepth ? CurrentStateData->ActiveDepth->GetDepthFormat() : VK_FORMAT_UNDEFINED;
    }

    // ----------------------------------------------------------------
    // 4. Fetch pipeline + layouts
    // ----------------------------------------------------------------
    VKPipelineCache::PipelineFetch F = PipelineCache->GetOrCreateFull(Key);
    if(F.Pipeline == VK_NULL_HANDLE || F.PipeLayout == VK_NULL_HANDLE) return false;

    // ----------------------------------------------------------------
    // 5. Allocate descriptor set
    // ----------------------------------------------------------------
    VkDescriptorSet DescSet = DescAlloc->Allocate(F.DescLayout);
    if(DescSet == VK_NULL_HANDLE) return false;

    // ----------------------------------------------------------------
    // 6. Allocate and pack UBOs from ring buffer
    // ----------------------------------------------------------------

    // --- Binding 0: ViewParameters (160 bytes) ---
    VKUBOAllocation VPAlloc = DescAlloc->AllocateUBO(160);
    if(VPAlloc.Buffer == VK_NULL_HANDLE) return false;
    PackViewParamsUBO(CoreScalars, VPAlloc.MappedPtr);

    VkDescriptorBufferInfo VPBuf{};
    VPBuf.buffer = VPAlloc.Buffer;
    VPBuf.offset = VPAlloc.Offset;
    VPBuf.range  = 160;

    // --- Binding 1: Transform (80 bytes) ---
    VKUBOAllocation TrAlloc = DescAlloc->AllocateUBO(80);
    if(TrAlloc.Buffer == VK_NULL_HANDLE) return false;
    PackTransformUBO(CoreScalars, TrAlloc.MappedPtr);

    VkDescriptorBufferInfo TrBuf{};
    TrBuf.buffer = TrAlloc.Buffer;
    TrBuf.offset = TrAlloc.Offset;
    TrBuf.range  = 80;

    // ----------------------------------------------------------------
    // 7. Collect descriptor writes (up to 4 bindings; the material layout's 12)
    // ----------------------------------------------------------------
    VkWriteDescriptorSet Writes[GDI_MATERIAL_TEXTURE_BINDING + GDI_MATERIAL_TEXTURE_SLOTS]{};
    uint32_t WriteCount = 0;

    // VP UBO (binding 0)
    Writes[WriteCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    Writes[WriteCount].dstSet          = DescSet;
    Writes[WriteCount].dstBinding      = 0;
    Writes[WriteCount].descriptorCount = 1;
    Writes[WriteCount].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    Writes[WriteCount].pBufferInfo     = &VPBuf;
    ++WriteCount;

    // Transform UBO (binding 1)
    Writes[WriteCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    Writes[WriteCount].dstSet          = DescSet;
    Writes[WriteCount].dstBinding      = 1;
    Writes[WriteCount].descriptorCount = 1;
    Writes[WriteCount].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    Writes[WriteCount].pBufferInfo     = &TrBuf;
    ++WriteCount;

    // --- Custom UBO at binding 2 (DLG_CustomUBO / DLG_UBOAndSampler) ---
    static const uint32_t kCustomUBOMaxBytes = 256;
    VKUBOAllocation CustAlloc;
    VkDescriptorBufferInfo CustBuf{};
    if(Group == DLG_CustomUBO || Group == DLG_UBOAndSampler)
    {
        CustAlloc = DescAlloc->AllocateUBO(kCustomUBOMaxBytes);
        if(CustAlloc.Buffer == VK_NULL_HANDLE) return false;

        const std::unordered_map<std::string, VKUniformValue>& PSScalars =
            PSRes ? PSRes->GetScalars() : std::unordered_map<std::string,VKUniformValue>();
        const uint32_t CustomRange = PackCustomUBO(PSScalars, CustAlloc.MappedPtr, kCustomUBOMaxBytes);

        CustBuf.buffer = CustAlloc.Buffer;
        CustBuf.offset = CustAlloc.Offset;
        CustBuf.range  = CustomRange;

        Writes[WriteCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        Writes[WriteCount].dstSet          = DescSet;
        Writes[WriteCount].dstBinding      = 2;
        Writes[WriteCount].descriptorCount = 1;
        Writes[WriteCount].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        Writes[WriteCount].pBufferInfo     = &CustBuf;
        ++WriteCount;
    }

    // --- Material layout: raw blocks at 2 and 3, textures at 4-11 ---
    VkDescriptorBufferInfo BlockBufs[2]{};
    VkDescriptorImageInfo  MaterialImages[GDI_MATERIAL_TEXTURE_SLOTS]{};
    if(Group == DLG_Material)
    {
        for(uint32_t b = 0; b < 2; ++b)
        {
            const std::vector<uint8_t>& Bytes = MaterialBlocks[b];
            // Never an empty range: a shader that declares the block still reads it.
            const uint32_t Size = Bytes.empty() ? 16u : uint32_t(Bytes.size());
            const bool bReuse = b == 1 && SceneBlockUploadSerial == MaterialBlockSerials[1]
                                && SceneBlockUploadEpoch == DescAlloc->GetEpoch()
                                && SceneBlockUploadBuffer != VK_NULL_HANDLE;
            VKUBOAllocation Block;
            if(bReuse)
            {
                Block.Buffer = SceneBlockUploadBuffer;
                Block.Offset = SceneBlockUploadOffset;
            }
            else
            {
                Block = DescAlloc->AllocateUBO(Size);
            }
            if(Block.Buffer == VK_NULL_HANDLE) return false;
            if(!bReuse)
            {
                memset(Block.MappedPtr, 0, Size);
                if(!Bytes.empty()) memcpy(Block.MappedPtr, Bytes.data(), Bytes.size());
                if(b == 1)
                {
                    SceneBlockUploadBuffer = Block.Buffer;
                    SceneBlockUploadOffset = Block.Offset;
                    SceneBlockUploadSerial = MaterialBlockSerials[1];
                    SceneBlockUploadEpoch  = DescAlloc->GetEpoch();
                }
            }

            BlockBufs[b].buffer = Block.Buffer;
            BlockBufs[b].offset = Block.Offset;
            BlockBufs[b].range  = Size;

            Writes[WriteCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            Writes[WriteCount].dstSet          = DescSet;
            Writes[WriteCount].dstBinding      = GDI_MATERIAL_BLOCK_BINDING + b;
            Writes[WriteCount].descriptorCount = 1;
            Writes[WriteCount].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            Writes[WriteCount].pBufferInfo     = &BlockBufs[b];
            ++WriteCount;
        }

        for(uint32_t s = 0; s < GDI_MATERIAL_TEXTURE_SLOTS; ++s)
        {
            auto* VKTex = dynamic_cast<VKTextureResource*>(MaterialTextures[s]);
            if(!VKTex) VKTex = dynamic_cast<VKTextureResource*>(NullTexture);
            if(!VKTex) return false;

            MaterialImages[s].sampler     = VKTex->GetSampler();
            MaterialImages[s].imageView   = VKTex->GetImageView();
            MaterialImages[s].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            Writes[WriteCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            Writes[WriteCount].dstSet          = DescSet;
            Writes[WriteCount].dstBinding      = GDI_MATERIAL_TEXTURE_BINDING + s;
            Writes[WriteCount].descriptorCount = 1;
            Writes[WriteCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            Writes[WriteCount].pImageInfo      = &MaterialImages[s];
            ++WriteCount;
        }
    }

    // --- Sampler bindings (DLG_Sampler uses binding 2; DLG_UBOAndSampler uses binding 3) ---
    VkDescriptorImageInfo SamplerImageInfo{};
    if(Group == DLG_Sampler || Group == DLG_UBOAndSampler)
    {
        const uint32_t SamplerBinding = (Group == DLG_UBOAndSampler) ? 3u : 2u;

        // Find texture at high-level index 0 from PS textures.
        GDITextureResource* TexRes = NullTexture;
        if(PSRes)
        {
            for(auto It = PSRes->GetTextures().begin();
                It != PSRes->GetTextures().end(); ++It)
            {
                if(It->second.Index == 0)
                {
                    TexRes = It->second.Resource ? It->second.Resource : NullTexture;
                    break;
                }
            }
        }

        auto* VKTex = dynamic_cast<VKTextureResource*>(TexRes);
        if(!VKTex) VKTex = dynamic_cast<VKTextureResource*>(NullTexture);
        if(!VKTex) return false; // no fallback available

#ifdef QT_DEBUG
        // A draw that expected a real texture but got the 1×1 transparent fallback
        // renders nothing (brush/blit shaders discard on alpha 0) — log it.
        if(VKTex == NullTexture)
        {
            static int NullTexWarnCount = 0;
            if(NullTexWarnCount < 20)
            {
                ++NullTexWarnCount;
                GDI_LOG("[VulkanDynamicGDI] Draw uses NullTexture fallback (FS=0x"
                      + GDIHex(reinterpret_cast<uintptr_t>(CurrentStateData->BoundFSModule))
                      + ", PS textures="
                      + ::GDILogDetail::GDINum(PSRes ? int(PSRes->GetTextures().size()) : -1) + ")")
            }
        }
#endif

        SamplerImageInfo.sampler     = VKTex->GetSampler();
        SamplerImageInfo.imageView   = VKTex->GetImageView();
        SamplerImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        Writes[WriteCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        Writes[WriteCount].dstSet          = DescSet;
        Writes[WriteCount].dstBinding      = SamplerBinding;
        Writes[WriteCount].descriptorCount = 1;
        Writes[WriteCount].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        Writes[WriteCount].pImageInfo      = &SamplerImageInfo;
        ++WriteCount;
    }

    vkUpdateDescriptorSets(DeviceObj->GetDevice(), WriteCount, Writes, 0, nullptr);

    // ----------------------------------------------------------------
    // 8. Bind pipeline + descriptor set. The actual vkCmdDraw is issued by
    //    GDIMesh::Draw() (VKMesh::Draw), which calls this first — so a direct
    //    Mesh->Draw() (e.g. layer compositing) binds a pipeline too, not just the
    //    DrawObject path.
    // ----------------------------------------------------------------
    vkCmdBindPipeline(CurrentStateData->CurrentCmd,
                      VK_PIPELINE_BIND_POINT_GRAPHICS, F.Pipeline);

    vkCmdBindDescriptorSets(CurrentStateData->CurrentCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            F.PipeLayout, 0, 1, &DescSet, 0, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// DrawObject / DrawScreenQuadPrimitive
// ---------------------------------------------------------------------------

// NOTE: the single-argument DrawObject(GDIViewObject*) is NOT overridden here —
// the GDIDynamicDriver base version routes to the two-argument overload below with
// the object's own view. (An earlier override called Mesh->Draw() directly, which
// skipped UpdateCoreUniforms/UpdateUserDefinedUniforms — so shaders drawn through
// that path, e.g. the brush cursor's Stencil texture, never reached the descriptor
// set and the wrong pipeline layout group was selected.)

void VulkanDynamicGDI::DrawObject(GDIViewObject* InObject, GDIView* InView)
{
    if(!InObject || !InObject->Mesh) return;

    if(BoundVertexShader)
        BoundVertexShader->UpdateCoreUniforms(InView, InObject->Transform);
    if(BoundPixelShader)
        BoundPixelShader->UpdateCoreUniforms(InView, InObject->Transform);

    InObject->Mesh->Draw();
}

void VulkanDynamicGDI::DrawScreenQuadPrimitive()
{
    if(!DeviceObj || !CurrentStateData) return;

    // Flush the bound pixel shader's user uniforms into its resource, exactly like
    // the OpenGL DrawScreenQuadPrimitive. Without this the screen-blit texture set
    // via Screen_PS::SetScreenBlitTexture never reaches the shader resource, so
    // BindDrawState picks a samplerless pipeline layout and DrawTexture (the
    // flood-fill / restore write-back path) draws nothing.
    if(BoundPixelShader)
        BoundPixelShader->UpdateUserDefinedUniforms();

    // Lazily create a full-screen NDC quad on first use.
    if(!ScreenMesh)
    {
        // 6 verts covering the full NDC square [-1,1]×[-1,1].
        const float Verts[] = {
            -1.f, -1.f, 0.f,
            -1.f,  1.f, 0.f,
             1.f,  1.f, 0.f,
             1.f,  1.f, 0.f,
             1.f, -1.f, 0.f,
            -1.f, -1.f, 0.f
        };
        const float UVs[] = {
            0.f, 0.f,
            0.f, 1.f,
            1.f, 1.f,
            1.f, 1.f,
            1.f, 0.f,
            0.f, 0.f
        };
        ScreenMesh = CreateQuadMesh(Verts, UVs, 18, 12, false);
    }

    if(!ScreenMesh) return;
    ScreenMesh->Draw();
}

#pragma endregion

#pragma region Misc stubs

bool VulkanDynamicGDI::CheckError(const std::string&) { return false; }

Color VulkanDynamicGDI::GetColorFromSamplePoint(const Point& InPoint, const Size& InSize)
{
    if(!DeviceObj || !CurrentStateData) return Color();

    // Make sure the just-composited frame is on the GPU before we sample it.
    FlushForReadback();

    VKRenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT || !RT->IsValid()) return Color();

    // RT memory is GL-oriented (row 0 = visual bottom — same matrices as the GL
    // backend), so flip Y from Qt screen space exactly like the OpenGL path's
    // glReadPixels(x, height - y).
    const int X = InPoint.X;
    const int Y = InSize.Height - InPoint.Y;
    if(X < 0 || Y < 0 || X >= int(RT->GetWidth()) || Y >= int(RT->GetHeight()))
        return Color();

    VkDevice Dev = DeviceObj->GetDevice();

    VkBuffer       Buf = VK_NULL_HANDLE;
    VkDeviceMemory Mem = VK_NULL_HANDLE;
    if(!AllocStagingBuffer(DeviceObj, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buf, Mem))
        return Color();

    VkCommandPool   Pool = VK_NULL_HANDLE;
    VkCommandBuffer Cmd  = VK_NULL_HANDLE;
    if(!BeginTransientCmd(DeviceObj, Pool, Cmd))
    {
        vkDestroyBuffer(Dev, Buf, nullptr);
        vkFreeMemory(Dev, Mem, nullptr);
        return Color();
    }

    const VkImageLayout Cur = RT->GetCurrentLayout();
    RecordBarrier(Cmd, MakeImageBarrier(
        RT->GetImage(), Cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT));

    VkBufferImageCopy Region{};
    Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Region.imageSubresource.layerCount = 1;
    Region.imageOffset = { X, Y, 0 };
    Region.imageExtent = { 1, 1, 1 };
    vkCmdCopyImageToBuffer(Cmd, RT->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Buf, 1, &Region);

    // Restore the RT to the layout the resumed rendering region expects.
    RecordBarrier(Cmd, MakeImageBarrier(
        RT->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Cur,
        VK_PIPELINE_STAGE_2_COPY_BIT,         VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));

    EndAndSubmitTransientCmd(DeviceObj, Pool, Cmd);

    void* Mapped = nullptr;
    vkMapMemory(Dev, Mem, 0, 4, 0, &Mapped);
    const uchar* Px = static_cast<const uchar*>(Mapped);
    Color Out(Px[0], Px[1], Px[2]);   // VK_FORMAT_R8G8B8A8_UNORM byte order
    vkUnmapMemory(Dev, Mem);

    vkDestroyBuffer(Dev, Buf, nullptr);
    vkFreeMemory(Dev, Mem, nullptr);
    return Out;
}

#pragma endregion

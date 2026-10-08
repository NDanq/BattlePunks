#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include <mutex>
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VKCommandManager.h"
#include "Public/GDI/Vulkan/VKRenderTarget.h"
#include "Public/GDI/Vulkan/VKSync.h"
#include "Public/GDI/Vulkan/VKDescriptorAllocator.h"

#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
#include <vector>

extern GDISync* GSync;

#pragma region VKUtil decoders

VkBlendOp VKUtil::DecodeBlendOp(EBlendOp InOp)
{
    switch (InOp)
    {
        case BO_Max: return VK_BLEND_OP_MAX;
        case BO_Min: return VK_BLEND_OP_MIN;
        case BO_Add:
        default:     return VK_BLEND_OP_ADD;
    }
}

VkBlendFactor VKUtil::DecodeBlendFactor(EBlendMode InMode)
{
    switch (InMode)
    {
        case BM_Zero:                       return VK_BLEND_FACTOR_ZERO;
        case BM_One:                        return VK_BLEND_FACTOR_ONE;
        case BM_Src_Color:                  return VK_BLEND_FACTOR_SRC_COLOR;
        case BM_One_Minus_Src_Color:        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BM_Dest_Color:                 return VK_BLEND_FACTOR_DST_COLOR;
        case BM_One_Minus_Dest_Color:       return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BM_Src_Alpha:                  return VK_BLEND_FACTOR_SRC_ALPHA;
        case BM_One_Minus_Src_Alpha:        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BM_Dest_Alpha:                 return VK_BLEND_FACTOR_DST_ALPHA;
        case BM_One_Minus_Dest_Alpha:       return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BM_Constant_Color:             return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BM_One_Minus_Constant_Color:   return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BM_Constant_Alpha:             return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BM_One_Minus_Constant_Alpha:   return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ZERO;
}

VkFlags VKUtil::DecodeColorMask(EColorMask InColorMask)
{
    switch(InColorMask)
    {
        case EColM_R:    return VK_COLOR_COMPONENT_R_BIT;
        case EColM_G:    return VK_COLOR_COMPONENT_G_BIT;
        case EColM_B:    return VK_COLOR_COMPONENT_B_BIT;
        case EColM_A:    return VK_COLOR_COMPONENT_A_BIT;
        case EColM_RGB:  return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        case EColM_RGBA: return VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        default:         return 0;
    }
}

VkCompareOp VKUtil::DecodeCompareOp(ECompareTestMode InMode)
{
    switch(InMode)
    {
        case CTM_Never:    return VK_COMPARE_OP_NEVER;
        case CTM_ALWAYS:   return VK_COMPARE_OP_ALWAYS;
        case CTM_LESS:     return VK_COMPARE_OP_LESS;
        case CTM_LEQUAL:   return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CTM_GREATER:  return VK_COMPARE_OP_GREATER;
        case CTM_GEQUAL:   return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CTM_EQUAL:    return VK_COMPARE_OP_EQUAL;
        case CTM_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        default:           return VK_COMPARE_OP_LESS;
    }
}

#pragma endregion

#pragma region Frame begin/end and clear

void VulkanDynamicGDI::BeginRendering()
{
    if(!CommandManager || !CurrentStateData) return;

    // Reentrancy guard: a nested BeginRendering (a render command or slot calling
    // GDIBeginRendering while a frame is already recording) must be a no-op. The
    // naive path would deadlock — BeginFrame resets the frame fence then waits on
    // it, but only a submit (which never comes) can signal it. Keep recording into
    // the already-open frame instead; the matching EndRendering submits it.
    if(CurrentStateData->CurrentCmd != VK_NULL_HANDLE)
    {
#ifdef QT_DEBUG
        static int NestedWarnCount = 0;
        if(NestedWarnCount < 10)
        {
            ++NestedWarnCount;
            GDI_LOG("[VulkanDynamicGDI] Nested BeginRendering ignored (frame already recording).")
        }
#endif
        return;
    }

    // BeginFrame waits the command buffer's completion fence, so the previous
    // submission is guaranteed done here — safe to reclaim last frame's descriptor
    // sets and to destroy resources released while that frame was in flight.
    VkCommandBuffer Cmd = CommandManager->BeginFrame();
    ForgetBoundDrawState();   // a command buffer begun: nothing bound in it yet
    ResetRecordingContexts();   // last frame's secondaries and rings, now done on the GPU
    if(DescAlloc) DescAlloc->Reset();
    DrainDeferredDestruction();
    CompletedTimelineValue = RenderTimelineCounter;   // what BeginReadback's readbacks wait on

    // The previous block's timestamps are complete (its fence was just waited):
    // read them, then reset the pool for this block's, outside any rendering.
    CollectTimestamps();
    if(TimestampPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(Cmd, TimestampPool, 0, GDI_GPU_TIMESTAMP_MAX);
    }

    CurrentStateData->CurrentCmd          = Cmd;
    CurrentStateData->ActiveRenderTarget  = nullptr;
    CurrentStateData->ActiveColorCount    = 0;
    CurrentStateData->ActiveDepth         = nullptr;
    CurrentStateData->bRenderingActive    = false;
    CurrentStateData->DebugLabelDepth     = 0;
}

void VulkanDynamicGDI::EndRendering()
{
    if(!CommandManager || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;

    EndActiveRenderingRegion();

    // Transition the active RT to SHADER_READ_ONLY_OPTIMAL so the UI-thread present
    // pass can sample it with the screen-quad blit pipeline (VKSwapchain::PresentFrame).
    if(VKRenderTarget* MainRT = CurrentStateData->ActiveRenderTarget)
    {
        VkImageMemoryBarrier2 Barrier{};
        Barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        Barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        Barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        Barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        Barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        Barrier.oldLayout     = MainRT->GetCurrentLayout();
        Barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.image         = MainRT->GetImage();
        Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Barrier.subresourceRange.levelCount = 1;
        Barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo Dep{};
        Dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        Dep.imageMemoryBarrierCount = 1;
        Dep.pImageMemoryBarriers    = &Barrier;
        vkCmdPipelineBarrier2(CurrentStateData->CurrentCmd, &Dep);
        MainRT->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    CommandManager->EndFrame();

    // Submit, signaling the RenderTimeline (for the UI-thread present) and the command
    // manager's FrameFence (so the next BeginFrame can safely reuse the buffer).
    const VkFence SignalFence = CommandManager->GetFrameFence();

    VkCommandBuffer Cmd          = CurrentStateData->CurrentCmd;
    const uint64_t  SignalValue  = ++RenderTimelineCounter;

    VkTimelineSemaphoreSubmitInfo TimelineSubmit{};
    TimelineSubmit.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    TimelineSubmit.signalSemaphoreValueCount = 1;
    TimelineSubmit.pSignalSemaphoreValues    = &SignalValue;

    VkSubmitInfo Submit{};
    Submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    Submit.pNext                = (RenderTimeline != VK_NULL_HANDLE) ? &TimelineSubmit : nullptr;
    Submit.commandBufferCount   = 1;
    Submit.pCommandBuffers      = &Cmd;
    Submit.signalSemaphoreCount = (RenderTimeline != VK_NULL_HANDLE) ? 1u : 0u;
    Submit.pSignalSemaphores    = &RenderTimeline;

    // Publish + submit atomically w.r.t. the UI-thread present (which reads the
    // published value and submits under the same mutex). This guarantees that a
    // present submitted after this frame waits for >= this frame's timeline value,
    // and a present submitted before it is ordered by the frame's ALL_COMMANDS
    // layout barrier — either way the present never samples a frame mid-render.
    // (Timeline semaphores permit a wait to be submitted before its signal.)
    {
        std::lock_guard<std::mutex> QueueLock(DeviceObj->GetQueueMutex());
        if(RenderTimeline != VK_NULL_HANDLE)
            RenderTimelinePublished.store(SignalValue);
        DeviceObj->QueueSubmitLocked(1, &Submit, SignalFence);
    }

    CurrentStateData->CurrentCmd          = VK_NULL_HANDLE;
    CurrentStateData->bRenderingActive    = false;

    // A full frame has now been rendered (into freshly-resized targets, if a resize
    // happened this frame) and submitted — the next present is safe to run.
    bResizing.store(false);
}

void VulkanDynamicGDI::Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth, uint InStencilMask)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive) return;

    VKRenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    const bool bClearColor = (InClearMode & CM_Color) != 0;
    const bool bClearDepth = (InClearMode & CM_Depth) != 0 && CurrentStateData->ActiveDepth != nullptr;
    if(!bClearColor && !bClearDepth) return;

    // Every bound colour target, then the depth.
    VkClearAttachment ClearAtts[GDI_MAX_COLOR_TARGETS + 1]{};
    uint32_t          NumClears = 0;
    if(bClearColor)
    {
        const uint32_t ColorCount = CurrentStateData->ActiveColorCount > 0 ? CurrentStateData->ActiveColorCount : 1u;
        for(uint32_t Index = 0; Index < ColorCount; ++Index)
        {
            VkClearAttachment& ClearAtt = ClearAtts[NumClears++];
            ClearAtt.aspectMask                 = VK_IMAGE_ASPECT_COLOR_BIT;
            ClearAtt.colorAttachment            = Index;
            ClearAtt.clearValue.color.float32[0] = float(InClearColor.Rf());
            ClearAtt.clearValue.color.float32[1] = float(InClearColor.Gf());
            ClearAtt.clearValue.color.float32[2] = float(InClearColor.Bf());
            ClearAtt.clearValue.color.float32[3] = float(InClearColor.Af());
        }
    }
    if(bClearDepth)
    {
        VkClearAttachment& ClearAtt = ClearAtts[NumClears++];
        ClearAtt.aspectMask                    = VK_IMAGE_ASPECT_DEPTH_BIT;
        ClearAtt.clearValue.depthStencil.depth = InDepth;
    }

    VkClearRect ClearRect{};
    if(RT)
    {
        ClearRect.rect.extent = { RT->GetWidth(), RT->GetHeight() };
    }
    ClearRect.layerCount = 1;

    CloseDrawPredicate();   // conditional rendering would skip the clear
    vkCmdClearAttachments(CurrentStateData->CurrentCmd, NumClears, ClearAtts, 1, &ClearRect);
}

void VulkanDynamicGDI::SetViewport(uint InX, uint InY, uint InWidth, uint InHeight)
{
    if(!CurrentStateData) return;
    /* Negative height (core since Vulkan 1.1): NDC +1 lands on the rect's
     * first row, so every target is stored top-down -- row 0 the top of the
     * image, as GDI's other backends store it and as RenderDoc shows it. */
    VkViewport& V = CurrentStateData->ViewportRect;
    V.x        = float(InX);
    V.y        = float(InY + InHeight);
    V.width    = float(InWidth);
    V.height   = -float(InHeight);
    V.minDepth = 0.0f;
    V.maxDepth = 1.0f;

    if(CurrentStateData->CurrentCmd != VK_NULL_HANDLE)
    {
        vkCmdSetViewport(CurrentStateData->CurrentCmd, 0, 1, &V);
    }

    VkRect2D& S = CurrentStateData->ScissorRect;
    S.offset = { int32_t(InX), int32_t(InY) };
    S.extent = { InWidth, InHeight };
    if(CurrentStateData->CurrentCmd != VK_NULL_HANDLE)
    {
        vkCmdSetScissor(CurrentStateData->CurrentCmd, 0, 1, &S);
    }
}

void VulkanDynamicGDI::SetScissorRect(uint InX, uint InY, uint InWidth, uint InHeight)
{
    if(!CurrentStateData) return;

    /* The target's own rows from its top, as the viewport. */
    VkRect2D& S = CurrentStateData->ScissorRect;
    S.offset = { int32_t(InX), int32_t(InY) };
    S.extent = { InWidth, InHeight };
    if(CurrentStateData->CurrentCmd != VK_NULL_HANDLE)
    {
        vkCmdSetScissor(CurrentStateData->CurrentCmd, 0, 1, &S);
    }
}

#pragma endregion

#pragma region State-tracking stubs (used at pipeline build time in Phase 2)

void VulkanDynamicGDI::SetDepthState(IDepthStateDesc InDesc)
{
    if(CurrentStateData && CurrentStateData->DepthStencilState)
        CurrentStateData->DepthStencilState->SetDepthState(InDesc);
}

void VulkanDynamicGDI::SetBlendState(IBlendStateDesc InDesc)
{
    if(CurrentStateData && CurrentStateData->BlendState)
        CurrentStateData->BlendState->SetBlendState(InDesc);
}

void VulkanDynamicGDI::SetStencilState(IStencilStateDesc InDesc)
{
    if(CurrentStateData && CurrentStateData->DepthStencilState)
        CurrentStateData->DepthStencilState->SetStencilState(InDesc);
}

void VulkanDynamicGDI::SetColorMask(EColorMask InMask)
{
    if(CurrentStateData && CurrentStateData->BlendState)
        CurrentStateData->BlendState->SetColorMask(InMask);
}

void VulkanDynamicGDI::BeginDrawEvent(const std::string& EventName)
{
    // Labels need VK_EXT_debug_utils on the instance (VulkanGDIContext::HasDebugUtils).
    if(!VulkanGDIContext::HasDebugUtils()) return;
    if(!DeviceObj || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;

    static auto BeginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(DeviceObj->GetInstance(), "vkCmdBeginDebugUtilsLabelEXT"));
    if(!BeginLabel) return;

    // EventName is already a std::string; c_str() is the pointer Vulkan wants.
    VkDebugUtilsLabelEXT Label{};
    Label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    Label.pLabelName = EventName.c_str();
    Label.color[0] = Label.color[1] = Label.color[2] = Label.color[3] = 1.0f;
    BeginLabel(CurrentStateData->CurrentCmd, &Label);
    ++CurrentStateData->DebugLabelDepth;
}

void VulkanDynamicGDI::EndDrawEvent()
{
    if(!VulkanGDIContext::HasDebugUtils()) return;
    if(!DeviceObj || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;

    // Only close a label that was opened in the current command buffer. A mid-frame
    // FlushForReadback resets the depth to 0, so labels whose Begin landed in an
    // already-submitted buffer are not (invalidly) closed here.
    if(CurrentStateData->DebugLabelDepth <= 0) return;

    static auto EndLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(DeviceObj->GetInstance(), "vkCmdEndDebugUtilsLabelEXT"));
    if(!EndLabel) return;

    EndLabel(CurrentStateData->CurrentCmd);
    --CurrentStateData->DebugLabelDepth;
}

#pragma endregion

#pragma region GPU timestamps

void VulkanDynamicGDI::CreateTimestampPool()
{
    if(!DeviceObj || !DeviceObj->IsValid())
    {
        return;
    }
    VkPhysicalDeviceProperties Properties{};
    vkGetPhysicalDeviceProperties(DeviceObj->GetPhysicalDevice(), &Properties);
    uint32_t FamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(DeviceObj->GetPhysicalDevice(), &FamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> Families(FamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(DeviceObj->GetPhysicalDevice(), &FamilyCount, Families.data());
    const uint32_t Family    = DeviceObj->GetGraphicsQueueFamily();
    const uint32_t ValidBits = Family < FamilyCount ? Families[Family].timestampValidBits : 0;
    if(ValidBits == 0 || Properties.limits.timestampPeriod <= 0.0f)
    {
        GDI_LOG("[VulkanDynamicGDI] The graphics queue cannot write timestamps; GPU timing is off.")
        return;
    }

    VkQueryPoolCreateInfo Info{};
    Info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    Info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    Info.queryCount = GDI_GPU_TIMESTAMP_MAX;
    if(vkCreateQueryPool(DeviceObj->GetDevice(), &Info, nullptr, &TimestampPool) != VK_SUCCESS)
    {
        TimestampPool = VK_NULL_HANDLE;
        return;
    }
    TimestampPeriodNs  = double(Properties.limits.timestampPeriod);
    TimestampValidMask = ValidBits >= 64 ? ~uint64_t(0) : (uint64_t(1) << ValidBits) - 1;
}

void VulkanDynamicGDI::WriteGPUTimestamp(const uint32_t InIndex)
{
    // Once per index per block: a written query must be reset before it is written again.
    const uint64_t Bit = uint64_t(1) << InIndex;
    if(TimestampPool == VK_NULL_HANDLE || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE
       || InIndex >= GDI_GPU_TIMESTAMP_MAX || (TimestampsWritten & Bit))
    {
        return;
    }
    vkCmdWriteTimestamp2(CurrentStateData->CurrentCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TimestampPool, InIndex);
    TimestampsWritten |= Bit;
}

void VulkanDynamicGDI::CollectTimestamps()
{
    PreviousTimestamps = 0;
    if(TimestampPool == VK_NULL_HANDLE || TimestampsWritten == 0)
    {
        return;
    }
    for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
    {
        if(!(TimestampsWritten & (uint64_t(1) << Index)))
        {
            continue;
        }
        // No WAIT bit: the block's fence has been waited, and a query that is
        // somehow unavailable must not stall the frame -- it is just skipped.
        uint64_t Ticks = 0;
        if(vkGetQueryPoolResults(DeviceObj->GetDevice(), TimestampPool, Index, 1, sizeof(Ticks), &Ticks, sizeof(Ticks),
                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            PreviousTimestampMs[Index] = double(Ticks & TimestampValidMask) * TimestampPeriodNs / 1.0e6;
            PreviousTimestamps |= uint64_t(1) << Index;
        }
    }
    TimestampsWritten = 0;
}

uint64_t VulkanDynamicGDI::GetPreviousGPUTimestamps(double* OutMilliseconds)
{
    for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
    {
        if(PreviousTimestamps & (uint64_t(1) << Index))
        {
            OutMilliseconds[Index] = PreviousTimestampMs[Index];
        }
    }
    return PreviousTimestamps;
}

#pragma endregion

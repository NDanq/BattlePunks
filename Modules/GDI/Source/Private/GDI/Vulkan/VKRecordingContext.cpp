#include "Public/GDI/Vulkan/VKRecordingContext.h"
#include "Public/GDI/Vulkan/VKDescriptorAllocator.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VKMesh.h"
#include "Public/GDI/Vulkan/VKRenderTarget.h"
#include "Public/GDI/Vulkan/VKShader.h"
#include "Private/GDI/Vulkan/VKCorePack.h"
#include "Public/GDI/GDILog.h"

/*
 * Parallel passes on Vulkan (GDIBeginParallelPass; RenderCore's
 * MULTITHREADED_RENDERING.md, phase 2).
 *
 * The primary's rendering is reopened on the same attachments with
 * VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT; each context records a
 * secondary that continues it (VkCommandBufferInheritanceRenderingInfo: the
 * attachments' formats), setting its own viewport and scissor, pipelines and
 * descriptor sets -- a secondary inherits none of them. EndParallelPass
 * executes the secondaries in chunk order, then reopens the rendering for
 * inline commands, as it was. Conditional rendering is never inherited: a
 * predicate brackets the draws of one chunk, closed before the chunk ends.
 */

namespace
{
    VKRecordingContext* AsVK(GDIRecordingContext* InContext)
    {
        return static_cast<VKRecordingContext*>(InContext);
    }

    VkShaderModule ModuleOf(GDIShader* InShader)
    {
        return InShader && InShader->Resource ? static_cast<VKShaderResource*>(InShader->Resource)->GetShaderModule()
                                              : VK_NULL_HANDLE;
    }
}

void VulkanDynamicGDI::ReopenRendering(const VkRenderingFlags InFlags)
{
    if(!CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE) return;
    const VKRenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT || CurrentStateData->ActiveColorCount == 0) return;

    VkRenderingAttachmentInfo Colors[GDI_MAX_COLOR_TARGETS]{};
    for(uint32_t Index = 0; Index < CurrentStateData->ActiveColorCount; ++Index)
    {
        Colors[Index].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        Colors[Index].imageView   = CurrentStateData->ActiveColors[Index]->GetImageView();
        Colors[Index].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        Colors[Index].loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        Colors[Index].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo Info{};
    Info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    Info.flags                = InFlags;
    Info.renderArea.extent    = { RT->GetWidth(), RT->GetHeight() };
    Info.layerCount           = 1;
    Info.colorAttachmentCount = CurrentStateData->ActiveColorCount;
    Info.pColorAttachments    = Colors;
    VkRenderingAttachmentInfo Depth{};
    if(const VKRenderTarget* DepthRT = CurrentStateData->ActiveDepth)
    {
        Depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        Depth.imageView   = DepthRT->GetDepthView();
        Depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        Depth.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        Depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        Info.pDepthAttachment = &Depth;
    }
    vkCmdBeginRendering(CurrentStateData->CurrentCmd, &Info);
    CurrentStateData->bRenderingActive = true;
}

bool VulkanDynamicGDI::BeginParallelPass(const uint32_t InChunks, GDIRecordingContext** OutContexts)
{
    if(bParallelPass || !DeviceObj || !DeviceObj->IsValid() || !CurrentStateData || !CurrentStateData->bRenderingActive
       || CurrentStateData->CurrentCmd == VK_NULL_HANDLE || !CurrentStateData->ActiveRenderTarget
       || InChunks == 0 || InChunks > GDI_PARALLEL_CHUNKS_MAX)
    {
        return false;
    }
    const VkDevice Dev  = DeviceObj->GetDevice();
    const uint32_t Slot = FrameSlot;

    /* The contexts, made on first need and kept; each one's resources for
     * this frame slot likewise. */
    while(RecordingContexts.size() < InChunks)
    {
        RecordingContexts.push_back(new VKRecordingContext());
    }
    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        VKRecordingContext::FrameResources& Frame = RecordingContexts[Chunk]->Frames[Slot];
        if(Frame.DescAlloc) continue;
        VkCommandPoolCreateInfo PoolInfo{};
        PoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        PoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        PoolInfo.queueFamilyIndex = DeviceObj->GetGraphicsQueueFamily();
        auto* Alloc = new VKDescriptorAllocator();
        if(vkCreateCommandPool(Dev, &PoolInfo, nullptr, &Frame.Pool) != VK_SUCCESS || !Alloc->Initialize(DeviceObj))
        {
            GDI_LOG("[VulkanDynamicGDI] a recording context could not be made; recording serially")
            if(Frame.Pool != VK_NULL_HANDLE) { vkDestroyCommandPool(Dev, Frame.Pool, nullptr); }
            Frame.Pool = VK_NULL_HANDLE;
            Alloc->Shutdown();
            delete Alloc;
            return false;
        }
        Frame.DescAlloc = Alloc;
    }

    /* A secondary for each chunk, allocated before anything is recorded: a
     * failure here leaves the frame as it was. */
    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        VKRecordingContext::FrameResources& Frame = RecordingContexts[Chunk]->Frames[Slot];
        if(Frame.NextBuffer < Frame.Buffers.size()) continue;
        VkCommandBufferAllocateInfo Alloc{};
        Alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        Alloc.commandPool        = Frame.Pool;
        Alloc.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        Alloc.commandBufferCount = 1;
        VkCommandBuffer Buffer = VK_NULL_HANDLE;
        if(vkAllocateCommandBuffers(Dev, &Alloc, &Buffer) != VK_SUCCESS)
        {
            GDI_LOG("[VulkanDynamicGDI] vkAllocateCommandBuffers failed for a recording context; recording serially")
            return false;
        }
        Frame.Buffers.push_back(Buffer);
    }

    /* The secondaries continue the primary's rendering: it is reopened for
     * their contents, the attachments' formats inherited. */
    EndActiveRenderingRegion();
    ReopenRendering(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);

    VkFormat Formats[GDI_MAX_COLOR_TARGETS] = {};
    for(uint32_t Index = 0; Index < CurrentStateData->ActiveColorCount; ++Index)
    {
        Formats[Index] = CurrentStateData->ActiveColors[Index]->GetFormat();
    }
    VkCommandBufferInheritanceRenderingInfo Rendering{};
    Rendering.sType                   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
    Rendering.colorAttachmentCount    = CurrentStateData->ActiveColorCount;
    Rendering.pColorAttachmentFormats = Formats;
    Rendering.depthAttachmentFormat   = CurrentStateData->ActiveDepth ? CurrentStateData->ActiveDepth->GetDepthFormat()
                                                                      : VK_FORMAT_UNDEFINED;
    Rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    Rendering.rasterizationSamples    = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo Inherit{};
    Inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    Inherit.pNext = &Rendering;
    VkCommandBufferBeginInfo Begin{};
    Begin.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    Begin.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    Begin.pInheritanceInfo = &Inherit;

    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        VKRecordingContext& Context = *RecordingContexts[Chunk];
        VKRecordingContext::FrameResources& Frame = Context.Frames[Slot];
        Context.Cmd       = Frame.Buffers[Frame.NextBuffer++];
        Context.DescAlloc = Frame.DescAlloc;
        vkBeginCommandBuffer(Context.Cmd, &Begin);
        Context.Caches.ForgetBound();

        /* The driver's state as the pass begins: what a serial issue would start
         * from. The blocks' serials start a new generation of this context's own
         * (high bits), so its caches never take an upload made for other bytes
         * that happened to carry the same serial. */
        Context.BoundVertexShader = BoundVertexShader;
        Context.BoundPixelShader  = BoundPixelShader;
        ++Context.PassGeneration;
        for(uint32_t Block = 0; Block < 4; ++Block)
        {
            Context.MaterialBlocks[Block]       = MaterialBlocks[Block];
            Context.MaterialBlockSerials[Block] = Context.PassGeneration << 32;
        }
        for(uint32_t Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { Context.MaterialTextures[Slot] = MaterialTextures[Slot]; }
        Context.InstanceBuffer     = InstanceBuffer;
        Context.InstanceCount      = InstanceCount;
        Context.IndirectArgs       = IndirectArgs;
        Context.IndirectArgsOffset = IndirectArgsOffset;
        Context.PredicateSet       = ImmediatePredicateSet();
        Context.PredicateIndex     = PredicateIndex;
        Context.ViewportX          = ViewportX;
        Context.ViewportY          = ViewportY;
        Context.ViewportWidth      = ViewportWidth;
        Context.ViewportHeight     = ViewportHeight;
        Context.DrawFirst          = 0;
        Context.DrawCount          = 0;
        Context.Index              = Chunk;
        Context.DrawsRecorded      = 0;
        if(CurrentStateData->BlendState)        { Context.Blend = *CurrentStateData->BlendState; }
        if(CurrentStateData->DepthStencilState) { Context.Depth = *CurrentStateData->DepthStencilState; }
        Context.Viewport   = CurrentStateData->ViewportRect;
        Context.Scissor    = CurrentStateData->ScissorRect;
        Context.Core       = VKShaderResource::CoreBlocks();
        Context.LabelDepth = 0;
        vkCmdSetViewport(Context.Cmd, 0, 1, &Context.Viewport);
        vkCmdSetScissor(Context.Cmd, 0, 1, &Context.Scissor);
        Context.bRecording = true;
        OutContexts[Chunk] = &Context;
    }
    ParallelChunks = InChunks;
    bParallelPass  = true;
    return true;
}

void VulkanDynamicGDI::EndParallelPass()
{
    if(!bParallelPass) return;
    VkCommandBuffer Secondaries[GDI_PARALLEL_CHUNKS_MAX];
    for(uint32_t Chunk = 0; Chunk < ParallelChunks; ++Chunk)
    {
        VKRecordingContext& Context = *RecordingContexts[Chunk];
        CloseDrawPredicate(Context.Caches, Context.Cmd);
        while(Context.LabelDepth > 0) { RecordEndDrawEvent(&Context); }
        vkEndCommandBuffer(Context.Cmd);
        Context.bRecording  = false;
        Secondaries[Chunk] = Context.Cmd;
    }
    vkCmdExecuteCommands(CurrentStateData->CurrentCmd, ParallelChunks, Secondaries);

    /* Back to inline commands on the same attachments. What the primary had
     * bound -- pipeline, sets, dynamic state -- is undefined after executing
     * secondaries: forgotten, the viewport and scissor set again. */
    EndActiveRenderingRegion();
    ReopenRendering(0);
    vkCmdSetViewport(CurrentStateData->CurrentCmd, 0, 1, &CurrentStateData->ViewportRect);
    vkCmdSetScissor(CurrentStateData->CurrentCmd, 0, 1, &CurrentStateData->ScissorRect);
    ForgetBoundDrawState();
    ParallelChunks = 0;
    bParallelPass  = false;
}

void VulkanDynamicGDI::ResetRecordingContexts(const uint32_t InSlot)
{
    if(!DeviceObj || !DeviceObj->IsValid()) return;
    for(VKRecordingContext* Context : RecordingContexts)
    {
        VKRecordingContext::FrameResources& Frame = Context->Frames[InSlot];
        if(!Frame.DescAlloc) continue;
        vkResetCommandPool(DeviceObj->GetDevice(), Frame.Pool, 0);
        Frame.NextBuffer = 0;
        Frame.DescAlloc->Reset();
        Context->Caches.ForgetBound();
    }
}

void VulkanDynamicGDI::DestroyRecordingContexts()
{
    for(VKRecordingContext* Context : RecordingContexts)
    {
        for(VKRecordingContext::FrameResources& Frame : Context->Frames)
        {
            if(DeviceObj && DeviceObj->IsValid() && Frame.Pool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(DeviceObj->GetDevice(), Frame.Pool, nullptr);
            }
            if(Frame.DescAlloc) { Frame.DescAlloc->Shutdown(); delete Frame.DescAlloc; }
        }
        delete Context;
    }
    RecordingContexts.clear();
}

void VulkanDynamicGDI::RecordDepthState(GDIRecordingContext* InContext, const IDepthStateDesc InDesc)
{
    AsVK(InContext)->Depth.SetDepthState(InDesc);
}

void VulkanDynamicGDI::RecordBlendState(GDIRecordingContext* InContext, const IBlendStateDesc InDesc)
{
    AsVK(InContext)->Blend.SetBlendState(InDesc);
}

void VulkanDynamicGDI::RecordStencilState(GDIRecordingContext* InContext, const IStencilStateDesc InDesc)
{
    AsVK(InContext)->Depth.SetStencilState(InDesc);
}

void VulkanDynamicGDI::RecordColorMask(GDIRecordingContext* InContext, const EColorMask InMask)
{
    AsVK(InContext)->Blend.SetColorMask(InMask);
}

void VulkanDynamicGDI::RecordViewport(GDIRecordingContext* InContext, const uint InX, const uint InY, const uint InWidth,
                                      const uint InHeight)
{
    VKRecordingContext& Context = *AsVK(InContext);
    if(!Context.bRecording) return;
    /* As SetViewport: negative height, so targets are stored top-down. */
    Context.Viewport.x        = float(InX);
    Context.Viewport.y        = float(InY + InHeight);
    Context.Viewport.width    = float(InWidth);
    Context.Viewport.height   = -float(InHeight);
    Context.Viewport.minDepth = 0.0f;
    Context.Viewport.maxDepth = 1.0f;
    Context.Scissor.offset    = { int32_t(InX), int32_t(InY) };
    Context.Scissor.extent    = { InWidth, InHeight };
    vkCmdSetViewport(Context.Cmd, 0, 1, &Context.Viewport);
    vkCmdSetScissor(Context.Cmd, 0, 1, &Context.Scissor);
}

void VulkanDynamicGDI::RecordScissorRect(GDIRecordingContext* InContext, const uint InX, const uint InY, const uint InWidth,
                                         const uint InHeight)
{
    VKRecordingContext& Context = *AsVK(InContext);
    if(!Context.bRecording) return;
    Context.Scissor.offset = { int32_t(InX), int32_t(InY) };
    Context.Scissor.extent = { InWidth, InHeight };
    vkCmdSetScissor(Context.Cmd, 0, 1, &Context.Scissor);
}

void VulkanDynamicGDI::RecordDrawObject(GDIRecordingContext* InContext, GDIViewObject* InObject, GDIView* InView)
{
    VKRecordingContext& Context = *AsVK(InContext);
    if(!Context.bRecording || !InObject || !InObject->Mesh) return;

    /* The core blocks into the context, not onto the (shared) shaders. A
     * shader's user uniforms by name come from it as the render thread left
     * them: in a context they are read, never written. */
    if(InView)
    {
        VKCorePack::PackCoreView(InView, Context.Core.View);
        Context.Core.bView = true;
    }
    if(InObject->Transform)
    {
        VKCorePack::PackCoreTransform(InObject->Transform, Context.Core.Transform);
        Context.Core.bTransform = true;
    }

    VKDrawInputs In;
    In.Cmd                = Context.Cmd;
    In.bRenderingActive   = true;
    In.VS                 = Context.BoundVertexShader;
    In.PS                 = Context.BoundPixelShader;
    In.VSModule           = ModuleOf(Context.BoundVertexShader);
    In.FSModule           = ModuleOf(Context.BoundPixelShader);
    In.Blocks             = Context.MaterialBlocks;
    In.Serials            = Context.MaterialBlockSerials;
    In.Textures           = Context.MaterialTextures;
    In.InstanceBuffer     = Context.InstanceBuffer;
    In.InstanceCount      = Context.InstanceCount;
    In.IndirectArgs       = Context.IndirectArgs;
    In.IndirectArgsOffset = Context.IndirectArgsOffset;
    In.Blend              = &Context.Blend;
    In.Depth              = &Context.Depth;
    In.bHasTarget         = true;
    In.RTFormat           = CurrentStateData->ActiveRenderTarget->GetFormat();
    In.ColorCount         = CurrentStateData->ActiveColorCount > 0 ? CurrentStateData->ActiveColorCount : 1u;
    for(uint32_t Index = 0; Index < GDI_MAX_COLOR_TARGETS; ++Index)
    {
        const VKRenderTarget* Each = Index < CurrentStateData->ActiveColorCount ? CurrentStateData->ActiveColors[Index] : nullptr;
        In.ColorFormats[Index] = Each ? Each->GetFormat() : VK_FORMAT_UNDEFINED;
    }
    In.DepthFormat        = CurrentStateData->ActiveDepth ? CurrentStateData->ActiveDepth->GetDepthFormat() : VK_FORMAT_UNDEFINED;
    In.PredicateSet       = UsablePredicate(Context.PredicateSet, Context.PredicateIndex);
    In.PredicateIndex     = In.PredicateSet ? Context.PredicateIndex : -1;
    In.Core               = &Context.Core;
    In.DrawFirst          = Context.DrawFirst;
    In.DrawCount          = Context.DrawCount;
    In.Caches             = &Context.Caches;
    In.Alloc              = Context.DescAlloc;
    static_cast<VKMesh*>(InObject->Mesh)->Record(In);
}

void VulkanDynamicGDI::RecordBeginDrawEvent(GDIRecordingContext* InContext, const std::string& InName)
{
    VKRecordingContext& Context = *AsVK(InContext);
    if(!Context.bRecording || !VulkanGDIContext::HasDebugUtils() || !DeviceObj) return;
    static const auto BeginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(DeviceObj->GetInstance(), "vkCmdBeginDebugUtilsLabelEXT"));
    if(!BeginLabel) return;
    VkDebugUtilsLabelEXT Label{};
    Label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    Label.pLabelName = InName.c_str();
    Label.color[0] = Label.color[1] = Label.color[2] = Label.color[3] = 1.0f;
    BeginLabel(Context.Cmd, &Label);
    ++Context.LabelDepth;
}

void VulkanDynamicGDI::RecordEndDrawEvent(GDIRecordingContext* InContext)
{
    VKRecordingContext& Context = *AsVK(InContext);
    if(!Context.bRecording || Context.LabelDepth <= 0 || !DeviceObj) return;
    static const auto EndLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(DeviceObj->GetInstance(), "vkCmdEndDebugUtilsLabelEXT"));
    if(!EndLabel) return;
    EndLabel(Context.Cmd);
    --Context.LabelDepth;
}

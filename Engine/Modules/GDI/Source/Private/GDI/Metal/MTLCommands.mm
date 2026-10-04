#include "Public/GDI/Metal/MetalDynamicGDI.h"
#include <algorithm>
#include "Public/GDI/Metal/MTLDevice.h"
#include "Public/GDI/Metal/MTLCommandManager.h"
#include "Public/GDI/Metal/MTLRenderTarget.h"
#include "Public/GDI/Metal/MTLSync.h"
#include "Private/GDI/Metal/MTLUtil.h"

#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

extern GDISync* GSync;

// ===========================================================================
//  Encoder management
//
//  Metal has no equivalent of Vulkan's vkCmdBeginRendering/vkCmdEndRendering pair
//  operating on an already-open command buffer: a render pass IS an encoder, and
//  its load/store actions are fixed when the encoder is created. So every
//  operation that Vulkan expresses as "end the region, do something, begin it
//  again" (SetRenderTarget, Clear, a blit copy, a mid-frame flush) is expressed
//  here as "end the encoder, then open a new one".
// ===========================================================================

void MetalDynamicGDI::EndActiveEncoder()
{
    if(!CurrentStateData) return;
    if(CurrentStateData->CurrentEncoder != nullptr)
    {
        [(id<MTLRenderCommandEncoder>)CurrentStateData->CurrentEncoder endEncoding];
        [(id<MTLRenderCommandEncoder>)CurrentStateData->CurrentEncoder release];
        CurrentStateData->CurrentEncoder = nullptr;
    }
    CurrentStateData->bRenderingActive = false;
}

void MetalDynamicGDI::BeginEncoderForRT(MTLRenderTarget* InRT, bool bClear, const EngineCore::Color& ClearColor)
{
    if(!CurrentStateData || !InRT || !InRT->IsValid()) return;
    if(CurrentStateData->CurrentCmd == nullptr) return;

    MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
    Pass.colorAttachments[0].texture     = (id<MTLTexture>)InRT->GetTexture();
    Pass.colorAttachments[0].loadAction  = bClear ? MTLLoadActionClear : MTLLoadActionLoad;
    Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if(bClear)
    {
        Pass.colorAttachments[0].clearColor = MTLClearColorMake(ClearColor.Rf(),
                                                                ClearColor.Gf(),
                                                                ClearColor.Bf(),
                                                                ClearColor.Af());
    }

    id<MTLRenderCommandEncoder> Encoder =
        [[(id<MTLCommandBuffer>)CurrentStateData->CurrentCmd renderCommandEncoderWithDescriptor:Pass] retain];
    if(Encoder == nil)
    {
        GDI_LOG("[MetalDynamicGDI] renderCommandEncoderWithDescriptor returned nil.")
        return;
    }
    Encoder.label = [NSString stringWithFormat:@"Pass:%s", InRT->GetName().c_str()];

    CurrentStateData->CurrentEncoder     = Encoder;
    CurrentStateData->ActiveRenderTarget = InRT;
    CurrentStateData->bRenderingActive   = true;

    // The pass has now written (or cleared) the target, so subsequent binds can
    // safely use MTLLoadActionLoad.
    InRT->SetHasContent(true);

    ApplyViewportToEncoder();
}

void MetalDynamicGDI::ApplyViewportToEncoder()
{
    if(!CurrentStateData || CurrentStateData->CurrentEncoder == nullptr) return;
    id<MTLRenderCommandEncoder> Enc = (id<MTLRenderCommandEncoder>)CurrentStateData->CurrentEncoder;

    MTLViewport VP;
    VP.originX = CurrentStateData->ViewportX;
    VP.originY = CurrentStateData->ViewportY;
    VP.width   = CurrentStateData->ViewportW;
    VP.height  = CurrentStateData->ViewportH;
    VP.znear   = 0.0;
    VP.zfar    = 1.0;
    if(VP.width > 0.0 && VP.height > 0.0)
    {
        [Enc setViewport:VP];
    }

    // Metal rejects a scissor rect that leaves the attachment, and the GDI viewport
    // can legitimately be larger than the render target for a frame or two during a
    // window resize -- clamp instead of asserting.
    if(MTLRenderTarget* RT = CurrentStateData->ActiveRenderTarget)
    {
        const uint RTW = RT->GetWidth();
        const uint RTH = RT->GetHeight();
        if(RTW == 0 || RTH == 0) return;

        const NSUInteger X = std::min<NSUInteger>(CurrentStateData->ScissorX, RTW);
        const NSUInteger Y = std::min<NSUInteger>(CurrentStateData->ScissorY, RTH);
        const NSUInteger W = std::min<NSUInteger>(CurrentStateData->ScissorW, RTW - X);
        const NSUInteger H = std::min<NSUInteger>(CurrentStateData->ScissorH, RTH - Y);
        if(W > 0 && H > 0)
        {
            MTLScissorRect Scissor;
            Scissor.x      = X;
            Scissor.y      = Y;
            Scissor.width  = W;
            Scissor.height = H;
            [Enc setScissorRect:Scissor];
        }
    }
}

// ===========================================================================
//  Frame begin / end
// ===========================================================================

void MetalDynamicGDI::BeginRendering()
{
    if(!CommandManager || !CurrentStateData) return;

    // Reentrancy guard, matching the Vulkan backend: a nested BeginRendering (a
    // render command or slot calling GDIBeginRendering while a frame is already
    // recording) must be a no-op. Here it would otherwise leak the open command
    // buffer and lose everything recorded into it.
    if(CurrentStateData->CurrentCmd != nullptr)
    {
#ifdef QT_DEBUG
        static int NestedWarnCount = 0;
        if(NestedWarnCount < 10)
        {
            ++NestedWarnCount;
            GDI_LOG("[MetalDynamicGDI] Nested BeginRendering ignored (frame already recording).")
        }
#endif
        return;
    }

    // BeginFrame blocks until the previous submission completed, so one frame is
    // in flight at a time (the analog of waiting Vulkan's FrameFence).
    MTLCommandBufferRef Cmd = CommandManager->BeginFrame();
    if(Cmd == nullptr) return;

    CurrentStateData->CurrentCmd         = Cmd;
    CurrentStateData->CurrentEncoder     = nullptr;
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->bRenderingActive   = false;
    CurrentStateData->DebugLabelDepth    = 0;
}

void MetalDynamicGDI::EndRendering()
{
    if(!CommandManager || !CurrentStateData || CurrentStateData->CurrentCmd == nullptr) return;

    EndActiveEncoder();
    CloseDebugGroups();

    CommandManager->EndFrame();
    CommandManager->Commit();

    CurrentStateData->CurrentCmd       = nullptr;
    CurrentStateData->bRenderingActive = false;

    FramesCommitted.fetch_add(1);

    // A full frame has now been rendered (into freshly-resized targets, if a resize
    // happened this frame) and committed -- the next present is safe to run.
    bResizing.store(false);
}

// ===========================================================================
//  Clear / viewport
// ===========================================================================

void MetalDynamicGDI::Clear(uint InClearMode, EngineCore::Color InClearColor, float /*InDepth*/, uint /*InStencilMask*/)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive) return;
    if(!(InClearMode & CM_Color)) return;

    MTLRenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT) return;

    // Metal clears at pass start, so a mid-pass clear means closing the encoder and
    // reopening the same attachment with MTLLoadActionClear. This is where Vulkan
    // would issue vkCmdClearAttachments. Depth and stencil bits are ignored: the
    // render targets are colour-only, exactly as in the Vulkan backend.
    EndActiveEncoder();
    BeginEncoderForRT(RT, /*bClear*/ true, InClearColor);
}

void MetalDynamicGDI::SetViewport(uint InX, uint InY, uint InWidth, uint InHeight)
{
    if(!CurrentStateData) return;

    CurrentStateData->ViewportX = double(InX);
    CurrentStateData->ViewportY = double(InY);
    CurrentStateData->ViewportW = double(InWidth);
    CurrentStateData->ViewportH = double(InHeight);

    CurrentStateData->ScissorX = InX;
    CurrentStateData->ScissorY = InY;
    CurrentStateData->ScissorW = InWidth;
    CurrentStateData->ScissorH = InHeight;

    ApplyViewportToEncoder();
}

// ===========================================================================
//  Render state
//
//  Metal bakes blend and colour-write into the pipeline object and depth/stencil
//  into a separate state object, so these setters only record intent -- the state
//  is resolved into cached Metal objects by BindDrawState at draw time.
// ===========================================================================

void MetalDynamicGDI::SetDepthState(IDepthStateDesc InDesc)
{
    if(CurrentStateData) CurrentStateData->DepthState = InDesc;
}

void MetalDynamicGDI::SetBlendState(IBlendStateDesc InDesc)
{
    if(CurrentStateData) CurrentStateData->BlendState = InDesc;
}

void MetalDynamicGDI::SetStencilState(IStencilStateDesc InDesc)
{
    if(CurrentStateData) CurrentStateData->StencilState = InDesc;
}

void MetalDynamicGDI::SetColorMask(EColorMask InMask)
{
    if(CurrentStateData) CurrentStateData->ColorMask = InMask;
}

// ===========================================================================
//  Debug events (Xcode / Instruments GPU capture)
// ===========================================================================

// On the command buffer, not the encoder: an event spans encoder changes
// (SetRenderTarget, Clear, a blit), as RenderCore's stage events do.

void MetalDynamicGDI::BeginDrawEvent(const std::string& EventName)
{
    if(!CurrentStateData || CurrentStateData->CurrentCmd == nullptr) return;

    NSString* Label = [NSString stringWithUTF8String:EventName.c_str()];
    if(Label == nil) return;

    [(id<MTLCommandBuffer>)CurrentStateData->CurrentCmd pushDebugGroup:Label];
    ++CurrentStateData->DebugLabelDepth;
}

void MetalDynamicGDI::EndDrawEvent()
{
    if(!CurrentStateData || CurrentStateData->CurrentCmd == nullptr) return;

    // Only pop a group opened on the CURRENT command buffer: a mid-frame
    // FlushForReadback commits the old one (closing its groups) and starts the
    // depth again at 0 -- the same guard the Vulkan backend needs for labels.
    if(CurrentStateData->DebugLabelDepth <= 0) return;

    [(id<MTLCommandBuffer>)CurrentStateData->CurrentCmd popDebugGroup];
    --CurrentStateData->DebugLabelDepth;
}

void MetalDynamicGDI::CloseDebugGroups()
{
    if(!CurrentStateData) return;
    if(CurrentStateData->CurrentCmd != nullptr)
    {
        for(; CurrentStateData->DebugLabelDepth > 0; --CurrentStateData->DebugLabelDepth)
        {
            [(id<MTLCommandBuffer>)CurrentStateData->CurrentCmd popDebugGroup];
        }
    }
    CurrentStateData->DebugLabelDepth = 0;
}

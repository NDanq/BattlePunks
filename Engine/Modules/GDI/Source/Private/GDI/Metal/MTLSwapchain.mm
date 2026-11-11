#include "Private/GDI/Metal/MTLInternal.h"

#include <algorithm>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "Public/GDI/GDIPlatform.h"
#include "Public/GDI/GDILog.h"

/*
 * The present: the finished render target drawn into the window's CAMetalLayer
 * drawable by a full-screen triangle. UI thread (GDI.h), on the driver's queue,
 * so it runs after the frames committed before it.
 *
 * The blit shader is the backend's own, not a cooked one: it needs no engine
 * block, only the source texture, and does not depend on the cook having run.
 * Both are top-down (GDI.h), so the rows copy as they are.
 */

namespace
{
    NSString* const kBlitSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Interp { float4 Position [[position]]; float2 UV; };

vertex Interp BlitVS(uint Id [[vertex_id]])
{
    // One triangle covering the screen; v = 0 at the top row.
    const float2 UV = float2((Id << 1) & 2, Id & 2);
    Interp Out;
    Out.Position = float4(UV * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    Out.UV       = UV;
    return Out;
}

fragment float4 BlitPS(Interp In [[stage_in]], texture2d<float> Source [[texture(0)]], sampler Linear [[sampler(0)]])
{
    return float4(Source.sample(Linear, In.UV).rgb, 1.0);
}
)MSL";
}

bool MTLSwapchainImpl::Initialize(id<MTLDevice> InDevice, id<MTLCommandQueue> InQueue, const EngineCore::WindowHandle InWindow)
{
    Device = InDevice;
    Queue  = InQueue;
    Window = InWindow;
    if(!Device || !Queue || !Window.IsValid()) return false;

    /*
     * The game's NSView is backed by a CAMetalLayer already (GameWindowNativeMac.cpp,
     * SDL_Metal_CreateView). Another host's view -- the editor's Qt viewport, a
     * native QWidget -- gets one of its own as a sublayer, kept to the view's
     * size at every present (UI thread, which AppKit wants for layers).
     */
    View = (__bridge NSView*)Window.Native;
    if(!View) return false;
    if([[View layer] isKindOfClass:[CAMetalLayer class]])
    {
        Layer = (CAMetalLayer*)[View layer];
    }
    else
    {
        View.wantsLayer = YES;
        Layer = [CAMetalLayer layer];
        Layer.opaque = YES;
        [[View layer] addSublayer:Layer];
        bOwnLayer = true;
        FitOwnLayer();
    }
    Layer.device          = Device;
    Layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    Layer.framebufferOnly = YES;
    // [Rendering] SwapchainExtraImages: 0 is two drawables, more is three (a CAMetalLayer's most).
    Layer.maximumDrawableCount = GDIGetConfiguredSwapchainExtraImages() == 0 ? 2 : 3;
    UpdateDrawableSize();

    NSError* Error = nil;
    id<MTLLibrary> Library = [Device newLibraryWithSource:kBlitSource options:nil error:&Error];
    if(!Library)
    {
        MTLUtil::LogError("the present blit shader", Error);
        return false;
    }
    MTLRenderPipelineDescriptor* Desc = [[MTLRenderPipelineDescriptor alloc] init];
    Desc.vertexFunction   = [Library newFunctionWithName:@"BlitVS"];
    Desc.fragmentFunction = [Library newFunctionWithName:@"BlitPS"];
    Desc.colorAttachments[0].pixelFormat = Layer.pixelFormat;
    Pipeline = [Device newRenderPipelineStateWithDescriptor:Desc error:&Error];
    if(!Pipeline)
    {
        MTLUtil::LogError("the present pipeline", Error);
        return false;
    }
    MTLSamplerDescriptor* SamplerDesc = [[MTLSamplerDescriptor alloc] init];
    SamplerDesc.minFilter    = MTLSamplerMinMagFilterLinear;
    SamplerDesc.magFilter    = MTLSamplerMinMagFilterLinear;
    SamplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    SamplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    Sampler = [Device newSamplerStateWithDescriptor:SamplerDesc];
    GDI_LOG("[MTLSwapchain] Present layer ready.")
    return Sampler != nil;
}

void MTLSwapchainImpl::FitOwnLayer()
{
    if(!bOwnLayer || !Layer || !View) return;
    const CGFloat Scale = View.window ? View.window.backingScaleFactor : 1.0;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];   // no implicit animation of the resize
    Layer.frame         = View.layer.bounds;
    Layer.contentsScale = Scale;
    [CATransaction commit];
}

MTLSwapchainImpl::~MTLSwapchainImpl()
{
    if(bOwnLayer && Layer) [Layer removeFromSuperlayer];
}

void MTLSwapchainImpl::UpdateDrawableSize()
{
    FitOwnLayer();
    uint32_t Width = 0, Height = 0;
    if(!Layer || !GDIPlatform::appGetWindowPixelSize(Window, Width, Height)) return;
    const CGSize Wanted = CGSizeMake(std::max<uint32_t>(1, Width), std::max<uint32_t>(1, Height));
    // Assigning drawableSize remakes the drawable pool: only when it changed.
    if(!CGSizeEqualToSize(Layer.drawableSize, Wanted))
    {
        Layer.drawableSize = Wanted;
    }
}

void MTLSwapchainImpl::Present(MTLRenderTargetImpl* InSource)
{
    if(!Layer || !Pipeline || !InSource || !InSource->GetColor() || !InSource->HasContent()) return;

    // nextDrawable and present inside a pool, or drawables pile up until the event loop drains.
    @autoreleasepool
    {
        UpdateDrawableSize();
        // VSync (GDIGetVSync), read every present so a change applies at the next.
        const BOOL bVSync = GDIGetVSync() ? YES : NO;
        if(Layer.displaySyncEnabled != bVSync) Layer.displaySyncEnabled = bVSync;

        id<CAMetalDrawable> Drawable = [Layer nextDrawable];
        if(!Drawable) return;

        id<MTLCommandBuffer> Cmd = [Queue commandBuffer];
        Cmd.label = @"Nimble.Present";
        MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
        Pass.colorAttachments[0].texture     = Drawable.texture;
        Pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
        Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> Encoder = [Cmd renderCommandEncoderWithDescriptor:Pass];
        [Encoder setRenderPipelineState:Pipeline];
        [Encoder setFragmentTexture:InSource->GetColor() atIndex:0];
        [Encoder setFragmentSamplerState:Sampler atIndex:0];
        [Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [Encoder endEncoding];
        [Cmd presentDrawable:Drawable];
        [Cmd commit];
    }
}

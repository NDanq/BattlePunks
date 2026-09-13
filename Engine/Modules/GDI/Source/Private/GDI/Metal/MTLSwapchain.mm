#include "Public/GDI/Metal/MTLSwapchain.h"
#include "Public/GDI/GDIPlatform.h"
#include "Public/Core/CoreFileProvider.h"
#include <vector>
#include <algorithm>
#include "Public/GDI/Metal/MTLDevice.h"
#include "Public/GDI/Metal/MTLRenderTarget.h"
#include "Private/GDI/Metal/MTLPipelineCache.h"   // MTLBindings

#import <QuartzCore/CAMetalLayer.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

/**
 * Full-screen NDC quad, interleaved {float3 position, float2 texcoord}.
 *
 * V mapping: Metal's NDC +Y is the TOP of the attachment (row 0), while the
 * offscreen RT holds GL-ordered rows (row 0 = visual bottom -- the render thread
 * draws through a Y-flipped projection; see PackViewParamsUBO in MTLCommands.mm).
 * So the drawable's top row must sample v=1, i.e. v = (y+1)/2. This is the
 * OPPOSITE of the Vulkan blit quad, whose NDC +Y is the bottom of the image.
 */
static const float kQuadVerts[] = {
    // pos.x pos.y pos.z    u     v
    -1.f, -1.f, 0.f,       0.f,  0.f,
    -1.f,  1.f, 0.f,       0.f,  1.f,
     1.f,  1.f, 0.f,       1.f,  1.f,
     1.f,  1.f, 0.f,       1.f,  1.f,
     1.f, -1.f, 0.f,       1.f,  0.f,
    -1.f, -1.f, 0.f,       0.f,  0.f,
};

/** Sampler/texture index for Screen.fsh -- SAMPLER2D(2, ScreenTexture). */
static constexpr NSUInteger kScreenTextureIndex = 2;

MTLSwapchain::~MTLSwapchain()
{
    Shutdown();
}

bool MTLSwapchain::Initialize(MTLDevice* InDevice, EngineCore::WindowHandle InWindow)
{
    if(bInitFailed) return false;

    Device = InDevice;
    Window = InWindow;
    if(!Device || !Device->IsValid() || !Window.IsValid())
    {
        bInitFailed = true;
        return false;
    }

    // Grab the layer lazily on the first present: winId() forces window creation,
    // and the CAMetalLayer only exists once the platform window is up. This mirrors
    // the Vulkan backend deferring surfaceForWindow() to the first PresentToWindow.
    // WindowHandle already carries the platform's native view handle -- on
    // Apple that is the NSView*/UIView* the Qt bridge obtained from winId().
    // There is no QWindow to dereference here any more.
#if TARGET_OS_OSX
    NSView* View = reinterpret_cast<NSView*>(Window.Native);
#else
    UIView* View = reinterpret_cast<UIView*>(Window.Native);
#endif
    if(View == nil)
    {
        // Not latched as a failure: the platform window may simply not be up yet,
        // and the next present will retry.
        GDI_LOG("[MTLSwapchain] Window has no native view yet.")
        return false;
    }

    CALayer* BackingLayer = [View layer];
    if(BackingLayer == nil || ![BackingLayer isKindOfClass:[CAMetalLayer class]])
    {
        GDI_LOG("[MTLSwapchain] Window's backing layer is not a CAMetalLayer -- "
             "was the host's surface type set to a Metal surface before the window was created?")
        bInitFailed = true;
        return false;
    }

    Layer = (CAMetalLayer*)BackingLayer;
    Layer.device          = Device->GetDevice();
    Layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    // The drawable is only ever a render target here, never sampled or copied.
    Layer.framebufferOnly = YES;
    UpdateDrawableSize();

    if(!CreateBlitPipeline())
    {
        Layer       = nullptr;
        bInitFailed = true;
        return false;
    }

    GDI_LOG("[MTLSwapchain] Present layer ready.")
    return true;
}

void MTLSwapchain::Shutdown()
{
    DestroyBlitPipeline();
    // The layer is owned by the NSView/Qt; just drop the reference.
    Layer  = nullptr;
    Window = nullptr;
    Device = nullptr;
}

void MTLSwapchain::UpdateDrawableSize()
{
    if(Layer == nullptr || !Window.IsValid()) return;

    // Physical pixels straight from the platform, replacing
    // width()/height() * devicePixelRatio(). See appGetWindowPixelSize.
    uint32_t PixelWidth = 0, PixelHeight = 0;
    if(!GDIPlatform::appGetWindowPixelSize(Window, PixelWidth, PixelHeight)) return;

    const CGSize Wanted = CGSizeMake(std::max<double>(1.0, double(PixelWidth)),
                                     std::max<double>(1.0, double(PixelHeight)));

    // Assigning drawableSize reallocates the drawable pool, so only do it when the
    // size actually changed -- this runs on every present.
    const CGSize Current = Layer.drawableSize;
    if(Current.width != Wanted.width || Current.height != Wanted.height)
    {
        Layer.drawableSize = Wanted;
    }
}

// ---------------------------------------------------------------------------
//  Blit pipeline
// ---------------------------------------------------------------------------

MTLFunctionRef MTLSwapchain::LoadFunction(const std::string& MslResourcePath, MTLLibraryRef& OutLibrary) const
{
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(Provider == nullptr)
    {
        GDI_LOG("[MTLSwapchain] No file provider installed for: " + MslResourcePath)
        return nullptr;
    }

    std::vector<uint8_t> Bytes;
    if(!Provider->ReadAll(MslResourcePath, Bytes))
    {
        GDI_LOG("[MTLSwapchain] Failed to open MSL resource: " + MslResourcePath)
        return nullptr;
    }
    if(Bytes.empty())
    {
        GDI_LOG("[MTLSwapchain] MSL resource is empty: " + MslResourcePath)
        return nullptr;
    }

    NSString* Source = [[[NSString alloc] initWithBytes:Bytes.data()
                                                 length:(NSUInteger)Bytes.size()
                                               encoding:NSUTF8StringEncoding] autorelease];
    NSError* Error = nil;
    id<MTLLibrary> Library = [Device->GetDevice() newLibraryWithSource:Source
                                                               options:nil
                                                                 error:&Error];   // +1
    if(Library == nil)
    {
        const char* Reason = Error ? [[Error localizedDescription] UTF8String] : "unknown error";
        GDI_LOG("[MTLSwapchain] newLibraryWithSource failed for " + MslResourcePath
             + ": " + ::GDILogDetail::ToLogString(Reason))
        return nullptr;
    }

    id<MTLFunction> Function = [Library newFunctionWithName:@"main0"];   // +1
    if(Function == nil)
    {
        GDI_LOG("[MTLSwapchain] Entry point 'main0' not found in: " + MslResourcePath)
        [Library release];
        return nullptr;
    }

    OutLibrary = Library;
    return Function;
}

bool MTLSwapchain::CreateBlitPipeline()
{
    BlitVS = LoadFunction("Shaders/Cooked/Metal/Screen.vsh.metal", BlitVSLibrary);
    BlitFS = LoadFunction("Shaders/Cooked/Metal/Screen.fsh.metal", BlitFSLibrary);
    if(BlitVS == nullptr || BlitFS == nullptr)
    {
        DestroyBlitPipeline();
        return false;
    }

    // Interleaved quad: both attributes come from one buffer, unlike the render
    // thread's split position/texcoord streams.
    MTLVertexDescriptor* VtxDesc = [MTLVertexDescriptor vertexDescriptor];
    VtxDesc.attributes[0].format      = MTLVertexFormatFloat3;
    VtxDesc.attributes[0].offset      = 0;
    VtxDesc.attributes[0].bufferIndex = MTLBindings::PositionBuffer;
    VtxDesc.attributes[1].format      = MTLVertexFormatFloat2;
    VtxDesc.attributes[1].offset      = sizeof(float) * 3;
    VtxDesc.attributes[1].bufferIndex = MTLBindings::PositionBuffer;
    VtxDesc.layouts[MTLBindings::PositionBuffer].stride       = sizeof(float) * 5;
    VtxDesc.layouts[MTLBindings::PositionBuffer].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* Desc = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    Desc.vertexFunction                  = (id<MTLFunction>)BlitVS;
    Desc.fragmentFunction                = (id<MTLFunction>)BlitFS;
    Desc.vertexDescriptor                = VtxDesc;
    Desc.colorAttachments[0].pixelFormat = Layer.pixelFormat;

    NSError* Error = nil;
    BlitPipeline = [Device->GetDevice() newRenderPipelineStateWithDescriptor:Desc error:&Error];   // +1
    if(BlitPipeline == nullptr)
    {
        const char* Reason = Error ? [[Error localizedDescription] UTF8String] : "unknown error";
        GDI_LOG(std::string("[MTLSwapchain] Blit pipeline creation failed: ") + ::GDILogDetail::ToLogString(Reason))
        DestroyBlitPipeline();
        return false;
    }

    MTLSamplerDescriptor* SamplerDesc = [[[MTLSamplerDescriptor alloc] init] autorelease];
    SamplerDesc.minFilter    = MTLSamplerMinMagFilterLinear;
    SamplerDesc.magFilter    = MTLSamplerMinMagFilterLinear;
    SamplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    SamplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    BlitSampler = [Device->GetDevice() newSamplerStateWithDescriptor:SamplerDesc];   // +1
    if(BlitSampler == nullptr)
    {
        DestroyBlitPipeline();
        return false;
    }

    QuadBuffer = [Device->GetDevice() newBufferWithBytes:kQuadVerts
                                                  length:sizeof(kQuadVerts)
                                                 options:MTLResourceStorageModeShared];   // +1
    if(QuadBuffer == nullptr)
    {
        DestroyBlitPipeline();
        return false;
    }
    return true;
}

void MTLSwapchain::DestroyBlitPipeline()
{
    if(QuadBuffer   != nullptr) { [(id<MTLBuffer>)QuadBuffer release];                    QuadBuffer   = nullptr; }
    if(BlitSampler  != nullptr) { [(id<MTLSamplerState>)BlitSampler release];             BlitSampler  = nullptr; }
    if(BlitPipeline != nullptr) { [(id<MTLRenderPipelineState>)BlitPipeline release];     BlitPipeline = nullptr; }
    if(BlitVS       != nullptr) { [(id<MTLFunction>)BlitVS release];                      BlitVS       = nullptr; }
    if(BlitFS       != nullptr) { [(id<MTLFunction>)BlitFS release];                      BlitFS       = nullptr; }
    if(BlitVSLibrary!= nullptr) { [(id<MTLLibrary>)BlitVSLibrary release];                BlitVSLibrary= nullptr; }
    if(BlitFSLibrary!= nullptr) { [(id<MTLLibrary>)BlitFSLibrary release];                BlitFSLibrary= nullptr; }
}

// ---------------------------------------------------------------------------
//  Present
// ---------------------------------------------------------------------------

void MTLSwapchain::PresentFrame(MTLRenderTarget* InMainRT)
{
    if(!IsValid() || !InMainRT || !InMainRT->IsValid()) return;

    // Nothing has been rendered into this target yet -- presenting it would show
    // uninitialized private memory.
    if(!InMainRT->HasContent()) return;

    // nextDrawable / present must run inside an autorelease pool: without one the
    // drawables accumulate until the UI event loop drains, and the layer stalls.
    @autoreleasepool
    {
        UpdateDrawableSize();

        id<CAMetalDrawable> Drawable = [Layer nextDrawable];
        if(Drawable == nil) return;   // pool exhausted this vsync; try again next frame

        id<MTLCommandBuffer> Cmd = [Device->GetGraphicsQueue() commandBuffer];
        Cmd.label = @"Odessa.Present";

        MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
        Pass.colorAttachments[0].texture     = Drawable.texture;
        Pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;   // quad covers everything
        Pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> Encoder = [Cmd renderCommandEncoderWithDescriptor:Pass];
        Encoder.label = @"Odessa.PresentBlit";

        MTLViewport VP;
        VP.originX = 0.0;
        VP.originY = 0.0;
        VP.width   = double(Drawable.texture.width);
        VP.height  = double(Drawable.texture.height);
        VP.znear   = 0.0;
        VP.zfar    = 1.0;
        [Encoder setViewport:VP];

        [Encoder setRenderPipelineState:(id<MTLRenderPipelineState>)BlitPipeline];
        [Encoder setVertexBuffer:(id<MTLBuffer>)QuadBuffer offset:0 atIndex:MTLBindings::PositionBuffer];
        [Encoder setFragmentTexture:(id<MTLTexture>)InMainRT->GetTexture() atIndex:kScreenTextureIndex];
        [Encoder setFragmentSamplerState:(id<MTLSamplerState>)BlitSampler atIndex:kScreenTextureIndex];
        [Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [Encoder endEncoding];

        [Cmd presentDrawable:Drawable];
        [Cmd commit];
    }
}

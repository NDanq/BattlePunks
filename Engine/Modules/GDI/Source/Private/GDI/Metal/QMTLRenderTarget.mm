#include "Public/GDI/Metal/QMTLRenderTarget.h"
#include "Public/GDI/Metal/QMTLDevice.h"
#include "Public/GDI/Metal/QMTLTexture.h"
#include "Public/GDI/Metal/QMetalDynamicGDI.h"
#include "Public/GDI/GDITexture.h"

#include <cstring>
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// Global GDI driver (declared in GDI.cpp) -- used to route synchronous readbacks
// and render-target resolves through the active command buffer / device.
extern GDIDynamicDriver* GDynamicGDI;

static QMetalDynamicGDI* GetMTLDriver()
{
    // Yields null during driver teardown (the derived object is already gone),
    // which the callers below treat as "nothing in flight to coordinate with".
    return dynamic_cast<QMetalDynamicGDI*>(GDynamicGDI);
}

QMTLRenderTarget::QMTLRenderTarget(QMTLDevice* InDevice, const std::string& InName, uint InWidth, uint InHeight)
    : GDIRenderTarget(InWidth, InHeight)
    , Device(InDevice)
    , Name(InName)
{
    Allocate();
}

QMTLRenderTarget::~QMTLRenderTarget()
{
    if(QMetalDynamicGDI* GDI = GetMTLDriver())
    {
        GDI->UnregisterRenderTarget(this);
    }
    DestroyTexture();
}

void QMTLRenderTarget::Allocate()
{
    if(!Device || !Device->IsValid()) return;
    if(Width == 0 || Height == 0)     return;

    MTLTextureDescriptor* Desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:(NSUInteger)Width
                                                           height:(NSUInteger)Height
                                                        mipmapped:NO];
    // RenderTarget: written as a color attachment.
    // ShaderRead:   sampled by the layer/blit shaders and by the present blit.
    Desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    Desc.storageMode = MTLStorageModePrivate;

    Texture = [Device->GetDevice() newTextureWithDescriptor:Desc];   // +1
    if(Texture == nullptr)
    {
        GDI_LOG("[QMTLRenderTarget] newTextureWithDescriptor failed for " + Name)
        return;
    }
    Texture.label = [NSString stringWithFormat:@"RT:%s", Name.c_str()];

    // Fresh allocation: nothing has been rendered into it yet.
    bHasContent = false;
}

void QMTLRenderTarget::DestroyTexture()
{
    // The sample-wrapper borrows Texture; tear it down before the texture goes.
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

    if(Texture != nullptr)
    {
        [Texture release];
        Texture = nullptr;
    }
    bHasContent = false;
}

void QMTLRenderTarget::Bind()
{
    // The render pass (and therefore the encoder) is opened by
    // QMetalDynamicGDI::SetRenderTarget, which owns the command buffer.
}

void QMTLRenderTarget::Release()
{
    DestroyTexture();
}

void QMTLRenderTarget::Resize(const uint InWidth, const uint InHeight)
{
    // This RT may be the render thread's *active* target with an open encoder in
    // the current (not-yet-committed) command buffer. Close and commit that buffer
    // first -- WITHOUT re-binding this RT -- so the encoder finishes against a
    // still-valid texture before we release it.
    if(QMetalDynamicGDI* GDI = GetMTLDriver())
    {
        // Tell the UI-thread present to skip until a full frame is re-rendered into
        // the new targets (cleared in EndRendering). Prevents the present from
        // sampling a half-recreated target.
        GDI->SetResizing(true);
        GDI->FlushForReadback(/*bResumeActiveRegion*/ false);
        GDI->WaitIdle();
    }

    // No device-wide wait is needed beyond that. A UI-thread present may still hold
    // this texture, but a committed command buffer retains its resources, so the
    // release below only drops OUR reference -- the texture outlives us until that
    // present finishes. This is what replaces Vulkan's WaitIdle-before-destroy.

    GDIRenderTarget::Resize(InWidth, InHeight);
    DestroyTexture();
    Allocate();
}

bool QMTLRenderTarget::IsValid()
{
    return Texture != nullptr;
}

EngineCore::Image QMTLRenderTarget::ToImage()
{
    if(!Device || !Device->IsValid() || Texture == nullptr) return EngineCore::Image();
    if(Width == 0 || Height == 0)                           return EngineCore::Image();

    QMetalDynamicGDI* GDI = GetMTLDriver();

    // Submit & finish any in-flight draws targeting this RT so the readback sees them.
    if(GDI) GDI->FlushForReadback();

    // Buffer<->texture blits have a per-device row-alignment requirement; rounding
    // the destination stride up to 256 satisfies every GPU family, so the readback
    // arrives padded and is copied out row by row below.
    const NSUInteger TightRowBytes = (NSUInteger)Width * 4u;
    const NSUInteger BytesPerRow   = ((TightRowBytes + 255u) / 256u) * 256u;
    const NSUInteger DataSize      = BytesPerRow * (NSUInteger)Height;

    // Private storage: the pixels have to come back through a blit encoder into a
    // shared buffer (the Metal analog of vkCmdCopyImageToBuffer + staging memory).
    id<MTLBuffer> Readback = [Device->GetDevice() newBufferWithLength:DataSize
                                                              options:MTLResourceStorageModeShared];
    if(Readback == nil) return EngineCore::Image();

    id<MTLCommandBuffer>     Cmd  = [Device->GetGraphicsQueue() commandBuffer];
    id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
    [Blit copyFromTexture:Texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)Width, (NSUInteger)Height, 1)
                 toBuffer:Readback
        destinationOffset:0
   destinationBytesPerRow:BytesPerRow
 destinationBytesPerImage:DataSize];
    [Blit endEncoding];
    [Cmd commit];
    [Cmd waitUntilCompleted];

    // The render thread draws with a Y-flipped projection so RT memory is
    // row-identical to a GL framebuffer (row 0 = visual bottom) -- see
    // QMetalDynamicGDI's PackViewParamsUBO. Mirror vertically exactly like the
    // OpenGL and Vulkan backends so all three return the same orientation.
    EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    const auto* Src = static_cast<const uint8_t*>([Readback contents]);
    for(uint Row = 0; Row < Height; ++Row)
    {
        std::memcpy(Result.scanLine(int(Row)), Src + Row * BytesPerRow, (size_t)TightRowBytes);
    }
    [Readback release];
    Result.FlipVertical();
    return Result;
}

GDITexture2D* QMTLRenderTarget::ToGDITexture2D()
{
    if(Texture == nullptr) return nullptr;

    // Lazily wrap the RT's color texture in a GDITexture2D so the layer / blit
    // shaders can sample it. The adopting QMTLTextureResource borrows the texture
    // (it does not release it). Invalidated by DestroyTexture on resize.
    if(!TextureWrapper)
    {
        TextureWrapper           = new GDITexture2D();
        TextureWrapper->Width    = Width;
        TextureWrapper->Height   = Height;
        TextureWrapper->Resource =
            new QMTLTextureResource(Device, Texture, TextureWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Linear);
    }
    return TextureWrapper;
}

void QMTLRenderTarget::ResolveRenderTarget(GDIRenderTarget* Dest)
{
    auto* DstRT = dynamic_cast<QMTLRenderTarget*>(Dest);
    if(!DstRT) return;
    if(QMetalDynamicGDI* GDI = GetMTLDriver())
    {
        GDI->RecordResolve(this, DstRT);
    }
}

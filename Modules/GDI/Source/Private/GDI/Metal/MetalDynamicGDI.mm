#include "Private/GDI/Metal/MTLInternal.h"

#include <algorithm>
#include <cstring>

#include "Private/GDI/GDICorePack.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDIView.h"
#include "Public/GDI/GDILog.h"
#include "Public/GDI/Metal/MTLSync.h"

using namespace EngineCore;

/*
 * The Metal driver. Its shape follows D3D12DynamicGDI, with Metal's encoder
 * model underneath:
 *
 *  - PASSES OPEN LAZILY. SetRenderTargets only records the targets; the first
 *    draw opens a render encoder on them. A Clear before that becomes the
 *    pass's load action (no draw needed); a Clear after ends the encoder and
 *    the next pass loads with a clear. Anything that is not a draw -- a
 *    dispatch, a buffer update, a copy, new targets -- ends the encoder first,
 *    and a pass still waiting to clear is opened and closed so the clear
 *    happens in order.
 *  - FRAMES IN FLIGHT: [Rendering] FramesInFlight blocks may run on the GPU at
 *    once; BeginRendering waits for the oldest. Each slot has its own upload
 *    ring for blocks and instance data.
 *  - LIFETIMES: command buffers retain what they reference, so a resource
 *    released while the GPU still reads it lives until that work is done --
 *    no deferred-destruction queues.
 */

// ===========================================================================
// MetalState helpers
// ===========================================================================

id<MTLRenderCommandEncoder> MetalState::RenderEncoder()
{
    if(Encoder) return Encoder;
    if(!Cmd || !bPassBound || ColorCount == 0 || !Colors[0] || !Colors[0]->GetColor()) return nil;

    MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
    for(uint32_t Index = 0; Index < ColorCount; ++Index)
    {
        MTLRenderPassColorAttachmentDescriptor* Color = Pass.colorAttachments[Index];
        Color.texture     = Colors[Index]->GetColor();
        Color.loadAction  = bClearColor ? MTLLoadActionClear : MTLLoadActionLoad;
        Color.clearColor  = ClearColor;
        Color.storeAction = MTLStoreActionStore;
        Colors[Index]->MarkContent();
    }
    if(DepthRT && DepthRT->GetDepth())
    {
        Pass.depthAttachment.texture     = DepthRT->GetDepth();
        Pass.depthAttachment.loadAction  = bClearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        Pass.depthAttachment.clearDepth  = ClearDepth;
        Pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    bClearColor = false;
    bClearDepth = false;

    Encoder = [Cmd renderCommandEncoderWithDescriptor:Pass];
    if(!Encoder) return nil;
    Bound.Forget();

    // Counter-clockwise on the target as stored is front (EGDICullMode), as D3D12's FrontCounterClockwise.
    [Encoder setFrontFacingWinding:MTLWindingCounterClockwise];

    // The viewport and scissor, inside the attachment (Metal refuses a scissor past its edge).
    const NSUInteger Width  = Colors[0]->GetColor().width;
    const NSUInteger Height = Colors[0]->GetColor().height;
    [Encoder setViewport:Viewport];
    MTLScissorRect Clipped = Scissor;
    Clipped.x      = std::min<NSUInteger>(Clipped.x, Width);
    Clipped.y      = std::min<NSUInteger>(Clipped.y, Height);
    Clipped.width  = std::min<NSUInteger>(Clipped.width, Width - Clipped.x);
    Clipped.height = std::min<NSUInteger>(Clipped.height, Height - Clipped.y);
    if(Clipped.width == 0 || Clipped.height == 0)
    {
        Clipped = MTLScissorRect{ 0, 0, 0, 0 };   // a zero scissor draws nothing, as asked
    }
    [Encoder setScissorRect:Clipped];
    return Encoder;
}

void MetalState::EndPass()
{
    if(!Encoder && Cmd && bPassBound && (bClearColor || bClearDepth))
    {
        RenderEncoder();   // opened with its clear as the load action, so the clear happens
    }
    if(Encoder)
    {
        [Encoder endEncoding];
        Encoder = nil;
    }
    Bound.Forget();
}

MTLUploadRing::Allocation MetalState::Upload(const void* InData, const NSUInteger InBytes, const NSUInteger InPadTo)
{
    const NSUInteger Bytes = std::max<NSUInteger>({ InBytes, InPadTo, 16 });
    MTLUploadRing::Allocation Out;
    if(Cmd)
    {
        Out = Ring.Allocate(FrameSlot, Bytes);
    }
    else
    {
        // Outside a block: its own buffer, kept alive by the command buffer that reads it.
        Out.Buffer = [Device newBufferWithLength:Bytes options:MTLResourceStorageModeShared];
        Out.Offset = 0;
        Out.CPU    = Out.Buffer.contents;
    }
    if(Out.CPU)
    {
        if(InData && InBytes) std::memcpy(Out.CPU, InData, InBytes);
        if(Bytes > InBytes)   std::memset(static_cast<uint8_t*>(Out.CPU) + InBytes, 0, Bytes - InBytes);
    }
    return Out;
}

id<MTLCommandBuffer> MetalState::CommandBufferForWork(bool& bOutTransient)
{
    if(Cmd)
    {
        EndPass();
        bOutTransient = false;
        return Cmd;
    }
    bOutTransient = true;
    id<MTLCommandBuffer> Transient = [Queue commandBuffer];
    Transient.label = @"Nimble.Immediate";
    return Transient;
}

// ===========================================================================
// Lifetime
// ===========================================================================

MetalDynamicGDI::MetalDynamicGDI(WindowHandle InWindow)
    : GDIDynamicDriver(InWindow)
    , S(new MetalState())
{
    S->Driver = this;
}

MetalDynamicGDI::~MetalDynamicGDI()
{
    Destroy();
    delete S;
    S = nullptr;
}

void MetalDynamicGDI::AssignContext(GDIContext* InContext)
{
    Context = InContext;
}

void MetalDynamicGDI::Allocate()
{
    if(bInitialized) return;
    SetThreadID(appThreadID());

    if(!Context || Context->ContextType != GDIContext::MetalContext)
    {
        GDI_LOG_ERROR("[MetalDynamicGDI] No MetalGDIContext assigned.")
        return;
    }

    @autoreleasepool
    {
        S->Device = MTLCreateSystemDefaultDevice();
        if(!S->Device)
        {
            GDI_LOG_ERROR("[MetalDynamicGDI] No Metal device.")
            return;
        }
        S->Queue = [S->Device newCommandQueue];
        S->Queue.label = @"Nimble.Graphics";

        S->FramesInFlight = GDIGetConfiguredFramesInFlight();
        FramesInFlight    = S->FramesInFlight;
        FrameSlot         = 0;
        S->FrameSlot      = 0;
        S->FrameSemaphore = dispatch_semaphore_create(long(S->FramesInFlight));
        S->Ring.Initialize(S->Device, S->FramesInFlight);
        S->Pipelines = new MTLPipelineCache(S->Device);

        // What a draw samples where nothing is bound (D3D12's null texture: 1x1, zeroed).
        Image NullImage(1, 1, EImageFormat::RGBA8);
        S->NullTexture = new MTLTextureResourceImpl(S, nullptr, &NullImage, 1, GDI_TB_Repeat, GDI_TF_Nearest);

        bInitialized = true;
        GDI_LOG("[MetalDynamicGDI] " + std::string([[S->Device name] UTF8String]) + ", "
                + std::to_string(S->FramesInFlight) + " frame(s) in flight ([Rendering] FramesInFlight).")
    }
}

void MetalDynamicGDI::Destroy()
{
    // Before the device goes: cached shaders and textures hold Metal objects (GDI.h).
    ReleaseCachedResources();
    if(!S || !S->Device) return;

    if(S->Cmd)
    {
        S->EndPass();
        [S->Cmd commit];
        [S->Cmd waitUntilCompleted];
        S->Cmd = nil;
    }
    WaitForCommitted();

    if(S->ScreenMesh)  { S->ScreenMesh->Release(); delete S->ScreenMesh; S->ScreenMesh = nullptr; }
    if(S->NullTexture) { delete S->NullTexture; S->NullTexture = nullptr; }
    for(auto& Entry : S->Swapchains) { delete Entry.second; }
    S->Swapchains.clear();
    S->LiveRenderTargets.clear();
    delete S->Pipelines;
    S->Pipelines = nullptr;
    S->FrameSemaphore = nullptr;
    S->Queue  = nil;
    S->Device = nil;
    bInitialized = false;
}

void MetalDynamicGDI::SwapBuffers()
{
}

void MetalDynamicGDI::Flush()
{
}

void MetalDynamicGDI::SetResizing(const bool bInResizing)
{
    S->bResizing.store(bInResizing);
}

bool MetalDynamicGDI::CheckError(const std::string&)
{
    return false;
}

// ===========================================================================
// Frames
// ===========================================================================

void MetalDynamicGDI::BeginRendering()
{
    if(!bInitialized || S->Cmd) return;

    // The slot's last frame must be done before its upload ring is reused.
    dispatch_semaphore_wait(S->FrameSemaphore, DISPATCH_TIME_FOREVER);
    FrameSlot = S->FrameSlot;
    S->Ring.Rewind(S->FrameSlot);

    S->Cmd = [S->Queue commandBuffer];
    S->Cmd.label = @"Nimble.Frame";
    S->bPassBound  = false;
    S->ColorCount  = 0;
    S->DepthRT     = nullptr;
    S->bClearColor = false;
    S->bClearDepth = false;
    S->DebugGroupDepth = 0;
    S->Bound.Forget();
}

void MetalDynamicGDI::EndRendering()
{
    if(!S->Cmd) return;
    S->EndPass();
    while(S->DebugGroupDepth > 0)
    {
        [S->Cmd popDebugGroup];
        --S->DebugGroupDepth;
    }

    dispatch_semaphore_t Semaphore = S->FrameSemaphore;
    [S->Cmd addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(Semaphore); }];
    [S->Cmd commit];
    {
        std::lock_guard<std::mutex> Lock(S->CommitLock);
        S->LastCommitted = S->Cmd;
    }
    S->Cmd        = nil;
    S->bPassBound = false;
    S->FrameSlot  = (S->FrameSlot + 1) % S->FramesInFlight;
    S->bResizing.store(false);
}

void MetalDynamicGDI::FlushAndWait(const bool bInResume)
{
    if(!S->Cmd)
    {
        WaitForCommitted();
        return;
    }

    MTLRenderTargetImpl* Colors[GDI_MAX_COLOR_TARGETS] = {};
    const uint32_t       ColorCount = S->ColorCount;
    std::copy(S->Colors, S->Colors + GDI_MAX_COLOR_TARGETS, Colors);
    MTLRenderTargetImpl* Depth  = S->DepthRT;
    const bool           bBound = S->bPassBound;

    S->EndPass();
    const int Groups = S->DebugGroupDepth;
    for(int Each = 0; Each < Groups; ++Each) { [S->Cmd popDebugGroup]; }
    [S->Cmd commit];
    [S->Cmd waitUntilCompleted];
    {
        std::lock_guard<std::mutex> Lock(S->CommitLock);
        S->LastCommitted = S->Cmd;
    }

    // The block goes on in a new command buffer, in the same frame slot.
    S->Cmd = [S->Queue commandBuffer];
    S->Cmd.label = @"Nimble.Frame";
    S->DebugGroupDepth = 0;
    S->bPassBound = false;
    S->ColorCount = 0;
    S->DepthRT    = nullptr;
    if(bInResume && bBound && ColorCount > 0)
    {
        std::copy(Colors, Colors + GDI_MAX_COLOR_TARGETS, S->Colors);
        S->ColorCount = ColorCount;
        S->DepthRT    = Depth;
        S->bPassBound = true;
    }
}

void MetalDynamicGDI::WaitForCommitted()
{
    id<MTLCommandBuffer> Last = nil;
    {
        std::lock_guard<std::mutex> Lock(S->CommitLock);
        Last = S->LastCommitted;
    }
    if(Last) [Last waitUntilCompleted];
}

void MTLSync::WaitSync()
{
    if(!bFenced) return;
    if(MetalDynamicGDI* Driver = GetMetalDriver()) Driver->WaitForCommitted();
    bFenced = false;
}

// ===========================================================================
// Render targets and clears
// ===========================================================================

GDIRenderTarget* MetalDynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    return CreateRenderTargetFormat(std::move(Name), InWidth, InHeight, GDI_RTF_RGBA8);
}

GDIRenderTarget* MetalDynamicGDI::CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat)
{
    if(!bInitialized) return nullptr;
    auto* RT = new MTLRenderTargetImpl(S, Name, InWidth, InHeight, MTLUtil::RenderTargetFormat(InFormat));
    S->LiveRenderTargets.insert(RT);
    return RT;
}

bool MetalDynamicGDI::SupportsRenderTargetFormat(const EGDIRenderTargetFormat InFormat) const
{
    return InFormat == GDI_RTF_RGBA8 || InFormat == GDI_RTF_RGBA16F || InFormat == GDI_RTF_RGB10A2
        || InFormat == GDI_RTF_RGBA32F || InFormat == GDI_RTF_R32F;
}

void MetalDynamicGDI::SetRenderTarget(GDIRenderTarget* InRenderTarget)
{
    SetRenderTargets(&InRenderTarget, 1, InRenderTarget);
}

namespace
{
    MTLRenderTargetImpl* GActiveTarget = nullptr;   // colour 0 of the last targets bound: what PresentToWindow shows
}

void MetalDynamicGDI::SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom)
{
    if(!S->Cmd || !InColors || InCount == 0) return;
    InCount = std::min<uint>(InCount, GDI_MAX_COLOR_TARGETS);

    MTLRenderTargetImpl* Colors[GDI_MAX_COLOR_TARGETS] = {};
    for(uint Index = 0; Index < InCount; ++Index)
    {
        Colors[Index] = dynamic_cast<MTLRenderTargetImpl*>(InColors[Index]);
        if(!Colors[Index] || !Colors[Index]->IsValid()) return;
        if(Colors[Index]->GetWidth() != Colors[0]->GetWidth() || Colors[Index]->GetHeight() != Colors[0]->GetHeight())
        {
            GDI_LOG("[MetalDynamicGDI] SetRenderTargets: targets of different sizes; nothing bound")
            return;
        }
    }
    auto* Depth = dynamic_cast<MTLRenderTargetImpl*>(InDepthFrom);
    if(Depth && (!Depth->GetDepth() || Depth->GetWidth() != Colors[0]->GetWidth() || Depth->GetHeight() != Colors[0]->GetHeight()))
    {
        Depth = nullptr;
    }

    S->EndPass();
    std::copy(Colors, Colors + GDI_MAX_COLOR_TARGETS, S->Colors);
    S->ColorCount  = InCount;
    S->DepthRT     = Depth;
    S->bPassBound  = true;
    S->bClearColor = false;
    S->bClearDepth = false;
    if(Depth && Depth->TakeDepthNeedsClear())
    {
        S->bClearDepth = true;
        S->ClearDepth  = 1.0;
    }
    GActiveTarget = Colors[0];
}

void MetalDynamicGDI::ReleaseRenderTarget(GDIRenderTarget* InRenderTarget)
{
    if(InRenderTarget) InRenderTarget->Release();
}

void MetalDynamicGDI::ForgetRenderTarget(MTLRenderTargetImpl* InRT)
{
    if(!S) return;
    S->LiveRenderTargets.erase(InRT);
    bool bBound = S->DepthRT == InRT;
    for(uint32_t Index = 0; Index < S->ColorCount; ++Index) { bBound = bBound || S->Colors[Index] == InRT; }
    if(bBound)
    {
        S->EndPass();
        S->bPassBound = false;
        S->ColorCount = 0;
        S->DepthRT    = nullptr;
    }
    if(GActiveTarget == InRT) GActiveTarget = nullptr;
}

void MetalDynamicGDI::Clear(uint InClearMode, Color InClearColor, float InDepth, uint /*InStencilMask*/)
{
    if(!S->Cmd || !S->bPassBound) return;
    // An open pass ends; the next opens with the clear as its load action.
    if(S->Encoder) S->EndPass();
    if(InClearMode & CM_Color)
    {
        S->bClearColor = true;
        S->ClearColor  = MTLClearColorMake(InClearColor.Rf(), InClearColor.Gf(), InClearColor.Bf(), InClearColor.Af());
    }
    if((InClearMode & CM_Depth) && S->DepthRT)
    {
        S->bClearDepth = true;
        S->ClearDepth  = InDepth;
    }
}

void MetalDynamicGDI::SetViewport(uint InX, uint InY, uint InWidth, uint InHeight)
{
    // Rows from the top, as Metal's viewport counts them (GDI.h's orientation).
    S->Viewport = MTLViewport{ double(InX), double(InY), double(std::max<uint>(InWidth, 1)), double(std::max<uint>(InHeight, 1)), 0.0, 1.0 };
    S->Scissor  = MTLScissorRect{ InX, InY, InWidth, InHeight };
    if(S->Encoder)
    {
        [S->Encoder setViewport:S->Viewport];
        SetScissorRect(InX, InY, InWidth, InHeight);
    }
}

void MetalDynamicGDI::SetScissorRect(uint InX, uint InY, uint InWidth, uint InHeight)
{
    S->Scissor = MTLScissorRect{ InX, InY, InWidth, InHeight };
    if(S->Encoder && S->ColorCount > 0 && S->Colors[0])
    {
        const NSUInteger W = S->Colors[0]->GetColor().width, H = S->Colors[0]->GetColor().height;
        MTLScissorRect Clipped = S->Scissor;
        Clipped.x      = std::min<NSUInteger>(Clipped.x, W);
        Clipped.y      = std::min<NSUInteger>(Clipped.y, H);
        Clipped.width  = std::min<NSUInteger>(Clipped.width, W - Clipped.x);
        Clipped.height = std::min<NSUInteger>(Clipped.height, H - Clipped.y);
        [S->Encoder setScissorRect:Clipped];
    }
}

void MetalDynamicGDI::SetDepthState(IDepthStateDesc InDesc)   { S->DepthState = InDesc; }
void MetalDynamicGDI::SetBlendState(IBlendStateDesc InDesc)   { S->BlendState = InDesc; }
void MetalDynamicGDI::SetStencilState(IStencilStateDesc InDesc) { S->StencilState = InDesc; }
void MetalDynamicGDI::SetColorMask(EColorMask InMask)         { S->ColorMask = InMask; }

void MetalDynamicGDI::BeginDrawEvent(const std::string& EventName)
{
    if(!S->Cmd || !GDIShaderDebugEnabled()) return;
    [S->Cmd pushDebugGroup:MTLUtil::ToNS(EventName)];
    ++S->DebugGroupDepth;
}

void MetalDynamicGDI::EndDrawEvent()
{
    if(!S->Cmd || S->DebugGroupDepth <= 0) return;
    [S->Cmd popDebugGroup];
    --S->DebugGroupDepth;
}

// ===========================================================================
// Resources
// ===========================================================================

GDIShaderResource* MetalDynamicGDI::CreateShaderResource(EShaderType InType, const std::string& InSrcPath)
{
    @autoreleasepool
    {
        return new MTLShaderResourceImpl(S->Device, InType, ProcessShaderFromSource(InSrcPath, ERP_Metal));
    }
}

void MetalDynamicGDI::SetBoundShader(GDIShader* InVS, GDIShader* InPS)
{
    BoundVertexShader = InVS;
    BoundPixelShader  = InPS;
    S->BoundVS = (InVS && InVS->Resource) ? static_cast<MTLShaderResourceImpl*>(InVS->Resource) : nullptr;
    S->BoundPS = (InPS && InPS->Resource) ? static_cast<MTLShaderResourceImpl*>(InPS->Resource) : nullptr;
}

GDIMesh* MetalDynamicGDI::CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int InNumTexCoords,
                                         bool bIsInstanced)
{
    if(!bInitialized) return nullptr;
    GDIMesh::ORenderData RenderData;
    RenderData.Position = static_cast<float*>(malloc(sizeof(float) * size_t(InNumVerts)));
    std::memcpy(RenderData.Position, VertData, sizeof(float) * size_t(InNumVerts));
    RenderData.TexCoord = static_cast<float*>(malloc(sizeof(float) * size_t(InNumTexCoords)));
    std::memcpy(RenderData.TexCoord, TexData, sizeof(float) * size_t(InNumTexCoords));
    RenderData.NumVertices = uint(InNumVerts);

    auto* Mesh = new MTLMeshImpl(S);
    Mesh->SetRenderData(RenderData);
    Mesh->SetSupportsInstanced(bIsInstanced);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* MetalDynamicGDI::CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced)
{
    const float HW = InWidth * 0.5f, HH = InHeight * 0.5f;
    const float Verts[] = { -HW, -HH, 0.f,  -HW, HH, 0.f,  HW, HH, 0.f,  HW, HH, 0.f,  HW, -HH, 0.f,  -HW, -HH, 0.f };
    const float UVs[]   = { 0.f, 0.f,  0.f, 1.f,  1.f, 1.f,  1.f, 1.f,  1.f, 0.f,  0.f, 0.f };
    return CreateQuadMesh(Verts, UVs, 18, 12, bIsInstanced);
}

GDIMesh* MetalDynamicGDI::CreateMesh(const GDIMeshDesc& InDesc)
{
    if(!bInitialized) return nullptr;
    GDIMesh::ORenderData RenderData;
    if(!GDIMakeIndexedRenderData(InDesc, RenderData)) return nullptr;
    auto* Mesh = new MTLMeshImpl(S);
    Mesh->SetRenderData(RenderData);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* MetalDynamicGDI::CreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    if(!bInitialized) return nullptr;
    auto* Mesh = new MTLMeshImpl(S);
    if(!Mesh->InitDynamic(InMaxVertices, InMaxIndices, S->FramesInFlight))
    {
        delete Mesh;
        return nullptr;
    }
    return Mesh;
}

GDITextureResource* MetalDynamicGDI::CreateTextureResource(const Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    if(!bInitialized) return nullptr;
    @autoreleasepool { return new MTLTextureResourceImpl(S, nullptr, &InImage, 1, InBorder, InFilter); }
}

GDITextureResource* MetalDynamicGDI::CreateTextureResource(GDITexture2D* InParent, const Image& InImage, const EGDITextureBorder InBorder,
                                                           const EGDITextureFilter InFilter)
{
    if(!bInitialized) return nullptr;
    @autoreleasepool { return new MTLTextureResourceImpl(S, InParent, &InImage, 1, InBorder, InFilter); }
}

GDITextureResource* MetalDynamicGDI::CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    if(!bInitialized) return nullptr;
    return new MTLTextureResourceImpl(S, InParent, InBorder, InFilter);
}

GDITextureResource* MetalDynamicGDI::CreateTextureResourceMips(const Image* InLevels, const uint32_t InCount, EGDITextureBorder InBorder,
                                                               EGDITextureFilter InFilter)
{
    if(!bInitialized || !InLevels || InCount == 0) return nullptr;
    @autoreleasepool { return new MTLTextureResourceImpl(S, nullptr, InLevels, InCount, InBorder, InFilter); }
}

namespace
{
    MTLPixelFormat ToMetalCompressed(const EGDICompressedFormat InFormat)
    {
        switch(InFormat)
        {
            case GDI_CF_BC1: return MTLPixelFormatBC1_RGBA;
            case GDI_CF_BC3: return MTLPixelFormatBC3_RGBA;
            case GDI_CF_BC4: return MTLPixelFormatBC4_RUnorm;
            case GDI_CF_BC5: return MTLPixelFormatBC5_RGUnorm;
            case GDI_CF_BC7: return MTLPixelFormatBC7_RGBAUnorm;
            default:         return MTLPixelFormatInvalid;
        }
    }
}

bool MetalDynamicGDI::SupportsCompressedFormat(const EGDICompressedFormat InFormat) const
{
    if(!S->Device || ToMetalCompressed(InFormat) == MTLPixelFormatInvalid) return false;
    if(@available(macOS 11.0, *)) { return S->Device.supportsBCTextureCompression; }
    return false;
}

GDITextureResource* MetalDynamicGDI::CreateCompressedTextureResource(EGDICompressedFormat InFormat, const GDICompressedLevel* InLevels,
                                                                     uint32_t InCount, EGDITextureBorder InBorder, EGDITextureFilter InFilter)
{
    if(!SupportsCompressedFormat(InFormat) || !InLevels || InCount == 0) return nullptr;
    if(InLevels[0].Width % 4 != 0 || InLevels[0].Height % 4 != 0) return nullptr;
    @autoreleasepool
    {
        auto* Texture = new MTLTextureResourceImpl(S, ToMetalCompressed(InFormat), InLevels, InCount, InBorder, InFilter);
        if(!Texture->GetTexture())
        {
            delete Texture;
            return nullptr;
        }
        return Texture;
    }
}

// ===========================================================================
// The draw path
// ===========================================================================

namespace
{
    using ScalarMap = std::unordered_map<std::string, MTLUniformValue>;

    bool StartsWith(const std::string& InText, const char* InPrefix)
    {
        return InText.rfind(InPrefix, 0) == 0;
    }

    void WriteScalar(const ScalarMap& InScalars, const char* InName, float* OutData, const int InCount)
    {
        const auto It = InScalars.find(InName);
        if(It != InScalars.end()) std::memcpy(OutData, It->second.Data.v, size_t(InCount) * sizeof(float));
    }

    /* ViewParameters, 160 bytes: 0 Projection, 64 View, 128 MouseCoords (+4), 144 Resolution. */
    void PackViewParams(const ScalarMap& InScalars, float (&OutView)[40])
    {
        std::memset(OutView, 0, sizeof(OutView));
        WriteScalar(InScalars, "ViewParameters.Projection",  OutView + 0,  16);
        WriteScalar(InScalars, "ViewParameters.View",        OutView + 16, 16);
        WriteScalar(InScalars, "ViewParameters.MouseCoords", OutView + 32, 3);
        WriteScalar(InScalars, "ViewParameters.Resolution",  OutView + 36, 4);
    }

    /* Transform, 144 bytes: 0 PositionWorldSpace (+4), 16 Model, 80 PrevModel (Model unless set). */
    void PackTransform(const ScalarMap& InScalars, float (&OutTransform)[36])
    {
        std::memset(OutTransform, 0, sizeof(OutTransform));
        WriteScalar(InScalars, "Transform.PositionWorldSpace", OutTransform + 0, 3);
        WriteScalar(InScalars, "Transform.Model",              OutTransform + 4, 16);
        WriteScalar(InScalars, "Transform.Model",              OutTransform + 20, 16);
        WriteScalar(InScalars, "Transform.PrevModel",          OutTransform + 20, 16);
    }

    /* Every non-core scalar in map order, as D3D12's PackCustom: each custom block holds one today. */
    constexpr uint32_t kCustomBytes = 256;
    void PackCustom(const ScalarMap& InScalars, uint8_t (&OutBytes)[kCustomBytes])
    {
        std::memset(OutBytes, 0, sizeof(OutBytes));
        uint32_t Offset = 0;
        for(const auto& Entry : InScalars)
        {
            if(StartsWith(Entry.first, "ViewParameters.") || StartsWith(Entry.first, "Transform.")) continue;
            const MTLUniformValue& V = Entry.second;
            float       IntAsFloat = 0.f;
            uint32_t    Size = 0;
            const void* Ptr  = nullptr;
            switch(V.Tag)
            {
                case MTLUniformValue::INT:   IntAsFloat = float(V.Data.i); Size = 4; Ptr = &IntAsFloat; break;
                case MTLUniformValue::FLOAT: Size = 4;  Ptr = &V.Data.f; break;
                case MTLUniformValue::VEC2:  Size = 8;  Ptr = V.Data.v;  break;
                case MTLUniformValue::VEC3:  Size = 12; Ptr = V.Data.v;  break;
                case MTLUniformValue::VEC4:  Size = 16; Ptr = V.Data.v;  break;
                case MTLUniformValue::MAT:   Size = 64; Ptr = V.Data.v;  break;
            }
            if(!Ptr || Offset + Size > kCustomBytes) continue;
            std::memcpy(OutBytes + Offset, Ptr, Size);
            Offset += Size;
        }
    }

    void SetBothBytes(id<MTLRenderCommandEncoder> InEncoder, const void* InData, const NSUInteger InBytes, const NSUInteger InIndex)
    {
        [InEncoder setVertexBytes:InData length:InBytes atIndex:InIndex];
        [InEncoder setFragmentBytes:InData length:InBytes atIndex:InIndex];
    }

    void SetBothBuffer(id<MTLRenderCommandEncoder> InEncoder, const MTLUploadRing::Allocation& InAt, const NSUInteger InIndex)
    {
        [InEncoder setVertexBuffer:InAt.Buffer offset:InAt.Offset atIndex:InIndex];
        [InEncoder setFragmentBuffer:InAt.Buffer offset:InAt.Offset atIndex:InIndex];
    }

    void SetBothTexture(id<MTLRenderCommandEncoder> InEncoder, MTLTextureResourceImpl* InTexture, id<MTLSamplerState> InSampler,
                        const NSUInteger InBinding)
    {
        [InEncoder setVertexTexture:InTexture->GetTexture() atIndex:InBinding];
        [InEncoder setFragmentTexture:InTexture->GetTexture() atIndex:InBinding];
        [InEncoder setVertexSamplerState:InSampler atIndex:MTLBindings::Sampler(InBinding)];
        [InEncoder setFragmentSamplerState:InSampler atIndex:MTLBindings::Sampler(InBinding)];
    }
}

void MetalDynamicGDI::DrawMesh(MTLMeshImpl* InMesh)
{
    if(!InMesh || !InMesh->IsValid() || !S->Cmd) return;
    MTLShaderResourceImpl* VS = S->BoundVS;
    MTLShaderResourceImpl* PS = S->BoundPS;
    if(!VS || !PS || !VS->IsValid() || !PS->IsValid()) return;

    @autoreleasepool
    {
        id<MTLRenderCommandEncoder> Encoder = S->RenderEncoder();
        if(!Encoder) return;

        // The pipeline.
        MTLPipelineCache::Key Key;
        Key.VertexID   = VS->GetID();
        Key.FragmentID = PS->GetID();
        Key.Layout     = InMesh->GetLayout(GetInstanceBuffer() != nullptr);
        Key.bBlend     = S->BlendState.bBlend;
        Key.Src        = MTLUtil::BlendFactor(S->BlendState.Source);
        Key.Dst        = MTLUtil::BlendFactor(S->BlendState.Dest);
        Key.ColorOp    = MTLUtil::BlendOp(S->BlendState.Op);
        Key.AlphaOp    = MTLUtil::BlendOp(S->BlendState.AlphaOp);
        Key.WriteMask  = MTLUtil::ColorMask(S->ColorMask);
        Key.ColorCount = S->ColorCount;
        for(uint32_t Index = 0; Index < S->ColorCount; ++Index) { Key.Colors[Index] = S->Colors[Index]->GetFormat(); }
        Key.DepthFormat = (S->DepthRT && S->DepthRT->GetDepth()) ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;

        id<MTLRenderPipelineState> Pipeline = S->Pipelines->GetRender(Key, VS, PS);
        if(!Pipeline) return;
        if(S->Bound.Pipeline != Pipeline)
        {
            [Encoder setRenderPipelineState:Pipeline];
            S->Bound.Pipeline = Pipeline;
        }

        const bool bDepth = Key.DepthFormat != MTLPixelFormatInvalid && S->DepthState.bDepthTest;
        id<MTLDepthStencilState> DepthStencil = S->Pipelines->GetDepthStencil(bDepth, S->DepthState.bDepthMask,
                                                                              MTLUtil::Compare(S->DepthState.DepthMode));
        if(S->Bound.DepthStencil != DepthStencil)
        {
            [Encoder setDepthStencilState:DepthStencil];
            S->Bound.DepthStencil = DepthStencil;
        }
        const MTLCullMode Cull = S->DepthState.CullMode == GDI_CULL_FRONT ? MTLCullModeFront
                               : S->DepthState.CullMode == GDI_CULL_BACK  ? MTLCullModeBack : MTLCullModeNone;
        if(!S->Bound.bCullKnown || S->Bound.Cull != Cull)
        {
            [Encoder setCullMode:Cull];
            S->Bound.Cull       = Cull;
            S->Bound.bCullKnown = true;
        }

        // The core blocks: both stages carry them; the pixel shader's copy has the custom scalars too.
        const ScalarMap& Scalars = PS->GetScalars();
        const MTLShaderResourceImpl::CoreBlocks& Core = PS->GetCoreBlocks();
        alignas(16) float View[40];
        if(Core.bView) std::memcpy(View, Core.View, sizeof(View));
        else           PackViewParams(Scalars, View);
        GDICorePack::RemapClipZ(View);   // GL's z in [-1, 1] to Metal's [0, 1]; no y flip (GDI.h)
        alignas(16) float Transform[36];
        if(Core.bTransform) std::memcpy(Transform, Core.Transform, sizeof(Transform));
        else                PackTransform(Scalars, Transform);
        SetBothBytes(Encoder, View, sizeof(View), MTLBindings::View);
        SetBothBytes(Encoder, Transform, sizeof(Transform), MTLBindings::Transform);

        if(IsMaterialDraw())
        {
            // The material layout's blocks, uploaded when their serial changes (the scene: once a frame).
            struct BlockSpec { uint32_t Block; NSUInteger Binding; NSUInteger PadTo; uint64_t* BoundSerial; };
            const BlockSpec Blocks[] = {
                { 0, MTLBindings::Custom,   256,                      &S->Bound.MaterialSerial },
                { 1, MTLBindings::Scene,    256,                      &S->Bound.SceneSerial },
                { 2, MTLBindings::Skin,     GDI_SKIN_BLOCK_MAX_BYTES, &S->Bound.SkinSerial[0] },
                { 3, MTLBindings::PrevSkin, GDI_SKIN_BLOCK_MAX_BYTES, &S->Bound.SkinSerial[1] },
                { 4, MTLBindings::Light,    GDI_LIGHT_BLOCK_MAX_BYTES, &S->Bound.LightSerial },
            };
            for(const BlockSpec& Each : Blocks)
            {
                if(*Each.BoundSerial == MaterialBlockSerials[Each.Block]) continue;
                const std::vector<uint8_t>& Bytes = MaterialBlocks[Each.Block];
                const NSUInteger Size = (std::max<NSUInteger>(Bytes.size(), Each.PadTo) + 255) & ~NSUInteger(255);
                const MTLUploadRing::Allocation At = S->Upload(Bytes.empty() ? nullptr : Bytes.data(), Bytes.size(), Size);
                if(!At.Buffer) return;
                SetBothBuffer(Encoder, At, Each.Binding);
                *Each.BoundSerial = MaterialBlockSerials[Each.Block];
            }

            for(uint32_t Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot)
            {
                GDITextureResource* Resource = MaterialTextures[Slot];
                if(S->Bound.bMaterialTexturesKnown && Resource && S->Bound.MaterialTextures[Slot] == Resource) continue;
                auto* Texture = static_cast<MTLTextureResourceImpl*>(Resource);
                if(!Texture || !Texture->GetTexture()) Texture = S->NullTexture;
                SetBothTexture(Encoder, Texture, S->Pipelines->GetSampler(Texture->GetBorder(), Texture->GetFilter(), Texture->HasMips()),
                               GDI_MATERIAL_TEXTURE_BINDING + Slot);
                S->Bound.MaterialTextures[Slot] = Resource;
            }
            S->Bound.bMaterialTexturesKnown = true;
        }
        else
        {
            uint8_t Custom[kCustomBytes];
            PackCustom(Scalars, Custom);
            SetBothBytes(Encoder, Custom, sizeof(Custom), MTLBindings::Custom);
            S->Bound.MaterialSerial = ~0ull;   // binding 2 no longer holds the material block
            S->Bound.bMaterialTexturesKnown = false;

            // The one texture, at 2 and 3: a shader samples one or the other as it declares a custom block or not.
            auto* Texture = static_cast<MTLTextureResourceImpl*>(PS->GetTexture0());
            if(!Texture || !Texture->GetTexture()) Texture = S->NullTexture;
            id<MTLSamplerState> Sampler = S->Pipelines->GetSampler(Texture->GetBorder(), Texture->GetFilter(), Texture->HasMips());
            SetBothTexture(Encoder, Texture, Sampler, 2);
            SetBothTexture(Encoder, Texture, Sampler, 3);
        }

        InMesh->Encode(Encoder, *S);
    }
}

void MetalDynamicGDI::DrawObject(GDIViewObject* InObject, GDIView* InView)
{
    if(!InObject || !InObject->Mesh) return;

    // The core blocks packed once onto both shaders (GDICorePack), the user uniforms by name.
    alignas(16) float View[40];
    alignas(16) float Transform[36];
    if(InView) GDICorePack::PackCoreView(InView, View);
    if(InObject->Transform) GDICorePack::PackCoreTransform(InObject->Transform, Transform);
    for(GDIShader* Shader : { BoundVertexShader, BoundPixelShader })
    {
        if(!Shader || !Shader->Resource) continue;
        MTLShaderResourceImpl::CoreBlocks& Core = static_cast<MTLShaderResourceImpl*>(Shader->Resource)->GetCoreBlocks();
        if(InView)
        {
            std::memcpy(Core.View, View, sizeof(View));
            Core.bView = true;
        }
        if(InObject->Transform)
        {
            std::memcpy(Core.Transform, Transform, sizeof(Transform));
            Core.bTransform = true;
        }
        Shader->UpdateUserDefinedUniforms();
    }
    InObject->Mesh->Draw();
}

void MetalDynamicGDI::DrawScreenQuadPrimitive()
{
    if(!bInitialized) return;
    if(BoundPixelShader) BoundPixelShader->UpdateUserDefinedUniforms();
    if(!S->ScreenMesh)
    {
        const float Verts[] = { -1.f, -1.f, 0.f,  -1.f, 1.f, 0.f,  1.f, 1.f, 0.f,  1.f, 1.f, 0.f,  1.f, -1.f, 0.f,  -1.f, -1.f, 0.f };
        // v = 0 at NDC +1, the target's row 0: an identity copy.
        const float UVs[]   = { 0.f, 1.f,  0.f, 0.f,  1.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f };
        S->ScreenMesh = CreateQuadMesh(Verts, UVs, 18, 12, false);
    }
    if(S->ScreenMesh) S->ScreenMesh->Draw();
}

Color MetalDynamicGDI::GetColorFromSamplePoint(const EngineCore::Point& InPoint, const EngineCore::Size& /*InSize*/)
{
    MTLRenderTargetImpl* RT = GActiveTarget;
    if(!RT || !RT->IsValid()) return Color();
    const Image Pixels = RT->ToImage();
    if(InPoint.X < 0 || InPoint.Y < 0 || InPoint.X >= Pixels.GetWidth() || InPoint.Y >= Pixels.GetHeight()) return Color();
    const uint8_t* P = Pixels.GetData() + (size_t(InPoint.Y) * size_t(Pixels.GetWidth()) + size_t(InPoint.X)) * 4;
    return Color(P[0], P[1], P[2]);
}

// ===========================================================================
// Compute and buffers
// ===========================================================================

bool MetalDynamicGDI::SupportsCompute() const
{
    return bInitialized && S->Pipelines != nullptr;
}

GDIBuffer* MetalDynamicGDI::CreateBuffer(const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
{
    if(!SupportsCompute()) return nullptr;
    auto* Buffer = new MTLBufferImpl(S->Device, InBytes, InUsage, InInitialData);
    if(!Buffer->IsValid())
    {
        delete Buffer;
        return nullptr;
    }
    return Buffer;
}

bool MetalDynamicGDI::DispatchCompute(GDIShader* InShader, const void* InParams, const uint32_t InParamBytes, GDIBuffer* const* InBuffers,
                                      const uint32_t InBufferCount, GDITextureResource* InTexture, const uint32_t InX, const uint32_t InY,
                                      const uint32_t InZ)
{
    if(!SupportsCompute() || !InShader || !InShader->Resource) return false;
    auto* CS = static_cast<MTLShaderResourceImpl*>(InShader->Resource);
    id<MTLComputePipelineState> Pipeline = S->Pipelines->GetCompute(CS);
    if(!Pipeline) return false;

    MTLBufferImpl* Buffers[GDI_COMPUTE_MAX_STORAGE_BUFFERS] = {};
    for(uint32_t Each = 0; Each < InBufferCount; ++Each)
    {
        Buffers[Each] = static_cast<MTLBufferImpl*>(InBuffers[Each]);
        if(!Buffers[Each] || !Buffers[Each]->IsValid()) return false;
    }
    auto* Texture = static_cast<MTLTextureResourceImpl*>(InTexture);
    if(Texture && !Texture->GetTexture()) return false;
    if(!Texture) Texture = S->NullTexture;

    @autoreleasepool
    {
        bool bTransient = false;
        id<MTLCommandBuffer> Cmd = S->CommandBufferForWork(bTransient);
        if(!Cmd) return false;

        const NSUInteger ParamBytes = (std::max<NSUInteger>(256, InParamBytes) + 255) & ~NSUInteger(255);
        const MTLUploadRing::Allocation Params = S->Upload(InParams, InParamBytes, ParamBytes);
        if(!Params.Buffer) return false;

        id<MTLComputeCommandEncoder> Encoder = [Cmd computeCommandEncoder];
        [Encoder setComputePipelineState:Pipeline];
        [Encoder setBuffer:Params.Buffer offset:Params.Offset atIndex:0];
        // Storage at 1.. in order; the unused slots repeat the last, as D3D12's root UAVs do.
        for(uint32_t Slot = 0; InBufferCount > 0 && Slot < GDI_COMPUTE_MAX_STORAGE_BUFFERS; ++Slot)
        {
            [Encoder setBuffer:Buffers[std::min(Slot, InBufferCount - 1)]->GetBuffer() offset:0 atIndex:1 + Slot];
        }
        [Encoder setTexture:Texture->GetTexture() atIndex:GDI_COMPUTE_TEXTURE_BINDING];
        [Encoder setSamplerState:S->Pipelines->GetSampler(Texture->GetBorder(), Texture->GetFilter(), Texture->HasMips())
                         atIndex:MTLBindings::Sampler(GDI_COMPUTE_TEXTURE_BINDING)];
        [Encoder dispatchThreadgroups:MTLSizeMake(InX, InY, InZ) threadsPerThreadgroup:CS->GetWorkGroupSize()];
        [Encoder endEncoding];

        if(bTransient)
        {
            [Cmd commit];
            [Cmd waitUntilCompleted];
        }
    }
    return true;
}

bool MetalDynamicGDI::UpdateBuffer(GDIBuffer* InBuffer, const uint32_t InOffset, const void* InData, const uint32_t InBytes)
{
    auto* Buffer = static_cast<MTLBufferImpl*>(InBuffer);
    if(!SupportsCompute() || !Buffer || !Buffer->IsValid() || !InData || InBytes == 0) return false;
    @autoreleasepool
    {
        bool bTransient = false;
        id<MTLCommandBuffer> Cmd = S->CommandBufferForWork(bTransient);
        if(!Cmd) return false;
        const MTLUploadRing::Allocation Staging = S->Upload(InData, InBytes);
        if(!Staging.Buffer) return false;
        // In frame order: a blit between the passes before and after.
        id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
        [Blit copyFromBuffer:Staging.Buffer sourceOffset:Staging.Offset toBuffer:Buffer->GetBuffer() destinationOffset:InOffset size:InBytes];
        [Blit endEncoding];
        if(bTransient)
        {
            [Cmd commit];
            [Cmd waitUntilCompleted];
        }
    }
    return true;
}

bool MetalDynamicGDI::ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes)
{
    auto* Buffer = static_cast<MTLBufferImpl*>(InBuffer);
    if(!Buffer || !Buffer->IsValid()) return false;
    FlushAndWait(true);   // what the open block recorded runs first
    const uint8_t* Data = static_cast<const uint8_t*>(Buffer->GetBuffer().contents);
    OutBytes.assign(Data, Data + Buffer->GetSize());
    return true;
}

// ===========================================================================
// Present
// ===========================================================================

void MetalDynamicGDI::PresentToWindow(WindowHandle InWindow)
{
    PresentRenderTargetToWindow(InWindow, GActiveTarget);
}

void MetalDynamicGDI::PresentRenderTargetToWindow(WindowHandle InWindow, GDIRenderTarget* InSource)
{
    auto* Source = dynamic_cast<MTLRenderTargetImpl*>(InSource);
    if(!bInitialized || !InWindow.IsValid() || !Source || S->bResizing.load()) return;

    MTLSwapchainImpl* Swapchain = nullptr;
    for(const auto& Entry : S->Swapchains)
    {
        if(Entry.first == InWindow.Native) Swapchain = Entry.second;
    }
    if(!Swapchain)
    {
        Swapchain = new MTLSwapchainImpl();
        if(!Swapchain->Initialize(S->Device, S->Queue, InWindow))
        {
            delete Swapchain;
            return;
        }
        S->Swapchains.emplace_back(InWindow.Native, Swapchain);
    }
    Swapchain->Present(Source);
}

void MetalDynamicGDI::ReleaseWindow(WindowHandle InWindow)
{
    for(auto It = S->Swapchains.begin(); It != S->Swapchains.end(); ++It)
    {
        if(It->first == InWindow.Native)
        {
            WaitForCommitted();
            delete It->second;
            S->Swapchains.erase(It);
            return;
        }
    }
}

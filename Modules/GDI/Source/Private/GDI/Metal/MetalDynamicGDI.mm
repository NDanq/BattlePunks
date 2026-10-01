#include "Public/GDI/Metal/MetalDynamicGDI.h"
#include <algorithm>
#include "Public/GDI/Metal/MTLDevice.h"
#include "Public/GDI/Metal/MTLSync.h"
#include "Public/GDI/Metal/MTLShader.h"
#include "Public/GDI/Metal/MTLTexture.h"
#include "Public/GDI/Metal/MTLMesh.h"
#include "Public/GDI/Metal/MTLRenderTarget.h"
#include "Public/GDI/Metal/MTLCommandManager.h"
#include "Public/GDI/Metal/MTLSwapchain.h"
#include "Private/GDI/Metal/MTLPipelineCache.h"
#include "Private/GDI/Metal/MTLUtil.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDIView.h"


#include <cstring>
#include <cstdlib>
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
#include <unordered_map>
#include "Public/Core/CoreString.h"

extern GDISync* GSync;

// ===========================================================================
//  MetalGDIContext
// ===========================================================================

MetalGDIContext::MetalGDIContext()
{
    ContextType = GDIContext::MetalContext;
}

MetalGDIContext::~MetalGDIContext()
{
    // Base ~GDIContext deletes the QOffscreenSurface.
}

// ===========================================================================
//  Life-cycle
// ===========================================================================

MetalDynamicGDI::MetalDynamicGDI(EngineCore::WindowHandle InWindow)
    : GDIDynamicDriver(InWindow)
{
}

MetalDynamicGDI::~MetalDynamicGDI()
{
    Destroy();
}

void MetalDynamicGDI::AssignContext(GDIContext* InContext)
{
    Context = InContext;
}

void MetalDynamicGDI::Allocate()
{
    if(bInitialized) return;

    SetThreadID(EngineCore::appThreadID());

    DeviceObj = new MTLDevice();
    if(!DeviceObj->Initialize())
    {
        GDI_LOG("[MetalDynamicGDI] Metal device initialization failed.")
        delete DeviceObj;
        DeviceObj = nullptr;
        return;
    }

    CommandManager = new MTLCommandManager();
    if(!CommandManager->Initialize(DeviceObj))
    {
        GDI_LOG("[MetalDynamicGDI] CommandManager init failed.")
        return;
    }

    PipelineCache = new MTLPipelineCache();
    if(!PipelineCache->Initialize(DeviceObj))
    {
        GDI_LOG("[MetalDynamicGDI] PipelineCache init failed.")
        return;
    }

    // The layer is adopted lazily on the first PresentToWindow (UI thread) -- the
    // same deferral the Vulkan backend applies to surface creation.
    Swapchain = new MTLSwapchain();

    // Bind the global GDISync to the device so the renderer can fence on it.
    if(auto* Sync = dynamic_cast<MTLSync*>(GSync))
    {
        Sync->Bind(DeviceObj);
    }

    // 1x1 transparent fallback for shaders that declare a sampler but were handed
    // no texture -- without it the draw would sample an unbound slot.
    {
        EngineCore::Image NullImg(1, 1, EngineCore::EImageFormat::RGBA8);
        // Zero-initialised by construction, i.e. transparent black -- the same
        // thing QImage::fill(Qt::transparent) produced.
        NullTexture = new MTLTextureResource(DeviceObj, NullImg, GDI_TB_Repeat, GDI_TF_Nearest);
    }

    CurrentStateData = new MTLCurrentStateData();

    bInitialized = true;
    GDI_LOG("[MetalDynamicGDI] Allocate complete.")
}

void MetalDynamicGDI::Destroy()
{
    /* BEFORE the early-out, and before the device goes: cached shaders and
     * textures hold a raw device pointer -- see ~GDIDynamicDriver. */
    ReleaseCachedResources();

    if(!bInitialized) return;

    WaitIdle();

    if(GSync)
    {
        GSync->DestroySync();
    }

    if(CurrentStateData)
    {
        EndActiveEncoder();
        delete CurrentStateData;
        CurrentStateData = nullptr;
    }

    if(ScreenMesh)  { ScreenMesh->Release();  delete ScreenMesh;  ScreenMesh  = nullptr; }
    if(NullTexture) { NullTexture->Release(); delete NullTexture; NullTexture = nullptr; }

    if(Swapchain)      { Swapchain->Shutdown();      delete Swapchain;      Swapchain      = nullptr; }
    if(PipelineCache)  { PipelineCache->Shutdown();  delete PipelineCache;  PipelineCache  = nullptr; }
    if(CommandManager) { CommandManager->Shutdown(); delete CommandManager; CommandManager = nullptr; }

    if(DeviceObj)
    {
        DeviceObj->Shutdown();
        delete DeviceObj;
        DeviceObj = nullptr;
    }

    bInitialized = false;
}

void MetalDynamicGDI::SwapBuffers()
{
    // No-op on the render thread. Present is performed by the UI thread via
    // PresentToWindow, exactly as in the Vulkan backend.
}

void MetalDynamicGDI::Flush()
{
    // No-op: EndRendering already commits the frame.
}

void MetalDynamicGDI::WaitIdle()
{
    if(CommandManager) CommandManager->WaitIdle();
}

// ===========================================================================
//  Present
// ===========================================================================

void MetalDynamicGDI::PresentToWindow(EngineCore::WindowHandle InWindow)
{
    if(!bInitialized || !Swapchain || !InWindow) return;

    // Skip while render targets are being resized -- they're mid-recreation on the
    // render thread. EndRendering clears this once a full frame has been rendered
    // into the new targets.
    if(bResizing.load()) return;

    // Nothing committed yet: there is no frame to show.
    if(FramesCommitted.load() == 0) return;

    if(!Swapchain->IsValid())
    {
        // Initialize() logs and latches unrecoverable failures itself, so this can
        // safely run every frame: a not-yet-created platform window retries, a
        // misconfigured one gives up after the first attempt.
        if(Swapchain->HasFailed() || !Swapchain->Initialize(DeviceObj, InWindow))
        {
            return;
        }
    }

    // The renderer always rebinds Main right before EndRendering, so the last
    // bound RT is the main RT. Reading it from the UI thread is a benign stale
    // read at worst -- and unlike Vulkan there is no layout to corrupt, because
    // ordering against the render thread comes from both submitting to the same
    // MTLCommandQueue (see the class comment).
    MTLRenderTarget* MainRT = CurrentStateData ? CurrentStateData->ActiveRenderTarget : nullptr;
    if(!MainRT) return;

    Swapchain->PresentFrame(MainRT);
}

// ===========================================================================
//  Render targets
// ===========================================================================

GDIRenderTarget* MetalDynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    if(!DeviceObj) return nullptr;
    auto* RT = new MTLRenderTarget(DeviceObj, Name, InWidth, InHeight);
    RegisterRenderTarget(RT);
    return RT;
}

void MetalDynamicGDI::SetRenderTarget(GDIRenderTarget* InRenderTarget)
{
    auto* MTLRT = dynamic_cast<MTLRenderTarget*>(InRenderTarget);
    if(!MTLRT || !CurrentStateData) return;
    if(CurrentStateData->CurrentCmd == nullptr) return;

    // Everything the Vulkan backend does here -- sweeping every live RT back to
    // SHADER_READ_ONLY, barriering the new one into COLOR_ATTACHMENT -- has no
    // Metal counterpart: there are no layouts, and Metal hazard-tracks reads and
    // writes of these textures across encoders itself. Binding a target is simply
    // closing the current encoder and opening one on the new attachment.
    EndActiveEncoder();

    // A target that has never been rendered into holds uninitialized private
    // memory, so its first pass must clear rather than load.
    BeginEncoderForRT(MTLRT, /*bClear*/ !MTLRT->HasContent(), EngineCore::Color(0, 0, 0, 0));
}

void MetalDynamicGDI::ReleaseRenderTarget(GDIRenderTarget* InRenderTarget)
{
    if(InRenderTarget) InRenderTarget->Release();
}

void MetalDynamicGDI::RegisterRenderTarget(MTLRenderTarget* InRT)
{
    LiveRenderTargets.insert(InRT);
}

void MetalDynamicGDI::UnregisterRenderTarget(MTLRenderTarget* InRT)
{
    LiveRenderTargets.remove(InRT);

    // Never leave the current state pointing at a destroyed target -- the UI-thread
    // present reads ActiveRenderTarget.
    if(CurrentStateData && CurrentStateData->ActiveRenderTarget == InRT)
    {
        EndActiveEncoder();
        CurrentStateData->ActiveRenderTarget = nullptr;
    }
}

// ===========================================================================
//  Mid-frame flush / resolve
// ===========================================================================

void MetalDynamicGDI::FlushForReadback(bool bResumeActiveRegion)
{
    if(!CommandManager || !CurrentStateData || !DeviceObj) return;
    if(CurrentStateData->CurrentCmd == nullptr) return;   // no frame recording

    MTLRenderTarget*  RT         = CurrentStateData->ActiveRenderTarget;
    const bool        bWasActive = CurrentStateData->bRenderingActive;

    EndActiveEncoder();
    CommandManager->EndFrame();
    CommandManager->Commit();

    // Block until the flushed work is complete -- the readback (or the resize that
    // is about to destroy this target) depends on it having actually executed.
    CommandManager->WaitIdle();

    MTLCommandBufferRef NewCmd = CommandManager->BeginFrame();
    CurrentStateData->CurrentCmd         = NewCmd;
    CurrentStateData->CurrentEncoder     = nullptr;
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->bRenderingActive   = false;
    CurrentStateData->DebugLabelDepth    = 0;

    // Re-open the pass on the RT that was active so the frame continues exactly as
    // before the flush (loadAction Load preserves its contents). Skipped for a
    // resize, where the target is about to be destroyed.
    if(bResumeActiveRegion && bWasActive && RT && NewCmd != nullptr)
    {
        BeginEncoderForRT(RT, /*bClear*/ false, EngineCore::Color(0, 0, 0, 0));
    }
}

void MetalDynamicGDI::RecordResolve(MTLRenderTarget* Src, MTLRenderTarget* Dst)
{
    if(!Src || !Dst || !CurrentStateData || !DeviceObj) return;
    // Self-resolve is a no-op: the OpenGL path self-blits an FBO onto itself, but a
    // same-texture full-extent copy is invalid here (as it is in Vulkan).
    if(Src == Dst) return;
    if(CurrentStateData->CurrentCmd == nullptr) return;
    if(!Src->IsValid() || !Dst->IsValid())      return;
    if(!Src->HasContent())                      return;

    // A copy cannot be recorded inside a render encoder.
    EndActiveEncoder();
    CurrentStateData->ActiveRenderTarget = nullptr;

    const NSUInteger W = std::min(Src->GetWidth(),  Dst->GetWidth());
    const NSUInteger H = std::min(Src->GetHeight(), Dst->GetHeight());
    if(W == 0 || H == 0) return;

    id<MTLBlitCommandEncoder> Blit =
        [(id<MTLCommandBuffer>)CurrentStateData->CurrentCmd blitCommandEncoder];
    [Blit copyFromTexture:(id<MTLTexture>)Src->GetTexture()
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(W, H, 1)
                toTexture:(id<MTLTexture>)Dst->GetTexture()
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [Blit endEncoding];

    Dst->SetHasContent(true);
}

// ===========================================================================
//  Shader resources
// ===========================================================================

GDIShaderResource* MetalDynamicGDI::CreateShaderResource(EShaderType InType, const std::string& InSrcPath)
{
    if(!DeviceObj) return nullptr;
    const std::string MslPath = ProcessShaderFromSource(InSrcPath, ERenderPlatform::ERP_Metal);
    return new MTLShaderResource(DeviceObj, InType, MslPath);
}

void MetalDynamicGDI::SetBoundShader(GDIShader* In_VS, GDIShader* In_PS)
{
    BoundVertexShader = In_VS;
    BoundPixelShader  = In_PS;
    if(!CurrentStateData) return;

    auto ExtractFunction = [](GDIShader* Shader) -> MTLFunctionRef
    {
        if(!Shader || !Shader->Resource) return nullptr;
        return static_cast<MTLShaderResource*>(Shader->Resource)->GetFunction();
    };
    CurrentStateData->BoundVSFunction = ExtractFunction(In_VS);
    CurrentStateData->BoundFSFunction = ExtractFunction(In_PS);
}

// ===========================================================================
//  Mesh creation
// ===========================================================================

GDIMesh* MetalDynamicGDI::CreateQuadMesh(const float* VertData, const float* TexData,
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

    auto* Mesh = new MTLMesh(DeviceObj, CurrentStateData);
    Mesh->SetRenderData(RenderData);
    Mesh->SetSupportsInstanced(bIsInstanced);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* MetalDynamicGDI::CreateMesh(const GDIMeshDesc& /*InDesc*/)
{
    static bool bWarned = false;
    if(!bWarned)
    {
        GDI_LOG("[MetalDynamicGDI] indexed meshes are not supported on Metal yet; meshes draw as quads")
        bWarned = true;
    }
    return nullptr;
}

GDIMesh* MetalDynamicGDI::CreateQuadMesh(const float& InWidth, const float& InHeight,
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

// ===========================================================================
//  Texture creation
// ===========================================================================

GDITextureResource* MetalDynamicGDI::CreateTextureResource(
        const EngineCore::Image& InImage,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new MTLTextureResource(DeviceObj, InImage, InBorder, InFilter);
}

GDITextureResource* MetalDynamicGDI::CreateTextureResource(
        GDITexture2D* InParent,
        const EngineCore::Image& InImage,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new MTLTextureResource(DeviceObj, InParent, InImage, InBorder, InFilter);
}

GDITextureResource* MetalDynamicGDI::CreateTextureResource(
        GDITexture2D* InParent,
        const EGDITextureBorder InBorder,
        const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new MTLTextureResource(DeviceObj, InParent, InBorder, InFilter);
}

// ===========================================================================
//  Uniform packing
//
//  The byte layouts below are NOT re-derived for Metal: the MSL structs are
//  transpiled from the very same SPIR-V the Vulkan backend consumes, so
//  spirv-cross emits std140-compatible layouts and the offsets are identical to
//  VulkanDynamicGDI's. Keep the two in sync.
// ===========================================================================

/** Pack one scalar into a raw byte buffer at Offset; returns the new offset. */
static uint32_t PackScalar(const MTLUniformValue& V, uint8_t* Dst, uint32_t Offset, uint32_t MaxBytes)
{
    uint32_t    Size = 0;
    const void* Ptr  = nullptr;

    float IntAsFloat = 0.f;   // temporary for the INT conversion

    switch(V.Tag)
    {
        case MTLUniformValue::INT:
            IntAsFloat = static_cast<float>(V.Data.i);
            Size = 4;  Ptr = &IntAsFloat;   break;
        case MTLUniformValue::FLOAT:
            Size = 4;  Ptr = &V.Data.f;     break;
        case MTLUniformValue::VEC2:
            Size = 8;  Ptr = V.Data.v;      break;
        case MTLUniformValue::VEC3:
            Size = 12; Ptr = V.Data.v;      break;
        case MTLUniformValue::VEC4:
            Size = 16; Ptr = V.Data.v;      break;
        case MTLUniformValue::MAT:
            Size = 64; Ptr = V.Data.v;      break;
        default:
            return Offset;
    }
    if(Offset + Size > MaxBytes) return Offset;
    memcpy(Dst + Offset, Ptr, Size);
    return Offset + Size;
}

/**
 * Pack the ViewParameters struct into a 160-byte block:
 *   offset   0: mat4  Projection   (64)
 *   offset  64: mat4  View         (64)
 *   offset 128: vec3  MouseCoords  (12 + 4 pad)
 *   offset 144: vec4  Resolution   (16)
 *
 * Two fix-ups are applied to the projection, both of which exist because the
 * camera builds a GL projection:
 *
 *  1. Depth range. GL clips z to [-1,1]; Metal (like Vulkan) clips to [0,1], so
 *     geometry would be discarded outright. Remap z' = 0.5*z + 0.5*w.
 *
 *  2. Y direction. This is where Metal and Vulkan genuinely differ. Vulkan's NDC
 *     +Y points at framebuffer row 0, so leaving the GL projection alone already
 *     lands GL-ordered rows in memory. Metal's NDC +Y points at the TOP of the
 *     attachment (row 0), i.e. the same direction as GL on screen but the
 *     opposite of GL in memory -- so without a flip here, every render-to-texture
 *     -> sample-texture hop (layer compositing, stroke buffers) would invert the
 *     image. Negating row 1 makes Metal's render targets byte-identical to the
 *     Vulkan and OpenGL ones (row 0 = visual bottom), which is what lets
 *     ToImage, GetColorFromSamplePoint and the shared shaders behave the same on
 *     all three backends.
 *
 * Floats are column-major (D[Col*4 + Row]).
 */
static void PackViewParamsUBO(const std::unordered_map<std::string, MTLUniformValue>& S, void* Dst)
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

    for(int Col = 0; Col < 4; ++Col)
    {
        const float Z = D[Col * 4 + 2];
        const float W = D[Col * 4 + 3];
        D[Col * 4 + 2] = 0.5f * Z + 0.5f * W;   // GL depth range -> Metal
        D[Col * 4 + 1] = -D[Col * 4 + 1];       // GL row order   -> Metal
    }
}

/**
 * Pack the Transform struct into an 80-byte block:
 *   offset  0: vec3  PositionWorldSpace (12 + 4 pad)
 *   offset 16: mat4  Model              (64)
 */
static void PackTransformUBO(const std::unordered_map<std::string, MTLUniformValue>& S, void* Dst)
{
    float* D = static_cast<float*>(Dst);
    memset(D, 0, 80);

    auto Write = [&](const std::string& Name, float* Out, int Count)
    {
        auto It = S.find(Name);
        if(It != S.end())
            memcpy(Out, It->second.Data.v, static_cast<size_t>(Count) * sizeof(float));
    };

    Write("Transform.PositionWorldSpace", D + 0, 3);   // offset  0, 12 bytes (+ 4 pad)
    Write("Transform.Model",              D + 4, 16);  // offset 16, 64 bytes
}

/**
 * Pack every non-core scalar from S into Dst.
 * QHash iteration is unordered, which is safe only because every custom UBO in
 * the shader set has exactly ONE member (Basic_Block, LayersBlit_Block,
 * UberBrush_Block). A second member would need an explicit offset table -- do not
 * rely on iteration order if one is ever added.
 */
static void PackCustomUBO(const std::unordered_map<std::string, MTLUniformValue>& S, void* Dst, uint32_t MaxBytes)
{
    uint8_t* D = static_cast<uint8_t*>(Dst);
    memset(D, 0, MaxBytes);
    uint32_t Offset = 0;
    for(auto It = S.begin(); It != S.end(); ++It)
    {
        const std::string& N = It->first;
        if(EngineCore::StartsWith(N, "ViewParameters.") || EngineCore::StartsWith(N, "Transform.")) continue;
        Offset = PackScalar(It->second, D, Offset, MaxBytes);
    }
}

// ===========================================================================
//  Core draw path
// ===========================================================================

bool MetalDynamicGDI::BindDrawState(GDIMesh* InMesh, bool bIsInstanced)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive) return false;
    if(!InMesh || !PipelineCache || !DeviceObj)                  return false;

    id<MTLRenderCommandEncoder> Enc = (id<MTLRenderCommandEncoder>)CurrentStateData->CurrentEncoder;
    if(Enc == nil) return false;

    if(CurrentStateData->BoundVSFunction == nullptr ||
       CurrentStateData->BoundFSFunction == nullptr) return false;

    // ----------------------------------------------------------------
    // 1. Gather shader resources
    // ----------------------------------------------------------------
    MTLShaderResource* PSRes = nullptr;
    MTLShaderResource* VSRes = nullptr;
    if(BoundPixelShader  && BoundPixelShader->Resource)
        PSRes = static_cast<MTLShaderResource*>(BoundPixelShader->Resource);
    if(BoundVertexShader && BoundVertexShader->Resource)
        VSRes = static_cast<MTLShaderResource*>(BoundVertexShader->Resource);

    static const std::unordered_map<std::string, MTLUniformValue> EmptyScalars;
    // Prefer the PS for the core blocks -- both stages receive identical core data.
    const std::unordered_map<std::string, MTLUniformValue>& CoreScalars =
        PSRes ? PSRes->GetScalars() : (VSRes ? VSRes->GetScalars() : EmptyScalars);

    // ----------------------------------------------------------------
    // 2. Work out which slots this shader pair actually uses
    //
    // The binding indices are fixed by the SPIR-V (preserved through
    // --msl-decoration-binding): a fragment shader with a custom UBO puts it at
    // buffer(2) and pushes its sampler to 2->3. This is the same decision the
    // Vulkan backend expresses as an EDescriptorLayoutGroup; Metal needs no
    // pre-declared layout, just the right index.
    // ----------------------------------------------------------------
    bool bHasCustomUBO     = false;
    bool bHasCustomSampler = false;
    if(PSRes)
    {
        for(auto It = PSRes->GetScalars().begin(); It != PSRes->GetScalars().end(); ++It)
        {
            const std::string& N = It->first;
            if(!EngineCore::StartsWith(N, "ViewParameters.") && !EngineCore::StartsWith(N, "Transform."))
            {
                bHasCustomUBO = true;
                break;
            }
        }
        bHasCustomSampler = !PSRes->GetTextures().empty();
    }
    const NSUInteger SamplerIndex = bHasCustomUBO ? 3u : 2u;

    // ----------------------------------------------------------------
    // 3. Pipeline state
    // ----------------------------------------------------------------
    MTLPipelineCache::PipelineKey Key;
    Key.VertexFunction   = CurrentStateData->BoundVSFunction;
    Key.FragmentFunction = CurrentStateData->BoundFSFunction;
    Key.VertexLayoutID   = bIsInstanced ? MVL_Instanced : MVL_Standard;

    const IBlendStateDesc& Blend = CurrentStateData->BlendState;
    Key.BlendEnable   = Blend.bBlend;
    Key.SrcColorBlend = MTLUtil::DecodeBlendFactor(Blend.Source);
    Key.DstColorBlend = MTLUtil::DecodeBlendFactor(Blend.Dest);
    // glBlendFunc applies the same factors to colour AND alpha; without mirroring
    // them here the alpha channel would blend with the pipeline default (ONE/ZERO)
    // and every draw would REPLACE destination alpha instead of accumulating it.
    Key.SrcAlphaBlend = Key.SrcColorBlend;
    Key.DstAlphaBlend = Key.DstColorBlend;
    Key.ColorBlendOp  = MTLUtil::DecodeBlendOp(Blend.Op);
    Key.AlphaBlendOp  = MTLUtil::DecodeBlendOp(Blend.AlphaOp);
    Key.ColorWriteMask = MTLUtil::DecodeColorMask(CurrentStateData->ColorMask);

    if(MTLRenderTarget* RT = CurrentStateData->ActiveRenderTarget)
    {
        Key.RTFormat = [(id<MTLTexture>)RT->GetTexture() pixelFormat];
    }

    id<MTLRenderPipelineState> Pipeline = PipelineCache->GetOrCreatePipeline(Key);
    if(Pipeline == nil) return false;
    [Enc setRenderPipelineState:Pipeline];

    // ----------------------------------------------------------------
    // 4. Depth / stencil state
    // ----------------------------------------------------------------
    MTLPipelineCache::DepthStencilKey DSKey;
    const IDepthStateDesc&   Depth   = CurrentStateData->DepthState;
    const IStencilStateDesc& Stencil = CurrentStateData->StencilState;
    DSKey.DepthTest        = Depth.bDepthTest;
    DSKey.DepthWrite       = Depth.bDepthMask;
    DSKey.DepthCompare     = MTLUtil::DecodeCompareFunction(Depth.DepthMode);
    DSKey.StencilTest      = Stencil.bStencilTest;
    DSKey.StencilCompare   = MTLUtil::DecodeCompareFunction(Stencil.StencilMode);
    DSKey.StencilFail      = MTLUtil::DecodeStencilOp(Stencil.SrcFail);
    DSKey.DepthFail        = MTLUtil::DecodeStencilOp(Stencil.DestFail);
    DSKey.DepthStencilPass = MTLUtil::DecodeStencilOp(Stencil.SDPass);
    DSKey.StencilMask      = Stencil.Mask;

    if(id<MTLDepthStencilState> DSState = PipelineCache->GetOrCreateDepthStencil(DSKey))
    {
        [Enc setDepthStencilState:DSState];
    }
    if(Stencil.bStencilTest)
    {
        [Enc setStencilReferenceValue:(uint32_t)Stencil.Reference];
    }

    // A fresh encoder carries no viewport/scissor, and encoders restart often.
    ApplyViewportToEncoder();

    // ----------------------------------------------------------------
    // 5. Uniform blocks
    //
    // setVertexBytes/setFragmentBytes copies straight into the command buffer, so
    // the Vulkan backend's descriptor sets and UBO ring allocator are unnecessary
    // -- every block here is far below the 4 KB inline limit.
    // ----------------------------------------------------------------
    alignas(16) uint8_t VPBlock[160];
    alignas(16) uint8_t TransformBlock[80];
    PackViewParamsUBO(CoreScalars, VPBlock);
    PackTransformUBO(CoreScalars, TransformBlock);

    [Enc setVertexBytes:VPBlock        length:sizeof(VPBlock)        atIndex:MTLBindings::ViewParamsBuffer];
    [Enc setVertexBytes:TransformBlock length:sizeof(TransformBlock) atIndex:MTLBindings::TransformBuffer];
    [Enc setFragmentBytes:VPBlock        length:sizeof(VPBlock)        atIndex:MTLBindings::ViewParamsBuffer];
    [Enc setFragmentBytes:TransformBlock length:sizeof(TransformBlock) atIndex:MTLBindings::TransformBuffer];

    if(bHasCustomUBO && PSRes)
    {
        // Always send the full block rather than just the packed bytes: Metal
        // validates that the bound buffer covers everything the shader's struct
        // declares, and std140 padding makes the struct larger than the payload
        // (e.g. Basic_Block's vec3 occupies 16 bytes).
        alignas(16) uint8_t CustomBlock[256];
        PackCustomUBO(PSRes->GetScalars(), CustomBlock, sizeof(CustomBlock));
        [Enc setVertexBytes:CustomBlock   length:sizeof(CustomBlock) atIndex:MTLBindings::CustomUBOBuffer];
        [Enc setFragmentBytes:CustomBlock length:sizeof(CustomBlock) atIndex:MTLBindings::CustomUBOBuffer];
    }

    // ----------------------------------------------------------------
    // 6. Texture + sampler
    // ----------------------------------------------------------------
    if(bHasCustomSampler)
    {
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

        auto* MTLTex = dynamic_cast<MTLTextureResource*>(TexRes);
        if(!MTLTex) MTLTex = NullTexture;
        if(!MTLTex) return false;   // no fallback available

#ifdef QT_DEBUG
        // A draw that expected a real texture but got the 1x1 transparent fallback
        // renders nothing (the brush/blit shaders discard on alpha 0) -- log it.
        if(MTLTex == NullTexture)
        {
            static int NullTexWarnCount = 0;
            if(NullTexWarnCount < 20)
            {
                ++NullTexWarnCount;
                GDI_LOG(std::string("[MetalDynamicGDI] Draw uses NullTexture fallback (PS textures=%1)")
                     .arg(PSRes ? PSRes->GetTextures().size() : -1))
            }
        }
#endif

        [Enc setFragmentTexture:(id<MTLTexture>)MTLTex->GetTexture() atIndex:SamplerIndex];
        [Enc setFragmentSamplerState:(id<MTLSamplerState>)MTLTex->GetSampler() atIndex:SamplerIndex];
    }

    return true;
}

void MetalDynamicGDI::DrawObject(GDIViewObject* InObject, GDIView* InView)
{
    if(!InObject || !InObject->Mesh) return;

    if(BoundVertexShader)
        BoundVertexShader->UpdateCoreUniforms(InView, InObject->Transform);
    if(BoundPixelShader)
        BoundPixelShader->UpdateCoreUniforms(InView, InObject->Transform);

    InObject->Mesh->Draw();
}

void MetalDynamicGDI::DrawScreenQuadPrimitive()
{
    if(!DeviceObj || !CurrentStateData) return;

    // Flush the bound pixel shader's user uniforms into its resource, exactly like
    // the OpenGL and Vulkan paths. Without this the screen-blit texture set via
    // Screen_PS::SetScreenBlitTexture never reaches the shader resource, so
    // BindDrawState finds no sampler and DrawTexture (the flood-fill / restore
    // write-back path) draws nothing.
    if(BoundPixelShader)
        BoundPixelShader->UpdateUserDefinedUniforms();

    if(!ScreenMesh)
    {
        // 6 verts covering the full NDC square.
        //
        // The UVs are V-flipped relative to the OpenGL and Vulkan screen quads.
        // Screen.vsh bypasses the projection (gl_Position = InPosition), so the
        // Y-flip that PackViewParamsUBO applies to ordinary geometry does not
        // apply here -- and Metal's NDC +Y is attachment row 0 while the source
        // texture's row 0 is its visual bottom. Pairing NDC +1 with v=0 is what
        // keeps this an identity copy instead of a vertical mirror.
        const float Verts[] = {
            -1.f, -1.f, 0.f,
            -1.f,  1.f, 0.f,
             1.f,  1.f, 0.f,
             1.f,  1.f, 0.f,
             1.f, -1.f, 0.f,
            -1.f, -1.f, 0.f
        };
        const float UVs[] = {
            0.f, 1.f,
            0.f, 0.f,
            1.f, 0.f,
            1.f, 0.f,
            1.f, 1.f,
            0.f, 1.f
        };
        ScreenMesh = CreateQuadMesh(Verts, UVs, 18, 12, false);
    }

    if(!ScreenMesh) return;
    ScreenMesh->Draw();
}

// ===========================================================================
//  Misc
// ===========================================================================

bool MetalDynamicGDI::CheckError(const std::string&)
{
    // Metal reports failures per command buffer rather than through a global error
    // flag; MTLCommandManager surfaces those. Nothing to poll here.
    return false;
}

EngineCore::Color MetalDynamicGDI::GetColorFromSamplePoint(const EngineCore::Point& InPoint, const EngineCore::Size& InSize)
{
    if(!DeviceObj || !CurrentStateData) return EngineCore::Color();

    // Make sure the just-composited frame has actually executed before sampling it.
    FlushForReadback();

    MTLRenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT || !RT->IsValid() || !RT->HasContent()) return EngineCore::Color();

    // RT memory is GL-oriented (row 0 = visual bottom -- see PackViewParamsUBO), so
    // flip Y from Qt screen space exactly like the OpenGL path's
    // glReadPixels(x, height - y).
    const int X = InPoint.X;
    const int Y = InSize.Height - InPoint.Y;
    if(X < 0 || Y < 0 || X >= int(RT->GetWidth()) || Y >= int(RT->GetHeight()))
        return EngineCore::Color();

    // 256-byte row stride: buffer<->texture blits have per-device row alignment
    // requirements, and 256 satisfies every one of them for a single-row copy.
    static constexpr NSUInteger kRowBytes = 256;
    id<MTLBuffer> Readback = [DeviceObj->GetDevice() newBufferWithLength:kRowBytes
                                                                 options:MTLResourceStorageModeShared];
    if(Readback == nil) return EngineCore::Color();

    id<MTLCommandBuffer>      Cmd  = [DeviceObj->GetGraphicsQueue() commandBuffer];
    id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
    [Blit copyFromTexture:(id<MTLTexture>)RT->GetTexture()
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake((NSUInteger)X, (NSUInteger)Y, 0)
               sourceSize:MTLSizeMake(1, 1, 1)
                 toBuffer:Readback
        destinationOffset:0
   destinationBytesPerRow:kRowBytes
 destinationBytesPerImage:kRowBytes];
    [Blit endEncoding];
    [Cmd commit];
    [Cmd waitUntilCompleted];

    const auto* Px = static_cast<const uchar*>([Readback contents]);
    EngineCore::Color Out(Px[0], Px[1], Px[2]);   // MTLPixelFormatRGBA8Unorm byte order
    [Readback release];
    return Out;
}

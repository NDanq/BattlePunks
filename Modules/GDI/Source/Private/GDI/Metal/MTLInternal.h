#ifndef NIMBLE_MTLINTERNAL_H
#define NIMBLE_MTLINTERNAL_H

/*
 * The Metal backend's own types. Objective-C++ (ARC) only: every Metal object
 * the backend holds lives in one of these, out of the plain-C++ public headers
 * (MetalDynamicGDI.h says why).
 */
#ifndef __OBJC__
#error "MTLInternal.h is Objective-C++ only; include it from a .mm file."
#endif
#if !__has_feature(objc_arc)
#error "The Metal backend compiles with ARC (-fobjc-arc; GDI's CMakeLists.txt)."
#endif

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIBuffer.h"
#include "Public/GDI/GDIMesh.h"
#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDITexture.h"
#include "Public/GDI/Metal/MetalDynamicGDI.h"

// ===========================================================================
// Bindings
// ===========================================================================
/*
 * Metal indices, per stage. Blocks and textures keep their SPIR-V binding
 * (--msl-decoration-binding): buffer(0) the view, (1) the transform, (2) the
 * custom or material block, (3) the scene, (20)/(21) the skin palettes, (22)
 * the lights; texture(2)/(3) a non-material draw's texture, texture(4 + slot)
 * the material textures. A sampler is its binding modulo 16 (Metal's limit is
 * 16 a stage; cook_material_graphs.py remaps them the same way).
 *
 * Vertex streams take buffers 10..18 -- the D3D12 backend's input slot plus
 * 10 -- which no vertex shader uses for a block (the vertex stage reads 0..3
 * and 20..22). Compute: params at 0, storage at 1..6, the texture at 8.
 */
namespace MTLBindings
{
    constexpr NSUInteger View      = 0;
    constexpr NSUInteger Transform = 1;
    constexpr NSUInteger Custom    = 2;   // GDI_MATERIAL_BLOCK_BINDING for a material draw
    constexpr NSUInteger Scene     = 3;
    constexpr NSUInteger Skin      = 20;
    constexpr NSUInteger PrevSkin  = 21;
    constexpr NSUInteger Light     = 22;

    constexpr NSUInteger VertexStreamBase = 10;
    enum EStream : NSUInteger
    {
        Position = 0, TexCoord = 1, Normal = 2, Color = 3, UV1 = 4, Instance = 5, Joints = 6, Weights = 7, Tangent = 8,
        // The old instanced quads' per-instance streams (Basic_Instanced.vsh).
        QuadTransform = 2, QuadOpacity = 3, QuadColor = 4,
    };
    inline NSUInteger Stream(const NSUInteger InStream) { return VertexStreamBase + InStream; }

    inline NSUInteger Sampler(const NSUInteger InBinding) { return InBinding % 16; }
}

/* The vertex layouts, as the D3D12 backend numbers them. */
enum EMTLVertexLayout : uint32_t
{
    MVL_Quad          = 0,   // position, texcoord
    MVL_QuadInstanced = 1,   // + per-instance transform, opacity, colour
    MVL_Mesh          = 2,   // + normal, colour, UV1, tangent
    MVL_MeshInstanced = 3,   // + an instance buffer (GDISetInstanceBuffer)
    MVL_MeshSkinned   = 4,   // + bone indices and weights
};

namespace MTLUtil
{
    MTLBlendFactor      BlendFactor(EBlendMode InMode);
    MTLBlendOperation   BlendOp(EBlendOp InOp);
    MTLColorWriteMask   ColorMask(EColorMask InMask);
    MTLCompareFunction  Compare(ECompareTestMode InMode);
    MTLPixelFormat      RenderTargetFormat(EGDIRenderTargetFormat InFormat);
    /* Whether Apple GPUs blend into InFormat (not the 32-bit float formats). */
    bool                IsBlendable(MTLPixelFormat InFormat);
    NSString*           ToNS(const std::string& InString);
    void                LogError(const std::string& InWhat, NSError* InError);
}

// ===========================================================================
// Shaders
// ===========================================================================
struct MTLUniformValue
{
    enum Tag : uint8_t { INT, FLOAT, VEC2, VEC3, VEC4, MAT } Tag = INT;
    union
    {
        int   i;
        float f;
        float v[16];
    } Data{};
};

struct MTLTextureBinding
{
    GDITextureResource* Resource = nullptr;
    uint                Index    = 0;
};

class MTLShaderResourceImpl : public GDIShaderResource
{
public:
    MTLShaderResourceImpl(id<MTLDevice> InDevice, EShaderType InType, const std::string& InMslPath);

    void SetUniform(std::string InName, const int InValue) override;
    void SetUniform(std::string InName, const float InValue) override;
    void SetUniform(std::string InName, const Vector2 InVector) override;
    void SetUniform(std::string InName, const Vector3 InVector) override;
    void SetUniform(std::string InName, const Vector4 InVector) override;
    void SetUniform(std::string InName, const Matrix4x4 InMatrix) override;
    void SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex) override;
    void SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex) override;
    void Release(void) override;

    bool            IsValid()     const { return Function != nil; }
    id<MTLFunction> GetFunction() const { return Function; }
    /* Process-unique, never reused: the pipeline cache keys on it. */
    uint64_t        GetID()       const { return ID; }
    /* A compute shader's local_size (spirv-cross writes gl_WorkGroupSize into the MSL). */
    MTLSize         GetWorkGroupSize() const { return WorkGroupSize; }

    const std::unordered_map<std::string, MTLUniformValue>& GetScalars() const { return Scalars; }
    GDITextureResource* GetTexture0() const { return Texture0; }

    /* The core blocks of the shader's last DrawObject, packed std140 (GDICorePack). */
    struct CoreBlocks
    {
        alignas(16) float View[40]      = {};
        alignas(16) float Transform[36] = {};
        bool bView      = false;
        bool bTransform = false;
    };
    CoreBlocks&       GetCoreBlocks()       { return Core; }
    const CoreBlocks& GetCoreBlocks() const { return Core; }

private:
    id<MTLLibrary>  Library  = nil;
    id<MTLFunction> Function = nil;
    uint64_t        ID       = 0;
    MTLSize         WorkGroupSize = MTLSizeMake(1, 1, 1);

    std::unordered_map<std::string, MTLUniformValue>   Scalars;
    std::unordered_map<std::string, MTLTextureBinding> Textures;
    GDITextureResource*                                Texture0 = nullptr;
    CoreBlocks                                         Core;
};

// ===========================================================================
// Textures
// ===========================================================================
class MTLTextureResourceImpl : public GDITextureResource
{
public:
    /* RGBA8, with InCount levels (1: no mips). */
    MTLTextureResourceImpl(MetalState* InState, GDITexture2D* InParent, const EngineCore::Image* InLevels, uint32_t InCount,
                           EGDITextureBorder InBorder, EGDITextureFilter InFilter);
    /* Block-compressed, with its mips. */
    MTLTextureResourceImpl(MetalState* InState, MTLPixelFormat InFormat, const GDICompressedLevel* InLevels, uint32_t InCount,
                           EGDITextureBorder InBorder, EGDITextureFilter InFilter);
    /* No data until Update(). */
    MTLTextureResourceImpl(MetalState* InState, GDITexture2D* InParent, EGDITextureBorder InBorder, EGDITextureFilter InFilter);
    /* A render target's colour or depth, not owned. */
    MTLTextureResourceImpl(MetalState* InState, id<MTLTexture> InExternal, GDITexture2D* InParent,
                           EGDITextureBorder InBorder, EGDITextureFilter InFilter);
    ~MTLTextureResourceImpl() override;

    void              Update(const EngineCore::Image& InImage) override;
    void              Release() override;
    EngineCore::Image ToImage() const override;
    void              SetDebugName(const std::string& InName) override;

    id<MTLTexture>    GetTexture() const { return Texture; }
    EGDITextureBorder GetBorder()  const { return Border; }
    EGDITextureFilter GetFilter()  const { return Filter; }
    bool              HasMips()    const { return Texture != nil && Texture.mipmapLevelCount > 1; }
    /* Replaces an adopted texture (a render target remade by a resize). */
    void              Adopt(id<MTLTexture> InExternal) { Texture = InExternal; }

private:
    void Upload(const EngineCore::Image* InLevels, uint32_t InCount);

    MetalState*       State   = nullptr;
    id<MTLTexture>    Texture = nil;
    EGDITextureBorder Border  = GDI_TB_Repeat;
    EGDITextureFilter Filter  = GDI_TF_Nearest;
};

// ===========================================================================
// Render targets
// ===========================================================================
class MTLRenderTargetImpl : public GDIRenderTarget
{
public:
    MTLRenderTargetImpl(MetalState* InState, const std::string& InName, uint InWidth, uint InHeight, MTLPixelFormat InFormat);
    ~MTLRenderTargetImpl() override;

    void Bind() override {}
    void Release() override;
    void Resize(const uint InWidth, const uint InHeight) override;
    bool IsValid() override { return Color != nil; }
    EngineCore::Image ToImage() override;
    GDITexture2D* ToGDITexture2D() override;
    GDITexture2D* ToDepthTexture2D() override;
    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    id<MTLTexture> GetColor()  const { return Color; }
    id<MTLTexture> GetDepth()  const { return Depth; }
    MTLPixelFormat GetFormat() const { return Format; }

    /* True once after the depth buffer is made: its first pass clears it. */
    bool TakeDepthNeedsClear() { const bool bWas = bDepthNeedsClear; bDepthNeedsClear = false; return bWas; }
    /* Drawn into at least once: the present shows nothing before. */
    bool HasContent() const   { return bHasContent.load(std::memory_order_acquire); }
    void MarkContent()        { bHasContent.store(true, std::memory_order_release); }

protected:
    void Allocate() override;

private:
    void Destroy();

    MetalState*    State  = nullptr;
    std::string    Name;
    MTLPixelFormat Format = MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> Color  = nil;
    id<MTLTexture> Depth  = nil;
    bool           bDepthNeedsClear = false;
    std::atomic<bool> bHasContent{false};
    GDITexture2D*  ColorWrapper = nullptr;
    GDITexture2D*  DepthWrapper = nullptr;
};

// ===========================================================================
// Buffers
// ===========================================================================
class MTLBufferImpl : public GDIBuffer
{
public:
    MTLBufferImpl(id<MTLDevice> InDevice, uint32_t InBytes, uint8_t InUsage, const void* InInitialData);

    void Release() override;
    void SetDebugName(const std::string& InName) override;

    bool          IsValid()   const { return Buffer != nil; }
    id<MTLBuffer> GetBuffer() const { return Buffer; }

private:
    id<MTLBuffer> Buffer = nil;
};

// ===========================================================================
// Meshes
// ===========================================================================
class MTLMeshImpl : public GDIMesh
{
public:
    explicit MTLMeshImpl(MetalState* InState);
    ~MTLMeshImpl() override;

    void InitResource() override;
    void Release() override;
    void Draw() override;
    void SetDebugName(const std::string& InName) override;

    bool InitDynamic(uint32_t InMaxVertices, uint32_t InMaxIndices, uint32_t InRegions);

    bool IsValid() const { return bValid; }
    uint32_t GetVertexCount() const { return RenderData.NumVertices / 3; }

    /* The streams and the draw, on InEncoder with the pipeline already set. */
    void Encode(id<MTLRenderCommandEncoder> InEncoder, MetalState& InState);

    /* Which vertex layout a draw of this mesh uses, given an instance buffer. */
    EMTLVertexLayout GetLayout(bool bInInstanceBuffer) const;

protected:
    bool WriteDynamic(const GDIMeshDesc& InDesc, uint32_t InBaseVertex, uint32_t InBaseIndex) override;
    void AddInstancedTransformInner() override {}
    void ResetInstancedTransformInner() override {}

private:
    void ApplyDebugName();

    MetalState*   State = nullptr;
    id<MTLBuffer> Streams[9] = {};   // MTLBindings::EStream
    id<MTLBuffer> Indices    = nil;
    bool          bValid     = false;
};

// ===========================================================================
// Pipelines
// ===========================================================================
class MTLPipelineCache
{
public:
    struct Key
    {
        uint64_t          VertexID   = 0;
        uint64_t          FragmentID = 0;
        uint32_t          Layout     = MVL_Quad;
        bool              bBlend     = false;
        MTLBlendFactor    Src        = MTLBlendFactorOne;
        MTLBlendFactor    Dst        = MTLBlendFactorZero;
        MTLBlendOperation ColorOp    = MTLBlendOperationAdd;
        MTLBlendOperation AlphaOp    = MTLBlendOperationAdd;
        MTLColorWriteMask WriteMask  = MTLColorWriteMaskAll;
        uint32_t          ColorCount = 1;
        MTLPixelFormat    Colors[GDI_MAX_COLOR_TARGETS] = {};
        MTLPixelFormat    DepthFormat = MTLPixelFormatInvalid;

        bool operator==(const Key& InOther) const;
    };

    explicit MTLPipelineCache(id<MTLDevice> InDevice) : Device(InDevice) {}

    /* nil (and remembered) if the pipeline cannot be made. */
    id<MTLRenderPipelineState>  GetRender(const Key& InKey, const MTLShaderResourceImpl* InVS, const MTLShaderResourceImpl* InPS);
    id<MTLComputePipelineState> GetCompute(const MTLShaderResourceImpl* InCS);
    id<MTLDepthStencilState>    GetDepthStencil(bool bInTest, bool bInWrite, MTLCompareFunction InCompare);
    id<MTLSamplerState>         GetSampler(EGDITextureBorder InBorder, EGDITextureFilter InFilter, bool bInMips);

    static MTLVertexDescriptor* VertexDescriptor(uint32_t InLayout);
    static uint64_t             Hash(const Key& InKey);

private:
    id<MTLDevice> Device = nil;

    struct Entry { Key K; id<MTLRenderPipelineState> Pipeline; };
    std::unordered_multimap<uint64_t, Entry>                      Pipelines;
    std::unordered_map<uint64_t, id<MTLComputePipelineState>>     ComputePipelines;
    std::unordered_map<uint32_t, id<MTLDepthStencilState>>        DepthStencils;
    std::unordered_map<uint32_t, id<MTLSamplerState>>             Samplers;
    std::shared_mutex                                             Lock;
};

// ===========================================================================
// Uniform ring
// ===========================================================================
/*
 * Per-frame-slot upload memory for blocks, instance data and staging: bump
 * allocated, 256-byte aligned, rewound when its slot's frame comes round again
 * (BeginRendering waits for that frame first). A slot that runs out chains a
 * bigger chunk, kept until the slot rewinds.
 */
class MTLUploadRing
{
public:
    struct Allocation
    {
        id<MTLBuffer> Buffer = nil;
        NSUInteger    Offset = 0;
        void*         CPU    = nullptr;
    };

    void       Initialize(id<MTLDevice> InDevice, uint32_t InSlots);
    void       Rewind(uint32_t InSlot);
    Allocation Allocate(uint32_t InSlot, NSUInteger InBytes);

private:
    struct Chunk { id<MTLBuffer> Buffer; NSUInteger Used = 0; };
    struct Slot  { std::vector<Chunk> Chunks; };

    id<MTLDevice>     Device = nil;
    std::vector<Slot> Slots;
    static constexpr NSUInteger kChunkBytes = 4u << 20;
};

// ===========================================================================
// Swapchains
// ===========================================================================
@class NSView;

class MTLSwapchainImpl
{
public:
    ~MTLSwapchainImpl();
    bool Initialize(id<MTLDevice> InDevice, id<MTLCommandQueue> InQueue, EngineCore::WindowHandle InWindow);
    void Present(MTLRenderTargetImpl* InSource);

private:
    void UpdateDrawableSize();
    void FitOwnLayer();

    NSView*                    View     = nil;
    bool                       bOwnLayer = false;   // a sublayer of a view not backed by Metal (the editor's)
    id<MTLDevice>              Device   = nil;
    id<MTLCommandQueue>        Queue    = nil;
    CAMetalLayer*              Layer    = nil;
    id<MTLRenderPipelineState> Pipeline = nil;
    id<MTLSamplerState>        Sampler  = nil;
    EngineCore::WindowHandle   Window;
};

// ===========================================================================
// Occlusion queries (MTLQueries.mm)
// ===========================================================================
/*
 * A set's round is a range of its frame slot's visibility buffer, which every
 * render pass of the frame carries (MetalState::Visibility). Boolean mode: a
 * query's word is nonzero if anything passed. Read on the CPU once the frame
 * has completed.
 */
class MTLOcclusionQueriesImpl final : public GDIOcclusionQueries
{
public:
    explicit MTLOcclusionQueriesImpl(const uint32_t InCapacity) : GDIOcclusionQueries(InCapacity), Ended(InCapacity, 0), Split(InCapacity, 0) {}
    void Release() override;

    id<MTLBuffer>        Buffer;            // the slot's visibility buffer of this round
    NSUInteger           Base     = 0;      // first word
    uint32_t             Count    = 0;
    uint32_t             Slot     = 0;
    uint64_t             Serial   = 0;      // the frame it was recorded in
    int32_t              Open     = -1;
    bool                 bResolved = false;
    bool                 bPending  = false;
    std::vector<uint8_t> Ended;
    std::vector<uint8_t> Split;             // spanned two passes: answered visible
};

// ===========================================================================
// The driver's state
// ===========================================================================
struct MetalState
{
    MetalDynamicGDI*    Driver = nullptr;
    id<MTLDevice>       Device = nil;
    id<MTLCommandQueue> Queue  = nil;
    MTLPipelineCache*   Pipelines = nullptr;
    MTLUploadRing       Ring;

    // Frames in flight: BeginRendering waits on the semaphore, each frame's
    // completion signals it.
    uint32_t             FramesInFlight = 1;
    uint32_t             FrameSlot      = 0;
    dispatch_semaphore_t FrameSemaphore = nullptr;

    // The open block.
    id<MTLCommandBuffer>        Cmd     = nil;
    id<MTLRenderCommandEncoder> Encoder = nil;
    id<MTLCommandBuffer>        LastCommitted = nil;
    std::mutex                  CommitLock;   // LastCommitted, read by the UI thread's GSync wait
    int                         DebugGroupDepth = 0;

    // The bound targets (SetRenderTargets), opened as a pass at the first draw.
    MTLRenderTargetImpl* Colors[GDI_MAX_COLOR_TARGETS] = {};
    uint32_t             ColorCount = 0;
    MTLRenderTargetImpl* DepthRT    = nullptr;
    bool                 bPassBound = false;
    // A Clear before the pass opens becomes its load action.
    bool                 bClearColor = false;
    bool                 bClearDepth = false;
    MTLClearColor        ClearColor  = MTLClearColorMake(0, 0, 0, 0);
    double               ClearDepth  = 1.0;

    // Draw state.
    MTLViewport           Viewport{0, 0, 1, 1, 0, 1};
    MTLScissorRect        Scissor{0, 0, 1, 1};
    IDepthStateDesc       DepthState;
    IBlendStateDesc       BlendState;
    IStencilStateDesc     StencilState;
    EColorMask            ColorMask = EColM_RGBA;
    MTLShaderResourceImpl* BoundVS = nullptr;
    MTLShaderResourceImpl* BoundPS = nullptr;

    // What the open encoder has bound, so a draw sets only what changed.
    struct EncoderCache
    {
        __unsafe_unretained id<MTLRenderPipelineState> Pipeline = nil;
        __unsafe_unretained id<MTLDepthStencilState>   DepthStencil = nil;
        MTLCullMode                                    Cull = MTLCullModeNone;
        bool                                           bCullKnown = false;
        uint64_t MaterialSerial = ~0ull, SceneSerial = ~0ull, SkinSerial[2] = { ~0ull, ~0ull }, LightSerial = ~0ull;
        GDITextureResource* MaterialTextures[GDI_MATERIAL_TEXTURE_SLOTS] = {};
        bool     bMaterialTexturesKnown = false;
        void Forget() { *this = EncoderCache(); }
    } Bound;

    // Frames, for answers read back after completion.
    uint64_t              FrameSerial = 0;            // the open block's
    std::atomic<uint64_t> CompletedSerial{0};

    // Occlusion: a visibility buffer per frame slot (MTLQueries.mm).
    id<MTLBuffer>            Visibility[GDI_FRAMES_IN_FLIGHT_MAX];
    NSUInteger               VisibilityUsed[GDI_FRAMES_IN_FLIGHT_MAX] = {};
    __unsafe_unretained id<MTLBuffer> EncoderVisibility = nil;   // what the open render encoder carries
    MTLOcclusionQueriesImpl* OpenQuerySet   = nullptr;
    uint32_t                 OpenQueryIndex = 0;

    // GPU timestamps: samples at encoder boundaries (MTLQueries.mm).
    id<MTLCounterSampleBuffer> Samples[GDI_FRAMES_IN_FLIGHT_MAX];   // one per slot: Metal caps each at 32 KB
    uint32_t              SamplesPerSlot = 0;
    uint32_t              SamplesUsed[GDI_FRAMES_IN_FLIGHT_MAX] = {};
    int32_t               TimestampSample[GDI_FRAMES_IN_FLIGHT_MAX][GDI_GPU_TIMESTAMP_MAX];
    uint64_t              TimestampsWritten[GDI_FRAMES_IN_FLIGHT_MAX] = {};
    std::vector<uint32_t> PendingTimestamps;           // written before any encoder ran: the next one's start
    int32_t               LastEndSample    = -1;        // the last ended encoder's end sample (in its slot's buffer)
    int32_t               EncoderEndSample = -1;        // the open render encoder's
    bool                  bTiming          = false;     // a timestamp has been asked for: encoders sample
    uint64_t              PreviousTimestamps = 0;
    double                PreviousTimestampMs[GDI_GPU_TIMESTAMP_MAX] = {};

    /* A start/end sample pair for an encoder about to open (-1, -1 when not
     * timing or out of room); hands the start to any pending timestamps. */
    void AllocateSamples(int32_t& OutStart, int32_t& OutEnd);
    /* An encoder with InEnd as its end sample has ended. */
    void EncoderEnded(int32_t InEnd) { if(InEnd >= 0) LastEndSample = InEnd; }
    /* This slot's frame has completed: its timestamps become the previous block's. */
    void CollectTimestamps(uint32_t InSlot);
    /* A compute or blit encoder in the frame, sampled while timing. */
    id<MTLComputeCommandEncoder> ComputeEncoder(id<MTLCommandBuffer> InCmd, bool bInTransient, int32_t& OutEnd);
    id<MTLBlitCommandEncoder>    BlitEncoder(id<MTLCommandBuffer> InCmd, bool bInTransient, int32_t& OutEnd);

    MTLTextureResourceImpl* NullTexture = nullptr;
    GDIMesh*                ScreenMesh  = nullptr;

    std::unordered_set<MTLRenderTargetImpl*>             LiveRenderTargets;
    std::vector<std::pair<void*, MTLSwapchainImpl*>>     Swapchains;
    std::atomic<bool>                                    bResizing{false};

    // ---- Helpers (MetalDynamicGDI.mm) ----
    /* The render encoder for the bound targets, opened if need be; nil without targets. */
    id<MTLRenderCommandEncoder> RenderEncoder();
    /* Ends the open encoder; a pending clear is carried out first. */
    void EndPass();
    /* An upload for this frame (between BeginRendering and EndRendering) or,
     * outside one, a buffer of its own. */
    MTLUploadRing::Allocation Upload(const void* InData, NSUInteger InBytes, NSUInteger InPadTo = 0);
    /* A command buffer to record into now: the frame's (after EndPass) or, outside
     * a frame, a fresh one the caller commits and waits for (bOutTransient). */
    id<MTLCommandBuffer> CommandBufferForWork(bool& bOutTransient);
};

/* The timestamp sample buffer, where the device samples at stage boundaries (MTLQueries.mm). */
void MTLCreateTimestampSamples(MetalState& InState);

/* The driver, when it is the Metal backend. */
MetalDynamicGDI* GetMetalDriver();

#endif // NIMBLE_MTLINTERNAL_H

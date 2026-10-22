#ifndef CORECLIENT_D3D12DYNAMICGDI_H
#define CORECLIENT_D3D12DYNAMICGDI_H

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Public/GDI/GDI.h"
#include "Public/GDI/D3D12/D3D12MemoryHeap.h"
#include "Public/GDI/D3D12/D3D12PipelineCache.h"

class D3D12Device;
class D3D12CommandManager;
class D3D12Swapchain;
class D3D12RenderTarget;
class D3D12PipelineCache;
class D3D12DescriptorAllocator;
class D3D12ShaderResource;
class D3D12TextureResource;
class D3D12OcclusionQueries;
class D3D12RecordingContext;

/* D3D12 needs nothing from the host beyond the window the driver already holds. */
class D3D12GDIContext : public GDIContext
{
public:
    D3D12GDIContext()
    {
        ContextType = GDIContext::D3D12Context;
    }
};

namespace D3D12Util
{
    D3D12_BLEND    DecodeBlendFactor(EBlendMode InMode);
    D3D12_BLEND_OP DecodeBlendOp(EBlendOp InOp);
    UINT8          DecodeColorMask(EColorMask InColorMask);
    D3D12_COMPARISON_FUNC DecodeCompareFunc(ECompareTestMode InMode);

    /** InName (UTF-8) on InObject for PIX and the debug layer; nothing for a null object, an
     *  empty name, or with [Shaders] ShaderDebug off (GDIShaderDebugEnabled). */
    void SetObjectName(ID3D12Object* InObject, const std::string& InName);
}

/** Render-thread state captured between BeginRendering and EndRendering. */
class D3D12CurrentStateData
{
public:
    D3D12_VIEWPORT              ViewportRect{};
    D3D12_RECT                  ScissorRect{};

    IBlendStateDesc             BlendState;
    EColorMask                  ColorMask           = EColM_RGBA;
    IDepthStateDesc             DepthState;
    IStencilStateDesc           StencilState;

    D3D12RenderTarget*          ActiveRenderTarget  = nullptr;   // colour 0: what the present shows
    D3D12RenderTarget*          ActiveColors[GDI_MAX_COLOR_TARGETS] = {};
    uint32_t                    ActiveColorCount    = 0;
    D3D12RenderTarget*          ActiveDepth         = nullptr;   // whose depth is bound; null for none
    bool                        bRenderingActive    = false;
    ID3D12GraphicsCommandList*  CurrentCmd          = nullptr;

    // PIX events opened in the current list; a mid-frame flush resets it.
    int                         DebugEventDepth     = 0;
    // Their names, outermost first: a parallel pass's split closes and reopens them.
    std::vector<std::string>    DebugEventNames;

    D3D12ShaderResource*        BoundVS             = nullptr;
    D3D12ShaderResource*        BoundPS             = nullptr;
};

/**
 * What BindDrawState remembers between draws, so a draw redoes only what
 * changed (MULTITHREADED_RENDERING.md, phase 1). The driver keeps one for the
 * frame list; each recording context its own.
 *
 * Uploads: the view, material, scene and skin blocks' last, each reused while
 * its contents (view) or serial (the rest) and the ring's epoch are unchanged.
 * The pipeline: the last key looked up and what it found. Bound: what the list
 * has set, by root parameter, so an unchanged one is not set again --
 * forgotten whenever the list is (re)opened or its pipeline set elsewhere.
 */
struct D3D12DrawCaches
{
    uint64_t SceneAddress = 0;   // D3D12_GPU_VIRTUAL_ADDRESS
    uint64_t SceneSerial  = ~0ull;
    uint64_t SceneEpoch   = ~0ull;
    uint64_t SkinAddress[2] = {};   // the palette, the frame before's
    uint64_t SkinSerial[2]  = { ~0ull, ~0ull };
    uint64_t SkinEpoch[2]   = { ~0ull, ~0ull };
    uint64_t LightAddress = 0;   // the light block, b22
    uint64_t LightSerial  = ~0ull;
    uint64_t LightEpoch   = ~0ull;
    uint64_t MaterialAddress = 0;   // a material draw's b2
    uint64_t MaterialSerial  = ~0ull;
    uint64_t MaterialEpoch   = ~0ull;
    uint64_t ViewAddress = 0;
    uint64_t ViewEpoch   = ~0ull;
    alignas(16) float View[40] = {};   // as uploaded, clip z remapped

    D3D12PipelineCache::PipelineKey PipelineKey;
    ID3D12PipelineState*            Pipeline = nullptr;

    ID3D12PipelineState* BoundPipeline = nullptr;
    uint64_t             BoundRoot[RP_Count] = {};   // a CBV's address or a table's GPU handle; 0: not known
    GDITextureResource*  BoundTextures[GDI_MATERIAL_TEXTURE_SLOTS] = {};   // what BoundRoot's material tables came from

    void ForgetBound()
    {
        BoundPipeline = nullptr;
        for(uint64_t& Each : BoundRoot) { Each = 0; }
        for(GDITextureResource*& Each : BoundTextures) { Each = nullptr; }
    }
    void Forget() { *this = D3D12DrawCaches(); }
};

/**
 * Everything one draw reads (D3D12Mesh::Record, BindDrawState), so the
 * render thread's draws and a recording context's share no mutable state
 * (RenderCore's MULTITHREADED_RENDERING.md, phase 2). The render thread's come
 * from the driver (MakeDrawInputs); a context's from itself.
 */
struct D3D12DrawInputs
{
    ID3D12GraphicsCommandList* List       = nullptr;
    /* The list's serial for D3D12Buffer's state tracking; 0 in a recording
     * context, whose buffers are not transitioned: a buffer decays to COMMON
     * between ExecuteCommandLists calls and is promoted on first read. */
    uint64_t                   ListSerial = 0;

    D3D12ShaderResource*        VS        = nullptr;
    D3D12ShaderResource*        PS        = nullptr;
    bool                        bMaterial = false;
    const std::vector<uint8_t>* Blocks    = nullptr;   // [GDI_UNIFORM_BLOCK_COUNT], as GDIRecordingContext::MaterialBlocks
    const uint64_t*             Serials   = nullptr;   // [4]
    GDITextureResource* const*  Textures  = nullptr;   // [GDI_MATERIAL_TEXTURE_SLOTS]
    D3D12DrawCaches*            Caches    = nullptr;

    GDIBuffer*                 InstanceBuffer     = nullptr;
    uint32_t                   InstanceCount      = 0;
    GDIBuffer*                 IndirectArgs       = nullptr;
    uint32_t                   IndirectArgsOffset = 0;

    const IBlendStateDesc*     Blend     = nullptr;
    EColorMask                 ColorMask = EColM_RGBA;
    const IDepthStateDesc*     Depth     = nullptr;

    D3D12OcclusionQueries*     PredicateSet   = nullptr;
    int32_t                    PredicateIndex = -1;

    /* The core blocks, packed (GDICorePack); null: from the pixel shader's
     * scalars, as UpdateCoreUniforms left them. */
    const float*               CoreView      = nullptr;   // [40]
    const float*               CoreTransform = nullptr;   // [36]

    /* GDISetDrawRange's in a context (0 and 0: the mesh's own). */
    uint32_t                   DrawFirst = 0;
    uint32_t                   DrawCount = 0;
};

/**
 * Direct3D 12 driver, structured after VulkanDynamicGDI: the render thread
 * records one command list per frame and submits it in EndRendering, and the
 * UI thread presents the last finished frame through PresentToWindow.
 */
class D3D12DynamicGDI : public GDIDynamicDriver
{
public:
    explicit D3D12DynamicGDI(EngineCore::WindowHandle InWindow);
    ~D3D12DynamicGDI();

    void AssignContext(GDIContext* InContext) override;

    void Allocate() override;
    void Destroy() override;

    void SwapBuffers() override;
    void Flush() override;

    void Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth = 1.0f, uint InStencilMask = 0x00) override;
    void SetViewport(uint InX, uint InY, uint InWidth, uint InHeight) override;

    void SetScissorRect(uint InX, uint InY, uint InWidth, uint InHeight) override;
    void SetDepthState(IDepthStateDesc) override;
    void SetBlendState(IBlendStateDesc) override;
    void SetStencilState(IStencilStateDesc) override;
    void SetColorMask(EColorMask) override;

    void BeginRendering() override;
    void EndRendering() override;

    GDIRenderTarget* CreateRenderTarget(std::string Name, uint InWidth, uint InHeight) override;
    GDIRenderTarget* CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat) override;
    bool             SupportsRenderTargetFormat(EGDIRenderTargetFormat InFormat) const override;
    void SetRenderTarget(GDIRenderTarget* InRenderTarget) override;
    void SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom) override;
    void ReleaseRenderTarget(GDIRenderTarget* InRenderTarget) override;

    GDIShaderResource* CreateShaderResource(EShaderType InType, const std::string& SourceCode) override;
    void DrawScreenQuadPrimitive() override;
    using GDIDynamicDriver::DrawObject;
    void DrawObject(class GDIViewObject*, class GDIView*) override;
    void SetBoundShader(GDIShader*, GDIShader*) override;

    GDIMesh* CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) override;
    GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) override;
    GDIMesh* CreateMesh(const GDIMeshDesc& InDesc) override;

    GDIMesh* CreateDynamicMesh(uint32_t InMaxVertices, uint32_t InMaxIndices) override;

    GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResourceMips(const EngineCore::Image* InLevels, uint32_t InCount, EGDITextureBorder InBorder, EGDITextureFilter InFilter) override;
    bool                SupportsCompressedFormat(EGDICompressedFormat InFormat) const override;
    GDITextureResource* CreateCompressedTextureResource(EGDICompressedFormat InFormat, const GDICompressedLevel* InLevels, uint32_t InCount,
                                                        EGDITextureBorder InBorder, EGDITextureFilter InFilter) override;
    bool                GetTextureMemoryStats(GDITextureMemoryStats& OutStats) const override;

    bool CheckError(const std::string& FailStr) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_D3D12; }
    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    void BeginDrawEvent(const std::string& EventName) override;
    void EndDrawEvent() override;

    bool     SupportsGPUTimestamps() const override { return TimestampHeap != nullptr; }
    void     WriteGPUTimestamp(uint32_t InIndex) override;
    uint64_t GetPreviousGPUTimestamps(double* OutMilliseconds) override;

    /* Compute and GPU buffers (D3D12Compute.cpp): every device at feature level 11_0 has them. */
    bool       SupportsCompute() const override { return DeviceObj != nullptr && PipelineCache != nullptr; }
    GDIBuffer* CreateBuffer(uint32_t InBytes, uint8_t InUsage, const void* InInitialData) override;
    bool       DispatchCompute(GDIShader* InShader, const void* InParams, uint32_t InParamBytes,
                               GDIBuffer* const* InBuffers, uint32_t InBufferCount,
                               uint32_t InX, uint32_t InY, uint32_t InZ) override;
    bool       ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes) override;
    bool       SupportsIndirectDraw() const override { return SupportsCompute(); }

    /* Occlusion queries (D3D12Query.cpp): binary occlusion, predicated with
     * SetPredication -- core at every feature level. */
    bool                 SupportsOcclusionQueries() const override { return DeviceObj != nullptr; }

    /* Parallel passes (D3D12RecordingContext.cpp): a direct command list per
     * chunk (bundles cannot clear, query or set targets), executed in chunk
     * order between the frame list's halves. Nothing is inherited: each chunk
     * list binds its own targets and state, and predicates its own draws. */
    GDIParallelRecordingSupport GetParallelRecordingSupport() const override
    {
        GDIParallelRecordingSupport Out;
        Out.bSupported = DeviceObj != nullptr;
        Out.Mechanism  = "direct command lists";
        Out.Reason     = Out.bSupported ? "" : "no D3D12 device";
        return Out;
    }
    bool BeginParallelPass(uint32_t InChunks, GDIRecordingContext** OutContexts) override;
    void EndParallelPass() override;
    void RecordDepthState(GDIRecordingContext* InContext, IDepthStateDesc InDesc) override;
    void RecordBlendState(GDIRecordingContext* InContext, IBlendStateDesc InDesc) override;
    void RecordStencilState(GDIRecordingContext* InContext, IStencilStateDesc InDesc) override;
    void RecordColorMask(GDIRecordingContext* InContext, EColorMask InMask) override;
    void RecordViewport(GDIRecordingContext* InContext, uint InX, uint InY, uint InWidth, uint InHeight) override;
    void RecordScissorRect(GDIRecordingContext* InContext, uint InX, uint InY, uint InWidth, uint InHeight) override;
    void RecordDrawObject(GDIRecordingContext* InContext, class GDIViewObject* InObject, class GDIView* InView) override;
    void RecordBeginDrawEvent(GDIRecordingContext* InContext, const std::string& InName) override;
    void RecordEndDrawEvent(GDIRecordingContext* InContext) override;
    bool                 SupportsOcclusionPredication() const override { return DeviceObj != nullptr; }
    GDIOcclusionQueries* CreateOcclusionQueries(uint32_t InCapacity) override;
    void                 BeginOcclusionRound(GDIOcclusionQueries* InSet, uint32_t InCount) override;
    void                 BeginOcclusionQuery(GDIOcclusionQueries* InSet, uint32_t InIndex) override;
    void                 EndOcclusionQuery(GDIOcclusionQueries* InSet, uint32_t InIndex) override;
    void                 ResolveOcclusionQueries(GDIOcclusionQueries* InSet) override;
    void                 SetOcclusionPredicate(GDIOcclusionQueries* InSet, int32_t InIndex) override;
    bool                 ReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible) override;

    /** D3D12OcclusionQueries::Release: its heap and buffers, freed once the GPU is done. */
    void ReleaseOcclusionQueries(class D3D12OcclusionQueries* InSet);

    /** D3D12Mesh::Record, around its draw: predicates it on InSet's query InIndex,
     *  if set; true if it did, to pass to EndDrawPredicate. */
    static bool BeginDrawPredicate(ID3D12GraphicsCommandList* InList, D3D12OcclusionQueries* InSet, int32_t InIndex);
    static void EndDrawPredicate(ID3D12GraphicsCommandList* InList, bool bInBegun);

    /** InSet if its query InIndex can predicate a draw now (resolved); else null. */
    static D3D12OcclusionQueries* UsablePredicate(GDIOcclusionQueries* InSet, int32_t InIndex);

    /** GDISetOcclusionPredicate's set as the render thread has it: a parallel pass's starting predicate. */
    GDIOcclusionQueries* ImmediatePredicateSet() const;

    /**
     * The open frame list's serial, new each time a frame list is opened
     * (BeginRendering, FlushForReadback); D3D12Buffer tracks its state per
     * list with it. AllocateListSerial gives a transient list its own.
     */
    inline uint64_t GetListSerial() const { return FrameListSerial; }
    inline uint64_t AllocateListSerial()  { return ++NextListSerial; }

    /** UI thread: draws the last finished frame into the swapchain and presents it. */
    void PresentToWindow(EngineCore::WindowHandle InWindow) override;
    void PresentRenderTargetToWindow(EngineCore::WindowHandle InWindow, GDIRenderTarget* InSource) override;
    void ReleaseWindow(EngineCore::WindowHandle InWindow) override;

    /**
     * Submits what has been recorded this frame, waits for it, and resumes
     * recording on a fresh list. For readbacks that must see pixels drawn but
     * not yet submitted. Pass false when the active RT is about to be destroyed.
     */
    void FlushForReadback(bool bResumeActiveRegion = true);

    /** Records a full copy Src -> Dst; both are left in PIXEL_SHADER_RESOURCE. */
    void RecordResolve(D3D12RenderTarget* Src, D3D12RenderTarget* Dst);

    /** The render thread's next draw's inputs, from the driver's state. False if
     *  nothing is open to draw into. */
    bool MakeDrawInputs(D3D12DrawInputs& OutIn);

    /** Binds pipeline, constant buffers and textures for InIn's draw of InMesh
     *  into InIn.List. The render thread, or a thread recording a chunk. */
    bool BindDrawState(const D3D12DrawInputs& InIn, GDIMesh* InMesh, bool bIsInstanced);

    /** While set, PresentToWindow skips: the offscreen RTs are being recreated. Cleared by EndRendering. */
    inline void SetResizing(bool b) { bResizing.store(b); }

    void RegisterRenderTarget(D3D12RenderTarget* InRT)   { LiveRenderTargets.insert(InRT); }
    void UnregisterRenderTarget(D3D12RenderTarget* InRT) { LiveRenderTargets.erase(InRT); }

    /**
     * GL callers release resources straight after the draw that uses them, while
     * the list referencing them is still open or in flight. These hold them,
     * each tagged with the fence value of the frame being recorded (which covers
     * every frame in flight before it), until that frame has finished.
     */
    /** InAllocation (D3D12TextureHeap's) goes back to its block once InResource is released. */
    void DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource> InResource, const D3D12Allocation& InAllocation = D3D12Allocation());
    void DeferFreeSRV(uint32_t InSlot);
    void DeferReleaseQueryHeap(Microsoft::WRL::ComPtr<ID3D12QueryHeap> InHeap);
    /** An upload's command list, submitted without a wait (D3D12Ops::EndAndSubmitTransientCmd). */
    void DeferReleaseTransient(Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    InAllocator,
                               Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> InList);

    /** Frees what is queued above for frames that have finished (all of it, after a full wait),
     *  and gives back the staging ring's ranges those frames' uploads used. */
    void DrainDeferredDestruction(bool bInAll = false);

    /** The fence value the frame being recorded will signal (D3D12CommandManager). */
    uint64_t GetRecordingFenceValue() const;
    /** Whether the frame that signals InValue has finished on the GPU (no wait). */
    bool     IsFrameComplete(uint64_t InValue) const;

    inline D3D12Device*              GetDeviceObj()      const { return DeviceObj; }
    inline D3D12CommandManager*      GetCommandManager() const { return CommandManager; }
    inline D3D12PipelineCache*       GetPipelineCache()  const { return PipelineCache; }
    inline D3D12DescriptorAllocator* GetDescAlloc()      const { return DescAlloc; }

private:
    /** InWindow's swapchain, created and initialized on first use; null if that
     *  failed (retried on the next present). UI thread. */
    D3D12Swapchain* FindOrCreateSwapchain(EngineCore::WindowHandle InWindow);

    /** Presents InRT through InWindow's swapchain. UI thread. */
    void PresentToSwapchain(EngineCore::WindowHandle InWindow, D3D12RenderTarget* InRT);

    /** Root signature, heaps and topology a freshly opened list needs before any draw. */
    void PrepareCommandList(ID3D12GraphicsCommandList* InList);

    /** Closes and submits the frame list, publishing each RT's state atomically with the submit. */
    void SubmitFrame();

    /** Records the bound colours and depth, viewport and scissor into InList: a
     *  list opened mid-rendering (a chunk's, the frame list's second half). */
    void BindActiveTargets(ID3D12GraphicsCommandList* InList) const;

    /** BeginRendering, after its slot's fence wait: each recording context's
     *  allocator for frame slot InSlot rewound. */
    void ResetRecordingContexts(uint32_t InSlot);
    void DestroyRecordingContexts();

    D3D12Device*              DeviceObj        = nullptr;
    D3D12CommandManager*      CommandManager   = nullptr;
    /** One per presented window, keyed by its native handle. UI thread only
     *  (and Destroy, once the UI thread no longer presents). */
    std::vector<std::pair<void*, D3D12Swapchain*>> Swapchains;
    D3D12PipelineCache*       PipelineCache    = nullptr;
    D3D12DescriptorAllocator* DescAlloc        = nullptr;
    D3D12CurrentStateData*    CurrentStateData = nullptr;
    uint32_t                  FramesInFlightCount = 1;   // [Rendering] FramesInFlight, read at Allocate

    /* The frame list's draws' caches (a recording context keeps its own). */
    D3D12DrawCaches           DrawCaches;

    /* Parallel passes' contexts, made on first need and kept (D3D12RecordingContext.cpp). */
    std::vector<D3D12RecordingContext*> RecordingContexts;
    uint32_t                  ParallelChunks = 0;
    bool                      bParallelPass  = false;

    GDIMesh*                  ScreenMesh       = nullptr;
    D3D12TextureResource*     NullTexture      = nullptr;

    uint64_t                  NextListSerial   = 0;
    uint64_t                  FrameListSerial  = 0;

    std::atomic<bool>         bResizing{false};

    std::unordered_set<D3D12RenderTarget*>             LiveRenderTargets;

    /* Deferred destruction (render thread only), each entry tagged with the
     * fence value after which nothing uses it; queued in that order. */
    struct DeferredResource  { Microsoft::WRL::ComPtr<ID3D12Resource>  Resource; D3D12Allocation Allocation; uint64_t DoneAt; };
    struct DeferredQueryHeap { Microsoft::WRL::ComPtr<ID3D12QueryHeap> Heap;     uint64_t DoneAt; };
    struct DeferredSRV       { uint32_t Slot; uint64_t DoneAt; };
    struct DeferredTransient
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    Allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> List;
        uint64_t                                          DoneAt;
    };
    std::vector<DeferredResource>  DeferredResources;
    std::vector<DeferredQueryHeap> DeferredQueryHeaps;   // released occlusion query sets'
    std::vector<DeferredSRV>       DeferredSRVs;
    std::vector<DeferredTransient> DeferredTransients;   // uploads into fresh textures

    /* GDISetOcclusionPredicate's: the draws after it are predicated on it. */
    D3D12OcclusionQueries*       PredicateSet   = nullptr;
    int32_t                      PredicateIndex = -1;

    /*
     * GPU timestamps (GDIDynamicDriver::WriteGPUTimestamp), render thread only:
     * a heap of GDI_GPU_TIMESTAMP_MAX per frame slot (queries [Slot * MAX,
     * (Slot + 1) * MAX)), each slot's resolved into its range of
     * TimestampReadback by the submits of its block, and read at the
     * BeginRendering that reuses the slot, after its fence is waited.
     */
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> TimestampHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  TimestampReadback;
    double   TimestampTicksPerMs = 0.0;
    uint64_t TimestampsWritten[GDI_FRAMES_IN_FLIGHT_MAX]  = {};   // each slot's block's, by index
    uint64_t TimestampsResolved[GDI_FRAMES_IN_FLIGHT_MAX] = {};   // resolved by a submit, not yet read
    uint64_t PreviousTimestamps  = 0;   // the last block read back's
    double   PreviousTimestampMs[GDI_GPU_TIMESTAMP_MAX] = {};
    void CreateTimestampHeap();
    void ResolveTimestamps(ID3D12GraphicsCommandList* InList);
    void CollectTimestamps(uint32_t InSlot);
};

#endif //CORECLIENT_D3D12DYNAMICGDI_H

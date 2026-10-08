#ifndef CORECLIENT_VULKANDYNAMICGDI_H
#define CORECLIENT_VULKANDYNAMICGDI_H

#include <vulkan/vulkan.h>
#include "Public/GDI/Vulkan/GDIVulkanPlatform.h"
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>


#include <atomic>

#include "Public/GDI/GDI.h"
#include "Public/GDI/Vulkan/VKPipelineCache.h"
#include "Public/GDI/Vulkan/VKShader.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <vector>
#include <string>

class VKDevice;
class VKCommandManager;
class VKSwapchain;
class VKRenderTarget;
class VKSync;
class VKPipelineCache;
class VKDescriptorAllocator;

class VulkanGDIContext : public GDIContext
{
public:
    VulkanGDIContext();
    ~VulkanGDIContext() override;

    /*
     * Process-wide VkInstance, lazily created and owned by GDI.
     *
     * Both threads share it. The Qt bridge adopts it into a QVulkanInstance
     * for the host's window; GDI itself never sees a Qt type.
     */
    static VkInstance GetSharedVkInstance();

    /* VK_EXT_debug_utils is enabled on the shared instance: draw events become
     * command buffer labels and render targets are named. Non-shipping builds,
     * whenever the loader (or a capture tool's layer) offers the extension. */
    static bool HasDebugUtils();

    VkInstance VKContext = VK_NULL_HANDLE;
};

namespace VKUtil
{
    VkBlendFactor DecodeBlendFactor(EBlendMode InMode);
    VkBlendOp     DecodeBlendOp(EBlendOp InOp);
    VkFlags       DecodeColorMask(EColorMask InColorMask);
    VkCompareOp   DecodeCompareOp(ECompareTestMode InMode);
}

class VKRasterizationState
{
public:
    VKRasterizationState()
    {
        StateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    }
    VkPipelineRasterizationStateCreateInfo StateInfo{};
};

class VKDepthStencilState
{
public:
    VKDepthStencilState()
    {
        StateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    }
    void SetDepthState(IDepthStateDesc InDepthState)
    {
        StateInfo.depthTestEnable  = InDepthState.bDepthTest ? VK_TRUE : VK_FALSE;
        StateInfo.depthWriteEnable = InDepthState.bDepthMask ? VK_TRUE : VK_FALSE;
        StateInfo.depthCompareOp   = VKUtil::DecodeCompareOp(InDepthState.DepthMode);
    }
    void SetStencilState(IStencilStateDesc InStencilState)
    {
        StateInfo.stencilTestEnable = InStencilState.bStencilTest ? VK_TRUE : VK_FALSE;
    }
    VkPipelineDepthStencilStateCreateInfo StateInfo{};
};

class VKBlendState
{
public:
    VKBlendState()
    {
        StateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    }
    void SetBlendState(IBlendStateDesc InDesc)
    {
        ColorAttachment.blendEnable         = InDesc.bBlend ? VK_TRUE : VK_FALSE;
        ColorAttachment.srcColorBlendFactor = VKUtil::DecodeBlendFactor(InDesc.Source);
        ColorAttachment.dstColorBlendFactor = VKUtil::DecodeBlendFactor(InDesc.Dest);
        // glBlendFunc applies the same factors to color AND alpha; without this the
        // alpha channel blends with the pipeline default (ONE/ZERO), i.e. every draw
        // REPLACES destination alpha instead of accumulating it.
        ColorAttachment.srcAlphaBlendFactor = ColorAttachment.srcColorBlendFactor;
        ColorAttachment.dstAlphaBlendFactor = ColorAttachment.dstColorBlendFactor;
        ColorAttachment.colorBlendOp        = VKUtil::DecodeBlendOp(InDesc.Op);
        ColorAttachment.alphaBlendOp        = VKUtil::DecodeBlendOp(InDesc.AlphaOp);
    }
    void SetColorMask(EColorMask InMask)
    {
        ColorAttachment.colorWriteMask = VKUtil::DecodeColorMask(InMask);
    }
    VkPipelineColorBlendAttachmentState ColorAttachment{};
    VkPipelineColorBlendStateCreateInfo StateInfo{};
};

/**
 * Per-frame state captured between BeginRendering / EndRendering.
 * Phase 1 only uses ScissorRect and ViewportRect for vkCmdSetViewport;
 * the state-object members are populated for future pipeline construction in Phase 2.
 */
class VKCurrentStateData
{
public:
    VKCurrentStateData();
    ~VKCurrentStateData();

    VkRect2D                ScissorRect{};
    VkViewport              ViewportRect{};
    VKBlendState*           BlendState          = nullptr;
    VKDepthStencilState*    DepthStencilState   = nullptr;

    // Active dynamic-rendering region (the RTs bound via SetRenderTargets).
    // ActiveRenderTarget is colour 0: what the present shows and Clear sizes by.
    VKRenderTarget*         ActiveRenderTarget  = nullptr;
    VKRenderTarget*         ActiveColors[GDI_MAX_COLOR_TARGETS] = {};
    uint32_t                ActiveColorCount    = 0;
    VKRenderTarget*         ActiveDepth         = nullptr;   // whose depth is attached; null for none
    bool                    bRenderingActive    = false;
    VkCommandBuffer         CurrentCmd          = VK_NULL_HANDLE;

    // Open vkCmdBeginDebugUtilsLabelEXT regions in the current command buffer.
    // Reset to 0 whenever a fresh command buffer starts so EndDrawEvent never emits
    // an End label without a matching Begin in the same buffer (which a mid-frame
    // FlushForReadback would otherwise cause).
    int                     DebugLabelDepth     = 0;

    // Shader modules cached by SetBoundShader for use in PipelineKey construction.
    // VK_NULL_HANDLE means no shader is bound; DrawObject will skip the draw.
    VkShaderModule          BoundVSModule       = VK_NULL_HANDLE;
    VkShaderModule          BoundFSModule       = VK_NULL_HANDLE;
};

/*
 * What a draw reuses rather than rebuilds, allocates or binds again
 * (MULTITHREADED_RENDERING.md, phases 1 and 2): one set for the render
 * thread's primary command buffer, one per recording context.
 *
 *   LastPipelineKey/Fetch  the last pipeline lookup: the same key (a run of
 *                          draws with one shader pair and state) skips the map
 *   Bound*                 what the command buffer has bound: not bound again.
 *                          Forgotten whenever a command buffer begins
 *                          (FlushForReadback restarts the same handle, so the
 *                          handle cannot tell)
 *   ViewUpload*            the ViewParameters block's last upload: reused while
 *                          its bytes and the ring's epoch are unchanged
 *   BlockUpload*, Skin*    the material, scene and skin blocks', while their
 *                          serials are
 *   DescSets               descriptor sets by their contents, for the ring's
 *                          epoch: the ring-backed blocks are dynamic uniform
 *                          buffers, so draws differing only in their offsets
 *                          share one set
 *   *Predicate*            the conditional rendering held open (ApplyDrawPredicate)
 */
struct VKDrawCaches
{
    struct DescSetKey
    {
        VkDescriptorSetLayout Layout = VK_NULL_HANDLE;
        VkBuffer              Buffers[6]  = {};
        uint32_t              Ranges[6]   = {};
        VkSampler             Samplers[GDI_MATERIAL_TEXTURE_SLOTS] = {};
        VkImageView           Views[GDI_MATERIAL_TEXTURE_SLOTS]    = {};
        bool operator==(const DescSetKey& InOther) const { return std::memcmp(this, &InOther, sizeof(DescSetKey)) == 0; }
    };
    struct DescSetKeyHash
    {
        size_t operator()(const DescSetKey& InKey) const
        {
            const uint64_t* Words = reinterpret_cast<const uint64_t*>(&InKey);
            uint64_t Hash = 1469598103934665603ull;
            for(size_t Index = 0; Index < sizeof(DescSetKey) / sizeof(uint64_t); ++Index)
            {
                Hash = (Hash ^ Words[Index]) * 1099511628211ull;
                Hash ^= Hash >> 29;
            }
            return size_t(Hash);
        }
    };

    /* A command buffer begun: nothing bound in it, no predicate open. */
    void ForgetBound()
    {
        BoundPipeline       = VK_NULL_HANDLE;
        BoundLayout         = VK_NULL_HANDLE;
        BoundSet            = VK_NULL_HANDLE;
        BoundOffsetCount    = 0;
        bPredicateOpen      = false;
        OpenPredicateBuffer = VK_NULL_HANDLE;
        OpenPredicateOffset = 0;
    }

    VKPipelineCache::PipelineKey   LastPipelineKey{};
    VKPipelineCache::PipelineFetch LastPipelineFetch{};
    bool                           bLastPipelineValid = false;

    VkPipeline              BoundPipeline    = VK_NULL_HANDLE;
    VkPipelineLayout        BoundLayout      = VK_NULL_HANDLE;
    VkDescriptorSet         BoundSet         = VK_NULL_HANDLE;
    uint32_t                BoundOffsets[6]  = {};
    uint32_t                BoundOffsetCount = 0;

    alignas(16) float       ViewUploadBytes[40] = {};
    VkBuffer                ViewUploadBuffer  = VK_NULL_HANDLE;
    VkDeviceSize            ViewUploadOffset  = 0;
    uint64_t                ViewUploadEpoch   = ~0ull;

    /* [0] the material block, [1] the scene block. */
    VkBuffer                BlockUploadBuffer[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceSize            BlockUploadOffset[2] = {};
    uint32_t                BlockUploadSize[2]   = {};
    uint64_t                BlockUploadSerial[2] = { ~0ull, ~0ull };
    uint64_t                BlockUploadEpoch[2]  = { ~0ull, ~0ull };
    /* The skin blocks: the palette, the frame before's. */
    VkBuffer                SkinBlockUploadBuffer[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceSize            SkinBlockUploadOffset[2] = {};
    uint32_t                SkinBlockUploadSize[2]   = {};
    uint64_t                SkinBlockUploadSerial[2] = { ~0ull, ~0ull };
    uint64_t                SkinBlockUploadEpoch[2]  = { ~0ull, ~0ull };

    std::unordered_map<DescSetKey, VkDescriptorSet, DescSetKeyHash> DescSets;
    uint64_t                DescSetsEpoch = ~0ull;

    bool                    bPredicateOpen      = false;
    VkBuffer                OpenPredicateBuffer = VK_NULL_HANDLE;
    VkDeviceSize            OpenPredicateOffset = 0;
};

/*
 * Everything one draw is recorded with: the render thread's own state and
 * the frame's primary command buffer (VulkanDynamicGDI::MakeImmediateInputs),
 * or a recording context's copies and its secondary. BindDrawState and
 * VKMesh::Record read nothing else of the driver's but its shared, read-only
 * pieces -- the device, the pipeline cache (locked), the null texture.
 */
struct VKDrawInputs
{
    VkCommandBuffer             Cmd = VK_NULL_HANDLE;
    bool                        bRenderingActive = false;
    GDIShader*                  VS = nullptr;
    GDIShader*                  PS = nullptr;
    VkShaderModule              VSModule = VK_NULL_HANDLE;
    VkShaderModule              FSModule = VK_NULL_HANDLE;
    const std::vector<uint8_t>* Blocks  = nullptr;        // [4], as GDIDynamicDriver::MaterialBlocks
    const uint64_t*             Serials = nullptr;        // [4]
    GDITextureResource* const*  Textures = nullptr;       // [GDI_MATERIAL_TEXTURE_SLOTS]
    GDIBuffer*                  InstanceBuffer = nullptr;
    uint32_t                    InstanceCount  = 0;
    GDIBuffer*                  IndirectArgs   = nullptr;
    uint32_t                    IndirectArgsOffset = 0;
    const class VKBlendState*        Blend = nullptr;
    const class VKDepthStencilState* Depth = nullptr;
    bool                        bHasTarget = false;
    VkFormat                    RTFormat   = VK_FORMAT_UNDEFINED;
    uint32_t                    ColorCount = 1;
    VkFormat                    ColorFormats[GDI_MAX_COLOR_TARGETS] = {};
    VkFormat                    DepthFormat = VK_FORMAT_UNDEFINED;
    class VKOcclusionQueries*   PredicateSet   = nullptr;
    int32_t                     PredicateIndex = -1;
    /* The draw's own core blocks (a context's); null: the bound shaders'. */
    const VKShaderResource::CoreBlocks* Core = nullptr;
    /* The draw's own index range (a context's); 0 and 0: the mesh's. */
    uint32_t                    DrawFirst = 0;
    uint32_t                    DrawCount = 0;
    VKDrawCaches*               Caches = nullptr;
    class VKDescriptorAllocator* Alloc = nullptr;
};

class VulkanDynamicGDI : public GDIDynamicDriver
{
public:
    explicit VulkanDynamicGDI(EngineCore::WindowHandle InWindow);
    ~VulkanDynamicGDI();

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
    // Single-arg DrawObject intentionally NOT overridden: the base implementation
    // forwards to the two-arg overload, which flushes core + user uniforms (the
    // cursor's stencil texture) before the draw.
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

    bool CheckError(const std::string& FailStr) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_Vulkan; }
    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    void BeginDrawEvent(const std::string& EventName) override;
    /* Labels need VK_EXT_debug_utils on the instance: RenderDoc's layer, or the validation layers. */
    bool HasDrawEvents() const override { return VulkanGDIContext::HasDebugUtils(); }
    void EndDrawEvent() override;

    /** Compute and GPU buffers (VKCompute.cpp): dispatches record into the
     *  frame's one command buffer, with full memory barriers around them. */
    bool       SupportsCompute() const override;
    bool       SupportsIndirectDraw() const override { return SupportsCompute(); }
    GDIBuffer* CreateBuffer(uint32_t InBytes, uint8_t InUsage, const void* InInitialData) override;
    bool       DispatchCompute(GDIShader* InShader, const void* InParams, uint32_t InParamBytes,
                               GDIBuffer* const* InBuffers, uint32_t InBufferCount,
                               uint32_t InX, uint32_t InY, uint32_t InZ) override;
    bool       ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes) override;

    bool     SupportsGPUTimestamps() const override { return TimestampPool != VK_NULL_HANDLE; }
    void     WriteGPUTimestamp(uint32_t InIndex) override;
    uint64_t GetPreviousGPUTimestamps(double* OutMilliseconds) override;

    /** Occlusion queries (VKQuery.cpp): a query pool and a host-visible result
     *  buffer per set; predication through VK_EXT_conditional_rendering. */
    bool                 SupportsOcclusionQueries() const override;
    GDIParallelRecordingSupport GetParallelRecordingSupport() const override;
    bool                 SupportsOcclusionPredication() const override;
    GDIOcclusionQueries* CreateOcclusionQueries(uint32_t InCapacity) override;
    void                 BeginOcclusionRound(GDIOcclusionQueries* InSet, uint32_t InCount) override;
    void                 BeginOcclusionQuery(GDIOcclusionQueries* InSet, uint32_t InIndex) override;
    void                 EndOcclusionQuery(GDIOcclusionQueries* InSet, uint32_t InIndex) override;
    void                 ResolveOcclusionQueries(GDIOcclusionQueries* InSet) override;
    void                 SetOcclusionPredicate(GDIOcclusionQueries* InSet, int32_t InIndex) override;
    bool                 ReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible) override;

    /** VKOcclusionQueries::Release: its pool and buffer, destroyed once the GPU is done. */
    void ReleaseOcclusionQueries(class VKOcclusionQueries* InSet);

    /**
     * Puts the draw about to be recorded under GDISetOcclusionPredicate's
     * predicate (VKMesh::Draw). Conditional rendering is held open across
     * consecutive draws with the same predicate -- an entity's sections share
     * its query, and begun and ended round each draw it cost the driver ~2.7 us
     * a draw -- switched when the predicate changes, ended for a draw with
     * none, and closed by CloseDrawPredicate before anything it would affect
     * (a clear) or that must not span it (the rendering's end, a query).
     */
    void ApplyDrawPredicate(const VKDrawInputs& InInputs);
    void CloseDrawPredicate(VKDrawCaches& InCaches, VkCommandBuffer InCmd);
    void CloseDrawPredicate();   // the render thread's

    /** Vulkan present path: UI thread acquires + blits the offscreen RT and presents. */
    void PresentToWindow(EngineCore::WindowHandle InWindow) override;
    void PresentRenderTargetToWindow(EngineCore::WindowHandle InWindow, GDIRenderTarget* InSource) override;
    void ReleaseWindow(EngineCore::WindowHandle InWindow) override;

    /**
     * Submit all commands recorded so far this frame and block until the GPU
     * finishes, then resume recording into a fresh command buffer (restoring the
     * viewport/scissor and the active rendering region).  Used by synchronous
     * readbacks (VKRenderTarget::ToImage, GetColorFromSamplePoint) that must
     * observe pixels just drawn but not yet submitted.
     *
     * @param bResumeActiveRegion Re-open the rendering region on the previously
     *        active RT after the flush. Pass false when the active RT is about to
     *        be destroyed (resize) so the fresh command buffer doesn't reference it.
     */
    void FlushForReadback(bool bResumeActiveRegion = true);

    /**
     * Record a full-image copy Src → Dst into the active command buffer (ends any
     * open rendering region first).  Backs VKRenderTarget::ResolveRenderTarget,
     * which the layer manager uses to back up / restore the canvas buffer.
     * Both render targets are left in SHADER_READ_ONLY_OPTIMAL.
     */
    void RecordResolve(class VKRenderTarget* Src, class VKRenderTarget* Dst);

    /**
     * Record a copy of Src's colour into Dst (a host-visible buffer) in the frame
     * being recorded, resuming any rendering region after it. False when no frame
     * is recording. OutDoneAt: the value IsSubmissionDone reports it done at.
     * Backs VKRenderTarget::BeginReadback.
     */
    bool RecordReadback(class VKRenderTarget* Src, VkBuffer Dst, uint64_t& OutDoneAt);
    bool IsSubmissionDone(uint64_t InDoneAt) const { return InDoneAt <= CompletedTimelineValue; }

    /**
     * Build the pipeline + descriptor set for the current GDI state and bind them
     * into the active command buffer. Called by GDIMesh::Draw() (VKMesh::Draw)
     * immediately before it issues the vkCmdDraw, so EVERY draw — both the
     * DrawObject path and a direct Mesh->Draw() (layer compositing) — gets a bound
     * pipeline. Returns false (and binds nothing) if state is incomplete.
     */
    bool BindDrawState(const VKDrawInputs& InInputs, GDIMesh* InMesh, bool bIsInstanced);

    /** The render thread's draw inputs: its state, the frame's primary. */
    void MakeImmediateInputs(VKDrawInputs& OutInputs);

    /* Parallel passes (GDIBeginParallelPass): VKRecordingContext.cpp. */
    bool BeginParallelPass(uint32_t InChunks, GDIRecordingContext** OutContexts) override;
    void EndParallelPass() override;
    void RecordDepthState(GDIRecordingContext* InContext, IDepthStateDesc InDesc) override;
    void RecordBlendState(GDIRecordingContext* InContext, IBlendStateDesc InDesc) override;
    void RecordStencilState(GDIRecordingContext* InContext, IStencilStateDesc InDesc) override;
    void RecordColorMask(GDIRecordingContext* InContext, EColorMask InMask) override;
    void RecordViewport(GDIRecordingContext* InContext, uint InX, uint InY, uint InWidth, uint InHeight) override;
    void RecordScissorRect(GDIRecordingContext* InContext, uint InX, uint InY, uint InWidth, uint InHeight) override;
    void RecordDrawObject(GDIRecordingContext* InContext, GDIViewObject* InObject, GDIView* InView) override;
    void RecordBeginDrawEvent(GDIRecordingContext* InContext, const std::string& InName) override;
    void RecordEndDrawEvent(GDIRecordingContext* InContext) override;
    /* Each context's per-frame resources reset (BeginRendering, once the
     * frame's fence is waited). */
    void ResetRecordingContexts();
    void DestroyRecordingContexts();
    /* InSet's query InIndex if it can predicate a draw now (resolved this
     * round, in range); else null (VKQuery.cpp). */
    class VKOcclusionQueries* UsablePredicate(GDIOcclusionQueries* InSet, int32_t InIndex) const;
    /* GDISetOcclusionPredicate's set, as GDI's type (VKQuery.cpp). */
    GDIOcclusionQueries*      ImmediatePredicateSet() const;

    /**
     * Marks that render targets are being resized (render thread). While set, the
     * UI-thread present skips entirely — the offscreen RTs are mid-recreation and
     * their layouts/contents are inconsistent. Cleared by EndRendering once a full
     * frame has been rendered into the new targets, so the next present is valid.
     */
    inline void SetResizing(bool b) { bResizing.store(b); }

    /**
     * Render-target registry (render thread only). SetRenderTarget uses it to
     * transition EVERY render target that was last written as a color attachment
     * to SHADER_READ_ONLY_OPTIMAL before a new rendering region opens — so any RT
     * sampled as a texture (layer buffers, the brush stroke buffer) is guaranteed
     * to be in the layout the sampler descriptors declare, regardless of binding
     * order. Registered by CreateRenderTarget, unregistered by ~VKRenderTarget.
     */
    void RegisterRenderTarget(VKRenderTarget* InRT)   { LiveRenderTargets.insert(InRT); }
    void UnregisterRenderTarget(VKRenderTarget* InRT) { LiveRenderTargets.erase(InRT); }

    /**
     * Deferred destruction (render thread only). GL callers routinely release a
     * texture/mesh immediately after the draw that uses it (e.g. QCanvasLayer::Restore's
     * temp texture in the flood-fill path) — legal on GL, but on Vulkan the handles
     * are still referenced by the open (or in-flight) frame command buffer, and
     * destroying them invalidates it. Handles queued here are destroyed only after
     * the next frame-fence wait proves all prior submits completed.
     */
    void DeferDestroyImage(VkImage InImage, VkDeviceMemory InMemory, VkImageView InView, VkSampler InSampler);
    void DeferDestroyBuffer(VkBuffer InBuffer, VkDeviceMemory InMemory);
    /** Destroy everything queued above. Caller must guarantee the GPU no longer references them. */
    void DrainDeferredDestruction();

    inline VKDevice*                GetDeviceObj()       const { return DeviceObj; }
    inline VKCommandManager*        GetCommandManager()  const { return CommandManager; }
    inline VKPipelineCache*         GetPipelineCache()   const { return PipelineCache; }
    inline VKDescriptorAllocator*   GetDescAlloc()       const { return DescAlloc; }

    void ForgetBoundDrawState() { Immediate.ForgetBound(); }

private:
    void EndActiveRenderingRegion();
    /* vkCmdBeginRendering again on the active targets, every attachment
     * loaded, with InFlags (a parallel pass's: secondaries' contents). */
    void ReopenRendering(VkRenderingFlags InFlags);

    /** InWork(Cmd) outside the open rendering region, reopened after it (VKQuery.cpp). */
    template<typename WorkFn> void RecordOutsideRendering(WorkFn&& InWork);

    /** VK_EXT_conditional_rendering's commands, where the device enabled it. */
    void LoadQueryFunctions();
    PFN_vkCmdBeginConditionalRenderingEXT CmdBeginConditionalRendering = nullptr;
    PFN_vkCmdEndConditionalRenderingEXT   CmdEndConditionalRendering   = nullptr;

    /** GDISetOcclusionPredicate's: the draws after it are conditional on it. */
    class VKOcclusionQueries* PredicateSet   = nullptr;
    int32_t                   PredicateIndex = -1;

    /** InWindow's swapchain, created and initialized on first use; null if that
     *  failed (retried on the next present). UI thread. */
    VKSwapchain* FindOrCreateSwapchain(EngineCore::WindowHandle InWindow);

    /** Presents InRT through InWindow's swapchain. UI thread. */
    void PresentToSwapchain(EngineCore::WindowHandle InWindow, VKRenderTarget* InRT);

    VKDevice*               DeviceObj           = nullptr;
    VKCommandManager*       CommandManager      = nullptr;
    /** One per presented window, keyed by its native handle. UI thread only
     *  (and Destroy, once the UI thread no longer presents). */
    std::vector<std::pair<void*, VKSwapchain*>> Swapchains;
    VKPipelineCache*        PipelineCache       = nullptr;
    VKDescriptorAllocator*  DescAlloc           = nullptr;
    VKCurrentStateData*     CurrentStateData    = nullptr;


    /* The render thread's draw caches (VKDrawCaches): what its draws on the
     * frame's primary command buffer reuse. */
    VKDrawCaches            Immediate;

    /* The parallel passes' contexts (BeginParallelPass), made on first need
     * and kept; ParallelChunks of them record the open pass. */
    std::vector<class VKRecordingContext*> RecordingContexts;
    uint32_t                ParallelChunks = 0;
    bool                    bParallelPass  = false;

    /** Lazily-created full-screen NDC quad for DrawScreenQuadPrimitive. */
    GDIMesh*                ScreenMesh          = nullptr;
    /** 1×1 transparent texture used as a fallback for unbound descriptor slots. */
    class VKTextureResource* NullTexture       = nullptr;

    /**
     * Timeline semaphore signaled by the render thread's EndRendering submit and
     * awaited by the UI-thread present. A timeline (monotonic) value is used instead
     * of a binary semaphore so the render and present threads — which run on
     * independent triggers (render Tick vs. expose/update events) — never desync:
     * the present waits for >= the last published value, which is idempotent.
     */
    VkSemaphore             RenderTimeline        = VK_NULL_HANDLE;
    uint64_t                RenderTimelineCounter = 0;            // render thread only
    uint64_t                CompletedTimelineValue = 0;           // every frame up to it has finished on the GPU
    std::atomic<uint64_t>   RenderTimelinePublished{0};          // render → UI handoff

    /*
     * GPU timestamps (GDIDynamicDriver::WriteGPUTimestamp), render thread
     * only. One pool of GDI_GPU_TIMESTAMP_MAX, reset at each BeginRendering
     * after the previous block's are read back -- its fence has just been
     * waited, so they are complete. Null if the graphics queue cannot time.
     */
    VkQueryPool TimestampPool      = VK_NULL_HANDLE;
    double      TimestampPeriodNs  = 0.0;                         // nanoseconds per tick
    uint64_t    TimestampValidMask = 0;                           // the queue's timestampValidBits
    uint64_t    TimestampsWritten  = 0;                           // this block's, by index
    uint64_t    PreviousTimestamps = 0;                           // the previous block's, read back
    double      PreviousTimestampMs[GDI_GPU_TIMESTAMP_MAX] = {};
    void CreateTimestampPool();
    void CollectTimestamps();

    // Set by RT resize (render thread), cleared by EndRendering; read by the
    // UI-thread present to skip frames while targets are being recreated.
    std::atomic<bool>       bResizing{false};

    // All live VKRenderTargets (render thread only — creation, destruction and
    // SetRenderTarget all run on the render thread).
    std::unordered_set<VKRenderTarget*>  LiveRenderTargets;

    // Deferred-destruction queues (render thread only). Drained after each
    // frame-fence wait (BeginRendering / FlushForReadback) and at Destroy.
    struct DeferredImage  { VkImage Image; VkDeviceMemory Memory; VkImageView View; VkSampler Sampler; };
    struct DeferredBuffer { VkBuffer Buffer; VkDeviceMemory Memory; };
    std::vector<DeferredImage>  DeferredImages;
    std::vector<DeferredBuffer> DeferredBuffers;
    std::vector<VkQueryPool>    DeferredQueryPools;   // released occlusion query sets'
};

#endif //CORECLIENT_VULKANDYNAMICGDI_H

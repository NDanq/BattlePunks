#ifndef CORECLIENT_VULKANDYNAMICGDI_H
#define CORECLIENT_VULKANDYNAMICGDI_H

#include <vulkan/vulkan.h>
#include "Public/GDI/Vulkan/GDIVulkanPlatform.h"
#include <unordered_set>
#include <utility>


#include <atomic>

#include "Public/GDI/GDI.h"

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
    void SetDepthState(IDepthStateDesc) override;
    void SetBlendState(IBlendStateDesc) override;
    void SetStencilState(IStencilStateDesc) override;
    void SetColorMask(EColorMask) override;

    void BeginRendering() override;
    void EndRendering() override;

    GDIRenderTarget* CreateRenderTarget(std::string Name, uint InWidth, uint InHeight) override;
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

    GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResourceMips(const EngineCore::Image* InLevels, uint32_t InCount, EGDITextureBorder InBorder, EGDITextureFilter InFilter) override;

    bool CheckError(const std::string& FailStr) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_Vulkan; }
    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    void BeginDrawEvent(const std::string& EventName) override;
    void EndDrawEvent() override;

    bool     SupportsGPUTimestamps() const override { return TimestampPool != VK_NULL_HANDLE; }
    void     WriteGPUTimestamp(uint32_t InIndex) override;
    uint64_t GetPreviousGPUTimestamps(double* OutMilliseconds) override;

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
     * Build the pipeline + descriptor set for the current GDI state and bind them
     * into the active command buffer. Called by GDIMesh::Draw() (VKMesh::Draw)
     * immediately before it issues the vkCmdDraw, so EVERY draw — both the
     * DrawObject path and a direct Mesh->Draw() (layer compositing) — gets a bound
     * pipeline. Returns false (and binds nothing) if state is incomplete.
     */
    bool BindDrawState(GDIMesh* InMesh, bool bIsInstanced);

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

private:
    void EndActiveRenderingRegion();

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

    /** The scene block's last upload, reused while its serial and the ring's
     *  epoch are unchanged: it is set once per scene, not per draw. */
    VkBuffer                SceneBlockUploadBuffer = VK_NULL_HANDLE;
    VkDeviceSize            SceneBlockUploadOffset = 0;
    uint64_t                SceneBlockUploadSerial = ~0ull;
    uint64_t                SceneBlockUploadEpoch  = ~0ull;

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
};

#endif //CORECLIENT_VULKANDYNAMICGDI_H

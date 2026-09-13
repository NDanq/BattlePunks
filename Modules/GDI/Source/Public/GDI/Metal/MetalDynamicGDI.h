#ifndef CORECLIENT_METALDYNAMICGDI_H
#define CORECLIENT_METALDYNAMICGDI_H

#include <atomic>
#include <unordered_set>


#include "Public/GDI/GDI.h"
#include "Public/GDI/Metal/MTLForward.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>

class MTLDevice;
class MTLSync;
class MTLCommandManager;
class MTLSwapchain;
class MTLPipelineCache;
class MTLRenderTarget;
class MTLTextureResource;

/**
 * Metal GDIContext. Mirrors VulkanGDIContext but carries no instance object --
 * Metal has no VkInstance analog. The base GDIContext still allocates a
 * QOffscreenSurface (unused by the Metal path but harmless); QCanvasRenderer
 * moves it and this context onto the render thread.
 */
class MetalGDIContext : public GDIContext
{
public:
    MetalGDIContext();
    ~MetalGDIContext() override;
};

/**
 * Per-frame state captured between BeginRendering / EndRendering -- the Metal
 * counterpart of VKCurrentStateData.
 *
 * The render state is kept in the platform-neutral GDI structs rather than
 * pre-baked descriptors: Metal bakes blend and write-mask into the pipeline
 * object and depth/stencil into a separate state object, both of which are
 * looked up from these values at draw time (MTLPipelineCache).
 */
class MTLCurrentStateData
{
public:
    // Viewport / scissor, in render-target pixels. Kept as plain scalars so this
    // header stays includable from ordinary C++ translation units.
    double  ViewportX = 0.0, ViewportY = 0.0, ViewportW = 0.0, ViewportH = 0.0;
    uint    ScissorX  = 0,   ScissorY  = 0,   ScissorW  = 0,   ScissorH  = 0;

    IDepthStateDesc     DepthState;
    IStencilStateDesc   StencilState;
    IBlendStateDesc     BlendState;
    EColorMask          ColorMask = EColM_RGBA;

    /** The RT currently bound via SetRenderTarget (the encoder's color attachment). */
    MTLRenderTarget*    ActiveRenderTarget = nullptr;
    /** True while a render command encoder is open. */
    bool                bRenderingActive   = false;

    MTLCommandBufferRef        CurrentCmd     = nullptr;
    MTLRenderCommandEncoderRef CurrentEncoder = nullptr;

    /**
     * Open pushDebugGroup: regions on the CURRENT encoder. Reset to 0 whenever an
     * encoder is closed, so EndDrawEvent never pops a group whose push landed on an
     * encoder that has already ended (which a mid-frame SetRenderTarget or
     * FlushForReadback would otherwise cause).
     */
    int                 DebugLabelDepth = 0;

    /**
     * MTLFunctions cached by SetBoundShader for the pipeline key. nullptr means no
     * shader is bound and DrawObject will skip the draw.
     */
    MTLFunctionRef      BoundVSFunction = nullptr;
    MTLFunctionRef      BoundFSFunction = nullptr;
};

/**
 * Metal backend for GDIDynamicDriver -- a 1:1 port of VulkanDynamicGDI.
 *
 * What Metal removes relative to the Vulkan backend:
 *   - image layouts and pipeline barriers (no LiveRenderTargets layout sweep),
 *   - deferred destruction (a committed command buffer retains what it references),
 *   - descriptor sets and the UBO ring allocator (uniforms go up as setVertexBytes/
 *     setFragmentBytes; every block here is well under the 4 KB limit).
 *
 * What it keeps:
 *   - one frame in flight, paced by MTLCommandManager,
 *   - the render thread draws into offscreen render targets and the UI thread
 *     blits the Main RT into the window's CAMetalLayer (MTLSwapchain),
 *   - mid-frame FlushForReadback for synchronous readbacks and resizes.
 *
 * Render/present ordering: both threads submit to the SINGLE MTLCommandQueue owned
 * by MTLDevice. Metal executes command buffers in commit order on a queue and
 * hazard-tracks the render targets between them, which is exactly the guarantee
 * the Vulkan backend had to build by hand out of a queue mutex, a timeline
 * semaphore and an ALL_COMMANDS layout barrier.
 */
class MetalDynamicGDI : public GDIDynamicDriver
{
public:
    explicit MetalDynamicGDI(EngineCore::WindowHandle InWindow);
    ~MetalDynamicGDI();

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
    void ReleaseRenderTarget(GDIRenderTarget* InRenderTarget) override;

    GDIShaderResource* CreateShaderResource(EShaderType InType, const std::string& SourceCode) override;
    void DrawScreenQuadPrimitive() override;

    // Single-arg DrawObject uses the base forward-to-two-arg behavior, so the
    // two-arg overload's core+user uniform flush runs for every draw.
    using GDIDynamicDriver::DrawObject;
    void DrawObject(class GDIViewObject*, class GDIView*) override;
    void SetBoundShader(GDIShader*, GDIShader*) override;

    GDIMesh* CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) override;
    GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) override;

    GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    bool CheckError(const std::string& FailStr) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_Metal; }
    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    void BeginDrawEvent(const std::string& EventName) override;
    void EndDrawEvent() override;

    /** Metal present path: the UI thread blits the offscreen Main RT into the
     *  window's CAMetalLayer drawable. */
    void PresentToWindow(EngineCore::WindowHandle InWindow) override;

    /**
     * Commit all commands recorded so far this frame and block until the GPU
     * finishes, then resume recording into a fresh command buffer (restoring the
     * viewport and re-opening the pass on the active RT). Used by synchronous
     * readbacks (MTLRenderTarget::ToImage, GetColorFromSamplePoint) that must
     * observe pixels just drawn but not yet committed.
     *
     * @param bResumeActiveRegion Re-open the pass on the previously active RT after
     *        the flush. Pass false when that RT is about to be destroyed (resize) so
     *        the fresh command buffer never references it.
     */
    void FlushForReadback(bool bResumeActiveRegion = true);

    /**
     * Record a full-texture copy Src -> Dst into the active command buffer (closing
     * any open encoder first). Backs MTLRenderTarget::ResolveRenderTarget, which
     * the layer manager uses to back up / restore the canvas buffer.
     */
    void RecordResolve(class MTLRenderTarget* Src, class MTLRenderTarget* Dst);

    /**
     * Build and bind the pipeline, depth/stencil state, uniforms and textures for
     * the current GDI state. Called by MTLMesh::Draw() immediately before the
     * draw call, so EVERY draw -- the DrawObject path and a direct Mesh->Draw()
     * (layer compositing) alike -- gets a bound pipeline. Returns false (and binds
     * nothing) if the state is incomplete.
     */
    bool BindDrawState(GDIMesh* InMesh, bool bIsInstanced);

    /**
     * Marks that render targets are being resized (render thread). While set, the
     * UI-thread present skips entirely -- the offscreen RTs are mid-recreation.
     * Cleared by EndRendering once a full frame has been rendered into the new
     * targets, so the next present is valid.
     */
    inline void SetResizing(bool b) { bResizing.store(b); }

    /**
     * Render-target registry (render thread only). Unlike Vulkan's, this exists
     * purely so a destroyed RT can be cleared out of the current state instead of
     * leaving a dangling ActiveRenderTarget -- there are no layouts to sweep.
     */
    void RegisterRenderTarget(MTLRenderTarget* InRT);
    void UnregisterRenderTarget(MTLRenderTarget* InRT);

    /** Block until every command buffer submitted by this driver has completed. */
    void WaitIdle();

    MTLDevice*          GetDeviceObj()      const { return DeviceObj; }
    MTLCommandManager* GetCommandManager() const { return CommandManager; }
    MTLSwapchain*       GetSwapchain()      const { return Swapchain; }
    MTLPipelineCache*   GetPipelineCache()  const { return PipelineCache; }

private:
    /** Close the open render command encoder, if any. */
    void EndActiveEncoder();

    /**
     * Open a render command encoder targeting InRT.
     * @param bClear      Use MTLLoadActionClear instead of Load.
     * @param ClearColor  Clear value; ignored unless bClear.
     */
    void BeginEncoderForRT(MTLRenderTarget* InRT, bool bClear, const EngineCore::Color& ClearColor);

    /** Apply viewport + scissor to the freshly opened encoder. */
    void ApplyViewportToEncoder();

    // NOTE: the context lives in GDIDynamicDriver::Context (a GDIContext*) -- do not
    // redeclare it here, or AssignContext would populate a shadowing member that
    // nothing else reads.
    MTLDevice*            DeviceObj        = nullptr;
    MTLCommandManager*    CommandManager   = nullptr;
    MTLSwapchain*         Swapchain        = nullptr;
    MTLPipelineCache*     PipelineCache    = nullptr;
    MTLCurrentStateData* CurrentStateData = nullptr;

    /** Lazily-created full-screen NDC quad for DrawScreenQuadPrimitive. */
    GDIMesh*              ScreenMesh       = nullptr;
    /** 1x1 transparent texture used as a fallback for unbound sampler slots. */
    MTLTextureResource*   NullTexture      = nullptr;

    /**
     * Number of frames the render thread has committed. The UI-thread present uses
     * it only to answer "has anything been rendered yet?"; the actual GPU ordering
     * comes from committing both on the same queue (see the class comment).
     */
    std::atomic<uint64_t> FramesCommitted{0};

    // Set by RT resize (render thread), cleared by EndRendering; read by the
    // UI-thread present to skip frames while targets are being recreated.
    std::atomic<bool>     bResizing{false};

    /** All live MTLRenderTargets (render thread only). */
    std::unordered_set<MTLRenderTarget*> LiveRenderTargets;
};

#endif //CORECLIENT_METALDYNAMICGDI_H

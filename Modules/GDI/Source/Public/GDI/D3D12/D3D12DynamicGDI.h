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

class D3D12Device;
class D3D12CommandManager;
class D3D12Swapchain;
class D3D12RenderTarget;
class D3D12PipelineCache;
class D3D12DescriptorAllocator;
class D3D12ShaderResource;
class D3D12TextureResource;

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

    D3D12ShaderResource*        BoundVS             = nullptr;
    D3D12ShaderResource*        BoundPS             = nullptr;
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
    using GDIDynamicDriver::DrawObject;
    void DrawObject(class GDIViewObject*, class GDIView*) override;
    void SetBoundShader(GDIShader*, GDIShader*) override;

    GDIMesh* CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) override;
    GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) override;
    GDIMesh* CreateMesh(const GDIMeshDesc& InDesc) override;

    GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;
    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    bool CheckError(const std::string& FailStr) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_D3D12; }
    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    void BeginDrawEvent(const std::string& EventName) override;
    void EndDrawEvent() override;

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

    /** Binds pipeline, constant buffers and textures for the next draw. Called by D3D12Mesh::Draw. */
    bool BindDrawState(GDIMesh* InMesh, bool bIsInstanced);

    /** While set, PresentToWindow skips: the offscreen RTs are being recreated. Cleared by EndRendering. */
    inline void SetResizing(bool b) { bResizing.store(b); }

    void RegisterRenderTarget(D3D12RenderTarget* InRT)   { LiveRenderTargets.insert(InRT); }
    void UnregisterRenderTarget(D3D12RenderTarget* InRT) { LiveRenderTargets.erase(InRT); }

    /**
     * GL callers release resources straight after the draw that uses them, while
     * the list referencing them is still open or in flight. These hold them until
     * the next frame-fence wait proves the GPU is done with them.
     */
    void DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource> InResource);
    void DeferFreeSRV(uint32_t InSlot);
    void DrainDeferredDestruction();

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

    D3D12Device*              DeviceObj        = nullptr;
    D3D12CommandManager*      CommandManager   = nullptr;
    /** One per presented window, keyed by its native handle. UI thread only
     *  (and Destroy, once the UI thread no longer presents). */
    std::vector<std::pair<void*, D3D12Swapchain*>> Swapchains;
    D3D12PipelineCache*       PipelineCache    = nullptr;
    D3D12DescriptorAllocator* DescAlloc        = nullptr;
    D3D12CurrentStateData*    CurrentStateData = nullptr;

    GDIMesh*                  ScreenMesh       = nullptr;
    D3D12TextureResource*     NullTexture      = nullptr;

    std::atomic<bool>         bResizing{false};

    std::unordered_set<D3D12RenderTarget*>             LiveRenderTargets;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> DeferredResources;
    std::vector<uint32_t>                               DeferredSRVs;
};

#endif //CORECLIENT_D3D12DYNAMICGDI_H

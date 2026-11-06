#ifndef NIMBLE_METALDYNAMICGDI_H
#define NIMBLE_METALDYNAMICGDI_H

#include <string>
#include <vector>

#include "Public/GDI/GDI.h"

/*
 * The Metal GDI backend -- Apple's only GPU backend (CMakeLists.txt, "Backend
 * selection"), with the D3D12 backend's shape: the same bindings, the same
 * clip-space and orientation conventions (Metal, like D3D12, maps NDC +1 to a
 * target's first row and clips z to [0, 1]), the same draw state.
 *
 * PLAIN C++ ON PURPOSE. GDI.cpp includes this header from a C++ translation
 * unit, and the backend's .mm files compile with ARC, under which an `id`
 * member is a strong reference with a destructor the C++ side would not see.
 * So nothing Objective-C appears here: every Metal object lives in MetalState
 * (Private/GDI/Metal/MTLInternal.h), which only the .mm files can see.
 *
 * Shaders: NimbleCook's Metal target (cook_material_graphs.py) cross-compiles
 * the Vulkan GLSL to MSL with spirv-cross, keeping each SPIR-V binding as the
 * Metal index -- a block at binding N is buffer(N), a sampler2D texture(N) and
 * sampler(N % 16). The backend compiles the MSL source as it loads it.
 *
 * Not yet here (each GDI default: refused, or drawn without): occlusion
 * queries and predication, GPU timestamps, parallel recording.
 */

class MetalGDIContext : public GDIContext
{
public:
    MetalGDIContext()
    {
        ContextType = GDIContext::MetalContext;
    }
};

struct MetalState;
class MTLRenderTargetImpl;
class MTLMeshImpl;

class MetalDynamicGDI : public GDIDynamicDriver
{
public:
    explicit MetalDynamicGDI(EngineCore::WindowHandle InWindow);
    ~MetalDynamicGDI() override;

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

    bool CheckError(const std::string& FailStr) override;
    ERenderPlatform GetRenderPlatform() const override { return ERP_Metal; }
    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    void BeginDrawEvent(const std::string& EventName) override;
    void EndDrawEvent() override;

    bool       SupportsCompute() const override;
    GDIBuffer* CreateBuffer(uint32_t InBytes, uint8_t InUsage, const void* InInitialData) override;
    bool       DispatchCompute(GDIShader* InShader, const void* InParams, uint32_t InParamBytes,
                               GDIBuffer* const* InBuffers, uint32_t InBufferCount, GDITextureResource* InTexture,
                               uint32_t InX, uint32_t InY, uint32_t InZ) override;
    bool       ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes) override;
    bool       UpdateBuffer(GDIBuffer* InBuffer, uint32_t InOffset, const void* InData, uint32_t InBytes) override;
    bool       SupportsIndirectDraw() const override { return SupportsCompute(); }
    bool       SupportsIndexBuffers() const override { return SupportsCompute(); }

    void PresentToWindow(EngineCore::WindowHandle InWindow) override;
    void PresentRenderTargetToWindow(EngineCore::WindowHandle InWindow, GDIRenderTarget* InSource) override;
    void ReleaseWindow(EngineCore::WindowHandle InWindow) override;

    // --- Backend-internal (the .mm files) ----------------------------------

    MetalState* GetState() const { return S; }

    /* Commits what the open block recorded and waits for it, then opens a new
     * command buffer -- and, with bInResume, the same render targets again,
     * their contents kept. Readbacks and resizes go through it. */
    void FlushAndWait(bool bInResume = true);

    /* Blocks until every command buffer committed so far has completed (GSync). */
    void WaitForCommitted();

    /* Draws InMesh with the driver's current state (GDIMesh::Draw). */
    void DrawMesh(MTLMeshImpl* InMesh);

    /* InRT is being destroyed: forget it wherever the driver holds it. */
    void ForgetRenderTarget(MTLRenderTargetImpl* InRT);

    void SetResizing(bool bInResizing);

private:
    MetalState* S = nullptr;
};

#endif // NIMBLE_METALDYNAMICGDI_H

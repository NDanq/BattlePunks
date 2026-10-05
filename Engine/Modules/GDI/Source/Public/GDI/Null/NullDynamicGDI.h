#ifndef CORECLIENT_NULLDYNAMICGDI_H
#define CORECLIENT_NULLDYNAMICGDI_H

#include <cstring>
#include <vector>

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDIMesh.h"
#include "Public/GDI/GDITexture.h"

#include <cstdint>

/*
 * A context type for hosts that want to hand AssignContextGDI() something
 * concrete. Entirely optional -- NullDynamicGDI::AssignContext accepts
 * nullptr just as happily, since it never dereferences its context.
 */
class NullGDIContext : public GDIContext
{
public:
    NullGDIContext()
    {
        ContextType = GDIContext::NullContext;
    }
};

/*
 * Call counters and last-set state, kept for verification (the GDISmokeTest)
 * and for a host that wants to confirm what a "headless" run actually asked
 * the renderer to do. Deliberately NOT logged on the hot path -- a server
 * calling DrawObject every frame must not spam the log -- and deliberately
 * NOT a virtual on GDIDynamicDriver, so the base class vtable stays what
 * every other backend already expects. Reach it via
 * static_cast<NullDynamicGDI*>(GDynamicGDI)->GetStats().
 *
 * Plain integers: these are read and written only from the render thread,
 * same as the rest of NullDynamicGDI's state. If that ever changes, promote
 * them to std::atomic<uint64_t> the way GDI.cpp's own queue diagnostics do.
 */
struct NullGDIStats
{
    uint64_t ClearCalls            = 0;
    uint64_t SwapBufferCalls       = 0;
    uint64_t FlushCalls            = 0;
    uint64_t BeginRenderingCalls   = 0;
    uint64_t EndRenderingCalls     = 0;
    uint64_t DrawObjectCalls       = 0;
    uint64_t DrawScreenQuadCalls   = 0;
    uint64_t RenderTargetsCreated  = 0;
    uint64_t MeshesCreated         = 0;
    uint64_t ShaderResourcesCreated  = 0;
    uint64_t TextureResourcesCreated = 0;

    EngineCore::Color LastClearColor;
    uint LastViewportX = 0, LastViewportY = 0, LastViewportWidth = 0, LastViewportHeight = 0;
    /* The backend's scissor: the target's rows, row 0 the top (SetScissorRect). */
    uint LastScissorX = 0, LastScissorY = 0, LastScissorWidth = 0, LastScissorHeight = 0;
    uint64_t ScissorCalls          = 0;
    uint64_t DynamicMeshesCreated  = 0;
    uint64_t BuffersCreated        = 0;
    uint64_t DispatchCalls         = 0;
    uint32_t LastDispatchGroups[3] = {};
    uint32_t LastDispatchBuffers   = 0;
    uint32_t LastDispatchParamBytes = 0;
    uint64_t InstancedDrawCalls    = 0;   // draws with GDISetInstanceBuffer's buffer set
    uint32_t LastInstanceCount     = 0;
    GDIShader* LastBoundVertexShader = nullptr;
    GDIShader* LastBoundPixelShader  = nullptr;
};

/* A Null buffer: its bytes on the CPU, which ReadBuffer returns. Dispatches
 * run nothing, so they stay what they were made with. */
class NullBuffer final : public GDIBuffer
{
public:
    NullBuffer(const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
        : GDIBuffer(InBytes, InUsage), Data(InBytes, 0)
    {
        if(InInitialData) { std::memcpy(Data.data(), InInitialData, InBytes); }
    }
    void Release() override { delete this; }
    std::vector<uint8_t> Data;
};

/*
 * The no-op GDI backend. Selected explicitly with InitializeGDI(ERP_Null, ...)
 * -- see the note at GDI.cpp's InitializeGDI -- never as a fallback for a
 * missing GPU backend. Intended for a process (a dedicated server, a batch
 * tool) that shares its call graph with client code that issues GDI calls,
 * but has no window, no GPU context, and possibly no GPU driver at all.
 *
 * Every Create* below returns a real, owned object rather than nullptr:
 * GDIRenderTargetManager::Resize()/Release() and
 * GDIShader::UpdateUserDefinedUniforms() dereference their driver-created
 * objects with no null check, so a null-returning "null" backend would be a
 * crash on the first real use, not a no-op.
 */
class NullDynamicGDI : public GDIDynamicDriver
{
public:
    explicit NullDynamicGDI(EngineCore::WindowHandle InWindow) : GDIDynamicDriver(InWindow) {}

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

    void SetRenderTarget(GDIRenderTarget* InRenderTarget) override;
    void SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom) override;

    void ReleaseRenderTarget(GDIRenderTarget* InRenderTarget) override;

    GDIShaderResource* CreateShaderResource(EShaderType InType, const std::string& SrcCode) override;

    void DrawScreenQuadPrimitive() override;

    void DrawObject(class GDIViewObject*) override;

    void DrawObject(class GDIViewObject*, class GDIView*) override;

    void BeginDrawEvent(const std::string& EventName) override;
    bool HasDrawEvents() const override { return false; }

    void EndDrawEvent() override;

    void SetBoundShader(GDIShader*, GDIShader*) override;

    GDIMesh* CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) override;

    GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) override;

    GDIMesh* CreateMesh(const GDIMeshDesc& InDesc) override;

    GDIMesh* CreateDynamicMesh(uint32_t InMaxVertices, uint32_t InMaxIndices) override;

    GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    bool CheckError(const std::string& FailStr) override;

    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_Null; }

    /* Compute "supported": buffers are created and dispatches counted, so a
     * caller's whole path runs without a GPU. Nothing executes. */
    bool       SupportsCompute() const override { return true; }
    GDIBuffer* CreateBuffer(uint32_t InBytes, uint8_t InUsage, const void* InInitialData) override;
    bool       DispatchCompute(GDIShader* InShader, const void* InParams, uint32_t InParamBytes,
                               GDIBuffer* const* InBuffers, uint32_t InBufferCount,
                               uint32_t InX, uint32_t InY, uint32_t InZ) override;
    bool       ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes) override;

    const NullGDIStats& GetStats() const { return Stats; }
    void ResetStats() { Stats = NullGDIStats(); }

private:
    NullGDIStats Stats;
};

#endif //CORECLIENT_NULLDYNAMICGDI_H

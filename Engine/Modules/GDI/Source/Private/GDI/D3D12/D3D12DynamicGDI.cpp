#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12CommandManager.h"
#include "Public/GDI/D3D12/D3D12Swapchain.h"
#include "Public/GDI/D3D12/D3D12RenderTarget.h"
#include "Public/GDI/D3D12/D3D12Sync.h"
#include "Public/GDI/D3D12/D3D12Mesh.h"
#include "Public/GDI/D3D12/D3D12PipelineCache.h"
#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Public/GDI/D3D12/D3D12Shader.h"
#include "Public/GDI/D3D12/D3D12Texture.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDIView.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "Public/Core/CoreString.h"
#include "Public/GDI/GDILog.h"

// At global scope, not inside a module namespace, so these are wider than the
// usual directive -- contained only because nothing includes a .cpp.
using namespace EngineCore;
using namespace D3D12Ops;
using namespace D3D12Util;

extern GDISync* GSync;

#pragma region D3D12DynamicGDI

D3D12DynamicGDI::D3D12DynamicGDI(WindowHandle InWindow)
    : GDIDynamicDriver(InWindow)
{
}

D3D12DynamicGDI::~D3D12DynamicGDI()
{
    Destroy();
}

void D3D12DynamicGDI::AssignContext(GDIContext* InContext)
{
    Context = InContext;
}

void D3D12DynamicGDI::Allocate()
{
    if(bInitialized) return;

    SetThreadID(appThreadID());

    if(!Context || Context->ContextType != GDIContext::D3D12Context)
    {
        GDI_LOG("[D3D12DynamicGDI] No D3D12GDIContext assigned.")
        return;
    }

    DeviceObj = new D3D12Device();
    if(!DeviceObj->Initialize())
    {
        GDI_LOG("[D3D12DynamicGDI] Device init failed.")
        return;
    }

    CommandManager = new D3D12CommandManager();
    if(!CommandManager->Initialize(DeviceObj))
    {
        GDI_LOG("[D3D12DynamicGDI] CommandManager init failed.")
        return;
    }

    if(auto* Sync = dynamic_cast<D3D12Sync*>(GSync))
    {
        Sync->Bind(DeviceObj);
    }

    // Created lazily by the first PresentToWindow, on the UI thread.
    Swapchain = new D3D12Swapchain();

    PipelineCache = new D3D12PipelineCache();
    if(!PipelineCache->Initialize(DeviceObj))
    {
        GDI_LOG("[D3D12DynamicGDI] PipelineCache init failed.")
        return;
    }

    DescAlloc = new D3D12DescriptorAllocator();
    if(!DescAlloc->Initialize(DeviceObj))
    {
        GDI_LOG("[D3D12DynamicGDI] DescriptorAllocator init failed.")
        return;
    }

    // Transparent 1x1 fallback for draws whose texture slot is unbound.
    Image NullImg(1, 1, EImageFormat::RGBA8);
    NullTexture = new D3D12TextureResource(DeviceObj, NullImg, GDI_TB_Repeat, GDI_TF_Nearest);

    CurrentStateData = new D3D12CurrentStateData();

    bInitialized = true;
    GDI_LOG("[D3D12DynamicGDI] Allocate complete.")
}

void D3D12DynamicGDI::Destroy()
{
    /* BEFORE the early-out, and before the device goes: cached shaders and
     * textures hold a raw Device pointer -- see ~GDIDynamicDriver. */
    ReleaseCachedResources();

    // Allocate may have failed part way; tear down whatever it built.
    if(!DeviceObj) return;

    if(DeviceObj->IsValid())
    {
        DeviceObj->WaitIdle();
    }

    if(GSync)
    {
        GSync->DestroySync();
    }

    delete CurrentStateData; CurrentStateData = nullptr;

    if(ScreenMesh)  { ScreenMesh->Release();  delete ScreenMesh;  ScreenMesh  = nullptr; }
    if(NullTexture) { NullTexture->Release(); delete NullTexture; NullTexture = nullptr; }

    DrainDeferredDestruction();

    if(Swapchain)      { Swapchain->Shutdown();      delete Swapchain;      Swapchain      = nullptr; }
    if(CommandManager) { CommandManager->Shutdown(); delete CommandManager; CommandManager = nullptr; }
    if(DescAlloc)      { DescAlloc->Shutdown();      delete DescAlloc;      DescAlloc      = nullptr; }
    if(PipelineCache)  { PipelineCache->Shutdown();  delete PipelineCache;  PipelineCache  = nullptr; }

    LiveRenderTargets.clear();

    DeviceObj->Shutdown();
    delete DeviceObj;
    DeviceObj = nullptr;

    bInitialized = false;
}

void D3D12DynamicGDI::SwapBuffers()
{
    // Presenting is the UI thread's job, through PresentToWindow.
}

void D3D12DynamicGDI::Flush()
{
    // EndRendering already submits.
}

void D3D12DynamicGDI::PresentToWindow(WindowHandle InWindow)
{
    if(!bInitialized || !Swapchain || !InWindow.IsValid()) return;
    if(bResizing.load()) return;

    if(!Swapchain->IsValid() && !Swapchain->Initialize(DeviceObj, InWindow))
    {
        GDI_LOG("[D3D12DynamicGDI] Swapchain initialize failed.")
        return;
    }

    // The renderer rebinds Main right before EndRendering, so the last active RT is the frame.
    D3D12RenderTarget* MainRT = CurrentStateData ? CurrentStateData->ActiveRenderTarget : nullptr;
    if(!MainRT) return;

    Swapchain->PresentFrame(MainRT);
}

void D3D12DynamicGDI::PrepareCommandList(ID3D12GraphicsCommandList* InList)
{
    ID3D12DescriptorHeap* Heaps[] = { DescAlloc->GetSRVHeap(), DescAlloc->GetSamplerHeap() };
    InList->SetDescriptorHeaps(2, Heaps);
    InList->SetGraphicsRootSignature(PipelineCache->GetRootSignature());
    InList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D12DynamicGDI::SubmitFrame()
{
    CommandManager->EndFrame();

    std::lock_guard<std::mutex> QueueLock(DeviceObj->GetQueueMutex());
    for(D3D12RenderTarget* RT : LiveRenderTargets)
    {
        RT->PublishSubmittedState();
    }
    CommandManager->SubmitLocked();
}

GDIRenderTarget* D3D12DynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    if(!DeviceObj) return nullptr;
    auto* RT = new D3D12RenderTarget(DeviceObj, Name, InWidth, InHeight);
    RegisterRenderTarget(RT);
    return RT;
}

void D3D12DynamicGDI::SetRenderTarget(GDIRenderTarget* InRenderTarget)
{
    auto* NewRT = dynamic_cast<D3D12RenderTarget*>(InRenderTarget);
    if(!NewRT || !NewRT->IsValid() || !CurrentStateData || !CurrentStateData->CurrentCmd) return;

    ID3D12GraphicsCommandList* List = CurrentStateData->CurrentCmd;

    // Every other target still bound for writing becomes sampleable, since
    // layers and stroke buffers are sampled in no particular order.
    for(D3D12RenderTarget* RT : LiveRenderTargets)
    {
        if(RT == NewRT || !RT->IsValid()) continue;
        if(RT->GetCurrentState() != D3D12_RESOURCE_STATE_RENDER_TARGET) continue;

        RecordTransition(List, RT->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        RT->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    RecordTransition(List, NewRT->GetResource(), NewRT->GetCurrentState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    NewRT->SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE RTV = NewRT->GetRTV();
    if(NewRT->HasDepth())
    {
        // Lives in DEPTH_WRITE, so needs no transition; cleared the first time
        // it is bound, since a new depth buffer's contents are undefined.
        const D3D12_CPU_DESCRIPTOR_HANDLE DSV = NewRT->GetDSV();
        List->OMSetRenderTargets(1, &RTV, FALSE, &DSV);
        if(NewRT->TakeDepthNeedsClear())
        {
            List->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
    }
    else
    {
        List->OMSetRenderTargets(1, &RTV, FALSE, nullptr);
    }

    CurrentStateData->ActiveRenderTarget = NewRT;
    CurrentStateData->bRenderingActive   = true;
}

void D3D12DynamicGDI::ReleaseRenderTarget(GDIRenderTarget* InRenderTarget)
{
    if(InRenderTarget) InRenderTarget->Release();
}

void D3D12DynamicGDI::DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource> InResource)
{
    if(InResource) DeferredResources.push_back(std::move(InResource));
}

void D3D12DynamicGDI::DeferFreeSRV(uint32_t InSlot)
{
    DeferredSRVs.push_back(InSlot);
}

void D3D12DynamicGDI::DrainDeferredDestruction()
{
    DeferredResources.clear();
    if(DescAlloc)
    {
        for(uint32_t Slot : DeferredSRVs)
        {
            DescAlloc->FreeSRV(Slot);
        }
    }
    DeferredSRVs.clear();
}

void D3D12DynamicGDI::FlushForReadback(bool bResumeActiveRegion)
{
    if(!CommandManager || !CurrentStateData || !DeviceObj || !CurrentStateData->CurrentCmd) return;

    D3D12RenderTarget* RT         = CurrentStateData->ActiveRenderTarget;
    const bool         bWasActive = CurrentStateData->bRenderingActive;

    SubmitFrame();

    // BeginFrame waits for the submit above, so deferred releases are safe to drop too.
    ID3D12GraphicsCommandList* List = CommandManager->BeginFrame();
    DrainDeferredDestruction();

    CurrentStateData->CurrentCmd         = List;
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->bRenderingActive   = false;
    CurrentStateData->DebugEventDepth    = 0;
    if(!List) return;

    PrepareCommandList(List);
    List->RSSetViewports(1, &CurrentStateData->ViewportRect);
    List->RSSetScissorRects(1, &CurrentStateData->ScissorRect);

    if(bResumeActiveRegion && bWasActive && RT)
    {
        SetRenderTarget(RT);
    }
}

void D3D12DynamicGDI::RecordResolve(D3D12RenderTarget* Src, D3D12RenderTarget* Dst)
{
    if(!Src || !Dst || Src == Dst || !CurrentStateData || !CurrentStateData->CurrentCmd) return;
    if(!Src->IsValid() || !Dst->IsValid()) return;

    ID3D12GraphicsCommandList* List = CurrentStateData->CurrentCmd;

    // Unbind first: a copy destination may not also be the bound render target.
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->bRenderingActive   = false;
    List->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    RecordTransition(List, Src->GetResource(), Src->GetCurrentState(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    RecordTransition(List, Dst->GetResource(), Dst->GetCurrentState(), D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION SrcLoc{};
    SrcLoc.pResource = Src->GetResource();
    SrcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION DstLoc{};
    DstLoc.pResource = Dst->GetResource();
    DstLoc.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    const D3D12_BOX Box{ 0, 0, 0, (std::min)(Src->GetWidth(), Dst->GetWidth()), (std::min)(Src->GetHeight(), Dst->GetHeight()), 1 };
    List->CopyTextureRegion(&DstLoc, 0, 0, 0, &SrcLoc, &Box);

    RecordTransition(List, Src->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    RecordTransition(List, Dst->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Src->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Dst->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

#pragma endregion

#pragma region Resource creation

GDIShaderResource* D3D12DynamicGDI::CreateShaderResource(EShaderType InType, const std::string& InSrcPath)
{
    return new D3D12ShaderResource(InType, ProcessShaderFromSource(InSrcPath, ERP_D3D12));
}

void D3D12DynamicGDI::SetBoundShader(GDIShader* In_VS, GDIShader* In_PS)
{
    BoundVertexShader = In_VS;
    BoundPixelShader  = In_PS;
    if(!CurrentStateData) return;

    auto ExtractResource = [](GDIShader* Shader) -> D3D12ShaderResource*
    {
        return (Shader && Shader->Resource) ? static_cast<D3D12ShaderResource*>(Shader->Resource) : nullptr;
    };
    CurrentStateData->BoundVS = ExtractResource(In_VS);
    CurrentStateData->BoundPS = ExtractResource(In_PS);
}

GDIMesh* D3D12DynamicGDI::CreateQuadMesh(const float* VertData, const float* TexData,
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

    auto* Mesh = new D3D12Mesh(DeviceObj, CurrentStateData);
    Mesh->SetRenderData(RenderData);
    Mesh->SetSupportsInstanced(bIsInstanced);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* D3D12DynamicGDI::CreateMesh(const GDIMeshDesc& InDesc)
{
    if(!DeviceObj || !CurrentStateData) return nullptr;

    GDIMesh::ORenderData RenderData;
    if(!GDIMakeIndexedRenderData(InDesc, RenderData)) return nullptr;

    auto* Mesh = new D3D12Mesh(DeviceObj, CurrentStateData);
    Mesh->SetRenderData(RenderData);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* D3D12DynamicGDI::CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced)
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

GDITextureResource* D3D12DynamicGDI::CreateTextureResource(const Image& InImage,
                                                           const EGDITextureBorder InBorder,
                                                           const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new D3D12TextureResource(DeviceObj, InImage, InBorder, InFilter);
}

GDITextureResource* D3D12DynamicGDI::CreateTextureResource(GDITexture2D* InParent,
                                                           const Image& InImage,
                                                           const EGDITextureBorder InBorder,
                                                           const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new D3D12TextureResource(DeviceObj, InParent, InImage, InBorder, InFilter);
}

GDITextureResource* D3D12DynamicGDI::CreateTextureResource(GDITexture2D* InParent,
                                                           const EGDITextureBorder InBorder,
                                                           const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new D3D12TextureResource(DeviceObj, InParent, InBorder, InFilter);
}

#pragma endregion

#pragma region Core draw path

namespace
{
    using ScalarMap = std::unordered_map<std::string, D3D12UniformValue>;

    void WriteScalar(const ScalarMap& S, const char* Name, float* Out, int Count)
    {
        auto It = S.find(Name);
        if(It != S.end())
        {
            memcpy(Out, It->second.Data.v, size_t(Count) * sizeof(float));
        }
    }

    /*
     * ViewParameters, 160 bytes -- the same layout as std140 here:
     *   0 Projection, 64 View, 128 MouseCoords (+4 pad), 144 Resolution
     */
    void PackViewParams(const ScalarMap& S, void* Dst)
    {
        float* D = static_cast<float*>(Dst);
        memset(D, 0, 160);
        WriteScalar(S, "ViewParameters.Projection",  D + 0,  16);
        WriteScalar(S, "ViewParameters.View",        D + 16, 16);
        WriteScalar(S, "ViewParameters.MouseCoords", D + 32, 3);
        WriteScalar(S, "ViewParameters.Resolution",  D + 36, 4);

        // The camera builds a GL projection (z in [-1,1]); D3D clips z to [0,1].
        // z' = 0.5z + 0.5w, column-major. Y is flipped in the cooked vertex
        // shaders instead, so every shader agrees, not just projected ones.
        for(int Col = 0; Col < 4; ++Col)
        {
            D[Col * 4 + 2] = 0.5f * D[Col * 4 + 2] + 0.5f * D[Col * 4 + 3];
        }
    }

    /* Transform, 80 bytes: 0 PositionWorldSpace (+4 pad), 16 Model. */
    void PackTransform(const ScalarMap& S, void* Dst)
    {
        float* D = static_cast<float*>(Dst);
        memset(D, 0, 80);
        WriteScalar(S, "Transform.PositionWorldSpace", D + 0, 3);
        WriteScalar(S, "Transform.Model",              D + 4, 16);
    }

    /* Every non-core scalar in map order; each custom cbuffer holds one today. */
    void PackCustom(const ScalarMap& S, void* Dst, uint32_t MaxBytes)
    {
        uint8_t* D = static_cast<uint8_t*>(Dst);
        memset(D, 0, MaxBytes);

        uint32_t Offset = 0;
        for(const auto& Entry : S)
        {
            if(StartsWith(Entry.first, "ViewParameters.") || StartsWith(Entry.first, "Transform.")) continue;

            const D3D12UniformValue& V = Entry.second;
            float    IntAsFloat = 0.f;
            uint32_t Size       = 0;
            const void* Ptr     = nullptr;
            switch(V.Tag)
            {
                case D3D12UniformValue::INT:   IntAsFloat = float(V.Data.i); Size = 4;  Ptr = &IntAsFloat; break;
                case D3D12UniformValue::FLOAT: Size = 4;  Ptr = &V.Data.f; break;
                case D3D12UniformValue::VEC2:  Size = 8;  Ptr = V.Data.v;  break;
                case D3D12UniformValue::VEC3:  Size = 12; Ptr = V.Data.v;  break;
                case D3D12UniformValue::VEC4:  Size = 16; Ptr = V.Data.v;  break;
                case D3D12UniformValue::MAT:   Size = 64; Ptr = V.Data.v;  break;
            }
            if(!Ptr || Offset + Size > MaxBytes) continue;
            memcpy(D + Offset, Ptr, Size);
            Offset += Size;
        }
    }
}

bool D3D12DynamicGDI::BindDrawState(GDIMesh* InMesh, bool bIsInstanced)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive || !CurrentStateData->CurrentCmd) return false;
    if(!InMesh || !PipelineCache || !DescAlloc || !CurrentStateData->ActiveRenderTarget) return false;

    D3D12ShaderResource* VSRes = CurrentStateData->BoundVS;
    D3D12ShaderResource* PSRes = CurrentStateData->BoundPS;
    if(!VSRes || !PSRes) return false;

    const IBlendStateDesc& Blend = CurrentStateData->BlendState;

    D3D12PipelineCache::PipelineKey Key;
    Key.VertexShaderID = VSRes->GetID();
    Key.PixelShaderID  = PSRes->GetID();
    Key.VertexLayoutID = InMesh->IsIndexed() ? 2u : (bIsInstanced ? 1u : 0u);
    Key.BlendEnable    = Blend.bBlend ? TRUE : FALSE;
    Key.SrcBlend       = DecodeBlendFactor(Blend.Source);
    Key.DestBlend      = DecodeBlendFactor(Blend.Dest);
    Key.BlendOp        = DecodeBlendOp(Blend.Op);
    Key.BlendOpAlpha   = DecodeBlendOp(Blend.AlphaOp);
    Key.WriteMask      = DecodeColorMask(CurrentStateData->ColorMask);
    Key.RTFormat       = CurrentStateData->ActiveRenderTarget->GetFormat();
    Key.DSVFormat      = CurrentStateData->ActiveRenderTarget->GetDepthFormat();
    Key.DepthEnable    = CurrentStateData->DepthState.bDepthTest ? TRUE : FALSE;
    Key.DepthWrite     = CurrentStateData->DepthState.bDepthMask ? TRUE : FALSE;
    Key.DepthFunc      = DecodeCompareFunc(CurrentStateData->DepthState.DepthMode);

    ID3D12PipelineState* Pipeline = PipelineCache->GetOrCreate(Key, VSRes, PSRes);
    if(!Pipeline) return false;

    // Both stages carry identical core uniforms; the PS copy also has the custom ones.
    const ScalarMap& Scalars = PSRes->GetScalars();

    static constexpr uint32_t kCustomBytes = 256;
    const D3D12CBAllocation VP     = DescAlloc->AllocateCB(160);
    const D3D12CBAllocation TR     = DescAlloc->AllocateCB(80);
    const D3D12CBAllocation Custom = DescAlloc->AllocateCB(kCustomBytes);
    if(!VP.MappedPtr || !TR.MappedPtr || !Custom.MappedPtr) return false;

    PackViewParams(Scalars, VP.MappedPtr);
    PackTransform(Scalars, TR.MappedPtr);
    PackCustom(Scalars, Custom.MappedPtr, kCustomBytes);

    GDITextureResource* TexRes = nullptr;
    for(const auto& Entry : PSRes->GetTextures())
    {
        if(Entry.second.Index == 0)
        {
            TexRes = Entry.second.Resource;
            break;
        }
    }
    auto* Texture = dynamic_cast<D3D12TextureResource*>(TexRes);
    if(!Texture || Texture->GetSRVSlot() == UINT32_MAX) Texture = NullTexture;
    if(!Texture || Texture->GetSRVSlot() == UINT32_MAX) return false;

    const D3D12_GPU_DESCRIPTOR_HANDLE SRV     = DescAlloc->GetSRVGPUHandle(Texture->GetSRVSlot());
    const D3D12_GPU_DESCRIPTOR_HANDLE Sampler = DescAlloc->GetSamplerGPUHandle(Texture->GetBorder(), Texture->GetFilter());

    ID3D12GraphicsCommandList* List = CurrentStateData->CurrentCmd;
    List->SetPipelineState(Pipeline);
    List->SetGraphicsRootConstantBufferView(RP_ViewParameters, VP.GPUAddress);
    List->SetGraphicsRootConstantBufferView(RP_Transform,      TR.GPUAddress);
    List->SetGraphicsRootConstantBufferView(RP_Custom,         Custom.GPUAddress);

    // Shaders sample at t2 or t3 depending on whether they declare a custom
    // cbuffer; binding the one texture to both saves re-deriving which.
    List->SetGraphicsRootDescriptorTable(RP_Texture2, SRV);
    List->SetGraphicsRootDescriptorTable(RP_Texture3, SRV);
    List->SetGraphicsRootDescriptorTable(RP_Sampler2, Sampler);
    List->SetGraphicsRootDescriptorTable(RP_Sampler3, Sampler);
    return true;
}

void D3D12DynamicGDI::DrawObject(GDIViewObject* InObject, GDIView* InView)
{
    if(!InObject || !InObject->Mesh) return;

    if(BoundVertexShader) BoundVertexShader->UpdateCoreUniforms(InView, InObject->Transform);
    if(BoundPixelShader)  BoundPixelShader->UpdateCoreUniforms(InView, InObject->Transform);

    InObject->Mesh->Draw();
}

void D3D12DynamicGDI::DrawScreenQuadPrimitive()
{
    if(!DeviceObj || !CurrentStateData) return;

    // Flushes e.g. Screen_PS's blit texture into the resource, as the GL path does.
    if(BoundPixelShader) BoundPixelShader->UpdateUserDefinedUniforms();

    if(!ScreenMesh)
    {
        const float Verts[] = {
            -1.f, -1.f, 0.f,
            -1.f,  1.f, 0.f,
             1.f,  1.f, 0.f,
             1.f,  1.f, 0.f,
             1.f, -1.f, 0.f,
            -1.f, -1.f, 0.f
        };
        const float UVs[] = {
            0.f, 0.f,
            0.f, 1.f,
            1.f, 1.f,
            1.f, 1.f,
            1.f, 0.f,
            0.f, 0.f
        };
        ScreenMesh = CreateQuadMesh(Verts, UVs, 18, 12, false);
    }

    if(ScreenMesh) ScreenMesh->Draw();
}

#pragma endregion

#pragma region Misc

bool D3D12DynamicGDI::CheckError(const std::string&)
{
    if(!DeviceObj || !DeviceObj->GetDevice()) return false;

    const HRESULT Reason = DeviceObj->GetDevice()->GetDeviceRemovedReason();
    if(FAILED(Reason))
    {
        GDI_LOG_ERROR("[D3D12DynamicGDI] Device removed (" + GDIHex(uint32_t(Reason)) + ").")
        return true;
    }
    return false;
}

Color D3D12DynamicGDI::GetColorFromSamplePoint(const Point& InPoint, const Size& InSize)
{
    if(!DeviceObj || !CurrentStateData) return Color();

    FlushForReadback();

    D3D12RenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT || !RT->IsValid()) return Color();

    // RT rows are GL-ordered; flip from top-down screen space like glReadPixels(x, height - y).
    const int X = InPoint.X;
    const int Y = InSize.Height - InPoint.Y;
    if(X < 0 || Y < 0 || X >= int(RT->GetWidth()) || Y >= int(RT->GetHeight())) return Color();

    std::vector<uint8_t> Pixel;
    if(!ReadbackTexture(DeviceObj, RT->GetResource(), RT->GetCurrentState(), uint32_t(X), uint32_t(Y), 1, 1, Pixel))
    {
        return Color();
    }
    return Color(Pixel[0], Pixel[1], Pixel[2]);
}

#pragma endregion

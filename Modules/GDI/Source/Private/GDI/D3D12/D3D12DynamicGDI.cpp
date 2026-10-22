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
#include "Private/GDI/GDICorePack.h"
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

    /* [Rendering] FramesInFlight (GDI.cpp): the command allocators, the
     * constant-buffer ring's regions, timestamps and dynamic meshes' regions
     * are per frame slot, each reused once its slot's fence is waited. */
    FramesInFlightCount = GDIGetConfiguredFramesInFlight();
    FramesInFlight      = FramesInFlightCount;
    FrameSlot           = 0;

    CommandManager = new D3D12CommandManager();
    if(!CommandManager->Initialize(DeviceObj, FramesInFlightCount))
    {
        GDI_LOG("[D3D12DynamicGDI] CommandManager init failed.")
        return;
    }

    if(auto* Sync = dynamic_cast<D3D12Sync*>(GSync))
    {
        Sync->Bind(DeviceObj);
    }

    // Swapchains are created on the UI thread, one per window, by the first
    // present to it (FindOrCreateSwapchain).

    PipelineCache = new D3D12PipelineCache();
    if(!PipelineCache->Initialize(DeviceObj))
    {
        GDI_LOG("[D3D12DynamicGDI] PipelineCache init failed.")
        return;
    }

    DescAlloc = new D3D12DescriptorAllocator();
    if(!DescAlloc->Initialize(DeviceObj, FramesInFlightCount))
    {
        GDI_LOG("[D3D12DynamicGDI] DescriptorAllocator init failed.")
        return;
    }

    // Transparent 1x1 fallback for draws whose texture slot is unbound.
    Image NullImg(1, 1, EImageFormat::RGBA8);
    NullTexture = new D3D12TextureResource(DeviceObj, NullImg, GDI_TB_Repeat, GDI_TF_Nearest);

    CurrentStateData = new D3D12CurrentStateData();

    CreateTimestampHeap();

    bInitialized = true;
    GDI_LOG("[D3D12DynamicGDI] Allocate complete: " + std::to_string(FramesInFlightCount)
            + " frame(s) in flight ([Rendering] FramesInFlight).")
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
    DestroyRecordingContexts();

    if(GSync)
    {
        GSync->DestroySync();
    }

    delete CurrentStateData; CurrentStateData = nullptr;

    TimestampHeap.Reset();
    TimestampReadback.Reset();

    if(ScreenMesh)  { ScreenMesh->Release();  delete ScreenMesh;  ScreenMesh  = nullptr; }
    if(NullTexture) { NullTexture->Release(); delete NullTexture; NullTexture = nullptr; }

    DrainDeferredDestruction(true);

    for(auto& Entry : Swapchains) { Entry.second->Shutdown(); delete Entry.second; }
    Swapchains.clear();
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
    // The renderer rebinds Main right before EndRendering, so the last active RT is the frame.
    PresentToSwapchain(InWindow, CurrentStateData ? CurrentStateData->ActiveRenderTarget : nullptr);
}

void D3D12DynamicGDI::PresentRenderTargetToWindow(WindowHandle InWindow, GDIRenderTarget* InSource)
{
    PresentToSwapchain(InWindow, static_cast<D3D12RenderTarget*>(InSource));
}

void D3D12DynamicGDI::PresentToSwapchain(WindowHandle InWindow, D3D12RenderTarget* InRT)
{
    if(!bInitialized || !InWindow.IsValid() || !InRT) return;
    if(bResizing.load()) return;

    if(D3D12Swapchain* Swapchain = FindOrCreateSwapchain(InWindow))
    {
        Swapchain->PresentFrame(InRT);
    }
}

D3D12Swapchain* D3D12DynamicGDI::FindOrCreateSwapchain(WindowHandle InWindow)
{
    for(const auto& Entry : Swapchains)
    {
        if(Entry.first == InWindow.Native) return Entry.second;
    }

    auto* Swapchain = new D3D12Swapchain();
    if(!Swapchain->Initialize(DeviceObj, InWindow))
    {
        GDI_LOG("[D3D12DynamicGDI] Swapchain initialize failed.")
        delete Swapchain;
        return nullptr;
    }
    Swapchains.emplace_back(InWindow.Native, Swapchain);
    return Swapchain;
}

void D3D12DynamicGDI::ReleaseWindow(WindowHandle InWindow)
{
    for(auto It = Swapchains.begin(); It != Swapchains.end(); ++It)
    {
        if(It->first == InWindow.Native)
        {
            // Its last present may still be in flight.
            if(DeviceObj && DeviceObj->IsValid()) { DeviceObj->WaitIdle(); }
            It->second->Shutdown();
            delete It->second;
            Swapchains.erase(It);
            return;
        }
    }
}

void D3D12DynamicGDI::PrepareCommandList(ID3D12GraphicsCommandList* InList)
{
    ID3D12DescriptorHeap* Heaps[] = { DescAlloc->GetSRVHeap(), DescAlloc->GetSamplerHeap() };
    InList->SetDescriptorHeaps(2, Heaps);
    InList->SetGraphicsRootSignature(PipelineCache->GetRootSignature());
    InList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(CurrentStateData && InList == CurrentStateData->CurrentCmd) { DrawCaches.ForgetBound(); }   // a chunk's: BeginParallelPass
}

void D3D12DynamicGDI::SubmitFrame()
{
    ResolveTimestamps(CommandManager->GetCommandList());
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

GDIRenderTarget* D3D12DynamicGDI::CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat)
{
    if(!DeviceObj) return nullptr;
    const DXGI_FORMAT Format = InFormat == GDI_RTF_RGBA16F ? DXGI_FORMAT_R16G16B16A16_FLOAT
                             : InFormat == GDI_RTF_RGB10A2 ? DXGI_FORMAT_R10G10B10A2_UNORM
                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    auto* RT = new D3D12RenderTarget(DeviceObj, Name, InWidth, InHeight, Format);
    RegisterRenderTarget(RT);
    return RT;
}

bool D3D12DynamicGDI::SupportsRenderTargetFormat(const EGDIRenderTargetFormat InFormat) const
{
    /* R16G16B16A16_FLOAT and R10G10B10A2_UNORM render targets, blending and sampling
     * are required at feature level 11_0. */
    return InFormat == GDI_RTF_RGBA8 || InFormat == GDI_RTF_RGBA16F || InFormat == GDI_RTF_RGB10A2;
}

void D3D12DynamicGDI::SetRenderTarget(GDIRenderTarget* InRenderTarget)
{
    SetRenderTargets(&InRenderTarget, 1, InRenderTarget);
}

void D3D12DynamicGDI::SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom)
{
    if(!CurrentStateData || !CurrentStateData->CurrentCmd || !InColors || InCount == 0) return;
    if(InCount > GDI_MAX_COLOR_TARGETS) InCount = GDI_MAX_COLOR_TARGETS;

    D3D12RenderTarget* Colors[GDI_MAX_COLOR_TARGETS] = {};
    for(uint Index = 0; Index < InCount; ++Index)
    {
        Colors[Index] = dynamic_cast<D3D12RenderTarget*>(InColors[Index]);
        if(!Colors[Index] || !Colors[Index]->IsValid()) return;
        if(Colors[Index]->GetWidth() != Colors[0]->GetWidth() || Colors[Index]->GetHeight() != Colors[0]->GetHeight())
        {
            GDI_LOG("[D3D12DynamicGDI] SetRenderTargets: targets of different sizes; nothing bound")
            return;
        }
    }
    D3D12RenderTarget* DepthRT = dynamic_cast<D3D12RenderTarget*>(InDepthFrom);
    if(DepthRT && (!DepthRT->HasDepth() || DepthRT->GetWidth() != Colors[0]->GetWidth()
                   || DepthRT->GetHeight() != Colors[0]->GetHeight()))
    {
        DepthRT = nullptr;
    }
    const auto IsBoundColor = [&](const D3D12RenderTarget* InRT)
    {
        for(uint Index = 0; Index < InCount; ++Index) { if(Colors[Index] == InRT) { return true; } }
        return false;
    };

    ID3D12GraphicsCommandList* List = CurrentStateData->CurrentCmd;

    // Every other target still bound for writing becomes sampleable, since
    // layers and stroke buffers are sampled in no particular order -- and
    // every depth buffer but the one being bound, likewise.
    for(D3D12RenderTarget* RT : LiveRenderTargets)
    {
        if(!RT->IsValid()) continue;
        if(!IsBoundColor(RT) && RT->GetCurrentState() == D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            RecordTransition(List, RT->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            RT->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if(RT != DepthRT && RT->HasDepth() && RT->GetDepthState() == D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            RecordTransition(List, RT->GetDepthResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            RT->SetDepthState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RTVs[GDI_MAX_COLOR_TARGETS]{};
    for(uint Index = 0; Index < InCount; ++Index)
    {
        RecordTransition(List, Colors[Index]->GetResource(), Colors[Index]->GetCurrentState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        Colors[Index]->SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
        RTVs[Index] = Colors[Index]->GetRTV();
    }

    if(DepthRT)
    {
        RecordTransition(List, DepthRT->GetDepthResource(), DepthRT->GetDepthState(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        DepthRT->SetDepthState(D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // Cleared on first bind (contents undefined).
        const D3D12_CPU_DESCRIPTOR_HANDLE DSV = DepthRT->GetDSV();
        List->OMSetRenderTargets(InCount, RTVs, FALSE, &DSV);
        if(DepthRT->TakeDepthNeedsClear())
        {
            List->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
    }
    else
    {
        List->OMSetRenderTargets(InCount, RTVs, FALSE, nullptr);
    }

    CurrentStateData->ActiveRenderTarget = Colors[0];
    for(uint Index = 0; Index < GDI_MAX_COLOR_TARGETS; ++Index)
    {
        CurrentStateData->ActiveColors[Index] = Index < InCount ? Colors[Index] : nullptr;
    }
    CurrentStateData->ActiveColorCount = InCount;
    CurrentStateData->ActiveDepth      = DepthRT;
    CurrentStateData->bRenderingActive = true;
}

void D3D12DynamicGDI::ReleaseRenderTarget(GDIRenderTarget* InRenderTarget)
{
    if(InRenderTarget) InRenderTarget->Release();
}

uint64_t D3D12DynamicGDI::GetRecordingFenceValue() const
{
    return CommandManager ? CommandManager->GetNextFenceValue() : 0;
}

bool D3D12DynamicGDI::IsFrameComplete(const uint64_t InValue) const
{
    return !CommandManager || CommandManager->IsComplete(InValue);
}

void D3D12DynamicGDI::DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource> InResource, const D3D12Allocation& InAllocation)
{
    if(InResource || InAllocation.IsValid()) DeferredResources.push_back({ std::move(InResource), InAllocation, GetRecordingFenceValue() });
}

void D3D12DynamicGDI::DeferReleaseTransient(Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    InAllocator,
                                            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> InList)
{
    DeferredTransients.push_back({ std::move(InAllocator), std::move(InList), GetRecordingFenceValue() });
}

void D3D12DynamicGDI::DeferFreeSRV(uint32_t InSlot)
{
    DeferredSRVs.push_back({ InSlot, GetRecordingFenceValue() });
}

void D3D12DynamicGDI::DeferReleaseQueryHeap(Microsoft::WRL::ComPtr<ID3D12QueryHeap> InHeap)
{
    if(InHeap) DeferredQueryHeaps.push_back({ std::move(InHeap), GetRecordingFenceValue() });
}

void D3D12DynamicGDI::DrainDeferredDestruction(const bool bInAll)
{
    /* Each queue in the order released, so in DoneAt order: what has finished is a prefix. */
    const auto Done = [this, bInAll](const uint64_t InDoneAt) { return bInAll || IsFrameComplete(InDoneAt); };
    size_t Count = 0;
    while(Count < DeferredResources.size() && Done(DeferredResources[Count].DoneAt))
    {
        /* The placed texture first, then its range: never a live resource over a reused range. */
        DeferredResources[Count].Resource.Reset();
        if(DeviceObj) { DeviceObj->GetTextureHeap().Free(DeferredResources[Count].Allocation); }
        ++Count;
    }
    DeferredResources.erase(DeferredResources.begin(), DeferredResources.begin() + ptrdiff_t(Count));

    Count = 0;
    while(Count < DeferredTransients.size() && Done(DeferredTransients[Count].DoneAt)) { ++Count; }
    DeferredTransients.erase(DeferredTransients.begin(), DeferredTransients.begin() + ptrdiff_t(Count));

    if(DeviceObj)
    {
        DeviceObj->GetStagingRing().Reclaim(bInAll || !CommandManager ? ~0ull : CommandManager->GetCompletedValue());
    }

    Count = 0;
    while(Count < DeferredQueryHeaps.size() && Done(DeferredQueryHeaps[Count].DoneAt)) { ++Count; }
    DeferredQueryHeaps.erase(DeferredQueryHeaps.begin(), DeferredQueryHeaps.begin() + ptrdiff_t(Count));

    Count = 0;
    while(Count < DeferredSRVs.size() && Done(DeferredSRVs[Count].DoneAt))
    {
        if(DescAlloc) { DescAlloc->FreeSRV(DeferredSRVs[Count].Slot); }
        ++Count;
    }
    DeferredSRVs.erase(DeferredSRVs.begin(), DeferredSRVs.begin() + ptrdiff_t(Count));
}

void D3D12DynamicGDI::FlushForReadback(bool bResumeActiveRegion)
{
    if(!CommandManager || !CurrentStateData || !DeviceObj || !CurrentStateData->CurrentCmd) return;

    D3D12RenderTarget* RT         = CurrentStateData->ActiveRenderTarget;
    const bool         bWasActive = CurrentStateData->bRenderingActive;
    GDIRenderTarget*   Colors[GDI_MAX_COLOR_TARGETS] = {};
    const uint32_t     ColorCount = CurrentStateData->ActiveColorCount;
    for(uint32_t Index = 0; Index < ColorCount; ++Index) { Colors[Index] = CurrentStateData->ActiveColors[Index]; }
    GDIRenderTarget*   Depth      = CurrentStateData->ActiveDepth;

    SubmitFrame();

    // BeginFrame stays on this slot and waits for the submit above -- and so for
    // every frame before it -- so deferred releases are all safe to drop too.
    ID3D12GraphicsCommandList* List = CommandManager->BeginFrame();
    DrainDeferredDestruction(true);

    CurrentStateData->CurrentCmd         = List;
    FrameListSerial                      = AllocateListSerial();
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->ActiveColorCount   = 0;
    CurrentStateData->ActiveDepth        = nullptr;
    CurrentStateData->bRenderingActive   = false;
    CurrentStateData->DebugEventDepth    = 0;
    CurrentStateData->DebugEventNames.clear();
    if(!List) return;

    PrepareCommandList(List);
    List->RSSetViewports(1, &CurrentStateData->ViewportRect);
    List->RSSetScissorRects(1, &CurrentStateData->ScissorRect);

    if(bResumeActiveRegion && bWasActive && RT && ColorCount > 0)
    {
        SetRenderTargets(Colors, ColorCount, Depth);
    }
}

void D3D12DynamicGDI::RecordResolve(D3D12RenderTarget* Src, D3D12RenderTarget* Dst)
{
    if(!Src || !Dst || Src == Dst || !CurrentStateData || !CurrentStateData->CurrentCmd) return;
    if(!Src->IsValid() || !Dst->IsValid()) return;

    ID3D12GraphicsCommandList* List = CurrentStateData->CurrentCmd;

    // Unbind first: a copy destination may not also be the bound render target.
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->ActiveColorCount   = 0;
    CurrentStateData->ActiveDepth        = nullptr;
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

GDIMesh* D3D12DynamicGDI::CreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    if(!DeviceObj || !CurrentStateData) return nullptr;

    auto* Mesh = new D3D12Mesh(DeviceObj, CurrentStateData);
    if(!Mesh->InitDynamic(InMaxVertices, InMaxIndices, FramesInFlightCount))
    {
        delete Mesh;
        return nullptr;
    }
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

GDITextureResource* D3D12DynamicGDI::CreateTextureResourceMips(const Image* InLevels,
                                                               const uint32_t InCount,
                                                               const EGDITextureBorder InBorder,
                                                               const EGDITextureFilter InFilter)
{
    if(!DeviceObj || !InLevels || InCount == 0) return nullptr;
    return new D3D12TextureResource(DeviceObj, InLevels, InCount, InBorder, InFilter);
}

GDITextureResource* D3D12DynamicGDI::CreateTextureResource(GDITexture2D* InParent,
                                                           const EGDITextureBorder InBorder,
                                                           const EGDITextureFilter InFilter)
{
    if(!DeviceObj) return nullptr;
    return new D3D12TextureResource(DeviceObj, InParent, InBorder, InFilter);
}

namespace
{
    /* The UNORM BC formats. BC1 has no alpha-free variant here: the encoder makes no
     * 3-colour blocks (TextureCompression.cpp), so none reads transparent black. */
    DXGI_FORMAT ToDXGICompressedFormat(const EGDICompressedFormat InFormat)
    {
        switch(InFormat)
        {
            case GDI_CF_BC1: return DXGI_FORMAT_BC1_UNORM;
            case GDI_CF_BC3: return DXGI_FORMAT_BC3_UNORM;
            case GDI_CF_BC4: return DXGI_FORMAT_BC4_UNORM;
            case GDI_CF_BC5: return DXGI_FORMAT_BC5_UNORM;
            case GDI_CF_BC7: return DXGI_FORMAT_BC7_UNORM;
            default: break;
        }
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool D3D12DynamicGDI::GetTextureMemoryStats(GDITextureMemoryStats& OutStats) const
{
    if(!DeviceObj || !DeviceObj->IsValid()) { return false; }
    const D3D12TextureHeap::Stats Heap = DeviceObj->GetTextureHeap().GetStats();
    OutStats.BlockBytes      = Heap.BlockBytes;
    OutStats.BlockUsedBytes  = Heap.UsedBytes;
    OutStats.DedicatedBytes  = Heap.DedicatedBytes;
    OutStats.Blocks          = Heap.Blocks;
    OutStats.BlockImages     = Heap.Allocations;
    OutStats.DedicatedImages = Heap.Dedicated;
    OutStats.StagingBytes    = DeviceObj->GetStagingRing().GetCapacity();
    OutStats.StagingInUse    = DeviceObj->GetStagingRing().GetBytesInUse();
    return true;
}

bool D3D12DynamicGDI::SupportsCompressedFormat(const EGDICompressedFormat InFormat) const
{
    const DXGI_FORMAT Format = ToDXGICompressedFormat(InFormat);
    if(!DeviceObj || !DeviceObj->GetDevice() || Format == DXGI_FORMAT_UNKNOWN) return false;

    // Required at feature level 11_0, but asked rather than assumed.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT Support{ Format };
    return SUCCEEDED(DeviceObj->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &Support, sizeof(Support)))
           && (Support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (Support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
}

GDITextureResource* D3D12DynamicGDI::CreateCompressedTextureResource(const EGDICompressedFormat InFormat,
                                                                     const GDICompressedLevel*  InLevels,
                                                                     const uint32_t             InCount,
                                                                     const EGDITextureBorder    InBorder,
                                                                     const EGDITextureFilter    InFilter)
{
    if(!InLevels || InCount == 0 || !SupportsCompressedFormat(InFormat)) return nullptr;
    /* A BC texture's top level must be whole blocks; the caller decodes any other to RGBA8. */
    if(InLevels[0].Width % 4 != 0 || InLevels[0].Height % 4 != 0) return nullptr;

    auto* Texture = new D3D12TextureResource(DeviceObj, ToDXGICompressedFormat(InFormat), InLevels, InCount, InBorder, InFilter);
    if(Texture->GetSRVSlot() == UINT32_MAX)
    {
        Texture->Release();
        delete Texture;
        return nullptr;
    }
    return Texture;
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
        // z' = 0.5z + 0.5w, column-major. Y is not flipped: D3D12 maps NDC +1
        // to a target's first row, GDI's top-down convention (GDI.h).
        for(int Col = 0; Col < 4; ++Col)
        {
            D[Col * 4 + 2] = 0.5f * D[Col * 4 + 2] + 0.5f * D[Col * 4 + 3];
        }
    }

    /* Transform, 144 bytes: 0 PositionWorldSpace (+4 pad), 16 Model, 80 PrevModel (velocity's; Model unless set). */
    constexpr uint32_t kTransformBytes = 144;
    void PackTransform(const ScalarMap& S, void* Dst)
    {
        float* D = static_cast<float*>(Dst);
        memset(D, 0, kTransformBytes);
        WriteScalar(S, "Transform.PositionWorldSpace", D + 0, 3);
        WriteScalar(S, "Transform.Model",              D + 4, 16);
        WriteScalar(S, "Transform.Model",              D + 20, 16);
        WriteScalar(S, "Transform.PrevModel",          D + 20, 16);
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

bool D3D12DynamicGDI::MakeDrawInputs(D3D12DrawInputs& OutIn)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive || !CurrentStateData->CurrentCmd) return false;
    OutIn = D3D12DrawInputs();
    OutIn.List               = CurrentStateData->CurrentCmd;
    OutIn.ListSerial         = FrameListSerial;
    OutIn.VS                 = CurrentStateData->BoundVS;
    OutIn.PS                 = CurrentStateData->BoundPS;
    OutIn.bMaterial          = IsMaterialDraw();
    OutIn.Blocks             = MaterialBlocks;
    OutIn.Serials            = MaterialBlockSerials;
    OutIn.Textures           = MaterialTextures;
    OutIn.Caches             = &DrawCaches;
    OutIn.InstanceBuffer     = GetInstanceBuffer();
    OutIn.InstanceCount      = GetInstanceCount();
    OutIn.IndirectArgs       = GetIndirectArgs();
    OutIn.IndirectArgsOffset = GetIndirectArgsOffset();
    OutIn.Blend              = &CurrentStateData->BlendState;
    OutIn.ColorMask          = CurrentStateData->ColorMask;
    OutIn.Depth              = &CurrentStateData->DepthState;
    OutIn.PredicateSet       = PredicateSet;
    OutIn.PredicateIndex     = PredicateIndex;
    if(const D3D12ShaderResource* PS = CurrentStateData->BoundPS)
    {
        const D3D12ShaderResource::CoreBlocks& Core = PS->GetCoreBlocks();
        OutIn.CoreView      = Core.bView ? Core.View : nullptr;
        OutIn.CoreTransform = Core.bTransform ? Core.Transform : nullptr;
    }
    return true;
}

bool D3D12DynamicGDI::BindDrawState(const D3D12DrawInputs& InIn, GDIMesh* InMesh, bool bIsInstanced)
{
    if(!InIn.List || !InMesh || !PipelineCache || !DescAlloc || !CurrentStateData || !CurrentStateData->ActiveRenderTarget) return false;

    D3D12ShaderResource* VSRes = InIn.VS;
    D3D12ShaderResource* PSRes = InIn.PS;
    if(!VSRes || !PSRes) return false;

    const IBlendStateDesc& Blend = *InIn.Blend;
    const IDepthStateDesc& Depth = *InIn.Depth;

    D3D12PipelineCache::PipelineKey Key;
    Key.VertexShaderID = VSRes->GetID();
    Key.PixelShaderID  = PSRes->GetID();
    // 3: an indexed mesh drawn instanced from GDISetInstanceBuffer's buffer.
    // 4: a skinned mesh, its bone streams in slots 6 and 7 (never instanced).
    Key.VertexLayoutID = InMesh->IsIndexed() ? (InIn.InstanceBuffer ? 3u : (InMesh->IsSkinned() ? 4u : 2u)) : (bIsInstanced ? 1u : 0u);
    Key.BlendEnable    = Blend.bBlend ? TRUE : FALSE;
    Key.SrcBlend       = DecodeBlendFactor(Blend.Source);
    Key.DestBlend      = DecodeBlendFactor(Blend.Dest);
    Key.BlendOp        = DecodeBlendOp(Blend.Op);
    Key.BlendOpAlpha   = DecodeBlendOp(Blend.AlphaOp);
    Key.WriteMask      = DecodeColorMask(InIn.ColorMask);
    /* The bound targets are the render thread's, unchanged while a pass records. */
    Key.RTFormat       = CurrentStateData->ActiveRenderTarget->GetFormat();
    Key.ColorCount     = CurrentStateData->ActiveColorCount > 0 ? CurrentStateData->ActiveColorCount : 1u;
    for(uint32_t Index = 0; Index < GDI_MAX_COLOR_TARGETS; ++Index)
    {
        const D3D12RenderTarget* Each = Index < CurrentStateData->ActiveColorCount ? CurrentStateData->ActiveColors[Index] : nullptr;
        Key.ColorFormats[Index] = Each ? Each->GetFormat() : DXGI_FORMAT_UNKNOWN;
    }
    Key.DSVFormat      = CurrentStateData->ActiveDepth ? CurrentStateData->ActiveDepth->GetDepthFormat() : DXGI_FORMAT_UNKNOWN;
    Key.DepthEnable    = Depth.bDepthTest ? TRUE : FALSE;
    Key.DepthWrite     = Depth.bDepthMask ? TRUE : FALSE;
    Key.DepthFunc      = DecodeCompareFunc(Depth.DepthMode);

    D3D12DrawCaches& Caches = *InIn.Caches;
    ID3D12GraphicsCommandList* List = InIn.List;

    /* The pipeline: the last key's again, or looked up (hashed, under the cache's lock). */
    if(!Caches.Pipeline || !(Caches.PipelineKey == Key))
    {
        Caches.Pipeline    = PipelineCache->GetOrCreate(Key, VSRes, PSRes);
        Caches.PipelineKey = Key;
    }
    ID3D12PipelineState* Pipeline = Caches.Pipeline;
    if(!Pipeline) return false;

    // A root CBV or table set only if it differs from what the list has.
    const auto SetCBV = [List, &Caches](const UINT InParam, const uint64_t InAddress)
    {
        if(Caches.BoundRoot[InParam] == InAddress) return;
        List->SetGraphicsRootConstantBufferView(InParam, InAddress);
        Caches.BoundRoot[InParam] = InAddress;
    };
    const auto SetTable = [List, &Caches](const UINT InParam, const D3D12_GPU_DESCRIPTOR_HANDLE InHandle)
    {
        if(Caches.BoundRoot[InParam] == InHandle.ptr) return;
        List->SetGraphicsRootDescriptorTable(InParam, InHandle);
        Caches.BoundRoot[InParam] = InHandle.ptr;
    };

    // Both stages carry identical core uniforms; the PS copy also has the custom ones.
    // In a recording context they are only read: its core blocks come packed (InIn.Core*).
    const ScalarMap& Scalars = PSRes->GetScalars();

    static constexpr uint32_t kCustomBytes = 256;
    const bool bMaterial = InIn.bMaterial;
    const std::vector<uint8_t>* Blocks = InIn.Blocks;
    const uint64_t Epoch = DescAlloc->GetEpoch();

    /* A cbuffer is read in 256-byte units; the material layout's blocks are
     * raw bytes (GDISetUniformBlock), rounded up, never empty. */
    const auto BlockBytes = [](const std::vector<uint8_t>& InBlock)
    {
        return InBlock.empty() ? 256u : uint32_t((InBlock.size() + 255) & ~size_t(255));
    };

    /* The view block, as the shaders read it (clip z remapped): uploaded once
     * per view, while its bytes are unchanged. */
    alignas(16) float View[40];
    if(InIn.CoreView)
    {
        memcpy(View, InIn.CoreView, sizeof(View));
        GDICorePack::RemapClipZ(View);
    }
    else
    {
        PackViewParams(Scalars, View);
    }
    if(Caches.ViewEpoch != Epoch || Caches.ViewAddress == 0 || memcmp(Caches.View, View, sizeof(View)) != 0)
    {
        const D3D12CBAllocation VP = DescAlloc->AllocateCB(160);
        if(!VP.MappedPtr) return false;
        memcpy(VP.MappedPtr, View, sizeof(View));
        memcpy(Caches.View, View, sizeof(View));
        Caches.ViewAddress = VP.GPUAddress;
        Caches.ViewEpoch   = Epoch;
    }

    /* The transform: every draw's own. */
    const D3D12CBAllocation TR = DescAlloc->AllocateCB(kTransformBytes);
    if(!TR.MappedPtr) return false;
    if(InIn.CoreTransform) { memcpy(TR.MappedPtr, InIn.CoreTransform, kTransformBytes); }
    else                   { PackTransform(Scalars, TR.MappedPtr); }

    /* b2: a material draw's block once per change (by serial), as the scene's
     * and the skin's; anything else's custom scalars every draw. */
    uint64_t CustomAddress = 0;
    if(bMaterial)
    {
        const auto Upload = [this, &BlockBytes](const std::vector<uint8_t>& InBlock, const uint32_t InBytes,
                                                uint64_t& OutAddress) -> bool
        {
            const D3D12CBAllocation At = DescAlloc->AllocateCB(InBytes);
            if(!At.MappedPtr) return false;
            memset(At.MappedPtr, 0, InBytes);
            if(!InBlock.empty()) memcpy(At.MappedPtr, InBlock.data(), InBlock.size());
            OutAddress = At.GPUAddress;
            return true;
        };
        if(Caches.MaterialSerial != InIn.Serials[0] || Caches.MaterialEpoch != Epoch || Caches.MaterialAddress == 0)
        {
            if(!Upload(Blocks[0], BlockBytes(Blocks[0]), Caches.MaterialAddress)) return false;
            Caches.MaterialSerial = InIn.Serials[0];
            Caches.MaterialEpoch  = Epoch;
        }
        CustomAddress = Caches.MaterialAddress;

        if(Caches.SceneSerial != InIn.Serials[1] || Caches.SceneEpoch != Epoch || Caches.SceneAddress == 0)
        {
            if(!Upload(Blocks[1], BlockBytes(Blocks[1]), Caches.SceneAddress)) return false;
            Caches.SceneSerial = InIn.Serials[1];
            Caches.SceneEpoch  = Epoch;
        }

        /* The skin blocks (b12, b13): the last palette set and the frame
         * before's. A skinned shader declares the whole palette: upload all of it. */
        for(uint32_t SkinIndex = 0; SkinIndex < 2; ++SkinIndex)
        {
            const std::vector<uint8_t>& Bytes = Blocks[2 + SkinIndex];
            if(Caches.SkinSerial[SkinIndex] == InIn.Serials[2 + SkinIndex] && Caches.SkinEpoch[SkinIndex] == Epoch
               && Caches.SkinAddress[SkinIndex] != 0)
            {
                continue;
            }
            if(!Upload(Bytes, Bytes.empty() ? 256u : GDI_SKIN_BLOCK_MAX_BYTES, Caches.SkinAddress[SkinIndex])) return false;
            Caches.SkinSerial[SkinIndex] = InIn.Serials[2 + SkinIndex];
            Caches.SkinEpoch[SkinIndex]  = Epoch;
        }

        /* The light block (b22): the frame's light list, bound whole, uploaded once per change. */
        if(Caches.LightSerial != InIn.Serials[4] || Caches.LightEpoch != Epoch || Caches.LightAddress == 0)
        {
            if(!Upload(Blocks[4], GDI_LIGHT_BLOCK_MAX_BYTES, Caches.LightAddress)) return false;
            Caches.LightSerial = InIn.Serials[4];
            Caches.LightEpoch  = Epoch;
        }
    }
    else
    {
        const D3D12CBAllocation Custom = DescAlloc->AllocateCB(kCustomBytes);
        if(!Custom.MappedPtr) return false;
        PackCustom(Scalars, Custom.MappedPtr, kCustomBytes);
        CustomAddress = Custom.GPUAddress;
    }

    // Every texture resource this backend makes is a D3D12TextureResource.
    auto* Texture = static_cast<D3D12TextureResource*>(PSRes->GetTexture0());
    if(!Texture || Texture->GetSRVSlot() == UINT32_MAX) Texture = NullTexture;
    if(!Texture || Texture->GetSRVSlot() == UINT32_MAX) return false;

    if(Caches.BoundPipeline != Pipeline)
    {
        List->SetPipelineState(Pipeline);
        Caches.BoundPipeline = Pipeline;
    }
    SetCBV(RP_ViewParameters, Caches.ViewAddress);
    SetCBV(RP_Transform,      TR.GPUAddress);
    SetCBV(RP_Custom,         CustomAddress);

    // Shaders sample at t2 or t3 depending on whether they declare a custom
    // cbuffer; binding the one texture to both saves re-deriving which.
    const D3D12_GPU_DESCRIPTOR_HANDLE SRV     = DescAlloc->GetSRVGPUHandle(Texture->GetSRVSlot());
    const D3D12_GPU_DESCRIPTOR_HANDLE Sampler = DescAlloc->GetSamplerGPUHandle(Texture->GetBorder(), Texture->GetFilter());
    SetTable(RP_Texture2, SRV);
    SetTable(RP_Texture3, SRV);
    SetTable(RP_Sampler2, Sampler);
    SetTable(RP_Sampler3, Sampler);

    if(bMaterial)
    {
        SetCBV(RP_Scene,    Caches.SceneAddress);
        SetCBV(RP_Skin,     Caches.SkinAddress[0]);
        SetCBV(RP_PrevSkin, Caches.SkinAddress[1]);
        SetCBV(RP_Light,    Caches.LightAddress);
        for(uint32_t Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot)
        {
            /* The same resource as the list has bound: its tables are already
             * these (any reopened list forgets them, so a resource replaced
             * since -- a resize goes through a flush -- is bound again). */
            GDITextureResource* Resource = InIn.Textures[Slot];
            if(Resource && Resource == Caches.BoundTextures[Slot]) continue;
            auto* SlotTexture = static_cast<D3D12TextureResource*>(Resource);
            if(!SlotTexture || SlotTexture->GetSRVSlot() == UINT32_MAX) SlotTexture = NullTexture;
            SetTable(RP_MaterialTexture0 + Slot, DescAlloc->GetSRVGPUHandle(SlotTexture->GetSRVSlot()));
            SetTable(RP_MaterialSampler0 + Slot, DescAlloc->GetSamplerGPUHandle(SlotTexture->GetBorder(), SlotTexture->GetFilter()));
            Caches.BoundTextures[Slot] = Resource;
        }
    }
    return true;
}

void D3D12DynamicGDI::DrawObject(GDIViewObject* InObject, GDIView* InView)
{
    if(!InObject || !InObject->Mesh) return;

    /* The core blocks, packed once as BindDrawState uploads them, onto both
     * shaders -- what UpdateCoreUniforms' seven named scalars each held, with
     * no string per value (MULTITHREADED_RENDERING.md, phase 1). A shader's
     * user uniforms still go through by name. */
    alignas(16) float View[40];
    alignas(16) float Transform[36];
    if(InView) { GDICorePack::PackCoreView(InView, View); }
    if(InObject->Transform) { GDICorePack::PackCoreTransform(InObject->Transform, Transform); }
    for(GDIShader* Shader : { static_cast<GDIShader*>(BoundVertexShader), static_cast<GDIShader*>(BoundPixelShader) })
    {
        if(!Shader || !Shader->Resource) continue;
        D3D12ShaderResource::CoreBlocks& Core = static_cast<D3D12ShaderResource*>(Shader->Resource)->GetCoreBlocks();
        if(InView)
        {
            memcpy(Core.View, View, sizeof(View));
            Core.bView = true;
        }
        if(InObject->Transform)
        {
            memcpy(Core.Transform, Transform, sizeof(Transform));
            Core.bTransform = true;
        }
        Shader->UpdateUserDefinedUniforms();
    }

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
        const float UVs[] = {   // v = 0 at NDC +1, the target's row 0: an identity copy
            0.f, 1.f,
            0.f, 0.f,
            1.f, 0.f,
            1.f, 0.f,
            1.f, 1.f,
            0.f, 1.f
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

Color D3D12DynamicGDI::GetColorFromSamplePoint(const Point& InPoint, const Size& /*InSize*/)
{
    if(!DeviceObj || !CurrentStateData) return Color();

    FlushForReadback();

    D3D12RenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT || !RT->IsValid()) return Color();

    // RT rows are top-down, as the point's screen space: the point is the row.
    const int X = InPoint.X;
    const int Y = InPoint.Y;
    if(X < 0 || Y < 0 || X >= int(RT->GetWidth()) || Y >= int(RT->GetHeight())) return Color();

    std::vector<uint8_t> Pixel;
    if(!ReadbackTexture(DeviceObj, RT->GetResource(), RT->GetCurrentState(), uint32_t(X), uint32_t(Y), 1, 1, Pixel))
    {
        return Color();
    }
    return Color(Pixel[0], Pixel[1], Pixel[2]);
}

#pragma endregion

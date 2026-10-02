#include "Public/GDI/D3D12/D3D12RenderTarget.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Public/GDI/D3D12/D3D12Texture.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"
#include "Public/GDI/GDITexture.h"

#include <cstring>
#include <vector>

#include "Public/GDI/GDILog.h"

extern GDIDynamicDriver* GDynamicGDI;

namespace
{
    D3D12DynamicGDI* GetD3D12Driver()
    {
        return dynamic_cast<D3D12DynamicGDI*>(GDynamicGDI);
    }
}

D3D12RenderTarget::D3D12RenderTarget(D3D12Device* InDevice, const std::string& InName, uint InWidth, uint InHeight)
    : GDIRenderTarget(InWidth, InHeight)
    , Device(InDevice)
    , Name(InName)
{
    Allocate();
}

D3D12RenderTarget::~D3D12RenderTarget()
{
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        GDI->UnregisterRenderTarget(this);
    }
    DestroyImage();
}

void D3D12RenderTarget::Allocate()
{
    if(!Device || !Device->IsValid()) return;
    if(Width == 0 || Height == 0) return;

    D3D12DynamicGDI* GDI = GetD3D12Driver();
    if(!GDI || !GDI->GetDescAlloc()) return;

    ID3D12Device* Dev = Device->GetDevice();

    D3D12_HEAP_PROPERTIES Heap{};
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = Width;
    Desc.Height           = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = Format;
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // COMMON rather than a sampleable state, so a present never samples an RT
    // nothing has rendered to yet.
    if(FAILED(Dev->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON,
                                           nullptr, IID_PPV_ARGS(&Resource))))
    {
        GDI_LOG_ERROR("[D3D12RenderTarget] CreateCommittedResource failed for " + Name)
        return;
    }

    D3D12_DESCRIPTOR_HEAP_DESC RTVDesc{};
    RTVDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RTVDesc.NumDescriptors = 1;
    if(FAILED(Dev->CreateDescriptorHeap(&RTVDesc, IID_PPV_ARGS(&RTVHeap))))
    {
        GDI_LOG_ERROR("[D3D12RenderTarget] RTV heap creation failed for " + Name)
        DestroyImage();
        return;
    }
    Dev->CreateRenderTargetView(Resource.Get(), nullptr, RTVHeap->GetCPUDescriptorHandleForHeapStart());

    SRVSlot = GDI->GetDescAlloc()->AllocateSRV();
    if(SRVSlot != D3D12DescriptorAllocator::InvalidSlot)
    {
        Dev->CreateShaderResourceView(Resource.Get(), nullptr, GDI->GetDescAlloc()->GetSRVCPUHandle(SRVSlot));
    }

#ifndef NDEBUG
    const std::wstring WideName(L"RT:" + std::wstring(Name.begin(), Name.end()));
    Resource->SetName(WideName.c_str());
#endif

    CurrentState = D3D12_RESOURCE_STATE_COMMON;
    SubmittedState.store(D3D12_RESOURCE_STATE_COMMON);

    AllocateDepth();
}

void D3D12RenderTarget::AllocateDepth()
{
    ID3D12Device* Dev = Device->GetDevice();

    D3D12_HEAP_PROPERTIES Heap{};
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = Width;
    Desc.Height           = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R32_TYPELESS;   // D32 to the DSV, R32 to the SRV
    Desc.SampleDesc.Count = 1;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE Clear{};
    Clear.Format             = DepthFormat;
    Clear.DepthStencil.Depth = 1.0f;

    D3D12_DESCRIPTOR_HEAP_DESC DSVDesc{};
    DSVDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    DSVDesc.NumDescriptors = 1;

    if(FAILED(Dev->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                           &Clear, IID_PPV_ARGS(&DepthResource)))
       || FAILED(Dev->CreateDescriptorHeap(&DSVDesc, IID_PPV_ARGS(&DSVHeap))))
    {
        GDI_LOG_ERROR("[D3D12RenderTarget] no depth buffer for " + Name + "; it draws without a depth test")
        DepthResource.Reset();
        DSVHeap.Reset();
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC DSV{};
    DSV.Format        = DepthFormat;
    DSV.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    Dev->CreateDepthStencilView(DepthResource.Get(), &DSV, DSVHeap->GetCPUDescriptorHandleForHeapStart());
    DepthState       = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bDepthNeedsClear = true;

    // Sampled as R32_FLOAT (ToDepthTexture2D).
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        DepthSRVSlot = GDI->GetDescAlloc()->AllocateSRV();
        if(DepthSRVSlot != D3D12DescriptorAllocator::InvalidSlot)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
            SRV.Format                  = DXGI_FORMAT_R32_FLOAT;
            SRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRV.Texture2D.MipLevels     = 1;
            Dev->CreateShaderResourceView(DepthResource.Get(), &SRV, GDI->GetDescAlloc()->GetSRVCPUHandle(DepthSRVSlot));
        }
        else
        {
            DepthSRVSlot = UINT32_MAX;
        }
    }
}

void D3D12RenderTarget::DestroyImage()
{
    if(TextureWrapper)
    {
        if(TextureWrapper->Resource)
        {
            TextureWrapper->Resource->Release();
            delete TextureWrapper->Resource;
            TextureWrapper->Resource = nullptr;
        }
        delete TextureWrapper;
        TextureWrapper = nullptr;
    }
    if(DepthWrapper)
    {
        if(DepthWrapper->Resource)
        {
            DepthWrapper->Resource->Release();
            delete DepthWrapper->Resource;
            DepthWrapper->Resource = nullptr;
        }
        delete DepthWrapper;
        DepthWrapper = nullptr;
    }

    // During driver teardown the cast fails; Destroy() has already waited idle.
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        if(Resource) GDI->DeferRelease(Resource);
        if(DepthResource) GDI->DeferRelease(DepthResource);
        if(SRVSlot != UINT32_MAX) GDI->DeferFreeSRV(SRVSlot);
        if(DepthSRVSlot != UINT32_MAX) GDI->DeferFreeSRV(DepthSRVSlot);
    }

    Resource.Reset();
    RTVHeap.Reset();
    DepthResource.Reset();
    DSVHeap.Reset();
    bDepthNeedsClear = false;
    DepthState       = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    DepthSRVSlot     = UINT32_MAX;
    SRVSlot      = UINT32_MAX;
    CurrentState = D3D12_RESOURCE_STATE_COMMON;
    SubmittedState.store(D3D12_RESOURCE_STATE_COMMON);
}

void D3D12RenderTarget::Bind()
{
    // D3D12DynamicGDI::SetRenderTarget records the transition and binding.
}

void D3D12RenderTarget::Release()
{
    DestroyImage();
}

void D3D12RenderTarget::Resize(const uint InWidth, const uint InHeight)
{
    // This may be the active target of the open list: submit that list first,
    // without re-binding, then wait out the present that may be sampling it.
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        GDI->SetResizing(true);
        GDI->FlushForReadback(false);
    }
    if(Device) Device->WaitIdle();

    GDIRenderTarget::Resize(InWidth, InHeight);
    DestroyImage();
    Allocate();
}

bool D3D12RenderTarget::IsValid()
{
    return Resource != nullptr && RTVHeap != nullptr;
}

EngineCore::Image D3D12RenderTarget::ToImage()
{
    if(!Device || !Device->IsValid() || !Resource) return EngineCore::Image();
    if(Width == 0 || Height == 0) return EngineCore::Image();

    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        GDI->FlushForReadback();
    }

    std::vector<uint8_t> Pixels;
    if(!D3D12Ops::ReadbackTexture(Device, Resource.Get(), CurrentState, 0, 0, Width, Height, Pixels))
    {
        return EngineCore::Image();
    }

    // RT rows are GL-ordered (see cook_shaders.py's --flip-vert-y), so mirror
    // like the GL and Vulkan backends to return the same orientation.
    EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    std::memcpy(Result.GetData(), Pixels.data(), Pixels.size());
    Result.FlipVertical();
    return Result;
}

GDITexture2D* D3D12RenderTarget::ToGDITexture2D()
{
    if(!Resource || SRVSlot == UINT32_MAX) return nullptr;

    if(!TextureWrapper)
    {
        TextureWrapper         = new GDITexture2D();
        TextureWrapper->Width  = Width;
        TextureWrapper->Height = Height;
        TextureWrapper->Resource =
            new D3D12TextureResource(Device, Resource.Get(), SRVSlot, TextureWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Linear);
    }
    return TextureWrapper;
}

GDITexture2D* D3D12RenderTarget::ToDepthTexture2D()
{
    if(!DepthResource || DepthSRVSlot == UINT32_MAX) return nullptr;

    if(!DepthWrapper)
    {
        DepthWrapper         = new GDITexture2D();
        DepthWrapper->Width  = Width;
        DepthWrapper->Height = Height;
        DepthWrapper->Resource =
            new D3D12TextureResource(Device, DepthResource.Get(), DepthSRVSlot, DepthWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Nearest);
    }
    return DepthWrapper;
}

void D3D12RenderTarget::ResolveRenderTarget(GDIRenderTarget* Dest)
{
    auto* DstRT = dynamic_cast<D3D12RenderTarget*>(Dest);
    if(!DstRT) return;
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        GDI->RecordResolve(this, DstRT);
    }
}

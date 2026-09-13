#include "Public/GDI/D3D12/D3D12Texture.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <cstring>
#include <vector>

#include "Public/GDI/GDILog.h"

extern GDIDynamicDriver* GDynamicGDI;

namespace
{
    // Null during driver teardown, where Destroy() has already waited idle.
    D3D12DynamicGDI* GetD3D12Driver()
    {
        return dynamic_cast<D3D12DynamicGDI*>(GDynamicGDI);
    }
}

D3D12TextureResource::D3D12TextureResource(D3D12Device*             InDevice,
                                           const EngineCore::Image& InImage,
                                           EGDITextureBorder        InBorder,
                                           EGDITextureFilter        InFilter)
    : GDITextureResource(InImage, 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    AllocateImage();
    if(!InImage.IsNull())
    {
        UploadImageData(InImage);
        bInitialized = true;
    }
}

D3D12TextureResource::D3D12TextureResource(D3D12Device*             InDevice,
                                           GDITexture2D*            InParent,
                                           const EngineCore::Image& InImage,
                                           EGDITextureBorder        InBorder,
                                           EGDITextureFilter        InFilter)
    : GDITextureResource(InParent, InImage, 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    AllocateImage();
    if(!InImage.IsNull())
    {
        UploadImageData(InImage);
        bInitialized = true;
    }
}

D3D12TextureResource::D3D12TextureResource(D3D12Device*      InDevice,
                                           GDITexture2D*     InParent,
                                           EGDITextureBorder InBorder,
                                           EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    if(InParent)
    {
        Width  = static_cast<int>(InParent->Width);
        Height = static_cast<int>(InParent->Height);
    }
    AllocateImage();
}

D3D12TextureResource::D3D12TextureResource(D3D12Device*      InDevice,
                                           ID3D12Resource*   InExternalResource,
                                           uint32_t          InExternalSRVSlot,
                                           GDITexture2D*     InParent,
                                           EGDITextureBorder InBorder,
                                           EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , Device(InDevice)
    , ExternalResource(InExternalResource)
    , SRVSlot(InExternalSRVSlot)
    , bOwnsImage(false)
    , CurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    if(InParent)
    {
        Width  = static_cast<int>(InParent->Width);
        Height = static_cast<int>(InParent->Height);
    }
    bInitialized = (ExternalResource != nullptr);
}

D3D12TextureResource::~D3D12TextureResource()
{
    Release();
}

void D3D12TextureResource::Update(const EngineCore::Image& InImage)
{
    if(!Device || !Device->IsValid() || InImage.IsNull() || !bOwnsImage) return;

    if(InImage.GetWidth() != Width || InImage.GetHeight() != Height)
    {
        Width  = InImage.GetWidth();
        Height = InImage.GetHeight();
        DestroyImage();
        AllocateImage();
    }

    UploadImageData(InImage);
    bInitialized = true;
}

void D3D12TextureResource::Release()
{
    if(bOwnsImage)
    {
        DestroyImage();
    }
    ExternalResource = nullptr;
    SRVSlot          = UINT32_MAX;
    bInitialized     = false;
}

EngineCore::Image D3D12TextureResource::ToImage() const
{
    ID3D12Resource* Texture = GetResource();
    if(!bInitialized || !Device || !Device->IsValid() || !Texture) return EngineCore::Image();
    if(Width <= 0 || Height <= 0) return EngineCore::Image();

    std::vector<uint8_t> Pixels;
    if(!D3D12Ops::ReadbackTexture(Device, Texture, CurrentState, 0, 0, uint32_t(Width), uint32_t(Height), Pixels))
    {
        return EngineCore::Image();
    }

    EngineCore::Image Result(Width, Height, EngineCore::EImageFormat::RGBA8);
    std::memcpy(Result.GetData(), Pixels.data(), Pixels.size());
    return Result;
}

void D3D12TextureResource::AllocateImage()
{
    if(!Device || !Device->IsValid() || Width <= 0 || Height <= 0) return;

    D3D12DynamicGDI* GDI = GetD3D12Driver();
    if(!GDI || !GDI->GetDescAlloc()) return;

    D3D12_HEAP_PROPERTIES Heap{};
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = UINT64(Width);
    Desc.Height           = UINT(Height);
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    Desc.SampleDesc.Count = 1;

    if(FAILED(Device->GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&Resource))))
    {
        GDI_LOG_ERROR("[D3D12Texture] CreateCommittedResource failed.")
        return;
    }
    CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;

    SRVSlot = GDI->GetDescAlloc()->AllocateSRV();
    if(SRVSlot != D3D12DescriptorAllocator::InvalidSlot)
    {
        Device->GetDevice()->CreateShaderResourceView(Resource.Get(), nullptr, GDI->GetDescAlloc()->GetSRVCPUHandle(SRVSlot));
    }
}

void D3D12TextureResource::DestroyImage()
{
    // Deferred: Update() and Release() both run while recorded draws may still reference the texture.
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        if(Resource) GDI->DeferRelease(Resource);
        if(SRVSlot != UINT32_MAX) GDI->DeferFreeSRV(SRVSlot);
    }
    Resource.Reset();
    SRVSlot      = UINT32_MAX;
    CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
}

void D3D12TextureResource::UploadImageData(const EngineCore::Image& InImage)
{
    if(!Device || !Device->IsValid() || !Resource) return;

    const EngineCore::Image Formatted = InImage.ConvertedTo(EngineCore::EImageFormat::RGBA8);
    D3D12Ops::UploadTexture(Device, Resource.Get(), CurrentState, Formatted.GetData(),
                            uint32_t(Formatted.GetWidth()), uint32_t(Formatted.GetHeight()));
}

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

D3D12TextureResource::D3D12TextureResource(D3D12Device*             InDevice,
                                           const EngineCore::Image* InLevels,
                                           const uint32_t           InCount,
                                           EGDITextureBorder        InBorder,
                                           EGDITextureFilter        InFilter)
    : GDITextureResource(InLevels[0], 0, InBorder, InFilter)
    , Device(InDevice)
    , MipLevels(InCount > 0 ? InCount : 1)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    AllocateImage();
    if(!InLevels[0].IsNull())
    {
        UploadMipChain(InLevels, MipLevels);
        bInitialized = true;
    }
}

D3D12TextureResource::D3D12TextureResource(D3D12Device*              InDevice,
                                           const DXGI_FORMAT         InFormat,
                                           const GDICompressedLevel* InLevels,
                                           const uint32_t            InCount,
                                           EGDITextureBorder         InBorder,
                                           EGDITextureFilter         InFilter)
    : GDITextureResource()
    , Device(InDevice)
    , MipLevels(InCount > 0 ? InCount : 1)
    , Format(InFormat)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    Width  = int(InLevels[0].Width);
    Height = int(InLevels[0].Height);
    AllocateImage();
    if(!Resource) return;

    // The blocks as they are, a row of blocks a footprint row.
    std::vector<D3D12Ops::SubresourceData> Levels(MipLevels);
    for(uint32_t Level = 0; Level < MipLevels; ++Level)
    {
        Levels[Level] = { InLevels[Level].Data, InLevels[Level].Bytes };
    }
    if(!D3D12Ops::UploadSubresources(Device, Resource.Get(), CurrentState, Levels.data(), MipLevels, TakeAsyncUpload()))
    {
        DestroyImage();   // no SRV slot: the driver reports it unmade
        return;
    }
    bInitialized = true;
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

    // If dimensions changed (or it had mips or blocks, which one RGBA8 image cannot refill), recreate the resource.
    if(InImage.GetWidth() != Width || InImage.GetHeight() != Height || MipLevels != 1 || Format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        Width     = InImage.GetWidth();
        Height    = InImage.GetHeight();
        MipLevels = 1;
        Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    if(!bInitialized || !Device || !Device->IsValid() || !Texture || Format != DXGI_FORMAT_R8G8B8A8_UNORM) return EngineCore::Image();
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

void D3D12TextureResource::SetDebugName(const std::string& InName)
{
    GDITextureResource::SetDebugName(InName);
    if(bOwnsImage) D3D12Util::SetObjectName(Resource.Get(), DebugName);   // an adopted render target names itself
}

void D3D12TextureResource::AllocateImage()
{
    if(!Device || !Device->IsValid() || Width <= 0 || Height <= 0) return;

    D3D12DynamicGDI* GDI = GetD3D12Driver();
    if(!GDI || !GDI->GetDescAlloc()) return;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width            = UINT64(Width);
    Desc.Height           = UINT(Height);
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = UINT16(MipLevels);   // the SRV (null desc) sees every level
    Desc.Format           = Format;
    Desc.SampleDesc.Count = 1;

    /* Placed in a heap block when small, else committed (D3D12TextureHeap). */
    if(!Device->GetTextureHeap().CreateTexture(Desc, D3D12_RESOURCE_STATE_COPY_DEST, Resource, Allocation))
    {
        GDI_LOG_ERROR("[D3D12Texture] No device memory for the texture.")
        return;
    }
    CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
    bFresh       = true;
    D3D12Util::SetObjectName(Resource.Get(), DebugName);

    SRVSlot = GDI->GetDescAlloc()->AllocateSRV();
    if(SRVSlot != D3D12DescriptorAllocator::InvalidSlot)
    {
        Device->GetDevice()->CreateShaderResourceView(Resource.Get(), nullptr, GDI->GetDescAlloc()->GetSRVCPUHandle(SRVSlot));
    }
}

void D3D12TextureResource::DestroyImage()
{
    // Deferred: Update() and Release() both run while recorded draws may still reference the texture.
    // Its heap range goes back with it, once no frame reads it.
    if(D3D12DynamicGDI* GDI = GetD3D12Driver())
    {
        if(Resource || Allocation.IsValid()) GDI->DeferRelease(Resource, Allocation);
        if(SRVSlot != UINT32_MAX) GDI->DeferFreeSRV(SRVSlot);
        Resource.Reset();
    }
    else
    {
        Resource.Reset();
        if(Device) Device->GetTextureHeap().Free(Allocation);
    }
    Allocation   = D3D12Allocation();
    SRVSlot      = UINT32_MAX;
    CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
    bFresh       = false;
}

D3D12DynamicGDI* D3D12TextureResource::TakeAsyncUpload()
{
    D3D12DynamicGDI* GDI = bFresh ? GetD3D12Driver() : nullptr;
    bFresh = false;
    return GDI;
}

void D3D12TextureResource::UploadImageData(const EngineCore::Image& InImage)
{
    if(!Device || !Device->IsValid() || !Resource) return;

    // An RGBA8 image is read in place; any other is converted (a copy).
    EngineCore::Image        Converted;
    const EngineCore::Image* Formatted = &InImage;
    if(InImage.GetFormat() != EngineCore::EImageFormat::RGBA8)
    {
        Converted = InImage.ConvertedTo(EngineCore::EImageFormat::RGBA8);
        Formatted = &Converted;
    }
    if(Formatted->IsNull()) return;
    D3D12Ops::UploadTexture(Device, Resource.Get(), CurrentState, Formatted->GetData(),
                            uint32_t(Formatted->GetWidth()), uint32_t(Formatted->GetHeight()), TakeAsyncUpload());
}

void D3D12TextureResource::UploadMipChain(const EngineCore::Image* InLevels, const uint32_t InCount)
{
    if(!Device || !Device->IsValid() || !Resource || InCount == 0) return;

    /* RGBA8 levels are read in place; only the others are converted (a copy each). */
    std::vector<EngineCore::Image>         Formatted;
    std::vector<D3D12Ops::SubresourceData> Levels;
    Formatted.reserve(InCount);   // never reallocates: Levels points into it
    Levels.reserve(InCount);
    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        const EngineCore::Image* Each = &InLevels[Level];
        if(Each->GetFormat() != EngineCore::EImageFormat::RGBA8)
        {
            Formatted.push_back(Each->ConvertedTo(EngineCore::EImageFormat::RGBA8));
            Each = &Formatted.back();
        }
        Levels.push_back({ Each->GetData(), size_t(Each->GetWidth()) * size_t(Each->GetHeight()) * 4u });
    }
    D3D12Ops::UploadSubresources(Device, Resource.Get(), CurrentState, Levels.data(), InCount, TakeAsyncUpload());
}

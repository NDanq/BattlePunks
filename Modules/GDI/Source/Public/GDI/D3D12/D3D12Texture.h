#ifndef CORECLIENT_D3D12TEXTURE_H
#define CORECLIENT_D3D12TEXTURE_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Public/GDI/GDITexture.h"

class D3D12Device;

/**
 * R8G8B8A8_UNORM (or block-compressed BC1-BC7) texture with its own SRV slot.
 * Uploads and readbacks run synchronously on a transient command list, render
 * thread only.
 */
class D3D12TextureResource : public GDITextureResource
{
public:
    D3D12TextureResource(D3D12Device*             InDevice,
                         const EngineCore::Image& InImage,
                         EGDITextureBorder        InBorder,
                         EGDITextureFilter        InFilter);

    D3D12TextureResource(D3D12Device*             InDevice,
                         GDITexture2D*            InParent,
                         const EngineCore::Image& InImage,
                         EGDITextureBorder        InBorder,
                         EGDITextureFilter        InFilter);

    /** With its mip chain: InLevels[0] full size, each next half the last (GDICreateTextureResourceMips). */
    D3D12TextureResource(D3D12Device*             InDevice,
                         const EngineCore::Image* InLevels,
                         uint32_t                 InCount,
                         EGDITextureBorder        InBorder,
                         EGDITextureFilter        InFilter);

    /** A block-compressed texture (GDICreateCompressedTextureResource): InFormat a DXGI BC
     *  format, InLevels[0] full size, each next half the last. Invalid (no SRV) on failure. */
    D3D12TextureResource(D3D12Device*              InDevice,
                         DXGI_FORMAT               InFormat,
                         const GDICompressedLevel* InLevels,
                         uint32_t                  InCount,
                         EGDITextureBorder         InBorder,
                         EGDITextureFilter         InFilter);

    /** No initial data; Update() must run before it is sampled. */
    D3D12TextureResource(D3D12Device*      InDevice,
                         GDITexture2D*     InParent,
                         EGDITextureBorder InBorder,
                         EGDITextureFilter InFilter);

    /** Adopts a render target's resource and SRV slot without owning either. */
    D3D12TextureResource(D3D12Device*      InDevice,
                         ID3D12Resource*   InExternalResource,
                         uint32_t          InExternalSRVSlot,
                         GDITexture2D*     InParent,
                         EGDITextureBorder InBorder,
                         EGDITextureFilter InFilter);

    ~D3D12TextureResource();

    void Update(const EngineCore::Image& InImage) override;
    void Release() override;
    EngineCore::Image ToImage() const override;

    /** Names its resource, and again whenever Update() remakes it. */
    void SetDebugName(const std::string& InName) override;

    inline uint32_t          GetSRVSlot() const { return SRVSlot; }
    inline EGDITextureBorder GetBorder()  const { return StoredBorder; }
    inline EGDITextureFilter GetFilter()  const { return StoredFilter; }

private:
    void AllocateImage();
    void DestroyImage();
    void UploadImageData(const EngineCore::Image& InImage);
    void UploadMipChain(const EngineCore::Image* InLevels, uint32_t InCount);

    ID3D12Resource* GetResource() const { return bOwnsImage ? Resource.Get() : ExternalResource; }

    D3D12Device*                            Device           = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource>  Resource;
    ID3D12Resource*                         ExternalResource = nullptr;
    uint32_t                                SRVSlot          = UINT32_MAX;
    bool                                    bOwnsImage       = true;
    uint32_t                                MipLevels        = 1;
    DXGI_FORMAT                             Format           = DXGI_FORMAT_R8G8B8A8_UNORM;   // or a BC format: blocks, never Update()d in place

    D3D12_RESOURCE_STATES                   CurrentState     = D3D12_RESOURCE_STATE_COPY_DEST;

    EGDITextureBorder                       StoredBorder     = GDI_TB_Repeat;
    EGDITextureFilter                       StoredFilter     = GDI_TF_Nearest;
};

#endif //CORECLIENT_D3D12TEXTURE_H

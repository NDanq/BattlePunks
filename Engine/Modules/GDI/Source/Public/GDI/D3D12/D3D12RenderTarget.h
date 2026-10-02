#ifndef CORECLIENT_D3D12RENDERTARGET_H
#define CORECLIENT_D3D12RENDERTARGET_H

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

#include "Public/GDI/GDIRenderTarget.h"

class D3D12Device;

/** A single R8G8B8A8_UNORM colour target, sampleable through its own SRV slot. */
class D3D12RenderTarget : public GDIRenderTarget
{
public:
    D3D12RenderTarget(D3D12Device* InDevice, const std::string& InName, uint InWidth, uint InHeight);
    ~D3D12RenderTarget();

    void Bind() override;
    void Release() override;
    void Resize(const uint InWidth, const uint InHeight) override;
    bool IsValid() override;
    EngineCore::Image ToImage() override;
    GDITexture2D* ToGDITexture2D() override;
    GDITexture2D* ToDepthTexture2D() override;
    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    inline ID3D12Resource*             GetResource() const { return Resource.Get(); }
    inline D3D12_CPU_DESCRIPTOR_HANDLE GetRTV()      const { return RTVHeap->GetCPUDescriptorHandleForHeapStart(); }
    inline uint32_t                    GetSRVSlot()  const { return SRVSlot; }
    inline DXGI_FORMAT                 GetFormat()   const { return Format; }

    /* D32 (an R32_TYPELESS resource, so it can also be sampled): DEPTH_WRITE
     * while bound, PIXEL_SHADER_RESOURCE otherwise (SetRenderTargets). False
     * only if creation failed (logged). */
    inline bool                        HasDepth()       const { return DepthResource != nullptr; }
    inline ID3D12Resource*             GetDepthResource() const { return DepthResource.Get(); }
    inline D3D12_RESOURCE_STATES       GetDepthState()  const { return DepthState; }
    inline void                        SetDepthState(D3D12_RESOURCE_STATES InState) { DepthState = InState; }
    inline D3D12_CPU_DESCRIPTOR_HANDLE GetDSV()         const { return DSVHeap->GetCPUDescriptorHandleForHeapStart(); }
    inline DXGI_FORMAT                 GetDepthFormat() const { return HasDepth() ? DepthFormat : DXGI_FORMAT_UNKNOWN; }

    /* True once after the depth buffer is (re)created: its first bind clears it. */
    inline bool TakeDepthNeedsClear() { const bool bWas = bDepthNeedsClear; bDepthNeedsClear = false; return bWas; }

    /** State as of the last command recorded on the render thread. */
    inline D3D12_RESOURCE_STATES GetCurrentState() const              { return CurrentState; }
    inline void                  SetCurrentState(D3D12_RESOURCE_STATES InState) { CurrentState = InState; }

    /**
     * State at the end of the last list actually submitted. Written under the
     * queue mutex by the render thread and read under it by the UI-thread
     * present, which is what lets the present order itself against a frame.
     */
    inline D3D12_RESOURCE_STATES GetSubmittedState() const { return SubmittedState.load(); }
    inline void                  PublishSubmittedState()   { SubmittedState.store(CurrentState); }

protected:
    void Allocate() override;

private:
    void DestroyImage();
    void AllocateDepth();

    D3D12Device*                                    Device  = nullptr;
    std::string                                     Name;
    DXGI_FORMAT                                     Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    Microsoft::WRL::ComPtr<ID3D12Resource>          Resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    RTVHeap;
    uint32_t                                        SRVSlot = UINT32_MAX;

    DXGI_FORMAT                                     DepthFormat = DXGI_FORMAT_D32_FLOAT;
    Microsoft::WRL::ComPtr<ID3D12Resource>          DepthResource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    DSVHeap;
    bool                                            bDepthNeedsClear = false;
    D3D12_RESOURCE_STATES                           DepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t                                        DepthSRVSlot = UINT32_MAX;
    GDITexture2D*                                   DepthWrapper = nullptr;

    D3D12_RESOURCE_STATES                           CurrentState = D3D12_RESOURCE_STATE_COMMON;
    std::atomic<D3D12_RESOURCE_STATES>              SubmittedState{D3D12_RESOURCE_STATE_COMMON};

    /** Adopts Resource/SRVSlot so the target can be sampled. Rebuilt after a resize. */
    GDITexture2D*                                   TextureWrapper = nullptr;
};

#endif //CORECLIENT_D3D12RENDERTARGET_H

#ifndef CORECLIENT_D3D12DESCRIPTORALLOCATOR_H
#define CORECLIENT_D3D12DESCRIPTORALLOCATOR_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "Public/GDI/GDIDefinitions.h"

class D3D12Device;

struct D3D12CBAllocation
{
    D3D12_GPU_VIRTUAL_ADDRESS GPUAddress = 0;
    void*                     MappedPtr  = nullptr;
};

/**
 * Render-thread descriptor and constant-buffer storage:
 *
 *   - A GPU-visible SRV heap. Each texture and render target owns one slot for
 *     its lifetime, so a draw binds a table straight at that slot with no
 *     per-draw descriptor copies.
 *   - A GPU-visible sampler heap holding one sampler per border/filter pair.
 *   - A persistently mapped upload buffer sub-allocated per draw for constant
 *     buffers and rewound by Reset() once the previous frame has executed.
 */
class D3D12DescriptorAllocator
{
public:
    static constexpr uint32_t InvalidSlot = UINT32_MAX;

    D3D12DescriptorAllocator() = default;
    ~D3D12DescriptorAllocator() { Shutdown(); }

    bool Initialize(D3D12Device* InDevice);
    void Shutdown();

    /** Rewinds the constant-buffer ring. Only once the frame that used it has executed. */
    void Reset();

    D3D12CBAllocation AllocateCB(uint32_t Size);

    uint32_t AllocateSRV();
    void     FreeSRV(uint32_t InSlot);

    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPUHandle(uint32_t InSlot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGPUHandle(uint32_t InSlot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerGPUHandle(EGDITextureBorder InBorder, EGDITextureFilter InFilter) const;

    inline ID3D12DescriptorHeap* GetSRVHeap()     const { return SRVHeap.Get(); }
    inline ID3D12DescriptorHeap* GetSamplerHeap() const { return SamplerHeap.Get(); }
    inline bool                  IsValid()        const { return SRVHeap != nullptr; }

private:
    bool CreateSamplers();

    D3D12Device*                                    Device = nullptr;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    SRVHeap;
    UINT                                            SRVIncrement = 0;
    std::vector<uint32_t>                           FreeSRVSlots;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    SamplerHeap;
    UINT                                            SamplerIncrement = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource>          RingBuffer;
    uint8_t*                                        RingMapped = nullptr;
    uint32_t                                        RingOffset = 0;

    static constexpr uint32_t kSRVCapacity  = 4096;
    static constexpr uint32_t kRingCapacity = 1024 * 1024;
    static constexpr uint32_t kCBAlignment  = 256;
};

#endif //CORECLIENT_D3D12DESCRIPTORALLOCATOR_H

#ifndef CORECLIENT_D3D12DESCRIPTORALLOCATOR_H
#define CORECLIENT_D3D12DESCRIPTORALLOCATOR_H

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
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
 *     buffers: a region per frame in flight, each rewound by Reset(Slot) once
 *     the frame that last used it has executed. AllocateCB is safe from any
 *     thread (a parallel pass's recording contexts allocate at once);
 *     everything else is the render thread's.
 */
class D3D12DescriptorAllocator
{
public:
    static constexpr uint32_t InvalidSlot = UINT32_MAX;

    D3D12DescriptorAllocator() = default;
    ~D3D12DescriptorAllocator() { Shutdown(); }

    /** InSlots: the frames in flight, each with a region of the constant-buffer ring. */
    bool Initialize(D3D12Device* InDevice, uint32_t InSlots = 1);
    void Shutdown();

    /** Allocates from InSlot's region of the ring, rewound. Only once the frame
     *  that last used that region has executed. */
    void Reset(uint32_t InSlot = 0);

    /** Bumped by Reset(): an allocation from an older epoch is stale. */
    uint64_t GetEpoch() const { return Epoch; }

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
    std::atomic<uint32_t>                           RingOffset{0};   // absolute, within [RingBase, RingEnd)
    uint32_t                                        RingBase   = 0;
    uint32_t                                        RingEnd    = 0;
    uint32_t                                        RingSlots  = 1;
    uint64_t                                        Epoch      = 0;

    static constexpr uint32_t kSRVCapacity  = 4096;
    static constexpr uint32_t kRingCapacity = 4 * 1024 * 1024;   // per slot: shadow passes draw each caster once per view
    static constexpr uint32_t kCBAlignment  = 256;
};

#endif //CORECLIENT_D3D12DESCRIPTORALLOCATOR_H

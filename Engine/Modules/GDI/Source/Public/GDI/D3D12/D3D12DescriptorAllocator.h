#ifndef CORECLIENT_D3D12DESCRIPTORALLOCATOR_H
#define CORECLIENT_D3D12DESCRIPTORALLOCATOR_H

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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
 *   - Persistently mapped upload buffers sub-allocated per draw for constant
 *     buffers: chunks of kRingCapacity bytes per frame in flight, rewound to
 *     the first by Reset(Slot) once the frame that last used them has
 *     executed. A frame that fills a chunk moves on to the next, made on first
 *     need and kept (up to kRingChunkMax), so a busy frame grows the ring
 *     rather than dropping draws -- as the Vulkan backend's does. AllocateCB
 *     is safe from any thread (a parallel pass's recording contexts allocate
 *     at once); everything else is the render thread's.
 */
class D3D12DescriptorAllocator
{
public:
    static constexpr uint32_t InvalidSlot = UINT32_MAX;

    D3D12DescriptorAllocator() = default;
    ~D3D12DescriptorAllocator() { Shutdown(); }

    /** InSlots: the frames in flight, each with its own chunks of the constant-buffer ring. */
    bool Initialize(D3D12Device* InDevice, uint32_t InSlots = 1);
    void Shutdown();

    /** Allocates from InSlot's chunks, rewound to the first. Only once the frame
     *  that last used them has executed. */
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
    static constexpr uint32_t kSRVCapacity  = 4096;
    static constexpr uint32_t kRingCapacity = 4 * 1024 * 1024;   // a chunk: shadow passes draw each caster once per view
    static constexpr uint32_t kRingChunkMax = 16;                // 64 MB a frame, at most
    static constexpr uint32_t kCBAlignment  = 256;

    struct RingChunk
    {
        Microsoft::WRL::ComPtr<ID3D12Resource>      Buffer;
        uint8_t*                                    Mapped     = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS                   GPUAddress = 0;
    };

    bool CreateSamplers();
    bool CreateRingChunk(RingChunk& OutChunk);

    D3D12Device*                                    Device = nullptr;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    SRVHeap;
    UINT                                            SRVIncrement = 0;
    std::vector<uint32_t>                           FreeSRVSlots;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>    SamplerHeap;
    UINT                                            SamplerIncrement = 0;

    /* Each frame in flight's chunks, sized at Initialize and never resized: a
     * chunk is made in place (under RingGrowLock) while other threads read
     * the ones before it. The first of each is made at Initialize. */
    std::vector<std::array<RingChunk, kRingChunkMax>> RingChunks;
    std::atomic<uint64_t>                           RingCursor{0};   // the active chunk << 32 | the offset in it
    std::mutex                                      RingGrowLock;
    uint32_t                                        RingSlot    = 0;
    uint32_t                                        RingSlots   = 1;
    bool                                            bWarnedFull = false;
    uint64_t                                        Epoch       = 0;
};

#endif //CORECLIENT_D3D12DESCRIPTORALLOCATOR_H

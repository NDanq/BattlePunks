#ifndef CORECLIENT_D3D12MEMORYHEAP_H
#define CORECLIENT_D3D12MEMORYHEAP_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

class D3D12Device;

/*
 * Texture memory for the D3D12 backend, owned by D3D12Device (it outlives
 * every texture, so a free always has somewhere to go), as VKMemoryHeap.h is
 * for Vulkan:
 *
 *   D3D12TextureHeap   sampled textures: small ones placed in 4 MB
 *                      ID3D12Heap blocks, larger ones committed on their own
 *   D3D12StagingRing   one persistently mapped upload buffer, reused frame
 *                      after frame instead of a buffer made and freed per upload
 *
 * Render thread, but both lock: a texture may be released elsewhere.
 */

/** Where a texture's memory is: a range of a heap block, or its own committed resource. */
struct D3D12Allocation
{
    ID3D12Heap* Heap   = nullptr;      // null: committed
    UINT64      Offset = 0;
    UINT64      Size   = 0;
    uint32_t    Block  = UINT32_MAX;   // UINT32_MAX: committed

    bool IsValid() const { return Size != 0; }
};

class D3D12TextureHeap
{
public:
    /* Textures under SMALL_LIMIT -- most of them by count: a streamed
     * texture's tail, UI images -- share BLOCK_BYTES blocks; larger ones are
     * committed. Large textures in shared blocks strand memory (measured on
     * Vulkan: GPU memory rose 825 -> 963 MB), since nothing can move one out
     * to compact its block. */
    static constexpr UINT64 BLOCK_BYTES = 4ull * 1024 * 1024;
    static constexpr UINT64 SMALL_LIMIT = 512ull * 1024;

    D3D12TextureHeap() = default;
    ~D3D12TextureHeap() { Shutdown(); }

    D3D12TextureHeap(const D3D12TextureHeap&)            = delete;
    D3D12TextureHeap& operator=(const D3D12TextureHeap&) = delete;

    void Initialize(D3D12Device* InDevice) { Device = InDevice; }

    /** Releases every block. What is still placed in one may no longer be used. */
    void Shutdown();

    /**
     * A texture of InDesc (never a render target or depth target) in
     * InState: placed in a block's range when its size is under SMALL_LIMIT,
     * else committed. Placed textures take 4 KB alignment when the runtime
     * allows it. False if neither could be made.
     */
    bool CreateTexture(const D3D12_RESOURCE_DESC& InDesc, D3D12_RESOURCE_STATES InState,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource, D3D12Allocation& OutAllocation);

    /** Back to its block (coalesced) once the texture is released and no
     *  frame reads it; for a committed one, only the count. A block left
     *  empty is released when another empty block is already kept. */
    void Free(const D3D12Allocation& InAllocation);

    struct Stats
    {
        uint64_t BlockBytes     = 0;   // the blocks' total
        uint64_t UsedBytes      = 0;   // ... handed out from them
        uint64_t DedicatedBytes = 0;
        uint32_t Blocks         = 0;
        uint32_t Allocations    = 0;   // from blocks
        uint32_t Dedicated      = 0;
    };
    Stats GetStats() const;

private:
    struct Range
    {
        UINT64 Offset = 0;
        UINT64 Size   = 0;
    };
    struct Block
    {
        Microsoft::WRL::ComPtr<ID3D12Heap> Heap;   // null: a freed slot, reusable
        UINT64             Bytes = 0;              // BLOCK_BYTES
        UINT64             Used  = 0;
        uint32_t           Live  = 0;
        std::vector<Range> Free;                   // by offset, never touching
    };

    /* InSize at InAlign from Blocks[InBlock]'s free ranges, first fit. */
    bool Carve(uint32_t InBlock, UINT64 InSize, UINT64 InAlign, D3D12Allocation& OutAllocation);

    /* A range of a block, a new block made if none has room. */
    bool AllocateRange(UINT64 InSize, UINT64 InAlign, D3D12Allocation& OutAllocation);

    D3D12Device*       Device = nullptr;
    mutable std::mutex Mutex;
    std::vector<Block> Blocks;
    uint64_t           DedicatedBytes = 0;
    uint32_t           DedicatedCount = 0;
};

class D3D12StagingRing
{
public:
    static constexpr UINT64 RING_BYTES = 16ull * 1024 * 1024;

    D3D12StagingRing() = default;
    ~D3D12StagingRing() { Shutdown(); }

    D3D12StagingRing(const D3D12StagingRing&)            = delete;
    D3D12StagingRing& operator=(const D3D12StagingRing&) = delete;

    void Initialize(D3D12Device* InDevice) { Device = InDevice; }
    void Shutdown();

    /**
     * InBytes of the ring at InAlign, kept until the frame that signals
     * InDoneAt has finished (Reclaim): its buffer, offset and mapped address.
     * Made on first use. False when the ring is full or InBytes larger than
     * it: the caller makes a staging buffer of its own.
     */
    bool Allocate(UINT64 InBytes, UINT64 InAlign, uint64_t InDoneAt,
                  ID3D12Resource*& OutBuffer, UINT64& OutOffset, uint8_t*& OutMapped);

    /** Gives back what frames up to fence value InCompleted used (all of it by default). */
    void Reclaim(uint64_t InCompleted = ~0ull);

    UINT64 GetBytesInUse() const;
    /* RING_BYTES once made, else 0. */
    UINT64 GetCapacity() const;

private:
    bool Create();

    struct Region
    {
        UINT64   Start  = 0;
        UINT64   End    = 0;
        uint64_t DoneAt = 0;
    };

    D3D12Device*                           Device = nullptr;
    mutable std::mutex                     Mutex;
    Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
    uint8_t*                               Mapped = nullptr;
    bool                                   bTried = false;
    UINT64                                 Head   = 0;   // where the next allocation starts
    std::deque<Region>                     Regions;      // live, oldest first
};

#endif //CORECLIENT_D3D12MEMORYHEAP_H

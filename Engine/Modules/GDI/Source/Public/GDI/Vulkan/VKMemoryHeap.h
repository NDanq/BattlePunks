#ifndef CORECLIENT_VKMEMORYHEAP_H
#define CORECLIENT_VKMEMORYHEAP_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

class VKDevice;

/*
 * Texture memory for the Vulkan backend, owned by VKDevice (it outlives every
 * texture, so a free always has somewhere to go):
 *
 *   VKImageHeap     device-local memory for sampled images, sub-allocated
 *                   (small ones) from 4 MB blocks instead of one
 *                   vkAllocateMemory each
 *   VKStagingRing   one persistently mapped upload buffer, reused frame
 *                   after frame instead of a buffer made and freed per upload
 *
 * Render thread, but both lock: a texture may be released elsewhere.
 */

/** Where an image's memory is: a range of a heap block, or its own allocation. */
struct VKAllocation
{
    VkDeviceMemory Memory    = VK_NULL_HANDLE;
    VkDeviceSize   Offset    = 0;
    VkDeviceSize   Size      = 0;
    uint32_t       Block     = UINT32_MAX;   // UINT32_MAX: its own vkAllocateMemory
    uint32_t       TypeIndex = 0;

    bool IsValid() const { return Memory != VK_NULL_HANDLE; }
};

class VKImageHeap
{
public:
    /* Images under SMALL_LIMIT -- most of them by count: a streamed
     * texture's tail, UI images -- share BLOCK_BYTES blocks; larger ones get
     * their own allocation. Large images in shared blocks were measured to
     * strand memory: once streaming dropped them, partly used blocks held
     * 3x what they still served, and nothing can be moved to compact them. */
    static constexpr VkDeviceSize BLOCK_BYTES = 4ull * 1024 * 1024;
    static constexpr VkDeviceSize SMALL_LIMIT = 512ull * 1024;

    VKImageHeap() = default;
    ~VKImageHeap() { Shutdown(); }

    VKImageHeap(const VKImageHeap&)            = delete;
    VKImageHeap& operator=(const VKImageHeap&) = delete;

    void Initialize(VKDevice* InDevice) { Device = InDevice; }

    /** Frees every block. What is still bound to one may no longer be used. */
    void Shutdown();

    /** InRequirements' bytes in a memory type with InProperties: a block's
     *  range under SMALL_LIMIT, else its own allocation. False if the
     *  device has no such type or memory. */
    bool Allocate(const VkMemoryRequirements& InRequirements, VkMemoryPropertyFlags InProperties, VKAllocation& OutAllocation);

    /** Back to its block (coalesced), or freed. A block left empty is freed
     *  when another empty block of its type is already kept. */
    void Free(const VKAllocation& InAllocation);

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
        VkDeviceSize Offset = 0;
        VkDeviceSize Size   = 0;
    };
    struct Block
    {
        VkDeviceMemory     Memory    = VK_NULL_HANDLE;   // null: a freed slot, reusable
        uint32_t           TypeIndex = 0;
        VkDeviceSize       Bytes     = 0;    // BLOCK_BYTES
        VkDeviceSize       Used      = 0;
        uint32_t           Live      = 0;
        std::vector<Range> Free;                          // by offset, never touching
    };

    /* InSize at InAlign from Blocks[InBlock]'s free ranges, first fit. */
    bool Carve(uint32_t InBlock, VkDeviceSize InSize, VkDeviceSize InAlign, VKAllocation& OutAllocation);

    VKDevice*          Device = nullptr;
    mutable std::mutex Mutex;
    std::vector<Block> Blocks;
    uint64_t           DedicatedBytes = 0;
    uint32_t           DedicatedCount = 0;
};

class VKStagingRing
{
public:
    static constexpr VkDeviceSize RING_BYTES = 16ull * 1024 * 1024;

    VKStagingRing() = default;
    ~VKStagingRing() { Shutdown(); }

    VKStagingRing(const VKStagingRing&)            = delete;
    VKStagingRing& operator=(const VKStagingRing&) = delete;

    void Initialize(VKDevice* InDevice) { Device = InDevice; }
    void Shutdown();

    /**
     * InBytes of the ring at InAlign, kept until the frame InDoneAt has
     * finished (Reclaim): its buffer, offset and mapped address. Made on
     * first use. False when the ring is full or InBytes larger than it: the
     * caller makes a staging buffer of its own.
     */
    bool Allocate(VkDeviceSize InBytes, VkDeviceSize InAlign, uint64_t InDoneAt,
                  VkBuffer& OutBuffer, VkDeviceSize& OutOffset, uint8_t*& OutMapped);

    /** Gives back what frames up to InCompleted used (all of it by default). */
    void Reclaim(uint64_t InCompleted = ~0ull);

    VkDeviceSize GetBytesInUse() const;
    /* RING_BYTES once made, else 0. */
    VkDeviceSize GetCapacity() const;

private:
    bool Create();

    struct Region
    {
        VkDeviceSize Start  = 0;
        VkDeviceSize End    = 0;
        uint64_t     DoneAt = 0;
    };

    VKDevice*          Device = nullptr;
    mutable std::mutex Mutex;
    VkBuffer           Buffer = VK_NULL_HANDLE;
    VkDeviceMemory     Memory = VK_NULL_HANDLE;
    uint8_t*           Mapped = nullptr;
    bool               bTried = false;
    VkDeviceSize       Head   = 0;        // where the next allocation starts
    std::deque<Region> Regions;           // live, oldest first
};

#endif //CORECLIENT_VKMEMORYHEAP_H

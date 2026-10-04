#ifndef CORECLIENT_VKDESCRIPTORALLOCATOR_H
#define CORECLIENT_VKDESCRIPTORALLOCATOR_H

#include <vulkan/vulkan.h>
#include <vector>

class VKDevice;

/**
 * Result of a per-frame UBO sub-allocation from the ring buffer.
 * Remains valid until VKDescriptorAllocator::Reset() is called.
 */
struct VKUBOAllocation
{
    VkBuffer      Buffer    = VK_NULL_HANDLE; ///< The shared ring buffer.
    VkDeviceSize  Offset    = 0;              ///< Byte offset within Buffer.
    void*         MappedPtr = nullptr;        ///< CPU pointer; write data here before the draw.
};

/**
 * Per-frame Vulkan resource allocator — two responsibilities:
 *
 *   1. Descriptor set allocation
 *      A ring of growable VkDescriptorPools (reset-semantics, no FREE_DESCRIPTOR_SET_BIT).
 *      Reset() reclaims all sets allocated during the previous frame.
 *      Pool sizes cover UNIFORM_BUFFER (up to 4 per set) and
 *      COMBINED_IMAGE_SAMPLER (up to 2 per set).
 *
 *   2. Per-frame UBO ring buffer
 *      Host-coherent VkBuffer chunks (kRingCapacity bytes each), sub-allocated
 *      linearly with 256-byte alignment (conservative
 *      minUniformBufferOffsetAlignment). A frame that fills a chunk moves on
 *      to the next, made on first need and kept (up to kRingChunkMax), so a
 *      busy frame -- shadow passes draw every caster once per view -- grows
 *      the ring rather than dropping draws. Reset() rewinds to the first.
 *      Callers write UBO data into VKUBOAllocation::MappedPtr and then
 *      reference the buffer + offset in VkDescriptorBufferInfo.
 */
class VKDescriptorAllocator
{
public:
    VKDescriptorAllocator()  = default;
    ~VKDescriptorAllocator() { Shutdown(); }

    /** Create the first descriptor pool and the UBO ring buffer. */
    bool Initialize(VKDevice* InDevice);

    /**
     * vkResetDescriptorPool all pools, restart from pool 0.
     * Also rewinds the UBO ring offset to 0.
     * Call once at frame start (BeginRendering already does this via GSync).
     */
    void Reset();

    /**
     * Allocate one descriptor set from Layout.
     * Grows the pool ring if the active pool is exhausted.
     * Returns VK_NULL_HANDLE on unrecoverable failure (a _LOG is emitted).
     */
    VkDescriptorSet Allocate(VkDescriptorSetLayout Layout);

    /**
     * Sub-allocate Size bytes from the per-frame UBO ring buffer.
     * The returned offset is aligned to kUBOAlignment bytes.
     * Returns an invalid allocation (.Buffer == VK_NULL_HANDLE) if the ring
     * is full (very unlikely at kRingCapacity for typical frame budgets).
     */
    VKUBOAllocation AllocateUBO(VkDeviceSize Size);

    /** Destroy all pools, ring buffer, and release device reference. */
    void Shutdown();

    inline bool IsValid() const { return !Pools.empty(); }

    /** Bumped by Reset(): an allocation from an older epoch is stale. */
    inline uint64_t GetEpoch() const { return Epoch; }

private:
    /** Append one new VkDescriptorPool to the ring. Returns false on failure. */
    bool GrowPool();

    /** Allocate and persistently map one more UBO ring chunk. */
    bool CreateRingChunk();

    struct RingChunk
    {
        VkBuffer       Buffer = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        void*          Mapped = nullptr;
    };

    VKDevice*               Device      = nullptr;

    // --- Descriptor pool ring ---
    std::vector<VkDescriptorPool> Pools;
    int                     ActivePool  = 0;

    // --- Per-frame UBO ring buffer (host-coherent, persistently mapped) ---
    std::vector<RingChunk>  RingChunks;
    size_t                  ActiveChunk = 0;
    VkDeviceSize            RingOffset  = 0;   // within RingChunks[ActiveChunk]
    uint64_t                Epoch       = 0;
    bool                    bWarnedFull = false;

    static constexpr uint32_t     kSetsPerPool  = 64;
    static constexpr VkDeviceSize kRingCapacity = 1024 * 1024; // 1 MB a chunk
    static constexpr size_t       kRingChunkMax = 64;          // 64 MB a frame, at most
    static constexpr VkDeviceSize kUBOAlignment = 256;        // conservative minUniformBufferOffsetAlignment
};

#endif //CORECLIENT_VKDESCRIPTORALLOCATOR_H

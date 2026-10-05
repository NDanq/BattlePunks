#ifndef CORECLIENT_VKMESH_H
#define CORECLIENT_VKMESH_H

#include <vulkan/vulkan.h>

#include "Public/GDI/GDIMesh.h"

class VKDevice;
class VKCurrentStateData;

/**
 * Vulkan GDIMesh implementation.
 *
 * Owns:
 *   - PositionBuffer  — device-local VkBuffer for vec3 positions  (binding 0)
 *   - TexCoordBuffer  — device-local VkBuffer for vec2 UVs         (binding 1)
 *   - InstanceBuffer  — host-coherent VkBuffer for mat4 transforms (binding 2, instanced)
 *
 * Vertex layout matches the OpenGL VBO layout so the same GLSL/SPIR-V
 * shaders work on both backends:
 *   location 0 — vec3  position
 *   location 1 — vec2  texcoord
 *   locations 2-5 — mat4 instance transform (4 × vec4, divisor 1)
 *
 * Buffer allocation uses raw vkAllocateMemory (Phase 2).
 * Phase 3 will migrate to VMA once vk_mem_alloc.h is vendored.
 *
 * Thread safety: InitResource() / Release() must be called from the render
 * thread (same thread that owns VKDevice).  Draw() is always called from
 * the render thread inside BeginRendering / EndRendering.
 */
class VKMesh : public GDIMesh
{
public:
    /**
     * @param InDevice    Borrowed pointer to the logical-device wrapper.
     * @param InStateData Borrowed pointer to the per-frame state block;
     *                    CurrentCmd is read at Draw() time.
     */
    VKMesh(VKDevice* InDevice, VKCurrentStateData* InStateData);
    ~VKMesh() override;

    /** Upload vertex data to device-local buffers via a transient command buffer. */
    void InitResource() override;

    /**
     * Make this a dynamic mesh (GDICreateDynamicMesh) instead: every stream of
     * the indexed layout in host-visible, persistently mapped memory, written
     * in place by UpdateDynamic. False (nothing allocated) on failure.
     */
    bool InitDynamic(uint32_t InMaxVertices, uint32_t InMaxIndices);

    /** Destroy all VkBuffers + memory, free CPU-side render data. */
    void Release() override;

    /**
     * Record draw call into the current command buffer.
     * Binds vertex buffers at bindings 0-2 and calls vkCmdDraw[Instanced].
     */
    void Draw() override;

protected:
    /** memcpy into the mapped streams; nothing is in flight there (GDIMesh::UpdateDynamic). */
    bool WriteDynamic(const GDIMeshDesc& InDesc, uint32_t InBaseVertex, uint32_t InBaseIndex) override;

    /**
     * Copy the last added transform into the persistently-mapped instance buffer.
     * The buffer is a frame-scoped append ring: draws are only recorded now and
     * execute at submit time, so earlier batches of the frame must keep their
     * slots. Each batch appends behind the previous one and Draw() offsets into
     * it via vkCmdDraw's firstInstance. When the ring runs out of room the GPU
     * is drained (FlushForReadback) before rewinding to slot 0.
     */
    void AddInstancedTransformInner() override;

    /** No-op: the ring cursor persists across batches; see AddInstancedTransformInner. */
    void ResetInstancedTransformInner() override;

private:
    /**
     * Allocate a VkBuffer + backing memory with the given usage / property flags.
     * Binds the memory to the buffer before returning.
     */
    bool CreateBuffer(VkDeviceSize         Size,
                      VkBufferUsageFlags   Usage,
                      VkMemoryPropertyFlags MemProps,
                      VkBuffer&            OutBuffer,
                      VkDeviceMemory&      OutMemory);

    /**
     * Upload Data (Size bytes) into DstBuffer (must have TRANSFER_DST usage)
     * via a host-visible staging buffer and a transient command buffer.
     * Submits synchronously and waits for completion.
     */
    bool UploadBufferData(VkBuffer DstBuffer, const void* Data, VkDeviceSize Size);

    // -----------------------------------------------------------------------
    // Resources (all VK_NULL_HANDLE until InitResource succeeds)
    // -----------------------------------------------------------------------
    VKDevice*            Device        = nullptr;
    VKCurrentStateData*  StateData     = nullptr;

    VkBuffer             PositionBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory       PositionMemory  = VK_NULL_HANDLE;

    VkBuffer             TexCoordBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory       TexCoordMemory  = VK_NULL_HANDLE;

    /** Indexed meshes only (GDICreateMesh): vec3 normals (binding 2, location 8) and uint32 indices. */
    VkBuffer             NormalBuffer    = VK_NULL_HANDLE;
    VkDeviceMemory       NormalMemory    = VK_NULL_HANDLE;
    VkBuffer             VertexColorBuffer = VK_NULL_HANDLE;   // indexed only; binding 3, location 9
    VkDeviceMemory       VertexColorMemory = VK_NULL_HANDLE;
    VkBuffer             UV1Buffer       = VK_NULL_HANDLE;     // indexed only; binding 4, location 10
    VkDeviceMemory       UV1Memory       = VK_NULL_HANDLE;
    VkBuffer             IndexBuffer     = VK_NULL_HANDLE;
    VkDeviceMemory       IndexMemory     = VK_NULL_HANDLE;

    /** A dynamic mesh's persistent mappings of the six buffers above. */
    void*                PositionMapped    = nullptr;
    void*                TexCoordMapped    = nullptr;
    void*                NormalMapped      = nullptr;
    void*                VertexColorMapped = nullptr;
    void*                UV1Mapped         = nullptr;
    void*                IndexMapped       = nullptr;

    /** Only valid when bSupportsInstanced == true. */
    VkBuffer             InstanceBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory       InstanceMemory  = VK_NULL_HANDLE;
    void*                InstanceMapped  = nullptr;   // persistently mapped

    /**
     * Parallel per-instance opacity ring (float per instance, binding 3 /
     * location 6). Advances in lockstep with the transform ring using the same
     * RingCursor / BatchStart, so no separate exhaustion handling is needed.
     */
    VkBuffer             OpacityBuffer   = VK_NULL_HANDLE;
    VkDeviceMemory       OpacityMemory   = VK_NULL_HANDLE;
    void*                OpacityMapped   = nullptr;   // persistently mapped

    /**
     * Parallel per-instance color ring (vec3 per instance, binding 4 /
     * location 7). Advances in lockstep with the transform ring using the same
     * RingCursor / BatchStart. Drives per-stamp RGB for pressure color transfer.
     */
    VkBuffer             ColorBuffer     = VK_NULL_HANDLE;
    VkDeviceMemory       ColorMemory     = VK_NULL_HANDLE;
    void*                ColorMapped     = nullptr;   // persistently mapped

    /** Ring sizing: room for this many MaxInstanced-sized batches per frame. */
    static constexpr int InstanceRingBatches = 16;
    /** Next free slot in the instance ring (absolute, persists across batches). */
    int                  RingCursor      = 0;
    /** Absolute slot of the current batch's first instance; Draw()'s firstInstance. */
    int                  BatchStart      = 0;

    bool                 bResourcesValid = false;
};

#endif //CORECLIENT_VKMESH_H

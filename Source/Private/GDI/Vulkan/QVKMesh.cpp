#include "Public/GDI/Vulkan/QVKMesh.h"
#include "Public/GDI/Vulkan/QVKDevice.h"
#include "Public/GDI/Vulkan/QVulkanDynamicGDI.h"   // QVKCurrentStateData, BindDrawState

#include <cstring>
#include "Public/GDI/GDILog.h"

// Global GDI driver (declared in GDI.cpp) — used to bind the pipeline/descriptors
// for the current state before issuing the draw.
extern GDIDynamicDriver* GDynamicGDI;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

QVKMesh::QVKMesh(QVKDevice* InDevice, QVKCurrentStateData* InStateData)
    : Device(InDevice)
    , StateData(InStateData)
{
}

QVKMesh::~QVKMesh()
{
    Release();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool QVKMesh::CreateBuffer(VkDeviceSize          Size,
                            VkBufferUsageFlags    Usage,
                            VkMemoryPropertyFlags MemProps,
                            VkBuffer&             OutBuffer,
                            VkDeviceMemory&       OutMemory)
{
    VkBufferCreateInfo BufInfo{};
    BufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufInfo.size        = Size;
    BufInfo.usage       = Usage;
    BufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(Device->GetDevice(), &BufInfo, nullptr, &OutBuffer) != VK_SUCCESS)
    {
        GDI_LOG("[QVKMesh] vkCreateBuffer failed.")
        return false;
    }

    VkMemoryRequirements MemReq{};
    vkGetBufferMemoryRequirements(Device->GetDevice(), OutBuffer, &MemReq);

    const uint32_t MemTypeIndex = Device->FindMemoryType(MemReq.memoryTypeBits, MemProps);
    if(MemTypeIndex == UINT32_MAX)
    {
        GDI_LOG("[QVKMesh] No suitable memory type for buffer.")
        vkDestroyBuffer(Device->GetDevice(), OutBuffer, nullptr);
        OutBuffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReq.size;
    AllocInfo.memoryTypeIndex = MemTypeIndex;

    if(vkAllocateMemory(Device->GetDevice(), &AllocInfo, nullptr, &OutMemory) != VK_SUCCESS)
    {
        GDI_LOG("[QVKMesh] vkAllocateMemory failed.")
        vkDestroyBuffer(Device->GetDevice(), OutBuffer, nullptr);
        OutBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(Device->GetDevice(), OutBuffer, OutMemory, 0);
    return true;
}

bool QVKMesh::UploadBufferData(VkBuffer DstBuffer, const void* Data, VkDeviceSize Size)
{
    // --- Staging buffer (host-visible + coherent) ---
    VkBuffer       StagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory StagingMem = VK_NULL_HANDLE;

    if(!CreateBuffer(Size,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     StagingBuf, StagingMem))
    {
        return false;
    }

    void* Mapped = nullptr;
    vkMapMemory(Device->GetDevice(), StagingMem, 0, Size, 0, &Mapped);
    std::memcpy(Mapped, Data, static_cast<size_t>(Size));
    vkUnmapMemory(Device->GetDevice(), StagingMem);

    // --- Transient command pool + buffer for the upload ---
    VkCommandPoolCreateInfo PoolInfo{};
    PoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    PoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    PoolInfo.queueFamilyIndex = Device->GetGraphicsQueueFamily();

    VkCommandPool UploadPool = VK_NULL_HANDLE;
    vkCreateCommandPool(Device->GetDevice(), &PoolInfo, nullptr, &UploadPool);

    VkCommandBufferAllocateInfo CBInfo{};
    CBInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    CBInfo.commandPool        = UploadPool;
    CBInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    CBInfo.commandBufferCount = 1;

    VkCommandBuffer UploadCmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(Device->GetDevice(), &CBInfo, &UploadCmd);

    VkCommandBufferBeginInfo BeginInfo{};
    BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(UploadCmd, &BeginInfo);

    VkBufferCopy Region{};
    Region.size = Size;
    vkCmdCopyBuffer(UploadCmd, StagingBuf, DstBuffer, 1, &Region);

    vkEndCommandBuffer(UploadCmd);

    // Submit and wait synchronously (staging upload is always a one-shot operation).
    VkSubmitInfo SubmitInfo{};
    SubmitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    SubmitInfo.commandBufferCount = 1;
    SubmitInfo.pCommandBuffers    = &UploadCmd;
    Device->QueueSubmit(1, &SubmitInfo, VK_NULL_HANDLE);
    Device->WaitIdle();

    // Cleanup transient resources (pool destroys the command buffer with it).
    vkDestroyCommandPool(Device->GetDevice(), UploadPool, nullptr);
    vkDestroyBuffer(Device->GetDevice(), StagingBuf, nullptr);
    vkFreeMemory(Device->GetDevice(), StagingMem, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// GDIMesh overrides
// ---------------------------------------------------------------------------

void QVKMesh::InitResource()
{
    if(!Device || !Device->IsValid()) return;

    // RenderData.NumVertices is the total float count of the position array
    // (e.g. 18 for a 6-vertex quad: 6 verts × 3 floats/vert).
    const uint NumPosFloats  = RenderData.NumVertices;
    const uint NumActualVerts = NumPosFloats / 3;

    // --- Position buffer (device-local) ---
    const VkDeviceSize PosSize = static_cast<VkDeviceSize>(NumPosFloats) * sizeof(float);
    if(!CreateBuffer(PosSize,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     PositionBuffer, PositionMemory))
    {
        return;
    }
    if(!UploadBufferData(PositionBuffer, RenderData.Position, PosSize))
    {
        Release();
        return;
    }

    // --- TexCoord buffer (device-local) ---
    // Each vertex carries 2 UV floats; OGL stored the same NumVertices count
    // in the buffer for simplicity but only 2-per-vertex are meaningful.
    const VkDeviceSize TexSize = static_cast<VkDeviceSize>(NumActualVerts * 2) * sizeof(float);
    if(!CreateBuffer(TexSize,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     TexCoordBuffer, TexCoordMemory))
    {
        Release();
        return;
    }
    if(!UploadBufferData(TexCoordBuffer, RenderData.TexCoord, TexSize))
    {
        Release();
        return;
    }

    // --- Instance transform buffer (host-coherent, persistently mapped) ---
    if(bSupportsInstanced)
    {
        // 16 floats per mat4. Sized as a frame-scoped ring of InstanceRingBatches
        // batches so batches recorded earlier in the frame keep their slots until
        // the command buffer is submitted (see AddInstancedTransformInner).
        const VkDeviceSize InstSize =
            static_cast<VkDeviceSize>(sizeof(float) * 16 * MaxInstanced * InstanceRingBatches);

        if(!CreateBuffer(InstSize,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         InstanceBuffer, InstanceMemory))
        {
            Release();
            return;
        }

        vkMapMemory(Device->GetDevice(), InstanceMemory, 0, InstSize, 0, &InstanceMapped);

        // Parallel per-instance opacity ring: one float per instance, same slot
        // count as the transform ring so a shared RingCursor addresses both.
        const VkDeviceSize OpacSize =
            static_cast<VkDeviceSize>(sizeof(float) * MaxInstanced * InstanceRingBatches);

        if(!CreateBuffer(OpacSize,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         OpacityBuffer, OpacityMemory))
        {
            Release();
            return;
        }

        vkMapMemory(Device->GetDevice(), OpacityMemory, 0, OpacSize, 0, &OpacityMapped);

        // Parallel per-instance color ring: vec3 per instance, same slot count as
        // the transform ring so the shared RingCursor addresses it too.
        const VkDeviceSize ColorSize =
            static_cast<VkDeviceSize>(sizeof(float) * 3 * MaxInstanced * InstanceRingBatches);

        if(!CreateBuffer(ColorSize,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         ColorBuffer, ColorMemory))
        {
            Release();
            return;
        }

        vkMapMemory(Device->GetDevice(), ColorMemory, 0, ColorSize, 0, &ColorMapped);
    }

    bResourcesValid = true;
}

void QVKMesh::Release()
{
    if(!Device || !Device->IsValid()) return;

    // Unmap first (CPU-side, always safe); the buffer destruction itself must be
    // deferred — meshes are deleted mid-frame (e.g. GDICursorObject::Resize when
    // the brush size changes) while the open/in-flight frame command buffer still
    // references the vertex buffers.
    if(InstanceMapped)
    {
        vkUnmapMemory(Device->GetDevice(), InstanceMemory);
        InstanceMapped = nullptr;
    }
    if(OpacityMapped)
    {
        vkUnmapMemory(Device->GetDevice(), OpacityMemory);
        OpacityMapped = nullptr;
    }
    if(ColorMapped)
    {
        vkUnmapMemory(Device->GetDevice(), ColorMemory);
        ColorMapped = nullptr;
    }

    auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI);
    auto DestroyOrDefer = [&](VkBuffer& Buf, VkDeviceMemory& Mem)
    {
        if(Buf == VK_NULL_HANDLE && Mem == VK_NULL_HANDLE) return;
        if(GDI)
        {
            GDI->DeferDestroyBuffer(Buf, Mem);
        }
        else
        {
            if(Buf != VK_NULL_HANDLE) vkDestroyBuffer(Device->GetDevice(), Buf, nullptr);
            if(Mem != VK_NULL_HANDLE) vkFreeMemory(Device->GetDevice(), Mem, nullptr);
        }
        Buf = VK_NULL_HANDLE;
        Mem = VK_NULL_HANDLE;
    };

    DestroyOrDefer(InstanceBuffer, InstanceMemory);
    DestroyOrDefer(OpacityBuffer,  OpacityMemory);
    DestroyOrDefer(ColorBuffer,    ColorMemory);
    DestroyOrDefer(TexCoordBuffer, TexCoordMemory);
    DestroyOrDefer(PositionBuffer, PositionMemory);

    // Free the CPU-side copies that were malloc'd by CreateQuadMesh.
    if(RenderData.Position) { free(RenderData.Position); RenderData.Position = nullptr; }
    if(RenderData.TexCoord) { free(RenderData.TexCoord); RenderData.TexCoord = nullptr; }

    bResourcesValid = false;
}

void QVKMesh::Draw()
{
    if(!bResourcesValid || !StateData) return;

    const VkCommandBuffer Cmd = StateData->CurrentCmd;
    if(Cmd == VK_NULL_HANDLE) return;

    // RenderData.NumVertices is total position-float count; divide by 3 for actual vertex count.
    const uint32_t VertexCount = RenderData.NumVertices / 3;
    if(VertexCount == 0) return;

    // Bind the graphics pipeline + descriptor set for the current GDI state. This must
    // happen for EVERY draw — including a direct Mesh->Draw() (layer compositing /
    // LayerBlit), which does not go through DrawObject. If state is incomplete, skip
    // the draw rather than issue vkCmdDraw with no pipeline bound.
    if(auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI))
    {
        if(!GDI->BindDrawState(this, IsInstanced()))
            return;
    }

    // Bind position (binding 0) and texcoord (binding 1) buffers.
    const VkBuffer     Buffers[2] = { PositionBuffer, TexCoordBuffer };
    const VkDeviceSize Offsets[2] = { 0, 0 };
    vkCmdBindVertexBuffers(Cmd, 0, 2, Buffers, Offsets);

    if(bSupportsInstanced)
    {
        // Zero instances draws nothing — same as glDrawArraysInstanced(..., 0).
        // (Falling through to a non-instanced draw would render one phantom quad
        // with the instanced pipeline's binding 2 unbound.)
        if(NumInstanced <= 0 || InstanceBuffer == VK_NULL_HANDLE) return;

        // Bind instance transform buffer at binding 2 and the parallel opacity
        // buffer at binding 3. firstInstance offsets BOTH per-instance attribute
        // fetches to this batch's region of the shared ring.
        const VkDeviceSize InstOffset = 0;
        vkCmdBindVertexBuffers(Cmd, 2, 1, &InstanceBuffer, &InstOffset);
        if(OpacityBuffer != VK_NULL_HANDLE)
        {
            const VkDeviceSize OpacOffset = 0;
            vkCmdBindVertexBuffers(Cmd, 3, 1, &OpacityBuffer, &OpacOffset);
        }
        if(ColorBuffer != VK_NULL_HANDLE)
        {
            const VkDeviceSize ColorOffset = 0;
            vkCmdBindVertexBuffers(Cmd, 4, 1, &ColorBuffer, &ColorOffset);
        }
        vkCmdDraw(Cmd, VertexCount, static_cast<uint32_t>(NumInstanced), 0,
                  static_cast<uint32_t>(BatchStart));
    }
    else
    {
        vkCmdDraw(Cmd, VertexCount, 1, 0, 0);
    }
}

void QVKMesh::AddInstancedTransformInner()
{
    if(!InstanceMapped || InstancedTransforms.empty()) return;

    // NumInstanced has already been incremented by the base AddInstancedTransform() call;
    // the new transform is element NumInstanced - 1 of the current batch.
    const int Local = NumInstanced - 1;
    if(Local < 0 || Local >= MaxInstanced) return;

    if(Local == 0)
    {
        // New batch: append behind the previous batches of this frame — their draws
        // are only recorded and will read the buffer at submit time. If a full
        // batch (base caps batches at MaxInstanced) no longer fits, the ring is
        // exhausted: drain the GPU so the recorded draws complete, then rewind.
        if(RingCursor + MaxInstanced > MaxInstanced * InstanceRingBatches)
        {
            if(auto* GDI = dynamic_cast<QVulkanDynamicGDI*>(GDynamicGDI))
                GDI->FlushForReadback(true);
            RingCursor = 0;
        }
        BatchStart = RingCursor;
    }

    const int Slot = BatchStart + Local;

    const Matrix4x4& Transform = InstancedTransforms.back();

    // Convert to 16 packed floats (Elem may be EngineCore::Real/double in non-SIMD builds).
    // The mat4 attribute at locations 2-5 is assembled from 4 column vec4s, so
    // pack column-major — the same transpose QVKShaderResource::SetUniform
    // applies when uploading Matrix4x4 uniforms.
    float MatData[16];
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            MatData[Col * 4 + Row] = static_cast<float>(Transform.Data[Row][Col]);

    constexpr size_t MatStride = sizeof(float) * 16;
    std::memcpy(static_cast<uint8_t*>(InstanceMapped) + Slot * MatStride,
                MatData, MatStride);

    // Same slot in the parallel opacity ring.
    if(OpacityMapped && !InstancedOpacities.empty())
    {
        const float Opac = InstancedOpacities.back();
        std::memcpy(static_cast<uint8_t*>(OpacityMapped) + Slot * sizeof(float),
                    &Opac, sizeof(float));
    }

    // Same slot in the parallel color ring (vec3).
    if(ColorMapped && !InstancedColors.empty())
    {
        const Vector3& C = InstancedColors.back();
        const float ColorData[3] = {
            static_cast<float>(C.GetX()),
            static_cast<float>(C.GetY()),
            static_cast<float>(C.GetZ()) };
        constexpr size_t ColorStride = sizeof(float) * 3;
        std::memcpy(static_cast<uint8_t*>(ColorMapped) + Slot * ColorStride,
                    ColorData, ColorStride);
    }

    RingCursor = Slot + 1;
}

void QVKMesh::ResetInstancedTransformInner()
{
    // Intentionally does NOT rewind the ring: earlier batches of the frame are
    // referenced by draws that haven't executed yet. The next batch appends at
    // RingCursor; see AddInstancedTransformInner.  NumInstanced and
    // InstancedTransforms are already cleared by GDIMesh::ResetInstanced().
}

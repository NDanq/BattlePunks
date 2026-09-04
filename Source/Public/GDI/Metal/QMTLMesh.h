#ifndef CORECLIENT_QMTLMESH_H
#define CORECLIENT_QMTLMESH_H

#include "Public/GDI/GDIMesh.h"
#include "Public/GDI/Metal/MTLForward.h"

class QMTLDevice;
class QMTLCurrentStateData;

/**
 * Metal GDIMesh implementation -- the counterpart of QVKMesh.
 *
 * Owns:
 *   - PositionBuffer  -- float3 positions        (vertex buffer 30)
 *   - TexCoordBuffer  -- float2 UVs              (vertex buffer 29)
 *   - InstanceBuffer  -- mat4 per-instance xform (vertex buffer 28, instanced)
 *   - OpacityBuffer   -- float per instance      (vertex buffer 27, instanced)
 *   - ColorBuffer     -- float3 per instance     (vertex buffer 26, instanced)
 *
 * (The indices come from MTLBindings in QMTLPipelineCache.h -- attribute buffers
 * share the vertex stage's index space with the uniform buffers, so they are
 * allocated from the top of the range.)
 *
 * Every buffer uses SHARED storage, so the Vulkan staging-buffer + transient
 * command-buffer upload path collapses to a single memcpy into `contents`. The
 * per-instance buffers keep the same frame-scoped ring design as QVKMesh: batches
 * recorded earlier in a frame must keep their slots until the command buffer is
 * committed, so each batch appends behind the last and Draw() offsets into the
 * ring with baseInstance.
 *
 * Thread safety: InitResource() / Release() / Draw() are render-thread only.
 */
class QMTLMesh : public GDIMesh
{
public:
    /**
     * @param InDevice    Borrowed pointer to the device wrapper.
     * @param InStateData Borrowed pointer to the per-frame state block; the active
     *                    render encoder is read from it at Draw() time.
     */
    QMTLMesh(QMTLDevice* InDevice, QMTLCurrentStateData* InStateData);
    ~QMTLMesh() override;

    void InitResource() override;
    void Release() override;
    void Draw() override;

protected:
    /** Copy the last added transform/opacity/color into the shared instance ring. */
    void AddInstancedTransformInner() override;

    /** No-op: the ring cursor persists across batches; see AddInstancedTransformInner. */
    void ResetInstancedTransformInner() override;

private:
    /** Allocate a shared-storage MTLBuffer of Size bytes; nullptr on failure. */
    MTLBufferRef CreateSharedBuffer(uint64_t Size, const char* DebugLabel);

    QMTLDevice*           Device    = nullptr;
    QMTLCurrentStateData* StateData = nullptr;

    MTLBufferRef PositionBuffer = nullptr;
    MTLBufferRef TexCoordBuffer = nullptr;

    // Only valid when bSupportsInstanced == true.
    MTLBufferRef InstanceBuffer = nullptr;
    MTLBufferRef OpacityBuffer  = nullptr;
    MTLBufferRef ColorBuffer    = nullptr;

    /** Ring sizing: room for this many MaxInstanced-sized batches per frame. */
    static constexpr int InstanceRingBatches = 16;
    /** Next free slot in the instance ring (absolute, persists across batches). */
    int  RingCursor      = 0;
    /** Absolute slot of the current batch's first instance; Draw()'s baseInstance. */
    int  BatchStart      = 0;

    bool bResourcesValid = false;
};

#endif //CORECLIENT_QMTLMESH_H

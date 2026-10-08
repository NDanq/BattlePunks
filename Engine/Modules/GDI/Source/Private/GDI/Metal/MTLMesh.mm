#include "Public/GDI/Metal/MTLMesh.h"
#include "Public/GDI/Metal/MTLDevice.h"
#include "Public/GDI/Metal/MetalDynamicGDI.h"
#include "Private/GDI/Metal/MTLPipelineCache.h"   // MTLBindings

#include <cstring>
#include <cstdlib>
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// Global GDI driver (declared in GDI.cpp) -- used to bind the pipeline / uniforms
// for the current state before issuing the draw.
extern GDIDynamicDriver* GDynamicGDI;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MTLMesh::MTLMesh(MTLDevice* InDevice, MTLCurrentStateData* InStateData)
    : Device(InDevice)
    , StateData(InStateData)
{
}

MTLMesh::~MTLMesh()
{
    Release();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

MTLBufferRef MTLMesh::CreateSharedBuffer(uint64_t Size, const char* DebugLabel)
{
    if(!Device || !Device->IsValid() || Size == 0) return nullptr;

    id<MTLBuffer> Buffer = [Device->GetDevice() newBufferWithLength:(NSUInteger)Size
                                                            options:MTLResourceStorageModeShared];   // +1
    if(Buffer == nil)
    {
        GDI_LOG(std::string("[MTLMesh] newBufferWithLength failed for ") + ::GDILogDetail::ToLogString(DebugLabel))
        return nullptr;
    }
    Buffer.label = [NSString stringWithUTF8String:DebugLabel];
    return Buffer;
}

// ---------------------------------------------------------------------------
// GDIMesh overrides
// ---------------------------------------------------------------------------

void MTLMesh::InitResource()
{
    if(!Device || !Device->IsValid()) return;

    // RenderData.NumVertices is the total float count of the position array
    // (e.g. 18 for a 6-vertex quad: 6 verts x 3 floats/vert).
    const uint NumPosFloats   = RenderData.NumVertices;
    const uint NumActualVerts = NumPosFloats / 3;
    if(NumActualVerts == 0) return;

    // Shared storage: no staging buffer, no transient command buffer, no GPU
    // upload pass -- the CPU writes straight into the buffer the GPU will read.
    const uint64_t PosSize = uint64_t(NumPosFloats) * sizeof(float);
    PositionBuffer = CreateSharedBuffer(PosSize, "Odessa.Mesh.Position");
    if(PositionBuffer == nullptr) return;
    std::memcpy([(id<MTLBuffer>)PositionBuffer contents], RenderData.Position, size_t(PosSize));

    const uint64_t TexSize = uint64_t(NumActualVerts) * 2u * sizeof(float);
    TexCoordBuffer = CreateSharedBuffer(TexSize, "Odessa.Mesh.TexCoord");
    if(TexCoordBuffer == nullptr) { Release(); return; }
    std::memcpy([(id<MTLBuffer>)TexCoordBuffer contents], RenderData.TexCoord, size_t(TexSize));

    if(bSupportsInstanced)
    {
        // Frame-scoped rings: batches recorded earlier in the frame keep their slots
        // until the command buffer is committed (see AddInstancedTransformInner).
        const uint64_t Slots = uint64_t(MaxInstanced) * uint64_t(InstanceRingBatches);

        InstanceBuffer = CreateSharedBuffer(Slots * 16u * sizeof(float), "Odessa.Mesh.InstanceXform");
        if(InstanceBuffer == nullptr) { Release(); return; }

        OpacityBuffer = CreateSharedBuffer(Slots * sizeof(float), "Odessa.Mesh.InstanceOpacity");
        if(OpacityBuffer == nullptr) { Release(); return; }

        ColorBuffer = CreateSharedBuffer(Slots * 3u * sizeof(float), "Odessa.Mesh.InstanceColor");
        if(ColorBuffer == nullptr) { Release(); return; }
    }

    bResourcesValid = true;
}

void MTLMesh::Release()
{
    // No deferred destruction: a committed MTLCommandBuffer retains every buffer it
    // references, so releasing here cannot invalidate an in-flight frame even when
    // a mesh is deleted mid-frame (GDICursorObject::Resize does exactly that).
    auto ReleaseBuffer = [](MTLBufferRef& Buf)
    {
        if(Buf != nullptr)
        {
            [(id<MTLBuffer>)Buf release];
            Buf = nullptr;
        }
    };

    ReleaseBuffer(InstanceBuffer);
    ReleaseBuffer(OpacityBuffer);
    ReleaseBuffer(ColorBuffer);
    ReleaseBuffer(TexCoordBuffer);
    ReleaseBuffer(PositionBuffer);

    // Free the CPU-side copies malloc'd by CreateQuadMesh.
    if(RenderData.Position) { free(RenderData.Position); RenderData.Position = nullptr; }
    if(RenderData.TexCoord) { free(RenderData.TexCoord); RenderData.TexCoord = nullptr; }
    if(RenderData.Color)    { free(RenderData.Color);    RenderData.Color    = nullptr; }
    if(RenderData.UV1)      { free(RenderData.UV1);      RenderData.UV1      = nullptr; }
    if(RenderData.Joints)   { free(RenderData.Joints);   RenderData.Joints   = nullptr; }
    if(RenderData.Weights)  { free(RenderData.Weights);  RenderData.Weights  = nullptr; }

    bResourcesValid = false;
}

void MTLMesh::Draw()
{
    if(!bResourcesValid || !StateData) return;

    id<MTLRenderCommandEncoder> Encoder = (id<MTLRenderCommandEncoder>)StateData->CurrentEncoder;
    if(Encoder == nil) return;

    // RenderData.NumVertices is the total position-float count.
    const NSUInteger VertexCount = RenderData.NumVertices / 3;
    if(VertexCount == 0) return;

    // Bind pipeline + uniforms + textures for the current GDI state. This must
    // happen for EVERY draw, including a direct Mesh->Draw() (layer compositing),
    // which does not go through DrawObject. If state is incomplete, skip the draw
    // rather than issue it with no pipeline bound.
    if(auto* GDI = dynamic_cast<MetalDynamicGDI*>(GDynamicGDI))
    {
        if(!GDI->BindDrawState(this, IsInstanced()))
            return;
    }

    [Encoder setVertexBuffer:(id<MTLBuffer>)PositionBuffer offset:0 atIndex:MTLBindings::PositionBuffer];
    [Encoder setVertexBuffer:(id<MTLBuffer>)TexCoordBuffer offset:0 atIndex:MTLBindings::TexCoordBuffer];

    if(bSupportsInstanced)
    {
        // Zero instances draws nothing -- same as glDrawArraysInstanced(..., 0).
        // (Falling through to a non-instanced draw would render one phantom quad
        // through the instanced pipeline with its per-instance buffers unbound.)
        if(NumInstanced <= 0 || InstanceBuffer == nullptr) return;

        [Encoder setVertexBuffer:(id<MTLBuffer>)InstanceBuffer offset:0 atIndex:MTLBindings::InstanceBuffer];
        if(OpacityBuffer) [Encoder setVertexBuffer:(id<MTLBuffer>)OpacityBuffer offset:0 atIndex:MTLBindings::OpacityBuffer];
        if(ColorBuffer)   [Encoder setVertexBuffer:(id<MTLBuffer>)ColorBuffer   offset:0 atIndex:MTLBindings::ColorBuffer];

        // baseInstance offsets every per-instance attribute fetch to this batch's
        // region of the shared ring -- the exact role of Vulkan's firstInstance.
        [Encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:VertexCount
                  instanceCount:(NSUInteger)NumInstanced
                   baseInstance:(NSUInteger)BatchStart];
    }
    else
    {
        [Encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:VertexCount];
    }
}

void MTLMesh::AddInstancedTransformInner()
{
    if(InstanceBuffer == nullptr || InstancedTransforms.empty()) return;

    // NumInstanced was already incremented by the base AddInstancedTransform();
    // the new transform is element NumInstanced - 1 of the current batch.
    const int Local = NumInstanced - 1;
    if(Local < 0 || Local >= MaxInstanced) return;

    if(Local == 0)
    {
        // New batch: append behind the previous batches of this frame -- their draws
        // are only recorded and will read the buffer at execution time. If a full
        // batch no longer fits, the ring is exhausted: drain the GPU so the recorded
        // draws complete, then rewind.
        if(RingCursor + MaxInstanced > MaxInstanced * InstanceRingBatches)
        {
            if(auto* GDI = dynamic_cast<MetalDynamicGDI*>(GDynamicGDI))
                GDI->FlushForReadback(true);
            RingCursor = 0;
        }
        BatchStart = RingCursor;
    }

    const int Slot = BatchStart + Local;

    // Pack column-major: the mat4 attribute at locations 2-5 is assembled from four
    // column vec4s -- the same transpose MTLShaderResource::SetUniform applies.
    const Matrix4x4& Transform = InstancedTransforms.back();
    float MatData[16];
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            MatData[Col * 4 + Row] = static_cast<float>(Transform.Data[Row][Col]);

    constexpr size_t MatStride = sizeof(float) * 16;
    auto* InstanceBytes = static_cast<uint8_t*>([(id<MTLBuffer>)InstanceBuffer contents]);
    std::memcpy(InstanceBytes + Slot * MatStride, MatData, MatStride);

    // Same slot in the parallel opacity ring.
    if(OpacityBuffer != nullptr && !InstancedOpacities.empty())
    {
        const float Opac = InstancedOpacities.back();
        auto* OpacityBytes = static_cast<uint8_t*>([(id<MTLBuffer>)OpacityBuffer contents]);
        std::memcpy(OpacityBytes + Slot * sizeof(float), &Opac, sizeof(float));
    }

    // Same slot in the parallel color ring (float3).
    if(ColorBuffer != nullptr && !InstancedColors.empty())
    {
        const Vector3& C = InstancedColors.back();
        const float ColorData[3] = { static_cast<float>(C.GetX()),
                                     static_cast<float>(C.GetY()),
                                     static_cast<float>(C.GetZ()) };
        constexpr size_t ColorStride = sizeof(float) * 3;
        auto* ColorBytes = static_cast<uint8_t*>([(id<MTLBuffer>)ColorBuffer contents]);
        std::memcpy(ColorBytes + Slot * ColorStride, ColorData, ColorStride);
    }

    RingCursor = Slot + 1;
}

void MTLMesh::ResetInstancedTransformInner()
{
    // Intentionally does NOT rewind the ring: earlier batches of the frame are
    // referenced by draws that haven't executed yet. The next batch appends at
    // RingCursor. NumInstanced / InstancedTransforms are already cleared by
    // GDIMesh::ResetInstanced().
}

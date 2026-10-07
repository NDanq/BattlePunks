#include "Public/GDI/D3D12/D3D12Mesh.h"
#include "Public/GDI/D3D12/D3D12Buffer.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12PipelineCache.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <cstdlib>
#include <cstring>
#include <memory>

#include "Public/GDI/GDILog.h"

extern GDIDynamicDriver* GDynamicGDI;

D3D12Mesh::D3D12Mesh(D3D12Device* InDevice, D3D12CurrentStateData* InStateData)
    : Device(InDevice)
    , StateData(InStateData)
{
}

D3D12Mesh::~D3D12Mesh()
{
    Release();
}

bool D3D12Mesh::CreateRing(RingBuffer& OutRing, UINT InStride)
{
    const UINT64 Size = UINT64(InStride) * UINT64(MaxInstanced) * UINT64(InstanceRingBatches);
    OutRing.Buffer = D3D12Ops::CreateBuffer(Device, Size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!OutRing.Buffer) return false;

    const D3D12_RANGE NoRead{ 0, 0 };
    if(FAILED(OutRing.Buffer->Map(0, &NoRead, &OutRing.Mapped)))
    {
        OutRing.Buffer.Reset();
        return false;
    }
    OutRing.Stride = InStride;
    return true;
}

void D3D12Mesh::InitResource()
{
    if(!Device || !Device->IsValid()) return;

    // NumVertices is the position FLOAT count, three per vertex.
    const UINT NumVerts = RenderData.NumVertices / 3;
    PositionBytes = RenderData.NumVertices * sizeof(float);
    TexCoordBytes = NumVerts * 2 * sizeof(float);
    if(PositionBytes == 0) return;

    PositionBuffer = D3D12Ops::CreateBuffer(Device, PositionBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    TexCoordBuffer = D3D12Ops::CreateBuffer(Device, TexCoordBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if(!PositionBuffer || !TexCoordBuffer ||
       !D3D12Ops::UploadBuffer(Device, PositionBuffer.Get(), RenderData.Position, PositionBytes) ||
       !D3D12Ops::UploadBuffer(Device, TexCoordBuffer.Get(), RenderData.TexCoord, TexCoordBytes))
    {
        Release();
        return;
    }

    if(RenderData.Normal && RenderData.Color && RenderData.UV1 && IsIndexed())
    {
        const UINT ColorBytes = NumVerts * 4 * sizeof(float);
        UV1Buffer    = D3D12Ops::CreateBuffer(Device, TexCoordBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        IndexBytes   = RenderData.NumIndices * sizeof(uint32_t);
        NormalBuffer = D3D12Ops::CreateBuffer(Device, PositionBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        ColorBuffer  = D3D12Ops::CreateBuffer(Device, ColorBytes,    D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        IndexBuffer  = D3D12Ops::CreateBuffer(Device, IndexBytes,    D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        if(!NormalBuffer || !ColorBuffer || !UV1Buffer || !IndexBuffer ||
           !D3D12Ops::UploadBuffer(Device, NormalBuffer.Get(), RenderData.Normal, PositionBytes) ||
           !D3D12Ops::UploadBuffer(Device, ColorBuffer.Get(), RenderData.Color, ColorBytes) ||
           !D3D12Ops::UploadBuffer(Device, UV1Buffer.Get(), RenderData.UV1, TexCoordBytes) ||
           !D3D12Ops::UploadBuffer(Device, IndexBuffer.Get(), RenderData.Indices, IndexBytes))
        {
            Release();
            return;
        }
    }

    if(IsSkinned())
    {
        const UINT SkinBytes = NumVerts * 4 * sizeof(float);
        JointBuffer  = D3D12Ops::CreateBuffer(Device, SkinBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        WeightBuffer = D3D12Ops::CreateBuffer(Device, SkinBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        if(!JointBuffer || !WeightBuffer ||
           !D3D12Ops::UploadBuffer(Device, JointBuffer.Get(), RenderData.Joints, SkinBytes) ||
           !D3D12Ops::UploadBuffer(Device, WeightBuffer.Get(), RenderData.Weights, SkinBytes))
        {
            Release();
            return;
        }
    }

    if(bSupportsInstanced)
    {
        if(!CreateRing(TransformRing, sizeof(float) * 16) ||
           !CreateRing(OpacityRing,   sizeof(float)) ||
           !CreateRing(ColorRing,     sizeof(float) * 3))
        {
            Release();
            return;
        }
    }

    bResourcesValid = true;
    ApplyDebugName();
}

bool D3D12Mesh::InitDynamic(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    if(!Device || !Device->IsValid() || bResourcesValid) return false;
    MakeDynamic(InMaxVertices, InMaxIndices);

    PositionBytes = InMaxVertices * 3 * sizeof(float);
    TexCoordBytes = InMaxVertices * 2 * sizeof(float);
    IndexBytes    = InMaxIndices * sizeof(uint32_t);

    Microsoft::WRL::ComPtr<ID3D12Resource>* Buffers[6] = { std::addressof(PositionBuffer), std::addressof(TexCoordBuffer), std::addressof(NormalBuffer),
                                                           std::addressof(ColorBuffer), std::addressof(UV1Buffer), std::addressof(IndexBuffer) };
    const UINT Bytes[6] = { PositionBytes, TexCoordBytes, PositionBytes, InMaxVertices * 4 * UINT(sizeof(float)),
                            TexCoordBytes, IndexBytes };
    const D3D12_RANGE NoRead{ 0, 0 };
    for(int At = 0; At < 6; ++At)
    {
        *Buffers[At] = D3D12Ops::CreateBuffer(Device, Bytes[At], D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if(!*Buffers[At] || FAILED((*Buffers[At])->Map(0, &NoRead, &DynamicMapped[At])))
        {
            DynamicMapped[At] = nullptr;
            Release();
            return false;
        }
    }

    // Normals and UV1 are written only when an update has them: zero until then.
    std::memset(DynamicMapped[2], 0, Bytes[2]);
    std::memset(DynamicMapped[4], 0, Bytes[4]);

    bResourcesValid = true;
    ApplyDebugName();
    return true;
}

void D3D12Mesh::SetDebugName(const std::string& InName)
{
    GDIMesh::SetDebugName(InName);
    ApplyDebugName();
}

void D3D12Mesh::ApplyDebugName() const
{
    if(DebugName.empty()) return;
    const auto Name = [this](const Microsoft::WRL::ComPtr<ID3D12Resource>& InBuffer, const char* InStream)
    {
        D3D12Util::SetObjectName(InBuffer.Get(), DebugName + " " + InStream);
    };
    Name(PositionBuffer,       "Positions");
    Name(TexCoordBuffer,       "TexCoords");
    Name(NormalBuffer,         "Normals");
    Name(ColorBuffer,          "Colors");
    Name(UV1Buffer,            "UV1");
    Name(JointBuffer,          "Joints");
    Name(WeightBuffer,         "Weights");
    Name(IndexBuffer,          "Indices");
    Name(TransformRing.Buffer, "Instance Transforms");
    Name(OpacityRing.Buffer,   "Instance Opacities");
    Name(ColorRing.Buffer,     "Instance Colors");
}

bool D3D12Mesh::WriteDynamic(const GDIMeshDesc& InDesc, const uint32_t InBaseVertex, const uint32_t InBaseIndex)
{
    if(!bResourcesValid || !DynamicMapped[0]) return false;

    const size_t Base = InBaseVertex;
    const size_t N    = InDesc.NumVertices;
    std::memcpy(static_cast<float*>(DynamicMapped[0]) + Base * 3, InDesc.Positions, N * 3 * sizeof(float));
    std::memcpy(static_cast<float*>(DynamicMapped[1]) + Base * 2, InDesc.UV0,       N * 2 * sizeof(float));
    if(InDesc.Normals) std::memcpy(static_cast<float*>(DynamicMapped[2]) + Base * 3, InDesc.Normals, N * 3 * sizeof(float));
    if(InDesc.UV1)     std::memcpy(static_cast<float*>(DynamicMapped[4]) + Base * 2, InDesc.UV1,     N * 2 * sizeof(float));

    float* Colors = static_cast<float*>(DynamicMapped[3]) + Base * 4;
    if(InDesc.Colors)
    {
        std::memcpy(Colors, InDesc.Colors, N * 4 * sizeof(float));
    }
    else
    {
        for(size_t At = 0; At < N * 4; ++At) { Colors[At] = 1.0f; }
    }
    std::memcpy(static_cast<uint32_t*>(DynamicMapped[5]) + InBaseIndex, InDesc.Indices, size_t(InDesc.NumIndices) * sizeof(uint32_t));
    return true;
}

void D3D12Mesh::Release()
{
    // std::addressof, not &: a ComPtr's operator& releases what it holds, which here
    // would free the buffers at once instead of deferring them past the frames using them.
    Microsoft::WRL::ComPtr<ID3D12Resource>* Dynamic[6] = { std::addressof(PositionBuffer), std::addressof(TexCoordBuffer), std::addressof(NormalBuffer),
                                                           std::addressof(ColorBuffer), std::addressof(UV1Buffer), std::addressof(IndexBuffer) };
    for(int At = 0; At < 6; ++At)
    {
        if(DynamicMapped[At] && *Dynamic[At]) (*Dynamic[At])->Unmap(0, nullptr);
        DynamicMapped[At] = nullptr;
    }

    // Deferred: meshes are deleted mid-frame (a cursor resize) while the open list still binds them.
    auto* GDI = dynamic_cast<D3D12DynamicGDI*>(GDynamicGDI);
    auto ReleaseOrDefer = [GDI](Microsoft::WRL::ComPtr<ID3D12Resource>& Buffer)
    {
        if(Buffer && GDI) GDI->DeferRelease(Buffer);
        Buffer.Reset();
    };

    for(RingBuffer* Ring : { &TransformRing, &OpacityRing, &ColorRing })
    {
        if(Ring->Buffer && Ring->Mapped) Ring->Buffer->Unmap(0, nullptr);
        Ring->Mapped = nullptr;
        ReleaseOrDefer(Ring->Buffer);
    }
    ReleaseOrDefer(PositionBuffer);
    ReleaseOrDefer(TexCoordBuffer);
    ReleaseOrDefer(NormalBuffer);
    ReleaseOrDefer(ColorBuffer);
    ReleaseOrDefer(UV1Buffer);
    ReleaseOrDefer(JointBuffer);
    ReleaseOrDefer(WeightBuffer);
    ReleaseOrDefer(IndexBuffer);

    FreeRenderData();

    bResourcesValid = false;
}

void D3D12Mesh::BindRing(ID3D12GraphicsCommandList* InList, UINT InSlot, const RingBuffer& InRing) const
{
    D3D12_VERTEX_BUFFER_VIEW View{};
    View.BufferLocation = InRing.Buffer->GetGPUVirtualAddress();
    View.SizeInBytes    = UINT(InRing.Buffer->GetDesc().Width);
    View.StrideInBytes  = InRing.Stride;
    InList->IASetVertexBuffers(InSlot, 1, &View);
}

void D3D12Mesh::Draw()
{
    if(!bResourcesValid || !StateData || !StateData->CurrentCmd) return;

    const UINT VertexCount = RenderData.NumVertices / 3;
    if(VertexCount == 0) return;

    // Every draw binds state here, including a direct Mesh->Draw() that bypasses DrawObject.
    auto* GDI = dynamic_cast<D3D12DynamicGDI*>(GDynamicGDI);
    if(!GDI || !GDI->BindDrawState(this, IsInstanced())) return;

    ID3D12GraphicsCommandList* List = StateData->CurrentCmd;

    D3D12_VERTEX_BUFFER_VIEW Views[2]{};
    Views[0].BufferLocation = PositionBuffer->GetGPUVirtualAddress();
    Views[0].SizeInBytes    = PositionBytes;
    Views[0].StrideInBytes  = sizeof(float) * 3;
    Views[1].BufferLocation = TexCoordBuffer->GetGPUVirtualAddress();
    Views[1].SizeInBytes    = TexCoordBytes;
    Views[1].StrideInBytes  = sizeof(float) * 2;
    List->IASetVertexBuffers(0, 2, Views);

    if(IndexBuffer)
    {
        uint32_t First = 0, Count = 0;
        GetClampedDrawRange(First, Count);
        if(Count == 0) return;

        D3D12_VERTEX_BUFFER_VIEW MeshViews[3]{};
        MeshViews[0].BufferLocation = NormalBuffer->GetGPUVirtualAddress();
        MeshViews[0].SizeInBytes    = PositionBytes;
        MeshViews[0].StrideInBytes  = sizeof(float) * 3;
        MeshViews[1].BufferLocation = ColorBuffer->GetGPUVirtualAddress();
        MeshViews[1].SizeInBytes    = (PositionBytes / 3) * 4;
        MeshViews[1].StrideInBytes  = sizeof(float) * 4;
        MeshViews[2].BufferLocation = UV1Buffer->GetGPUVirtualAddress();
        MeshViews[2].SizeInBytes    = TexCoordBytes;
        MeshViews[2].StrideInBytes  = sizeof(float) * 2;
        List->IASetVertexBuffers(2, 3, MeshViews);
        if(JointBuffer && WeightBuffer)
        {
            // Vertex layout 4: bone indices and weights in slots 6 and 7.
            D3D12_VERTEX_BUFFER_VIEW SkinViews[2]{};
            for(int Stream = 0; Stream < 2; ++Stream)
            {
                SkinViews[Stream].BufferLocation = (Stream == 0 ? JointBuffer : WeightBuffer)->GetGPUVirtualAddress();
                SkinViews[Stream].SizeInBytes    = (PositionBytes / 3) * 4;
                SkinViews[Stream].StrideInBytes  = sizeof(float) * 4;
            }
            List->IASetVertexBuffers(6, 2, SkinViews);
        }

        D3D12_INDEX_BUFFER_VIEW IndexView{};
        IndexView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexView.SizeInBytes    = IndexBytes;
        IndexView.Format         = DXGI_FORMAT_R32_UINT;
        List->IASetIndexBuffer(&IndexView);

        // Instanced (GDISetInstanceBuffer): vertex layout 3 reads slot 5 per instance.
        UINT Instances = 1;
        if(auto* Instance = static_cast<D3D12Buffer*>(GDI->GetInstanceBuffer()))
        {
            if(!Instance->IsValid()) return;
            Instance->Transition(List, GDI->GetListSerial(), Instance->GetDrawReadState());

            D3D12_VERTEX_BUFFER_VIEW InstanceView{};
            InstanceView.BufferLocation = Instance->GetResource()->GetGPUVirtualAddress();
            InstanceView.SizeInBytes    = Instance->GetSize();
            InstanceView.StrideInBytes  = GDI_INSTANCE_STRIDE;
            List->IASetVertexBuffers(5, 1, &InstanceView);
            Instances = GDI->GetInstanceCount();

            // Indirect (GDISetIndirectArgs): the counts and range from a buffer the GPU wrote.
            if(auto* Args = static_cast<D3D12Buffer*>(GDI->GetIndirectArgs()))
            {
                if(!Args->IsValid() || !GDI->GetPipelineCache()->GetDrawIndexedSignature()) return;
                Args->Transition(List, GDI->GetListSerial(), Args->GetDrawReadState());
                List->ExecuteIndirect(GDI->GetPipelineCache()->GetDrawIndexedSignature(), 1, Args->GetResource(),
                                      GDI->GetIndirectArgsOffset(), nullptr, 0);
                return;
            }
        }
        // A dynamic mesh's last update sits at its base vertex and index; 0 otherwise.
        List->DrawIndexedInstanced(Count, Instances, GetDynamicBaseIndex() + First, INT(GetDynamicBaseVertex()), 0);
    }
    else if(bSupportsInstanced)
    {
        // Zero instances draws nothing, as glDrawArraysInstanced(..., 0) would.
        if(NumInstanced <= 0 || !TransformRing.Buffer) return;

        BindRing(List, 2, TransformRing);
        BindRing(List, 3, OpacityRing);
        BindRing(List, 4, ColorRing);
        List->DrawInstanced(VertexCount, UINT(NumInstanced), 0, UINT(BatchStart));
    }
    else
    {
        List->DrawInstanced(VertexCount, 1, 0, 0);
    }
}

void D3D12Mesh::AddInstancedTransformInner()
{
    if(!TransformRing.Mapped || InstancedTransforms.empty()) return;

    const int Local = NumInstanced - 1;
    if(Local < 0 || Local >= MaxInstanced) return;

    if(Local == 0)
    {
        if(RingCursor + MaxInstanced > MaxInstanced * InstanceRingBatches)
        {
            if(auto* GDI = dynamic_cast<D3D12DynamicGDI*>(GDynamicGDI))
                GDI->FlushForReadback(true);
            RingCursor = 0;
        }
        BatchStart = RingCursor;
    }

    const int Slot = BatchStart + Local;

    // Column-major, matching the column-per-semantic layout of the cooked HLSL.
    const Matrix4x4& Transform = InstancedTransforms.back();
    float MatData[16];
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            MatData[Col * 4 + Row] = static_cast<float>(Transform.Data[Row][Col]);
    std::memcpy(static_cast<uint8_t*>(TransformRing.Mapped) + Slot * TransformRing.Stride, MatData, sizeof(MatData));

    if(OpacityRing.Mapped && !InstancedOpacities.empty())
    {
        const float Opacity = InstancedOpacities.back();
        std::memcpy(static_cast<uint8_t*>(OpacityRing.Mapped) + Slot * OpacityRing.Stride, &Opacity, sizeof(float));
    }

    if(ColorRing.Mapped && !InstancedColors.empty())
    {
        const Vector3& C = InstancedColors.back();
        const float ColorData[3] = { float(C.GetX()), float(C.GetY()), float(C.GetZ()) };
        std::memcpy(static_cast<uint8_t*>(ColorRing.Mapped) + Slot * ColorRing.Stride, ColorData, sizeof(ColorData));
    }

    RingCursor = Slot + 1;
}

void D3D12Mesh::ResetInstancedTransformInner()
{
    // The cursor persists across batches; see AddInstancedTransformInner.
}

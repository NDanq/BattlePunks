#include "Public/GDI/D3D12/D3D12Mesh.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <cstdlib>
#include <cstring>

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
}

void D3D12Mesh::Release()
{
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

        D3D12_INDEX_BUFFER_VIEW IndexView{};
        IndexView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexView.SizeInBytes    = IndexBytes;
        IndexView.Format         = DXGI_FORMAT_R32_UINT;
        List->IASetIndexBuffer(&IndexView);
        List->DrawIndexedInstanced(Count, 1, First, 0, 0);
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

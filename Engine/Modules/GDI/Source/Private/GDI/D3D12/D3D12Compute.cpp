/*
 * D3D12DynamicGDI's compute and GPU buffers (GDIDispatchCompute,
 * GDICreateBuffer, GDIReadBuffer): dispatches record into the frame's one
 * command list, in order with the draws, on the compute root signature
 * (D3D12PipelineCache, ED3D12ComputeRootParam). Graphics and compute root
 * arguments are separate, and D3D12 has no render pass to close, so the
 * draws after a dispatch keep their targets.
 */
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12Buffer.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12Shader.h"
#include "Public/GDI/D3D12/D3D12PipelineCache.h"
#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <algorithm>
#include <cstring>

#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDILog.h"

using namespace D3D12Ops;

namespace
{
    /* Every UAV write before against every access after: a dispatch sees what earlier
     * dispatches wrote to its buffers, and the dispatches after it see what it wrote. */
    void RecordUAVBarrier(ID3D12GraphicsCommandList* InList)
    {
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Barrier.UAV.pResource = nullptr;
        InList->ResourceBarrier(1, &Barrier);
    }
}

GDIBuffer* D3D12DynamicGDI::CreateBuffer(const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
{
    if(!SupportsCompute()) return nullptr;
    auto* Buffer = new D3D12Buffer(DeviceObj, InBytes, InUsage, InInitialData);
    if(!Buffer->IsValid())
    {
        delete Buffer;
        return nullptr;
    }
    return Buffer;
}

bool D3D12DynamicGDI::DispatchCompute(GDIShader* InShader, const void* InParams, const uint32_t InParamBytes,
                                      GDIBuffer* const* InBuffers, const uint32_t InBufferCount,
                                      const uint32_t InX, const uint32_t InY, const uint32_t InZ)
{
    if(!CurrentStateData || !PipelineCache || !DescAlloc || !DeviceObj) return false;
    const auto* Resource = static_cast<D3D12ShaderResource*>(InShader->Resource);
    if(!Resource || !Resource->IsValid()) return false;

    D3D12Buffer* Buffers[GDI_COMPUTE_MAX_STORAGE_BUFFERS] = {};
    for(uint32_t Each = 0; Each < InBufferCount; ++Each)
    {
        Buffers[Each] = static_cast<D3D12Buffer*>(InBuffers[Each]);
        if(!Buffers[Each]->IsValid()) return false;
    }

    ID3D12PipelineState* Pipeline = PipelineCache->GetOrCreateCompute(Resource);
    if(!Pipeline) return false;

    const auto Record = [&](ID3D12GraphicsCommandList* InList, const uint64_t InSerial, const D3D12_GPU_VIRTUAL_ADDRESS InParamsAddress)
    {
        for(uint32_t Each = 0; Each < InBufferCount; ++Each)
        {
            Buffers[Each]->Transition(InList, InSerial, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        RecordUAVBarrier(InList);
        InList->SetComputeRootSignature(PipelineCache->GetComputeRootSignature());
        InList->SetPipelineState(Pipeline);
        InList->SetComputeRootConstantBufferView(CRP_Params, InParamsAddress);
        // Slots past the last buffer repeat it: the shader declares none there, but no root argument is left unset.
        for(uint32_t Slot = 0; InBufferCount > 0 && Slot < GDI_COMPUTE_MAX_STORAGE_BUFFERS; ++Slot)
        {
            InList->SetComputeRootUnorderedAccessView(CRP_Storage0 + Slot,
                                                      Buffers[(std::min)(Slot, InBufferCount - 1)]->GetResource()->GetGPUVirtualAddress());
        }
        InList->Dispatch(InX, InY, InZ);
        RecordUAVBarrier(InList);
    };

    /* The parameters (a cbuffer is read in 256-byte units, and is never empty). */
    const uint32_t ParamBytes = (std::max<uint32_t>)(256, (InParamBytes + 255) & ~255u);

    /* Outside a frame (a render command): on its own list, at once, waited for. Its
     * parameters get their own upload buffer, since the ring only rewinds per frame. */
    if(!CurrentStateData->CurrentCmd)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Params = D3D12Ops::CreateBuffer(DeviceObj, ParamBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                                     D3D12_RESOURCE_STATE_GENERIC_READ);
        void* Mapped = nullptr;
        const D3D12_RANGE NoRead{ 0, 0 };
        if(!Params || FAILED(Params->Map(0, &NoRead, &Mapped))) return false;
        std::memset(Mapped, 0, ParamBytes);
        if(InParamBytes > 0) { std::memcpy(Mapped, InParams, InParamBytes); }
        Params->Unmap(0, nullptr);

        TransientCmd Cmd;
        if(!BeginTransientCmd(DeviceObj, Cmd)) return false;
        Record(Cmd.List.Get(), AllocateListSerial(), Params->GetGPUVirtualAddress());
        EndAndSubmitTransientCmd(DeviceObj, Cmd);
        return true;
    }

    const D3D12CBAllocation Params = DescAlloc->AllocateCB(ParamBytes);
    if(!Params.MappedPtr) return false;
    std::memset(Params.MappedPtr, 0, ParamBytes);
    if(InParamBytes > 0) { std::memcpy(Params.MappedPtr, InParams, InParamBytes); }
    Record(CurrentStateData->CurrentCmd, GetListSerial(), Params.GPUAddress);
    DrawCaches.BoundPipeline = nullptr;   // the compute pipeline replaced the graphics one on the list
    return true;
}

bool D3D12DynamicGDI::UpdateBuffer(GDIBuffer* InBuffer, const uint32_t InOffset, const void* InData, const uint32_t InBytes)
{
    auto* Buffer = static_cast<D3D12Buffer*>(InBuffer);
    if(!CurrentStateData || !DeviceObj || !Buffer->IsValid()) return false;

    /* The bytes in an upload buffer of their own: the frame's ring rewinds
     * per frame and has no resource to copy from. */
    Microsoft::WRL::ComPtr<ID3D12Resource> Upload = D3D12Ops::CreateBuffer(DeviceObj, InBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                                          D3D12_RESOURCE_STATE_GENERIC_READ);
    void* Mapped = nullptr;
    const D3D12_RANGE NoRead{ 0, 0 };
    if(!Upload || FAILED(Upload->Map(0, &NoRead, &Mapped))) return false;
    std::memcpy(Mapped, InData, InBytes);
    Upload->Unmap(0, nullptr);

    const auto Record = [&](ID3D12GraphicsCommandList* InList, const uint64_t InSerial)
    {
        Buffer->Transition(InList, InSerial, D3D12_RESOURCE_STATE_COPY_DEST);
        InList->CopyBufferRegion(Buffer->GetResource(), InOffset, Upload.Get(), 0, InBytes);
    };

    /* Outside a frame: on its own list, at once, waited for. */
    if(!CurrentStateData->CurrentCmd)
    {
        TransientCmd Cmd;
        if(!BeginTransientCmd(DeviceObj, Cmd)) return false;
        Record(Cmd.List.Get(), AllocateListSerial());
        EndAndSubmitTransientCmd(DeviceObj, Cmd);
        return true;
    }

    /* The draws and dispatches after it move the buffer to their own states (Transition). */
    Record(CurrentStateData->CurrentCmd, GetListSerial());
    DeferRelease(Upload);
    return true;
}

bool D3D12DynamicGDI::ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes)
{
    auto* Buffer = static_cast<D3D12Buffer*>(InBuffer);
    if(!Buffer->IsValid() || !DeviceObj) return false;

    /* What the open frame has recorded runs first. */
    FlushForReadback();

    Microsoft::WRL::ComPtr<ID3D12Resource> Readback = D3D12Ops::CreateBuffer(DeviceObj, Buffer->GetSize(), D3D12_HEAP_TYPE_READBACK,
                                                                   D3D12_RESOURCE_STATE_COPY_DEST);
    TransientCmd Cmd;
    if(!Readback || !BeginTransientCmd(DeviceObj, Cmd)) return false;
    Buffer->Transition(Cmd.List.Get(), AllocateListSerial(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    Cmd.List->CopyBufferRegion(Readback.Get(), 0, Buffer->GetResource(), 0, Buffer->GetSize());
    EndAndSubmitTransientCmd(DeviceObj, Cmd);

    void* Mapped = nullptr;
    const D3D12_RANGE All{ 0, Buffer->GetSize() };
    if(FAILED(Readback->Map(0, &All, &Mapped))) return false;
    OutBytes.assign(static_cast<const uint8_t*>(Mapped), static_cast<const uint8_t*>(Mapped) + Buffer->GetSize());
    const D3D12_RANGE NoWrite{ 0, 0 };
    Readback->Unmap(0, &NoWrite);
    return true;
}

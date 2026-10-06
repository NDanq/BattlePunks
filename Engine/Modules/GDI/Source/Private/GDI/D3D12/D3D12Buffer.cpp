#include "Public/GDI/D3D12/D3D12Buffer.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <string>
#include <vector>

#include "Public/GDI/GDILog.h"

extern GDIDynamicDriver* GDynamicGDI;

D3D12Buffer::D3D12Buffer(D3D12Device* InDevice, const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
    : GDIBuffer(InBytes, InUsage)
    , Device(InDevice)
{
    if(!Device || !Device->IsValid() || InBytes == 0) return;

    D3D12_HEAP_PROPERTIES Heap{};
    Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = InBytes;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_UNKNOWN;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(FAILED(Device->GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON,
                                                           nullptr, IID_PPV_ARGS(&Resource))))
    {
        GDI_LOG_ERROR("[D3D12Buffer] CreateCommittedResource failed.")
        Resource.Reset();
        return;
    }

    /* Its first contents, synchronously: the data, or zeros, through an upload copy
     * (the buffer promoted from COMMON to COPY_DEST, and decayed back after). */
    const std::vector<uint8_t> Zeros(InInitialData ? 0 : InBytes, uint8_t(0));
    if(!D3D12Ops::UploadBuffer(Device, Resource.Get(), InInitialData ? InInitialData : Zeros.data(), InBytes))
    {
        GDI_LOG_ERROR("[D3D12Buffer] the initial upload failed.")
        Resource.Reset();
    }
}

void D3D12Buffer::Release()
{
    /* The open frame, or one in flight, may still read it: freed after the
     * next frame-fence wait, as a released texture is. */
    if(auto* GDI = dynamic_cast<D3D12DynamicGDI*>(GDynamicGDI))
    {
        GDI->DeferRelease(Resource);
    }
    Resource.Reset();
    delete this;
}

void D3D12Buffer::SetDebugName(const std::string& InName)
{
    GDIBuffer::SetDebugName(InName);
    D3D12Util::SetObjectName(Resource.Get(), InName);
}

D3D12_RESOURCE_STATES D3D12Buffer::GetDrawReadState() const
{
    D3D12_RESOURCE_STATES Read = D3D12_RESOURCE_STATE_COMMON;
    if(Usage & GDI_BU_Instance) { Read |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER; }
    if(Usage & GDI_BU_Indirect) { Read |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT; }
    return Read;
}

void D3D12Buffer::Transition(ID3D12GraphicsCommandList* InList, const uint64_t InListSerial, const D3D12_RESOURCE_STATES InTo)
{
    if(!InList || !Resource) return;
    D3D12Ops::RecordTransition(InList, Resource.Get(), GetState(InListSerial), InTo);
    State           = InTo;
    StateListSerial = InListSerial;
}

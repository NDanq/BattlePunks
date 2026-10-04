#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include "Public/GDI/GDILog.h"

bool D3D12DescriptorAllocator::Initialize(D3D12Device* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid()) return false;

    ID3D12Device* Dev = Device->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC SRVDesc{};
    SRVDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    SRVDesc.NumDescriptors = kSRVCapacity;
    SRVDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(Dev->CreateDescriptorHeap(&SRVDesc, IID_PPV_ARGS(&SRVHeap))))
    {
        GDI_LOG_ERROR("[D3D12DescriptorAllocator] Failed to create the SRV heap.")
        return false;
    }
    SRVIncrement = Dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Handed out from the back so low slots go first.
    FreeSRVSlots.reserve(kSRVCapacity);
    for(uint32_t Slot = kSRVCapacity; Slot > 0; --Slot)
    {
        FreeSRVSlots.push_back(Slot - 1);
    }

    if(!CreateSamplers()) return false;

    RingBuffer = D3D12Ops::CreateBuffer(Device, kRingCapacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!RingBuffer) return false;

    const D3D12_RANGE NoRead{ 0, 0 };
    void* Mapped = nullptr;
    if(FAILED(RingBuffer->Map(0, &NoRead, &Mapped)))
    {
        GDI_LOG_ERROR("[D3D12DescriptorAllocator] Failed to map the constant-buffer ring.")
        return false;
    }
    RingMapped = static_cast<uint8_t*>(Mapped);
    RingOffset = 0;
    return true;
}

bool D3D12DescriptorAllocator::CreateSamplers()
{
    ID3D12Device* Dev = Device->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC Desc{};
    Desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    Desc.NumDescriptors = UINT(GDI_TB_MAX) * UINT(GDI_TF_MAX);
    Desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(Dev->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&SamplerHeap))))
    {
        GDI_LOG_ERROR("[D3D12DescriptorAllocator] Failed to create the sampler heap.")
        return false;
    }
    SamplerIncrement = Dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    const D3D12_TEXTURE_ADDRESS_MODE Modes[GDI_TB_MAX] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    };
    const D3D12_FILTER Filters[GDI_TF_MAX] = {
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
    };

    D3D12_CPU_DESCRIPTOR_HANDLE Handle = SamplerHeap->GetCPUDescriptorHandleForHeapStart();
    for(int Border = 0; Border < GDI_TB_MAX; ++Border)
    {
        for(int Filter = 0; Filter < GDI_TF_MAX; ++Filter)
        {
            D3D12_SAMPLER_DESC Sampler{};
            Sampler.Filter         = Filters[Filter];
            Sampler.AddressU       = Modes[Border];
            Sampler.AddressV       = Modes[Border];
            Sampler.AddressW       = Modes[Border];
            Sampler.MaxAnisotropy  = 1;
            Sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            Sampler.MinLOD         = 0.0f;
            // Unclamped: a texture with a mip chain samples it, and LOD clamps to
            // the levels a resource has, so single-level textures and RTs are unchanged.
            Sampler.MaxLOD         = D3D12_FLOAT32_MAX;
            Dev->CreateSampler(&Sampler, Handle);
            Handle.ptr += SamplerIncrement;
        }
    }
    return true;
}

void D3D12DescriptorAllocator::Shutdown()
{
    if(RingBuffer && RingMapped)
    {
        RingBuffer->Unmap(0, nullptr);
    }
    RingMapped = nullptr;
    RingBuffer.Reset();
    SamplerHeap.Reset();
    SRVHeap.Reset();
    FreeSRVSlots.clear();
    RingOffset = 0;
    Device     = nullptr;
}

void D3D12DescriptorAllocator::Reset()
{
    RingOffset = 0;
    ++Epoch;
}

D3D12CBAllocation D3D12DescriptorAllocator::AllocateCB(uint32_t Size)
{
    D3D12CBAllocation Out;
    const uint32_t Aligned = (Size + kCBAlignment - 1) & ~(kCBAlignment - 1);
    if(!RingMapped || RingOffset + Aligned > kRingCapacity)
    {
        return Out;
    }

    Out.GPUAddress = RingBuffer->GetGPUVirtualAddress() + RingOffset;
    Out.MappedPtr  = RingMapped + RingOffset;
    RingOffset += Aligned;
    return Out;
}

uint32_t D3D12DescriptorAllocator::AllocateSRV()
{
    if(FreeSRVSlots.empty())
    {
        GDI_LOG_ERROR("[D3D12DescriptorAllocator] SRV heap exhausted.")
        return InvalidSlot;
    }
    const uint32_t Slot = FreeSRVSlots.back();
    FreeSRVSlots.pop_back();
    return Slot;
}

void D3D12DescriptorAllocator::FreeSRV(uint32_t InSlot)
{
    if(InSlot < kSRVCapacity)
    {
        FreeSRVSlots.push_back(InSlot);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::GetSRVCPUHandle(uint32_t InSlot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE Handle = SRVHeap->GetCPUDescriptorHandleForHeapStart();
    Handle.ptr += SIZE_T(InSlot) * SRVIncrement;
    return Handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::GetSRVGPUHandle(uint32_t InSlot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE Handle = SRVHeap->GetGPUDescriptorHandleForHeapStart();
    Handle.ptr += UINT64(InSlot) * SRVIncrement;
    return Handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::GetSamplerGPUHandle(EGDITextureBorder InBorder, EGDITextureFilter InFilter) const
{
    const UINT Border = UINT(InBorder) < UINT(GDI_TB_MAX) ? UINT(InBorder) : 0u;
    const UINT Filter = UINT(InFilter) < UINT(GDI_TF_MAX) ? UINT(InFilter) : 0u;

    D3D12_GPU_DESCRIPTOR_HANDLE Handle = SamplerHeap->GetGPUDescriptorHandleForHeapStart();
    Handle.ptr += UINT64(Border * UINT(GDI_TF_MAX) + Filter) * SamplerIncrement;
    return Handle;
}

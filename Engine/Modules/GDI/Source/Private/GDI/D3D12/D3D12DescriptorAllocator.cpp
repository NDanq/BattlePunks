#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDILog.h"

bool D3D12DescriptorAllocator::Initialize(D3D12Device* InDevice, const uint32_t InSlots)
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

    RingSlots = InSlots < 1 ? 1u : InSlots;
    RingChunks.resize(RingSlots);
    for(std::array<RingChunk, kRingChunkMax>& Chunks : RingChunks)
    {
        if(!CreateRingChunk(Chunks[0])) return false;
    }
    Reset(0);
    return true;
}

bool D3D12DescriptorAllocator::CreateRingChunk(RingChunk& OutChunk)
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Buffer =
        D3D12Ops::CreateBuffer(Device, kRingCapacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!Buffer) return false;

    const D3D12_RANGE NoRead{ 0, 0 };
    void* Mapped = nullptr;
    if(FAILED(Buffer->Map(0, &NoRead, &Mapped)))
    {
        GDI_LOG_ERROR("[D3D12DescriptorAllocator] Failed to map a constant-buffer ring chunk.")
        return false;
    }
    OutChunk.Mapped     = static_cast<uint8_t*>(Mapped);
    OutChunk.GPUAddress = Buffer->GetGPUVirtualAddress();
    OutChunk.Buffer     = std::move(Buffer);
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
            // Linear samplers are anisotropic when GDISetMaxAnisotropy asks (set before the
            // device is made); a single-level resource samples the same either way.
            const UINT Anisotropy  = UINT(GDIGetMaxAnisotropy());
            const bool bAnisotropic = Filter == GDI_TF_Linear && Anisotropy > 1;
            D3D12_SAMPLER_DESC Sampler{};
            Sampler.Filter         = bAnisotropic ? D3D12_FILTER_ANISOTROPIC : Filters[Filter];
            Sampler.AddressU       = Modes[Border];
            Sampler.AddressV       = Modes[Border];
            Sampler.AddressW       = Modes[Border];
            Sampler.MaxAnisotropy  = bAnisotropic ? (Anisotropy > 16 ? 16 : Anisotropy) : 1;
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
    for(std::array<RingChunk, kRingChunkMax>& Chunks : RingChunks)
    {
        for(RingChunk& Chunk : Chunks)
        {
            if(Chunk.Buffer && Chunk.Mapped) { Chunk.Buffer->Unmap(0, nullptr); }
        }
    }
    RingChunks.clear();
    SamplerHeap.Reset();
    SRVHeap.Reset();
    FreeSRVSlots.clear();
    RingCursor  = 0;
    bWarnedFull = false;
    Device      = nullptr;
}

void D3D12DescriptorAllocator::Reset(const uint32_t InSlot)
{
    RingSlot   = InSlot < RingSlots ? InSlot : 0u;
    RingCursor = 0;
    ++Epoch;   // an upload from another slot's chunks, or this one's last frame, is stale
}

D3D12CBAllocation D3D12DescriptorAllocator::AllocateCB(uint32_t Size)
{
    D3D12CBAllocation Out;
    const uint32_t Aligned = (Size + kCBAlignment - 1) & ~(kCBAlignment - 1);
    if(RingChunks.empty() || Aligned > kRingCapacity)
    {
        return Out;
    }

    std::array<RingChunk, kRingChunkMax>& Chunks = RingChunks[RingSlot];
    for(;;)
    {
        /* Claimed whole, by whichever thread gets there. */
        const uint64_t Cursor = RingCursor.fetch_add(Aligned);
        const uint32_t Chunk  = uint32_t(Cursor >> 32);
        const uint32_t Offset = uint32_t(Cursor);
        if(Offset <= kRingCapacity - Aligned)
        {
            Out.GPUAddress = Chunks[Chunk].GPUAddress + Offset;
            Out.MappedPtr  = Chunks[Chunk].Mapped + Offset;
            return Out;
        }

        /* Past the chunk's end: the first thread here moves every thread on
         * to the next chunk, made if this frame is the first to need it. */
        std::lock_guard<std::mutex> Lock(RingGrowLock);
        if(uint32_t(RingCursor.load() >> 32) != Chunk) continue;   // moved on already
        if(Chunk + 1 >= kRingChunkMax || (!Chunks[Chunk + 1].Buffer && !CreateRingChunk(Chunks[Chunk + 1])))
        {
            if(!bWarnedFull)
            {
                bWarnedFull = true;
                GDI_LOG_ERROR("[D3D12DescriptorAllocator] Constant-buffer ring full at "
                              + std::to_string(uint64_t(kRingChunkMax) * (kRingCapacity / 1024))
                              + " KB this frame; draws past it are dropped.")
            }
            return Out;
        }
        RingCursor = uint64_t(Chunk + 1) << 32;
    }
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

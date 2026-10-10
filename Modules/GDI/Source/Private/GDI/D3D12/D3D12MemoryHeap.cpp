#include "Public/GDI/D3D12/D3D12MemoryHeap.h"

#include <algorithm>
#include <string>

#include "Public/GDI/GDILog.h"
#include "Public/GDI/D3D12/D3D12Device.h"

namespace
{
    UINT64 AlignUp(const UINT64 InValue, const UINT64 InAlign)
    {
        return InAlign <= 1 ? InValue : (InValue + InAlign - 1) / InAlign * InAlign;
    }
}

// === D3D12TextureHeap =======================================================

void D3D12TextureHeap::Shutdown()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    Blocks.clear();
}

bool D3D12TextureHeap::Carve(const uint32_t InBlock, const UINT64 InSize, const UINT64 InAlign, D3D12Allocation& OutAllocation)
{
    Block& Owner = Blocks[InBlock];
    for(size_t Index = 0; Index < Owner.Free.size(); ++Index)
    {
        const Range  Each  = Owner.Free[Index];
        const UINT64 Start = AlignUp(Each.Offset, InAlign);
        if(Start + InSize > Each.Offset + Each.Size) { continue; }

        /* What is left before and after it stays free. */
        std::vector<Range> Left;
        if(Start > Each.Offset)                      { Left.push_back({ Each.Offset, Start - Each.Offset }); }
        if(Start + InSize < Each.Offset + Each.Size) { Left.push_back({ Start + InSize, Each.Offset + Each.Size - Start - InSize }); }
        Owner.Free.erase(Owner.Free.begin() + std::ptrdiff_t(Index));
        Owner.Free.insert(Owner.Free.begin() + std::ptrdiff_t(Index), Left.begin(), Left.end());

        Owner.Used += InSize;
        ++Owner.Live;
        OutAllocation.Heap   = Owner.Heap.Get();
        OutAllocation.Offset = Start;
        OutAllocation.Size   = InSize;
        OutAllocation.Block  = InBlock;
        return true;
    }
    return false;
}

bool D3D12TextureHeap::AllocateRange(const UINT64 InSize, const UINT64 InAlign, D3D12Allocation& OutAllocation)
{
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        for(uint32_t Index = 0; Index < uint32_t(Blocks.size()); ++Index)
        {
            if(Blocks[Index].Heap && Carve(Index, InSize, InAlign, OutAllocation)) { return true; }
        }
    }

    /* A new block, outside the lock. Only non-RT/DS textures go in, which
     * resource heap tier 1 requires and tier 2 allows. */
    D3D12_HEAP_DESC Desc{};
    Desc.SizeInBytes     = BLOCK_BYTES;
    Desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    Desc.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    Desc.Flags           = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    Microsoft::WRL::ComPtr<ID3D12Heap> Heap;
    if(FAILED(Device->GetDevice()->CreateHeap(&Desc, IID_PPV_ARGS(&Heap))))
    {
        GDI_LOG("[D3D12TextureHeap] a " + std::to_string(BLOCK_BYTES >> 20) + " MB heap was refused; committing the texture on its own.")
        return false;
    }

    std::lock_guard<std::mutex> Lock(Mutex);
    uint32_t Slot = 0;
    while(Slot < Blocks.size() && Blocks[Slot].Heap) { ++Slot; }
    if(Slot == Blocks.size()) { Blocks.emplace_back(); }
    Block& Made = Blocks[Slot];
    Made = Block();
    Made.Heap  = std::move(Heap);
    Made.Bytes = BLOCK_BYTES;
    Made.Free.push_back({ 0, BLOCK_BYTES });
    GDI_LOG("[D3D12TextureHeap] block " + std::to_string(Slot) + " made: " + std::to_string(BLOCK_BYTES >> 20) + " MB")
    return Carve(Slot, InSize, InAlign, OutAllocation);
}

bool D3D12TextureHeap::CreateTexture(const D3D12_RESOURCE_DESC& InDesc, const D3D12_RESOURCE_STATES InState,
                                     Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource, D3D12Allocation& OutAllocation)
{
    OutResource.Reset();
    OutAllocation = D3D12Allocation();
    if(!Device || !Device->IsValid()) { return false; }
    ID3D12Device* Dev = Device->GetDevice();

    /* 4 KB placement where the runtime grants it (small textures), else the default 64 KB. */
    D3D12_RESOURCE_DESC Placed = InDesc;
    Placed.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12_RESOURCE_ALLOCATION_INFO Info = Dev->GetResourceAllocationInfo(0, 1, &Placed);
    if(Info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
    {
        Placed.Alignment = 0;
        Info = Dev->GetResourceAllocationInfo(0, 1, &Placed);
    }

    if(Info.SizeInBytes != UINT64_MAX && Info.SizeInBytes < SMALL_LIMIT
       && AllocateRange(Info.SizeInBytes, Info.Alignment, OutAllocation))
    {
        if(SUCCEEDED(Dev->CreatePlacedResource(OutAllocation.Heap, OutAllocation.Offset, &Placed, InState,
                                               nullptr, IID_PPV_ARGS(&OutResource))))
        {
            return true;
        }
        GDI_LOG("[D3D12TextureHeap] CreatePlacedResource failed; committing the texture on its own.")
        Free(OutAllocation);
        OutAllocation = D3D12Allocation();
    }

    D3D12_HEAP_PROPERTIES HeapProps{};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC Committed = InDesc;
    Committed.Alignment = 0;
    if(FAILED(Dev->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Committed, InState,
                                           nullptr, IID_PPV_ARGS(&OutResource))))
    {
        OutResource.Reset();
        return false;
    }
    OutAllocation.Size = Dev->GetResourceAllocationInfo(0, 1, &Committed).SizeInBytes;
    std::lock_guard<std::mutex> Lock(Mutex);
    DedicatedBytes += OutAllocation.Size;
    ++DedicatedCount;
    return true;
}

void D3D12TextureHeap::Free(const D3D12Allocation& InAllocation)
{
    if(!InAllocation.IsValid()) { return; }
    std::lock_guard<std::mutex> Lock(Mutex);
    if(InAllocation.Block == UINT32_MAX)
    {
        DedicatedBytes -= (std::min<uint64_t>)(DedicatedBytes, InAllocation.Size);
        if(DedicatedCount > 0) { --DedicatedCount; }
        return;
    }

    if(InAllocation.Block >= Blocks.size() || Blocks[InAllocation.Block].Heap.Get() != InAllocation.Heap)
    {
        return;   // its block was released by Shutdown: nothing to give back
    }
    Block& Owner = Blocks[InAllocation.Block];

    /* Back in offset order, merged with whatever free range it touches. */
    const auto At = std::lower_bound(Owner.Free.begin(), Owner.Free.end(), InAllocation.Offset,
                                     [](const Range& InRange, const UINT64 InOffset) { return InRange.Offset < InOffset; });
    auto Inserted = Owner.Free.insert(At, Range{ InAllocation.Offset, InAllocation.Size });
    if(Inserted + 1 != Owner.Free.end() && Inserted->Offset + Inserted->Size == (Inserted + 1)->Offset)
    {
        Inserted->Size += (Inserted + 1)->Size;
        Owner.Free.erase(Inserted + 1);
    }
    if(Inserted != Owner.Free.begin() && (Inserted - 1)->Offset + (Inserted - 1)->Size == Inserted->Offset)
    {
        (Inserted - 1)->Size += Inserted->Size;
        Owner.Free.erase(Inserted);
    }
    Owner.Used -= (std::min)(Owner.Used, InAllocation.Size);
    if(Owner.Live > 0) { --Owner.Live; }

    /* Empty: released, unless it is the one empty block kept for the next texture. */
    if(Owner.Live == 0)
    {
        const bool bOtherEmpty = std::any_of(Blocks.begin(), Blocks.end(), [&](const Block& InOther)
        {
            return &InOther != &Owner && InOther.Heap && InOther.Live == 0;
        });
        if(bOtherEmpty) { Owner = Block(); }
    }
}

D3D12TextureHeap::Stats D3D12TextureHeap::GetStats() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    Stats Out;
    for(const Block& Each : Blocks)
    {
        if(!Each.Heap) { continue; }
        ++Out.Blocks;
        Out.BlockBytes  += Each.Bytes;
        Out.UsedBytes   += Each.Used;
        Out.Allocations += Each.Live;
    }
    Out.DedicatedBytes = DedicatedBytes;
    Out.Dedicated      = DedicatedCount;
    return Out;
}

// === D3D12StagingRing =======================================================

bool D3D12StagingRing::Create()
{
    bTried = true;
    if(!Device || !Device->IsValid()) { return false; }

    D3D12_HEAP_PROPERTIES Heap{};
    Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = RING_BYTES;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_UNKNOWN;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    void* Address = nullptr;
    const D3D12_RANGE NoRead{ 0, 0 };
    if(FAILED(Device->GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffer)))
       || FAILED(Buffer->Map(0, &NoRead, &Address)))
    {
        GDI_LOG("[D3D12StagingRing] could not make the 16 MB upload ring; uploads stage on their own.")
        Buffer.Reset();
        return false;
    }
    Buffer->SetName(L"D3D12StagingRing");
    Mapped = static_cast<uint8_t*>(Address);   // kept mapped: upload heaps allow it
    return true;
}

void D3D12StagingRing::Shutdown()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if(Buffer && Mapped) { Buffer->Unmap(0, nullptr); }
    Buffer.Reset();
    Mapped = nullptr;
    Regions.clear();
    Head   = 0;
    bTried = false;
}

bool D3D12StagingRing::Allocate(const UINT64 InBytes, const UINT64 InAlign, const uint64_t InDoneAt,
                                ID3D12Resource*& OutBuffer, UINT64& OutOffset, uint8_t*& OutMapped)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if(InBytes == 0 || InBytes > RING_BYTES) { return false; }
    if(!Mapped && (bTried || !Create())) { return false; }

    /* Live bytes run from the oldest region's start to Head, wrapping. */
    UINT64 Start = 0;
    if(Regions.empty())
    {
        Head  = 0;
        Start = 0;
    }
    else
    {
        const UINT64 Tail = Regions.front().Start;
        const UINT64 At   = AlignUp(Head, InAlign);
        if(Head > Tail)
        {
            if(At + InBytes <= RING_BYTES) { Start = At; }
            else if(InBytes < Tail)        { Start = 0; }   // wraps to the front
            else                           { return false; }
        }
        else
        {
            if(At + InBytes < Tail) { Start = At; }
            else                    { return false; }
        }
    }

    Head = Start + InBytes;
    Regions.push_back({ Start, Head, InDoneAt });
    OutBuffer = Buffer.Get();
    OutOffset = Start;
    OutMapped = Mapped + Start;
    return true;
}

void D3D12StagingRing::Reclaim(const uint64_t InCompleted)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    while(!Regions.empty() && Regions.front().DoneAt <= InCompleted) { Regions.pop_front(); }
}

UINT64 D3D12StagingRing::GetCapacity() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    return Mapped ? RING_BYTES : 0;
}

UINT64 D3D12StagingRing::GetBytesInUse() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    UINT64 Bytes = 0;
    for(const Region& Each : Regions) { Bytes += Each.End - Each.Start; }
    return Bytes;
}

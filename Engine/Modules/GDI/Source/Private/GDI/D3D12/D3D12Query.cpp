/*
 * D3D12DynamicGDI's occlusion queries (GDIQuery.h). A set is a binary
 * occlusion query heap and three buffers of a uint64 per query: Answers
 * (default heap), which ResolveQueryData fills and SetPredication reads;
 * Readback, a copy of it the CPU reads once the frame's fence has passed;
 * and Ones, all 1, copied over the answers of queries never ended -- a
 * query never begun has nothing to resolve, and must not hide anything.
 *
 * D3D12 needs no reset between rounds and has no render pass to close, so a
 * round begins and resolves in place, in order with the draws.
 */
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "Public/GDI/GDILog.h"

using namespace D3D12Ops;
using Microsoft::WRL::ComPtr;

extern GDIDynamicDriver* GDynamicGDI;

class D3D12OcclusionQueries final : public GDIOcclusionQueries
{
public:
    D3D12OcclusionQueries(D3D12Device* InDevice, const uint32_t InCapacity)
        : GDIOcclusionQueries(InCapacity)
    {
        D3D12_QUERY_HEAP_DESC HeapDesc{};
        HeapDesc.Type  = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        HeapDesc.Count = InCapacity;
        if(FAILED(InDevice->GetDevice()->CreateQueryHeap(&HeapDesc, IID_PPV_ARGS(&Heap))))
        {
            Heap.Reset();
            return;
        }
        const UINT64 Bytes = UINT64(InCapacity) * sizeof(uint64_t);
        Answers  = CreateBuffer(InDevice, Bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        Readback = CreateBuffer(InDevice, Bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        Ones     = CreateBuffer(InDevice, Bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* Mapped = nullptr;
        const D3D12_RANGE NoRead{ 0, 0 };
        if(!Answers || !Readback || !Ones || FAILED(Ones->Map(0, &NoRead, &Mapped)) || !Mapped)
        {
            Heap.Reset();
            return;
        }
        std::vector<uint64_t> AllOnes(InCapacity, 1ull);
        std::memcpy(Mapped, AllOnes.data(), size_t(Bytes));
        Ones->Unmap(0, nullptr);
        Ended.assign(InCapacity, 0);
    }

    void Release() override
    {
        if(auto* Driver = static_cast<D3D12DynamicGDI*>(GDynamicGDI))
        {
            Driver->ReleaseOcclusionQueries(this);
        }
        delete this;
    }

    bool IsValid() const { return Heap && Answers && Readback && Ones; }

    ComPtr<ID3D12QueryHeap> Heap;
    ComPtr<ID3D12Resource>  Answers;
    ComPtr<ID3D12Resource>  Readback;
    ComPtr<ID3D12Resource>  Ones;
    D3D12_RESOURCE_STATES   AnswersState = D3D12_RESOURCE_STATE_COPY_DEST;

    /* This round: how many queries, which have ended, whether one is open,
     * and the BeginRendering block it is in. Pending: resolved, not yet read. */
    uint32_t             Count      = 0;
    std::vector<uint8_t> Ended;
    int32_t              Open       = -1;
    uint64_t             RoundBlock = 0;
    bool                 bResolved  = false;
    bool                 bPending   = false;
};

GDIOcclusionQueries* D3D12DynamicGDI::CreateOcclusionQueries(const uint32_t InCapacity)
{
    auto* Set = new D3D12OcclusionQueries(DeviceObj, InCapacity);
    if(!Set->IsValid())
    {
        GDI_LOG("[D3D12DynamicGDI] could not create an occlusion query set of " + std::to_string(InCapacity))
        Set->Release();
        return nullptr;
    }
    return Set;
}

void D3D12DynamicGDI::ReleaseOcclusionQueries(D3D12OcclusionQueries* InSet)
{
    if(PredicateSet == InSet)
    {
        PredicateSet   = nullptr;
        PredicateIndex = -1;
    }
    DeferRelease(InSet->Answers);
    DeferRelease(InSet->Readback);
    DeferRelease(InSet->Ones);
    if(InSet->Heap) { DeferredQueryHeaps.push_back(InSet->Heap); }
}

void D3D12DynamicGDI::BeginOcclusionRound(GDIOcclusionQueries* InSet, const uint32_t InCount)
{
    auto* Set = static_cast<D3D12OcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || !CurrentStateData->CurrentCmd || Set->Open >= 0)
    {
        return;
    }
    Set->Count      = InCount;
    std::fill(Set->Ended.begin(), Set->Ended.end(), uint8_t(0));
    Set->RoundBlock = RenderingBlocks;
    Set->bResolved  = false;
    Set->bPending   = false;
}

void D3D12DynamicGDI::BeginOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<D3D12OcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || !CurrentStateData->CurrentCmd
       || InIndex >= Set->Count || Set->bResolved || Set->Open >= 0 || Set->Ended[InIndex])
    {
        return;
    }
    CurrentStateData->CurrentCmd->BeginQuery(Set->Heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, InIndex);
    Set->Open = int32_t(InIndex);
}

void D3D12DynamicGDI::EndOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<D3D12OcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || !CurrentStateData->CurrentCmd || Set->Open != int32_t(InIndex))
    {
        return;
    }
    CurrentStateData->CurrentCmd->EndQuery(Set->Heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, InIndex);
    Set->Ended[InIndex] = 1;
    Set->Open = -1;
}

void D3D12DynamicGDI::ResolveOcclusionQueries(GDIOcclusionQueries* InSet)
{
    auto* Set = static_cast<D3D12OcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || !CurrentStateData->CurrentCmd
       || Set->Count == 0 || Set->bResolved || Set->Open >= 0)
    {
        return;
    }
    ID3D12GraphicsCommandList* List = CurrentStateData->CurrentCmd;

    /* Copies are predicated too: none may be set while these record. */
    List->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

    RecordTransition(List, Set->Answers.Get(), Set->AnswersState, D3D12_RESOURCE_STATE_COPY_DEST);
    for(uint32_t Start = 0; Start < Set->Count;)
    {
        const uint8_t bEnded = Set->Ended[Start];
        uint32_t End = Start + 1;
        while(End < Set->Count && Set->Ended[End] == bEnded) { ++End; }
        const UINT64 Offset = UINT64(Start) * sizeof(uint64_t);
        if(bEnded)
        {
            List->ResolveQueryData(Set->Heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, Start, End - Start, Set->Answers.Get(), Offset);
        }
        else
        {
            List->CopyBufferRegion(Set->Answers.Get(), Offset, Set->Ones.Get(), Offset, UINT64(End - Start) * sizeof(uint64_t));
        }
        Start = End;
    }
    RecordTransition(List, Set->Answers.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    List->CopyBufferRegion(Set->Readback.Get(), 0, Set->Answers.Get(), 0, UINT64(Set->Count) * sizeof(uint64_t));
    RecordTransition(List, Set->Answers.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PREDICATION);
    Set->AnswersState = D3D12_RESOURCE_STATE_PREDICATION;
    Set->bResolved    = true;
    Set->bPending     = true;
}

void D3D12DynamicGDI::SetOcclusionPredicate(GDIOcclusionQueries* InSet, const int32_t InIndex)
{
    auto* Set = static_cast<D3D12OcclusionQueries*>(InSet);
    const bool bUsable = Set && Set->IsValid() && Set->bResolved && InIndex >= 0 && uint32_t(InIndex) < Set->Count;
    PredicateSet   = bUsable ? Set : nullptr;
    PredicateIndex = bUsable ? InIndex : -1;
}

bool D3D12DynamicGDI::ReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible)
{
    auto* Set = static_cast<D3D12OcclusionQueries*>(InSet);
    if(!Set->IsValid() || !Set->bPending || RenderingBlocks <= Set->RoundBlock)
    {
        return false;
    }
    void* Mapped = nullptr;
    const D3D12_RANGE Range{ 0, size_t(Set->Count) * sizeof(uint64_t) };
    if(FAILED(Set->Readback->Map(0, &Range, &Mapped)) || !Mapped)
    {
        return false;
    }
    const auto* Words = static_cast<const uint64_t*>(Mapped);
    OutVisible.resize(Set->Count);
    for(uint32_t Index = 0; Index < Set->Count; ++Index)
    {
        OutVisible[Index] = Words[Index] != 0 ? 1 : 0;
    }
    const D3D12_RANGE Nothing{ 0, 0 };
    Set->Readback->Unmap(0, &Nothing);
    Set->bPending = false;
    return true;
}

bool D3D12DynamicGDI::BeginDrawPredicate(ID3D12GraphicsCommandList* InList)
{
    if(!PredicateSet || PredicateIndex < 0 || !InList)
    {
        return false;
    }
    /* Skipped when its answer is 0: nothing passed. */
    InList->SetPredication(PredicateSet->Answers.Get(), UINT64(PredicateIndex) * sizeof(uint64_t), D3D12_PREDICATION_OP_EQUAL_ZERO);
    return true;
}

void D3D12DynamicGDI::EndDrawPredicate(ID3D12GraphicsCommandList* InList, const bool bInBegun)
{
    if(bInBegun && InList)
    {
        InList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
}

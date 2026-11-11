#include "Private/GDI/Metal/MTLInternal.h"

#include <algorithm>
#include <cstring>

#include "Public/GDI/GDILog.h"

/*
 * Occlusion queries and GPU timestamps on Metal.
 *
 * OCCLUSION. Metal counts visibility per render pass, into the pass's
 * visibilityResultBuffer, at an offset the encoder selects. So each frame slot
 * has one visibility buffer that every pass of its frame carries, and a set's
 * round takes a range of it. A query is answered once its frame completes
 * (ReadOcclusionResults never waits). A query open across two passes would be
 * answered by the second alone -- Boolean mode overwrites per pass -- so it is
 * answered visible, as an unended one is (D3D12's "ones").
 *
 * TIMESTAMPS. Apple GPUs sample counters only at encoder boundaries (a pass's
 * start and end), not between draws. While timing, every encoder in the frame
 * takes a start and an end sample, and WriteGPUTimestamp(i) is the time the GPU
 * reached that point: the end of the last encoder before it -- the open render
 * pass is ended there, so its end is exactly the point -- or, before any
 * encoder ran, the start of the next.
 */

namespace
{
    constexpr NSUInteger kVisibilityBytes = 1u << 20;   // 131072 queries a frame slot to start with
    constexpr uint32_t   kSamplesPerSlot  = 4096;
}

// ===========================================================================
// Occlusion
// ===========================================================================

void MTLOcclusionQueriesImpl::Release()
{
    if(MetalDynamicGDI* Driver = GetMetalDriver())
    {
        MetalState* S = Driver->GetState();
        if(S && S->OpenQuerySet == this) S->OpenQuerySet = nullptr;
    }
    Buffer = nil;   // command buffers still writing it keep it alive
    delete this;
}

GDIOcclusionQueries* MetalDynamicGDI::CreateOcclusionQueries(const uint32_t InCapacity)
{
    if(!bInitialized || InCapacity == 0) return nullptr;
    return new MTLOcclusionQueriesImpl(InCapacity);
}

void MetalDynamicGDI::BeginOcclusionRound(GDIOcclusionQueries* InSet, uint32_t InCount)
{
    auto* Set = static_cast<MTLOcclusionQueriesImpl*>(InSet);
    if(!Set || !S->Cmd || Set->Open >= 0) return;
    InCount = std::min(InCount, Set->GetCapacity());

    // The range: from the slot's buffer, a bigger one if it is full. Passes
    // already open carry the old buffer; BeginOcclusionQuery reopens them.
    const uint32_t   Slot  = S->FrameSlot;
    const NSUInteger Bytes = NSUInteger(InCount) * sizeof(uint64_t);
    if(!S->Visibility[Slot] || S->VisibilityUsed[Slot] + Bytes > S->Visibility[Slot].length)
    {
        const NSUInteger Size = std::max<NSUInteger>({ kVisibilityBytes, Bytes, S->Visibility[Slot] ? S->Visibility[Slot].length * 2 : 0 });
        S->Visibility[Slot] = [S->Device newBufferWithLength:Size options:MTLResourceStorageModeShared];
        S->Visibility[Slot].label = @"Nimble.Visibility";
        S->VisibilityUsed[Slot] = 0;
        if(!S->Visibility[Slot]) return;
    }
    Set->Buffer    = S->Visibility[Slot];
    Set->Base      = S->VisibilityUsed[Slot] / sizeof(uint64_t);
    Set->Count     = InCount;
    Set->Slot      = Slot;
    Set->Serial    = S->FrameSerial;
    Set->bResolved = false;
    Set->bPending  = false;
    std::fill(Set->Ended.begin(), Set->Ended.end(), uint8_t(0));
    std::fill(Set->Split.begin(), Set->Split.end(), uint8_t(0));
    // The slot's frame has completed (BeginRendering waited), so the CPU may clear it.
    std::memset(static_cast<uint8_t*>(Set->Buffer.contents) + Set->Base * sizeof(uint64_t), 0, Bytes);
    S->VisibilityUsed[Slot] += Bytes;
}

void MetalDynamicGDI::BeginOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<MTLOcclusionQueriesImpl*>(InSet);
    if(!Set || !S->Cmd || !Set->Buffer || InIndex >= Set->Count || Set->bResolved || Set->Open >= 0 || Set->Ended[InIndex]
       || S->OpenQuerySet)
    {
        return;
    }
    // A pass carrying another visibility buffer (one made before a growth) ends first.
    if(S->Encoder && S->EncoderVisibility != Set->Buffer) S->EndPass();
    id<MTLRenderCommandEncoder> Encoder = S->RenderEncoder();
    if(!Encoder || S->EncoderVisibility != Set->Buffer) return;
    [Encoder setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:(Set->Base + InIndex) * sizeof(uint64_t)];
    Set->Open          = int32_t(InIndex);
    S->OpenQuerySet    = Set;
    S->OpenQueryIndex  = InIndex;
}

void MetalDynamicGDI::EndOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<MTLOcclusionQueriesImpl*>(InSet);
    if(!Set || Set->Open != int32_t(InIndex)) return;
    if(S->Encoder && S->OpenQuerySet == Set)
    {
        [S->Encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    }
    Set->Ended[InIndex] = 1;
    Set->Open           = -1;
    S->OpenQuerySet     = nullptr;
}

void MetalDynamicGDI::ResolveOcclusionQueries(GDIOcclusionQueries* InSet)
{
    // Nothing to copy: the answers are in the visibility buffer when the frame completes.
    auto* Set = static_cast<MTLOcclusionQueriesImpl*>(InSet);
    if(!Set || !Set->Buffer || Set->Count == 0 || Set->bResolved || Set->Open >= 0) return;
    Set->bResolved = true;
    Set->bPending  = true;
}

bool MetalDynamicGDI::ReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible)
{
    auto* Set = static_cast<MTLOcclusionQueriesImpl*>(InSet);
    if(!Set || !Set->bPending || S->CompletedSerial.load(std::memory_order_acquire) < Set->Serial) return false;
    const auto* Words = static_cast<const uint64_t*>(Set->Buffer.contents) + Set->Base;
    OutVisible.resize(Set->Count);
    for(uint32_t Index = 0; Index < Set->Count; ++Index)
    {
        // Unended and split queries are answered visible, never wrongly culled.
        OutVisible[Index] = (!Set->Ended[Index] || Set->Split[Index] || Words[Index] != 0) ? 1 : 0;
    }
    Set->bPending = false;
    return true;
}

// ===========================================================================
// Timestamps
// ===========================================================================

bool MetalDynamicGDI::SupportsGPUTimestamps() const
{
    return S->Samples[0] != nil;
}

void MetalState::AllocateSamples(int32_t& OutStart, int32_t& OutEnd)
{
    OutStart = OutEnd = -1;
    if(!bTiming || !Samples[FrameSlot] || SamplesUsed[FrameSlot] + 2 > SamplesPerSlot) return;
    OutStart = int32_t(SamplesUsed[FrameSlot]);
    OutEnd   = OutStart + 1;
    SamplesUsed[FrameSlot] += 2;
    for(const uint32_t Index : PendingTimestamps)
    {
        TimestampSample[FrameSlot][Index] = OutStart;
    }
    PendingTimestamps.clear();
}

id<MTLComputeCommandEncoder> MetalState::ComputeEncoder(id<MTLCommandBuffer> InCmd, const bool bInTransient, int32_t& OutEnd)
{
    OutEnd = -1;
    int32_t Start = -1;
    if(!bInTransient) AllocateSamples(Start, OutEnd);
    if(Start < 0) return [InCmd computeCommandEncoder];
    MTLComputePassDescriptor* Pass = [MTLComputePassDescriptor computePassDescriptor];
    Pass.sampleBufferAttachments[0].sampleBuffer              = Samples[FrameSlot];
    Pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(Start);
    Pass.sampleBufferAttachments[0].endOfEncoderSampleIndex   = NSUInteger(OutEnd);
    return [InCmd computeCommandEncoderWithDescriptor:Pass];
}

id<MTLBlitCommandEncoder> MetalState::BlitEncoder(id<MTLCommandBuffer> InCmd, const bool bInTransient, int32_t& OutEnd)
{
    OutEnd = -1;
    int32_t Start = -1;
    if(!bInTransient) AllocateSamples(Start, OutEnd);
    if(Start < 0) return [InCmd blitCommandEncoder];
    MTLBlitPassDescriptor* Pass = [MTLBlitPassDescriptor blitPassDescriptor];
    Pass.sampleBufferAttachments[0].sampleBuffer              = Samples[FrameSlot];
    Pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(Start);
    Pass.sampleBufferAttachments[0].endOfEncoderSampleIndex   = NSUInteger(OutEnd);
    return [InCmd blitCommandEncoderWithDescriptor:Pass];
}

void MetalDynamicGDI::WriteGPUTimestamp(const uint32_t InIndex)
{
    if(!S->Samples[0] || !S->Cmd || InIndex >= GDI_GPU_TIMESTAMP_MAX) return;
    const uint32_t Slot = S->FrameSlot;
    if(S->TimestampsWritten[Slot] & (uint64_t(1) << InIndex)) return;
    S->TimestampsWritten[Slot] |= uint64_t(1) << InIndex;

    if(!S->bTiming)
    {
        // The first ask: encoders sample from now on. This one waits for the next.
        S->bTiming = true;
        if(S->Encoder) S->EndPass();
        S->PendingTimestamps.push_back(InIndex);
        return;
    }
    // Mid-pass: the pass ends here, so its end sample is this point.
    if(S->Encoder) S->EndPass();
    if(S->LastEndSample >= 0)
    {
        S->TimestampSample[Slot][InIndex] = S->LastEndSample;
    }
    else
    {
        S->PendingTimestamps.push_back(InIndex);
    }
}

void MetalState::CollectTimestamps(const uint32_t InSlot)
{
    PreviousTimestamps = 0;
    const uint32_t Used = SamplesUsed[InSlot];
    if(Samples[InSlot] && Used > 0 && TimestampsWritten[InSlot] != 0)
    {
        NSData* Resolved = [Samples[InSlot] resolveCounterRange:NSMakeRange(0, Used)];
        const auto* Ticks = static_cast<const MTLCounterResultTimestamp*>(Resolved.bytes);
        const NSUInteger Count = Resolved.length / sizeof(MTLCounterResultTimestamp);
        for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
        {
            const int32_t At = TimestampSample[InSlot][Index];
            if(!(TimestampsWritten[InSlot] & (uint64_t(1) << Index)) || At < 0) continue;
            const NSUInteger Local = NSUInteger(At);
            if(Local >= Count) continue;
            const uint64_t Value = Ticks[Local].timestamp;
            // An encoder with no GPU work records nothing (0, or the error value).
            if(Value == 0 || Value == MTLCounterErrorValue) continue;
            PreviousTimestampMs[Index] = double(Value) / 1.0e6;   // Apple GPU timestamps are nanoseconds
            PreviousTimestamps |= uint64_t(1) << Index;
        }
    }
    SamplesUsed[InSlot]       = 0;
    TimestampsWritten[InSlot] = 0;
    std::fill(std::begin(TimestampSample[InSlot]), std::end(TimestampSample[InSlot]), -1);
}

uint64_t MetalDynamicGDI::GetPreviousGPUTimestamps(double* OutMilliseconds)
{
    for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
    {
        if(S->PreviousTimestamps & (uint64_t(1) << Index)) OutMilliseconds[Index] = S->PreviousTimestampMs[Index];
    }
    return S->PreviousTimestamps;
}

/* The timestamp counter set, if the device samples at stage boundaries: every Apple GPU does. */
void MTLCreateTimestampSamples(MetalState& InState)
{
    for(auto& Each : InState.Samples) { Each = nil; }
    for(auto& Slot : InState.TimestampSample) { std::fill(std::begin(Slot), std::end(Slot), -1); }
    if(![InState.Device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return;
    id<MTLCounterSet> Timestamps = nil;
    for(id<MTLCounterSet> Each in InState.Device.counterSets)
    {
        if([Each.name isEqualToString:MTLCommonCounterSetTimestamp]) Timestamps = Each;
    }
    if(!Timestamps) return;
    MTLCounterSampleBufferDescriptor* Desc = [[MTLCounterSampleBufferDescriptor alloc] init];
    Desc.counterSet  = Timestamps;
    Desc.sampleCount = kSamplesPerSlot;   // 32 KB, the most a sample buffer may be
    Desc.storageMode = MTLStorageModeShared;
    Desc.label       = @"Nimble.Timestamps";
    for(uint32_t Slot = 0; Slot < InState.FramesInFlight; ++Slot)
    {
        NSError* Error = nil;
        InState.Samples[Slot] = [InState.Device newCounterSampleBufferWithDescriptor:Desc error:&Error];
        if(!InState.Samples[Slot])
        {
            MTLUtil::LogError("the GPU timestamp buffers (GPU timing is off)", Error);
            for(auto& Each : InState.Samples) { Each = nil; }
            return;
        }
    }
    InState.SamplesPerSlot = kSamplesPerSlot;
}

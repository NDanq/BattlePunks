#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VKRenderTarget.h"
#include "Private/GDI/Vulkan/VKTransientOps.h"

#include <algorithm>
#include <string>
#include <vector>
#include "Public/GDI/GDILog.h"

using namespace VKOps;

extern GDIDynamicDriver* GDynamicGDI;

/*
 * OCCLUSION QUERIES (GDIQuery.h). A set is a VK_QUERY_TYPE_OCCLUSION pool and
 * one host-visible buffer of a uint32 per query: the round's answers are
 * copied into it after the last query (ResolveOcclusionQueries), where
 * conditional rendering reads them on the GPU and, once the block's fence has
 * passed, the CPU reads them (ReadOcclusionResults).
 *
 * The pool is reset at the start of each round, outside any rendering, so a
 * round must begin before the first draw it measures, and both its reset and
 * its copy close and reopen the open rendering region, as a dispatch does.
 */
class VKOcclusionQueries final : public GDIOcclusionQueries
{
public:
    VKOcclusionQueries(VKDevice* InDevice, const uint32_t InCapacity)
        : GDIOcclusionQueries(InCapacity), Device(InDevice)
    {
        VkQueryPoolCreateInfo PoolInfo{};
        PoolInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        PoolInfo.queryType  = VK_QUERY_TYPE_OCCLUSION;
        PoolInfo.queryCount = InCapacity;
        if(vkCreateQueryPool(Device->GetDevice(), &PoolInfo, nullptr, &Pool) != VK_SUCCESS)
        {
            Pool = VK_NULL_HANDLE;
            return;
        }

        /* Conditional rendering reads it too, where the device has that. */
        VkBufferUsageFlags Usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(Device->SupportsConditionalRendering()) { Usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT; }
        if(!AllocStagingBuffer(Device, VkDeviceSize(InCapacity) * sizeof(uint32_t), Usage, Results, ResultsMemory, /*bInReadback*/ true))
        {
            vkDestroyQueryPool(Device->GetDevice(), Pool, nullptr);
            Pool = VK_NULL_HANDLE;
            return;
        }
        vkMapMemory(Device->GetDevice(), ResultsMemory, 0, VK_WHOLE_SIZE, 0, &Mapped);
        Ended.assign(InCapacity, 0);
    }

    ~VKOcclusionQueries() override = default;

    void Release() override
    {
        if(auto* Driver = static_cast<VulkanDynamicGDI*>(GDynamicGDI))
        {
            Driver->ReleaseOcclusionQueries(this);
        }
        delete this;
    }

    bool IsValid() const { return Pool != VK_NULL_HANDLE && Results != VK_NULL_HANDLE && Mapped; }

    VKDevice*            Device        = nullptr;
    VkQueryPool          Pool          = VK_NULL_HANDLE;
    VkBuffer             Results       = VK_NULL_HANDLE;
    VkDeviceMemory       ResultsMemory = VK_NULL_HANDLE;
    void*                Mapped        = nullptr;

    /* This round: how many queries, which have ended, whether one is open, and
     * the timeline value its block signals. Pending: resolved, not yet read. */
    uint32_t             Count    = 0;
    std::vector<uint8_t> Ended;
    int32_t              Open     = -1;
    uint64_t             DoneAt   = 0;
    bool                 bResolved = false;
    bool                 bPending  = false;
};

/* Secondary command buffers are core Vulkan; recording one inside a dynamic
 * rendering (VkCommandBufferInheritanceRenderingInfo) needs dynamic rendering,
 * which every device here is created with (API 1.3). The inheritance features
 * are extras: a query or predicate that brackets one draw is recorded inside
 * the chunk that draws it. */
GDIParallelRecordingSupport VulkanDynamicGDI::GetParallelRecordingSupport() const
{
    GDIParallelRecordingSupport Out;
    if(!DeviceObj || DeviceObj->GetDevice() == VK_NULL_HANDLE)
    {
        Out.Reason = "no Vulkan device";
        return Out;
    }
    Out.bSupported            = true;
    Out.bInheritedQueries     = DeviceObj->SupportsInheritedQueries();
    Out.bInheritedPredication = DeviceObj->SupportsInheritedConditionalRendering();
    Out.Mechanism             = "secondary command buffers inside dynamic rendering";
    return Out;
}

bool VulkanDynamicGDI::SupportsOcclusionQueries() const
{
    return DeviceObj && DeviceObj->IsValid();
}

bool VulkanDynamicGDI::SupportsOcclusionPredication() const
{
    return SupportsOcclusionQueries() && DeviceObj->SupportsConditionalRendering() && CmdBeginConditionalRendering && CmdEndConditionalRendering;
}

GDIOcclusionQueries* VulkanDynamicGDI::CreateOcclusionQueries(const uint32_t InCapacity)
{
    auto* Set = new VKOcclusionQueries(DeviceObj, InCapacity);
    if(!Set->IsValid())
    {
        GDI_LOG("[VulkanDynamicGDI] could not create an occlusion query set of " + std::to_string(InCapacity))
        Set->Release();
        return nullptr;
    }
    return Set;
}

void VulkanDynamicGDI::ReleaseOcclusionQueries(VKOcclusionQueries* InSet)
{
    if(Immediate.bPredicateOpen && Immediate.OpenPredicateBuffer == InSet->Results) { CloseDrawPredicate(); }
    if(PredicateSet == InSet)
    {
        PredicateSet   = nullptr;
        PredicateIndex = -1;
    }
    if(!DeviceObj || !DeviceObj->IsValid()) { return; }
    if(InSet->Mapped) { vkUnmapMemory(DeviceObj->GetDevice(), InSet->ResultsMemory); }
    InSet->Mapped = nullptr;
    DeferDestroyBuffer(InSet->Results, InSet->ResultsMemory);
    if(InSet->Pool != VK_NULL_HANDLE) { DeferredQueryPools.push_back(InSet->Pool); }
    InSet->Results       = VK_NULL_HANDLE;
    InSet->ResultsMemory = VK_NULL_HANDLE;
    InSet->Pool          = VK_NULL_HANDLE;
}

/* InWork outside the open rendering region, which is reopened after it (its contents loaded). */
template<typename WorkFn>
void VulkanDynamicGDI::RecordOutsideRendering(WorkFn&& InWork)
{
    const bool       bWasActive = CurrentStateData->bRenderingActive;
    GDIRenderTarget* Colors[GDI_MAX_COLOR_TARGETS] = {};
    const uint32_t   ColorCount = CurrentStateData->ActiveColorCount;
    for(uint32_t Index = 0; Index < ColorCount; ++Index) { Colors[Index] = CurrentStateData->ActiveColors[Index]; }
    GDIRenderTarget* Depth = CurrentStateData->ActiveDepth;

    EndActiveRenderingRegion();
    InWork(CurrentStateData->CurrentCmd);
    if(bWasActive && ColorCount > 0) { SetRenderTargets(Colors, ColorCount, Depth); }
}

void VulkanDynamicGDI::BeginOcclusionRound(GDIOcclusionQueries* InSet, const uint32_t InCount)
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE || Set->Open >= 0)
    {
        return;
    }
    RecordOutsideRendering([&](const VkCommandBuffer InCmd)
    {
        vkCmdResetQueryPool(InCmd, Set->Pool, 0, InCount);
    });
    Set->Count     = InCount;
    std::fill(Set->Ended.begin(), Set->Ended.end(), uint8_t(0));
    Set->DoneAt    = RenderTimelineCounter + 1;   // what this block's EndRendering signals
    Set->bResolved = false;
    Set->bPending  = false;
}

void VulkanDynamicGDI::BeginOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE
       || InIndex >= Set->Count || Set->bResolved || Set->Open >= 0 || Set->Ended[InIndex])
    {
        return;
    }
    CloseDrawPredicate();   // a query is not recorded under another's predicate
    /* Binary: any sample at all. */
    vkCmdBeginQuery(CurrentStateData->CurrentCmd, Set->Pool, InIndex, 0);
    Set->Open = int32_t(InIndex);
}

void VulkanDynamicGDI::EndOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE || Set->Open != int32_t(InIndex))
    {
        return;
    }
    vkCmdEndQuery(CurrentStateData->CurrentCmd, Set->Pool, InIndex);
    Set->Ended[InIndex] = 1;
    Set->Open = -1;
}

void VulkanDynamicGDI::ResolveOcclusionQueries(GDIOcclusionQueries* InSet)
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    if(!Set->IsValid() || !CurrentStateData || CurrentStateData->CurrentCmd == VK_NULL_HANDLE
       || Set->Count == 0 || Set->bResolved || Set->Open >= 0)
    {
        return;
    }
    RecordOutsideRendering([&](const VkCommandBuffer InCmd)
    {
        /* Runs of ended queries are copied, waiting for them on the GPU; a
         * run never ended -- never begun, so never available, which a copy
         * would wait on for ever -- is filled with 1: drawn. */
        for(uint32_t Start = 0; Start < Set->Count;)
        {
            const uint8_t bEnded = Set->Ended[Start];
            uint32_t End = Start + 1;
            while(End < Set->Count && Set->Ended[End] == bEnded) { ++End; }
            const VkDeviceSize Offset = VkDeviceSize(Start) * sizeof(uint32_t);
            if(bEnded)
            {
                vkCmdCopyQueryPoolResults(InCmd, Set->Pool, Start, End - Start, Set->Results, Offset, sizeof(uint32_t),
                                          VK_QUERY_RESULT_WAIT_BIT);
            }
            else
            {
                vkCmdFillBuffer(InCmd, Set->Results, Offset, VkDeviceSize(End - Start) * sizeof(uint32_t), 1u);
            }
            Start = End;
        }

        VkBufferMemoryBarrier2 Barrier{};
        Barrier.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        Barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        Barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        Barrier.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
        Barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        if(DeviceObj->SupportsConditionalRendering())
        {
            Barrier.dstStageMask  |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
            Barrier.dstAccessMask |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
        }
        Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.buffer = Set->Results;
        Barrier.offset = 0;
        Barrier.size   = VK_WHOLE_SIZE;

        VkDependencyInfo Dep{};
        Dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        Dep.bufferMemoryBarrierCount = 1;
        Dep.pBufferMemoryBarriers    = &Barrier;
        vkCmdPipelineBarrier2(InCmd, &Dep);
    });
    Set->bResolved = true;
    Set->bPending  = true;
}

VKOcclusionQueries* VulkanDynamicGDI::UsablePredicate(GDIOcclusionQueries* InSet, const int32_t InIndex) const
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    const bool bUsable = Set && Set->IsValid() && Set->bResolved && InIndex >= 0 && uint32_t(InIndex) < Set->Count;
    return bUsable ? Set : nullptr;
}

GDIOcclusionQueries* VulkanDynamicGDI::ImmediatePredicateSet() const
{
    return PredicateSet;
}

void VulkanDynamicGDI::SetOcclusionPredicate(GDIOcclusionQueries* InSet, const int32_t InIndex)
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    const bool bUsable = Set && Set->IsValid() && Set->bResolved && InIndex >= 0 && uint32_t(InIndex) < Set->Count;
    PredicateSet   = bUsable ? Set : nullptr;
    PredicateIndex = bUsable ? InIndex : -1;
}

bool VulkanDynamicGDI::ReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible)
{
    auto* Set = static_cast<VKOcclusionQueries*>(InSet);
    if(!Set->IsValid() || !Set->bPending || !IsSubmissionDone(Set->DoneAt))
    {
        return false;
    }
    const auto* Words = static_cast<const uint32_t*>(Set->Mapped);
    OutVisible.resize(Set->Count);
    for(uint32_t Index = 0; Index < Set->Count; ++Index)
    {
        OutVisible[Index] = Words[Index] != 0 ? 1 : 0;
    }
    Set->bPending = false;
    return true;
}

void VulkanDynamicGDI::ApplyDrawPredicate(const VKDrawInputs& In)
{
    VKDrawCaches& C = *In.Caches;
    const bool bWanted = In.PredicateSet && In.PredicateIndex >= 0 && CmdBeginConditionalRendering
                         && DeviceObj->SupportsConditionalRendering();
    const VkBuffer     Buffer = bWanted ? In.PredicateSet->Results : VK_NULL_HANDLE;
    const VkDeviceSize Offset = bWanted ? VkDeviceSize(In.PredicateIndex) * sizeof(uint32_t) : 0;
    if(C.bPredicateOpen && bWanted && Buffer == C.OpenPredicateBuffer && Offset == C.OpenPredicateOffset)
    {
        return;   // the same predicate as the draw before: still open
    }
    CloseDrawPredicate(C, In.Cmd);
    if(!bWanted)
    {
        return;
    }
    VkConditionalRenderingBeginInfoEXT Info{};
    Info.sType  = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
    Info.buffer = Buffer;
    Info.offset = Offset;
    CmdBeginConditionalRendering(In.Cmd, &Info);
    C.bPredicateOpen      = true;
    C.OpenPredicateBuffer = Buffer;
    C.OpenPredicateOffset = Offset;
}

void VulkanDynamicGDI::CloseDrawPredicate(VKDrawCaches& C, const VkCommandBuffer InCmd)
{
    if(C.bPredicateOpen && InCmd != VK_NULL_HANDLE)
    {
        CmdEndConditionalRendering(InCmd);
    }
    C.bPredicateOpen      = false;
    C.OpenPredicateBuffer = VK_NULL_HANDLE;
    C.OpenPredicateOffset = 0;
}

void VulkanDynamicGDI::CloseDrawPredicate()
{
    CloseDrawPredicate(Immediate, CurrentStateData ? CurrentStateData->CurrentCmd : VK_NULL_HANDLE);
}

void VulkanDynamicGDI::LoadQueryFunctions()
{
    CmdBeginConditionalRendering = nullptr;
    CmdEndConditionalRendering   = nullptr;
    if(!DeviceObj || !DeviceObj->IsValid() || !DeviceObj->SupportsConditionalRendering())
    {
        return;
    }
    CmdBeginConditionalRendering = reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(
        vkGetDeviceProcAddr(DeviceObj->GetDevice(), "vkCmdBeginConditionalRenderingEXT"));
    CmdEndConditionalRendering = reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(
        vkGetDeviceProcAddr(DeviceObj->GetDevice(), "vkCmdEndConditionalRenderingEXT"));
}

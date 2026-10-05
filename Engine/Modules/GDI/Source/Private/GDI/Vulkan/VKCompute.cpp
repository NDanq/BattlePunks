/*
 * VulkanDynamicGDI's compute and GPU buffers (GDIDispatchCompute,
 * GDICreateBuffer, GDIReadBuffer): dispatches record into the frame's one
 * command buffer, outside any render pass, in order with the draws.
 */
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/VKBuffer.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VKShader.h"
#include "Public/GDI/Vulkan/VKPipelineCache.h"
#include "Public/GDI/Vulkan/VKDescriptorAllocator.h"
#include "Public/GDI/Vulkan/VKRenderTarget.h"
#include "Private/GDI/Vulkan/VKTransientOps.h"

#include <algorithm>
#include <cstring>
#include "Public/GDI/GDILog.h"

using namespace VKOps;

namespace
{
    /*
     * Everything before against everything after, for the stages buffers
     * meet in: a dispatch sees what earlier draws, copies and dispatches did
     * to its buffers, and the draws and dispatches after it see what it wrote.
     */
    void RecordBufferBarrier(VkCommandBuffer InCmd)
    {
        constexpr VkPipelineStageFlags2 Stages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT
                                               | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        VkMemoryBarrier2 Barrier{};
        Barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        Barrier.srcStageMask  = Stages;
        Barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        Barrier.dstStageMask  = Stages;
        Barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
                              | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo Dep{};
        Dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        Dep.memoryBarrierCount = 1;
        Dep.pMemoryBarriers    = &Barrier;
        vkCmdPipelineBarrier2(InCmd, &Dep);
    }
}

bool VulkanDynamicGDI::SupportsCompute() const
{
    return DeviceObj && DeviceObj->IsValid() && DeviceObj->GraphicsQueueSupportsCompute();
}

GDIBuffer* VulkanDynamicGDI::CreateBuffer(const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
{
    if(!SupportsCompute()) return nullptr;
    auto* Buffer = new VKBuffer(DeviceObj, InBytes, InUsage, InInitialData);
    if(!Buffer->IsValid())
    {
        delete Buffer;
        return nullptr;
    }
    return Buffer;
}

bool VulkanDynamicGDI::DispatchCompute(GDIShader* InShader, const void* InParams, const uint32_t InParamBytes,
                                       GDIBuffer* const* InBuffers, const uint32_t InBufferCount,
                                       const uint32_t InX, const uint32_t InY, const uint32_t InZ)
{
    if(!CurrentStateData || !PipelineCache || !DescAlloc || !DeviceObj) return false;
    const auto*    Resource = static_cast<VKShaderResource*>(InShader->Resource);
    VkShaderModule Module   = Resource ? Resource->GetShaderModule() : VK_NULL_HANDLE;
    if(Module == VK_NULL_HANDLE) return false;
    for(uint32_t Each = 0; Each < InBufferCount; ++Each)
    {
        if(!static_cast<VKBuffer*>(InBuffers[Each])->IsValid()) return false;
    }

    const VKPipelineCache::PipelineFetch Fetch = PipelineCache->GetOrCreateCompute(Module, InBufferCount);
    if(Fetch.Pipeline == VK_NULL_HANDLE) return false;

    /* The parameters, in this frame's UBO ring (at least 16 bytes: a block is never empty). */
    const VkDeviceSize      ParamBytes = std::max<VkDeviceSize>(16, (InParamBytes + 15) & ~15u);
    const VKUBOAllocation   Params     = DescAlloc->AllocateUBO(ParamBytes);
    const VkDescriptorSet   Set        = DescAlloc->Allocate(Fetch.DescLayout);
    if(Params.Buffer == VK_NULL_HANDLE || Set == VK_NULL_HANDLE) return false;
    std::memset(Params.MappedPtr, 0, size_t(ParamBytes));
    if(InParamBytes > 0) { std::memcpy(Params.MappedPtr, InParams, InParamBytes); }

    VkDescriptorBufferInfo Infos[GDI_COMPUTE_MAX_STORAGE_BUFFERS + 1]{};
    VkWriteDescriptorSet   Writes[GDI_COMPUTE_MAX_STORAGE_BUFFERS + 1]{};
    Infos[0].buffer = Params.Buffer;
    Infos[0].offset = Params.Offset;
    Infos[0].range  = ParamBytes;
    for(uint32_t Each = 0; Each < InBufferCount; ++Each)
    {
        Infos[Each + 1].buffer = static_cast<VKBuffer*>(InBuffers[Each])->GetBuffer();
        Infos[Each + 1].offset = 0;
        Infos[Each + 1].range  = VK_WHOLE_SIZE;
    }
    for(uint32_t Each = 0; Each <= InBufferCount; ++Each)
    {
        Writes[Each].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        Writes[Each].dstSet          = Set;
        Writes[Each].dstBinding      = Each;
        Writes[Each].descriptorCount = 1;
        Writes[Each].descriptorType  = Each == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        Writes[Each].pBufferInfo     = &Infos[Each];
    }
    vkUpdateDescriptorSets(DeviceObj->GetDevice(), InBufferCount + 1, Writes, 0, nullptr);

    /* Outside a frame (a render command): on its own command buffer, at once, waited for. */
    if(CurrentStateData->CurrentCmd == VK_NULL_HANDLE)
    {
        VkCommandPool   Pool = VK_NULL_HANDLE;
        VkCommandBuffer Cmd  = VK_NULL_HANDLE;
        if(!BeginTransientCmd(DeviceObj, Pool, Cmd)) return false;
        RecordBufferBarrier(Cmd);
        vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Fetch.Pipeline);
        vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Fetch.PipeLayout, 0, 1, &Set, 0, nullptr);
        vkCmdDispatch(Cmd, InX, InY, InZ);
        RecordBufferBarrier(Cmd);
        EndAndSubmitTransientCmd(DeviceObj, Pool, Cmd);
        return true;
    }

    /* Outside the render pass, which is reopened after it (its contents loaded), as FlushForReadback does. */
    const bool       bWasActive = CurrentStateData->bRenderingActive;
    GDIRenderTarget* Colors[GDI_MAX_COLOR_TARGETS] = {};
    const uint32_t   ColorCount = CurrentStateData->ActiveColorCount;
    for(uint32_t Index = 0; Index < ColorCount; ++Index) { Colors[Index] = CurrentStateData->ActiveColors[Index]; }
    GDIRenderTarget* Depth = CurrentStateData->ActiveDepth;
    EndActiveRenderingRegion();

    VkCommandBuffer Cmd = CurrentStateData->CurrentCmd;
    RecordBufferBarrier(Cmd);
    vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Fetch.Pipeline);
    vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Fetch.PipeLayout, 0, 1, &Set, 0, nullptr);
    vkCmdDispatch(Cmd, InX, InY, InZ);
    RecordBufferBarrier(Cmd);

    if(bWasActive && ColorCount > 0) { SetRenderTargets(Colors, ColorCount, Depth); }
    return true;
}

bool VulkanDynamicGDI::ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes)
{
    auto* Buffer = static_cast<VKBuffer*>(InBuffer);
    if(!Buffer->IsValid() || !DeviceObj) return false;

    /* What the open frame has recorded runs first. */
    FlushForReadback();

    VkBuffer       Staging    = VK_NULL_HANDLE;
    VkDeviceMemory StagingMem = VK_NULL_HANDLE;
    if(!AllocStagingBuffer(DeviceObj, Buffer->GetSize(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, Staging, StagingMem)) return false;

    VkCommandPool   Pool = VK_NULL_HANDLE;
    VkCommandBuffer Cmd  = VK_NULL_HANDLE;
    bool bRead = false;
    if(BeginTransientCmd(DeviceObj, Pool, Cmd))
    {
        RecordBufferBarrier(Cmd);
        VkBufferCopy Region{};
        Region.size = Buffer->GetSize();
        vkCmdCopyBuffer(Cmd, Buffer->GetBuffer(), Staging, 1, &Region);
        EndAndSubmitTransientCmd(DeviceObj, Pool, Cmd);

        void* Mapped = nullptr;
        vkMapMemory(DeviceObj->GetDevice(), StagingMem, 0, Buffer->GetSize(), 0, &Mapped);
        OutBytes.assign(static_cast<const uint8_t*>(Mapped), static_cast<const uint8_t*>(Mapped) + Buffer->GetSize());
        vkUnmapMemory(DeviceObj->GetDevice(), StagingMem);
        bRead = true;
    }
    vkDestroyBuffer(DeviceObj->GetDevice(), Staging, nullptr);
    vkFreeMemory(DeviceObj->GetDevice(), StagingMem, nullptr);
    return bRead;
}

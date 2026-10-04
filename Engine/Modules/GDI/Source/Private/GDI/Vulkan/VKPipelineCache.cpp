#include "Public/GDI/Vulkan/VKPipelineCache.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/GDIDefinitions.h"

#include "Public/GDI/GDILog.h"
#include <string>
#include "Public/Core/CorePlatform.h"
#include <vector>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

/** Vertex layout IDs */
static constexpr uint32_t kLayoutStandard  = 0;
static constexpr uint32_t kLayoutInstanced = 1;
static constexpr uint32_t kLayoutMesh      = 2;

/** Maximum vertex bindings / attributes across all known layouts */
static constexpr uint32_t kMaxBindings = 5;
static constexpr uint32_t kMaxAttribs  = 8;

/** Push-constant range (bytes). Included in all layouts for future use. */
static constexpr uint32_t kPushConstantBytes = 128;

// ---------------------------------------------------------------------------
//  Static helpers
// ---------------------------------------------------------------------------

void VKPipelineCache::ApplyBlendState(PipelineKey&          Key,
                                        VkBool32              BlendEnable,
                                        VkBlendFactor         SrcColor,
                                        VkBlendFactor         DstColor,
                                        VkBlendFactor         SrcAlpha,
                                        VkBlendFactor         DstAlpha,
                                        VkColorComponentFlags WriteMask,
                                        VkBlendOp             ColorOp,
                                        VkBlendOp             AlphaOp)
{
    Key.BlendEnable    = BlendEnable;
    Key.SrcColorBlend  = SrcColor;
    Key.DstColorBlend  = DstColor;
    Key.SrcAlphaBlend  = SrcAlpha;
    Key.DstAlphaBlend  = DstAlpha;
    Key.ColorBlendOp   = ColorOp;
    Key.AlphaBlendOp   = AlphaOp;
    Key.ColorWriteMask = WriteMask;
}

void VKPipelineCache::ApplyDepthStencilState(PipelineKey& Key,
                                               VkBool32     DepthTest,
                                               VkBool32     DepthWrite,
                                               VkBool32     StencilTest,
                                               VkCompareOp  DepthCompare)
{
    Key.DepthTestEnable   = DepthTest;
    Key.DepthWriteEnable  = DepthWrite;
    Key.StencilTestEnable = StencilTest;
    Key.DepthCompareOp    = DepthCompare;
}

// ---------------------------------------------------------------------------
//  FNV-1a hash over the named fields of PipelineKey (no padding bytes)
// ---------------------------------------------------------------------------

uint64_t VKPipelineCache::HashKey(const PipelineKey& K)
{
    uint64_t H = 0xcbf29ce484222325ULL;
    auto Mix = [&H](const void* Data, size_t N)
    {
        const auto* B = static_cast<const uint8_t*>(Data);
        for(size_t i = 0; i < N; ++i)
        {
            H ^= static_cast<uint64_t>(B[i]);
            H *= 0x00000100000001B3ULL;
        }
    };

    Mix(&K.VertexShader,      sizeof(K.VertexShader));
    Mix(&K.FragmentShader,    sizeof(K.FragmentShader));
    Mix(&K.VertexLayoutID,    sizeof(K.VertexLayoutID));
    Mix(&K.DescLayoutGroup,   sizeof(K.DescLayoutGroup));
    Mix(&K.BlendEnable,       sizeof(K.BlendEnable));
    Mix(&K.SrcColorBlend,     sizeof(K.SrcColorBlend));
    Mix(&K.DstColorBlend,     sizeof(K.DstColorBlend));
    Mix(&K.SrcAlphaBlend,     sizeof(K.SrcAlphaBlend));
    Mix(&K.DstAlphaBlend,     sizeof(K.DstAlphaBlend));
    Mix(&K.ColorBlendOp,      sizeof(K.ColorBlendOp));
    Mix(&K.AlphaBlendOp,      sizeof(K.AlphaBlendOp));
    Mix(&K.ColorWriteMask,    sizeof(K.ColorWriteMask));
    Mix(&K.DepthTestEnable,   sizeof(K.DepthTestEnable));
    Mix(&K.DepthWriteEnable,  sizeof(K.DepthWriteEnable));
    Mix(&K.DepthCompareOp,    sizeof(K.DepthCompareOp));
    Mix(&K.StencilTestEnable, sizeof(K.StencilTestEnable));
    Mix(&K.RTFormat,          sizeof(K.RTFormat));
    Mix(&K.ColorCount,        sizeof(K.ColorCount));
    Mix(&K.DepthFormat,       sizeof(K.DepthFormat));
    Mix(&K.Topology,          sizeof(K.Topology));
    return H;
}

// ---------------------------------------------------------------------------
//  Vertex input layout tables
// ---------------------------------------------------------------------------

void VKPipelineCache::GetVertexInputState(
    uint32_t                           LayoutID,
    VkVertexInputBindingDescription*   OutBindings,
    uint32_t&                          OutBindingCount,
    VkVertexInputAttributeDescription* OutAttribs,
    uint32_t&                          OutAttribCount)
{
    // Binding 0: vec3 position (stride 12, per-vertex)
    OutBindings[0].binding   = 0;
    OutBindings[0].stride    = sizeof(float) * 3;
    OutBindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Binding 1: vec2 texcoord (stride 8, per-vertex)
    OutBindings[1].binding   = 1;
    OutBindings[1].stride    = sizeof(float) * 2;
    OutBindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Location 0: position (binding 0)
    OutAttribs[0].location = 0;
    OutAttribs[0].binding  = 0;
    OutAttribs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    OutAttribs[0].offset   = 0;

    // Location 1: texcoord (binding 1)
    OutAttribs[1].location = 1;
    OutAttribs[1].binding  = 1;
    OutAttribs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    OutAttribs[1].offset   = 0;

    OutBindingCount = 2;
    OutAttribCount  = 2;

    if(LayoutID == kLayoutMesh)
    {
        // Binding 2 / location 8: vec3 normal, per vertex.
        OutBindings[2].binding   = 2;
        OutBindings[2].stride    = sizeof(float) * 3;
        OutBindings[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        OutAttribs[2].location = 8;
        OutAttribs[2].binding  = 2;
        OutAttribs[2].format   = VK_FORMAT_R32G32B32_SFLOAT;
        OutAttribs[2].offset   = 0;

        // Binding 3 / location 9: vec4 colour, per vertex.
        OutBindings[3].binding   = 3;
        OutBindings[3].stride    = sizeof(float) * 4;
        OutBindings[3].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        OutAttribs[3].location = 9;
        OutAttribs[3].binding  = 3;
        OutAttribs[3].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        OutAttribs[3].offset   = 0;

        // Binding 4 / location 10: vec2 UV1 (lightmap UVs), per vertex.
        OutBindings[4].binding   = 4;
        OutBindings[4].stride    = sizeof(float) * 2;
        OutBindings[4].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        OutAttribs[4].location = 10;
        OutAttribs[4].binding  = 4;
        OutAttribs[4].format   = VK_FORMAT_R32G32_SFLOAT;
        OutAttribs[4].offset   = 0;

        OutBindingCount = 5;
        OutAttribCount  = 5;
        return;
    }

    if(LayoutID == kLayoutInstanced)
    {
        OutBindings[2].binding   = 2;
        OutBindings[2].stride    = sizeof(float) * 16;
        OutBindings[2].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        OutBindingCount = 3;

        for(uint32_t Row = 0; Row < 4; ++Row)
        {
            OutAttribs[2 + Row].location = 2 + Row;
            OutAttribs[2 + Row].binding  = 2;
            OutAttribs[2 + Row].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
            OutAttribs[2 + Row].offset   = Row * sizeof(float) * 4;
        }

        // Binding 3 / location 6: per-instance opacity (float, divisor 1). Drives
        // per-stamp alpha for pressure — see VKMesh's parallel opacity ring.
        OutBindings[3].binding   = 3;
        OutBindings[3].stride    = sizeof(float);
        OutBindings[3].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        OutBindingCount = 4;

        OutAttribs[6].location = 6;
        OutAttribs[6].binding  = 3;
        OutAttribs[6].format   = VK_FORMAT_R32_SFLOAT;
        OutAttribs[6].offset   = 0;

        // Binding 4 / location 7: per-instance color (vec3, divisor 1). Drives
        // per-stamp RGB for pressure color transfer — see VKMesh's color ring.
        OutBindings[4].binding   = 4;
        OutBindings[4].stride    = sizeof(float) * 3;
        OutBindings[4].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        OutBindingCount = 5;

        OutAttribs[7].location = 7;
        OutAttribs[7].binding  = 4;
        OutAttribs[7].format   = VK_FORMAT_R32G32B32_SFLOAT;
        OutAttribs[7].offset   = 0;
        OutAttribCount = 8;
    }
}

// ---------------------------------------------------------------------------
//  Life-cycle
// ---------------------------------------------------------------------------

bool VKPipelineCache::Initialize(VKDevice* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid()) return false;

    LoadFromDisk();
    if(!BuildLayouts()) return false;

    GDI_LOG("[VKPipelineCache] Initialized.")
    return true;
}

void VKPipelineCache::Shutdown()
{
    if(!Device || !Device->IsValid()) return;

    SaveToDisk();

    VkDevice Dev = Device->GetDevice();

    // std::unordered_map iterates pairs where QHash iterated values.
    for(auto& Entry : PipelineEntries)
    {
        if(Entry.second.Pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(Dev, Entry.second.Pipeline, nullptr);
    }
    PipelineEntries.clear();

    for(uint32_t g = 0; g < DLG_Count; ++g)
    {
        if(DescSetLayouts[g]  != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(Dev, DescSetLayouts[g],  nullptr); DescSetLayouts[g]  = VK_NULL_HANDLE; }
        if(PipelineLayouts[g] != VK_NULL_HANDLE) { vkDestroyPipelineLayout(Dev,       PipelineLayouts[g], nullptr); PipelineLayouts[g] = VK_NULL_HANDLE; }
    }

    if(VKCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(Dev, VKCache, nullptr);
        VKCache = VK_NULL_HANDLE;
    }

    Device = nullptr;
}

// ---------------------------------------------------------------------------
//  Disk persistence
// ---------------------------------------------------------------------------

/*
 * Writable per-user location for the pipeline cache blob.
 *
 * appGetAppDataDir creates the directory and returns "" if it cannot, which is
 * why the callers below treat an empty path as "no caching this run" rather
 * than failing the device. A cold pipeline cache costs startup time; it is
 * never a correctness problem.
 */
static std::string CacheFilePath()
{
    const std::string Dir = EngineCore::appGetAppDataDir("Odessa");
    if(Dir.empty())
    {
        return std::string();
    }
    return Dir + "/vk_pipeline.cache";
}

void VKPipelineCache::LoadFromDisk()
{
    std::vector<uint8_t> Blob;

    const std::string Path = CacheFilePath();
    if(!Path.empty() && EngineCore::appFileReadAll(Path, Blob))
    {
        GDI_LOG("[VKPipelineCache] Loaded cache blob from " + Path
             + " (" + ::GDILogDetail::GDINum(Blob.size()) + " bytes).")
    }

    VkPipelineCacheCreateInfo CacheInfo{};
    CacheInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    CacheInfo.initialDataSize = Blob.size();
    CacheInfo.pInitialData    = Blob.empty() ? nullptr : Blob.data();

    if(vkCreatePipelineCache(Device->GetDevice(), &CacheInfo, nullptr, &VKCache) != VK_SUCCESS)
    {
        GDI_LOG("[VKPipelineCache] vkCreatePipelineCache failed - using empty cache.")
        CacheInfo.initialDataSize = 0;
        CacheInfo.pInitialData    = nullptr;
        vkCreatePipelineCache(Device->GetDevice(), &CacheInfo, nullptr, &VKCache);
    }
}

void VKPipelineCache::SaveToDisk() const
{
    if(VKCache == VK_NULL_HANDLE) return;

    size_t DataSize = 0;
    vkGetPipelineCacheData(Device->GetDevice(), VKCache, &DataSize, nullptr);
    if(DataSize == 0) return;

    std::vector<uint8_t> Blob(DataSize);
    vkGetPipelineCacheData(Device->GetDevice(), VKCache, &DataSize, Blob.data());

    const std::string Path = CacheFilePath();
    if(Path.empty()) return;   // appGetAppDataDir already created the directory

    EngineCore::FileHandle File = EngineCore::appFileOpenWrite(Path);
    if(File == nullptr) return;

    const size_t Written = EngineCore::appFileWrite(File, Blob.data(), Blob.size());
    EngineCore::appFileClose(File);

    if(Written == Blob.size())
    {
        GDI_LOG("[VKPipelineCache] Saved " + ::GDILogDetail::GDINum(DataSize) + " bytes to " + Path)
    }
}

// ---------------------------------------------------------------------------
//  Layout construction — one VkDescriptorSetLayout + VkPipelineLayout per group
// ---------------------------------------------------------------------------

bool VKPipelineCache::BuildLayouts()
{
    VkDevice Dev = Device->GetDevice();

    const VkShaderStageFlags kAllStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Helper lambdas for common binding descriptors.
    auto MakeUBO = [&](uint32_t Binding) -> VkDescriptorSetLayoutBinding
    {
        VkDescriptorSetLayoutBinding B{};
        B.binding         = Binding;
        B.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        B.descriptorCount = 1;
        B.stageFlags      = kAllStages;
        return B;
    };
    auto MakeSampler = [&](uint32_t Binding) -> VkDescriptorSetLayoutBinding
    {
        VkDescriptorSetLayoutBinding B{};
        B.binding         = Binding;
        B.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        B.descriptorCount = 1;
        B.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        return B;
    };

    // Push constant range (128 bytes) included in every group layout.
    VkPushConstantRange Push{};
    Push.stageFlags = kAllStages;
    Push.offset     = 0;
    Push.size       = kPushConstantBytes;

    // --- Descriptor set layout bindings per group ---
    struct GroupDef
    {
        VkDescriptorSetLayoutBinding Bindings[GDI_MATERIAL_TEXTURE_BINDING + GDI_MATERIAL_TEXTURE_SLOTS];
        uint32_t                     Count;
    } Groups[DLG_Count];

    // DLG_Base: {UBO@0, UBO@1}
    Groups[DLG_Base].Bindings[0] = MakeUBO(0);
    Groups[DLG_Base].Bindings[1] = MakeUBO(1);
    Groups[DLG_Base].Count       = 2;

    // DLG_CustomUBO: {UBO@0, UBO@1, UBO@2}
    Groups[DLG_CustomUBO].Bindings[0] = MakeUBO(0);
    Groups[DLG_CustomUBO].Bindings[1] = MakeUBO(1);
    Groups[DLG_CustomUBO].Bindings[2] = MakeUBO(2);
    Groups[DLG_CustomUBO].Count       = 3;

    // DLG_Sampler: {UBO@0, UBO@1, Sampler@2}
    Groups[DLG_Sampler].Bindings[0] = MakeUBO(0);
    Groups[DLG_Sampler].Bindings[1] = MakeUBO(1);
    Groups[DLG_Sampler].Bindings[2] = MakeSampler(2);
    Groups[DLG_Sampler].Count       = 3;

    // DLG_UBOAndSampler: {UBO@0, UBO@1, UBO@2, Sampler@3}
    Groups[DLG_UBOAndSampler].Bindings[0] = MakeUBO(0);
    Groups[DLG_UBOAndSampler].Bindings[1] = MakeUBO(1);
    Groups[DLG_UBOAndSampler].Bindings[2] = MakeUBO(2);
    Groups[DLG_UBOAndSampler].Bindings[3] = MakeSampler(3);
    Groups[DLG_UBOAndSampler].Count       = 4;

    // DLG_Material: {UBO@0-3, Sampler@4-11}; textures visible to both stages
    // so a vertex offset can sample one later.
    for(uint32_t b = 0; b < GDI_MATERIAL_TEXTURE_BINDING; ++b)
    {
        Groups[DLG_Material].Bindings[b] = MakeUBO(b);
    }
    for(uint32_t s = 0; s < GDI_MATERIAL_TEXTURE_SLOTS; ++s)
    {
        VkDescriptorSetLayoutBinding Sampler = MakeSampler(GDI_MATERIAL_TEXTURE_BINDING + s);
        Sampler.stageFlags = kAllStages;
        Groups[DLG_Material].Bindings[GDI_MATERIAL_TEXTURE_BINDING + s] = Sampler;
    }
    Groups[DLG_Material].Count = GDI_MATERIAL_TEXTURE_BINDING + GDI_MATERIAL_TEXTURE_SLOTS;

    for(uint32_t g = 0; g < DLG_Count; ++g)
    {
        // Descriptor set layout
        VkDescriptorSetLayoutCreateInfo SetCI{};
        SetCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        SetCI.bindingCount = Groups[g].Count;
        SetCI.pBindings    = Groups[g].Bindings;
        if(vkCreateDescriptorSetLayout(Dev, &SetCI, nullptr, &DescSetLayouts[g]) != VK_SUCCESS)
        {
            GDI_LOG("[VKPipelineCache] vkCreateDescriptorSetLayout failed for group "
                 + ::GDILogDetail::GDINum(g))
            return false;
        }

        // Pipeline layout
        VkPipelineLayoutCreateInfo LayoutCI{};
        LayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        LayoutCI.setLayoutCount         = 1;
        LayoutCI.pSetLayouts            = &DescSetLayouts[g];
        LayoutCI.pushConstantRangeCount = 1;
        LayoutCI.pPushConstantRanges    = &Push;
        if(vkCreatePipelineLayout(Dev, &LayoutCI, nullptr, &PipelineLayouts[g]) != VK_SUCCESS)
        {
            GDI_LOG("[VKPipelineCache] vkCreatePipelineLayout failed for group "
                 + ::GDILogDetail::GDINum(g))
            return false;
        }
    }

    GDI_LOG("[VKPipelineCache] All " + ::GDILogDetail::GDINum(DLG_Count) + " layout groups built.")
    return true;
}

// ---------------------------------------------------------------------------
//  Public pipeline retrieval
// ---------------------------------------------------------------------------

VKPipelineCache::PipelineFetch VKPipelineCache::GetOrCreateFull(const PipelineKey& Key)
{
    PipelineFetch F;
    if(Key.VertexShader == VK_NULL_HANDLE || Key.FragmentShader == VK_NULL_HANDLE)
        return F;

    EDescriptorLayoutGroup Group = Key.DescLayoutGroup;
    F.PipeLayout = PipelineLayouts[Group];
    F.DescLayout = DescSetLayouts[Group];

    const uint64_t H = HashKey(Key);

    auto It = PipelineEntries.find(H);
    if(It != PipelineEntries.end())
    {
        F.Pipeline = It->second.Pipeline;
        return F;
    }

    VkPipeline New = CreatePipeline(Key);
    PipelineEntry Entry;
    Entry.Pipeline = New;
    Entry.Group    = Group;
    PipelineEntries[H] = Entry;

#ifdef QT_DEBUG
    GDI_LOG("[VKPipelineCache] New pipeline: VS=0x" + GDIHex(reinterpret_cast<uintptr_t>(Key.VertexShader))
          + " FS=0x" + GDIHex(reinterpret_cast<uintptr_t>(Key.FragmentShader))
          + " vtxLayout=" + ::GDILogDetail::GDINum(Key.VertexLayoutID)
          + " group=" + ::GDILogDetail::GDINum(int(Group)))
#endif

    F.Pipeline = New;
    return F;
}

// ---------------------------------------------------------------------------
//  Pipeline creation
// ---------------------------------------------------------------------------

VkPipeline VKPipelineCache::CreatePipeline(const PipelineKey& Key) const
{
    if(!Device || PipelineLayouts[Key.DescLayoutGroup] == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    // --- Shader stages ---
    VkPipelineShaderStageCreateInfo Stages[2]{};
    Stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    Stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    Stages[0].module = Key.VertexShader;
    Stages[0].pName  = "main";

    Stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    Stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    Stages[1].module = Key.FragmentShader;
    Stages[1].pName  = "main";

    // --- Vertex input ---
    VkVertexInputBindingDescription   Bindings[kMaxBindings]{};
    VkVertexInputAttributeDescription Attribs[kMaxAttribs]{};
    uint32_t BindingCount = 0, AttribCount = 0;
    GetVertexInputState(Key.VertexLayoutID, Bindings, BindingCount, Attribs, AttribCount);

    VkPipelineVertexInputStateCreateInfo VertexInput{};
    VertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VertexInput.vertexBindingDescriptionCount   = BindingCount;
    VertexInput.pVertexBindingDescriptions      = Bindings;
    VertexInput.vertexAttributeDescriptionCount = AttribCount;
    VertexInput.pVertexAttributeDescriptions    = Attribs;

    // --- Input assembly ---
    VkPipelineInputAssemblyStateCreateInfo InputAssembly{};
    InputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    InputAssembly.topology               = Key.Topology;
    InputAssembly.primitiveRestartEnable = VK_FALSE;

    // --- Viewport / scissor (fully dynamic) ---
    VkPipelineViewportStateCreateInfo ViewportState{};
    ViewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    ViewportState.viewportCount = 1;
    ViewportState.scissorCount  = 1;

    // --- Rasterization ---
    VkPipelineRasterizationStateCreateInfo Raster{};
    Raster.sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    Raster.depthClampEnable = VK_FALSE;
    Raster.polygonMode      = VK_POLYGON_MODE_FILL;
    Raster.cullMode         = VK_CULL_MODE_NONE;
    Raster.frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    Raster.lineWidth        = 1.0f;

    // --- Multisample ---
    VkPipelineMultisampleStateCreateInfo Multisample{};
    Multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Depth / stencil ---
    VkPipelineDepthStencilStateCreateInfo DepthStencil{};
    DepthStencil.sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    DepthStencil.depthTestEnable   = Key.DepthTestEnable;
    DepthStencil.depthWriteEnable  = Key.DepthWriteEnable;
    DepthStencil.depthCompareOp    = Key.DepthCompareOp;
    DepthStencil.stencilTestEnable = Key.StencilTestEnable;

    // --- Color blend ---
    VkPipelineColorBlendAttachmentState BlendAtt{};
    BlendAtt.blendEnable         = Key.BlendEnable;
    BlendAtt.srcColorBlendFactor = Key.SrcColorBlend;
    BlendAtt.dstColorBlendFactor = Key.DstColorBlend;
    BlendAtt.colorBlendOp        = Key.ColorBlendOp;
    BlendAtt.srcAlphaBlendFactor = Key.SrcAlphaBlend;
    BlendAtt.dstAlphaBlendFactor = Key.DstAlphaBlend;
    BlendAtt.alphaBlendOp        = Key.AlphaBlendOp;
    BlendAtt.colorWriteMask      = Key.ColorWriteMask;

    // The same blend for every colour attachment.
    const uint32_t ColorCount = Key.ColorCount == 0 ? 1u : (Key.ColorCount > GDI_MAX_COLOR_TARGETS ? GDI_MAX_COLOR_TARGETS : Key.ColorCount);
    VkPipelineColorBlendAttachmentState BlendAtts[GDI_MAX_COLOR_TARGETS];
    VkFormat                            ColorFormats[GDI_MAX_COLOR_TARGETS];
    for(uint32_t Index = 0; Index < ColorCount; ++Index)
    {
        BlendAtts[Index]    = BlendAtt;
        ColorFormats[Index] = Key.RTFormat;
    }

    VkPipelineColorBlendStateCreateInfo ColorBlend{};
    ColorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    ColorBlend.logicOpEnable   = VK_FALSE;
    ColorBlend.attachmentCount = ColorCount;
    ColorBlend.pAttachments    = BlendAtts;

    // --- Dynamic state ---
    static const VkDynamicState DynStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo DynamicState{};
    DynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    DynamicState.dynamicStateCount = 2;
    DynamicState.pDynamicStates    = DynStates;

    // --- Dynamic rendering pNext (no VkRenderPass needed) ---
    VkPipelineRenderingCreateInfo RenderingInfo{};
    RenderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    RenderingInfo.colorAttachmentCount    = ColorCount;
    RenderingInfo.pColorAttachmentFormats = ColorFormats;
    RenderingInfo.depthAttachmentFormat   = Key.DepthFormat;

    // --- Assemble ---
    VkGraphicsPipelineCreateInfo PipelineCI{};
    PipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    PipelineCI.pNext               = &RenderingInfo;
    PipelineCI.stageCount          = 2;
    PipelineCI.pStages             = Stages;
    PipelineCI.pVertexInputState   = &VertexInput;
    PipelineCI.pInputAssemblyState = &InputAssembly;
    PipelineCI.pViewportState      = &ViewportState;
    PipelineCI.pRasterizationState = &Raster;
    PipelineCI.pMultisampleState   = &Multisample;
    PipelineCI.pDepthStencilState  = &DepthStencil;
    PipelineCI.pColorBlendState    = &ColorBlend;
    PipelineCI.pDynamicState       = &DynamicState;
    PipelineCI.layout              = PipelineLayouts[Key.DescLayoutGroup];
    PipelineCI.renderPass          = VK_NULL_HANDLE;

    VkPipeline Result = VK_NULL_HANDLE;
    const VkResult Res = vkCreateGraphicsPipelines(
        Device->GetDevice(), VKCache, 1, &PipelineCI, nullptr, &Result);

    if(Res != VK_SUCCESS)
    {
        GDI_LOG("[VKPipelineCache] vkCreateGraphicsPipelines failed (" + ::GDILogDetail::GDINum(Res) + ").")
        return VK_NULL_HANDLE;
    }

    return Result;
}

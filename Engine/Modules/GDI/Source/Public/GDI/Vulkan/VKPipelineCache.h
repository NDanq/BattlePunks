#ifndef CORECLIENT_VKPIPELINECACHE_H
#define CORECLIENT_VKPIPELINECACHE_H

#include <vulkan/vulkan.h>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "Public/GDI/GDIDefinitions.h"

#include "Public/GDI/GDIDefinitions.h"

class VKDevice;

/**
 * Descriptor / pipeline layout group IDs.
 *
 * The compiled Vulkan SPIR-V shaders always use bindings 0 and 1 for the
 * core UBOs (ViewParameters, Transform).  Binding 2 and beyond depend on
 * which shader is loaded:
 *
 *   DLG_Base          — {UBO@0, UBO@1}
 *                        Vertex-only shaders with no fragment custom data.
 *
 *   DLG_CustomUBO     — {UBO@0, UBO@1, UBO@2}
 *                        Fragment shaders that add a custom UBO (e.g.
 *                        Basic.fsh with TestColor, LayersBlit.fsh with Opacity).
 *
 *   DLG_Sampler       — {UBO@0, UBO@1, Sampler@2}
 *                        Fragment shaders with a single sampler at binding 2
 *                        and no custom UBO (e.g. Cursor.fsh, Screen.fsh).
 *
 *   DLG_UBOAndSampler — {UBO@0, UBO@1, UBO@2, Sampler@3}
 *                        Fragment shaders with both a custom UBO at binding 2
 *                        AND a sampler at binding 3 (e.g. UberBrush.fsh).
 *
 *   DLG_Material      — {UBO@0-3, Sampler@4-11}
 *                        The material layout (GDIDefinitions.h): any draw whose
 *                        shader UsesMaterialLayout(). Chosen by that flag, not
 *                        by inspecting uniform maps.
 *
 * Each group gets its own VkDescriptorSetLayout and VkPipelineLayout.
 * ExecuteDrawCalls determines the correct group at draw time by inspecting
 * the bound shader's uniform maps.
 */
enum EDescriptorLayoutGroup : uint32_t
{
    DLG_Base          = 0,
    DLG_CustomUBO     = 1,
    DLG_Sampler       = 2,
    DLG_UBOAndSampler = 3,
    DLG_Material      = 4,
    DLG_Count         = 5
};

/**
 * Two-level Vulkan pipeline caching:
 *
 *   Level 1 — VkPipelineCache (driver blob)
 *     Loaded from / saved to %APPDATA%/Odessa/vk_pipeline.cache on every
 *     session.  Vulkan silently discards the blob on GPU or driver change.
 *
 *   Level 2 — in-process hash map (PipelineKey → VkPipeline)
 *     A unique VkPipeline is lazily created on the first draw that needs a
 *     particular (shaders, vertex layout, blend/depth/stencil, RT format,
 *     topology, descriptor layout group) combination.
 *
 * Each EDescriptorLayoutGroup has its own VkDescriptorSetLayout and
 * VkPipelineLayout (see enum doc above).  Push constants (128 bytes, all
 * stages) are included in every layout for future use.
 *
 * Vertex layout IDs (PipelineKey::VertexLayoutID):
 *   0 — standard:  binding 0 = vec3 position, binding 1 = vec2 texcoord
 *   1 — instanced: bindings 0+1 as above, binding 2 = mat4 per-instance transform
 *   2 — mesh:      bindings 0+1 as above, binding 2 = vec3 normal (location 8),
 *                  binding 3 = vec4 colour (location 9), binding 4 = vec2 UV1
 *                  (location 10), binding 8 = vec4 tangent (location 13; in 3
 *                  and 4 too); an indexed mesh from GDICreateMesh
 *   3 — mesh, instanced: 2 plus the instance buffer at binding 5
 *   4 — mesh, skinned: 2 plus bone indices and weights at bindings 6 and 7
 *                  (locations 11 and 12)
 */
class VKPipelineCache
{
public:
    // -------------------------------------------------------------------
    // Pipeline state key
    // -------------------------------------------------------------------
    struct PipelineKey
    {
        // Shader modules
        VkShaderModule        VertexShader      = VK_NULL_HANDLE;
        VkShaderModule        FragmentShader    = VK_NULL_HANDLE;

        uint32_t              VertexLayoutID    = 0;

        // Descriptor layout group (determines VkDescriptorSetLayout used).
        EDescriptorLayoutGroup DescLayoutGroup  = DLG_Base;

        // Blend state.
        VkBool32              BlendEnable       = VK_FALSE;
        VkBlendFactor         SrcColorBlend     = VK_BLEND_FACTOR_ONE;
        VkBlendFactor         DstColorBlend     = VK_BLEND_FACTOR_ZERO;
        VkBlendFactor         SrcAlphaBlend     = VK_BLEND_FACTOR_ONE;
        VkBlendFactor         DstAlphaBlend     = VK_BLEND_FACTOR_ZERO;
        VkBlendOp             ColorBlendOp      = VK_BLEND_OP_ADD;
        VkBlendOp             AlphaBlendOp      = VK_BLEND_OP_ADD;
        VkColorComponentFlags ColorWriteMask    = VK_COLOR_COMPONENT_R_BIT
                                               | VK_COLOR_COMPONENT_G_BIT
                                               | VK_COLOR_COMPONENT_B_BIT
                                               | VK_COLOR_COMPONENT_A_BIT;

        // Depth / stencil state.
        VkBool32              DepthTestEnable   = VK_FALSE;
        VkBool32              DepthWriteEnable  = VK_FALSE;
        VkCompareOp           DepthCompareOp    = VK_COMPARE_OP_LESS;
        VkBool32              StencilTestEnable = VK_FALSE;

        // Render-target format and primitive topology.
        VkFormat              RTFormat          = VK_FORMAT_R8G8B8A8_UNORM;
        // How many colour attachments (SetRenderTargets), and each one's format
        // (UNDEFINED: RTFormat) -- a G-buffer beside a half-float velocity.
        uint32_t              ColorCount        = 1;
        VkFormat              ColorFormats[GDI_MAX_COLOR_TARGETS] = {};
        // The render target's depth attachment, UNDEFINED for none. Part of the
        // pipeline (dynamic rendering requires it to match the attachment).
        VkFormat              DepthFormat       = VK_FORMAT_UNDEFINED;
        VkPrimitiveTopology   Topology          = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    };

    /* Every field of two keys equal (not their bytes: a key has padding). */
    static bool KeysEqual(const PipelineKey& A, const PipelineKey& B);

    // -------------------------------------------------------------------
    // Result returned by GetOrCreateFull
    // -------------------------------------------------------------------
    struct PipelineFetch
    {
        VkPipeline            Pipeline    = VK_NULL_HANDLE;
        VkPipelineLayout      PipeLayout  = VK_NULL_HANDLE;
        VkDescriptorSetLayout DescLayout  = VK_NULL_HANDLE;
    };

    // -------------------------------------------------------------------
    // Helpers for callers to build a PipelineKey from current GDI state
    // -------------------------------------------------------------------

    static void ApplyBlendState(PipelineKey& Key,
                                VkBool32              BlendEnable,
                                VkBlendFactor         SrcColor,
                                VkBlendFactor         DstColor,
                                VkBlendFactor         SrcAlpha,
                                VkBlendFactor         DstAlpha,
                                VkColorComponentFlags WriteMask,
                                VkBlendOp             ColorOp = VK_BLEND_OP_ADD,
                                VkBlendOp             AlphaOp = VK_BLEND_OP_ADD);

    static void ApplyDepthStencilState(PipelineKey& Key,
                                       VkBool32     DepthTest,
                                       VkBool32     DepthWrite,
                                       VkBool32     StencilTest,
                                       VkCompareOp  DepthCompare = VK_COMPARE_OP_LESS);

    // -------------------------------------------------------------------
    // Life-cycle
    // -------------------------------------------------------------------

    VKPipelineCache()  = default;
    ~VKPipelineCache() { Shutdown(); }

    bool Initialize(VKDevice* InDevice);
    void Shutdown();

    // -------------------------------------------------------------------
    // Pipeline retrieval
    // -------------------------------------------------------------------

    /**
     * Return (or lazily create) the VkPipeline for Key together with the
     * pipeline layout and descriptor set layout for the key's group.
     * All three are VK_NULL_HANDLE if Key has null shader modules or creation fails.
     */
    PipelineFetch GetOrCreateFull(const PipelineKey& Key);

    // -------------------------------------------------------------------
    // Per-group layout accessors
    // -------------------------------------------------------------------

    inline VkDescriptorSetLayout GetDescSetLayout(EDescriptorLayoutGroup Group) const
    {
        return DescSetLayouts[Group];
    }
    inline VkPipelineLayout GetPipelineLayout(EDescriptorLayoutGroup Group) const
    {
        return PipelineLayouts[Group];
    }

    inline bool IsValid() const { return PipelineLayouts[DLG_Base] != VK_NULL_HANDLE; }

    // -------------------------------------------------------------------
    // Compute (GDIDispatchCompute)
    // -------------------------------------------------------------------

    /**
     * The compute pipeline for InModule with InStorageCount storage buffers
     * (bindings 1..InStorageCount) after its parameter block (binding 0),
     * with that layout; created on first use. All null if creation fails.
     */
    PipelineFetch GetOrCreateCompute(VkShaderModule InModule, uint32_t InStorageCount);

    /** InModule is being destroyed: its compute pipelines leave the cache, so
     *  a later module given the same handle never finds them. They are
     *  destroyed at Shutdown, past any frame that may still use them. */
    void ForgetComputeModule(VkShaderModule InModule);

private:
    void LoadFromDisk();
    void SaveToDisk() const;
    bool BuildLayouts();

    VkPipeline CreatePipeline(const PipelineKey& Key) const;

    static void GetVertexInputState(
        uint32_t                           LayoutID,
        VkVertexInputBindingDescription*   OutBindings,
        uint32_t&                          OutBindingCount,
        VkVertexInputAttributeDescription* OutAttribs,
        uint32_t&                          OutAttribCount);

    static uint64_t HashKey(const PipelineKey& Key);

    // -------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------
    VKDevice*   Device = nullptr;

    VkPipelineCache VKCache = VK_NULL_HANDLE;

    // One descriptor-set layout + one pipeline layout per group.
    VkDescriptorSetLayout DescSetLayouts [DLG_Count]{};
    VkPipelineLayout      PipelineLayouts[DLG_Count]{};

    struct PipelineEntry
    {
        VkPipeline             Pipeline = VK_NULL_HANDLE;
        EDescriptorLayoutGroup Group    = DLG_Base;
        PipelineKey            Key;      // compared in full: two keys can share a hash
    };
    /* By the key's hash; several where hashes collide. Read concurrently by
     * recording contexts (MULTITHREADED_RENDERING.md): EntriesLock shared for
     * a lookup, exclusive for an insert. */
    std::unordered_multimap<uint64_t, PipelineEntry> PipelineEntries;
    mutable std::shared_mutex                         EntriesLock;

    // Compute: one layout per storage-buffer count (0..GDI_COMPUTE_MAX_STORAGE_BUFFERS),
    // made on first use; pipelines by module and count.
    static constexpr uint32_t kComputeLayouts = GDI_COMPUTE_MAX_STORAGE_BUFFERS + 1;
    VkDescriptorSetLayout ComputeSetLayouts [kComputeLayouts]{};
    VkPipelineLayout      ComputePipeLayouts[kComputeLayouts]{};
    std::unordered_map<uint64_t, VkPipeline> ComputePipelines;
    std::unordered_map<uint64_t, VkShaderModule> ComputeModules;   // each pipeline's module, by the same key
    std::vector<VkPipeline> RetiredComputePipelines;
};

#endif //CORECLIENT_VKPIPELINECACHE_H

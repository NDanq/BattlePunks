#ifndef CORECLIENT_QMTLPIPELINECACHE_H
#define CORECLIENT_QMTLPIPELINECACHE_H

// Backend-internal: the state keys below hold Metal enum types, so this header is
// only includable from Objective-C++ translation units.
#ifndef __OBJC__
#error "QMTLPipelineCache.h is Objective-C++ only; include it from a .mm translation unit."
#endif

#import <Metal/Metal.h>


class QMTLDevice;

/**
 * Buffer-index conventions for the Metal backend.
 *
 * Metal gives the vertex stage ONE 31-slot buffer index space shared by uniform
 * buffers and vertex-attribute buffers, so the two have to be kept apart by
 * convention:
 *
 *   0..2   uniform buffers. These indices are not ours to choose -- spirv-cross
 *          was run with --msl-decoration-binding, so the MSL `buffer(N)`
 *          qualifiers equal the original SPIR-V binding numbers, and the shaders
 *          declare ViewParameters at 0, Transform at 1, and the per-shader custom
 *          UBO at 2 (see Shaders/VulkanCommon.glsl).
 *   26..30 vertex-attribute buffers, allocated downward from the top of the range
 *          so they can never collide with a future uniform binding.
 *
 * Textures and samplers live in their OWN index spaces in Metal, so a sampler at
 * index 2 or 3 does not clash with the UBO at buffer 2.
 */
namespace MTLBindings
{
    constexpr uint32_t ViewParamsBuffer = 0;
    constexpr uint32_t TransformBuffer  = 1;
    constexpr uint32_t CustomUBOBuffer  = 2;

    constexpr uint32_t PositionBuffer   = 30;
    constexpr uint32_t TexCoordBuffer   = 29;
    constexpr uint32_t InstanceBuffer   = 28;
    constexpr uint32_t OpacityBuffer    = 27;
    constexpr uint32_t ColorBuffer      = 26;
}

/** Vertex layout IDs, mirroring QVKPipelineCache's VertexLayoutID. */
enum EMTLVertexLayout : uint32_t
{
    MVL_Standard  = 0,   ///< position (float3) + texcoord (float2)
    MVL_Instanced = 1,   ///< + per-instance mat4 transform, opacity, color
};

/**
 * In-process render-pipeline / depth-stencil state cache -- the Metal counterpart
 * of QVKPipelineCache.
 *
 * Two differences from the Vulkan version worth knowing:
 *
 *  - There is no descriptor-layout group. Metal binds buffers, textures and
 *    samplers straight onto the encoder by index, so the four
 *    EDescriptorLayoutGroup permutations the Vulkan backend needs to pre-declare
 *    collapse to nothing here.
 *  - There is no on-disk blob. Vulkan persists a VkPipelineCache to AppData;
 *    Metal already caches compiled pipelines in the system shader cache, so the
 *    only cache we maintain is this per-process hash map. (MTLBinaryArchive is
 *    the explicit-persistence option if profiling ever justifies it.)
 */
class QMTLPipelineCache
{
public:
    // -------------------------------------------------------------------
    // Pipeline state key
    // -------------------------------------------------------------------
    struct PipelineKey
    {
        // Shader functions (compared by pointer identity, like VkShaderModule).
        void*             VertexFunction   = nullptr;
        void*             FragmentFunction = nullptr;

        uint32_t          VertexLayoutID   = MVL_Standard;

        // Blend state.
        bool              BlendEnable      = false;
        MTLBlendFactor    SrcColorBlend    = MTLBlendFactorOne;
        MTLBlendFactor    DstColorBlend    = MTLBlendFactorZero;
        MTLBlendFactor    SrcAlphaBlend    = MTLBlendFactorOne;
        MTLBlendFactor    DstAlphaBlend    = MTLBlendFactorZero;
        MTLBlendOperation ColorBlendOp     = MTLBlendOperationAdd;
        MTLBlendOperation AlphaBlendOp     = MTLBlendOperationAdd;
        MTLColorWriteMask ColorWriteMask   = MTLColorWriteMaskAll;

        // Render-target format.
        MTLPixelFormat    RTFormat         = MTLPixelFormatRGBA8Unorm;
    };

    /** Depth/stencil state key -- Metal caches this as its own object. */
    struct DepthStencilKey
    {
        bool                DepthTest      = false;
        bool                DepthWrite     = false;
        MTLCompareFunction  DepthCompare   = MTLCompareFunctionLess;

        bool                StencilTest    = false;
        MTLCompareFunction  StencilCompare = MTLCompareFunctionLess;
        MTLStencilOperation StencilFail    = MTLStencilOperationKeep;
        MTLStencilOperation DepthFail      = MTLStencilOperationKeep;
        MTLStencilOperation DepthStencilPass = MTLStencilOperationKeep;
        uint32_t            StencilMask    = 1;
    };

    QMTLPipelineCache() = default;
    ~QMTLPipelineCache() { Shutdown(); }

    bool Initialize(QMTLDevice* InDevice);
    void Shutdown();

    /** Return (or lazily create) the pipeline state for Key; nil on failure. */
    id<MTLRenderPipelineState> GetOrCreatePipeline(const PipelineKey& Key);

    /** Return (or lazily create) the depth/stencil state for Key; nil on failure. */
    id<MTLDepthStencilState> GetOrCreateDepthStencil(const DepthStencilKey& Key);

    /** Build the MTLVertexDescriptor for a vertex layout ID (autoreleased). */
    static MTLVertexDescriptor* BuildVertexDescriptor(uint32_t LayoutID);

    bool IsValid() const { return Device != nullptr; }

private:
    static uint64_t HashPipelineKey(const PipelineKey& Key);
    static uint64_t HashDepthStencilKey(const DepthStencilKey& Key);

    id<MTLRenderPipelineState> CreatePipeline(const PipelineKey& Key) const;

    QMTLDevice* Device = nullptr;

    std::unordered_map<uint64_t, void*> Pipelines;          ///< id<MTLRenderPipelineState>, retained
    std::unordered_map<uint64_t, void*> DepthStencilStates; ///< id<MTLDepthStencilState>, retained
};

#endif //CORECLIENT_QMTLPIPELINECACHE_H

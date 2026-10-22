#ifndef CORECLIENT_D3D12PIPELINECACHE_H
#define CORECLIENT_D3D12PIPELINECACHE_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "Public/GDI/GDIDefinitions.h"

class D3D12Device;
class D3D12ShaderResource;

/**
 * Root parameters of the one root signature every GDI draw uses.
 *
 * Where Vulkan needs a descriptor layout per shader family, a D3D12 root
 * signature may be a superset of what a shader declares, so one covers all of
 * them: the core cbuffers at b0/b1, the custom cbuffer at b2, and the sampled
 * texture at t2 (Screen/Cursor) or t3 (UberBrush/LayersBlit). The material
 * layout (GDIDefinitions.h) adds the scene cbuffer at b3 and one table each
 * for t4-t11 and s4-s11; b2 is then the material block.
 */
enum ED3D12RootParam : UINT
{
    RP_ViewParameters = 0,
    RP_Transform,
    RP_Custom,
    RP_Texture2,
    RP_Texture3,
    RP_Sampler2,
    RP_Sampler3,
    RP_Scene,
    RP_MaterialTexture0,
    RP_MaterialSampler0 = RP_MaterialTexture0 + 8,   // GDI_MATERIAL_TEXTURE_SLOTS
    RP_Skin             = RP_MaterialSampler0 + 8,   // b12, GDI_SKIN_BLOCK_BINDING
    RP_PrevSkin,                                     // b13, GDI_PREV_SKIN_BLOCK_BINDING
    RP_Count
};

/**
 * Root parameters of the compute root signature (GDIDispatchCompute): the
 * parameter block at b0, then one root UAV per storage buffer at u1-u4 --
 * spirv-cross makes each std430 buffer a RWByteAddressBuffer at its binding.
 */
enum ED3D12ComputeRootParam : UINT
{
    CRP_Params = 0,
    CRP_Storage0,
    CRP_Count = CRP_Storage0 + 4   // GDI_COMPUTE_MAX_STORAGE_BUFFERS
};

/**
 * In-process PSO cache keyed on shaders, vertex layout, blend state and RT
 * format. Pipelines are created lazily on the first draw that needs one.
 *
 * Vertex layout IDs:
 *   0 -- standard:  slot 0 float3 position, slot 1 float2 texcoord
 *   1 -- instanced: plus slot 2 float4x4 transform, slot 3 float opacity, slot 4 float3 color
 *   2 -- indexed mesh: plus slot 2 float3 normal, slot 3 float4 colour, slot 4 float2 UV1,
 *        slot 8 float4 tangent (location 13; in 3 and 4 too)
 *   3 -- indexed mesh instanced from a GDIBuffer (GDISetInstanceBuffer): plus
 *        slot 5, one GDI_INSTANCE_STRIDE record per instance
 *   4 -- skinned mesh: 2 plus slot 6 float4 bone indices, slot 7 float4 weights
 *        (locations 11 and 12)
 */
class D3D12PipelineCache
{
public:
    struct PipelineKey
    {
        uint64_t                VertexShaderID  = 0;
        uint64_t                PixelShaderID   = 0;
        uint32_t                VertexLayoutID  = 0;

        BOOL                    BlendEnable     = FALSE;
        D3D12_BLEND             SrcBlend        = D3D12_BLEND_ONE;
        D3D12_BLEND             DestBlend       = D3D12_BLEND_ZERO;
        D3D12_BLEND_OP          BlendOp         = D3D12_BLEND_OP_ADD;
        D3D12_BLEND_OP          BlendOpAlpha    = D3D12_BLEND_OP_ADD;
        UINT8                   WriteMask       = D3D12_COLOR_WRITE_ENABLE_ALL;

        DXGI_FORMAT             RTFormat        = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT                    ColorCount      = 1;   // render targets
        DXGI_FORMAT             ColorFormats[GDI_MAX_COLOR_TARGETS] = {};   // each one's (UNKNOWN: RTFormat)

        // The render target's depth buffer (UNKNOWN for none) and its test.
        DXGI_FORMAT             DSVFormat       = DXGI_FORMAT_UNKNOWN;
        BOOL                    DepthEnable     = FALSE;
        BOOL                    DepthWrite      = FALSE;
        D3D12_COMPARISON_FUNC   DepthFunc       = D3D12_COMPARISON_FUNC_LESS;

        bool operator==(const PipelineKey& InOther) const;
    };

    D3D12PipelineCache()  = default;
    ~D3D12PipelineCache() { Shutdown(); }

    bool Initialize(D3D12Device* InDevice);
    void Shutdown();

    /** Null if either shader is missing bytecode or PSO creation fails. Failures are cached too.
     *  Any thread: a parallel pass's recording contexts look pipelines up at once (shared lock). */
    ID3D12PipelineState* GetOrCreate(const PipelineKey& Key, const D3D12ShaderResource* VS, const D3D12ShaderResource* PS);

    /** InCS's compute pipeline, made on first use; null (and cached) if creation fails. */
    ID3D12PipelineState* GetOrCreateCompute(const D3D12ShaderResource* InCS);

    inline ID3D12RootSignature*    GetRootSignature()        const { return RootSignature.Get(); }
    inline ID3D12RootSignature*    GetComputeRootSignature() const { return ComputeRootSignature.Get(); }
    /** ExecuteIndirect's signature for one D3D12_DRAW_INDEXED_ARGUMENTS (GDI_INDIRECT_ARGS_STRIDE). */
    inline ID3D12CommandSignature* GetDrawIndexedSignature() const { return DrawIndexedSignature.Get(); }
    inline bool                 IsValid()          const { return RootSignature != nullptr; }

private:
    bool BuildRootSignature();
    bool BuildComputeRootSignature();
    bool BuildDrawIndexedSignature();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreatePipeline(const PipelineKey& Key,
                                                               const D3D12ShaderResource* VS,
                                                               const D3D12ShaderResource* PS) const;
    static uint64_t HashKey(const PipelineKey& Key);

    D3D12Device*                                    Device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>     RootSignature;
    /* By the key's hash, each entry matched on its whole key: a hash alone can collide. */
    struct PipelineEntry
    {
        PipelineKey                                 Key;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
    };
    std::unordered_multimap<uint64_t, PipelineEntry> Pipelines;
    mutable std::shared_mutex                        PipelinesLock;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>     ComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature>  DrawIndexedSignature;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> ComputePipelines;   // by shader ID
};

#endif //CORECLIENT_D3D12PIPELINECACHE_H

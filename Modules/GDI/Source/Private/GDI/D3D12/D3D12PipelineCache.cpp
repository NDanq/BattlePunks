#include "Public/GDI/D3D12/D3D12PipelineCache.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12Shader.h"

#include <climits>
#include <string>

#include "Public/GDI/GDILog.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t kLayoutInstanced = 1;
    constexpr uint32_t kLayoutMesh      = 2;

    // spirv-cross emits TEXCOORD<location> for every stage input, matrices
    // flattened one column per location (--hlsl-flatten-matrix-vertex-input-semantics).
    const D3D12_INPUT_ELEMENT_DESC GStandardLayout[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // An indexed mesh (GDICreateMesh): the normal at location 8, slot 2.
    const D3D12_INPUT_ELEMENT_DESC GMeshLayout[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 8, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    const D3D12_INPUT_ELEMENT_DESC GInstancedLayout[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       1, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32_FLOAT,          3, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 7, DXGI_FORMAT_R32G32B32_FLOAT,    4, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    // *_COLOR factors are illegal in the alpha blend slots; the alpha channel of
    // the colour factor is the same quantity.
    D3D12_BLEND ToAlphaBlend(D3D12_BLEND InBlend)
    {
        switch(InBlend)
        {
            case D3D12_BLEND_SRC_COLOR:      return D3D12_BLEND_SRC_ALPHA;
            case D3D12_BLEND_INV_SRC_COLOR:  return D3D12_BLEND_INV_SRC_ALPHA;
            case D3D12_BLEND_DEST_COLOR:     return D3D12_BLEND_DEST_ALPHA;
            case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
            default:                         return InBlend;
        }
    }
}

bool D3D12PipelineCache::Initialize(D3D12Device* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid()) return false;
    return BuildRootSignature();
}

void D3D12PipelineCache::Shutdown()
{
    Pipelines.clear();
    RootSignature.Reset();
    Device = nullptr;
}

bool D3D12PipelineCache::BuildRootSignature()
{
    D3D12_ROOT_PARAMETER Params[RP_Count]{};
    D3D12_DESCRIPTOR_RANGE Ranges[4]{};

    for(UINT CB = 0; CB < 3; ++CB)
    {
        Params[RP_ViewParameters + CB].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[RP_ViewParameters + CB].Descriptor.ShaderRegister = CB;
        Params[RP_ViewParameters + CB].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    const struct { ED3D12RootParam Param; D3D12_DESCRIPTOR_RANGE_TYPE Type; UINT Register; } Tables[4] = {
        { RP_Texture2, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,     2 },
        { RP_Texture3, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,     3 },
        { RP_Sampler2, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2 },
        { RP_Sampler3, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 3 },
    };
    for(int i = 0; i < 4; ++i)
    {
        Ranges[i].RangeType          = Tables[i].Type;
        Ranges[i].NumDescriptors     = 1;
        Ranges[i].BaseShaderRegister = Tables[i].Register;

        D3D12_ROOT_PARAMETER& P                 = Params[Tables[i].Param];
        P.ParameterType                         = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        P.DescriptorTable.NumDescriptorRanges   = 1;
        P.DescriptorTable.pDescriptorRanges     = &Ranges[i];
        P.ShaderVisibility                      = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC Desc{};
    Desc.NumParameters = RP_Count;
    Desc.pParameters   = Params;
    Desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> Blob, Error;
    if(FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Error)))
    {
        GDI_LOG_ERROR(std::string("[D3D12PipelineCache] Root signature serialization failed: ")
                      + (Error ? static_cast<const char*>(Error->GetBufferPointer()) : ""))
        return false;
    }

    if(FAILED(Device->GetDevice()->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                                       IID_PPV_ARGS(&RootSignature))))
    {
        GDI_LOG_ERROR("[D3D12PipelineCache] CreateRootSignature failed.")
        return false;
    }
    return true;
}

uint64_t D3D12PipelineCache::HashKey(const PipelineKey& K)
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

    Mix(&K.VertexShaderID, sizeof(K.VertexShaderID));
    Mix(&K.PixelShaderID,  sizeof(K.PixelShaderID));
    Mix(&K.VertexLayoutID, sizeof(K.VertexLayoutID));
    Mix(&K.BlendEnable,    sizeof(K.BlendEnable));
    Mix(&K.SrcBlend,       sizeof(K.SrcBlend));
    Mix(&K.DestBlend,      sizeof(K.DestBlend));
    Mix(&K.BlendOp,        sizeof(K.BlendOp));
    Mix(&K.BlendOpAlpha,   sizeof(K.BlendOpAlpha));
    Mix(&K.WriteMask,      sizeof(K.WriteMask));
    Mix(&K.RTFormat,       sizeof(K.RTFormat));
    Mix(&K.DSVFormat,      sizeof(K.DSVFormat));
    Mix(&K.DepthEnable,    sizeof(K.DepthEnable));
    Mix(&K.DepthWrite,     sizeof(K.DepthWrite));
    Mix(&K.DepthFunc,      sizeof(K.DepthFunc));
    return H;
}

ID3D12PipelineState* D3D12PipelineCache::GetOrCreate(const PipelineKey& Key,
                                                     const D3D12ShaderResource* VS,
                                                     const D3D12ShaderResource* PS)
{
    if(!VS || !PS || !VS->IsValid() || !PS->IsValid() || !RootSignature) return nullptr;

    const uint64_t H = HashKey(Key);
    auto It = Pipelines.find(H);
    if(It != Pipelines.end())
    {
        return It->second.Get();
    }

    ComPtr<ID3D12PipelineState> New = CreatePipeline(Key, VS, PS);
    ID3D12PipelineState* Result = New.Get();
    Pipelines.emplace(H, std::move(New));
    return Result;
}

ComPtr<ID3D12PipelineState> D3D12PipelineCache::CreatePipeline(const PipelineKey& Key,
                                                               const D3D12ShaderResource* VS,
                                                               const D3D12ShaderResource* PS) const
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
    Desc.pRootSignature        = RootSignature.Get();
    Desc.VS                    = { VS->GetBytecode().data(), VS->GetBytecode().size() };
    Desc.PS                    = { PS->GetBytecode().data(), PS->GetBytecode().size() };

    if(Key.VertexLayoutID == kLayoutMesh)
    {
        Desc.InputLayout = { GMeshLayout, UINT(sizeof(GMeshLayout) / sizeof(GMeshLayout[0])) };
    }
    else if(Key.VertexLayoutID == kLayoutInstanced)
    {
        Desc.InputLayout = { GInstancedLayout, UINT(sizeof(GInstancedLayout) / sizeof(GInstancedLayout[0])) };
    }
    else
    {
        Desc.InputLayout = { GStandardLayout, UINT(sizeof(GStandardLayout) / sizeof(GStandardLayout[0])) };
    }

    D3D12_RENDER_TARGET_BLEND_DESC& Blend = Desc.BlendState.RenderTarget[0];
    Blend.BlendEnable           = Key.BlendEnable;
    Blend.SrcBlend              = Key.SrcBlend;
    Blend.DestBlend             = Key.DestBlend;
    Blend.BlendOp               = Key.BlendOp;
    Blend.SrcBlendAlpha         = ToAlphaBlend(Key.SrcBlend);
    Blend.DestBlendAlpha        = ToAlphaBlend(Key.DestBlend);
    Blend.BlendOpAlpha          = Key.BlendOpAlpha;
    Blend.LogicOp               = D3D12_LOGIC_OP_NOOP;
    Blend.RenderTargetWriteMask = Key.WriteMask;

    Desc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    Desc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    Desc.RasterizerState.DepthClipEnable = TRUE;

    // No depth attachment exists yet on any backend.
    // A depth test needs a depth buffer to test against; without one the
    // PSO must not enable it.
    const bool bDepth = Key.DSVFormat != DXGI_FORMAT_UNKNOWN && Key.DepthEnable;
    Desc.DepthStencilState.DepthEnable    = bDepth ? TRUE : FALSE;
    Desc.DepthStencilState.DepthWriteMask = (bDepth && Key.DepthWrite) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    Desc.DepthStencilState.DepthFunc      = Key.DepthFunc;
    Desc.DepthStencilState.StencilEnable  = FALSE;

    Desc.SampleMask            = UINT_MAX;
    Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Desc.NumRenderTargets      = 1;
    Desc.RTVFormats[0]         = Key.RTFormat;
    Desc.DSVFormat             = Key.DSVFormat;
    Desc.SampleDesc.Count      = 1;

    ComPtr<ID3D12PipelineState> Pipeline;
    const HRESULT Result = Device->GetDevice()->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&Pipeline));
    if(FAILED(Result))
    {
        GDI_LOG_ERROR("[D3D12PipelineCache] CreateGraphicsPipelineState failed (" + GDIHex(uint32_t(Result)) + ").")
    }
    return Pipeline;
}

#include "Public/GDI/D3D12/D3D12Shader.h"
#include "Public/GDI/GDITexture.h"
#include "Public/GDI/GDILog.h"

#include "Public/Core/CoreFileProvider.h"

#include <atomic>

namespace
{
    std::atomic<uint64_t> GNextShaderID{1};
}

D3D12ShaderResource::D3D12ShaderResource(EShaderType InType, const std::string& CsoPath)
    : GDIShaderResource(InType, CsoPath)
    , ID(GNextShaderID.fetch_add(1))
{
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(Provider == nullptr)
    {
        GDI_LOG("[D3D12ShaderResource] No file provider installed for: " + CsoPath)
        return;
    }

    if(!Provider->ReadAll(CsoPath, Bytecode) || Bytecode.empty())
    {
        Bytecode.clear();
        GDI_LOG("[D3D12ShaderResource] Failed to read shader bytecode: " + CsoPath)
        return;
    }

    bInitialized = true;
    GDI_LOG("[D3D12ShaderResource] Loaded shader: " + CsoPath)
}

void D3D12ShaderResource::SetUniform(std::string InName, const int InValue)
{
    D3D12UniformValue V;
    V.Tag    = D3D12UniformValue::INT;
    V.Data.i = InValue;
    Scalars[InName] = V;
}

void D3D12ShaderResource::SetUniform(std::string InName, const float InValue)
{
    D3D12UniformValue V;
    V.Tag    = D3D12UniformValue::FLOAT;
    V.Data.f = InValue;
    Scalars[InName] = V;
}

void D3D12ShaderResource::SetUniform(std::string InName, const Vector2 InVector)
{
    D3D12UniformValue V;
    V.Tag       = D3D12UniformValue::VEC2;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    Scalars[InName] = V;
}

void D3D12ShaderResource::SetUniform(std::string InName, const Vector3 InVector)
{
    D3D12UniformValue V;
    V.Tag       = D3D12UniformValue::VEC3;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    V.Data.v[2] = InVector.GetZ();
    Scalars[InName] = V;
}

void D3D12ShaderResource::SetUniform(std::string InName, const Vector4 InVector)
{
    D3D12UniformValue V;
    V.Tag       = D3D12UniformValue::VEC4;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    V.Data.v[2] = InVector.GetZ();
    V.Data.v[3] = InVector.GetW();
    Scalars[InName] = V;
}

void D3D12ShaderResource::SetUniform(std::string InName, const Matrix4x4 InMatrix)
{
    // Matrix4x4 is row-major; the cooked HLSL keeps SPIR-V's column-major
    // layout, so transpose exactly as the Vulkan backend does.
    D3D12UniformValue V;
    V.Tag = D3D12UniformValue::MAT;
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            V.Data.v[Col * 4 + Row] = static_cast<float>(InMatrix.Data[Row][Col]);
    Scalars[InName] = V;
}

void D3D12ShaderResource::SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex)
{
    if(!InTexture2D) return;
    SetUniform(InName, InTexture2D->Resource, InIndex);
}

void D3D12ShaderResource::SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex)
{
    if(!InTexResource) return;
    D3D12TextureBinding Binding;
    Binding.Resource = InTexResource;
    Binding.Index    = InIndex;
    Textures[InName] = Binding;
}

void D3D12ShaderResource::Release(void)
{
    Scalars.clear();
    Textures.clear();
    Bytecode.clear();
    bInitialized = false;
}

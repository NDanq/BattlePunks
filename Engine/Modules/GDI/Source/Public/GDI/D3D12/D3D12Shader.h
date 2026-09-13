#ifndef CORECLIENT_D3D12SHADER_H
#define CORECLIENT_D3D12SHADER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/GDIShader.h"

/** One staged scalar uniform, already in the float layout the constant buffers want. */
struct D3D12UniformValue
{
    enum Tag : uint8_t { INT, FLOAT, VEC2, VEC3, VEC4, MAT } Tag = INT;

    union
    {
        int   i;
        float f;
        float v[16];
    } Data{};
};

struct D3D12TextureBinding
{
    GDITextureResource* Resource = nullptr;
    uint                Index    = 0;
};

/**
 * Cooked shader bytecode (SPIR-V transpiled to HLSL, compiled by fxc) plus the
 * uniforms staged against it. D3D12DynamicGDI::BindDrawState packs the
 * scalars into constant buffers and binds the textures at draw time.
 */
class D3D12ShaderResource : public GDIShaderResource
{
public:
    D3D12ShaderResource(EShaderType InType, const std::string& CsoPath);

    void SetUniform(std::string InName, const int InValue) override;
    void SetUniform(std::string InName, const float InValue) override;
    void SetUniform(std::string InName, const Vector2 InVector) override;
    void SetUniform(std::string InName, const Vector3 InVector) override;
    void SetUniform(std::string InName, const Vector4 InVector) override;
    void SetUniform(std::string InName, const Matrix4x4 InMatrix) override;
    void SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex) override;
    void SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex) override;

    void Release(void) override;

    inline bool                        IsValid()     const { return !Bytecode.empty(); }
    inline const std::vector<uint8_t>& GetBytecode() const { return Bytecode; }

    /** Process-unique, never reused -- the pipeline cache keys on it instead of an address. */
    inline uint64_t                    GetID()       const { return ID; }

    inline const std::unordered_map<std::string, D3D12UniformValue>&   GetScalars()  const { return Scalars; }
    inline const std::unordered_map<std::string, D3D12TextureBinding>& GetTextures() const { return Textures; }

private:
    std::vector<uint8_t> Bytecode;
    uint64_t             ID = 0;

    std::unordered_map<std::string, D3D12UniformValue>   Scalars;
    std::unordered_map<std::string, D3D12TextureBinding> Textures;
};

#endif //CORECLIENT_D3D12SHADER_H

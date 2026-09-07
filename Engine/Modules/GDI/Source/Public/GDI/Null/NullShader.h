#ifndef CORECLIENT_NULLSHADER_H
#define CORECLIENT_NULLSHADER_H

#include "Public/GDI/GDIShader.h"

/*
 * Never reads SrcCodePath -- that is what lets GDIGetShader<>() succeed in a
 * process with no EngineCore::IFileProvider installed at all. Every backend
 * would normally hand SrcCodePath (already resolved to a cooked path by
 * ProcessShaderFromSource) to the driver; Null skips that call entirely
 * (see NullDynamicGDI::CreateShaderResource).
 */
class NullShaderResource : public GDIShaderResource
{
public:
    NullShaderResource() = default;

    NullShaderResource(EShaderType InShaderType, const std::string& SrcCodePath) :
        GDIShaderResource(InShaderType, SrcCodePath)
    {
    }

    void SetUniform(std::string InName, const int InValue) override {}
    void SetUniform(std::string InName, const float InValue) override {}
    void SetUniform(std::string InName, const Vector2 InVector) override {}
    void SetUniform(std::string InName, const Vector3 InVector) override {}
    void SetUniform(std::string InName, const Vector4 InVector) override {}
    void SetUniform(std::string InName, const Matrix4x4 InMatrix) override {}
    void SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex) override {}
    void SetUniform(std::string UniformName, GDITextureResource* InTextureResource, const uint InIdx) override {}

    void Release(void) override {}
};

#endif //CORECLIENT_NULLSHADER_H

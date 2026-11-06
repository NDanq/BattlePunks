#ifndef CORECLIENT_GDISHADER_H
#define CORECLIENT_GDISHADER_H


#include "Public/Math/Vector.h"
#include "Public/Math/Matrix.h"
#include "GDIDefinitions.h"
#include "GDITexture.h"
#include <string>
#include <unordered_map>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class GDIShaderResource
{
public:
    GDIShaderResource() = default;
    GDIShaderResource(EShaderType InShaderType, const std::string& SrcCodePath) :
        bInitialized(false)
    {
        ShaderType = InShaderType;
    }
    [[nodiscard]]
    inline EShaderType GetShaderType() const { return ShaderType; }

    virtual void SetUniform(std::string InName, const int InValue) = 0;
    virtual void SetUniform(std::string InName, const float InValue) = 0;
    virtual void SetUniform(std::string InName, const Vector2 InVector) = 0;
    virtual void SetUniform(std::string InName, const Vector3 InVector) = 0;
    virtual void SetUniform(std::string InName, const Vector4 InVector) = 0;
    virtual void SetUniform(std::string InName, const Matrix4x4 InMatrix) = 0;
    virtual void SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex)  = 0;
    virtual void SetUniform(std::string UniformName, GDITextureResource* InTextureResource, const uint InIdx) = 0;

    virtual void Release(void) = 0;

    /* Selects the material binding layout (GDIDefinitions.h) for any draw
     * this shader is bound to. Set once, after creation. */
    void SetMaterialLayout(const bool bInMaterialLayout) { bMaterialLayout = bInMaterialLayout; }
    [[nodiscard]] bool UsesMaterialLayout() const { return bMaterialLayout; }
protected:
    bool        bInitialized=false;
    bool        bMaterialLayout=false;
    EShaderType ShaderType = EShaderType::ST_MAX;
};



class GDIShader{
public:
    struct GDIUniformData
    {
        union DataType
        {
            DataType(){ }
            int _i;
            float _f;
            Vector2 _v2;
            Vector3 _v3;
            Vector4 _v4;
            Matrix4x4 _m;
            GDITexture2D* _t;
            GDITextureResource* _tr;
        };

        enum DataFrequency : char
        {
            INT = 0,
            FLOAT,
            VEC2,
            VEC3,
            VEC4,
            MAT,
            TEX,
            TEXRES
        } DataID;

        DataType Data;
        uint TexIndex;
    };

    GDIShader()
    {
        Resource = nullptr;
    }

    GDIShader(std::string InName) : Name(InName)
    {
        Resource = nullptr;
    }

    ~GDIShader()
    {
        CleanupAllocatedUniformData();
        if(Resource)
        {
            Resource->Release();
        }
        delete Resource;
    }

    void InitResource(EShaderType InShaderType, const std::string& SrcCodePath);

    /**
     * Call "RegisterUniforms" in this function for any user defined uniforms
     */
    virtual void InitUniformBindings() = 0;
    void RegisterUniform(std::string UniformName, int InData);
    void RegisterUniform(std::string UniformName, float InData);
    void RegisterUniform(std::string UniformName, Vector2 InData);
    void RegisterUniform(std::string UniformName, Vector3 InData);
    void RegisterUniform(std::string UniformName, Vector4 InData);
    void RegisterUniform(std::string UniformName, Matrix4x4 InData);
    void RegisterTexUniform(std::string UniformName, GDITexture2D* InTexture, const uint InIndex);
    void RegisterTexResUniform(std::string UniformName, GDITextureResource* InTexture, const uint InIndex);

    void SetUniform(std::string UniformName, int InData);
    void SetUniform(std::string UniformName, float InData);
    void SetUniform(std::string UniformName, Vector2 InData);
    void SetUniform(std::string UniformName, Vector3 InData);
    void SetUniform(std::string UniformName, Vector4 InData);
    void SetUniform(std::string UniformName, Matrix4x4 InData);
    void SetUniform(std::string UniformName, GDITexture2D* InTexture, const uint InIdx = 0);
    void SetUniform(std::string UniformName, GDITextureResource* InTextureResource, const uint InIdx = 0);

    void UpdateCoreUniforms( class GDIView* InView, class GDITransform* Transform);
    void UpdateUserDefinedUniforms() const;
    [[nodiscard]]
    inline EShaderType GetShaderType() const
    {
        if( Resource )
        {
            return Resource->GetShaderType();
        }
        return EShaderType::ST_MAX;
    }

    GDIShaderResource* Resource;
    std::unordered_map<std::string, GDIUniformData> Uniforms;
private:
    void CleanupAllocatedUniformData();
    std::string Name;
};

class GDIShaderData
{
public:
    GDIShaderData() = default;

    GDIShaderData( std::string InSrc, EShaderType InType) :
        ShaderPath(InSrc),
        ShaderType(InType)
    {

    }
    std::string ShaderPath;
    EShaderType ShaderType = EShaderType::ST_MAX;
};

std::string ProcessShaderFromSource( const std::string& SrcCodePath, ERenderPlatform InRenderPlatform );

#endif //CONT_GDISHADER_H

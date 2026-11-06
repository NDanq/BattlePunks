#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDI.h"


#include "Public/GDI/GDIView.h"
#include "Public/GDI/GDILog.h"

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreString.h"

#include <string>
#include <vector>

/*
 * Cooked-resource paths are now PROVIDER-RELATIVE: no ":" prefix. Under Qt the
 * QrcFileProvider prepends it; a standalone host resolves the same relative
 * path off disk. That is the whole point of the seam -- the path no longer
 * encodes which delivery mechanism is in use.
 */
std::string GetCookedShaderPath(const std::string& File, ERenderPlatform Platform)
{
    switch (Platform)
    {
        case ERP_OpenGL:
            return std::string("Shaders/Cooked/OpenGL/") + File;
        case ERP_Vulkan:
            return std::string("Shaders/Cooked/Vulkan/") + File + ".spv";
        case ERP_Metal:
            return std::string("Shaders/Cooked/Metal/") + File + ".metal";
        case ERP_D3D12:
            return std::string("Shaders/Cooked/D3D12/") + File + ".cso";
        default:
            break;
    }

    return "";
}

void GDIShader::InitResource(EShaderType InShaderType, const std::string& SrcCode)
{
    Resource = GDICreateShaderResource( InShaderType, SrcCode );
}

void GDIShader::UpdateCoreUniforms(class GDIView *InView, class GDITransform *Transform)
{
    if( Resource == nullptr ) return;

    if(InView)
    {
        Resource->SetUniform("ViewParameters.Projection", InView->Camera->Projection);
        Resource->SetUniform("ViewParameters.View", InView->Camera->ViewMat);
        Resource->SetUniform("ViewParameters.MouseCoords", Vector3(float(InView->MouseState.PosX),
                                                                   float(InView->MouseState.PosY),
                                                                   (InView->MouseState.bMouseDown) ? 1.0f : 0.0f));
        Resource->SetUniform("ViewParameters.Resolution", Vector4(float(InView->Viewport.ViewportWidth),
                                                                  float(InView->Viewport.ViewportHeight),
                                                                  1.0f / InView->Viewport.ViewportWidth,
                                                                  1.0f / InView->Viewport.ViewportHeight));
    }

    if(Transform)
    {
        const Matrix4x4 Model = Transform->GetModel();
        Resource->SetUniform("Transform.Model", Model);
        Resource->SetUniform("Transform.PrevModel", Transform->bHasPrevModel ? Transform->PrevModel : Model);
        Resource->SetUniform("Transform.PositionWorldSpace", Transform->Translate);
    }

    UpdateUserDefinedUniforms();
}

void GDIShader::RegisterUniform(std::string UniformName, int InData)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::INT;
    UniformData.Data._i = InData;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterUniform(std::string UniformName, float InData)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::FLOAT;
    UniformData.Data._f = InData;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterUniform(std::string UniformName, Vector2 InData)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::VEC2;
    UniformData.Data._v2 = InData;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterUniform(std::string UniformName, Vector3 InData)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::VEC3;
    UniformData.Data._v3 = InData;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterUniform(std::string UniformName, Vector4 InData)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::VEC4;
    UniformData.Data._v4 = InData;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterUniform(std::string UniformName, Matrix4x4 InData) {
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::MAT;
    UniformData.Data._m = InData;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterTexUniform(std::string UniformName, GDITexture2D *InTexture, const uint InIndex)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::TEX;
    UniformData.Data._t = InTexture;
    UniformData.TexIndex = InIndex;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::RegisterTexResUniform(std::string UniformName, GDITextureResource *InTexture, const uint InIndex)
{
    GDIUniformData UniformData;
    UniformData.DataID = GDIUniformData::TEXRES;
    UniformData.Data._tr = InTexture;
    UniformData.TexIndex = InIndex;
    Uniforms[UniformName] = UniformData;
}

void GDIShader::SetUniform(std::string UniformName, int InData) {
    Uniforms[UniformName].Data._i = InData;
}

void GDIShader::SetUniform(std::string UniformName, float InData) {
    Uniforms[UniformName].Data._f = InData;
}

void GDIShader::SetUniform(std::string UniformName, Vector2 InData) {
    Uniforms[UniformName].Data._v2 = InData;
}

void GDIShader::SetUniform(std::string UniformName, Vector3 InData) {
    Uniforms[UniformName].Data._v3 = InData;
}

void GDIShader::SetUniform(std::string UniformName, Vector4 InData) {
    Uniforms[UniformName].Data._v4 = InData;
}

void GDIShader::SetUniform(std::string UniformName, Matrix4x4 InData) {
    Uniforms[UniformName].Data._m = InData;
}

void GDIShader::SetUniform(std::string UniformName, GDITexture2D* InTexture, const uint InIdx)
{
    Uniforms[UniformName].Data._t = InTexture;
    Uniforms[UniformName].TexIndex = InIdx;
}

void GDIShader::SetUniform(std::string UniformName, GDITextureResource *InTextureResource, const uint InIdx)
{
    Uniforms[UniformName].Data._tr = InTextureResource;
    Uniforms[UniformName].TexIndex = InIdx;
}

void GDIShader::UpdateUserDefinedUniforms() const
{
    for(const auto& UniformEntry : Uniforms)
    {
        const auto& UniformName = UniformEntry.first;
        const auto& UniformData = UniformEntry.second;
        switch(UniformData.DataID)
        {
            case GDIUniformData::INT:
                Resource->SetUniform(UniformName, UniformData.Data._i);
                break;
            case GDIUniformData::FLOAT:
                Resource->SetUniform(UniformName, UniformData.Data._f);
                break;
            case GDIUniformData::VEC2:
                Resource->SetUniform(UniformName, UniformData.Data._v2);
                break;
            case GDIUniformData::VEC3:
                Resource->SetUniform(UniformName, UniformData.Data._v3);
                break;
            case GDIUniformData::VEC4:
                Resource->SetUniform(UniformName, UniformData.Data._v4);
                break;
            case GDIUniformData::MAT:
                Resource->SetUniform(UniformName, UniformData.Data._m);
                break;
            case GDIUniformData::TEX:
                Resource->SetUniform(UniformName, UniformData.Data._t, UniformData.TexIndex);
                break;
            case GDIUniformData::TEXRES:
                Resource->SetUniform(UniformName, UniformData.Data._tr, UniformData.TexIndex);
                break;
        }
    }
}

void GDIShader::CleanupAllocatedUniformData()
{
    for(auto& UniformEntry : Uniforms)
    {
        const auto& UniformName = UniformEntry.first;
        auto& UniformData = UniformEntry.second;
        switch(UniformData.DataID)
        {
            case GDIUniformData::TEX:
                {
                    UniformData.Data._t->Release();
                    delete UniformData.Data._t;
                }
                break;
        }
    }
}

namespace
{
    /*
     * #include recursion guard.
     *
     * LoadGLSLSource recurses on every `#include <...>` it finds and has never
     * had cycle detection: two shader chunks including each other hang the
     * process at startup with no diagnostic at all. That was survivable while
     * the paths were fixed qrc strings; with resolution now delegated to a
     * provider it is worth a cheap ceiling. The limit sits far above any real
     * shader's include depth, so reaching it means a cycle, not a deep tree.
     */
    constexpr int kMaxShaderIncludeDepth = 16;

    std::string LoadGLSLSourceImpl(const std::string& SrcCodePath,
                                   const EShaderType InShaderType,
                                   const int Depth)
    {
        if(Depth > kMaxShaderIncludeDepth)
        {
            GDI_LOG_ERROR("[GDI] Shader #include depth exceeded for '" + SrcCodePath
                          + "' -- almost certainly a circular include.")
            return std::string();
        }

        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        if(Provider == nullptr)
        {
            GDI_CHECK(0, "[GDI] No file provider installed; cannot load shader source: " + SrcCodePath)
            return std::string();
        }

        std::string RawSource;
        if(!Provider->ReadAllText(SrcCodePath, RawSource))
        {
            GDI_CHECK(0, "Failed to Open File! " + SrcCodePath)
            return std::string();
        }

        // KeepEmptyParts, matching the QString::split this replaces: blank
        // lines are significant in the emitted GLSL.
        const std::vector<std::string> Lines = EngineCore::Split(RawSource, '\n');

        bool        bInPreProcessorBlock  = false;
        EShaderType PreprocessorBlockType = ST_MAX;
        std::string PreprocessorBlockStr;
        std::string Result;

        for(const std::string& Line : Lines)
        {
            if(EngineCore::Contains(Line, "#include"))
            {
                const size_t Open  = Line.find('<');
                const size_t Close = Line.find('>');

                // A malformed include line is skipped rather than producing a
                // garbage path; the QString version would have built one out of
                // negative indices.
                if(Open == std::string::npos || Close == std::string::npos || Close <= Open + 1)
                {
                    continue;
                }

                const std::string Path = std::string("Shaders/")
                                       + Line.substr(Open + 1, Close - Open - 1);

                Result += LoadGLSLSourceImpl(Path, InShaderType, Depth + 1) + '\n';
                continue;
            }

            if(EngineCore::Contains(Line, "#if"))
            {
                bInPreProcessorBlock = true;
                PreprocessorBlockStr.clear();
                if(EngineCore::Contains(Line, "VERTEX_SHADER"))
                {
                    PreprocessorBlockType = ST_Vertex;
                }
                if(EngineCore::Contains(Line, "PIXEL_SHADER"))
                {
                    PreprocessorBlockType = ST_Pixel;
                }
                continue;
            }

            if(EngineCore::Contains(Line, "#endif"))
            {
                bInPreProcessorBlock = false;
                if(PreprocessorBlockType == InShaderType)
                {
                    Result += PreprocessorBlockStr + "\n";
                }
                continue;
            }

            if(bInPreProcessorBlock)
            {
                PreprocessorBlockStr += Line + '\n';
            }
            else
            {
                Result += Line + '\n';
            }
        }

        return Result;
    }
}

const std::string LoadGLSLSource( const std::string& SrcCodePath, const EShaderType InShaderType )
{
    return LoadGLSLSourceImpl(SrcCodePath, InShaderType, 0);
}

std::string ProcessShaderFromSource( const std::string& SrcCodePath, ERenderPlatform RenderPlatform )
{
    const std::string FilePath = GetCookedShaderPath(SrcCodePath, RenderPlatform);

    /*
     * Vulkan and Metal: the backend resource class opens this path itself and
     * reads it as raw BINARY (SPIR-V / MSL), so the cooked path is returned
     * as-is. Reading it here as text would corrupt the binary on Windows.
     *
     * This split between "return the path" and "return the text" is
     * deliberate. Do not collapse the two branches.
     */
    if(RenderPlatform != ERP_OpenGL)
    {
        return FilePath;
    }

    // OpenGL: return the preprocessed GLSL text to hand to glShaderSource.
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(Provider == nullptr)
    {
        GDI_CHECK(0, "[GDI] No file provider installed; cannot load shader: " + FilePath)
        return "";
    }

    std::string Source;
    if(!Provider->ReadAllText(FilePath, Source))
    {
        GDI_CHECK(0, "Failed to open file: " + FilePath)
        return "";
    }
    return Source;
}

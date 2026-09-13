#include "Public/GDI/OpenGL/OGLShader.h"
#include "Public/GDI/OpenGL/OGLTexture.h"
#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/Math/Matrix.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

GDIShaderResource* OpenGLDynamicGDI::CreateShaderResource(EShaderType InType, const std::string& SrcCode)
{
    return new OpenGLShaderResource(InType, SrcCode);
}

void OpenGLDynamicGDI::SetBoundShader(GDIShader *In_VS, GDIShader *In_PS)
{
    if(BoundShaderProgram)
    {
        glDeleteProgram(BoundShaderProgram);
        BoundShaderProgram = 0;
    }

    auto VS_GDI = (In_VS->GetShaderType() == ST_Vertex) ? ((OpenGLShaderResource*)In_VS->Resource) : nullptr;
    auto PS_GDI = (In_PS->GetShaderType() == ST_Pixel) ? ((OpenGLShaderResource*)In_PS->Resource) : nullptr;

    if(VS_GDI && PS_GDI)
    {
        BoundVertexShader = In_VS;
        BoundPixelShader = In_PS;
        BoundShaderProgram = glCreateProgram();
        glAttachShader(BoundShaderProgram, VS_GDI->ShaderHandle);
        glAttachShader(BoundShaderProgram, PS_GDI->ShaderHandle);
        glLinkProgram(BoundShaderProgram);
        int GLResult;
        glGetProgramiv(BoundShaderProgram, GL_LINK_STATUS, &GLResult);
        if(!GLResult)
        {
            const int InfoLogSize = 1024;
            char InfoLog[InfoLogSize];
            glGetProgramInfoLog(BoundShaderProgram, InfoLogSize, NULL, InfoLog);
            GDI_CHECK(0,"[RenderingThread] Failed to link shader program! " + std::string(InfoLog));
        }
        VS_GDI->ProgramHandle = BoundShaderProgram;
        PS_GDI->ProgramHandle = BoundShaderProgram;
        glUseProgram(BoundShaderProgram);
    }
}

OpenGLShaderResource::OpenGLShaderResource(EShaderType InShaderType, const std::string& SrcCodePath) :
    GDIShaderResource(InShaderType, SrcCodePath)
{
    const GLenum DecodedShaderType = DecodeShaderType(InShaderType);
    const std::string SrcCode = ProcessShaderFromSource(SrcCodePath, ERP_OpenGL);
    const char* SrcCode_CSTR = SrcCode.c_str();

    initializeOpenGLFunctions();

    ShaderHandle = glCreateShader(DecodedShaderType);
    glShaderSource(ShaderHandle, 1, &SrcCode_CSTR, NULL);
    glShaderSource(ShaderHandle, 1, &SrcCode_CSTR, NULL);
    glCompileShader(ShaderHandle);

    int GLStatus;
    glGetShaderiv(ShaderHandle, GL_COMPILE_STATUS, &GLStatus);
    if(!GLStatus)
    {
        return;
    }

    bInitialized = true;
}

void OpenGLShaderResource::SetUniform(std::string InName, const int InValue)
{
    glUniform1i(glGetUniformLocation( ProgramHandle,QSTR_TO_CONSTCHAR(InName)), InValue );
}

void OpenGLShaderResource::SetUniform(std::string InName, const float InValue) {
    glUniform1f( glGetUniformLocation( ProgramHandle,QSTR_TO_CONSTCHAR(InName)), InValue );
}

void OpenGLShaderResource::SetUniform(std::string InName, const Vector2 InVector) {
    glUniform2f( glGetUniformLocation( ProgramHandle,QSTR_TO_CONSTCHAR(InName) ), InVector.GetX(), InVector.GetY() );
}

void OpenGLShaderResource::SetUniform(std::string InName, const Vector3 InVector) {
    glUniform3f(glGetUniformLocation(ProgramHandle,QSTR_TO_CONSTCHAR(InName)), InVector.GetX(), InVector.GetY(), InVector.GetZ() );
}

void OpenGLShaderResource::SetUniform(std::string InName, const Vector4 InVector) {
    glUniform4f(glGetUniformLocation(ProgramHandle,QSTR_TO_CONSTCHAR(InName)), InVector.GetX(), InVector.GetY(), InVector.GetZ(), InVector.GetW());
}

void OpenGLShaderResource::SetUniform(std::string InName, const Matrix4x4 InMatrix) {
    GLfloat MatrixData[4][4];
    auto InMatrixData = InMatrix.Data;
    for(int i = 0; i < 4; i++)
    {
        for(int j = 0; j < 4; j++)
        {
            MatrixData[i][j] = float(InMatrixData[i][j]);
        }
    }
    glUniformMatrix4fv(glGetUniformLocation(ProgramHandle, QSTR_TO_CONSTCHAR(InName)), 1, GL_TRUE,
                       reinterpret_cast<const GLfloat *>(&MatrixData[0]) );
}

void OpenGLShaderResource::SetUniform(std::string InName, GDITexture2D *InTexture2D, const uint InIndex) {
    if(InTexture2D == nullptr)
    {
        return;
    }

    if( InTexture2D->Resource )
    {
        SetUniform(InName, InTexture2D->Resource, InIndex);
    }
}

void OpenGLShaderResource::SetUniform(std::string InName, GDITextureResource* InResource, const uint InIdx)
{
    if(InResource == nullptr)
    {
        return;
    }

    if(auto* Resource =dynamic_cast<OpenGLTextureResource*>(InResource))
    {
        glEnable(GL_TEXTURE_2D);
        const uint TextureHandle = Resource->TextureHandle;
        glActiveTexture(GL_TEXTURE0 + InIdx);
        glBindTexture(GL_TEXTURE_2D, TextureHandle);
        SetUniform(InName, int(InIdx));
    }
}

void OpenGLShaderResource::Release() {
    glDeleteShader(ShaderHandle);
    ShaderHandle = 0;
}

GLenum OpenGLShaderResource::DecodeShaderType(EShaderType InType)
{
    switch(InType)
    {
        case ST_Vertex:
            return GL_VERTEX_SHADER;
        case ST_Pixel:
            return GL_FRAGMENT_SHADER;
    }
    return -1;
};

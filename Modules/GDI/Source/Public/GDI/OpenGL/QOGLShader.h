#ifndef CORECLIENT_QOGLSHADER_H
#define CORECLIENT_QOGLSHADER_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/GDIShader.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class QOpenGLShaderResource : public GDIShaderResource,
        protected GDIGLFunctions
{
public:
    QOpenGLShaderResource(EShaderType InType, const std::string& SrcCode);

    void SetUniform(std::string InName, const int InValue) override;

    void SetUniform(std::string InName, const float InValue) override;

    void SetUniform(std::string InName, const Vector2 InVector) override;

    void SetUniform(std::string InName, const Vector3 InVector) override;

    void SetUniform(std::string InName, const Vector4 InVector) override;

    void SetUniform(std::string InName, const Matrix4x4 InMatrix) override;

    void SetUniform(std::string InName, GDITexture2D *InTexture2D, const uint InIndex) override;

    void SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex) override;

    void Release(void) override;

    uint ProgramHandle;
    uint ShaderHandle;
private:
    static GLenum DecodeShaderType(EShaderType InType);
};

#endif //CORECLIENT_QOGLSHADER_H

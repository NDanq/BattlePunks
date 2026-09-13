#include "Public/GDI/OpenGL/OGLTexture.h"
#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"
#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

GDITextureResource* OpenGLDynamicGDI::CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    return new OpenGLTextureResource(InImage, InBorder, InFilter);
}

GDITextureResource* OpenGLDynamicGDI::CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    return new OpenGLTextureResource(InParent, InImage, InBorder, InFilter);
}

GDITextureResource* OpenGLDynamicGDI::CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    return new OpenGLTextureResource(InParent, InBorder, InFilter);
}


OpenGLTextureResource::OpenGLTextureResource() :
GDITextureResource(),
TextureIndex(0),
TextureHandle(0)
{
    initializeOpenGLFunctions();
}

OpenGLTextureResource::OpenGLTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter) :
        GDITextureResource(InImage),
        TextureIndex(0)
{
    const GLenum TextureBorder = DecodeTB(InBorder);
    const GLenum TextureFilter = DecodeTF(InFilter);

    const EngineCore::Image FormattedImage = InImage.ConvertedTo(EngineCore::EImageFormat::BGRA8);

    initializeOpenGLFunctions();

    glGenTextures(1, &TextureHandle);
    glBindTexture(GL_TEXTURE_2D, TextureHandle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TextureFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, TextureFilter);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, FormattedImage.GetData());
    glGenerateMipmap(GL_TEXTURE_2D);

    bInitialized = true;
    glBindTexture(GL_TEXTURE_2D, 0);
}

OpenGLTextureResource::OpenGLTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter) :
        GDITextureResource(InParent, InImage),
        TextureIndex(0)
{
    const GLenum TextureBorder = DecodeTB(InBorder);
    const GLenum TextureFilter = DecodeTF(InFilter);

    const EngineCore::Image FormattedImage = InImage.ConvertedTo(EngineCore::EImageFormat::BGRA8);

    initializeOpenGLFunctions();

    glGenTextures(1, &TextureHandle);
    glBindTexture(GL_TEXTURE_2D, TextureHandle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TextureFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, TextureFilter);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, FormattedImage.GetData());
    glGenerateMipmap(GL_TEXTURE_2D);

    bInitialized = true;
    glBindTexture(GL_TEXTURE_2D, 0);
}

OpenGLTextureResource::OpenGLTextureResource(GDITexture2D *InParent, const EGDITextureBorder InBorder,
                                               const EGDITextureFilter InFilter) :
        GDITextureResource(InParent, InBorder, InFilter),
        TextureIndex(0)
{
    const GLenum TextureBorder = DecodeTB(InBorder);
    const GLenum TextureFilter = DecodeTF(InFilter);
    Width = InParent->Width;
    Height = InParent->Height;
    initializeOpenGLFunctions();

    glGenTextures(1, &TextureHandle);
    glBindTexture(GL_TEXTURE_2D, TextureHandle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TextureFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, TextureFilter);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    bInitialized = true;
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLTextureResource::Update(const EngineCore::Image &InImage)
{
    if(Width != InImage.GetWidth())
    {
        Width = InImage.GetWidth();
    }

    if(Height != InImage.GetHeight())
    {
        Height = InImage.GetHeight();
    }

    const EngineCore::Image FormattedImage = InImage.ConvertedTo(EngineCore::EImageFormat::BGRA8);

    glBindTexture(GL_TEXTURE_2D, TextureHandle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, FormattedImage.GetData());
    glBindTexture(GL_TEXTURE_2D, 0);
}

EngineCore::Image OpenGLTextureResource::ToImage() const
{
    GDIRenderTarget* TextureTemp = GDICreateRenderTarget("TextureTemp", Width, Height);
    TextureTemp->DrawTexture(Parent);
    EngineCore::Image Img = TextureTemp->ToImage();
    delete TextureTemp;
    TextureTemp = nullptr;

    return Img;
}

void OpenGLTextureResource::Release()
{
    glDeleteTextures(1, &TextureHandle);
    bInitialized = false;
}

GLenum OpenGLTextureResource::DecodeTB(EGDITextureBorder InBorder)
{
    switch (InBorder)
    {
        case GDI_TB_Repeat:
            return GL_REPEAT;
        case GDI_TB_Mirrored_Repeat:
            return GL_MIRRORED_REPEAT;
        case GDI_TB_Clamp_To_Edge:
            return GL_CLAMP_TO_EDGE;
        case GDI_TB_Clamp_To_Border:
            return GL_CLAMP_TO_BORDER;
        case GDI_TB_MAX:
            break;
    }
    return -1;
}

GLenum OpenGLTextureResource::DecodeTF(EGDITextureFilter InFilter)
{
    switch (InFilter)
    {
        case GDI_TF_Nearest:
            return GL_NEAREST;
        case GDI_TF_Linear:
            return GL_LINEAR;
        case GDI_TF_MAX:
            break;
    }
    return -1;
}
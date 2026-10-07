#include "Public/GDI/OpenGL/OGLTexture.h"

#include <algorithm>
#include <cstring>

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

GDITextureResource* OpenGLDynamicGDI::CreateTextureResourceMips(const EngineCore::Image* InLevels, const uint32_t InCount,
                                                                const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    return InLevels && InCount > 0 ? new OpenGLTextureResource(InLevels, InCount, InBorder, InFilter) : nullptr;
}

namespace
{
    /* The UNORM block formats; BC1 without alpha, so a 3-colour block's black stays opaque. */
    GLenum ToGLCompressedFormat(const EGDICompressedFormat InFormat)
    {
        switch(InFormat)
        {
            case GDI_CF_BC1: return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
            case GDI_CF_BC3: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            case GDI_CF_BC4: return GL_COMPRESSED_RED_RGTC1;
            case GDI_CF_BC5: return GL_COMPRESSED_RG_RGTC2;
            case GDI_CF_BC7: return GL_COMPRESSED_RGBA_BPTC_UNORM;
            default: break;
        }
        return 0;
    }
}

bool OpenGLDynamicGDI::SupportsCompressedFormat(const EGDICompressedFormat InFormat) const
{
    /* RGTC is core from 3.0 and BPTC from 4.2, under the 4.5 the backend
     * requires; S3TC is an extension, though every desktop driver has it. */
    if(InFormat == GDI_CF_BC4 || InFormat == GDI_CF_BC5 || InFormat == GDI_CF_BC7) { return true; }
    if(InFormat != GDI_CF_BC1 && InFormat != GDI_CF_BC3) { return false; }
    static const bool bS3TC = [this]
    {
        GLint Count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &Count);
        for(GLint Each = 0; Each < Count; ++Each)
        {
            const char* Name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(Each)));
            if(Name && std::strcmp(Name, "GL_EXT_texture_compression_s3tc") == 0) { return true; }
        }
        return false;
    }();
    return bS3TC;
}

GDITextureResource* OpenGLDynamicGDI::CreateCompressedTextureResource(const EGDICompressedFormat InFormat,
                                                                      const GDICompressedLevel* InLevels, const uint32_t InCount,
                                                                      const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    if(!InLevels || InCount == 0 || !SupportsCompressedFormat(InFormat)) { return nullptr; }
    return new OpenGLTextureResource(ToGLCompressedFormat(InFormat), InLevels, InCount, InBorder, InFilter);
}

OpenGLTextureResource::OpenGLTextureResource(const GLenum InInternalFormat, const GDICompressedLevel* InLevels,
                                             const uint32_t InCount, const EGDITextureBorder InBorder,
                                             const EGDITextureFilter InFilter) :
        GDITextureResource(),
        TextureIndex(0)
{
    Width  = int(InLevels[0].Width);
    Height = int(InLevels[0].Height);

    const GLenum TextureBorder = DecodeTB(InBorder);
    const GLenum Magnify       = DecodeTF(InFilter);
    const GLenum Minify        = InCount == 1 ? Magnify
                               : InFilter == GDI_TF_Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;

    initializeOpenGLFunctions();

    glGenTextures(1, &TextureHandle);
    glBindTexture(GL_TEXTURE_2D, TextureHandle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Minify);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Magnify);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(InCount) - 1);
    // GDISetMaxAnisotropy (GL 4.6, or EXT_texture_filter_anisotropic: the same enums),
    // clamped to the driver's limit; a driver without it reports 0 and is left alone.
    if(InFilter == GDI_TF_Linear && InCount > 1 && GDIGetMaxAnisotropy() > 1.0f)
    {
        GLfloat Limit = 0.0f;
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &Limit);
        if(Limit > 1.0f)
        {
            glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, std::min(GDIGetMaxAnisotropy(), float(Limit)));
        }
    }

    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        glCompressedTexImage2D(GL_TEXTURE_2D, GLint(Level), InInternalFormat, GLsizei(InLevels[Level].Width),
                               GLsizei(InLevels[Level].Height), 0, GLsizei(InLevels[Level].Bytes), InLevels[Level].Data);
    }

    bInitialized = true;
    glBindTexture(GL_TEXTURE_2D, 0);
}

OpenGLTextureResource::OpenGLTextureResource(const EngineCore::Image* InLevels, const uint32_t InCount,
                                             const EGDITextureBorder InBorder, const EGDITextureFilter InFilter) :
        GDITextureResource(InLevels[0]),
        TextureIndex(0)
{
    const GLenum TextureBorder = DecodeTB(InBorder);
    const GLenum Magnify       = DecodeTF(InFilter);
    const GLenum Minify        = InFilter == GDI_TF_Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;

    initializeOpenGLFunctions();

    glGenTextures(1, &TextureHandle);
    glBindTexture(GL_TEXTURE_2D, TextureHandle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TextureBorder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Minify);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Magnify);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(InCount) - 1);
    // GDISetMaxAnisotropy (GL 4.6, or EXT_texture_filter_anisotropic: the same enums),
    // clamped to the driver's limit; a driver without it reports 0 and is left alone.
    if(InFilter == GDI_TF_Linear && InCount > 1 && GDIGetMaxAnisotropy() > 1.0f)
    {
        GLfloat Limit = 0.0f;
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &Limit);
        if(Limit > 1.0f)
        {
            glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, std::min(GDIGetMaxAnisotropy(), float(Limit)));
        }
    }

    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        const EngineCore::Image Formatted = InLevels[Level].ConvertedTo(EngineCore::EImageFormat::BGRA8);
        glTexImage2D(GL_TEXTURE_2D, GLint(Level), GL_RGBA, Formatted.GetWidth(), Formatted.GetHeight(), 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, Formatted.GetData());
    }

    bInitialized = true;
    glBindTexture(GL_TEXTURE_2D, 0);
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);   // one image: a mip chain it had is stale now
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
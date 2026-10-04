#ifndef CORECLIENT_OGLTEXTURE_H
#define CORECLIENT_OGLTEXTURE_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDITexture.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class OpenGLTextureResource : public GDITextureResource,
        protected GDIGLFunctions
{
public:
    OpenGLTextureResource();

    OpenGLTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    OpenGLTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    /* A mip chain: InLevels[0] full size, each next half the last. */
    OpenGLTextureResource(const EngineCore::Image* InLevels, uint32_t InCount, EGDITextureBorder InBorder, EGDITextureFilter InFilter);

    OpenGLTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    void Update(const EngineCore::Image& InImage) override;

    void Release() override;

    EngineCore::Image ToImage() const override;

    uint TextureHandle;
    int TextureIndex;
private:
    GLenum DecodeTB(EGDITextureBorder);
    GLenum DecodeTF(EGDITextureFilter);

};


#endif //CORECLIENT_OGLTEXTURE_H

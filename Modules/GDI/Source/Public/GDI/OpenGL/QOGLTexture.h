#ifndef CORECLIENT_QOGLTEXTURE_H
#define CORECLIENT_QOGLTEXTURE_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDITexture.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class QOpenGLTextureResource : public GDITextureResource,
        protected GDIGLFunctions
{
public:
    QOpenGLTextureResource();

    QOpenGLTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    QOpenGLTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    QOpenGLTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    void Update(const EngineCore::Image& InImage) override;

    void Release() override;

    EngineCore::Image ToImage() const override;

    uint TextureHandle;
    int TextureIndex;
private:
    GLenum DecodeTB(EGDITextureBorder);
    GLenum DecodeTF(EGDITextureFilter);

};


#endif //CORECLIENT_QOGLTEXTURE_H

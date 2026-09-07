#ifndef CORECLIENT_NULLTEXTURE_H
#define CORECLIENT_NULLTEXTURE_H

#include "Public/GDI/GDITexture.h"

class NullTextureResource : public GDITextureResource
{
public:
    NullTextureResource() = default;

    explicit NullTextureResource(const EngineCore::Image& InImage, const uint InIdx = 0,
                                  const EGDITextureBorder InBorder = GDI_TB_Repeat,
                                  const EGDITextureFilter InFilter = GDI_TF_Nearest) :
        GDITextureResource(InImage, InIdx, InBorder, InFilter)
    {
    }

    explicit NullTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat,
                                  const EGDITextureFilter InFilter = GDI_TF_Nearest) :
        GDITextureResource(InParent, InBorder, InFilter)
    {
    }

    NullTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const uint InIdx = 0,
                        const EGDITextureBorder InBorder = GDI_TB_Repeat,
                        const EGDITextureFilter InFilter = GDI_TF_Nearest) :
        GDITextureResource(InParent, InImage, InIdx, InBorder, InFilter)
    {
    }

    void Update(const EngineCore::Image& InImage) override {}
    void Release() override {}

    /* Backend readback format is meaningless with no GPU behind it; a blank
     * Image (EImageFormat::None) is the honest answer. */
    EngineCore::Image ToImage() const override { return EngineCore::Image(); }
};

#endif //CORECLIENT_NULLTEXTURE_H

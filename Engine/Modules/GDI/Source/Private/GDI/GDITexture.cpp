#include "Public/GDI/GDI.h"
#include "Public/GDI/GDITexture.h"

GDITextureResource::GDITextureResource(GDITexture2D *InParent, const EngineCore::Image &InImage, const uint InIdx,
                                       const EGDITextureBorder InBorder, const EGDITextureFilter InFilter) :
                                       Parent(InParent),
                                       bInitialized(false),
                                       Width(InImage.GetWidth()),
                                       Height(InImage.GetHeight())

{

}

void GDITexture2D::InitResource()
{
    if(Image.IsNull())
    {
        return;
    }
    Release();

    Resource = GDICreateTextureResource(this, Image);
}

void GDITexture2D::InitResourceNoData()
{
    Resource = GDICreateTextureResource(this);
}

void GDITexture2D::UpdateTexture(const EngineCore::Image &InImage)
{
    // Deliberately no flip here; the GDITexture2D(Image) constructor mirrors
    // horizontally and this does not. See the note in GDITexture.h.
    Image = InImage;
    if(Resource)
    {
        Resource->Update(Image);
    }
}

void GDITexture2D::Release()
{
    if(Resource)
    {
        Resource->Release();
        delete Resource;
        Resource = nullptr;
    }
}

EngineCore::Image GDITexture2D::ToImage()
{
    if(Resource)
    {
        return Resource->ToImage();
    }
    return EngineCore::Image();
}

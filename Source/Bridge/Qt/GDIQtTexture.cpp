#include "GDIQtTexture.h"

#include "CoreQtConversions.h"

namespace GDIQt
{
GDITexture2D* GDIQtTexture::FromQImage(const QImage& InImage)
{
    // The GDITexture2D(Image) constructor applies the horizontal flip, exactly
    // as the removed GDITexture2D(QImage) constructor did.
    return new GDITexture2D(EngineCore::QtBridge::FromQImage(InImage));
}

QImage GDIQtTexture::ToQImage(GDITexture2D* InTexture)
{
    if(InTexture == nullptr)
    {
        return QImage();
    }
    return EngineCore::QtBridge::ToQImage(InTexture->ToImage());
}

QPixmap GDIQtTexture::ToQPixmap(GDITexture2D* InTexture)
{
    const QImage Img = ToQImage(InTexture);
    if(Img.isNull())
    {
        return QPixmap();
    }
    return QPixmap::fromImage(Img);
}
}

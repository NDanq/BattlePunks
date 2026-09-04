#ifndef CORECLIENT_GDIQTTEXTURE_H
#define CORECLIENT_GDIQTTEXTURE_H

#include <QImage>
#include <QPixmap>

#include "Public/GDI/GDITexture.h"

/*
 * Qt entry points for GDI textures.
 *
 * GDITexture2D used to take a QImage directly and hand back QImage/QPixmap.
 * Those overloads moved here so GDI's own interface speaks only
 * EngineCore::Image; the behaviour is unchanged, just relocated.
 *
 * ORIENTATION: FromQImage applies the SAME horizontal flip the old
 * GDITexture2D(QImage) constructor did, because it forwards to the
 * GDITexture2D(Image) constructor which still performs it. Callers that were
 * relying on the constructor's implicit mirror keep getting it, and callers
 * using UpdateTexture keep NOT getting it. That asymmetry is load-bearing --
 * see the note at the top of GDITexture.h.
 *
 * FORMAT: the conversion preserves the pixel format rather than normalising
 * it. A Format_ARGB32 QImage becomes a BGRA8 Image and back again byte for
 * byte, which is what keeps the OpenGL backend's channel order correct.
 */

namespace GDIQt
{
    class GDIQtTexture
    {
    public:
        /* Applies the constructor's horizontal flip. Caller owns the result. */
        static GDITexture2D* FromQImage(const QImage& InImage);

        /* Host readback, in the backend's own pixel format. */
        static QImage  ToQImage(GDITexture2D* InTexture);
        static QPixmap ToQPixmap(GDITexture2D* InTexture);
    };
}

#endif //CORECLIENT_GDIQTTEXTURE_H

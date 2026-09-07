#ifndef ENGINECORE_COREQTCONVERSIONS_H
#define ENGINECORE_COREQTCONVERSIONS_H

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <string>

#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreTypes.h"

/*
 * Qt <-> EngineCore value conversions.
 *
 * This header, and the target it belongs to, is one of exactly two places in
 * the EngineCore/GDI projects permitted to include Qt. Nothing here holds
 * state; these are pure value conversions that a Qt host uses at the boundary.
 *
 * ORIENTATION IS NOT THIS FILE'S JOB. No function here flips anything. The
 * renderer composes vertical and horizontal flips at specific, documented
 * points (the GL readback mirrors vertically, texture upload mirrors
 * horizontally, the in-memory layer restore mirrors both, and the disk-read
 * path deliberately mirrors neither). If a conversion here silently flipped,
 * every one of those would be off and the composition would no longer match
 * the comments that describe it. Convert the container; leave orientation to
 * the caller.
 */

namespace EngineCore
{
namespace QtBridge
{
    // --- pixels -----------------------------------------------------------

    /* Maps a Qt format to its EngineCore equivalent by MEMORY order.
     * EImageFormat::None for anything unsupported. */
    EImageFormat  FromQImageFormat(QImage::Format InFormat);
    QImage::Format ToQImageFormat(EImageFormat InFormat);

    /*
     * Deep copies, row by row, using the source's own stride.
     *
     * QImage pads scanlines to a 4-byte boundary. For a 32-bit format that
     * coincides with tight packing at every width, so a whole-buffer memcpy
     * passes every test that uses ARGB32 -- and corrupts the first time an
     * 8-bit Grayscale8 image of a width that is not a multiple of 4 appears.
     * Both directions here go row by row for that reason. Do not "optimize"
     * either into a single memcpy.
     *
     * An unsupported format or a null input yields a null result rather than
     * a partially-converted one.
     */
    Image  FromQImage(const QImage& InImage);
    QImage ToQImage(const Image& InImage);

    // --- scalars and geometry --------------------------------------------

    inline Color FromQColor(const QColor& InColor)
    {
        return Color(uint8_t(InColor.red()),
                     uint8_t(InColor.green()),
                     uint8_t(InColor.blue()),
                     uint8_t(InColor.alpha()));
    }

    inline QColor ToQColor(const Color& InColor)
    {
        return QColor(InColor.R, InColor.G, InColor.B, InColor.A);
    }

    /* QRect::right()/bottom() are inclusive and Rect reproduces that, so
     * width/height map straight across with no off-by-one adjustment. */
    inline Rect FromQRect(const QRect& InRect)
    {
        return Rect(InRect.x(), InRect.y(), InRect.width(), InRect.height());
    }

    inline QRect ToQRect(const Rect& InRect)
    {
        return QRect(InRect.X, InRect.Y, InRect.Width, InRect.Height);
    }

    inline Point  FromQPoint(const QPoint& InPoint) { return Point(InPoint.x(), InPoint.y()); }
    inline QPoint ToQPoint(const Point& InPoint)    { return QPoint(InPoint.X, InPoint.Y); }

    inline Size  FromQSize(const QSize& InSize) { return Size(InSize.width(), InSize.height()); }
    inline QSize ToQSize(const Size& InSize)    { return QSize(InSize.Width, InSize.Height); }

    // --- strings ----------------------------------------------------------
    // UTF-8 in both directions. GDI's string content is ASCII shader and
    // resource names today, but going through UTF-8 rather than latin1 means
    // a non-ASCII asset path does not quietly corrupt.

    inline std::string FromQString(const QString& InString)
    {
        const QByteArray Utf8 = InString.toUtf8();
        return std::string(Utf8.constData(), size_t(Utf8.size()));
    }

    inline QString ToQString(const std::string& InString)
    {
        return QString::fromUtf8(InString.data(), int(InString.size()));
    }
}
}

#endif //ENGINECORE_COREQTCONVERSIONS_H

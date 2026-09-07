#include "CoreQtConversions.h"

#include <QtGlobal>

#include <cstring>

/*
 * The BGRA8 <-> Format_ARGB32 mapping below is only correct on a
 * little-endian machine.
 *
 * QImage::Format_ARGB32 names the layout of the packed 32-bit integer, not the
 * byte order in memory. On little-endian that integer stores as B,G,R,A --
 * which is why BGRA8 is the right EngineCore format and why the existing GL
 * readback path reads GL_BGRA into a Format_ARGB32 wrapper and is correct
 * despite looking wrong. On a big-endian target the same code would swap red
 * and blue silently, so the assumption is asserted rather than assumed.
 */
static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN,
              "EngineCore's BGRA8 <-> QImage::Format_ARGB32 mapping assumes little-endian byte order.");

namespace EngineCore
{
namespace QtBridge
{
EImageFormat FromQImageFormat(const QImage::Format InFormat)
{
    switch(InFormat)
    {
        // Format_ARGB32 IS B,G,R,A in memory here -- see the note above.
        case QImage::Format_ARGB32:
        case QImage::Format_RGB32:          return EImageFormat::BGRA8;
        case QImage::Format_RGBA8888:       return EImageFormat::RGBA8;
        case QImage::Format_Grayscale8:     return EImageFormat::Gray8;
        default:                            return EImageFormat::None;
    }
}

QImage::Format ToQImageFormat(const EImageFormat InFormat)
{
    switch(InFormat)
    {
        case EImageFormat::BGRA8:   return QImage::Format_ARGB32;
        case EImageFormat::RGBA8:   return QImage::Format_RGBA8888;
        case EImageFormat::Gray8:   return QImage::Format_Grayscale8;
        case EImageFormat::None:    return QImage::Format_Invalid;
    }
    return QImage::Format_Invalid;
}

Image FromQImage(const QImage& InImage)
{
    if(InImage.isNull())
    {
        return Image();
    }

    QImage Source = InImage;

    // An unsupported source format is converted rather than rejected, so that
    // callers handing over an indexed or premultiplied image still get correct
    // pixels. ARGB32 is the conversion target because it is what the rest of
    // the pipeline already uses.
    EImageFormat Format = FromQImageFormat(Source.format());
    if(Format == EImageFormat::None)
    {
        Source = Source.convertToFormat(QImage::Format_ARGB32);
        Format = EImageFormat::BGRA8;
    }

    // Row by row, using QImage's stride -- never one memcpy. See the header.
    return Image::CopyFrom(Source.constBits(),
                           Source.width(),
                           Source.height(),
                           Format,
                           size_t(Source.bytesPerLine()));
}

QImage ToQImage(const Image& InImage)
{
    if(InImage.IsNull())
    {
        return QImage();
    }

    const QImage::Format QtFormat = ToQImageFormat(InImage.GetFormat());
    if(QtFormat == QImage::Format_Invalid)
    {
        return QImage();
    }

    QImage Out(InImage.GetWidth(), InImage.GetHeight(), QtFormat);
    if(Out.isNull())
    {
        return QImage();
    }

    const size_t SrcStride = InImage.GetStride();

    // QImage's own bytesPerLine() may exceed our tight stride; copying whole
    // buffers would misalign every row after the first.
    for(int Y = 0; Y < InImage.GetHeight(); ++Y)
    {
        std::memcpy(Out.scanLine(Y), InImage.GetScanline(Y), SrcStride);
    }

    return Out;
}
}
}

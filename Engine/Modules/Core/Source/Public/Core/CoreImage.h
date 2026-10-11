#ifndef ENGINECORE_COREIMAGE_H
#define ENGINECORE_COREIMAGE_H

#include <cstdint>
#include <cstddef>
#include <vector>

#include "Public/Core/CoreBytes.h"

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreTypes.h"

namespace EngineCore
{
    /*
     * Pixel layout is named for MEMORY ORDER, first byte first.
     *
     * This matters more than it looks. QImage::Format_ARGB32 is, on a
     * little-endian machine, B,G,R,A in memory -- the name describes the
     * packed 32-bit integer, not the bytes. That mismatch is exactly how a
     * red/blue channel swap gets introduced by someone "correcting" the
     * format name. Naming these by byte order removes the ambiguity: the
     * bridge maps Format_ARGB32 <-> BGRA8, and that is the correct mapping.
     */
    enum class EImageFormat : uint8_t
    {
        None = 0,
        BGRA8,      // B,G,R,A -- matches QImage::Format_ARGB32 on little-endian
        RGBA8,      // R,G,B,A -- matches QImage::Format_RGBA8888
        Gray8       // single 8-bit channel; brush stencils and masks
    };

    CORE_API int BytesPerPixel(EImageFormat InFormat);

    /*
     * A tightly-packed 2D pixel buffer.
     *
     * TIGHT PACKING IS PART OF THE CONTRACT: GetStride() == Width *
     * BytesPerPixel(Format), always, with no scanline padding. QImage pads
     * scanlines to a 4-byte boundary, which for a 4-byte format coincides with
     * tight packing at every width -- so a naive whole-buffer memcpy between
     * the two passes every test that uses a 32-bit format and corrupts the
     * first time an 8-bit Gray8 mask of odd width appears. Conversions must
     * therefore copy ROW BY ROW using the source's own stride. CopyFrom()
     * below exists so no caller has to remember that.
     */
    class CORE_API Image
    {
    public:
        Image() = default;
        Image(int InWidth, int InHeight, EImageFormat InFormat);
        /* Its pixels from InPool (CoreMemoryPool.h): a subsystem's own, for
         * reuse and accounting -- RenderCore's textures. Copies and
         * conversions keep the pool. */
        Image(int InWidth, int InHeight, EImageFormat InFormat, PoolID InPool);

        /*
         * Copy from a strided source. InSrcStride is the source's bytes per
         * row; pass 0 to mean "tightly packed". This is the ONLY sanctioned
         * way to ingest foreign pixel data.
         */
        static Image CopyFrom(const uint8_t* InSrc,
                              int InWidth,
                              int InHeight,
                              EImageFormat InFormat,
                              size_t InSrcStride = 0);

        bool         IsNull()    const { return Pixels.empty(); }
        int          GetWidth()  const { return Width; }
        int          GetHeight() const { return Height; }
        EImageFormat GetFormat() const { return Format; }
        Size         GetSize()   const { return Size(Width, Height); }

        /* Always Width * BytesPerPixel(Format) -- see the note above. */
        size_t GetStride()    const { return size_t(Width) * size_t(BytesPerPixel(Format)); }
        size_t GetSizeBytes() const { return Pixels.size(); }
        PoolID GetPool()      const { return Pixels.GetPool(); }

        uint8_t*       GetData()       { return Pixels.empty() ? nullptr : Pixels.data(); }
        const uint8_t* GetData() const { return Pixels.empty() ? nullptr : Pixels.data(); }

        uint8_t*       GetScanline(int InY);
        const uint8_t* GetScanline(int InY) const;

        /*
         * Returns a copy in the requested format.
         *
         * Only the 32-bit formats interconvert (BGRA8 <-> RGBA8 is a red/blue
         * byte swap); a same-format request is a plain copy. This exists
         * because each backend uploads in its own layout -- OpenGL wants
         * BGRA8, Vulkan and Metal want RGBA8 -- exactly as the QImage
         * convertToFormat calls it replaces did.
         *
         * A conversion that is not supported returns a null Image rather than
         * silently handing back wrongly-ordered channels.
         */
        Image ConvertedTo(EImageFormat InFormat) const;

        /*
         * Explicit, separate, and never folded together.
         *
         * The existing renderer composes these flips in a specific order at
         * specific points (upload mirrors horizontally; readbacks no longer
         * flip, since every backend stores targets top-down (GDI.h);
         * the in-memory restore path mirrors both, the disk-read path
         * deliberately does not). Reproduce each flip where it already
         * happens. Merging two of them into a "rotate 180" is the change that
         * a centred circular brush dab will not reveal.
         */
        void FlipVertical();
        void FlipHorizontal();

    private:
        PooledBytes          Pixels;
        int                  Width  = 0;
        int                  Height = 0;
        EImageFormat         Format = EImageFormat::None;
    };
}

#endif //ENGINECORE_COREIMAGE_H

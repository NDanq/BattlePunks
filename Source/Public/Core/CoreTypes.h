#ifndef ENGINECORE_CORETYPES_H
#define ENGINECORE_CORETYPES_H

#include <cstdint>

#include "Public/Core/CoreAPI.h"

/*
 * Scalar and geometry primitives.
 *
 * WHY THE GLOBAL TYPEDEFS BELOW ARE NOT NAMESPACED
 *
 * `uint`, `uchar`, `ushort` and `ulong` are used unqualified in ~190 places
 * across GDI and the app, and in exactly zero of them were they ever declared by this codebase --
 * they leak in from <QtGlobal> transitively. The moment Qt headers stop
 * arriving first, all 185 stop compiling. Namespacing them would mean editing
 * every one of those sites for no design benefit; they are C-style scalar
 * aliases, not an interface. So they are declared at global scope with exactly
 * Qt's spellings, which makes them a no-op redeclaration while Qt is still in
 * the build and the actual definition once it is gone. (Repeating an identical
 * typedef is well-formed C++, so the two coexist during the migration.)
 *
 * The real types -- Color, Point, Size, Rect, WindowHandle, Real -- are new,
 * have no existing call sites, and live in namespace EngineCore where they
 * belong.
 */

#ifndef CORE_NO_GLOBAL_ALIASES
typedef unsigned int    uint;
typedef unsigned char   uchar;
typedef unsigned short  ushort;
typedef unsigned long   ulong;
#endif

namespace EngineCore
{
    /* Replaces qreal. Kept as double so the SIMD/scalar math is unchanged. */
    using Real = double;

    /*
     * Opaque native window handle -- an HWND on Windows, an NSView* on macOS,
     * an SDL_Window* under SDL. GDI passes these through to the platform layer
     * and never dereferences them; the Qt bridge produces one from a QWindow*.
     */
    struct WindowHandle
    {
        void* Native = nullptr;

        WindowHandle() = default;
        explicit WindowHandle(void* InNative) : Native(InNative) {}

        bool IsValid() const { return Native != nullptr; }
    };

    /*
     * 8 bits per channel, deliberately NOT float.
     *
     * QColor is 8-bit per channel. A float colour here would round-trip
     * imprecisely through clear colour and the eyedropper, and the eyedropper
     * is specified to match a sampled pixel EXACTLY, not approximately.
     */
    struct Color
    {
        uint8_t R = 0;
        uint8_t G = 0;
        uint8_t B = 0;
        uint8_t A = 255;

        Color() = default;
        Color(uint8_t InR, uint8_t InG, uint8_t InB, uint8_t InA = 255)
            : R(InR), G(InG), B(InB), A(InA) {}

        /* Normalized accessors, for shader uniforms and GL clear colour. */
        float Rf() const { return float(R) / 255.0f; }
        float Gf() const { return float(G) / 255.0f; }
        float Bf() const { return float(B) / 255.0f; }
        float Af() const { return float(A) / 255.0f; }

        friend bool operator==(const Color& L, const Color& R2)
        {
            return L.R == R2.R && L.G == R2.G && L.B == R2.B && L.A == R2.A;
        }
        friend bool operator!=(const Color& L, const Color& R2) { return !(L == R2); }
    };

    struct Point
    {
        int X = 0;
        int Y = 0;

        Point() = default;
        Point(int InX, int InY) : X(InX), Y(InY) {}

        friend bool operator==(const Point& L, const Point& R) { return L.X == R.X && L.Y == R.Y; }
        friend bool operator!=(const Point& L, const Point& R) { return !(L == R); }
    };

    struct Size
    {
        int Width  = 0;
        int Height = 0;

        Size() = default;
        Size(int InWidth, int InHeight) : Width(InWidth), Height(InHeight) {}

        bool IsEmpty() const { return Width <= 0 || Height <= 0; }

        friend bool operator==(const Size& L, const Size& R) { return L.Width == R.Width && L.Height == R.Height; }
        friend bool operator!=(const Size& L, const Size& R) { return !(L == R); }
    };

    /*
     * Stored as origin + extent.
     *
     * NOTE THE QRect CONVENTION: Right() and Bottom() are INCLUSIVE, i.e.
     * Right() == X + Width - 1, matching QRect::right()/bottom() exactly. This
     * is the single most common off-by-one when converting between the two, so
     * the accessors reproduce Qt's definition rather than the arguably saner
     * exclusive one. If you want the exclusive edge, use X + Width directly.
     */
    struct Rect
    {
        int X      = 0;
        int Y      = 0;
        int Width  = 0;
        int Height = 0;

        Rect() = default;
        Rect(int InX, int InY, int InWidth, int InHeight)
            : X(InX), Y(InY), Width(InWidth), Height(InHeight) {}

        int Left()   const { return X; }
        int Top()    const { return Y; }
        int Right()  const { return X + Width  - 1; }   // inclusive, as QRect
        int Bottom() const { return Y + Height - 1; }   // inclusive, as QRect

        bool IsEmpty() const { return Width <= 0 || Height <= 0; }

        bool Contains(const Point& InPoint) const
        {
            return InPoint.X >= X && InPoint.X <= Right()
                && InPoint.Y >= Y && InPoint.Y <= Bottom();
        }

        friend bool operator==(const Rect& L, const Rect& R)
        {
            return L.X == R.X && L.Y == R.Y && L.Width == R.Width && L.Height == R.Height;
        }
        friend bool operator!=(const Rect& L, const Rect& R) { return !(L == R); }
    };
}

#endif //ENGINECORE_CORETYPES_H

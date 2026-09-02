#include "Public/Core/CoreImage.h"

#include <algorithm>
#include <cstring>

#include "Public/Core/CoreLog.h"

namespace EngineCore
{
int BytesPerPixel(const EImageFormat InFormat)
{
    switch(InFormat)
    {
        case EImageFormat::BGRA8:
        case EImageFormat::RGBA8:   return 4;
        case EImageFormat::Gray8:   return 1;
        case EImageFormat::None:    return 0;
    }
    return 0;
}

Image::Image(const int InWidth, const int InHeight, const EImageFormat InFormat)
{
    if(InWidth <= 0 || InHeight <= 0 || InFormat == EImageFormat::None)
    {
        return;
    }

    Width  = InWidth;
    Height = InHeight;
    Format = InFormat;
    Pixels.assign(size_t(Width) * size_t(Height) * size_t(BytesPerPixel(Format)), 0);
}

Image Image::CopyFrom(const uint8_t* InSrc,
                      const int InWidth,
                      const int InHeight,
                      const EImageFormat InFormat,
                      const size_t InSrcStride)
{
    Image Out(InWidth, InHeight, InFormat);
    if(Out.IsNull() || InSrc == nullptr)
    {
        return Out;
    }

    const size_t DstStride = Out.GetStride();
    const size_t SrcStride = (InSrcStride == 0) ? DstStride : InSrcStride;

    CORE_CHECK(SrcStride >= DstStride, "Image::CopyFrom: source stride is narrower than one packed row")

    // Row by row, never one big memcpy -- the source may be padded even when
    // the destination is not. See the tight-packing note in CoreImage.h.
    for(int Y = 0; Y < InHeight; ++Y)
    {
        std::memcpy(Out.Pixels.data() + size_t(Y) * DstStride,
                    InSrc            + size_t(Y) * SrcStride,
                    DstStride);
    }

    return Out;
}

uint8_t* Image::GetScanline(const int InY)
{
    if(InY < 0 || InY >= Height || Pixels.empty())
    {
        return nullptr;
    }
    return Pixels.data() + size_t(InY) * GetStride();
}

const uint8_t* Image::GetScanline(const int InY) const
{
    if(InY < 0 || InY >= Height || Pixels.empty())
    {
        return nullptr;
    }
    return Pixels.data() + size_t(InY) * GetStride();
}

Image Image::ConvertedTo(const EImageFormat InFormat) const
{
    if(IsNull() || InFormat == EImageFormat::None)
    {
        return Image();
    }

    if(InFormat == Format)
    {
        return *this;
    }

    const bool bSwizzle32 =
        (Format   == EImageFormat::BGRA8 && InFormat == EImageFormat::RGBA8) ||
        (Format   == EImageFormat::RGBA8 && InFormat == EImageFormat::BGRA8);

    if(!bSwizzle32)
    {
        // Deliberately a null result rather than a wrong-channel-order one:
        // an unsupported conversion is a programming error and should be
        // loud, not a subtly red/blue-swapped image.
        CORE_LOG_ERROR("Image::ConvertedTo: unsupported format conversion")
        return Image();
    }

    Image Out(Width, Height, InFormat);

    // BGRA <-> RGBA is the same operation in both directions: swap bytes 0
    // and 2, leave green and alpha alone.
    for(int Y = 0; Y < Height; ++Y)
    {
        const uint8_t* Src = GetScanline(Y);
        uint8_t*       Dst = Out.GetScanline(Y);
        for(int X = 0; X < Width; ++X)
        {
            const size_t Idx = size_t(X) * 4u;
            Dst[Idx + 0] = Src[Idx + 2];
            Dst[Idx + 1] = Src[Idx + 1];
            Dst[Idx + 2] = Src[Idx + 0];
            Dst[Idx + 3] = Src[Idx + 3];
        }
    }

    return Out;
}

void Image::FlipVertical()
{
    if(Pixels.empty()) return;

    const size_t Stride = GetStride();
    std::vector<uint8_t> Scratch(Stride);

    for(int Y = 0; Y < Height / 2; ++Y)
    {
        uint8_t* Top    = Pixels.data() + size_t(Y) * Stride;
        uint8_t* Bottom = Pixels.data() + size_t(Height - 1 - Y) * Stride;

        std::memcpy(Scratch.data(), Top,           Stride);
        std::memcpy(Top,            Bottom,        Stride);
        std::memcpy(Bottom,         Scratch.data(), Stride);
    }
}

void Image::FlipHorizontal()
{
    if(Pixels.empty()) return;

    const int    Bpp    = BytesPerPixel(Format);
    const size_t Stride = GetStride();

    for(int Y = 0; Y < Height; ++Y)
    {
        uint8_t* Row = Pixels.data() + size_t(Y) * Stride;
        for(int X = 0; X < Width / 2; ++X)
        {
            uint8_t* Left  = Row + size_t(X) * size_t(Bpp);
            uint8_t* Right = Row + size_t(Width - 1 - X) * size_t(Bpp);
            std::swap_ranges(Left, Left + Bpp, Right);
        }
    }
}
}

#ifndef CORECLIENT_GDIHALF_H
#define CORECLIENT_GDIHALF_H

#include <cstdint>
#include <cstring>

/*
 * IEEE half floats, for reading an RGBA16F render target back (ToImage) on
 * every backend: each channel to a float, then clamped into RGBA8 -- what the
 * texels hold, not tone mapped.
 */
inline float GDIHalfToFloat(const uint16_t InHalf)
{
    const uint32_t Sign = uint32_t(InHalf & 0x8000u) << 16;
    const uint32_t Exp  = (InHalf >> 10) & 0x1Fu;
    const uint32_t Man  = InHalf & 0x3FFu;
    if(Exp == 0)
    {
        const float Value = float(Man) / 1024.0f / 16384.0f;   // subnormal: man * 2^-24
        return Sign ? -Value : Value;
    }
    const uint32_t Bits = Exp == 31 ? (Sign | 0x7F800000u | (Man << 13)) : (Sign | ((Exp + 112u) << 23) | (Man << 13));
    float Value;
    std::memcpy(&Value, &Bits, sizeof(Value));
    return Value;
}

/* InCount half floats to bytes, each clamped to 0-1 and scaled to 0-255. */
inline void GDIHalvesToBytes(const uint16_t* InHalves, uint8_t* OutBytes, const size_t InCount)
{
    for(size_t Each = 0; Each < InCount; ++Each)
    {
        const float Value = GDIHalfToFloat(InHalves[Each]);
        OutBytes[Each] = uint8_t(Value <= 0.0f ? 0 : Value >= 1.0f ? 255 : int(Value * 255.0f + 0.5f));
    }
}

#endif //CORECLIENT_GDIHALF_H

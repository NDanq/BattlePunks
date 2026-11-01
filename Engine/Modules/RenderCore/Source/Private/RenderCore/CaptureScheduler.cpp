#include "Private/RenderCore/CaptureScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RenderCore
{
    namespace
    {
        /* FNV-1a over bytes. */
        struct Hasher
        {
            uint64_t Value = 1469598103934665603ull;
            void Bytes(const void* InData, const size_t InSize)
            {
                const auto* Each = static_cast<const uint8_t*>(InData);
                for(size_t Index = 0; Index < InSize; ++Index) { Value = (Value ^ Each[Index]) * 1099511628211ull; }
            }
            void Int(const int64_t InValue) { Bytes(&InValue, sizeof(InValue)); }
            void Color(const EngineCore::Color InColor) { const uint8_t Rgb[3] = { InColor.R, InColor.G, InColor.B }; Bytes(Rgb, 3); }
            void Percent(const float InValue) { Int(int64_t(std::llround(double(InValue) * 100.0))); }
            void Unit(const double InValue) { Int(int64_t(std::llround(InValue))); }
        };
    }

    float CaptureFaceUnits(const uint32_t InFace)
    {
        return 1.0f + float(double(InFace) * double(InFace) / 65536.0);
    }

    float CaptureBudget::GetAllowance(const float InBudgetMs, const float InMinimum) const
    {
        const float Unit = std::max(std::max(UnitMs, UnitCpuMs), 1e-3f);
        return std::max(InMinimum, std::max(InBudgetMs, 0.0f) / Unit);
    }

    void CaptureBudget::Record(const uint64_t InSerial, const float InUnits)
    {
        Serials[InSerial % RING] = InSerial;
        Units[InSerial % RING]   = InUnits;
    }

    void CaptureBudget::Measured(const uint64_t InSerial, const float InGpuMs)
    {
        const uint32_t Slot = uint32_t(InSerial % RING);
        if(InSerial == 0 || Serials[Slot] != InSerial || !(Units[Slot] >= 0.5f) || !(InGpuMs >= 0.0f))
        {
            return;
        }
        const float Sample = InGpuMs / Units[Slot];
        UnitMs    = bMeasured ? UnitMs + (Sample - UnitMs) * 0.25f : Sample;
        UnitMs    = std::clamp(UnitMs, 1e-3f, 100.0f);
        bMeasured = true;
        Units[Slot] = 0.0f;   // once
    }

    void CaptureBudget::MeasuredCpu(const float InUnits, const float InCpuMs)
    {
        if(!(InUnits >= 0.5f) || !(InCpuMs >= 0.0f))
        {
            return;
        }
        const float Sample = InCpuMs / InUnits;
        UnitCpuMs = UnitCpuMs > 0.0f ? UnitCpuMs + (Sample - UnitCpuMs) * 0.25f : Sample;
        UnitCpuMs = std::clamp(UnitCpuMs, 0.0f, 100.0f);
    }

    void CaptureBudget::Reset()
    {
        *this = CaptureBudget();
    }

    bool CaptureConvergence::Observe(const uint64_t InSignature)
    {
        if(bKnown && InSignature == Signature)
        {
            return false;
        }
        Signature   = InSignature;
        SinceChange = 0;
        bKnown      = true;
        return true;
    }

    bool CaptureConvergence::IsConverged(const uint64_t InTotal, const uint32_t InRounds) const
    {
        return bKnown && InRounds > 0 && SinceChange >= InTotal * uint64_t(InRounds);
    }

    uint64_t CaptureLightingSignature(const RenderLighting& InLighting, const float InSunStepDegrees)
    {
        Hasher Hash;
        const double Step = std::max(double(InSunStepDegrees), 0.01) * 3.14159265358979 / 180.0;

        /* The sun: its direction as azimuth and elevation, each in steps. */
        const double Length = InLighting.LightDirection.GetLength();
        if(Length > 1e-9)
        {
            const double X = InLighting.LightDirection.GetX() / Length;
            const double Y = InLighting.LightDirection.GetY() / Length;
            const double Z = InLighting.LightDirection.GetZ() / Length;
            Hash.Int(int64_t(std::floor(std::asin(std::clamp(Y, -1.0, 1.0)) / Step)));
            /* Straight up or down, the azimuth means nothing. */
            Hash.Int(std::abs(Y) > 0.9999 ? 0 : int64_t(std::floor(std::atan2(Z, X) / Step)));
        }
        Hash.Color(InLighting.LightColor);
        Hash.Percent(InLighting.LightIntensity);
        Hash.Int(InLighting.bLightCastShadows);

        Hash.Color(InLighting.SkyColor);
        Hash.Color(InLighting.GroundColor);
        Hash.Percent(InLighting.SkyIntensity);
        Hash.Bytes(InLighting.SkyEnvironment, std::strlen(InLighting.SkyEnvironment));
        Hash.Percent(InLighting.SkyEnvironmentIntensity);
        Hash.Percent(InLighting.SkyEnvironmentRotation);

        Hash.Int(InLighting.Shadows.bEnabled);
        Hash.Int(InLighting.Shadows.Resolution);

        /* The local lights, summed so their order does not count. */
        uint64_t Locals = 0;
        const uint32_t Count = std::min<uint32_t>(InLighting.LocalCount, RENDER_LOCAL_LIGHT_MAX);
        for(uint32_t Index = 0; Index < Count; ++Index)
        {
            const RenderLocalLight& Light = InLighting.Locals[Index];
            Hasher Each;
            Each.Unit(Light.Position.GetX()); Each.Unit(Light.Position.GetY()); Each.Unit(Light.Position.GetZ());
            Each.Percent(float(Light.Direction.GetX())); Each.Percent(float(Light.Direction.GetY())); Each.Percent(float(Light.Direction.GetZ()));
            Each.Color(Light.Color);
            Each.Percent(Light.Intensity);
            Each.Unit(Light.Range);
            Each.Int(Light.Type);
            Each.Percent(Light.InnerConeAngle); Each.Percent(Light.OuterConeAngle);
            Each.Unit(Light.Width); Each.Unit(Light.Height); Each.Percent(Light.SourceRadius);
            Each.Int(Light.bCastShadows);
            Locals += Each.Value;
        }
        Hash.Int(int64_t(Count));
        Hash.Bytes(&Locals, sizeof(Locals));
        return Hash.Value;
    }
}

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/LightShadow.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/TimeOfDay/TimeOfDay.h"
#include "Public/TimeOfDay/TimeOfDayComponent.h"

/* The sky: the atmosphere on the CPU (for the lights' colours) and the pass
 * that draws it with TimeOfDaySky.fsh. The constants match the shader's. */

namespace TimeOfDay
{
    namespace
    {
        constexpr float Pi            = 3.14159265f;
        constexpr float PlanetRadius  = 6360e3f;
        constexpr float AtmosRadius   = 6420e3f;
        constexpr float RayleighScale = 8000.0f;
        constexpr float MieScale      = 1200.0f;
        constexpr float RayleighBeta[3] = { 5.8e-6f, 13.5e-6f, 33.1e-6f };
        constexpr float MieBetaBase   = 21e-6f;
        constexpr float MieG          = 0.76f;
        constexpr float OzoneBeta[3]  = { 0.650e-6f, 1.881e-6f, 0.085e-6f };
        constexpr int   ViewSteps     = 16;
        constexpr int   LightSteps    = 6;

        /* What the shader is handed: TOD_Sky, TimeOfDaySky.fsh. */
        constexpr float SkySunIntensity  = 20.0f;
        constexpr float SkyMoonRatio     = 0.006f;
        constexpr float SkyMoonIntensity = SkySunIntensity * SkyMoonRatio;
        constexpr float SkyExposure      = 2.0f;
        /* The eye's adaptation: the exposure rises as the zenith dims, toward
         * AdaptTarget, by AdaptMax at most -- so a sunset and the blue hour
         * read, and night stays night. */
        constexpr float AdaptTarget      = 0.12f;
        constexpr float AdaptMax         = 8.0f;
        constexpr float EyeHeight        = 200.0f;   // m above the sea, for the atmosphere only

        struct V3
        {
            float X = 0.0f, Y = 0.0f, Z = 0.0f;
        };
        V3    Add(const V3& A, const V3& B)     { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
        V3    Scale(const V3& A, const float S) { return { A.X * S, A.Y * S, A.Z * S }; }
        float Dot(const V3& A, const V3& B)     { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
        float Length(const V3& A)               { return std::sqrt(Dot(A, A)); }
        V3    Normal(const V3& A)               { const float L = Length(A); return L > 0.0f ? Scale(A, 1.0f / L) : V3{ 0.0f, 1.0f, 0.0f }; }
        V3    From(const EngineCore::Vector3& InV) { return { float(InV.GetX()), float(InV.GetY()), float(InV.GetZ()) }; }

        float SphereFar(const V3& InOrigin, const V3& InDir, const float InRadius)
        {
            const float B = Dot(InOrigin, InDir);
            const float C = Dot(InOrigin, InOrigin) - InRadius * InRadius;
            const float D = B * B - C;
            return D < 0.0f ? -1.0f : -B + std::sqrt(D);
        }

        float Ozone(const float InHeight) { return (std::max)(0.0f, 1.0f - std::fabs(InHeight - 25000.0f) / 15000.0f); }

        /* Rayleigh, Mie and ozone optical depth from InPoint toward InLight;
         * false where the planet is in the way. */
        bool LightDepth(const V3& InPoint, const V3& InLight, float& OutR, float& OutM, float& OutO)
        {
            const float Step = SphereFar(InPoint, InLight, AtmosRadius) / float(LightSteps);
            OutR = OutM = OutO = 0.0f;
            for(int Index = 0; Index < LightSteps; ++Index)
            {
                const V3    P      = Add(InPoint, Scale(InLight, (float(Index) + 0.5f) * Step));
                const float Height = Length(P) - PlanetRadius;
                if(Height < 0.0f) { return false; }
                OutR += std::exp(-Height / RayleighScale) * Step;
                OutM += std::exp(-Height / MieScale) * Step;
                OutO += Ozone(Height) * Step;
            }
            return true;
        }

        float Luma(const SkyRGB& InRGB) { return 0.2126f * InRGB.R + 0.7152f * InRGB.G + 0.0722f * InRGB.B; }

        float PhaseRayleigh(const float InCos) { return 3.0f / (16.0f * Pi) * (1.0f + InCos * InCos); }
        float PhaseMie(const float InCos)
        {
            const float G2 = MieG * MieG;
            return 3.0f / (8.0f * Pi) * ((1.0f - G2) * (1.0f + InCos * InCos))
                 / ((2.0f + G2) * std::pow(1.0f + G2 - 2.0f * MieG * InCos, 1.5f));
        }

        V3 Origin() { return { 0.0f, PlanetRadius + EyeHeight, 0.0f }; }

        EngineCore::Color ToColor(const SkyRGB& InRGB, const float InBrightness)
        {
            const float Max = (std::max)((std::max)(InRGB.R, InRGB.G), (std::max)(InRGB.B, 1e-9f));
            const auto Byte = [&](const float InValue)
            {
                return uint8_t((std::min)(255.0f, std::round(InValue / Max * InBrightness * 255.0f)));
            };
            return EngineCore::Color(Byte(InRGB.R), Byte(InRGB.G), Byte(InRGB.B));
        }

        /* How much brighter the eye sees a sky with the sun toward InSun
         * than the noon one: AdaptTarget over the zenith's brightness, sun
         * and moon together, 1 to AdaptMax. */
        float Adaptation(const V3& InSun, const float InHaze)
        {
            const EngineCore::Vector3 Up(0.0f, 1.0f, 0.0f);
            const EngineCore::Vector3 Sun(InSun.X, InSun.Y, InSun.Z);
            const EngineCore::Vector3 Moon(-InSun.X, -InSun.Y, -InSun.Z);
            const float Zenith = Luma(EvaluateSky(Up, Sun, InHaze)) + SkyMoonRatio * Luma(EvaluateSky(Up, Moon, InHaze));
            return (std::min)(AdaptMax, (std::max)(1.0f, AdaptTarget / (Zenith + 1e-4f)));
        }

        /* The pass: the first clock's sky, extracted on the game thread. */
        struct SkyState
        {
            bool                         bDraw = false;
            RenderCore::RenderPassParams Params;
        };

        class SkyPass final : public RenderCore::CustomRenderPass
        {
        public:
            explicit SkyPass(GameCore::World& InWorld) : World(&InWorld) {}

            const char* GetName() const override { return "TimeOfDaySky"; }

            void Extract() override
            {
                SkyState State;
                World->Each<TimeOfDayComponent>([&State](GameCore::Entity, TimeOfDayComponent& InClock)
                {
                    if(State.bDraw || !InClock.bDrawSky) { return; }
                    State.bDraw = true;
                    const V3 Sun = From(SunDirection(InClock.Hour, InClock.NorthYaw, InClock.Latitude));
                    float* M = State.Params.Matrix;   // column-major: TOD_Sky[n] is M[4n..4n+3]
                    M[0]  = Sun.X;  M[1]  = Sun.Y;  M[2]  = Sun.Z;  M[3]  = SkySunIntensity;
                    M[4]  = -Sun.X; M[5]  = -Sun.Y; M[6]  = -Sun.Z; M[7]  = SkyMoonIntensity;
                    M[8]  = (std::max)(InClock.Haze, 0.0f);
                    M[9]  = std::cos((std::max)(InClock.SunDiscSize, 0.01f) * Pi / 180.0f);
                    M[10] = (std::max)(InClock.StarBrightness, 0.0f);
                    M[11] = SkyExposure * Adaptation(Sun, M[8]) * (std::max)(InClock.SkyBrightness, 0.0f);
                    M[12] = 0.3f;   M[13] = 0.28f;  M[14] = 0.25f;  M[15] = EyeHeight;
                });
                Mailbox.Put(State);
            }

            void Execute(RenderCore::RenderPassContext& InContext) override
            {
                const SkyState State = Mailbox.Get();
                if(!State.bDraw)
                {
                    return;
                }
                InContext.DrawFullscreen("TimeOfDaySky.fsh", RenderCore::RenderPassTarget{}, {}, State.Params,
                                         RenderCore::ERenderPassBlend::Opaque);
            }

        private:
            GameCore::World*                         World = nullptr;
            RenderCore::RenderPassMailbox<SkyState>  Mailbox;
        };

        /* Game thread only, as World hooks are. */
        std::map<const GameCore::World*, RenderCore::RenderPassHandle>& SkyPasses()
        {
            static std::map<const GameCore::World*, RenderCore::RenderPassHandle> Passes;
            return Passes;
        }
    }

    EngineCore::Vector3 SunDirection(const float InHour, const float InNorthYaw, const float InLatitude)
    {
        float Hour = std::fmod(InHour, 24.0f);
        if(Hour < 0.0f) { Hour += 24.0f; }
        /* Its arc across the whole day: 0 rising (6), 0.5 noon, 1 setting
         * (18), below the horizon either side. */
        const float Across    = (Hour - 6.0f) / 12.0f;
        const float Peak      = (std::max)(10.0f, 90.0f - std::fabs(InLatitude));
        const float Elevation = Peak * std::sin(Across * Pi);
        const float Yaw       = InNorthYaw - 90.0f + Across * 180.0f;
        const EngineCore::Vector3 Travel = RenderCore::LightForward(EngineCore::Vector3(Elevation, Yaw, 0.0f));
        return EngineCore::Vector3(-Travel.GetX(), -Travel.GetY(), -Travel.GetZ());
    }

    SkyRGB EvaluateSky(const EngineCore::Vector3& InDir, const EngineCore::Vector3& InSun, const float InHaze)
    {
        V3 Dir = From(InDir);
        Dir.Y  = (std::max)(Dir.Y, 0.002f);
        Dir    = Normal(Dir);
        const V3    Sun     = Normal(From(InSun));
        const V3    Eye     = Origin();
        const float MieBeta = MieBetaBase * InHaze;
        const float Step    = SphereFar(Eye, Dir, AtmosRadius) / float(ViewSteps);

        float ViewR = 0.0f, ViewM = 0.0f, ViewO = 0.0f;
        float SumR[3] = {}, SumM[3] = {};
        for(int Index = 0; Index < ViewSteps; ++Index)
        {
            const V3    P      = Add(Eye, Scale(Dir, (float(Index) + 0.5f) * Step));
            const float Height = (std::max)(Length(P) - PlanetRadius, 0.0f);
            const float DR     = std::exp(-Height / RayleighScale) * Step;
            const float DM     = std::exp(-Height / MieScale) * Step;
            ViewR += DR;
            ViewM += DM;
            ViewO += Ozone(Height) * Step;

            float SunR = 0.0f, SunM = 0.0f, SunO = 0.0f;
            if(!LightDepth(P, Sun, SunR, SunM, SunO)) { continue; }
            for(int C = 0; C < 3; ++C)
            {
                const float Atten = std::exp(-(RayleighBeta[C] * (ViewR + SunR) + MieBeta * 1.1f * (ViewM + SunM)
                                               + OzoneBeta[C] * (ViewO + SunO)));
                SumR[C] += Atten * DR;
                SumM[C] += Atten * DM;
            }
        }
        const float Cos = Dot(Dir, Sun);
        float Out[3];
        for(int C = 0; C < 3; ++C)
        {
            Out[C] = SkySunIntensity * (SumR[C] * RayleighBeta[C] * PhaseRayleigh(Cos) + SumM[C] * MieBeta * PhaseMie(Cos));
        }
        SkyRGB Result;
        Result.R = Out[0];
        Result.G = (std::min)(Out[1], (std::max)(Out[0], Out[2]));   // as the shader: no green horizon
        Result.B = Out[2];
        return Result;
    }

    SkyRGB SunTransmittance(const EngineCore::Vector3& InSun, const float InHaze)
    {
        SkyRGB Result;
        float R = 0.0f, M = 0.0f, O = 0.0f;
        if(!LightDepth(Origin(), Normal(From(InSun)), R, M, O))
        {
            return Result;
        }
        const float MieBeta = MieBetaBase * InHaze;
        Result.R = std::exp(-(RayleighBeta[0] * R + MieBeta * 1.1f * M + OzoneBeta[0] * O));
        Result.G = std::exp(-(RayleighBeta[1] * R + MieBeta * 1.1f * M + OzoneBeta[1] * O));
        Result.B = std::exp(-(RayleighBeta[2] * R + MieBeta * 1.1f * M + OzoneBeta[2] * O));
        return Result;
    }

    void AttachSky(GameCore::World& InWorld)
    {
        if(SkyPasses().count(&InWorld) != 0)
        {
            return;
        }
        /* First thing after the background: every surface draws over it. */
        const RenderCore::RenderPassHandle Handle =
            RenderCore::RegisterCustomRenderPass(&InWorld, std::make_shared<SkyPass>(InWorld), RenderCore::ERenderHook::BeforeScene, -100);
        if(Handle != 0)
        {
            SkyPasses()[&InWorld] = Handle;
        }
    }

    void DetachSky(GameCore::World& InWorld)
    {
        const auto Found = SkyPasses().find(&InWorld);
        if(Found == SkyPasses().end())
        {
            return;
        }
        RenderCore::UnregisterCustomRenderPass(Found->second);
        SkyPasses().erase(Found);
    }

    /* The lights' colours from the atmosphere: TimeOfDay.cpp's Tick, for a
     * clock with bDrawSky. By day the sun's is what reaches the ground; the
     * Sky Light's sky colour the zenith's and a ring round it, its ground the
     * horizon's off grey ground. Night keeps the preset's moonlight. */
    void SkyLightColors(const TimeOfDayComponent& InClock, EngineCore::Color& InOutSun, EngineCore::Color& OutSky,
                        EngineCore::Color& OutGround, bool& bOutDay)
    {
        const EngineCore::Vector3 Sun = SunDirection(InClock.Hour, InClock.NorthYaw, InClock.Latitude);
        const float Haze = (std::max)(InClock.Haze, 0.0f);

        const SkyRGB Through = SunTransmittance(Sun, Haze);
        bOutDay = Sun.GetY() > -0.02f && (Through.R + Through.G + Through.B) > 1e-4f;
        if(bOutDay)
        {
            InOutSun = ToColor(Through, 1.0f);
        }

        /* Lit by the sun by day; by night the moon, opposite, which the
         * model lights the same way only dimmer -- the colour is what counts. */
        const EngineCore::Vector3 Light = bOutDay ? Sun : EngineCore::Vector3(-Sun.GetX(), -Sun.GetY(), -Sun.GetZ());
        SkyRGB Up = EvaluateSky(EngineCore::Vector3(0.0f, 1.0f, 0.0f), Light, Haze);
        SkyRGB Horizon;
        for(int Index = 0; Index < 4; ++Index)
        {
            const float Angle = float(Index) * Pi * 0.5f;
            const SkyRGB Ring = EvaluateSky(EngineCore::Vector3(std::cos(Angle), 0.35f, std::sin(Angle)), Light, Haze);
            const SkyRGB Low  = EvaluateSky(EngineCore::Vector3(std::cos(Angle), 0.02f, std::sin(Angle)), Light, Haze);
            Up.R += Ring.R; Up.G += Ring.G; Up.B += Ring.B;
            Horizon.R += Low.R; Horizon.G += Low.G; Horizon.B += Low.B;
        }
        OutSky = ToColor(Up, 1.0f);
        Horizon.R *= 0.30f; Horizon.G *= 0.28f; Horizon.B *= 0.25f;   // the ground's albedo, as the shader's
        OutGround = ToColor(Horizon, 0.45f);
    }
}

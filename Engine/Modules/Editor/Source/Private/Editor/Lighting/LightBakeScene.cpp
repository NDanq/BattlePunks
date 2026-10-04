#if WITH_EDITOR

#include "Private/Editor/Lighting/LightBakeScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace LightBake
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;

        /* FNV-1a, 64-bit. */
        struct Hasher
        {
            uint64_t Value = 1469598103934665603ull;

            void Bytes(const void* InData, const size_t InSize)
            {
                const auto* Data = static_cast<const uint8_t*>(InData);
                for(size_t At = 0; At < InSize; ++At)
                {
                    Value ^= Data[At];
                    Value *= 1099511628211ull;
                }
            }
            template<typename T> void Add(const T& InValue) { Bytes(&InValue, sizeof(T)); }
            void Add(const Vec3& InValue) { Add(InValue.X); Add(InValue.Y); Add(InValue.Z); }
        };
    }

    Vec3 Normalize(const Vec3& InV)
    {
        const float Length = std::sqrt(Dot(InV, InV));
        return Length > 1e-20f ? InV * (1.0f / Length) : Vec3(0.0f, 1.0f, 0.0f);
    }

    Vec3 Sky::Radiance(const Vec3& InDirection) const
    {
        if(!bPresent) { return Vec3(); }
        if(Environment.empty() || Width <= 0 || Height <= 0)
        {
            const float Up = std::clamp(InDirection.Y * 0.5f + 0.5f, 0.0f, 1.0f);
            return GroundColor * (1.0f - Up) + SkyColor * Up;
        }

        /* IBL_SampleEquirect's mapping: +Y at the top row. */
        float U = std::atan2(InDirection.X, InDirection.Z) / (2.0f * PI) + 0.5f + Rotation / (2.0f * PI);
        U -= std::floor(U);
        const float V = std::acos(std::clamp(InDirection.Y, -1.0f, 1.0f)) / PI;
        const int X = std::clamp(int(U * float(Width)), 0, Width - 1);
        const int Y = std::clamp(int(V * float(Height)), 0, Height - 1);
        const float* Texel = &Environment[(size_t(Y) * size_t(Width) + size_t(X)) * 3];
        return Vec3(Texel[0], Texel[1], Texel[2]);
    }

    void Scene::Finish()
    {
        BoundsMin = BoundsMax = Vec3();
        for(Instance& Each : Instances) { Each.Area = 0.0f; }
        for(size_t Index = 0; Index < Triangles.size(); ++Index)
        {
            const Triangle& T = Triangles[Index];
            if(Index == 0) { BoundsMin = BoundsMax = T.P[0]; }
            for(const Vec3& P : T.P)
            {
                BoundsMin = Min(BoundsMin, P);
                BoundsMax = Max(BoundsMax, P);
            }
            if(T.Instance < Instances.size())
            {
                const Vec3 C = Cross(T.P[1] - T.P[0], T.P[2] - T.P[0]);
                Instances[T.Instance].Area += 0.5f * std::sqrt(Dot(C, C));
            }
        }

        Hasher H;
        H.Add(uint64_t(Triangles.size()));
        for(const Triangle& T : Triangles)
        {
            for(int Corner = 0; Corner < 3; ++Corner)
            {
                H.Add(T.P[Corner]);
                H.Add(T.N[Corner]);
                H.Add(T.UV1[Corner][0]);
                H.Add(T.UV1[Corner][1]);
            }
            H.Add(T.Instance);
            H.Add(T.Albedo);
        }
        H.Add(uint64_t(Instances.size()));
        for(const Instance& Each : Instances)
        {
            H.Add(Each.GuidHi);
            H.Add(Each.GuidLo);
            H.Add(uint8_t(Each.bLightmapped));
        }
        H.Add(uint64_t(Lights.size()));
        for(const Light& Each : Lights)
        {
            H.Add(uint8_t(Each.Type));
            H.Add(Each.Position);
            H.Add(Each.Direction);
            H.Add(Each.Radiance);
            H.Add(Each.Range);
            H.Add(Each.CosOuter);
            H.Add(Each.CosInner);
            H.Add(Each.SourceSize);
        }
        H.Add(uint8_t(SkyLight.bPresent));
        H.Add(SkyLight.SkyColor);
        H.Add(SkyLight.GroundColor);
        H.Add(SkyLight.Rotation);
        H.Add(SkyLight.Width);
        H.Add(SkyLight.Height);
        if(!SkyLight.Environment.empty()) { H.Bytes(SkyLight.Environment.data(), SkyLight.Environment.size() * sizeof(float)); }
        Hash = H.Value;
    }
}

#endif // WITH_EDITOR

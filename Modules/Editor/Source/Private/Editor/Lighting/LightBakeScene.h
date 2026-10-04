#ifndef NIMBLEEDITOR_LIGHTBAKESCENE_H
#define NIMBLEEDITOR_LIGHTBAKESCENE_H

#if WITH_EDITOR

#include <cstdint>
#include <vector>

/*
 * What a light bake sees (LightBaker.h): the static geometry as world-space
 * triangles, the static lights, and the sky -- plain data, made from a World
 * by LightBakeWorld.h's GatherLightBakeScene, or by hand in a test.
 *
 * Colours are linear. Radiances are the renderer's: a light's colour times
 * its intensity (Lighting.glsl's EvaluateLight); the sky's, what Sky.fsh and
 * the hemisphere ambient use.
 */
namespace LightBake
{
    struct Vec3
    {
        float X = 0.0f, Y = 0.0f, Z = 0.0f;

        Vec3() = default;
        constexpr Vec3(const float InX, const float InY, const float InZ) : X(InX), Y(InY), Z(InZ) {}

        Vec3  operator+(const Vec3& B) const { return { X + B.X, Y + B.Y, Z + B.Z }; }
        Vec3  operator-(const Vec3& B) const { return { X - B.X, Y - B.Y, Z - B.Z }; }
        Vec3  operator*(const float S) const { return { X * S, Y * S, Z * S }; }
        Vec3  operator*(const Vec3& B) const { return { X * B.X, Y * B.Y, Z * B.Z }; }
        Vec3& operator+=(const Vec3& B) { X += B.X; Y += B.Y; Z += B.Z; return *this; }
        float operator[](const int InAxis) const { return InAxis == 0 ? X : (InAxis == 1 ? Y : Z); }
    };

    inline float Dot(const Vec3& A, const Vec3& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
    inline Vec3  Cross(const Vec3& A, const Vec3& B) { return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X }; }
    Vec3 Normalize(const Vec3& InV);
    inline Vec3 Min(const Vec3& A, const Vec3& B) { return { A.X < B.X ? A.X : B.X, A.Y < B.Y ? A.Y : B.Y, A.Z < B.Z ? A.Z : B.Z }; }
    inline Vec3 Max(const Vec3& A, const Vec3& B) { return { A.X > B.X ? A.X : B.X, A.Y > B.Y ? A.Y : B.Y, A.Z > B.Z ? A.Z : B.Z }; }

    struct Triangle
    {
        Vec3     P[3];
        Vec3     N[3];          // unit vertex normals
        float    UV1[3][2] = {};
        uint32_t Instance = 0;
        Vec3     Albedo   = Vec3(0.5f, 0.5f, 0.5f);   // diffuse: base colour times (1 - metallic)
        Vec3     Emissive;                            // linear radiance it gives off: it lights the bake
    };

    /* One static RenderComponent: its triangles are Scene::Triangles[First, First + Count). */
    struct Instance
    {
        uint64_t GuidHi = 0;
        uint64_t GuidLo = 0;
        uint32_t First  = 0;
        uint32_t Count  = 0;
        bool     bLightmapped = false;   // its mesh has UV1
        float    Area   = 0.0f;          // world-space surface, for its lightmap's size
    };

    enum class ELightType : uint8_t { Directional, Point, Spot };

    struct Light
    {
        ELightType Type = ELightType::Point;
        Vec3  Position;
        Vec3  Direction = Vec3(0.0f, -1.0f, 0.0f);   // the way it travels (directional, spot)
        Vec3  Radiance  = Vec3(1.0f, 1.0f, 1.0f);    // linear colour times intensity
        float Range     = 0.0f;                      // point, spot: LocalFalloff's
        float CosOuter  = 0.0f;                      // spot
        float CosInner  = 1.0f;                      // spot
        float SourceSize = 0.0f;                     // soft shadows: a sphere's radius, or the sun's angular radius (radians)
        bool  bBakeDirect = true;                    // false: Stationary -- its direct light stays live; only its bounce is baked
    };

    /* Radiance arriving from a direction no geometry blocks: the HDRI if it
     * has one, else the sky colour above the horizon blended to the ground
     * colour below (Sky.fsh's gradient). */
    struct Sky
    {
        bool  bPresent = false;
        bool  bBakeDirect = true;   // false: a Stationary sky -- its live ambient lights surfaces; only its bounce is baked
        Vec3  SkyColor;      // linear, times intensity
        Vec3  GroundColor;   // linear, times intensity
        int   Width = 0, Height = 0;
        std::vector<float> Environment;   // RGB per texel, linear, intensity applied; empty: the gradient
        float Rotation = 0.0f;            // radians about +Y, as IBL_SampleEquirect

        Vec3 Radiance(const Vec3& InDirection) const;
    };

    struct Scene
    {
        std::vector<Triangle> Triangles;
        std::vector<Instance> Instances;
        std::vector<Light>    Lights;
        Sky                   SkyLight;
        Vec3                  BoundsMin, BoundsMax;   // of the triangles
        uint64_t              Hash = 0;               // what the bake is of: Finish's of the above, or a World's (LightBakeWorld.h)

        /* Computes BoundsMin/Max, each instance's Area, and Hash. */
        void Finish();
    };
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_LIGHTBAKESCENE_H

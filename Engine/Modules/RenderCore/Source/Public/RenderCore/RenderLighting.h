#ifndef RENDERCORE_RENDERLIGHTING_H
#define RENDERCORE_RENDERLIGHTING_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/PostProcessStack.h"

/*
 * The scene's lights and its post-process passes, built on the game thread by
 * WorldBridge from the light components, the [Shadows] config section and
 * PostProcessComponent, and sent down by value like RenderView. POD: render
 * commands capture trivially copyable values only.
 */
namespace RenderCore
{
    enum : uint32_t
    {
        RENDER_LOCAL_LIGHT_MAX          = 16,  // point + spot lights a frame, nearest first
        RENDER_SHADOWED_LOCAL_LIGHT_MAX = 4,   // of those, how many may draw shadows (the local atlas's 6x4 tiles)
        RENDER_CASCADE_MAX              = 4,
    };

    enum ERenderLocalLightType : uint8_t
    {
        RLL_Point = 0,
        RLL_Spot  = 1,
    };

    /* A point or spot light, as WorldBridge found it this frame. */
    struct RenderLocalLight
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Direction{ 0.0, 0.0, 1.0 };   // spot: unit, the way it shines
        EngineCore::Color   Color{ 255, 255, 255 };
        float               Intensity      = 1.0f;
        float               Range          = 500.0f;      // point: Radius
        float               InnerConeAngle = 0.0f;        // spot: half-angles, degrees
        float               OuterConeAngle = 0.0f;
        uint8_t             Type           = RLL_Point;

        uint8_t             bCastShadows     = 0;
        uint8_t             ShadowTechnique  = 0;         // ELightShadowTechnique
        EngineCore::Color   ShadowTint{ 40, 50, 80 };
        float               ShadowBias       = 0.002f;
        float               ShadowNormalBias = 1.0f;
    };

    /* The [Shadows] config section (ShadowSettings.cpp), read on the game
     * thread. bEnabled 0 -- the default here -- draws no shadow at all, so a
     * RenderLighting built by hand (the material preview's) has none. */
    struct RenderShadowSettings
    {
        uint8_t  bEnabled          = 0;
        int32_t  Resolution        = 2048;    // each atlas's side, texels
        int32_t  Filter            = 1;       // 0 hard, 1 PCF 3x3, 2 PCF 5x5
        int32_t  MaxShadowedLights = int32_t(RENDER_SHADOWED_LOCAL_LIGHT_MAX);
        float    DistanceScale     = 1.0f;    // multiplies the sun's ShadowDistance
    };

    struct RenderLighting
    {
        /* The direction light travels, world space; need not be unit length.
         * +Y is up, so (0.35, -1, 0.55) shines down and away -- the light
         * Mesh.fsh has, and what a scene with no Directional Light gets. */
        EngineCore::Vector3 LightDirection{ 0.35, -1.0, 0.55 };
        EngineCore::Color   LightColor{ 255, 255, 255 };
        float               LightIntensity = 3.0f;

        /* The directional light's shadow (DirectionalLightComponent). */
        uint8_t             bLightCastShadows    = 0;
        uint8_t             LightShadowTechnique = 0;      // ELightShadowTechnique
        EngineCore::Color   LightShadowTint{ 40, 50, 80 };
        float               LightShadowBias       = 0.0015f;
        float               LightShadowNormalBias = 2.0f;
        float               LightShadowDistance   = 4000.0f;
        int32_t             LightCascadeCount     = 4;
        float               LightCascadeSplitLambda = 0.75f;

        /* Ambient: SkyColor from above (+Y), GroundColor from below. */
        EngineCore::Color   SkyColor{ 120, 130, 150 };
        EngineCore::Color   GroundColor{ 60, 55, 50 };
        float               SkyIntensity = 0.6f;

        /* The Sky Light's HDRI (an RGBE8 equirectangular .nmtexture), empty for
         * none: what image-based lighting takes its sky from, and (with
         * bSkyBackground) what is drawn behind the scene. RENDERPIPELINE.md,
         * "Image-based lighting". */
        char                SkyEnvironment[128] = {};   // RENDER_MATERIAL_PATH_MAX
        float               SkyEnvironmentIntensity = 1.0f;
        float               SkyEnvironmentRotation  = 0.0f;   // degrees about +Y
        uint8_t             bSkyBackground          = 1;

        /* Point and spot lights, shadow casters first, then nearest the eye. */
        RenderLocalLight     Locals[RENDER_LOCAL_LIGHT_MAX];
        uint32_t             LocalCount = 0;

        RenderShadowSettings Shadows;
    };

    /* The frame's post processing: every Post Process volume the camera is
     * in, blended (WorldBridge::BuildPostProcess, BlendPostProcess). */
    struct RenderPostProcess
    {
        PostProcessSettings Settings;
    };
}

#endif //RENDERCORE_RENDERLIGHTING_H

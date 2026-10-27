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
        RENDER_LOCAL_LIGHT_MAX          = 256, // point + spot lights a frame, shadow casters then nearest first: the light list (GLOBAL_ILLUMINATION.md)
        RENDER_SCENE_LOCAL_LIGHT_MAX    = 16,  // of those, the first in the scene block too (Scene_Locals): volumetric fog's, and the shadowed ones'
        RENDER_SHADOWED_LOCAL_LIGHT_MAX = 4,   // of those, how many may draw shadows (the local atlas's 6x4 tiles)
        RENDER_CASCADE_MAX              = 4,
        RENDER_INSET_SHADOW_MAX         = 4,   // per-object sun shadows a view (in the local atlas's spare tiles)
    };

    enum ERenderLocalLightType : uint8_t
    {
        RLL_Point = 0,
        RLL_Spot  = 1,
        RLL_Rect  = 2,   // a Rect Light: Width x Height across Right and Up, facing Direction
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
        /* Area (GLOBAL_ILLUMINATION.md, Phase 3): a point or spot light's sphere,
         * and a rect light's rectangle -- its axes unit, its sizes world units. */
        float               SourceRadius   = 0.0f;
        EngineCore::Vector3 Right{ 1.0, 0.0, 0.0 };
        EngineCore::Vector3 Up{ 0.0, 1.0, 0.0 };
        float               Width          = 0.0f;
        float               Height         = 0.0f;

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
        int32_t  MaxInsetShadows   = int32_t(RENDER_INSET_SHADOW_MAX);   // Render components' inset shadows a view
        int32_t  InsetFilter       = 1;       // theirs, as Filter: their maps are sharp, so smaller
        int32_t  ForwardFilter     = 0;       // forward Lit (translucent) draws', as Filter: each overdrawn layer filters again, so one tap
        /* Sun contact shadows (RENDERPIPELINE.md, "Contact shadows"): a short screen-space march
         * toward the sun from each lit pixel, for the small shadows the shadow maps are too coarse for. */
        uint8_t  bContactShadows   = 0;
        float    ContactLength     = 25.0f;   // how far toward the sun it looks, world units
        float    ContactThickness  = 6.0f;    // how deep a surface in the depth buffer is taken to be, world units
        int32_t  ContactSteps      = 12;      // samples along the march, 4-32
        float    ContactStrength   = 1.0f;    // 0 none, 1 a full shadow where it hits
        uint8_t  bContactLocalLights = 1;     // shadowed point and spot lights march too
    };

    /* One Render component that asked for an inset shadow (Cast Inset
     * Shadow), as RenderScene offers it to the frame's shadow views: its
     * bounding sphere, world space. Rotation-invariant, so turning the
     * caster does not resize its shadow map. */
    struct InsetShadowCaster
    {
        EngineCore::Vector3 Centre;
        float               Radius = 0.0f;
    };

    /* What a world's bake holds of its lightmaps, at most (the baker refuses more). */
    constexpr uint32_t RENDER_BAKED_ATLASES_MAX = 8;

    /*
     * One entity's baked lighting, as WorldBridge works it out each frame
     * from the world's bake (BakedLightingAsset.h) and carries to its proxy:
     * its lightmap's square, or the light probes' blend where it stands.
     * What MaterialDescription.h's engine bytes and BakedLighting.glsl take.
     */
    struct RenderBakedDraw
    {
        uint8_t  Mode  = 0;               // EMaterialBakedLighting: 0 none, 1 lightmap, 2 ambient cube
        uint8_t  Pad[3] = {};
        uint32_t Atlas = 0;               // Lightmap: RenderLighting::BakedAtlases' index
        float    ScaleOffset[4] = {};     // Lightmap: UV1 * xy + zw
        float    Cube[6][3] = {};         // AmbientCube: +X -X +Y -Y +Z -Z, linear
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

        /* Inset shadows' light (RENDERPIPELINE.md, "Inset shadows"): the sun
         * that casts -- the live one above, or, drawing with a bake, the
         * static sun the bake holds (bInsetLightBaked). That one has no live
         * light to take away, so its inset shadows are Modulated by its
         * ShadowTint. bInsetLight 0: no sun casts, no inset shadows. */
        uint8_t             bInsetLight        = 0;
        uint8_t             bInsetLightBaked   = 0;
        EngineCore::Vector3 InsetLightDirection{ 0.35, -1.0, 0.55 };   // the way it travels
        EngineCore::Color   InsetShadowTint{ 40, 50, 80 };
        float               InsetShadowBias       = 0.0015f;
        float               InsetShadowNormalBias = 2.0f;

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

        /* Baked lighting (RENDERPIPELINE.md, "Baked lighting"): whether this
         * view draws with the world's bake -- its static lights then left
         * out above -- whether that bake holds the sky, and its lightmaps by
         * RenderBakedDraw::Atlas (runtime-relative .nmtexture). */
        uint8_t              bBaked    = 0;
        uint8_t              bBakedSky = 0;
        uint8_t              bBakedDirectional = 0;   // its lightmaps carry the light's direction
        uint32_t             BakedAtlasCount = 0;
        char                 BakedAtlases[RENDER_BAKED_ATLASES_MAX][128] = {};   // RENDER_MATERIAL_PATH_MAX
    };

    /* The frame's post processing: every Post Process volume the camera is
     * in, blended (WorldBridge::BuildPostProcess, BlendPostProcess). */
    struct RenderPostProcess
    {
        PostProcessSettings Settings;
    };
}

#endif //RENDERCORE_RENDERLIGHTING_H

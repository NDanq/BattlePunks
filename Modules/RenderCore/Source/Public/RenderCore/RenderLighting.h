#ifndef RENDERCORE_RENDERLIGHTING_H
#define RENDERCORE_RENDERLIGHTING_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"

/*
 * The scene's light and its post-process passes, built on the game thread by
 * WorldBridge from DirectionalLightComponent, SkyLightComponent and
 * PostProcessComponent, and sent down by value like RenderView. POD: render
 * commands capture trivially copyable values only.
 */
namespace RenderCore
{
    struct RenderLighting
    {
        /* The direction light travels, world space; need not be unit length.
         * +Y is up, so (0.35, -1, 0.55) shines down and away -- the light
         * Mesh.fsh has. */
        EngineCore::Vector3 LightDirection{ 0.35, -1.0, 0.55 };
        EngineCore::Color   LightColor{ 255, 255, 255 };
        float               LightIntensity = 3.0f;

        /* Ambient: SkyColor from above (+Y), GroundColor from below. */
        EngineCore::Color   SkyColor{ 120, 130, 150 };
        EngineCore::Color   GroundColor{ 60, 55, 50 };
        float               SkyIntensity = 0.6f;
    };

    enum : uint32_t { RENDER_POST_PROCESS_MAX = 8 };

    /* Active PostProcessComponents, in the order they run (Priority, low first). */
    struct RenderPostProcess
    {
        char     Materials[RENDER_POST_PROCESS_MAX][128] = {};  // RENDER_MATERIAL_PATH_MAX
        uint32_t Count = 0;
    };
}

#endif //RENDERCORE_RENDERLIGHTING_H

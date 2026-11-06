#ifndef RENDERCORE_RENDERPIPELINE_H
#define RENDERCORE_RENDERPIPELINE_H

#include <cstdint>

#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * The stages a frame runs, in order (RENDERPIPELINE.md). Each has a draw
 * policy -- its own array of draws -- that RenderContext fills as proxies
 * draw and issues stage by stage.
 */
namespace RenderCore
{
    enum class ERenderStage : uint8_t
    {
        PrePass = 0,        // culling; occluders' depth and the occlusion queries
        BasePass,           // Opaque and Masked, Lit: the G-buffer
        Shadows,            // Lighting's first part: shadow casters, into the shadow atlases
        Lighting,           // the G-buffer resolve, one full-screen pass
        Unlit,              // Opaque and Masked, Unlit; plain quads; tinted meshes
        Translucent,        // Lit (forward-lit) and Unlit together, back to front
        PostProcess,        // full-screen, in Post Process Priority order
        Count
    };

    RENDERCORE_API const char* GetRenderStageName(ERenderStage InStage);

    /* Where a material's draws go: its Domain and Shading decide. */
    RENDERCORE_API ERenderStage GetMaterialRenderStage(EMaterialDomain InDomain, EMaterialShading InShading);
}

#endif //RENDERCORE_RENDERPIPELINE_H

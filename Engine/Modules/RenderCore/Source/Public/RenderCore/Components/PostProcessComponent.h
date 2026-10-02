#ifndef RENDERCORE_POSTPROCESSCOMPONENT_H
#define RENDERCORE_POSTPROCESSCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/Components/RenderComponent.h"   // RENDER_MATERIAL_PATH_MAX

namespace RenderCore
{
    /* "Post Process". A PostProcess-domain material (a material graph's
     * runtime path) drawn over the whole scene. Every enabled one runs, Priority low first (RENDER_POST_PROCESS_MAX
     * at most); a material of another domain is warned about and skipped. */
    struct RENDERCORE_API PostProcessComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Material[RENDER_MATERIAL_PATH_MAX] = {};
        int32_t Priority = 0;
        uint8_t bEnabled = 1;
    };
}

#endif //RENDERCORE_POSTPROCESSCOMPONENT_H

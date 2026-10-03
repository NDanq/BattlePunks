#ifndef RENDERCORE_RENDERCOMPONENT_H
#define RENDERCORE_RENDERCOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

namespace RenderCore
{
    enum : size_t { RENDER_MATERIAL_PATH_MAX = 128 };  // including the NUL
    enum : size_t { RENDER_MESH_PATH_MAX     = 128 };  // including the NUL
    enum : size_t { RENDER_MATERIAL_SLOTS    = 8 };

    /* "Render". Draws Mesh (a runtime-relative .nmmesh path) if set and
     * loadable, else a unit quad (1 x 1, centred); both sized by the
     * Transform's Scale.
     *
     * Materials are slots, one per mesh section: each names a material
     * graph by its runtime path ("Materials/Glow.material.nmgraph"; the cook
     * makes its permutations from the graph's <gen_code>). Section i draws
     * with slot i, else its own material from the mesh, else DefaultLit; a
     * quad uses slot 0. Each material's Domain and Shading pick the stage it
     * draws in (RENDERPIPELINE.md), so one mesh can draw in several. An
     * unloadable slot is warned once and falls back. */
    struct RENDERCORE_API RenderComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        uint8_t           bVisible = 1;
        char              Materials[RENDER_MATERIAL_SLOTS][RENDER_MATERIAL_PATH_MAX] = {};
        char              Mesh[RENDER_MESH_PATH_MAX] = {};
    };

    /* RenderComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetRenderComponentFactory();
}

#endif //RENDERCORE_RENDERCOMPONENT_H

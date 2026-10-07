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
    enum : size_t { RENDER_MATERIAL_PARAMETERS_MAX     = 8 };    // overrides per component
    enum : size_t { RENDER_MATERIAL_PARAMETER_NAME_MAX = 32 };   // including the NUL

    /* One material parameter set on one entity: the Parameter node named Name
     * in slot Slot's material draws with Value instead of its default. A
     * float uses Value[0], a vec2 the first two, and so on. */
    struct RenderMaterialParameter
    {
        char    Name[RENDER_MATERIAL_PARAMETER_NAME_MAX] = {};   // "" = unused
        uint8_t Slot = 0;
        float   Value[4] = {};
    };

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
     * unloadable slot is warned once and falls back.
     *
     * Parameters are per entity: SetMaterialParameter overrides a slot's
     * material parameter for this entity's draws only, leaving every other
     * user of the material at its defaults. Up to RENDER_MATERIAL_PARAMETERS_MAX;
     * an entry with an empty Name is unused, wherever it is. A name the
     * material does not have is ignored when drawing. The editor lists them
     * under Material Parameters. */
    struct RENDERCORE_API RenderComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        uint8_t           bVisible = 1;
        char              Materials[RENDER_MATERIAL_SLOTS][RENDER_MATERIAL_PATH_MAX] = {};
        char              Mesh[RENDER_MESH_PATH_MAX] = {};
        RenderMaterialParameter Parameters[RENDER_MATERIAL_PARAMETERS_MAX] = {};

        /* Whether its Opaque and Masked sections draw into shadow maps. */
        uint8_t           bCastShadows = 1;

        /* Whether, while casting, it also gets a shadow map of its own from
         * the sun -- live or baked -- fitted round it alone: far sharper
         * than the sun's cascades, and the only shadow a dynamic entity casts
         * from a baked sun (RENDERPIPELINE.md, "Inset shadows"). The frame's
         * largest on screen get one, up to [Shadows] MaxInsetShadows. */
        uint8_t           bCastInsetShadow = 0;

        /* Whether its Opaque sections hide what is behind them from
         * occlusion culling (RENDERPIPELINE.md, "Occlusion culling"): its
         * depth is drawn first, and every other entity whose box it covers
         * is skipped. For large, solid things -- walls, terrain, buildings --
         * not for small or thin ones, which cost more to draw first than
         * they save. An occluder itself is never occlusion culled. */
        uint8_t           bOccluder = 0;

        /* Slot InSlot's material path; "" if unset or out of range. */
        const char* GetMaterial(int32_t InSlot) const;
        /* False if InSlot is out of range. */
        bool        SetMaterial(int32_t InSlot, const char* InMaterial);
        /* How many slots name a material. */
        int32_t     GetNumRegisteredMaterials() const;

        /* Overrides InName in slot InSlot's material for this entity. False
         * if InSlot is out of range, InName is empty or too long, or every
         * override is in use. */
        bool SetMaterialParameter(int32_t InSlot, const char* InName, float InX, float InY, float InZ, float InW);
        /* The override, if set; false (OutValue untouched) if not. */
        bool GetMaterialParameter(int32_t InSlot, const char* InName, float OutValue[4]) const;
        /* Back to the material's default. False if it was not set. */
        bool ClearMaterialParameter(int32_t InSlot, const char* InName);
        /* Overrides in use. */
        int32_t GetMaterialParameterCount() const;
        /* One past the last in use: an editor can leave an empty one between. */
        int32_t GetMaterialParameterSpan() const;
    };

    /* RenderComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetRenderComponentFactory();
}

#endif //RENDERCORE_RENDERCOMPONENT_H

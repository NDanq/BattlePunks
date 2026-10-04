#ifndef NIMBLEEDITOR_LIGHTBAKEWORLD_H
#define NIMBLEEDITOR_LIGHTBAKEWORLD_H

#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Private/Editor/Lighting/LightBaker.h"
#include "Private/Editor/Lighting/LightBakeScene.h"

namespace GameCore { class World; }

/*
 * A World to a bake scene and back to files -- what Build > Bake Lightmaps
 * runs either side of LightBake::Bake. Widget-free.
 *
 * Only static entities are in a bake: a static RenderComponent's mesh, and
 * static directional, point and spot lights and SkyLight -- Stationary ones
 * too, for the light they bounce. Everything dynamic is left to the renderer.
 *
 * A section's albedo and glow are its material's as the renderer draws it
 * (MeasuredMaterials, from BakeLightmaps' GPU pass) when it has them, else its
 * BaseColor parameter, else grey; the entity's BaseColor and Metallic
 * overrides win either way.
 */
namespace LightBake
{
    struct GatherReport
    {
        int  Meshes      = 0;   // static RenderComponents in the bake
        int  Lightmapped = 0;   // of those, with UV1
        int  Lights      = 0;   // static and Stationary lights
        bool bSky        = false;   // a static SkyLight: baked whole
        bool bAnySky     = false;   // a static or Stationary one: something of it is baked
        std::vector<std::string> Warnings;
    };

    /* A material's average base colour and emissive, linear, as the renderer
     * draws it on a quad (Scene_View's base colour and emissive channels). */
    struct MeasuredMaterial
    {
        Vec3 BaseColor = Vec3(0.5f, 0.5f, 0.5f);
        Vec3 Emissive;
    };
    using MeasuredMaterials = std::unordered_map<std::string, MeasuredMaterial>;

    /* Every material a static Render component's sections draw with. */
    std::vector<std::string> ListBakeMaterials(GameCore::World& InWorld);

    /* Fills OutScene (finished: bounds, areas, and Hash -- HashWorld's) from
     * InWorld's static entities and Stationary lights. */
    void GatherScene(GameCore::World& InWorld, Scene& OutScene, GatherReport& OutReport,
                     const MeasuredMaterials* InMaterials = nullptr);

    /* What a bake of InWorld is of, cheaply -- no triangles touched: its
     * baked lighting settings, and each static and Stationary entity's GUID,
     * mobility, place and what its Render component and lights say. Equal to
     * a bake's InputHash while the bake still matches. */
    uint64_t HashWorld(GameCore::World& InWorld);

    /*
     * Writes InResult beside InWorldFile (absolute, a .nmworld): the
     * atlases as Lightmap_<n>.nmtexture (RGBE8) and Lighting.nmlighting in
     * its Lighting folder, removing atlases a smaller bake no longer has.
     * OutBakedData is the .nmlighting's runtime path, for
     * WorldLightingSettings::BakedData. False with OutError.
     */
    bool SaveBake(const std::string& InWorldFile, const Scene& InScene, const Result& InResult, bool bInSkyBaked,
                  std::string& OutBakedData, std::string& OutError);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_LIGHTBAKEWORLD_H

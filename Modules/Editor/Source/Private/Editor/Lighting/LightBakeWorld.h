#ifndef NIMBLEEDITOR_LIGHTBAKEWORLD_H
#define NIMBLEEDITOR_LIGHTBAKEWORLD_H

#if WITH_EDITOR

#include <string>
#include <vector>

#include "Private/Editor/Lighting/LightBaker.h"
#include "Private/Editor/Lighting/LightBakeScene.h"

namespace GameCore { class World; }

/*
 * A World to a bake scene and back to files -- what Build > Bake Lightmaps
 * runs either side of LightBake::Bake. Widget-free.
 *
 * Only static entities are in a bake: a static RenderComponent's mesh (its
 * sections' albedo from their material's BaseColor and Metallic, overrides
 * included), and static directional, point and spot lights and SkyLight.
 * Everything dynamic is left to the renderer.
 */
namespace LightBake
{
    struct GatherReport
    {
        int  Meshes      = 0;   // static RenderComponents in the bake
        int  Lightmapped = 0;   // of those, with UV1
        int  Lights      = 0;   // static lights
        bool bSky        = false;
        std::vector<std::string> Warnings;
    };

    /* Fills OutScene (finished: bounds, areas, hash) from InWorld's static entities. */
    void GatherScene(GameCore::World& InWorld, Scene& OutScene, GatherReport& OutReport);

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

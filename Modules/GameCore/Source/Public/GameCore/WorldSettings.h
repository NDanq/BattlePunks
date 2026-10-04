#ifndef GAMECORE_WORLDSETTINGS_H
#define GAMECORE_WORLDSETTINGS_H

#include <cstdint>
#include <string>

#include "Public/GameCore/GameCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

namespace GameCore
{
    /*
     * Baked lighting (RenderCore's RENDERPIPELINE.md, "Baked lighting"): with
     * bBakedLighting on, the editor's Build > Bake Lightmaps lights the static
     * entities' RenderComponents from the static lights, and the renderer
     * draws them with that bake instead of lighting them itself. World units
     * are centimetres (a glTF metre imports as 100).
     */
    struct WorldLightingSettings
    {
        bool     bBakedLighting  = false;
        float    TexelsPerUnit   = 0.1f;    // lightmap texels per world unit: 10 per metre
        int32_t  Bounces         = 3;       // indirect bounces after the direct light
        int32_t  SamplesPerTexel = 128;     // hemisphere rays per lightmap texel and bounce
        float    ProbeSpacing    = 200.0f;  // world units between light probes, for dynamic entities
        int32_t  MaxAtlasSize    = 1024;    // a lightmap atlas's width and height, a power of two

        /* The bake (RenderCore's BakedLightingAsset.h, .nmlighting), runtime
         * relative; written by the bake, "" before one. The renderer draws
         * with it while bBakedLighting is on. */
        std::string BakedData;

        /* Every field into its range: what a load or an editor field passes through. */
        void Clamp();

        bool operator==(const WorldLightingSettings& InOther) const;
        bool operator!=(const WorldLightingSettings& InOther) const { return !(*this == InOther); }
    };

    /*
     * What a world holds besides its entities (World::GetSettings). Saved in
     * both world formats: a <Settings> element in a text world's manifest, a
     * WSET chunk in a binary one -- each optional, so a world from before
     * either loads with the defaults.
     */
    struct WorldSettings
    {
        WorldLightingSettings Lighting;

        bool operator==(const WorldSettings& InOther) const { return Lighting == InOther.Lighting; }
        bool operator!=(const WorldSettings& InOther) const { return !(*this == InOther); }
    };

    /* WSET's payload, both directions; Clamp()ed on load. Append new fields
     * at the end, behind a version check. */
    GAMECORE_API void SerializeWorldSettings(EngineCore::Archive& Ar, WorldSettings& InOutSettings);
}

#endif //GAMECORE_WORLDSETTINGS_H

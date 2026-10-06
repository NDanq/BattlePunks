#include "Private/RenderCore/ShadowSetup.h"

#include <algorithm>

#include "Public/Core/CoreConfig.h"

/*
 * The [Shadows] config section: every scene's shadow quality, over what each
 * light asks for. A project setting: the game's DefaultEngine.ini, which Edit >
 * Project Settings > Shadows edits and applies at once. Read on the game
 * thread by WorldBridge into RenderLighting::Shadows, so a change shows on the
 * next frame.
 */
NIMBLE_CONFIG_BOOL(GShadowsEnabled,          "Shadows", "Enabled",           true, nullptr);
NIMBLE_CONFIG_INT(GShadowsResolution,        "Shadows", "Resolution",        2048, nullptr);   // 1024, 2048 or 4096: each atlas's side
NIMBLE_CONFIG_INT(GShadowsFilter,            "Shadows", "Filter",            1,    nullptr);   // 0 hard, 1 PCF 3x3, 2 PCF 5x5
NIMBLE_CONFIG_INT(GShadowsMaxShadowedLights, "Shadows", "MaxShadowedLights", 4,    nullptr);   // point and spot lights, 0-4
NIMBLE_CONFIG_FLOAT(GShadowsDistanceScale,   "Shadows", "DistanceScale",     1.0f, nullptr);   // times every sun's Shadow Distance
NIMBLE_CONFIG_INT(GShadowsMaxInsetShadows,   "Shadows", "MaxInsetShadows",   4,    nullptr);   // Render components' inset shadows, 0-4
NIMBLE_CONFIG_INT(GShadowsInsetFilter,       "Shadows", "InsetFilter",       1,    nullptr);   // theirs: 0 hard, 1 PCF 3x3, 2 PCF 5x5

namespace RenderCore
{
    RenderShadowSettings ReadShadowSettings()
    {
        RenderShadowSettings Settings;
        Settings.bEnabled          = GShadowsEnabled.GetBool() ? 1 : 0;
        Settings.Resolution        = std::clamp(GShadowsResolution.GetInt(), 256, 8192);
        Settings.Filter            = std::clamp(GShadowsFilter.GetInt(), 0, 2);
        Settings.MaxShadowedLights = std::clamp(GShadowsMaxShadowedLights.GetInt(), 0, int32_t(RENDER_SHADOWED_LOCAL_LIGHT_MAX));
        Settings.DistanceScale     = std::max(GShadowsDistanceScale.GetFloat(), 0.0f);
        Settings.MaxInsetShadows   = std::clamp(GShadowsMaxInsetShadows.GetInt(), 0, int32_t(RENDER_INSET_SHADOW_MAX));
        Settings.InsetFilter       = std::clamp(GShadowsInsetFilter.GetInt(), 0, 2);
        return Settings;
    }
}

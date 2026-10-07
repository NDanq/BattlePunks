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
NIMBLE_CONFIG_BOOL(GShadowsContact,           "Shadows", "ContactShadows",    false, nullptr);   // the sun's contact shadows
NIMBLE_CONFIG_FLOAT(GShadowsContactLength,    "Shadows", "ContactLength",     25.0f, nullptr);   // their reach toward the sun, world units
NIMBLE_CONFIG_FLOAT(GShadowsContactThickness, "Shadows", "ContactThickness",  6.0f,  nullptr);   // a depth-buffer surface's assumed depth
NIMBLE_CONFIG_INT(GShadowsContactSteps,       "Shadows", "ContactSteps",      12,    nullptr);   // samples along the march, 4-32
NIMBLE_CONFIG_FLOAT(GShadowsContactStrength,  "Shadows", "ContactStrength",   1.0f,  nullptr);   // 0-1
NIMBLE_CONFIG_BOOL(GShadowsContactLocal,      "Shadows", "ContactLocalLights", true, nullptr);   // shadowed point and spot lights too

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
        Settings.bContactShadows   = GShadowsContact.GetBool() ? 1 : 0;
        Settings.ContactLength     = std::max(GShadowsContactLength.GetFloat(), 0.0f);
        Settings.ContactThickness  = std::max(GShadowsContactThickness.GetFloat(), 0.01f);
        Settings.ContactSteps      = std::clamp(GShadowsContactSteps.GetInt(), 4, 32);
        Settings.ContactStrength   = std::clamp(GShadowsContactStrength.GetFloat(), 0.0f, 1.0f);
        Settings.bContactLocalLights = GShadowsContactLocal.GetBool() ? 1 : 0;
        return Settings;
    }
}

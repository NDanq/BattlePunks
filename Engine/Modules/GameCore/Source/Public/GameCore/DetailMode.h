#ifndef GAMECORE_DETAILMODE_H
#define GAMECORE_DETAILMODE_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /*
     * A scalability tier: the detail a world runs at, and -- per entity -- the
     * lowest tier the entity renders at (its "bucket").
     *
     *   Low     the default bucket: renders at every tier
     *   Medium  renders at Medium and above
     *   High    renders at High and Ultra
     *   Ultra   renders only at Ultra -- extra clutter, hero dressing
     *
     * An entity renders when its bucket is at or below the world's effective
     * tier (World::GetEffectiveDetailMode). RENDER-ONLY: a bucketed-out
     * entity still collides, ticks, runs scripts and plays sound -- gameplay
     * must never depend on the player's quality setting. The tier also picks
     * the [Scalability_<Tier>] config overlay (Scalability.h); RenderCore's
     * SCALABILITY.md covers both.
     */
    enum class EDetailMode : uint8_t
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
        Ultra  = 3,
    };

    constexpr uint8_t DETAIL_MODE_COUNT = 4;

    /* "Low" / "Medium" / "High" / "Ultra". */
    GAMECORE_API const char* DetailModeName(EDetailMode InMode);

    /* DetailModeName's inverse, case-insensitive; also "Med" and the digits
     * 0-3. False, OutMode untouched, for anything else. */
    GAMECORE_API bool ParseDetailMode(const char* InText, EDetailMode& OutMode);

    /* Whether an entity in InBucket renders at InWorldMode. */
    constexpr bool IsDetailVisible(const EDetailMode InBucket, const EDetailMode InWorldMode)
    {
        return uint8_t(InBucket) <= uint8_t(InWorldMode);
    }
}

#endif //GAMECORE_DETAILMODE_H

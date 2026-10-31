#ifndef TIMEOFDAY_TIMEOFDAYCOMPONENT_H
#define TIMEOFDAY_TIMEOFDAYCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/TimeOfDay/TimeOfDayAPI.h"

namespace TimeOfDay
{
    /*
     * "Time Of Day". A clock for its entity's Directional Light and Sky
     * Light: each frame it advances Hour and sets
     *
     *   * the entity's Transform Rotation, so the light travels from the
     *     sun's place in the sky (rising in the east of NorthYaw, highest at
     *     noon, 90 - Latitude degrees up); at night the moon takes the same
     *     arc;
     *   * the Directional Light's Color and Intensity (x SunIntensityScale,
     *     for a level exposed for a brighter sun) and the Sky Light's
     *     SkyColor, GroundColor and Intensity, from Preset at Hour.
     *
     * Preset is a .todpreset ("TimeOfDay:/Presets/Default.todpreset", or a
     * project's own "Content/..."). Without a Directional Light on the entity
     * only the Sky Light follows, and the reverse.
     *
     * The server's clock is the clock: Hour replicates, and a client runs
     * its copy between updates.
     */
    struct TIMEOFDAY_API TimeOfDayComponent
    {
        GAMECORE_COMPONENT_BODY()

        float   Hour             = 9.0f;    // 0 to 24
        float   DayLengthMinutes = 0.0f;    // real minutes per day; 0: [TimeOfDay] DayLengthMinutes
        uint8_t bRunning         = 1;
        float   NorthYaw         = 0.0f;    // degrees about +Y: where the sun's arc faces
        float   Latitude         = 35.0f;   // degrees: the noon sun stands 90 - Latitude up
        float   SunIntensityScale = 1.0f;   // x the preset's SunIntensity
        char    Preset[128]      = "TimeOfDay:/Presets/Default.todpreset";
    };
}

#endif //TIMEOFDAY_TIMEOFDAYCOMPONENT_H

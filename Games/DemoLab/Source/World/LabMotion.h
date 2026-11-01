#ifndef DEMOLAB_LABMOTION_H
#define DEMOLAB_LABMOTION_H

#include <cstdint>

#include "Public/Math/Vector.h"
#include "Public/GameCore/ComponentType.h"

namespace GameCore { class World; }

/*
 * LabMotion: set-dressing life, driven by signals like the rest of the
 * level logic (LabSignals.h). Presentation only: every process runs it, the
 * authority keeps nothing.
 *
 *   Spin         degrees a second added to the Transform's Euler X / Y / Z
 *                (the rest pose is where the level put it): reactor rings,
 *                pump fans, a klaxon's rotating spot light. While SpinOn is
 *                set and false it holds still; AlarmScale multiplies it while
 *                AlarmOn is true (the reactor races on the alarm).
 *   Flicker      Hz: a random on/off flicker, on FlickerDuty of the time --
 *                its Render shows and hides (still under a LabSignalToggle on
 *                the same entity), its point / spot light jumps in strength
 *                and goes dark, a particle system's Tint blinks (a lightning
 *                arc). 0 is steady.
 *   Ramp         from the moment RampOn is true, over RampSeconds: material
 *                parameter RampParam (a float in slot RampSlot) goes from
 *                RampFrom to RampTo, and the point / spot light from 0 to
 *                RampLight if that is above 0 (the beacon charging).
 */
struct LabMotionComponent {
    GAMECORE_COMPONENT_BODY()
    EngineCore::Vector3 Spin;        // degrees a second about Euler X, Y, Z
    char    SpinOn[32] = "";         // "" always
    char    AlarmOn[32] = "";
    float   AlarmScale = 1.f;
    float   Flicker = 0.f;           // Hz
    float   FlickerDuty = 0.5f;
    char    RampOn[32] = "";
    float   RampSeconds = 10.f;
    char    RampParam[32] = "";
    uint8_t RampSlot = 0;
    float   RampFrom = 0.f;
    float   RampTo = 1.f;
    float   RampLight = 0.f;

    // runtime, not saved
    uint8_t bStarted = 0;
    EngineCore::Vector3 Rest;        // the Transform's rotation at the start
    EngineCore::Vector3 Angle;       // degrees turned so far
    float   Clock = 0.f;
    float   Ramp = 0.f;              // 0..1
    float   LightRest = 0.f;         // the light's authored strength
    uint8_t TintRest = 255;          // the particle tint's authored alpha
};

namespace LabMotion {
    /* Every process, after LabLogic::TickPresentation. */
    void Tick(GameCore::World& InWorld, float InSeconds);
}

#endif //DEMOLAB_LABMOTION_H

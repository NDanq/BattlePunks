#ifndef AICORE_PERCEPTIONCOMPONENT_H
#define AICORE_PERCEPTIONCOMPONENT_H

#include <cstdint>

#include "Public/AICore/AICoreAPI.h"
#include "Public/GameCore/ComponentType.h"

namespace AICore
{
    /*
     * "Perception". What its entity sees and hears (Perception.h): every
     * Stimulus of another team within SightRange and the cone, in line of
     * sight; noises (AI::ReportNoise) within HearingRange times their
     * loudness. What it perceived is remembered for ForgetSeconds, and the
     * brain's blackboard told (TargetActor, TargetVisible, LastKnownPos, ...).
     */
    struct AICORE_API PerceptionComponent
    {
        GAMECORE_COMPONENT_BODY()

        float   SightRange     = 3000.0f;
        float   FovHalfAngle   = 70.0f;    // degrees either side of facing
        float   EyeHeight      = 160.0f;   // above the entity's origin
        float   HearingRange   = 2500.0f;
        float   ForgetSeconds  = 6.0f;
        float   SightInterval  = 0.2f;     // seconds between sight checks
        uint8_t Team           = 1;        // 0: neutral, sees nobody as hostile
        uint8_t bEnabled       = 1;
    };

    /*
     * "AI Stimulus". Makes its entity something perception can see: the
     * player, a bot, a decoy. Team 0 is nobody's enemy.
     */
    struct AICORE_API AIStimulusComponent
    {
        GAMECORE_COMPONENT_BODY()

        uint8_t Team         = 1;
        uint8_t bEnabled     = 1;
        float   TargetHeight = 110.0f;   // where the eye aims, above the origin (the chest)
    };
}

#endif //AICORE_PERCEPTIONCOMPONENT_H

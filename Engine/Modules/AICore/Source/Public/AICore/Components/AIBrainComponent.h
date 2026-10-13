#ifndef AICORE_AIBRAINCOMPONENT_H
#define AICORE_AIBRAINCOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/AICore/AICoreAPI.h"
#include "Public/GameCore/ComponentType.h"

namespace AICore
{
    enum : size_t
    {
        BRAIN_TREE_MAX      = 128,   // including the NUL
        BRAIN_BLACKBOARD_MAX = 192,
    };

    /*
     * "AI Brain". Runs a behavior tree (.nmbt) for its entity: from the
     * level's start (bAutoStart), or when told (AI.StartBrain, AIWorld). The
     * tree's state and blackboard live with the World's AIWorld, not here:
     * components are POD.
     *
     * Blackboard sets keys before the first tick: "Key=Value;Key=Value"
     * (numbers, true/false, x,y,z vectors, names).
     *
     * Ticks in TP_PrePhysics ("AI.Brains"), before the agents move
     * ("AI.Nav"), every TickInterval seconds (0: every frame).
     */
    struct AICORE_API AIBrainComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Tree[BRAIN_TREE_MAX] = {};
        char    Blackboard[BRAIN_BLACKBOARD_MAX] = {};
        float   TickInterval = 0.0f;
        uint8_t bAutoStart   = 1;

        // === Engine-written: never saved ====================================
        uint8_t bRunning = 0;
    };
}

#endif //AICORE_AIBRAINCOMPONENT_H

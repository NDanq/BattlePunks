#ifndef DEMOLAB_LABINPUTDRIVER_H
#define DEMOLAB_LABINPUTDRIVER_H

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }
namespace NimbleEngine { struct InputSnapshot; }

/*
 * Writing a character's input from code, through the same map a player's devices
 * fill ("DemoLabPlayer"): the autoplay tests and the bots (LabBots.cpp)
 * press what a player would press, so everything downstream -- prediction,
 * combat, interaction -- runs as it does for a person.
 */
namespace LabDrive {
    constexpr const char* MAP = "DemoLabPlayer";

    void SetAxis(NimbleEngine::InputSnapshot& InOut, const char* InAxis, float InValue);
    /* Down this frame; and Pressed unless bInHold (a held button). */
    void Press(NimbleEngine::InputSnapshot& InOut, const char* InAction, bool bInHold = false);

    /* Turns the view toward InTarget (LookX/LookY, at most InMaxDegrees a
     * frame). Returns the angle left between the camera ray and the target, degrees. */
    float AimAt(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOut,
                const EngineCore::Vector3& InTarget, float InMaxDegrees = 5.f);

    /* Turns toward InTarget on the ground and walks there (running when far);
     * true once within InStop. Levels the view as it goes. */
    bool WalkTo(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOut,
                const EngineCore::Vector3& InTarget, float InStop);

    /* The chest of a character: where a shot is aimed. */
    EngineCore::Vector3 ChestOf(GameCore::World& InWorld, GameCore::EntityID InLab);
}

#endif //DEMOLAB_LABINPUTDRIVER_H

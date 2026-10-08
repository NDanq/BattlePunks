#ifndef GAMECORE_AUDIOLISTENERCOMPONENT_H
#define GAMECORE_AUDIOLISTENERCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /*
     * "Audio Listener": where the World is heard from -- the entity's world
     * position, facing its forward (+Z) with its up (+Y). The first active one
     * is used; with none, the active camera is (the Audio module's attach
     * decides: NimbleEngine's GameSession gives it the camera).
     *
     * In GameCore for the same reason as Audio Source: a level carrying one
     * loads without the Audio module.
     */
    struct GAMECORE_API AudioListenerComponent
    {
        GAMECORE_COMPONENT_BODY()

        uint8_t bActive   = 1;
        uint8_t Reserved0 = 0;
        uint8_t Reserved1 = 0;
        uint8_t Reserved2 = 0;
    };
}

#endif //GAMECORE_AUDIOLISTENERCOMPONENT_H

#ifndef NIMBLEENGINE_INPUTINTERNAL_H
#define NIMBLEENGINE_INPUTINTERNAL_H

#include <cstdint>

#include "Public/NimbleEngine/Input.h"

/* Input.cpp's engine-only entry points, for InputRouter. */
namespace NimbleEngine
{
    /* GameWindow's enums as InputKeys; the router's translation. */
    InputKey InputKeyFromKeyCode(uint16_t InKeyCode);
    InputKey InputKeyFromMouseButton(uint8_t InButton);
    InputKey InputKeyFromPadButton(uint8_t InButton);
    InputKey InputKeyFromPadAxis(uint8_t InAxis);
}

#endif //NIMBLEENGINE_INPUTINTERNAL_H

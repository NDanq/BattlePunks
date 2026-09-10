#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreTypes.h"

class GameWindow;

namespace NimbleEngine
{
    class NE_API Application
    {
    public:
        void Exec();
    private:
        bool Initialize();
        void Shutdown();

        GameWindow* Window = nullptr;
        bool bQuitRequested = false;
    };
};

#endif //NIMBLEENGINE_NIMBLEENGINE_H

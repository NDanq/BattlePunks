#include "Public/NimbleEngine/Level.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/World.h"

/* One method, out of line purely because it needs World complete and Level.h
 * deliberately does not include World.h -- see that header's forward
 * declaration comment. */

namespace NimbleEngine
{
    GameCore::World& ILevel::GetWorld()
    {
        /* As GameSession::GetWorld(): null outside OnLoad..OnUnload. */
        CORE_CHECK(OwnedWorld != nullptr,
                   "ILevel::GetWorld() called on a level that is not loaded")
        return *OwnedWorld;
    }
}

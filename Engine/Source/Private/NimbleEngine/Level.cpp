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
        /* Same contract, and the same reasoning, as Application::GetWorld():
         * there is no sensible reference to return when no World exists, only
         * a caller that got the lifecycle wrong. Outside OnLoad..OnUnload this
         * is null by construction -- Application clears it after the World is
         * destroyed. */
        CORE_CHECK(OwnedWorld != nullptr,
                   "ILevel::GetWorld() called on a level that is not loaded")
        return *OwnedWorld;
    }
}

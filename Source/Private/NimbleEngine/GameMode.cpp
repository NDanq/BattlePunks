#include "Public/NimbleEngine/GameMode.h"

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/World.h"

#include <string>

/*
 * IGameMode's context (GameMode.h): the session it runs in, and the
 * member-function bindings the session takes back after OnUnload
 * (GameSession::UnloadLevel).
 */

namespace NimbleEngine
{
    IGameMode* FindLevelGameMode(const GameCore::World& InWorld)
    {
        GameSession* Session = FindGameSession(InWorld);
        ILevel*      Level   = Session ? Session->GetLevel() : nullptr;
        IGameMode*   Mode    = Level ? Level->GetGameMode() : nullptr;
        /* Only while the session holds it: not before LoadLevel hands it the
         * session, nor once UnloadLevel has taken it back. */
        return Mode && Mode->HasSession() ? Mode : nullptr;
    }

    GameSession& IGameMode::GetSession() const
    {
        CORE_CHECK(Session != nullptr, "IGameMode::GetSession() outside the mode's level (before OnLoad or after OnUnload)")
        return *Session;
    }

    GameCore::World& IGameMode::GetWorld() const
    {
        return GetSession().GetWorld();
    }

    void IGameMode::BindSystem(const char* InName, const GameCore::ETickPhase InPhase, const SystemThunkFn InFn, const bool bInBeforeScripts)
    {
        GetWorld().RegisterSystem(InName, InPhase, InFn, static_cast<IGameMode*>(this),
                                  bInBeforeScripts ? GameCore::SO_BeforeScripts : GameCore::SO_AfterScripts);
    }

    void IGameMode::BindEvent(const char* InEvent, const EventThunkFn InFn)
    {
        GetWorld().GetEvents().Subscribe(InEvent, InFn, static_cast<IGameMode*>(this));
    }

    void IGameMode::BindAvatarInputFilter(const FilterThunkFn InFn)
    {
        AvatarWorld* Avatars = GetSession().GetAvatars();
        if(!Avatars)
        {
            CORE_LOG_WARN("[GameMode] SetAvatarInputFilter: the level has no avatars; no filter set")
            return;
        }
        Avatars->SetInputFilter(InFn, static_cast<IGameMode*>(this));
    }
}

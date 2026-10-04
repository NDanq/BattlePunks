#ifndef NIMBLEENGINE_LEVEL_H
#define NIMBLEENGINE_LEVEL_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"

/* Forward-declared: GetWorld() only returns a reference, and a subclass that
 * needs World complete includes Public/GameCore/World.h itself. */
namespace GameCore { class World; }

namespace NimbleEngine
{
    class GameSession;
    class GameState;
    class IGameMode;

    /*
     * The unit of load/unload: a World, the IGameMode acting on it and its
     * GameState. Each level owns its World, so unloading destroys everything.
     *
     *   OnLoad     empty World, scripts and renderer bound, startup script not
     *              yet run. Spawn entities, register systems.
     *   OnUnload   World still populated; unbind anything bound from outside.
     *
     * Full sequence: GameSession::LoadLevel. A level does not tick; register a
     * World system for per-frame work. Not NE_API; GetWorld() is annotated.
     */
    class ILevel
    {
    public:
        virtual ~ILevel() = default;

        /*
         * A stable name: used in logs, resolved by the save-game registry, and
         * what a future Lua level change would pass, since a script cannot hold
         * a C++ pointer. Must outlive the level -- a string literal is intended.
         */
        virtual const char* GetName() const = 0;

        /* The world file OnLoad loads ("Content/Levels/X.nmworld", runtime
         * relative), if it loads one: how the editor's Play starts the level
         * of the world that is open (FindLevelByWorldFile). nullptr: none. */
        virtual const char* GetWorldFile() const { return nullptr; }

        /* Populate the World. False aborts the load (OnUnload still runs).
         * Prefer GetWorld() to GameSession::GetWorld(). */
        virtual bool OnLoad(GameSession& InSession) { (void)InSession; return true; }

        /* The World is about to be destroyed. Pairs with OnLoad having been
         * called, not with it succeeding. Unbind what OnLoad bound -- ~World
         * warns about any built-in delegate still bound. */
        virtual void OnUnload() {}

        /*
         * A saved game is about to be restored into this World; only on the
         * load-from-save path. OnLoad and the startup script have both run, so
         * the World already holds the level's entities AND any the script
         * created -- and the save contains its own copies of both. Without
         * clearing here, the loaded scene is doubled.
         *
         * Only the level knows which entities to keep, so this is its call. Most
         * levels destroy everything (outside a tick, DestroyEntity is
         * immediate). Any handle OnLoad cached is stale afterwards; re-point it
         * once the save is in (IGameMode::OnPostLoadSave).
         */
        virtual void OnPrepareForLoadedGame() {}

        /* The rules for this level, or nullptr for a level with no logic (a menu
         * backdrop). Owned by the level; the engine re-reads it at every
         * lifecycle point, so it may change over the level's life. */
        virtual IGameMode* GetGameMode() { return nullptr; }

        /* The state this level's mode uses, or nullptr. Reset by the engine
         * before IGameMode::OnStart. */
        virtual GameState* GetGameState() { return nullptr; }

        /* The Lua file run after IGameMode::OnLoad. nullptr runs [Scripting]
         * StartupScript, as every level did before; "" runs none, for a level
         * whose scene is all its own. */
        virtual const char* GetStartupScript() const { return nullptr; }

        /* This level's World, from the start of OnLoad to the end of OnUnload;
         * CORE_CHECKed outside that window. Not virtual -- engine-owned. */
        NE_API GameCore::World& GetWorld();

    private:
        /* Sets OwnedWorld; a friend so subclasses cannot. */
        friend class GameSession;
        GameCore::World* OwnedWorld = nullptr;
    };
}

#endif //NIMBLEENGINE_LEVEL_H

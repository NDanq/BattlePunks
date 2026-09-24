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
     * WHAT CONTAINS THE ENTITIES: the unit of load/unload. A level holds a World,
     * the IGameMode acting on it and the GameState that mode uses. Unloading
     * destroys the World and everything in it -- isolation is structural, which
     * is why each level owns a World rather than tagging entities in a shared one.
     *
     *   OnLoad     the World exists, is empty, and has scripts and the renderer
     *              bound. Spawn entities, register systems. The startup script
     *              has not run yet.
     *   OnUnload   the World is still valid and populated; unbind anything
     *              bound to it from outside.
     *
     * GameSession::LoadLevel has the full sequence. A level does not tick; for
     * per-frame work it registers a World system.
     *
     * The engine allocates and frees the World (Memory::Alloc/Dealloc), so a
     * missed teardown shows up in ReportLeaks(). Not NE_API, like GameState;
     * GetWorld() is the one out-of-line member and carries its own annotation.
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

        /*
         * Populate the World. False aborts the load: OnUnload still runs, and a
         * startup level that fails shuts the engine down cleanly. InSession
         * outlives the level. Use GetWorld() below rather than
         * GameSession::GetWorld(), which only matches while this is the active
         * level.
         */
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

        /* This level's World, from the start of OnLoad to the end of OnUnload;
         * CORE_CHECKed outside that window. Not virtual -- engine-owned. */
        NE_API GameCore::World& GetWorld();

    private:
        /* Set by GameSession around OnLoad/OnUnload. A friend rather than
         * protected, so a subclass cannot assign its own World. */
        friend class GameSession;
        GameCore::World* OwnedWorld = nullptr;
    };
}

#endif //NIMBLEENGINE_LEVEL_H

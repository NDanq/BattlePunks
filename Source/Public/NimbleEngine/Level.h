#ifndef NIMBLEENGINE_LEVEL_H
#define NIMBLEENGINE_LEVEL_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"

/* Forward-declared, NOT included. A game's level subclass needs World complete
 * to do anything useful with GetWorld(), but it gets that from its own
 * #include <Public/GameCore/World.h> -- this header only hands back a
 * reference, and forcing every consumer of Level.h to parse World.h (and
 * through it ComponentPool, SpatialIndex, EventBus and Delegate) to declare a
 * level is a cost with nothing behind it. */
namespace GameCore { class World; }

namespace NimbleEngine
{
    class Application;
    class GameState;
    class IGameMode;

    /*
     * WHAT CONTAINS THE ENTITIES, and owns how long they live.
     *
     * A level is the unit of "load this, then unload it and load another":
     * it holds a World, the IGameMode that acts on that World, and the
     * GameState that mode reads and writes. Unloading a level destroys its
     * World outright, and with it every entity, component and registered
     * system inside -- isolation is STRUCTURAL rather than bookkeeping, which
     * is why this owns a World rather than tagging entities in a shared one.
     *
     * =============================== LIFECYCLE ===============================
     *
     *   OnLoad     the World exists and is EMPTY, and the ScriptEngine and
     *              Renderer are already bound to it. Populate it here --
     *              spawn entities, register systems. The startup script has
     *              NOT run yet.
     *
     *   (engine)   the startup Lua script runs, then GameState::OnReset,
     *              then IGameMode::OnStart.
     *
     *   OnUnload   the loop has ended, or another level was requested. The
     *              World is still valid and still populated; anything bound
     *              to it from outside must come off here.
     *
     * A LEVEL DOES NOT TICK. IGameMode::OnTick is the per-frame hook -- a
     * level is a container, not a behaviour. If a level wants per-frame work
     * it registers a World system in OnLoad, which is what systems are for.
     * ========================================================================
     *
     * THE WORLD IS THE ENGINE'S TO ALLOCATE, not the subclass's: Application
     * creates it before OnLoad and destroys it after OnUnload, through
     * EngineCore::Memory::Alloc/Dealloc, so the allocation is symmetric and a
     * missed teardown is something Sandbox's ReportLeaks() catches at exit.
     * A game's subclass never sees that allocation at all.
     *
     * Not NE_API for the same reason GameState is not: a game subclasses this
     * in its own image, and the engine only ever holds an ILevel*. GetWorld()
     * is the one exception -- it is defined out-of-line in Level.cpp because
     * it reaches engine-owned state the subclass has no access to, so it
     * carries the annotation itself.
     */
    class ILevel
    {
    public:
        virtual ~ILevel() = default;

        /*
         * A STABLE NAME for this level, used in logs today.
         *
         * Also the handle a future Lua binding would resolve against: levels
         * live in NimbleEngine and GameCore's ScriptBindings.cpp cannot see
         * them, so a Lua-driven level change would go through an engine-side
         * installer built on ScriptEngine::GetState() and would need a string
         * to name a level by -- a C++ pointer is not something a script can
         * hold. Must outlive the level; a string literal is the intended use.
         */
        virtual const char* GetName() const = 0;

        /*
         * Populate the World. Returning false aborts the load: the level is
         * unloaded again immediately (OnUnload still runs) and, if this was
         * the startup level, the engine shuts down cleanly.
         *
         * InApp outlives the level, so stashing it to reach GetRenderer() or
         * RequestQuit() later is the intended use. Reach the entities through
         * GetWorld() below rather than Application::GetWorld() -- they are the
         * same object during OnLoad, but GetWorld() is the one that stays
         * correct if this level is ever loaded while another is active.
         */
        virtual bool OnLoad(Application& InApp) { (void)InApp; return true; }

        /*
         * The level is going away, and its World is about to be destroyed.
         *
         * PAIRS WITH OnLoad HAVING BEEN CALLED AT ALL, not with it having
         * succeeded -- the same rule IGameMode::OnUnload follows. Whatever
         * OnLoad bound to the World's delegates, unbind here; ~World logs a
         * warning for any built-in delegate still holding entries, which is
         * exactly the diagnostic a missed unbind produces.
         */
        virtual void OnUnload() {}

        /*
         * The rules that act on this level, or nullptr for a level with no
         * logic of its own (a menu backdrop, an engine bring-up scene).
         *
         * OWNED BY THE LEVEL, whatever that means for a given game -- a member,
         * a pointer into something longer-lived. The engine only reads it, and
         * reads it fresh at every lifecycle point rather than caching, so a
         * level may return different modes over its life.
         */
        virtual IGameMode* GetGameMode() { return nullptr; }

        /* The state this level's mode reads and writes, or nullptr for a level
         * that tracks nothing. Reset by the engine before IGameMode::OnStart. */
        virtual GameState* GetGameState() { return nullptr; }

        /*
         * This level's entities. Valid from the start of OnLoad through the
         * end of OnUnload, and CORE_CHECKed outside that window -- calling it
         * on a level the engine has not loaded is a programming error in the
         * same class as Application::GetWorld() before Initialize().
         *
         * NOT virtual: the World is engine-owned (see the class comment), so
         * there is nothing for a subclass to override here.
         */
        NE_API GameCore::World& GetWorld();

    private:
        /* Set by Application around OnLoad/OnUnload. Private with a friend
         * rather than protected: a subclass has no business assigning its own
         * World, and making that impossible is cheaper than documenting it. */
        friend class Application;
        GameCore::World* OwnedWorld = nullptr;
    };
}

#endif //NIMBLEENGINE_LEVEL_H

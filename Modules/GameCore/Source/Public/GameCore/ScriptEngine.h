#ifndef GAMECORE_SCRIPTENGINE_H
#define GAMECORE_SCRIPTENGINE_H

#include <cstdint>

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/* Forward-declared, NOT included -- lua.h never appears in Public/. See
 * GameCoreInternal.h for why. A game that wants its own bindings links
 * Lua::Lua itself and uses GetState(). */
struct lua_State;

namespace GameCore { class World; }

/*
 * Lua lifetime, and the bridge between it and one World. Owned by
 * NimbleEngine::Application alongside its one World -- see DESIGN.md for
 * the teardown-order reasoning. Game thread only, no locks.
 */
namespace GameCore
{
    class GAMECORE_API ScriptEngine
    {
    public:
        ScriptEngine();
        ~ScriptEngine();

        ScriptEngine(const ScriptEngine&)            = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

        /* lua_newstate with an allocator routed through
         * EngineCore::SystemAlignedAlloc, luaL_openlibs, and print()
         * rerouted to GAMECORE_LOG. False if lua_newstate failed -- logged,
         * not fatal. */
        bool Initialize();

        /* lua_close. UnbindWorld() first if a World is still bound. */
        void Shutdown();

        bool IsInitialized() const;

        /* Installs the Lua libraries, the World's script driver (entity
         * scripts tick first in every phase), and the built-in delegate/event
         * bridge -- see DESIGN.md. Entities that already carry a script path
         * are instantiated on the first tick. One World at a time; rebinding
         * un-installs the previous one first. */
        void BindWorld(World* InWorld);

        /* Reverses everything BindWorld() did: runs OnDestroy and releases
         * every live script instance, Lua system and Events.On() binding.
         * Script PATHS stay on their entities, so a later BindWorld starts
         * them again from scratch. Safe to call with no World bound. */
        void UnbindWorld();

        /* Reads InPath through EngineCore's file seam, then loads and runs
         * it under a protected call. False on a read failure, a load error,
         * or a runtime error -- none of them fatal to the engine. */
        bool RunFile(const char* InPath);

        bool RunString(const char* InSource, const char* InChunkName);

        /* For a game's own bindings. nullptr if !IsInitialized(). */
        lua_State* GetState() const;

        /* Lifetime total, incremented by every logged script error --
         * see DESIGN.md for how this is tallied. */
        uint32_t GetErrorCount() const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_SCRIPTENGINE_H

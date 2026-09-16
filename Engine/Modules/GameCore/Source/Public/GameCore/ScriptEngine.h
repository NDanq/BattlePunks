#ifndef GAMECORE_SCRIPTENGINE_H
#define GAMECORE_SCRIPTENGINE_H

#include <cstdint>

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * Forward-declared, NOT included -- lua.h never appears in Public/. See
 * GameCoreInternal.h for why, and for the header GetState()'s return value
 * points a game's OWN bindings at (a game that wants to bind its own C
 * functions links Lua::Lua itself, the way NimbleEngine.h forward-declares
 * GameWindow/GDI for the same "do not leak a dependency through a public
 * header" reason).
 */
struct lua_State;

namespace GameCore { class World; }

/*
 * Lua lifetime, and the bridge between it and one World.
 *
 * NimbleEngine::Application owns exactly one ScriptEngine (see
 * Application::GetScriptEngine() in NimbleEngine.h) alongside its one World,
 * Initializes it once, BindWorld()s it to that World, and UnbindWorld() +
 * Shutdown()s it in the reverse order at teardown -- see NimbleEngine.cpp's
 * Shutdown() for why that order matters (instances reference the World; the
 * World's pools fire the removal observer, which needs a live lua_State).
 *
 * GAME THREAD ONLY, same contract as World and GameWindowSystem: no locks
 * anywhere in this class.
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

        /* lua_newstate with an allocator routed through EngineCore::Memory (so
         * Lua's own heap use is counted the way everything else in this engine
         * is), luaL_openlibs, and print() rerouted to GAMECORE_LOG with a
         * "[Lua] " sub-tag. False if lua_newstate itself failed (out of
         * memory) -- logged, not fatal. */
        bool Initialize();

        /* lua_close. UnbindWorld() first if a World is still bound -- see the
         * class comment's teardown-order note; calling this while still bound
         * is a programming error and is logged loudly rather than silently
         * leaking every live script instance's Lua-side state into a closed
         * VM. */
        void Shutdown();

        bool IsInitialized() const;

        /*
         * Installs World, Entity, the component-proxy libraries, TickPhase and
         * Events into this Lua state; registers the script-tick system into
         * EVERY ETickPhase bucket on InWorld (see ScriptSystem.cpp -- a
         * ScriptComponent's own Phase byte decides which bucket's pass actually
         * runs it); installs the ScriptComponent removal observer on InWorld's
         * ScriptComponent pool; and bridges InWorld's four built-in delegates
         * (OnEntityCreated etc.) onto InWorld->GetEvents() under the names
         * "EntityCreated", "EntityDestroyed", "ComponentAdded",
         * "ComponentRemoved".
         *
         * One World at a time -- calling this again before UnbindWorld()
         * un-installs the previous binding first, logging a warning: rebinding
         * is not a supported way to swap worlds mid-session, it exists so a
         * test fixture can bind a fresh World per test case.
         */
        void BindWorld(World* InWorld);

        /*
         * Reverses everything BindWorld() did: runs OnDestroy (under protected
         * call) and luaL_unref's every still-live script instance, releases
         * every Lua system and Events.On() binding a script made, unregisters
         * the script-tick system from every phase, removes the removal
         * observer. Safe to call when no World is bound (a no-op).
         */
        void UnbindWorld();

        /*
         * Reads InPath through EngineCore's file seam (GetFileProvider() when
         * one is installed, appFileReadAll otherwise -- NEVER <fstream>,
         * matching CoreConfig.h's own LoadConfigFile), then luaL_loadbuffer +
         * a protected call. False on a read failure, a load (syntax) error, or
         * a runtime error -- each logged with InPath as context, none of them
         * fatal to the engine.
         */
        bool RunFile(const char* InPath);

        bool RunString(const char* InSource, const char* InChunkName);

        /*
         * The C++ entry point to what Lua's e:AddScript("ClassName") does --
         * adds a ScriptComponent to InEntity bound to the class registered
         * under InClassName (via Lua's own Script.Register), reading that
         * class's TickPhase and CullOutsideActiveRegion fields the same way
         * the Lua path does. False if InClassName was never registered, or
         * InEntity already has a ScriptComponent.
         */
        bool InstantiateScript(EntityID InEntity, const char* InClassName);

        /* For a game's own bindings -- linking Lua::Lua itself to register
         * further C functions into this same state. nullptr if !IsInitialized(). */
        lua_State* GetState() const;

        /* Lifetime total, not "since the last Tick" -- incremented by every
         * logged script error (a load failure, a runtime error under
         * LuaCallProtected). What a host reads to decide whether scripting is
         * healthy without parsing log text. */
        uint32_t GetErrorCount() const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_SCRIPTENGINE_H

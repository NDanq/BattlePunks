#ifndef GAMECORE_SCRIPTENGINE_H
#define GAMECORE_SCRIPTENGINE_H

#include <cstdint>

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/* Forward-declared, NOT included -- lua.h never appears in Public/. See
 * GameCoreInternal.h for why. A game that wants its own bindings links
 * Lua::Lua itself and uses GetState(). */
struct lua_State;

#include "Public/GameCore/EventBus.h"

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

        /* Installs the Lua libraries, the script driver and the event bridge
         * (DESIGN.md). Existing script paths start on the first tick. One
         * World at a time. */
        void BindWorld(World* InWorld);

        /* Reverses BindWorld(): OnDestroy and release for every script
         * instance, Lua system and Events.On() binding. Script paths stay.
         * Safe with no World bound. */
        void UnbindWorld();

        /* Reads InPath through EngineCore's file seam, then loads and runs
         * it under a protected call. False on a read failure, a load error,
         * or a runtime error -- none of them fatal to the engine. */
        bool RunFile(const char* InPath);

        bool RunString(const char* InSource, const char* InChunkName);

        /*
         * Calls the Lua function at InPath -- a global, or a dotted path into
         * nested tables ("Menu.Play") -- with InArgs, under a protected call;
         * results are dropped. False, logged and counted, if a link of the
         * path is missing or not a function, or it raises. For callers that
         * name a function as data (a UI's OnClick="Lua:Menu.Play").
         */
        bool CallFunction(const char* InPath, const EventArgs& InArgs = EventArgs());

        /* Whether InPath names a function right now; nothing is called. */
        bool HasFunction(const char* InPath) const;

        // === Script objects ====================================================
        /*
         * An instance of a script class -- a .lua, or a script graph's stored
         * Lua, that returns a table -- held by C++ for as long as it likes: a
         * UI document's UI graph. Independent of any World (it survives
         * Bind/UnbindWorld), but not of this engine: release every object
         * before Shutdown. 0 is no object.
         */
        using Object = int32_t;

        /* A new `self` over the class at InClassPath (its methods through
         * __index), with InFieldCount fields set on it first (InFieldNames[i]
         * = InFields[i]). 0, logged once per path, if the class does not load. */
        Object CreateObject(const char* InClassPath, const char* const* InFieldNames = nullptr,
                            const EventArg* InFields = nullptr, int InFieldCount = 0);

        /* Whether InObject's class has a method InMethod. */
        bool HasObjectMethod(Object InObject, const char* InMethod) const;

        /* self:InMethod(args...) under a protected call. False if there is no
         * such method (silently) or it raises (logged and counted). */
        bool CallObjectMethod(Object InObject, const char* InMethod, const EventArgs& InArgs = EventArgs());

        /* self[InName] = InValue on InObject (a reused list row's prefix). False if there is no such object. */
        bool SetObjectField(Object InObject, const char* InName, const EventArg& InValue);

        /* Lets InObject go. Safe with 0. */
        void ReleaseObject(Object InObject);

        /* The next CreateObject of InClassPath reads it again (a saved graph). */
        void ForgetClass(const char* InClassPath);

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

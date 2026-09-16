#ifndef GAMECORE_GAMECOREINTERNAL_H
#define GAMECORE_GAMECOREINTERNAL_H

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <string>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * The module's private half: everything that touches lua.h directly.
 *
 * THIS IS THE ONLY HEADER IN THIS MODULE THAT INCLUDES lua.h/lauxlib.h/lualib.h.
 * That is what makes the PRIVATE link of Lua::Lua in CMakeLists.txt true rather
 * than aspirational -- exactly the discipline GameWindowInternal.h states for
 * SDL.h ("This is the only header in the module that includes SDL.h, and it
 * lives under Private/ so it cannot be reached from outside"). Public/ForwardEngine
 * headers forward-declare struct lua_State and return it as an opaque pointer
 * (see ScriptEngine.h::GetState()); nothing outside Private/GameCore/ ever
 * needs to know what a lua_State contains.
 *
 * ============================================================================
 * LUA IS C, AND ITS ERRORS ARE longjmp -- read before writing a lua_CFunction
 * ============================================================================
 *
 * lua_error / luaL_error / luaL_argerror do not return: they longjmp back to
 * the nearest lua_pcall, unwinding the C stack WITHOUT running C++ destructors
 * for any local between the raise and that pcall. This is undefined behaviour
 * to rely on in general C++, and Lua's own manual says so; in practice on every
 * toolchain this engine targets, it works up to the specific hazard below, and
 * it is the API's normal error-signalling mechanism -- there is no exception to
 * throw instead.
 *
 * RULE: a lua_CFunction may only have TRIVIALLY DESTRUCTIBLE locals in scope at
 * any point that can still raise. Argument-checking helpers (luaL_checkinteger
 * and friends, and the small wrappers below) go FIRST, before any std::string
 * or other non-trivial local is constructed; once a raising call could no
 * longer occur, ordinary C++ is fine again. GameCoreLog.h's macros build a
 * std::string but never raise, so logging is safe to place anywhere.
 */

struct lua_State;

namespace GameCore
{
    /*
     * Restores the Lua stack to its size at construction, on any exit path --
     * including a longjmp out of a raising call INSIDE the scope this guards,
     * PROVIDED the guard itself was constructed before that call and the
     * unwind does not cross this object's own scope (a longjmp that jumps past
     * a live LuaStackGuard skips its destructor exactly as it would skip any
     * other -- this class does not defend against that, it only saves the
     * common case of "several pushes, several pops, do not miscount them by
     * hand" from becoming a slow stack leak across thousands of calls).
     */
    class LuaStackGuard
    {
    public:
        explicit LuaStackGuard(lua_State* InState) : State(InState), Top(lua_gettop(InState)) {}
        ~LuaStackGuard() { lua_settop(State, Top); }

        LuaStackGuard(const LuaStackGuard&)            = delete;
        LuaStackGuard& operator=(const LuaStackGuard&) = delete;

    private:
        lua_State* State;
        int        Top;
    };

    /* Installed as the message handler argument to every lua_pcall this module
     * makes, so a runtime error carries a traceback rather than just the raw
     * message luaL_error was given -- the difference between "attempt to index
     * a nil value" and "attempt to index a nil value, in Mover.OnTick, called
     * from World's PostPhysics system". */
    int LuaTracebackHandler(lua_State* InState);

    /*
     * Calls lua_pcall with LuaTracebackHandler installed, and on failure logs
     * the error THROUGH GAMECORE_LOG_ERROR (so it reaches whatever LogHandler
     * the host installed, tagged, exactly like every other error this module
     * reports) and pops the error message off the stack. InContext names what
     * was being run, for the log line ("running Scripts/Main.lua",
     * "Mover.OnTick"). Returns true on success.
     */
    bool LuaCallProtected(lua_State* InState, int InNArgs, int InNResults, const char* InContext);

    /* Packs an EntityID as a full userdata with the "Entity" metatable --
     * ScriptBindings.cpp installs that metatable once, at BindWorld time.
     * Every other binding in this module that hands Lua an entity goes through
     * this one function. */
    void LuaPushEntity(lua_State* InState, EntityID InEntity);

    /* The inverse: reads the EntityID out of an "Entity" userdata at InIndex,
     * raising a Lua argument error (via luaL_argerror) if InIndex is not one --
     * this IS one of the raising calls the header comment above warns about,
     * so callers should not have a non-trivial local alive across it. */
    EntityID LuaCheckEntity(lua_State* InState, int InIndex);

    /* Packs {EntityID, ComponentTypeID} as a full userdata with the metatable
     * for InType's component name (e.g. "Transform") -- ScriptBindings.cpp
     * registers one such metatable per component type the first time Lua asks
     * for it by name. This is the proxy World.h's public Lua surface documents:
     * every method on it re-fetches through World::GetComponentRaw, so holding
     * one across a swap-remove reads nil rather than aliasing another entity. */
    void LuaPushComponentProxy(lua_State* InState, EntityID InEntity, ComponentTypeID InType);
}

#endif //GAMECORE_GAMECOREINTERNAL_H

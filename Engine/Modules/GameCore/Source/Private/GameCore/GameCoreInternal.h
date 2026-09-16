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
 * The module's private half: everything that touches lua.h directly. THIS IS
 * THE ONLY HEADER IN THIS MODULE THAT INCLUDES lua.h/lauxlib.h/lualib.h --
 * see DESIGN.md for why that matters and for the longjmp hazard every
 * lua_CFunction in this module must respect.
 */

struct lua_State;

namespace GameCore
{
    /* Restores the Lua stack to its size at construction, on any exit path.
     * Does not defend against a longjmp past its own scope -- see DESIGN.md. */
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

    /* Installed as the message handler for every lua_pcall this module
     * makes, so a runtime error carries a traceback. */
    int LuaTracebackHandler(lua_State* InState);

    /* Calls lua_pcall with LuaTracebackHandler installed, and on failure
     * logs through GAMECORE_LOG_ERROR and increments the error counter --
     * see DESIGN.md. InContext names what was being run, for the log line. */
    bool LuaCallProtected(lua_State* InState, int InNArgs, int InNResults, const char* InContext);

    /* Packs an EntityID as a full userdata with the "Entity" metatable.
     * Every binding that hands Lua an entity goes through this. */
    void LuaPushEntity(lua_State* InState, EntityID InEntity);

    /* The inverse -- raises a Lua argument error if InIndex is not an
     * Entity userdata. A raising call; no non-trivial local across it. */
    EntityID LuaCheckEntity(lua_State* InState, int InIndex);

    /* Packs {EntityID, ComponentTypeID} as a full userdata with the
     * metatable for InType's component name. Every method on it re-fetches
     * through World::GetComponentRaw, so holding one across a swap-remove
     * reads nil rather than aliasing another entity. */
    void LuaPushComponentProxy(lua_State* InState, EntityID InEntity, ComponentTypeID InType);
}

#endif //GAMECORE_GAMECOREINTERNAL_H

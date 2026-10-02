#ifndef GAMECORE_GAMECOREINTERNAL_H
#define GAMECORE_GAMECOREINTERNAL_H

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <string>

#include "Public/GameCore/ComponentScript.h"
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
    class World;

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

    /* Counts an error that never reached LuaCallProtected (missing file,
     * syntax error, no class returned). */
    void LuaCountError(lua_State* InState);

    /* Through EngineCore's file seam. False if unreadable. */
    bool LuaReadScriptFile(const char* InPath, std::string& OutSource);

    /* Packs an EntityID as a full userdata with the "Entity" metatable.
     * Every binding that hands Lua an entity goes through this. */
    void LuaPushEntity(lua_State* InState, EntityID InEntity);

    /* The inverse -- raises a Lua argument error if InIndex is not an
     * Entity userdata. A raising call; no non-trivial local across it. */
    EntityID LuaCheckEntity(lua_State* InState, int InIndex);

    /* Packs {EntityID, ComponentTypeID} as a full userdata with InType's
     * metatable, building it on first use (ComponentScript.cpp). Every method
     * re-fetches through World::GetComponentRaw, so a proxy held across a
     * swap-remove errors rather than aliasing another entity. */
    void LuaPushComponentProxy(lua_State* InState, EntityID InEntity, ComponentTypeID InType);

    /* The World ScriptEngine::BindWorld bound to InState, or nullptr. */
    World* LuaGetBoundWorld(lua_State* InState);

    /* LuaCheckEntity without the raise: false if InIndex is not an Entity, for
     * ScriptCall, which must never raise. */
    bool LuaToEntity(lua_State* InState, int InIndex, EntityID& OutEntity);

    /* A typed field or variable as its Lua lanes (three numbers for a Vector3),
     * and back from the lanes starting at InFirstArg. Shared by component
     * fields (ComponentScript.cpp) and library variables (ScriptLibrary.cpp).
     * The read raises on a bad argument. */
    int  LuaPushScriptValue(lua_State* InState, EScriptFieldType InType, uint32_t InSize, const uint8_t* InValue);
    void LuaReadScriptValue(lua_State* InState, EScriptFieldType InType, uint32_t InSize, int InFirstArg, uint8_t* OutValue);

    /* One global table per registered script library (ScriptLibrary.cpp);
     * called by ScriptBindings_Install. */
    void ScriptLibrary_Install(lua_State* InState);
}

#endif //GAMECORE_GAMECOREINTERNAL_H

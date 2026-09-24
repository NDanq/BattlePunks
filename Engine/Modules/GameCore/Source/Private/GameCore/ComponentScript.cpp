#include "Public/GameCore/ComponentScript.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include "Private/GameCore/GameCoreInternal.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/*
 * Component proxies and their selectively-exposed methods -- the Lua half of
 * ComponentScript.h. See DESIGN.md.
 *
 * ONE DISPATCHER, many closures. Each Lua method on a component is a C closure
 * over ComponentDispatch carrying three upvalues: the binding entry, the type
 * id, and what to do (get a field, set it, call a method, or GetEntity). A
 * type's metatable is built the first time a proxy of that type is pushed.
 *
 * THE LONGJMP RULE. Lua is compiled as C, so a Lua error is a longjmp.
 * ComponentDispatch keeps only trivially-destructible locals and raises errors
 * itself; binder code runs inside the ScriptCall, which never raises (see
 * ComponentScript.h) -- so no C++ destructor is ever skipped.
 */

namespace GameCore
{
namespace
{
    /* Constant-initialised, so registrars running during static init find a
     * valid table -- ComponentType.cpp's GRegistry argument. */
    enum : size_t { SCRIPT_ENTRY_MAX = 1024 };

    struct ScriptTypeRange
    {
        uint16_t First       = 0;
        uint16_t Count       = 0;
        bool     bRegistered = false;
    };

    ScriptBindingEntry GScriptEntries[SCRIPT_ENTRY_MAX];
    size_t             GScriptEntryCount = 0;
    ScriptTypeRange    GScriptTypes[COMPONENT_TYPE_MAX];

    /* A component proxy: which component, never a pointer to it. */
    struct ComponentProxy
    {
        EntityID        Entity;
        ComponentTypeID Type = COMPONENT_TYPE_INVALID;
    };

    enum EDispatchMode : int
    {
        DM_GetEntity = 0,
        DM_GetField,
        DM_SetField,
        DM_Method,
    };

    /* Registry key for a type's metatable, prefixed so a component named
     * "Camera" cannot collide with another library's metatable. */
    void MetatableName(const ComponentTypeID InType, char (&OutName)[96])
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        std::snprintf(OutName, sizeof(OutName), "GameCore.Component:%s", Info ? Info->Name : "Unknown");
    }

    const char* TypeNameOf(const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        return Info ? Info->Name : "Unknown";
    }

    // === Fields ==============================================================
    // Both run in ComponentDispatch's frame, where raising is safe.

    int PushField(lua_State* InState, const ScriptBindingEntry& InEntry, const uint8_t* InField)
    {
        switch(InEntry.FieldType)
        {
        case EScriptFieldType::Float:  lua_pushnumber(InState, *reinterpret_cast<const float*>(InField));    return 1;
        case EScriptFieldType::Double: lua_pushnumber(InState, *reinterpret_cast<const double*>(InField));   return 1;
        case EScriptFieldType::Int32:  lua_pushinteger(InState, *reinterpret_cast<const int32_t*>(InField)); return 1;
        case EScriptFieldType::UInt8:  lua_pushinteger(InState, *InField);                                     return 1;
        case EScriptFieldType::Bool:   lua_pushboolean(InState, *reinterpret_cast<const bool*>(InField));    return 1;
        case EScriptFieldType::Flag:   lua_pushboolean(InState, *InField != 0);                                return 1;

        case EScriptFieldType::Vector3:
        {
            const EngineCore::Vector3& V = *reinterpret_cast<const EngineCore::Vector3*>(InField);
            lua_pushnumber(InState, V.GetX());
            lua_pushnumber(InState, V.GetY());
            lua_pushnumber(InState, V.GetZ());
            return 3;
        }
        case EScriptFieldType::AABB:
        {
            const AABB& Box = *reinterpret_cast<const AABB*>(InField);
            lua_pushnumber(InState, Box.Min.GetX()); lua_pushnumber(InState, Box.Min.GetY()); lua_pushnumber(InState, Box.Min.GetZ());
            lua_pushnumber(InState, Box.Max.GetX()); lua_pushnumber(InState, Box.Max.GetY()); lua_pushnumber(InState, Box.Max.GetZ());
            return 6;
        }
        case EScriptFieldType::Color:
        {
            const EngineCore::Color& C = *reinterpret_cast<const EngineCore::Color*>(InField);
            lua_pushinteger(InState, C.R); lua_pushinteger(InState, C.G);
            lua_pushinteger(InState, C.B); lua_pushinteger(InState, C.A);
            return 4;
        }
        case EScriptFieldType::FixedString:
        {
            /* Bounded by the buffer, so an unterminated one is not read past. */
            const char* Text = reinterpret_cast<const char*>(InField);
            size_t Length = 0;
            while(Length < InEntry.Size && Text[Length] != '\0') { ++Length; }
            lua_pushlstring(InState, Text, Length);
            return 1;
        }
        }
        return 0;
    }

    uint8_t CheckByte(lua_State* InState, const int InArg)
    {
        const lua_Integer Value = luaL_checkinteger(InState, InArg);
        luaL_argcheck(InState, Value >= 0 && Value <= 255, InArg, "expected 0..255");
        return uint8_t(Value);
    }

    /* Arguments start at 2; 1 is the proxy. */
    void ReadField(lua_State* InState, const ScriptBindingEntry& InEntry, uint8_t* OutField)
    {
        switch(InEntry.FieldType)
        {
        case EScriptFieldType::Float:  *reinterpret_cast<float*>(OutField)   = float(luaL_checknumber(InState, 2)); return;
        case EScriptFieldType::Double: *reinterpret_cast<double*>(OutField)  = luaL_checknumber(InState, 2);        return;
        case EScriptFieldType::Int32:  *reinterpret_cast<int32_t*>(OutField) = int32_t(luaL_checkinteger(InState, 2)); return;
        case EScriptFieldType::UInt8:  *OutField                              = CheckByte(InState, 2);               return;
        case EScriptFieldType::Bool:   *reinterpret_cast<bool*>(OutField)    = lua_toboolean(InState, 2) != 0;      return;
        case EScriptFieldType::Flag:   *OutField                              = lua_toboolean(InState, 2) ? 1 : 0;   return;

        case EScriptFieldType::Vector3:
        {
            const double X = luaL_checknumber(InState, 2);
            const double Y = luaL_checknumber(InState, 3);
            const double Z = luaL_checknumber(InState, 4);
            *reinterpret_cast<EngineCore::Vector3*>(OutField) = EngineCore::Vector3(X, Y, Z);
            return;
        }
        case EScriptFieldType::AABB:
        {
            const double MinX = luaL_checknumber(InState, 2), MinY = luaL_checknumber(InState, 3), MinZ = luaL_checknumber(InState, 4);
            const double MaxX = luaL_checknumber(InState, 5), MaxY = luaL_checknumber(InState, 6), MaxZ = luaL_checknumber(InState, 7);
            *reinterpret_cast<AABB*>(OutField) = AABB(EngineCore::Vector3(MinX, MinY, MinZ), EngineCore::Vector3(MaxX, MaxY, MaxZ));
            return;
        }
        case EScriptFieldType::Color:
        {
            /* Alpha optional, defaulting to opaque. */
            EngineCore::Color& C = *reinterpret_cast<EngineCore::Color*>(OutField);
            const uint8_t R = CheckByte(InState, 2);
            const uint8_t G = CheckByte(InState, 3);
            const uint8_t B = CheckByte(InState, 4);
            const uint8_t A = lua_isnoneornil(InState, 5) ? uint8_t(255) : CheckByte(InState, 5);
            C.R = R; C.G = G; C.B = B; C.A = A;
            return;
        }
        case EScriptFieldType::FixedString:
        {
            size_t Length = 0;
            const char* Text = luaL_checklstring(InState, 2, &Length);
            char* Buffer = reinterpret_cast<char*>(OutField);
            const size_t Copy = (Length < InEntry.Size - 1) ? Length : InEntry.Size - 1;
            std::memcpy(Buffer, Text, Copy);
            std::memset(Buffer + Copy, 0, InEntry.Size - Copy);
            return;
        }
        }
    }

    // === Dispatch ============================================================

    int ComponentDispatch(lua_State* InState)
    {
        const ScriptBindingEntry* Entry = static_cast<const ScriptBindingEntry*>(lua_touserdata(InState, lua_upvalueindex(1)));
        const ComponentTypeID     Type  = ComponentTypeID(lua_tointeger(InState, lua_upvalueindex(2)));
        const int                 Mode  = int(lua_tointeger(InState, lua_upvalueindex(3)));

        char Metatable[96];
        MetatableName(Type, Metatable);
        const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, 1, Metatable));

        if(Mode == DM_GetEntity)
        {
            LuaPushEntity(InState, Proxy->Entity);
            return 1;
        }

        /* Re-fetched on every call, so a proxy held across a remove errors
         * instead of aliasing whatever now occupies the pool slot. */
        World* W = LuaGetBoundWorld(InState);
        void* Component = Entity(W, Proxy->Entity).GetComponentByType(Proxy->Type);
        if(!Component)
        {
            return luaL_error(InState, "%s proxy no longer refers to a live component", TypeNameOf(Type));
        }

        uint8_t* Field = static_cast<uint8_t*>(Component) + Entry->Offset;
        switch(Mode)
        {
        case DM_GetField:
            return PushField(InState, *Entry, Field);

        case DM_SetField:
            ReadField(InState, *Entry, Field);
            return 0;

        case DM_Method:
        {
            ScriptCall Call(InState, Entry->Name, Proxy->Entity, W);
            Entry->Invoker(Entry->Method, Component, Call);
            if(Call.HasError())
            {
                /* Raised here, after the binder's code has returned. */
                return luaL_error(InState, "%s", Call.GetErrorText());
            }
            return Call.GetResultCount();
        }
        }
        return 0;
    }

    void PushDispatch(lua_State* InState, const ScriptBindingEntry* InEntry, const ComponentTypeID InType,
                      const EDispatchMode InMode)
    {
        lua_pushlightuserdata(InState, const_cast<ScriptBindingEntry*>(InEntry));
        lua_pushinteger(InState, lua_Integer(InType));
        lua_pushinteger(InState, lua_Integer(InMode));
        lua_pushcclosure(InState, &ComponentDispatch, 3);
    }

    /* Sets Methods[InName] = the closure on top of the stack, warning if a
     * field's generated accessor and a method share a name. */
    void SetMethod(lua_State* InState, const ComponentTypeID InType, const char* InName)
    {
        lua_getfield(InState, -2, InName);
        const bool bTaken = !lua_isnil(InState, -1);
        lua_pop(InState, 1);
        if(bTaken)
        {
            GAMECORE_LOG_WARN(std::string("Component script: '") + TypeNameOf(InType) + "' binds '"
                              + InName + "' twice; the later binding wins")
        }
        lua_setfield(InState, -2, InName);
    }

    /* Builds InType's metatable on first use. Metatables live in the registry,
     * so a lua_State builds each one once. */
    void EnsureComponentMetatable(lua_State* InState, const ComponentTypeID InType)
    {
        char Metatable[96];
        MetatableName(InType, Metatable);
        if(luaL_newmetatable(InState, Metatable) == 0)
        {
            lua_pop(InState, 1);
            return;
        }

        lua_newtable(InState);   // the methods -- __index

        PushDispatch(InState, nullptr, InType, DM_GetEntity);
        SetMethod(InState, InType, "GetEntity");

        size_t Count = 0;
        const ScriptBindingEntry* Entries = FindComponentScript(InType, Count);
        for(size_t Index = 0; Index < Count; ++Index)
        {
            const ScriptBindingEntry& Entry = Entries[Index];
            if(Entry.bMethod)
            {
                PushDispatch(InState, &Entry, InType, DM_Method);
                SetMethod(InState, InType, Entry.Name);
                continue;
            }

            char Accessor[SCRIPT_BINDING_NAME_MAX + 4];
            std::snprintf(Accessor, sizeof(Accessor), "Get%s", Entry.Name);
            PushDispatch(InState, &Entry, InType, DM_GetField);
            SetMethod(InState, InType, Accessor);

            if(!Entry.bReadOnly)
            {
                std::snprintf(Accessor, sizeof(Accessor), "Set%s", Entry.Name);
                PushDispatch(InState, &Entry, InType, DM_SetField);
                SetMethod(InState, InType, Accessor);
            }
        }

        lua_setfield(InState, -2, "__index");
        lua_pop(InState, 1);
    }
}

// === Registry ===============================================================

bool RegisterComponentScript(const ComponentTypeID InType, const ScriptBindingEntry* InEntries,
                             const size_t InCount, const size_t InRejected)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        GAMECORE_LOG_ERROR("RegisterComponentScript: component type id " + std::to_string(InType) + " is out of range")
        return false;
    }

    if(InRejected > 0)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterComponentScript: '") + TypeNameOf(InType) + "' had "
                           + std::to_string(InRejected) + " binding(s) rejected -- more than "
                           + std::to_string(size_t(SCRIPT_BINDINGS_PER_TYPE_MAX))
                           + " bindings, or a name of " + std::to_string(size_t(SCRIPT_BINDING_NAME_MAX))
                           + "+ characters. Those will not exist in Lua.")
    }

    /* First wins, as RegisterComponentSerializer does: two binding sets for one
     * type would make the Lua API depend on registration order. */
    if(GScriptTypes[InType].bRegistered)
    {
        GAMECORE_LOG_WARN(std::string("RegisterComponentScript: '") + TypeNameOf(InType)
                          + "' already has script bindings; keeping the first")
        return false;
    }

    if(GScriptEntryCount + InCount > SCRIPT_ENTRY_MAX)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterComponentScript: the binding table is full; '")
                           + TypeNameOf(InType) + "' will have only GetEntity in Lua")
        return false;
    }

    GScriptTypes[InType].First       = uint16_t(GScriptEntryCount);
    GScriptTypes[InType].Count       = uint16_t(InCount);
    GScriptTypes[InType].bRegistered = true;
    for(size_t Index = 0; Index < InCount; ++Index)
    {
        GScriptEntries[GScriptEntryCount++] = InEntries[Index];
    }
    return true;
}

const ScriptBindingEntry* FindComponentScript(const ComponentTypeID InType, size_t& OutCount)
{
    OutCount = 0;
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || !GScriptTypes[InType].bRegistered)
    {
        return nullptr;
    }
    OutCount = GScriptTypes[InType].Count;
    return &GScriptEntries[GScriptTypes[InType].First];
}

// === Proxies ================================================================

void LuaPushComponentProxy(lua_State* InState, const EntityID InEntity, const ComponentTypeID InType)
{
    EnsureComponentMetatable(InState, InType);

    ComponentProxy* Proxy = static_cast<ComponentProxy*>(lua_newuserdatauv(InState, sizeof(ComponentProxy), 0));
    Proxy->Entity = InEntity;
    Proxy->Type   = InType;

    char Metatable[96];
    MetatableName(InType, Metatable);
    luaL_setmetatable(InState, Metatable);
}

// === ScriptCall =============================================================

/* ComponentDispatch raises after a ScriptCall is alive in its frame; that is
 * only safe while it has no destructor to skip. */
static_assert(std::is_trivially_destructible<ScriptCall>::value,
              "ScriptCall must stay trivially destructible -- ComponentDispatch longjmps past it");

ScriptCall::ScriptCall(lua_State* InState, const char* InMethodName, const EntityID InOwner, World* InWorld)
    : State(InState), MethodName(InMethodName ? InMethodName : ""), Owner(InOwner), OwnerWorld(InWorld)
{
}

int ScriptCall::GetArgCount() const { return lua_gettop(State) - 1; }

bool ScriptCall::HasArg(const int InArg) const { return !lua_isnoneornil(State, InArg + 1); }

void ScriptCall::Error(const char* InMessage)
{
    if(HasError()) { return; }
    std::snprintf(ErrorText, sizeof(ErrorText), "%s: %s", MethodName, InMessage ? InMessage : "error");
}

void ScriptCall::ArgError(const int InArg, const char* InExpected)
{
    if(HasError()) { return; }
    std::snprintf(ErrorText, sizeof(ErrorText), "bad argument #%d to '%s' (%s expected)", InArg, MethodName, InExpected);
}

double ScriptCall::GetNumber(const int InArg)
{
    if(HasError()) { return 0.0; }
    int bIsNumber = 0;
    const lua_Number Value = lua_tonumberx(State, InArg + 1, &bIsNumber);
    if(!bIsNumber) { ArgError(InArg, "number"); return 0.0; }
    return Value;
}

float ScriptCall::GetFloat(const int InArg) { return float(GetNumber(InArg)); }

int32_t ScriptCall::GetInt(const int InArg)
{
    if(HasError()) { return 0; }
    int bIsInteger = 0;
    const lua_Integer Value = lua_tointegerx(State, InArg + 1, &bIsInteger);
    if(!bIsInteger) { ArgError(InArg, "integer"); return 0; }
    return int32_t(Value);
}

bool ScriptCall::GetBool(const int InArg) { return lua_toboolean(State, InArg + 1) != 0; }

const char* ScriptCall::GetString(const int InArg)
{
    if(HasError()) { return ""; }
    const int Type = lua_type(State, InArg + 1);
    if(Type != LUA_TSTRING && Type != LUA_TNUMBER) { ArgError(InArg, "string"); return ""; }
    return lua_tostring(State, InArg + 1);
}

EngineCore::Vector3 ScriptCall::GetVector3(const int InFirstArg)
{
    const double X = GetNumber(InFirstArg);
    const double Y = GetNumber(InFirstArg + 1);
    const double Z = GetNumber(InFirstArg + 2);
    return EngineCore::Vector3(X, Y, Z);
}

/* lua_checkstack rather than assuming the 20-slot minimum a C function gets;
 * it reports failure instead of raising. */
#define SCRIPTCALL_RESERVE(Count) \
    if(HasError()) { return; }    \
    if(!lua_checkstack(State, (Count))) { Error("too many results"); return; }

void ScriptCall::PushNumber(const double InValue)      { SCRIPTCALL_RESERVE(1) lua_pushnumber(State, InValue);  ++Results; }
void ScriptCall::PushBool(const bool InValue)          { SCRIPTCALL_RESERVE(1) lua_pushboolean(State, InValue); ++Results; }
void ScriptCall::PushString(const char* InValue)       { SCRIPTCALL_RESERVE(1) lua_pushstring(State, InValue ? InValue : ""); ++Results; }
void ScriptCall::PushNil()                             { SCRIPTCALL_RESERVE(1) lua_pushnil(State); ++Results; }
void ScriptCall::PushEntity(const EntityID InEntity)   { SCRIPTCALL_RESERVE(1) LuaPushEntity(State, InEntity); ++Results; }

void ScriptCall::PushVector3(const EngineCore::Vector3& InValue)
{
    SCRIPTCALL_RESERVE(3)
    lua_pushnumber(State, InValue.GetX());
    lua_pushnumber(State, InValue.GetY());
    lua_pushnumber(State, InValue.GetZ());
    Results += 3;
}

#undef SCRIPTCALL_RESERVE
}

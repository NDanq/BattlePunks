#include "Private/GameCore/GameCoreInternal.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"

/*
 * The Lua-visible surface: World, Entity, TickPhase and Events. Component
 * proxies and their methods live in ComponentScript.cpp, and the built-in
 * components' bindings in ComponentScripts.cpp. See DESIGN.md.
 */

namespace GameCore
{
    /* ScriptSystem.cpp's entry points. Declared HERE, at GameCore namespace
     * scope, NOT inside the anonymous namespace below -- a declaration in an
     * anonymous namespace gets internal linkage and silently becomes a
     * different, never-defined symbol. See DESIGN.md for the link error that
     * caught this. */
    void ScriptSystem_AddLuaSystem(lua_State* InState, World* InWorld, const char* InName, ETickPhase InPhase, int InFunctionRef);
    bool ScriptSystem_RemoveLuaSystem(lua_State* InState, World* InWorld, const char* InName);

    namespace
    {
        constexpr const char* ENTITY_METATABLE = "GameCore.Entity";

        World* GetBoundWorld(lua_State* InState)
        {
            /* Stashed in the registry at the top of ScriptBindings_Install.
             * A registry slot, not a global in this file, since BindWorld's
             * "one at a time" contract makes "the current World" a per-state
             * concept. */
            lua_pushlightuserdata(InState, const_cast<void*>(static_cast<const void*>(&ENTITY_METATABLE)));
            lua_gettable(InState, LUA_REGISTRYINDEX);
            World* Result = static_cast<World*>(lua_touserdata(InState, -1));
            lua_pop(InState, 1);
            return Result;
        }

        void SetBoundWorld(lua_State* InState, World* InWorld)
        {
            lua_pushlightuserdata(InState, const_cast<void*>(static_cast<const void*>(&ENTITY_METATABLE)));
            lua_pushlightuserdata(InState, InWorld);
            lua_settable(InState, LUA_REGISTRYINDEX);
        }

        // === Entity userdata =================================================

        int Entity_IsValid(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            World* W = GetBoundWorld(InState);
            lua_pushboolean(InState, W && W->IsAlive(E));
            return 1;
        }

        int Entity_GetID(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            lua_pushinteger(InState, lua_Integer(E.Pack()));
            return 1;
        }

        int Entity_GetName(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            World* W = GetBoundWorld(InState);
            lua_pushstring(InState, Entity(W, E).GetName());
            return 1;
        }

        /* The persistent GUID as 32 hex digits; "" if not alive. */
        int Entity_GetGuid(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            World* W = GetBoundWorld(InState);
            const EntityGuid Guid = Entity(W, E).GetGuid();
            char Text[EntityGuid::STRING_BUFFER] = {};
            if(Guid.IsSet()) { Guid.ToString(Text); }
            lua_pushstring(InState, Text);
            return 1;
        }

        /* e:SetName(name) -- nil clears. */
        int Entity_SetName(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* Name = luaL_optstring(InState, 2, nullptr);
            World* W = GetBoundWorld(InState);
            lua_pushboolean(InState, Entity(W, E).SetName(Name));
            return 1;
        }

        int Entity_Destroy(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            if(World* W = GetBoundWorld(InState)) { W->DestroyEntity(E); }
            return 0;
        }

        int Entity_AddComponent(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* TypeName = luaL_checkstring(InState, 2);
            World* W = GetBoundWorld(InState);
            const ComponentTypeInfo* Info = FindComponentType(TypeName);
            if(!W || !Info)
            {
                lua_pushnil(InState);
                return 1;
            }

            /* Zero-initialised init bytes -- see DESIGN.md for why this
             * generic by-name path cannot call a real constructor. */
            std::vector<uint8_t> ZeroInit(Info->SizeBytes, 0);
            if(!Entity(W, E).AddComponentByType(Info->ID, ZeroInit.data()))
            {
                lua_pushnil(InState);
                return 1;
            }
            LuaPushComponentProxy(InState, E, Info->ID);
            return 1;
        }

        int Entity_GetComponent(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* TypeName = luaL_checkstring(InState, 2);
            World* W = GetBoundWorld(InState);
            const ComponentTypeInfo* Info = FindComponentType(TypeName);
            if(!W || !Info || !Entity(W, E).HasComponentByType(Info->ID))
            {
                lua_pushnil(InState);
                return 1;
            }
            LuaPushComponentProxy(InState, E, Info->ID);
            return 1;
        }

        int Entity_HasComponent(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* TypeName = luaL_checkstring(InState, 2);
            World* W = GetBoundWorld(InState);
            const ComponentTypeInfo* Info = FindComponentType(TypeName);
            lua_pushboolean(InState, Info && Entity(W, E).HasComponentByType(Info->ID));
            return 1;
        }

        int Entity_RemoveComponent(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* TypeName = luaL_checkstring(InState, 2);
            World* W = GetBoundWorld(InState);
            const ComponentTypeInfo* Info = FindComponentType(TypeName);
            lua_pushboolean(InState, Info && Entity(W, E).RemoveComponentByType(Info->ID));
            return 1;
        }

        /* e:SetScript("Scripts/Mover.lua") -- Entity::SetScript. nil or ""
         * clears. Deferred (the World is ticking). */
        int Entity_SetScript(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* Path = luaL_optstring(InState, 2, nullptr);
            World* W = GetBoundWorld(InState);
            lua_pushboolean(InState, Entity(W, E).SetScript(Path));
            return 1;
        }

        int Entity_ClearScript(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            World* W = GetBoundWorld(InState);
            lua_pushboolean(InState, Entity(W, E).ClearScript());
            return 1;
        }

        /* The path, or nil if there is none. */
        int Entity_GetScript(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            World* W = GetBoundWorld(InState);
            const char* Path = Entity(W, E).GetScriptPath();
            if(Path[0] == '\0') { lua_pushnil(InState); }
            else                 { lua_pushstring(InState, Path); }
            return 1;
        }

        const luaL_Reg GEntityMethods[] = {
            { "IsValid",         Entity_IsValid },
            { "GetID",           Entity_GetID },
            { "GetName",         Entity_GetName },
            { "GetGuid",         Entity_GetGuid },
            { "SetName",         Entity_SetName },
            { "Destroy",         Entity_Destroy },
            { "AddComponent",    Entity_AddComponent },
            { "GetComponent",    Entity_GetComponent },
            { "HasComponent",    Entity_HasComponent },
            { "RemoveComponent", Entity_RemoveComponent },
            { "SetScript",       Entity_SetScript },
            { "GetScript",       Entity_GetScript },
            { "ClearScript",     Entity_ClearScript },
            { nullptr, nullptr }
        };

        // === World table =====================================================

        int World_CreateEntity(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            if(!W) { return luaL_error(InState, "no World is bound"); }
            const char* Name = lua_isstring(InState, 1) ? lua_tostring(InState, 1) : nullptr;
            const EntityID E = Name ? W->CreateEntity(Name) : W->CreateEntity();
            LuaPushEntity(InState, E);
            return 1;
        }

        int World_DestroyEntity(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const EntityID E = LuaCheckEntity(InState, 1);
            if(W) { W->DestroyEntity(E); }
            return 0;
        }

        int World_FindEntity(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const char* Name = luaL_checkstring(InState, 1);
            const EntityID E = W ? W->FindEntityByName(Name) : EntityID();
            if(!E.IsSet())
            {
                lua_pushnil(InState);
                return 1;
            }
            LuaPushEntity(InState, E);
            return 1;
        }

        int World_GetEntityCount(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            lua_pushinteger(InState, lua_Integer(W ? W->GetEntityCount() : 0));
            return 1;
        }

        int World_Each(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const char* TypeName = luaL_checkstring(InState, 1);
            luaL_checktype(InState, 2, LUA_TFUNCTION);
            const ComponentTypeInfo* Info = W ? FindComponentType(TypeName) : nullptr;
            if(!W || !Info) { return 0; }

            ComponentPool* Pool = W->FindPool(Info->ID);
            if(!Pool) { return 0; }

            const size_t Count = Pool->GetCount();
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const EntityID E = Pool->GetEntityAt(Index);
                lua_pushvalue(InState, 2);
                LuaPushEntity(InState, E);
                LuaPushComponentProxy(InState, E, Info->ID);
                LuaCallProtected(InState, 2, 0, "World.Each callback");
            }
            return 0;
        }

        int World_AddSystem(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const char* Name = luaL_checkstring(InState, 1);
            const lua_Integer PhaseValue = luaL_checkinteger(InState, 2);
            luaL_checktype(InState, 3, LUA_TFUNCTION);
            if(!W || PhaseValue < 0 || PhaseValue >= TP_MAX)
            {
                return luaL_error(InState, "World.AddSystem given a bad phase or no World is bound");
            }
            lua_pushvalue(InState, 3);
            const int FunctionRef = luaL_ref(InState, LUA_REGISTRYINDEX);
            ScriptSystem_AddLuaSystem(InState, W, Name, ETickPhase(PhaseValue), FunctionRef);
            return 0;
        }

        int World_RemoveSystem(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const char* Name = luaL_checkstring(InState, 1);
            lua_pushboolean(InState, W && ScriptSystem_RemoveLuaSystem(InState, W, Name));
            return 1;
        }

        int World_QueryBounds(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const double MinX = luaL_checknumber(InState, 1), MinY = luaL_checknumber(InState, 2), MinZ = luaL_checknumber(InState, 3);
            const double MaxX = luaL_checknumber(InState, 4), MaxY = luaL_checknumber(InState, 5), MaxZ = luaL_checknumber(InState, 6);
            luaL_checktype(InState, 7, LUA_TFUNCTION);
            if(!W) { return 0; }

            const AABB Box(EngineCore::Vector3(MinX, MinY, MinZ), EngineCore::Vector3(MaxX, MaxY, MaxZ));

            /* The Lua callback is luaL_ref'd for this one call -- see
             * DESIGN.md for why a raw SpatialQueryFn cannot close over it. */
            struct QueryContext { lua_State* State; int FunctionRef; bool bStop; };
            lua_pushvalue(InState, 7);
            QueryContext Ctx{ InState, luaL_ref(InState, LUA_REGISTRYINDEX), false };

            W->QueryBounds(Box, [](const EntityID InEntity, void* InUserData) -> bool
            {
                QueryContext& C = *static_cast<QueryContext*>(InUserData);
                lua_rawgeti(C.State, LUA_REGISTRYINDEX, C.FunctionRef);
                LuaPushEntity(C.State, InEntity);
                LuaCallProtected(C.State, 1, 1, "World.QueryBounds callback");
                const bool bContinue = lua_isnil(C.State, -1) || lua_toboolean(C.State, -1);
                lua_pop(C.State, 1);
                return bContinue;
            }, &Ctx);

            luaL_unref(InState, LUA_REGISTRYINDEX, Ctx.FunctionRef);
            return 0;
        }

        int World_Raycast(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const double OX = luaL_checknumber(InState, 1), OY = luaL_checknumber(InState, 2), OZ = luaL_checknumber(InState, 3);
            const double DX = luaL_checknumber(InState, 4), DY = luaL_checknumber(InState, 5), DZ = luaL_checknumber(InState, 6);
            const double MaxT = luaL_checknumber(InState, 7);
            if(!W) { lua_pushnil(InState); return 1; }

            RaycastHit Hit;
            if(!W->Raycast(EngineCore::Vector3(OX, OY, OZ), EngineCore::Vector3(DX, DY, DZ), float(MaxT), Hit))
            {
                lua_pushnil(InState);
                return 1;
            }
            LuaPushEntity(InState, Hit.Entity);
            lua_pushnumber(InState, Hit.T);
            return 2;
        }

        int World_SetActiveRegion(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const double MinX = luaL_checknumber(InState, 1), MinY = luaL_checknumber(InState, 2), MinZ = luaL_checknumber(InState, 3);
            const double MaxX = luaL_checknumber(InState, 4), MaxY = luaL_checknumber(InState, 5), MaxZ = luaL_checknumber(InState, 6);
            if(W) { W->SetActiveRegion(AABB(EngineCore::Vector3(MinX, MinY, MinZ), EngineCore::Vector3(MaxX, MaxY, MaxZ))); }
            return 0;
        }

        int World_ClearActiveRegion(lua_State* InState)
        {
            if(World* W = GetBoundWorld(InState)) { W->ClearActiveRegion(); }
            return 0;
        }

        const luaL_Reg GWorldFunctions[] = {
            { "CreateEntity",     World_CreateEntity },
            { "DestroyEntity",    World_DestroyEntity },
            { "FindEntity",       World_FindEntity },
            { "GetEntityCount",   World_GetEntityCount },
            { "Each",             World_Each },
            { "AddSystem",        World_AddSystem },
            { "RemoveSystem",     World_RemoveSystem },
            { "QueryBounds",      World_QueryBounds },
            { "Raycast",          World_Raycast },
            { "SetActiveRegion",  World_SetActiveRegion },
            { "ClearActiveRegion", World_ClearActiveRegion },
            { nullptr, nullptr }
        };

        // === Events table ====================================================

        /* The one C++ thunk every Lua Events.On() subscription shares.
         * InUserData is a heap-allocated LuaEventBinding, released by
         * Events.Off or by ScriptSystem_Uninstall's sweep at unbind time. */
        struct LuaEventBinding
        {
            lua_State* State;
            int        FunctionRef;
            EventID    Event;
        };

        void LuaEventThunk(EventID /*InEvent*/, const EventArgs& InArgs, void* InUserData)
        {
            LuaEventBinding* Binding = static_cast<LuaEventBinding*>(InUserData);
            lua_State* L = Binding->State;

            lua_rawgeti(L, LUA_REGISTRYINDEX, Binding->FunctionRef);
            for(uint8_t Index = 0; Index < InArgs.Count; ++Index)
            {
                const EventArg& Arg = InArgs.Args[Index];
                switch(Arg.Type)
                {
                    case EventArg::ET_Number: lua_pushnumber(L, Arg.Number); break;
                    case EventArg::ET_Bool:   lua_pushboolean(L, Arg.Bool); break;
                    case EventArg::ET_String: lua_pushstring(L, Arg.String); break;
                    case EventArg::ET_Entity: LuaPushEntity(L, EntityID::Unpack(Arg.Entity)); break;
                    default:                  lua_pushnil(L); break;
                }
            }
            LuaCallProtected(L, int(InArgs.Count), 0, "Events subscriber");
        }

        int Events_On(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const char* Name = luaL_checkstring(InState, 1);
            luaL_checktype(InState, 2, LUA_TFUNCTION);
            if(!W) { return luaL_error(InState, "no World is bound"); }

            lua_pushvalue(InState, 2);
            const int FunctionRef = luaL_ref(InState, LUA_REGISTRYINDEX);

            LuaEventBinding* Binding = new LuaEventBinding();
            Binding->State       = InState;
            Binding->FunctionRef = FunctionRef;
            Binding->Event       = W->GetEvents().GetEventID(Name);

            const DelegateHandle Handle = W->GetEvents().Subscribe(Binding->Event, &LuaEventThunk, Binding);

            /* {Index, Serial} packed into one integer -- see DESIGN.md for
             * why the Binding pointer itself is never exposed to Lua. */
            lua_pushinteger(InState, (lua_Integer(Handle.Serial) << 32) | lua_Integer(Handle.Index));
            return 1;
        }

        int Events_Off(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const lua_Integer Packed = luaL_checkinteger(InState, 1);
            if(!W) { return 0; }

            DelegateHandle Handle;
            Handle.Index  = uint32_t(Packed & 0xFFFFFFFFu);
            Handle.Serial = uint32_t(uint64_t(Packed) >> 32);

            /* Tries every interned event, bounded by GetEventCount() -- see
             * DESIGN.md for why this is cheap and safe, and why the
             * LuaEventBinding is not deleted here. */
            const EventID EventCount = EventID(W->GetEvents().GetEventCount());
            for(EventID Event = 0; Event < EventCount; ++Event)
            {
                if(W->GetEvents().Unsubscribe(Event, Handle)) { break; }
            }
            return 0;
        }

        int Events_Emit(lua_State* InState)
        {
            World* W = GetBoundWorld(InState);
            const char* Name = luaL_checkstring(InState, 1);
            if(!W) { return 0; }

            EventArgs Args;
            const int ArgCount = lua_gettop(InState) - 1;
            for(int Index = 0; Index < ArgCount && Index < 4; ++Index)
            {
                const int StackIndex = Index + 2;
                if(lua_type(InState, StackIndex) == LUA_TSTRING)
                {
                    Args.Args[Index] = EventArg::Make(lua_tostring(InState, StackIndex));
                }
                else if(lua_type(InState, StackIndex) == LUA_TBOOLEAN)
                {
                    Args.Args[Index] = EventArg::Make(lua_toboolean(InState, StackIndex) != 0);
                }
                else if(lua_isuserdata(InState, StackIndex) && luaL_testudata(InState, StackIndex, ENTITY_METATABLE))
                {
                    Args.Args[Index] = EventArg::Make(LuaCheckEntity(InState, StackIndex));
                }
                else
                {
                    Args.Args[Index] = EventArg::Make(luaL_optnumber(InState, StackIndex, 0.0));
                }
                ++Args.Count;
            }

            W->GetEvents().Emit(Name, Args);
            return 0;
        }

        const luaL_Reg GEventsFunctions[] = {
            { "On",   Events_On },
            { "Off",  Events_Off },
            { "Emit", Events_Emit },
            { nullptr, nullptr }
        };

        // === installers ======================================================

        void RegisterMetatable(lua_State* InState, const char* InName, const luaL_Reg* InMethods)
        {
            luaL_newmetatable(InState, InName);
            lua_newtable(InState);
            luaL_setfuncs(InState, InMethods, 0);
            lua_setfield(InState, -2, "__index");
            lua_pop(InState, 1);
        }
    }

    void LuaPushEntity(lua_State* InState, const EntityID InEntity)
    {
        EntityID* Slot = static_cast<EntityID*>(lua_newuserdatauv(InState, sizeof(EntityID), 0));
        *Slot = InEntity;
        luaL_setmetatable(InState, ENTITY_METATABLE);
    }

    EntityID LuaCheckEntity(lua_State* InState, const int InIndex)
    {
        const EntityID* Slot = static_cast<const EntityID*>(luaL_checkudata(InState, InIndex, ENTITY_METATABLE));
        return *Slot;
    }

    World* LuaGetBoundWorld(lua_State* InState)
    {
        return GetBoundWorld(InState);
    }

    bool LuaToEntity(lua_State* InState, const int InIndex, EntityID& OutEntity)
    {
        const EntityID* Slot = static_cast<const EntityID*>(luaL_testudata(InState, InIndex, ENTITY_METATABLE));
        if(!Slot) { return false; }
        OutEntity = *Slot;
        return true;
    }

    void ScriptBindings_Install(lua_State* InState, World* InWorld)
    {
        SetBoundWorld(InState, InWorld);

        /* Component metatables are not registered here: ComponentScript.cpp
         * builds each type's the first time one of its proxies is pushed. */
        RegisterMetatable(InState, ENTITY_METATABLE, GEntityMethods);

        lua_newtable(InState);
        luaL_setfuncs(InState, GWorldFunctions, 0);
        lua_setglobal(InState, "World");

        lua_newtable(InState);
        luaL_setfuncs(InState, GEventsFunctions, 0);
        lua_setglobal(InState, "Events");

        lua_newtable(InState);
        lua_pushinteger(InState, TP_PrePhysics);  lua_setfield(InState, -2, "PrePhysics");
        lua_pushinteger(InState, TP_Physics);     lua_setfield(InState, -2, "Physics");
        lua_pushinteger(InState, TP_PostPhysics); lua_setfield(InState, -2, "PostPhysics");
        lua_pushinteger(InState, TP_PostUpdate);  lua_setfield(InState, -2, "PostUpdate");
        lua_setglobal(InState, "TickPhase");

        /* Last, so a library can never replace one of the tables above. */
        ScriptLibrary_Install(InState);
    }
}

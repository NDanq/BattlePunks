#include "Private/GameCore/GameCoreInternal.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"

/*
 * The Lua-visible surface: World, Entity, the component proxies for
 * Transform/Name/Bounds, TickPhase, and Events. See World.h's "The Lua
 * surface" doc comment for the API, and DESIGN.md for the userdata layouts.
 */

namespace GameCore
{
    /* ScriptSystem.cpp's entry points. Declared HERE, at GameCore namespace
     * scope, NOT inside the anonymous namespace below -- a declaration in an
     * anonymous namespace gets internal linkage and silently becomes a
     * different, never-defined symbol. See DESIGN.md for the link error that
     * caught this. */
    bool ScriptSystem_InstantiateScript(lua_State* InState, World* InWorld, EntityID InEntity, const char* InClassName);
    void ScriptSystem_AddLuaSystem(lua_State* InState, World* InWorld, const char* InName, ETickPhase InPhase, int InFunctionRef);
    bool ScriptSystem_RemoveLuaSystem(lua_State* InState, World* InWorld, const char* InName);

    namespace
    {
        constexpr const char* ENTITY_METATABLE = "GameCore.Entity";

        struct ComponentProxy
        {
            EntityID        Entity;
            ComponentTypeID Type = COMPONENT_TYPE_INVALID;
        };

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
            const NameComponent* Name = W ? static_cast<const NameComponent*>(W->GetComponentRaw(E, NameComponent::StaticType().ID)) : nullptr;
            lua_pushstring(InState, Name ? Name->Name : "");
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
            if(!W->AddComponentRaw(E, Info->ID, ZeroInit.data()))
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
            if(!W || !Info || !W->HasComponentRaw(E, Info->ID))
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
            lua_pushboolean(InState, W && Info && W->HasComponentRaw(E, Info->ID));
            return 1;
        }

        int Entity_RemoveComponent(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* TypeName = luaL_checkstring(InState, 2);
            World* W = GetBoundWorld(InState);
            const ComponentTypeInfo* Info = FindComponentType(TypeName);
            lua_pushboolean(InState, W && Info && W->RemoveComponentRaw(E, Info->ID));
            return 1;
        }

        /* ScriptSystem.cpp's entry point -- e:AddScript("ClassName") is the
         * Lua spelling of ScriptEngine::InstantiateScript, sharing the exact
         * same implementation. Declared at GameCore namespace scope above,
         * not re-declared here. */
        int Entity_AddScript(lua_State* InState)
        {
            const EntityID E = LuaCheckEntity(InState, 1);
            const char* ClassName = luaL_checkstring(InState, 2);
            World* W = GetBoundWorld(InState);
            lua_pushboolean(InState, W && ScriptSystem_InstantiateScript(InState, W, E, ClassName));
            return 1;
        }

        const luaL_Reg GEntityMethods[] = {
            { "IsValid",         Entity_IsValid },
            { "GetID",           Entity_GetID },
            { "GetName",         Entity_GetName },
            { "Destroy",         Entity_Destroy },
            { "AddComponent",    Entity_AddComponent },
            { "GetComponent",    Entity_GetComponent },
            { "HasComponent",    Entity_HasComponent },
            { "RemoveComponent", Entity_RemoveComponent },
            { "AddScript",       Entity_AddScript },
            { nullptr, nullptr }
        };

        // === Transform proxy ================================================

        TransformComponent* CheckTransformProxy(lua_State* InState, const int InIndex)
        {
            const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, InIndex, "Transform"));
            World* W = GetBoundWorld(InState);
            return W ? static_cast<TransformComponent*>(W->GetComponentRaw(Proxy->Entity, Proxy->Type)) : nullptr;
        }

        int Transform_GetEntity(lua_State* InState)
        {
            const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, 1, "Transform"));
            LuaPushEntity(InState, Proxy->Entity);
            return 1;
        }

        int Transform_GetPosition(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            lua_pushnumber(InState, T->Position.GetX());
            lua_pushnumber(InState, T->Position.GetY());
            lua_pushnumber(InState, T->Position.GetZ());
            return 3;
        }

        int Transform_SetPosition(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            const double X = luaL_checknumber(InState, 2);
            const double Y = luaL_checknumber(InState, 3);
            const double Z = luaL_checknumber(InState, 4);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            T->Position = EngineCore::Vector3(X, Y, Z);
            return 0;
        }

        int Transform_Translate(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            const double DX = luaL_checknumber(InState, 2);
            const double DY = luaL_checknumber(InState, 3);
            const double DZ = luaL_checknumber(InState, 4);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            T->Position = T->Position + EngineCore::Vector3(DX, DY, DZ);
            return 0;
        }

        int Transform_GetRotation(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            lua_pushnumber(InState, T->Rotation.GetX());
            lua_pushnumber(InState, T->Rotation.GetY());
            lua_pushnumber(InState, T->Rotation.GetZ());
            return 3;
        }

        int Transform_SetRotation(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            const double X = luaL_checknumber(InState, 2);
            const double Y = luaL_checknumber(InState, 3);
            const double Z = luaL_checknumber(InState, 4);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            T->Rotation = EngineCore::Vector3(X, Y, Z);
            return 0;
        }

        int Transform_GetScale(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            lua_pushnumber(InState, T->Scale.GetX());
            lua_pushnumber(InState, T->Scale.GetY());
            lua_pushnumber(InState, T->Scale.GetZ());
            return 3;
        }

        int Transform_SetScale(lua_State* InState)
        {
            TransformComponent* T = CheckTransformProxy(InState, 1);
            const double X = luaL_checknumber(InState, 2);
            const double Y = luaL_checknumber(InState, 3);
            const double Z = luaL_checknumber(InState, 4);
            if(!T) { return luaL_error(InState, "Transform proxy no longer refers to a live component"); }
            T->Scale = EngineCore::Vector3(X, Y, Z);
            return 0;
        }

        const luaL_Reg GTransformMethods[] = {
            { "GetEntity",    Transform_GetEntity },
            { "GetPosition",  Transform_GetPosition },
            { "SetPosition",  Transform_SetPosition },
            { "Translate",    Transform_Translate },
            { "GetRotation",  Transform_GetRotation },
            { "SetRotation",  Transform_SetRotation },
            { "GetScale",     Transform_GetScale },
            { "SetScale",     Transform_SetScale },
            { nullptr, nullptr }
        };

        // === Name proxy ======================================================

        NameComponent* CheckNameProxy(lua_State* InState, const int InIndex)
        {
            const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, InIndex, "Name"));
            World* W = GetBoundWorld(InState);
            return W ? static_cast<NameComponent*>(W->GetComponentRaw(Proxy->Entity, Proxy->Type)) : nullptr;
        }

        int Name_GetEntity(lua_State* InState)
        {
            const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, 1, "Name"));
            LuaPushEntity(InState, Proxy->Entity);
            return 1;
        }

        int Name_Get(lua_State* InState)
        {
            NameComponent* N = CheckNameProxy(InState, 1);
            if(!N) { return luaL_error(InState, "Name proxy no longer refers to a live component"); }
            lua_pushstring(InState, N->Name);
            return 1;
        }

        int Name_Set(lua_State* InState)
        {
            NameComponent* N = CheckNameProxy(InState, 1);
            const char* NewName = luaL_checkstring(InState, 2);
            if(!N) { return luaL_error(InState, "Name proxy no longer refers to a live component"); }
            std::strncpy(N->Name, NewName, sizeof(N->Name) - 1);
            N->Name[sizeof(N->Name) - 1] = '\0';
            return 0;
        }

        const luaL_Reg GNameMethods[] = {
            { "GetEntity", Name_GetEntity },
            { "Get",       Name_Get },
            { "Set",       Name_Set },
            { nullptr, nullptr }
        };

        // === Bounds proxy ====================================================

        BoundsComponent* CheckBoundsProxy(lua_State* InState, const int InIndex)
        {
            const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, InIndex, "Bounds"));
            World* W = GetBoundWorld(InState);
            return W ? static_cast<BoundsComponent*>(W->GetComponentRaw(Proxy->Entity, Proxy->Type)) : nullptr;
        }

        int Bounds_GetEntity(lua_State* InState)
        {
            const ComponentProxy* Proxy = static_cast<const ComponentProxy*>(luaL_checkudata(InState, 1, "Bounds"));
            LuaPushEntity(InState, Proxy->Entity);
            return 1;
        }

        int Bounds_SetLocal(lua_State* InState)
        {
            BoundsComponent* B = CheckBoundsProxy(InState, 1);
            const double MinX = luaL_checknumber(InState, 2), MinY = luaL_checknumber(InState, 3), MinZ = luaL_checknumber(InState, 4);
            const double MaxX = luaL_checknumber(InState, 5), MaxY = luaL_checknumber(InState, 6), MaxZ = luaL_checknumber(InState, 7);
            if(!B) { return luaL_error(InState, "Bounds proxy no longer refers to a live component"); }
            B->Local = AABB(EngineCore::Vector3(MinX, MinY, MinZ), EngineCore::Vector3(MaxX, MaxY, MaxZ));
            return 0;
        }

        int Bounds_GetWorld(lua_State* InState)
        {
            BoundsComponent* B = CheckBoundsProxy(InState, 1);
            if(!B) { return luaL_error(InState, "Bounds proxy no longer refers to a live component"); }
            lua_pushnumber(InState, B->World.Min.GetX()); lua_pushnumber(InState, B->World.Min.GetY()); lua_pushnumber(InState, B->World.Min.GetZ());
            lua_pushnumber(InState, B->World.Max.GetX()); lua_pushnumber(InState, B->World.Max.GetY()); lua_pushnumber(InState, B->World.Max.GetZ());
            return 6;
        }

        int Bounds_SetStatic(lua_State* InState)
        {
            BoundsComponent* B = CheckBoundsProxy(InState, 1);
            const bool bStatic = lua_toboolean(InState, 2) != 0;
            if(!B) { return luaL_error(InState, "Bounds proxy no longer refers to a live component"); }
            B->bStatic = bStatic ? 1 : 0;
            return 0;
        }

        const luaL_Reg GBoundsMethods[] = {
            { "GetEntity", Bounds_GetEntity },
            { "SetLocal",  Bounds_SetLocal },
            { "GetWorld",  Bounds_GetWorld },
            { "SetStatic", Bounds_SetStatic },
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

    void LuaPushComponentProxy(lua_State* InState, const EntityID InEntity, const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        ComponentProxy* Proxy = static_cast<ComponentProxy*>(lua_newuserdatauv(InState, sizeof(ComponentProxy), 0));
        Proxy->Entity = InEntity;
        Proxy->Type   = InType;
        luaL_setmetatable(InState, Info ? Info->Name : "Unknown");
    }

    void ScriptBindings_Install(lua_State* InState, World* InWorld)
    {
        SetBoundWorld(InState, InWorld);

        RegisterMetatable(InState, ENTITY_METATABLE, GEntityMethods);
        RegisterMetatable(InState, "Transform", GTransformMethods);
        RegisterMetatable(InState, "Name",      GNameMethods);
        RegisterMetatable(InState, "Bounds",    GBoundsMethods);

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
    }
}

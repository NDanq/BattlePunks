#include "Private/GameCore/GameCoreInternal.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/World.h"

/*
 * ScriptComponent's whole lifecycle: Script.Register, instantiation (shared by
 * the Lua e:AddScript() path and ScriptEngine::InstantiateScript), the
 * per-phase tick, Lua-defined systems (World.AddSystem), and the teardown that
 * runs at World unbind.
 *
 * THE CLASS REGISTRY. Script.Register(name, table) stores table in the Lua
 * REGISTRY under a private key, keyed by name -- registry space, not a Lua
 * global, so a script cannot accidentally clobber "Player" by assigning a
 * variable of that name at file scope. Reading it back is one lua_gettable.
 *
 * ONE SYSTEM PER PHASE, NOT ONE OVERALL. ScriptSystem_Install registers FOUR
 * systems -- one per ETickPhase -- each a thin C++ trampoline that walks the
 * ScriptComponent pool ONCE and skips any instance whose own Phase byte does
 * not match. World's own systems are per-phase buckets (see World.h), so this
 * is what lets a ScriptComponent declare TickPhase = TickPhase.PostPhysics and
 * have ITS OnTick actually run during PostPhysics without ScriptComponent
 * needing to be four different component types.
 */

namespace GameCore
{
    void ScriptSystem_AddLuaSystem(lua_State* InState, World* InWorld, const char* InName, ETickPhase InPhase, int InFunctionRef);
    bool ScriptSystem_RemoveLuaSystem(lua_State* InState, World* InWorld, const char* InName);

    namespace
    {
        constexpr const char* CLASS_REGISTRY_KEY = "GameCore.ScriptClasses";

        struct LuaSystemEntry
        {
            std::string Name;
            int         FunctionRef = LUA_NOREF;
        };

        /*
         * Per-World script state that does not belong on any single component
         * or on ScriptEngine::Impl (which is private to ScriptEngine.cpp) --
         * the Lua-defined systems a World is carrying, so Uninstall can
         * release every one of their refs, and the removal-observer plumbing.
         * Owned by ScriptSystem_Install, torn down by ScriptSystem_Uninstall;
         * there is exactly one of these per bound World, found through a
         * light userdata key in the Lua registry the same way GetBoundWorld
         * finds its World in ScriptBindings.cpp.
         */
        struct WorldScriptState
        {
            World* Owner = nullptr;
            std::vector<LuaSystemEntry> LuaSystems[TP_MAX];
        };

        void* StateRegistryKey() { static int Key; return &Key; }

        WorldScriptState* GetScriptState(lua_State* InState)
        {
            lua_pushlightuserdata(InState, StateRegistryKey());
            lua_gettable(InState, LUA_REGISTRYINDEX);
            WorldScriptState* Result = static_cast<WorldScriptState*>(lua_touserdata(InState, -1));
            lua_pop(InState, 1);
            return Result;
        }

        void SetScriptState(lua_State* InState, WorldScriptState* InState_)
        {
            lua_pushlightuserdata(InState, StateRegistryKey());
            lua_pushlightuserdata(InState, InState_);
            lua_settable(InState, LUA_REGISTRYINDEX);
        }

        /* Reads a script class table by name from the registry. Pushes it (or
         * nil) onto the stack and returns whether it was found -- callers pop
         * it themselves once done, so this never leaves the stack unbalanced
         * relative to what the caller expects. */
        bool PushScriptClass(lua_State* InState, const char* InClassName)
        {
            lua_getfield(InState, LUA_REGISTRYINDEX, CLASS_REGISTRY_KEY);
            lua_getfield(InState, -1, InClassName);
            lua_remove(InState, -2);   // drop the outer class-registry table, keep only the result
            const bool bFound = !lua_isnil(InState, -1);
            return bFound;
        }

        /* Reads ClassTable.TickPhase (an integer, defaulting to TP_PrePhysics)
         * and ClassTable.CullOutsideActiveRegion (a boolean, defaulting to
         * false) with ClassTable at the top of the stack. Leaves the stack
         * exactly as it found it. */
        void ReadClassFlags(lua_State* InState, uint8_t& OutPhase, uint8_t& OutCull)
        {
            lua_getfield(InState, -1, "TickPhase");
            const lua_Integer PhaseValue = lua_isinteger(InState, -1) ? lua_tointeger(InState, -1) : TP_PrePhysics;
            OutPhase = uint8_t((PhaseValue >= 0 && PhaseValue < TP_MAX) ? PhaseValue : TP_PrePhysics);
            lua_pop(InState, 1);

            lua_getfield(InState, -1, "CullOutsideActiveRegion");
            OutCull = lua_toboolean(InState, -1) ? 1 : 0;
            lua_pop(InState, 1);
        }

        int Script_Register(lua_State* InState)
        {
            const char* ClassName = luaL_checkstring(InState, 1);
            luaL_checktype(InState, 2, LUA_TTABLE);

            lua_getfield(InState, LUA_REGISTRYINDEX, CLASS_REGISTRY_KEY);
            lua_pushvalue(InState, 2);
            lua_setfield(InState, -2, ClassName);
            lua_pop(InState, 1);
            return 0;
        }

        const luaL_Reg GScriptFunctions[] = {
            { "Register", Script_Register },
            { nullptr, nullptr }
        };

        /*
         * Runs one lifecycle method (OnStart/OnTick/OnDestroy) on a script
         * instance, under protected call, tagged with the class and method
         * name for the error log. Sets ScriptComponent::bErrored on failure so
         * a broken script's OnTick is not retried every frame -- the "log one
         * error and stay alive" contract World.h documents for the scripting
         * cases.
         *
         * InDeltaSeconds is null for OnStart/OnDestroy (no argument beyond
         * self) and points at the frame delta for OnTick -- a fixed optional
         * argument rather than a generic "N values the caller already pushed"
         * count, because getting the FINAL ARGUMENT ORDER right by rotating an
         * arbitrary number of already-pushed values with lua_insert is exactly
         * the kind of stack arithmetic that is easy to get quietly backwards
         * (self and the extra argument trading places is invisible until a
         * script reads dt where it expected itself). Every value this function
         * pushes, it pushes in the order Lua should see it, and nothing is
         * ever inserted into the middle of the stack.
         */
        void CallLifecycleMethod(lua_State* InState, ScriptComponent& Script, const char* InMethodName,
                                  const float* InDeltaSeconds)
        {
            if(Script.bErrored) { return; }

            lua_rawgeti(InState, LUA_REGISTRYINDEX, Script.InstanceRef);   // the instance table (self)
            lua_getfield(InState, -1, InMethodName);                       // self[InMethodName]
            if(lua_isnil(InState, -1))
            {
                lua_pop(InState, 2);   // method (nil), self
                return;
            }

            /* function is now on top, self one below it -- swap them so the
             * call layout is [function, self, ...args], which is the ONLY
             * ordering lua_pcall below is ever asked to interpret. */
            lua_insert(InState, -2);

            int ArgCount = 1;   // self
            if(InDeltaSeconds)
            {
                lua_pushnumber(InState, *InDeltaSeconds);
                ++ArgCount;
            }

            const std::string Context = std::string(Script.ClassName) + "." + InMethodName;
            if(!LuaCallProtected(InState, ArgCount, 0, Context.c_str()))
            {
                Script.bErrored = 1;
            }
        }

        /*
         * The system every phase's trampoline shares: walk the ScriptComponent
         * pool ONCE (base pointer re-read every iteration -- the buffer may
         * have grown if OnStart/OnTick added a script to a NEW entity this
         * same pass, which World.h's Each<T> contract already forbids for
         * mutating T's OWN pool but does not forbid for a DIFFERENT entity's
         * ScriptComponent; re-reading Base defends the case anyway since it
         * costs nothing), skip anything not in InPhase, skip a culled entity
         * outside the active region, run OnStart once then OnTick every call.
         */
        void RunScriptPhase(World& InWorld, const float InDeltaSeconds, const ETickPhase InPhase, lua_State* InState)
        {
            ComponentPool* Pool = InWorld.FindPool(ScriptComponent::StaticType().ID);
            if(Pool)
            {
                const size_t Count = Pool->GetCount();
                for(size_t Index = 0; Index < Count; ++Index)
                {
                    ScriptComponent* Base = static_cast<ScriptComponent*>(Pool->GetDenseData());
                    ScriptComponent& Script = Base[Index];
                    if(Script.Phase != InPhase) { continue; }

                    if(Script.bCullOutsideActiveRegion && InWorld.HasActiveRegion())
                    {
                        const BoundsComponent* Bounds = Script.Owner.GetSibling<BoundsComponent>();
                        if(Bounds && !Bounds->World.Overlaps(InWorld.GetActiveRegion()))
                        {
                            continue;
                        }
                    }

                    if(!Script.bStarted)
                    {
                        Script.bStarted = 1;
                        CallLifecycleMethod(InState, Script, "OnStart", nullptr);
                    }

                    CallLifecycleMethod(InState, Script, "OnTick", &InDeltaSeconds);
                }
            }

            /* Lua-defined systems (World.AddSystem) for this phase, run after
             * every script component's own tick -- registration order between
             * "a script's OnTick" and "a Lua system" is therefore fixed
             * (scripts first), which is simple to state and matches what a
             * script author intuitively expects ("my object's own behaviour
             * runs, then whatever else is watching the world"). */
            if(WorldScriptState* State = GetScriptState(InState))
            {
                for(const LuaSystemEntry& Entry : State->LuaSystems[InPhase])
                {
                    lua_rawgeti(InState, LUA_REGISTRYINDEX, Entry.FunctionRef);
                    lua_pushnumber(InState, InDeltaSeconds);
                    LuaCallProtected(InState, 1, 0, Entry.Name.c_str());
                }
            }
        }

        /* One trampoline per phase, each a plain SystemFn (function pointer +
         * void* = the lua_State*) registered with World -- see World.h's
         * "function pointer + void*, not a virtual interface" rule. */
        void ScriptPhase_PrePhysics(World& InWorld, const float InDelta, void* InUserData)  { RunScriptPhase(InWorld, InDelta, TP_PrePhysics,  static_cast<lua_State*>(InUserData)); }
        void ScriptPhase_Physics(World& InWorld, const float InDelta, void* InUserData)     { RunScriptPhase(InWorld, InDelta, TP_Physics,     static_cast<lua_State*>(InUserData)); }
        void ScriptPhase_PostPhysics(World& InWorld, const float InDelta, void* InUserData) { RunScriptPhase(InWorld, InDelta, TP_PostPhysics, static_cast<lua_State*>(InUserData)); }
        void ScriptPhase_PostUpdate(World& InWorld, const float InDelta, void* InUserData)  { RunScriptPhase(InWorld, InDelta, TP_PostUpdate,  static_cast<lua_State*>(InUserData)); }

        /* ComponentPool's removal observer for ScriptComponent -- runs OnDestroy
         * under protected call, then releases the instance's registry ref.
         * Fires from inside ComponentPool::Remove, which after World's own
         * deferral only ever happens at the end of a phase or outside a tick --
         * never from inside a system's own iteration of a DIFFERENT pool, so
         * this is free to call back into Lua without re-entering the very
         * walk that triggered it. */
        void ScriptComponent_OnRemoved(EntityID /*InEntity*/, void* InComponent, void* InUserData)
        {
            lua_State* InState = static_cast<lua_State*>(InUserData);
            ScriptComponent& Script = *static_cast<ScriptComponent*>(InComponent);

            if(Script.InstanceRef != LUA_NOREF && Script.InstanceRef != LUA_REFNIL)
            {
                CallLifecycleMethod(InState, Script, "OnDestroy", nullptr);
                luaL_unref(InState, LUA_REGISTRYINDEX, Script.InstanceRef);
                Script.InstanceRef = LUA_NOREF;
            }
        }
    }

    bool ScriptSystem_InstantiateScript(lua_State* InState, World* InWorld, const EntityID InEntity, const char* InClassName)
    {
        if(InWorld->HasComponentRaw(InEntity, ScriptComponent::StaticType().ID))
        {
            GAMECORE_LOG_WARN(std::string("InstantiateScript: entity already has a ScriptComponent (class \"")
                               + InClassName + "\" not added)")
            return false;
        }

        LuaStackGuard Guard(InState);
        if(!PushScriptClass(InState, InClassName))
        {
            GAMECORE_LOG_ERROR(std::string("InstantiateScript: no class registered as \"") + InClassName + "\"")
            return false;
        }
        const int ClassIndex = lua_gettop(InState);

        uint8_t Phase = TP_PrePhysics;
        uint8_t Cull  = 0;
        ReadClassFlags(InState, Phase, Cull);

        /* The instance: a fresh table whose __index metatable is the class
         * table -- unset methods and any class-level defaults fall through to
         * it, exactly Lua's usual OOP-by-metatable idiom. */
        lua_newtable(InState);
        const int InstanceIndex = lua_gettop(InState);

        lua_newtable(InState);                       // the instance's own metatable
        lua_pushvalue(InState, ClassIndex);
        lua_setfield(InState, -2, "__index");
        lua_setmetatable(InState, InstanceIndex);

        LuaPushEntity(InState, InEntity);
        lua_setfield(InState, InstanceIndex, "Entity");

        lua_pushvalue(InState, InstanceIndex);
        const int InstanceRef = luaL_ref(InState, LUA_REGISTRYINDEX);

        ScriptComponent Init;
        Init.InstanceRef = InstanceRef;
        std::strncpy(Init.ClassName, InClassName, sizeof(Init.ClassName) - 1);
        Init.ClassName[sizeof(Init.ClassName) - 1] = '\0';
        Init.Phase                    = Phase;
        Init.bCullOutsideActiveRegion = Cull;

        InWorld->AddComponentRaw(InEntity, ScriptComponent::StaticType().ID, &Init);
        return true;
    }

    void ScriptSystem_AddLuaSystem(lua_State* InState, World* InWorld, const char* InName, const ETickPhase InPhase, const int InFunctionRef)
    {
        WorldScriptState* State = GetScriptState(InState);
        if(!State) { luaL_unref(InState, LUA_REGISTRYINDEX, InFunctionRef); return; }

        LuaSystemEntry Entry;
        Entry.Name        = InName;
        Entry.FunctionRef = InFunctionRef;
        State->LuaSystems[InPhase].push_back(Entry);
        (void)InWorld;   // the phase trampolines already read World's ScriptState by lua_State, not by World*
    }

    bool ScriptSystem_RemoveLuaSystem(lua_State* InState, World* /*InWorld*/, const char* InName)
    {
        WorldScriptState* State = GetScriptState(InState);
        if(!State) { return false; }

        for(auto& Bucket : State->LuaSystems)
        {
            for(size_t Index = 0; Index < Bucket.size(); ++Index)
            {
                if(Bucket[Index].Name == InName)
                {
                    luaL_unref(InState, LUA_REGISTRYINDEX, Bucket[Index].FunctionRef);
                    Bucket.erase(Bucket.begin() + long(Index));
                    return true;
                }
            }
        }
        return false;
    }

    void ScriptSystem_Install(lua_State* InState, World* InWorld)
    {
        lua_newtable(InState);
        luaL_setfuncs(InState, GScriptFunctions, 0);
        lua_setglobal(InState, "Script");

        /* The class registry: a fresh table each BindWorld, so a second bind
         * (a test fixture binding a new World) starts with no classes left
         * over from the previous one. */
        lua_newtable(InState);
        lua_setfield(InState, LUA_REGISTRYINDEX, CLASS_REGISTRY_KEY);

        WorldScriptState* State = new WorldScriptState();
        State->Owner = InWorld;
        SetScriptState(InState, State);

        InWorld->RegisterSystem("Script.PrePhysics",  TP_PrePhysics,  &ScriptPhase_PrePhysics,  InState);
        InWorld->RegisterSystem("Script.Physics",     TP_Physics,     &ScriptPhase_Physics,     InState);
        InWorld->RegisterSystem("Script.PostPhysics", TP_PostPhysics, &ScriptPhase_PostPhysics, InState);
        InWorld->RegisterSystem("Script.PostUpdate",  TP_PostUpdate,  &ScriptPhase_PostUpdate,  InState);

        ComponentPool* Pool = InWorld->GetPool(ScriptComponent::StaticType().ID);
        if(Pool)
        {
            Pool->SetRemovedObserver(&ScriptComponent_OnRemoved, InState);
        }
    }

    void ScriptSystem_Uninstall(lua_State* InState, World* InWorld)
    {
        InWorld->UnregisterAllSystems(InState);

        if(ComponentPool* Pool = InWorld->FindPool(ScriptComponent::StaticType().ID))
        {
            Pool->SetRemovedObserver(nullptr, nullptr);

            /* Every still-live script instance gets OnDestroy + unref, exactly
             * as if its entity had been destroyed -- ScriptEngine.h's contract
             * that UnbindWorld "runs OnDestroy and luaL_unref's every still-
             * live script instance". Walked back-to-front is not required
             * (this does not remove from the pool, only clears each instance's
             * own state), so forward is fine. */
            ScriptComponent* Base = static_cast<ScriptComponent*>(Pool->GetDenseData());
            const size_t Count = Pool->GetCount();
            for(size_t Index = 0; Index < Count; ++Index)
            {
                ScriptComponent& Script = Base[Index];
                if(Script.InstanceRef != LUA_NOREF && Script.InstanceRef != LUA_REFNIL)
                {
                    CallLifecycleMethod(InState, Script, "OnDestroy", nullptr);
                    luaL_unref(InState, LUA_REGISTRYINDEX, Script.InstanceRef);
                    Script.InstanceRef = LUA_NOREF;
                }
            }
        }

        if(WorldScriptState* State = GetScriptState(InState))
        {
            for(auto& Bucket : State->LuaSystems)
            {
                for(const LuaSystemEntry& Entry : Bucket)
                {
                    luaL_unref(InState, LUA_REGISTRYINDEX, Entry.FunctionRef);
                }
            }
            delete State;
            SetScriptState(InState, nullptr);
        }

        (void)InWorld;
    }
}

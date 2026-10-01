#include "Private/GameCore/GameCoreInternal.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/World.h"

/*
 * The World's ScriptDriver: class loading (cached by path), instantiation,
 * the per-phase tick ahead of every system, Lua systems (World.AddSystem),
 * and release (OnDestroy). See DESIGN.md.
 */

namespace GameCore
{
    void ScriptSystem_AddLuaSystem(lua_State* InState, World* InWorld, const char* InName, ETickPhase InPhase, int InFunctionRef);
    bool ScriptSystem_RemoveLuaSystem(lua_State* InState, World* InWorld, const char* InName);

    namespace
    {
        /* path -> class table, or false if it failed (reported once per bind).
         * Rebuilt on every BindWorld, which picks up edits. */
        constexpr const char* CLASS_CACHE_KEY = "GameCore.ScriptClasses";

        struct LuaSystemEntry
        {
            std::string Name;
            int         FunctionRef = LUA_NOREF;
        };

        /* The bound World's Lua systems, under a light userdata registry key. */
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

        bool IsLiveRef(const int32_t InRef) { return InRef != LUA_NOREF && InRef != LUA_REFNIL; }

        /* Pushes the script's class table, loading it on first use. False
         * (nothing pushed, logged once) unless it returns a table. */
        bool PushScriptClass(lua_State* InState, const char* InPath)
        {
            lua_getfield(InState, LUA_REGISTRYINDEX, CLASS_CACHE_KEY);   // cache
            lua_getfield(InState, -1, InPath);                           // cache, entry
            if(lua_istable(InState, -1))
            {
                lua_remove(InState, -2);
                return true;
            }
            const bool bKnownBad = !lua_isnil(InState, -1);
            lua_pop(InState, 1);                                         // cache

            if(bKnownBad)
            {
                lua_pop(InState, 1);
                return false;
            }

            /* Everything below leaves exactly one value (the class, or false)
             * above the cache before storing it. */
            bool bLoaded = false;
            std::string Source;
            if(!LuaReadScriptFile(InPath, Source))
            {
                GAMECORE_LOG_ERROR(std::string("Script: could not read \"") + InPath + "\"")
                LuaCountError(InState);
            }
            else
            {
                const std::string ChunkName = std::string("@") + InPath;
                if(luaL_loadbuffer(InState, Source.data(), Source.size(), ChunkName.c_str()) != LUA_OK)
                {
                    const char* ErrorText = lua_tostring(InState, -1);
                    GAMECORE_LOG_ERROR(std::string("Lua load error (") + InPath + "): "
                                       + (ErrorText ? ErrorText : "<no message>"))
                    lua_pop(InState, 1);
                    LuaCountError(InState);
                }
                else if(LuaCallProtected(InState, 0, 1, InPath))   // counts its own error
                {
                    if(lua_istable(InState, -1))
                    {
                        bLoaded = true;
                    }
                    else
                    {
                        GAMECORE_LOG_ERROR(std::string("Script: \"") + InPath
                                           + "\" must return its class table (got "
                                           + luaL_typename(InState, -1) + ")")
                        lua_pop(InState, 1);
                        LuaCountError(InState);
                    }
                }
            }

            if(!bLoaded)
            {
                lua_pushboolean(InState, 0);
            }
            lua_pushvalue(InState, -1);
            lua_setfield(InState, -3, InPath);   // cache[path] = class or false
            lua_remove(InState, -2);             // drop the cache

            if(!bLoaded)
            {
                lua_pop(InState, 1);
            }
            return bLoaded;
        }

        /* Reads TickPhase and CullOutsideActiveRegion (if present) from the
         * class table at the stack top. Stack-neutral. */
        void ReadClassFlags(lua_State* InState, uint8_t& OutPhase, uint8_t& InOutCull)
        {
            lua_getfield(InState, -1, "TickPhase");
            const lua_Integer PhaseValue = lua_isinteger(InState, -1) ? lua_tointeger(InState, -1) : TP_PrePhysics;
            OutPhase = uint8_t((PhaseValue >= 0 && PhaseValue < TP_MAX) ? PhaseValue : TP_PrePhysics);
            lua_pop(InState, 1);

            lua_getfield(InState, -1, "CullOutsideActiveRegion");
            if(!lua_isnil(InState, -1))
            {
                InOutCull = lua_toboolean(InState, -1) ? 1 : 0;
            }
            lua_pop(InState, 1);
        }

        /* Protected call of one method on instance InRef. True if clean or
         * absent. InDeltaSeconds is non-null only for OnTick (DESIGN.md). */
        bool CallMethod(lua_State* InState, const int InRef, const std::string& InPath,
                        const char* InMethodName, const float* InDeltaSeconds)
        {
            lua_rawgeti(InState, LUA_REGISTRYINDEX, InRef);   // the instance table (self)
            lua_getfield(InState, -1, InMethodName);           // self[InMethodName]
            if(lua_isnil(InState, -1))
            {
                lua_pop(InState, 2);   // method (nil), self
                return true;
            }

            /* Swap so the call layout is [function, self], the only
             * ordering lua_pcall below interprets. */
            lua_insert(InState, -2);

            int ArgCount = 1;   // self
            if(InDeltaSeconds)
            {
                lua_pushnumber(InState, *InDeltaSeconds);
                ++ArgCount;
            }

            const std::string Context = InPath + "." + InMethodName;
            return LuaCallProtected(InState, ArgCount, 0, Context.c_str());
        }

        /* Sets bErrored on failure so a broken OnTick isn't retried. The state
         * is re-fetched after the call. */
        void CallLifecycleMethod(lua_State* InState, World& InWorld, const EntityID InEntity,
                                 const char* InMethodName, const float* InDeltaSeconds)
        {
            const Entity Handle(&InWorld, InEntity);
            const EntityScriptState* Script = Handle.GetScriptState();
            if(!Script || Script->bErrored || !IsLiveRef(Script->InstanceRef)) { return; }

            const int Ref = Script->InstanceRef;
            if(!CallMethod(InState, Ref, Handle.GetScriptPath(), InMethodName, InDeltaSeconds))
            {
                EntityScriptState* After = Handle.GetScriptState();
                if(After && After->InstanceRef == Ref)
                {
                    After->bErrored = 1;
                }
            }
        }

        /* Builds the entity's instance. A failure marks it errored (reported
         * once, never ticked). */
        void InstantiateScript(lua_State* InState, World& InWorld, const EntityID InEntity)
        {
            const Entity Handle(&InWorld, InEntity);
            EntityScriptState* Script = Handle.GetScriptState();
            if(!Script) { return; }
            Script->bPendingInstantiate = 0;

            const std::string Path = Handle.GetScriptPath();

            LuaStackGuard Guard(InState);
            const bool bHaveClass = PushScriptClass(InState, Path.c_str());

            /* A first load ran the file's top level, which may have destroyed
             * this entity or queued a new path for it. */
            Script = Handle.GetScriptState();
            if(!Script || Path != Handle.GetScriptPath()) { return; }

            if(!bHaveClass)
            {
                Script->bErrored = 1;
                return;
            }
            const int ClassIndex = lua_gettop(InState);

            ReadClassFlags(InState, Script->Phase, Script->bCullOutsideActiveRegion);

            /* A fresh table with the class as its __index. */
            lua_newtable(InState);
            const int InstanceIndex = lua_gettop(InState);

            lua_newtable(InState);                       // the instance's own metatable
            lua_pushvalue(InState, ClassIndex);
            lua_setfield(InState, -2, "__index");
            lua_setmetatable(InState, InstanceIndex);

            LuaPushEntity(InState, InEntity);
            lua_setfield(InState, InstanceIndex, "Entity");

            lua_pushvalue(InState, InstanceIndex);
            Script->InstanceRef = luaL_ref(InState, LUA_REGISTRYINDEX);
        }

        /* ScriptDriver::TickPhase: per scripted entity, instantiate if
         * pending, skip if another phase or culled, then OnStart once and
         * OnTick. The list is stable while ticking. */
        void ScriptDriver_TickPhase(World& InWorld, const ETickPhase InPhase, const float InDeltaSeconds, void* InUserData)
        {
            lua_State* InState = static_cast<lua_State*>(InUserData);

            const size_t Count = InWorld.GetScriptedEntityCount();
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const EntityID EntityId = InWorld.GetScriptedEntityAt(Index);
                const Entity Handle(&InWorld, EntityId);

                EntityScriptState* Script = Handle.GetScriptState();
                if(!Script) { continue; }

                if(Script->bPendingInstantiate)
                {
                    InstantiateScript(InState, InWorld, EntityId);
                    Script = Handle.GetScriptState();
                    if(!Script) { continue; }
                }

                if(Script->bErrored || Script->Phase != InPhase || !IsLiveRef(Script->InstanceRef)) { continue; }

                if(Script->bCullOutsideActiveRegion && InWorld.HasActiveRegion())
                {
                    const BoundsComponent* Bounds = Handle.GetComponent<BoundsComponent>();
                    if(Bounds && !Bounds->World.Overlaps(InWorld.GetActiveRegion()))
                    {
                        continue;
                    }
                }

                if(!Script->bStarted)
                {
                    Script->bStarted = 1;
                    CallLifecycleMethod(InState, InWorld, EntityId, "OnStart", nullptr);
                }

                CallLifecycleMethod(InState, InWorld, EntityId, "OnTick", &InDeltaSeconds);
            }

            /* Lua systems: after entity scripts, before C++ systems. */
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

        /* ScriptDriver::Release: OnDestroy, then unref. The ref is cleared
         * first so re-entrant releases are no-ops. Errored scripts are skipped. */
        void ScriptDriver_Release(World& InWorld, const EntityID InEntity, void* InUserData)
        {
            lua_State* InState = static_cast<lua_State*>(InUserData);
            const Entity Handle(&InWorld, InEntity);

            EntityScriptState* Script = Handle.GetScriptState();
            if(!Script || !IsLiveRef(Script->InstanceRef)) { return; }

            const int  Ref       = Script->InstanceRef;
            const bool bErrored  = Script->bErrored != 0;
            Script->InstanceRef  = LUA_NOREF;

            if(!bErrored)
            {
                CallMethod(InState, Ref, Handle.GetScriptPath(), "OnDestroy", nullptr);
            }
            luaL_unref(InState, LUA_REGISTRYINDEX, Ref);
        }
    }

    void ScriptSystem_AddLuaSystem(lua_State* InState, World* InWorld, const char* InName, const ETickPhase InPhase, const int InFunctionRef)
    {
        WorldScriptState* State = GetScriptState(InState);
        if(!State) { luaL_unref(InState, LUA_REGISTRYINDEX, InFunctionRef); return; }

        LuaSystemEntry Entry;
        Entry.Name        = InName;
        Entry.FunctionRef = InFunctionRef;
        State->LuaSystems[InPhase].push_back(Entry);
        (void)InWorld;   // the driver reads World's ScriptState by lua_State, not by World*
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
        /* A fresh class cache per bind, so edits are picked up. */
        lua_newtable(InState);
        lua_setfield(InState, LUA_REGISTRYINDEX, CLASS_CACHE_KEY);

        WorldScriptState* State = new WorldScriptState();
        State->Owner = InWorld;
        SetScriptState(InState, State);

        ScriptDriver Driver;
        Driver.TickPhase = &ScriptDriver_TickPhase;
        Driver.Release   = &ScriptDriver_Release;
        Driver.UserData  = InState;
        InWorld->SetScriptDriver(&Driver);
    }

    void ScriptSystem_Uninstall(lua_State* InState, World* InWorld)
    {
        /* OnDestroy + unref each instance, keeping its path as pending.
         * Collected first: OnDestroy may reorder the list. */
        std::vector<EntityID> Scripted;
        const size_t Count = InWorld->GetScriptedEntityCount();
        Scripted.reserve(Count);
        for(size_t Index = 0; Index < Count; ++Index)
        {
            Scripted.push_back(InWorld->GetScriptedEntityAt(Index));
        }

        for(const EntityID EntityId : Scripted)
        {
            ScriptDriver_Release(*InWorld, EntityId, InState);

            if(EntityScriptState* Script = Entity(InWorld, EntityId).GetScriptState())
            {
                const uint8_t Cull = Script->bCullOutsideActiveRegion;
                *Script = EntityScriptState();
                Script->bCullOutsideActiveRegion = Cull;
                Script->bPendingInstantiate      = 1;
            }
        }

        InWorld->SetScriptDriver(nullptr);

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

        lua_pushnil(InState);
        lua_setfield(InState, LUA_REGISTRYINDEX, CLASS_CACHE_KEY);
    }
}

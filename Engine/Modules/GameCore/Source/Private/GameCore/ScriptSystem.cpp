#include "Private/GameCore/GameCoreInternal.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/TickPhase.h"
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

        /* Reads TickPhase (if present) from the class table at the stack
         * top: the phase OnTick stands for. Stack-neutral. */
        void ReadClassTickPhase(lua_State* InState, uint8_t& OutPhase)
        {
            lua_getfield(InState, -1, "TickPhase");
            const lua_Integer PhaseValue = lua_isinteger(InState, -1) ? lua_tointeger(InState, -1) : TP_PrePhysics;
            OutPhase = uint8_t((PhaseValue >= 0 && PhaseValue < TP_MAX) ? PhaseValue : TP_PrePhysics);
            lua_pop(InState, 1);
        }

        /* Protected call of one method on instance InRef. True if clean or
         * absent. InDeltaSeconds is non-null only for a tick (DESIGN.md);
         * the entity arguments follow it, in order. */
        bool CallMethod(lua_State* InState, const int InRef, const std::string& InPath,
                        const char* InMethodName, const float* InDeltaSeconds, const EntityID* InEntityArg = nullptr,
                        const EntityID* InSecondEntityArg = nullptr)
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
            if(InEntityArg)
            {
                LuaPushEntity(InState, *InEntityArg);
                ++ArgCount;
            }
            if(InSecondEntityArg)
            {
                LuaPushEntity(InState, *InSecondEntityArg);
                ++ArgCount;
            }

            const std::string Context = InPath + "." + InMethodName;
            return LuaCallProtected(InState, ArgCount, 0, Context.c_str());
        }

        // === Prefab instance scripts ==========================================

        /* "OnPrePhysics" .. "OnPostUpdate". */
        std::string PhaseFunctionName(const ETickPhase InPhase)
        {
            return std::string("On") + TickPhaseName(InPhase);
        }

        /* Marks an instance's script broken (reported once, never called
         * again) if Ref is still its instance -- a raise from a replaced
         * instance must not mark its successor. */
        void MarkInstanceErrored(World& InWorld, const PrefabInstanceID InInstance, const int InRef)
        {
            InstanceScriptState* Script = InWorld.GetInstanceScriptState(InInstance);
            if(Script && Script->InstanceRef == InRef) { Script->bErrored = 1; }
        }

        /*
         * Builds InInstance's script object and starts it: the class (cached by
         * path), the phases it defines, then OnStart.
         *
         * self.Entity and self.Root are the root entity, and self.Instance is
         * the PrefabInstance. Runs at the start of a phase, never during a phase's
         * walk, so World may re-file the instance freely.
         */
        /* InInstance's native behaviour, cached in its state until the
         * registry changes (a module loading or unloading). */
        const NativePrefabBehavior* NativeFor(World& InWorld, const PrefabInstanceID InInstance, InstanceScriptState& InOutScript)
        {
            const char* Name = InWorld.GetInstanceNativeBehavior(InInstance);
            if(Name[0] == '\0') { return nullptr; }
            const uint32_t Generation = GetNativePrefabGeneration();
            if(InOutScript.NativeGeneration != Generation)
            {
                InOutScript.Native           = FindNativePrefabBehavior(Name);
                InOutScript.NativeGeneration = Generation;
            }
            return InOutScript.Native;
        }

        void InstantiateInstanceScript(lua_State* InState, World& InWorld, const PrefabInstanceID InInstance)
        {
            const PrefabInstance Instance(&InWorld, InInstance);
            const std::string Path = InWorld.GetInstanceScriptPath(InInstance);

            LuaStackGuard Guard(InState);
            const bool bHaveClass = !Path.empty() && PushScriptClass(InState, Path.c_str());

            /* A first load ran the file's top level, which may have ended it. */
            InstanceScriptState* Script = InWorld.GetInstanceScriptState(InInstance);
            if(!Script || Script->bPendingInstantiate || Script->bStarted || IsLiveRef(Script->InstanceRef)) { return; }
            if(!Path.empty() && !bHaveClass)
            {
                Script->bErrored = 1;
                return;
            }

            /* The phases: an On<Phase> function each, and OnTick for its
             * TickPhase unless that phase has its own function. */
            uint8_t LuaMask = 0;
            int     Ref     = LUA_NOREF;
            Script->OnTickPhase = TP_MAX;
            if(bHaveClass)
            {
                const int ClassIndex = lua_gettop(InState);
                for(int Phase = 0; Phase < TP_MAX; ++Phase)
                {
                    lua_getfield(InState, ClassIndex, PhaseFunctionName(ETickPhase(Phase)).c_str());
                    if(lua_isfunction(InState, -1)) { LuaMask |= uint8_t(1u << Phase); }
                    lua_pop(InState, 1);
                }
                lua_getfield(InState, ClassIndex, "OnTick");
                const bool bHasOnTick = lua_isfunction(InState, -1) != 0;
                lua_pop(InState, 1);
                if(bHasOnTick)
                {
                    uint8_t TickPhase = TP_PrePhysics;
                    ReadClassTickPhase(InState, TickPhase);
                    if(!(LuaMask & (1u << TickPhase)))
                    {
                        Script->OnTickPhase = TickPhase;
                        LuaMask |= uint8_t(1u << TickPhase);
                    }
                }

                /* A fresh table with the class as its __index. */
                lua_newtable(InState);
                const int InstanceIndex = lua_gettop(InState);
                lua_newtable(InState);
                lua_pushvalue(InState, ClassIndex);
                lua_setfield(InState, -2, "__index");
                lua_setmetatable(InState, InstanceIndex);

                const EntityID Root = Instance.GetRoot().GetID();
                LuaPushEntity(InState, Root);
                lua_setfield(InState, InstanceIndex, "Entity");
                LuaPushEntity(InState, Root);
                lua_setfield(InState, InstanceIndex, "Root");
                LuaPushInstance(InState, InInstance);
                lua_setfield(InState, InstanceIndex, "Instance");

                lua_pushvalue(InState, InstanceIndex);
                Ref = luaL_ref(InState, LUA_REGISTRYINDEX);
                Script->InstanceRef = Ref;
            }

            /* Native callbacks: their phases join the script's. */
            const NativePrefabBehavior* Native = NativeFor(InWorld, InInstance, *Script);
            uint8_t NativeMask = 0;
            for(int Phase = 0; Native && Phase < TP_MAX; ++Phase)
            {
                if(Native->OnPhase[Phase]) { NativeMask |= uint8_t(1u << Phase); }
            }

            Script->LuaPhaseMask = LuaMask;
            Script->bStarted     = 1;
            InWorld.SetInstanceScriptPhaseMask(InInstance, uint8_t(LuaMask | NativeMask));

            /* OnStart: the callbacks first, then the script. */
            if(Native && Native->OnStart)
            {
                Native->OnStart(Instance, Native->UserData);
            }
            if(bHaveClass && IsLiveRef(Ref) && !CallMethod(InState, Ref, Path, "OnStart", nullptr))
            {
                MarkInstanceErrored(InWorld, InInstance, Ref);
                InWorld.SetInstanceScriptPhaseMask(InInstance, uint8_t(LuaMask | NativeMask));   // re-filed: errored ticks nowhere
            }
        }

        /* ScriptDriver::ReleaseInstance: OnDestroy -- the callbacks, then the
         * script -- then unref, the ref cleared first. */
        void ScriptDriver_ReleaseInstance(World& InWorld, const PrefabInstanceID InInstance, void* InUserData)
        {
            lua_State* InState = static_cast<lua_State*>(InUserData);
            InstanceScriptState* Script = InWorld.GetInstanceScriptState(InInstance);
            if(!Script || (!Script->bStarted && !IsLiveRef(Script->InstanceRef))) { return; }

            const int  Ref      = Script->InstanceRef;
            const bool bErrored = Script->bErrored != 0;
            Script->InstanceRef = LUA_NOREF;

            if(!bErrored)
            {
                if(const NativePrefabBehavior* Native = NativeFor(InWorld, InInstance, *Script); Native && Native->OnDestroy)
                {
                    Native->OnDestroy(PrefabInstance(&InWorld, InInstance), Native->UserData);
                }
                if(IsLiveRef(Ref))
                {
                    CallMethod(InState, Ref, InWorld.GetInstanceScriptPath(InInstance), "OnDestroy", nullptr);
                }
            }
            if(IsLiveRef(Ref))
            {
                luaL_unref(InState, LUA_REGISTRYINDEX, Ref);
            }
        }

        /* ScriptDriver::CallInstanceMethod: InMethod(self, InArg, InMine) --
         * the native overlap callback for an overlap, then the script. */
        bool ScriptDriver_CallInstanceMethod(World& InWorld, const PrefabInstanceID InInstance, const char* InMethod,
                                             const EntityID InArg, const EntityID InMine, void* InUserData)
        {
            lua_State* InState = static_cast<lua_State*>(InUserData);
            InstanceScriptState* Script = InWorld.GetInstanceScriptState(InInstance);
            if(!Script || !Script->bStarted || Script->bErrored) { return false; }
            const PrefabInstance Instance(&InWorld, InInstance);
            if(Instance.GetTickMode() == EPrefabTickMode::Off) { return false; }

            bool bRan = false;
            if(const NativePrefabBehavior* Native = NativeFor(InWorld, InInstance, *Script))
            {
                const auto Callback = std::strcmp(InMethod, "OnOverlapBegin") == 0 ? Native->OnOverlapBegin
                                    : std::strcmp(InMethod, "OnOverlapEnd") == 0   ? Native->OnOverlapEnd
                                                                                   : nullptr;
                if(Callback)
                {
                    Callback(Instance, Entity(&InWorld, InArg), Entity(&InWorld, InMine), Native->UserData);
                    bRan = true;
                }
            }

            const int Ref = Script->InstanceRef;
            if(!IsLiveRef(Ref)) { return bRan; }
            if(!CallMethod(InState, Ref, InWorld.GetInstanceScriptPath(InInstance), InMethod, nullptr, &InArg, &InMine))
            {
                MarkInstanceErrored(InWorld, InInstance, Ref);
                return bRan;
            }
            return true;
        }

        /* InPhase's instances: On<Phase>(self, dt), or OnTick where that
         * stands for this phase. The list cannot change during the walk
         * (World defers every change to the flush); an instance can still
         * be marked errored, so state is re-read each time. */
        void TickInstances(lua_State* InState, World& InWorld, const ETickPhase InPhase, const float InDeltaSeconds)
        {
            const std::string PhaseFunction = PhaseFunctionName(InPhase);
            const size_t Count = InWorld.GetTickingInstanceCount(InPhase);
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const PrefabInstanceID Instance = InWorld.GetTickingInstanceAt(InPhase, Index);
                InstanceScriptState* Script = InWorld.GetInstanceScriptState(Instance);
                if(!Script || Script->bErrored || !Script->bStarted) { continue; }

                /* The callback first, then the script. */
                if(const NativePrefabBehavior* Native = NativeFor(InWorld, Instance, *Script); Native && Native->OnPhase[InPhase])
                {
                    Native->OnPhase[InPhase](PrefabInstance(&InWorld, Instance), InDeltaSeconds, Native->UserData);
                    Script = InWorld.GetInstanceScriptState(Instance);
                    if(!Script || Script->bErrored) { continue; }
                }

                const int Ref = Script->InstanceRef;
                if(!(Script->LuaPhaseMask & (1u << InPhase)) || !IsLiveRef(Ref)) { continue; }
                const char* Method = Script->OnTickPhase == InPhase ? "OnTick" : PhaseFunction.c_str();
                if(!CallMethod(InState, Ref, InWorld.GetInstanceScriptPath(Instance), Method, &InDeltaSeconds))
                {
                    MarkInstanceErrored(InWorld, Instance, Ref);
                }
            }
        }

        /* ScriptDriver::TickPhase: start any instance spawned since the last
         * phase start, then run this phase's list -- the native callback,
         * then the Lua method. The list is stable while ticking. */
        void ScriptDriver_TickPhase(World& InWorld, const ETickPhase InPhase, const float InDeltaSeconds, void* InUserData)
        {
            lua_State* InState = static_cast<lua_State*>(InUserData);

            /* Started first, so an instance spawned last phase runs this one.
             * One spawned by an OnStart here is popped in this same loop. */
            PrefabInstanceID Pending;
            while(InWorld.PopPendingInstanceScript(Pending))
            {
                InstantiateInstanceScript(InState, InWorld, Pending);
            }

            TickInstances(InState, InWorld, InPhase, InDeltaSeconds);

            /* Lua systems: after every script, before C++ systems. */
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
        Driver.ReleaseInstance    = &ScriptDriver_ReleaseInstance;
        Driver.CallInstanceMethod = &ScriptDriver_CallInstanceMethod;
        Driver.UserData  = InState;
        InWorld->SetScriptDriver(&Driver);
    }

    void ScriptSystem_Uninstall(lua_State* InState, World* InWorld)
    {
        /* OnDestroy + unref each instance's script, then queued to start
         * afresh at the next bind. Collected first: OnDestroy may end some. */
        std::vector<PrefabInstanceID> Instances;
        InWorld->ForEachInstance([](const PrefabInstanceID InInstance, void* InUserData)
        {
            static_cast<std::vector<PrefabInstanceID>*>(InUserData)->push_back(InInstance);
        }, &Instances);
        for(const PrefabInstanceID Instance : Instances)
        {
            ScriptDriver_ReleaseInstance(*InWorld, Instance, InState);
            InWorld->RequeueInstanceScript(Instance);
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

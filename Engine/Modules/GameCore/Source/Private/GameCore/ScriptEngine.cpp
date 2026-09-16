#include "Public/GameCore/ScriptEngine.h"

#include <cstring>
#include <string>
#include <vector>

#include "Private/GameCore/GameCoreInternal.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/World.h"

/*
 * The forward declarations below are ScriptBindings.cpp's and ScriptSystem.cpp's
 * entry points into this file's Impl -- deliberately free functions taking a
 * lua_State* and a World*, not methods on ScriptEngine::Impl, so that neither
 * of those two .cpp files needs to see this file's Impl definition (which is
 * private to this translation unit) or #include GameCoreInternal.h's full
 * surface twice for no reason. Each is implemented in its own file; see the
 * comment at each call site below for what it does.
 */
namespace GameCore
{
    void ScriptBindings_Install(lua_State* InState, World* InWorld); // ScriptBindings.cpp
    void ScriptSystem_Install(lua_State* InState, World* InWorld);   // ScriptSystem.cpp
    void ScriptSystem_Uninstall(lua_State* InState, World* InWorld);
    bool ScriptSystem_InstantiateScript(lua_State* InState, World* InWorld, EntityID InEntity, const char* InClassName);
}

namespace GameCore
{
    namespace
    {
        /*
         * Routes Lua's own heap use through EngineCore::SystemAlignedAlloc/
         * Free rather than plain malloc/free -- so a leak inside the VM (a
         * runaway table, a script that never releases a reference) shows up in
         * the same accounting every other allocation in this engine does,
         * rather than being invisible to it.
         *
         * lua_Alloc IS realloc-shaped (osize/nsize, ptr may be null, nsize may
         * be zero for a free), but SystemAlignedAlloc/Free are not a matched
         * realloc pair -- there is no aligned-realloc exposed by
         * CoreMemoryPool.h, deliberately, because nothing else in this engine
         * needs one. So this allocates fresh, copies min(osize, nsize) bytes
         * forward, and frees the old block -- a real cost on every grow, which
         * for a scripting VM's typical allocation pattern (many small objects,
         * few large reallocations) is not where time goes. If that ever shows
         * in a profile, the fix is a dedicated pool registered through
         * RegisterMemoryPool, not a change to this function's contract.
         *
         * Alignment is alignof(std::max_align_t) -- Lua itself does not ask
         * for anything stricter, and this allocator has no type information to
         * do better with; it is a raw byte allocator exactly as malloc is.
         */
        void* LuaAllocator(void* /*ud*/, void* InPtr, const size_t InOldSize, const size_t InNewSize)
        {
            if(InNewSize == 0)
            {
                if(InPtr)
                {
                    EngineCore::SystemAlignedFree(InPtr);
                }
                return nullptr;
            }

            void* NewPtr = EngineCore::SystemAlignedAlloc(InNewSize, alignof(std::max_align_t));
            if(!NewPtr)
            {
                return nullptr;   // Lua treats a null return from grow as LUA_ERRMEM
            }

            if(InPtr)
            {
                std::memcpy(NewPtr, InPtr, InOldSize < InNewSize ? InOldSize : InNewSize);
                EngineCore::SystemAlignedFree(InPtr);
            }
            return NewPtr;
        }

        /* Lua's print() rebuilt over GAMECORE_LOG instead of stdout -- tab-
         * separated tostring() of each argument, matching lbaselib.c's own
         * luaB_print exactly except for the destination, so script output
         * reaches whatever LogHandler the host installed rather than a
         * console a shipped game may not have. */
        int LuaPrint(lua_State* InState)
        {
            const int ArgCount = lua_gettop(InState);
            std::string Line;

            for(int Index = 1; Index <= ArgCount; ++Index)
            {
                if(Index > 1) { Line += '\t'; }

                size_t      Length = 0;
                const char* Text = luaL_tolstring(InState, Index, &Length);   // may raise for a __tostring error
                Line.append(Text, Length);
                lua_pop(InState, 1);   // pop the string luaL_tolstring pushed
            }

            GAMECORE_LOG(std::string("[Lua] ") + Line)
            return 0;
        }
    }

    int LuaTracebackHandler(lua_State* InState)
    {
        const char* Message = lua_tostring(InState, 1);
        luaL_traceback(InState, InState, Message, 1);
        return 1;
    }

    namespace
    {
        /* Registry key for the per-state error counter -- the address of this
         * static is the key itself (a light userdata), the same "a unique
         * static's address IS the registry key" idiom ScriptBindings.cpp's
         * GetBoundWorld/SetBoundWorld and ScriptSystem.cpp's GetScriptState/
         * SetScriptState both use. The VALUE stored under that key is a plain
         * Lua number (lua_Integer), read and rewritten whole on every error --
         * one lua_gettable/lua_settable pair per error, never a hot path,
         * simpler than a light-userdata-boxed counter for no cost that matters
         * here. */
        int GErrorCountRegistryKey;

        uint32_t GetLuaErrorCount(lua_State* InState)
        {
            lua_pushlightuserdata(InState, &GErrorCountRegistryKey);
            lua_gettable(InState, LUA_REGISTRYINDEX);
            const uint32_t Count = lua_isnumber(InState, -1) ? uint32_t(lua_tointeger(InState, -1)) : 0u;
            lua_pop(InState, 1);
            return Count;
        }

        void IncrementErrorCount(lua_State* InState)
        {
            const uint32_t NewCount = GetLuaErrorCount(InState) + 1;
            lua_pushlightuserdata(InState, &GErrorCountRegistryKey);
            lua_pushinteger(InState, lua_Integer(NewCount));
            lua_settable(InState, LUA_REGISTRYINDEX);
        }
    }

    bool LuaCallProtected(lua_State* InState, const int InNArgs, const int InNResults, const char* InContext)
    {
        /* The message handler must sit BELOW the function and its arguments on
         * the stack -- pushed first, then the function is moved above it via
         * lua_insert, matching luaL_dostring's own idiom in lauxlib.c. */
        const int HandlerIndex = lua_gettop(InState) - InNArgs;
        lua_pushcfunction(InState, LuaTracebackHandler);
        lua_insert(InState, HandlerIndex);

        const int Result = lua_pcall(InState, InNArgs, InNResults, HandlerIndex);
        lua_remove(InState, HandlerIndex);   // pop the handler regardless of outcome

        if(Result != LUA_OK)
        {
            const char* ErrorText = lua_tostring(InState, -1);
            GAMECORE_LOG_ERROR(std::string("Lua error (") + (InContext ? InContext : "?") + "): "
                                + (ErrorText ? ErrorText : "<no message>"))
            lua_pop(InState, 1);
            /* THE ONE CHOKE POINT every runtime error passes through --
             * RunString/RunFile's own luaL_loadbuffer syntax-error path is the
             * only error kind that does NOT go through here (nothing has been
             * called yet at that point), so it increments separately below.
             * Before this, a lifecycle callback error (ScriptSystem.cpp's
             * CallLifecycleMethod, which calls this function directly) bumped
             * ONLY ScriptComponent::bErrored and never reached
             * ScriptEngine::GetErrorCount() at all -- caught by the smoke
             * test expecting exactly one counted error per broken script. */
            IncrementErrorCount(InState);
            return false;
        }
        return true;
    }

    struct ScriptEngine::Impl
    {
        lua_State* State           = nullptr;
        World*     BoundWorld      = nullptr;
        uint32_t   ErrorCount      = 0;
    };

    ScriptEngine::ScriptEngine() : P(new Impl()) {}

    ScriptEngine::~ScriptEngine()
    {
        if(P->State)
        {
            GAMECORE_LOG_WARN("ScriptEngine destroyed without calling Shutdown() first; closing now")
            Shutdown();
        }
        delete P;
    }

    bool ScriptEngine::Initialize()
    {
        if(P->State)
        {
            GAMECORE_LOG_WARN("ScriptEngine::Initialize called twice; ignoring")
            return true;
        }

        P->State = lua_newstate(&LuaAllocator, nullptr);
        if(!P->State)
        {
            GAMECORE_LOG_ERROR("lua_newstate failed (out of memory)")
            return false;
        }

        luaL_openlibs(P->State);

        lua_pushcfunction(P->State, &LuaPrint);
        lua_setglobal(P->State, "print");

        GAMECORE_LOG("Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR " initialised")
        return true;
    }

    void ScriptEngine::Shutdown()
    {
        if(!P->State)
        {
            return;
        }

        if(P->BoundWorld)
        {
            GAMECORE_LOG_WARN("ScriptEngine::Shutdown called with a World still bound; unbinding first")
            UnbindWorld();
        }

        lua_close(P->State);
        P->State = nullptr;
    }

    bool ScriptEngine::IsInitialized() const
    {
        return P->State != nullptr;
    }

    void ScriptEngine::BindWorld(World* InWorld)
    {
        if(!P->State)
        {
            GAMECORE_LOG_ERROR("BindWorld called before Initialize(); ignoring")
            return;
        }
        if(!InWorld)
        {
            GAMECORE_LOG_ERROR("BindWorld given a null World; ignoring")
            return;
        }
        if(P->BoundWorld)
        {
            GAMECORE_LOG_WARN("BindWorld called with a World already bound; unbinding it first")
            UnbindWorld();
        }

        ScriptBindings_Install(P->State, InWorld);   // World/Entity/component-proxy/TickPhase/Events libraries
        ScriptSystem_Install(P->State, InWorld);     // Script.Register, the per-phase tick, the removal observer

        P->BoundWorld = InWorld;
    }

    void ScriptEngine::UnbindWorld()
    {
        if(!P->BoundWorld)
        {
            return;
        }
        ScriptSystem_Uninstall(P->State, P->BoundWorld);
        P->BoundWorld = nullptr;
    }

    bool ScriptEngine::RunFile(const char* InPath)
    {
        if(!P->State)
        {
            GAMECORE_LOG_ERROR("RunFile called before Initialize(); ignoring")
            return false;
        }

        std::string Source;
        bool bRead = false;
        if(EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider())
        {
            bRead = Provider->ReadAllText(InPath, Source);
        }
        else
        {
            std::vector<uint8_t> Bytes;
            bRead = EngineCore::appFileReadAll(InPath, Bytes);
            if(bRead)
            {
                Source.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
            }
        }

        if(!bRead)
        {
            GAMECORE_LOG_ERROR(std::string("RunFile: could not read \"") + InPath + "\"")
            ++P->ErrorCount;
            return false;
        }

        return RunString(Source.c_str(), InPath);
    }

    bool ScriptEngine::RunString(const char* InSource, const char* InChunkName)
    {
        if(!P->State)
        {
            GAMECORE_LOG_ERROR("RunString called before Initialize(); ignoring")
            return false;
        }

        LuaStackGuard Guard(P->State);

        const std::string ChunkLabel = InChunkName ? (std::string("@") + InChunkName) : std::string("=(RunString)");
        if(luaL_loadbuffer(P->State, InSource, std::strlen(InSource), ChunkLabel.c_str()) != LUA_OK)
        {
            const char* ErrorText = lua_tostring(P->State, -1);
            GAMECORE_LOG_ERROR(std::string("Lua load error (") + (InChunkName ? InChunkName : "?") + "): "
                                + (ErrorText ? ErrorText : "<no message>"))
            ++P->ErrorCount;
            return false;
        }

        /* LuaCallProtected already counts this error itself (see its own
         * comment on IncrementErrorCount) -- no ++P->ErrorCount here, or a
         * single runtime error would be counted twice. */
        return LuaCallProtected(P->State, 0, 0, InChunkName ? InChunkName : "(RunString)");
    }

    bool ScriptEngine::InstantiateScript(const EntityID InEntity, const char* InClassName)
    {
        if(!P->State || !P->BoundWorld)
        {
            GAMECORE_LOG_ERROR("InstantiateScript called before Initialize()/BindWorld(); ignoring")
            return false;
        }

        /* Implemented in ScriptSystem.cpp, which owns the class registry and
         * the instance-table construction this shares with the Lua
         * e:AddScript() path. */
        return ScriptSystem_InstantiateScript(P->State, P->BoundWorld, InEntity, InClassName);
    }

    lua_State* ScriptEngine::GetState() const
    {
        return P->State;
    }

    uint32_t ScriptEngine::GetErrorCount() const
    {
        /* P->ErrorCount counts what never reaches LuaCallProtected (a file
         * read failure, a luaL_loadbuffer syntax error); the registry counter
         * counts every runtime error THROUGH LuaCallProtected, from ANY
         * caller -- RunString/RunFile's own top-level call, and every
         * lifecycle callback ScriptSystem.cpp drives (OnStart/OnTick/
         * OnDestroy). Summed here rather than merged into one counter at the
         * write side, so P->ErrorCount stays a plain member for the cases
         * that have no lua_State to stash a registry entry in yet (a read
         * failure can happen before Initialize() -- see RunFile's own check
         * above it -- though in practice IsInitialized() already refuses
         * that case first). */
        return P->ErrorCount + (P->State ? GetLuaErrorCount(P->State) : 0u);
    }
}

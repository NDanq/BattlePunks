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
 * ScriptBindings.cpp's and ScriptSystem.cpp's entry points into this file's
 * Impl -- free functions, not Impl methods, so neither file needs to see
 * Impl's definition (private to this TU).
 */
namespace GameCore
{
    void ScriptBindings_Install(lua_State* InState, World* InWorld); // ScriptBindings.cpp
    void ScriptSystem_Install(lua_State* InState, World* InWorld);   // ScriptSystem.cpp
    void ScriptSystem_Uninstall(lua_State* InState, World* InWorld);
}

namespace GameCore
{
    namespace
    {
        /* Routes Lua's own heap use through SystemAlignedAlloc/Free -- see
         * DESIGN.md for why this is not a matched realloc pair and what
         * that costs. */
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

        /* Lua's print() rebuilt over GAMECORE_LOG -- see DESIGN.md. */
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
        /* Registry key for the per-state error counter -- see DESIGN.md for
         * the "address is the key" idiom this shares with ScriptBindings.cpp
         * and ScriptSystem.cpp. */
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

    void LuaCountError(lua_State* InState)
    {
        IncrementErrorCount(InState);
    }

    bool LuaReadScriptFile(const char* InPath, std::string& OutSource)
    {
        if(EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider())
        {
            return Provider->ReadAllText(InPath, OutSource);
        }

        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(InPath, Bytes))
        {
            return false;
        }
        OutSource.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        return true;
    }

    bool LuaCallProtected(lua_State* InState, const int InNArgs, const int InNResults, const char* InContext)
    {
        /* The message handler sits BELOW the function and its arguments,
         * pushed first then moved above via lua_insert. */
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
            /* The one choke point every runtime error passes through --
             * see DESIGN.md for the bug this fixed. */
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
        ScriptSystem_Install(P->State, InWorld);     // the World's script driver

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
        if(!LuaReadScriptFile(InPath, Source))
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

        /* LuaCallProtected already counts a runtime error itself -- no
         * ++P->ErrorCount here, or it would be counted twice. */
        return LuaCallProtected(P->State, 0, 0, InChunkName ? InChunkName : "(RunString)");
    }

    lua_State* ScriptEngine::GetState() const
    {
        return P->State;
    }

    uint32_t ScriptEngine::GetErrorCount() const
    {
        /* Sums what never reaches LuaCallProtected with what does -- see
         * DESIGN.md. */
        return P->ErrorCount + (P->State ? GetLuaErrorCount(P->State) : 0u);
    }
}

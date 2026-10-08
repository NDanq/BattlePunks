#include "Public/GameCore/ScriptEngine.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "Private/GameCore/GameCoreInternal.h"
#include "Public/GameCore/GraphText.h"
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

    bool LuaReadScriptFile(const char* InPath, std::string& OutSource);

    /* A file through the provider, compiled: what the stock loadfile does by
     * fopen. Pushes the function, or nil and a message. */
    int LuaLoadProviderFile(lua_State* InState, const char* InPath, const char* InMode)
    {
        std::string Source;
        if(!LuaReadScriptFile(InPath, Source))
        {
            lua_pushnil(InState);
            lua_pushfstring(InState, "cannot open %s", InPath);
            return 2;
        }
        const std::string ChunkName = std::string("@") + InPath;
        if(luaL_loadbufferx(InState, Source.data(), Source.size(), ChunkName.c_str(), InMode) != LUA_OK)
        {
            lua_pushnil(InState);
            lua_insert(InState, -2);   // nil, message
            return 2;
        }
        return 1;
    }

    /* loadfile(path [, mode [, env]]) */
    int LuaLoadFile(lua_State* InState)
    {
        const char* Path = luaL_checkstring(InState, 1);
        const char* Mode = luaL_optstring(InState, 2, "bt");
        const bool  bEnv = !lua_isnone(InState, 3);
        if(LuaLoadProviderFile(InState, Path, Mode) != 1)
        {
            return 2;
        }
        if(bEnv)
        {
            lua_pushvalue(InState, 3);
            if(!lua_setupvalue(InState, -2, 1)) { lua_pop(InState, 1); }   // its _ENV
        }
        return 1;
    }

    /* dofile(path): errors as the stock one does. */
    int LuaDoFile(lua_State* InState)
    {
        const char* Path = luaL_checkstring(InState, 1);
        lua_settop(InState, 1);
        if(LuaLoadProviderFile(InState, Path, "bt") != 1)
        {
            return lua_error(InState);
        }
        lua_call(InState, 0, LUA_MULTRET);
        return lua_gettop(InState) - 1;
    }

    /* require's Lua searcher, through the provider: package.path's templates,
     * '?' the module name with '.' as '/'. */
    int LuaSearchProvider(lua_State* InState)
    {
        const char* Module = luaL_checkstring(InState, 1);
        std::string Name = Module;
        std::replace(Name.begin(), Name.end(), '.', '/');

        lua_getglobal(InState, "package");
        lua_getfield(InState, -1, "path");
        const std::string Templates = lua_isstring(InState, -1) ? lua_tostring(InState, -1) : "?.lua";
        lua_pop(InState, 2);

        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        std::string Tried;
        size_t Start = 0;
        while(Start <= Templates.size())
        {
            const size_t End = std::min(Templates.find(';', Start), Templates.size());
            std::string Candidate = Templates.substr(Start, End - Start);
            Start = End + 1;
            if(Candidate.empty()) { continue; }
            for(size_t At = Candidate.find('?'); At != std::string::npos; At = Candidate.find('?', At + Name.size()))
            {
                Candidate.replace(At, 1, Name);
            }
            const bool bThere = Provider ? Provider->Exists(Candidate) : EngineCore::appFileExists(Candidate);
            if(bThere)
            {
                if(LuaLoadProviderFile(InState, Candidate.c_str(), "bt") != 1)
                {
                    return luaL_error(InState, "error loading module '%s' from '%s':\n\t%s", Module, Candidate.c_str(),
                                      lua_tostring(InState, -1));
                }
                lua_pushstring(InState, Candidate.c_str());
                return 2;
            }
            Tried += "\n\tno file '" + Candidate + "'";
        }
        lua_pushstring(InState, Tried.c_str());
        return 1;
    }

    /* The stock libraries open files with fopen; a packaged game has its
     * scripts only in its content pack. dofile, loadfile and require's Lua
     * searcher read through the file provider instead. */
    void LuaReadThroughProvider(lua_State* InState)
    {
        lua_pushcfunction(InState, &LuaDoFile);
        lua_setglobal(InState, "dofile");
        lua_pushcfunction(InState, &LuaLoadFile);
        lua_setglobal(InState, "loadfile");

        lua_getglobal(InState, "package");
        lua_getfield(InState, -1, "searchers");
        if(lua_istable(InState, -1))
        {
            lua_pushcfunction(InState, &LuaSearchProvider);
            lua_rawseti(InState, -2, 2);   // [1] preload, [2] Lua files, [3..] C
        }
        lua_pop(InState, 2);
    }

    bool LuaReadScriptFile(const char* InPath, std::string& OutSource)
    {
        std::string Text;
        if(EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider())
        {
            if(!Provider->ReadAllText(InPath, Text))
            {
                return false;
            }
        }
        else
        {
            std::vector<uint8_t> Bytes;
            if(!EngineCore::appFileReadAll(InPath, Bytes))
            {
                return false;
            }
            Text.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        }

        /* A script graph runs the Lua it was saved with. */
        static const char GRAPH_EXTENSION[] = ".nmgraph";
        const size_t Length    = std::strlen(InPath);
        const size_t ExtLength = sizeof(GRAPH_EXTENSION) - 1;
        bool bGraph = Length > ExtLength;
        for(size_t Index = 0; bGraph && Index < ExtLength; ++Index)
        {
            const char Each = InPath[Length - ExtLength + Index];
            bGraph = (Each >= 'A' && Each <= 'Z' ? char(Each - 'A' + 'a') : Each) == GRAPH_EXTENSION[Index];
        }
        if(bGraph)
        {
            if(!ExtractGraphGeneratedCode(Text, ".lua", OutSource))
            {
                GAMECORE_LOG_ERROR(std::string("\"") + InPath + "\" has no generated Lua -- open it in the editor and save it")
                return false;
            }
            return true;
        }
        OutSource = std::move(Text);
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

    /* Pushes the script's class table, loading it on first use. False
     * (nothing pushed, logged once) unless it returns a table. */
    bool LuaPushScriptClass(lua_State* InState, const char* InCacheKey, const char* InPath)
    {
        lua_getfield(InState, LUA_REGISTRYINDEX, InCacheKey);        // cache
        if(!lua_istable(InState, -1))
        {
            lua_pop(InState, 1);
            lua_newtable(InState);
            lua_pushvalue(InState, -1);
            lua_setfield(InState, LUA_REGISTRYINDEX, InCacheKey);
        }
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
        LuaReadThroughProvider(P->State);

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

    namespace
    {
        /* Pushes the value at InPath (globals, then "." into tables); nil if
         * a link is missing. Plain C locals only: nothing here may raise. */
        void PushPath(lua_State* InState, const char* InPath)
        {
            lua_pushglobaltable(InState);
            const char* Start = InPath;
            while(true)
            {
                const char* Dot = std::strchr(Start, '.');
                const size_t Length = Dot ? size_t(Dot - Start) : std::strlen(Start);
                if(!lua_istable(InState, -1) || Length == 0)
                {
                    lua_pop(InState, 1);
                    lua_pushnil(InState);
                    return;
                }
                lua_pushlstring(InState, Start, Length);
                lua_rawget(InState, -2);
                lua_remove(InState, -2);
                if(!Dot)
                {
                    return;
                }
                Start = Dot + 1;
            }
        }
    }

    namespace
    {
        /* The registry table script objects' classes are cached in: apart
         * from the World's, which every BindWorld drops. */
        constexpr const char* OBJECT_CLASS_CACHE_KEY = "GameCore.ObjectClasses";

        void PushEventArg(lua_State* InState, const EventArg& InArg)
        {
            switch(InArg.Type)
            {
                case EventArg::ET_Number: lua_pushnumber(InState, InArg.Number); break;
                case EventArg::ET_Bool:   lua_pushboolean(InState, InArg.Bool); break;
                case EventArg::ET_String: lua_pushstring(InState, InArg.String); break;
                case EventArg::ET_Entity: LuaPushEntity(InState, EntityID::Unpack(InArg.Entity)); break;
                case EventArg::ET_None:   lua_pushnil(InState); break;
            }
        }

        /* InArgs onto the stack; how many. */
        int PushEventArgs(lua_State* InState, const EventArgs& InArgs)
        {
            const int Count = InArgs.Count < 4 ? int(InArgs.Count) : 4;
            for(int Index = 0; Index < Count; ++Index) { PushEventArg(InState, InArgs.Args[Index]); }
            return Count;
        }
    }

    bool ScriptEngine::CallFunction(const char* InPath, const EventArgs& InArgs)
    {
        if(!P->State || !InPath || !*InPath)
        {
            return false;
        }

        LuaStackGuard Guard(P->State);
        PushPath(P->State, InPath);
        if(!lua_isfunction(P->State, -1))
        {
            GAMECORE_LOG_ERROR(std::string("Lua call: '") + InPath + "' is not a function")
            ++P->ErrorCount;
            return false;
        }
        const int Count = PushEventArgs(P->State, InArgs);
        /* LuaCallProtected counts a runtime error itself. */
        return LuaCallProtected(P->State, Count, 0, InPath);
    }

    bool ScriptEngine::HasFunction(const char* InPath) const
    {
        if(!P->State || !InPath || !*InPath)
        {
            return false;
        }
        LuaStackGuard Guard(P->State);
        PushPath(P->State, InPath);
        return lua_isfunction(P->State, -1);
    }

    ScriptEngine::Object ScriptEngine::CreateObject(const char* InClassPath, const char* const* InFieldNames,
                                                   const EventArg* InFields, const int InFieldCount)
    {
        if(!P->State || !InClassPath || !*InClassPath)
        {
            return 0;
        }
        LuaStackGuard Guard(P->State);
        if(!LuaPushScriptClass(P->State, OBJECT_CLASS_CACHE_KEY, InClassPath))   // class
        {
            return 0;
        }
        lua_newtable(P->State);                                                  // class, self
        lua_newtable(P->State);                                                  // class, self, meta
        lua_pushvalue(P->State, -3);
        lua_setfield(P->State, -2, "__index");
        lua_setmetatable(P->State, -2);                                          // class, self
        for(int Index = 0; Index < InFieldCount && InFieldNames && InFields; ++Index)
        {
            PushEventArg(P->State, InFields[Index]);
            lua_setfield(P->State, -2, InFieldNames[Index]);
        }
        return Object(luaL_ref(P->State, LUA_REGISTRYINDEX));                   // pops self
    }

    bool ScriptEngine::HasObjectMethod(const Object InObject, const char* InMethod) const
    {
        if(!P->State || InObject <= 0 || !InMethod)
        {
            return false;
        }
        LuaStackGuard Guard(P->State);
        lua_rawgeti(P->State, LUA_REGISTRYINDEX, InObject);
        if(!lua_istable(P->State, -1))
        {
            return false;
        }
        lua_getfield(P->State, -1, InMethod);
        return lua_isfunction(P->State, -1);
    }

    bool ScriptEngine::CallObjectMethod(const Object InObject, const char* InMethod, const EventArgs& InArgs)
    {
        if(!P->State || InObject <= 0 || !InMethod)
        {
            return false;
        }
        LuaStackGuard Guard(P->State);
        lua_rawgeti(P->State, LUA_REGISTRYINDEX, InObject);                      // self
        if(!lua_istable(P->State, -1))
        {
            return false;
        }
        lua_getfield(P->State, -1, InMethod);                                    // self, fn
        if(!lua_isfunction(P->State, -1))
        {
            return false;
        }
        lua_insert(P->State, -2);                                                // fn, self
        const int Count = PushEventArgs(P->State, InArgs);
        /* LuaCallProtected counts a runtime error itself. */
        return LuaCallProtected(P->State, Count + 1, 0, InMethod);
    }

    bool ScriptEngine::SetObjectField(const Object InObject, const char* InName, const EventArg& InValue)
    {
        if(!P->State || InObject <= 0 || !InName)
        {
            return false;
        }
        LuaStackGuard Guard(P->State);
        lua_rawgeti(P->State, LUA_REGISTRYINDEX, InObject);
        if(!lua_istable(P->State, -1))
        {
            return false;
        }
        PushEventArg(P->State, InValue);
        lua_setfield(P->State, -2, InName);
        return true;
    }

    void ScriptEngine::ReleaseObject(const Object InObject)
    {
        if(P->State && InObject > 0)
        {
            luaL_unref(P->State, LUA_REGISTRYINDEX, InObject);
        }
    }

    void ScriptEngine::ForgetClass(const char* InClassPath)
    {
        if(!P->State || !InClassPath)
        {
            return;
        }
        LuaStackGuard Guard(P->State);
        lua_getfield(P->State, LUA_REGISTRYINDEX, OBJECT_CLASS_CACHE_KEY);
        if(lua_istable(P->State, -1))
        {
            lua_pushnil(P->State);
            lua_setfield(P->State, -2, InClassPath);
        }
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

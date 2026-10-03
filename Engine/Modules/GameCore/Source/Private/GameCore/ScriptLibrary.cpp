#include "Public/GameCore/ScriptLibrary.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Private/GameCore/GameCoreInternal.h"

/*
 * Script libraries -- the registry and the Lua half of ScriptLibrary.h. See
 * DESIGN.md.
 *
 * BY HANDLE, NOT BY POINTER. Each Lua function is a C closure over
 * LibraryDispatch whose upvalues are the library's handle and the member's
 * index, and the dispatcher looks both up on every call. Unregistering (the
 * declaring module unloading) therefore turns a stale closure into a Lua error
 * instead of a call into unmapped code, and the registry's vectors may move.
 *
 * THE LONGJMP RULE, as in ComponentScript.cpp: LibraryDispatch keeps only
 * trivially-destructible locals and raises only after the game's function has
 * returned.
 */

namespace GameCore
{
namespace
{
    struct Library
    {
        ScriptLibraryHandle              Handle = SCRIPT_LIBRARY_INVALID;
        std::string                      Name;
        std::vector<ScriptLibraryMember> Members;
    };

    /* Function-local, so a registrar in any image, run at any point of static
     * initialisation, finds it constructed. */
    struct Registry
    {
        std::vector<Library> Libraries;
        ScriptLibraryHandle  NextHandle = 1;
    };

    Registry& GetRegistry()
    {
        static Registry Instance;
        return Instance;
    }

    const Library* FindLibrary(const ScriptLibraryHandle InHandle)
    {
        for(const Library& Each : GetRegistry().Libraries)
        {
            if(Each.Handle == InHandle) { return &Each; }
        }
        return nullptr;
    }

    bool IsIdentifier(const char* InText)
    {
        if(!InText || InText[0] == '\0' || (InText[0] >= '0' && InText[0] <= '9')) { return false; }
        for(const char* Ch = InText; *Ch != '\0'; ++Ch)
        {
            const bool bOk = (*Ch >= 'a' && *Ch <= 'z') || (*Ch >= 'A' && *Ch <= 'Z') || (*Ch >= '0' && *Ch <= '9') || *Ch == '_';
            if(!bOk) { return false; }
        }
        return true;
    }

    /* Globals a library must not replace: the engine's own tables and the
     * standard libraries luaL_openlibs installs. */
    bool IsReservedGlobal(const char* InName)
    {
        static const char* const Reserved[] = {
            "World", "Events", "TickPhase", "Entity",
            "_G", "_ENV", "coroutine", "debug", "io", "math", "os", "package", "string", "table", "utf8",
            "print", "require", "type", "pairs", "ipairs", "select", "error", "assert", "tostring", "tonumber",
        };
        for(const char* Each : Reserved)
        {
            if(std::strcmp(Each, InName) == 0) { return true; }
        }
        return false;
    }

    enum ELibraryMode : int
    {
        LM_Call = 0,
        LM_Get,
        LM_Set,
    };

    int LibraryDispatch(lua_State* InState)
    {
        const ScriptLibraryHandle Handle = ScriptLibraryHandle(lua_tointeger(InState, lua_upvalueindex(1)));
        const size_t              Index  = size_t(lua_tointeger(InState, lua_upvalueindex(2)));
        const int                 Mode   = int(lua_tointeger(InState, lua_upvalueindex(3)));

        const Library* Lib = FindLibrary(Handle);
        if(!Lib || Index >= Lib->Members.size())
        {
            /* Upvalue 4 is the qualified name, kept for exactly this message. */
            return luaL_error(InState, "%s belongs to a script library that has unloaded",
                              lua_tostring(InState, lua_upvalueindex(4)));
        }
        const ScriptLibraryMember& Member = Lib->Members[Index];

        switch(Mode)
        {
        case LM_Get:
            return LuaPushScriptValue(InState, Member.FieldType, Member.Size, Member.FieldStruct,
                                      static_cast<const uint8_t*>(Member.Storage));

        case LM_Set:
            LuaReadScriptValue(InState, Member.FieldType, Member.Size, Member.FieldStruct, 1, static_cast<uint8_t*>(Member.Storage));
            return 0;

        default:
        {
            ScriptCall Call(InState, Member.Name, EntityID(), LuaGetBoundWorld(InState), /*InArgBase=*/0);
            Member.Function(Call);
            if(Call.HasError())
            {
                /* Raised here, after the game's code has returned. */
                return luaL_error(InState, "%s", Call.GetErrorText());
            }
            return Call.GetResultCount();
        }
        }
    }

    void SetDispatch(lua_State* InState, const Library& InLibrary, const size_t InIndex, const ELibraryMode InMode,
                     const char* InLuaName)
    {
        lua_pushinteger(InState, lua_Integer(InLibrary.Handle));
        lua_pushinteger(InState, lua_Integer(InIndex));
        lua_pushinteger(InState, lua_Integer(InMode));
        lua_pushfstring(InState, "%s.%s", InLibrary.Name.c_str(), InLuaName);
        lua_pushcclosure(InState, &LibraryDispatch, 4);
        lua_setfield(InState, -2, InLuaName);
    }
}

ScriptLibraryHandle RegisterScriptLibrary(const char* InName, const ScriptLibraryMember* InMembers,
                                          const size_t InCount, const size_t InRejected)
{
    const std::string Name = InName ? InName : "";
    if(!IsIdentifier(Name.c_str()) || Name.size() >= SCRIPT_LIBRARY_NAME_MAX || IsReservedGlobal(Name.c_str()))
    {
        GAMECORE_LOG_ERROR("RegisterScriptLibrary: '" + Name + "' cannot be a script library: it must be an identifier of under "
                           + std::to_string(size_t(SCRIPT_LIBRARY_NAME_MAX)) + " characters and not a global Lua or the engine uses")
        return SCRIPT_LIBRARY_INVALID;
    }

    Registry& R = GetRegistry();
    for(const Library& Each : R.Libraries)
    {
        if(Each.Name == Name)
        {
            /* First wins, as RegisterComponentScript does; the second image's
             * registrar then has nothing to unregister. */
            GAMECORE_LOG_WARN("RegisterScriptLibrary: a library named '" + Name + "' is already registered; keeping the first")
            return SCRIPT_LIBRARY_INVALID;
        }
    }

    if(InRejected > 0)
    {
        GAMECORE_LOG_ERROR("RegisterScriptLibrary: '" + Name + "' had " + std::to_string(InRejected)
                           + " member(s) rejected -- more than " + std::to_string(size_t(SCRIPT_LIBRARY_MEMBERS_MAX))
                           + ", a missing function or variable, or a name or signature too long to store unchanged")
    }

    Library Added;
    Added.Handle = R.NextHandle++;
    Added.Name   = Name;
    for(size_t Index = 0; Index < InCount; ++Index)
    {
        const ScriptLibraryMember& Member = InMembers[Index];

        bool bDuplicate = false;
        for(const ScriptLibraryMember& Earlier : Added.Members)
        {
            bDuplicate = bDuplicate || std::strcmp(Earlier.Name, Member.Name) == 0;
        }
        if(!IsIdentifier(Member.Name) || bDuplicate)
        {
            GAMECORE_LOG_ERROR("RegisterScriptLibrary: '" + Name + "." + Member.Name + "' is "
                               + (bDuplicate ? "declared twice" : "not an identifier") + "; dropped")
            continue;
        }
        Added.Members.push_back(Member);
    }

    R.Libraries.push_back(std::move(Added));
    return R.Libraries.back().Handle;
}

void UnregisterScriptLibrary(const ScriptLibraryHandle InHandle)
{
    if(InHandle == SCRIPT_LIBRARY_INVALID) { return; }

    std::vector<Library>& Libraries = GetRegistry().Libraries;
    for(auto It = Libraries.begin(); It != Libraries.end(); ++It)
    {
        if(It->Handle == InHandle)
        {
            Libraries.erase(It);
            return;
        }
    }
}

size_t GetScriptLibraryCount()
{
    return GetRegistry().Libraries.size();
}

bool GetScriptLibrary(const size_t InIndex, ScriptLibraryInfo& OutInfo)
{
    const std::vector<Library>& Libraries = GetRegistry().Libraries;
    if(InIndex >= Libraries.size())
    {
        OutInfo = ScriptLibraryInfo();
        return false;
    }
    OutInfo.Name    = Libraries[InIndex].Name.c_str();
    OutInfo.Members = Libraries[InIndex].Members.data();
    OutInfo.Count   = Libraries[InIndex].Members.size();
    return true;
}

void ScriptLibrary_Install(lua_State* InState)
{
    for(const Library& Lib : GetRegistry().Libraries)
    {
        lua_newtable(InState);
        for(size_t Index = 0; Index < Lib.Members.size(); ++Index)
        {
            const ScriptLibraryMember& Member = Lib.Members[Index];
            switch(Member.Kind)
            {
            case EScriptLibraryMember::Function:
            case EScriptLibraryMember::Pure:
                SetDispatch(InState, Lib, Index, LM_Call, Member.Name);
                break;

            case EScriptLibraryMember::Variable:
            {
                char Accessor[SCRIPT_BINDING_NAME_MAX + 4];
                std::snprintf(Accessor, sizeof(Accessor), "Get%s", Member.Name);
                SetDispatch(InState, Lib, Index, LM_Get, Accessor);
                if(!Member.bReadOnly)
                {
                    std::snprintf(Accessor, sizeof(Accessor), "Set%s", Member.Name);
                    SetDispatch(InState, Lib, Index, LM_Set, Accessor);
                }
                break;
            }

            case EScriptLibraryMember::Event:
                /* Nothing callable: events go through Events.On / Events.Emit.
                 * The name is published so a script can avoid the literal. */
                lua_pushstring(InState, Member.Name);
                lua_setfield(InState, -2, Member.Name);
                break;
            }
        }
        lua_setglobal(InState, Lib.Name.c_str());
    }
}
}

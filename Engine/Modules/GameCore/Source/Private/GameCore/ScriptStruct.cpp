#include "Public/GameCore/ScriptStruct.h"

#include <cstring>
#include <string>

#include "Public/GameCore/GameCoreLog.h"

/*
 * The script struct registry (ScriptStruct.h). Constant-initialised, as
 * ComponentType.cpp's table is, so a registrar in any image finds it valid
 * during static initialisation, and ids are never reused: an id is a name.
 */

namespace GameCore
{
namespace
{
    ScriptStructInfo GStructs[SCRIPT_STRUCT_MAX];
    size_t           GStructCount = 0;

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
}

ScriptStructID FindOrAddScriptStruct(const char* InName)
{
    for(size_t Index = 0; Index < GStructCount; ++Index)
    {
        if(std::strcmp(GStructs[Index].Name, InName ? InName : "") == 0) { return ScriptStructID(Index); }
    }

    /* A graph pin type and part of generated Lua names, so an identifier. */
    if(!IsIdentifier(InName) || std::strlen(InName) >= SCRIPT_STRUCT_NAME_MAX || GStructCount >= SCRIPT_STRUCT_MAX)
    {
        GAMECORE_LOG_ERROR(std::string("FindOrAddScriptStruct: '") + (InName ? InName : "") + "' cannot be a script struct: "
                           "it must be an identifier of under " + std::to_string(size_t(SCRIPT_STRUCT_NAME_MAX))
                           + " characters, and at most " + std::to_string(size_t(SCRIPT_STRUCT_MAX)) + " structs exist")
        return SCRIPT_STRUCT_INVALID;
    }

    ScriptStructInfo& Added = GStructs[GStructCount];
    std::strncpy(Added.Name, InName, SCRIPT_STRUCT_NAME_MAX - 1);
    Added.ID = ScriptStructID(GStructCount);
    return ScriptStructID(GStructCount++);
}

bool RegisterScriptStruct(const ScriptStructID InID, const uint32_t InSizeBytes, const ScriptStructField* InFields,
                          const size_t InCount, const size_t InRejected)
{
    if(InID >= GStructCount)
    {
        GAMECORE_LOG_ERROR("RegisterScriptStruct: struct id " + std::to_string(InID) + " was never assigned")
        return false;
    }

    ScriptStructInfo& Info = GStructs[InID];
    if(Info.bRegistered)
    {
        GAMECORE_LOG_WARN(std::string("RegisterScriptStruct: '") + Info.Name + "' is already registered; keeping the first")
        return false;
    }
    if(InRejected > 0)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterScriptStruct: '") + Info.Name + "' had " + std::to_string(InRejected)
                           + " field(s) rejected -- more than " + std::to_string(size_t(SCRIPT_STRUCT_FIELDS_MAX))
                           + ", or a name too long to store unchanged")
    }

    Info.FieldCount = 0;
    Info.Lanes      = 0;
    for(size_t Index = 0; Index < InCount && Index < SCRIPT_STRUCT_FIELDS_MAX; ++Index)
    {
        const ScriptStructField& Field = InFields[Index];

        /* Field names become Lua locals (Stats_Value) and graph pins. */
        bool bDuplicate = false;
        for(uint16_t Earlier = 0; Earlier < Info.FieldCount; ++Earlier)
        {
            bDuplicate = bDuplicate || std::strcmp(Info.Fields[Earlier].Name, Field.Name) == 0;
        }
        if(!IsIdentifier(Field.Name) || bDuplicate || Field.Type == EScriptFieldType::Struct)
        {
            GAMECORE_LOG_ERROR(std::string("RegisterScriptStruct: '") + Info.Name + "." + Field.Name
                               + "' is a repeated or invalid name, or a nested struct; dropped")
            continue;
        }

        Info.Fields[Info.FieldCount++] = Field;
        Info.Lanes = uint16_t(Info.Lanes + GetScriptFieldLanes(Field.Type, SCRIPT_STRUCT_INVALID));
    }
    Info.SizeBytes   = InSizeBytes;
    Info.bRegistered = true;
    return true;
}

void UnregisterScriptStruct(const ScriptStructID InID)
{
    if(InID >= GStructCount || !GStructs[InID].bRegistered) { return; }

    /* The name and id stay: they are the struct's identity. */
    ScriptStructInfo& Info = GStructs[InID];
    Info.bRegistered = false;
    Info.FieldCount  = 0;
    Info.Lanes       = 0;
    Info.SizeBytes   = 0;
}

const ScriptStructInfo* GetScriptStruct(const ScriptStructID InID)
{
    return InID < GStructCount && GStructs[InID].bRegistered ? &GStructs[InID] : nullptr;
}

const char* GetScriptStructName(const ScriptStructID InID)
{
    return InID < GStructCount ? GStructs[InID].Name : "";
}

size_t GetScriptStructCount()
{
    return GStructCount;
}

int GetScriptFieldLanes(const EScriptFieldType InType, const ScriptStructID InStruct)
{
    switch(InType)
    {
    case EScriptFieldType::Vector3: return 3;
    case EScriptFieldType::AABB:    return 6;
    case EScriptFieldType::Color:   return 4;
    case EScriptFieldType::Struct:
    {
        const ScriptStructInfo* Info = GetScriptStruct(InStruct);
        return Info ? int(Info->Lanes) : 0;
    }
    default:                        return 1;
    }
}
}

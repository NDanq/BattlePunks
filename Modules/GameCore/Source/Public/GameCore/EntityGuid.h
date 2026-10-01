#ifndef GAMECORE_ENTITYGUID_H
#define GAMECORE_ENTITYGUID_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

/*
 * An entity's persistent identity: 128 random bits kept across saves. Files
 * name entities by this (EntityID is per-run). See DESIGN.md.
 *
 * POD, 16 bytes. Written as 32 lowercase hex digits, Hi then Lo.
 */
namespace GameCore
{
    struct EntityGuid
    {
        uint64_t Hi = 0;
        uint64_t Lo = 0;

        /* All-zero is "no GUID"; Generate() never returns it. */
        bool IsSet() const { return Hi != 0 || Lo != 0; }

        /* 32 hex digits plus the NUL. */
        enum : size_t { STRING_LENGTH = 32, STRING_BUFFER = STRING_LENGTH + 1 };

        void ToString(char (&OutText)[STRING_BUFFER]) const
        {
            static const char Digits[] = "0123456789abcdef";
            for(int Index = 0; Index < 16; ++Index)
            {
                OutText[Index]      = Digits[(Hi >> (60 - 4 * Index)) & 0xFu];
                OutText[16 + Index] = Digits[(Lo >> (60 - 4 * Index)) & 0xFu];
            }
            OutText[STRING_LENGTH] = '\0';
        }

        /* Exactly 32 hex digits (either case); else false, OutGuid untouched. */
        static bool Parse(const char* InText, EntityGuid& OutGuid)
        {
            if(!InText) { return false; }
            EntityGuid Parsed;
            for(int Index = 0; Index < int(STRING_LENGTH); ++Index)
            {
                const char Ch = InText[Index];
                uint64_t Nibble;
                if(Ch >= '0' && Ch <= '9')      { Nibble = uint64_t(Ch - '0'); }
                else if(Ch >= 'a' && Ch <= 'f') { Nibble = uint64_t(Ch - 'a' + 10); }
                else if(Ch >= 'A' && Ch <= 'F') { Nibble = uint64_t(Ch - 'A' + 10); }
                else                            { return false; }   // also catches an early NUL

                uint64_t& Half = Index < 16 ? Parsed.Hi : Parsed.Lo;
                Half = (Half << 4) | Nibble;
            }
            if(InText[STRING_LENGTH] != '\0') { return false; }
            OutGuid = Parsed;
            return true;
        }

        /* Out-of-line: one generator, in GameCore.dll. */
        static GAMECORE_API EntityGuid Generate();

        friend bool operator==(const EntityGuid& L, const EntityGuid& R) { return L.Hi == R.Hi && L.Lo == R.Lo; }
        friend bool operator!=(const EntityGuid& L, const EntityGuid& R) { return !(L == R); }
        friend bool operator<(const EntityGuid& L, const EntityGuid& R)  { return L.Hi != R.Hi ? L.Hi < R.Hi : L.Lo < R.Lo; }
    };

    /* For unordered containers keyed by GUID. The bits are already random. */
    struct EntityGuidHash
    {
        size_t operator()(const EntityGuid& InGuid) const { return size_t(InGuid.Hi ^ (InGuid.Lo * 0x9E3779B97F4A7C15ull)); }
    };
}

#endif //GAMECORE_ENTITYGUID_H

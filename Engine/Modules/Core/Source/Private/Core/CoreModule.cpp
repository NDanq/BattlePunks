#include "Public/Core/CoreModule.h"

#include <cstring>
#include <cstdlib>

/*
 * ModuleReloadState lives here rather than in the header for the reason the
 * header spells out at length: every one of these functions must exist exactly
 * once in the process, inside EngineCore.dll, so that the buffer is allocated
 * and freed by the same allocator no matter which image asked. Do not move any
 * of them inline.
 */

namespace EngineCore
{
ModuleReloadState::~ModuleReloadState()
{
    Reset();
}

ModuleReloadState::ModuleReloadState(ModuleReloadState&& InOther) noexcept
    : StateVersion(InOther.StateVersion)
    , SizeBytes   (InOther.SizeBytes)
    , Capacity    (InOther.Capacity)
    , Data        (InOther.Data)
{
    InOther.Data         = nullptr;
    InOther.Capacity     = 0;
    InOther.SizeBytes    = 0;
    InOther.StateVersion = 0;
}

ModuleReloadState& ModuleReloadState::operator=(ModuleReloadState&& InOther) noexcept
{
    if(this == &InOther) return *this;

    Reset();        // frees inside EngineCore, which is the whole point

    StateVersion = InOther.StateVersion;
    SizeBytes    = InOther.SizeBytes;
    Capacity     = InOther.Capacity;
    Data         = InOther.Data;

    InOther.Data         = nullptr;
    InOther.Capacity     = 0;
    InOther.SizeBytes    = 0;
    InOther.StateVersion = 0;
    return *this;
}

bool ModuleReloadState::Write(const uint32_t InStateVersion, const void* InData, const size_t InBytes)
{
    // Grow only. A reload that shrinks the payload keeps the larger buffer,
    // which costs a few bytes and saves an allocation on the next grow back --
    // and reload is exactly the moment not to be clever about memory.
    if(InBytes > Capacity)
    {
        uint8_t* Grown = static_cast<uint8_t*>(std::realloc(Data, InBytes));
        if(!Grown)
        {
            // Leave the previous payload intact. A failed Write must not
            // destroy the state the caller was trying to replace, because the
            // caller is mid-shutdown and has nowhere to put it instead.
            return false;
        }
        Data     = Grown;
        Capacity = InBytes;
    }

    if(InBytes > 0)
    {
        if(!InData)
        {
            return false;
        }
        std::memcpy(Data, InData, InBytes);
    }

    StateVersion = InStateVersion;
    SizeBytes    = InBytes;
    return true;
}

bool ModuleReloadState::Read(const uint32_t InExpectedVersion, void* OutDst, const size_t InBytes) const
{
    /*
     * All or nothing, on BOTH fields. A partial or best-effort read is the one
     * behaviour this must never have: the caller is about to treat OutDst as a
     * struct, and half-filling it is worse than not filling it, because the
     * caller cannot tell.
     */
    if(InExpectedVersion != StateVersion || InBytes != SizeBytes)
    {
        return false;
    }

    if(InBytes == 0)
    {
        return true;        // a zero-byte payload reads back as a successful nothing
    }

    if(!OutDst || !Data)
    {
        return false;
    }

    std::memcpy(OutDst, Data, InBytes);
    return true;
}

void ModuleReloadState::Reset()
{
    std::free(Data);
    Data         = nullptr;
    Capacity     = 0;
    SizeBytes    = 0;
    StateVersion = 0;
}
}

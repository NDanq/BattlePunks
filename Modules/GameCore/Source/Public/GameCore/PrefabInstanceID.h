#ifndef GAMECORE_PREFABINSTANCEID_H
#define GAMECORE_PREFABINSTANCEID_H

#include <cstdint>

/*
 * A prefab instance in a World: an index into World's instance table and a
 * generation, exactly like EntityID (see EntityID.h). POD, 8 bytes.
 */
namespace GameCore
{
    struct PrefabInstanceID
    {
        uint32_t Index      = 0xFFFFFFFFu;
        uint32_t Generation = 0;

        PrefabInstanceID() = default;
        PrefabInstanceID(uint32_t InIndex, uint32_t InGeneration) : Index(InIndex), Generation(InGeneration) {}

        /* Ever assigned -- NOT alive. World::IsInstanceAlive is the liveness check. */
        bool IsSet() const { return Index != 0xFFFFFFFFu; }

        uint64_t Pack() const { return (uint64_t(Generation) << 32) | uint64_t(Index); }
        static PrefabInstanceID Unpack(uint64_t InPacked)
        {
            return PrefabInstanceID(uint32_t(InPacked & 0xFFFFFFFFu), uint32_t(InPacked >> 32));
        }

        friend bool operator==(const PrefabInstanceID& L, const PrefabInstanceID& R)
        {
            return L.Index == R.Index && L.Generation == R.Generation;
        }
        friend bool operator!=(const PrefabInstanceID& L, const PrefabInstanceID& R) { return !(L == R); }
    };
}

#endif //GAMECORE_PREFABINSTANCEID_H

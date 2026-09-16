#ifndef GAMECORE_ENTITYID_H
#define GAMECORE_ENTITYID_H

#include <cstdint>

/*
 * An entity is nothing but this: an index into World's slot tables, and a
 * generation that catches a handle to a slot that has since been recycled --
 * see DESIGN.md for why a generation is needed at all. POD, 8 bytes,
 * trivially copyable, stored by value everywhere.
 */
namespace GameCore
{
    struct EntityID
    {
        uint32_t Index      = 0xFFFFFFFFu;
        uint32_t Generation = 0;

        EntityID() = default;
        EntityID(uint32_t InIndex, uint32_t InGeneration) : Index(InIndex), Generation(InGeneration) {}

        /* Only checks that this handle was ever assigned a real slot -- NOT that
         * the entity is still alive. World::IsAlive() is the liveness check; this
         * is the cheaper "was this ever set" check a default-constructed EntityID
         * fails. */
        bool IsSet() const { return Index != 0xFFFFFFFFu; }

        /* Crosses into Lua as a single lua_Integer and back -- see DESIGN.md
         * for why Index is in the low bits. */
        uint64_t Pack() const { return (uint64_t(Generation) << 32) | uint64_t(Index); }
        static EntityID Unpack(uint64_t InPacked)
        {
            return EntityID(uint32_t(InPacked & 0xFFFFFFFFu), uint32_t(InPacked >> 32));
        }

        friend bool operator==(const EntityID& L, const EntityID& R)
        {
            return L.Index == R.Index && L.Generation == R.Generation;
        }
        friend bool operator!=(const EntityID& L, const EntityID& R) { return !(L == R); }
    };

    /* The value an unset EntityID compares equal to -- spelled out for call
     * sites that want a named constant rather than {}. */
    inline EntityID InvalidEntityID() { return EntityID(); }
}

#endif //GAMECORE_ENTITYID_H

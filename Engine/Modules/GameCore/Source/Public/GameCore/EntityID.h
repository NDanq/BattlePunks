#ifndef GAMECORE_ENTITYID_H
#define GAMECORE_ENTITYID_H

#include <cstdint>

/*
 * An entity is nothing but this: an index into World's slot tables, and a
 * generation that catches a handle to a slot that has since been recycled.
 *
 * WHY A GENERATION AND NOT JUST AN INDEX
 *
 * Indices are reused the instant an entity is destroyed -- CreateEntity() pulls
 * from a free list, it does not grow forever. Without a generation, an EntityID
 * saved before a destroy would silently address whatever new entity took that
 * slot after it, which is a much worse failure than a null: it does not crash,
 * it reads and writes the wrong entity's data. World bumps Generation on every
 * DestroyEntity, and IsAlive()/every GetComponent call checks it before trusting
 * Index.
 *
 * POD, 8 bytes, trivially copyable -- an EntityID is stored BY VALUE everywhere
 * (inside ComponentOwner, inside the spatial index's leaves, across the Lua
 * boundary as a packed uint64_t) and never needs its own lifetime management.
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

        /* Crosses into Lua as a single lua_Integer (64-bit in a default Lua 5.4
         * build) and back. Index in the low 32 bits so a bare cast to uint32_t on
         * the Lua side -- a mistake, but a survivable one -- still names the
         * right slot instead of the generation. */
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

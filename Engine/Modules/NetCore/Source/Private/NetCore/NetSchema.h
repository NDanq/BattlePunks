#ifndef NETCORE_NETSCHEMA_H
#define NETCORE_NETSCHEMA_H

#include <cstdint>
#include <vector>

#include "Public/GameCore/ComponentType.h"

/*
 * What both ends must agree on: the component types that replicate or have
 * RPCs, sorted by name -- a type's position is its wire type -- and a hash of
 * their replicated fields and RPCs. Built from GameCore's registries; rebuilt
 * when they change (a game module loaded, or hot reloaded).
 *
 * By NAME, never ComponentTypeID: ids are handed out in first-use order and
 * differ between processes.
 */

namespace NetCore
{
    enum : uint16_t { NET_WIRE_TYPE_INVALID = 0xFFFF };

    class NetSchema
    {
    public:
        /* Rebuilds if GameCore's tables changed since the last call. */
        void Refresh();

        uint64_t GetHash() const { return Hash; }

        uint16_t                  ToWire(GameCore::ComponentTypeID InType) const;
        GameCore::ComponentTypeID FromWire(uint16_t InWire) const;

    private:
        void Rebuild();

        std::vector<GameCore::ComponentTypeID> Types;    // wire type -> local id
        std::vector<uint16_t>                  ToWireMap; // local id -> wire type
        uint64_t                               Hash = 0;
        uint32_t                               BuiltGeneration = 0xFFFFFFFFu;
        size_t                                 BuiltTypeCount  = 0;
    };
}

#endif //NETCORE_NETSCHEMA_H

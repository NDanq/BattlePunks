#ifndef NETCORE_NETPROTOCOL_H
#define NETCORE_NETPROTOCOL_H

#include <cstdint>

/*
 * The wire. Every packet is one EngineCore::Archive (little-endian, length-
 * prefixed strings) that starts with an ENetMessage byte. Bump
 * NET_PROTOCOL_VERSION for any change below; a peer on another version is
 * refused at the handshake.
 *
 * GAME CHANNEL (NetTransport.h's NET_CHANNEL_GAME), in order:
 *
 *   Hello          C->S  u32 version, u64 schema hash, string level
 *   Welcome        S->C  i32 client id
 *   Reject         S->C  string reason; the server then disconnects
 *   Spawn          S->C  i32 net id, u64 guid hi, u64 guid lo, string name,
 *                        u64 parent guid hi, u64 parent guid lo (0,0: none),
 *                        u16 count, then per component:
 *                            string type name, then its serializer's DATA chunk
 *                        (versioned, as a cooked world; EntityIDs as net ids)
 *   Destroy        S->C  i32 net id
 *   State          S->C  i32 net id, u16 wire type, u32 field mask, then each
 *                        masked field's value in table order (NetReplication.h)
 *   Rpc            both  i32 net id, u16 wire type, u8 rpc index, u32 bytes, bytes
 *
 * Wire types index NetSchema's name-sorted list of the types that replicate
 * or have RPCs -- identical on both ends because the schema hash matched.
 *
 * EDITOR CHANNEL: the Editor* messages (NetEditorLink.h).
 */

namespace NetCore
{
    constexpr uint32_t NET_PROTOCOL_VERSION = 1;

    enum class ENetMessage : uint8_t
    {
        Hello = 1,
        Welcome,
        Reject,
        Spawn,
        Destroy,
        State,
        Rpc,

        EditorHello = 64,     // u32 version, string user name
        EditorCommand,        // string user name, string command text (STUB)
        EditorPropertyEdit,   // reserved: guid, type name, field name, value (STUB)
    };
}

#endif //NETCORE_NETPROTOCOL_H

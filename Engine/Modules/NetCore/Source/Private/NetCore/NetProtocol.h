#ifndef NETCORE_NETPROTOCOL_H
#define NETCORE_NETPROTOCOL_H

#include <cstdint>

/*
 * The wire. Every packet is one EngineCore::Archive (little-endian, length-
 * prefixed strings) that starts with an ENetMessage byte. Bump
 * NET_PROTOCOL_VERSION for any change below; a peer on another version is
 * refused at the handshake.
 *
 * GAME CHANNEL (NetTransport.h's NET_CHANNEL_GAME): reliable, in order.
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
 *   Rpc            both  i32 net id, u16 wire type, u8 rpc index, u32 bytes, bytes
 *
 * STATE CHANNEL (NET_CHANNEL_STATE): unreliable, sequenced -- a packet older
 * than the newest received is dropped, never delivered late.
 *
 *   StateBatch     S->C  u32 sequence, u32 server time in ms, u16 count, then
 *                        per entry: i32 net id, u16 wire type, u32 field mask,
 *                        each masked field's value in table order
 *                        (NetReplication.h). At least one a server tick, to
 *                        each client, entries or not: its clock.
 *   StateAck       C->S  u32 newest sequence applied, u32 bits: bit i set if
 *                        sequence - 1 - i was applied too
 *
 * Each batch holds, for every component it mentions, every field that
 * differs from what the client has acknowledged: applied over that, it gives
 * the server's state at the batch's time, whatever was lost in between.
 * NetSession.cpp ("acknowledged baselines") has the bookkeeping.
 *
 * Wire types index NetSchema's name-sorted list of the types that replicate
 * or have RPCs -- identical on both ends because the schema hash matched.
 *
 * EDITOR CHANNEL: the Editor* messages (NetEditorLink.h).
 */

namespace NetCore
{
    constexpr uint32_t NET_PROTOCOL_VERSION = 3;

    enum class ENetMessage : uint8_t
    {
        Hello = 1,
        Welcome,
        Reject,
        Spawn,
        Destroy,
        Rpc,
        StateBatch,           // S->C, unreliable: replicated fields, against the acknowledged baseline
        StateAck,             // C->S, unreliable: which batches were applied

        EditorHello = 64,     // u32 version, string user name
        EditorCommand,        // string user name, string command text (STUB)
        EditorPropertyEdit,   // reserved: guid, type name, field name, value (STUB)
    };
}

#endif //NETCORE_NETPROTOCOL_H

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
 *   Hello          C->S  u32 version, u64 schema hash, string level,
 *                        u64 reconnect token (0: a new client)
 *   Welcome        S->C  i32 client id, u64 reconnect token, u8 returning
 *                        (1: the token's client, back -- same id, same entities)
 *   SnapshotDone   S->C  (nothing): every networked entity has been sent;
 *                        a returning client drops what was not
 *   Reject         S->C  string reason; the server then disconnects
 *   Spawn          S->C  i32 net id, u64 guid hi, u64 guid lo, string name,
 *                        u64 parent guid hi, u64 parent guid lo (0,0: none),
 *                        u16 count, then per component:
 *                            string type name, then its serializer's DATA chunk
 *                        (versioned, as a cooked world; EntityIDs as net ids)
 *   Destroy        S->C  i32 net id; also when the entity stops replicating
 *                        (bReplicate off, or its NetComponent removed)
 *   AddComponents  S->C  i32 net id, then as Spawn from u16 count on: the
 *                        components it gained since it was sent
 *   RemoveComponents S->C i32 net id, u16 count, then per component: string
 *                        type name
 *   Reparent       S->C  i32 net id, u64 parent guid hi, u64 parent guid lo
 *                        (0,0: none)
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
 * NetSession.cpp ("acknowledged baselines") has the bookkeeping. A batch
 * carries only components the client has been told the entity has (Spawn,
 * AddComponents), so one that names a component the client lacks and was not
 * told of is ahead of its AddComponents: it is not acknowledged, and comes
 * again.
 *
 * Wire types index NetSchema's name-sorted list of the types that replicate
 * or have RPCs -- identical on both ends because the schema hash matched.
 *
 * EDITOR CHANNEL: the Editor* messages (NetEditorLink.h).
 */

namespace NetCore
{
    constexpr uint32_t NET_PROTOCOL_VERSION = 5;

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
        AddComponents,        // S->C: components a sent entity gained
        RemoveComponents,     // S->C: components it lost
        Reparent,             // S->C: its new parent
        SnapshotDone,         // S->C: the snapshot a (re)joining client gets is complete

        EditorHello = 64,     // u32 version, string user name
        EditorCommand,        // string user name, string command text (STUB)
        EditorPropertyEdit,   // reserved: guid, type name, field name, value (STUB)
    };
}

#endif //NETCORE_NETPROTOCOL_H

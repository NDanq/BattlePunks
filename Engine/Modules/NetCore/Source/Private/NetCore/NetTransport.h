#ifndef NETCORE_NETTRANSPORT_H
#define NETCORE_NETTRANSPORT_H

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/NetCore/NetTypes.h"

/*
 * A thin layer over ENet, private to NetCore: enet.h (and through it
 * winsock2.h and windows.h) never reaches a header outside this module.
 *
 * Peers are numbered by this layer, never exposed as ENetPeer*. Service()
 * never blocks; everything runs on the game thread.
 */

struct _ENetHost;
struct _ENetPeer;

namespace NetCore
{
    enum : uint8_t
    {
        NET_CHANNEL_GAME   = 0,   // reliable, ordered: handshake, spawns, destroys, RPCs
        NET_CHANNEL_EDITOR = 1,   // reliable, ordered: the editor link
        NET_CHANNEL_STATE  = 2,   // unreliable, sequenced: replicated state and its acks
        NET_CHANNEL_COUNT  = 3,
    };

    /* The most a state packet carries, so it fits one UDP datagram under the
     * usual 1400-odd byte path MTU with ENet's headers. */
    constexpr size_t NET_STATE_PACKET_BYTES = 1200;

    using NetPeerID = uint32_t;
    constexpr NetPeerID NET_PEER_NONE = 0xFFFFFFFFu;

    /* Why a peer went, as the other end hears it (ENet's disconnect data). */
    enum ENetDisconnectReason : uint32_t
    {
        NET_DISCONNECT_LOST     = 0,   // timed out, or no word: it may come back (NetSession reconnects)
        NET_DISCONNECT_QUIT     = 1,   // it left on purpose
        NET_DISCONNECT_CLOSED   = 2,   // the server shut down
        NET_DISCONNECT_REJECTED = 3,   // refused or kicked
    };

    struct NetTransportEvent
    {
        enum class EType : uint8_t { Connected, Disconnected, Received };

        EType                EventType = EType::Received;
        NetPeerID            Peer      = NET_PEER_NONE;
        uint8_t              Channel   = 0;
        uint32_t             Reason    = NET_DISCONNECT_LOST;   // Disconnected's
        std::vector<uint8_t> Data;
    };

    class NetTransport
    {
    public:
        NetTransport();
        ~NetTransport();

        NetTransport(const NetTransport&)            = delete;
        NetTransport& operator=(const NetTransport&) = delete;

        /* A server's socket on InPort (0: any). */
        bool Listen(uint16_t InPort, size_t InMaxPeers, uint32_t InTimeoutMs);

        /* A client's socket, and the connection to InHost:InPort under way;
         * Connected arrives through Service. OutPeer is the server's id. */
        bool Connect(const char* InHost, uint16_t InPort, uint32_t InTimeoutMs, NetPeerID& OutPeer);

        /* The next event, without waiting; false when there is none. */
        bool Service(NetTransportEvent& OutEvent);

        /* Reliable: delivered, in order on its channel, resent until it is.
         * Unreliable: once; lost, or dropped if a newer one on its channel
         * arrived first. */
        void Send(NetPeerID InPeer, uint8_t InChannel, const std::vector<uint8_t>& InBytes, bool bInReliable = true);
        void Flush();

        /* Politely: Disconnected arrives through Service. InReason is what the
         * other end hears. */
        void Disconnect(NetPeerID InPeer, uint32_t InReason = NET_DISCONNECT_REJECTED);

        /* Disconnects every peer at once, telling them InReason, and closes the socket. */
        void Close(uint32_t InReason = NET_DISCONNECT_QUIT);

        bool     IsOpen() const { return Host != nullptr; }
        uint16_t GetPort() const;
        size_t   GetPeerCount() const { return Peers.size(); }

        /* "1.2.3.4:5678", for logs. */
        std::string DescribePeer(NetPeerID InPeer) const;

        /* InPeer's round trip and loss (ENet's) and traffic (counted here).
         * False for a peer not connected. */
        bool GetPeerStats(NetPeerID InPeer, NetPeerStats& OutStats) const;

    private:
        NetPeerID Track(_ENetPeer* InPeer);

        /* What went to and came from a peer; the rates over whole seconds, at Flush. */
        struct Traffic
        {
            uint64_t BytesSent = 0, BytesReceived = 0, PacketsSent = 0, PacketsReceived = 0;
            uint64_t WindowSent = 0, WindowReceived = 0;
            std::chrono::steady_clock::time_point WindowStart = std::chrono::steady_clock::now();
            float    SendRate = 0.0f, ReceiveRate = 0.0f;
        };
        std::unordered_map<NetPeerID, Traffic> Counters;

        _ENetHost*                                Host         = nullptr;
        bool                                      bInitialized = false;
        uint32_t                                  TimeoutMs    = 5000;
        NetPeerID                                 NextPeer     = 1;
        std::unordered_map<NetPeerID, _ENetPeer*> Peers;
    };
}

#endif //NETCORE_NETTRANSPORT_H

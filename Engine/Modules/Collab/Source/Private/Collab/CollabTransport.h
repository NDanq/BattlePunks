#ifndef COLLAB_COLLABTRANSPORT_H
#define COLLAB_COLLABTRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * ENet for Collab, private to the module: enet.h (and winsock2.h, windows.h)
 * never reaches a header outside it. NetCore's NetTransport, cut down to one
 * reliable, ordered channel -- every Collab message is small and must arrive.
 * Its own because NetCore's is private to NetCore, which needs GameCore.
 *
 * Peers are numbered here, never exposed as ENetPeer*. Service() never
 * blocks. One thread.
 */

struct _ENetHost;
struct _ENetPeer;

namespace Collab
{
    using PeerID = uint32_t;
    constexpr PeerID PEER_NONE = 0xFFFFFFFFu;

    struct TransportEvent
    {
        enum class EType : uint8_t { Connected, Disconnected, Received };

        EType                EventType = EType::Received;
        PeerID               Peer      = PEER_NONE;
        std::vector<uint8_t> Data;
    };

    class CollabTransport
    {
    public:
        CollabTransport();
        ~CollabTransport();

        CollabTransport(const CollabTransport&)            = delete;
        CollabTransport& operator=(const CollabTransport&) = delete;

        /* A server's socket on InPort (0: any). */
        bool Listen(uint16_t InPort, size_t InMaxPeers, uint32_t InTimeoutMs);

        /* A client's socket and the connection under way; Connected (or, if
         * nothing answers within InTimeoutMs, Disconnected) arrives through
         * Service. OutPeer is the server's id. */
        bool Connect(const char* InHost, uint16_t InPort, uint32_t InTimeoutMs, PeerID& OutPeer);

        /* The next event, without waiting; false when there is none. */
        bool Service(TransportEvent& OutEvent);

        /* Reliable and ordered. */
        void Send(PeerID InPeer, const std::vector<uint8_t>& InBytes);
        void Flush();

        /* After what was sent to it is delivered: a Reject's reason arrives. */
        void DisconnectLater(PeerID InPeer);
        void Disconnect(PeerID InPeer);

        /* Disconnects every peer at once and closes the socket. */
        void Close();

        bool     IsOpen() const { return Host != nullptr; }
        uint16_t GetPort() const;

        /* "1.2.3.4:5678", for logs. */
        std::string DescribePeer(PeerID InPeer) const;

    private:
        PeerID Track(_ENetPeer* InPeer);

        _ENetHost*                             Host         = nullptr;
        bool                                   bInitialized = false;
        uint32_t                               TimeoutMs    = 10000;
        PeerID                                 NextPeer     = 1;
        std::unordered_map<PeerID, _ENetPeer*> Peers;
    };
}

#endif //COLLAB_COLLABTRANSPORT_H

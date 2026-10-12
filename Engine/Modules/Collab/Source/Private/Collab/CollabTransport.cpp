#include "Private/Collab/CollabTransport.h"

#include <cstring>
#include <mutex>

#include <enet/enet.h>

#include "Public/Core/CoreLog.h"

/* ENet's library-wide setup, reference-counted so a server and clients in one
 * process (the loopback test) each own a transport. NetCore counts its own:
 * enet_initialize is WSAStartup on Windows, which counts too. */

namespace Collab
{
namespace
{
    /* A mutex: the content worker's tests run a worker on its own thread. */
    std::mutex GENetMutex;
    int        GENetUsers = 0;

    bool AcquireENet()
    {
        std::lock_guard<std::mutex> Lock(GENetMutex);
        if(GENetUsers == 0 && enet_initialize() != 0)
        {
            CORE_LOG_ERROR("Collab: ENet failed to initialize")
            return false;
        }
        ++GENetUsers;
        return true;
    }

    void ReleaseENet()
    {
        std::lock_guard<std::mutex> Lock(GENetMutex);
        if(GENetUsers > 0 && --GENetUsers == 0) { enet_deinitialize(); }
    }

    /* InTimeoutMs is the most a peer may go unanswered; the minimum is ENet's
     * 5 s at most, as NetCore's (NetTransport.cpp says why). */
    void ApplyTimeout(ENetPeer* InPeer, const uint32_t InTimeoutMs)
    {
        const uint32_t Minimum = InTimeoutMs < 5000u ? InTimeoutMs : 5000u;
        enet_peer_timeout(InPeer, 0, Minimum, InTimeoutMs);
    }

    PeerID PeerIdOf(const ENetPeer* InPeer)
    {
        return InPeer && InPeer->data ? PeerID(reinterpret_cast<uintptr_t>(InPeer->data)) : PEER_NONE;
    }
}

CollabTransport::CollabTransport() = default;

CollabTransport::~CollabTransport()
{
    Close();
}

bool CollabTransport::Listen(const uint16_t InPort, const size_t InMaxPeers, const uint32_t InTimeoutMs)
{
    Close();
    if(!AcquireENet()) { return false; }
    bInitialized = true;
    TimeoutMs    = InTimeoutMs;

    ENetAddress Address;
    Address.host = ENET_HOST_ANY;
    Address.port = InPort;
    Host = enet_host_create(&Address, InMaxPeers, 1, 0, 0);
    if(!Host)
    {
        CORE_LOG_ERROR("Collab: could not listen on UDP port " + std::to_string(InPort) + " (in use?)")
        Close();
        return false;
    }
    return true;
}

bool CollabTransport::Connect(const char* InHost, const uint16_t InPort, const uint32_t InTimeoutMs, PeerID& OutPeer)
{
    OutPeer = PEER_NONE;
    Close();
    if(!AcquireENet()) { return false; }
    bInitialized = true;
    TimeoutMs    = InTimeoutMs;

    Host = enet_host_create(nullptr, 1, 1, 0, 0);
    if(!Host)
    {
        CORE_LOG_ERROR("Collab: could not open a client socket")
        Close();
        return false;
    }

    ENetAddress Address;
    Address.port = InPort;
    if(enet_address_set_host(&Address, InHost ? InHost : "") != 0)
    {
        CORE_LOG_ERROR(std::string("Collab: could not resolve '") + (InHost ? InHost : "") + "'")
        Close();
        return false;
    }

    ENetPeer* Peer = enet_host_connect(Host, &Address, 1, 0);
    if(!Peer)
    {
        CORE_LOG_ERROR("Collab: no free peer for the connection")
        Close();
        return false;
    }
    ApplyTimeout(Peer, InTimeoutMs);
    OutPeer = Track(Peer);
    return true;
}

PeerID CollabTransport::Track(ENetPeer* InPeer)
{
    const PeerID ID = NextPeer++;
    InPeer->data = reinterpret_cast<void*>(uintptr_t(ID));
    Peers[ID] = InPeer;
    return ID;
}

bool CollabTransport::Service(TransportEvent& OutEvent)
{
    if(!Host) { return false; }

    ENetEvent Event;
    while(enet_host_service(Host, &Event, 0) > 0)
    {
        switch(Event.type)
        {
            case ENET_EVENT_TYPE_CONNECT:
            {
                /* A client's own outgoing peer is already tracked. */
                PeerID ID = PeerIdOf(Event.peer);
                if(ID == PEER_NONE)
                {
                    ID = Track(Event.peer);
                    ApplyTimeout(Event.peer, TimeoutMs);
                }
                OutEvent.EventType = TransportEvent::EType::Connected;
                OutEvent.Peer      = ID;
                OutEvent.Data.clear();
                return true;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
            {
                const PeerID ID = PeerIdOf(Event.peer);
                Event.peer->data = nullptr;
                Peers.erase(ID);
                OutEvent.EventType = TransportEvent::EType::Disconnected;
                OutEvent.Peer      = ID;
                OutEvent.Data.clear();
                return true;
            }
            case ENET_EVENT_TYPE_RECEIVE:
            {
                OutEvent.EventType = TransportEvent::EType::Received;
                OutEvent.Peer      = PeerIdOf(Event.peer);
                OutEvent.Data.assign(Event.packet->data, Event.packet->data + Event.packet->dataLength);
                enet_packet_destroy(Event.packet);
                return true;
            }
            default:
                break;
        }
    }
    return false;
}

void CollabTransport::Send(const PeerID InPeer, const std::vector<uint8_t>& InBytes)
{
    const auto Found = Peers.find(InPeer);
    if(!Host || Found == Peers.end() || InBytes.empty()) { return; }

    ENetPacket* Packet = enet_packet_create(InBytes.data(), InBytes.size(), ENET_PACKET_FLAG_RELIABLE);
    if(Packet && enet_peer_send(Found->second, 0, Packet) != 0)
    {
        /* Not queued, so still ours to free. */
        enet_packet_destroy(Packet);
    }
}

void CollabTransport::Flush()
{
    if(Host) { enet_host_flush(Host); }
}

void CollabTransport::DisconnectLater(const PeerID InPeer)
{
    const auto Found = Peers.find(InPeer);
    if(Found != Peers.end()) { enet_peer_disconnect_later(Found->second, 0); }
}

void CollabTransport::Disconnect(const PeerID InPeer)
{
    const auto Found = Peers.find(InPeer);
    if(Found != Peers.end()) { enet_peer_disconnect(Found->second, 0); }
}

void CollabTransport::Close()
{
    if(Host)
    {
        /* Tell peers now, best effort: there is no later Service to finish a
         * polite disconnect in. */
        for(auto& Entry : Peers)
        {
            Entry.second->data = nullptr;
            enet_peer_disconnect_now(Entry.second, 0);
        }
        enet_host_flush(Host);
        enet_host_destroy(Host);
        Host = nullptr;
    }
    Peers.clear();
    if(bInitialized)
    {
        ReleaseENet();
        bInitialized = false;
    }
}

uint16_t CollabTransport::GetPort() const
{
    return Host ? Host->address.port : 0;
}

std::string CollabTransport::DescribePeer(const PeerID InPeer) const
{
    const auto Found = Peers.find(InPeer);
    if(Found == Peers.end()) { return "peer " + std::to_string(InPeer); }
    char Ip[64] = {};
    if(enet_address_get_host_ip(&Found->second->address, Ip, sizeof(Ip)) != 0) { std::strcpy(Ip, "?"); }
    return std::string(Ip) + ":" + std::to_string(Found->second->address.port);
}
}

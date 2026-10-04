#include "Private/NetCore/NetTransport.h"

#include <cstring>

#include <enet/enet.h>

#include "Public/Core/CoreLog.h"

/* ENet's library-wide setup is reference-counted here, so a server and a
 * client (the loopback test, or an editor that hosts and plays) can each own
 * a transport. Game thread only, like the rest of NetCore. */

namespace NetCore
{
namespace
{
    int GENetUsers = 0;

    bool AcquireENet()
    {
        if(GENetUsers == 0 && enet_initialize() != 0)
        {
            CORE_LOG_ERROR("NetCore: ENet failed to initialize")
            return false;
        }
        ++GENetUsers;
        return true;
    }

    void ReleaseENet()
    {
        if(GENetUsers > 0 && --GENetUsers == 0) { enet_deinitialize(); }
    }

    /*
     * InTimeoutMs is the most a peer may go unanswered. The minimum stays
     * ENet's 5s, with its 32-resend limit, and that matters: ENet checks
     * timeouts BEFORE it reads the socket, so after a long frame (a client's
     * first, compiling pipelines) the ack that arrived meanwhile is not seen
     * yet. Minimum == maximum would drop the connection on any hitch that long.
     */
    void ApplyTimeout(ENetPeer* InPeer, const uint32_t InTimeoutMs)
    {
        const uint32_t Minimum = InTimeoutMs < 5000u ? InTimeoutMs : 5000u;
        enet_peer_timeout(InPeer, 0, Minimum, InTimeoutMs);
    }

    NetPeerID PeerIdOf(const ENetPeer* InPeer)
    {
        return InPeer && InPeer->data ? NetPeerID(reinterpret_cast<uintptr_t>(InPeer->data)) : NET_PEER_NONE;
    }
}

NetTransport::NetTransport() = default;

NetTransport::~NetTransport()
{
    Close();
}

bool NetTransport::Listen(const uint16_t InPort, const size_t InMaxPeers, const uint32_t InTimeoutMs)
{
    Close();
    if(!AcquireENet()) { return false; }
    bInitialized = true;
    TimeoutMs    = InTimeoutMs;

    ENetAddress Address;
    Address.host = ENET_HOST_ANY;
    Address.port = InPort;
    Host = enet_host_create(&Address, InMaxPeers, NET_CHANNEL_COUNT, 0, 0);
    if(!Host)
    {
        CORE_LOG_ERROR("NetCore: could not listen on port " + std::to_string(InPort) + " (in use?)")
        Close();
        return false;
    }
    return true;
}

bool NetTransport::Connect(const char* InHost, const uint16_t InPort, const uint32_t InTimeoutMs, NetPeerID& OutPeer)
{
    OutPeer = NET_PEER_NONE;
    Close();
    if(!AcquireENet()) { return false; }
    bInitialized = true;
    TimeoutMs    = InTimeoutMs;

    Host = enet_host_create(nullptr, 1, NET_CHANNEL_COUNT, 0, 0);
    if(!Host)
    {
        CORE_LOG_ERROR("NetCore: could not open a client socket")
        Close();
        return false;
    }

    ENetAddress Address;
    Address.port = InPort;
    if(enet_address_set_host(&Address, InHost ? InHost : "") != 0)
    {
        CORE_LOG_ERROR(std::string("NetCore: could not resolve '") + (InHost ? InHost : "") + "'")
        Close();
        return false;
    }

    ENetPeer* Peer = enet_host_connect(Host, &Address, NET_CHANNEL_COUNT, 0);
    if(!Peer)
    {
        CORE_LOG_ERROR("NetCore: no free peer for the connection")
        Close();
        return false;
    }
    ApplyTimeout(Peer, InTimeoutMs);
    OutPeer = Track(Peer);
    return true;
}

NetPeerID NetTransport::Track(ENetPeer* InPeer)
{
    const NetPeerID ID = NextPeer++;
    InPeer->data = reinterpret_cast<void*>(uintptr_t(ID));
    Peers[ID] = InPeer;
    return ID;
}

bool NetTransport::Service(NetTransportEvent& OutEvent)
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
                NetPeerID ID = PeerIdOf(Event.peer);
                if(ID == NET_PEER_NONE)
                {
                    ID = Track(Event.peer);
                    ApplyTimeout(Event.peer, TimeoutMs);
                }
                OutEvent.EventType = NetTransportEvent::EType::Connected;
                OutEvent.Peer      = ID;
                OutEvent.Channel   = 0;
                OutEvent.Data.clear();
                return true;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
            {
                const NetPeerID ID = PeerIdOf(Event.peer);
                Event.peer->data = nullptr;
                Peers.erase(ID);
                OutEvent.EventType = NetTransportEvent::EType::Disconnected;
                OutEvent.Peer      = ID;
                OutEvent.Channel   = 0;
                OutEvent.Data.clear();
                return true;
            }
            case ENET_EVENT_TYPE_RECEIVE:
            {
                OutEvent.EventType = NetTransportEvent::EType::Received;
                OutEvent.Peer      = PeerIdOf(Event.peer);
                OutEvent.Channel   = Event.channelID;
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

void NetTransport::Send(const NetPeerID InPeer, const uint8_t InChannel, const std::vector<uint8_t>& InBytes)
{
    const auto Found = Peers.find(InPeer);
    if(!Host || Found == Peers.end() || InBytes.empty()) { return; }

    ENetPacket* Packet = enet_packet_create(InBytes.data(), InBytes.size(), ENET_PACKET_FLAG_RELIABLE);
    if(Packet && enet_peer_send(Found->second, InChannel, Packet) != 0)
    {
        /* Not queued, so still ours to free. */
        enet_packet_destroy(Packet);
    }
}

void NetTransport::Flush()
{
    if(Host) { enet_host_flush(Host); }
}

void NetTransport::Disconnect(const NetPeerID InPeer)
{
    const auto Found = Peers.find(InPeer);
    if(Found != Peers.end()) { enet_peer_disconnect(Found->second, 0); }
}

void NetTransport::Close()
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

uint16_t NetTransport::GetPort() const
{
    return Host ? Host->address.port : 0;
}

std::string NetTransport::DescribePeer(const NetPeerID InPeer) const
{
    const auto Found = Peers.find(InPeer);
    if(Found == Peers.end()) { return "peer " + std::to_string(InPeer); }
    char Ip[64] = {};
    if(enet_address_get_host_ip(&Found->second->address, Ip, sizeof(Ip)) != 0) { std::strcpy(Ip, "?"); }
    return std::string(Ip) + ":" + std::to_string(Found->second->address.port);
}
}

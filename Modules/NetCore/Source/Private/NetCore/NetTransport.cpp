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
     * InTimeoutMs is the most a peer may go unanswered, on both ends: ENet's
     * minimum AND maximum. ENet drops a peer once a command has been resent
     * enough times (its limit, 32: six tries, doubling) and the minimum has
     * passed. On a fast link the tries run out in milliseconds, so a minimum
     * below InTimeoutMs is the real timeout of the end still running: ENet's
     * 5s default dropped a client stalled 6s in a long first frame (compiling
     * pipelines) however long InTimeoutMs was. The stalled end itself, waking,
     * has resent nothing meanwhile, so only the maximum can drop it -- and ENet
     * checks timeouts BEFORE it reads the socket, so that holds even though
     * the answer is waiting: a stall longer than InTimeoutMs still loses the
     * connection.
     */
    void ApplyTimeout(ENetPeer* InPeer, const uint32_t InTimeoutMs)
    {
        enet_peer_timeout(InPeer, 0, InTimeoutMs, InTimeoutMs);
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
    Counters[ID] = Traffic();
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
                Counters.erase(ID);
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
                if(auto Found = Counters.find(OutEvent.Peer); Found != Counters.end())
                {
                    Found->second.BytesReceived += Event.packet->dataLength;
                    ++Found->second.PacketsReceived;
                }
                enet_packet_destroy(Event.packet);
                return true;
            }
            default:
                break;
        }
    }
    return false;
}

void NetTransport::Send(const NetPeerID InPeer, const uint8_t InChannel, const std::vector<uint8_t>& InBytes, const bool bInReliable)
{
    const auto Found = Peers.find(InPeer);
    if(!Host || Found == Peers.end() || InBytes.empty()) { return; }

    /* Unreliable is sequenced: ENet drops one older than the newest that
     * arrived on its channel. */
    ENetPacket* Packet = enet_packet_create(InBytes.data(), InBytes.size(), bInReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if(Packet && enet_peer_send(Found->second, InChannel, Packet) != 0)
    {
        /* Not queued, so still ours to free. */
        enet_packet_destroy(Packet);
        return;
    }
    Traffic& Count = Counters[InPeer];
    Count.BytesSent += InBytes.size();
    ++Count.PacketsSent;
}

void NetTransport::Flush()
{
    if(!Host) { return; }
    enet_host_flush(Host);

    const auto Now = std::chrono::steady_clock::now();
    for(auto& Entry : Counters)
    {
        Traffic& T = Entry.second;
        const double Seconds = std::chrono::duration<double>(Now - T.WindowStart).count();
        if(Seconds < 1.0) { continue; }
        T.SendRate       = float(double(T.BytesSent - T.WindowSent) / Seconds);
        T.ReceiveRate    = float(double(T.BytesReceived - T.WindowReceived) / Seconds);
        T.WindowSent     = T.BytesSent;
        T.WindowReceived = T.BytesReceived;
        T.WindowStart    = Now;
    }
}

bool NetTransport::GetPeerStats(const NetPeerID InPeer, NetPeerStats& OutStats) const
{
    const auto Found = Peers.find(InPeer);
    if(Found == Peers.end() || Found->second->state != ENET_PEER_STATE_CONNECTED) { return false; }
    const ENetPeer* Peer = Found->second;
    OutStats = NetPeerStats();
    OutStats.RoundTripMs         = Peer->roundTripTime;
    OutStats.RoundTripVarianceMs = Peer->roundTripTimeVariance;
    OutStats.PacketLossPercent   = 100.0f * float(Peer->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE);
    if(const auto Count = Counters.find(InPeer); Count != Counters.end())
    {
        OutStats.BytesSent             = Count->second.BytesSent;
        OutStats.BytesReceived         = Count->second.BytesReceived;
        OutStats.PacketsSent           = Count->second.PacketsSent;
        OutStats.PacketsReceived       = Count->second.PacketsReceived;
        OutStats.SendBytesPerSecond    = Count->second.SendRate;
        OutStats.ReceiveBytesPerSecond = Count->second.ReceiveRate;
    }
    return true;
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
    Counters.clear();
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

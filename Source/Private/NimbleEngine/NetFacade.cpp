#include "Public/NimbleEngine/NetFacade.h"

#include "Public/GameCore/Components/NetComponent.h"
#include "Private/NimbleEngine/NetInternal.h"

#if WITH_NETCORE
#include "Public/NetCore/NetSession.h"
#endif

/* NetFacade.h: the active NetCore session's answers, or Standalone's. */

namespace NimbleEngine
{
#if WITH_NETCORE
    bool IsNetworkingAvailable() { return true; }

    ENetMode GetNetMode()
    {
        const NetCore::NetSession* Session = NetCore::NetSession::GetActive();
        if(!Session) { return ENetMode::Standalone; }
        switch(Session->GetRole())
        {
            case NetCore::ENetRole::Server: return ENetMode::Server;
            case NetCore::ENetRole::Client: return ENetMode::Client;
            default:                        return ENetMode::Standalone;
        }
    }

    GameCore::NetClientID GetLocalClientId()
    {
        const NetCore::NetSession* Session = NetCore::NetSession::GetActive();
        return Session ? Session->GetLocalClientId() : GameCore::NET_CLIENT_NONE;
    }

    bool IsNetConnected()
    {
        const NetCore::NetSession* Session = NetCore::NetSession::GetActive();
        return Session && Session->IsConnected();
    }

    uint32_t GetNetTimeMs()
    {
        const NetCore::NetSession* Session = NetCore::NetSession::GetActive();
        return Session ? Session->GetTimeMs() : 0u;
    }

    bool IsNetReconnecting()
    {
        const NetCore::NetSession* Session = NetCore::NetSession::GetActive();
        return Session && Session->IsReconnecting();
    }

    bool GetNetStats(const GameCore::NetClientID InClient, NetStats& OutStats)
    {
        const NetCore::NetSession* Session = NetCore::NetSession::GetActive();
        NetCore::NetPeerStats Peer;
        if(!Session || !(Session->IsClient() ? Session->GetServerStats(Peer) : Session->GetClientStats(InClient, Peer))) { return false; }
        OutStats.RoundTripMs           = Peer.RoundTripMs;
        OutStats.RoundTripVarianceMs   = Peer.RoundTripVarianceMs;
        OutStats.PacketLossPercent     = Peer.PacketLossPercent;
        OutStats.BytesSent             = Peer.BytesSent;
        OutStats.BytesReceived         = Peer.BytesReceived;
        OutStats.PacketsSent           = Peer.PacketsSent;
        OutStats.PacketsReceived       = Peer.PacketsReceived;
        OutStats.SendBytesPerSecond    = Peer.SendBytesPerSecond;
        OutStats.ReceiveBytesPerSecond = Peer.ReceiveBytesPerSecond;
        return true;
    }
#else
    bool                  IsNetworkingAvailable() { return false; }
    ENetMode              GetNetMode()            { return ENetMode::Standalone; }
    GameCore::NetClientID GetLocalClientId()      { return GameCore::NET_CLIENT_NONE; }
    bool                  IsNetConnected()        { return false; }
    bool                  GetNetStats(GameCore::NetClientID, NetStats&) { return false; }
    bool                  IsNetReconnecting()     { return false; }
    uint32_t              GetNetTimeMs()          { return 0; }
#endif

    namespace
    {
        bool GDedicatedServer = false;
    }

    void SetDedicatedServerFlag(const bool bInDedicated) { GDedicatedServer = bInDedicated; }

    namespace
    {
        NetCorrectionFn GCorrectionHandler  = nullptr;
        void*           GCorrectionUserData = nullptr;
    }

    void SetNetCorrectionHandler(const NetCorrectionFn InHandler, void* InUserData)
    {
        GCorrectionHandler  = InHandler;
        GCorrectionUserData = InHandler ? InUserData : nullptr;
    }

    NetCorrectionFn GetNetCorrectionHandler(void*& OutUserData)
    {
        OutUserData = GCorrectionUserData;
        return GCorrectionHandler;
    }

    bool IsDedicatedServer() { return GDedicatedServer && IsServer(); }

    bool IsLocallyOwned(const GameCore::NetComponent& InNet)
    {
        return IsClient() ? InNet.OwnerClient == GetLocalClientId() && InNet.OwnerClient != GameCore::NET_CLIENT_NONE
                          : InNet.OwnerClient == GameCore::NET_CLIENT_NONE;
    }
}

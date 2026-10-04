#include "Public/NimbleEngine/NetFacade.h"

#include "Public/GameCore/Components/NetComponent.h"

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
#else
    bool                  IsNetworkingAvailable() { return false; }
    ENetMode              GetNetMode()            { return ENetMode::Standalone; }
    GameCore::NetClientID GetLocalClientId()      { return GameCore::NET_CLIENT_NONE; }
    bool                  IsNetConnected()        { return false; }
#endif

    bool IsLocallyOwned(const GameCore::NetComponent& InNet)
    {
        return IsClient() ? InNet.OwnerClient == GetLocalClientId() && InNet.OwnerClient != GameCore::NET_CLIENT_NONE
                          : InNet.OwnerClient == GameCore::NET_CLIENT_NONE;
    }
}

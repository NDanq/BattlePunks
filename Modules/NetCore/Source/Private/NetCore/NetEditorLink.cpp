#include "Public/NetCore/NetEditorLink.h"

#include <string>
#include <unordered_map>

#include "Private/NetCore/NetProtocol.h"
#include "Private/NetCore/NetTransport.h"

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"

/* STUB(NetCore-Editor) -- see NetEditorLink.h for what this is and is not. */

namespace NetCore
{
namespace
{
    constexpr uint32_t EDITOR_LINK_VERSION = 1;
    constexpr size_t   EDITOR_LINK_MAX_PEERS = 16;
}

struct NetEditorLink::Impl
{
    NetTransport Transport;
    bool         bHosting   = false;
    NetPeerID    HostPeer   = NET_PEER_NONE;
    std::string  UserName   = "Editor";

    /* The host's peers, by the name they said hello with. */
    std::unordered_map<NetPeerID, std::string> Peers;

    CommandFn OnCommand = nullptr;
    void*     UserData  = nullptr;

    void Send(const NetPeerID InPeer, const EngineCore::Archive& Ar)
    {
        if(!Ar.HasError()) { Transport.Send(InPeer, NET_CHANNEL_EDITOR, Ar.GetBytes()); }
    }

    EngineCore::Archive Command(const std::string& InUser, const std::string& InText)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        uint8_t     Type = uint8_t(ENetMessage::EditorCommand);
        std::string User = InUser, Text = InText;
        Ar << Type << User << Text;
        return Ar;
    }

    void Hello(const NetPeerID InPeer)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        uint8_t     Type    = uint8_t(ENetMessage::EditorHello);
        uint32_t    Version = EDITOR_LINK_VERSION;
        std::string User    = UserName;
        Ar << Type << Version << User;
        Send(InPeer, Ar);
    }

    void Received(const NetPeerID InPeer, const std::vector<uint8_t>& InData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InData);
        uint8_t Type = 0;
        Ar << Type;
        switch(ENetMessage(Type))
        {
            case ENetMessage::EditorHello:
            {
                uint32_t    Version = 0;
                std::string User;
                Ar << Version << User;
                if(Ar.HasError() || Version != EDITOR_LINK_VERSION) { Transport.Disconnect(InPeer); return; }
                if(bHosting) { Peers[InPeer] = User; }
                CORE_LOG("Editor link: " + User + " joined")
                break;
            }
            case ENetMessage::EditorCommand:
            {
                std::string User, Text;
                Ar << User << Text;
                if(Ar.HasError()) { Transport.Disconnect(InPeer); return; }

                /* The host relays to everyone else. */
                if(bHosting)
                {
                    for(const auto& Entry : Peers)
                    {
                        if(Entry.first != InPeer) { Send(Entry.first, Command(User, Text)); }
                    }
                }

                CORE_LOG("Editor link: " + User + ": " + Text)
                if(OnCommand) { OnCommand(User.c_str(), Text.c_str(), UserData); }
                break;
            }
            case ENetMessage::EditorPropertyEdit:   // STUB(NetCore-Editor): reserved, nothing sends it yet
            default:
                break;
        }
    }
};

NetEditorLink::NetEditorLink() : P(new Impl()) {}

NetEditorLink::~NetEditorLink()
{
    Leave();
    delete P;
}

void NetEditorLink::SetUserName(const char* InName)
{
    P->UserName = (InName && InName[0] != '\0') ? InName : "Editor";
}

bool NetEditorLink::Host(const uint16_t InPort)
{
    Leave();
    if(!P->Transport.Listen(InPort, EDITOR_LINK_MAX_PEERS, 30000)) { return false; }
    P->bHosting = true;
    CORE_LOG("Editor link: hosting on port " + std::to_string(P->Transport.GetPort()))
    return true;
}

bool NetEditorLink::Join(const char* InAddress, const uint16_t InPort)
{
    Leave();
    if(!P->Transport.Connect(InAddress, InPort, 30000, P->HostPeer)) { return false; }
    CORE_LOG(std::string("Editor link: joining ") + (InAddress ? InAddress : "") + ":" + std::to_string(InPort))
    return true;
}

void NetEditorLink::Leave()
{
    if(!P->Transport.IsOpen()) { return; }
    P->Transport.Close();
    P->Peers.clear();
    P->bHosting = false;
    P->HostPeer = NET_PEER_NONE;
    CORE_LOG("Editor link: closed")
}

bool     NetEditorLink::IsActive() const     { return P->Transport.IsOpen(); }
bool     NetEditorLink::IsHosting() const    { return P->bHosting; }
size_t   NetEditorLink::GetPeerCount() const { return P->Transport.GetPeerCount(); }
uint16_t NetEditorLink::GetPort() const      { return P->Transport.GetPort(); }

void NetEditorLink::Poll()
{
    NetTransportEvent Event;
    while(P->Transport.Service(Event))
    {
        switch(Event.EventType)
        {
            case NetTransportEvent::EType::Connected:
                P->Hello(Event.Peer);
                break;
            case NetTransportEvent::EType::Disconnected:
            {
                const auto Found = P->Peers.find(Event.Peer);
                CORE_LOG("Editor link: " + (Found != P->Peers.end() ? Found->second : std::string("the host")) + " left")
                if(Found != P->Peers.end()) { P->Peers.erase(Found); }
                if(Event.Peer == P->HostPeer) { P->HostPeer = NET_PEER_NONE; }
                break;
            }
            case NetTransportEvent::EType::Received:
                if(Event.Channel == NET_CHANNEL_EDITOR) { P->Received(Event.Peer, Event.Data); }
                break;
        }
    }
    P->Transport.Flush();
}

void NetEditorLink::SendCommand(const char* InText)
{
    if(!P->Transport.IsOpen() || !InText) { return; }
    const EngineCore::Archive Ar = P->Command(P->UserName, InText);
    if(P->bHosting)
    {
        for(const auto& Entry : P->Peers) { P->Send(Entry.first, Ar); }
    }
    else if(P->HostPeer != NET_PEER_NONE)
    {
        P->Send(P->HostPeer, Ar);
    }
}

void NetEditorLink::SetCommandHandler(const CommandFn InFn, void* InUserData)
{
    P->OnCommand = InFn;
    P->UserData  = InUserData;
}
}

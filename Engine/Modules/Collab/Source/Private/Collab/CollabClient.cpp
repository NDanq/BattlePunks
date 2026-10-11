#include "Public/Collab/CollabClient.h"

#include <algorithm>

#include "Private/Collab/CollabTransport.h"
#include "Public/Core/CoreLog.h"

namespace Collab
{
struct CollabClient::Impl
{
    CollabTransport Transport;
    PeerID          Server = PEER_NONE;
    ClientIdentity  Identity;
    std::string     Address;
    bool            bConnected = false;   // the transport's, before any Welcome
    bool            bRejected  = false;   // the Disconnected that follows is expected
    bool            bLeaving   = false;
};

CollabClient::CollabClient()
    : P(new Impl())
{
}

CollabClient::~CollabClient()
{
    P->Transport.Close();
}

bool CollabClient::Connect(const std::string& InHost, const uint16_t InPort, const ClientIdentity& InIdentity, const uint32_t InTimeoutMs)
{
    P->Transport.Close();
    Reset();
    P->Identity = InIdentity;
    P->Address  = InHost + ":" + std::to_string(InPort);
    if(!P->Transport.Connect(InHost.c_str(), InPort, InTimeoutMs, P->Server))
    {
        return false;
    }
    State = EState::Connecting;
    Mine.User = InIdentity.User;
    if(Mine.Head.empty())
    {
        Mine.Head = InIdentity.Head;
    }
    CORE_LOG("[Collab] connecting to " + P->Address + " as " + InIdentity.User)
    return true;
}

void CollabClient::Leave()
{
    if(State == EState::Idle)
    {
        return;
    }
    P->bLeaving = true;
    P->Transport.Disconnect(P->Server);
    P->Transport.Flush();
}

void CollabClient::Reset()
{
    State     = EState::Idle;
    MyId      = 0;
    SessionId = 0;
    SessionEngine.clear();
    LastBuild = BuildStatusMessage();
    Roster.clear();
    Sent      = MemberState();
    bSentOnce = false;
    KnownOverlaps.clear();
    P->bConnected = false;
    P->bRejected  = false;
    P->bLeaving   = false;
}

void CollabClient::Poll()
{
    if(!P->Transport.IsOpen())
    {
        return;
    }

    TransportEvent Event;
    while(P->Transport.Service(Event))
    {
        switch(Event.EventType)
        {
            case TransportEvent::EType::Connected:
            {
                P->bConnected = true;
                HelloMessage Hello;
                Hello.User         = P->Identity.User;
                Hello.ProjectId    = P->Identity.ProjectId;
                Hello.Branch       = P->Identity.Branch;
                Hello.Head         = P->Identity.Head;
                Hello.EngineTree   = P->Identity.EngineTree;
                Hello.bEngineDirty = P->Identity.bEngineDirty;
                Hello.Password     = P->Identity.Password;
                P->Transport.Send(P->Server, Encode(Hello));
                break;
            }
            case TransportEvent::EType::Disconnected:
            {
                const bool bWasJoined    = State == EState::Joined;
                const bool bWasConnected = P->bConnected;
                const bool bExpected     = P->bRejected || P->bLeaving;
                const bool bLeft         = P->bLeaving;
                P->Transport.Close();
                Reset();
                if(P->bRejected || bExpected)
                {
                    if(bLeft)
                    {
                        ClientEvent Out;
                        Out.Type = ClientEvent::EType::Disconnected;
                        Out.Text = "You left the session.";
                        Push(Out);
                    }
                    /* A Rejected was already queued. */
                }
                else if(!bWasConnected)
                {
                    ClientEvent Out;
                    Out.Type = ClientEvent::EType::ConnectFailed;
                    Out.Text = "No answer from " + P->Address + ". Is NimbleEditorServer running, and is UDP open to it?";
                    Push(Out);
                }
                else
                {
                    ClientEvent Out;
                    Out.Type = ClientEvent::EType::Disconnected;
                    Out.Text = bWasJoined ? "Lost the connection to " + P->Address + "." : "The server closed the connection.";
                    Push(Out);
                }
                CORE_LOG("[Collab] disconnected from " + P->Address)
                return;   // the transport is closed
            }
            case TransportEvent::EType::Received:
            {
                switch(PeekType(Event.Data))
                {
                    case EMessage::Welcome:
                    {
                        WelcomeMessage Welcome;
                        if(!Decode(Event.Data, Welcome))
                        {
                            break;
                        }
                        State         = EState::Joined;
                        MyId          = Welcome.YourId;
                        SessionId     = Welcome.SessionId;
                        SessionEngine = Welcome.EngineTree;
                        for(MemberState& Each : Welcome.Roster)
                        {
                            Roster[Each.MemberId] = Each;
                        }
                        CORE_LOG("[Collab] joined session " + std::to_string(SessionId) + " with "
                                 + std::to_string(Roster.size()) + " member(s)")
                        ClientEvent Out;
                        Out.Type = ClientEvent::EType::Joined;
                        Out.MemberId = MyId;
                        Push(Out);
                        SendPresence(true);
                        CheckOverlaps();
                        break;
                    }
                    case EMessage::Reject:
                    {
                        RejectMessage Reject;
                        Decode(Event.Data, Reject);
                        P->bRejected = true;
                        CORE_LOG_WARN("[Collab] refused (" + std::string(GetRejectReasonName(Reject.Reason)) + "): " + Reject.Text)
                        ClientEvent Out;
                        Out.Type   = ClientEvent::EType::Rejected;
                        Out.Reason = Reject.Reason;
                        Out.Text   = Reject.Text;
                        Push(Out);
                        break;
                    }
                    case EMessage::MemberJoined:
                    {
                        MemberJoinedMessage Joined;
                        if(!Decode(Event.Data, Joined))
                        {
                            break;
                        }
                        Roster[Joined.Member.MemberId] = Joined.Member;
                        ClientEvent Out;
                        Out.Type     = ClientEvent::EType::MemberJoined;
                        Out.MemberId = Joined.Member.MemberId;
                        Out.User     = Joined.Member.User;
                        Push(Out);
                        CheckOverlaps();
                        break;
                    }
                    case EMessage::MemberLeft:
                    {
                        MemberLeftMessage Left;
                        if(!Decode(Event.Data, Left))
                        {
                            break;
                        }
                        ClientEvent Out;
                        Out.Type     = ClientEvent::EType::MemberLeft;
                        Out.MemberId = Left.MemberId;
                        Out.Text     = Left.Reason;
                        const auto Found = Roster.find(Left.MemberId);
                        if(Found != Roster.end())
                        {
                            Out.User = Found->second.User;
                            Roster.erase(Found);
                        }
                        Push(Out);
                        CheckOverlaps();
                        break;
                    }
                    case EMessage::Presence:
                    {
                        PresenceMessage Presence;
                        if(!Decode(Event.Data, Presence))
                        {
                            break;
                        }
                        const auto Found = Roster.find(Presence.MemberId);
                        if(Found == Roster.end())
                        {
                            break;
                        }
                        MemberState& Member = Found->second;
                        Member.Head      = Presence.Head;
                        Member.OpenLevel = Presence.OpenLevel;
                        Member.Play      = Presence.Play;
                        if(Presence.bFullDirty)
                        {
                            Member.Dirty = Presence.DirtyAdded;
                            SortUnique(Member.Dirty);
                        }
                        else
                        {
                            ApplyDiff(Member.Dirty, Presence.DirtyAdded, Presence.DirtyRemoved);
                        }
                        ClientEvent Out;
                        Out.Type     = ClientEvent::EType::MemberChanged;
                        Out.MemberId = Member.MemberId;
                        Out.User     = Member.User;
                        Push(Out);
                        CheckOverlaps();
                        break;
                    }
                    case EMessage::BranchAdvanced:
                    {
                        BranchAdvancedMessage Advanced;
                        if(!Decode(Event.Data, Advanced))
                        {
                            break;
                        }
                        ClientEvent Out;
                        Out.Type     = ClientEvent::EType::BranchAdvanced;
                        Out.MemberId = Advanced.PusherId;
                        Out.User     = Advanced.Pusher;
                        Out.Move     = std::move(Advanced.Move);
                        Push(std::move(Out));
                        break;
                    }
                    case EMessage::BuildStatus:
                    {
                        ClientEvent Out;
                        if(!Decode(Event.Data, Out.Build))
                        {
                            break;
                        }
                        LastBuild = Out.Build;
                        Out.Type = ClientEvent::EType::BuildStatus;
                        Push(std::move(Out));
                        break;
                    }
                    case EMessage::ValidationReport:
                    {
                        ClientEvent Out;
                        if(!Decode(Event.Data, Out.Report))
                        {
                            break;
                        }
                        Out.Type = ClientEvent::EType::Validation;
                        Push(std::move(Out));
                        break;
                    }
                    case EMessage::Notify:
                    {
                        NotifyMessage Notify;
                        if(!Decode(Event.Data, Notify))
                        {
                            break;
                        }
                        ClientEvent Out;
                        Out.Type     = ClientEvent::EType::Notified;
                        Out.Severity = Notify.Severity;
                        Out.Title    = Notify.Title;
                        Out.Text     = Notify.Body;
                        Push(Out);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
        }
    }
    P->Transport.Flush();
}

bool CollabClient::NextEvent(ClientEvent& OutEvent)
{
    if(Events.empty())
    {
        return false;
    }
    OutEvent = std::move(Events.front());
    Events.pop_front();
    return true;
}

void CollabClient::SetPresence(const std::string& InHead, const std::string& InOpenLevel, const EPlayState InPlay,
                               std::vector<std::string> InDirty)
{
    SortUnique(InDirty);
    Mine.Head      = InHead;
    Mine.OpenLevel = InOpenLevel;
    Mine.Play      = InPlay;
    Mine.Dirty     = std::move(InDirty);
    if(State == EState::Joined)
    {
        SendPresence(false);
        CheckOverlaps();
    }
}

void CollabClient::SendPresence(const bool bInFull)
{
    PresenceMessage Presence;
    Presence.Head      = Mine.Head;
    Presence.OpenLevel = Mine.OpenLevel;
    Presence.Play      = Mine.Play;
    Presence.bFullDirty = bInFull || !bSentOnce;
    if(Presence.bFullDirty)
    {
        Presence.DirtyAdded = Mine.Dirty;
    }
    else
    {
        DiffSorted(Sent.Dirty, Mine.Dirty, Presence.DirtyAdded, Presence.DirtyRemoved);
        if(Presence.DirtyAdded.empty() && Presence.DirtyRemoved.empty() && Sent.Head == Mine.Head
           && Sent.OpenLevel == Mine.OpenLevel && Sent.Play == Mine.Play)
        {
            return;   // nothing new
        }
    }
    P->Transport.Send(P->Server, Encode(Presence));
    P->Transport.Flush();
    Sent      = Mine;
    bSentOnce = true;

    /* Our own roster entry follows what we sent. */
    const auto Self = Roster.find(MyId);
    if(Self != Roster.end())
    {
        Self->second.Head      = Mine.Head;
        Self->second.OpenLevel = Mine.OpenLevel;
        Self->second.Play      = Mine.Play;
        Self->second.Dirty     = Mine.Dirty;
    }
}

void CollabClient::AnnouncePush(const BranchMove& InMove)
{
    if(State != EState::Joined)
    {
        return;
    }
    PushedMessage Pushed;
    Pushed.Move = InMove;
    P->Transport.Send(P->Server, Encode(Pushed));
    P->Transport.Flush();
}

std::vector<std::string> CollabClient::WhoHasDirty(const std::string& InPath) const
{
    std::vector<std::string> Users;
    for(const auto& Each : Roster)
    {
        if(Each.first != MyId && std::binary_search(Each.second.Dirty.begin(), Each.second.Dirty.end(), InPath))
        {
            Users.push_back(Each.second.User);
        }
    }
    return Users;
}

std::map<std::string, std::vector<std::string>> CollabClient::GetOthersDirty() const
{
    std::map<std::string, std::vector<std::string>> Out;
    for(const auto& Each : Roster)
    {
        if(Each.first == MyId)
        {
            continue;
        }
        for(const std::string& Path : Each.second.Dirty)
        {
            Out[Path].push_back(Each.second.User);
        }
    }
    return Out;
}

void CollabClient::CheckOverlaps()
{
    std::vector<std::string> Current;
    for(const auto& Each : Roster)
    {
        if(Each.first == MyId)
        {
            continue;
        }
        for(const std::string& Path : Each.second.Dirty)
        {
            if(std::binary_search(Mine.Dirty.begin(), Mine.Dirty.end(), Path))
            {
                Current.push_back(Path + "\n" + Each.second.User);
            }
        }
    }
    SortUnique(Current);

    std::vector<std::string> New, Gone;
    DiffSorted(KnownOverlaps, Current, New, Gone);
    for(const std::string& Pair : New)
    {
        const size_t Break = Pair.find('\n');
        ClientEvent Out;
        Out.Type = ClientEvent::EType::Overlap;
        Out.Path = Pair.substr(0, Break);
        Out.User = Pair.substr(Break + 1);
        Push(Out);
    }
    KnownOverlaps.swap(Current);
}
}

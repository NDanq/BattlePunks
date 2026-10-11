#include "Public/Collab/CollabServer.h"

#include <chrono>
#include <map>
#include <unordered_map>

#include "Private/Collab/CollabTransport.h"
#include "Public/Core/CoreLog.h"

namespace Collab
{
namespace
{
    using Clock = std::chrono::steady_clock;

    std::string Short(const std::string& InOid)
    {
        return InOid.size() > 10 ? InOid.substr(0, 10) : InOid;
    }
}

struct CollabServer::Impl
{
    struct Peer
    {
        Clock::time_point Connected;
        uint32_t          SessionId = 0;   // 0: no Hello yet
        uint32_t          MemberId  = 0;
        bool              bRefused  = false;   // told why; waiting for the disconnect
    };

    struct Member
    {
        PeerID      Peer = PEER_NONE;
        MemberState State;
    };

    struct Session
    {
        uint32_t    Id = 0;
        std::string ProjectId;
        std::string Branch;
        std::string EngineTree;
        std::string Tip;   // the branch's last pushed commit, as editors announced it
        std::map<uint32_t, Member> Members;
    };

    ServerConfig    Config;
    CollabTransport Transport;
    std::unordered_map<PeerID, Peer> Peers;
    std::map<uint32_t, Session>      Sessions;
    uint32_t NextSessionId = 1;
    uint32_t NextMemberId  = 1;

    template<typename TMessage>
    void Send(const PeerID InPeer, TMessage& InMessage)
    {
        Transport.Send(InPeer, Encode(InMessage));
    }

    /* To everyone in InSession but InExcept. */
    template<typename TMessage>
    void Broadcast(const Session& InSession, TMessage& InMessage, const uint32_t InExcept)
    {
        const std::vector<uint8_t> Bytes = Encode(InMessage);
        for(const auto& Each : InSession.Members)
        {
            if(Each.first != InExcept)
            {
                Transport.Send(Each.second.Peer, Bytes);
            }
        }
    }

    void Refuse(const PeerID InPeer, const ERejectReason InReason, const std::string& InText)
    {
        CORE_LOG_WARN("[Collab] refused " + Transport.DescribePeer(InPeer) + " (" + GetRejectReasonName(InReason) + "): " + InText)
        RejectMessage Reject;
        Reject.Reason = InReason;
        Reject.Text   = InText;
        Send(InPeer, Reject);
        Transport.DisconnectLater(InPeer);
        const auto Found = Peers.find(InPeer);
        if(Found != Peers.end())
        {
            Found->second.bRefused = true;
        }
    }

    Session* FindSession(const std::string& InProjectId, const std::string& InBranch)
    {
        for(auto& Each : Sessions)
        {
            if(Each.second.ProjectId == InProjectId && Each.second.Branch == InBranch)
            {
                return &Each.second;
            }
        }
        return nullptr;
    }

    void OnHello(const PeerID InPeer, Peer& InState, const std::vector<uint8_t>& InBytes)
    {
        HelloMessage Hello;
        if(!Decode(InBytes, Hello))
        {
            /* An older or newer Hello whose layout we can't read is still most
             * likely a protocol mismatch: say so if the version field made it. */
            Refuse(InPeer, ERejectReason::BadHello, "The server couldn't read this editor's hello. Is it the same engine version?");
            return;
        }
        if(Hello.Protocol != ProtocolVersion)
        {
            Refuse(InPeer, ERejectReason::Protocol,
                   "This editor speaks Collab protocol " + std::to_string(Hello.Protocol) + "; the server speaks "
                   + std::to_string(ProtocolVersion) + ". Update the older one.");
            return;
        }
        if(!Config.Password.empty() && !ConstantTimeEquals(Hello.Password, Config.Password))
        {
            Refuse(InPeer, ERejectReason::Password, Hello.Password.empty() ? "This server needs a session password."
                                                                           : "Wrong session password.");
            return;
        }
        if(Hello.User.empty() || Hello.ProjectId.empty() || Hello.Branch.empty() || Hello.EngineTree.empty())
        {
            Refuse(InPeer, ERejectReason::BadHello, "The hello is missing a user, project, branch or engine identity.");
            return;
        }
        if(Hello.bEngineDirty)
        {
            Refuse(InPeer, ERejectReason::EngineDirty,
                   "You have uncommitted changes under Engine/. Engine changes aren't supported during a session: "
                   "commit or set them aside, then connect again.");
            return;
        }

        Session* Joined = FindSession(Hello.ProjectId, Hello.Branch);
        if(Joined && Joined->EngineTree != Hello.EngineTree)
        {
            Refuse(InPeer, ERejectReason::EngineMismatch,
                   "Your engine (" + Short(Hello.EngineTree) + ") differs from the session's (" + Short(Joined->EngineTree)
                   + "). Pull the session branch and rebuild the engine, then connect again.");
            return;
        }
        if(!Joined)
        {
            Session New;
            New.Id         = NextSessionId++;
            New.ProjectId  = Hello.ProjectId;
            New.Branch     = Hello.Branch;
            New.EngineTree = Hello.EngineTree;
            Joined = &(Sessions[New.Id] = New);
            CORE_LOG("[Collab] session " + std::to_string(Joined->Id) + " started: " + Joined->ProjectId + " on " + Joined->Branch
                     + ", engine " + Short(Joined->EngineTree))
        }

        Member New;
        New.Peer           = InPeer;
        New.State.MemberId = NextMemberId++;
        New.State.User     = Hello.User;
        New.State.Head     = Hello.Head;
        InState.SessionId  = Joined->Id;
        InState.MemberId   = New.State.MemberId;
        Joined->Members[New.State.MemberId] = New;

        WelcomeMessage Welcome;
        Welcome.SessionId  = Joined->Id;
        Welcome.YourId     = New.State.MemberId;
        Welcome.EngineTree = Joined->EngineTree;
        for(const auto& Each : Joined->Members)
        {
            Welcome.Roster.push_back(Each.second.State);
        }
        Send(InPeer, Welcome);

        MemberJoinedMessage Joining;
        Joining.Member = New.State;
        Broadcast(*Joined, Joining, New.State.MemberId);

        CORE_LOG("[Collab] " + Hello.User + " (" + Transport.DescribePeer(InPeer) + ") joined session " + std::to_string(Joined->Id)
                 + "; " + std::to_string(Joined->Members.size()) + " in it")
    }

    void OnPresence(const Peer& InState, const std::vector<uint8_t>& InBytes)
    {
        const auto FoundSession = Sessions.find(InState.SessionId);
        if(FoundSession == Sessions.end())
        {
            return;
        }
        Session& In = FoundSession->second;
        const auto FoundMember = In.Members.find(InState.MemberId);
        if(FoundMember == In.Members.end())
        {
            return;
        }

        PresenceMessage Presence;
        if(!Decode(InBytes, Presence))
        {
            CORE_LOG_WARN("[Collab] an unreadable presence from " + FoundMember->second.State.User)
            return;
        }
        MemberState& State = FoundMember->second.State;
        State.Head      = Presence.Head;
        State.OpenLevel = Presence.OpenLevel;
        State.Play      = Presence.Play;
        if(Presence.bFullDirty)
        {
            State.Dirty = Presence.DirtyAdded;
            SortUnique(State.Dirty);
        }
        else
        {
            ApplyDiff(State.Dirty, Presence.DirtyAdded, Presence.DirtyRemoved);
        }

        Presence.MemberId = State.MemberId;
        Broadcast(In, Presence, State.MemberId);
    }

    void OnPushed(const PeerID InPeer, const Peer& InState, const std::vector<uint8_t>& InBytes)
    {
        const auto FoundSession = Sessions.find(InState.SessionId);
        if(FoundSession == Sessions.end())
        {
            return;
        }
        Session& In = FoundSession->second;
        const auto FoundMember = In.Members.find(InState.MemberId);
        if(FoundMember == In.Members.end())
        {
            return;
        }
        PushedMessage Pushed;
        if(!Decode(InBytes, Pushed))
        {
            CORE_LOG_WARN("[Collab] an unreadable push from " + FoundMember->second.State.User)
            return;
        }
        const std::string& User = FoundMember->second.State.User;
        if(Pushed.Move.Branch != In.Branch)
        {
            /* Another branch is nobody else's business here; say so to the pusher. */
            NotifyMessage Aside;
            Aside.Severity = ESeverity::Info;
            Aside.Title    = "Pushed " + Pushed.Move.Branch + ", not the session branch";
            Aside.Body     = "The others work on " + In.Branch + " and weren't told.";
            Send(InPeer, Aside);
            return;
        }
        if(Pushed.Move.NewOid.empty() || Pushed.Move.NewOid == In.Tip)
        {
            return;   // nothing new
        }
        if(Pushed.Move.OldOid.empty())
        {
            Pushed.Move.OldOid = In.Tip;
        }
        In.Tip = Pushed.Move.NewOid;
        CORE_LOG("[Collab] " + User + " pushed " + In.Branch + " to " + Short(In.Tip) + ": "
                 + std::to_string(Pushed.Move.Commits.size()) + " commit(s), " + std::to_string(Pushed.Move.Paths.size()) + " path(s)")

        BranchAdvancedMessage Advanced;
        Advanced.PusherId = InState.MemberId;
        Advanced.Pusher   = User;
        Advanced.Move     = std::move(Pushed.Move);
        Broadcast(In, Advanced, InState.MemberId);
    }

    void Drop(const PeerID InPeer, const std::string& InReason)
    {
        const auto Found = Peers.find(InPeer);
        if(Found == Peers.end())
        {
            return;
        }
        const Peer Gone = Found->second;
        Peers.erase(Found);

        const auto FoundSession = Sessions.find(Gone.SessionId);
        if(FoundSession == Sessions.end())
        {
            return;
        }
        Session& In = FoundSession->second;
        const auto FoundMember = In.Members.find(Gone.MemberId);
        if(FoundMember == In.Members.end())
        {
            return;
        }
        const std::string User = FoundMember->second.State.User;
        In.Members.erase(FoundMember);
        CORE_LOG("[Collab] " + User + " left session " + std::to_string(In.Id) + " (" + InReason + "); "
                 + std::to_string(In.Members.size()) + " in it")

        MemberLeftMessage Left;
        Left.MemberId = Gone.MemberId;
        Left.Reason   = InReason;
        Broadcast(In, Left, 0);

        if(In.Members.empty())
        {
            CORE_LOG("[Collab] session " + std::to_string(In.Id) + " ended")
            Sessions.erase(FoundSession);
        }
    }
};

CollabServer::CollabServer()
    : P(new Impl())
{
}

CollabServer::~CollabServer()
{
    Stop();
}

bool CollabServer::Start(const ServerConfig& InConfig)
{
    Stop();
    P->Config = InConfig;
    if(!P->Transport.Listen(InConfig.Port, InConfig.MaxPeers, InConfig.TimeoutMs))
    {
        return false;
    }
    CORE_LOG("[Collab] listening on UDP port " + std::to_string(P->Transport.GetPort())
             + (InConfig.Password.empty() ? std::string(", no password") : std::string(", with a password")))
    return true;
}

void CollabServer::Stop()
{
    if(P->Transport.IsOpen())
    {
        CORE_LOG("[Collab] stopping")
    }
    P->Transport.Close();
    P->Peers.clear();
    P->Sessions.clear();
}

bool CollabServer::IsRunning() const
{
    return P->Transport.IsOpen();
}

uint16_t CollabServer::GetPort() const
{
    return P->Transport.GetPort();
}

void CollabServer::Poll()
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
                Impl::Peer New;
                New.Connected = Clock::now();
                P->Peers[Event.Peer] = New;
                break;
            }
            case TransportEvent::EType::Disconnected:
                P->Drop(Event.Peer, "disconnected");
                break;
            case TransportEvent::EType::Received:
            {
                const auto Found = P->Peers.find(Event.Peer);
                if(Found == P->Peers.end())
                {
                    break;
                }
                const EMessage Type = PeekType(Event.Data);
                if(Found->second.bRefused)
                {
                    break;   // already told why
                }
                if(Found->second.SessionId == 0)
                {
                    if(Type == EMessage::Hello)
                    {
                        P->OnHello(Event.Peer, Found->second, Event.Data);
                    }
                    else
                    {
                        P->Refuse(Event.Peer, ERejectReason::BadHello, "Say hello first.");
                    }
                }
                else if(Type == EMessage::Presence)
                {
                    P->OnPresence(Found->second, Event.Data);
                }
                else if(Type == EMessage::Pushed)
                {
                    P->OnPushed(Event.Peer, Found->second, Event.Data);
                }
                break;
            }
        }
    }

    /* A connection that never says hello isn't kept. */
    const Clock::time_point Now = Clock::now();
    for(auto& Each : P->Peers)
    {
        if(Each.second.SessionId == 0 && !Each.second.bRefused
           && Now - Each.second.Connected > std::chrono::milliseconds(P->Config.HelloTimeoutMs))
        {
            P->Refuse(Each.first, ERejectReason::BadHello, "No hello in time.");
        }
    }

    P->Transport.Flush();
}

std::vector<SessionSummary> CollabServer::GetSessions() const
{
    std::vector<SessionSummary> Out;
    for(const auto& Each : P->Sessions)
    {
        SessionSummary Summary;
        Summary.SessionId  = Each.second.Id;
        Summary.ProjectId  = Each.second.ProjectId;
        Summary.Branch     = Each.second.Branch;
        Summary.EngineTree = Each.second.EngineTree;
        Summary.Tip        = Each.second.Tip;
        for(const auto& Member : Each.second.Members)
        {
            Summary.Members.push_back(Member.second.State);
        }
        Out.push_back(std::move(Summary));
    }
    return Out;
}

size_t CollabServer::GetMemberCount() const
{
    size_t Count = 0;
    for(const auto& Each : P->Sessions)
    {
        Count += Each.second.Members.size();
    }
    return Count;
}
}

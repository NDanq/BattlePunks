#include "Public/Collab/CollabServer.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <unordered_map>

#include "Private/Collab/CollabProcess.h"
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

    /* 128 random bits, hex: a worker's key. */
    std::string RandomKey()
    {
        std::random_device Device;
        static const char Hex[] = "0123456789abcdef";
        std::string Key;
        for(int Index = 0; Index < 32; ++Index)
        {
            Key += Hex[Device() & 15u];
        }
        return Key;
    }

    /* A folder name from free text: letters, digits, '-', '_'. */
    std::string FolderName(const std::string& InText)
    {
        std::string Out;
        for(const char Each : InText)
        {
            Out += std::isalnum(static_cast<unsigned char>(Each)) || Each == '-' || Each == '_' ? Each : '-';
        }
        return Out;
    }

    bool SameUser(const std::string& InA, const std::string& InB)
    {
        if(InA.size() != InB.size())
        {
            return false;
        }
        for(size_t Index = 0; Index < InA.size(); ++Index)
        {
            if(std::tolower(static_cast<unsigned char>(InA[Index])) != std::tolower(static_cast<unsigned char>(InB[Index])))
            {
                return false;
            }
        }
        return true;
    }
}

const char* GetDefaultWorkerPreset()
{
#if defined(_WIN32)
    return "server-relwithdebinfo-msvc";
#else
    return "server-relwithdebinfo-clang";
#endif
}

struct CollabServer::Impl
{
    struct Peer
    {
        Clock::time_point Connected;
        uint32_t          SessionId = 0;   // 0: no Hello yet
        uint32_t          MemberId  = 0;
        bool              bRefused  = false;   // told why; waiting for the disconnect
        bool              bWorker   = false;   // a session's content worker, not an editor
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
        std::string Tip;   // the branch's last pushed commit, as editors or the worker announced it
        std::map<uint32_t, Member> Members;

        /* The content worker (ContentWorker.h), when the server has a checkout. */
        std::string        WorkerKey;
        PeerID             WorkerPeer = PEER_NONE;
        ChildProcess       Worker;
        BuildStatusMessage LastBuild;   // State Idle until the worker reports
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

    // --- The content worker ---------------------------------------------------------

    void StartWorker(Session& InSession)
    {
        if(Config.RepoPath.empty())
        {
            return;
        }
        namespace fs = std::filesystem;
        const std::string Module = InSession.ProjectId.substr(0, InSession.ProjectId.find('@'));
        std::error_code Error;
        const fs::path Data = fs::absolute(fs::u8path(Config.DataDir.empty() ? std::string("Saved/CollabWorkers") : Config.DataDir), Error);

        WorkerConfig Worker;
        Worker.ServerHost      = "127.0.0.1";
        Worker.ServerPort      = Transport.GetPort();
        Worker.SessionId       = InSession.Id;
        Worker.Key             = RandomKey();
        Worker.RepoPath        = fs::absolute(fs::u8path(Config.RepoPath), Error).generic_u8string();
        /* One per game and branch, kept between sessions: the next build is incremental. */
        Worker.WorktreeDir     = (Data / "Worktrees" / (FolderName(Module) + "-" + FolderName(InSession.Branch))).generic_u8string();
        Worker.Branch          = InSession.Branch;
        Worker.Module          = Module;
        Worker.Preset          = Config.WorkerPreset.empty() ? std::string(GetDefaultWorkerPreset()) : Config.WorkerPreset;
        Worker.CMake           = Config.WorkerCMake;
        Worker.FetchIntervalMs = Config.WorkerFetchIntervalMs;
        InSession.WorkerKey    = Worker.Key;

        if(Config.LaunchWorker)
        {
            Config.LaunchWorker(Worker);
            CORE_LOG("[Collab] session " + std::to_string(InSession.Id) + ": content worker started (in-process)")
            return;
        }

        std::string Executable = Config.WorkerExecutable;
        if(Executable.empty())
        {
#if defined(_WIN32)
            Executable = GetExecutableDir() + "/NimbleContentWorker.exe";
#else
            Executable = GetExecutableDir() + "/NimbleContentWorker";
#endif
        }
        InSession.Worker = SpawnProcess({ Executable,
                                          "--connect", Worker.ServerHost + ":" + std::to_string(Worker.ServerPort),
                                          "--session", std::to_string(Worker.SessionId),
                                          "--key", Worker.Key,
                                          "--repo", Worker.RepoPath,
                                          "--worktree", Worker.WorktreeDir,
                                          "--branch", Worker.Branch,
                                          "--module", Worker.Module,
                                          "--preset", Worker.Preset,
                                          "--fetch-interval", std::to_string(Worker.FetchIntervalMs),
                                          "--cmake", Worker.CMake.empty() ? std::string("auto") : Worker.CMake,
                                          "--logs", (Data / "Logs").generic_u8string() });
        if(InSession.Worker.IsValid())
        {
            CORE_LOG("[Collab] session " + std::to_string(InSession.Id) + ": content worker started, worktree " + Worker.WorktreeDir)
        }
        else
        {
            CORE_LOG_ERROR("[Collab] session " + std::to_string(InSession.Id) + ": couldn't start " + Executable)
        }
    }

    void StopWorker(Session& InSession)
    {
        if(InSession.WorkerPeer != PEER_NONE)
        {
            Transport.Disconnect(InSession.WorkerPeer);
            InSession.WorkerPeer = PEER_NONE;
        }
        if(Config.StopWorker && !InSession.WorkerKey.empty())
        {
            Config.StopWorker(InSession.Id);
        }
        if(InSession.Worker.IsValid())
        {
            KillProcess(InSession.Worker);
            CORE_LOG("[Collab] session " + std::to_string(InSession.Id) + ": content worker stopped")
        }
        InSession.WorkerKey.clear();
    }

    void OnWorkerHello(const PeerID InPeer, Peer& InState, const std::vector<uint8_t>& InBytes)
    {
        WorkerHelloMessage Hello;
        const auto Found = Decode(InBytes, Hello) ? Sessions.find(Hello.SessionId) : Sessions.end();
        if(Found == Sessions.end() || Found->second.WorkerKey.empty() || !ConstantTimeEquals(Hello.Key, Found->second.WorkerKey)
           || Found->second.WorkerPeer != PEER_NONE)
        {
            Refuse(InPeer, ERejectReason::BadHello, "Not this server's worker.");
            return;
        }
        InState.bWorker   = true;
        InState.SessionId = Hello.SessionId;
        Found->second.WorkerPeer = InPeer;
        CORE_LOG("[Collab] session " + std::to_string(Hello.SessionId) + ": content worker connected")
    }

    /* A move of the session branch, announced by an editor (InPusherId) or
     * found by the worker (0). */
    void ApplyPush(Session& In, BranchMove InMove, const uint32_t InPusherId, std::string InPusher)
    {
        if(InMove.NewOid.empty() || InMove.NewOid == In.Tip)
        {
            return;   // nothing new
        }
        if(InMove.OldOid.empty())
        {
            InMove.OldOid = In.Tip;
        }
        In.Tip = InMove.NewOid;
        if(InPusher.empty())
        {
            /* Found by the worker: its commits' authors. */
            for(const CommitInfo& Each : InMove.Commits)
            {
                if(InPusher.find(Each.Author) == std::string::npos)
                {
                    InPusher += (InPusher.empty() ? "" : " and ") + Each.Author;
                }
            }
            if(InPusher.empty())
            {
                InPusher = "Someone";
            }
        }
        CORE_LOG("[Collab] " + InPusher + " pushed " + In.Branch + " to " + Short(In.Tip) + ": "
                 + std::to_string(InMove.Commits.size()) + " commit(s), " + std::to_string(InMove.Paths.size()) + " path(s)"
                 + (InPusherId == 0 ? std::string(" (found by the worker)") : std::string()))

        BranchAdvancedMessage Advanced;
        Advanced.PusherId = InPusherId;
        Advanced.Pusher   = InPusher;
        Advanced.Move     = std::move(InMove);
        Broadcast(In, Advanced, InPusherId);
        /* An editor's push: the worker needn't wait for its next fetch. */
        if(InPusherId != 0 && In.WorkerPeer != PEER_NONE)
        {
            Send(In.WorkerPeer, Advanced);
        }
    }

    void OnWorkerMessage(const Peer& InState, const EMessage InType, const std::vector<uint8_t>& InBytes)
    {
        const auto FoundSession = Sessions.find(InState.SessionId);
        if(FoundSession == Sessions.end())
        {
            return;
        }
        Session& In = FoundSession->second;
        switch(InType)
        {
            case EMessage::Pushed:
            {
                PushedMessage Pushed;
                if(Decode(InBytes, Pushed) && Pushed.Move.Branch == In.Branch)
                {
                    ApplyPush(In, std::move(Pushed.Move), 0, std::string());
                }
                break;
            }
            case EMessage::BuildStatus:
            {
                BuildStatusMessage Status;
                if(!Decode(InBytes, Status))
                {
                    break;
                }
                CORE_LOG("[Collab] session " + std::to_string(In.Id) + ": " + Short(Status.Oid) + " " + GetBuildStateName(Status.State)
                         + (Status.FirstError.empty() ? std::string() : ": " + Status.FirstError))
                In.LastBuild = Status;
                Broadcast(In, Status, 0);
                break;
            }
            case EMessage::ValidationReport:
            {
                ValidationReportMessage Report;
                if(!Decode(InBytes, Report))
                {
                    break;
                }
                /* To whoever made it, if they're here; else to everyone. */
                bool bSent = false;
                for(const auto& Each : In.Members)
                {
                    if(SameUser(Each.second.State.User, Report.Author))
                    {
                        Send(Each.second.Peer, Report);
                        bSent = true;
                    }
                }
                if(!bSent)
                {
                    Broadcast(In, Report, 0);
                }
                CORE_LOG("[Collab] session " + std::to_string(In.Id) + ": " + Short(Report.Oid) + " content: "
                         + std::to_string(Report.Problems.size()) + " problem(s) in " + std::to_string(Report.Checked) + " file(s)")
                break;
            }
            default:
                break;
        }
    }

    // --- Editors -----------------------------------------------------------------------

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
        bool bNewSession = false;
        if(!Joined)
        {
            Session New;
            New.Id         = NextSessionId++;
            New.ProjectId  = Hello.ProjectId;
            New.Branch     = Hello.Branch;
            New.EngineTree = Hello.EngineTree;
            Joined = &(Sessions[New.Id] = New);
            bNewSession = true;
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

        /* Where the session's build stands, for a late joiner. */
        if(Joined->LastBuild.State != EBuildState::Idle)
        {
            Send(InPeer, Joined->LastBuild);
        }

        MemberJoinedMessage Joining;
        Joining.Member = New.State;
        Broadcast(*Joined, Joining, New.State.MemberId);

        CORE_LOG("[Collab] " + Hello.User + " (" + Transport.DescribePeer(InPeer) + ") joined session " + std::to_string(Joined->Id)
                 + "; " + std::to_string(Joined->Members.size()) + " in it")

        if(bNewSession)
        {
            StartWorker(*Joined);
        }
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
        ApplyPush(In, std::move(Pushed.Move), InState.MemberId, FoundMember->second.State.User);
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
        if(Gone.bWorker)
        {
            if(In.WorkerPeer == InPeer)
            {
                In.WorkerPeer = PEER_NONE;
                CORE_LOG_WARN("[Collab] session " + std::to_string(In.Id) + ": content worker disconnected")
            }
            return;
        }
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
            StopWorker(In);
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
             + (InConfig.Password.empty() ? std::string(", no password") : std::string(", with a password"))
             + (InConfig.RepoPath.empty() ? std::string(", no content worker (no --repo)")
                                          : ", content workers on " + InConfig.RepoPath))
    return true;
}

void CollabServer::Stop()
{
    if(P->Transport.IsOpen())
    {
        CORE_LOG("[Collab] stopping")
    }
    for(auto& Each : P->Sessions)
    {
        P->StopWorker(Each.second);
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
                if(Found->second.bWorker)
                {
                    P->OnWorkerMessage(Found->second, Type, Event.Data);
                }
                else if(Found->second.SessionId == 0)
                {
                    if(Type == EMessage::Hello)
                    {
                        P->OnHello(Event.Peer, Found->second, Event.Data);
                    }
                    else if(Type == EMessage::WorkerHello)
                    {
                        P->OnWorkerHello(Event.Peer, Found->second, Event.Data);
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
        Summary.LastBuild  = Each.second.LastBuild;
        Summary.bWorkerConnected = Each.second.WorkerPeer != PEER_NONE;
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

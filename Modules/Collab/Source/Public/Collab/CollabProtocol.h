#ifndef COLLAB_COLLABPROTOCOL_H
#define COLLAB_COLLABPROTOCOL_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Public/Collab/CollabAPI.h"
#include "Public/Core/CoreArchive.h"

/*
 * The Collab protocol, v1 (Modules/Editor/COLLABORATION.md, "Protocol, v1").
 *
 * A message is one Archive: a "CLAB" chunk whose version is the message's own
 * layout version, holding a u16 EMessage and the payload. A reader steps over
 * fields a newer sender appended (Archive's chunk rule), so a field may be
 * added at the end of a payload without bumping ProtocolVersion; reordering or
 * removing one bumps it.
 *
 * SESSIONS. A session is a project (its ProjectId) on a branch. The first
 * editor to join sets the session's engine identity; later ones must match it
 * and have no uncommitted engine changes -- how "engine changes are not
 * supported during a session" is enforced.
 *
 * In use: Hello / Welcome / Reject, Presence, MemberJoined / MemberLeft,
 * Notify, (P3) Pushed / BranchAdvanced, (P4) the worker's, and (P5) Play*.
 */
namespace Collab
{
    constexpr uint16_t DefaultPort     = 7788;
    constexpr uint32_t ProtocolVersion = 1;

    /* What a connection may hold, so a bad length can't allocate gigabytes. */
    constexpr uint32_t MaxListEntries  = 100000;

    enum class EMessage : uint16_t
    {
        None = 0,

        Hello = 1,       // C->S
        Welcome,         // S->C
        Reject,          // S->C, then the server disconnects
        Presence,        // C->S, then S->others with the member's id
        MemberJoined,    // S->others
        MemberLeft,      // S->others
        Notify,          // S->C: a server message, shown as is

        Pushed = 16,     // C->S: this editor pushed the session branch
        BranchAdvanced,  // S->others: the session branch moved

        BuildStatus,     // worker->S->all: the session branch's build
        ValidationReport,// worker->S->the author: content problems

        PlayRequest,     // C->S: start a shared run on a level (or stop it)
        PlayInvite,      // S->all: a run is up; join it
        PlayEnded,       // S->all: the run is over, and why

        WorkerHello = 32,// worker->S: the session's content worker, by its key
        WorkerPlay,      // S->worker: start (or stop) the game server
        WorkerPlayState  // worker->S: it's up on a port, or it isn't, and why
    };

    enum class EPlayState : uint8_t
    {
        Editing = 0,
        Playing,         // Play-in-Editor, on its own
        PlayingTogether  // P5: in a shared run
    };

    enum class ERejectReason : uint8_t
    {
        BadHello = 0,       // not a Hello, or unreadable
        Protocol,           // a different ProtocolVersion
        Password,           // wrong or missing session password
        EngineMismatch,     // a different engine from the session's
        EngineDirty,        // uncommitted engine changes
        Full                // the server's limit
    };

    enum class ESeverity : uint8_t { Info = 0, Success, Warning, Error };

    COLLAB_API const char* GetRejectReasonName(ERejectReason InReason);

    // === Payloads ===========================================================

    struct COLLAB_API HelloMessage
    {
        static constexpr EMessage Type = EMessage::Hello;

        uint32_t    Protocol = ProtocolVersion;
        std::string User;
        std::string ProjectId;     // MakeProjectId
        std::string Branch;        // the session branch
        std::string Head;          // HEAD's commit
        std::string EngineTree;    // `git rev-parse HEAD:Engine`
        bool        bEngineDirty = false;   // `git diff --quiet HEAD -- Engine` said otherwise
        std::string Password;      // empty without one

        void Serialize(EngineCore::Archive& Ar);
    };

    /* One editor in a session, as everyone else sees it. */
    struct COLLAB_API MemberState
    {
        uint32_t    MemberId = 0;
        std::string User;
        std::string Head;
        std::string OpenLevel;     // repository-relative; empty if none
        EPlayState  Play = EPlayState::Editing;
        std::vector<std::string> Dirty;   // repository-relative paths with changes not yet pushed, sorted

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API WelcomeMessage
    {
        static constexpr EMessage Type = EMessage::Welcome;

        uint32_t                 SessionId = 0;
        uint32_t                 YourId    = 0;
        std::string              EngineTree;   // the session's
        std::vector<MemberState> Roster;       // you included

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API RejectMessage
    {
        static constexpr EMessage Type = EMessage::Reject;

        ERejectReason Reason = ERejectReason::BadHello;
        std::string   Text;   // says what differs, for the notification

        void Serialize(EngineCore::Archive& Ar);
    };

    /* A member's state, as a change: the whole dirty list the first time
     * (bFullDirty), then only what was added and removed. */
    struct COLLAB_API PresenceMessage
    {
        static constexpr EMessage Type = EMessage::Presence;

        uint32_t    MemberId = 0;   // S->C; ignored from a client
        std::string Head;
        std::string OpenLevel;
        EPlayState  Play = EPlayState::Editing;
        bool        bFullDirty = false;
        std::vector<std::string> DirtyAdded;
        std::vector<std::string> DirtyRemoved;

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API MemberJoinedMessage
    {
        static constexpr EMessage Type = EMessage::MemberJoined;

        MemberState Member;

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API MemberLeftMessage
    {
        static constexpr EMessage Type = EMessage::MemberLeft;

        uint32_t    MemberId = 0;
        std::string Reason;

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API NotifyMessage
    {
        static constexpr EMessage Type = EMessage::Notify;

        ESeverity   Severity = ESeverity::Info;
        std::string Title;
        std::string Body;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* One commit, as a notification names it. */
    struct COLLAB_API CommitInfo
    {
        std::string Oid;
        std::string Author;
        std::string Summary;   // the subject line

        void Serialize(EngineCore::Archive& Ar);
    };

    /* What a push moved: OldOid..NewOid on Branch, its commits (newest first)
     * and every path they changed, repository-relative. */
    struct COLLAB_API BranchMove
    {
        std::string              Branch;
        std::string              OldOid;    // empty for a branch's first push
        std::string              NewOid;
        std::vector<CommitInfo>  Commits;
        std::vector<std::string> Paths;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* C->S: this editor pushed the session branch. (P4's worker will notice
     * pushes from outside the editor; until then only editors announce.) */
    struct COLLAB_API PushedMessage
    {
        static constexpr EMessage Type = EMessage::Pushed;

        BranchMove Move;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* S->everyone else: the session branch moved, and who moved it. */
    struct COLLAB_API BranchAdvancedMessage
    {
        static constexpr EMessage Type = EMessage::BranchAdvanced;

        uint32_t    PusherId = 0;   // 0: not an editor in the session
        std::string Pusher;
        BranchMove  Move;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* The content worker (ContentWorker.h) introducing itself: the key the
     * server started it with, for the session it was started for. */
    struct COLLAB_API WorkerHelloMessage
    {
        static constexpr EMessage Type = EMessage::WorkerHello;

        uint32_t    Protocol  = ProtocolVersion;
        uint32_t    SessionId = 0;
        std::string Key;

        void Serialize(EngineCore::Archive& Ar);
    };

    enum class EBuildState : uint8_t
    {
        Idle = 0,     // nothing built yet
        Building,     // Oid, being built and checked
        Succeeded,    // Oid built; SchemaHash is its
        Failed        // Oid didn't build: FirstError
    };

    COLLAB_API const char* GetBuildStateName(EBuildState InState);

    /* The session branch's build on the server host: GameModule and the game's
     * dedicated server, at Oid. */
    struct COLLAB_API BuildStatusMessage
    {
        static constexpr EMessage Type = EMessage::BuildStatus;

        std::string Oid;
        EBuildState State = EBuildState::Idle;
        uint64_t    SchemaHash = 0;   // NetCore's, for Oid's build: what a Play Together client must match
        std::string Author;           // Oid's
        std::string Summary;          // Oid's subject line
        std::string FirstError;       // Failed: the first compiler or linker error
        uint32_t    Seconds = 0;      // how long it took

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API ContentProblem
    {
        std::string Path;      // repository-relative
        std::string Message;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* What the worker's check of Oid's content found: every world and prefab
     * loaded by the game's own build (Application::ExecReport). */
    struct COLLAB_API ValidationReportMessage
    {
        static constexpr EMessage Type = EMessage::ValidationReport;

        std::string Oid;
        std::string Author;
        uint32_t    Checked = 0;
        std::vector<ContentProblem> Problems;

        void Serialize(EngineCore::Archive& Ar);
    };

    // === Play Together (P5) =================================================

    /* C->S: start a shared run of Level -- runtime-relative, as the game names
     * worlds ("Content/Levels/Courtyard.nmworld"); empty, the game's start
     * level -- or, bStop, end the one that's going. */
    struct COLLAB_API PlayRequestMessage
    {
        static constexpr EMessage Type = EMessage::PlayRequest;

        std::string Level;
        bool        bStop = false;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* S->all: the session's game server is up. Address empty: the session
     * server's own host, as each editor reached it. */
    struct COLLAB_API PlayInviteMessage
    {
        static constexpr EMessage Type = EMessage::PlayInvite;

        uint32_t    FromId = 0;
        std::string From;
        std::string Level;
        std::string Address;
        uint16_t    Port = 0;
        std::string Oid;          // the build the game server runs
        uint64_t    SchemaHash = 0;   // its NetCore schema: a client's must match
        std::string GameMode;         // the level's game mode
        bool        bNetworked = true;// false: players won't see each other (not a NetGameMode)

        void Serialize(EngineCore::Archive& Ar);
    };

    struct COLLAB_API PlayEndedMessage
    {
        static constexpr EMessage Type = EMessage::PlayEnded;

        std::string Reason;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* S->worker: run <Game>Server on Level and Port, at Oid's build; bStop, end it. */
    struct COLLAB_API WorkerPlayMessage
    {
        static constexpr EMessage Type = EMessage::WorkerPlay;

        bool        bStop = false;
        std::string Level;
        uint16_t    Port = 0;
        std::string Oid;

        void Serialize(EngineCore::Archive& Ar);
    };

    /* worker->S: the game server is running (on Port), or it isn't (Error says
     * why: never started, or stopped). */
    struct COLLAB_API WorkerPlayStateMessage
    {
        static constexpr EMessage Type = EMessage::WorkerPlayState;

        bool        bRunning = false;
        uint16_t    Port = 0;
        std::string Error;
        std::string GameMode;           // the level's
        bool        bNetworked = true;  // its mode gives each client an avatar (a NetGameMode)

        void Serialize(EngineCore::Archive& Ar);
    };

    // === Framing ============================================================

    constexpr uint32_t MessageTag = EngineCore::MakeArchiveTag('C', 'L', 'A', 'B');

    /* InMessage, framed. */
    template<typename TMessage>
    std::vector<uint8_t> Encode(TMessage& InMessage)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        uint16_t Version = 1;
        Ar.OpenChunk(MessageTag, Version);
        uint16_t Type = uint16_t(TMessage::Type);
        Ar << Type;
        InMessage.Serialize(Ar);
        Ar.CloseChunk();
        return Ar.HasError() ? std::vector<uint8_t>() : Ar.GetBytes();
    }

    /* The message's type, or None if it isn't a Collab message. */
    COLLAB_API EMessage PeekType(const std::vector<uint8_t>& InBytes);

    /* False if InBytes isn't a TMessage, or is cut short. */
    template<typename TMessage>
    bool Decode(const std::vector<uint8_t>& InBytes, TMessage& OutMessage)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        uint16_t Version = 0;
        if(!Ar.OpenChunk(MessageTag, Version))
        {
            return false;
        }
        uint16_t Type = 0;
        Ar << Type;
        if(Ar.HasError() || Type != uint16_t(TMessage::Type))
        {
            return false;
        }
        OutMessage.Serialize(Ar);
        Ar.CloseChunk();
        return !Ar.HasError();
    }

    // === Helpers ============================================================

    /*
     * A remote URL reduced to host/path, so the same repository reached by
     * https, ssh or scp syntax, with or without ".git", a user or a port,
     * compares equal: "git@GitHub.com:Me/Game.git" and
     * "https://me@github.com/Me/Game" are both "github.com/Me/Game". The host
     * is lowercased; the path keeps its case. A local path is kept, with
     * '\\' made '/'.
     */
    COLLAB_API std::string NormalizeRemoteUrl(const std::string& InUrl);

    /* "<[Game] Module>@<NormalizeRemoteUrl(origin)>". */
    COLLAB_API std::string MakeProjectId(const std::string& InModule, const std::string& InRemoteUrl);

    /* Equal, in a time that depends on the lengths only: the session password. */
    COLLAB_API bool ConstantTimeEquals(const std::string& InA, const std::string& InB);

    /* InList sorted and unique. */
    COLLAB_API void SortUnique(std::vector<std::string>& InOutList);

    /* What to send for a dirty list going from InOld to InNew (both sorted). */
    COLLAB_API void DiffSorted(const std::vector<std::string>& InOld, const std::vector<std::string>& InNew,
                               std::vector<std::string>& OutAdded, std::vector<std::string>& OutRemoved);

    /* InOutList with InAdded and InRemoved applied; stays sorted. */
    COLLAB_API void ApplyDiff(std::vector<std::string>& InOutList, const std::vector<std::string>& InAdded,
                              const std::vector<std::string>& InRemoved);
}

#endif //COLLAB_COLLABPROTOCOL_H

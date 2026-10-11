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
 * Messages P2 uses: Hello / Welcome / Reject, Presence, MemberJoined /
 * MemberLeft, Notify. The rest are numbered for P3-P5 and not sent yet.
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

        /* Reserved for later phases; not sent yet. */
        Pushed = 16,     // P3: C->S branch, oid
        BranchAdvanced,  // P3: S->all
        BuildStatus,     // P4: S->all
        ValidationReport,// P4: S->author
        PlayRequest,     // P5: C->S
        PlayInvite,      // P5: S->all
        PlayEnded        // P5: S->all
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

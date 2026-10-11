#ifndef COLLAB_COLLABCLIENT_H
#define COLLAB_COLLABCLIENT_H

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Public/Collab/CollabAPI.h"
#include "Public/Collab/CollabProtocol.h"

/*
 * An editor's side of Collab: connect with who and what it is (Identity), keep
 * its presence up to date, and hear about everyone else. No Qt: the editor's
 * EditorCollabSession turns the events into notifications and docks.
 *
 * Poll() often (each frame, or a 20 ms timer); then NextEvent until false.
 *
 * OVERLAPS. A path this editor has dirty that someone else has dirty too is
 * an overlap: two people about to make a conflict git will only report at
 * pull time. Each new one is an EEventType::Overlap.
 */
namespace Collab
{
    struct COLLAB_API ClientIdentity
    {
        std::string User;
        std::string ProjectId;     // MakeProjectId
        std::string Branch;
        std::string Head;
        std::string EngineTree;
        bool        bEngineDirty = false;
        std::string Password;
    };

    struct COLLAB_API ClientEvent
    {
        enum class EType : uint8_t
        {
            Joined,         // welcomed: GetRoster is filled
            Rejected,       // Reason, Text; disconnected
            ConnectFailed,  // nothing answered; Text
            Disconnected,   // lost or left; Text
            MemberJoined,   // MemberId
            MemberLeft,     // MemberId (gone from the roster already), User, Text
            MemberChanged,  // MemberId: presence
            Overlap,        // Path, User: someone else also has Path dirty
            BranchAdvanced, // MemberId, User (who pushed), Move: the session branch moved
            BuildStatus,    // Build: the session's server build moved on (GetBuildStatus too)
            Validation,     // Report: content problems the worker found in a commit
            PlayInvite,     // Invite: a shared run is up (GetActiveRun too)
            PlayEnded,      // Text: the run is over, and why
            Notified        // Severity, Title, Text
        };

        EType         Type = EType::Joined;
        uint32_t      MemberId = 0;
        std::string   User;
        std::string   Path;
        std::string   Title;
        std::string   Text;
        ERejectReason Reason   = ERejectReason::BadHello;
        ESeverity     Severity = ESeverity::Info;
        BranchMove    Move;
        BuildStatusMessage      Build;
        ValidationReportMessage Report;
        PlayInviteMessage       Invite;
    };

    class COLLAB_API CollabClient
    {
    public:
        enum class EState : uint8_t { Idle, Connecting, Joined };

        CollabClient();
        ~CollabClient();

        CollabClient(const CollabClient&)            = delete;
        CollabClient& operator=(const CollabClient&) = delete;

        /* Starts connecting; false (logged) if the address doesn't resolve. */
        bool Connect(const std::string& InHost, uint16_t InPort, const ClientIdentity& InIdentity, uint32_t InTimeoutMs = 10000);

        /* Leaves the session; Disconnected follows. */
        void Leave();

        void Poll();
        bool NextEvent(ClientEvent& OutEvent);

        EState GetState() const { return State; }
        bool   IsJoined() const { return State == EState::Joined; }

        /* This editor's state; sent now if joined and it changed, else on joining.
         * InDirty: repository-relative paths, any order. */
        void SetPresence(const std::string& InHead, const std::string& InOpenLevel, EPlayState InPlay,
                         std::vector<std::string> InDirty);

        /* Everyone in the session, this editor included. */
        const std::map<uint32_t, MemberState>& GetRoster() const { return Roster; }
        uint32_t GetMyId() const { return MyId; }
        uint32_t GetSessionId() const { return SessionId; }
        const std::string& GetSessionEngineTree() const { return SessionEngine; }

        /* The session's server build, as the worker last reported it; Idle without one. */
        const BuildStatusMessage& GetBuildStatus() const { return LastBuild; }

        /* This editor pushed the session branch: the others hear BranchAdvanced. */
        void AnnouncePush(const BranchMove& InMove);

        /* Play Together: a shared run of InLevel (runtime-relative; empty, the
         * start level) on the server's game server; the invite follows. */
        void RequestPlay(const std::string& InLevel);
        /* Ends the run, if this editor started it (or its starter has gone). */
        void RequestStopPlay();

        /* The run that's going, if any. */
        bool IsRunActive() const { return bRunActive; }
        const PlayInviteMessage& GetActiveRun() const { return ActiveRun; }

        /* Who else has InPath dirty. */
        std::vector<std::string> WhoHasDirty(const std::string& InPath) const;

        /* Every path dirty for someone else, with who; for the Content Browser's badge. */
        std::map<std::string, std::vector<std::string>> GetOthersDirty() const;

        struct Impl;

    private:
        void Push(ClientEvent InEvent) { Events.push_back(std::move(InEvent)); }
        void SendPresence(bool bInFull);
        void CheckOverlaps();
        void Reset();

        std::unique_ptr<Impl> P;
        EState   State     = EState::Idle;
        uint32_t MyId      = 0;
        uint32_t SessionId = 0;
        std::string SessionEngine;
        BuildStatusMessage LastBuild;
        PlayInviteMessage  ActiveRun;
        bool               bRunActive = false;
        std::map<uint32_t, MemberState> Roster;
        std::deque<ClientEvent>        Events;

        /* What this editor is, and what the server has of it. */
        MemberState Mine;
        MemberState Sent;
        bool        bSentOnce = false;

        /* "path\nuser" pairs already reported. */
        std::vector<std::string> KnownOverlaps;
    };
}

#endif //COLLAB_COLLABCLIENT_H

#ifndef COLLAB_COLLABSERVER_H
#define COLLAB_COLLABSERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Public/Collab/CollabAPI.h"
#include "Public/Collab/CollabProtocol.h"
#include "Public/Collab/ContentWorker.h"

/*
 * The server side of Collab: what NimbleEditorServer runs. Many sessions at
 * once, each a project on a branch (CollabProtocol.h); an editor's Hello puts
 * it in one, or is refused with a reason it can show.
 *
 * Poll() once in a while from one thread -- the server's loop, or a test's.
 * It never blocks. Everything is logged ("[Collab] ...").
 */
namespace Collab
{
    struct COLLAB_API ServerConfig
    {
        uint16_t    Port      = DefaultPort;   // 0: any free one (tests)
        std::string Password;                  // empty: none
        uint32_t    TimeoutMs = 15000;         // an editor silent this long is gone
        uint32_t    HelloTimeoutMs = 10000;    // a connection that never says Hello
        size_t      MaxPeers  = 64;

        /* Content workers (ContentWorker.h), one per session, when RepoPath is
         * this host's checkout of the project; empty, none. */
        std::string RepoPath;
        std::string DataDir;                   // worktrees and worker logs; empty: a per-user folder (%USERPROFILE%/.nimblecollab)
        std::string WorkerExecutable;          // empty: NimbleContentWorker beside this executable
        std::string WorkerPreset;              // empty: GetDefaultWorkerPreset()
        std::string WorkerCMake;               // empty: the worker finds one (ContentWorker.h)
        uint32_t    WorkerFetchIntervalMs = 30000;

        /* Play Together: a session's game server listens on GamePortBase plus
         * (session id - 1) % 100, and stops once nobody has been playing for
         * PlayIdleTimeoutMs. */
        uint16_t    GamePortBase      = 7777;
        uint32_t    PlayIdleTimeoutMs = 120000;

        /* Tests: a worker started (and stopped, by session id) in-process
         * instead of as NimbleContentWorker. */
        std::function<void(const WorkerConfig&)> LaunchWorker;
        std::function<void(uint32_t)>            StopWorker;
    };

    /* server-relwithdebinfo-msvc on Windows, -clang elsewhere. */
    COLLAB_API const char* GetDefaultWorkerPreset();

    /* For the server's status line and the tests. */
    struct COLLAB_API SessionSummary
    {
        uint32_t                 SessionId = 0;
        std::string              ProjectId;
        std::string              Branch;
        std::string              EngineTree;
        std::string              Tip;       // the last pushed commit, if any was announced
        BuildStatusMessage       LastBuild; // the worker's last report; Idle without one
        bool                     bWorkerConnected = false;
        bool                     bPlaying = false;   // a shared run is up
        PlayInviteMessage        Invite;             // bPlaying: what was sent
        std::vector<MemberState> Members;
    };

    class COLLAB_API CollabServer
    {
    public:
        CollabServer();
        ~CollabServer();

        CollabServer(const CollabServer&)            = delete;
        CollabServer& operator=(const CollabServer&) = delete;

        /* Listens; false (logged) if the port can't be had. */
        bool Start(const ServerConfig& InConfig);

        /* Everyone disconnected, the socket closed. */
        void Stop();

        bool     IsRunning() const;
        uint16_t GetPort() const;

        /* Connections, messages and timeouts. */
        void Poll();

        std::vector<SessionSummary> GetSessions() const;
        size_t GetMemberCount() const;

        struct Impl;

    private:
        std::unique_ptr<Impl> P;
    };
}

#endif //COLLAB_COLLABSERVER_H

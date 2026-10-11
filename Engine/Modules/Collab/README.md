# Collab

Editor collaboration: the protocol, the server's sessions and an editor's
client, over ENet. `NimbleEditorServer` (`Engine/Tools/EditorServer`) runs
`CollabServer`; the editor's `EditorCollabSession` runs `CollabClient`. The
plan and the phases still to come are in
`Engine/Modules/Editor/COLLABORATION.md`.

**No Qt, no GameCore.** Core and ENet only, so the server builds anywhere a
compiler and CMake are, and no game code can take it down. ENet is NetCore's
vendored copy (`Modules/NetCore/External/enet-1.3.18`); Collab doesn't need
NetCore to be on. Built STATIC, for Editor and Server targets.

## Pieces

| Piece | What it is |
|---|---|
| `CollabProtocol.h` | Message types and payloads, framing (`Encode` / `Decode`, one `CLAB` Archive chunk each), `MakeProjectId` / `NormalizeRemoteUrl`, `ConstantTimeEquals`, sorted-list diffs |
| `CollabServer.h` | Sessions, one per project and branch: the handshake and its refusals, presence relayed as changes, members leaving or timing out |
| `CollabClient.h` | One editor's connection: joining, its presence, the roster, overlap detection, events to poll |
| `CollabTransport` (private) | NetCore's `NetTransport` cut down to one reliable, ordered channel |

## Sessions

A session is a **project on a branch**. The project ID is `[Game] Module`
plus origin's URL reduced to `host/path`, so https, ssh and scp clones of
one repository agree. The first editor to join sets the session's engine
identity, `git rev-parse HEAD:Engine`. A later editor is refused when:

- its engine tree differs (the reason names both trees, shortened);
- it has uncommitted changes under `Engine/`;
- the password is wrong (compared in constant time), when the server has one;
- it speaks another protocol version, or never says hello (10 s).

This is how "engine changes aren't supported during a session" is enforced.
A session ends when its last editor leaves.

## Presence

Each editor sends its HEAD, its open level, whether it's playing, and its
**dirty paths**: what git sees changed and not pushed, plus the open level
while it has unsaved edits. The first send carries the whole list, later ones
only what was added and removed; the server keeps each member's full list and
relays the change to the others. A path two editors both have dirty is an
**overlap**. Each editor gets one `Overlap` event per path and person, which
the editor shows as a warning before git has a conflict to report.

## Protocol, v1

| Message | Direction | Payload |
|---|---|---|
| `Hello` | C→S | protocol, user, project ID, branch, HEAD, engine tree, engine dirty, password |
| `Welcome` | S→C | session ID, your member ID, the session's engine tree, the roster (you included) |
| `Reject` | S→C | reason, text; then the server disconnects |
| `Presence` | C→S→others | member ID (from the server), HEAD, open level, play state, full flag, dirty added, dirty removed |
| `MemberJoined` / `MemberLeft` | S→others | the member · member ID, reason |
| `Notify` | S→C | severity, title, body |
| `Pushed` | C→S | branch, old commit, new commit, the commits (oid, author, summary), changed paths |
| `BranchAdvanced` | S→others | the pusher's member ID and name, and the push as above |
| `WorkerHello` | worker→S | protocol, session ID, the key the server started it with |
| `BuildStatus` | worker→S→all | commit, state (building, built, failed), schema hash, author, summary, first error, seconds |
| `ValidationReport` | worker→S→the author | commit, author, files checked, problems (path, message) |
| `PlayRequest` | C→S | level (runtime-relative; empty, the start level), or stop |
| `WorkerPlay` | S→worker | start (level, port, commit) or stop the game server |
| `WorkerPlayState` | worker→S | running on a port, or not and why; the level's game mode and whether it's networked |
| `PlayInvite` | S→all | who started it, level, address (empty: the session server's host), port, commit, schema hash, the level's game mode and whether it's networked |
| `PlayEnded` | S→all | why |

Appending a field at the end of a payload is compatible both ways: a newer
message's extra fields are skipped by an older reader (Archive's chunk rule),
and a newer reader reads them only if the message has them
(`if(Ar.IsSaving() || !Ar.IsAtEnd())`, as `PlayInvite`'s game mode does). Any
other layout change bumps `ProtocolVersion`.

## The branch moving

An editor that pushes the session branch sends `Pushed`, with what moved as
its own git reads it. The server keeps the session's tip and relays the push to
everyone else as `BranchAdvanced`; a push of the same tip again, or of another
branch, isn't relayed (the pusher is told the latter). Pushes made outside the
editor aren't seen until P4's worker fetches the branch itself.

## The content worker

Given `--repo` (this host's checkout of the project), the server starts a
`NimbleContentWorker` for each session. It connects back over loopback as a
Collab client, introduced by a one-time key (`WorkerHello`), and for each new
tip of the session branch:

1. keeps a detached `git worktree` of it under `--data`, at `wt/<8 hex>` (one
   per game and branch, kept between sessions so builds are incremental;
   short, because a game's build tree is deep and Windows' 260-character path
   limit is near -- give `--data` a short folder), fetching every
   30 s and at once when an editor pushes;
2. tells the server what moved if no editor did -- a push from a terminal
   reaches the editors as a `BranchAdvanced` too;
3. builds the game module and `<Game>Server` with `--preset`
   (`server-relwithdebinfo-msvc` on Windows, `-clang` elsewhere; Visual
   Studio's own CMake and Ninja on Windows unless `--cmake` says otherwise);
4. runs `<Game>Server -collab-report=<file>` (`Application::ExecReport`):
   the NetCore schema hash, and every world and prefab loaded by that build,
   their warnings and errors and any asset path they name that doesn't exist;
5. reports `BuildStatus` to everyone (a late joiner gets the last one) and the
   `ValidationReport` to the commit's author, if they're in the session.

Game code only ever runs in the `<Game>Server` child: never in the worker or
the server. The worker stops with its session. It builds and runs whatever is
pushed, so a server with `--repo` is for a trusted team only.

## Play Together

An editor asks for a shared run of a level (`PlayRequest`). The server checks
the session has a worker and a good build, picks a port (`GamePortBase` +
session id - 1; 7777 by default) and has the worker run that build's
`<Game>Server` on the level -- the `[World_*]` whose `File` is that level -- and
port. Once it's up, everyone gets a `PlayInvite` (late joiners too); whoever
asked plays at once.

An editor offers Join only when it can play: its own game build's schema hash
matches the invite's, and its HEAD has the run's commit. Otherwise it says why
(Compile Source, or Pull & Reload) and never points Play at the server.
Joining plays as a client of the session server's host and that port,
whatever Editor Settings > Play says.

Who is playing is presence (`PlayingTogether`). The run ends when its starter
stops it (anyone may, once the starter has gone), when nobody has been playing
for `PlayIdleTimeoutMs` (two minutes), or when the game server stops; everyone
gets `PlayEnded` and those playing stop. While it runs, the worker fetches and
announces new commits but builds them only after: the running server's files
are locked.

## Running the server

```
NimbleEditorServer [--port 7788] [--password TEXT] [--logs Saved/Logs]
                   [--repo DIR [--data DIR] [--preset NAME] [--cmake PATH]]
```

`NIMBLE_COLLAB_PASSWORD` can stand in for `--password`, so it stays out of
process listings. In the editor: **File > Collaboration Session**, the Session
dock, or `collab.connect host[:port] [name] [password]`, `collab.leave`,
`collab.status` in the console.

> **Trust boundary.** A session is for a trusted team on a LAN or VPN. Don't
> expose the server to the internet.

## Tests

`CollabLoopbackTests` (`Collab.Loopback`): a server and several clients over
127.0.0.1 covering the helpers, joining, presence and its changes, overlaps,
another branch's own session, leaving, every refusal, and both timeouts
(nothing answering, an editor gone silent). And the content worker on real git
repositories with a fake build: its first build and schema hash, a push from
outside the editor found and relayed, a broken entity file reported, a failed
build's first error, a late joiner's build status, and the worker stopping
with its session. The editor's side is
`NimbleEditorCollabTests` (`Editor.Collab`).

## Sources

Code derived from outside work carries a `Derived from:` header in its source
file and is listed here. Nothing in P2 is.

- [ENet 1.3.18](http://enet.bespin.org/) (MIT): the transport, vendored in NetCore.

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

`Pushed`, `BranchAdvanced`, `BuildStatus`, `ValidationReport` and the `Play*`
messages are numbered for P3–P5 and not sent yet. Appending a field at the end
of a payload is compatible (Archive skips what a reader doesn't know); any
other layout change bumps `ProtocolVersion`.

## Running the server

```
NimbleEditorServer [--port 7788] [--password TEXT] [--logs Saved/Logs]
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
(nothing answering, an editor gone silent). The editor's side is
`NimbleEditorCollabTests` (`Editor.Collab`).

## Sources

Code derived from outside work carries a `Derived from:` header in its source
file and is listed here. Nothing in P2 is.

- [ENet 1.3.18](http://enet.bespin.org/) (MIT): the transport, vendored in NetCore.

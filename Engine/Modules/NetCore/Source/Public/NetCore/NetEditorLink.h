#ifndef NETCORE_NETEDITORLINK_H
#define NETCORE_NETEDITORLINK_H

#include <cstddef>
#include <cstdint>

#include "Public/NetCore/NetCoreAPI.h"

/*
 * STUB(NetCore-Editor): editors connected to one another, the start of
 * several people editing one world together.
 *
 * What exists: one editor hosts, others join it by address (a star: the host
 * relays), and each tells the rest what it did, as text -- the undo history's
 * description of each recorded edit. Received edits are only reported, never
 * applied.
 *
 * What it is for, and is not yet (Modules/NetCore/README.md):
 *   - a snapshot of the host's world sent to an editor that joins
 *   - edits as data -- entity GUID, component type NAME, field NAME, the
 *     value encoded by field type (EditorPropertyEdit, reserved in
 *     NetProtocol.h) -- never the undo history's raw component bytes, whose
 *     layout is the build's
 *   - applying a remote edit through the document, so it is undoable
 *   - conflicts: who holds an entity while it is being edited
 *
 * Its own channel (NetTransport.h's NET_CHANNEL_EDITOR), so it could share a
 * socket with a play session later. Editor's game thread only.
 */
namespace NetCore
{
    class NETCORE_API NetEditorLink
    {
    public:
        NetEditorLink();
        ~NetEditorLink();

        NetEditorLink(const NetEditorLink&)            = delete;
        NetEditorLink& operator=(const NetEditorLink&) = delete;

        /* How this editor introduces itself. */
        void SetUserName(const char* InName);

        bool Host(uint16_t InPort);
        bool Join(const char* InAddress, uint16_t InPort);
        void Leave();

        bool     IsActive() const;
        bool     IsHosting() const;
        size_t   GetPeerCount() const;
        uint16_t GetPort() const;

        /* Connections and messages; call once a frame. */
        void Poll();

        /* This editor did something: InText is what its undo history calls it. */
        void SendCommand(const char* InText);

        /* Another editor did something. */
        using CommandFn = void (*)(const char* InUser, const char* InText, void* InUserData);
        void SetCommandHandler(CommandFn InFn, void* InUserData);

        struct Impl;

    private:
        Impl* P;
    };
}

#endif //NETCORE_NETEDITORLINK_H

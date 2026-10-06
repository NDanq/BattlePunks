#ifndef NIMBLEEDITOR_EDITORNETSESSION_H
#define NIMBLEEDITOR_EDITORNETSESSION_H

#if WITH_EDITOR && WITH_NETCORE

#include <QObject>
#include <QString>

#include "Public/NetCore/NetEditorLink.h"

class EditorConsole;
class EditorDocument;
class QTimer;

/*
 * STUB(NetCore-Editor): this editor linked to others over NetCore's
 * NetEditorLink, toward several people editing one world together.
 *
 * Today: `net.host [port]`, `net.join host[:port]`, `net.leave`, `net.status`
 * in the console; every step the level document records (EditorDocument's
 * CommandRecorded) is announced to the others by its undo text, and theirs
 * are printed here. Nothing is applied -- see NetEditorLink.h for the road
 * from here (a snapshot on join, edits as data, applying them undoably,
 * ownership while editing).
 *
 * Built only with NetCore (Modules/Editor/CMakeLists.txt). GUI thread only.
 */
class EditorNetSession
{
public:
    EditorNetSession(EditorDocument& InDocument, EditorConsole& InConsole);
    ~EditorNetSession();

    EditorNetSession(const EditorNetSession&)            = delete;
    EditorNetSession& operator=(const EditorNetSession&) = delete;

private:
    void RegisterCommands();
    void OnCommandRecorded(const QString& InText);
    static void OnRemoteCommand(const char* InUser, const char* InText, void* InUserData);

    EditorDocument&       Document;
    EditorConsole&        Console;
    NetCore::NetEditorLink Link;
    QTimer*               PollTimer = nullptr;
};

#endif // WITH_EDITOR && WITH_NETCORE

#endif //NIMBLEEDITOR_EDITORNETSESSION_H

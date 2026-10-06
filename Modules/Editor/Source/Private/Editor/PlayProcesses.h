#ifndef NIMBLEEDITOR_PLAYPROCESSES_H
#define NIMBLEEDITOR_PLAYPROCESSES_H

#if WITH_EDITOR

#include <cstdint>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

/*
 * The game's own executables, as the editor knows them: the targets
 * nimble_add_game_launcher / nimble_add_server_launcher recorded for the
 * project's [Game] Module (the generated EditorProject.h). Built by Build >
 * Compile Source beside the module; they run from the editor's directory,
 * which is the game's runtime directory.
 */
namespace GameExecutables
{
    /* [Game] Module; empty if unset. */
    QString GetModule();

    /* InKind "client" or "server": the targets recorded for InModule. */
    QStringList FindTargets(const QString& InModule, const QString& InKind);

    /* Where InTarget's executable is (beside the editor). */
    QString GetPath(const QString& InTarget);
}

/*
 * Play-in-Editor's other processes: the dedicated server a networked Play
 * starts first, and the game windows that join it as more clients (Play >
 * New Client Window). Their output is the editor's Console, tagged
 * "[Server]" and "[Client N]". All of them stop with the run.
 */
class PlayProcesses : public QObject
{
    Q_OBJECT

public:
    explicit PlayProcesses(QObject* InParent = nullptr);
    ~PlayProcesses() override;   // StopAll()

    /* <Game>Server -port=InPort. False, logged, without a built server. */
    bool StartServer(uint16_t InPort);

    /* <Game> -connect=InAddress:InPort, in a window of its own. InExtraArgs:
     * more of its command line, space-separated. */
    bool StartClient(const QString& InAddress, uint16_t InPort, const QString& InExtraArgs = QString());

    /* Ends every process this started. */
    void StopAll();

    int GetClientCount() const { return ClientCount; }

private:
    bool Launch(const QString& InTarget, const QStringList& InArgs, const QString& InTag);

    struct Running
    {
        QProcess* Process = nullptr;
        QString   Tag;
        QByteArray Pending;
    };
    std::vector<Running*> Processes;
    int                   ClientCount = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PLAYPROCESSES_H

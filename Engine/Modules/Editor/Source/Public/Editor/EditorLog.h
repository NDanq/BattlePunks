#ifndef NIMBLEEDITOR_EDITORLOG_H
#define NIMBLEEDITOR_EDITORLOG_H

#if WITH_EDITOR

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <QObject>
#include <QString>
#include <QTime>

/*
 * Everything the editor process logs, kept for the Console panel.
 *
 * Install() makes this EngineCore's log handler, so every CORE_LOG from Core,
 * GDI, GameWindow, GameCore, RenderCore and the editor itself lands here -- and
 * is still forwarded to Qt's message system, so the debugger's output window
 * sees it too. The console's own traffic (a typed command, its reply) is
 * appended alongside, so the panel is one ordered transcript.
 *
 * THREADS. Append is callable from any thread: the render thread logs, and so
 * do module loads. It queues under a mutex and posts one Flush to the GUI
 * thread, which moves the batch into the history and emits EntriesAppended.
 * Everything else -- GetEntries, Clear, the signals -- is GUI-thread only. A
 * message logged on the GUI thread is queued the same way, so entries keep the
 * order they were logged in whichever thread wrote them.
 *
 * The history is a bounded ring (MaxEntries): a console left open all day must
 * not grow without limit. Logs from before the panel exists -- the renderer's
 * startup, the config load -- are in it, so the panel shows them when it opens.
 */
class EditorLog : public QObject
{
    Q_OBJECT

public:
    enum class EKind : uint8_t
    {
        Log = 0,
        Warning,
        Error,
        Fatal,

        /* The console's own lines, never filtered out: */
        Input,         // a command as the user typed it
        Output,        // a command's reply
        OutputError    // a command that failed, and why
    };

    struct Entry
    {
        EKind   Kind = EKind::Log;
        QTime   Time;
        QString Text;   // may span lines
    };

    static constexpr size_t MaxEntries = 20000;

    /* Creates the log (on the calling thread, which must be the GUI thread) and
     * installs it as EngineCore's handler. Call once QApplication exists. */
    static void Install();

    /* Restores EngineCore's default handler and destroys the log. Call before
     * QApplication goes. */
    static void Uninstall();

    /* Null outside Install/Uninstall. */
    static EditorLog* Get();

    /* Any thread. */
    void Append(EKind InKind, const QString& InText);

    /* GUI thread. Oldest first. */
    const std::deque<Entry>& GetEntries() const { return Entries; }

    /* Counts over the whole history, for the panel's filter buttons. */
    int GetCount(EKind InKind) const { return Counts[size_t(InKind)]; }

    /* GUI thread. Empties the history; emits Cleared. */
    void Clear();

signals:
    /* The last InCount entries of GetEntries() are new. Older ones may have
     * been dropped off the front to stay under MaxEntries. */
    void EntriesAppended(int InCount);

    void Cleared();

private:
    EditorLog() = default;

    Q_INVOKABLE void Flush();

    std::deque<Entry> Entries;
    int               Counts[size_t(EKind::OutputError) + 1] = {};

    std::mutex         PendingMutex;
    std::vector<Entry> Pending;
    bool               bFlushQueued = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORLOG_H

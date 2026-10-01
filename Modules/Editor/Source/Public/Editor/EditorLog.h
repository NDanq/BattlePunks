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
 * The process's log transcript for the Console panel. Install() makes it
 * EngineCore's log handler (still forwarding to Qt's message system);
 * console commands and replies are appended alongside.
 *
 * THREADS. Append works from any thread: it queues under a mutex and posts a
 * Flush to the GUI thread, preserving order. Everything else is GUI-only.
 *
 * A bounded ring (MaxEntries), filled from startup on.
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

    /* On the GUI thread, once QApplication exists. */
    static void Install();

    /* Before QApplication is destroyed. */
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
    /* The last InCount entries are new; older ones may have been dropped. */
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

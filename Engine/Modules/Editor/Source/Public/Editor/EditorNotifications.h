#ifndef NIMBLEEDITOR_EDITORNOTIFICATIONS_H
#define NIMBLEEDITOR_EDITORNOTIFICATIONS_H

#if WITH_EDITOR

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

/*
 * The editor's notifications: what finished, failed or needs an answer, as
 * toasts over the main window (NotificationToasts), a history in the
 * Notifications dock (NotificationCenter) and a bell in the status bar with
 * the unread count (NotificationStatusItem). EditorMain installs it beside
 * EditorLog. Plain confirmations ("Saved") stay in the status bar.
 *
 * REPLACE KEY. A post whose ReplaceKey matches a notification still in the
 * history updates that one in place (and shows its toast again) instead of
 * adding another: "3 new commits" becomes "4 new commits", a bake's progress
 * becomes its result.
 *
 * DO NOT DISTURB. Toasts are held back, except errors; the history and the
 * unread count carry on.
 *
 * THREADS. Post works from any thread; off the GUI thread it is queued to it
 * and returns 0. Everything else is GUI-only, as are the action callbacks.
 *
 * Design reference: VS Code's workbench notifications (MIT) -- toasts, a
 * center, a status item, sticky errors with actions, do not disturb. No code
 * is taken from it.
 */
class EditorNotifications : public QObject
{
    Q_OBJECT

public:
    enum class ESeverity : uint8_t
    {
        Info = 0,
        Success,
        Warning,
        Error
    };

    struct Action
    {
        QString               Label;
        std::function<void()> Run;
    };

    /* No progress, a busy bar, or 0..100. */
    static constexpr int NoProgress    = -1;
    static constexpr int Indeterminate = -2;

    struct Notification
    {
        ESeverity     Severity = ESeverity::Info;
        QString       Title;
        QString       Body;        // may be empty; may span lines
        QString       Source;      // "Build", "Play", "Bake": shown in the history
        QString       ReplaceKey;  // see above; empty never replaces
        QList<Action> Actions;     // at most MaxActions are shown
        int           Progress = NoProgress;
        bool          bSticky  = false;   // stays until closed; see IsSticky

        /* Set by Post. */
        uint64_t  Id = 0;
        QDateTime Time;
        bool      bRead        = false;
        bool      bToastClosed = false;   // its toast was closed or timed out; still in the history
    };

    static constexpr int    MaxActions = 3;
    static constexpr size_t MaxHistory = 200;

    /* On the GUI thread, once QApplication exists. */
    static void Install();

    /* Before QApplication is destroyed. */
    static void Uninstall();

    /* Null outside Install/Uninstall. */
    static EditorNotifications* Get();

    /* Install()ed, or nothing happens. Any thread. */
    static uint64_t Notify(ESeverity InSeverity, const QString& InTitle, const QString& InBody = QString(),
                           const QString& InSource = QString(), const QString& InReplaceKey = QString());

    /* Any thread. The notification's id on the GUI thread (the replaced one's,
     * for a replace); 0 when queued from another thread. */
    uint64_t Post(Notification InNotification);

    /* Errors, anything with actions or unfinished progress, and bSticky ones
     * stay until closed; the rest time out (ToastTimeoutMs). */
    static bool IsSticky(const Notification& InNotification);

    /* GUI thread from here on. Null if gone. */
    const Notification* Find(uint64_t InId) const;
    const Notification* FindByKey(const QString& InReplaceKey) const;

    /* Oldest first. */
    const std::deque<Notification>& GetHistory() const { return History; }

    /* Closes the toast; the history keeps it. */
    void CloseToast(uint64_t InId);

    /* Runs action InIndex, then closes the toast. */
    void RunAction(uint64_t InId, int InIndex);

    /* From the history too. */
    void Remove(uint64_t InId);
    void Clear();

    int  GetUnreadCount() const { return UnreadCount; }
    void MarkAllRead();

    bool IsDoNotDisturb() const { return bDoNotDisturb; }
    void SetDoNotDisturb(bool bInOn);

    /* Whether its toast should be on screen now: not closed, and not held
     * back by Do Not Disturb. */
    bool WantsToast(const Notification& InNotification) const;

    /* How long a timed toast stays, in milliseconds; tests shorten it. */
    int  GetToastTimeoutMs() const { return ToastTimeoutMs; }
    void SetToastTimeoutMs(int InMs) { ToastTimeoutMs = InMs; }

signals:
    void Added(uint64_t InId);
    /* Replaced, read, or its toast closed. */
    void Changed(uint64_t InId);
    void Removed(uint64_t InId);
    void Cleared();
    void UnreadCountChanged(int InCount);
    void DoNotDisturbChanged(bool bInOn);

private:
    EditorNotifications() = default;

    uint64_t PostOnGuiThread(Notification InNotification);
    Q_INVOKABLE void FlushPending();
    Notification* FindMutable(uint64_t InId);
    void RecountUnread();

    std::deque<Notification> History;
    int  UnreadCount    = 0;
    bool bDoNotDisturb  = false;
    int  ToastTimeoutMs = 6000;

    std::atomic<uint64_t> NextId{ 1 };

    std::mutex                PendingMutex;
    std::vector<Notification> Pending;
    bool                      bFlushQueued = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORNOTIFICATIONS_H

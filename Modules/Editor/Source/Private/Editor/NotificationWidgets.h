#ifndef NIMBLEEDITOR_NOTIFICATIONWIDGETS_H
#define NIMBLEEDITOR_NOTIFICATIONWIDGETS_H

#if WITH_EDITOR

#include <cstdint>
#include <map>

#include <QColor>
#include <QDockWidget>
#include <QFrame>
#include <QList>
#include <QTimer>
#include <QWidget>

#include "Public/Editor/EditorNotifications.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QMainWindow;
class QProgressBar;
class QToolButton;
class QVBoxLayout;

/*
 * EditorNotifications on screen (EditorNotifications.h has the model):
 *
 *   NotificationToasts      the toasts, bottom right of the main window's
 *                           central area, newest lowest; at most MaxVisible,
 *                           the rest queued behind a "+N more" line.
 *   NotificationCenter      the Notifications dock: the history, newest
 *                           first, unread in bold; right-click for a
 *                           notification's actions. Seeing it marks all read.
 *   NotificationStatusItem  the status bar's Do Not Disturb toggle and bell,
 *                           with the unread count; the bell shows or hides
 *                           the dock.
 */
namespace EditorNotificationStyle
{
    /* Error red, warning amber, success green (the status bar's own), info
     * the palette's highlight. */
    QColor ForSeverity(EditorNotifications::ESeverity InSeverity);

    QIcon IconFor(EditorNotifications::ESeverity InSeverity);
}

/* One toast. */
class NotificationToast : public QFrame
{
    Q_OBJECT

public:
    NotificationToast(EditorNotifications& InModel, uint64_t InId, QWidget* InParent);

    uint64_t GetId() const { return Id; }

    /* From the model's copy, after a replace; restarts the timer. */
    void Refresh();

    /* The timer is running: a timed toast, not hovered. */
    bool IsTiming() const { return Timer.isActive(); }

protected:
    void enterEvent(QEnterEvent* InEvent) override;
    void leaveEvent(QEvent* InEvent) override;

private:
    void Expire();

    EditorNotifications& Model;
    uint64_t             Id;

    QLabel*       Icon     = nullptr;
    QLabel*       Title    = nullptr;
    QLabel*       Body     = nullptr;
    QProgressBar* Progress = nullptr;
    QWidget*      Actions  = nullptr;
    QTimer        Timer;
    bool          bExpiring = false;
};

class NotificationToasts : public QWidget
{
    Q_OBJECT

public:
    static constexpr int MaxVisible = 3;

    /* A frameless tool window over InHost: an overlay child would sit under
     * the viewport's native window. */
    explicit NotificationToasts(QMainWindow& InHost);

    /* Shown toasts, top to bottom. */
    QList<NotificationToast*> GetVisibleToasts() const;

    /* Toasts waiting for a slot. */
    int GetQueuedCount() const { return QueuedCount; }

protected:
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    void Sync();
    void Reposition();

    EditorNotifications&  Model;
    QMainWindow&          Host;
    QVBoxLayout*          Layout = nullptr;
    QLabel*               MoreLabel = nullptr;
    std::map<uint64_t, NotificationToast*> Toasts;
    int                   QueuedCount = 0;
};

class NotificationCenter : public QDockWidget
{
    Q_OBJECT

public:
    explicit NotificationCenter(QWidget* InParent);

    QListWidget* GetList() const { return List; }

    /* Shown and in front, or hidden. */
    void Toggle();

private:
    void Rebuild();
    void FillItem(QListWidgetItem* InItem, const EditorNotifications::Notification& InNotification) const;
    QListWidgetItem* FindItem(uint64_t InId) const;
    void MarkReadIfSeen();
    void ShowContextMenu(const QPoint& InPos);

    EditorNotifications& Model;
    QListWidget*         List = nullptr;
    QToolButton*         DoNotDisturb = nullptr;
};

class NotificationStatusItem : public QWidget
{
    Q_OBJECT

public:
    NotificationStatusItem(NotificationCenter& InCenter, QWidget* InParent);

    QToolButton* GetBell() const { return Bell; }
    QToolButton* GetDoNotDisturb() const { return DoNotDisturb; }

private:
    void Update();

    EditorNotifications& Model;
    QToolButton*         Bell = nullptr;
    QToolButton*         DoNotDisturb = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_NOTIFICATIONWIDGETS_H

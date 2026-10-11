/*
 * NimbleEditorNotificationTests -- EditorNotifications and its widgets,
 * offscreen: posting, replacing by key, actions, timed toasts expiring and
 * sticky ones staying, the queue past three toasts, Do Not Disturb, posts
 * from another thread, and the dock and bell's unread count.
 */

#include <cstdio>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QToolButton>

#include "Public/Editor/EditorNotifications.h"
#include "Private/Editor/NotificationWidgets.h"

using ESeverity    = EditorNotifications::ESeverity;
using Notification = EditorNotifications::Notification;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static void Pump(const int InMs = 0)
{
    QElapsedTimer Clock;
    Clock.start();
    do
    {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    while(Clock.elapsed() < InMs);
}

static Notification Make(const ESeverity InSeverity, const QString& InTitle, const QString& InKey = QString())
{
    Notification New;
    New.Severity   = InSeverity;
    New.Title      = InTitle;
    New.Source     = QStringLiteral("Test");
    New.ReplaceKey = InKey;
    return New;
}

static QLabel* TitleOf(NotificationToast* InToast)
{
    return InToast ? InToast->findChild<QLabel*>(QStringLiteral("ToastTitle")) : nullptr;
}

static void TestModel(EditorNotifications& Model)
{
    std::printf("Post, replace, remove\n");
    Model.Clear();

    int Added = 0, Changed = 0;
    const auto A = QObject::connect(&Model, &EditorNotifications::Added,   [&Added](uint64_t) { ++Added; });
    const auto C = QObject::connect(&Model, &EditorNotifications::Changed, [&Changed](uint64_t) { ++Changed; });

    const uint64_t First = Model.Post(Make(ESeverity::Info, QStringLiteral("Alice pushed 3 commits"), QStringLiteral("commits")));
    Expect(First != 0, "a GUI-thread post returns its id");
    Expect(Added == 1 && Model.GetHistory().size() == 1, "and adds it");
    Expect(Model.GetUnreadCount() == 1, "unread");

    const uint64_t Again = Model.Post(Make(ESeverity::Info, QStringLiteral("Alice pushed 4 commits"), QStringLiteral("commits")));
    Expect(Again == First, "the same key replaces: same id");
    Expect(Model.GetHistory().size() == 1, "no second entry");
    Expect(Changed == 1 && Added == 1, "Changed, not Added");
    Expect(Model.Find(First) && Model.Find(First)->Title == QStringLiteral("Alice pushed 4 commits"), "with the new title");

    Model.Post(Make(ESeverity::Warning, QStringLiteral("Unkeyed")));
    Model.Post(Make(ESeverity::Warning, QStringLiteral("Unkeyed")));
    Expect(Model.GetHistory().size() == 3, "no key never replaces");
    Expect(Model.GetUnreadCount() == 3, "three unread");

    Model.MarkAllRead();
    Expect(Model.GetUnreadCount() == 0, "MarkAllRead");
    Model.Post(Make(ESeverity::Info, QStringLiteral("Alice pushed 5 commits"), QStringLiteral("commits")));
    Expect(Model.GetUnreadCount() == 1, "a replace is unread again");

    Model.Remove(First);
    Expect(!Model.Find(First) && Model.GetHistory().size() == 2, "Remove");

    Expect(EditorNotifications::IsSticky(Make(ESeverity::Error, QStringLiteral("e"))), "errors are sticky");
    Notification WithAction = Make(ESeverity::Info, QStringLiteral("a"));
    WithAction.Actions = { { QStringLiteral("Go"), nullptr } };
    Expect(EditorNotifications::IsSticky(WithAction), "anything with actions is sticky");
    Notification Busy = Make(ESeverity::Info, QStringLiteral("p"));
    Busy.Progress = EditorNotifications::Indeterminate;
    Expect(EditorNotifications::IsSticky(Busy), "unfinished progress is sticky");
    Expect(!EditorNotifications::IsSticky(Make(ESeverity::Success, QStringLiteral("s"))), "a plain success is timed");

    for(size_t Index = 0; Index < EditorNotifications::MaxHistory + 5; ++Index)
    {
        Model.Post(Make(ESeverity::Info, QStringLiteral("Spam")));
    }
    Expect(Model.GetHistory().size() == EditorNotifications::MaxHistory, "the history is bounded");

    QObject::disconnect(A);
    QObject::disconnect(C);
    Model.Clear();
    Expect(Model.GetHistory().empty() && Model.GetUnreadCount() == 0, "Clear");
}

static void TestThreads(EditorNotifications& Model)
{
    std::printf("Posts from another thread\n");
    Model.Clear();
    uint64_t Returned = 1;
    std::thread Worker([&Model, &Returned]()
    {
        Returned = Model.Post(Make(ESeverity::Success, QStringLiteral("From a worker")));
    });
    Worker.join();
    Expect(Returned == 0, "off the GUI thread, Post returns 0");
    Expect(Model.GetHistory().empty(), "and is queued");
    Pump();
    Expect(Model.GetHistory().size() == 1 && Model.GetHistory().front().Title == QStringLiteral("From a worker"),
           "delivered on the GUI thread");
    Model.Clear();
}

static void TestToasts(EditorNotifications& Model, QMainWindow& Window, NotificationToasts& Toasts)
{
    std::printf("Toasts\n");
    Model.Clear();
    Model.SetToastTimeoutMs(150);
    Pump();
    Expect(Toasts.GetVisibleToasts().isEmpty() && !Toasts.isVisible(), "none at first");

    const uint64_t Timed = Model.Post(Make(ESeverity::Success, QStringLiteral("Compiled WorldsLab")));
    Pump();
    Expect(Toasts.isVisible() && Toasts.GetVisibleToasts().size() == 1, "a post shows a toast");
    Expect(Toasts.GetVisibleToasts().front()->IsTiming(), "a success is timed");

    const QRect Area(Window.centralWidget()->mapToGlobal(QPoint(0, 0)), Window.centralWidget()->size());
    Expect(Area.contains(Toasts.geometry().bottomRight()), "over the central area's corner");

    const uint64_t Sticky = Model.Post(Make(ESeverity::Error, QStringLiteral("Compile failed")));
    Pump(150 + 250 + 200);   // the timeout, the fade, and slack
    Expect(Model.Find(Timed) && Model.Find(Timed)->bToastClosed, "the timed one expires");
    Expect(Model.Find(Timed) != nullptr, "but stays in the history");
    Expect(Toasts.GetVisibleToasts().size() == 1 && Toasts.GetVisibleToasts().front()->GetId() == Sticky, "the error stays");

    /* Replace: same toast, new text. */
    Notification Baking = Make(ESeverity::Info, QStringLiteral("Baking 10%"), QStringLiteral("bake"));
    Baking.Progress = 10;
    Model.Post(Baking);
    Pump();
    NotificationToast* Bake = Toasts.GetVisibleToasts().back();
    Baking.Title    = QStringLiteral("Baking 60%");
    Baking.Progress = 60;
    Model.Post(Baking);
    Pump();
    Expect(Toasts.GetVisibleToasts().size() == 2, "a replace doesn't stack");
    Expect(Toasts.GetVisibleToasts().back() == Bake, "the same toast");
    Expect(TitleOf(Bake) && TitleOf(Bake)->text() == QStringLiteral("Baking 60%"), "updated in place");

    /* Close: gone from the screen, kept in the history. */
    Bake->findChild<QToolButton*>(QStringLiteral("ToastClose"))->click();
    Pump();
    Expect(Toasts.GetVisibleToasts().size() == 1, "closing removes the toast");
    Expect(Model.FindByKey(QStringLiteral("bake")) != nullptr, "but not the notification");

    /* Actions run, then close the toast. */
    int Ran = 0;
    Notification Ask = Make(ESeverity::Info, QStringLiteral("Alice pushed 3 commits"), QStringLiteral("pull"));
    Ask.Actions = { { QStringLiteral("Pull & Reload"), [&Ran]() { ++Ran; } }, { QStringLiteral("Later"), nullptr } };
    const uint64_t AskId = Model.Post(Ask);
    Pump();
    NotificationToast* AskToast = Toasts.GetVisibleToasts().back();
    Expect(AskToast->GetId() == AskId && !AskToast->IsTiming(), "a toast with actions is sticky");
    auto* Action = AskToast->findChild<QPushButton*>(QStringLiteral("ToastAction0"));
    Expect(Action && Action->text() == QStringLiteral("Pull & Reload"), "its first action is a button");
    Expect(AskToast->findChild<QPushButton*>(QStringLiteral("ToastAction1")) != nullptr, "and its second");
    if(Action)
    {
        Action->click();
    }
    Pump();
    Expect(Ran == 1, "clicking runs the action");
    Expect(Model.Find(AskId)->bToastClosed, "and closes the toast");

    /* At most three; the rest wait. */
    Model.Clear();
    for(int Index = 0; Index < 5; ++Index)
    {
        Model.Post(Make(ESeverity::Error, QStringLiteral("Error %1").arg(Index)));
    }
    Pump();
    Expect(Toasts.GetVisibleToasts().size() == NotificationToasts::MaxVisible, "three visible");
    Expect(Toasts.GetQueuedCount() == 2, "two queued");
    Expect(TitleOf(Toasts.GetVisibleToasts().front())->text() == QStringLiteral("Error 0"), "oldest on top");
    Model.CloseToast(Toasts.GetVisibleToasts().front()->GetId());
    Pump();
    Expect(Toasts.GetQueuedCount() == 1 && TitleOf(Toasts.GetVisibleToasts().back())->text() == QStringLiteral("Error 3"),
           "a closed one lets the next in");

    /* Do Not Disturb holds back all but errors. */
    Model.Clear();
    Model.SetDoNotDisturb(true);
    Model.Post(Make(ESeverity::Warning, QStringLiteral("Held")));
    Model.Post(Make(ESeverity::Error, QStringLiteral("Shown anyway")));
    Pump();
    Expect(Toasts.GetVisibleToasts().size() == 1 && TitleOf(Toasts.GetVisibleToasts().front())->text() == QStringLiteral("Shown anyway"),
           "Do Not Disturb shows errors only");
    Expect(Model.GetHistory().size() == 2, "and still records both");
    Model.SetDoNotDisturb(false);
    Pump();
    Expect(Toasts.GetVisibleToasts().size() == 2, "turning it off shows the held one");

    Model.Clear();
    Pump();
    Expect(!Toasts.isVisible(), "hidden with nothing to show");
    Model.SetToastTimeoutMs(6000);
}

static void TestCenterAndBell(EditorNotifications& Model, NotificationCenter& Center, NotificationStatusItem& Bell)
{
    std::printf("Notifications dock and bell\n");
    Model.Clear();
    Center.hide();
    Pump();

    Model.Post(Make(ESeverity::Info, QStringLiteral("First")));
    Model.Post(Make(ESeverity::Warning, QStringLiteral("Second")));
    Pump();
    Expect(Center.GetList()->count() == 2, "the dock lists the history");
    Expect(Center.GetList()->item(0)->text().startsWith(QStringLiteral("Second")), "newest first");
    Expect(Center.GetList()->item(0)->font().bold(), "unread in bold");
    Expect(Bell.GetBell()->text() == QStringLiteral("2"), "the bell counts unread");

    Bell.GetBell()->click();   // shows the dock: read
    Pump();
    Expect(Center.isVisible(), "the bell shows the dock");
    Expect(Model.GetUnreadCount() == 0 && Bell.GetBell()->text().isEmpty(), "seeing it marks all read");
    Expect(!Center.GetList()->item(0)->font().bold(), "no longer bold");

    Model.Post(Make(ESeverity::Info, QStringLiteral("While open")));
    Pump();
    Expect(Model.GetUnreadCount() == 0, "posts while it is open are read");

    Bell.GetBell()->click();
    Pump();
    Expect(!Center.isVisible(), "the bell hides it again");

    Bell.GetDoNotDisturb()->click();
    Expect(Model.IsDoNotDisturb(), "the status bar toggles Do Not Disturb");
    Model.SetDoNotDisturb(false);
    Expect(!Bell.GetDoNotDisturb()->isChecked(), "and follows the model");

    Model.Clear();
    Pump();
    Expect(Center.GetList()->count() == 0, "Clear empties the dock");
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);
    std::printf("NimbleEditorNotificationTests\n");

    EditorNotifications::Install();
    EditorNotifications& Model = *EditorNotifications::Get();

    {
        QMainWindow Window;
        Window.setCentralWidget(new QWidget(&Window));
        Window.resize(1200, 800);
        auto* Center = new NotificationCenter(&Window);
        Window.addDockWidget(Qt::BottomDockWidgetArea, Center);
        auto* Bell = new NotificationStatusItem(*Center, &Window);
        auto* Toasts = new NotificationToasts(Window);
        Window.show();
        Center->hide();   // seen, it marks everything read
        Pump();

        TestModel(Model);
        TestThreads(Model);
        TestToasts(Model, Window, *Toasts);
        TestCenterAndBell(Model, *Center, *Bell);
    }

    EditorNotifications::Uninstall();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

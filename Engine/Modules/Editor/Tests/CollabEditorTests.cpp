/*
 * NimbleEditorCollabTests -- two editors' collaboration sessions in one
 * process, offscreen, against a CollabServer on 127.0.0.1: joining, the
 * Session dock's roster, presence (open level, playing, unsaved), the overlap
 * warning, a refusal and leaving, as notifications.
 *
 * ConnectAs, not Connect: the identity is made here rather than asked of git.
 */

#include <cstdio>
#include <functional>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTreeWidget>

#include "Public/Collab/CollabServer.h"
#include "Public/Editor/EditorNotifications.h"
#include "Private/Editor/Collab/CollabSessionWindow.h"
#include "Private/Editor/Collab/EditorCollabSession.h"

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

static bool PumpUntil(Collab::CollabServer& InServer, const std::function<bool()>& InDone, const int InMs = 5000)
{
    QElapsedTimer Clock;
    Clock.start();
    while(Clock.elapsed() < InMs)
    {
        InServer.Poll();
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if(InDone())
        {
            return true;
        }
    }
    return InDone();
}

static Collab::ClientIdentity Identity(const char* InUser, const char* InEngine = "aaaaaaaaaaaa1111")
{
    Collab::ClientIdentity Out;
    Out.User       = InUser;
    Out.ProjectId  = Collab::MakeProjectId("WorldsLab", "https://github.com/Nimble/WorldsLab.git");
    Out.Branch     = "collab/forest";
    Out.Head       = "1234567890123456789012345678901234567890";
    Out.EngineTree = InEngine;
    return Out;
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);
    std::printf("NimbleEditorCollabTests\n");
    EditorNotifications::Install();
    EditorNotifications& Notes = *EditorNotifications::Get();

    Collab::CollabServer Server;
    Collab::ServerConfig Config;
    Config.Port = 0;
    Expect(Server.Start(Config), "a server");
    const uint16_t Port = Server.GetPort();

    {
        /* Not repositories: paths pass through as given. */
        EditorCollabSession Alice(QDir::tempPath(), QString());
        EditorCollabSession Bob(QDir::tempPath(), QString());
        CollabSessionWindow AliceDock(Alice, nullptr);

        std::printf("Join\n");
        Alice.ConnectAs(QStringLiteral("127.0.0.1"), Port, Identity("alice"));
        Expect(Alice.GetState() == Collab::CollabClient::EState::Connecting, "connecting");
        Expect(PumpUntil(Server, [&]() { return Alice.IsJoined(); }), "Alice joins");
        const EditorNotifications::Notification* State = Notes.FindByKey(QStringLiteral("collab.state"));
        Expect(State && State->Severity == EditorNotifications::ESeverity::Success, "a success toast says so");

        Bob.ConnectAs(QStringLiteral("127.0.0.1"), Port, Identity("bob"));
        Expect(PumpUntil(Server, [&]() { return Bob.IsJoined() && Alice.GetClient().GetRoster().size() == 2; }), "Bob joins; Alice sees him");
        const EditorNotifications::Notification* Roster = Notes.FindByKey(QStringLiteral("collab.roster"));
        Expect(Roster && Roster->Title.contains(QStringLiteral("bob")), "\"bob joined the session\"");
        Expect(AliceDock.GetRoster()->topLevelItemCount() == 2, "the Session dock lists both");

        std::printf("Presence\n");
        const QString Level = QStringLiteral("Games/WorldsLab/Content/Levels/Forest.nmworld");
        Alice.SetOpenLevel(Level);
        Alice.SetPlaying(true);
        Expect(PumpUntil(Server, [&]()
        {
            const auto& Members = Bob.GetClient().GetRoster();
            for(const auto& Each : Members)
            {
                if(Each.second.User == "alice")
                {
                    return Each.second.OpenLevel == Level.toStdString() && Each.second.Play == Collab::EPlayState::Playing;
                }
            }
            return false;
        }), "Bob sees Alice's level, playing");
        Expect(Bob.DescribeRoster().join(QLatin1Char('\n')).contains(QStringLiteral("playing Forest.nmworld")), "collab.status says so");

        std::printf("Overlap\n");
        Alice.SetPlaying(false);
        Alice.SetLevelUnsaved(true);
        Bob.SetOpenLevel(Level);
        Bob.SetLevelUnsaved(true);
        const QString OverlapKey = QStringLiteral("collab.overlap.") + Level;
        Expect(PumpUntil(Server, [&]() { return Notes.FindByKey(OverlapKey) != nullptr; }), "both with unsaved edits to one level: a warning");
        const EditorNotifications::Notification* Overlap = Notes.FindByKey(OverlapKey);
        Expect(Overlap && Overlap->Severity == EditorNotifications::ESeverity::Warning, "a warning");
        Expect(PumpUntil(Server, [&]() { return Bob.GetOthersDirty().size() == 1; }), "GetOthersDirty");
        const QHash<QString, QStringList> Others = Bob.GetOthersDirty();
        Expect(!Others.isEmpty() && Others.begin().value() == QStringList{ QStringLiteral("alice") }, "names who");

        std::printf("Refused\n");
        EditorCollabSession Carol(QDir::tempPath(), QString());
        Carol.ConnectAs(QStringLiteral("127.0.0.1"), Port, Identity("carol", "zzzzzzzzzzzz9999"));
        Expect(PumpUntil(Server, [&]()
        {
            const EditorNotifications::Notification* Last = Notes.FindByKey(QStringLiteral("collab.state"));
            return Last && Last->Title.contains(QStringLiteral("refused"));
        }), "another engine: \"The session refused this editor\"");
        Expect(PumpUntil(Server, [&]() { return Carol.GetState() == Collab::CollabClient::EState::Idle; }), "and it's out");

        std::printf("Leave\n");
        Alice.Leave();
        Expect(PumpUntil(Server, [&]() { return !Alice.IsJoined() && Bob.GetClient().GetRoster().size() == 1; }), "Alice leaves; Bob sees");
        Roster = Notes.FindByKey(QStringLiteral("collab.roster"));
        Expect(Roster && Roster->Title.contains(QStringLiteral("alice left")), "\"alice left the session\"");
        Expect(Bob.GetOthersDirty().isEmpty(), "and her changes are no longer listed");
        Expect(AliceDock.GetRoster()->topLevelItemCount() == 0, "her dock empties");
        Bob.Leave();
        PumpUntil(Server, [&]() { return Server.GetSessions().empty(); });
    }

    Server.Stop();
    EditorNotifications::Uninstall();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

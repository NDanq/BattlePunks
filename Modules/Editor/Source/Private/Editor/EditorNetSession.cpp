#include "Private/Editor/EditorNetSession.h"

#if WITH_EDITOR && WITH_NETCORE

#include <QSysInfo>
#include <QTimer>

#include "Public/Editor/EditorConsole.h"
#include "Public/Editor/EditorDocument.h"

/* STUB(NetCore-Editor) -- see EditorNetSession.h. */

namespace
{
    constexpr uint16_t EDITOR_LINK_DEFAULT_PORT = 7787;

    /* "host[:port]" into its halves; false if the port is not a number. */
    bool SplitAddress(const QString& InText, QString& OutHost, uint16_t& OutPort)
    {
        const int Colon = InText.lastIndexOf(QLatin1Char(':'));
        OutHost = Colon < 0 ? InText : InText.left(Colon);
        OutPort = EDITOR_LINK_DEFAULT_PORT;
        if(Colon < 0) { return !OutHost.isEmpty(); }
        bool bOk = false;
        const uint Port = InText.mid(Colon + 1).toUInt(&bOk);
        OutPort = uint16_t(Port);
        return bOk && Port > 0 && Port <= 0xFFFF && !OutHost.isEmpty();
    }
}

EditorNetSession::EditorNetSession(EditorDocument& InDocument, EditorConsole& InConsole)
    : Document(InDocument)
    , Console(InConsole)
{
    Link.SetUserName(QSysInfo::machineHostName().toUtf8().constData());
    Link.SetCommandHandler(&EditorNetSession::OnRemoteCommand, this);

    /* The link is polled, like a game session; 30 times a second is plenty
     * for people typing. Only while it is open. */
    PollTimer = new QTimer();
    PollTimer->setInterval(33);
    QObject::connect(PollTimer, &QTimer::timeout, PollTimer, [this] { Link.Poll(); });

    QObject::connect(&Document, &EditorDocument::CommandRecorded, PollTimer,
                     [this](const QString& InText) { OnCommandRecorded(InText); });

    RegisterCommands();
}

EditorNetSession::~EditorNetSession()
{
    delete PollTimer;   // and with it, both connections made on it
    Link.Leave();
}

void EditorNetSession::RegisterCommands()
{
    Console.Register(QStringLiteral("net.host"), QStringLiteral("[port]"),
                     QStringLiteral("Let other editors join this one (stub: edits are announced, not applied)"),
                     [this](const QStringList& InArgs)
    {
        bool bOk = true;
        const uint Port = InArgs.isEmpty() ? EDITOR_LINK_DEFAULT_PORT : InArgs.first().toUInt(&bOk);
        if(!bOk || Port == 0 || Port > 0xFFFF)
        {
            Console.PrintError(QStringLiteral("net.host: '%1' is not a port").arg(InArgs.first()));
            return;
        }
        if(!Link.Host(uint16_t(Port)))
        {
            Console.PrintError(QStringLiteral("net.host: could not listen on port %1").arg(Port));
            return;
        }
        PollTimer->start();
        Console.Print(QStringLiteral("Hosting on port %1").arg(Link.GetPort()));
    });

    Console.Register(QStringLiteral("net.join"), QStringLiteral("<host[:port]>"),
                     QStringLiteral("Join another editor (stub: edits are announced, not applied)"),
                     [this](const QStringList& InArgs)
    {
        QString  Host;
        uint16_t Port = 0;
        if(InArgs.isEmpty() || !SplitAddress(InArgs.first(), Host, Port))
        {
            Console.PrintError(QStringLiteral("net.join: expected host[:port]"));
            return;
        }
        if(!Link.Join(Host.toUtf8().constData(), Port))
        {
            Console.PrintError(QStringLiteral("net.join: could not reach %1:%2").arg(Host).arg(Port));
            return;
        }
        PollTimer->start();
        Console.Print(QStringLiteral("Joining %1:%2").arg(Host).arg(Port));
    });

    Console.Register(QStringLiteral("net.leave"), QString(), QStringLiteral("Leave or stop hosting the editor link"),
                     [this](const QStringList&)
    {
        Link.Leave();
        PollTimer->stop();
        Console.Print(QStringLiteral("Editor link closed"));
    });

    Console.Register(QStringLiteral("net.status"), QString(), QStringLiteral("The editor link's state"),
                     [this](const QStringList&)
    {
        if(!Link.IsActive())   { Console.Print(QStringLiteral("Editor link: not connected")); }
        else if(Link.IsHosting()) { Console.Print(QStringLiteral("Editor link: hosting on port %1, %2 editor(s) joined").arg(Link.GetPort()).arg(Link.GetPeerCount())); }
        else                   { Console.Print(QStringLiteral("Editor link: joined (%1 connection)").arg(Link.GetPeerCount())); }
    });
}

void EditorNetSession::OnCommandRecorded(const QString& InText)
{
    if(Link.IsActive()) { Link.SendCommand(InText.toUtf8().constData()); }
}

void EditorNetSession::OnRemoteCommand(const char* InUser, const char* InText, void* InUserData)
{
    EditorNetSession& Self = *static_cast<EditorNetSession*>(InUserData);
    /* STUB(NetCore-Editor): reported, not applied. */
    Self.Console.Print(QStringLiteral("[%1] %2").arg(QString::fromUtf8(InUser), QString::fromUtf8(InText)));
}

#endif // WITH_EDITOR && WITH_NETCORE

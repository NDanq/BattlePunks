#if WITH_EDITOR

#include "Private/Editor/Build/StartupCook.h"

#include <string>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QSplashScreen>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorNotifications.h"
#include "Public/NimbleEngine/ShaderCook.h"

#include "Private/Editor/Build/ContentCook.h"
#include "Private/Editor/Graph/GraphAssets.h"

/* Run NimbleCook before the editor opens, so the shaders it draws with are
 * what the sources say (EditorStartupCook::Run). */
NIMBLE_CONFIG_BOOL(GEditorCookShadersOnStartup, "Editor", "CookShadersOnStartup", true, nullptr);

namespace
{
    /* A splash a stray click does not dismiss: it goes when the cook is done. */
    class CookSplash final : public QSplashScreen
    {
    public:
        using QSplashScreen::QSplashScreen;

    protected:
        void mousePressEvent(QMouseEvent*) override {}
    };

    /* The application icon on the editor's dark background, its name below. */
    QPixmap MakeSplashPixmap()
    {
        const qreal Ratio = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
        QPixmap Pixmap(QSize(520, 300) * Ratio);
        Pixmap.setDevicePixelRatio(Ratio);
        Pixmap.fill(QColor(0x1b, 0x1d, 0x22));

        QPainter Painter(&Pixmap);
        Painter.setRenderHint(QPainter::Antialiasing);
        Painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QPixmap Icon = QGuiApplication::windowIcon().pixmap(QSize(112, 112) * Ratio);
        Painter.drawPixmap(QRect(204, 40, 112, 112), Icon);

        QFont Title = Painter.font();
        Title.setPointSize(18);
        Title.setBold(true);
        Painter.setFont(Title);
        Painter.setPen(QColor(0xe6, 0xe8, 0xec));
        Painter.drawText(QRect(0, 164, 520, 36), Qt::AlignCenter, QCoreApplication::applicationName());
        Painter.end();
        return Pixmap;
    }
}

namespace EditorStartupCook
{
    void Run(const RenderCore::ERenderBackend InBackend)
    {
        const QString Cook = EditorCook::GetCookerPath();
        if(!GEditorCookShadersOnStartup.GetBool() || InBackend == RenderCore::ERenderBackend::Null)
        {
            return;
        }
        if(Cook.isEmpty())
        {
            CORE_LOG_WARN("[Shaders] no NimbleCook beside the editor: the shaders are what the build last staged")
            return;
        }

        const QString     Root = EditorGraphs::GetProjectRoot();
        const QStringList Args = EditorCook::GetCookerArgs(Root);
        CORE_LOG("[Shaders] cooking what changed: " + Cook.toStdString() + " " + Args.join(QLatin1Char(' ')).toStdString())

        CookSplash Splash(MakeSplashPixmap());
        QString    Phase = QStringLiteral("Checking shaders");
        QString    Detail;
        const auto Show = [&Splash, &Phase, &Detail]()
        {
            Splash.showMessage(Phase + QStringLiteral("...\n") + Detail, Qt::AlignHCenter | Qt::AlignBottom, QColor(0x9a, 0xa0, 0xab));
        };
        Splash.show();
        Show();

        /* "NimbleCook: cooking <what> <variant> (...)": something to cook;
         * then, as each of its runs ends (several at once),
         * "NimbleCook: [3/15] done: Engine Runtime for Vulkan" and that run's lines. */
        static const QString           Cooking = QStringLiteral("NimbleCook: cooking ");
        static const QRegularExpression Ended(QStringLiteral("^NimbleCook: \\[(\\d+)/(\\d+)\\] (\\w+): (.*)$"));
        const auto Take = [&Phase, &Detail, &Show](const QString& InLine)
        {
            const QString Line = InLine.trimmed();
            if(Line.isEmpty()) { return; }
            CORE_LOG("[Shaders] " + Line.toStdString())
            const QRegularExpressionMatch Match = Ended.match(Line);
            if(Match.hasMatch())
            {
                Phase  = QStringLiteral("Compiling shaders (%1 of %2)").arg(Match.captured(1), Match.captured(2));
                Detail = QStringLiteral("%1 %2").arg(Match.captured(3) == QStringLiteral("done") ? QStringLiteral("Done:")
                                                                                                  : QStringLiteral("Failed:"),
                                                     Match.captured(4));
                Show();
            }
            else if(Line.startsWith(Cooking))
            {
                Phase = QStringLiteral("Compiling shaders");
                Show();
            }
        };

        QProcess Process;
        QEventLoop Loop;
        bool       bOk = false;
        QString    Why;
        Process.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&Process, &QProcess::readyRead, &Loop, [&Process, &Take]()
        {
            while(Process.canReadLine())
            {
                Take(QString::fromLocal8Bit(Process.readLine()));
            }
        });
        QObject::connect(&Process, &QProcess::finished, &Loop, [&](const int InExitCode, const QProcess::ExitStatus InStatus)
        {
            for(const QString& Line : QString::fromLocal8Bit(Process.readAll()).split(QLatin1Char('\n')))
            {
                Take(Line);
            }
            bOk = InStatus == QProcess::NormalExit && InExitCode == 0;
            Why = QStringLiteral("exit code %1").arg(InExitCode);
            Loop.quit();
        });
        QObject::connect(&Process, &QProcess::errorOccurred, &Loop, [&](const QProcess::ProcessError InError)
        {
            if(InError == QProcess::FailedToStart)
            {
                Why = Process.errorString();
                Loop.quit();
            }
        });
        Process.start(Cook, Args);
        Loop.exec(QEventLoop::ExcludeUserInputEvents);

        if(!bOk)
        {
            CORE_LOG_WARN("[Shaders] NimbleCook failed (" + Why.toStdString() + "); its lines above say why. "
                          "The editor draws with what was cooked before")
            if(EditorNotifications* Notifications = EditorNotifications::Get())
            {
                EditorNotifications::Notification Failed;
                Failed.Severity = EditorNotifications::ESeverity::Warning;
                Failed.Title    = QStringLiteral("Shader cook failed");
                Failed.Body     = QStringLiteral("NimbleCook did not cook every shader (%1). The Console's [Shaders] lines say why; "
                                                 "the editor draws with what was cooked before.").arg(Why);
                Failed.Source   = QStringLiteral("Shaders");
                Notifications->Post(Failed);
            }
        }

        /* What is newer beside the editor, where its file provider reads first. */
        Phase = QStringLiteral("Staging shaders");
        Detail.clear();
        Show();
        const int Copied = NimbleEngine::ShaderCook::RestageCookedShaders(QCoreApplication::applicationDirPath().toStdString(),
                                                                         EditorCook::GetEngineDir().toStdString(),
                                                                         Root.toStdString(), true);
        if(Copied > 0)
        {
            CORE_LOG("[Shaders] " + std::to_string(Copied) + " cooked file(s) copied beside the editor")
        }
        Splash.close();
    }
}

#endif // WITH_EDITOR

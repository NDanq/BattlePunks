#if WITH_EDITOR

#include "Private/Editor/SourceControl/GitClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>

#include "Public/Core/CoreLog.h"

namespace
{
    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }
}

QString GitClient::Result::Summary() const
{
    if(!bStarted)
    {
        return QStringLiteral("git didn't start. Is it installed and on PATH?");
    }
    const QString Text = Err.isEmpty() ? QString::fromUtf8(Out).trimmed() : Err;
    /* git's own verdict first: progress ("From ...") comes before it. */
    for(const QString& Line : Text.split(QLatin1Char('\n')))
    {
        const QString Trimmed = Line.trimmed();
        if(Trimmed.startsWith(QStringLiteral("error:")) || Trimmed.startsWith(QStringLiteral("fatal:")))
        {
            return Trimmed;
        }
    }
    for(const QString& Line : Text.split(QLatin1Char('\n')))
    {
        const QString Trimmed = Line.trimmed();
        /* "hint:" lines say what to try, not what went wrong. */
        if(!Trimmed.isEmpty() && !Trimmed.startsWith(QStringLiteral("hint:")))
        {
            return Trimmed;
        }
    }
    return QStringLiteral("git exited with code %1.").arg(ExitCode);
}

GitClient::GitClient(const QString& InRepositoryRoot, QObject* InParent)
    : QObject(InParent)
    , Root(InRepositoryRoot)
{
}

GitClient::~GitClient()
{
    Queue.clear();
    if(Process)
    {
        Process->disconnect(this);
        Process->kill();
        Process->waitForFinished(2000);
        delete Process;
        Process = nullptr;
    }
}

QString GitClient::FindRepositoryRoot(const QString& InDir)
{
    QDir Dir(InDir);
    if(InDir.isEmpty() || !Dir.exists())
    {
        return QString();
    }
    Dir.makeAbsolute();
    while(true)
    {
        if(QFileInfo::exists(Dir.filePath(QStringLiteral(".git"))))
        {
            return Dir.absolutePath();
        }
        if(!Dir.cdUp())
        {
            return QString();
        }
    }
}

QString GitClient::GetGitCommand()
{
    return QStringLiteral("git");
}

void GitClient::Run(const QStringList& InArgs, Callback InCallback, const QByteArray& InStdin)
{
    Job New;
    New.Args  = InArgs;
    New.Stdin = InStdin;
    New.Done  = std::move(InCallback);
    Enqueue(std::move(New));
}

void GitClient::RunQuiet(const QStringList& InArgs, Callback InCallback)
{
    Job New;
    New.Args   = InArgs;
    New.Done   = std::move(InCallback);
    New.bQuiet = true;
    Enqueue(std::move(New));
}

void GitClient::Enqueue(Job InJob)
{
    if(!IsBusy())
    {
        emit BusyChanged(true);
    }
    Queue.push_back(std::move(InJob));
    if(!Process)
    {
        StartNext();
    }
}

void GitClient::StartNext()
{
    while(!Queue.empty() && !Process)
    {
        Current = std::move(Queue.front());
        Queue.pop_front();

        Process = new QProcess(this);
        Process->setWorkingDirectory(Root);
        QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
        Env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
        /* status may not take the index lock: the user's own git can run beside it. */
        Env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
        Process->setProcessEnvironment(Env);
        connect(Process, &QProcess::finished, this, &GitClient::OnFinished);

        Process->start(GetGitCommand(), Current.Args);
        if(!Process->waitForStarted(10000))
        {
            CORE_LOG_ERROR("[Git] couldn't start git " + ToStd(Current.Args.join(QLatin1Char(' '))) + ": " + ToStd(Process->errorString()))
            Process->disconnect(this);
            delete Process;
            Process = nullptr;

            Result Failed;
            Failed.bStarted = false;
            const Callback Done = std::move(Current.Done);
            Current = Job();
            if(Done)
            {
                Done(Failed);
            }
            continue;
        }
        if(!Current.Stdin.isEmpty())
        {
            Process->write(Current.Stdin);
        }
        Process->closeWriteChannel();
        return;
    }

    if(!Process && Queue.empty())
    {
        emit BusyChanged(false);
    }
}

void GitClient::OnFinished(const int InExitCode, const QProcess::ExitStatus InStatus)
{
    Result Done;
    Done.bStarted = true;
    Done.ExitCode = InStatus == QProcess::NormalExit ? InExitCode : -1;
    Done.Out      = Process->readAllStandardOutput();
    Done.Err      = QString::fromUtf8(Process->readAllStandardError()).trimmed();

    const QString Command = QStringLiteral("git ") + Current.Args.join(QLatin1Char(' '));
    if(Done.Ok() || Current.bQuiet)
    {
        CORE_LOG("[Git] " + ToStd(Command) + (Done.Ok() ? std::string() : " -> " + std::to_string(Done.ExitCode)))
    }
    else
    {
        CORE_LOG_WARN("[Git] " + ToStd(Command) + " failed (" + std::to_string(Done.ExitCode) + "): " + ToStd(Done.Err))
    }

    Process->deleteLater();
    Process = nullptr;
    const Callback Finished = std::move(Current.Done);
    Current = Job();
    if(Finished)
    {
        Finished(Done);
    }
    StartNext();
}

bool GitClient::WaitForIdle(const int InTimeoutMs)
{
    QElapsedTimer Clock;
    Clock.start();
    while(IsBusy() && Clock.elapsed() < InTimeoutMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if(Process)
        {
            Process->waitForFinished(20);
        }
    }
    QCoreApplication::processEvents();
    return !IsBusy();
}

// --- The dock's operations ----------------------------------------------------------

void GitClient::Status(std::function<void(bool, const EditorGit::Status&, const QString&)> InCallback)
{
    Run({ QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("--branch"), QStringLiteral("-z"),
          QStringLiteral("--untracked-files=all") },
        [InCallback](const Result& InResult)
        {
            EditorGit::Status Parsed;
            if(!InResult.Ok())
            {
                InCallback(false, Parsed, InResult.Summary());
                return;
            }
            std::string Error;
            const std::string Output(InResult.Out.constData(), size_t(InResult.Out.size()));
            if(!EditorGit::ParseStatusV2(Output, Parsed, &Error))
            {
                InCallback(false, EditorGit::Status(), QString::fromStdString(Error));
                return;
            }
            InCallback(true, Parsed, QString());
        });
}

void GitClient::Stage(const QStringList& InPaths, Callback InCallback)
{
    if(InPaths.isEmpty())
    {
        if(InCallback) { Result Nothing; Nothing.bStarted = true; Nothing.ExitCode = 0; InCallback(Nothing); }
        return;
    }
    Run(QStringList{ QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--") } + InPaths, std::move(InCallback));
}

void GitClient::Unstage(const QStringList& InPaths, const bool bInInitial, Callback InCallback)
{
    if(InPaths.isEmpty())
    {
        if(InCallback) { Result Nothing; Nothing.bStarted = true; Nothing.ExitCode = 0; InCallback(Nothing); }
        return;
    }
    /* Before the first commit there is no HEAD to restore from. */
    const QStringList Args = bInInitial
        ? QStringList{ QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("--") }
        : QStringList{ QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--") };
    Run(Args + InPaths, std::move(InCallback));
}

void GitClient::Discard(const QStringList& InTracked, const QStringList& InUntracked, const bool bInInitial, Callback InCallback)
{
    if(InTracked.isEmpty() && InUntracked.isEmpty())
    {
        if(InCallback) { Result Nothing; Nothing.bStarted = true; Nothing.ExitCode = 0; InCallback(Nothing); }
        return;
    }
    const QStringList Clean = { QStringLiteral("clean"), QStringLiteral("-f"), QStringLiteral("-q"), QStringLiteral("--") };
    if(InTracked.isEmpty())
    {
        Run(Clean + InUntracked, std::move(InCallback));
        return;
    }
    /* Before the first commit there is no HEAD: what is staged is new, and goes. */
    const QStringList Args = bInInitial
        ? QStringList{ QStringLiteral("rm"), QStringLiteral("-f"), QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("--") }
        : QStringList{ QStringLiteral("restore"), QStringLiteral("--source=HEAD"), QStringLiteral("--staged"), QStringLiteral("--worktree"),
                       QStringLiteral("--") };
    if(InUntracked.isEmpty())
    {
        Run(Args + InTracked, std::move(InCallback));
        return;
    }
    /* The untracked go only if the tracked did: one answer, the first failure. */
    Run(Args + InTracked, [this, Clean, InUntracked, InCallback](const Result& InResult)
    {
        if(!InResult.Ok())
        {
            if(InCallback) { InCallback(InResult); }
            return;
        }
        Run(Clean + InUntracked, InCallback);
    });
}

void GitClient::Commit(const QString& InMessage, Callback InCallback)
{
    Run({ QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-") }, std::move(InCallback), InMessage.toUtf8());
}

void GitClient::Fetch(Callback InCallback)
{
    Run({ QStringLiteral("fetch"), QStringLiteral("--prune") }, std::move(InCallback));
}

void GitClient::Pull(const bool bInRebase, Callback InCallback)
{
    /* A rebase sets local edits aside and puts them back (--autostash). */
    const QStringList Args = bInRebase
        ? QStringList{ QStringLiteral("pull"), QStringLiteral("--rebase"), QStringLiteral("--autostash") }
        : QStringList{ QStringLiteral("pull"), QStringLiteral("--ff-only") };
    Run(Args, std::move(InCallback));
}

void GitClient::Push(const bool bInSetUpstream, Callback InCallback)
{
    const QStringList Args = bInSetUpstream
        ? QStringList{ QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("HEAD") }
        : QStringList{ QStringLiteral("push") };
    Run(Args, std::move(InCallback));
}

void GitClient::Diff(const QString& InPath, const bool bInStaged, Callback InCallback)
{
    QStringList Args = { QStringLiteral("diff"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("-M") };
    if(bInStaged)
    {
        Args << QStringLiteral("--cached");
    }
    Args << QStringLiteral("--") << InPath;
    Run(Args, std::move(InCallback));
}

void GitClient::Branches(std::function<void(const QStringList&)> InCallback)
{
    Run({ QStringLiteral("for-each-ref"), QStringLiteral("--format=%(refname:short)"), QStringLiteral("refs/heads") },
        [InCallback](const Result& InResult)
        {
            QStringList Names;
            if(InResult.Ok())
            {
                for(const QString& Line : QString::fromUtf8(InResult.Out).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                {
                    Names << Line.trimmed();
                }
            }
            InCallback(Names);
        });
}

void GitClient::SwitchBranch(const QString& InBranch, const bool bInCreate, Callback InCallback)
{
    QStringList Args = { QStringLiteral("switch") };
    if(bInCreate)
    {
        Args << QStringLiteral("-c");
    }
    Args << InBranch;
    Run(Args, std::move(InCallback));
}

bool GitClient::UsesLfs() const
{
    QFile Attributes(QDir(Root).filePath(QStringLiteral(".gitattributes")));
    return Attributes.open(QIODevice::ReadOnly) && Attributes.readAll().contains("filter=lfs");
}

#endif // WITH_EDITOR

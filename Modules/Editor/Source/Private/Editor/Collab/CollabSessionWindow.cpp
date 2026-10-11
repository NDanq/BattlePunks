#if WITH_EDITOR

#include "Private/Editor/Collab/CollabSessionWindow.h"

#include <algorithm>

#include <QApplication>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Private/Editor/Collab/EditorCollabSession.h"
#include "Private/Editor/SettingsDialog.h"

NIMBLE_CONFIG_STRING(GEditorCollabServer,   "Editor", "CollabServer",   "127.0.0.1:7788", nullptr);
NIMBLE_CONFIG_STRING(GEditorCollabUserName, "Editor", "CollabUserName", "",               nullptr);

namespace
{
    QString ToQ(const std::string& InText)
    {
        return QString::fromUtf8(InText.c_str(), int(InText.size()));
    }
}

CollabSessionWindow::CollabSessionWindow(EditorCollabSession& InSession, QWidget* InParent)
    : QDockWidget(QStringLiteral("Session"), InParent)
    , Session(InSession)
{
    setObjectName(QStringLiteral("CollabSessionDock"));

    auto* Body = new QWidget(this);
    auto* Column = new QVBoxLayout(Body);
    Column->setContentsMargins(6, 6, 6, 6);
    Column->setSpacing(6);

    auto* Form = new QFormLayout();
    Address = new QLineEdit(QString::fromUtf8(GEditorCollabServer.GetString()), Body);
    Address->setPlaceholderText(QStringLiteral("192.168.1.20:7788"));
    Address->setToolTip(QStringLiteral("Where NimbleEditorServer runs: host or host:port (UDP, 7788 by default)"));
    Form->addRow(QStringLiteral("Server"), Address);
    UserName = new QLineEdit(QString::fromUtf8(GEditorCollabUserName.GetString()), Body);
    UserName->setPlaceholderText(QStringLiteral("Your git user.name"));
    UserName->setToolTip(QStringLiteral("How the others see you; empty uses git's user.name"));
    Form->addRow(QStringLiteral("Name"), UserName);
    Password = new QLineEdit(Body);
    Password->setEchoMode(QLineEdit::Password);
    Password->setPlaceholderText(QStringLiteral("If the server has one"));
    Password->setToolTip(QStringLiteral("The session password; never saved"));
    Form->addRow(QStringLiteral("Password"), Password);
    Column->addLayout(Form);

    auto* Buttons = new QHBoxLayout();
    StateLabel = new QLabel(Body);
    StateLabel->setWordWrap(true);
    Buttons->addWidget(StateLabel, 1);
    JoinButton = new QPushButton(QStringLiteral("Join"), Body);
    JoinButton->setAutoDefault(false);
    LeaveButton = new QPushButton(QStringLiteral("Leave"), Body);
    LeaveButton->setAutoDefault(false);
    Buttons->addWidget(JoinButton);
    Buttons->addWidget(LeaveButton);
    Column->addLayout(Buttons);

    BuildLabel = new QLabel(Body);
    BuildLabel->setObjectName(QStringLiteral("CollabBuild"));
    BuildLabel->setWordWrap(true);
    BuildLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Column->addWidget(BuildLabel);

    Roster = new QTreeWidget(Body);
    Roster->setObjectName(QStringLiteral("CollabRoster"));
    Roster->setColumnCount(4);
    Roster->setHeaderLabels({ QStringLiteral("Who"), QStringLiteral("Level"), QStringLiteral("HEAD"), QStringLiteral("Changed") });
    Roster->setRootIsDecorated(false);
    Roster->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Roster->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    Column->addWidget(Roster, 1);
    setWidget(Body);

    connect(JoinButton, &QPushButton::clicked, this, [this]()
    {
        Session.Connect(Address->text(), UserName->text(), Password->text());
    });
    connect(Address, &QLineEdit::returnPressed, JoinButton, &QPushButton::click);
    connect(Password, &QLineEdit::returnPressed, JoinButton, &QPushButton::click);
    connect(LeaveButton, &QPushButton::clicked, this, [this]() { Session.Leave(); });

    connect(&Session, &EditorCollabSession::StateChanged, this, [this]()
    {
        /* Joined: what worked is what's offered next time. */
        if(Session.IsJoined())
        {
            bool bServerChanged = false, bNameChanged = false;
            GEditorCollabServer.SetFromString(Address->text().trimmed().toStdString(), &bServerChanged);
            GEditorCollabUserName.SetFromString(UserName->text().trimmed().toStdString(), &bNameChanged);
            if(bServerChanged || bNameChanged)
            {
                SettingsDialog::SaveUserSettings();
            }
        }
        UpdateState();
    });
    connect(&Session, &EditorCollabSession::RosterChanged, this, &CollabSessionWindow::UpdateRoster);
    connect(&Session, &EditorCollabSession::BuildChanged, this, &CollabSessionWindow::UpdateBuild);
    connect(&Session, &EditorCollabSession::StateChanged, this, &CollabSessionWindow::UpdateBuild);

    UpdateState();
    UpdateRoster();
    UpdateBuild();
}

void CollabSessionWindow::UpdateState()
{
    using EState = Collab::CollabClient::EState;
    const EState State = Session.GetState();
    const bool bIdle = State == EState::Idle;
    JoinButton->setEnabled(bIdle);
    LeaveButton->setEnabled(!bIdle);
    for(QLineEdit* Each : { Address, UserName, Password })
    {
        Each->setEnabled(bIdle);
    }
    switch(State)
    {
        case EState::Idle:
            StateLabel->setText(Session.GetRepositoryRoot().isEmpty() ? QStringLiteral("This project isn't in a git repository.")
                                                                      : QStringLiteral("Not in a session."));
            break;
        case EState::Connecting:
            StateLabel->setText(QStringLiteral("Joining %1…").arg(Session.GetServerAddress()));
            break;
        case EState::Joined:
            StateLabel->setText(QStringLiteral("In the session at %1.").arg(Session.GetServerAddress()));
            break;
    }
}

void CollabSessionWindow::UpdateRoster()
{
    Roster->clear();
    const Collab::CollabClient& Client = Session.GetClient();
    if(!Client.IsJoined())
    {
        return;
    }
    const auto MineIt = Client.GetRoster().find(Client.GetMyId());
    for(const auto& Each : Client.GetRoster())
    {
        const Collab::MemberState& Member = Each.second;
        const bool bMe = Each.first == Client.GetMyId();
        auto* Item = new QTreeWidgetItem(Roster);
        Item->setText(0, ToQ(Member.User) + (bMe ? QStringLiteral(" (you)") : QString()));
        QString Level = Member.OpenLevel.empty() ? QStringLiteral("—") : QFileInfo(ToQ(Member.OpenLevel)).completeBaseName();
        if(Member.Play != Collab::EPlayState::Editing)
        {
            Level += QStringLiteral("  ▶ playing");
        }
        Item->setText(1, Level);
        Item->setToolTip(1, ToQ(Member.OpenLevel));
        Item->setText(2, ToQ(Member.Head).left(8));
        Item->setText(3, QString::number(Member.Dirty.size()));

        /* The changed files; those you've changed too, marked. */
        QStringList Lines;
        int Shared = 0;
        for(const std::string& Path : Member.Dirty)
        {
            const bool bAlsoMine = !bMe && MineIt != Client.GetRoster().end()
                                && std::binary_search(MineIt->second.Dirty.begin(), MineIt->second.Dirty.end(), Path);
            Shared += bAlsoMine ? 1 : 0;
            Lines << (bAlsoMine ? QStringLiteral("⚠ ") : QStringLiteral("   ")) + ToQ(Path);
        }
        if(Lines.size() > 40)
        {
            const int More = int(Lines.size()) - 40;
            Lines = Lines.mid(0, 40);
            Lines << QStringLiteral("… and %1 more").arg(More);
        }
        Item->setToolTip(3, Lines.isEmpty() ? QStringLiteral("Nothing changed") : Lines.join(QLatin1Char('\n')));
        if(Shared > 0)
        {
            Item->setForeground(3, QColor(230, 170, 60));
            Item->setText(3, QStringLiteral("%1 (%2 also yours)").arg(Member.Dirty.size()).arg(Shared));
        }
    }
}


void CollabSessionWindow::UpdateBuild()
{
    const Collab::BuildStatusMessage& Build = Session.GetClient().GetBuildStatus();
    const QString Short = ToQ(Build.Oid).left(8);
    switch(Session.IsJoined() ? Build.State : Collab::EBuildState::Idle)
    {
        case Collab::EBuildState::Idle:
            BuildLabel->setText(Session.IsJoined() ? QStringLiteral("Server build: none yet (the server runs no content worker, or it hasn't reported)")
                                                   : QString());
            break;
        case Collab::EBuildState::Building:
            BuildLabel->setText(QStringLiteral("Server build: building %12026").arg(Short));
            break;
        case Collab::EBuildState::Succeeded:
            BuildLabel->setText(QStringLiteral("Server build: %1 built, schema %2").arg(Short)
                                .arg(QString::number(qulonglong(Build.SchemaHash), 16).rightJustified(16, QLatin1Char('0'))));
            break;
        case Collab::EBuildState::Failed:
            BuildLabel->setText(QStringLiteral("Server build: %1 failed: %2").arg(Short, ToQ(Build.FirstError)));
            break;
    }
    BuildLabel->setVisible(!BuildLabel->text().isEmpty());
}

#endif // WITH_EDITOR

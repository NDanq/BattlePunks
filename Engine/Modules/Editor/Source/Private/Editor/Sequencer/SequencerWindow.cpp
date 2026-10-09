#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Private/Editor/Sequencer/SequencerAssets.h"
#include "Private/Editor/Sequencer/SequencerTab.h"

namespace
{
    /* The one dock; a QPointer, so a deleted one reads as null. */
    QPointer<SequencerWindow>& Instance()
    {
        static QPointer<SequencerWindow> Window;
        return Window;
    }
}

SequencerWindow& SequencerWindow::Install(QMainWindow& InHost, QDockWidget* InTabifyWith)
{
    if(Instance()) { return *Instance(); }
    auto* Dock = new SequencerWindow(&InHost);
    Instance() = Dock;
    InHost.addDockWidget(Qt::BottomDockWidgetArea, Dock);
    if(InTabifyWith) { InHost.tabifyDockWidget(InTabifyWith, Dock); }
    Dock->hide();
    return *Dock;
}

SequencerWindow& SequencerWindow::Get()
{
    /* No main window (a test): a floating dock of its own. */
    if(!Instance())
    {
        Instance() = new SequencerWindow();
        Instance()->setFloating(true);
        Instance()->resize(1500, 700);
    }
    return *Instance();
}

SequencerWindow* SequencerWindow::Find()
{
    return Instance();
}

SequencerWindow::SequencerWindow(QWidget* InParent)
    : QDockWidget(QStringLiteral("Sequencer"), InParent)
{
    setObjectName(QStringLiteral("SequencerDock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    Pages = new QStackedWidget(this);

    /* Nothing open: how to open something. */
    auto* Empty  = new QWidget(Pages);
    auto* Layout = new QVBoxLayout(Empty);
    Layout->addStretch(1);
    auto* Note = new QLabel(QStringLiteral("No sequence is open. Double-click a .nmseq in the Content Browser, or:"), Empty);
    Note->setAlignment(Qt::AlignCenter);
    Note->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(Note);
    auto* Buttons = new QHBoxLayout();
    Buttons->addStretch(1);
    auto* Open = new QPushButton(QStringLiteral("Open Sequence..."), Empty);
    auto* New  = new QPushButton(QStringLiteral("New Sequence..."), Empty);
    Buttons->addWidget(Open);
    Buttons->addWidget(New);
    Buttons->addStretch(1);
    Layout->addLayout(Buttons);
    Layout->addStretch(1);
    connect(Open, &QPushButton::clicked, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Sequence"), EditorSequencer::GetDefaultFolder(),
                                                          QStringLiteral("Sequences (*.nmseq)"));
        if(!Path.isEmpty()) { EditorSequencer::OpenSequence(Path, this); }
    });
    connect(New, &QPushButton::clicked, this, [this]() { EditorSequencer::CreateSequence(QString(), this); });
    Pages->addWidget(Empty);

    Tabs = new QTabWidget(Pages);
    Tabs->setObjectName(QStringLiteral("SequencerTabs"));
    Tabs->setDocumentMode(true);
    Tabs->setTabsClosable(true);
    Tabs->setMovable(true);
    Tabs->tabBar()->setExpanding(false);
    Tabs->tabBar()->installEventFilter(this);
    Pages->addWidget(Tabs);
    setWidget(Pages);

    connect(Tabs, &QTabWidget::currentChanged, this, &SequencerWindow::UpdateTitles);
    connect(Tabs, &QTabWidget::tabCloseRequested, this, [this](const int InIndex)
    {
        if(auto* Tab = qobject_cast<SequencerTab*>(Tabs->widget(InIndex))) { CloseTab(Tab); }
    });

    const auto Step = [this](const QList<QKeySequence>& InKeys, const int InBy)
    {
        auto* Action = new QAction(this);
        Action->setShortcuts(InKeys);
        Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(Action, &QAction::triggered, this, [this, InBy]() { StepTab(InBy); });
        addAction(Action);
    };
    Step({ QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown) }, 1);
    Step({ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageUp) }, -1);

    UpdateTitles();
}

void SequencerWindow::AddTab(SequencerTab* InTab)
{
    Tabs->addTab(InTab, InTab->GetTabTitle());
    connect(InTab, &SequencerTab::TitleChanged, this, &SequencerWindow::UpdateTitles);
    connect(InTab, &SequencerTab::CloseRequested, this, [this, InTab]() { CloseTab(InTab); });
    ShowTab(InTab);
}

void SequencerWindow::ShowTab(SequencerTab* InTab)
{
    Tabs->setCurrentWidget(InTab);
    UpdateTitles();
    show();
    raise();
    if(isFloating()) { activateWindow(); }
}

bool SequencerWindow::CloseTab(SequencerTab* InTab)
{
    const int Index = Tabs->indexOf(InTab);
    if(Index < 0 || !InTab->ConfirmClose()) { return false; }
    Tabs->removeTab(Index);
    delete InTab;
    UpdateTitles();
    return true;
}

bool SequencerWindow::CloseAllTabs()
{
    /* Every sequence is asked before any goes, so a Cancel loses nothing. */
    for(SequencerTab* Tab : GetTabs())
    {
        if(!Tab->ConfirmClose()) { return false; }
    }
    for(SequencerTab* Tab : GetTabs())
    {
        Tabs->removeTab(Tabs->indexOf(Tab));
        delete Tab;
    }
    UpdateTitles();
    return true;
}

QList<SequencerTab*> SequencerWindow::GetTabs() const
{
    QList<SequencerTab*> Result;
    for(int Index = 0; Index < Tabs->count(); ++Index)
    {
        if(auto* Tab = qobject_cast<SequencerTab*>(Tabs->widget(Index))) { Result << Tab; }
    }
    return Result;
}

SequencerTab* SequencerWindow::CurrentTab() const
{
    return qobject_cast<SequencerTab*>(Tabs->currentWidget());
}

void SequencerWindow::UpdateTitles()
{
    for(SequencerTab* Tab : GetTabs())
    {
        const int Index = Tabs->indexOf(Tab);
        Tabs->setTabText(Index, Tab->GetTabTitle());
        Tabs->setTabToolTip(Index, Tab->GetPath());
    }
    Pages->setCurrentIndex(Tabs->count() > 0 ? 1 : 0);

    const SequencerTab* Current = CurrentTab();
    setWindowTitle(Current ? QStringLiteral("Sequencer - %1").arg(Current->GetTabTitle()) : QStringLiteral("Sequencer"));
}

void SequencerWindow::StepTab(const int InBy)
{
    const int Count = Tabs->count();
    if(Count > 1) { Tabs->setCurrentIndex((Tabs->currentIndex() + InBy + Count) % Count); }
}

void SequencerWindow::closeEvent(QCloseEvent* InEvent)
{
    /* The dock stays, hidden; its sequences close, the level put back. */
    if(!CloseAllTabs())
    {
        InEvent->ignore();
        return;
    }
    QDockWidget::closeEvent(InEvent);
}

bool SequencerWindow::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* A middle click on a tab closes it, as in a browser. */
    if(InWatched == Tabs->tabBar() && InEvent->type() == QEvent::MouseButtonRelease)
    {
        const auto* Mouse = static_cast<QMouseEvent*>(InEvent);
        if(Mouse->button() == Qt::MiddleButton)
        {
            const int Index = Tabs->tabBar()->tabAt(Mouse->position().toPoint());
            if(auto* Tab = qobject_cast<SequencerTab*>(Tabs->widget(Index))) { CloseTab(Tab); }
            return true;
        }
    }
    return QDockWidget::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR

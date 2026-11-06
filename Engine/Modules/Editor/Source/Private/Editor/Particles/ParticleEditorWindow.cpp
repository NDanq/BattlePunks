#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleEditorWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>

#include "Private/Editor/Particles/ParticleEditorTab.h"

namespace
{
    /* The one window; a QPointer, so a closed (deleted) one reads as null. */
    QPointer<ParticleEditorWindow>& Instance()
    {
        static QPointer<ParticleEditorWindow> Window;
        return Window;
    }
}

ParticleEditorWindow& ParticleEditorWindow::Get()
{
    /* Top-level and unowned: the particle window may sit behind the main one. */
    if(!Instance()) { Instance() = new ParticleEditorWindow(); }
    return *Instance();
}

ParticleEditorWindow* ParticleEditorWindow::Find()
{
    return Instance();
}

ParticleEditorWindow::ParticleEditorWindow(QWidget* InParent)
    : QMainWindow(InParent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 850);

    Tabs = new QTabWidget(this);
    Tabs->setObjectName(QStringLiteral("ParticleTabs"));
    Tabs->setDocumentMode(true);
    Tabs->setTabsClosable(true);
    Tabs->setMovable(true);
    Tabs->tabBar()->setExpanding(false);
    Tabs->tabBar()->installEventFilter(this);
    setCentralWidget(Tabs);

    connect(Tabs, &QTabWidget::currentChanged, this, &ParticleEditorWindow::UpdateTitles);
    connect(Tabs, &QTabWidget::tabCloseRequested, this, [this](const int InIndex)
    {
        if(auto* Tab = qobject_cast<ParticleEditorTab*>(Tabs->widget(InIndex))) { CloseTab(Tab); }
    });

    const auto Step = [this](const QList<QKeySequence>& InKeys, const int InBy)
    {
        auto* Action = new QAction(this);
        Action->setShortcuts(InKeys);
        connect(Action, &QAction::triggered, this, [this, InBy]() { StepTab(InBy); });
        addAction(Action);
    };
    Step({ QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown) }, 1);
    Step({ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageUp) }, -1);

    UpdateTitles();
}

void ParticleEditorWindow::AddTab(ParticleEditorTab* InTab)
{
    Tabs->addTab(InTab, InTab->GetTabTitle());
    connect(InTab, &ParticleEditorTab::TitleChanged, this, &ParticleEditorWindow::UpdateTitles);
    connect(InTab, &ParticleEditorTab::CloseRequested, this, [this, InTab]() { CloseTab(InTab); });
    ShowTab(InTab);
}

void ParticleEditorWindow::ShowTab(ParticleEditorTab* InTab)
{
    Tabs->setCurrentWidget(InTab);
    if(isMinimized()) { showNormal(); }
    else              { show(); }
    raise();
    activateWindow();
}

bool ParticleEditorWindow::CloseTab(ParticleEditorTab* InTab)
{
    const int Index = Tabs->indexOf(InTab);
    if(Index < 0 || !InTab->ConfirmClose()) { return false; }

    /* Now, not later: the tab's viewport releases its swapchain against its
     * native window, which must still exist. */
    Tabs->removeTab(Index);
    delete InTab;
    if(Tabs->count() == 0) { close(); }
    return true;
}

QList<ParticleEditorTab*> ParticleEditorWindow::GetTabs() const
{
    QList<ParticleEditorTab*> Result;
    for(int Index = 0; Index < Tabs->count(); ++Index)
    {
        if(auto* Tab = qobject_cast<ParticleEditorTab*>(Tabs->widget(Index))) { Result << Tab; }
    }
    return Result;
}

ParticleEditorTab* ParticleEditorWindow::CurrentTab() const
{
    return qobject_cast<ParticleEditorTab*>(Tabs->currentWidget());
}

void ParticleEditorWindow::UpdateTitles()
{
    for(ParticleEditorTab* Tab : GetTabs())
    {
        const int Index = Tabs->indexOf(Tab);
        Tabs->setTabText(Index, Tab->GetTabTitle());
        Tabs->setTabToolTip(Index, Tab->GetPath());
    }

    const ParticleEditorTab* Current = CurrentTab();
    setWindowTitle(Current ? QStringLiteral("%1 - Particle Editor - Nimble Editor").arg(Current->GetTabTitle())
                           : QStringLiteral("Particle Editor - Nimble Editor"));
}

void ParticleEditorWindow::StepTab(const int InBy)
{
    const int Count = Tabs->count();
    if(Count > 1) { Tabs->setCurrentIndex((Tabs->currentIndex() + InBy + Count) % Count); }
}

void ParticleEditorWindow::closeEvent(QCloseEvent* InEvent)
{
    /* Every system is asked before any goes, so a Cancel loses nothing. */
    for(ParticleEditorTab* Tab : GetTabs())
    {
        if(!Tab->ConfirmClose())
        {
            InEvent->ignore();
            return;
        }
    }

    /* The tabs go now, while their viewports' native windows still exist. */
    for(ParticleEditorTab* Tab : GetTabs())
    {
        Tabs->removeTab(Tabs->indexOf(Tab));
        delete Tab;
    }
    InEvent->accept();
}

bool ParticleEditorWindow::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* A middle click on a tab closes it, as in a browser. */
    if(InWatched == Tabs->tabBar() && InEvent->type() == QEvent::MouseButtonRelease)
    {
        const auto* Mouse = static_cast<QMouseEvent*>(InEvent);
        if(Mouse->button() == Qt::MiddleButton)
        {
            const int Index = Tabs->tabBar()->tabAt(Mouse->position().toPoint());
            if(auto* Tab = qobject_cast<ParticleEditorTab*>(Tabs->widget(Index))) { CloseTab(Tab); }
            return true;
        }
    }
    return QMainWindow::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR

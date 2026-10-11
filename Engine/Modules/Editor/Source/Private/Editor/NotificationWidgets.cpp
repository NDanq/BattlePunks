#if WITH_EDITOR

#include "Private/Editor/NotificationWidgets.h"

#include <QApplication>
#include <QClipboard>
#include <QEnterEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPalette>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "Private/Editor/EditorIcons.h"

using ESeverity = EditorNotifications::ESeverity;

namespace
{
    constexpr int ToastWidth  = 340;
    constexpr int ToastMargin = 12;   // from the central area's corner
    constexpr int FadeMs      = 250;

    QString SourceAndTime(const EditorNotifications::Notification& InNotification)
    {
        const QString Time = InNotification.Time.toString(QStringLiteral("HH:mm"));
        return InNotification.Source.isEmpty() ? Time : InNotification.Source + QStringLiteral(" · ") + Time;
    }
}

namespace EditorNotificationStyle
{
    QColor ForSeverity(const ESeverity InSeverity)
    {
        switch(InSeverity)
        {
            case ESeverity::Error:   return QColor(225, 85, 85);
            case ESeverity::Warning: return QColor(230, 170, 60);
            case ESeverity::Success: return QColor(90, 200, 110);
            case ESeverity::Info:    break;
        }
        return QApplication::palette().color(QPalette::Highlight);
    }

    QIcon IconFor(const ESeverity InSeverity)
    {
        EditorIcons::EIcon Icon = EditorIcons::EIcon::Info;
        switch(InSeverity)
        {
            case ESeverity::Error:   Icon = EditorIcons::EIcon::Error;   break;
            case ESeverity::Warning: Icon = EditorIcons::EIcon::Warning; break;
            case ESeverity::Success: Icon = EditorIcons::EIcon::Success; break;
            case ESeverity::Info:    Icon = EditorIcons::EIcon::Info;    break;
        }
        return EditorIcons::Get(Icon, ForSeverity(InSeverity));
    }
}

// === NotificationToast ==========================================================

NotificationToast::NotificationToast(EditorNotifications& InModel, const uint64_t InId, QWidget* InParent)
    : QFrame(InParent)
    , Model(InModel)
    , Id(InId)
{
    setObjectName(QStringLiteral("NotificationToast"));
    setFixedWidth(ToastWidth);
    setAttribute(Qt::WA_StyledBackground);

    auto* Grid = new QGridLayout(this);
    Grid->setContentsMargins(10, 8, 6, 8);
    Grid->setHorizontalSpacing(8);
    Grid->setVerticalSpacing(4);

    Icon = new QLabel(this);
    Icon->setFixedSize(16, 16);
    Grid->addWidget(Icon, 0, 0, Qt::AlignTop);

    Title = new QLabel(this);
    Title->setObjectName(QStringLiteral("ToastTitle"));
    Title->setWordWrap(true);
    QFont Bold = Title->font();
    Bold.setBold(true);
    Title->setFont(Bold);
    Grid->addWidget(Title, 0, 1);

    auto* Close = new QToolButton(this);
    Close->setObjectName(QStringLiteral("ToastClose"));
    Close->setIcon(EditorIcons::Get(EditorIcons::EIcon::Close));
    Close->setIconSize(QSize(12, 12));
    Close->setAutoRaise(true);
    Close->setToolTip(QStringLiteral("Close (stays in the Notifications dock)"));
    connect(Close, &QToolButton::clicked, this, [this]() { Model.CloseToast(Id); });
    Grid->addWidget(Close, 0, 2, Qt::AlignTop);

    Body = new QLabel(this);
    Body->setObjectName(QStringLiteral("ToastBody"));
    Body->setWordWrap(true);
    Body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Body->setForegroundRole(QPalette::PlaceholderText);
    Grid->addWidget(Body, 1, 1, 1, 2);

    Progress = new QProgressBar(this);
    Progress->setTextVisible(false);
    Progress->setFixedHeight(4);
    Grid->addWidget(Progress, 2, 1, 1, 2);

    Actions = new QWidget(this);
    auto* ActionRow = new QHBoxLayout(Actions);
    ActionRow->setContentsMargins(0, 2, 0, 0);
    ActionRow->setSpacing(6);
    Grid->addWidget(Actions, 3, 1, 1, 2);

    Grid->setColumnStretch(1, 1);

    Timer.setSingleShot(true);
    connect(&Timer, &QTimer::timeout, this, &NotificationToast::Expire);

    Refresh();
}

void NotificationToast::Refresh()
{
    const EditorNotifications::Notification* Found = Model.Find(Id);
    if(!Found)
    {
        return;
    }

    const QColor Accent = EditorNotificationStyle::ForSeverity(Found->Severity);
    Icon->setPixmap(EditorNotificationStyle::IconFor(Found->Severity).pixmap(QSize(16, 16), devicePixelRatioF()));
    Title->setText(Found->Title);
    Body->setText(Found->Body);
    Body->setVisible(!Found->Body.isEmpty());

    Progress->setVisible(Found->Progress != EditorNotifications::NoProgress);
    if(Found->Progress == EditorNotifications::Indeterminate)
    {
        Progress->setRange(0, 0);
    }
    else
    {
        Progress->setRange(0, 100);
        Progress->setValue(qBound(0, Found->Progress, 100));
    }

    /* The buttons again: a replace may change them. Later, as a button's own
     * action may have posted the replace. */
    QLayout* Row = Actions->layout();
    while(QLayoutItem* Item = Row->takeAt(0))
    {
        if(QWidget* Old = Item->widget())
        {
            Old->hide();
            Old->deleteLater();
        }
        delete Item;
    }
    const int Count = qMin(int(Found->Actions.size()), EditorNotifications::MaxActions);
    for(int Index = 0; Index < Count; ++Index)
    {
        auto* Button = new QPushButton(Found->Actions[Index].Label, Actions);
        Button->setObjectName(QStringLiteral("ToastAction%1").arg(Index));
        Button->setAutoDefault(false);
        Button->setFocusPolicy(Qt::NoFocus);
        connect(Button, &QPushButton::clicked, this, [this, Index]() { Model.RunAction(Id, Index); });
        Row->addWidget(Button);
    }
    static_cast<QHBoxLayout*>(Row)->addStretch(1);
    Actions->setVisible(Count > 0);

    /* Errors keep the severity's border; the rest a quiet one. */
    const QString Border = Found->Severity == ESeverity::Error ? Accent.name() : QStringLiteral("palette(mid)");
    setStyleSheet(QStringLiteral("QFrame#NotificationToast { background: palette(window); border: 1px solid %1; border-radius: 6px; }")
                  .arg(Border));

    /* A fade under way stops with its effect. */
    bExpiring = false;
    setGraphicsEffect(nullptr);
    if(EditorNotifications::IsSticky(*Found))
    {
        Timer.stop();
    }
    else if(!underMouse())
    {
        Timer.start(Model.GetToastTimeoutMs());
    }
    adjustSize();
}

void NotificationToast::enterEvent(QEnterEvent* InEvent)
{
    /* Held while read. */
    Timer.stop();
    QFrame::enterEvent(InEvent);
}

void NotificationToast::leaveEvent(QEvent* InEvent)
{
    const EditorNotifications::Notification* Found = Model.Find(Id);
    if(Found && !EditorNotifications::IsSticky(*Found) && !bExpiring)
    {
        Timer.start(Model.GetToastTimeoutMs());
    }
    QFrame::leaveEvent(InEvent);
}

void NotificationToast::Expire()
{
    bExpiring = true;
    auto* Effect = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(Effect);
    auto* Fade = new QPropertyAnimation(Effect, "opacity", this);
    Fade->setDuration(FadeMs);
    Fade->setStartValue(1.0);
    Fade->setEndValue(0.0);
    connect(Fade, &QPropertyAnimation::finished, this, [this]()
    {
        if(bExpiring)
        {
            Model.CloseToast(Id);   // -> NotificationToasts::Sync deletes this
        }
    });
    Fade->start(QAbstractAnimation::DeleteWhenStopped);
}

// === NotificationToasts =========================================================

NotificationToasts::NotificationToasts(QMainWindow& InHost)
    : QWidget(&InHost, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
    , Model(*EditorNotifications::Get())
    , Host(InHost)
{
    setObjectName(QStringLiteral("NotificationToasts"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);

    Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->setSpacing(8);
    Layout->setSizeConstraint(QLayout::SetFixedSize);

    MoreLabel = new QLabel(this);
    MoreLabel->setAlignment(Qt::AlignRight);
    MoreLabel->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(MoreLabel);

    connect(&Model, &EditorNotifications::Added,   this, &NotificationToasts::Sync);
    connect(&Model, &EditorNotifications::Changed, this, [this](const uint64_t InId)
    {
        const auto Found = Toasts.find(InId);
        if(Found != Toasts.end())
        {
            Found->second->Refresh();
        }
        Sync();
    });
    connect(&Model, &EditorNotifications::Removed, this, &NotificationToasts::Sync);
    connect(&Model, &EditorNotifications::Cleared, this, &NotificationToasts::Sync);
    connect(&Model, &EditorNotifications::DoNotDisturbChanged, this, &NotificationToasts::Sync);

    Host.installEventFilter(this);
    Sync();
}

QList<NotificationToast*> NotificationToasts::GetVisibleToasts() const
{
    QList<NotificationToast*> Out;
    for(int Index = 0; Index < Layout->count(); ++Index)
    {
        if(auto* Toast = qobject_cast<NotificationToast*>(Layout->itemAt(Index)->widget()))
        {
            Out.push_back(Toast);
        }
    }
    return Out;
}

void NotificationToasts::Sync()
{
    /* Wanted, oldest first: the first MaxVisible are shown. */
    QList<uint64_t> Wanted;
    for(const EditorNotifications::Notification& Each : Model.GetHistory())
    {
        if(Model.WantsToast(Each))
        {
            Wanted.push_back(Each.Id);
        }
    }
    const int Shown = qMin(int(Wanted.size()), MaxVisible);
    QueuedCount = int(Wanted.size()) - Shown;

    for(auto It = Toasts.begin(); It != Toasts.end();)
    {
        if(Wanted.indexOf(It->first) < 0 || Wanted.indexOf(It->first) >= Shown)
        {
            /* Later: this may be its own close or action button's click. */
            Layout->removeWidget(It->second);
            It->second->hide();
            It->second->deleteLater();
            It = Toasts.erase(It);
        }
        else
        {
            ++It;
        }
    }

    /* Newest lowest, nearest the corner. */
    for(int Index = 0; Index < Shown; ++Index)
    {
        NotificationToast*& Toast = Toasts[Wanted[Index]];
        if(!Toast)
        {
            Toast = new NotificationToast(Model, Wanted[Index], this);
        }
        Layout->removeWidget(Toast);
        Layout->insertWidget(1 + Index, Toast);
    }

    MoreLabel->setText(QStringLiteral("+%1 more").arg(QueuedCount));
    MoreLabel->setVisible(QueuedCount > 0);

    const bool bShow = Shown > 0 && Host.isVisible() && !Host.isMinimized();
    if(bShow)
    {
        Reposition();
        show();
        raise();
    }
    else
    {
        hide();
    }
}

void NotificationToasts::Reposition()
{
    adjustSize();
    const QWidget* Area = Host.centralWidget() ? Host.centralWidget() : &Host;
    const QPoint Corner = Area->mapToGlobal(QPoint(Area->width(), Area->height()));
    move(Corner - QPoint(width() + ToastMargin, height() + ToastMargin));
}

bool NotificationToasts::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    if(InWatched == &Host)
    {
        switch(InEvent->type())
        {
            case QEvent::Move:
            case QEvent::Resize:
                if(isVisible())
                {
                    Reposition();
                }
                break;
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::WindowStateChange:
                QTimer::singleShot(0, this, &NotificationToasts::Sync);
                break;
            default:
                break;
        }
    }
    return QWidget::eventFilter(InWatched, InEvent);
}

// === NotificationCenter =========================================================

NotificationCenter::NotificationCenter(QWidget* InParent)
    : QDockWidget(QStringLiteral("Notifications"), InParent)
    , Model(*EditorNotifications::Get())
{
    setObjectName(QStringLiteral("NotificationsDock"));

    auto* Body = new QWidget(this);
    auto* Column = new QVBoxLayout(Body);
    Column->setContentsMargins(0, 0, 0, 0);
    Column->setSpacing(0);

    auto* Bar = new QToolBar(Body);
    Bar->setIconSize(QSize(14, 14));
    Bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    DoNotDisturb = new QToolButton(Bar);
    DoNotDisturb->setText(QStringLiteral("Do Not Disturb"));
    DoNotDisturb->setIcon(EditorIcons::Get(EditorIcons::EIcon::BellOff));
    DoNotDisturb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    DoNotDisturb->setCheckable(true);
    DoNotDisturb->setToolTip(QStringLiteral("Hold back toasts except errors; they still collect here"));
    connect(DoNotDisturb, &QToolButton::toggled, &Model, &EditorNotifications::SetDoNotDisturb);
    Bar->addWidget(DoNotDisturb);
    auto* Spacer = new QWidget(Bar);
    Spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    Bar->addWidget(Spacer);
    QAction* ClearAll = Bar->addAction(QStringLiteral("Clear All"));
    connect(ClearAll, &QAction::triggered, &Model, &EditorNotifications::Clear);
    Column->addWidget(Bar);

    List = new QListWidget(Body);
    List->setObjectName(QStringLiteral("NotificationList"));
    List->setIconSize(QSize(16, 16));
    List->setWordWrap(true);
    List->setContextMenuPolicy(Qt::CustomContextMenu);
    List->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(List, &QListWidget::customContextMenuRequested, this, &NotificationCenter::ShowContextMenu);
    Column->addWidget(List);
    setWidget(Body);

    connect(&Model, &EditorNotifications::Added, this, [this](const uint64_t InId)
    {
        if(const EditorNotifications::Notification* Found = Model.Find(InId))
        {
            auto* Item = new QListWidgetItem();
            FillItem(Item, *Found);
            List->insertItem(0, Item);
        }
        MarkReadIfSeen();
    });
    connect(&Model, &EditorNotifications::Changed, this, [this](const uint64_t InId)
    {
        const EditorNotifications::Notification* Found = Model.Find(InId);
        QListWidgetItem* Item = FindItem(InId);
        if(!Found || !Item)
        {
            return;
        }
        /* A replace is news again: to the top. */
        if(!Found->bRead && List->row(Item) != 0)
        {
            List->takeItem(List->row(Item));
            List->insertItem(0, Item);
        }
        FillItem(Item, *Found);
        MarkReadIfSeen();
    });
    connect(&Model, &EditorNotifications::Removed, this, [this](const uint64_t InId) { delete FindItem(InId); });
    connect(&Model, &EditorNotifications::Cleared, List, &QListWidget::clear);
    connect(&Model, &EditorNotifications::UnreadCountChanged, this, [this](int)
    {
        /* Bold for unread, without rebuilding. */
        for(int Row = 0; Row < List->count(); ++Row)
        {
            QListWidgetItem* Item = List->item(Row);
            if(const EditorNotifications::Notification* Found = Model.Find(Item->data(Qt::UserRole).toULongLong()))
            {
                QFont Font = List->font();
                Font.setBold(!Found->bRead);
                Item->setFont(Font);
            }
        }
    });
    connect(&Model, &EditorNotifications::DoNotDisturbChanged, this, [this](const bool bInOn)
    {
        const QSignalBlocker Block(DoNotDisturb);
        DoNotDisturb->setChecked(bInOn);
    });
    connect(this, &QDockWidget::visibilityChanged, this, [this](const bool bInVisible)
    {
        if(bInVisible)
        {
            MarkReadIfSeen();
        }
    });

    Rebuild();
}

void NotificationCenter::Toggle()
{
    /* Tabbed behind another dock counts as hidden: bring it forward. */
    if(isVisible() && !visibleRegion().isEmpty())
    {
        hide();
        return;
    }
    show();
    raise();
    MarkReadIfSeen();
}

void NotificationCenter::Rebuild()
{
    List->clear();
    for(const EditorNotifications::Notification& Each : Model.GetHistory())
    {
        auto* Item = new QListWidgetItem();
        FillItem(Item, Each);
        List->insertItem(0, Item);
    }
}

void NotificationCenter::FillItem(QListWidgetItem* InItem, const EditorNotifications::Notification& InNotification) const
{
    InItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(InNotification.Id));
    InItem->setIcon(EditorNotificationStyle::IconFor(InNotification.Severity));

    QString Text = InNotification.Title;
    const QString FirstLine = InNotification.Body.section(QLatin1Char('\n'), 0, 0);
    if(!FirstLine.isEmpty())
    {
        Text += QStringLiteral("\n") + FirstLine;
    }
    Text += QStringLiteral("\n") + SourceAndTime(InNotification);
    InItem->setText(Text);
    InItem->setToolTip(InNotification.Body.isEmpty() ? InNotification.Title : InNotification.Body);

    QFont Font = List->font();
    Font.setBold(!InNotification.bRead);
    InItem->setFont(Font);
}

QListWidgetItem* NotificationCenter::FindItem(const uint64_t InId) const
{
    for(int Row = 0; Row < List->count(); ++Row)
    {
        if(List->item(Row)->data(Qt::UserRole).toULongLong() == InId)
        {
            return List->item(Row);
        }
    }
    return nullptr;
}

void NotificationCenter::MarkReadIfSeen()
{
    /* Visible, and not a tab behind another dock. */
    if(isVisible() && !visibleRegion().isEmpty())
    {
        Model.MarkAllRead();
    }
}

void NotificationCenter::ShowContextMenu(const QPoint& InPos)
{
    QListWidgetItem* Item = List->itemAt(InPos);
    if(!Item)
    {
        return;
    }
    const uint64_t Id = Item->data(Qt::UserRole).toULongLong();
    const EditorNotifications::Notification* Found = Model.Find(Id);
    if(!Found)
    {
        return;
    }

    QMenu Menu(this);
    for(int Index = 0; Index < int(Found->Actions.size()); ++Index)
    {
        QAction* Run = Menu.addAction(Found->Actions[Index].Label);
        connect(Run, &QAction::triggered, this, [this, Id, Index]() { Model.RunAction(Id, Index); });
    }
    if(!Found->Actions.isEmpty())
    {
        Menu.addSeparator();
    }
    const QString Copy = Found->Body.isEmpty() ? Found->Title : Found->Title + QStringLiteral("\n") + Found->Body;
    connect(Menu.addAction(QStringLiteral("Copy")), &QAction::triggered, this, [Copy]() { QApplication::clipboard()->setText(Copy); });
    connect(Menu.addAction(QStringLiteral("Remove")), &QAction::triggered, this, [this, Id]() { Model.Remove(Id); });
    Menu.exec(List->viewport()->mapToGlobal(InPos));
}

// === NotificationStatusItem =====================================================

NotificationStatusItem::NotificationStatusItem(NotificationCenter& InCenter, QWidget* InParent)
    : QWidget(InParent)
    , Model(*EditorNotifications::Get())
{
    auto* Row = new QHBoxLayout(this);
    Row->setContentsMargins(0, 0, 0, 0);
    Row->setSpacing(0);

    DoNotDisturb = new QToolButton(this);
    DoNotDisturb->setObjectName(QStringLiteral("DoNotDisturbButton"));
    DoNotDisturb->setIcon(EditorIcons::Get(EditorIcons::EIcon::BellOff));
    DoNotDisturb->setIconSize(QSize(14, 14));
    DoNotDisturb->setAutoRaise(true);
    DoNotDisturb->setCheckable(true);
    DoNotDisturb->setToolTip(QStringLiteral("Do Not Disturb: hold back toasts except errors"));
    connect(DoNotDisturb, &QToolButton::toggled, &Model, &EditorNotifications::SetDoNotDisturb);
    Row->addWidget(DoNotDisturb);

    Bell = new QToolButton(this);
    Bell->setObjectName(QStringLiteral("NotificationBell"));
    Bell->setIconSize(QSize(14, 14));
    Bell->setAutoRaise(true);
    Bell->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    Bell->setToolTip(QStringLiteral("Notifications"));
    connect(Bell, &QToolButton::clicked, &InCenter, &NotificationCenter::Toggle);
    Row->addWidget(Bell);

    connect(&Model, &EditorNotifications::UnreadCountChanged, this, &NotificationStatusItem::Update);
    connect(&Model, &EditorNotifications::DoNotDisturbChanged, this, &NotificationStatusItem::Update);
    Update();
}

void NotificationStatusItem::Update()
{
    const int Unread = Model.GetUnreadCount();
    {
        const QSignalBlocker Block(DoNotDisturb);
        DoNotDisturb->setChecked(Model.IsDoNotDisturb());
    }
    Bell->setIcon(EditorIcons::Get(EditorIcons::EIcon::Bell,
                                   Unread > 0 ? QApplication::palette().color(QPalette::Highlight) : QColor()));
    Bell->setText(Unread > 0 ? QString::number(Unread) : QString());
    Bell->setToolTip(Unread > 0 ? QStringLiteral("Notifications: %1 unread").arg(Unread) : QStringLiteral("Notifications"));
}

#endif // WITH_EDITOR

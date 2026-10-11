#if WITH_EDITOR

#include "Public/Editor/EditorNotifications.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

namespace
{
    std::atomic<EditorNotifications*> GNotifications{ nullptr };

    bool IsGuiThread()
    {
        const QCoreApplication* App = QCoreApplication::instance();
        return App && QThread::currentThread() == App->thread();
    }
}

void EditorNotifications::Install()
{
    if(GNotifications.load())
    {
        return;
    }
    GNotifications.store(new EditorNotifications(), std::memory_order_release);
}

void EditorNotifications::Uninstall()
{
    delete GNotifications.exchange(nullptr);
}

EditorNotifications* EditorNotifications::Get()
{
    return GNotifications.load(std::memory_order_acquire);
}

uint64_t EditorNotifications::Notify(const ESeverity InSeverity, const QString& InTitle, const QString& InBody,
                                     const QString& InSource, const QString& InReplaceKey)
{
    EditorNotifications* Self = Get();
    if(!Self)
    {
        return 0;
    }
    Notification New;
    New.Severity   = InSeverity;
    New.Title      = InTitle;
    New.Body       = InBody;
    New.Source     = InSource;
    New.ReplaceKey = InReplaceKey;
    return Self->Post(std::move(New));
}

uint64_t EditorNotifications::Post(Notification InNotification)
{
    if(IsGuiThread())
    {
        return PostOnGuiThread(std::move(InNotification));
    }

    bool bQueueFlush = false;
    {
        std::lock_guard<std::mutex> Lock(PendingMutex);
        Pending.push_back(std::move(InNotification));
        if(!bFlushQueued)
        {
            bFlushQueued = true;
            bQueueFlush  = true;
        }
    }
    if(bQueueFlush)
    {
        QMetaObject::invokeMethod(this, "FlushPending", Qt::QueuedConnection);
    }
    return 0;
}

void EditorNotifications::FlushPending()
{
    std::vector<Notification> Batch;
    {
        std::lock_guard<std::mutex> Lock(PendingMutex);
        Batch.swap(Pending);
        bFlushQueued = false;
    }
    for(Notification& Each : Batch)
    {
        PostOnGuiThread(std::move(Each));
    }
}

uint64_t EditorNotifications::PostOnGuiThread(Notification InNotification)
{
    InNotification.Time         = QDateTime::currentDateTime();
    InNotification.bRead        = false;
    InNotification.bToastClosed = false;

    if(!InNotification.ReplaceKey.isEmpty())
    {
        for(Notification& Existing : History)
        {
            if(Existing.ReplaceKey == InNotification.ReplaceKey)
            {
                InNotification.Id = Existing.Id;
                Existing = std::move(InNotification);
                RecountUnread();
                emit Changed(Existing.Id);
                return Existing.Id;
            }
        }
    }

    InNotification.Id = NextId.fetch_add(1);
    const uint64_t Id = InNotification.Id;
    History.push_back(std::move(InNotification));

    while(History.size() > MaxHistory)
    {
        const uint64_t Oldest = History.front().Id;
        History.pop_front();
        emit Removed(Oldest);
    }

    RecountUnread();
    emit Added(Id);
    return Id;
}

bool EditorNotifications::IsSticky(const Notification& InNotification)
{
    return InNotification.bSticky
        || InNotification.Severity == ESeverity::Error
        || !InNotification.Actions.isEmpty()
        || InNotification.Progress != NoProgress;
}

const EditorNotifications::Notification* EditorNotifications::Find(const uint64_t InId) const
{
    const auto Found = std::find_if(History.begin(), History.end(), [InId](const Notification& Each) { return Each.Id == InId; });
    return Found == History.end() ? nullptr : &*Found;
}

const EditorNotifications::Notification* EditorNotifications::FindByKey(const QString& InReplaceKey) const
{
    if(InReplaceKey.isEmpty())
    {
        return nullptr;
    }
    const auto Found = std::find_if(History.begin(), History.end(),
                                    [&InReplaceKey](const Notification& Each) { return Each.ReplaceKey == InReplaceKey; });
    return Found == History.end() ? nullptr : &*Found;
}

EditorNotifications::Notification* EditorNotifications::FindMutable(const uint64_t InId)
{
    return const_cast<Notification*>(Find(InId));
}

void EditorNotifications::CloseToast(const uint64_t InId)
{
    Notification* Found = FindMutable(InId);
    if(!Found || Found->bToastClosed)
    {
        return;
    }
    Found->bToastClosed = true;
    emit Changed(InId);
}

void EditorNotifications::RunAction(const uint64_t InId, const int InIndex)
{
    const Notification* Found = Find(InId);
    if(!Found || InIndex < 0 || InIndex >= Found->Actions.size())
    {
        return;
    }
    /* A copy: the callback may post, replace or remove this very notification. */
    const std::function<void()> Run = Found->Actions[InIndex].Run;
    CloseToast(InId);
    if(Run)
    {
        Run();
    }
}

void EditorNotifications::Remove(const uint64_t InId)
{
    const auto Found = std::find_if(History.begin(), History.end(), [InId](const Notification& Each) { return Each.Id == InId; });
    if(Found == History.end())
    {
        return;
    }
    History.erase(Found);
    RecountUnread();
    emit Removed(InId);
}

void EditorNotifications::Clear()
{
    History.clear();
    RecountUnread();
    emit Cleared();
}

void EditorNotifications::MarkAllRead()
{
    for(Notification& Each : History)
    {
        Each.bRead = true;
    }
    RecountUnread();
}

void EditorNotifications::SetDoNotDisturb(const bool bInOn)
{
    if(bDoNotDisturb == bInOn)
    {
        return;
    }
    bDoNotDisturb = bInOn;
    emit DoNotDisturbChanged(bInOn);
}

bool EditorNotifications::WantsToast(const Notification& InNotification) const
{
    if(InNotification.bToastClosed)
    {
        return false;
    }
    return !bDoNotDisturb || InNotification.Severity == ESeverity::Error;
}

void EditorNotifications::RecountUnread()
{
    const int Count = int(std::count_if(History.begin(), History.end(), [](const Notification& Each) { return !Each.bRead; }));
    if(Count != UnreadCount)
    {
        UnreadCount = Count;
        emit UnreadCountChanged(Count);
    }
}

#endif // WITH_EDITOR

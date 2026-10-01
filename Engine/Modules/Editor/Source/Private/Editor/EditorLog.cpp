#if WITH_EDITOR

#include "Public/Editor/EditorLog.h"

#include <algorithm>
#include <atomic>

#include <QMetaObject>

#include "Public/Core/CoreLog.h"

namespace
{
    /* Atomic because the handler reads it from whichever thread logs, while
     * Install/Uninstall write it on the GUI thread. */
    std::atomic<EditorLog*> GEditorLog{ nullptr };

    EditorLog::EKind ToKind(const EngineCore::LogVerbosity InVerbosity)
    {
        switch(InVerbosity)
        {
            case EngineCore::LogVerbosity::Log:     return EditorLog::EKind::Log;
            case EngineCore::LogVerbosity::Warning: return EditorLog::EKind::Warning;
            case EngineCore::LogVerbosity::Error:   return EditorLog::EKind::Error;
            case EngineCore::LogVerbosity::Fatal:   return EditorLog::EKind::Fatal;
        }
        return EditorLog::EKind::Log;
    }

    /* To the history and Qt's message system (a WIN32 exe has no stderr).
     * Fatal still aborts via qFatal, after recording the line. */
    void EditorLogHandler(const EngineCore::LogVerbosity InVerbosity, const char* InMessage)
    {
        const char* Message = InMessage ? InMessage : "";

        if(EditorLog* Log = GEditorLog.load(std::memory_order_acquire))
        {
            Log->Append(ToKind(InVerbosity), QString::fromUtf8(Message));
        }

        switch(InVerbosity)
        {
            case EngineCore::LogVerbosity::Log:     qInfo("%s", Message);     break;
            case EngineCore::LogVerbosity::Warning: qWarning("%s", Message);  break;
            case EngineCore::LogVerbosity::Error:   qCritical("%s", Message); break;
            case EngineCore::LogVerbosity::Fatal:   qFatal("%s", Message);    break;
        }
    }
}

void EditorLog::Install()
{
    if(GEditorLog.load())
    {
        return;
    }
    GEditorLog.store(new EditorLog(), std::memory_order_release);
    EngineCore::SetLogHandler(&EditorLogHandler);
}

void EditorLog::Uninstall()
{
    EngineCore::SetLogHandler(nullptr);
    /* Safe: the render thread was joined in EditorContext::Shutdown. */
    delete GEditorLog.exchange(nullptr);
}

EditorLog* EditorLog::Get()
{
    return GEditorLog.load(std::memory_order_acquire);
}

void EditorLog::Append(const EKind InKind, const QString& InText)
{
    Entry New;
    New.Kind = InKind;
    New.Time = QTime::currentTime();
    New.Text = InText;

    bool bQueueFlush = false;
    {
        std::lock_guard<std::mutex> Lock(PendingMutex);
        Pending.push_back(std::move(New));
        if(!bFlushQueued)
        {
            bFlushQueued = true;
            bQueueFlush  = true;
        }
    }

    /* One Flush per batch. Queued even on the GUI thread: logs can come from
     * inside a paint or model reset. */
    if(bQueueFlush)
    {
        QMetaObject::invokeMethod(this, "Flush", Qt::QueuedConnection);
    }
}

void EditorLog::Flush()
{
    std::vector<Entry> Batch;
    {
        std::lock_guard<std::mutex> Lock(PendingMutex);
        Batch.swap(Pending);
        bFlushQueued = false;
    }
    if(Batch.empty())
    {
        return;
    }

    for(Entry& Item : Batch)
    {
        ++Counts[size_t(Item.Kind)];
        Entries.push_back(std::move(Item));
    }
    while(Entries.size() > MaxEntries)
    {
        --Counts[size_t(Entries.front().Kind)];
        Entries.pop_front();
    }

    emit EntriesAppended(int(std::min(Batch.size(), Entries.size())));
}

void EditorLog::Clear()
{
    Entries.clear();
    for(int& Count : Counts)
    {
        Count = 0;
    }
    emit Cleared();
}

#endif // WITH_EDITOR

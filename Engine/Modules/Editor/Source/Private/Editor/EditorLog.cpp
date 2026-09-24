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

    /*
     * CoreLog into the console's history AND Qt's message system, so output
     * still reaches the debugger (a WIN32 exe has no console for the default
     * stderr sink). Fatal stays fatal: qFatal aborts, as the default handler
     * would -- after the history has the line, for a debugger stopped there.
     */
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
    /* Nothing logs through the handler after this, but a thread already inside
     * it may still hold the pointer -- by Uninstall the render thread has been
     * joined (EditorContext::Shutdown), which is what makes the delete safe. */
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

    /* One posted Flush per batch, however many lines the render thread writes
     * before the GUI thread gets round to it. Queued even from the GUI thread:
     * a log line can come from inside a paint or a model reset, which is no
     * place to be emitting into the console's text view. */
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

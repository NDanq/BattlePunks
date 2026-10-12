#if WITH_EDITOR

#include "Public/Editor/EditorContentEvents.h"

#include <QCoreApplication>
#include <QPointer>

EditorContentEvents& EditorContentEvents::Get()
{
    static QPointer<EditorContentEvents> Instance;
    if(!Instance)
    {
        Instance = new EditorContentEvents(QCoreApplication::instance());
    }
    return *Instance;
}

void EditorContentEvents::Announce(const QStringList& InFiles)
{
    if(!InFiles.isEmpty())
    {
        emit ContentChanged(InFiles);
    }
}

#endif // WITH_EDITOR

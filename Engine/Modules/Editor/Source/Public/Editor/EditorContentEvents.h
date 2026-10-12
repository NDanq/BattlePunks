#ifndef NIMBLEEDITOR_EDITORCONTENTEVENTS_H
#define NIMBLEEDITOR_EDITORCONTENTEVENTS_H

#if WITH_EDITOR

#include <QObject>
#include <QStringList>

/*
 * The editor-wide "files changed under you" event: what a pull brought in
 * (Collaboration's Pull & Reload), or anything else that rewrote project files
 * behind the editor's back. Whoever caches a file listens and refreshes it:
 * the main window restages assets and graphs (so the viewport and Play see the
 * new ones) and the Content Browser redraws.
 *
 * Saves the editor makes itself don't go through here: their editors already
 * restage what they write.
 *
 * GUI thread only. Made on first use, owned by the application.
 */
class EditorContentEvents : public QObject
{
    Q_OBJECT

public:
    static EditorContentEvents& Get();

    /* InFiles: absolute paths, changed or deleted. */
    void Announce(const QStringList& InFiles);

signals:
    void ContentChanged(const QStringList& InFiles);

private:
    explicit EditorContentEvents(QObject* InParent) : QObject(InParent) {}
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCONTENTEVENTS_H

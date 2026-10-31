#ifndef NIMBLEEDITOR_EDITORLAYOUTSETTINGS_H
#define NIMBLEEDITOR_EDITORLAYOUTSETTINGS_H

#if WITH_EDITOR

class QSettings;

/*
 * Window state the editor keeps between runs: the main window's docks and
 * size, the recent worlds, the Details panel's collapsed sections. Written to
 * EditorLayout.ini beside EditorSettings.ini, in Qt's own format: these are
 * not config vars, and nothing but the editor reads them.
 */
namespace EditorLayoutSettings
{
    QSettings& Get();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORLAYOUTSETTINGS_H

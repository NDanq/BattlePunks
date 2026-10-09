#ifndef NIMBLEEDITOR_SEQUENCERASSETS_H
#define NIMBLEEDITOR_SEQUENCERASSETS_H

#if WITH_EDITOR

#include <QString>

class EditorContext;
class SequencerTab;
class ViewportWidget;
class QWidget;

/*
 * Sequences as project files (*.nmseq, Modules/Sequencer): where new ones
 * go, and opening, creating and closing them in the Sequencer.
 *
 * A Sequence Player names one by runtime path
 * ("Content/Cinematics/Intro.nmseq"). Saving stages it and invalidates the
 * Sequencer's cache, so the next Play in the editor plays the change.
 */
namespace EditorSequencer
{
    /* Set by the main window; nullptr when there is none. */
    void           SetEditorContext(EditorContext* InContext);
    EditorContext* GetEditorContext();

    /* The level's viewport: what the Sequencer's camera cuts and pilot drive. */
    void            SetLevelViewport(ViewportWidget* InViewport);
    ViewportWidget* GetLevelViewport();

    /* <project>/Content/Cinematics. */
    QString GetDefaultFolder();

    /* Adds or focuses the tab; nullptr (message box shown) if it cannot be opened. */
    SequencerTab* OpenSequence(const QString& InPath, QWidget* InParent);

    /* Asks for a name and writes an empty five-second sequence in InFolder
     * (GetDefaultFolder() if empty), staged; opens it if bInOpen. Returns its
     * path, empty if cancelled or it could not be written. */
    QString CreateSequence(const QString& InFolder, QWidget* InParent, bool bInOpen = true);

    /* Offers to save each unsaved sequence, and puts the level back. False if cancelled. */
    bool CloseAll();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERASSETS_H

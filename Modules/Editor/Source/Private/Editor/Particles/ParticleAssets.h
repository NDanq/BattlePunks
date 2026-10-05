#ifndef NIMBLEEDITOR_PARTICLEASSETS_H
#define NIMBLEEDITOR_PARTICLEASSETS_H

#if WITH_EDITOR

#include <QString>

class EditorContext;
class ParticleEditorTab;
class QWidget;

/*
 * Particle systems as project files (*.nmparticle, RenderCore's
 * ParticleSystemAsset.h): where new ones go, and opening, creating and
 * closing them in the Particle Editor.
 *
 * A Particle System component names one by runtime path
 * ("Content/Particles/Smoke.nmparticle": project-relative, staged at the
 * same path beside the exe). Saving stages it and invalidates the cache, so
 * every component naming it -- in the level, or playing -- shows the change
 * on the next frame.
 */
namespace EditorParticles
{
    /* Set by the main window; nullptr when there is none. */
    void           SetEditorContext(EditorContext* InContext);
    EditorContext* GetEditorContext();

    /* <project>/Content/Particles. */
    QString GetDefaultFolder();

    /* A runtime path as a file in the project (or the engine's content). */
    QString ToProjectPath(const QString& InRuntimePath);

    /* Adds or focuses the tab; nullptr (message box shown) if it cannot be
     * opened -- no renderer, or not a readable particle system. */
    ParticleEditorTab* OpenParticleSystem(const QString& InPath, QWidget* InParent);

    /* Asks for a name and writes a system of one emitter in InFolder
     * (GetDefaultFolder() if empty), staged; opens it if bInOpen. Returns
     * its path, empty if cancelled or it could not be written. */
    QString CreateParticleSystem(const QString& InFolder, QWidget* InParent, bool bInOpen = true);

    /* Offers to save each unsaved system. False if cancelled. */
    bool CloseAll();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEASSETS_H

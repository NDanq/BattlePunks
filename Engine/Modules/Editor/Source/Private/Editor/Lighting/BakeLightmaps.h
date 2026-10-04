#ifndef NIMBLEEDITOR_BAKELIGHTMAPS_H
#define NIMBLEEDITOR_BAKELIGHTMAPS_H

#if WITH_EDITOR

#include <functional>

#include <QString>

class EditorDocument;
class QWidget;

/*
 * Build > Bake Lightmaps (and the "lighting bake" console command): the edited
 * world's static lighting baked into its Lighting folder (LightBakeWorld.h),
 * on a worker thread behind a progress dialog that can cancel it. The world
 * must be saved (the bake goes beside it) and have baked lighting on (World
 * Settings; offered if off). Afterwards its settings name the bake -- one
 * undo step, saved with the world -- and the renderer draws with it.
 */
namespace EditorLighting
{
    /* InWorldFile: the world's file, absolute; empty if it has none yet.
     * InStatus shows a one-line outcome. True if a bake was written. */
    bool BakeLightmaps(EditorDocument& InDocument, const QString& InWorldFile, QWidget* InParent,
                       const std::function<void(const QString&)>& InStatus);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_BAKELIGHTMAPS_H

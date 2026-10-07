#ifndef NIMBLEEDITOR_ANIMATIONASSETS_H
#define NIMBLEEDITOR_ANIMATIONASSETS_H

#if WITH_EDITOR

#include <string>

#include <QString>

#include "Public/Core/CoreArchive.h"

class QWidget;

/*
 * What the editor's animation windows share (ANIMATION.md): reading the
 * files off disk, finding a mesh to preview an animation on, and making
 * and opening anim states.
 */
namespace EditorAnimation
{
    /* InPath's file whole, read directly (not through the file provider,
     * which is rooted at the executable's staged copies). Empty if unreadable. */
    EngineCore::Archive ReadAssetArchive(const QString& InPath);

    /* A runtime path's file: the engine's content, else the project's. */
    QString FileOfRuntimePath(const QString& InRuntimePath);

    /* The skeleton an .nmanim or .nmanimstate plays on: a clip's own, an anim
     * state's first clip's. "" if it names none. */
    std::string SkeletonOfAnimation(const QString& InPath);

    /* The first skinned mesh on InSkeleton in the project, then the engine; "" if none. */
    QString FindMeshOn(const std::string& InSkeleton);

    /* Asks for a name and writes a starter anim state in InFolder (the
     * project's Content/Animation when empty): a Base layer with an Idle state
     * and a Speed parameter. Opened if bInOpen. Its path, or "" if cancelled. */
    QString CreateAnimState(const QString& InFolder, QWidget* InParent, bool bInOpen = true);

    /* The anim state editor on InPath. */
    void OpenAnimState(const QString& InPath, QWidget* InParent);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ANIMATIONASSETS_H

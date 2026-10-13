#ifndef NIMBLEEDITOR_PATHTESTERTOOL_H
#define NIMBLEEDITOR_PATHTESTERTOOL_H

#if WITH_EDITOR && WITH_AICORE

#include <functional>

#include "Public/Editor/ViewportTool.h"

/*
 * The Navigation dock's path tester: click the level for the path's start,
 * click again for its end -- the navmesh path between them drawn at once
 * (EditorAI's overlay), its corners and length told to the dock. A third
 * click starts again; Shift+click moves only the end.
 */
class PathTesterTool final : public ViewportTool
{
public:
    /* Told after each click: the dock refreshes its line. */
    std::function<void()> OnChanged;

    bool OnPress(ViewportWidget& InViewport, const Ray& InRay, Qt::KeyboardModifiers InModifiers) override;
};

#endif // WITH_EDITOR && WITH_AICORE

#endif //NIMBLEEDITOR_PATHTESTERTOOL_H

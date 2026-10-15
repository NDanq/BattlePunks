#ifndef NIMBLEEDITOR_PROCEDURALEDITING_H
#define NIMBLEEDITOR_PROCEDURALEDITING_H

#if WITH_EDITOR

#include <functional>

#include <QString>
#include <QStringList>

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

class EditorConsole;
class EditorContext;
class EditorDocument;
class ViewportWidget;

/*
 * Splines and Procedural Spawners in the editor (PROCEDURAL.md).
 *
 * THE OVERLAY, every viewport frame: each Spline as a line (the selected
 * one bright, with its points as handles and its Width as a band either
 * side), and a selected Box spawner's box.
 *
 * EDITING POINTS (World > Edit Spline Points, or `spline edit on`): with an
 * entity selected (a Spline is added to it if it has none), the viewport's
 * left button edits its points:
 *
 *   Left drag on a point   move it over the ground and meshes under the
 *                          cursor (Shift: up and down instead)
 *   Ctrl + left click      a new point there: after the selected point, or
 *                          at the end
 *   Delete / Backspace     remove the selected point
 *   Esc, right click       cancel a drag; Esc again leaves point editing
 *
 * Each drag, addition or removal is one undo step. A Procedural Spawner on
 * the entity makes its instances again as the points move.
 *
 * Console: `spline ...` and `pcg ...` (GetSplineUsage, GetProceduralUsage)
 * do the same from scripts and tests.
 */
namespace EditorProcedural
{
    struct Host
    {
        EditorContext*  Context  = nullptr;
        EditorDocument* Document = nullptr;
        ViewportWidget* Viewport = nullptr;
    };

    /* Once, when the main window is built: keeps InHost and adds the overlay. */
    void Install(const Host& InHost);

    /* Point editing on or off. */
    void SetEditingPoints(bool bInEditing);
    bool IsEditingPoints();
    /* The menu's check follows. */
    void SetEditingChangedCallback(std::function<void(bool)> InCallback);

    /* New entities, selected, each one undo step. */
    GameCore::EntityID CreateSpline(const EngineCore::Vector3& InPosition);
    GameCore::EntityID CreateSpawner(const EngineCore::Vector3& InPosition);

    QString GetSplineUsage();
    void    RunSplineCommand(EditorConsole& InConsole, const QStringList& InArgs);
    QString GetProceduralUsage();
    void    RunProceduralCommand(EditorConsole& InConsole, const QStringList& InArgs);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROCEDURALEDITING_H

#ifndef NIMBLEEDITOR_EDITORAI_H
#define NIMBLEEDITOR_EDITORAI_H

#if WITH_EDITOR && WITH_AICORE

#include <functional>
#include <vector>

#include <QString>

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

class EditorContext;
class EditorDocument;
class ViewportWidget;
namespace AICore { class AIWorld; }
namespace GameCore { class World; }

/*
 * What the editor's AI tools share (Engine/Modules/AICore/README.md, "In the
 * editor"): the Navigation dock (NavigationPanel.h), the path tester
 * (PathTesterTool.h), the AI debugger (AIDebuggerPanel.h) and the behavior
 * tree editor (BehaviorTreeEditorWindow.h).
 *
 * THE WORLD. The active World's AIWorld: while Play-in-Editor lends the
 * playing level (EditorDocument::BorrowWorld), the game's own, brains and
 * all; otherwise one this attaches to the edited World for building and
 * drawing its navmesh -- no brains run there, as the edited World never ticks.
 *
 * THE OVERLAY, every viewport frame (ViewportWidget::AddOverlay): the navmesh
 * (Show Nav Mesh), the path tester's path, and the debugger's agents.
 */
namespace EditorAI
{
    struct Host
    {
        EditorContext*  Context  = nullptr;
        EditorDocument* Document = nullptr;
        ViewportWidget* Viewport = nullptr;
        std::function<QString()> GetWorldFile;   // the level's .nmworld, absolute; empty if unsaved
    };
    void SetHost(const Host& InHost);
    const Host& GetHost();

    /* The active World's AIWorld (see THE WORLD); null without a document. */
    AICore::AIWorld* GetAI();
    /* Whether GetAI() is the running game's. */
    bool IsPlaying();

    /* What the overlay draws. */
    struct OverlaySettings
    {
        bool bNavMesh    = false;
        bool bTileBounds = false;
        bool bObstacles  = true;
        bool bPaths      = true;    // agents' corridors (the debugger)
        bool bSight      = true;    // sight cones and what each agent sees
        bool bModifiers  = true;    // Nav Modifier and Nav Mesh Volume boxes
    };
    OverlaySettings& GetOverlaySettings();

    /* The lines each gizmo drew in the last full viewport frame (the
     * "ai.overlaystats" command; a test). */
    struct OverlayStats
    {
        int NavMesh = 0, TileBounds = 0, Obstacles = 0, Modifiers = 0, TestPath = 0, Paths = 0, Sight = 0;
    };
    const OverlayStats& GetOverlayStats();

    /* The path tester's ends and result; drawn while set. */
    struct TestPath
    {
        bool bHasStart = false, bHasEnd = false;
        EngineCore::Vector3 Start, End;
        std::vector<EngineCore::Vector3> Points;
        int   Status = 0;            // AICore::ENavPathStatus
        float Length = 0.0f;
    };
    TestPath& GetTestPath();
    /* Recomputes GetTestPath's points from its ends. */
    void UpdateTestPath();

    /* The agent the debugger inspects; unset for none. */
    GameCore::EntityID& GetDebuggedAgent();

    /* Installs the overlay on the viewport; once. */
    void InstallOverlay();

    /* The navmesh's file on disk for the level's volume, and its runtime
     * path: the volume's NavMesh, else <level folder>/<level>.nmnav. */
    bool GetNavMeshPaths(QString& OutRuntime, QString& OutDisk);
}

#endif // WITH_EDITOR && WITH_AICORE

#endif //NIMBLEEDITOR_EDITORAI_H

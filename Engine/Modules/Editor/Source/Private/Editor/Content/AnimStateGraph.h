#ifndef NIMBLEEDITOR_ANIMSTATEGRAPH_H
#define NIMBLEEDITOR_ANIMSTATEGRAPH_H

#if WITH_EDITOR

#include <utility>
#include <vector>

#include <QPointF>
#include <QWidget>

#include "Public/RenderCore/AnimStateAsset.h"

class GraphView;
class QGraphicsScene;
class QLabel;

namespace AnimStateGraphItems
{
    class StateItem;
    class TransitionItem;
    class StateScene;
}

/*
 * One layer of an anim state as a graph (ANIMATION.md, "In the editor"):
 * each state a box -- the entry one marked, the one playing in the preview
 * lit -- and each transition an arrow, from its state or from Any State.
 * Drag a box to move it; right-click for Add State, Make Transition (then
 * click where it goes), Set as Entry and Delete; Delete removes what is
 * selected. Wheel zooms, middle-drag pans.
 *
 * It shows the layer and says what was asked; the window makes the change
 * and calls SetLayer again.
 */
class AnimStateGraph final : public QWidget
{
    Q_OBJECT

public:
    explicit AnimStateGraph(QWidget* InParent = nullptr);

    /* Draws InLayer (null: nothing) afresh, keeping the view. */
    void SetLayer(const RenderCore::AnimLayer* InLayer, bool bInReadOnly);

    /* What is selected (-1: none), without saying so. */
    void SetSelection(int InState, int InTransition);

    /* The state playing in the preview (-1: none), lit; bInBlending: still
     * crossfading into it. */
    void SetLive(int InState, bool bInBlending);

    /* Everything in view. */
    void FrameAll();

    /* Positions for InOutLayer's states (and Any State) in a grid, entry
     * first; marks it placed. For a layer no one has arranged yet. */
    static void LayOut(RenderCore::AnimLayer& InOutLayer);

signals:
    void StateSelected(int InState);
    void TransitionSelected(int InTransition);
    void NothingSelected();
    void StateMoved(int InState, const QPointF& InAt);   // -1: Any State
    void AddStateRequested(const QPointF& InAt);
    void RemoveStateRequested(int InState);
    void EntryRequested(int InState);
    void AddTransitionRequested(int InFrom, int InTo);   // InFrom -1: from any state
    void RemoveTransitionRequested(int InTransition);

private:
    friend class AnimStateGraphItems::StateItem;
    friend class AnimStateGraphItems::StateScene;

    /* Re-routes every arrow: a box moved. */
    void UpdateArrows();
    void OnSelectionChanged();

    /* The scene's input, in scene coordinates. */
    void BeginLink(int InFrom);
    bool IsLinking() const { return LinkFrom != NO_LINK; }
    void MoveLink(const QPointF& InAt);
    void EndLink(const QPointF& InAt);   // on a state: a transition to it; else none
    void CancelLink();
    void ContextMenu(const QPointF& InAt, const QPoint& InScreen);
    void DeleteSelected();

    AnimStateGraphItems::StateItem* StateAt(const QPointF& InAt) const;

    static constexpr int NO_LINK = -2;

    AnimStateGraphItems::StateScene* Scene = nullptr;
    GraphView*                       View  = nullptr;
    QLabel*                          Hint  = nullptr;

    std::vector<AnimStateGraphItems::StateItem*>      States;   // by state; Any State apart
    AnimStateGraphItems::StateItem*                   Any = nullptr;
    std::vector<AnimStateGraphItems::TransitionItem*> Arrows;   // by transition
    std::vector<std::pair<int, int>>                  LinkEnds; // by transition: from (-1: Any), to
    class QGraphicsLineItem*                          LinkLine = nullptr;

    bool bReadOnly  = false;
    bool bQuiet     = false;    // selection set from outside: not echoed
    bool bFramed    = false;    // framed once already
    int  LinkFrom   = NO_LINK;  // a transition being drawn from this state (-1: Any)
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ANIMSTATEGRAPH_H

#ifndef NIMBLEEDITOR_VIEWPORTTOOL_H
#define NIMBLEEDITOR_VIEWPORTTOOL_H

#if WITH_EDITOR

#include <Qt>

#include "Public/Math/Vector.h"

class QKeyEvent;
class QWheelEvent;
class ViewportWidget;

/*
 * An editor mode that takes the viewport's plain left button: the terrain
 * brushes, and whatever paints or places by clicking next. While a tool is
 * set (ViewportWidget::SetTool), a left press the tool accepts becomes its
 * stroke instead of a selection click or a handle drag; the camera keeps its
 * buttons (right, middle, Alt+left), so a tool never blocks navigation.
 *
 * Rays are in world space from the camera through the cursor; the direction
 * is unit length. Every call is on the editor's (game) thread.
 */
class ViewportTool
{
public:
    struct Ray
    {
        EngineCore::Vector3 Origin;
        EngineCore::Vector3 Direction;
    };

    virtual ~ViewportTool() = default;

    /* A left press. True: the tool takes it -- OnDrag follows each move, and
     * OnRelease the release. False: the viewport handles it as usual. */
    virtual bool OnPress(ViewportWidget& InViewport, const Ray& InRay, Qt::KeyboardModifiers InModifiers) = 0;
    virtual void OnDrag(ViewportWidget& InViewport, const Ray& InRay, Qt::KeyboardModifiers InModifiers)
    {
        (void)InViewport; (void)InRay; (void)InModifiers;
    }
    /* bInCancel: Esc or the viewport losing the button. */
    virtual void OnRelease(ViewportWidget& InViewport, bool bInCancel) { (void)InViewport; (void)bInCancel; }

    /* The cursor moved over the viewport (InRay) or left it (null). */
    virtual void OnHover(ViewportWidget& InViewport, const Ray* InRay, Qt::KeyboardModifiers InModifiers)
    {
        (void)InViewport; (void)InRay; (void)InModifiers;
    }

    /* Every viewport frame while set: a held stroke's continuous work. */
    virtual void Tick(ViewportWidget& InViewport, float InDeltaSeconds) { (void)InViewport; (void)InDeltaSeconds; }

    /* Every viewport frame while set: debug lines (EditorContext::DrawDebugLine). */
    virtual void QueueOverlay(ViewportWidget& InViewport) { (void)InViewport; }

    /* First refusal of keys and the wheel; true if used. */
    virtual bool OnKey(ViewportWidget& InViewport, QKeyEvent* InEvent) { (void)InViewport; (void)InEvent; return false; }
    virtual bool OnWheel(ViewportWidget& InViewport, QWheelEvent* InEvent) { (void)InViewport; (void)InEvent; return false; }
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_VIEWPORTTOOL_H

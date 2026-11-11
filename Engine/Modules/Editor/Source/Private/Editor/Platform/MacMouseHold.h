#ifndef NIMBLE_EDITOR_MACMOUSEHOLD_H
#define NIMBLE_EDITOR_MACMOUSEHOLD_H

#if defined(__APPLE__)

#include <functional>

/*
 * Play-in-Editor's held mouse on macOS (ViewportWidget::UpdateMouseHold).
 *
 * Elsewhere the viewport measures each move from its centre and warps the
 * cursor back (QCursor::setPos). On macOS that over-counts: the warp is not
 * synchronous with the event stream, so moves already queued still carry
 * positions measured from where the cursor was, and the same distance is
 * read again -- the look runs far too fast. And with the cursor pinned, Qt
 * drops the moves whose position has not changed.
 *
 * So, the way Mac games do it: the cursor is detached from the mouse
 * (CGAssociateMouseAndMouseCursorPosition) and stays put, and each move's own
 * delta (NSEvent deltaX/deltaY, in points, accelerated as the cursor would
 * be) comes from a local event monitor.
 */
namespace MacMouseHold
{
    /* InOnMove(DX, DY) for every mouse move (dragging too) until Stop. */
    void Start(std::function<void(double, double)> InOnMove);
    void Stop();
}

#endif

#endif // NIMBLE_EDITOR_MACMOUSEHOLD_H

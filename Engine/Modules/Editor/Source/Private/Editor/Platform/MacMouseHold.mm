#include "Private/Editor/Platform/MacMouseHold.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <utility>

namespace
{
    id                                  GMonitor = nil;
    std::function<void(double, double)> GOnMove;
}

void MacMouseHold::Start(std::function<void(double, double)> InOnMove)
{
    Stop();
    GOnMove = std::move(InOnMove);
    const NSEventMask Moves = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged
                            | NSEventMaskOtherMouseDragged;
    GMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:Moves handler:^NSEvent*(NSEvent* InEvent) {
        if(GOnMove && (InEvent.deltaX != 0.0 || InEvent.deltaY != 0.0))
        {
            GOnMove(InEvent.deltaX, InEvent.deltaY);   // y grows downwards, as the viewport's
        }
        return InEvent;
    }];
    CGAssociateMouseAndMouseCursorPosition(false);
}

void MacMouseHold::Stop()
{
    if(GMonitor)
    {
        [NSEvent removeMonitor:GMonitor];
        GMonitor = nil;
        CGAssociateMouseAndMouseCursorPosition(true);
    }
    GOnMove = nullptr;
}

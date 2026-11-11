#include "Public/GDI/GDIPlatform.h"

#include "Public/GDI/GDILog.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

/*
 * macOS implementation of GDI's graphics-window seam.
 *
 * WindowHandle::Native is an NSView* whose backing layer is a CAMetalLayer
 * (GameWindowNativeMac.cpp makes it with SDL_Metal_CreateView). Metal is the
 * only GPU backend on Apple (CMakeLists.txt, "Backend selection"), so OpenGL
 * and Vulkan have nothing here.
 */

namespace GDIPlatform
{
bool appGetWindowPixelSize(EngineCore::WindowHandle InWindow, uint32_t& OutWidth, uint32_t& OutHeight)
{
    if(!InWindow.IsValid())
    {
        return false;
    }

    NSView* View = (__bridge NSView*)InWindow.Native;

    /*
     * The layer's drawableSize is what the swapchain renders at, but it is
     * ours to set, so it cannot be the source. Physical pixels are the layer's
     * bounds times its contentsScale (SDL keeps that at the screen's backing
     * scale for a high-pixel-density window: 2 on a Retina display). Read off
     * the layer, not the view: the render thread asks too, AppKit view geometry
     * wants the main thread, and hopping there could deadlock against a main
     * thread waiting on the render thread.
     */
    CALayer* Layer = [View layer];
    if(Layer == nil)
    {
        return false;
    }
    const CGFloat Scale = [Layer contentsScale] > 0.0 ? [Layer contentsScale] : 1.0;
    NSRect Backing = [Layer bounds];
    Backing.size.width  *= Scale;
    Backing.size.height *= Scale;

    if(Backing.size.width <= 0.0 || Backing.size.height <= 0.0)
    {
        return false;
    }

    OutWidth  = uint32_t(Backing.size.width  + 0.5);
    OutHeight = uint32_t(Backing.size.height + 0.5);
    return true;
}
}

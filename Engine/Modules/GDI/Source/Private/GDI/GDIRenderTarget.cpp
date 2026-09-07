#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

void GDIRenderTarget::DrawTexture(GDITexture2D *InTexture2D)
{
    GDI_CHECK(GDIInRenderThread(), "Attempted to call GDIRenderTarget::DrawTexture function outside of Render Thread");
    if(!InTexture2D) return;

    // Route through the driver, not Bind(): on OpenGL SetRenderTarget IS Bind(),
    // but on Vulkan Bind() is a no-op — the driver must open a dynamic-rendering
    // region (and record layout transitions) or the clear and draw below are
    // silently dropped.
    GDISetRenderTarget(this);
    GDISetViewport(0, 0, Width, Height);
    GDISetColorMask(EColM_RGBA);
    GDIClear(CM_Color | CM_Depth, EngineCore::Color(255, 255, 255), 1.0);

    auto S_VS = GDIGetShader<Screen_VS>();
    auto S_PS = GDIGetShader<Screen_PS>();
    GDISetBoundShader(S_VS, S_PS);
    S_PS->SetScreenBlitTexture(InTexture2D);
    GDIDrawScreenQuadPrimitive();

    ResolveRenderTarget(this);
    GDIFlush();
}
#ifndef RENDERCORE_RENDERCOREINTERNAL_H
#define RENDERCORE_RENDERCOREINTERNAL_H

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/GDIView.h"
#include "Public/GDI/Null/NullDynamicGDI.h"
#include "Public/RenderCore/RenderTypes.h"

/* Guarded because the Vulkan backend is optional (GDI_WITH_VULKAN) and this
 * header pulls in vulkan.h through it -- GDI's own GDI.cpp guards its
 * equivalent include the same way, and GDIVulkanPlatform.h's comment states
 * the reason: a build with Vulkan off must not pay for vulkan.h, and a
 * machine without the SDK must still configure. RenderThread.cpp's
 * VulkanGDIContext construction sits behind the same #if. */
#if WITH_VULKAN_GDI
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#endif

/*
 * This is the ONLY header that includes a GDI header, and it lives under
 * Private/ so it cannot be reached from outside -- exactly the discipline
 * GameCoreInternal.h states for lua.h and GameWindowInternal.h states for
 * SDL.h ("this is the only header in the module that includes SDL.h"). That
 * is what makes the PRIVATE link of GDI::GDI in CMakeLists.txt true rather
 * than aspirational: no public RenderCore header ever names a GDI type, so a
 * consumer of RenderCore.dll never sees one either.
 *
 * RenderCore.dll is therefore the ONLY image in the process that links GDI.
 * See RenderCoreAPI.h and DESIGN.md for why that has to be true of a STATIC
 * library: two images linking it would each get their own GDynamicGDI, their
 * own GRenderCommands and their own render-thread latch, with nothing to say
 * so.
 *
 * GDI.h is included for InitializeGDI/AssignContextGDI/AllocateResourcesGDI/
 * ReleaseGDI, GDynamicGDI, GDISwapBuffers and the GDIBeginDrawEvent/EndDrawEvent
 * pair -- RenderThread.cpp and RenderContext.cpp's whole reason for including
 * this header. GDIView.h is included for GDIView/GDIViewObject/GDITransform,
 * which GDIShader.h (pulled in by GDI.h) only forward-declares --
 * RenderContext.cpp's DrawQuad needs the complete types to construct and
 * mutate them. NullDynamicGDI.h is included so a caller with no real
 * drawable (RenderCoreSmokeTest, and Sandbox's own ERP_Null default) can
 * hand AssignContextGDI a NullGDIContext, matching NimbleEngine.cpp's own
 * ERP_Null path before this module existed.
 */
namespace RenderCore
{
    /*
     * ERenderBackend mirrors ERenderPlatform value-for-value so a caller can
     * cast between them freely (NimbleEngine.cpp reads an ini int into one,
     * RenderThread.cpp hands the other to InitializeGDI). Checked here,
     * against the ACTUAL enum values, so a future reorder on either side --
     * including a subtree pull that reverts the deliberate GDI reorder this
     * module depends on -- fails the build instead of silently selecting the
     * wrong backend. See RenderTypes.h's own comment on why Null is zero.
     */
    static_assert(int(ERenderBackend::Null)   == int(ERP_Null),   "ERenderBackend::Null must match ERP_Null");
    static_assert(int(ERenderBackend::OpenGL) == int(ERP_OpenGL), "ERenderBackend::OpenGL must match ERP_OpenGL");
    static_assert(int(ERenderBackend::Vulkan) == int(ERP_Vulkan), "ERenderBackend::Vulkan must match ERP_Vulkan");
    static_assert(int(ERenderBackend::Metal)  == int(ERP_Metal),  "ERenderBackend::Metal must match ERP_Metal");
    static_assert(int(ERenderBackend::D3D12)  == int(ERP_D3D12),  "ERenderBackend::D3D12 must match ERP_D3D12");
}

#endif //RENDERCORE_RENDERCOREINTERNAL_H

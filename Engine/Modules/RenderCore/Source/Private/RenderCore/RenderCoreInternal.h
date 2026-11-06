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
 * THE ONLY HEADER THAT INCLUDES A GDI HEADER, and it lives under Private/ so
 * it cannot be reached from outside -- the same discipline GameCoreInternal.h
 * states for lua.h and GameWindowInternal.h for SDL.h. That is what makes the
 * PRIVATE link of GDI::GDI in CMakeLists.txt true rather than aspirational: no
 * public RenderCore header names a GDI type, so a consumer of RenderCore.dll
 * never sees one.
 *
 * RenderCore.dll is therefore the ONLY image in the process that links GDI.
 * Two images linking a STATIC GDI would each get their own GDynamicGDI,
 * GRenderCommands and render-thread latch, with nothing to say so -- see
 * RenderCoreAPI.h and DESIGN.md.
 *
 * What each include is for: GDI.h, the Initialize/AssignContext/
 * AllocateResources/Release entry points plus GDynamicGDI, GDISwapBuffers and
 * the GDIBeginDrawEvent/EndDrawEvent pair. GDIView.h, the complete
 * GDIView/GDIViewObject/GDITransform types that GDIShader.h only
 * forward-declares and RenderContext.cpp's DrawQuad must construct.
 * NullDynamicGDI.h, so a caller with no real drawable (RenderCoreSmokeTest,
 * Sandbox's ERP_Null default) can hand AssignContextGDI a NullGDIContext.
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

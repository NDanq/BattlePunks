#ifndef CORECLIENT_GDIVULKANPLATFORM_H
#define CORECLIENT_GDIVULKANPLATFORM_H

#include <vulkan/vulkan.h>

#include <vector>

#include "Public/Core/CoreTypes.h"

/*
 * The Vulkan half of GDI's window-system seam.
 *
 * Kept beside the Vulkan backend rather than in GDIPlatform.h so that the
 * OpenGL and Metal translation units never pull in vulkan.h for nothing --
 * the same reason GDIOpenGL.h lives under OpenGL/.
 *
 * Implemented per platform in Source/Private/GDI/Platform/. An SDL host swaps
 * these two functions for SDL_Vulkan_GetInstanceExtensions and
 * SDL_Vulkan_CreateSurface and changes nothing else in the backend.
 */

namespace GDIPlatform
{
    /*
     * Instance extensions this platform needs in order to present.
     *
     * Surface creation is the only platform-specific part of a Vulkan
     * instance, so this is the whole of what the window system contributes:
     * VK_KHR_surface plus the OS-specific surface extension.
     */
    std::vector<const char*> appVulkanRequiredInstanceExtensions();

    /*
     * Creates a presentable surface for InWindow.
     *
     * Replaces QVulkanInstance::surfaceForWindow. Returns the VkResult so the
     * caller can log it; OutSurface is left untouched on failure.
     */
    VkResult appVulkanCreateSurface(VkInstance InInstance,
                                    EngineCore::WindowHandle InWindow,
                                    VkSurfaceKHR* OutSurface);
}

#endif //CORECLIENT_GDIVULKANPLATFORM_H

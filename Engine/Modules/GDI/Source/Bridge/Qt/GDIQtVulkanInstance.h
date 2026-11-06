#ifndef CORECLIENT_GDIQTVULKANINSTANCE_H
#define CORECLIENT_GDIQTVULKANINSTANCE_H

#include <QVulkanInstance>

#include <vulkan/vulkan.h>

/*
 * Wraps GDI's VkInstance so a Qt window can be told about it.
 *
 * THE OWNERSHIP INVERSION
 *
 * This used to run the other way: Qt created a QVulkanInstance and GDI
 * borrowed the VkInstance out of it. A renderer should not need a UI toolkit
 * to open its device, so GDI creates the instance now
 * (VulkanGDIContext::GetSharedVkInstance) and this ADOPTS it via
 * QVulkanInstance::setVkInstance.
 *
 * Adoption rather than creation is what keeps both sides happy: Qt still
 * requires a QVulkanInstance on the window BEFORE QWindow::create() for a
 * VulkanSurface window, and GDI still owns the only VkInstance in the process.
 *
 * The QVulkanInstance does NOT own the VkInstance -- it is told about one that
 * already exists, and destroying it must not destroy GDI's.
 */

namespace GDIQt
{
    class GDIQtVulkanInstance
    {
    public:
        /*
         * Returns a process-wide QVulkanInstance wrapping GDI's VkInstance,
         * created on first use. Null if GDI's instance could not be created.
         *
         * Call before the window is created -- see the note above.
         */
        static QVulkanInstance* Get();
    };
}

#endif //CORECLIENT_GDIQTVULKANINSTANCE_H

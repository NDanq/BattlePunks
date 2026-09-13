#include "GDIQtVulkanInstance.h"

#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"

namespace GDIQt
{
QVulkanInstance* GDIQtVulkanInstance::Get()
{
    static QVulkanInstance* Wrapper = nullptr;
    if(Wrapper != nullptr)
    {
        return Wrapper;
    }

    // GDI owns the instance; this only adopts it.
    const VkInstance Native = VulkanGDIContext::GetSharedVkInstance();
    if(Native == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    Wrapper = new QVulkanInstance();

    /*
     * setVkInstance must precede create(). With an existing handle supplied,
     * create() adopts rather than creates -- it wires up Qt's function
     * resolution against the instance we already made, and does NOT take
     * ownership of it.
     */
    Wrapper->setVkInstance(Native);
    if(!Wrapper->create())
    {
        delete Wrapper;
        Wrapper = nullptr;
    }

    return Wrapper;
}
}

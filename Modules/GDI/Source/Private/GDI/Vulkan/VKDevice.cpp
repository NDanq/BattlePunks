#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include <mutex>


#include <vector>
#include <cstring>
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>

VKDevice::VKDevice() = default;

VKDevice::~VKDevice()
{
    Shutdown();
}

bool VKDevice::Initialize(VkInstance InInstance)
{
    // Borrowed, not owned -- GDI's VulkanGDIContext owns the instance.
    Instance = InInstance;
    if(Instance == VK_NULL_HANDLE)
    {
        GDI_LOG("[VKDevice] Null VkInstance.")
        return false;
    }

    if(!PickPhysicalDevice())
    {
        GDI_LOG("[VKDevice] No suitable physical device.")
        return false;
    }

    if(!CreateLogicalDevice())
    {
        GDI_LOG("[VKDevice] Failed to create logical device.")
        return false;
    }

    vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &MemoryProperties);

    GDI_LOG("[VKDevice] Initialized.")
    return true;
}

void VKDevice::Shutdown()
{
    if(Device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(Device);
        vkDestroyDevice(Device, nullptr);
        Device = VK_NULL_HANDLE;
    }
    GraphicsQueue = VK_NULL_HANDLE;
    PhysicalDevice = VK_NULL_HANDLE;
    Instance = VK_NULL_HANDLE;
    GraphicsQueueFamilyIndex = UINT32_MAX;
}

bool VKDevice::PickPhysicalDevice()
{
    uint32_t DeviceCount = 0;
    vkEnumeratePhysicalDevices(Instance, &DeviceCount, nullptr);
    if(DeviceCount == 0) return false;

    std::vector<VkPhysicalDevice> Devices(DeviceCount);
    vkEnumeratePhysicalDevices(Instance, &DeviceCount, Devices.data());

    // Score devices: prefer discrete GPU, must have a graphics-capable queue family.
    int BestScore = -1;
    VkPhysicalDevice Best = VK_NULL_HANDLE;
    uint32_t BestQueueFamily = UINT32_MAX;

    for(VkPhysicalDevice Candidate : Devices)
    {
        VkPhysicalDeviceProperties Props{};
        vkGetPhysicalDeviceProperties(Candidate, &Props);

        uint32_t QueueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &QueueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> QueueFamilies(QueueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &QueueFamilyCount, QueueFamilies.data());

        uint32_t QueueFamily = UINT32_MAX;
        for(uint32_t i = 0; i < QueueFamilyCount; ++i)
        {
            if(QueueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                QueueFamily = i;
                break;
            }
        }
        if(QueueFamily == UINT32_MAX) continue;

        int Score = 0;
        if(Props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)        Score += 1000;
        else if(Props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) Score += 100;
        Score += static_cast<int>(Props.limits.maxImageDimension2D / 1024);

        if(Score > BestScore)
        {
            BestScore = Score;
            Best = Candidate;
            BestQueueFamily = QueueFamily;
        }
    }

    if(Best == VK_NULL_HANDLE) return false;
    PhysicalDevice = Best;
    GraphicsQueueFamilyIndex = BestQueueFamily;
    {
        uint32_t FamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &FamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> Families(FamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &FamilyCount, Families.data());
        bGraphicsQueueCompute = GraphicsQueueFamilyIndex < FamilyCount
                             && (Families[GraphicsQueueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    }

    VkPhysicalDeviceProperties Props{};
    vkGetPhysicalDeviceProperties(PhysicalDevice, &Props);
    GDI_LOG(std::string("[VKDevice] Picked GPU: ") + Props.deviceName)
    return true;
}

bool VKDevice::CreateLogicalDevice()
{
    const float Priority = 1.0f;
    VkDeviceQueueCreateInfo QueueCreateInfo{};
    QueueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    QueueCreateInfo.queueFamilyIndex = GraphicsQueueFamilyIndex;
    QueueCreateInfo.queueCount       = 1;
    QueueCreateInfo.pQueuePriorities = &Priority;

    // Required device extensions for Phase 1: swapchain + dynamic rendering.
    const char* DeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    };

    VkPhysicalDeviceTimelineSemaphoreFeatures TimelineFeatures{};
    TimelineFeatures.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    TimelineFeatures.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceSynchronization2Features Sync2Features{};
    Sync2Features.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    Sync2Features.synchronization2 = VK_TRUE;
    Sync2Features.pNext            = &TimelineFeatures;

    VkPhysicalDeviceDynamicRenderingFeatures DynamicRenderingFeatures{};
    DynamicRenderingFeatures.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    DynamicRenderingFeatures.dynamicRendering = VK_TRUE;
    DynamicRenderingFeatures.pNext            = &Sync2Features;

    /* BC block formats (RenderCore's compressed textures), when there are any:
     * without them those textures are decoded to RGBA8 instead. */
    VkPhysicalDeviceFeatures Supported{};
    vkGetPhysicalDeviceFeatures(PhysicalDevice, &Supported);

    VkPhysicalDeviceFeatures2 Features2{};
    Features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    Features2.pNext = &DynamicRenderingFeatures;
    Features2.features.textureCompressionBC = Supported.textureCompressionBC;

    VkDeviceCreateInfo CreateInfo{};
    CreateInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    CreateInfo.pNext                = &Features2;
    CreateInfo.queueCreateInfoCount = 1;
    CreateInfo.pQueueCreateInfos    = &QueueCreateInfo;
    CreateInfo.enabledExtensionCount = static_cast<uint32_t>(sizeof(DeviceExtensions) / sizeof(DeviceExtensions[0]));
    CreateInfo.ppEnabledExtensionNames = DeviceExtensions;

    if(vkCreateDevice(PhysicalDevice, &CreateInfo, nullptr, &Device) != VK_SUCCESS)
    {
        return false;
    }

    vkGetDeviceQueue(Device, GraphicsQueueFamilyIndex, 0, &GraphicsQueue);
    bTextureCompressionBC = Supported.textureCompressionBC == VK_TRUE;
    return true;
}

bool VKDevice::SupportsSampledFormat(const VkFormat InFormat) const
{
    if(!bTextureCompressionBC || PhysicalDevice == VK_NULL_HANDLE) { return false; }
    VkFormatProperties Properties{};
    vkGetPhysicalDeviceFormatProperties(PhysicalDevice, InFormat, &Properties);
    const VkFormatFeatureFlags Needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                                      | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (Properties.optimalTilingFeatures & Needed) == Needed;
}

VkResult VKDevice::QueueSubmit(uint32_t SubmitCount, const VkSubmitInfo* Submits, VkFence Fence)
{
    std::lock_guard<std::mutex> Lock(QueueMutex);
    return vkQueueSubmit(GraphicsQueue, SubmitCount, Submits, Fence);
}

VkResult VKDevice::QueueSubmitLocked(uint32_t SubmitCount, const VkSubmitInfo* Submits, VkFence Fence)
{
    return vkQueueSubmit(GraphicsQueue, SubmitCount, Submits, Fence);
}

VkResult VKDevice::QueuePresent(const VkPresentInfoKHR* PresentInfo)
{
    std::lock_guard<std::mutex> Lock(QueueMutex);
    return vkQueuePresentKHR(GraphicsQueue, PresentInfo);
}

void VKDevice::WaitIdle()
{
    // vkDeviceWaitIdle requires external synchronization of all queues; lock the
    // queue mutex so it can't race a concurrent submit/present (e.g. a render-thread
    // resize WaitIdle vs. a UI-thread present).
    std::lock_guard<std::mutex> Lock(QueueMutex);
    if(Device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(Device);
    }
}

uint32_t VKDevice::FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Properties) const
{
    for(uint32_t i = 0; i < MemoryProperties.memoryTypeCount; ++i)
    {
        if((TypeBits & (1u << i)) &&
           (MemoryProperties.memoryTypes[i].propertyFlags & Properties) == Properties)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

void VKDevice::SetObjectName(const VkObjectType InType, const uint64_t InHandle, const std::string& InName) const
{
    if(InHandle == 0 || InName.empty() || Device == VK_NULL_HANDLE || !VulkanGDIContext::HasDebugUtils())
    {
        return;
    }
    static const auto SetName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(Instance, "vkSetDebugUtilsObjectNameEXT"));
    if(!SetName)
    {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT Info{};
    Info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    Info.objectType   = InType;
    Info.objectHandle = InHandle;
    Info.pObjectName  = InName.c_str();
    SetName(Device, &Info);
}

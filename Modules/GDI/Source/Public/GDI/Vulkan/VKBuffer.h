#ifndef CORECLIENT_VKBUFFER_H
#define CORECLIENT_VKBUFFER_H

#include <vulkan/vulkan.h>

#include "Public/GDI/GDIBuffer.h"

class VKDevice;

/**
 * Vulkan GDIBuffer: one device-local VkBuffer usable as a storage buffer
 * (compute), an instance vertex buffer and a transfer source and destination
 * (its initial upload, ReadBuffer). Filled once at creation by a synchronous
 * staging copy, or zeroed; after that only shaders write it.
 */
class VKBuffer final : public GDIBuffer
{
public:
    VKBuffer(VKDevice* InDevice, uint32_t InBytes, uint8_t InUsage, const void* InInitialData);
    ~VKBuffer() override;

    /** Defers the free past the frames that may still read it, then deletes this. */
    void Release() override;

    bool     IsValid()   const { return Buffer != VK_NULL_HANDLE; }
    VkBuffer GetBuffer() const { return Buffer; }

private:
    VKDevice*      Device = nullptr;
    VkBuffer       Buffer = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
};

#endif //CORECLIENT_VKBUFFER_H

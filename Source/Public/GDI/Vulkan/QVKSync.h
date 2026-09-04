#ifndef CORECLIENT_QVKSYNC_H
#define CORECLIENT_QVKSYNC_H

#include <vulkan/vulkan.h>

#include "Public/GDI/GDISync.h"

class QVKDevice;

/**
 * GDISync implementation backed by a single VkFence.
 *
 * The GDISync API mirrors what the OpenGL impl (QOGLSync) provides:
 *  - Fence(): record a fence point. Caller must already have submitted work that
 *             signals the underlying VkFence (we expose GetFence() for that).
 *  - WaitSync(): block until the fence is signaled.
 *
 * Phase 1 caller uses QVulkanDynamicGDI::EndRendering to submit work signaling this fence.
 */
class QVKSync : public GDISync
{
public:
    QVKSync();
    ~QVKSync();

    /** Bind to the device that will signal this fence. Re-entrant. */
    void Bind(QVKDevice* InDevice);

    void Initialize() override;
    void Fence() override;
    void WaitSync() override;
    void DestroySync() override;
    bool IsFenced() override;

    inline VkFence GetFence() const { return InternalFence; }

private:
    QVKDevice*  Device          = nullptr;
    VkFence     InternalFence   = VK_NULL_HANDLE;
    bool        bFenced         = false;
};

#endif //CORECLIENT_QVKSYNC_H

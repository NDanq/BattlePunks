#ifndef NIMBLE_MTLSYNC_H
#define NIMBLE_MTLSYNC_H

#include "Public/GDI/GDISync.h"

/*
 * GSync on Metal. Fence marks a point after the render thread commits; WaitSync
 * blocks until the GPU has finished every command buffer committed by then
 * (MetalDynamicGDI::WaitForCommitted).
 *
 * Plain C++ for the reason MetalDynamicGDI.h gives.
 */
class MTLSync : public GDISync
{
public:
    MTLSync() = default;
    ~MTLSync() override = default;

    void Initialize() override {}
    void Fence() override { bFenced = true; }
    void WaitSync() override;
    void DestroySync() override { bFenced = false; }
    bool IsFenced() override { return bFenced; }

private:
    bool bFenced = false;
};

#endif // NIMBLE_MTLSYNC_H

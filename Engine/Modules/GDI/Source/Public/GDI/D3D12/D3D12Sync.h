#ifndef CORECLIENT_D3D12SYNC_H
#define CORECLIENT_D3D12SYNC_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Public/GDI/GDISync.h"

class D3D12Device;

/**
 * GDISync over an ID3D12Fence. Fence() queues a signal behind everything
 * submitted so far; WaitSync() blocks until the GPU reaches it.
 */
class D3D12Sync : public GDISync
{
public:
    D3D12Sync() = default;
    ~D3D12Sync();

    void Bind(D3D12Device* InDevice);

    void Initialize() override;
    void Fence() override;
    void WaitSync() override;
    void DestroySync() override;
    bool IsFenced() override;

private:
    D3D12Device*                        Device      = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> InternalFence;
    uint64_t                            FenceValue  = 0;
    bool                                bFenced     = false;
};

#endif //CORECLIENT_D3D12SYNC_H

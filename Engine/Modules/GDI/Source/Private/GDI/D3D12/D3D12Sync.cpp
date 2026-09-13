#include "Public/GDI/D3D12/D3D12Sync.h"
#include "Public/GDI/D3D12/D3D12Device.h"

#include <mutex>

D3D12Sync::~D3D12Sync()
{
    DestroySync();
}

void D3D12Sync::Bind(D3D12Device* InDevice)
{
    if(Device == InDevice) return;
    DestroySync();
    Device = InDevice;
    Initialize();
}

void D3D12Sync::Initialize()
{
    if(!Device || !Device->IsValid() || InternalFence) return;
    Device->CreateFence(InternalFence);
    FenceValue = 0;
    bFenced    = false;
}

void D3D12Sync::Fence()
{
    if(!Device || !InternalFence) return;

    std::lock_guard<std::mutex> Lock(Device->GetQueueMutex());
    Device->GetQueue()->Signal(InternalFence.Get(), ++FenceValue);
    bFenced = true;
}

void D3D12Sync::WaitSync()
{
    if(!bFenced || !InternalFence) return;
    D3D12Device::WaitForFence(InternalFence.Get(), FenceValue);
    bFenced = false;
}

void D3D12Sync::DestroySync()
{
    InternalFence.Reset();
    FenceValue = 0;
    bFenced    = false;
}

bool D3D12Sync::IsFenced()
{
    return bFenced;
}

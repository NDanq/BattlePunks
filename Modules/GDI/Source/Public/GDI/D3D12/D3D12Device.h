#ifndef CORECLIENT_D3D12DEVICE_H
#define CORECLIENT_D3D12DEVICE_H

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>

/**
 * Owns the adapter, the ID3D12Device and the single direct queue.
 *
 * Both the render thread and the UI-thread present submit to the queue. The
 * queue itself is free-threaded, but a submit sometimes has to be atomic with
 * CPU-side bookkeeping (see D3D12RenderTarget::GetSubmittedState), so every
 * submit goes through GetQueueMutex().
 */
class D3D12Device
{
public:
    D3D12Device() = default;
    ~D3D12Device();

    bool Initialize();
    void Shutdown();

    /** Blocks until everything submitted so far has executed. Holds the queue mutex while waiting. */
    void WaitIdle();

    bool CreateFence(Microsoft::WRL::ComPtr<ID3D12Fence>& OutFence) const;

    /** Blocks until InFence reaches InValue. Returns at once on a removed device. */
    static void WaitForFence(ID3D12Fence* InFence, uint64_t InValue);

    inline std::mutex&          GetQueueMutex()       { return QueueMutex; }
    inline ID3D12Device*        GetDevice()  const    { return Device.Get(); }
    inline ID3D12CommandQueue*  GetQueue()   const    { return Queue.Get(); }
    inline IDXGIFactory4*       GetFactory() const    { return Factory.Get(); }
    inline bool                 IsValid()    const    { return Device != nullptr; }

private:
    bool PickAdapter();
    bool TryAdapter(IDXGIAdapter1* InAdapter);

    Microsoft::WRL::ComPtr<IDXGIFactory4>       Factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>       Adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>        Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>  Queue;

    Microsoft::WRL::ComPtr<ID3D12Fence>         IdleFence;
    uint64_t                                    IdleFenceValue = 0;

    /* The debug layer's message callback (debug builds, where the layer is
     * installed): its errors reach the GDI log, not only a debugger. */
    void HookDebugMessages();
    void UnhookDebugMessages();
    DWORD                                       DebugMessageCookie = 0;

    std::mutex                                  QueueMutex;
};

#endif //CORECLIENT_D3D12DEVICE_H

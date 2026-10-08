#ifndef CORECLIENT_D3D12COMMANDMANAGER_H
#define CORECLIENT_D3D12COMMANDMANAGER_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

class D3D12Device;

/**
 * One allocator + one direct command list, reused every frame. One instance
 * per recording thread (render thread, UI-thread present).
 */
class D3D12CommandManager
{
public:
    D3D12CommandManager() = default;
    ~D3D12CommandManager();

    bool Initialize(D3D12Device* InDevice);
    void Shutdown();

    /** Waits for the previous submission of the list, then resets and opens it. */
    ID3D12GraphicsCommandList* BeginFrame();

    void EndFrame();

    /** Executes the closed list and signals the frame fence behind it. Caller holds the queue mutex. */
    void SubmitLocked();

    /** Executes the closed list without signalling: a frame's first half, the
     *  rest recorded after Resume and covered by its SubmitLocked. Caller holds
     *  the queue mutex. */
    void ExecuteLocked();

    /** Reopens the list after ExecuteLocked, on the same allocator and with no
     *  wait: the allocator keeps both halves until BeginFrame rewinds it. */
    ID3D12GraphicsCommandList* Resume();

    /** Blocks until the last submitted list has executed. */
    void WaitForCompletion();

    inline ID3D12GraphicsCommandList* GetCommandList() const { return CommandList.Get(); }
    inline bool                       IsRecording()    const { return bRecording; }

private:
    D3D12Device*                                        Device          = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>   CommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>                 FrameFence;
    uint64_t                                            FrameFenceValue = 0;
    bool                                                bRecording      = false;
};

#endif //CORECLIENT_D3D12COMMANDMANAGER_H

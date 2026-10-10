#ifndef CORECLIENT_D3D12COMMANDMANAGER_H
#define CORECLIENT_D3D12COMMANDMANAGER_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Public/GDI/GDIDefinitions.h"

class D3D12Device;

/**
 * One direct command list, recorded each frame on its frame slot's allocator.
 * One instance per recording thread (render thread, UI-thread present).
 *
 * With several slots ([Rendering] FramesInFlight, the render thread's), the
 * list records frame N+1 on the next slot's allocator while the GPU runs
 * frame N: BeginFrame waits only for the submission that last used its slot,
 * FramesInFlight frames back -- and, the queue being in order, every one
 * before it. A list can be reset as soon as it has been executed; only an
 * allocator must wait for the GPU.
 */
class D3D12CommandManager
{
public:
    D3D12CommandManager() = default;
    ~D3D12CommandManager();

    bool Initialize(D3D12Device* InDevice, uint32_t InSlots = 1);
    void Shutdown();

    /** Waits for the current slot's last submission, then resets its allocator
     *  and opens the list on it. */
    ID3D12GraphicsCommandList* BeginFrame();

    void EndFrame();

    /** Executes the closed list and signals the frame fence behind it, the
     *  value the current slot then waits on. Caller holds the queue mutex. */
    void SubmitLocked();

    /** Executes the closed list without signalling: a frame's first half, the
     *  rest recorded after Resume and covered by its SubmitLocked. Caller holds
     *  the queue mutex. */
    void ExecuteLocked();

    /** Reopens the list after ExecuteLocked, on the same allocator and with no
     *  wait: the allocator keeps both halves until BeginFrame rewinds it. */
    ID3D12GraphicsCommandList* Resume();

    /** The next frame slot (after a frame's SubmitLocked; a mid-frame flush stays
     *  on its slot, whose BeginFrame then waits for everything submitted). */
    void Advance() { Slot = (Slot + 1) % SlotCount; }

    /** Blocks until everything submitted has executed. */
    void WaitForCompletion();

    /** The value the open frame's submit will signal: what a resource it uses
     *  waits for before it is freed or read back. */
    inline uint64_t GetNextFenceValue() const { return FrameFenceValue + 1; }

    /** Whether the submit that signals InValue has finished on the GPU (no wait). */
    bool IsComplete(uint64_t InValue) const;

    /** The last frame fence value the GPU has reached (every value, with no fence). */
    inline uint64_t GetCompletedValue() const { return FrameFence ? FrameFence->GetCompletedValue() : ~0ull; }

    inline uint32_t                   GetSlot()        const { return Slot; }
    inline uint32_t                   GetSlotCount()   const { return SlotCount; }
    inline ID3D12GraphicsCommandList* GetCommandList() const { return CommandList.Get(); }
    inline bool                       IsRecording()    const { return bRecording; }

private:
    D3D12Device*                                        Device          = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      Allocators[GDI_FRAMES_IN_FLIGHT_MAX];
    uint64_t                                            SlotFenceValue[GDI_FRAMES_IN_FLIGHT_MAX] = {};   // each slot's last submit's
    uint32_t                                            SlotCount       = 1;
    uint32_t                                            Slot            = 0;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>   CommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>                 FrameFence;
    uint64_t                                            FrameFenceValue = 0;
    bool                                                bRecording      = false;
};

#endif //CORECLIENT_D3D12COMMANDMANAGER_H

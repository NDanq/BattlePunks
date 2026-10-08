#include "Public/GDI/D3D12/D3D12CommandManager.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/GDILog.h"

D3D12CommandManager::~D3D12CommandManager()
{
    Shutdown();
}

bool D3D12CommandManager::Initialize(D3D12Device* InDevice, const uint32_t InSlots)
{
    Device = InDevice;
    if(!Device || !Device->IsValid()) return false;

    SlotCount = InSlots < 1 ? 1u : (InSlots > GDI_FRAMES_IN_FLIGHT_MAX ? GDI_FRAMES_IN_FLIGHT_MAX : InSlots);
    Slot      = 0;

    ID3D12Device* Dev = Device->GetDevice();
    for(uint32_t Each = 0; Each < SlotCount; ++Each)
    {
        if(FAILED(Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Allocators[Each]))))
        {
            GDI_LOG_ERROR("[D3D12CommandManager] CreateCommandAllocator failed.")
            return false;
        }
        SlotFenceValue[Each] = 0;
    }

    if(FAILED(Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocators[0].Get(), nullptr, IID_PPV_ARGS(&CommandList))))
    {
        GDI_LOG_ERROR("[D3D12CommandManager] CreateCommandList failed.")
        return false;
    }

    // Lists are created open; BeginFrame expects a closed one.
    CommandList->Close();

    return Device->CreateFence(FrameFence);
}

void D3D12CommandManager::Shutdown()
{
    if(FrameFence)
    {
        WaitForCompletion();
    }
    CommandList.Reset();
    for(auto& Allocator : Allocators) { Allocator.Reset(); }
    FrameFence.Reset();
    FrameFenceValue = 0;
    Device          = nullptr;
    bRecording      = false;
}

ID3D12GraphicsCommandList* D3D12CommandManager::BeginFrame()
{
    if(!CommandList) return nullptr;

    D3D12Device::WaitForFence(FrameFence.Get(), SlotFenceValue[Slot]);

    Allocators[Slot]->Reset();
    CommandList->Reset(Allocators[Slot].Get(), nullptr);
    bRecording = true;
    return CommandList.Get();
}

void D3D12CommandManager::EndFrame()
{
    if(!bRecording) return;
    CommandList->Close();
    bRecording = false;
}

void D3D12CommandManager::SubmitLocked()
{
    if(!Device || !CommandList) return;

    ID3D12CommandList* Lists[] = { CommandList.Get() };
    Device->GetQueue()->ExecuteCommandLists(1, Lists);
    Device->GetQueue()->Signal(FrameFence.Get(), ++FrameFenceValue);
    SlotFenceValue[Slot] = FrameFenceValue;
}

void D3D12CommandManager::ExecuteLocked()
{
    if(!Device || !CommandList) return;

    ID3D12CommandList* Lists[] = { CommandList.Get() };
    Device->GetQueue()->ExecuteCommandLists(1, Lists);
}

ID3D12GraphicsCommandList* D3D12CommandManager::Resume()
{
    if(!CommandList || bRecording) return nullptr;

    CommandList->Reset(Allocators[Slot].Get(), nullptr);
    bRecording = true;
    return CommandList.Get();
}

bool D3D12CommandManager::IsComplete(const uint64_t InValue) const
{
    return !FrameFence || FrameFence->GetCompletedValue() >= InValue;
}

void D3D12CommandManager::WaitForCompletion()
{
    D3D12Device::WaitForFence(FrameFence.Get(), FrameFenceValue);
}

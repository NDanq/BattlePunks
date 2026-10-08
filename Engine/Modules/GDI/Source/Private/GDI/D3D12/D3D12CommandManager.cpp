#include "Public/GDI/D3D12/D3D12CommandManager.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/GDILog.h"

D3D12CommandManager::~D3D12CommandManager()
{
    Shutdown();
}

bool D3D12CommandManager::Initialize(D3D12Device* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid()) return false;

    ID3D12Device* Dev = Device->GetDevice();
    if(FAILED(Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Allocator))))
    {
        GDI_LOG_ERROR("[D3D12CommandManager] CreateCommandAllocator failed.")
        return false;
    }

    if(FAILED(Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(), nullptr, IID_PPV_ARGS(&CommandList))))
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
    Allocator.Reset();
    FrameFence.Reset();
    FrameFenceValue = 0;
    Device          = nullptr;
    bRecording      = false;
}

ID3D12GraphicsCommandList* D3D12CommandManager::BeginFrame()
{
    if(!CommandList) return nullptr;

    WaitForCompletion();

    Allocator->Reset();
    CommandList->Reset(Allocator.Get(), nullptr);
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

    CommandList->Reset(Allocator.Get(), nullptr);
    bRecording = true;
    return CommandList.Get();
}

void D3D12CommandManager::WaitForCompletion()
{
    D3D12Device::WaitForFence(FrameFence.Get(), FrameFenceValue);
}

#include "Public/GDI/D3D12/D3D12Device.h"

#include <dxgi1_6.h>

#include <string>

#include "Public/GDI/GDILog.h"

using Microsoft::WRL::ComPtr;

D3D12Device::~D3D12Device()
{
    Shutdown();
}

bool D3D12Device::Initialize()
{
    UINT FactoryFlags = 0;

#ifndef NDEBUG
    // Absent unless the Graphics Tools optional feature is installed; not an error.
    ComPtr<ID3D12Debug> Debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug))))
    {
        Debug->EnableDebugLayer();
        FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    if(FAILED(CreateDXGIFactory2(FactoryFlags, IID_PPV_ARGS(&Factory))) &&
       FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory))))
    {
        GDI_LOG_ERROR("[D3D12Device] CreateDXGIFactory2 failed.")
        return false;
    }

    if(!PickAdapter())
    {
        GDI_LOG_ERROR("[D3D12Device] No adapter supports Direct3D 12.")
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC QueueDesc{};
    QueueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if(FAILED(Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(&Queue))))
    {
        GDI_LOG_ERROR("[D3D12Device] CreateCommandQueue failed.")
        Device.Reset();
        return false;
    }

    if(!CreateFence(IdleFence))
    {
        Queue.Reset();
        Device.Reset();
        return false;
    }

    GDI_LOG("[D3D12Device] Initialized.")
    return true;
}

void D3D12Device::Shutdown()
{
    if(Device)
    {
        WaitIdle();
    }
    IdleFence.Reset();
    Queue.Reset();
    Device.Reset();
    Adapter.Reset();
    Factory.Reset();
    IdleFenceValue = 0;
}

bool D3D12Device::PickAdapter()
{
    ComPtr<IDXGIFactory6> Factory6;
    if(SUCCEEDED(Factory.As(&Factory6)))
    {
        ComPtr<IDXGIAdapter1> Candidate;
        for(UINT i = 0;
            Factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&Candidate)) != DXGI_ERROR_NOT_FOUND;
            ++i)
        {
            if(TryAdapter(Candidate.Get())) return true;
        }
    }

    ComPtr<IDXGIAdapter1> Candidate;
    for(UINT i = 0; Factory->EnumAdapters1(i, &Candidate) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        if(TryAdapter(Candidate.Get())) return true;
    }
    return false;
}

bool D3D12Device::TryAdapter(IDXGIAdapter1* InAdapter)
{
    DXGI_ADAPTER_DESC1 Desc{};
    if(!InAdapter || FAILED(InAdapter->GetDesc1(&Desc))) return false;
    if(Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;

    if(FAILED(D3D12CreateDevice(InAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)))) return false;

    Adapter = InAdapter;

    char Name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, Desc.Description, -1, Name, int(sizeof(Name)) - 1, nullptr, nullptr);
    GDI_LOG(std::string("[D3D12Device] Picked GPU: ") + Name)
    return true;
}

bool D3D12Device::CreateFence(ComPtr<ID3D12Fence>& OutFence) const
{
    if(!Device || FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&OutFence))))
    {
        GDI_LOG_ERROR("[D3D12Device] CreateFence failed.")
        return false;
    }
    return true;
}

void D3D12Device::WaitForFence(ID3D12Fence* InFence, uint64_t InValue)
{
    // A null event makes SetEventOnCompletion block until the value is reached.
    if(InFence && InFence->GetCompletedValue() < InValue)
    {
        InFence->SetEventOnCompletion(InValue, nullptr);
    }
}

void D3D12Device::WaitIdle()
{
    if(!Queue || !IdleFence) return;

    std::lock_guard<std::mutex> Lock(QueueMutex);
    const uint64_t Value = ++IdleFenceValue;
    Queue->Signal(IdleFence.Get(), Value);
    WaitForFence(IdleFence.Get(), Value);
}

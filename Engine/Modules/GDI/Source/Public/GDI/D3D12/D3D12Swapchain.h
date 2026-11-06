#ifndef CORECLIENT_D3D12SWAPCHAIN_H
#define CORECLIENT_D3D12SWAPCHAIN_H

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/GDI/D3D12/D3D12CommandManager.h"

class D3D12Device;
class D3D12RenderTarget;

/**
 * UI-thread-owned flip-model swapchain plus a self-contained Screen.vsh/.fsh
 * blit: the render thread's pipeline cache targets the RT format and is not
 * safe to touch from this thread.
 *
 * A present's command allocator and blit SRV are one of kBufferCount slots, so
 * a present waits only for the one that last used its slot -- not the previous
 * present, which the GPU runs behind the render thread's frames still queued
 * ahead of it.
 *
 * With VSync off, presents never block (the MAILBOX behaviour the Vulkan
 * backend prefers): the chain's frame-latency waitable object says whether DXGI
 * will take another frame now, and when it will not, this one is not presented
 * -- the next present shows a newer frame. A blocking Present held a fast game
 * loop to DXGI's pace (GpuTest: 5.0 ms a frame on D3D12, 3.8 on Vulkan). With
 * VSync on (GDIGetVSync) that pace is the point: the present waits on the same
 * object and presents with sync interval 1.
 */
class D3D12Swapchain
{
public:
    D3D12Swapchain() = default;
    ~D3D12Swapchain();

    bool Initialize(D3D12Device* InDevice, EngineCore::WindowHandle InWindow);
    void Shutdown();

    /**
     * Blits InMainRT to the back buffer and presents. Skipped unless the RT was
     * left in PIXEL_SHADER_RESOURCE by the last list the render thread submitted;
     * that check and this submit happen under the same queue mutex.
     */
    void PresentFrame(D3D12RenderTarget* InMainRT);

    inline bool IsValid() const { return SwapChain != nullptr; }

private:
    bool CreateBackBufferViews();
    void ReleaseBackBuffers();
    bool ResizeIfNeeded();
    bool CreateBlitPipeline();
    bool LoadShader(const std::string& InPath, std::vector<uint8_t>& OutBytes) const;

    D3D12Device*                                        Device = nullptr;
    EngineCore::WindowHandle                            Window;

    Microsoft::WRL::ComPtr<IDXGISwapChain3>             SwapChain;
    HANDLE                                              FrameLatencyWaitable = nullptr;   // the chain's; closed at Shutdown
    bool                                                bLatencySignalHeld = false;       // taken from FrameLatencyWaitable, not yet presented
    static constexpr DWORD                              kVSyncWaitMs = 100;               // a stalled display skips the present, not the app
    static constexpr UINT                               kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    DXGI_FORMAT                                         Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t                                            Width  = 0;
    uint32_t                                            Height = 0;

    static constexpr UINT                               kBufferCount = 3;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> BackBuffers;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        BackBufferRTVs;

    D3D12CommandManager                                 UICommands;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>         BlitRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>         BlitPipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        BlitSRVHeap;   // a descriptor per UICommands slot
    UINT                                                BlitSRVIncrement = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource>              QuadVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                            QuadView{};
};

#endif //CORECLIENT_D3D12SWAPCHAIN_H

#include "Public/GDI/D3D12/D3D12Swapchain.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12RenderTarget.h"
#include "Public/GDI/D3D12/GDID3D12Platform.h"
#include "Public/GDI/GDIPlatform.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <climits>
#include <cstring>
#include <mutex>

#include "Public/Core/CoreFileProvider.h"
#include "Public/GDI/GDILog.h"

using Microsoft::WRL::ComPtr;

D3D12Swapchain::~D3D12Swapchain()
{
    Shutdown();
}

bool D3D12Swapchain::Initialize(D3D12Device* InDevice, EngineCore::WindowHandle InWindow)
{
    Device = InDevice;
    Window = InWindow;
    if(!Device || !Device->IsValid() || !Window.IsValid()) return false;

    if(!GDIPlatform::appGetWindowPixelSize(Window, Width, Height) || Width == 0 || Height == 0)
    {
        GDI_LOG("[D3D12Swapchain] Window has no drawable area yet.")
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 Desc{};
    Desc.Width            = Width;
    Desc.Height           = Height;
    Desc.Format           = Format;
    Desc.SampleDesc.Count = 1;
    Desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Desc.BufferCount      = kBufferCount;
    Desc.Scaling          = DXGI_SCALING_STRETCH;
    Desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> SwapChain1;
    const HRESULT Result = GDIPlatform::appD3D12CreateSwapChain(Device->GetFactory(), Device->GetQueue(),
                                                                Window, Desc, &SwapChain1);
    if(FAILED(Result) || FAILED(SwapChain1.As(&SwapChain)))
    {
        GDI_LOG_ERROR("[D3D12Swapchain] appD3D12CreateSwapChain failed (" + GDIHex(uint32_t(Result)) + ").")
        return false;
    }

    if(!CreateBackBufferViews())        return false;
    if(!UICommands.Initialize(Device))  return false;
    if(!CreateBlitPipeline())
    {
        GDI_LOG_ERROR("[D3D12Swapchain] Blit pipeline failed to build; presents will be skipped.")
        return false;
    }
    return true;
}

void D3D12Swapchain::Shutdown()
{
    UICommands.Shutdown();
    ReleaseBackBuffers();
    BackBufferRTVs.Reset();
    QuadVertexBuffer.Reset();
    BlitSRVHeap.Reset();
    BlitPipeline.Reset();
    BlitRootSignature.Reset();
    SwapChain.Reset();
    Device = nullptr;
    Window = EngineCore::WindowHandle();
}

bool D3D12Swapchain::CreateBackBufferViews()
{
    ID3D12Device* Dev = Device->GetDevice();

    if(!BackBufferRTVs)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc{};
        HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HeapDesc.NumDescriptors = kBufferCount;
        if(FAILED(Dev->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&BackBufferRTVs)))) return false;
    }

    const UINT Increment = Dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE Handle = BackBufferRTVs->GetCPUDescriptorHandleForHeapStart();

    BackBuffers.resize(kBufferCount);
    for(UINT i = 0; i < kBufferCount; ++i)
    {
        if(FAILED(SwapChain->GetBuffer(i, IID_PPV_ARGS(&BackBuffers[i])))) return false;
        Dev->CreateRenderTargetView(BackBuffers[i].Get(), nullptr, Handle);
        Handle.ptr += Increment;
    }
    return true;
}

void D3D12Swapchain::ReleaseBackBuffers()
{
    BackBuffers.clear();
}

bool D3D12Swapchain::ResizeIfNeeded()
{
    uint32_t NewWidth = 0, NewHeight = 0;
    if(!GDIPlatform::appGetWindowPixelSize(Window, NewWidth, NewHeight)) return false;

    // Minimized: nothing to present into.
    if(NewWidth == 0 || NewHeight == 0) return false;
    if(NewWidth == Width && NewHeight == Height) return true;

    // ResizeBuffers requires every back-buffer reference gone, including queued lists'.
    UICommands.WaitForCompletion();
    ReleaseBackBuffers();

    if(FAILED(SwapChain->ResizeBuffers(kBufferCount, NewWidth, NewHeight, Format, 0)))
    {
        GDI_LOG_ERROR("[D3D12Swapchain] ResizeBuffers failed.")
        return false;
    }
    Width  = NewWidth;
    Height = NewHeight;
    return CreateBackBufferViews();
}

bool D3D12Swapchain::LoadShader(const std::string& InPath, std::vector<uint8_t>& OutBytes) const
{
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(!Provider || !Provider->ReadAll(InPath, OutBytes) || OutBytes.empty())
    {
        GDI_LOG("[D3D12Swapchain] Failed to read shader bytecode: " + InPath)
        return false;
    }
    return true;
}

bool D3D12Swapchain::CreateBlitPipeline()
{
    ID3D12Device* Dev = Device->GetDevice();

    std::vector<uint8_t> VS, PS;
    if(!LoadShader("Shaders/Cooked/D3D12/Screen.vsh.cso", VS) ||
       !LoadShader("Shaders/Cooked/D3D12/Screen.fsh.cso", PS))
    {
        return false;
    }

    // Screen.fsh samples ScreenTexture at t2/s2; the Common.glsl cbuffers are unused and compiled out.
    D3D12_DESCRIPTOR_RANGE Range{};
    Range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors     = 1;
    Range.BaseShaderRegister = 2;

    D3D12_ROOT_PARAMETER Param{};
    Param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Param.DescriptorTable.NumDescriptorRanges = 1;
    Param.DescriptorTable.pDescriptorRanges   = &Range;
    Param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC Sampler{};
    Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    Sampler.ShaderRegister   = 2;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC RootDesc{};
    RootDesc.NumParameters     = 1;
    RootDesc.pParameters       = &Param;
    RootDesc.NumStaticSamplers = 1;
    RootDesc.pStaticSamplers   = &Sampler;
    RootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> Blob, Error;
    if(FAILED(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Error)) ||
       FAILED(Dev->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(), IID_PPV_ARGS(&BlitRootSignature))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc{};
    HeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = 1;
    HeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(Dev->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&BlitSRVHeap)))) return false;

    // Interleaved {pos3, uv2}. The swapchain's top row is NDC +1 and the RT's
    // row 0 its top (GDI.h): v = 0 at NDC +1 copies the rows as they are.
    const float QuadVerts[] = {
        -1.f, -1.f, 0.f,   0.f, 1.f,
        -1.f,  1.f, 0.f,   0.f, 0.f,
         1.f,  1.f, 0.f,   1.f, 0.f,
         1.f,  1.f, 0.f,   1.f, 0.f,
         1.f, -1.f, 0.f,   1.f, 1.f,
        -1.f, -1.f, 0.f,   0.f, 1.f,
    };
    QuadVertexBuffer = D3D12Ops::CreateBuffer(Device, sizeof(QuadVerts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!QuadVertexBuffer) return false;

    void* Mapped = nullptr;
    const D3D12_RANGE NoRead{ 0, 0 };
    if(FAILED(QuadVertexBuffer->Map(0, &NoRead, &Mapped))) return false;
    std::memcpy(Mapped, QuadVerts, sizeof(QuadVerts));
    QuadVertexBuffer->Unmap(0, nullptr);

    QuadView.BufferLocation = QuadVertexBuffer->GetGPUVirtualAddress();
    QuadView.SizeInBytes    = sizeof(QuadVerts);
    QuadView.StrideInBytes  = 5 * sizeof(float);

    const D3D12_INPUT_ELEMENT_DESC Layout[] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
    Desc.pRootSignature                        = BlitRootSignature.Get();
    Desc.VS                                    = { VS.data(), VS.size() };
    Desc.PS                                    = { PS.data(), PS.size() };
    Desc.InputLayout                           = { Layout, 2 };
    Desc.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
    Desc.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_ZERO;
    Desc.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    Desc.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    Desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    Desc.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    Desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    Desc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    Desc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    Desc.RasterizerState.DepthClipEnable       = TRUE;
    Desc.SampleMask                            = UINT_MAX;
    Desc.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    Desc.NumRenderTargets                      = 1;
    Desc.RTVFormats[0]                         = Format;
    Desc.SampleDesc.Count                      = 1;

    if(FAILED(Dev->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&BlitPipeline))))
    {
        GDI_LOG_ERROR("[D3D12Swapchain] CreateGraphicsPipelineState (blit) failed.")
        return false;
    }
    return true;
}

void D3D12Swapchain::PresentFrame(D3D12RenderTarget* InMainRT)
{
    if(!IsValid() || !BlitPipeline || !InMainRT || !InMainRT->IsValid()) return;

    // Cheap early-out; the authoritative check is repeated under the queue mutex below.
    if(InMainRT->GetSubmittedState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;

    if(!ResizeIfNeeded()) return;

    // Waits for the previous present, so rewriting the one SRV below is safe.
    ID3D12GraphicsCommandList* List = UICommands.BeginFrame();
    if(!List) return;

    ID3D12Device* Dev = Device->GetDevice();
    Dev->CreateShaderResourceView(InMainRT->GetResource(), nullptr, BlitSRVHeap->GetCPUDescriptorHandleForHeapStart());

    const UINT BackIndex = SwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* BackBuffer = BackBuffers[BackIndex].Get();

    D3D12_CPU_DESCRIPTOR_HANDLE RTV = BackBufferRTVs->GetCPUDescriptorHandleForHeapStart();
    RTV.ptr += SIZE_T(BackIndex) * Dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12Ops::RecordTransition(List, BackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_VIEWPORT Viewport{ 0.f, 0.f, float(Width), float(Height), 0.f, 1.f };
    const D3D12_RECT     Scissor{ 0, 0, LONG(Width), LONG(Height) };

    List->OMSetRenderTargets(1, &RTV, FALSE, nullptr);
    List->RSSetViewports(1, &Viewport);
    List->RSSetScissorRects(1, &Scissor);
    List->SetGraphicsRootSignature(BlitRootSignature.Get());
    ID3D12DescriptorHeap* Heaps[] = { BlitSRVHeap.Get() };
    List->SetDescriptorHeaps(1, Heaps);
    List->SetGraphicsRootDescriptorTable(0, BlitSRVHeap->GetGPUDescriptorHandleForHeapStart());
    List->SetPipelineState(BlitPipeline.Get());
    List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    List->IASetVertexBuffers(0, 1, &QuadView);
    List->DrawInstanced(6, 1, 0, 0);

    D3D12Ops::RecordTransition(List, BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    UICommands.EndFrame();

    // A render-thread submit may have landed since the early-out, leaving the RT
    // mid-frame at execution time. If so the recorded list is simply dropped.
    std::lock_guard<std::mutex> QueueLock(Device->GetQueueMutex());
    if(InMainRT->GetSubmittedState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;

    UICommands.SubmitLocked();

    // Sync interval 0 on a flip-discard chain replaces a queued frame rather than
    // waiting a vblank for it -- the MAILBOX behaviour the Vulkan backend prefers.
    const HRESULT Result = SwapChain->Present(0, 0);
    if(Result == DXGI_ERROR_DEVICE_REMOVED || Result == DXGI_ERROR_DEVICE_RESET)
    {
        GDI_LOG_ERROR("[D3D12Swapchain] Device lost during Present (" +
                      GDIHex(uint32_t(Dev->GetDeviceRemovedReason())) + ").")
    }
}

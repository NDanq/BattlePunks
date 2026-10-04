#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12CommandManager.h"
#include "Public/GDI/D3D12/D3D12DescriptorAllocator.h"
#include "Public/GDI/D3D12/D3D12RenderTarget.h"
#include "Private/GDI/D3D12/D3D12TransientOps.h"

#include <mutex>

#include "Public/GDI/GDILog.h"

#pragma region D3D12Util decoders

D3D12_BLEND_OP D3D12Util::DecodeBlendOp(EBlendOp InOp)
{
    switch(InOp)
    {
        case BO_Max: return D3D12_BLEND_OP_MAX;
        case BO_Min: return D3D12_BLEND_OP_MIN;
        case BO_Add:
        default:     return D3D12_BLEND_OP_ADD;
    }
}

D3D12_BLEND D3D12Util::DecodeBlendFactor(EBlendMode InMode)
{
    // D3D12 has one blend factor for colour and alpha, so the constant-alpha
    // modes share it with the constant-colour ones.
    switch(InMode)
    {
        case BM_Zero:                       return D3D12_BLEND_ZERO;
        case BM_One:                        return D3D12_BLEND_ONE;
        case BM_Src_Color:                  return D3D12_BLEND_SRC_COLOR;
        case BM_One_Minus_Src_Color:        return D3D12_BLEND_INV_SRC_COLOR;
        case BM_Dest_Color:                 return D3D12_BLEND_DEST_COLOR;
        case BM_One_Minus_Dest_Color:       return D3D12_BLEND_INV_DEST_COLOR;
        case BM_Src_Alpha:                  return D3D12_BLEND_SRC_ALPHA;
        case BM_One_Minus_Src_Alpha:        return D3D12_BLEND_INV_SRC_ALPHA;
        case BM_Dest_Alpha:                 return D3D12_BLEND_DEST_ALPHA;
        case BM_One_Minus_Dest_Alpha:       return D3D12_BLEND_INV_DEST_ALPHA;
        case BM_Constant_Color:
        case BM_Constant_Alpha:             return D3D12_BLEND_BLEND_FACTOR;
        case BM_One_Minus_Constant_Color:
        case BM_One_Minus_Constant_Alpha:   return D3D12_BLEND_INV_BLEND_FACTOR;
        case BM_Src_Saturate:               return D3D12_BLEND_SRC_ALPHA_SAT;
        default:                            break;
    }
    return D3D12_BLEND_ZERO;
}

UINT8 D3D12Util::DecodeColorMask(EColorMask InColorMask)
{
    switch(InColorMask)
    {
        case EColM_R:    return D3D12_COLOR_WRITE_ENABLE_RED;
        case EColM_G:    return D3D12_COLOR_WRITE_ENABLE_GREEN;
        case EColM_B:    return D3D12_COLOR_WRITE_ENABLE_BLUE;
        case EColM_A:    return D3D12_COLOR_WRITE_ENABLE_ALPHA;
        case EColM_RGB:  return D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        case EColM_RGBA: return D3D12_COLOR_WRITE_ENABLE_ALL;
        default:         return 0;
    }
}

D3D12_COMPARISON_FUNC D3D12Util::DecodeCompareFunc(ECompareTestMode InMode)
{
    switch(InMode)
    {
        case CTM_Never:    return D3D12_COMPARISON_FUNC_NEVER;
        case CTM_ALWAYS:   return D3D12_COMPARISON_FUNC_ALWAYS;
        case CTM_LESS:     return D3D12_COMPARISON_FUNC_LESS;
        case CTM_LEQUAL:   return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CTM_GREATER:  return D3D12_COMPARISON_FUNC_GREATER;
        case CTM_GEQUAL:   return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CTM_EQUAL:    return D3D12_COMPARISON_FUNC_EQUAL;
        case CTM_NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        default:           return D3D12_COMPARISON_FUNC_LESS;
    }
}

#pragma endregion

#pragma region Frame begin/end and clear

void D3D12DynamicGDI::BeginRendering()
{
    if(!CommandManager || !CurrentStateData) return;

    // Nested BeginRendering keeps recording into the open frame; re-beginning
    // would wait on a fence only the never-coming submit can signal.
    if(CurrentStateData->CurrentCmd) return;

    // BeginFrame waits for the previous frame, so its constant buffers and
    // released resources are no longer referenced.
    ID3D12GraphicsCommandList* List = CommandManager->BeginFrame();
    if(!List) return;
    if(DescAlloc) DescAlloc->Reset();
    DrainDeferredDestruction();
    CollectTimestamps();   // the previous block's, complete now its fence is waited

    CurrentStateData->CurrentCmd         = List;
    CurrentStateData->ActiveRenderTarget = nullptr;
    CurrentStateData->ActiveColorCount   = 0;
    CurrentStateData->ActiveDepth        = nullptr;
    CurrentStateData->bRenderingActive   = false;
    CurrentStateData->DebugEventDepth    = 0;

    PrepareCommandList(List);
}

void D3D12DynamicGDI::EndRendering()
{
    if(!CommandManager || !CurrentStateData || !CurrentStateData->CurrentCmd) return;

    // Leave the final target sampleable for the UI-thread present.
    if(D3D12RenderTarget* MainRT = CurrentStateData->ActiveRenderTarget)
    {
        if(MainRT->IsValid())
        {
            D3D12Ops::RecordTransition(CurrentStateData->CurrentCmd, MainRT->GetResource(),
                                       MainRT->GetCurrentState(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            MainRT->SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    SubmitFrame();

    CurrentStateData->CurrentCmd       = nullptr;
    CurrentStateData->bRenderingActive = false;

    bResizing.store(false);
}

void D3D12DynamicGDI::Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth, uint InStencilMask)
{
    if(!CurrentStateData || !CurrentStateData->bRenderingActive) return;

    D3D12RenderTarget* RT = CurrentStateData->ActiveRenderTarget;
    if(!RT || !RT->IsValid()) return;

    // Every bound colour target, and the bound depth.
    if(InClearMode & CM_Color)
    {
        const float Color[4] = { InClearColor.Rf(), InClearColor.Gf(), InClearColor.Bf(), InClearColor.Af() };
        const uint32_t ColorCount = CurrentStateData->ActiveColorCount > 0 ? CurrentStateData->ActiveColorCount : 1u;
        for(uint32_t Index = 0; Index < ColorCount; ++Index)
        {
            D3D12RenderTarget* Each = CurrentStateData->ActiveColors[Index] ? CurrentStateData->ActiveColors[Index] : RT;
            CurrentStateData->CurrentCmd->ClearRenderTargetView(Each->GetRTV(), Color, 0, nullptr);
        }
    }
    if((InClearMode & CM_Depth) && CurrentStateData->ActiveDepth)
    {
        CurrentStateData->CurrentCmd->ClearDepthStencilView(CurrentStateData->ActiveDepth->GetDSV(), D3D12_CLEAR_FLAG_DEPTH,
                                                            InDepth, 0, 0, nullptr);
    }
}

void D3D12DynamicGDI::SetViewport(uint InX, uint InY, uint InWidth, uint InHeight)
{
    if(!CurrentStateData) return;

    D3D12_VIEWPORT& V = CurrentStateData->ViewportRect;
    V.TopLeftX = float(InX);
    V.TopLeftY = float(InY);
    V.Width    = float(InWidth);
    V.Height   = float(InHeight);
    V.MinDepth = 0.0f;
    V.MaxDepth = 1.0f;

    D3D12_RECT& S = CurrentStateData->ScissorRect;
    S.left   = LONG(InX);
    S.top    = LONG(InY);
    S.right  = LONG(InX + InWidth);
    S.bottom = LONG(InY + InHeight);

    if(CurrentStateData->CurrentCmd)
    {
        CurrentStateData->CurrentCmd->RSSetViewports(1, &V);
        CurrentStateData->CurrentCmd->RSSetScissorRects(1, &S);
    }
}

#pragma endregion

#pragma region State tracking (consumed at pipeline build time)

void D3D12DynamicGDI::SetDepthState(IDepthStateDesc InDesc)
{
    if(CurrentStateData) CurrentStateData->DepthState = InDesc;
}

void D3D12DynamicGDI::SetBlendState(IBlendStateDesc InDesc)
{
    if(CurrentStateData) CurrentStateData->BlendState = InDesc;
}

void D3D12DynamicGDI::SetStencilState(IStencilStateDesc InDesc)
{
    if(CurrentStateData) CurrentStateData->StencilState = InDesc;
}

void D3D12DynamicGDI::SetColorMask(EColorMask InMask)
{
    if(CurrentStateData) CurrentStateData->ColorMask = InMask;
}

void D3D12DynamicGDI::BeginDrawEvent(const std::string& EventName)
{
    if(!CurrentStateData || !CurrentStateData->CurrentCmd) return;

    // Metadata 1 is PIX's ANSI-string encoding, readable without WinPixEventRuntime.
    CurrentStateData->CurrentCmd->BeginEvent(1, EventName.c_str(), UINT(EventName.size() + 1));
    ++CurrentStateData->DebugEventDepth;
}

void D3D12DynamicGDI::EndDrawEvent()
{
    if(!CurrentStateData || !CurrentStateData->CurrentCmd) return;

    // Only close events opened in this list; a mid-frame flush starts a new one.
    if(CurrentStateData->DebugEventDepth <= 0) return;

    CurrentStateData->CurrentCmd->EndEvent();
    --CurrentStateData->DebugEventDepth;
}

#pragma endregion

#pragma region GPU timestamps

void D3D12DynamicGDI::CreateTimestampHeap()
{
    UINT64 Frequency = 0;
    if(!DeviceObj || !DeviceObj->GetQueue() || FAILED(DeviceObj->GetQueue()->GetTimestampFrequency(&Frequency)) || Frequency == 0)
    {
        GDI_LOG("[D3D12DynamicGDI] The queue cannot report its timestamp frequency; GPU timing is off.")
        return;
    }

    D3D12_QUERY_HEAP_DESC HeapDesc{};
    HeapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    HeapDesc.Count = GDI_GPU_TIMESTAMP_MAX;
    if(FAILED(DeviceObj->GetDevice()->CreateQueryHeap(&HeapDesc, IID_PPV_ARGS(&TimestampHeap))))
    {
        TimestampHeap.Reset();
        return;
    }

    D3D12_HEAP_PROPERTIES Readback{};
    Readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC Buffer{};
    Buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Buffer.Width            = sizeof(uint64_t) * GDI_GPU_TIMESTAMP_MAX;
    Buffer.Height           = 1;
    Buffer.DepthOrArraySize = 1;
    Buffer.MipLevels        = 1;
    Buffer.SampleDesc.Count = 1;
    Buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(DeviceObj->GetDevice()->CreateCommittedResource(&Readback, D3D12_HEAP_FLAG_NONE, &Buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                              nullptr, IID_PPV_ARGS(&TimestampReadback))))
    {
        TimestampHeap.Reset();
        TimestampReadback.Reset();
        return;
    }
    TimestampTicksPerMs = double(Frequency) / 1000.0;
}

void D3D12DynamicGDI::WriteGPUTimestamp(const uint32_t InIndex)
{
    const uint64_t Bit = uint64_t(1) << InIndex;
    if(!TimestampHeap || !CurrentStateData || !CurrentStateData->CurrentCmd || InIndex >= GDI_GPU_TIMESTAMP_MAX
       || (TimestampsWritten & Bit))
    {
        return;
    }
    CurrentStateData->CurrentCmd->EndQuery(TimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, InIndex);
    TimestampsWritten |= Bit;
}

void D3D12DynamicGDI::ResolveTimestamps(ID3D12GraphicsCommandList* InList)
{
    // Each written query into its own slot of the readback buffer; a submit
    // mid-block (FlushForReadback) resolves what is written so far, the last
    // submit the rest.
    if(!TimestampHeap || !InList)
    {
        return;
    }
    for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
    {
        if(TimestampsWritten & (uint64_t(1) << Index))
        {
            InList->ResolveQueryData(TimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Index, 1, TimestampReadback.Get(),
                                     sizeof(uint64_t) * Index);
        }
    }
    TimestampsResolved |= TimestampsWritten;
}

void D3D12DynamicGDI::CollectTimestamps()
{
    PreviousTimestamps = 0;
    if(!TimestampReadback || TimestampsResolved == 0)
    {
        TimestampsWritten = 0;
        return;
    }
    D3D12_RANGE Range{ 0, sizeof(uint64_t) * GDI_GPU_TIMESTAMP_MAX };
    void* Mapped = nullptr;
    if(SUCCEEDED(TimestampReadback->Map(0, &Range, &Mapped)) && Mapped)
    {
        const uint64_t* Ticks = static_cast<const uint64_t*>(Mapped);
        for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
        {
            if(TimestampsResolved & (uint64_t(1) << Index))
            {
                PreviousTimestampMs[Index] = double(Ticks[Index]) / TimestampTicksPerMs;
            }
        }
        D3D12_RANGE Nothing{ 0, 0 };
        TimestampReadback->Unmap(0, &Nothing);
        PreviousTimestamps = TimestampsResolved;
    }
    TimestampsWritten  = 0;
    TimestampsResolved = 0;
}

uint64_t D3D12DynamicGDI::GetPreviousGPUTimestamps(double* OutMilliseconds)
{
    for(uint32_t Index = 0; Index < GDI_GPU_TIMESTAMP_MAX; ++Index)
    {
        if(PreviousTimestamps & (uint64_t(1) << Index))
        {
            OutMilliseconds[Index] = PreviousTimestampMs[Index];
        }
    }
    return PreviousTimestamps;
}

#pragma endregion

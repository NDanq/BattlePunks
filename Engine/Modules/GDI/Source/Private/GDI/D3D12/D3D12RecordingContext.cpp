#include "Public/GDI/D3D12/D3D12RecordingContext.h"
#include "Public/GDI/D3D12/D3D12CommandManager.h"
#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12Mesh.h"
#include "Public/GDI/D3D12/D3D12RenderTarget.h"
#include "Public/GDI/D3D12/D3D12Shader.h"
#include "Private/GDI/GDICorePack.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDIView.h"
#include "Public/GDI/GDILog.h"

#include <mutex>

/*
 * Parallel passes on D3D12 (GDIBeginParallelPass; RenderCore's
 * MULTITHREADED_RENDERING.md, phase 2).
 *
 * Each chunk records a direct command list of its own. A list inherits
 * nothing, so each binds the heaps, root signature, targets, viewport and
 * scissor as the frame list has them when the pass begins, then its own
 * pipelines, blocks and predicates draw by draw. EndParallelPass splits the
 * frame list round them: its first half is executed, then the chunks in chunk
 * order, and it reopens on the same allocator for the rest of the frame, the
 * targets bound again and its PIX events reopened. The frame fence behind
 * EndRendering's submit covers all of them.
 *
 * The first half and the chunks go in separate ExecuteCommandLists calls: a
 * buffer decays to COMMON between calls, so the instance and indirect-argument
 * buffers a chunk reads (written by compute in the first half) are promoted
 * implicitly, with no barrier -- a chunk cannot transition a buffer whose state
 * other threads track. Textures and render targets keep their states across
 * lists; the frame list transitioned them before the pass, and nothing in a
 * chunk changes them.
 */

namespace
{
    D3D12RecordingContext* AsD3D12(GDIRecordingContext* InContext)
    {
        return static_cast<D3D12RecordingContext*>(InContext);
    }

    D3D12ShaderResource* ResourceOf(GDIShader* InShader)
    {
        return InShader && InShader->Resource ? static_cast<D3D12ShaderResource*>(InShader->Resource) : nullptr;
    }

    bool UsesMaterialLayout(GDIShader* InShader)
    {
        return InShader && InShader->Resource && InShader->Resource->UsesMaterialLayout();
    }
}

void D3D12DynamicGDI::BindActiveTargets(ID3D12GraphicsCommandList* InList) const
{
    if(!InList || !CurrentStateData) return;

    D3D12_CPU_DESCRIPTOR_HANDLE RTVs[GDI_MAX_COLOR_TARGETS]{};
    const uint32_t Count = CurrentStateData->ActiveColorCount;
    for(uint32_t Index = 0; Index < Count; ++Index)
    {
        RTVs[Index] = CurrentStateData->ActiveColors[Index]->GetRTV();
    }
    if(const D3D12RenderTarget* Depth = CurrentStateData->ActiveDepth)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE DSV = Depth->GetDSV();
        InList->OMSetRenderTargets(Count, RTVs, FALSE, &DSV);
    }
    else
    {
        InList->OMSetRenderTargets(Count, RTVs, FALSE, nullptr);
    }
    InList->RSSetViewports(1, &CurrentStateData->ViewportRect);
    InList->RSSetScissorRects(1, &CurrentStateData->ScissorRect);
}

bool D3D12DynamicGDI::BeginParallelPass(const uint32_t InChunks, GDIRecordingContext** OutContexts)
{
    if(bParallelPass || !DeviceObj || !DeviceObj->IsValid() || !CommandManager || !CurrentStateData
       || !CurrentStateData->bRenderingActive || !CurrentStateData->CurrentCmd || !CurrentStateData->ActiveRenderTarget
       || CurrentStateData->ActiveColorCount == 0 || InChunks == 0 || InChunks > GDI_PARALLEL_CHUNKS_MAX)
    {
        return false;
    }
    ID3D12Device*  Dev  = DeviceObj->GetDevice();
    const uint32_t Slot = FrameSlot;

    /* The contexts, made on first need and kept, and each one's allocator for
     * this frame slot likewise; a list is made open, and closed here so each
     * pass can reopen it. */
    while(RecordingContexts.size() < InChunks)
    {
        RecordingContexts.push_back(new D3D12RecordingContext());
    }
    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        D3D12RecordingContext& Context = *RecordingContexts[Chunk];
        const std::string Name = "Recording Context " + std::to_string(Chunk);
        if(!Context.Allocators[Slot])
        {
            if(FAILED(Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Context.Allocators[Slot]))))
            {
                GDI_LOG("[D3D12DynamicGDI] a recording context's allocator could not be made; recording serially")
                return false;
            }
            D3D12Util::SetObjectName(Context.Allocators[Slot].Get(), Name + " Allocator " + std::to_string(Slot));
        }
        if(!Context.List)
        {
            if(FAILED(Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Context.Allocators[Slot].Get(), nullptr,
                                             IID_PPV_ARGS(&Context.List))))
            {
                GDI_LOG("[D3D12DynamicGDI] a recording context's list could not be made; recording serially")
                return false;
            }
            Context.List->Close();
            D3D12Util::SetObjectName(Context.List.Get(), Name);
        }
    }

    /* Every chunk's list reopened before anything is recorded: a failure here
     * closes those already open and leaves the frame as it was. */
    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        D3D12RecordingContext& Context = *RecordingContexts[Chunk];
        if(FAILED(Context.List->Reset(Context.Allocators[Slot].Get(), nullptr)))
        {
            GDI_LOG("[D3D12DynamicGDI] a recording context's list could not be reset; recording serially")
            for(uint32_t Open = 0; Open < Chunk; ++Open) { RecordingContexts[Open]->List->Close(); }
            return false;
        }
    }

    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        D3D12RecordingContext& Context = *RecordingContexts[Chunk];
        ID3D12GraphicsCommandList* List = Context.List.Get();
        PrepareCommandList(List);
        BindActiveTargets(List);

        /* The driver's state as the pass begins: what a serial issue would
         * start from. The caches are forgotten, so the context's first draw
         * uploads its blocks and its serials then only rise. */
        Context.BoundVertexShader = BoundVertexShader;
        Context.BoundPixelShader  = BoundPixelShader;
        for(uint32_t Block = 0; Block < GDI_UNIFORM_BLOCK_COUNT; ++Block)
        {
            Context.MaterialBlocks[Block]       = MaterialBlocks[Block];
            Context.MaterialBlockSerials[Block] = MaterialBlockSerials[Block];
        }
        for(uint32_t Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { Context.MaterialTextures[Slot] = MaterialTextures[Slot]; }
        Context.InstanceBuffer     = InstanceBuffer;
        Context.InstanceCount      = InstanceCount;
        Context.IndirectArgs       = IndirectArgs;
        Context.IndirectArgsOffset = IndirectArgsOffset;
        Context.IndexBuffer        = IndexBuffer;
        Context.PredicateSet       = ImmediatePredicateSet();
        Context.PredicateIndex     = PredicateIndex;
        Context.ViewportX          = ViewportX;
        Context.ViewportY          = ViewportY;
        Context.ViewportWidth      = ViewportWidth;
        Context.ViewportHeight     = ViewportHeight;
        Context.DrawFirst          = 0;
        Context.DrawCount          = 0;
        Context.Index              = Chunk;
        Context.DrawsRecorded      = 0;
        Context.Caches.Forget();   // a fresh list: nothing bound, and the uploads are this pass's
        Context.Blend          = CurrentStateData->BlendState;
        Context.ColorMask      = CurrentStateData->ColorMask;
        Context.Depth          = CurrentStateData->DepthState;
        Context.bCoreView      = false;
        Context.bCoreTransform = false;
        Context.EventDepth     = 0;
        Context.bRecording     = true;
        OutContexts[Chunk] = &Context;
    }
    ParallelChunks = InChunks;
    bParallelPass  = true;
    return true;
}

void D3D12DynamicGDI::EndParallelPass()
{
    if(!bParallelPass) return;

    ID3D12CommandList* Chunks[GDI_PARALLEL_CHUNKS_MAX] = {};
    for(uint32_t Chunk = 0; Chunk < ParallelChunks; ++Chunk)
    {
        D3D12RecordingContext& Context = *RecordingContexts[Chunk];
        while(Context.EventDepth > 0) { RecordEndDrawEvent(&Context); }
        Context.List->Close();
        Context.bRecording = false;
        Chunks[Chunk] = Context.List.Get();
    }

    /* The frame list's first half: its PIX events closed here, reopened in
     * the second. */
    ID3D12GraphicsCommandList* Frame = CurrentStateData->CurrentCmd;
    for(int Event = 0; Event < CurrentStateData->DebugEventDepth; ++Event) { Frame->EndEvent(); }
    CommandManager->EndFrame();
    {
        /* Each RT's state published with the submit, as SubmitFrame does: the
         * UI thread's present reads it. */
        std::lock_guard<std::mutex> QueueLock(DeviceObj->GetQueueMutex());
        for(D3D12RenderTarget* RT : LiveRenderTargets)
        {
            RT->PublishSubmittedState();
        }
        CommandManager->ExecuteLocked();
        DeviceObj->GetQueue()->ExecuteCommandLists(ParallelChunks, Chunks);
    }

    /* The second half, on the same allocator: a new list as far as buffer
     * states go, its targets and events as they were. */
    Frame = CommandManager->Resume();
    CurrentStateData->CurrentCmd = Frame;
    FrameListSerial = AllocateListSerial();
    if(Frame)
    {
        PrepareCommandList(Frame);
        BindActiveTargets(Frame);
        for(const std::string& Name : CurrentStateData->DebugEventNames)
        {
            Frame->BeginEvent(1, Name.c_str(), UINT(Name.size() + 1));
        }
    }
    else
    {
        CurrentStateData->bRenderingActive = false;
        CurrentStateData->DebugEventDepth  = 0;
        CurrentStateData->DebugEventNames.clear();
    }
    ParallelChunks = 0;
    bParallelPass  = false;
}

void D3D12DynamicGDI::ResetRecordingContexts(const uint32_t InSlot)
{
    for(D3D12RecordingContext* Context : RecordingContexts)
    {
        if(Context->Allocators[InSlot]) { Context->Allocators[InSlot]->Reset(); }
    }
}

void D3D12DynamicGDI::DestroyRecordingContexts()
{
    for(D3D12RecordingContext* Context : RecordingContexts)
    {
        delete Context;
    }
    RecordingContexts.clear();
}

void D3D12DynamicGDI::RecordDepthState(GDIRecordingContext* InContext, const IDepthStateDesc InDesc)
{
    AsD3D12(InContext)->Depth = InDesc;
}

void D3D12DynamicGDI::RecordBlendState(GDIRecordingContext* InContext, const IBlendStateDesc InDesc)
{
    AsD3D12(InContext)->Blend = InDesc;
}

void D3D12DynamicGDI::RecordStencilState(GDIRecordingContext* /*InContext*/, const IStencilStateDesc /*InDesc*/)
{
    // Stencil is never enabled in a D3D12 pipeline (D3D12PipelineCache::CreatePipeline), as SetStencilState.
}

void D3D12DynamicGDI::RecordColorMask(GDIRecordingContext* InContext, const EColorMask InMask)
{
    AsD3D12(InContext)->ColorMask = InMask;
}

void D3D12DynamicGDI::RecordViewport(GDIRecordingContext* InContext, const uint InX, const uint InY, const uint InWidth,
                                     const uint InHeight)
{
    D3D12RecordingContext& Context = *AsD3D12(InContext);
    if(!Context.bRecording) return;
    /* As SetViewport: the scissor follows the viewport. */
    const D3D12_VIEWPORT Viewport{ float(InX), float(InY), float(InWidth), float(InHeight), 0.0f, 1.0f };
    const D3D12_RECT     Scissor{ LONG(InX), LONG(InY), LONG(InX + InWidth), LONG(InY + InHeight) };
    Context.List->RSSetViewports(1, &Viewport);
    Context.List->RSSetScissorRects(1, &Scissor);
}

void D3D12DynamicGDI::RecordScissorRect(GDIRecordingContext* InContext, const uint InX, const uint InY, const uint InWidth,
                                        const uint InHeight)
{
    D3D12RecordingContext& Context = *AsD3D12(InContext);
    if(!Context.bRecording) return;
    const D3D12_RECT Scissor{ LONG(InX), LONG(InY), LONG(InX + InWidth), LONG(InY + InHeight) };
    Context.List->RSSetScissorRects(1, &Scissor);
}

void D3D12DynamicGDI::RecordDrawObject(GDIRecordingContext* InContext, GDIViewObject* InObject, GDIView* InView)
{
    D3D12RecordingContext& Context = *AsD3D12(InContext);
    if(!Context.bRecording || !InObject || !InObject->Mesh) return;

    /* The core blocks into the context, not onto the (shared) shaders. A
     * shader's user uniforms by name come from it as the render thread left
     * them: in a context they are read, never written. */
    if(InView)
    {
        GDICorePack::PackCoreView(InView, Context.CoreView);
        Context.bCoreView = true;
    }
    if(InObject->Transform)
    {
        GDICorePack::PackCoreTransform(InObject->Transform, Context.CoreTransform);
        Context.bCoreTransform = true;
    }

    D3D12DrawInputs In;
    In.List               = Context.List.Get();
    In.ListSerial         = 0;
    In.VS                 = ResourceOf(Context.BoundVertexShader);
    In.PS                 = ResourceOf(Context.BoundPixelShader);
    In.bMaterial          = UsesMaterialLayout(Context.BoundVertexShader) || UsesMaterialLayout(Context.BoundPixelShader);
    In.Blocks             = Context.MaterialBlocks;
    In.Serials            = Context.MaterialBlockSerials;
    In.Textures           = Context.MaterialTextures;
    In.Caches             = &Context.Caches;
    In.InstanceBuffer     = Context.InstanceBuffer;
    In.InstanceCount      = Context.InstanceCount;
    In.IndirectArgs       = Context.IndirectArgs;
    In.IndirectArgsOffset = Context.IndirectArgsOffset;
    In.IndexBuffer        = Context.IndexBuffer;
    In.Blend              = &Context.Blend;
    In.ColorMask          = Context.ColorMask;
    In.Depth              = &Context.Depth;
    In.PredicateSet       = UsablePredicate(Context.PredicateSet, Context.PredicateIndex);
    In.PredicateIndex     = In.PredicateSet ? Context.PredicateIndex : -1;
    In.CoreView           = Context.bCoreView ? Context.CoreView : nullptr;
    In.CoreTransform      = Context.bCoreTransform ? Context.CoreTransform : nullptr;
    In.DrawFirst          = Context.DrawFirst;
    In.DrawCount          = Context.DrawCount;
    static_cast<D3D12Mesh*>(InObject->Mesh)->Record(In);
}

void D3D12DynamicGDI::RecordBeginDrawEvent(GDIRecordingContext* InContext, const std::string& InName)
{
    D3D12RecordingContext& Context = *AsD3D12(InContext);
    if(!Context.bRecording) return;
    // Metadata 1 is PIX's ANSI-string encoding, as BeginDrawEvent.
    Context.List->BeginEvent(1, InName.c_str(), UINT(InName.size() + 1));
    ++Context.EventDepth;
}

void D3D12DynamicGDI::RecordEndDrawEvent(GDIRecordingContext* InContext)
{
    D3D12RecordingContext& Context = *AsD3D12(InContext);
    if(!Context.bRecording || Context.EventDepth <= 0) return;
    Context.List->EndEvent();
    --Context.EventDepth;
}

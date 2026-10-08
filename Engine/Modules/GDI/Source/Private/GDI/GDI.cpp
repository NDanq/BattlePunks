#include "Public/GDI/GDI.h"
#include "Public/Core/CorePlatform.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
// Unconditional on purpose: an earlier revision had these inside the
// WITH_METAL_GDI block, where they vanished on every non-Apple build.

#include <string>
#if WITH_OPENGL_GDI
#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"
#include "Public/GDI/OpenGL/OGLSync.h"
#endif
#include "Public/GDI/GDIView.h"
#include "Public/GDI/GDIMesh.h"
#include "Public/GDI/GDITexture.h"
#include "Public/GDI/GDISync.h"

#include "Public/GDI/Shaders/Classes/TestbedShaders.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"
#include "Public/GDI/Shaders/Classes/BrushShaders.h"

#if WITH_VULKAN_GDI
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/VKSync.h"
#endif

#if WITH_D3D12_GDI
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/D3D12/D3D12Sync.h"
#endif

#if WITH_METAL_GDI
#include "Public/GDI/Metal/MetalDynamicGDI.h"
#include "Public/GDI/Metal/MTLSync.h"
#endif

// Unconditional -- NullGDI has no platform dependency and is always
// available, unlike the three GPU-backed drivers above.
#include "Public/GDI/Null/NullDynamicGDI.h"
#include "Public/GDI/Null/NullSync.h"


GDIDynamicDriver*   GDynamicGDI = nullptr;
GDISync*            GSync = nullptr;
EngineCore::ThreadSafeQueue<GDIRenderCommandBase*> GRenderCommands;

std::atomic<int> GDebugGroupID{0};

/* The calling thread's recording context (GDISetCurrentRecording): while set,
 * the draw path's GDI* setters write into it, not the driver. */
namespace
{
    thread_local GDIRecordingContext* GCurrentRecording = nullptr;
}

/* Edit > Editor Settings > Shaders (GDI.h). Here, not in the editor: GDI is
 * static in RenderCore alone, so these register once. */
NIMBLE_CONFIG_BOOL(GShaderDebug, "Shaders", "ShaderDebug", true, nullptr);
NIMBLE_CONFIG_BOOL(GShaderPDB, "Shaders", "ShaderPDB", false, nullptr);
NIMBLE_CONFIG_STRING(GShaderPDBDir, "Shaders", "PDBDir", "", nullptr);

/* [Rendering] FramesInFlight: how many frames the CPU may record ahead of
 * the GPU (RenderCore's MULTITHREADED_RENDERING.md, phase 4). 1 waits for
 * each frame before recording the next; 2 records frame N+1 while the GPU
 * draws frame N. Read when the device is made (Vulkan, D3D12). */
NIMBLE_CONFIG_INT(GFramesInFlight, "Rendering", "FramesInFlight", 2, nullptr);

uint32_t GDIGetConfiguredFramesInFlight()
{
    const int Value = GFramesInFlight.GetInt();
    return Value < 1 ? 1u : (Value > int(GDI_FRAMES_IN_FLIGHT_MAX) ? GDI_FRAMES_IN_FLIGHT_MAX : uint32_t(Value));
}

bool GDIShaderDebugEnabled()
{
    return GShaderDebug.GetBool();
}

std::string GDIGetShaderPDBPath(const std::string& InCookedPath)
{
    static const std::string COOKED = "Shaders/Cooked/";
    if(!GShaderPDB.GetBool() || InCookedPath.compare(0, COOKED.size(), COOKED) != 0)
    {
        return std::string();
    }
    const char* Dir = GShaderPDBDir.GetString();
    std::string Root = Dir ? Dir : "";
    if(Root.empty())
    {
        return std::string();
    }
    if(Root.back() != '/' && Root.back() != '\\') { Root += '/'; }

    /* "Shaders/Cooked/Vulkan/Basic.vsh.spv" -> "<PDBDir>/Vulkan/Basic.vsh.spv";
     * D3D12's PDB beside its .cso's name, as the cook writes it. */
    std::string Path = Root + InCookedPath.substr(COOKED.size());
    static const std::string CSO = ".cso";
    if(Path.size() > CSO.size() && Path.compare(Path.size() - CSO.size(), CSO.size(), CSO) == 0)
    {
        Path.replace(Path.size() - CSO.size(), CSO.size(), ".pdb");
    }
    return std::ifstream(Path, std::ios::binary).good() ? Path : std::string();
}

void GDISetDebugName(GDIMesh* InMesh, const std::string& InName)
{
    if(InMesh) { InMesh->SetDebugName(InName); }
}

void GDISetDebugName(GDIBuffer* InBuffer, const std::string& InName)
{
    if(InBuffer) { InBuffer->SetDebugName(InName); }
}

void GDISetDebugName(GDITextureResource* InTexture, const std::string& InName)
{
    if(InTexture) { InTexture->SetDebugName(InName); }
}

// ---------------------------------------------------------------------------
// TRANSITIONAL DIAGNOSTIC -- migration risk F4. See the note in GDI.h.
// ---------------------------------------------------------------------------
namespace
{
    std::atomic<uint64_t> GEnqueueThread{0};
    std::atomic<uint64_t> GDrainThread{0};
    std::atomic<bool>     GbReportedMultiProducer{false};
    std::atomic<bool>     GbReportedMultiConsumer{false};
    std::atomic<bool>     GbReportedSameThread{false};
    std::atomic<bool>     GbReportedReentrant{false};

    // Set while the render thread is inside its drain loop, so an enqueue
    // issued from within a command's Execute() can be told apart from a normal
    // UI-thread enqueue. That distinction matters: a batch/snapshot drain would
    // silently defer such commands by a frame, where the current
    // dequeue-one-at-a-time loop runs them in the same frame.
    thread_local bool GbInDrain = false;
}

void GDIRenderQueue_NoteEnqueue()
{
    const uint64_t Me = uint64_t(EngineCore::appThreadID());

    uint64_t Expected = 0;
    if(GEnqueueThread.compare_exchange_strong(Expected, Me))
    {
        GDI_LOG("[GDI][F4] First render-command ENQUEUE on thread " + ::GDILogDetail::GDINum(Me))
    }
    else if(Expected != Me && !GbReportedMultiProducer.exchange(true))
    {
        GDI_LOG_ERROR("[GDI][F4] Render commands are enqueued from MORE THAN ONE thread ("
                      + ::GDILogDetail::GDINum(Expected) + " and " + ::GDILogDetail::GDINum(Me)
                      + "). The queue must be synchronized before the container changes.")
    }

    if(GbInDrain && !GbReportedReentrant.exchange(true))
    {
        GDI_LOG("[GDI][F4] Render command enqueued from INSIDE the drain loop -- the"
                " same-frame execution behaviour is real and must be preserved.")
    }

    const uint64_t Drainer = GDrainThread.load();
    if(Drainer != 0 && Drainer == Me && !GbReportedSameThread.exchange(true))
    {
        GDI_LOG("[GDI][F4] Enqueue and drain observed on the SAME thread (" + ::GDILogDetail::GDINum(Me)
                + ") -- no cross-thread hazard on this path.")
    }
}

void GDIRenderQueue_NoteDrain()
{
    const uint64_t Me = uint64_t(EngineCore::appThreadID());

    uint64_t Expected = 0;
    if(GDrainThread.compare_exchange_strong(Expected, Me))
    {
        GDI_LOG("[GDI][F4] First render-command DRAIN on thread " + ::GDILogDetail::GDINum(Me))
    }
    else if(Expected != Me && !GbReportedMultiConsumer.exchange(true))
    {
        GDI_LOG_ERROR("[GDI][F4] Render commands are drained from MORE THAN ONE thread ("
                      + ::GDILogDetail::GDINum(Expected) + " and " + ::GDILogDetail::GDINum(Me)
                      + "). IsEmpty()-then-Dequeue() is unsafe with multiple consumers.")
    }

    GbInDrain = true;
}

void GDIRenderQueue_NoteDrainEnd()
{
    // Cleared so a later UI-thread enqueue is not misreported as re-entrant.
    GbInDrain = false;
}

/*
 * The platform is now a PARAMETER rather than something GDI reaches out to
 * fetch. It used to call OdessaDeviceStats::Get()->GetRendererPlatform(),
 * which was the single functional dependency GDI had on the application --
 * everything else was just the logging macros. The host reads its own setting
 * and tells GDI what to build.
 */
void InitializeGDI( ERenderPlatform Platform, EngineCore::WindowHandle InWindow )
{
    {
        if(Platform == ERP_Null)
        {
            GDynamicGDI = new NullDynamicGDI(InWindow);
            GSync = new NullSync();
        }
        else
#if WITH_METAL_GDI
        if(Platform == ERP_Metal)
        {
            GDynamicGDI = new MetalDynamicGDI(InWindow);
            GSync = new MTLSync();
        }
        else
#endif
#if WITH_D3D12_GDI
        if(Platform == ERP_D3D12)
        {
            GDynamicGDI = new D3D12DynamicGDI(InWindow);
            GSync = new D3D12Sync();
        }
        else
#endif
#if WITH_VULKAN_GDI
        if(Platform == ERP_Vulkan)
        {
            GDynamicGDI = new VulkanDynamicGDI(InWindow);
            GSync = new VKSync();
        }
        else
#endif
        {
#if WITH_OPENGL_GDI
            GDynamicGDI = new OpenGLDynamicGDI(InWindow);
            GSync = new OGLSync();
#else
            // Apple builds compile out OpenGL and Vulkan entirely, so there is
            // no fallback backend: the host clamps the platform to ERP_Metal
            // and the branch above is the only one that can be taken.
            GDI_CHECK(0, "[GDI] No render backend available for the selected platform!")
#endif
        }
        //NOTICE Initialize Shaders Here.
        GDIRegisterShader<Basic_VS>("Basic.vsh", ST_Vertex);
        GDIRegisterShader<Basic_Instanced_VS>("Basic_Instanced.vsh", ST_Vertex);
        GDIRegisterShader<Screen_VS>("Screen.vsh", ST_Vertex);
        GDIRegisterShader<Basic_PS>("Basic.fsh", ST_Pixel);
        GDIRegisterShader<Cursor_PS>("Cursor.fsh", ST_Pixel);
        GDIRegisterShader<Screen_PS>("Screen.fsh", ST_Pixel);
        GDIRegisterShader<UberBrush_PS>("UberBrush.fsh", ST_Pixel);
        GDIRegisterShader<LayerBlit_PS>("LayersBlit.fsh", ST_Pixel);
        GDIRegisterShader<Checkerboard_PS>("Checkerboard.fsh", ST_Pixel);
        GDIRegisterShader<Mesh_VS>("Mesh.vsh", ST_Vertex);
        GDIRegisterShader<Mesh_PS>("Mesh.fsh", ST_Pixel);
    }
}

void AssignContextGDI( GDIContext* InContext )
{
    if( GDynamicGDI ) GDynamicGDI->AssignContext(InContext);
}

void AllocateResourcesGDI()
{
    if( GDynamicGDI ) GDynamicGDI->Allocate();
}

void ReleaseGDI()
{
    delete GDynamicGDI;
    GDynamicGDI = nullptr;

    // Previously leaked: nothing deleted or nulled GSync, so a second
    // InitializeGDI() call would leak the old one.
    delete GSync;
    GSync = nullptr;
}

bool GDIValidateMeshDesc(const GDIMeshDesc& InDesc, const bool bInNormalsOptional)
{
    if(!InDesc.Positions || (!InDesc.Normals && !bInNormalsOptional) || !InDesc.UV0 || !InDesc.Indices) return false;
    if(InDesc.NumVertices == 0 || InDesc.NumIndices == 0 || InDesc.NumIndices % 3 != 0) return false;
    for(uint32_t At = 0; At < InDesc.NumIndices; ++At)
    {
        if(InDesc.Indices[At] >= InDesc.NumVertices) return false;
    }
    return true;
}

bool GDIMakeIndexedRenderData(const GDIMeshDesc& InDesc, GDIMesh::ORenderData& OutData)
{
    if(!GDIValidateMeshDesc(InDesc, false)) return false;

    const auto Copy = [](const void* InSource, const size_t InBytes) -> void*
    {
        void* Block = malloc(InBytes);
        if(Block) memcpy(Block, InSource, InBytes);
        return Block;
    };

    GDIMesh::ORenderData Data;
    Data.Position   = static_cast<float*>(Copy(InDesc.Positions, sizeof(float) * 3 * InDesc.NumVertices));
    Data.Normal     = static_cast<float*>(Copy(InDesc.Normals, sizeof(float) * 3 * InDesc.NumVertices));
    Data.TexCoord   = static_cast<float*>(Copy(InDesc.UV0, sizeof(float) * 2 * InDesc.NumVertices));
    if(InDesc.Colors)
    {
        Data.Color = static_cast<float*>(Copy(InDesc.Colors, sizeof(float) * 4 * InDesc.NumVertices));
    }
    else if((Data.Color = static_cast<float*>(malloc(sizeof(float) * 4 * InDesc.NumVertices))) != nullptr)
    {
        /* White, so every backend's mesh layout always has a colour stream. */
        for(uint32_t At = 0; At < InDesc.NumVertices * 4; ++At) { Data.Color[At] = 1.0f; }
    }
    /* Zero without lightmap UVs, so the stream is always there too. */
    Data.UV1 = InDesc.UV1 ? static_cast<float*>(Copy(InDesc.UV1, sizeof(float) * 2 * InDesc.NumVertices))
                          : static_cast<float*>(calloc(size_t(InDesc.NumVertices) * 2, sizeof(float)));
    /* Skinning streams only when both are given: a static mesh has neither. */
    const bool bSkinned = InDesc.Joints && InDesc.Weights;
    if(bSkinned)
    {
        Data.Joints  = static_cast<float*>(Copy(InDesc.Joints,  sizeof(float) * 4 * InDesc.NumVertices));
        Data.Weights = static_cast<float*>(Copy(InDesc.Weights, sizeof(float) * 4 * InDesc.NumVertices));
    }
    Data.Indices    = static_cast<uint32_t*>(Copy(InDesc.Indices, sizeof(uint32_t) * InDesc.NumIndices));
    Data.NumIndices = InDesc.NumIndices;

    /* As CreateQuadMesh: the POSITION FLOAT count, not the vertex count. */
    Data.NumVertices = InDesc.NumVertices * 3;

    if(!Data.Position || !Data.Normal || !Data.TexCoord || !Data.Color || !Data.UV1 || !Data.Indices
       || (bSkinned && (!Data.Joints || !Data.Weights)))
    {
        free(Data.Position);
        free(Data.Normal);
        free(Data.Color);
        free(Data.UV1);
        free(Data.Joints);
        free(Data.Weights);
        free(Data.TexCoord);
        free(Data.Indices);
        return false;
    }
    OutData = Data;
    return true;
}

void GDIDynamicDriver::SetUniformBlock(const uint InBinding, const void* InData, const uint InSize)
{
    GDIStoreUniformBlock(MaterialBlocks, MaterialBlockSerials, InBinding, InData, InSize);
}

bool GDIStoreUniformBlock(std::vector<uint8_t> (&MaterialBlocks)[4], uint64_t (&MaterialBlockSerials)[4], const uint InBinding,
                          const void* InData, const uint InSize)
{
    uint Index = 0, MaxBytes = 0;
    switch(InBinding)
    {
        case GDI_MATERIAL_BLOCK_BINDING: Index = 0; MaxBytes = GDI_MATERIAL_BLOCK_MAX_BYTES; break;
        case GDI_SCENE_BLOCK_BINDING:    Index = 1; MaxBytes = GDI_SCENE_BLOCK_MAX_BYTES;    break;
        case GDI_SKIN_BLOCK_BINDING:     Index = 2; MaxBytes = GDI_SKIN_BLOCK_MAX_BYTES;     break;
        case GDI_PREV_SKIN_BLOCK_BINDING: Index = 3; MaxBytes = GDI_SKIN_BLOCK_MAX_BYTES;    break;
        default: return false;
    }
    std::vector<uint8_t>& Block = MaterialBlocks[Index];
    const uint Size = InData ? (InSize < MaxBytes ? InSize : MaxBytes) : 0;
    const auto* Bytes = static_cast<const uint8_t*>(InData);

    /* Unchanged bytes keep the serial, so a backend reuses its upload. */
    if(Block.size() == Size && (Size == 0 || memcmp(Block.data(), Bytes, Size) == 0)) return true;
    Block.assign(Bytes, Bytes + Size);
    ++MaterialBlockSerials[Index];
    return true;
}

void GDIDynamicDriver::SetRenderTargets(GDIRenderTarget* const* InColors, const uint InCount, GDIRenderTarget* InDepthFrom)
{
    if(!InColors || InCount == 0 || !InColors[0]) return;
    if(InCount > 1 || InDepthFrom != InColors[0])
    {
        static bool bWarned = false;
        if(!bWarned)
        {
            bWarned = true;
            GDI_LOG("[GDI] this backend binds one render target with its own depth; "
                    "SetRenderTargets draws into the first only")
        }
    }
    SetRenderTarget(InColors[0]);
}

void GDIDynamicDriver::SetMaterialTexture(const uint InSlot, GDITextureResource* InTexture)
{
    if(InSlot < GDI_MATERIAL_TEXTURE_SLOTS) MaterialTextures[InSlot] = InTexture;
}

bool GDIDynamicDriver::IsMaterialDraw() const
{
    return (BoundVertexShader && BoundVertexShader->Resource && BoundVertexShader->Resource->UsesMaterialLayout())
        || (BoundPixelShader  && BoundPixelShader->Resource  && BoundPixelShader->Resource->UsesMaterialLayout());
}

void GDIDynamicDriver::DrawObject(GDIViewObject *InObject)
{
    const auto View = InObject->View;
    DrawObject(InObject, View);
}

void GDIDynamicDriver::DrawObject(GDIViewObject *InObject, GDIView* InOverrideView)
{
    const auto Mesh = InObject->Mesh;
    const auto Transform = InObject->Transform;
    const auto View = InOverrideView;

    GDI_CHECK(BoundVertexShader, "[GDI] Bound Vertex Shader not set!")
    GDI_CHECK(BoundPixelShader, "[GDI] Bound Pixel Shader not set!")
    BoundVertexShader->UpdateCoreUniforms(View, Transform);
    BoundPixelShader->UpdateCoreUniforms(View,Transform);
    Mesh->Draw();
}

//Expose Global Functions
IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SwapBuffers,
                            (),
                            ,())

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            Flush,
                            (),
                            ,())

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            Clear,
                            (uint InColorMask, EngineCore::Color InColor, float InDepth, uint InStencil),
                            ,(InColorMask, InColor, InDepth, InStencil))

/* Hand-written: GDISetScissorRect clips to the viewport. */
void GDISetViewport(const uint InX, const uint InY, const uint InWidth, const uint InHeight)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        Rec->ViewportX = InX; Rec->ViewportY = InY; Rec->ViewportWidth = InWidth; Rec->ViewportHeight = InHeight;
        GDynamicGDI->RecordViewport(Rec, InX, InY, InWidth, InHeight);
        return;
    }
    GDynamicGDI->ViewportX      = InX;
    GDynamicGDI->ViewportY      = InY;
    GDynamicGDI->ViewportWidth  = InWidth;
    GDynamicGDI->ViewportHeight = InHeight;
    GDynamicGDI->SetViewport(InX, InY, InWidth, InHeight);
}

void GDISetScissorRect(const uint InX, const uint InY, const uint InWidth, const uint InHeight)
{
    if(!GDynamicGDI) return;
    const GDIDynamicDriver& D = *GDynamicGDI;
    GDIRecordingContext* Rec = GCurrentRecording;
    const uint VX = Rec ? Rec->ViewportX : D.ViewportX, VY = Rec ? Rec->ViewportY : D.ViewportY;
    const uint VW = Rec ? Rec->ViewportWidth : D.ViewportWidth, VH = Rec ? Rec->ViewportHeight : D.ViewportHeight;

    /* Clipped to the viewport. InY counts down from its top, as the target's
     * rows do: every backend stores row 0 at the top. */
    const uint Left   = InX < VW ? InX : VW;
    const uint Top    = InY < VH ? InY : VH;
    const uint Width  = InWidth < VW - Left ? InWidth : VW - Left;
    const uint Height = InHeight < VH - Top ? InHeight : VH - Top;
    if(Rec) { GDynamicGDI->RecordScissorRect(Rec, VX + Left, VY + Top, Width, Height); }
    else    { GDynamicGDI->SetScissorRect(VX + Left, VY + Top, Width, Height); }
}

void GDIClearScissorRect()
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        GDynamicGDI->RecordScissorRect(Rec, Rec->ViewportX, Rec->ViewportY, Rec->ViewportWidth, Rec->ViewportHeight);
        return;
    }
    const GDIDynamicDriver& D = *GDynamicGDI;
    GDynamicGDI->SetScissorRect(D.ViewportX, D.ViewportY, D.ViewportWidth, D.ViewportHeight);
}

void GDISetDepthState(IDepthStateDesc InDepthState)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording) { GDynamicGDI->RecordDepthState(Rec, InDepthState); return; }
    GDynamicGDI->SetDepthState(InDepthState);
}

void GDISetBlendState(IBlendStateDesc InBlendState)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording) { GDynamicGDI->RecordBlendState(Rec, InBlendState); return; }
    GDynamicGDI->SetBlendState(InBlendState);
}

void GDISetStencilState(IStencilStateDesc InStencilState)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording) { GDynamicGDI->RecordStencilState(Rec, InStencilState); return; }
    GDynamicGDI->SetStencilState(InStencilState);
}

void GDISetColorMask(EColorMask InColorMask)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording) { GDynamicGDI->RecordColorMask(Rec, InColorMask); return; }
    GDynamicGDI->SetColorMask(InColorMask);
}

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDIRenderTarget*,
                            CreateRenderTarget,
                            (std::string InName, uint InWidth, uint InHeight),
                            return,
                            (InName, InWidth, InHeight))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetRenderTarget,
                            (GDIRenderTarget* InRenderTarget),
                            ,
                            (InRenderTarget))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetRenderTargets,
                            (GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom),
                            ,
                            (InColors, InCount, InDepthFrom))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            ReleaseRenderTarget,
                            (GDIRenderTarget* InRenderTarget),
                            ,
                            (InRenderTarget))

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDIShaderResource*,
                            CreateShaderResource,
                            (EShaderType InShaderType, const std::string& SrcCode),
                            return,
                            (InShaderType, SrcCode))

/* Hand-written: counts the blocks, for dynamic meshes. */
void GDIBeginRendering()
{
    if(!GDynamicGDI) return;
    ++GDynamicGDI->RenderingSerial;
    GDynamicGDI->BeginRendering();
}

uint64_t GDIGetRenderingSerial()
{
    return GDynamicGDI ? GDynamicGDI->RenderingSerial : 0;
}

uint32_t GDIGetFramesInFlight()
{
    return GDynamicGDI ? GDynamicGDI->FramesInFlight : 1;
}

GDIMesh* GDICreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    if(!GDynamicGDI || InMaxVertices == 0 || InMaxIndices == 0) return nullptr;
    return GDynamicGDI->CreateDynamicMesh(InMaxVertices, InMaxIndices);
}

bool GDIUpdateDynamicMesh(GDIMesh* InMesh, const GDIMeshDesc& InDesc)
{
    return GDynamicGDI && InMesh && InMesh->UpdateDynamic(InDesc, GDynamicGDI->RenderingSerial, GDynamicGDI->FrameSlot);
}

bool GDIMesh::UpdateDynamic(const GDIMeshDesc& InDesc, const uint64_t InRenderingSerial, const uint32_t InFrameSlot)
{
    if(!bDynamic || !GDIValidateMeshDesc(InDesc, true)) return false;

    /* A new block: the frame slot's region was last written FramesInFlight
     * blocks ago, whose draws have finished (the backend waited for them
     * before recording this one), so start it over. */
    if(InRenderingSerial != DynamicSerial)
    {
        DynamicSerial     = InRenderingSerial;
        DynamicNextVertex = 0;
        DynamicNextIndex  = 0;
    }
    if(InDesc.NumVertices > DynamicVertexCapacity - DynamicNextVertex ||
       InDesc.NumIndices  > DynamicIndexCapacity  - DynamicNextIndex)
    {
        return false;
    }
    /* The slot's region of the buffers (the backend allocated one per frame in flight). */
    const uint32_t RegionVertex = InFrameSlot * DynamicVertexCapacity;
    const uint32_t RegionIndex  = InFrameSlot * DynamicIndexCapacity;
    if(!WriteDynamic(InDesc, RegionVertex + DynamicNextVertex, RegionIndex + DynamicNextIndex)) return false;

    DynamicBaseVertex       = RegionVertex + DynamicNextVertex;
    DynamicBaseIndex        = RegionIndex + DynamicNextIndex;
    DynamicNextVertex      += InDesc.NumVertices;
    DynamicNextIndex       += InDesc.NumIndices;
    RenderData.NumVertices  = InDesc.NumVertices * 3;   // position floats, as everywhere
    RenderData.NumIndices   = InDesc.NumIndices;
    DrawFirstIndex          = 0;
    DrawIndexCount          = 0;
    return true;
}

IMPLEMENT_GDI_FUNCTION_BODY(void,
                           EndRendering,
                            (), ,()
                            )

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            DrawScreenQuadPrimitive,
                            (), ,()
                            )

void GDIDrawObject(GDIViewObject* InViewObject)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        if(!InViewObject) return;
        GDynamicGDI->RecordDrawObject(Rec, InViewObject, InViewObject->View);
        ++Rec->DrawsRecorded;
        return;
    }
    GDynamicGDI->DrawObject(InViewObject);   // null included: a backend decides (Null counts it)
}

void GDIDrawObject(GDIViewObject* InViewObject, GDIView* InOverrideView)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        if(!InViewObject) return;
        GDynamicGDI->RecordDrawObject(Rec, InViewObject, InOverrideView);
        ++Rec->DrawsRecorded;
        return;
    }
    GDynamicGDI->DrawObject(InViewObject, InOverrideView);   // null included: a backend decides (Null counts it)
}

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDIMesh*,
                            CreateQuadMesh,
                            (const float* InVertexData, const float* InTexCoordData, const int InNumVerts, const int NumTexCoord, bool bIsInstanced),
                            return,
                            (InVertexData, InTexCoordData, InNumVerts, NumTexCoord, bIsInstanced)
                            )

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDIMesh*,
                            CreateQuadMesh,
                            (const float& InWidth, const float& InHeight, bool bIsInstanced),
                            return,
                            (InWidth, InHeight, bIsInstanced)
                            )

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDIMesh*,
                            CreateMesh,
                            (const GDIMeshDesc& InDesc),
                            return,
                            (InDesc)
                            )

void GDISetBoundShader(GDIShader* In_VS, GDIShader* In_PS)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        Rec->BoundVertexShader = In_VS;
        Rec->BoundPixelShader  = In_PS;
        return;
    }
    GDynamicGDI->SetBoundShader(In_VS, In_PS);
}

void GDISetUniformBlock(uint InBinding, const void* InData, uint InSize)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        GDIStoreUniformBlock(Rec->MaterialBlocks, Rec->MaterialBlockSerials, InBinding, InData, InSize);
        return;
    }
    GDynamicGDI->SetUniformBlock(InBinding, InData, InSize);
}

void GDISetMaterialTexture(uint InSlot, GDITextureResource* InTexture)
{
    if(!GDynamicGDI) return;
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        if(InSlot < GDI_MATERIAL_TEXTURE_SLOTS) { Rec->MaterialTextures[InSlot] = InTexture; }
        return;
    }
    GDynamicGDI->SetMaterialTexture(InSlot, InTexture);
}

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDITextureResource*,
                            CreateTextureResource,
                            (const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter),
                            return,
                            (InImage, InBorder, InFilter))

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDITextureResource*,
                                CreateTextureResource,
                                (GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter),
                                return,
                                (InParent, InImage, InBorder, InFilter))

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDITextureResource*,
                                CreateTextureResource,
                                (GDITexture2D* InParent, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter),
                                return,
                                (InParent, InBorder, InFilter))

/* A thread recording a chunk of a parallel pass counts: it records render
 * commands, on the render thread's behalf. */
bool GDIInRenderThread()
{
    if(!GDynamicGDI) return false;
    return GCurrentRecording != nullptr || GDynamicGDI->InRenderThread();
}

IMPLEMENT_GDI_FUNCTION_BODY(void,
                       RegisterTexture,
                       (const std::string& TextureKey, GDITexture2D* InTexture),,
                        (TextureKey, InTexture))

IMPLEMENT_GDI_FUNCTION_BODY_RET( GDITexture2D*,
                                GetTexture,
                                (const std::string& InTextureKey),
                                return,
                                (InTextureKey) )

IMPLEMENT_GDI_FUNCTION_BODY_RET(bool,
                                CheckError,
                                (const std::string& InFailStr),
                                return,
                                (InFailStr))

EngineCore::Color GDIGetColorFromSamplePoint(const EngineCore::Point& InPoint, const EngineCore::Size& InSize)
{
    if(GDynamicGDI)
    {
        return GDynamicGDI->GetColorFromSamplePoint(InPoint, InSize);
    }
    return EngineCore::Color();
}

/*
 * Hand-written rather than IMPLEMENT_GDI_FUNCTION_BODY_RET: that macro's
 * no-driver path is `return NULL`, which for an `enum : char` decays to 0 --
 * ERP_Null since GDIDefinitions.h's reorder, so the macro's own fallback
 * would now be the honest answer too. Kept hand-written for clarity rather
 * than out of necessity, matching GDIGetColorFromSamplePoint's hand-written
 * fallback just above.
 */
ERenderPlatform GDIGetRenderPlatform()
{
    if(GDynamicGDI)
    {
        return GDynamicGDI->GetRenderPlatform();
    }
    return ERP_Null;
}

/*
 * Hand-written for [Shaders] ShaderDebug: each Begin remembers whether it
 * reached the driver, so turning the setting over between a Begin and its
 * End neither leaves a region open nor closes one that was never begun.
 */
namespace
{
    std::vector<bool>& DrawEventsIssued()
    {
        thread_local std::vector<bool> Issued;
        return Issued;
    }
}

void GDIBeginDrawEvent(const std::string& InEventName)
{
    const bool bIssue = GDynamicGDI && GShaderDebug.GetBool();
    DrawEventsIssued().push_back(bIssue);
    if(bIssue)
    {
        if(GDIRecordingContext* Rec = GCurrentRecording) { GDynamicGDI->RecordBeginDrawEvent(Rec, InEventName); }
        else                                             { GDynamicGDI->BeginDrawEvent(InEventName); }
    }
}

void GDIEndDrawEvent()
{
    std::vector<bool>& Issued = DrawEventsIssued();
    if(Issued.empty())
    {
        return;
    }
    const bool bIssued = Issued.back();
    Issued.pop_back();
    if(bIssued && GDynamicGDI)
    {
        if(GDIRecordingContext* Rec = GCurrentRecording) { GDynamicGDI->RecordEndDrawEvent(Rec); }
        else                                             { GDynamicGDI->EndDrawEvent(); }
    }
}

bool GDIHasDrawEvents()
{
    return GDynamicGDI && GShaderDebug.GetBool() && GDynamicGDI->HasDrawEvents();
}

GDIRenderTarget* GDICreateRenderTargetFormat(std::string InName, const uint InWidth, const uint InHeight, const EGDIRenderTargetFormat InFormat)
{
    return GDynamicGDI ? GDynamicGDI->CreateRenderTargetFormat(std::move(InName), InWidth, InHeight, InFormat) : nullptr;
}

namespace
{
    float GMaxAnisotropy = 1.0f;   // GDISetMaxAnisotropy: what the backends' samplers ask for
}

void GDISetMaxAnisotropy(const float InMax)
{
    GMaxAnisotropy = InMax < 1.0f ? 1.0f : (InMax > 16.0f ? 16.0f : InMax);
}

float GDIGetMaxAnisotropy()
{
    return GMaxAnisotropy;
}

bool GDISupportsRenderTargetFormat(const EGDIRenderTargetFormat InFormat)
{
    return GDynamicGDI && GDynamicGDI->SupportsRenderTargetFormat(InFormat);
}

GDITextureResource* GDICreateTextureResourceMips(const EngineCore::Image* InLevels, const uint32_t InCount,
                                                 const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    return GDynamicGDI ? GDynamicGDI->CreateTextureResourceMips(InLevels, InCount, InBorder, InFilter) : nullptr;
}

bool GDISupportsCompressedFormat(const EGDICompressedFormat InFormat)
{
    return GDynamicGDI && InFormat < GDI_CF_MAX && GDynamicGDI->SupportsCompressedFormat(InFormat);
}

GDITextureResource* GDICreateCompressedTextureResource(const EGDICompressedFormat InFormat, const GDICompressedLevel* InLevels,
                                                       const uint32_t InCount, const EGDITextureBorder InBorder,
                                                       const EGDITextureFilter InFilter)
{
    if(!GDynamicGDI || !InLevels || InCount == 0 || !GDISupportsCompressedFormat(InFormat)) { return nullptr; }
    return GDynamicGDI->CreateCompressedTextureResource(InFormat, InLevels, InCount, InBorder, InFilter);
}

bool GDISupportsCompute()
{
    return GDynamicGDI && GDynamicGDI->SupportsCompute();
}

GDIBuffer* GDICreateBuffer(const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
{
    if(!GDISupportsCompute() || InBytes == 0 || (InUsage & (GDI_BU_Storage | GDI_BU_Instance)) == 0) { return nullptr; }
    return GDynamicGDI->CreateBuffer(InBytes, InUsage, InInitialData);
}

bool GDIDispatchCompute(GDIShader* InShader, const void* InParams, const uint32_t InParamBytes,
                        GDIBuffer* const* InBuffers, const uint32_t InBufferCount,
                        const uint32_t InX, const uint32_t InY, const uint32_t InZ)
{
    if(!GDISupportsCompute() || !InShader || !InShader->Resource || InShader->GetShaderType() != ST_Compute) { return false; }
    if(InParamBytes > GDI_COMPUTE_PARAMS_MAX_BYTES || (InParamBytes > 0 && !InParams)) { return false; }
    if(InBufferCount > GDI_COMPUTE_MAX_STORAGE_BUFFERS || (InBufferCount > 0 && !InBuffers)) { return false; }
    for(uint32_t Each = 0; Each < InBufferCount; ++Each)
    {
        if(!InBuffers[Each] || (InBuffers[Each]->GetUsage() & GDI_BU_Storage) == 0) { return false; }
    }
    if(InX == 0 || InY == 0 || InZ == 0) { return false; }
    return GDynamicGDI->DispatchCompute(InShader, InParams, InParamBytes, InBuffers, InBufferCount, InX, InY, InZ);
}

bool GDIReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes)
{
    OutBytes.clear();
    return GDISupportsCompute() && InBuffer && GDynamicGDI->ReadBuffer(InBuffer, OutBytes);
}

void GDISetInstanceBuffer(GDIBuffer* InBuffer, const uint32_t InCount)
{
    if(!GDynamicGDI) { return; }
    const bool bValid = GDynamicGDI->SupportsCompute() && InBuffer && InCount > 0
                     && (InBuffer->GetUsage() & GDI_BU_Instance) != 0
                     && uint64_t(InCount) * GDI_INSTANCE_STRIDE <= InBuffer->GetSize();
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        Rec->InstanceBuffer = bValid ? InBuffer : nullptr;
        Rec->InstanceCount  = bValid ? InCount : 0;
        return;
    }
    GDynamicGDI->InstanceBuffer = bValid ? InBuffer : nullptr;
    GDynamicGDI->InstanceCount  = bValid ? InCount : 0;
}

bool GDISupportsIndirectDraw()
{
    return GDynamicGDI && GDynamicGDI->SupportsIndirectDraw();
}

void GDISetIndirectArgs(GDIBuffer* InArgs, const uint32_t InOffsetBytes)
{
    if(!GDynamicGDI) { return; }
    const bool bValid = GDynamicGDI->SupportsIndirectDraw() && InArgs && (InArgs->GetUsage() & GDI_BU_Indirect) != 0
                     && InOffsetBytes % 4 == 0 && uint64_t(InOffsetBytes) + GDI_INDIRECT_ARGS_STRIDE <= InArgs->GetSize();
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        Rec->IndirectArgs       = bValid ? InArgs : nullptr;
        Rec->IndirectArgsOffset = bValid ? InOffsetBytes : 0;
        return;
    }
    GDynamicGDI->IndirectArgs       = bValid ? InArgs : nullptr;
    GDynamicGDI->IndirectArgsOffset = bValid ? InOffsetBytes : 0;
}

bool GDISupportsGPUTimestamps()
{
    return GDynamicGDI && GDynamicGDI->SupportsGPUTimestamps();
}

void GDIWriteGPUTimestamp(const uint32_t InIndex)
{
    if(GDynamicGDI && InIndex < GDI_GPU_TIMESTAMP_MAX)
    {
        GDynamicGDI->WriteGPUTimestamp(InIndex);
    }
}

uint64_t GDIGetPreviousGPUTimestamps(double* OutMilliseconds)
{
    return GDynamicGDI && OutMilliseconds ? GDynamicGDI->GetPreviousGPUTimestamps(OutMilliseconds) : 0;
}

bool GDISupportsOcclusionQueries()
{
    return GDynamicGDI && GDynamicGDI->SupportsOcclusionQueries();
}

bool GDISupportsOcclusionPredication()
{
    return GDISupportsOcclusionQueries() && GDynamicGDI->SupportsOcclusionPredication();
}

GDIParallelRecordingSupport GDIGetParallelRecordingSupport()
{
    if(!GDynamicGDI)
    {
        GDIParallelRecordingSupport None;
        None.Reason = "no GDI driver";
        return None;
    }
    return GDynamicGDI->GetParallelRecordingSupport();
}

bool GDISupportsParallelRecording()
{
    return GDIGetParallelRecordingSupport().bSupported;
}

GDIOcclusionQueries* GDICreateOcclusionQueries(const uint32_t InCapacity)
{
    return GDISupportsOcclusionQueries() && InCapacity > 0 ? GDynamicGDI->CreateOcclusionQueries(InCapacity) : nullptr;
}

void GDIBeginOcclusionRound(GDIOcclusionQueries* InSet, const uint32_t InCount)
{
    if(GDynamicGDI && InSet && InCount > 0 && InCount <= InSet->GetCapacity())
    {
        GDynamicGDI->BeginOcclusionRound(InSet, InCount);
    }
}

void GDIBeginOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    if(GDynamicGDI && InSet && InIndex < InSet->GetCapacity())
    {
        GDynamicGDI->BeginOcclusionQuery(InSet, InIndex);
    }
}

void GDIEndOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    if(GDynamicGDI && InSet && InIndex < InSet->GetCapacity())
    {
        GDynamicGDI->EndOcclusionQuery(InSet, InIndex);
    }
}

void GDIResolveOcclusionQueries(GDIOcclusionQueries* InSet)
{
    if(GDynamicGDI && InSet)
    {
        GDynamicGDI->ResolveOcclusionQueries(InSet);
    }
}

void GDISetOcclusionPredicate(GDIOcclusionQueries* InSet, const int32_t InIndex)
{
    if(!GDynamicGDI)
    {
        return;
    }
    const bool bSet = InSet && InIndex >= 0 && uint32_t(InIndex) < InSet->GetCapacity();
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        Rec->PredicateSet   = bSet ? InSet : nullptr;
        Rec->PredicateIndex = bSet ? InIndex : -1;
        return;
    }
    GDynamicGDI->SetOcclusionPredicate(bSet ? InSet : nullptr, bSet ? InIndex : -1);
}

bool GDIBeginParallelPass(const uint32_t InChunks, GDIRecordingContext** OutContexts)
{
    if(!GDynamicGDI || !OutContexts || InChunks == 0 || InChunks > GDI_PARALLEL_CHUNKS_MAX || GCurrentRecording)
    {
        return false;
    }
    return GDynamicGDI->BeginParallelPass(InChunks, OutContexts);
}

void GDIEndParallelPass()
{
    if(GDynamicGDI && !GCurrentRecording) { GDynamicGDI->EndParallelPass(); }
}

void GDISetCurrentRecording(GDIRecordingContext* InContext)
{
    GCurrentRecording = InContext;
}

GDIRecordingContext* GDIGetCurrentRecording()
{
    return GCurrentRecording;
}

void GDISetDrawRange(GDIMesh* InMesh, const uint32_t InFirst, const uint32_t InCount)
{
    if(GDIRecordingContext* Rec = GCurrentRecording)
    {
        Rec->DrawFirst = InFirst;
        Rec->DrawCount = InCount;
        return;
    }
    if(InMesh) { InMesh->SetDrawRange(InFirst, InCount); }
}

bool GDIReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible)
{
    return GDynamicGDI && InSet && GDynamicGDI->ReadOcclusionResults(InSet, OutVisible);
}
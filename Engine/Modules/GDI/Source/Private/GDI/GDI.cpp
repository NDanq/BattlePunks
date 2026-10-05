#include "Public/GDI/GDI.h"
#include "Public/Core/CorePlatform.h"

#include <atomic>
#include <cstring>

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

int GDebugGroupID = 0;

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
    Data.Indices    = static_cast<uint32_t*>(Copy(InDesc.Indices, sizeof(uint32_t) * InDesc.NumIndices));
    Data.NumIndices = InDesc.NumIndices;

    /* As CreateQuadMesh: the POSITION FLOAT count, not the vertex count. */
    Data.NumVertices = InDesc.NumVertices * 3;

    if(!Data.Position || !Data.Normal || !Data.TexCoord || !Data.Color || !Data.UV1 || !Data.Indices)
    {
        free(Data.Position);
        free(Data.Normal);
        free(Data.Color);
        free(Data.UV1);
        free(Data.TexCoord);
        free(Data.Indices);
        return false;
    }
    OutData = Data;
    return true;
}

void GDIDynamicDriver::SetUniformBlock(const uint InBinding, const void* InData, const uint InSize)
{
    if(InBinding < GDI_MATERIAL_BLOCK_BINDING || InBinding > GDI_SCENE_BLOCK_BINDING) return;
    const uint Index = InBinding - GDI_MATERIAL_BLOCK_BINDING;
    std::vector<uint8_t>& Block = MaterialBlocks[Index];
    const uint MaxBytes = InBinding == GDI_SCENE_BLOCK_BINDING ? GDI_SCENE_BLOCK_MAX_BYTES : GDI_MATERIAL_BLOCK_MAX_BYTES;
    const uint Size = InData ? (InSize < MaxBytes ? InSize : MaxBytes) : 0;
    const auto* Bytes = static_cast<const uint8_t*>(InData);

    /* Unchanged bytes keep the serial, so a backend reuses its upload. */
    if(Block.size() == Size && (Size == 0 || memcmp(Block.data(), Bytes, Size) == 0)) return;
    Block.assign(Bytes, Bytes + Size);
    ++MaterialBlockSerials[Index];
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

    /* Clipped to the viewport. InY counts down from its top, as the target's
     * rows do: every backend stores row 0 at the top. */
    const uint Left   = InX < D.ViewportWidth ? InX : D.ViewportWidth;
    const uint Top    = InY < D.ViewportHeight ? InY : D.ViewportHeight;
    const uint Width  = InWidth < D.ViewportWidth - Left ? InWidth : D.ViewportWidth - Left;
    const uint Height = InHeight < D.ViewportHeight - Top ? InHeight : D.ViewportHeight - Top;
    GDynamicGDI->SetScissorRect(D.ViewportX + Left, D.ViewportY + Top, Width, Height);
}

void GDIClearScissorRect()
{
    if(!GDynamicGDI) return;
    const GDIDynamicDriver& D = *GDynamicGDI;
    GDynamicGDI->SetScissorRect(D.ViewportX, D.ViewportY, D.ViewportWidth, D.ViewportHeight);
}

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetDepthState,
                            (IDepthStateDesc InDepthState),
                            ,
                            (InDepthState))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetBlendState,
                            (IBlendStateDesc InBlendState),
                            ,
                            (InBlendState))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetStencilState,
                            (IStencilStateDesc InStencilState),
                            ,
                            (InStencilState))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetColorMask,
                            (EColorMask InColorMask),
                            ,
                            (InColorMask))

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

GDIMesh* GDICreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    if(!GDynamicGDI || InMaxVertices == 0 || InMaxIndices == 0) return nullptr;
    return GDynamicGDI->CreateDynamicMesh(InMaxVertices, InMaxIndices);
}

bool GDIUpdateDynamicMesh(GDIMesh* InMesh, const GDIMeshDesc& InDesc)
{
    return GDynamicGDI && InMesh && InMesh->UpdateDynamic(InDesc, GDynamicGDI->RenderingSerial);
}

bool GDIMesh::UpdateDynamic(const GDIMeshDesc& InDesc, const uint64_t InRenderingSerial)
{
    if(!bDynamic || !GDIValidateMeshDesc(InDesc, true)) return false;

    /* A new block: the previous one's draws have finished (every backend
     * waits for them before recording again), so start over. */
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
    if(!WriteDynamic(InDesc, DynamicNextVertex, DynamicNextIndex)) return false;

    DynamicBaseVertex       = DynamicNextVertex;
    DynamicBaseIndex        = DynamicNextIndex;
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

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            DrawObject,
                            (GDIViewObject* InViewObject),,
                            (InViewObject)
                            )

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            DrawObject,
                            (GDIViewObject* InViewObject, GDIView* InOverrideView),,
                            (InViewObject, InOverrideView)
)

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

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetBoundShader,
                            (GDIShader* In_VS, GDIShader* In_PS), ,(In_VS, In_PS))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetUniformBlock,
                            (uint InBinding, const void* InData, uint InSize), ,(InBinding, InData, InSize))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetMaterialTexture,
                            (uint InSlot, GDITextureResource* InTexture), ,(InSlot, InTexture))

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

IMPLEMENT_GDI_FUNCTION_BODY_RET(bool,
                                InRenderThread,
                                (),
                                return,
                                ())

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

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            BeginDrawEvent,
                            (const std::string& InEventName),,
                            (InEventName))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            EndDrawEvent,
                            (),,())

bool GDIHasDrawEvents()
{
    return GDynamicGDI && GDynamicGDI->HasDrawEvents();
}

GDIRenderTarget* GDICreateRenderTargetFormat(std::string InName, const uint InWidth, const uint InHeight, const EGDIRenderTargetFormat InFormat)
{
    return GDynamicGDI ? GDynamicGDI->CreateRenderTargetFormat(std::move(InName), InWidth, InHeight, InFormat) : nullptr;
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
    GDynamicGDI->InstanceBuffer = bValid ? InBuffer : nullptr;
    GDynamicGDI->InstanceCount  = bValid ? InCount : 0;
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
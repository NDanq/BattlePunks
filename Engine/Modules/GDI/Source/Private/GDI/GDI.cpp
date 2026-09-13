#include "Public/GDI/GDI.h"
#include "Public/Core/CorePlatform.h"

#include <atomic>

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

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetViewport,
                            (uint InX, uint InY, uint InWidth, uint InHeight),
                            ,
                            (InX, InY, InWidth, InHeight))

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
                            ReleaseRenderTarget,
                            (GDIRenderTarget* InRenderTarget),
                            ,
                            (InRenderTarget))

IMPLEMENT_GDI_FUNCTION_BODY_RET(GDIShaderResource*,
                            CreateShaderResource,
                            (EShaderType InShaderType, const std::string& SrcCode),
                            return,
                            (InShaderType, SrcCode))

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            BeginRendering,
                            (), ,()
                            )

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

IMPLEMENT_GDI_FUNCTION_BODY(void,
                            SetBoundShader,
                            (GDIShader* In_VS, GDIShader* In_PS), ,(In_VS, In_PS))

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
 * ERP_OpenGL, the most misleading answer possible for "no driver is
 * installed". ERP_Null is the honest answer here, matching
 * GDIGetColorFromSamplePoint's hand-written fallback just above.
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
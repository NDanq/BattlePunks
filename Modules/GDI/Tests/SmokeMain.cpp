/*
 * GDISmokeTest -- links GDI and EngineCore only, deliberately not the Qt
 * bridge (see CMakeLists.txt). Its main job here is to exercise NullGDI, the
 * headless/server render backend: InitializeGDI(ERP_Null, ...) with no
 * window, no GPU context, and -- importantly -- no EngineCore::IFileProvider
 * installed at all. If NullGDI ever reaches for a file or a real GPU handle,
 * this test is where that shows up.
 *
 * A log handler is installed before anything else so a GDI_CHECK Fatal
 * (CoreLog.h) returns instead of aborting -- same reasoning as
 * EngineCoreSmokeTest: we want to report every failure, not die on the
 * first one.
 */

#include <cstdio>
#include <string>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreImage.h"

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/GDITexture.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/Null/NullDynamicGDI.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"

using namespace EngineCore;

static int GFailures = 0;
static int GChecks   = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static int GFatalCalls = 0;

static void TestLogHandler(const LogVerbosity Verbosity, const char* /*Message*/)
{
    if(Verbosity == LogVerbosity::Fatal)
    {
        ++GFatalCalls;
    }
}

// ---------------------------------------------------------------------------

static void TestInitAndPlatform()
{
    std::printf("[init]\n");

    InitializeGDI(ERP_Null, WindowHandle());
    Expect(GDynamicGDI != nullptr, "GDynamicGDI is set after InitializeGDI(ERP_Null)");
    Expect(GSync != nullptr, "GSync is set after InitializeGDI(ERP_Null)");
    Expect(GDIGetRenderPlatform() == ERP_Null, "GDIGetRenderPlatform reports ERP_Null");

    // Behaviour #1: Allocate() must latch the render thread without a
    // context. AssignContextGDI(nullptr) is legal input -- NullGDI never
    // dereferences its context.
    AssignContextGDI(nullptr);
    AllocateResourcesGDI();
    Expect(GDIInRenderThread(), "the calling thread is the render thread after AllocateResourcesGDI()");
}

static void TestFreeFunctionSweep()
{
    std::printf("[free functions]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    GDIBeginRendering();
    GDIClear(CM_Color | CM_Depth, EngineCore::Color(1, 2, 3), 1.0f, 0);
    GDISetViewport(0, 0, 640, 480);
    GDISetDepthState(IDepthStateDesc());
    GDISetBlendState(IBlendStateDesc());
    GDISetStencilState(IStencilStateDesc());
    GDISetColorMask(EColM_RGBA);
    GDIBeginDrawEvent("SmokeTestEvent");
    GDIEndDrawEvent();
    GDIEndRendering();
    GDISwapBuffers();
    GDIFlush();

    const NullGDIStats& Stats = Null->GetStats();
    Expect(Stats.ClearCalls == 1, "one Clear call counted");
    Expect(Stats.LastClearColor.R == 1 && Stats.LastClearColor.G == 2 && Stats.LastClearColor.B == 3,
           "Clear's color is recorded");
    Expect(Stats.LastViewportWidth == 640 && Stats.LastViewportHeight == 480, "SetViewport is recorded");
    Expect(Stats.BeginRenderingCalls == 1, "one BeginRendering call counted");
    Expect(Stats.EndRenderingCalls == 1, "one EndRendering call counted");
    Expect(Stats.SwapBufferCalls == 1, "one SwapBuffers call counted");
    Expect(Stats.FlushCalls == 1, "one Flush call counted");
    Expect(!GDICheckError("smoke"), "CheckError reports no error");
    Expect(GDIGetColorFromSamplePoint(Point(), Size()) == EngineCore::Color(), "GetColorFromSamplePoint returns a default-constructed Color");
}

static void TestResourceCreation()
{
    std::printf("[resource creation]\n");

    // GDIRenderTargetManager::Resize()/Release() (GDIRenderTarget.h) and
    // GDIShader::UpdateUserDefinedUniforms() (GDIShader.cpp) both dereference
    // whatever GDICreateXxx hands back with no null check -- every one of
    // these must be non-null.
    GDIRenderTarget* RT = GDICreateRenderTarget("SmokeRT", 64, 64);
    Expect(RT != nullptr, "GDICreateRenderTarget returns non-null");
    Expect(RT->IsValid(), "a Null render target reports itself valid");
    Expect(RT->ToGDITexture2D() != nullptr, "a Null render target's ToGDITexture2D is non-null");

    GDIMesh* Mesh = GDICreateQuadMesh(64.0f, 64.0f);
    Expect(Mesh != nullptr, "GDICreateQuadMesh(w,h) returns non-null");

    const EngineCore::Image ScratchImage(4, 4, EngineCore::EImageFormat::RGBA8);
    GDITextureResource* Tex = GDICreateTextureResource(ScratchImage);
    Expect(Tex != nullptr, "GDICreateTextureResource(Image) returns non-null");

    GDIShaderResource* Shader = GDICreateShaderResource(ST_Vertex, "Basic.vsh");
    Expect(Shader != nullptr, "GDICreateShaderResource returns non-null");

    // These three were created directly (not through GDIShader/GDITexture2D
    // ownership), so this test owns them and cleans them up itself.
    Tex->Release();
    delete Tex;
    Shader->Release();
    delete Shader;
    Mesh->Release();
    delete Mesh;
    GDIReleaseRenderTarget(RT);
    delete RT;
}

static void TestRenderTargetManagerCycle()
{
    std::printf("[render target manager]\n");

    // The unguarded-dereference path: every method here calls straight
    // through GDIRenderTarget* with no null check.
    GDIRenderTargetManager Manager;
    Manager.Allocate(64, 64);
    Expect(Manager.GetMain() != nullptr, "manager's Main target is non-null after Allocate");
    Expect(Manager.GetBuffer1() != nullptr, "manager's Buffer1 target is non-null after Allocate");

    Manager.Resize(128, 128);
    Expect(Manager.GetMain()->GetWidth() == 128, "Resize updates the Main target's width");

    Manager.ResolveCanvasBuffer();
    Manager.RestoreCanvasBuffer();
    Manager.Release();
}

static void TestDrawObjectOverrides()
{
    std::printf("[DrawObject]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    // GDIDynamicDriver::DrawObject (GDI.cpp) GDI_CHECKs BoundVertexShader /
    // BoundPixelShader -- never initialized by the base constructor -- and
    // then dereferences InObject->View and Mesh->Draw(). NullDynamicGDI
    // overrides both overloads instead of inheriting them, so a null object
    // here must be a harmless counted no-op, not a fatal or a crash.
    GDIDrawObject(nullptr);
    GDIDrawObject(nullptr, nullptr);

    Expect(Null->GetStats().DrawObjectCalls == 2, "both DrawObject overloads route to NullDynamicGDI and are counted");
    Expect(GFatalCalls == 0, "no GDI_CHECK fired while exercising DrawObject with null input");
}

/*
 * Exercises GDIRenderTarget::DrawTexture, which is the one call site that
 * combines GDI_CHECK(GDIInRenderThread(), ...) (fatal if Allocate() failed to
 * latch the thread) with GDIGetShader<Screen_VS/Screen_PS>() (fatal if no
 * file provider is reachable).
 *
 * The texture handed to DrawTexture is deliberately never freed here:
 * Screen_PS::SetScreenBlitTexture stores it in the shader's Uniforms map, and
 * GDIShader::CleanupAllocatedUniformData (GDIShader.cpp) deletes whatever is
 * stored there -- unconditionally, with no null check -- when the shader
 * itself is destroyed at ReleaseGDI(). Freeing it here as well would be a
 * double free; leaving Screen_PS's texture uniform at its default nullptr
 * would instead crash on that same unconditional delete. A single
 * test-owned, never-touched-again texture is the safe shape for this call.
 */
static void TestRenderTargetDrawTexture()
{
    std::printf("[DrawTexture]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    GDIRenderTarget* RT = GDICreateRenderTarget("DrawTextureRT", 32, 32);
    const EngineCore::Image ScratchImage(32, 32, EngineCore::EImageFormat::RGBA8);
    auto* ScratchTexture = new GDITexture2D(ScratchImage);

    RT->DrawTexture(ScratchTexture);

    Expect(GFatalCalls == 0, "DrawTexture does not hit a fatal GDI_CHECK on the render thread");
    Expect(Null->GetStats().DrawScreenQuadCalls == 1, "DrawTexture issues one DrawScreenQuadPrimitive call");
    Expect(Null->GetStats().LastBoundVertexShader != nullptr, "DrawTexture binds a vertex shader");
    Expect(Null->GetStats().LastBoundPixelShader != nullptr, "DrawTexture binds a pixel shader");

    GDIReleaseRenderTarget(RT);
    delete RT;
    // ScratchTexture is intentionally not deleted -- see the function comment.
}

static void TestRelease()
{
    std::printf("[release]\n");

    ReleaseGDI();
    Expect(GDynamicGDI == nullptr, "GDynamicGDI is null after ReleaseGDI");
    Expect(GSync == nullptr, "GSync is null after ReleaseGDI");
}

int main()
{
    SetLogHandler(&TestLogHandler);

    TestInitAndPlatform();
    TestFreeFunctionSweep();
    TestResourceCreation();
    TestRenderTargetManagerCycle();
    TestDrawObjectOverrides();
    TestRenderTargetDrawTexture();
    TestRelease();

    std::printf("\n%d/%d checks passed\n", GChecks - GFailures, GChecks);
    return GFailures == 0 ? 0 : 1;
}

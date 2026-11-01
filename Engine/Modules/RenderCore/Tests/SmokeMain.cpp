/*
 * RenderCoreSmokeTest -- links RenderCore and NOTHING else (see
 * CMakeLists.txt's comment: it must NOT link GDI::GDI, which is the
 * structural check that no GDI header has leaked into Public/).
 *
 * Through step 4 (WorldBridge + the Renderer facade) -- the whole surface
 * exists now; see DESIGN.md's "Build order" and this module's own "Smoke
 * test" section for the full case list. Written in GameCoreSmokeTest's
 * Expect()-counter style.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreJob.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/Core/CorePack.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/ParallelRecording.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreVersion.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/RenderThread.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/RenderCore/ShaderCache.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Public/RenderCore/TextureCompression.h"
#include "Public/RenderCore/UIDrawList.h"
#include "Private/RenderCore/RenderThreadInternal.h"
#include "Private/RenderCore/ParticleSimulation.h"
#include "Private/RenderCore/ShadowSetup.h"
#include "Private/RenderCore/TextureStreaming.h"

using namespace RenderCore;
using namespace EngineCore;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static void TestModuleName()
{
    std::printf("[module]\n");
    Expect(std::strcmp(GetModuleName(), "RenderCore") == 0,
           "GetModuleName() names this module, proving the link against RenderCore.dll");
}

/* RenderComponents.cpp registers these from RenderCore.dll into GameCore.dll's
 * one registry. The Lua side is covered by GameCore's own smoke test; this
 * proves the registration crosses the image boundary and lists what it should. */
static void TestScriptBindings()
{
    std::printf("[script bindings]\n");

    size_t RenderCount = 0;
    const GameCore::ScriptBindingEntry* Render =
        GameCore::FindComponentScript(RenderComponent::StaticType().ID, RenderCount);
    Expect(Render != nullptr && RenderCount == 14,
           "Render exposes Visible, Mesh, slots by index, material parameters, CastShadows, CastInsetShadow, Occluder, "
           "CustomPassGroups and RevealPlayers");
    const auto HasBinding = [&](const char* InName)
    {
        for(size_t Index = 0; Render && Index < RenderCount; ++Index)
        {
            if(std::strcmp(Render[Index].Name, InName) == 0) { return true; }
        }
        return false;
    };
    Expect(HasBinding("GetMaterial") && HasBinding("SetMaterial") && HasBinding("GetNumRegisteredMaterials")
           && !HasBinding("Material0") && !HasBinding("Material"),
           "material slots are reached by index, not one field per slot");
    Expect(HasBinding("SetMaterialParameter") && HasBinding("SetMaterialColor") && HasBinding("GetMaterialParameter")
           && HasBinding("ClearMaterialParameter"), "material parameters can be set, read and cleared");
    Expect(HasBinding("CastShadows"), "and whether it casts shadows");
    Expect(HasBinding("Occluder"), "and whether it occludes");
    Expect(HasBinding("CustomPassGroups"), "and its custom render pass mesh groups");
    Expect(HasBinding("RevealPlayers"), "and whether it is dithered where it hides a player");

    bool bProxyExposed = false;
    for(size_t Index = 0; Index < RenderCount; ++Index)
    {
        if(std::strstr(Render[Index].Name, "Proxy") != nullptr) { bProxyExposed = true; }
    }
    Expect(!bProxyExposed, "the render proxy handle is not exposed");

    size_t CameraCount = 0;
    GameCore::FindComponentScript(CameraComponent::StaticType().ID, CameraCount);
    Expect(CameraCount == 5, "Camera exposes ZNear, ZFar, Active, Projection and FovY");
}

static void TestThreadLifecycle(RenderThread& InThread)
{
    std::printf("[thread]\n");

    Expect(!IsInRenderThread(), "the test's own thread is not the render thread");

    std::atomic<ThreadID> SeenThreadID{0};
    ENQUEUE_RENDER_COMMAND(CaptureThreadID)([&SeenThreadID](RenderContext&)
    {
        SeenThreadID.store(appThreadID(), std::memory_order_release);
    });
    FlushRenderCommands();

    Expect(SeenThreadID.load(std::memory_order_acquire) == InThread.GetThreadID(),
           "a command runs on the thread RenderThread::GetThreadID() names");
    Expect(SeenThreadID.load(std::memory_order_acquire) != appThreadID(),
           "that thread is not this (the game/test) thread");
}

static void TestOrdering()
{
    std::printf("[ordering]\n");

    static std::atomic<int> NextIndex{0};
    static bool bOutOfOrder = false;

    for(int Index = 0; Index < 200; ++Index)
    {
        ENQUEUE_RENDER_COMMAND(OrderingCheck)([Index](RenderContext&)
        {
            const int Expected = NextIndex.fetch_add(1, std::memory_order_relaxed);
            if(Expected != Index)
            {
                bOutOfOrder = true;
            }
        });
    }
    FlushRenderCommands();

    Expect(NextIndex.load() == 200, "all 200 enqueued commands ran exactly once");
    Expect(!bOutOfOrder, "200 commands enqueued in order ran in that same order (FIFO)");
}

static void TestInlineExecution()
{
    std::printf("[inline]\n");

    static bool bInnerRanFirst = false;

    ENQUEUE_RENDER_COMMAND(Outer)([](RenderContext&)
    {
        bool bInnerRan = false;
        ENQUEUE_RENDER_COMMAND(Inner)([&bInnerRan](RenderContext&)
        {
            bInnerRan = true;
        });
        bInnerRanFirst = bInnerRan;
    });
    FlushRenderCommands();

    Expect(bInnerRanFirst, "a command enqueued from inside another already-executing command runs inline, before control returns");
}

static void TestMarkers(RenderThread& InThread)
{
    std::printf("[markers]\n");

    RenderStats Before;
    InThread.GetStats(Before);

    static bool bARan = false;
    static bool bBRan = false;
    bARan = false;
    bBRan = false;

    ENQUEUE_RENDER_COMMAND(MarkA)([](RenderContext&) { bARan = true; });
    InThread.EndFrame(Before.FramesRendered + 1);

    ENQUEUE_RENDER_COMMAND(MarkB)([](RenderContext&) { bBRan = true; });

    InThread.WaitForFrame();

    Expect(bARan, "a command enqueued before EndFrame() ran once WaitForFrame() returns");
    Expect(!bBRan, "a command enqueued AFTER EndFrame() sits behind that frame's marker and has not run yet");

    RenderStats AfterFirstFrame;
    InThread.GetStats(AfterFirstFrame);
    Expect(AfterFirstFrame.FramesRendered == Before.FramesRendered + 1, "FramesRendered advances by exactly one per EndFrame()");

    InThread.EndFrame(Before.FramesRendered + 2);
    InThread.WaitForFrame();

    Expect(bBRan, "the command from the previous frame ran once the NEXT frame's marker is reached");

    RenderStats AfterSecondFrame;
    InThread.GetStats(AfterSecondFrame);
    Expect(AfterSecondFrame.FramesRendered == Before.FramesRendered + 2, "FramesRendered advances again on the second EndFrame()");

    const uint64_t BeforeFlushFrames = AfterSecondFrame.FramesRendered;
    InThread.Flush();
    RenderStats AfterFlush;
    InThread.GetStats(AfterFlush);
    Expect(AfterFlush.FramesRendered == BeforeFlushFrames, "Flush() does not present and does not advance FramesRendered");
}

/*
 * The capture rule (RenderCommand.h): a lambda capturing a non-trivial type
 * such as std::string fails RenderCommandEnqueuer::operator()'s
 * static_assert. Verified by hand once, as GameCoreSmokeTest does for
 * GAMECORE_COMPONENT_BODY() -- uncomment to see it fail to compile:
 *
 *     std::string Owned = "not trivially copyable";
 *     ENQUEUE_RENDER_COMMAND(BadCapture)([Owned](RenderContext&) {});
 */

/*
 * A hand-built scene driven entirely by explicit commands -- no World, no
 * WorldBridge (step 4). Exercises RenderProxy's lifecycle, RenderScene's
 * add/remove/cull/render, and RenderContext::DrawQuad end to end.
 */
static void TestHandBuiltScene(RenderThread& InThread)
{
    std::printf("[scene]\n");

    const bool bRegistered = RegisterProxyType(RenderComponent::StaticType().ID, GetRenderComponentFactory());
    Expect(bRegistered, "RegisterProxyType succeeds for a fresh component type");

    RenderScene* Scene = nullptr;
    ENQUEUE_RENDER_COMMAND(CreateScene)([&Scene](RenderContext&)
    {
        Scene = new RenderScene();
    });
    FlushRenderCommands();
    Expect(Scene != nullptr, "the scene was constructed on the render thread");

    /* Built on the game thread -- CreateProxyFn's contract -- then handed to
     * AddProxy through a command, exactly WorldBridge's own lifecycle
     * (a later step) will drive it. */
    RenderComponent Init;

    const GameCore::EntityID Owner(1, 0);
    CreateProxyFn Factory = FindProxyFactory(RenderComponent::StaticType().ID);
    Expect(Factory != nullptr, "FindProxyFactory returns the factory RegisterProxyType installed");

    RenderProxy* Proxy = Factory(Owner, &Init);
    Expect(Proxy != nullptr, "the factory constructs a proxy");
    Expect(Proxy->GetOwner() == Owner, "the proxy remembers the entity it mirrors");

    ENQUEUE_RENDER_COMMAND(AddProxy)([Scene, Proxy](RenderContext& Ctx)
    {
        Scene->AddProxy(Proxy, Ctx);
    });
    FlushRenderCommands();
    Expect(Scene->GetProxyCount() == 1, "AddProxy grows the scene's proxy count");

    /* A transform at the origin, an orthographic view over the whole
     * viewport -- the proxy's world bounds must fall inside it. */
    RenderTransform Transform;
    Transform.Position = EngineCore::Vector3(100.0, 50.0, 0.0);

    ENQUEUE_RENDER_COMMAND(SetTransform)([Proxy, Transform](RenderContext&)
    {
        Proxy->SetTransform(Transform);
    });

    const RenderStats StatsBefore = [&InThread]() { RenderStats S; InThread.GetStats(S); return S; }();

    ENQUEUE_RENDER_COMMAND(RenderVisible)([Scene](RenderContext& Ctx)
    {
        Scene->SetView(RenderView::MakeOrthographic(EngineCore::Size(1280, 720), EngineCore::Vector3(0, 0, 0), -1.0f, 1000.0f));
        Scene->Render(Ctx);
    });
    InThread.EndFrame(StatsBefore.FramesRendered + 1);
    InThread.WaitForFrame();

    RenderStats StatsAfterVisible;
    InThread.GetStats(StatsAfterVisible);
    Expect(StatsAfterVisible.DrawCalls == StatsBefore.DrawCalls + 1, "a proxy inside the view's bounds is drawn exactly once");
    Expect(StatsAfterVisible.ProxiesLive == 1, "RenderStats reports one live proxy");
    Expect(StatsAfterVisible.ProxiesCulledLastFrame == 0, "a proxy inside the view is not counted as culled");

    /* Move it far outside the view and render again: it must be culled, not drawn. */
    RenderTransform FarTransform;
    FarTransform.Position = EngineCore::Vector3(100000.0, 100000.0, 0.0);

    ENQUEUE_RENDER_COMMAND(SetFarTransform)([Proxy, FarTransform](RenderContext&)
    {
        Proxy->SetTransform(FarTransform);
    });
    ENQUEUE_RENDER_COMMAND(RenderCulled)([Scene](RenderContext& Ctx)
    {
        Scene->Render(Ctx);
    });
    InThread.EndFrame(StatsAfterVisible.FramesRendered + 1);
    InThread.WaitForFrame();

    RenderStats StatsAfterCulled;
    InThread.GetStats(StatsAfterCulled);
    Expect(StatsAfterCulled.DrawCalls == StatsAfterVisible.DrawCalls, "a proxy outside the view's bounds is not drawn");
    Expect(StatsAfterCulled.ProxiesCulledLastFrame == 1, "the out-of-view proxy is counted as culled");

    /* Component update through ApplyComponent, not just SetTransform. */
    RenderComponent Updated = Init;
    Updated.bVisible = 0;
    ENQUEUE_RENDER_COMMAND(ApplyUpdate)([Proxy, Updated](RenderContext&)
    {
        Proxy->ApplyComponent(&Updated);
    });
    ENQUEUE_RENDER_COMMAND(RenderInvisible)([Scene, FarTransform, Proxy](RenderContext& Ctx)
    {
        /* Back inside the view, but bVisible=0 from ApplyComponent above. */
        Proxy->SetTransform(RenderTransform());
        Scene->Render(Ctx);
    });
    InThread.EndFrame(StatsAfterCulled.FramesRendered + 1);
    InThread.WaitForFrame();

    RenderStats StatsAfterInvisible;
    InThread.GetStats(StatsAfterInvisible);
    Expect(StatsAfterInvisible.DrawCalls == StatsAfterCulled.DrawCalls,
           "ApplyComponent(bVisible=0) hides a proxy even when it is back inside the view");

    ENQUEUE_RENDER_COMMAND(RemoveProxy)([Scene, Proxy](RenderContext& Ctx)
    {
        Scene->RemoveProxy(Proxy, Ctx);
    });
    FlushRenderCommands();
    Expect(Scene->GetProxyCount() == 0, "RemoveProxy drops the scene's proxy count back to zero");

    ENQUEUE_RENDER_COMMAND(DestroyScene)([&Scene](RenderContext&)
    {
        delete Scene;
        Scene = nullptr;
    });
    FlushRenderCommands();

    UnregisterProxyType(RenderComponent::StaticType().ID);
    Expect(FindProxyFactory(RenderComponent::StaticType().ID) == nullptr,
           "UnregisterProxyType clears the factory table slot");
}

// ============================================================================
// A test-defined proxy type, registered from THIS executable -- a different
// image from RenderCore.dll -- proving RegisterProxyType/CreateProxyFn work
// across the image boundary the way GameCore's own component registry does.
// ============================================================================

namespace
{
    struct TestProxyStats
    {
        int InitCalls    = 0;
        int ApplyCalls   = 0;
        int DrawCalls    = 0;
        int ReleaseCalls = 0;
    };
    TestProxyStats GTestProxyStats;

    struct TestComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()
        int Value = 0;
    };

    class TestProxy final : public RenderProxy
    {
    public:
        TestProxy(const GameCore::EntityID InOwner, const TestComponent& InInit) : RenderProxy(InOwner)
        {
            ApplyComponent(&InInit);
        }

        void InitRenderResources(RenderContext&) override { ++GTestProxyStats.InitCalls; }
        void ReleaseRenderResources(RenderContext&) override { ++GTestProxyStats.ReleaseCalls; }

        void ApplyComponent(const void* InComponentBytes) override
        {
            Value = static_cast<const TestComponent*>(InComponentBytes)->Value;
            ++GTestProxyStats.ApplyCalls;
        }

        void Draw(RenderContext&) override { ++GTestProxyStats.DrawCalls; }

    private:
        int Value = 0;
    };
}
GAMECORE_DEFINE_COMPONENT(TestComponent, "RenderCoreSmokeTest.Test")

namespace
{
    RenderProxy* TestComponent_CreateProxy(const GameCore::EntityID InEntity, const void* InBytes)
    {
        return EngineCore::Memory::Alloc<TestProxy>(InEntity, *static_cast<const TestComponent*>(InBytes));
    }
}

// ============================================================================
// The World-driven suite: a real GameCore::World bound to a Renderer.
// ============================================================================

/*
 * "stat unit": Renderer times the frame and the game thread, the render
 * thread its own busy time; averaged over half a second into RenderStats and
 * the overlay's snapshot, shown when Stats.Unit is set. Null backend.
 */
static void TestStatUnit()
{
    std::printf("[stat unit]\n");
    StatUnitSnapshot Sample;
    Sample.FrameMs        = 16.666f;
    Sample.GameThreadMs   = 2.5f;
    Sample.RenderThreadMs = 4.25f;
    Sample.WaitMs         = 1.0f;
    Sample.PresentMs      = 12.5f;
    Sample.IdleMs         = 0.67f;
    const std::string Text = FormatStatUnit(Sample);
    Expect(Text == "Frame:     16.67 ms\nGame:       2.50 ms\nRender:     4.25 ms\nWait:       1.00 ms\nPresent:   12.50 ms\n"
                   "Idle:       0.67 ms\nFPS:        60.0", "the overlay's text, a line each");

    ConfigVar* Unit = ConfigFindVarByPath("Stats.Unit");
    Expect(Unit != nullptr && !Unit->GetBool(), "Stats.Unit exists, off by default");

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null) for stat unit");
    if(bInit && Unit)
    {
        Unit->SetBool(true);
        const EngineCore::Size Viewport(320, 200);
        const auto Start = std::chrono::steady_clock::now();
        uint64_t Frame = 1;
        while(std::chrono::steady_clock::now() - Start < std::chrono::milliseconds(700))
        {
            Rend.BeginGameFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(4));   // the game's tick
            Rend.WaitForPreviousFrame();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
        }
        Rend.WaitForPreviousFrame();
        RenderStats Stats;
        Rend.GetStats(Stats);
        std::printf("    frame %.2f ms, game %.2f ms, render %.3f ms\n", double(Stats.FrameMs), double(Stats.GameThreadMs),
                    double(Stats.RenderThreadMs));
        Expect(Stats.FrameMs >= 4.0f && Stats.GameThreadMs >= 3.5f && Stats.GameThreadMs <= Stats.FrameMs,
               "after half a second, frame and game thread times are averaged in");
        Expect(Stats.RenderThreadMs >= 0.0f && Stats.RenderThreadMs < Stats.FrameMs, "and the render thread's, less than the frame");
        Expect(GetStatUnit().bShow, "Stats.Unit reaches the overlay");
        Unit->SetBool(false);
        Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
        Rend.WaitForPreviousFrame();
        Expect(!GetStatUnit().bShow, "and turning it off hides it");

        /* "stat frame" on Null: nothing to time, so nothing is reported. */
        ConfigVar* FrameVar = ConfigFindVarByPath("Stats.Frame");
        Expect(FrameVar != nullptr && !FrameVar->GetBool(), "Stats.Frame exists, off by default");
        if(FrameVar)
        {
            FrameVar->SetBool(true);
            for(int Each = 0; Each < 3; ++Each)
            {
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
                Rend.WaitForPreviousFrame();
            }
            RenderStats Null;
            Rend.GetStats(Null);
            Expect(Null.GpuMs[RGT_Total] < 0.0f && Null.GpuMs[RGT_BasePass] < 0.0f, "the Null backend times no GPU work");
            Expect(Null.QueueCpuMs >= 0.0f && Null.StageDraws[RGT_Total] >= 0,
                   "but the render thread's queueing and the stages' draws are measured on every backend");

            /* Parallel draw recording's gate (ParallelRecording.h): Null runs
             * the chunks serially, so it is available without workers. */
            Expect(Null.bParallelRecordingAvailable, "the gate finds parallel recording available on Null (serial chunks)");
            const ParallelRecordingState Auto = EvaluateParallelRecording();
            Expect(Auto.bBackend && Auto.bSerialOnly && Auto.bEnabled && Auto.Mode == EParallelRecordingMode::Auto,
                   "Auto by default, the backend able, serially");
            Expect(!ShouldRecordInParallel(Auto, Auto.MinDraws - 1) && ShouldRecordInParallel(Auto, Auto.MinDraws),
                   "Auto splits a stage from ParallelMinDraws draws, not below");
            if(ConfigVar* ModeVar = ConfigFindVarByPath("Rendering.ParallelRecording"))
            {
                ModeVar->SetString("off");
                const ParallelRecordingState Off = EvaluateParallelRecording();
                Expect(!Off.bEnabled && !ShouldRecordInParallel(Off, 100000), "ParallelRecording=Off (any case) turns it off");
                ModeVar->SetString("On");
                const ParallelRecordingState On = EvaluateParallelRecording();
                Expect(On.bEnabled && ShouldRecordInParallel(On, 1) && !ShouldRecordInParallel(On, 0),
                       "On splits any stage with draws");
                ModeVar->SetString("Auto");
            }
            else
            {
                Expect(false, "[Rendering] ParallelRecording exists");
            }
            FrameVar->SetBool(false);
        }
        Rend.Shutdown();
    }

    const float   Times[RGT_Count] = { 0.01f, 0.5f, -1.0f, 1.25f, 0.2f, 0.05f, 0.75f, 2.8f, 0.6f, 0.3f };
    const float   Cpu[RGT_Count]   = { 0.12f, 3.5f, 0.02f, 4.25f, 0.1f, 0.4f, 0.6f, 9.1f, 1.5f, 0.05f };
    const int32_t Draws[RGT_Count] = { 80, 1200, -1, 1800, 1, 24, 6, 3111, -1, -1 };
    const std::string FrameText = FormatStatFrame(Times, Cpu, Draws, 2.25f, true);
    Expect(FrameText ==
                        "Stage       GPU ms CPU ms  Draws\n"
                        "PrePass       0.01   0.12     80\n"
                        "BasePass      0.50   3.50   1200\n"
                        "AO               -   0.02      -\n"
                        "Shadows       1.25   4.25   1800\n"
                        "Lighting      0.20   0.10      1\n"
                        "Forward       0.05   0.40     24\n"
                        "PostProcess   0.75   0.60      6\n"
                        "Total         2.80   9.10   3111\n"
                        "Captures      0.60   1.50      -\n"
                        "Particles     0.30   0.05      -\n"
                        "Queue            -   2.25      -",
           "stat frame's text: a line per stage with GPU ms, render-thread ms and draws, captures and particles after "
           "the total, then the queueing");
    Expect(FormatStatFrame(Times, Cpu, Draws, 2.25f, false) ==
                        "Stage       GPU ms CPU ms  Draws\n"
                        "PrePass          -   0.12     80\n"
                        "BasePass         -   3.50   1200\n"
                        "AO               -   0.02      -\n"
                        "Shadows          -   4.25   1800\n"
                        "Lighting         -   0.10      1\n"
                        "Forward          -   0.40     24\n"
                        "PostProcess      -   0.60      6\n"
                        "Total            -   9.10   3111\n"
                        "Captures         -   1.50      -\n"
                        "Particles        -   0.05      -\n"
                        "Queue            -   2.25      -\n"
                        "GPU: not timed on this backend",
           "and where the GPU is not timed, its column empty and a line saying so");
}

/*
 * Static draw lists: a static entity's proxy is extracted only when it
 * changes, and drawn from a list captured once (RENDERPIPELINE.md). Null.
 */
static void TestStaticDrawLists()
{
    std::printf("[static draw lists]\n");
    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null) for static draw lists");
    if(!bInit)
    {
        return;
    }
    RegisterProxyType(TestComponent::StaticType().ID, &TestComponent_CreateProxy);

    GameCore::World World;
    const GameCore::Entity Wall = World.CreateEntity("Wall");
    Wall.AddComponent<GameCore::TransformComponent>();
    TestComponent Init;
    Init.Value = 7;
    Wall.AddComponent(Init);
    Wall.AddComponent<RenderComponent>();
    Rend.BindWorld(World);

    uint64_t Frame = 1;
    const auto Draw = [&]()
    {
        Rend.EndFrame(FrameInfo{Frame++, 0.016, EngineCore::Size(640, 360)});
        Rend.WaitForPreviousFrame();
    };
    Draw();
    Draw();

    /* Dynamic: drawn and extracted every frame. */
    GTestProxyStats = TestProxyStats();
    Draw();
    Draw();
    Expect(GTestProxyStats.DrawCalls == 2 && GTestProxyStats.ApplyCalls == 2, "a dynamic proxy is extracted and drawn every frame");

    /* Static: one record for the change, then nothing; one capture, replayed. */
    Wall.SetMobility(GameCore::EMobility::Static);
    Draw();
    RenderStats After;
    Rend.GetStats(After);
    Expect(After.StaticProxiesDrawn == 1 && After.ProxiesCulledLastFrame == 1,
           "the visible proxy is drawn from its static list (the empty quad is culled, static or not)");
    GTestProxyStats = TestProxyStats();
    const uint64_t Builds = After.StaticListBuilds;
    RenderStats Before;
    Rend.GetStats(Before);
    for(int Each = 0; Each < 5; ++Each) { Draw(); }
    RenderStats Steady;
    Rend.GetStats(Steady);
    Expect(GTestProxyStats.ApplyCalls == 0, "an unchanged static proxy is not extracted again");
    Expect(GTestProxyStats.DrawCalls == 0, "nor asked to draw: its list is replayed");
    Expect(Steady.StaticListBuilds == Builds, "and no list is rebuilt");
    Expect(Steady.CommandsExecuted - Before.CommandsExecuted < 5 * 4, "frames with nothing static changed enqueue no extraction");

    /* Moving it: one record, one rebuild. */
    Wall.GetComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(10.0, 0.0, 0.0);
    Draw();
    Draw();
    RenderStats Moved;
    Rend.GetStats(Moved);
    Expect(GTestProxyStats.ApplyCalls == 1 && GTestProxyStats.DrawCalls == 1, "a moved static proxy is extracted and captured once");
    Expect(Moved.StaticListBuilds == Builds + 1, "its list rebuilt once");

    /* An edited component, the same. */
    Wall.GetComponent<TestComponent>()->Value = 8;
    Draw();
    Expect(GTestProxyStats.ApplyCalls == 2, "an edited static component is extracted again");

    /* Dynamic again: every frame again. */
    Wall.SetMobility(GameCore::EMobility::Dynamic);
    GTestProxyStats = TestProxyStats();
    Draw();
    Draw();
    RenderStats Back;
    Rend.GetStats(Back);
    Expect(GTestProxyStats.DrawCalls == 2 && Back.StaticProxiesDrawn == 0, "made dynamic, it is drawn every frame again");

    Rend.UnbindWorld();
    Rend.Shutdown();
}

static void TestRendererWithWorld()
{
    std::printf("[renderer + world]\n");

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds");
    if(!bInit)
    {
        return;
    }

    const bool bTestTypeRegistered = RegisterProxyType(TestComponent::StaticType().ID, &TestComponent_CreateProxy);
    Expect(bTestTypeRegistered, "a test-defined proxy type registers from a different image than RenderCore.dll");

    /* One World for the whole suite. A component added BEFORE BindWorld
     * exercises the existing-components sweep, not the OnComponentAdded
     * delegate path -- created here, first, so BindWorld genuinely finds it
     * already present. */
    GameCore::World World;

    const GameCore::Entity PreExisting = World.CreateEntity();
    PreExisting.AddComponent<GameCore::TransformComponent>();
    RenderComponent PrimInit;
    PreExisting.AddComponent(PrimInit);

    Rend.BindWorld(World);

    RenderStats StatsAfterSweep;
    Rend.WaitForPreviousFrame();
    Rend.EndFrame(FrameInfo{100, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();
    Rend.GetStats(StatsAfterSweep);
    Expect(StatsAfterSweep.ProxiesLive == 1, "BindWorld's existing-components sweep created a proxy for the pre-existing RenderComponent");

    World.DestroyEntity(PreExisting);
    Rend.EndFrame(FrameInfo{101, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();

    const GameCore::Entity E1 = World.CreateEntity();
    E1.AddComponent<GameCore::TransformComponent>();
    E1.AddComponent<RenderComponent>();

    const GameCore::Entity TestEntity = World.CreateEntity();
    TestEntity.AddComponent<GameCore::TransformComponent>();
    TestComponent TestInit;
    TestInit.Value = 42;
    TestEntity.AddComponent(TestInit);

    Rend.WaitForPreviousFrame();
    Rend.EndFrame(FrameInfo{0, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();

    RenderStats Stats;
    Rend.GetStats(Stats);
    Expect(Stats.ProxiesLive == 2, "OnComponentAdded created a proxy for each of the two render components");
    Expect(GTestProxyStats.InitCalls == 1, "the test proxy's InitRenderResources ran once, on the render thread, after AddProxy");
    Expect(GTestProxyStats.ApplyCalls >= 2, "ApplyComponent ran at least once from the constructor plus once from extraction");
    Expect(GTestProxyStats.DrawCalls >= 1, "the test proxy's Draw ran during Render()");

    /* Move the entity, tick another frame, confirm extraction actually
     * carries the CURRENT transform, not merely the one from creation. */
    GameCore::TransformComponent* Transform = E1.GetComponent<GameCore::TransformComponent>();
    Transform->Position = EngineCore::Vector3(500.0, 500.0, 0.0);

    Rend.EndFrame(FrameInfo{1, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();

    /* RemoveComponent -- proves OnComponentRemoved's teardown path. */
    E1.RemoveComponent<RenderComponent>();
    Rend.EndFrame(FrameInfo{2, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();

    RenderStats StatsAfterRemove;
    Rend.GetStats(StatsAfterRemove);
    Expect(StatsAfterRemove.ProxiesLive == 1, "RemoveComponent<RenderComponent> removed exactly its own proxy");

    /* DestroyEntity -- proves the OTHER teardown path (World.cpp broadcasts
     * OnComponentRemoved for every component before the entity's slot is
     * recycled). */
    World.DestroyEntity(TestEntity);
    Rend.EndFrame(FrameInfo{3, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();

    RenderStats StatsAfterDestroy;
    Rend.GetStats(StatsAfterDestroy);
    Expect(StatsAfterDestroy.ProxiesLive == 0, "DestroyEntity removed the last remaining proxy");
    Expect(GTestProxyStats.ReleaseCalls == 1, "ReleaseRenderResources ran once for the destroyed test proxy");

    /* Camera: an active CameraComponent at a known position moves the view's
     * origin -- read back through a render command against the scene's
     * current view, the same technique a real caller would use to debug one. */
    const GameCore::Entity CameraEntity = World.CreateEntity();
    GameCore::TransformComponent* CamTransform = CameraEntity.AddComponent<GameCore::TransformComponent>();
    CamTransform->Position = EngineCore::Vector3(100.0, 50.0, 0.0);
    CameraComponent Cam;
    CameraEntity.AddComponent(Cam);

    Rend.EndFrame(FrameInfo{4, 0.016, EngineCore::Size(1280, 720)});
    Rend.WaitForPreviousFrame();

    std::atomic<double> SeenOriginX{-1.0};
    ENQUEUE_RENDER_COMMAND(ReadViewOrigin)([&SeenOriginX](RenderContext& Ctx)
    {
        SeenOriginX.store(Ctx.GetView().Origin.GetX(), std::memory_order_release);
    });
    FlushRenderCommands();
    Expect(SeenOriginX.load(std::memory_order_acquire) == 100.0, "an active CameraComponent's sibling Transform sets the view's origin");

    UnregisterProxyType(TestComponent::StaticType().ID);
    Expect(FindProxyFactory(TestComponent::StaticType().ID) == nullptr, "UnregisterProxyType clears the test type's table slot");

    Rend.UnbindWorld();
    Rend.Shutdown();
}

// ============================================================================
// Custom render passes (CustomRenderPass.h): each hook once a frame, in order,
// for its own World only, and none once unregistered
// ============================================================================

namespace
{
    struct CustomPassLog
    {
        std::mutex               Lock;
        std::vector<std::string> Runs;      // "<pass>@<hook>", render thread
        std::atomic<int>         Extracts{0};
        std::atomic<int>         Targets{0};

        std::vector<std::string> Take()
        {
            std::lock_guard<std::mutex> Guard(Lock);
            std::vector<std::string> Out;
            Out.swap(Runs);
            return Out;
        }
    };

    class LoggingPass final : public CustomRenderPass
    {
    public:
        LoggingPass(CustomPassLog& InLog, std::string InName) : Log(InLog), Name(std::move(InName)) {}

        const char* GetName() const override { return Name.c_str(); }
        void Extract() override { Log.Extracts.fetch_add(1); }
        void Execute(RenderPassContext& InContext) override
        {
            {
                std::lock_guard<std::mutex> Guard(Log.Lock);
                Log.Runs.push_back(Name + "@" + GetRenderHookName(InContext.GetHook()));
            }
            /* The API with no GPU: a target is made and kept; a draw whose shader is not cooked fails, quietly after once. */
            RenderPassTargetDesc Desc;
            Desc.Width  = 16;
            Desc.Height = 16;
            const RenderPassTarget First  = InContext.AcquireTarget("Scratch", Desc);
            const RenderPassTarget Second = InContext.AcquireTarget("Scratch", Desc);
            if(First.IsValid() && First.Handle == Second.Handle) { Log.Targets.fetch_add(1); }
            RenderPassParams Params;
            InContext.DrawFullscreen("NoSuchPass.fsh", First, {}, Params);
            Expect(InContext.GetMeshGroupDrawCount(RENDER_PASS_MESH_GROUPS) == 0, "a mesh group past the last has no draws");
        }

    private:
        CustomPassLog& Log;
        std::string    Name;
    };

    /* SPLITSCREEN: what each run saw -- its view, whether it is a main view,
     * its targets' size, and the target its pool handed it. */
    struct ViewPassRun { uint32_t View; bool bMain; uint32_t Width, Height; const void* Target; };

    class ViewPass final : public CustomRenderPass
    {
    public:
        explicit ViewPass(std::vector<ViewPassRun>& InRuns) : Runs(InRuns) {}

        const char* GetName() const override { return "Views"; }
        void Execute(RenderPassContext& InContext) override
        {
            RenderPassTargetDesc Desc;
            Desc.Width  = InContext.GetWidth();
            Desc.Height = InContext.GetHeight();
            const RenderPassTarget Target = InContext.AcquireTarget("PerView", Desc);
            Runs.push_back({ InContext.GetViewIndex(), InContext.IsMainView(), InContext.GetWidth(), InContext.GetHeight(),
                             Target.Handle });
        }

    private:
        std::vector<ViewPassRun>& Runs;   // render thread; read after a flush
    };
}

static void TestCustomRenderPasses()
{
    std::printf("[custom render passes]\n");

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds");
    if(!bInit)
    {
        return;
    }
    GameCore::World World;
    GameCore::World Other;
    Rend.BindWorld(World);

    CustomPassLog Log;
    const auto Make = [&Log](const char* InName) { return std::make_shared<LoggingPass>(Log, InName); };

    /* One a hook, registered back to front, and two more at AfterForward around the one there. */
    std::vector<RenderPassHandle> Handles;
    for(int Hook = int(ERenderHook::Count) - 1; Hook >= 0; --Hook)
    {
        Handles.push_back(RegisterCustomRenderPass(&World, Make(GetRenderHookName(ERenderHook(Hook))), ERenderHook(Hook)));
    }
    const RenderPassHandle Late  = RegisterCustomRenderPass(&World, Make("Late"), ERenderHook::AfterForward, 5);
    const RenderPassHandle Early = RegisterCustomRenderPass(&World, Make("Early"), ERenderHook::AfterForward, -5);
    RegisterCustomRenderPass(&Other, Make("OtherWorld"), ERenderHook::BeforeScene);
    Expect(RegisterCustomRenderPass(&World, nullptr, ERenderHook::BeforeScene) == 0, "a null pass is not registered");
    Expect(GetCustomRenderPassCount(&World) == size_t(ERenderHook::Count) + 2, "the World has its passes, not the other World's");

    Rend.EndFrame(FrameInfo{1, 0.016, EngineCore::Size(320, 200)});
    Rend.WaitForPreviousFrame();
    FlushRenderCommands();

    /* Every hook but Sky, which runs only when image-based lighting captures the sky (not on Null). */
    Expect(std::string(GetRenderHookName(ERenderHook::Sky)) == "Sky", "the Sky hook has its name");
    std::vector<std::string> Expected;
    for(int Hook = 0; Hook < int(ERenderHook::Sky); ++Hook)
    {
        const std::string Name = GetRenderHookName(ERenderHook(Hook));
        if(ERenderHook(Hook) == ERenderHook::AfterForward) { Expected.push_back("Early@" + Name); }
        Expected.push_back(Name + "@" + Name);
        if(ERenderHook(Hook) == ERenderHook::AfterForward) { Expected.push_back("Late@" + Name); }
    }
    const std::vector<std::string> Ran = Log.Take();
    Expect(Ran == Expected, "every hook's passes ran once, in frame order, by Order then registration");
    if(Ran != Expected)
    {
        for(const std::string& Each : Ran) { std::printf("    ran: %s\n", Each.c_str()); }
    }
    Expect(Log.Extracts.load() == int(ERenderHook::Count) + 2, "Extract ran once a frame for each of the World's passes");
    Expect(Log.Targets.load() == int(ERenderHook::Count) + 1, "AcquireTarget made each pass's target once and handed it back");

    /* Unregistered: gone from the next frame on. */
    Expect(UnregisterCustomRenderPass(Late), "UnregisterCustomRenderPass finds the pass");
    Expect(!UnregisterCustomRenderPass(Late), "and not a second time");
    Rend.EndFrame(FrameInfo{2, 0.016, EngineCore::Size(320, 200)});
    Rend.WaitForPreviousFrame();
    FlushRenderCommands();
    const std::vector<std::string> After = Log.Take();
    Expect(After.size() == Expected.size() - 1 && std::find(After.begin(), After.end(), "Late@AfterForward") == After.end(),
           "an unregistered pass no longer runs; the rest still do");
    (void)Early;

    UnregisterCustomRenderPasses(&World);
    UnregisterCustomRenderPasses(&Other);
    Expect(GetCustomRenderPassCount(&World) == 0, "UnregisterCustomRenderPasses removes all of a World's");
    Rend.EndFrame(FrameInfo{3, 0.016, EngineCore::Size(320, 200)});
    Rend.WaitForPreviousFrame();
    FlushRenderCommands();
    Expect(Log.Take().empty(), "with none registered, nothing runs");

    /* SPLITSCREEN: a pass runs once per view, each a main view, each with its own pool. */
    {
        std::vector<ViewPassRun> Runs;
        const RenderPassHandle PerView = RegisterCustomRenderPass(&World, std::make_shared<ViewPass>(Runs), ERenderHook::AfterForward);
        Rend.SetViewCount(RENDER_DEFAULT_SCENE, 2);
        for(uint64_t Frame = 4; Frame <= 5; ++Frame)
        {
            Rend.EndFrame(FrameInfo{Frame, 0.016, EngineCore::Size(320, 200)});
            Rend.WaitForPreviousFrame();
        }
        FlushRenderCommands();
        const bool bTwoAFrame = Runs.size() == 4 && Runs[0].View == 0 && Runs[1].View == 1 && Runs[2].View == 0 && Runs[3].View == 1;
        Expect(bTwoAFrame, "a custom pass runs for each view, in order");
        if(bTwoAFrame)
        {
            Expect(Runs[0].bMain && Runs[1].bMain, "every split view is a main view (IsMainView)");
            Expect(Runs[0].Width == 320 && Runs[0].Height == 100 && Runs[1].Height == 100, "each view's targets are its own rectangle's size");
            Expect(Runs[0].Target && Runs[1].Target && Runs[0].Target != Runs[1].Target
                       && Runs[2].Target == Runs[0].Target && Runs[3].Target == Runs[1].Target,
                   "each view has its own pass targets, kept from frame to frame");
        }
        Rend.SetViewCount(RENDER_DEFAULT_SCENE, 1);
        UnregisterCustomRenderPass(PerView);
    }

    Rend.UnbindWorld();
    Rend.Shutdown();
}

#if WITH_EDITOR
// ============================================================================
// Several scenes: one World each, rendered every frame, stats from the default
// ============================================================================

/* The view the last-rendered scene drew with: scenes render in creation order,
 * the default first, so this is the last one that rendered this frame. */
static double LastRenderedViewOriginX()
{
    std::atomic<double> Seen{-1.0};
    ENQUEUE_RENDER_COMMAND(ReadLastViewOrigin)([&Seen](RenderContext& Ctx)
    {
        Seen.store(Ctx.GetView().Origin.GetX(), std::memory_order_release);
    });
    FlushRenderCommands();
    return Seen.load(std::memory_order_acquire);
}

static void AddCameraAt(GameCore::World& InWorld, const double InX)
{
    const GameCore::Entity Camera = InWorld.CreateEntity();
    Camera.AddComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(InX, 0.0, 0.0);
    Camera.AddComponent<CameraComponent>();
}

static void TestMultipleScenes()
{
    std::printf("[multiple scenes]\n");

    Renderer Rend;
    Expect(Rend.CreateScene() == RENDER_INVALID_SCENE, "CreateScene before Initialize() is refused");

    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds for the scene test");
    if(!bInit)
    {
        return;
    }

    GameCore::World Level;
    AddCameraAt(Level, 100.0);
    for(int Index = 0; Index < 2; ++Index)
    {
        const GameCore::Entity E = Level.CreateEntity();
        E.AddComponent<GameCore::TransformComponent>();
        E.AddComponent<RenderComponent>();
    }
    Rend.BindWorld(Level);

    GameCore::World Prefab;
    AddCameraAt(Prefab, 7.0);
    const GameCore::Entity Lone = Prefab.CreateEntity();
    Lone.AddComponent<GameCore::TransformComponent>();
    Lone.AddComponent<RenderComponent>();

    const RenderSceneID Second = Rend.CreateScene();
    Expect(Second != RENDER_INVALID_SCENE && Second != RENDER_DEFAULT_SCENE, "CreateScene makes a new scene");
    Rend.BindWorld(Second, Prefab);

    /* No viewport yet: the second scene is skipped, so the default rendered last. */
    Rend.EndFrame(FrameInfo{0, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    Expect(LastRenderedViewOriginX() == 100.0, "a scene with no viewport is not rendered");

    Rend.SetViewportPixels(Second, EngineCore::Size(320, 240));
    Rend.EndFrame(FrameInfo{1, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    Expect(LastRenderedViewOriginX() == 7.0, "with one, it renders after the default, from its own World's camera");

    RenderStats Stats;
    Rend.GetStats(Stats);
    Expect(Stats.ProxiesLive == 2, "RenderStats count the default scene's proxies only");

    /* Presenting with no window is a no-op on every path. */
    Rend.PresentToWindow(Second, WindowHandle());
    Rend.PresentToWindow(WindowHandle());
    Rend.ReleaseWindow(WindowHandle());

    /* A view override and overlay lines go to their own scene. */
    RenderView Override = RenderView::MakeOrthographic(EngineCore::Size(320, 240), EngineCore::Vector3(55.0, 0.0, -1.0), -1.0f, 1000.0f);
    Rend.SetViewOverride(Second, &Override);
    DebugLine Line;
    Line.End = EngineCore::Vector3(1.0, 1.0, 0.0);
    Rend.DrawDebugLine(Second, Line);
    Rend.EndFrame(FrameInfo{2, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    Expect(LastRenderedViewOriginX() == 55.0, "SetViewOverride(scene) replaces that scene's camera only");

    Rend.DestroyScene(Second);
    Rend.EndFrame(FrameInfo{3, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    Expect(LastRenderedViewOriginX() == 100.0, "DestroyScene stops it rendering");

    /* Its World was unbound: new render components there reach no scene. */
    const GameCore::Entity After = Prefab.CreateEntity();
    After.AddComponent<GameCore::TransformComponent>();
    After.AddComponent<RenderComponent>();
    Rend.EndFrame(FrameInfo{4, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    Rend.GetStats(Stats);
    Expect(Stats.ProxiesLive == 2, "a destroyed scene's World is unbound");

    Rend.DestroyScene(RENDER_DEFAULT_SCENE);   // ignored
    Rend.DestroyScene(Second);                 // already gone: ignored
    Rend.EndFrame(FrameInfo{5, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    Expect(LastRenderedViewOriginX() == 100.0, "the default scene cannot be destroyed");

    /* A scene left alive is torn down by Shutdown, with its World still bound. */
    const RenderSceneID Third = Rend.CreateScene();
    Rend.BindWorld(Third, Prefab);
    Rend.SetViewportPixels(Third, EngineCore::Size(64, 64));
    Rend.EndFrame(FrameInfo{6, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();

    Rend.Shutdown();
    Expect(Rend.CreateScene() == RENDER_INVALID_SCENE, "CreateScene after Shutdown() is refused");
}

/* What the last-rendered scene's main view was, as the context last had it. */
static RenderView LastRenderedView()
{
    RenderView Seen;
    ENQUEUE_RENDER_COMMAND(ReadLastView)([&Seen](RenderContext& Ctx) { Seen = Ctx.GetView(); });
    FlushRenderCommands();
    return Seen;
}

/* SPLITSCREEN: one scene, several views (RenderTypes.h's RENDER_VIEWS_MAX). */
static void TestSplitscreenViews()
{
    std::printf("[splitscreen views]\n");

    RenderViewRect Rects[RENDER_VIEWS_MAX];
    Expect(MakeSplitscreenLayout(EngineCore::Size(641, 361), 2, false, Rects) == 2
               && Rects[0].Height == 180 && Rects[1].Y == 180 && Rects[1].Height == 181 && Rects[1].Width == 641,
           "two views: top and bottom halves, tiling an odd height");
    Expect(MakeSplitscreenLayout(EngineCore::Size(640, 360), 2, true, Rects) == 2 && Rects[1].X == 320 && Rects[1].Height == 360,
           "two views side by side: left and right halves");
    Expect(MakeSplitscreenLayout(EngineCore::Size(640, 360), 3, false, Rects) == 3
               && Rects[0].Width == 640 && Rects[2].X == 320 && Rects[2].Y == 180,
           "three views: one across the top, two below");
    Expect(MakeSplitscreenLayout(EngineCore::Size(640, 360), 9, false, Rects) == RENDER_VIEWS_MAX
               && Rects[3].X == 320 && Rects[3].Y == 180 && Rects[3].Width == 320,
           "more than RENDER_VIEWS_MAX: quadrants");

    Renderer Rend;
    if(!Rend.Initialize(ERenderBackend::Null, WindowHandle()))
    {
        Expect(false, "Renderer::Initialize(Null, {}) succeeds for the splitscreen test");
        return;
    }
    GameCore::World Level;
    AddCameraAt(Level, 100.0);
    const GameCore::Entity Second = Level.CreateEntity();
    Second.AddComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(300.0, 0.0, 0.0);
    Second.AddComponent<CameraComponent>()->bActive = false;
    const GameCore::Entity Mesh = Level.CreateEntity();
    Mesh.AddComponent<GameCore::TransformComponent>();
    Mesh.AddComponent<RenderComponent>();
    Rend.BindWorld(Level);

    Rend.EndFrame(FrameInfo{0, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    RenderView View = LastRenderedView();
    Expect(Rend.GetViewCount() == 1 && View.ViewportPixels.Width == 640 && View.ViewportPixels.Height == 360,
           "one view by default, the whole viewport");

    Rend.SetViewCamera(RENDER_DEFAULT_SCENE, 1, Second.GetID());
    Expect(Rend.GetViewCamera(RENDER_DEFAULT_SCENE, 1) == Second.GetID() && !Rend.GetViewCamera(RENDER_DEFAULT_SCENE, 2).IsSet(),
           "SetViewCamera(scene, view, camera) is per view");
    Rend.SetViewCount(RENDER_DEFAULT_SCENE, 2);
    for(uint64_t Frame = 1; Frame <= 3; ++Frame)   // targets made, then reused
    {
        Rend.EndFrame(FrameInfo{Frame, 0.016, EngineCore::Size(640, 360)});
        Rend.WaitForPreviousFrame();
    }
    View = LastRenderedView();
    Expect(Rend.GetViewCount() == 2 && View.ViewIndex == 0 && View.ViewportPixels.Width == 640 && View.ViewportPixels.Height == 180
               && View.Origin.GetX() == 100.0,
           "two views: the main one the top half, through the first active camera");

    Rend.SetViewCount(RENDER_DEFAULT_SCENE, 4);
    Rend.EndFrame(FrameInfo{4, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    View = LastRenderedView();
    Expect(Rend.GetViewCount() == 4 && View.ViewportPixels.Width == 320 && View.ViewportPixels.Height == 180,
           "four views: quadrants (views 2 and 3 through view 0's camera)");

    /* The dynamic split: every view the whole viewport, cut by its sites. */
    RenderViewSites Sites;
    Sites.Count = 2;
    Sites.X[0] = 200.0f; Sites.Y[0] = 180.0f;
    Sites.X[1] = 440.0f; Sites.Y[1] = 180.0f;
    Rend.SetViewSites(RENDER_DEFAULT_SCENE, Sites);
    for(uint64_t Frame = 10; Frame <= 12; ++Frame)   // the second view appears, seeded from the first, then carries on
    {
        Rend.EndFrame(FrameInfo{Frame, 0.016, EngineCore::Size(640, 360)});
        Rend.WaitForPreviousFrame();
    }
    View = LastRenderedView();
    Expect(Rend.GetViewCount() == 2 && View.ViewportPixels.Width == 640 && View.ViewportPixels.Height == 360
               && View.ViewportX == 0 && View.ViewportY == 0,
           "sites: two views, each the whole viewport, over SetViewCount's four");
    Sites.Count = 1;
    Rend.SetViewSites(RENDER_DEFAULT_SCENE, Sites);
    Rend.EndFrame(FrameInfo{13, 0.016, EngineCore::Size(640, 360)});
    Rend.WaitForPreviousFrame();
    View = LastRenderedView();
    Expect(Rend.GetViewCount() == 4 && View.ViewportPixels.Width == 320, "fewer than two sites: the rectangles again");

    Rend.SetViewCount(RENDER_DEFAULT_SCENE, 1);
    Rend.EndFrame(FrameInfo{5, 0.016, EngineCore::Size(800, 600)});
    Rend.WaitForPreviousFrame();
    View = LastRenderedView();
    Expect(View.ViewportPixels.Width == 800 && View.ViewportPixels.Height == 600 && View.ViewportX == 0 && View.ViewportY == 0,
           "back to one view: the whole viewport again, resized");

    Rend.UnbindWorld();
    Rend.Shutdown();
}
#endif // WITH_EDITOR

// ============================================================================
// Debug lines: queued on the game thread, drawn once, then gone
// ============================================================================

/* UIDrawList (UIDrawList.h): batching, clipping, nine-slice, packing. CPU only. */
static void TestUIDrawList()
{
    std::printf("[ui draw list]\n");

    const uint32_t White = PackUIColor(1.0f, 1.0f, 1.0f, 1.0f);
    Expect(White == 0xFFFFFFFFu && PackUIColor(1.0f, 0.0f, 0.0f, 1.0f) == 0xFF0000FFu, "PackUIColor puts R in the low byte");

    UIDrawList List;
    Expect(List.IsEmpty() && List.GetTextures().size() == 1 && List.GetTextures()[0].empty(), "a new list is empty, texture 0 white");

    List.AddRect({ 0, 0, 10, 10 }, White);
    List.AddRect({ 20, 0, 10, 10 }, White);
    Expect(List.GetCommands().size() == 1 && List.GetCommands()[0].IndexCount == 12 && List.GetVertices().size() == 8,
           "two rects in one texture and clip are one command");

    const uint32_t Atlas = List.FindOrAddTexture("ui:font");
    Expect(Atlas == 1 && List.FindOrAddTexture("ui:font") == 1, "FindOrAddTexture adds a key once");
    List.AddImage({ 0, 20, 8, 8 }, { 0, 0, 1, 1 }, White, Atlas);
    Expect(List.GetCommands().size() == 2 && List.GetCommands()[1].Texture == Atlas, "a texture change starts a command");

    List.SetSolidFill(Atlas, 0.0f, 0.0f);
    List.AddRect({ 0, 40, 8, 8 }, White);
    Expect(List.GetCommands().size() == 2, "a solid fill from the atlas joins the atlas's command");

    List.PushClip({ 5.5f, 5.0f, 10.0f, 10.0f });
    List.PushClip({ 0.0f, 10.0f, 100.0f, 100.0f });
    const UIClipRect Clip = List.GetClip();
    Expect(Clip.X == 5 && Clip.Y == 10 && Clip.W == 11 && Clip.H == 5, "nested clips intersect, rounded out to whole pixels");
    List.AddRect({ 0, 0, 50, 50 }, White);
    Expect(List.GetCommands().size() == 3 && List.GetCommands()[2].Clip == Clip, "a clip change starts a command");
    List.PopClip();
    List.PopClip();
    Expect(List.GetClip().IsNone(), "popping every clip leaves none");

    List.PushClip({ 0, 0, 10, 10 });
    List.PushClip({ 50, 50, 10, 10 });
    const size_t Before = List.GetIndices().size();
    List.AddRect({ 0, 0, 100, 100 }, White);
    Expect(List.GetIndices().size() == Before, "nothing is drawn inside clips that do not meet");
    List.PopClip();
    List.PopClip();

    List.AddRect({ 0, 0, 10, 10 }, PackUIColor(1.0f, 1.0f, 1.0f, 0.0f));
    Expect(List.GetIndices().size() == Before, "a fully transparent rect is skipped");

    UIDrawList Nine;
    const float Margins[4]   = { 4, 4, 4, 4 };
    const float UVMargins[4] = { 0.25f, 0.25f, 0.25f, 0.25f };
    Nine.AddNineSlice({ 0, 0, 100, 40 }, Margins, { 0, 0, 1, 1 }, UVMargins, White, Nine.FindOrAddTexture("Textures/Button.nmtexture"));
    Expect(Nine.GetVertices().size() == 36 && Nine.GetIndices().size() == 54, "a nine-slice is nine quads");
    Expect(Nine.GetVertices()[4].X == 4.0f && Nine.GetVertices()[4].U == 0.25f, "a nine-slice's middle column starts at its margin");
    UIDrawList Small;
    Small.AddNineSlice({ 0, 0, 6, 40 }, Margins, { 0, 0, 1, 1 }, UVMargins, White, 0);
    Expect(Small.GetVertices()[1].X == 3.0f, "margins wider than the rect shrink to fit");

    UIDrawList Combined;
    Combined.AddRect({ 0, 0, 4, 4 }, White);
    Combined.Append(Nine);
    Expect(Combined.GetCommands().size() == 2 && Combined.GetVertices().size() == 40 && Combined.GetIndices()[6] == 4,
           "Append offsets the other list's indices past this list's vertices");

    std::vector<uint8_t> Block(List.PackedSize());
    List.Pack(Block.data());
    UIDrawListView View;
    Expect(View.Parse(Block.data(), Block.size()) && View.VertexCount == List.GetVertices().size()
           && View.IndexCount == List.GetIndices().size() && View.CommandCount == List.GetCommands().size()
           && View.Textures.size() == 2 && View.Textures[1] == "ui:font", "Pack and Parse round-trip");
    Expect(!View.Parse(Block.data(), Block.size() - 1), "Parse refuses a truncated block");
    Block[sizeof(uint32_t)] ^= 0x40u;   // the vertex count
    Expect(!View.Parse(Block.data(), Block.size()), "Parse refuses a block whose counts do not add up");

    List.Reset();
    Expect(List.IsEmpty() && List.GetTextures().size() == 1, "Reset empties the list and its textures");
}

/* Renderer::SubmitUIDrawList on the Null backend: accepted, kept, cleared. */
static void TestUISubmit()
{
    std::printf("[ui submit]\n");

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds for the UI test");
    if(!bInit)
    {
        Rend.Shutdown();
        return;
    }
    const EngineCore::Size Viewport(320, 200);
    UIDrawList List;
    List.AddRect({ 10, 10, 50, 20 }, PackUIColor(1.0f, 0.0f, 0.0f, 1.0f));
    Rend.SubmitUIDrawList(RENDER_DEFAULT_SCENE, List);
    Rend.SubmitUIDrawList(RENDER_INVALID_SCENE, List);   // ignored

    EngineCore::Image Atlas(4, 4, EImageFormat::RGBA8);
    Rend.SetUIImage("ui:test", Atlas);
    Rend.SetUIImage("not-ui", Atlas);                    // ignored: no "ui:" prefix
    for(uint64_t Frame = 0; Frame < 3; ++Frame)
    {
        Rend.EndFrame(FrameInfo{Frame, 0.016, Viewport});
        Rend.WaitForPreviousFrame();
    }
    Rend.ReleaseUIImage("ui:test");
    Rend.SubmitUIDrawList(RENDER_DEFAULT_SCENE, UIDrawList());
    Rend.EndFrame(FrameInfo{3, 0.016, Viewport});
    Rend.WaitForPreviousFrame();
    Rend.Shutdown();
    Expect(true, "a UI list and image survive frames and Shutdown on the Null backend");
}

#if WITH_EDITOR   /* main runs it in editor builds only: it uses the editor's Renderer API */
static void TestDebugLines()
{
    std::printf("[debug lines]\n");

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds for the debug-line test");
    if(!bInit)
    {
        Rend.Shutdown();
        return;
    }

    /* A perspective camera at the origin looking down +Z, as the editor sets. */
    const EngineCore::Size Viewport(1280, 720);
    const RenderView View = RenderView::MakePerspective(Viewport, EngineCore::Vector3(0.0, 0.0, 0.0),
                                                        EngineCore::Vector3(0.0, 0.0, 1.0), 60.0f, 1.0f, 10000.0f);
    Rend.SetViewOverride(&View);

    auto DrawCallsForFrame = [&Rend, &Viewport](const uint64_t InFrame) -> uint64_t
    {
        RenderStats Before;
        Rend.GetStats(Before);
        Rend.EndFrame(FrameInfo{InFrame, 0.016, Viewport});
        Rend.WaitForPreviousFrame();
        RenderStats After;
        Rend.GetStats(After);
        return After.DrawCalls - Before.DrawCalls;
    };

    Rend.DrawDebugBox(GameCore::AABB(EngineCore::Vector3(-50.0, -50.0, 400.0), EngineCore::Vector3(50.0, 50.0, 500.0)),
                      EngineCore::Color(255, 160, 0));
    Expect(DrawCallsForFrame(0) == 12, "a solid box draws its twelve edges");

    Expect(DrawCallsForFrame(1) == 0, "lines last one frame: the next EndFrame draws none");

    Rend.DrawDebugBox(GameCore::AABB(EngineCore::Vector3(-50.0, -50.0, 400.0), EngineCore::Vector3(50.0, 50.0, 400.0)),
                      EngineCore::Color(255, 160, 0));
    Expect(DrawCallsForFrame(2) == 4, "a flat box draws its outline; zero-length edges are skipped");

    DebugLine EndOn;
    EndOn.Start = EngineCore::Vector3(0.0, 0.0, 100.0);
    EndOn.End   = EngineCore::Vector3(0.0, 0.0, 200.0);
    Rend.DrawDebugLine(EndOn);
    DebugLine Behind;
    Behind.Start = EngineCore::Vector3(-10.0, 0.0, -100.0);
    Behind.End   = EngineCore::Vector3( 10.0, 0.0, -100.0);
    Rend.DrawDebugLine(Behind);
    Expect(DrawCallsForFrame(3) == 0, "a segment seen end-on, or behind the camera, is not drawn");

    Rend.DrawDebugBox(GameCore::AABB(), EngineCore::Color(255, 255, 255));
    Expect(DrawCallsForFrame(4) == 0, "an empty AABB draws nothing");

    Rend.SetViewOverride(nullptr);
    Rend.Shutdown();
}
#endif // WITH_EDITOR

// ============================================================================
// Materials: a primitive naming a material, missing then present
// ============================================================================

namespace
{
    std::atomic<int> GMaterialLoads{0};
    std::atomic<int> GMaterialFallbacks{0};
    LogHandler       GPreviousLogHandler = nullptr;

    /* Counts material vs fallback draws via the log; Null draws nothing. */
    void MaterialLogHandler(const LogVerbosity InVerbosity, const char* InMessage)
    {
        const std::string Text = InMessage ? InMessage : "";
        if(Text.find("material 'SmokeGlow' loaded") != std::string::npos)  { ++GMaterialLoads; }
        if(Text.find("material 'SmokeGlow':") != std::string::npos
           && Text.find("plain tint") != std::string::npos)                { ++GMaterialFallbacks; }
        if(GPreviousLogHandler) { GPreviousLogHandler(InVerbosity, InMessage); }
    }

    /* Counts block textures the backend could not sample, so decoded to RGBA8 instead. */
    std::atomic<int> GBlockDecodes{0};
    void BlockLogHandler(const LogVerbosity InVerbosity, const char* InMessage)
    {
        if(InMessage && std::strstr(InMessage, "so it is decoded to RGBA8")) { ++GBlockDecodes; }
        if(GPreviousLogHandler) { GPreviousLogHandler(InVerbosity, InMessage); }
    }
}

#if WITH_EDITOR   /* main runs it in editor builds only: it uses the editor's Renderer API */
static void TestMaterials()
{
    std::printf("[materials]\n");

    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreMaterials";
    std::error_code Ignored;
    std::filesystem::remove_all(Root, Ignored);
    std::filesystem::create_directories(Root / "Materials", Ignored);

    FilesystemProvider Provider(Root.generic_string());
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Provider);
    GPreviousLogHandler = GetLogHandler();
    SetLogHandler(&MaterialLogHandler);

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds for the material test");
    if(bInit)
    {
        const EngineCore::Size Viewport(640, 360);
        const RenderView View = RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
        Rend.SetViewOverride(&View);

        GameCore::World World;
        const GameCore::Entity Quad = World.CreateEntity("Glowing");
        Quad.AddComponent<GameCore::TransformComponent>()->Scale = EngineCore::Vector3(100.0, 100.0, 1.0);
        RenderComponent Prim;
        std::strcpy(Prim.Materials[0], "SmokeGlow");
        Quad.AddComponent(Prim);
        Rend.BindWorld(World);

        uint64_t Frame = 200;
        auto DrawFrame = [&]() -> uint64_t
        {
            RenderStats Before;
            Rend.GetStats(Before);
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            RenderStats After;
            Rend.GetStats(After);
            return After.DrawCalls - Before.DrawCalls;
        };

        DrawFrame();   // extracts; the proxy draws from the next frame
        const uint64_t MissingDraws = DrawFrame();
        Expect(MissingDraws == 1, "a primitive whose material is missing still draws, as the plain tint");
        Expect(GMaterialFallbacks == 1 && GMaterialLoads == 0, "a missing material is warned about");
        DrawFrame();
        Expect(GMaterialFallbacks == 1, "and warned about once, not every frame");

        std::ofstream(Root / "Materials" / "SmokeGlow.material")
            << "# a test material\nNimbleMaterial 3\nDomain Translucent\nShading Unlit\nBlockSize 176\n"
               "Param Tint vec3 160 1 0.5 0.25 0\n";
        Rend.ReloadMaterial("SmokeGlow");
        const uint64_t LoadedDraws = DrawFrame();
        Expect(GMaterialLoads == 1, "ReloadMaterial makes the next draw read the material afresh");
        Expect(LoadedDraws == 1, "a primitive with a loaded material draws once");

        DrawFrame();
        Expect(GMaterialLoads == 1, "a loaded material is kept, not reloaded per frame");

        /* Answered once, next frame; Null reads back nothing. */
        const uint64_t Preview = Rend.RequestMaterialPreview("SmokeGlow", 64, EngineCore::Color(255, 255, 255));
        const uint64_t Missing = Rend.RequestMaterialPreview("NoSuchMaterial", 64, EngineCore::Color(255, 255, 255));
        Expect(Preview != 0 && Missing != 0 && Preview != Missing, "a preview request gets its own id");
        EngineCore::Image Pixels;
        std::string       Error;
        Expect(!Rend.TakeMaterialPreview(Preview, Pixels, Error), "a preview is not ready before a frame renders it");
        DrawFrame();
        Expect(Rend.TakeMaterialPreview(Preview, Pixels, Error) && Error.find("reads nothing back") != std::string::npos,
               "the Null backend answers a preview with why it has no picture");
        Expect(!Rend.TakeMaterialPreview(Preview, Pixels, Error), "a preview is taken once");
        Expect(Rend.TakeMaterialPreview(Missing, Pixels, Error) && Error.find("could not be loaded") != std::string::npos,
               "a preview of a missing material says so");
        Expect(Rend.RequestMaterialPreview("", 64, EngineCore::Color()) == 0, "a preview needs a name");

        std::ofstream(Root / "Materials" / "SmokeGlow.material", std::ios::trunc) << "NimbleMaterial 7\n";
        Rend.ReloadMaterial(nullptr);
        DrawFrame();
        Expect(GMaterialFallbacks == 2, "a material file of an unknown version falls back, warned");

        Rend.UnbindWorld();
        Rend.SetViewOverride(nullptr);
    }
    Rend.Shutdown();

    SetLogHandler(GPreviousLogHandler);
    SetFileProvider(PreviousProvider);
    std::filesystem::remove_all(Root, Ignored);
}
#endif // WITH_EDITOR

/* .material v3, the material block, permutation names, the shader cache and
 * the layered provider it mounts on -- no renderer needed. */
static void TestMaterialRuntime()
{
    std::printf("[material runtime]\n");

    // --- .material v3 -------------------------------------------------------------
    MaterialDescription Desc;
    std::string Error;
    const char* Wave = "# generated\nNimbleMaterial 3\nDomain Masked\nShading Lit\nTwoSided 1\nClipValue 0.25\n"
                       "MaxOffset 2\nHasOffset 1\nBlockSize 176\nParam Color vec3 160 0.2 0.6 1 0\n"
                       "Param Height float 172 0.5 0 0 0\nSomeFutureLine 1 2 3\n";
    Expect(ParseMaterialDescription(Wave, Desc, Error), "a version 3 .material parses");
    Expect(Desc.Domain == EMaterialDomain::Masked && Desc.Shading == EMaterialShading::Lit && Desc.bTwoSided
           && Desc.bHasOffset && Desc.MaxOffset == 2.0f && Desc.ClipValue == 0.25f && Desc.BlockSize == 176,
           "its header lines are read");
    const MaterialParameter* Height = Desc.FindParameter("Height");
    Expect(Desc.Parameters.size() == 2 && Height && Height->Offset == 172 && Height->Type == "float" && Height->Default[0] == 0.5f,
           "its parameters keep their offsets and defaults");

    std::vector<uint8_t> Block;
    BuildMaterialBlock(Desc, EngineCore::Color(255, 0, 0, 255), Block);
    float Read[44] = {};
    if(Block.size() == 176) { std::memcpy(Read, Block.data(), sizeof(Read)); }
    bool bNoBake = true;
    for(int Index = 8; Index < 40; ++Index) { bNoBake &= Read[Index] == 0.0f; }
    Expect(Block.size() == 176 && Read[0] == 1.0f && Read[1] == 0.0f && Read[3] == 1.0f && Read[4] == 0.25f && Read[5] == 1.0f
           && Read[40] == 0.2f && Read[42] == 1.0f && Read[43] == 0.5f,
           "the block is the tint, clip value and two-sided flag, then each default at its offset");
    Expect(bNoBake, "the engine's baked lighting bytes start empty: no bake (MaterialDescription.h)");

    Expect(!ParseMaterialDescription("NimbleMaterial 2\nParam Tint vec3 32 1 1 1 0\n", Desc, Error)
           && Error.find("recompile") != std::string::npos, "a version 2 .material is refused, asking for a recompile");
    Expect(!ParseMaterialDescription("NimbleMaterial 3\nBlockSize 160\nParam X vec4 160 0 0 0 0\n", Desc, Error),
           "a parameter outside the block is refused");
    Expect(!ParseMaterialDescription("NimbleMaterial 3\nBlockSize 176\nParam X vec4 32 0 0 0 0\n", Desc, Error),
           "a parameter over the engine's bytes is refused");
    Expect(!ParseMaterialDescription("NimbleMaterial 3\nDomain Sideways\n", Desc, Error), "an unknown domain is refused");

    // --- Shading models (SHADING_MODELS.md) ---------------------------------------------
    Expect(ParseMaterialDescription("NimbleMaterial 3\nShading Lit\nShadingModel Skin\n", Desc, Error)
           && Desc.ShadingModel == EMaterialShadingModel::Skin, "a ShadingModel line names the model");
    Expect(ParseMaterialDescription("NimbleMaterial 3\nShading Lit\n", Desc, Error) && Desc.ShadingModel == EMaterialShadingModel::Default,
           "without one it is Default");
    Expect(!ParseMaterialDescription("NimbleMaterial 3\nShadingModel Velour\n", Desc, Error), "an unknown shading model is refused");
    Expect(GetPixelPermutationKey(EMaterialDomain::Opaque, EMaterialShading::Lit) == 0u
           && GetPixelPermutationKey(EMaterialDomain::Opaque, EMaterialShading::Lit, EMaterialShadingModel::ClearCoat) == 0x20u
           && GetPixelPermutationKey(EMaterialDomain::Masked, EMaterialShading::Lit, EMaterialShadingModel::Skin) == 0xa1u,
           "a shading model takes the pixel key's bits 5-7; Default's keys are unchanged");
    for(uint32_t Each = 0; Each < MATERIAL_SHADING_MODEL_COUNT; ++Each)
    {
        EMaterialShadingModel Found = EMaterialShadingModel::Default;
        Expect(FindShadingModel(GetShadingModelName(EMaterialShadingModel(Each)), Found) && uint32_t(Found) == Each,
               "a shading model's name finds it again");
    }

    // --- Texture lines: what a Texture Sample writes -----------------------------------
    const char* Textured = "NimbleMaterial 3\nBlockSize 160\n"
                           "Texture 1 Default Default Content/Textures/Brick.nmtexture\n"
                           "Texture 2 Nearest Clamp Content/Textures/Height Map.nmtexture\r\n"
                           "Texture 3 Linear Mirror Content/Water.nmtexture\n";
    Expect(ParseMaterialDescription(Textured, Desc, Error) && Desc.Textures.size() == 3, "Texture lines parse");
    if(Desc.Textures.size() == 3)
    {
        const MaterialTexture& Brick  = Desc.Textures[0];
        const MaterialTexture& Height = Desc.Textures[1];
        const MaterialTexture& Water  = Desc.Textures[2];
        Expect(Brick.Slot == 1 && Brick.Path == "Content/Textures/Brick.nmtexture" && !Brick.bFilter && !Brick.bWrap,
               "Default keeps the texture asset's own filter and wrap");
        Expect(Height.Slot == 2 && Height.Path == "Content/Textures/Height Map.nmtexture"
               && Height.bFilter && Height.Filter == ETextureFilter::Nearest && Height.bWrap && Height.Wrap == ETextureWrap::Clamp,
               "a node's overrides are read, and a path is the rest of its line, spaces and all");
        Expect(Water.bFilter && Water.Filter == ETextureFilter::Linear && Water.bWrap && Water.Wrap == ETextureWrap::Mirror,
               "Linear and Mirror are read too");
    }
    Expect(!ParseMaterialDescription("NimbleMaterial 3\nTexture 0 Default Default Content/A.nmtexture\n", Desc, Error)
           && !ParseMaterialDescription("NimbleMaterial 3\nTexture 8 Default Default Content/A.nmtexture\n", Desc, Error),
           "slot 0 (a PostProcess scene's) and slots past 7 are refused");
    Expect(!ParseMaterialDescription("NimbleMaterial 3\nTexture 1 Blurry Default Content/A.nmtexture\n", Desc, Error)
           && !ParseMaterialDescription("NimbleMaterial 3\nTexture 1 Default Default\n", Desc, Error),
           "an unknown sampler setting, or no path, is refused");

    // --- Permutation names: what cook_material_graphs.py writes ---------------------
    Expect(GetPermutationFileName("Glow", GetPixelPermutationKey(EMaterialDomain::Translucent, EMaterialShading::Unlit), false)
           == "Glow@0000000a.fsh", "Translucent Unlit is pixel key 0xa");
    Expect(GetPermutationFileName(MATERIAL_DEFAULT_VERTEX, GetVertexPermutationKey(EMaterialVertexInput::Mesh), true)
           == "MaterialDefault@00000100.vsh", "the shared mesh vertex shader is key 0x100");

    // --- Material names: a graph's runtime path, or an older bare name ------------------
    {
        std::string Stem;
        Expect(GetMaterialStem("Materials/Glow.material.nmgraph", Stem) && Stem == "Materials/Glow",
               "a material graph's path names its cooked files by its stem");
        Expect(GetMaterialStem("Engine/Content/Materials/DefaultLit.material.nmgraph", Stem)
               && Stem == "Engine/Content/Materials/DefaultLit", "the engine's are below Engine/Content");
        Expect(GetMaterialStem("Glow", Stem) && Stem == "Materials/Glow", "a bare name, as worlds saved them before, is Materials/<name>");
        Expect(!GetMaterialStem("Materials/Glow.material", Stem) && !GetMaterialStem("Scripts/Mover.lua", Stem)
               && !GetMaterialStem("Material.001", Stem) && !GetMaterialStem("", Stem),
               "anything else names no material (an imported mesh's own material names, say)");
        Expect(GetPermutationFileName("Materials/Glow", 0xa, false) == "Materials/Glow@0000000a.fsh",
               "a permutation is named by the stem, folders and all");
    }
    Expect(GetShaderCachePath("SandboxGame", ERenderBackend::Vulkan) == "Shaders/Cache/SandboxGame.Vulkan.nmsc",
           "a module's cache is named by module and platform");

    // --- The shader cache ---------------------------------------------------------------
    const auto Fnv = [](const std::vector<uint8_t>& InBytes)
    {
        uint64_t Hash = 0xcbf29ce484222325ull;
        for(const uint8_t Byte : InBytes) { Hash ^= Byte; Hash *= 0x100000001b3ull; }
        return Hash;
    };
    const auto MakeCache = [&](const uint32_t InTemplateVersion, const bool bInCorrupt)
    {
        Archive Ar = Archive::ForWriting();
        uint16_t Version = 1;
        std::string Module = "Smoke", Platform = "Vulkan";
        uint32_t Template = InTemplateVersion;
        Ar.OpenChunk(MakeArchiveTag('N','M','S','C'), Version);
        Ar << Module << Platform << Template;
        Ar.CloseChunk();

        std::vector<uint8_t> Shader = { 1, 2, 3, 4, 5 };
        uint64_t Hash = Fnv(Shader);
        uint32_t Size = uint32_t(Shader.size());
        if(bInCorrupt) { Shader[2] = 9; }
        Ar.OpenChunk(MakeArchiveTag('B','L','O','B'), Version);
        Ar << Hash << Size;
        Ar.SerializeBytes(Shader.data(), Shader.size());
        Ar.CloseChunk();
        for(const char* Path : { "Shaders/Cooked/Vulkan/Materials/A@00000000.fsh.spv", "Shaders/Cooked/Vulkan/Materials/B@00000000.fsh.spv" })
        {
            std::string Text = Path;
            Ar.OpenChunk(MakeArchiveTag('F','I','L','E'), Version);
            Ar << Text << Hash;
            Ar.CloseChunk();
        }
        return Ar.GetBytes();
    };

    ShaderCacheContents Contents;
    Expect(ReadShaderCache(MakeCache(MATERIAL_TEMPLATE_VERSION, false), Contents, Error)
           && Contents.Module == "Smoke" && Contents.Platform == "Vulkan" && Contents.TemplateVersion == MATERIAL_TEMPLATE_VERSION
           && Contents.Files.size() == 2, "a cache reads its header and every file");
    const auto A = Contents.Files.find("Shaders/Cooked/Vulkan/Materials/A@00000000.fsh.spv");
    const auto B = Contents.Files.find("Shaders/Cooked/Vulkan/Materials/B@00000000.fsh.spv");
    Expect(A != Contents.Files.end() && B != Contents.Files.end() && A->second == B->second && A->second->size() == 5,
           "two paths with the same bytes share one blob");
    Expect(!ReadShaderCache(MakeCache(MATERIAL_TEMPLATE_VERSION, true), Contents, Error) && Error.find("corrupt") != std::string::npos,
           "a blob whose bytes do not match its hash is refused");
    std::vector<uint8_t> Truncated = MakeCache(MATERIAL_TEMPLATE_VERSION, false);
    Truncated.resize(Truncated.size() - 7);
    Expect(!ReadShaderCache(Truncated, Contents, Error), "a truncated cache is refused");
    Expect(!ReadShaderCache({ 'n', 'o', 'p', 'e' }, Contents, Error), "a file that is not a cache is refused");

    /* One NimbleCook cooked and the build staged, when this runs from the runtime directory. */
    std::vector<uint8_t> Cooked;
    if(appFileReadAll(GetShaderCachePath("Engine", ERenderBackend::Vulkan), Cooked))
    {
        Expect(ReadShaderCache(Cooked, Contents, Error) && Contents.Module == "Engine"
               && Contents.Files.count("Engine/Content/Materials/DefaultLit.material") == 1
               && Contents.Files.count("Shaders/Cooked/Vulkan/Engine/Content/Materials/DefaultLit@00000000.fsh.spv") == 1
               && Contents.Files.count("Shaders/Cooked/Vulkan/Materials/MaterialDefault@00000100.vsh.spv") == 1,
               "the Engine cache cook_material_graphs.py wrote reads back: DefaultLit, cooked from its graph's <gen_code> "
               "at its graph's path, and the shared vertex shaders");
    }

    // --- The layered provider ----------------------------------------------------------------
    std::map<std::string, MemoryFileProvider::FileBytes> First, Second;
    First["Shared.txt"]  = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{ 'f' });
    First["First.txt"]   = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{ '1' });
    Second["Shared.txt"] = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{ 's' });

    MemoryFileProvider Base({ { "Shared.txt", std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{ 'b' }) } });
    LayeredFileProvider Layers(&Base, false);
    std::string Text;
    Expect(Layers.ReadAllText("Shared.txt", Text) && Text == "b", "with no layers the base answers");
    Expect(Layers.MountLayer("First", std::make_shared<MemoryFileProvider>(First))
           && Layers.MountLayer("Second", std::make_shared<MemoryFileProvider>(Second)), "layers mount");
    Expect(Layers.ReadAllText("Shared.txt", Text) && Text == "s", "the newest layer shadows the older and the base");
    Expect(Layers.ReadAllText("First.txt", Text) && Text == "1" && Layers.Exists("First.txt"), "an older layer still answers what it alone has");
    Expect(Layers.UnmountLayer("Second") && Layers.ReadAllText("Shared.txt", Text) && Text == "f", "unmounting uncovers the layer below");
    Expect(!Layers.UnmountLayer("Second"), "a layer unmounts once");

    LayeredFileProvider BaseFirst(&Base, true);
    BaseFirst.MountLayer("First", std::make_shared<MemoryFileProvider>(First));
    Expect(BaseFirst.ReadAllText("Shared.txt", Text) && Text == "b" && BaseFirst.ReadAllText("First.txt", Text) && Text == "1",
           "base-first (the editor's) asks the disk before any layer");
    Expect(!FilesystemProvider().MountLayer("X", std::make_shared<MemoryFileProvider>(First)), "a plain provider takes no layers");

    // --- Vertex colours in .nmmesh ---------------------------------------------------------
    MeshData Coloured;
    Coloured.Positions = { 0, 0, 0,  1, 0, 0,  0, 1, 0 };
    Coloured.Normals   = { 0, 0, -1, 0, 0, -1, 0, 0, -1 };
    Coloured.UV0       = { 0, 0,  1, 0,  0, 1 };
    Coloured.Colors    = { 1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 1, 0.5f };
    Coloured.Indices   = { 0, 1, 2 };
    Archive Save = Archive::ForWriting();
    Expect(SerializeMesh(Save, Coloured), "a mesh with vertex colours saves");
    Archive Load = Archive::ForReading(Save.GetBytes());
    MeshData Back;
    Expect(SerializeMesh(Load, Back) && Back.Colors == Coloured.Colors, "and loads them back");

    MeshData Plain = Coloured;
    Plain.Colors.clear();
    Archive SavePlain = Archive::ForWriting();
    SerializeMesh(SavePlain, Plain);
    Archive LoadPlain = Archive::ForReading(SavePlain.GetBytes());
    MeshData PlainBack;
    Expect(SerializeMesh(LoadPlain, PlainBack) && PlainBack.Colors.empty() && PlainBack.Indices == Plain.Indices,
           "a mesh without colours has no colour chunk, and loads without one (white)");

    Coloured.Colors.pop_back();
    Archive Bad = Archive::ForWriting();
    Expect(!SerializeMesh(Bad, Coloured), "colours for only some vertices are refused");

    // --- Tangents (MTAN) in .nmmesh ---------------------------------------------------------
    MeshData Tangented = Plain;
    Tangented.Tangents = { 1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, -1 };
    Archive SaveTangents = Archive::ForWriting();
    Expect(SerializeMesh(SaveTangents, Tangented), "a mesh with tangents saves");
    Archive LoadTangents = Archive::ForReading(SaveTangents.GetBytes());
    MeshData TangentsBack;
    Expect(SerializeMesh(LoadTangents, TangentsBack) && TangentsBack.Tangents == Tangented.Tangents
           && TangentsBack.Indices == Tangented.Indices, "and loads them back, handedness and all");
    Expect(PlainBack.Tangents.empty(), "a mesh saved without tangents (every file before MTAN) loads with none");
    Tangented.Tangents.resize(8);
    Archive BadTangents = Archive::ForWriting();
    Expect(!SerializeMesh(BadTangents, Tangented), "tangents for only some vertices are refused");
}

#if WITH_EDITOR
/*
 * OPT-IN, on a real GPU: NIMBLE_SMOKE_GPU=Vulkan (or D3D12), run from a
 * runtime directory the build staged (Binaries/<Platform>_<Config>). Renders
 * material previews offscreen -- no window, no present -- through the mounted
 * Engine and SandboxGame shader caches, asked before any loose file, and
 * checks the pixels: the templates, the uniform blocks, World Position
 * Offset's vertex shaders and a PostProcess pass, end to end.
 */
static void TestMaterialsOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[materials on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[materials on the GPU: %s]\n", GetShaderPlatformName(Backend));

    FilesystemProvider  Disk;                       // the working directory
    LayeredFileProvider Layers(&Disk, false);       // caches first: they are what is tested
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        Rend.MountShaderCache("SandboxGame");   // Initialize mounted Engine

        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
        Rend.SetViewOverride(&View);
        GameCore::World World;
        Rend.BindWorld(World);

        uint64_t Frame = 1;
        const auto DrawFrame = [&]
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
        };
        /* The frame that draws a preview, then as many as the GPU may run
         * behind ([Rendering] FramesInFlight, 3 at most): its readback has
         * finished. */
        const auto SettlePreviews = [&]
        {
            for(int Each = 0; Each < 4; ++Each) { DrawFrame(); }
        };
        DrawFrame();

        const std::vector<std::string> Layers_ = Layers.GetLayerNames();
        Expect(std::find(Layers_.begin(), Layers_.end(), GetShaderCacheLayerName("Engine")) != Layers_.end()
               && std::find(Layers_.begin(), Layers_.end(), GetShaderCacheLayerName("SandboxGame")) != Layers_.end(),
               "the Engine and SandboxGame caches are mounted");

        /* The centre pixel of each preview, as R, G, B; and its quarter
         * points -- top-left, top-right, bottom-left, bottom-right. */
        struct Check { const char* Material; uint64_t Request; int Rgb[3]; bool bOk; int Quarter[4][3]; };
        /* By their graph paths: a bare name ("Wave") finds a loose Materials/<name> file first, which an
         * old build may have left behind, cooked against another template. */
        Check Checks[] = { { "Engine/Content/Materials/DefaultLit.material.nmgraph", 0, {}, false, {} },
                           { "Content/Materials/Glow.material.nmgraph", 0, {}, false, {} },
                           { "Content/Materials/Wave.material.nmgraph", 0, {}, false, {} },
                           { "Content/Materials/Negative.material.nmgraph", 0, {}, false, {} },
                           { "Content/Materials/Textured.material.nmgraph", 0, {}, false, {} },   { "Content/Materials/MipProbe.material.nmgraph", 0, {}, false, {} },
                           { "Content/Materials/NormalProbe.material.nmgraph", 0, {}, false, {} },
                           { "Content/Materials/TwoSidedProbe.material.nmgraph", 0, {}, false, {} } };
        for(Check& Each : Checks)
        {
            Each.Request = Rend.RequestMaterialPreview(Each.Material, 64, EngineCore::Color(255, 255, 255, 255));
        }
        SettlePreviews();
        for(Check& Each : Checks)
        {
            EngineCore::Image Pixels;
            std::string Error;
            Each.bOk = Rend.TakeMaterialPreview(Each.Request, Pixels, Error) && Pixels.GetWidth() == 64 && Pixels.GetData();
            if(!Each.bOk)
            {
                std::printf("    %s: %s\n", Each.Material, Error.c_str());
                continue;
            }
            const bool bBgra = Pixels.GetFormat() == EImageFormat::BGRA8;
            const auto Read = [&](const int InX, const int InY, int (&OutRgb)[3])
            {
                const uint8_t* Px = Pixels.GetData() + Pixels.GetStride() * InY + InX * BytesPerPixel(Pixels.GetFormat());
                OutRgb[0] = bBgra ? Px[2] : Px[0];
                OutRgb[1] = Px[1];
                OutRgb[2] = bBgra ? Px[0] : Px[2];
            };
            Read(32, 32, Each.Rgb);
            Read(16, 16, Each.Quarter[0]);
            Read(48, 16, Each.Quarter[1]);
            Read(16, 48, Each.Quarter[2]);
            Read(48, 48, Each.Quarter[3]);
            std::printf("    %s centre = %d %d %d\n", Each.Material, Each.Rgb[0], Each.Rgb[1], Each.Rgb[2]);
        }

        const auto Near = [](const int InValue, const int InWant) { return InValue >= InWant - 12 && InValue <= InWant + 12; };
        const Check& DefaultLit = Checks[0];
        const Check& Glow       = Checks[1];
        const Check& Wave       = Checks[2];
        const Check& Negative   = Checks[3];
        Expect(DefaultLit.bOk && DefaultLit.Rgb[0] > 60 && std::abs(DefaultLit.Rgb[0] - DefaultLit.Rgb[2]) < 16,
               "DefaultLit draws its white tint lit grey, not the clear colour");
        Expect(Glow.bOk && Near(Glow.Rgb[0], 51) && Near(Glow.Rgb[1], 204) && Near(Glow.Rgb[2], 255),
               "Glow's centre is its Color parameter, read from the material block");
        Expect(Wave.bOk && Wave.Rgb[2] > Wave.Rgb[0] + 40, "Wave (Lit, World Position Offset) draws its blue Color, lit");
        Expect(Negative.bOk && Near(Negative.Rgb[0], 255 - 38) && Near(Negative.Rgb[1], 255 - 40) && Near(Negative.Rgb[2], 255 - 46),
               "Negative (PostProcess) inverts the scene it runs over: the preview's clear colour");

        /* Quadrants.nmtexture: red, green / blue, white, rows top first. */
        const Check& Textured = Checks[4];
        const auto Is = [&](const int (&InRgb)[3], const int InR, const int InG, const int InB)
        {
            return Near(InRgb[0], InR) && Near(InRgb[1], InG) && Near(InRgb[2], InB);
        };
        std::printf("    Textured quarters = %d %d %d / %d %d %d / %d %d %d / %d %d %d\n",
                    Textured.Quarter[0][0], Textured.Quarter[0][1], Textured.Quarter[0][2],
                    Textured.Quarter[1][0], Textured.Quarter[1][1], Textured.Quarter[1][2],
                    Textured.Quarter[2][0], Textured.Quarter[2][1], Textured.Quarter[2][2],
                    Textured.Quarter[3][0], Textured.Quarter[3][1], Textured.Quarter[3][2]);
        Expect(Textured.bOk && Is(Textured.Quarter[0], 255, 0, 0) && Is(Textured.Quarter[1], 0, 255, 0)
               && Is(Textured.Quarter[2], 0, 0, 255) && Is(Textured.Quarter[3], 255, 255, 255),
               "Textured shows its texture upright: the image's top row at the quad's top");
        const bool bOneTexel = Is(Textured.Rgb, 255, 0, 0) || Is(Textured.Rgb, 0, 255, 0) || Is(Textured.Rgb, 0, 0, 255)
                            || Is(Textured.Rgb, 255, 255, 255);
        Expect(Textured.bOk && bOneTexel, "its node's Nearest overrides the texture's Linear: the centre is one texel, not a blend");

        /* MipProbe: level 0 red, the rest green, tiled 16 times over 64 pixels: 16 texels a pixel. */
        const Check& Mips = Checks[5];
        Expect(Mips.bOk && Mips.Rgb[1] > 200 && Mips.Rgb[0] < 40,
               "a minified texture samples its baked mip chain (green), not level 0 (red)");

        /* Block compression: Quadrants as BC1 (each quarter one 565 colour, so exact), from a layer
         * over its file. The GPU backends here sample BC1, so it is drawn as blocks: RenderCore warns
         * when it has to decode them instead. */
        {
            const char* QuadrantsPath = "Content/Textures/Quadrants.nmtexture";
            TextureData Blocky;
            Blocky.Filter      = ETextureFilter::Nearest;
            Blocky.Compression = ETextureCompression::BC1;
            Blocky.Pixels      = Image(16, 16, EImageFormat::RGBA8);
            for(int Y = 0; Y < 16; ++Y)
            {
                for(int X = 0; X < 16; ++X)
                {
                    const bool    bRight = X >= 8;
                    const uint8_t Quarter[4][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
                    std::memcpy(Blocky.Pixels.GetScanline(Y) + X * 4, Quarter[(Y >= 8 ? 2 : 0) + (bRight ? 1 : 0)], 4);
                }
            }
            std::string EncodeError;
            Archive Writer = Archive::ForWriting();
            const bool bEncoded = CompressTexture(Blocky, EncodeError) && SerializeTexture(Writer, Blocky);
            std::map<std::string, MemoryFileProvider::FileBytes> Files;
            Files[QuadrantsPath] = std::make_shared<const std::vector<uint8_t>>(Writer.GetBytes());
            Layers.MountLayer("SmokeBlocks", std::make_shared<MemoryFileProvider>(std::move(Files)));
            TextureAssetCache::Invalidate(QuadrantsPath);

            GBlockDecodes       = 0;
            GPreviousLogHandler = GetLogHandler();
            SetLogHandler(&BlockLogHandler);
            const uint64_t Request = Rend.RequestMaterialPreview("Content/Materials/Textured.material.nmgraph", 64,
                                                                 EngineCore::Color(255, 255, 255, 255));
            SettlePreviews();
            SetLogHandler(GPreviousLogHandler);
            const bool bBlocks = GBlockDecodes == 0;
            EngineCore::Image Pixels;
            std::string Error;
            const bool bTaken = Rend.TakeMaterialPreview(Request, Pixels, Error) && Pixels.GetWidth() == 64 && Pixels.GetData();
            int Quarter[4][3] = {};
            for(int Each = 0; bTaken && Each < 4; ++Each)
            {
                const bool     bBgra = Pixels.GetFormat() == EImageFormat::BGRA8;
                const uint8_t* Px    = Pixels.GetData() + Pixels.GetStride() * (Each < 2 ? 16 : 48)
                                     + (Each % 2 == 0 ? 16 : 48) * BytesPerPixel(Pixels.GetFormat());
                Quarter[Each][0] = bBgra ? Px[2] : Px[0];
                Quarter[Each][1] = Px[1];
                Quarter[Each][2] = bBgra ? Px[0] : Px[2];
            }
            std::printf("    BC1 quarters (%s) = %d %d %d / %d %d %d / %d %d %d / %d %d %d\n", bBlocks ? "as blocks" : "decoded",
                        Quarter[0][0], Quarter[0][1], Quarter[0][2], Quarter[1][0], Quarter[1][1], Quarter[1][2],
                        Quarter[2][0], Quarter[2][1], Quarter[2][2], Quarter[3][0], Quarter[3][1], Quarter[3][2]);
            Expect(bEncoded && bBlocks && bTaken && Is(Quarter[0], 255, 0, 0) && Is(Quarter[1], 0, 255, 0)
                   && Is(Quarter[2], 0, 0, 255) && Is(Quarter[3], 255, 255, 255),
                   "a BC1 texture is sampled as its blocks: the same quarters, upright");
            Layers.UnmountLayer("SmokeBlocks");
            TextureAssetCache::Invalidate(QuadrantsPath);
        }

        /* NormalProbe: an OpenGL normal tilted right and up the image, as 0.5 + 0.5 N. The quad faces the
         * camera (-Z), its image upright: right is +X, up +Y. A flipped green reads 64 in G. */
        const Check& Normals = Checks[6];
        Expect(Normals.bOk && Near(Normals.Rgb[0], 191) && Near(Normals.Rgb[1], 191) && Near(Normals.Rgb[2], 37),
               "the Normal Map node tilts the normal the way its map points: right and up the image, out of the surface");

        /* TwoSidedProbe: DefaultLit with Two Sided on. The preview quad faces the camera, so it is a front
         * face (gl_FrontFacing) and keeps its normal: lit as DefaultLit, not as its back. */
        const Check& TwoSided = Checks[7];
        Expect(TwoSided.bOk && DefaultLit.bOk && std::abs(TwoSided.Rgb[0] - DefaultLit.Rgb[0]) <= 4
               && std::abs(TwoSided.Rgb[2] - DefaultLit.Rgb[2]) <= 4,
               "Two Sided lights a face toward the camera as its front, as a one-sided material does");

        /* Mesh previews: the engine's primitives, as the mesh viewer and the
         * material editor draw them. */
        const auto Preview = [&](Renderer::MeshPreview InPreview, EngineCore::Image& OutPixels, std::string& OutError)
        {
            InPreview.Width  = 96;
            InPreview.Height = 64;
            const uint64_t Request = Rend.RequestMeshPreview(InPreview);
            SettlePreviews();
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, OutError) && OutPixels.GetWidth() == 96;
        };
        const auto Pixel = [](const EngineCore::Image& InPixels, const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };
        {
            Renderer::MeshPreview Sphere;
            Sphere.Mesh = "Engine/Content/Meshes/Sphere.nmmesh";
            EngineCore::Image Pixels;
            std::string Error;
            const bool bOk = Preview(Sphere, Pixels, Error);
            int Centre[3] = {}, Corner[3] = {};
            if(bOk) { Pixel(Pixels, 48, 32, Centre); Pixel(Pixels, 2, 2, Corner); }
            std::printf("    Sphere centre = %d %d %d, corner = %d %d %d%s\n", Centre[0], Centre[1], Centre[2],
                        Corner[0], Corner[1], Corner[2], bOk ? "" : (" -- " + Error).c_str());
            Expect(bOk && Is(Corner, 38, 40, 46) && !Is(Centre, 38, 40, 46) && Centre[0] > 60 && std::abs(Centre[0] - Centre[2]) < 24,
                   "a mesh preview frames the Sphere, its own material (DefaultLit) lit grey on the clear colour");
        }
        {
            Renderer::MeshPreview Box;
            Box.Mesh     = "Engine/Content/Meshes/Box.nmmesh";
            Box.Material = "Content/Materials/Textured.material.nmgraph";
            Box.Yaw      = 0.0f;
            Box.Pitch    = 0.0f;
            EngineCore::Image Pixels;
            std::string Error;
            int Centre[3] = {};
            const bool bOk = Preview(Box, Pixels, Error);
            if(bOk) { Pixel(Pixels, 48, 32, Centre); }
            Expect(bOk && (Is(Centre, 255, 0, 0) || Is(Centre, 0, 255, 0) || Is(Centre, 0, 0, 255) || Is(Centre, 255, 255, 255)),
                   "a preview's Material goes on every section: the Box's face shows Quadrants");

            Box.Material.clear();
            Box.Sections = { "Content/Materials/Textured.material.nmgraph" };
            int Slotted[3] = {};
            const bool bSlotted = Preview(Box, Pixels, Error);
            if(bSlotted) { Pixel(Pixels, 48, 32, Slotted); }
            Expect(bSlotted && Slotted[0] == Centre[0] && Slotted[1] == Centre[1] && Slotted[2] == Centre[2],
                   "or a section's own slot does");
        }
        {
            Renderer::MeshPreview Missing;
            Missing.Mesh = "Engine/Content/Meshes/Nope.nmmesh";
            EngineCore::Image Pixels;
            std::string Error;
            Expect(!Preview(Missing, Pixels, Error) && Error.find("could not be loaded") != std::string::npos,
                   "a preview of a missing mesh says so");
            Renderer::MeshPreview Nothing;
            Expect(Rend.RequestMeshPreview(Nothing) == 0, "a mesh preview needs a mesh");
        }

        Rend.UnbindWorld();
        Rend.SetViewOverride(nullptr);
    }
    Rend.Shutdown();
    SetFileProvider(PreviousProvider);
}
#endif

/* A two-triangle square in two sections -- small enough to check by hand. */
static MeshData MakeSmokeMesh()
{
    MeshData Mesh;
    Mesh.Positions = { 0, 0, 0,   10, 0, 0,   10, -10, 0,   0, -10, 5 };
    Mesh.Normals   = { 0, 0, -1,  0, 0, -1,   0, 0, -1,     0, 0, -1 };
    Mesh.UV0       = { 0, 0,      1, 0,       1, 1,         0, 1 };
    Mesh.Indices   = { 0, 1, 2,   0, 2, 3 };
    Mesh.Sections  = { MeshSection{ "Front", "Glow", 0, 3 }, MeshSection{ "Back", "", 3, 3 } };
    Mesh.ImportInfo.SourcePath = "Source/Square.glb";
    Mesh.ImportInfo.SourceTime = 1234;
    Mesh.ImportInfo.Settings   = "Scale=1\n";
    ComputeMeshBounds(Mesh);
    return Mesh;
}

static bool LoadMeshFrom(const Archive& InSaved, MeshData& OutMesh)
{
    Archive Reader = Archive::ForReading(InSaved.GetBytes());
    return SerializeMesh(Reader, OutMesh);
}

#if WITH_EDITOR
/*
 * The UI on the GPU (NIMBLE_SMOKE_GPU=Vulkan): a list over an empty scene,
 * read back. Proves the pass's orientation -- the list's top-left is the
 * image's -- and its scissor: a rect clipped to a smaller one fills only that.
 */
static void TestUIOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[ui on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[ui on the GPU: %s]\n", GetShaderPlatformName(Backend));

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
        Rend.SetViewOverride(&View);
        Rend.SetClearColor(EngineCore::Color(0, 0, 0, 255));
        GameCore::World World;
        Rend.BindWorld(World);

        const uint32_t Red   = PackUIColor(1.0f, 0.0f, 0.0f, 1.0f);
        const uint32_t Green = PackUIColor(0.0f, 1.0f, 0.0f, 1.0f);
        UIDrawList List;
        List.AddRect({ 0, 0, 16, 16 }, Green);           // the top-left corner
        List.PushClip({ 100, 120, 40, 20 });
        List.AddRect({ 80, 100, 100, 100 }, Red);         // only the clip shows
        List.PopClip();
        List.AddRect({ 200, 200, 30, 30 }, PackUIColor(0.0f, 0.0f, 1.0f, 0.5f));   // half over black
        Rend.SubmitUIDrawList(RENDER_DEFAULT_SCENE, List);

        uint64_t Frame = 1;
        EngineCore::Image Pixels;
        const auto Capture = [&]
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            const uint64_t Request = Rend.RequestSceneCapture();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, Pixels, Error) && Pixels.GetWidth() == 256;
        };
        const auto Rgb = [&Pixels](const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = Pixels.GetData() + Pixels.GetStride() * InY + InX * BytesPerPixel(Pixels.GetFormat());
            const bool bBgra = Pixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };

        const bool bCaptured = Capture();
        Expect(bCaptured, "the UI frame reads back");
        if(bCaptured)
        {
            int C[3];
            Rgb(4, 4, C);
            Expect(C[1] > 200 && C[0] < 40, "the list's top-left is the image's top-left (green)");
            Rgb(4, 250, C);
            Expect(C[1] < 40, "the bottom-left is not green");
            Rgb(120, 130, C);
            Expect(C[0] > 200 && C[1] < 40, "inside the clip, the red rect shows");
            Rgb(90, 110, C);
            Expect(C[0] < 40, "inside the red rect but outside the clip, nothing (scissor, top edge)");
            Rgb(150, 130, C);
            Expect(C[0] < 40, "right of the clip, nothing (scissor, right edge)");
            Rgb(120, 150, C);
            Expect(C[0] < 40, "below the clip, nothing (scissor, bottom edge)");
            Rgb(215, 215, C);
            Expect(C[2] > 90 && C[2] < 170 && C[0] < 40, "a half-transparent rect blends over the scene");
        }

        /* Unchanged list: drawn again without a resubmit. Then cleared. */
        const bool bAgain = Capture();
        int Again[3] = {};
        if(bAgain) { Rgb(4, 4, Again); }
        Expect(bAgain && Again[1] > 200, "a submitted list keeps drawing every frame");
        Rend.SubmitUIDrawList(RENDER_DEFAULT_SCENE, UIDrawList());
        const bool bCleared = Capture();
        int Cleared[3] = {};
        if(bCleared) { Rgb(4, 4, Cleared); }
        Expect(bCleared && Cleared[1] < 40, "an empty list clears the UI");

        Rend.UnbindWorld();
    }
    Rend.Shutdown();
    SetFileProvider(PreviousProvider);
}

static void TestMeshAssets()
{
    std::printf("[mesh assets]\n");

    MeshData Mesh = MakeSmokeMesh();
    Expect(Mesh.BoundsMin[0] == 0.0f && Mesh.BoundsMin[1] == -10.0f && Mesh.BoundsMax[0] == 10.0f && Mesh.BoundsMax[2] == 5.0f,
           "ComputeMeshBounds spans every position");

    Archive Writer = Archive::ForWriting();
    Expect(SerializeMesh(Writer, Mesh), "a well-formed mesh saves");

    MeshData Loaded;
    Expect(LoadMeshFrom(Writer, Loaded), "and loads back");
    Expect(Loaded.Positions == Mesh.Positions && Loaded.Normals == Mesh.Normals && Loaded.UV0 == Mesh.UV0
           && Loaded.Indices == Mesh.Indices, "with the same vertices and indices");
    Expect(Loaded.Sections.size() == 2 && Loaded.Sections[0].Name == "Front" && Loaded.Sections[0].Material == "Glow"
           && Loaded.Sections[1].FirstIndex == 3 && Loaded.Sections[1].IndexCount == 3, "the same sections");
    Expect(Loaded.BoundsMax[2] == 5.0f && Loaded.ImportInfo.SourcePath == "Source/Square.glb"
           && Loaded.ImportInfo.SourceTime == 1234 && Loaded.ImportInfo.Settings == "Scale=1\n",
           "the same bounds and import source");

    MeshData NoSource = MakeSmokeMesh();
    NoSource.ImportInfo = AssetImportInfo();
    Archive NoSourceWriter = Archive::ForWriting();
    MeshData NoSourceLoaded;
    Expect(SerializeMesh(NoSourceWriter, NoSource) && LoadMeshFrom(NoSourceWriter, NoSourceLoaded)
           && NoSourceLoaded.ImportInfo.IsEmpty(), "the import source is optional");

    MeshData BadIndex = MakeSmokeMesh();
    BadIndex.Indices[4] = 9;
    Archive BadIndexWriter = Archive::ForWriting();
    SerializeMesh(BadIndexWriter, BadIndex);
    MeshData BadIndexLoaded;
    Expect(!LoadMeshFrom(BadIndexWriter, BadIndexLoaded), "an index past the last vertex is refused on load");

    MeshData BadSection = MakeSmokeMesh();
    BadSection.Sections[1].IndexCount = 6;
    Archive BadSectionWriter = Archive::ForWriting();
    SerializeMesh(BadSectionWriter, BadSection);
    MeshData BadSectionLoaded;
    Expect(!LoadMeshFrom(BadSectionWriter, BadSectionLoaded), "a section past the last index is refused on load");

    MeshData NoNormals = MakeSmokeMesh();
    NoNormals.Normals.pop_back();
    Archive NoNormalsWriter = Archive::ForWriting();
    Expect(!SerializeMesh(NoNormalsWriter, NoNormals), "a mesh missing a normal does not save");

    std::vector<uint8_t> Truncated = Writer.GetBytes();
    Truncated.resize(Truncated.size() / 2);
    Archive TruncatedReader = Archive::ForReading(Truncated);
    MeshData TruncatedLoaded;
    Expect(!SerializeMesh(TruncatedReader, TruncatedLoaded), "a truncated mesh fails to load");

    TextureData Texture;
    Texture.Pixels = Image(2, 2, EImageFormat::RGBA8);
    Archive TextureWriter = Archive::ForWriting();
    SerializeTexture(TextureWriter, Texture);
    Archive WrongTag = Archive::ForReading(TextureWriter.GetBytes());
    MeshData WrongTagLoaded;
    Expect(!SerializeMesh(WrongTag, WrongTagLoaded), "a texture is not a mesh");

    /* Through files and the cache, as a RenderComponent finds one. */
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreMeshes";
    std::error_code Ignored;
    std::filesystem::remove_all(Root, Ignored);
    FilesystemProvider Provider(Root.generic_string());
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Provider);

    MeshData ToSave = MakeSmokeMesh();
    Expect(SaveMeshFile((Root / "Content" / "Square.nmmesh").generic_string(), ToSave), "SaveMeshFile writes, making folders");

    MeshHeader Header;
    Expect(LoadMeshHeader("Content/Square.nmmesh", Header) && Header.VertexCount == 4 && Header.IndexCount == 6
           && Header.SectionCount == 2 && Header.BoundsMin[1] == -10.0f, "LoadMeshHeader reads counts and bounds");

    MeshAssetCache::Invalidate(std::string());
    const std::shared_ptr<const MeshData> First = MeshAssetCache::Find("Content/Square.nmmesh");
    Expect(First && First->GetTriangleCount() == 2, "MeshAssetCache::Find loads through the file provider");
    Expect(MeshAssetCache::Find("Content/Square.nmmesh") == First, "and keeps what it loaded");
    Expect(!MeshAssetCache::Find("Content/Missing.nmmesh"), "a missing mesh is null");
    MeshAssetCache::Invalidate("Content/Square.nmmesh");
    const std::shared_ptr<const MeshData> Second = MeshAssetCache::Find("Content/Square.nmmesh");
    Expect(Second && Second != First, "Invalidate makes the next Find load afresh");
    MeshAssetCache::Invalidate(std::string());

    SetFileProvider(PreviousProvider);
    std::filesystem::remove_all(Root, Ignored);
}
#endif // WITH_EDITOR

static void TestTextureAssets()
{
    std::printf("[texture assets]\n");

    TextureData Texture;
    Texture.Pixels = Image(3, 2, EImageFormat::RGBA8);
    for(size_t At = 0; At < Texture.Pixels.GetSizeBytes(); ++At) { Texture.Pixels.GetData()[At] = uint8_t(At * 7); }
    Texture.bSRGB  = false;
    Texture.Filter = ETextureFilter::Nearest;
    Texture.Wrap   = ETextureWrap::Mirror;
    Texture.ImportInfo.SourcePath = "Source/Bricks.png";

    Archive Writer = Archive::ForWriting();
    Expect(SerializeTexture(Writer, Texture), "an RGBA8 texture saves");

    TextureData Loaded;
    Archive Reader = Archive::ForReading(Writer.GetBytes());
    Expect(SerializeTexture(Reader, Loaded), "and loads back");
    Expect(Loaded.Pixels.GetWidth() == 3 && Loaded.Pixels.GetHeight() == 2
           && std::memcmp(Loaded.Pixels.GetData(), Texture.Pixels.GetData(), Texture.Pixels.GetSizeBytes()) == 0,
           "with the same pixels");
    Expect(!Loaded.bSRGB && Loaded.Filter == ETextureFilter::Nearest && Loaded.Wrap == ETextureWrap::Mirror
           && Loaded.ImportInfo.SourcePath == "Source/Bricks.png", "the same sampling and import source");

    TextureData Gray;
    Gray.Pixels = Image(2, 2, EImageFormat::Gray8);
    Archive GrayWriter = Archive::ForWriting();
    Expect(!SerializeTexture(GrayWriter, Gray), "a non-RGBA8 texture does not save");

    std::vector<uint8_t> Truncated = Writer.GetBytes();
    Truncated.resize(Truncated.size() - 8);
    Archive TruncatedReader = Archive::ForReading(Truncated);
    TextureData TruncatedLoaded;
    Expect(!SerializeTexture(TruncatedReader, TruncatedLoaded), "a truncated texture fails to load");

    MeshData Mesh = MakeSmokeMesh();
    Archive MeshWriter = Archive::ForWriting();
    SerializeMesh(MeshWriter, Mesh);
    Archive WrongTag = Archive::ForReading(MeshWriter.GetBytes());
    TextureData WrongTagLoaded;
    Expect(!SerializeTexture(WrongTag, WrongTagLoaded), "a mesh is not a texture");
}

/* Mip chains (TextureAsset.h): built, saved, read, and what a v1 file reads as. */
/*
 * TEXTURE STREAMING (TEXTURE_STREAMING.md), its files: a texture's levels
 * found by their chunk headers and read by range -- RGBA8 and blocks -- and
 * the range reads beneath, including a scrambled pack's.
 */
static TextureData MakeStreamingTexture(const int InSize)
{
    TextureData Texture;
    Texture.MipGen = ETextureMipGen::Box;
    Texture.Pixels = Image(InSize, InSize, EImageFormat::RGBA8);
    for(int Y = 0; Y < InSize; ++Y)
    {
        uint8_t* Row = Texture.Pixels.GetScanline(Y);
        for(int X = 0; X < InSize; ++X)
        {
            Row[X * 4 + 0] = uint8_t(X * 7 + Y);
            Row[X * 4 + 1] = uint8_t(Y * 3);
            Row[X * 4 + 2] = uint8_t((X ^ Y) * 5);
            Row[X * 4 + 3] = 255;
        }
    }
    BuildTextureMips(Texture);
    return Texture;
}

static bool SameImage(const Image& InA, const Image& InB)
{
    return InA.GetWidth() == InB.GetWidth() && InA.GetHeight() == InB.GetHeight() && InA.GetSizeBytes() == InB.GetSizeBytes()
        && std::memcmp(InA.GetData(), InB.GetData(), InA.GetSizeBytes()) == 0;
}

static void TestTextureStreamingFiles()
{
    std::printf("[texture streaming files]\n");
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreStreaming";
    std::filesystem::create_directories(Root);
    const std::string File = (Root / "Levels.nmtexture").string();

#if WITH_EDITOR
    TextureData Source = MakeStreamingTexture(256);
    Expect(SaveTextureFile(File, Source), "a 256 x 256 texture with its mips saves");

    TextureFileLayout Layout;
    Expect(ReadTextureLayout(File, Layout) && Layout.GetMipCount() == 9 && !Layout.bBlocks
           && Layout.LevelBytes[0] == 256u * 256u * 4u && Layout.LevelBytes[8] == 4u,
           "its layout: nine RGBA8 levels, level 0 first");

    TextureData Tail;
    Expect(LoadTextureLevels(File, Layout, 2, -1, Tail) && Tail.GetMipCount() == 7 && SameImage(Tail.Pixels, Source.Mips[1])
           && SameImage(Tail.Mips.back(), Source.Mips.back()) && Tail.MipGen == ETextureMipGen::Box,
           "levels 2 on read by range are the file's: 64 x 64 down to 1 x 1");
    TextureData Middle;
    Expect(LoadTextureLevels(File, Layout, 0, 2, Middle) && Middle.GetMipCount() == 2 && SameImage(Middle.Pixels, Source.Pixels)
           && SameImage(Middle.Mips[0], Source.Mips[0]),
           "levels 0 and 1 read across TMIP's chunk header");
    Expect(!LoadTextureLevels(File, Layout, 3, 3, Middle), "an empty range is refused");

    if(IsTextureCompressionAvailable(ETextureCompression::BC1))
    {
        TextureData Blocky = Source;
        Blocky.Compression = ETextureCompression::BC1;
        TextureData ForGpu;
        Expect(SaveTextureFile(File, Blocky) && LoadTextureFile(File, ForGpu, ETextureLoad::ForGpu)
               && ReadTextureLayout(File, Layout) && Layout.bBlocks && Layout.GetMipCount() == 9,
               "a BC1 file's layout is its blocks (past its import info)");
        TextureData Blocks;
        Expect(LoadTextureLevels(File, Layout, 3, -1, Blocks) && Blocks.Blocks.size() == 6 && Blocks.Pixels.IsNull()
               && Blocks.Blocks[0].Width == 32 && Blocks.Blocks[0].Bytes == ForGpu.Blocks[3].Bytes
               && Blocks.Blocks[5].Bytes == ForGpu.Blocks[8].Bytes && Blocks.Compression == ETextureCompression::BC1,
               "and its blocks from level 3 on read as LoadTextureFile's");
    }
#endif

    /* A material's TextureUV lines: the smallest scale a texture's samples apply to UV0. */
    MaterialDescription Scaled;
    std::string ParseError;
    const bool bParsed = ParseMaterialDescription("NimbleMaterial 3\nBlockSize 208\n"
                                                  "Param UVScale vec2 192 0.5 0.25 0 0\n"
                                                  "Texture 1 Default Default A.nmtexture\n"
                                                  "Texture 2 Default Default B.nmtexture\n"
                                                  "Texture 3 Default Default C.nmtexture\n"
                                                  "TextureUV 1 1 UVScale\nTextureUV 1 4\nTextureUV 3 2 Missing\n", Scaled, ParseError);
    Expect(bParsed && Scaled.Textures.size() == 3 && Scaled.Textures[0].StreamUVScale == 0.5f
           && Scaled.Textures[1].StreamUVScale == 1.0f && Scaled.Textures[2].StreamUVScale == 1.0f,
           "TextureUV: a parameter's largest component, the smallest of a slot's samples; 1 without, or with an unknown parameter");

    /* Range reads: the default (whole then slice), a scrambled pack's, the filesystem's. */
    std::vector<uint8_t> Bytes(5000);
    for(size_t Index = 0; Index < Bytes.size(); ++Index) { Bytes[Index] = uint8_t(Index * 31 + 7); }
    const std::vector<uint8_t> Want(Bytes.begin() + 13, Bytes.begin() + 13 + 1001);
    std::vector<uint8_t> Got;

    std::map<std::string, MemoryFileProvider::FileBytes> Files;
    Files["A.bin"] = std::make_shared<const std::vector<uint8_t>>(Bytes);
    MemoryFileProvider Memory(Files);
    Expect(Memory.ReadRange("A.bin", 13, 1001, Got) && Got == Want && !Memory.ReadRange("A.bin", 4500, 501, Got),
           "a provider's default range read: the slice, and nothing past the end");

    const std::string PackFile = (Root / "Range.nmpak").string();
    std::string Error;
    Expect(WritePackFile(PackFile, "Range", { PackInput{ "Pad.bin", std::vector<uint8_t>(37, 1) }, PackInput{ "A.bin", Bytes } },
                         false, true, 0, &Error), "a scrambled pack writes");
    std::vector<uint8_t> PackBytes;
    std::shared_ptr<PackFileProvider> Pack = appFileReadAll(PackFile, PackBytes) ? PackFileProvider::Load(std::move(PackBytes), Error) : nullptr;
    std::vector<uint8_t> All;
    Expect(Pack && Pack->IsScrambled() && Pack->ReadAll("A.bin", All) && All == Bytes && Pack->ReadRange("A.bin", 13, 1001, Got) && Got == Want,
           "a scrambled pack's range unscrambles from an unaligned offset");

    const std::string Plain = (Root / "A.bin").string();
    Archive Writer = Archive::ForWriting();
    Writer.SerializeBytes(Bytes.data(), Bytes.size());
    Expect(SaveArchiveToFile(Plain, Writer) && appFileReadRange(Plain, 13, 1001, Got) && Got == Want
           && !appFileReadRange(Plain, 4999, 2, Got) && Got.empty(),
           "the filesystem's range read seeks, and fails short");
    std::filesystem::remove_all(Root);
}

/*
 * TEXTURE STREAMING, the streamer: a texture starts with its 64-texel tail,
 * loads to the level its demand asks for, drops back when asked for less,
 * and the pool raises every wanted level when they do not fit.
 */
static void TestTextureStreamer()
{
    std::printf("[texture streamer]\n");
#if WITH_EDITOR
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreStreamer";
    std::filesystem::create_directories(Root);
    const std::string File  = (Root / "Big.nmtexture").string();
    const std::string Small = (Root / "Small.nmtexture").string();
    TextureData Source = MakeStreamingTexture(1024);
    TextureData Tiny   = MakeStreamingTexture(64);
    Expect(SaveTextureFile(File, Source) && SaveTextureFile(Small, Tiny), "a 1024 and a 64 texture save");

    ConfigVar* MinResident = ConfigFindVar("Rendering", "TextureStreamingMinResident");
    ConfigVar* DropDelay   = ConfigFindVar("Rendering", "TextureStreamingDropDelay");
    ConfigVar* Pool        = ConfigFindVar("Rendering", "TextureStreamingPoolMB");
    ConfigVar* Enabled     = ConfigFindVar("Rendering", "TextureStreaming");
    Expect(MinResident && DropDelay && Pool && Enabled, "its settings exist");
    if(!MinResident || !DropDelay || !Pool || !Enabled) { return; }
    MinResident->SetInt(64);
    DropDelay->SetFloat(0.0f);
    Pool->SetInt(512);
    Enabled->SetBool(true);

    {
        TextureStreamer Streamer;
        Expect(Streamer.FindEntry(Small) == nullptr, "a texture no larger than MinResident is not streamed");
        TextureStreamer::Entry* Entry = Streamer.FindEntry(File);
        Expect(Entry && TextureStreamer::GetResidentTop(*Entry) == 4 && TextureStreamer::GetResident(*Entry)->Pixels.GetWidth() == 64
               && TextureStreamer::GetResident(*Entry)->GetMipCount() == 7,
               "a 1024 texture starts with levels 4 on: 64 x 64 down");
        if(Entry)
        {
            /* 1024 pixels a UV unit: level 0. */
            TextureStreamer::NoteDemand(*Entry, 1024.0f);
            Streamer.Update();
            Streamer.Flush();
            const std::shared_ptr<const TextureData> Full = TextureStreamer::GetResident(*Entry);
            Expect(TextureStreamer::GetResidentTop(*Entry) == 0 && Full->GetMipCount() == 11 && SameImage(Full->Pixels, Source.Pixels)
                   && SameImage(Full->Mips[3], Source.Mips[3]) && SameImage(Full->Mips.back(), Source.Mips.back()),
                   "asked for at 1024 pixels a UV unit, it loads to level 0: the file's levels, joined to the tail");
            const TextureStreamingStats Stats = Streamer.GetStats();
            Expect(Stats.Textures == 1 && Stats.ResidentBytes == Stats.FullBytes && Stats.Loads >= 1 && Stats.BytesRead > 0,
                   "the stats count it whole, and the load");

            /* 256 pixels: level 2 (1024 / 256 = 4 texels a pixel). DropDelay 0: dropped at once. */
            TextureStreamer::NoteDemand(*Entry, 256.0f);
            Streamer.Update();
            Expect(TextureStreamer::GetWanted(*Entry) == 2 && TextureStreamer::GetResidentTop(*Entry) == 2
                   && TextureStreamer::GetResident(*Entry)->Pixels.GetWidth() == 256
                   && SameImage(TextureStreamer::GetResident(*Entry)->Pixels, Source.Mips[1]),
                   "asked for at 256 pixels, it drops to level 2, without a read");

            TextureStreamer::NoteDemand(*Entry, 700.0f);   // 1.46 texels a pixel: still level 0
            Streamer.Update();
            Streamer.Flush();
            Expect(TextureStreamer::GetResidentTop(*Entry) == 0, "between two levels, the sharper one");

            /* A 1 MB pool cannot hold 1024 (5.3 MB) or 512 (1.3 MB): 256 on, bias 2. */
            Pool->SetInt(1);
            TextureStreamer::NoteDemand(*Entry, 0.0f);   // not known: all of it
            Streamer.Update();
            Expect(TextureStreamer::GetWanted(*Entry) == 2 && Streamer.GetStats().BudgetBias == 2
                   && TextureStreamer::GetResidentTop(*Entry) == 2,
                   "over the pool, every wanted level is raised alike, and what is over is dropped at once");
            Pool->SetInt(512);

            /* Not drawn past DropDelay: back to its tail. */
            Streamer.Update();
            Expect(TextureStreamer::GetWanted(*Entry) == 4 && TextureStreamer::GetResidentTop(*Entry) == 4,
                   "not drawn for DropDelay, it keeps only its tail");

            /* A reimport: read again, from its tail. */
            TextureStreamer::NoteDemand(*Entry, 1024.0f);
            Streamer.Update();
            Streamer.Flush();
            TextureAssetCache::Invalidate(File);
            Streamer.Update();
            Expect(TextureStreamer::GetResidentTop(*Entry) == 4, "an invalidated texture starts again from its tail");
        }
        Enabled->SetBool(false);
        Expect(Streamer.FindEntry(File) == nullptr, "with streaming off, nothing is streamed");
        Enabled->SetBool(true);
    }
    MinResident->ResetToDefault();
    DropDelay->ResetToDefault();
    Pool->ResetToDefault();
    Enabled->ResetToDefault();
    std::filesystem::remove_all(Root);
#endif
}

/*
 * TEXTURE MEMORY: Core's BucketMemoryPool (blocks kept and reused, capped),
 * PooledBytes (a vector's subset over a pool), Image in a pool, and texture
 * files loading into RenderCore's "Textures" pool.
 */
static void TestTextureMemory()
{
    std::printf("[texture memory]\n");

    BucketMemoryPool Bucket("SmokeBuckets", 4096, 1 << 20, 3 << 20);
    const PoolID Id = RegisterMemoryPool(&Bucket);
    Expect(Id != POOL_Invalid, "a bucket pool registers");

    {
        PooledBytes A(Id);
        A.resize(100000);
        const uint8_t* First = A.data();
        bool bZero = true;
        for(size_t Index = 0; Index < A.size(); ++Index) { bZero = bZero && A[Index] == 0; }
        Expect(A.size() == 100000 && bZero && A.GetPool() == Id, "PooledBytes resizes in its pool, zeroed");
        A.clear();
        Expect(Bucket.GetCachedBytes() > 0, "a freed block is kept");
        PooledBytes B(Id);
        B.resize(99000);   // the same class: 96K < 99000 + header <= 128K
        Expect(B.data() == First && Bucket.GetCachedBytes() == 0, "and handed out again for the next of its class");

        PooledBytes C = B;
        C[5] = 7;
        Expect(C.GetPool() == Id && C.size() == B.size() && C != B && C.data() != B.data(), "a copy is its own, in the same pool");
        PooledBytes D = std::move(C);
        Expect(D.size() == 99000 && C.empty() && D[5] == 7, "a move takes the block");
        D = { 1, 2, 3 };
        D.pop_back();
        Expect(D.size() == 2 && D[1] == 2 && D == PooledBytes{ 1, 2 }, "assign, pop_back and equality as a vector's");
    }

    /* The cap: 3 MB kept; four 1 MB-class blocks freed keep at most that. */
    {
        std::vector<PooledBytes> Many(4, PooledBytes(Id));
        for(PooledBytes& Each : Many) { Each.resize(1000000); }
    }
    Expect(Bucket.GetCachedBytes() <= (size_t(3) << 20) && Bucket.GetCachedBytes() > 0, "the cache keeps no more than its cap");
    Bucket.SetMaxCachedBytes(0);
    MemoryPoolStats Stats;
    Bucket.GetStats(Stats);
    Expect(Bucket.GetCachedBytes() == 0 && Stats.LiveAllocations == 0 && Stats.BytesInUse == 0,
           "a zero cap trims everything; nothing is live");
    {
        PooledBytes Huge(Id);
        Huge.resize(size_t(4) << 20);   // past the largest class: the system both ways
        Expect(Huge.size() == (size_t(4) << 20), "a request past the classes still allocates");
    }
    Bucket.GetStats(Stats);
    Expect(Stats.LiveAllocations == 0 && Bucket.GetCachedBytes() == 0, "and is freed, not kept");

    {
        const Image Pooled(16, 8, EImageFormat::RGBA8, Id);
        const Image Converted = Pooled.ConvertedTo(EImageFormat::BGRA8);
        const Image Copied = Pooled;
        Expect(Pooled.GetPool() == Id && Converted.GetPool() == Id && Copied.GetPool() == Id && Pooled.GetSizeBytes() == 16 * 8 * 4,
               "an Image in a pool, and its copies and conversions with it");
    }
    Bucket.GetStats(Stats);
    Expect(Stats.LiveAllocations == 0, "every allocation came back");
    UnregisterMemoryPool(Id);

#if WITH_EDITOR
    /* A texture file loads into the "Textures" pool. */
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreTextureMemory";
    std::filesystem::create_directories(Root);
    const std::string File = (Root / "Pooled.nmtexture").string();
    TextureData Source = MakeStreamingTexture(128);
    Expect(SaveTextureFile(File, Source), "a texture saves");
    MemoryPoolStats Before, After;
    Expect(GetTextureMemory() != nullptr && FindMemoryPool("Textures") == GetTextureMemoryPool(), "RenderCore registers its Textures pool");
    GetTextureMemory()->GetStats(Before);
    {
        TextureData Loaded;
        Expect(LoadTextureFile(File, Loaded) && Loaded.Pixels.GetPool() == GetTextureMemoryPool()
               && Loaded.Mips.back().GetPool() == GetTextureMemoryPool(), "its levels load into it");
        GetTextureMemory()->GetStats(After);
        Expect(After.BytesInUse >= Before.BytesInUse + TextureGpuBytes(Loaded), "and count there");
    }
    GetTextureMemory()->GetStats(After);
    Expect(After.BytesInUse == Before.BytesInUse, "and go back when it is gone");
    std::filesystem::remove_all(Root);
#endif
}

static void TestTextureMips()
{
    std::printf("[texture mips]\n");

    Expect(FullMipCount(1024, 1024) == 11 && FullMipCount(8, 2) == 4 && FullMipCount(1, 1) == 1 && FullMipCount(5, 3) == 3,
           "FullMipCount: down to 1x1, each axis halved (at least 1)");

    /* A 4x4 checker of black and white: every 2x2 averages to grey. */
    TextureData Checker;
    Checker.Pixels = Image(4, 4, EImageFormat::RGBA8);
    for(int Y = 0; Y < 4; ++Y)
    {
        for(int X = 0; X < 4; ++X)
        {
            uint8_t* Px = Checker.Pixels.GetScanline(Y) + X * 4;
            Px[0] = Px[1] = Px[2] = ((X + Y) & 1) ? 255 : 0;
            Px[3] = 255;
        }
    }
    Checker.bSRGB  = false;
    Checker.MipGen = ETextureMipGen::Box;
    BuildTextureMips(Checker);
    Expect(Checker.GetMipCount() == 3 && Checker.Mips[0].GetWidth() == 2 && Checker.Mips[1].GetWidth() == 1,
           "a 4x4 texture gets 2x2 and 1x1 levels");
    Expect(std::abs(int(Checker.Mips[0].GetData()[0]) - 128) <= 1 && std::abs(int(Checker.Mips[1].GetData()[0]) - 128) <= 1,
           "Box averages a linear texture's 2x2 blocks: grey");
    Checker.bSRGB = true;
    BuildTextureMips(Checker);
    Expect(std::abs(int(Checker.Mips[0].GetData()[0]) - 188) <= 1, "and an sRGB texture's in linear light: half the light is 188, not 128");
    Expect(Checker.Mips[0].GetData()[3] == 255, "alpha is averaged linearly either way");

    Checker.MipGen = ETextureMipGen::Kaiser;
    BuildTextureMips(Checker);
    Expect(Checker.GetMipCount() == 3 && std::abs(int(Checker.Mips[0].GetData()[0]) - 188) <= 3,
           "Kaiser keeps a checker's mean (its lobes cancel on a repeating pattern)");

    /* A flat colour stays itself through every filter and wrap. */
    TextureData Flat;
    Flat.Pixels = Image(8, 2, EImageFormat::RGBA8);
    for(size_t At = 0; At < Flat.Pixels.GetSizeBytes(); At += 4)
    {
        uint8_t* Px = Flat.Pixels.GetData() + At;
        Px[0] = 200; Px[1] = 90; Px[2] = 30; Px[3] = 140;
    }
    bool bFlatKept = true;
    for(const ETextureMipGen Gen : { ETextureMipGen::Box, ETextureMipGen::Kaiser })
    {
        for(const ETextureWrap Wrap : { ETextureWrap::Repeat, ETextureWrap::Clamp, ETextureWrap::Mirror })
        {
            Flat.MipGen = Gen;
            Flat.Wrap   = Wrap;
            BuildTextureMips(Flat);
            bFlatKept = bFlatKept && Flat.GetMipCount() == 4 && Flat.Mips.back().GetWidth() == 1 && Flat.Mips.back().GetHeight() == 1;
            for(const Image& Level : Flat.Mips)
            {
                const uint8_t* Px = Level.GetData();
                bFlatKept = bFlatKept && std::abs(Px[0] - 200) <= 1 && std::abs(Px[1] - 90) <= 1 && std::abs(Px[2] - 30) <= 1
                            && std::abs(Px[3] - 140) <= 1;
            }
        }
    }
    Expect(bFlatKept, "a flat colour is unchanged at every level, by Box and Kaiser, with every wrap; 8x2 goes 4x1, 2x1, 1x1");
    Flat.MipGen = ETextureMipGen::None;
    BuildTextureMips(Flat);
    Expect(Flat.Mips.empty() && Flat.GetMipCount() == 1, "None clears the chain");

    /* Saved and read back: the chain, the settings, the header. */
    Checker.MipGen      = ETextureMipGen::Box;
    Checker.Compression = ETextureCompression::BC7;
    BuildTextureMips(Checker);
    Archive Writer = Archive::ForWriting();
    Expect(SerializeTexture(Writer, Checker), "a texture with mips saves");
    TextureData Loaded;
    Archive Reader = Archive::ForReading(Writer.GetBytes());
    Expect(SerializeTexture(Reader, Loaded) && Loaded.GetMipCount() == 3 && Loaded.MipGen == ETextureMipGen::Box
           && Loaded.Compression == ETextureCompression::BC7, "and loads with its chain, mip generation and compression");
    Expect(Loaded.Mips.size() == 2 && std::memcmp(Loaded.Mips[0].GetData(), Checker.Mips[0].GetData(), Checker.Mips[0].GetSizeBytes()) == 0
           && std::memcmp(Loaded.Mips[1].GetData(), Checker.Mips[1].GetData(), 4) == 0, "each level's texels as saved");

    TextureData Wrong = Checker;
    Wrong.Mips[0] = Image(3, 2, EImageFormat::RGBA8);
    Archive WrongWriter = Archive::ForWriting();
    Expect(!SerializeTexture(WrongWriter, Wrong), "a level that is not half the last does not save");

    std::vector<uint8_t> Truncated = Writer.GetBytes();
    Truncated.resize(Truncated.size() - 3);
    Archive TruncatedReader = Archive::ForReading(Truncated);
    TextureData TruncatedLoaded;
    Expect(!SerializeTexture(TruncatedReader, TruncatedLoaded), "a truncated chain fails to load");

    /* A v1 file (before mips): THDR v1, no TMIP. Written by hand. */
    {
        Archive Old = Archive::ForWriting();
        uint16_t Version = 1;
        Old.OpenChunk(MakeArchiveTag('N','T','E','X'), Version);
        Version = 1;
        Old.OpenChunk(MakeArchiveTag('T','H','D','R'), Version);
        int32_t Width = 2, Height = 1;
        uint8_t Format = 0, Filter = 1, Wrap = 1;
        bool    bSRGB  = false;
        Old << Width << Height << Format << bSRGB << Filter << Wrap;
        Old.CloseChunk();
        Version = 1;
        Old.OpenChunk(MakeArchiveTag('T','P','I','X'), Version);
        uint8_t Texels[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        Old.SerializeBytes(Texels, 8);
        Old.CloseChunk();
        Old.CloseChunk();
        TextureData FromV1;
        Archive OldReader = Archive::ForReading(Old.GetBytes());
        Expect(SerializeTexture(OldReader, FromV1) && FromV1.GetMipCount() == 1 && FromV1.MipGen == ETextureMipGen::None
               && FromV1.Compression == ETextureCompression::None && FromV1.Wrap == ETextureWrap::Clamp
               && FromV1.Pixels.GetData()[7] == 8, "a v1 texture reads as before: no mips, no compression");
    }

    /* Sizes: as stored (RGBA8), and what a block format would take. */
    Expect(TextureGpuBytes(Checker) == uint64_t(16 + 4 + 1) * 4, "TextureGpuBytes counts every level");
    Expect(TextureCompressedBytes(256, 256, 1, ETextureCompression::BC1) == 256 * 256 / 2
           && TextureCompressedBytes(256, 256, 1, ETextureCompression::BC7) == 256 * 256
           && TextureCompressedBytes(4, 4, 3, ETextureCompression::BC3) == 3 * 16
           && TextureCompressedBytes(4, 4, 3, ETextureCompression::None) == (16 + 4 + 1) * 4,
           "TextureCompressedBytes: BC1 and BC4 half a byte a texel, the rest one, a 4x4 block at least per level");

    Expect(std::string(GetTextureCompressionName(ETextureCompression::BC5)) == "BC5"
           && std::string(GetTextureMipGenName(ETextureMipGen::Kaiser)) == "Kaiser", "the settings' names");
}

/*
 * Block compression (TextureCompression.h): the built-in encoder in an editor
 * build, every format encoded and decoded close to its source, the blocks
 * saved beside the pixels and loaded ForGpu without them.
 */
static void TestTextureCompression()
{
    Expect(IsTextureCompressionAvailable(ETextureCompression::None) && TextureBlockBytes(ETextureCompression::BC1) == 8
           && TextureBlockBytes(ETextureCompression::BC4) == 8 && TextureBlockBytes(ETextureCompression::BC7) == 16,
           "None is always honoured; BC1 and BC4 are 8 bytes a block, the rest 16");

    /* Decoding needs no encoder: a solid BC4 block (both endpoints 200) is 200 everywhere, reading (r, 0, 0, 1). */
    TextureBlockLevel Solid;
    Solid.Width  = 3;
    Solid.Height = 2;
    Solid.Bytes  = { 200, 200, 0, 0, 0, 0, 0, 0 };
    Image SolidImage;
    Expect(DecodeTextureBlocks(Solid, ETextureCompression::BC4, SolidImage) && SolidImage.GetWidth() == 3
           && SolidImage.GetHeight() == 2 && SolidImage.GetData()[0] == 200 && SolidImage.GetData()[1] == 0
           && SolidImage.GetData()[3] == 255 && SolidImage.GetScanline(1)[8] == 200,
           "a BC4 block decodes as the GPU reads it, cut to the level's size");
    Solid.Bytes.pop_back();
    Expect(!DecodeTextureBlocks(Solid, ETextureCompression::BC4, SolidImage), "blocks of the wrong size do not decode");

#if WITH_EDITOR
    Expect(GetTextureCompressor() && std::string(GetTextureCompressor()->GetName()) == "bc7enc_rdo"
           && IsTextureCompressionAvailable(ETextureCompression::BC1) && IsTextureCompressionAvailable(ETextureCompression::BC7),
           "an editor build encodes with bc7enc_rdo");

    /* Smooth ramps, 13x9 so the edge blocks are partial, alpha its own ramp. */
    TextureData Source;
    Source.Pixels = Image(13, 9, EImageFormat::RGBA8);
    for(int Y = 0; Y < 9; ++Y)
    {
        uint8_t* Row = Source.Pixels.GetScanline(Y);
        for(int X = 0; X < 13; ++X)
        {
            Row[X * 4 + 0] = uint8_t(X * 18);
            Row[X * 4 + 1] = uint8_t(Y * 28);
            Row[X * 4 + 2] = uint8_t(255 - X * 9 - Y * 9);
            Row[X * 4 + 3] = uint8_t(40 + (X + Y) * 9);
        }
    }
    Source.bSRGB  = true;
    Source.MipGen = ETextureMipGen::Box;
    BuildTextureMips(Source);

    /* Each format's channels, and how far a level-0 texel may stray on
     * average: loose, since three channels ramping apart is a block format's
     * worst case (about 8 for BC1, 4 for BC7), and a broken codec strays by
     * tens. The smaller levels ramp twice as steeply each, so get twice it. */
    struct Case { ETextureCompression Compression; int Channels; bool bAlpha; double MeanError; };
    const Case Cases[] = {
        { ETextureCompression::BC1, 3, false, 12.0 },
        { ETextureCompression::BC3, 3, true,  12.0 },
        { ETextureCompression::BC4, 1, false, 6.0 },
        { ETextureCompression::BC5, 2, false, 6.0 },
        { ETextureCompression::BC7, 3, true,  6.0 },
    };
    double LevelZeroError[6] = {};
    for(const Case& Each : Cases)
    {
        TextureData Texture = Source;
        Texture.Compression = Each.Compression;
        std::string Error;
        bool bClose = CompressTexture(Texture, Error) && TextureBlocksMatch(Texture) && Texture.Blocks.size() == 4
                      && TextureGpuBytes(Texture) == TextureCompressedBytes(13, 9, 4, Each.Compression);
        for(int Level = 0; bClose && Level < Texture.GetMipCount(); ++Level)
        {
            const Image& Mip = *Texture.GetMip(Level);
            Image Decoded;
            bClose = DecodeTextureBlocks(Texture.Blocks[size_t(Level)], Each.Compression, Decoded)
                     && Decoded.GetWidth() == Mip.GetWidth() && Decoded.GetHeight() == Mip.GetHeight();
            double Error = 0.0;
            int    Count = 0;
            for(int Y = 0; bClose && Y < Mip.GetHeight(); ++Y)
            {
                const uint8_t* Want = Mip.GetScanline(Y);
                const uint8_t* Got  = Decoded.GetScanline(Y);
                for(int X = 0; X < Mip.GetWidth(); ++X)
                {
                    for(int Channel = 0; Channel < 4; ++Channel)
                    {
                        const int Texel = X * 4 + Channel;
                        if(Channel < Each.Channels || (Channel == 3 && Each.bAlpha))
                        {
                            Error += std::abs(int(Want[Texel]) - int(Got[Texel]));
                            ++Count;
                        }
                        else if(Got[Texel] != (Channel == 3 ? 255 : 0))
                        {
                            bClose = false;   // what the format lacks reads 0, alpha 1
                        }
                    }
                }
            }
            if(Level == 0) { LevelZeroError[int(Each.Compression)] = Error / std::max(Count, 1); }
            bClose = bClose && Error / std::max(Count, 1) <= Each.MeanError * (Level == 0 ? 1.0 : 2.0);
        }
        Expect(bClose, (std::string(GetTextureCompressionName(Each.Compression))
                        + ": every level encodes, and decodes close to its source").c_str());
    }
    Expect(LevelZeroError[int(ETextureCompression::BC7)] < LevelZeroError[int(ETextureCompression::BC1)],
           "BC7 is closer than BC1");

    /* Saved beside the pixels; loaded ForGpu without them. */
    TextureData Texture = Source;
    Texture.Compression = ETextureCompression::BC7;
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreCompression";
    std::filesystem::create_directories(Root);
    const std::string File = (Root / "Ramp.nmtexture").string();
    Expect(SaveTextureFile(File, Texture) && TextureBlocksMatch(Texture), "SaveTextureFile encodes what it has not");
    TextureData Everything, ForGpu;
    Expect(LoadTextureFile(File, Everything) && Everything.Blocks.size() == 4 && Everything.Mips.size() == 3
           && Everything.Blocks[0].Bytes == Texture.Blocks[0].Bytes && Everything.Blocks[3].Bytes == Texture.Blocks[3].Bytes
           && std::memcmp(Everything.Pixels.GetData(), Source.Pixels.GetData(), Source.Pixels.GetSizeBytes()) == 0,
           "and it loads with the RGBA8 source and the blocks");
    Expect(LoadTextureFile(File, ForGpu, ETextureLoad::ForGpu) && ForGpu.Pixels.IsNull() && ForGpu.Mips.empty()
           && ForGpu.Blocks.size() == 4 && ForGpu.Blocks[0].Width == 13 && ForGpu.Blocks[3].Width == 1
           && ForGpu.Blocks[2].Bytes == Texture.Blocks[2].Bytes && ForGpu.Compression == ETextureCompression::BC7,
           "ForGpu, only the blocks");
    TextureHeader Header;
    Expect(LoadTextureHeader(File, Header) && Header.bBlocks && Header.MipCount == 4, "the header says blocks follow");

    TextureData Stale = Texture;
    Stale.Blocks.pop_back();
    Archive StaleWriter = Archive::ForWriting();
    Expect(!SerializeTexture(StaleWriter, Stale), "blocks that are not every level do not save");
    Stale.Blocks = Texture.Blocks;
    Stale.Compression = ETextureCompression::BC1;
    Expect(!TextureBlocksMatch(Stale), "nor do blocks of another compression");
    Expect(SaveTextureFile(File, Stale) && TextureBlocksMatch(Stale)
           && Stale.Blocks[0].Bytes.size() == TextureCompressedBytes(13, 9, 1, ETextureCompression::BC1),
           "which SaveTextureFile encodes again");
    Stale.Compression = ETextureCompression::None;
    Expect(SaveTextureFile(File, Stale) && Stale.Blocks.empty() && LoadTextureFile(File, ForGpu, ETextureLoad::ForGpu)
           && ForGpu.Blocks.empty() && !ForGpu.Pixels.IsNull(), "None drops the blocks: ForGpu then has the pixels");
    BuildTextureMips(Texture);
    Expect(Texture.Blocks.empty(), "rebuilding the mips clears the blocks made from them");

    TextureData Hdri = Source;
    Hdri.Format      = ETexturePixelFormat::RGBE8;
    Hdri.Compression = ETextureCompression::BC7;
    std::string Error;
    Expect(CompressTexture(Hdri, Error) && Hdri.Blocks.empty(), "an RGBE8 texture is never compressed");
    std::filesystem::remove_all(Root);
#else
    Expect(GetTextureCompressor() == nullptr && !IsTextureCompressionAvailable(ETextureCompression::BC7),
           "a game build has no encoder: only None is honoured");
#endif
}

/*
 * RenderComponent's serializer writes every slot by name, as a graph path, and
 * reads it back. Through a recording archive, so no file format is involved.
 */
/* RenderComponent's slot and parameter API, as Lua and graphs reach it. */
static void TestRenderComponentMaterials()
{
    std::printf("[render component materials]\n");

    RenderComponent C;
    Expect(C.GetNumRegisteredMaterials() == 0 && std::strcmp(C.GetMaterial(0), "") == 0, "no material to start with");
    Expect(C.SetMaterial(0, "Materials/A.material.nmgraph") && C.SetMaterial(3, "Materials/B.material.nmgraph"),
           "SetMaterial by slot index");
    Expect(C.GetNumRegisteredMaterials() == 2 && std::strcmp(C.GetMaterial(3), "Materials/B.material.nmgraph") == 0,
           "GetMaterial by index; GetNumRegisteredMaterials counts the slots set");
    Expect(!C.SetMaterial(8, "x") && !C.SetMaterial(-1, "x") && std::strcmp(C.GetMaterial(9), "") == 0,
           "an index outside the slots is refused, and reads as none");

    float Value[4] = {};
    Expect(C.SetMaterialParameter(0, "Color", 1, 0, 0, 1) && C.GetMaterialParameter(0, "Color", Value) && Value[0] == 1.0f,
           "a parameter override is set and read");
    Expect(C.SetMaterialParameter(0, "Color", 0, 1, 0, 1) && C.GetMaterialParameterCount() == 1
           && C.GetMaterialParameter(0, "Color", Value) && Value[1] == 1.0f, "setting it again replaces it");
    Expect(!C.GetMaterialParameter(1, "Color", Value), "an override belongs to one slot");
    Expect(C.SetMaterialParameter(1, "Color", 0, 0, 1, 1) && C.SetMaterialParameter(0, "Speed", 2, 0, 0, 0)
           && C.GetMaterialParameterCount() == 3, "several overrides, across slots");
    Expect(C.ClearMaterialParameter(0, "Color") && !C.GetMaterialParameter(0, "Color", Value)
           && C.GetMaterialParameterCount() == 2 && C.GetMaterialParameter(0, "Speed", Value) && Value[0] == 2.0f,
           "clearing one keeps the rest, packed");
    Expect(!C.ClearMaterialParameter(0, "Missing") && !C.SetMaterialParameter(0, "", 0, 0, 0, 0)
           && !C.SetMaterialParameter(8, "Color", 0, 0, 0, 0), "bad names and slots are refused");

    RenderComponent Full;
    char Name[16];
    bool bAll = true;
    for(size_t Index = 0; Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
    {
        std::snprintf(Name, sizeof(Name), "P%zu", Index);
        bAll = bAll && Full.SetMaterialParameter(0, Name, 0, 0, 0, 0);
    }
    Expect(bAll && !Full.SetMaterialParameter(0, "OneMore", 0, 0, 0, 0) && Full.SetMaterialParameter(0, "P3", 1, 0, 0, 0),
           "a full table refuses a new name but still updates one it has");
}

static void TestRenderComponentSerializer()
{
    std::printf("[render component serializer]\n");

    struct RecordingArchive final : GameCore::ComponentArchive
    {
        bool                     bLoading = false;
        uint16_t                 Version  = 2;
        std::map<std::string, std::string> Strings;   // what a load reads, and what a save wrote
        std::map<std::string, double>      Numbers;   // the same, for float, int32 and uint8 fields
        std::vector<std::string> Names;

        /* A save records InOut; a load returns the recorded value, or InOut. */
        double Number(const char* InName, const double InOut)
        {
            Names.push_back(InName);
            if(!bLoading) { Numbers[InName] = InOut; return InOut; }
            const auto Found = Numbers.find(InName);
            return Found != Numbers.end() ? Found->second : InOut;
        }

        bool     IsLoading()  const override { return bLoading; }
        uint16_t GetVersion() const override { return Version; }
        void Field(const char* InName, float& InOut) override         { InOut = float(Number(InName, InOut)); }
        void Field(const char* InName, double&) override              { Names.push_back(InName); }
        void Field(const char* InName, int32_t& InOut) override       { InOut = int32_t(Number(InName, InOut)); }
        void Field(const char* InName, uint32_t&) override            { Names.push_back(InName); }
        void Field(const char* InName, uint8_t& InOut) override       { InOut = uint8_t(Number(InName, InOut)); }
        void Field(const char* InName, bool&) override                { Names.push_back(InName); }
        void Field(const char* InName, EngineCore::Vector3&) override { Names.push_back(InName); }
        void Field(const char* InName, GameCore::AABB&) override      { Names.push_back(InName); }
        void Field(const char* InName, EngineCore::Color&) override   { Names.push_back(InName); }
        void Flag(const char* InName, uint8_t&) override              { Names.push_back(InName); }
        void Entity(const char* InName, GameCore::EntityID&) override { Names.push_back(InName); }
        void FixedString(const char* InName, char* InOutBuffer, const size_t InCapacity) override
        {
            Names.push_back(InName);
            if(!bLoading)
            {
                Strings[InName] = InOutBuffer;
                return;
            }
            const auto Found = Strings.find(InName);
            if(Found != Strings.end())
            {
                std::strncpy(InOutBuffer, Found->second.c_str(), InCapacity - 1);
                InOutBuffer[InCapacity - 1] = '\0';
            }
        }
    };

    const GameCore::ComponentSerializerInfo* Info = GameCore::FindComponentSerializer(RenderComponent::StaticType().ID);
    Expect(Info && Info->Version == 7, "RenderComponent saves as version 7 (material parameters, CastShadows, CastInsetShadow, Occluder, CustomPassGroups, RevealPlayers)");
    if(!Info) { return; }

    RenderComponent Saved;
    std::strcpy(Saved.Materials[0], "Materials/Body.material.nmgraph");
    std::strcpy(Saved.Materials[7], "Engine/Content/Materials/DefaultLit.material.nmgraph");
    RecordingArchive Out;
    Out.Version = Info->Version;   // a save writes the current version
    Info->Fn(Out, &Saved);
    Expect(Out.Strings["Material0"] == "Materials/Body.material.nmgraph"
           && Out.Strings["Material7"] == "Engine/Content/Materials/DefaultLit.material.nmgraph" && Out.Strings.count("Material") == 0,
           "each slot is written as Material0 to Material7, a full graph path");
    Expect(std::find(Out.Names.begin(), Out.Names.end(), std::string("Width")) == Out.Names.end()
           && std::find(Out.Names.begin(), Out.Names.end(), std::string("Tint")) == Out.Names.end(),
           "and no Width, Height or Tint");

    RecordingArchive Back;
    Back.bLoading = true;
    Back.Strings  = Out.Strings;
    RenderComponent Again;
    Info->Fn(Back, &Again);
    Expect(std::strcmp(Again.Materials[0], "Materials/Body.material.nmgraph") == 0
           && std::strcmp(Again.Materials[7], "Engine/Content/Materials/DefaultLit.material.nmgraph") == 0,
           "and reads them back unchanged");

    Expect(std::find(Out.Names.begin(), Out.Names.end(), std::string("CastShadows")) != Out.Names.end(),
           "version 3 writes CastShadows");
    /* Material parameters: a count, then that many. */
    RenderComponent Tinted;
    Expect(Tinted.SetMaterialParameter(0, "Color", 1.0f, 0.5f, 0.25f, 1.0f) && Tinted.SetMaterialParameter(1, "Speed", 3.0f, 0, 0, 0),
           "a component takes parameter overrides");
    RecordingArchive TintOut;
    Info->Fn(TintOut, &Tinted);
    Expect(TintOut.Numbers["ParameterCount"] == 2 && TintOut.Strings["Parameter0Name"] == "Color"
           && TintOut.Numbers["Parameter1Slot"] == 1 && TintOut.Numbers["Parameter0Y"] == 0.5
           && TintOut.Numbers.count("Parameter2X") == 0,
           "only the overrides in use are written");
    RecordingArchive TintBack;
    TintBack.bLoading = true;
    TintBack.Strings  = TintOut.Strings;
    TintBack.Numbers  = TintOut.Numbers;
    RenderComponent TintAgain;
    Info->Fn(TintBack, &TintAgain);
    float Value[4] = {};
    Expect(TintAgain.GetMaterialParameterCount() == 2 && TintAgain.GetMaterialParameter(0, "Color", Value)
           && Value[0] == 1.0f && Value[2] == 0.25f && TintAgain.GetMaterialParameter(1, "Speed", Value) && Value[0] == 3.0f,
           "and read back");

    RecordingArchive Old;
    Old.bLoading = true;
    Old.Version  = 1;
    RenderComponent FromOld;
    FromOld.bCastShadows = 1;
    Info->Fn(Old, &FromOld);
    Expect(std::find(Old.Names.begin(), Old.Names.end(), std::string("ParameterCount")) == Old.Names.end()
           && FromOld.GetMaterialParameterCount() == 0, "a version 1 component reads no parameters");
    Expect(std::find(Old.Names.begin(), Old.Names.end(), std::string("CastShadows")) == Old.Names.end() && FromOld.bCastShadows == 1,
           "and no CastShadows, and keeps casting");
    Expect(std::find(Old.Names.begin(), Old.Names.end(), std::string("CastInsetShadow")) == Old.Names.end() && FromOld.bCastInsetShadow == 0,
           "nor CastInsetShadow, which stays off");
    Expect(std::find(Old.Names.begin(), Old.Names.end(), std::string("Occluder")) == Old.Names.end() && FromOld.bOccluder == 0,
           "nor Occluder, which stays off");

    Expect(std::find(Out.Names.begin(), Out.Names.end(), std::string("CastInsetShadow")) != Out.Names.end(),
           "a version 4 save writes CastInsetShadow");
    Expect(std::find(Out.Names.begin(), Out.Names.end(), std::string("Occluder")) != Out.Names.end(),
           "a version 5 save writes Occluder");

    /* The lights: version 2 dropped the sun's Direction (it is the
     * Transform's forward now) and added shadows; version 3 its baked
     * Source Angle, as point and spot lights' version 2 their Source Radius. */
    const GameCore::ComponentSerializerInfo* SunInfo = GameCore::FindComponentSerializer(DirectionalLightComponent::StaticType().ID);
    Expect(SunInfo && SunInfo->Version == 3, "DirectionalLightComponent saves as version 3");
    if(SunInfo)
    {
        RecordingArchive SunOut;
        SunOut.Version = SunInfo->Version;
        DirectionalLightComponent Sun;
        SunInfo->Fn(SunOut, &Sun);
        const auto Has = [](const RecordingArchive& InArchive, const char* InName)
        {
            return std::find(InArchive.Names.begin(), InArchive.Names.end(), std::string(InName)) != InArchive.Names.end();
        };
        Expect(!Has(SunOut, "Direction") && Has(SunOut, "ShadowTechnique") && Has(SunOut, "CascadeCount") && Has(SunOut, "SourceAngle"),
               "version 3 writes no Direction, and the shadow fields and Source Angle");
        RecordingArchive SunOld;
        SunOld.bLoading = true;
        SunOld.Version  = 1;
        SunInfo->Fn(SunOld, &Sun);
        Expect(Has(SunOld, "Direction") && !Has(SunOld, "ShadowTechnique"), "a version 1 save's Direction is read (and dropped)");
        RecordingArchive SunTwo;
        SunTwo.bLoading = true;
        SunTwo.Version  = 2;
        DirectionalLightComponent FromTwo;
        SunInfo->Fn(SunTwo, &FromTwo);
        Expect(!Has(SunTwo, "SourceAngle") && FromTwo.SourceAngle == 0.75f, "a version 2 sun keeps the default Source Angle");
    }
    const GameCore::ComponentSerializerInfo* PointInfo = GameCore::FindComponentSerializer(PointLightComponent::StaticType().ID);
    const GameCore::ComponentSerializerInfo* SpotInfo  = GameCore::FindComponentSerializer(SpotLightComponent::StaticType().ID);
    Expect(PointInfo && PointInfo->Version == 2 && SpotInfo && SpotInfo->Version == 2, "Point and Spot Light save as version 2 (Source Radius)");
    const GameCore::ComponentSerializerInfo* CameraInfo = GameCore::FindComponentSerializer(CameraComponent::StaticType().ID);
    Expect(CameraInfo && CameraInfo->Version == 2, "CameraComponent saves as version 2 (Projection, FovY)");

    const GameCore::ComponentSerializerInfo* PostInfo = GameCore::FindComponentSerializer(PostProcessComponent::StaticType().ID);
    Expect(PostInfo && PostInfo->Version == 3, "PostProcessComponent saves as version 3 (Stack, BlendDistance; Weight)");
    if(PostInfo)
    {
        const auto HasName = [](const RecordingArchive& InArchive, const char* InName)
        {
            return std::find(InArchive.Names.begin(), InArchive.Names.end(), std::string(InName)) != InArchive.Names.end();
        };
        RecordingArchive PostOut;
        PostOut.Version = PostInfo->Version;
        PostProcessComponent Volume;
        std::strcpy(Volume.Stack, "Content/PostProcess/Arch.pp.nmgraph");
        PostInfo->Fn(PostOut, &Volume);
        Expect(HasName(PostOut, "Stack") && HasName(PostOut, "BlendDistance") && HasName(PostOut, "Weight") && !HasName(PostOut, "Material"),
               "version 3 writes Stack, BlendDistance and Weight");
        RecordingArchive PostTwo;
        PostTwo.bLoading = true;
        PostTwo.Version  = 2;
        PostProcessComponent FromTwo;
        PostInfo->Fn(PostTwo, &FromTwo);
        Expect(HasName(PostTwo, "BlendDistance") && !HasName(PostTwo, "Weight") && FromTwo.Weight == 1.0f,
               "a version 2 save reads no Weight and keeps full strength");
        RecordingArchive PostOld;
        PostOld.bLoading = true;
        PostOld.Version  = 1;
        PostOld.Strings["Material"] = "Materials/Negative.material.nmgraph";
        PostProcessComponent FromOld;
        PostInfo->Fn(PostOld, &FromOld);
        Expect(std::strcmp(FromOld.Stack, "Materials/Negative.material.nmgraph") == 0 && !HasName(PostOld, "BlendDistance"),
               "a version 1 save's Material reads into Stack");
    }
}

#if WITH_EDITOR
/*
 * Shadows on the GPU (NIMBLE_SMOKE_GPU=Vulkan): a box over a floor, drawn
 * with shadows on and off, compared pixel by pixel. A shadow only takes light
 * away; Cast Shadows off takes none away; a Modulated shadow tints.
 */
static void TestShadowsOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[shadows on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[shadows on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    ConfigVar* Enabled = ConfigFindVarByPath("Shadows.Enabled");
    Expect(Enabled != nullptr, "[Shadows] Enabled is a config var");

    Renderer Rend;
    const bool bInit = Enabled && Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakePerspective(Viewport, Vector3(0.0, 700.0, -600.0), Vector3(0.0, -700.0, 600.0),
                                                            50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&View);
        GameCore::World World;
        Rend.BindWorld(World);

        const auto Placed = [&World](const char* InName, const Vector3& InPosition, const Vector3& InRotation, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InPosition;
            Transform.Rotation = InRotation;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            return Each;
        };
        const auto Mesh = [](const char* InPath)
        {
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InPath);
            return Render;
        };

        Placed("Floor", Vector3(0.0, 0.0, 0.0), Vector3(), Vector3(3000.0, 1.0, 3000.0)).AddComponent(Mesh("Engine/Content/Meshes/Plane.nmmesh"));
        const GameCore::Entity Box = Placed("Box", Vector3(0.0, 90.0, 0.0), Vector3(), Vector3(120.0, 120.0, 120.0));
        Box.AddComponent(Mesh("Engine/Content/Meshes/Box.nmmesh"));

        /* Down and toward +X: the shadow falls beside the box, where the camera sees it. */
        DirectionalLightComponent SunInit;
        SunInit.ShadowTechnique = LST_Standard;
        SunInit.ShadowDistance  = 3000.0f;
        const GameCore::Entity Sun = Placed("Sun", Vector3(), Vector3(55.0, 90.0, 0.0), Vector3(1.0, 1.0, 1.0));
        Sun.AddComponent(SunInit);
        Placed("Sky", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(SkyLightComponent());

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            const uint64_t Request = Rend.RequestSceneCapture();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Rgb = [](const EngineCore::Image& InPixels, const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };
        /* NIMBLE_SMOKE_DUMP=<dir>: each capture as a PPM there, to look at. */
        const char* DumpDir = std::getenv("NIMBLE_SMOKE_DUMP");
        const auto Dump = [&](const EngineCore::Image& InPixels, const char* InName)
        {
            if(!DumpDir || !DumpDir[0] || InPixels.GetWidth() == 0) { return; }
            std::ofstream File(std::string(DumpDir) + "/" + InName + ".ppm", std::ios::binary);
            File << "P6\n" << InPixels.GetWidth() << " " << InPixels.GetHeight() << "\n255\n";
            for(int Y = 0; Y < int(InPixels.GetHeight()); ++Y)
            {
                for(int X = 0; X < int(InPixels.GetWidth()); ++X)
                {
                    int Value[3];
                    Rgb(InPixels, X, Y, Value);
                    const char Bytes[3] = { char(Value[0]), char(Value[1]), char(Value[2]) };
                    File.write(Bytes, 3);
                }
            }
        };

        /* Pixels InA has darker than InB, brighter, and how much bluer the darker ones lean. */
        struct Difference { int Darker = 0; int Brighter = 0; double BlueLean = 0.0; };
        const auto Compare = [&](const EngineCore::Image& InA, const EngineCore::Image& InB)
        {
            Difference Out;
            for(int Y = 0; Y < 256; ++Y)
            {
                for(int X = 0; X < 256; ++X)
                {
                    int A[3], B[3];
                    Rgb(InA, X, Y, A);
                    Rgb(InB, X, Y, B);
                    const int Delta = (A[0] + A[1] + A[2]) - (B[0] + B[1] + B[2]);
                    if(Delta < -45)
                    {
                        ++Out.Darker;
                        Out.BlueLean += double(A[2] - A[0]) - double(B[2] - B[0]);
                    }
                    if(Delta > 45) { ++Out.Brighter; }
                }
            }
            if(Out.Darker > 0) { Out.BlueLean /= Out.Darker; }
            return Out;
        };

        EngineCore::Image Lit, Shadowed, NoCaster, Modulated, SpotLit, SpotShadowed;
        Enabled->SetFromString("0");
        const bool bLit = Capture(Lit);
        Enabled->SetFromString("1");
        const bool bShadowed = Capture(Shadowed);
        Expect(bLit && bShadowed, "the scene is captured with shadows off and on");
        if(bLit && bShadowed)
        {
            Dump(Lit, "sun-lit");
            Dump(Shadowed, "sun-shadowed");
            const Difference Sunlit = Compare(Shadowed, Lit);
            std::printf("    sun: %d pixels darker, %d brighter\n", Sunlit.Darker, Sunlit.Brighter);
            Expect(Sunlit.Darker > 300, "the box shadows the floor under a Standard sun");
            Expect(Sunlit.Brighter < 20, "and a shadow only takes light away");

            Box.GetComponent<RenderComponent>()->bCastShadows = 0;
            const bool bNoCaster = Capture(NoCaster);
            const Difference Off = bNoCaster ? Compare(NoCaster, Lit) : Difference();
            std::printf("    cast shadows off: %d pixels darker\n", Off.Darker);
            Expect(bNoCaster && Off.Darker < 40, "a mesh with Cast Shadows off casts none");
            Box.GetComponent<RenderComponent>()->bCastShadows = 1;

            DirectionalLightComponent* SunLight = Sun.GetComponent<DirectionalLightComponent>();
            SunLight->ShadowTechnique = LST_Modulated;
            SunLight->ShadowTint      = EngineCore::Color(20, 40, 220);
            const bool bModulated = Capture(Modulated);
            Dump(Modulated, "sun-modulated");
            const Difference Tinted = bModulated ? Compare(Modulated, Lit) : Difference();
            std::printf("    modulated: %d pixels darker, blue lean %.1f\n", Tinted.Darker, Tinted.BlueLean);
            Expect(bModulated && Tinted.Darker > 300 && Tinted.BlueLean > 8.0, "a Modulated shadow darkens toward its tint");

            SunLight->ShadowTechnique = LST_Cascaded;
            EngineCore::Image Cascaded;
            const bool bCascaded = Capture(Cascaded);
            Dump(Cascaded, "sun-cascaded");
            const Difference Cascades = bCascaded ? Compare(Cascaded, Lit) : Difference();
            std::printf("    cascaded: %d pixels darker\n", Cascades.Darker);
            Expect(bCascaded && Cascades.Darker > 300 && Cascades.Brighter < 20, "a Cascaded sun shadows the floor too");

            /* Inset shadows: the sun's Shadow Distance cut short of the box,
             * so its own view misses it; Cast Inset Shadow gives the box a
             * map of its own, which [Shadows] MaxInsetShadows 0 takes away. */
            SunLight->ShadowTechnique = LST_Standard;
            SunLight->ShadowDistance  = 1.0f;
            ConfigVar* MaxInsets = ConfigFindVarByPath("Shadows.MaxInsetShadows");
            Expect(MaxInsets != nullptr, "[Shadows] MaxInsetShadows is a config var");
            EngineCore::Image Short, Inset, NoInsets;
            const bool bShort = Capture(Short);
            const Difference NoReach = bShort ? Compare(Short, Lit) : Difference();
            Box.GetComponent<RenderComponent>()->bCastInsetShadow = 1;
            const bool bInset = Capture(Inset);
            Dump(Inset, "sun-inset");
            const Difference Own = bInset ? Compare(Inset, Lit) : Difference();
            if(MaxInsets) { MaxInsets->SetFromString("0"); }
            const bool bNoInsets = Capture(NoInsets);
            const Difference Capped = bNoInsets ? Compare(NoInsets, Lit) : Difference();
            if(MaxInsets) { MaxInsets->SetFromString("4"); }
            std::printf("    inset: %d pixels darker without, %d with (%d brighter), %d with MaxInsetShadows 0\n",
                        NoReach.Darker, Own.Darker, Own.Brighter, Capped.Darker);
            Expect(bShort && NoReach.Darker < 40, "a sun whose Shadow Distance stops short draws no shadow of the box");
            Expect(bInset && Own.Darker > 300 && Own.Brighter < 20, "Cast Inset Shadow shadows the floor from the box's own map");
            Expect(bNoInsets && Capped.Darker < 40, "MaxInsetShadows 0 turns inset shadows off");

            /* InsetFilter: every setting draws the shadow; hard and 5x5 edges differ. */
            ConfigVar* InsetFilter = ConfigFindVarByPath("Shadows.InsetFilter");
            Expect(InsetFilter != nullptr, "[Shadows] InsetFilter is a config var");
            EngineCore::Image Hard, Soft;
            if(InsetFilter) { InsetFilter->SetFromString("0"); }
            const bool bHard = Capture(Hard);
            if(InsetFilter) { InsetFilter->SetFromString("2"); }
            const bool bSoft = Capture(Soft);
            if(InsetFilter) { InsetFilter->SetFromString("1"); }
            const Difference HardShadow = bHard ? Compare(Hard, Lit) : Difference();
            const Difference SoftShadow = bSoft ? Compare(Soft, Lit) : Difference();
            int EdgePixels = 0;
            for(int Y = 0; bHard && bSoft && Y < 256; ++Y)
            {
                for(int X = 0; X < 256; ++X)
                {
                    int A[3], B[3];
                    Rgb(Hard, X, Y, A);
                    Rgb(Soft, X, Y, B);
                    EdgePixels += std::abs((A[0] + A[1] + A[2]) - (B[0] + B[1] + B[2])) > 12 ? 1 : 0;
                }
            }
            std::printf("    inset filter: hard %d pixels darker, 5x5 %d; %d edge pixels differ\n", HardShadow.Darker, SoftShadow.Darker, EdgePixels);
            Expect(bHard && bSoft && HardShadow.Darker > 300 && SoftShadow.Darker > 300, "every InsetFilter draws the inset shadow");
            Expect(EdgePixels > 20, "and a hard filter's edge differs from a 5x5 one's");
            Box.GetComponent<RenderComponent>()->bCastInsetShadow = 0;

            /* Contact shadows ([Shadows] ContactShadows): the sun's map still stops short of
             * the box, so only the screen-space march toward the sun can find it -- from the
             * floor under it, where its shadow falls. The box floats 30 above the floor and
             * is 120 deep, so the march reaches further and takes thicker surfaces than the
             * defaults. Off (the default), Short above already showed nothing. */
            ConfigVar* Contact          = ConfigFindVarByPath("Shadows.ContactShadows");
            ConfigVar* ContactLength    = ConfigFindVarByPath("Shadows.ContactLength");
            ConfigVar* ContactThickness = ConfigFindVarByPath("Shadows.ContactThickness");
            Expect(Contact && ContactLength && ContactThickness, "[Shadows] ContactShadows, ContactLength and ContactThickness are config vars");
            EngineCore::Image Contacted;
            bool bContacted = false;
            if(Contact && ContactLength && ContactThickness)
            {
                ContactLength->SetFromString("150");
                ContactThickness->SetFromString("200");
                Contact->SetFromString("true");
                bContacted = Capture(Contacted);
                Contact->ResetToDefault();
                ContactLength->ResetToDefault();
                ContactThickness->ResetToDefault();
            }
            Dump(Contacted, "sun-contact");
            const Difference Touch = bContacted ? Compare(Contacted, Short) : Difference();
            std::printf("    contact: %d pixels darker, %d brighter\n", Touch.Darker, Touch.Brighter);
            Expect(bContacted && Touch.Darker > 300 && Touch.Brighter < 20, "sun contact shadows find the box from the floor under it");

            /* A caster out of the view's bounds altogether -- above and behind
             * the camera -- whose shadow falls on the floor it sees: offered
             * all the same, since its shadow reaches the view. */
            GameCore::TransformComponent* SunAt = Sun.GetComponent<GameCore::TransformComponent>();
            const Vector3 SunRotation = SunAt->Rotation;
            SunAt->Rotation = Vector3(55.0, 0.0, 0.0);   // down and toward +Z, the way the camera looks
            const Vector3 Travel = LightForward(SunAt->Rotation);
            const GameCore::Entity Overhead = Placed("Overhead", Travel * -1500.0, Vector3(), Vector3(120.0, 120.0, 120.0));
            RenderComponent OverheadRender = Mesh("Engine/Content/Meshes/Box.nmmesh");
            OverheadRender.bCastInsetShadow = 1;
            Overhead.AddComponent(OverheadRender);
            EngineCore::Image Unlit, FromAbove, NotOffered;
            Enabled->SetFromString("0");
            const bool bUnlit = Capture(Unlit);
            Enabled->SetFromString("1");
            const bool bFromAbove = Capture(FromAbove);
            Dump(FromAbove, "sun-inset-offscreen");
            Overhead.GetComponent<RenderComponent>()->bCastInsetShadow = 0;
            const bool bNotOffered = Capture(NotOffered);
            const Difference Above = bUnlit && bFromAbove ? Compare(FromAbove, Unlit) : Difference();
            const Difference Plain = bUnlit && bNotOffered ? Compare(NotOffered, Unlit) : Difference();
            std::printf("    off-screen caster: %d pixels darker with an inset, %d without\n", Above.Darker, Plain.Darker);
            Expect(bFromAbove && Above.Darker > 300 && Above.Brighter < 20,
                   "a caster outside the view's bounds still gets an inset shadow on the floor it sees");
            Expect(bNotOffered && Plain.Darker < 40, "which the sun's short view alone does not draw");
            World.DestroyEntity(Overhead.GetID());
            SunAt->Rotation = SunRotation;
            SunLight->ShadowDistance = 3000.0f;

            /* The sun off; a spot light straight down over the box. */
            SunLight->Intensity = 0.0f;
            SpotLightComponent SpotInit;
            SpotInit.Intensity      = 30.0f;
            SpotInit.Range          = 1200.0f;
            SpotInit.OuterConeAngle = 45.0f;
            SpotInit.InnerConeAngle = 35.0f;
            const GameCore::Entity Spot = Placed("Spot", Vector3(60.0, 500.0, -60.0), Vector3(90.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0));
            Spot.AddComponent(SpotInit);
            Enabled->SetFromString("0");
            const bool bSpotLit = Capture(SpotLit);
            Enabled->SetFromString("1");
            const bool bSpotShadowed = Capture(SpotShadowed);
            Dump(SpotShadowed, "spot-shadowed");
            const Difference SpotShadow = bSpotLit && bSpotShadowed ? Compare(SpotShadowed, SpotLit) : Difference();
            std::printf("    spot: %d pixels darker, %d brighter\n", SpotShadow.Darker, SpotShadow.Brighter);
            Expect(SpotShadow.Darker > 200 && SpotShadow.Brighter < 20, "a spot light's shadow falls under the box");

            /* The spot's contact shadows ([Shadows] ContactLocalLights): the box out of the
             * shadow map (Cast Shadows off), so only the march toward the light finds it. */
            {
                ConfigVar* Contact          = ConfigFindVarByPath("Shadows.ContactShadows");
                ConfigVar* ContactLength    = ConfigFindVarByPath("Shadows.ContactLength");
                ConfigVar* ContactThickness = ConfigFindVarByPath("Shadows.ContactThickness");
                ConfigVar* ContactLocal     = ConfigFindVarByPath("Shadows.ContactLocalLights");
                Expect(Contact && ContactLength && ContactThickness && ContactLocal, "[Shadows] ContactLocalLights is a config var");
                EngineCore::Image NoMap, SpotContact, SpotSunOnly;
                bool bNoMap = false, bSpotContact = false, bSunOnly = false;
                if(Contact && ContactLength && ContactThickness && ContactLocal)
                {
                    Box.GetComponent<RenderComponent>()->bCastShadows = 0;
                    bNoMap = Capture(NoMap);
                    ContactLength->SetFromString("400");
                    ContactThickness->SetFromString("200");
                    Contact->SetFromString("true");
                    bSpotContact = Capture(SpotContact);
                    ContactLocal->SetFromString("false");
                    bSunOnly = Capture(SpotSunOnly);
                    for(ConfigVar* Each : { Contact, ContactLength, ContactThickness, ContactLocal }) { Each->ResetToDefault(); }
                    Box.GetComponent<RenderComponent>()->bCastShadows = 1;
                }
                Dump(SpotContact, "spot-contact");
                const Difference Local  = bNoMap && bSpotContact ? Compare(SpotContact, NoMap) : Difference();
                const Difference Sunned = bNoMap && bSunOnly ? Compare(SpotSunOnly, NoMap) : Difference();
                std::printf("    spot contact: %d pixels darker, %d brighter; %d darker with ContactLocalLights off\n",
                            Local.Darker, Local.Brighter, Sunned.Darker);
                Expect(bSpotContact && Local.Darker > 200 && Local.Brighter < 20, "a shadowed spot light's contact shadows find the box");
                Expect(bSunOnly && Sunned.Darker < 40, "and ContactLocalLights off leaves them to the sun alone");
            }

            /* The spot swapped for a point light over the box. */
            World.DestroyEntity(Spot);
            PointLightComponent PointInit;
            PointInit.Intensity    = 30.0f;
            PointInit.Radius       = 1500.0f;
            PointInit.bCastShadows = 1;
            Placed("Point", Vector3(80.0, 450.0, -80.0), Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(PointInit);
            EngineCore::Image PointLit, PointShadowed;
            Enabled->SetFromString("0");
            const bool bPointLit = Capture(PointLit);
            Enabled->SetFromString("1");
            const bool bPointShadowed = Capture(PointShadowed);
            Dump(PointShadowed, "point-shadowed");
            const Difference PointShadow = bPointLit && bPointShadowed ? Compare(PointShadowed, PointLit) : Difference();
            std::printf("    point: %d pixels darker, %d brighter\n", PointShadow.Darker, PointShadow.Brighter);
            Expect(PointShadow.Darker > 200 && PointShadow.Brighter < 20, "a point light's shadow falls under the box");
        }

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    if(Enabled) { Enabled->SetFromString("1"); }
    SetFileProvider(PreviousProvider);
}
#endif // WITH_EDITOR

#if WITH_EDITOR
/*
 * Lit translucency on the GPU (NIMBLE_SMOKE_GPU=Vulkan or D3D12;
 * LIT_TRANSLUCENCY.md, "Validation"): small scenes drawn with the Engine
 * cache's Testing materials (Engine/Content/Materials/Testing), their pixels
 * checked against what each phase predicts. NIMBLE_SMOKE_DUMP=<dir> keeps
 * each capture as a PPM.
 */
static void TestTranslucencyOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[translucency on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[translucency on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakePerspective(Viewport, Vector3(0.0, 100.0, -600.0), Vector3(0.0, 0.0, 1.0),
                                                            50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&View);

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 4; ++Each)
            {
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
                Rend.WaitForPreviousFrame();
            }
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Rgb = [](const EngineCore::Image& InPixels, const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };
        const char* DumpDir = std::getenv("NIMBLE_SMOKE_DUMP");
        const auto Dump = [&](const EngineCore::Image& InPixels, const char* InName)
        {
            if(!DumpDir || !DumpDir[0] || InPixels.GetWidth() == 0) { return; }
            std::ofstream File(std::string(DumpDir) + "/" + InName + ".ppm", std::ios::binary);
            File << "P6\n" << InPixels.GetWidth() << " " << InPixels.GetHeight() << "\n255\n";
            for(int Y = 0; Y < int(InPixels.GetHeight()); ++Y)
            {
                for(int X = 0; X < int(InPixels.GetWidth()); ++X)
                {
                    int Value[3];
                    Rgb(InPixels, X, Y, Value);
                    const char Bytes[3] = { char(Value[0]), char(Value[1]), char(Value[2]) };
                    File.write(Bytes, 3);
                }
            }
        };

        GameCore::World World;
        Rend.BindWorld(World);
        const auto Placed = [&World](const char* InName, const Vector3& InPosition, const Vector3& InRotation, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InPosition;
            Transform.Rotation = InRotation;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            return Each;
        };
        const auto Drawn = [](const char* InMesh, const char* InMaterial)
        {
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InMesh);
            std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "Engine/Content/Materials/Testing/%s.material.nmgraph", InMaterial);
            return Render;
        };
        const char* BOX = "Engine/Content/Meshes/Box.nmmesh";

        /* A sun from behind the camera, and the sky. */
        DirectionalLightComponent SunInit;
        SunInit.ShadowDistance = 3000.0f;
        const GameCore::Entity Sun = Placed("Sun", Vector3(), Vector3(35.0, 10.0, 0.0), Vector3(1.0, 1.0, 1.0));
        Sun.AddComponent(SunInit);
        Placed("Sky", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(SkyLightComponent());

        /* Phase 1: a red Lit quad and a green Unlit one overlapping in the middle of
         * the view; whichever is nearer wins there, both ways round. */
        {
            const GameCore::Entity Red   = Placed("Red", Vector3(-40.0, 100.0, 0.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            const GameCore::Entity Green = Placed("Green", Vector3(40.0, 100.0, 150.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            Red.AddComponent(Drawn(BOX, "TransLitRed"));
            Green.AddComponent(Drawn(BOX, "TransUnlitGreen"));
            EngineCore::Image RedFront, GreenFront;
            const bool bRedFront = Capture(RedFront);
            Red.GetComponent<GameCore::TransformComponent>()->Position   = Vector3(-40.0, 100.0, 150.0);
            Green.GetComponent<GameCore::TransformComponent>()->Position = Vector3(40.0, 100.0, 0.0);
            const bool bGreenFront = Capture(GreenFront);
            Expect(bRedFront && bGreenFront, "the overlapping quads are captured both ways round");
            if(bRedFront && bGreenFront)
            {
                Dump(RedFront, "order-red-front");
                Dump(GreenFront, "order-green-front");
                int A[3], B[3];
                Rgb(RedFront, 128, 128, A);
                Rgb(GreenFront, 128, 128, B);
                std::printf("    overlap: red in front %d %d %d, green in front %d %d %d\n", A[0], A[1], A[2], B[0], B[1], B[2]);
                Expect(A[0] > A[1] + 40, "a Lit quad in front of an Unlit one covers it");
                Expect(B[1] > B[0] + 40, "and an Unlit quad in front of a Lit one covers that (one translucent stage)");
            }
            World.DestroyEntity(Red);
            World.DestroyEntity(Green);
        }

        /* Phase 2: a white quad at half opacity, its left half behind an opaque blocker
         * from the sun (up the light from x -50 on the quad, above the view of it). Its
         * shadowed half is darker; with shadows off the halves match. */
        ConfigVar* ShadowsOn = ConfigFindVarByPath("Shadows.Enabled");
        if(ShadowsOn)
        {
            const GameCore::Entity Pane    = Placed("Pane", Vector3(0.0, 100.0, 0.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            const GameCore::Entity Blocker = Placed("Blocker", Vector3(-85.0, 243.0, -202.0), Vector3(), Vector3(100.0, 40.0, 100.0));
            Pane.AddComponent(Drawn(BOX, "TransLitWhite"));
            Blocker.AddComponent(Drawn(BOX, "OpaqueBlack"));
            EngineCore::Image On, Off;
            ShadowsOn->SetFromString("1");
            const bool bOn = Capture(On);
            ShadowsOn->SetFromString("0");
            const bool bOff = Capture(Off);
            ShadowsOn->SetFromString("1");
            Expect(bOn && bOff, "the half-shadowed pane is captured with shadows on and off");
            if(bOn && bOff)
            {
                Dump(On, "pane-shadowed");
                Dump(Off, "pane-unshadowed");
                const auto Grey = [&](const EngineCore::Image& InPixels, const int InX)
                {
                    int Sum = 0;
                    for(int Y = 118; Y <= 138; ++Y)
                    {
                        int Value[3];
                        Rgb(InPixels, InX, Y, Value);
                        Sum += Value[0] + Value[1] + Value[2];
                    }
                    return Sum / (21 * 3);
                };
                const int ShadowedOn = Grey(On, 101), LitOn = Grey(On, 155), ShadowedOff = Grey(Off, 101), LitOff = Grey(Off, 155);
                std::printf("    pane: shadows on %d | %d, off %d | %d (left, the blocker's side | right)\n", ShadowedOn, LitOn, ShadowedOff, LitOff);
                Expect(LitOn - ShadowedOn > 15, "a Translucent Lit surface is shadowed by an opaque caster (forward shadows)");
                Expect(std::abs(LitOff - ShadowedOff) < 6, "and with shadows off its halves match");
            }
            World.DestroyEntity(Pane);
            World.DestroyEntity(Blocker);
        }

        /* Phase 3: a pane at opacity 0.02, roughness 0.3, over black, the sun straight
         * down the view so its highlight is at the centre. Translucency Lighting Surface
         * keeps the highlight; Legacy fades it with the opacity to nearly nothing. */
        {
            Sun.GetComponent<GameCore::TransformComponent>()->Rotation = Vector3(0.0, 0.0, 0.0);
            const GameCore::Entity Back = Placed("Back", Vector3(0.0, 100.0, 120.0), Vector3(), Vector3(800.0, 800.0, 2.0));
            GameCore::Entity Pane = Placed("Pane", Vector3(0.0, 100.0, 0.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            Back.AddComponent(Drawn(BOX, "OpaqueBlack"));
            Pane.AddComponent(Drawn(BOX, "TransLitShiny"));
            EngineCore::Image Surface, Legacy;
            const bool bSurface = Capture(Surface);
            World.DestroyEntity(Pane);
            Pane = Placed("PaneLegacy", Vector3(0.0, 100.0, 0.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            Pane.AddComponent(Drawn(BOX, "TransLitShinyLegacy"));
            const bool bLegacy = Capture(Legacy);
            Expect(bSurface && bLegacy, "the shiny pane is captured as Surface and as Legacy");
            if(bSurface && bLegacy)
            {
                Dump(Surface, "shiny-surface");
                Dump(Legacy, "shiny-legacy");
                const auto Centre = [&](const EngineCore::Image& InPixels)
                {
                    int Sum = 0;
                    for(int Y = 126; Y <= 130; ++Y)
                    {
                        for(int X = 126; X <= 130; ++X)
                        {
                            int Value[3];
                            Rgb(InPixels, X, Y, Value);
                            Sum += Value[0] + Value[1] + Value[2];
                        }
                    }
                    return Sum / 75;
                };
                int Outside[3];
                Rgb(Legacy, 20, 128, Outside);
                const int SurfaceGrey = Centre(Surface), LegacyGrey = Centre(Legacy);
                std::printf("    shiny pane: Surface %d, Legacy %d, the backdrop %d\n", SurfaceGrey, LegacyGrey, (Outside[0] + Outside[1] + Outside[2]) / 3);
                Expect(SurfaceGrey - LegacyGrey > 20, "Translucency Lighting Surface keeps a clear pane's highlight");
                /* Legacy keeps 2% of an HDR highlight, which the tone map still shows a little. */
                const int Backdrop = (Outside[0] + Outside[1] + Outside[2]) / 3;
                Expect(LegacyGrey - Backdrop < (SurfaceGrey - Backdrop) / 2, "and Legacy fades it with the opacity, as before");
            }
            World.DestroyEntity(Back);
            World.DestroyEntity(Pane);
            Sun.GetComponent<GameCore::TransformComponent>()->Rotation = Vector3(35.0, 10.0, 0.0);
        }

        /* Phase 5: a Volume quad over black with the sun behind it, shining toward the camera.
         * Scattering forward (g 0.6) it is brighter than scattering back (g -0.6); with Normal
         * Influence 0 it is lit evenly across, not by its facing. */
        {
            Sun.GetComponent<GameCore::TransformComponent>()->Rotation = Vector3(20.0, 180.0, 0.0);
            Sun.GetComponent<DirectionalLightComponent>()->Intensity = 0.15f;   // forward scattering is ten times the light: below the tone map's top
            const GameCore::Entity Back = Placed("Back", Vector3(0.0, 100.0, 120.0), Vector3(), Vector3(800.0, 800.0, 2.0));
            GameCore::Entity Puff = Placed("Puff", Vector3(0.0, 100.0, 0.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            Back.AddComponent(Drawn(BOX, "OpaqueBlack"));
            Back.GetComponent<RenderComponent>()->bCastShadows = 0;   // it stands between the sun and the quad
            Puff.AddComponent(Drawn(BOX, "VolumeForward"));
            EngineCore::Image Forward, Backward;
            const bool bForward = Capture(Forward);
            World.DestroyEntity(Puff);
            Puff = Placed("PuffBack", Vector3(0.0, 100.0, 0.0), Vector3(), Vector3(200.0, 200.0, 2.0));
            Puff.AddComponent(Drawn(BOX, "VolumeBack"));
            const bool bBackward = Capture(Backward);
            Expect(bForward && bBackward, "the Volume quad is captured scattering forward and back");
            if(bForward && bBackward)
            {
                Dump(Forward, "volume-forward");
                Dump(Backward, "volume-back");
                const auto Grey = [&](const EngineCore::Image& InPixels, const int InX)
                {
                    int Sum = 0;
                    for(int Y = 118; Y <= 138; ++Y)
                    {
                        int Value[3];
                        Rgb(InPixels, InX, Y, Value);
                        Sum += Value[0] + Value[1] + Value[2];
                    }
                    return Sum / (21 * 3);
                };
                const int ForwardMid = Grey(Forward, 128), BackMid = Grey(Backward, 128);
                const int ForwardLeft = Grey(Forward, 90), ForwardRight = Grey(Forward, 166);
                std::printf("    volume: forward %d, back %d; forward across %d | %d | %d\n", ForwardMid, BackMid, ForwardLeft, ForwardMid,
                            ForwardRight);
                Expect(ForwardMid - BackMid > 15, "a Volume medium scattering forward is brighter against the light than one scattering back");
                Expect(std::abs(ForwardLeft - ForwardRight) < 8, "and with Normal Influence 0 it is lit evenly across");
            }
            World.DestroyEntity(Back);
            World.DestroyEntity(Puff);
            Sun.GetComponent<GameCore::TransformComponent>()->Rotation = Vector3(35.0, 10.0, 0.0);
            Sun.GetComponent<DirectionalLightComponent>()->Intensity   = SunInit.Intensity;
        }

        /* Phase 7: a cloud of half-opaque particles that draw nothing (an opacity-0
         * material) but cast, under a sun straight down, over a white floor. The floor under
         * it is darker with translucent shadows on than off; an emitter of Shadow Density 0
         * leaves it as off; the floor away from it is the same either way. */
        ConfigVar* Translucent = ConfigFindVarByPath("Shadows.TranslucentShadows");
        Expect(Translucent != nullptr, "[Shadows] TranslucentShadows is a config var");
        if(Translucent)
        {
            std::filesystem::create_directories("Saved/TranslucencySmoke");
            const auto WriteSystem = [](const char* InPath, const char* InDensity)
            {
                std::ofstream File(InPath, std::ios::binary);
                File << "NimbleParticleSystem 1\n\nEmitter Puffs\n    Enabled true\n    Type Sprite\n"
                        "    Material Engine/Content/Materials/Testing/Invisible.material.nmgraph\n"
                        "    SpawnMode Burst\n    MaxParticles 64\n    BurstCount 48\n    BurstInterval 0\n    BurstCycles 1\n"
                        "    Lifetime 60\n    LifetimeVariance 0\n    EmitterRadius 25\n    Velocity 0,0,0\n    VelocitySpread 0\n"
                        "    StartSize 200\n    EndSize 200\n    StartColor 255,255,255,128\n    EndColor 255,255,255,128\n"
                        "    FadeIn 0\n    FadeOut 0\n    PauseOffscreen false\n    CastShadows true\n    ShadowDensity "
                     << InDensity << "\n";
            };
            WriteSystem("Saved/TranslucencySmoke/Casting.nmparticle", "1");
            WriteSystem("Saved/TranslucencySmoke/Faint.nmparticle", "0");

            Sun.GetComponent<GameCore::TransformComponent>()->Rotation = Vector3(90.0, 0.0, 0.0);
            const GameCore::Entity Floor = Placed("Floor", Vector3(0.0, -1.0, 300.0), Vector3(), Vector3(2000.0, 2.0, 2000.0));
            Floor.AddComponent(Drawn(BOX, "OpaqueWhite"));
            const auto Cloud = [&](const char* InSystem)
            {
                const GameCore::Entity Each = Placed("Cloud", Vector3(0.0, 220.0, 300.0), Vector3(), Vector3(1.0, 1.0, 1.0));
                ParticleSystemComponent System;
                std::snprintf(System.System, sizeof(System.System), "%s", InSystem);
                Each.AddComponent(System);
                return Each;
            };
            const auto FloorGrey = [&](const EngineCore::Image& InPixels, const int InX)
            {
                int Sum = 0;
                for(int Y = 154; Y <= 162; ++Y)
                {
                    for(int X = InX - 4; X <= InX + 4; ++X)
                    {
                        int Value[3];
                        Rgb(InPixels, X, Y, Value);
                        Sum += Value[0] + Value[1] + Value[2];
                    }
                }
                return Sum / (81 * 3);
            };
            EngineCore::Image On, Off, Faint;
            GameCore::Entity Casting = Cloud("Saved/TranslucencySmoke/Casting.nmparticle");
            Translucent->SetFromString("1");
            const bool bOn = Capture(On);
            Translucent->SetFromString("0");
            const bool bOff = Capture(Off);
            Translucent->SetFromString("1");
            World.DestroyEntity(Casting);
            Casting = Cloud("Saved/TranslucencySmoke/Faint.nmparticle");
            const bool bFaint = Capture(Faint);
            Expect(bOn && bOff && bFaint, "the casting cloud is captured with translucent shadows on, off and at density 0");
            if(bOn && bOff && bFaint)
            {
                Dump(On, "cloud-shadow-on");
                Dump(Off, "cloud-shadow-off");
                const int UnderOn = FloorGrey(On, 128), UnderOff = FloorGrey(Off, 128), UnderFaint = FloorGrey(Faint, 128);
                const int AsideOn = FloorGrey(On, 24), AsideOff = FloorGrey(Off, 24);
                std::printf("    cloud: floor under it %d on, %d off, %d at density 0; aside %d on, %d off\n", UnderOn, UnderOff,
                            UnderFaint, AsideOn, AsideOff);
                Expect(UnderOn < UnderOff * 85 / 100, "particles with Cast Shadows darken the floor under them (translucent shadows)");
                Expect(std::abs(UnderFaint - UnderOff) < 4, "an emitter of Shadow Density 0 casts none");
                Expect(std::abs(AsideOn - AsideOff) < 4, "and the floor away from them is unchanged");
            }
            World.DestroyEntity(Casting);
            World.DestroyEntity(Floor);

            /* Phase 8: single casting Volume particles under the same sun. A lone one keeps at
             * least nine tenths of its light with translucent shadows on (it is not shadowed by
             * itself); one with another straight above it is darker than it was alone. */
            const auto WriteOne = [](const char* InPath)
            {
                std::ofstream File(InPath, std::ios::binary);
                File << "NimbleParticleSystem 1\n\nEmitter One\n    Enabled true\n    Type Sprite\n"
                        "    Material Engine/Content/Materials/Testing/VolumeForward.material.nmgraph\n"
                        "    SpawnMode Burst\n    MaxParticles 1\n    BurstCount 1\n    BurstInterval 0\n    BurstCycles 1\n"
                        "    Lifetime 60\n    LifetimeVariance 0\n    EmitterRadius 0\n    Velocity 0,0,0\n    VelocitySpread 0\n"
                        "    StartSize 120\n    EndSize 120\n    StartColor 255,255,255,180\n    EndColor 255,255,255,180\n"
                        "    FadeIn 0\n    FadeOut 0\n    PauseOffscreen false\n    CastShadows true\n    ShadowDensity 1\n";
            };
            WriteOne("Saved/TranslucencySmoke/One.nmparticle");
            const auto Single = [&](const double InHeight)
            {
                const GameCore::Entity Each = Placed("One", Vector3(0.0, InHeight, 300.0), Vector3(), Vector3(1.0, 1.0, 1.0));
                ParticleSystemComponent System;
                std::snprintf(System.System, sizeof(System.System), "%s", "Saved/TranslucencySmoke/One.nmparticle");
                Each.AddComponent(System);
                return Each;
            };
            const auto Centre = [&](const EngineCore::Image& InPixels, const int InY)
            {
                int Sum = 0;
                for(int Y = InY - 3; Y <= InY + 3; ++Y)
                {
                    for(int X = 125; X <= 131; ++X)
                    {
                        int Value[3];
                        Rgb(InPixels, X, Y, Value);
                        Sum += Value[0] + Value[1] + Value[2];
                    }
                }
                return Sum / (49 * 3);
            };
            const GameCore::Entity Lower = Single(200.0);
            EngineCore::Image AloneOn, AloneOff, Paired;
            Translucent->SetFromString("0");
            const bool bAloneOff = Capture(AloneOff);
            Translucent->SetFromString("1");
            const bool bAloneOn = Capture(AloneOn);
            const GameCore::Entity Upper = Single(420.0);
            const bool bPaired = Capture(Paired);
            Expect(bAloneOff && bAloneOn && bPaired, "the single particles are captured alone and paired");
            if(bAloneOff && bAloneOn && bPaired)
            {
                Dump(AloneOn, "particle-alone");
                Dump(Paired, "particle-paired");
                /* The lower particle at y 200, 900 in front of the eye at y 100: 30 pixels above the middle. */
                const int Off = Centre(AloneOff, 97), On = Centre(AloneOn, 97), Below = Centre(Paired, 97);
                std::printf("    self-shadow: alone %d off, %d on; under another %d\n", Off, On, Below);
                Expect(On * 10 >= Off * 9, "a lone casting particle is not shadowed by itself (at least nine tenths of its light)");
                Expect(Below * 10 < On * 9, "and one under another is shadowed by it");
            }
            World.DestroyEntity(Lower);

            /* Ambient self-occlusion: the sun nearly off, so the sky lights the lower particle
             * alone; under the casting one, a medium with Self Occlusion 1 is darker than one
             * with 0. */
            const auto WriteMaterial = [](const char* InPath, const char* InMaterial)
            {
                std::ofstream File(InPath, std::ios::binary);
                File << "NimbleParticleSystem 1\n\nEmitter One\n    Enabled true\n    Type Sprite\n"
                        "    Material Engine/Content/Materials/Testing/" << InMaterial << ".material.nmgraph\n"
                        "    SpawnMode Burst\n    MaxParticles 1\n    BurstCount 1\n    BurstInterval 0\n    BurstCycles 1\n"
                        "    Lifetime 60\n    LifetimeVariance 0\n    EmitterRadius 0\n    Velocity 0,0,0\n    VelocitySpread 0\n"
                        "    StartSize 120\n    EndSize 120\n    StartColor 255,255,255,180\n    EndColor 255,255,255,180\n"
                        "    FadeIn 0\n    FadeOut 0\n    PauseOffscreen false\n";
            };
            WriteMaterial("Saved/TranslucencySmoke/Occluded.nmparticle", "VolumeOccluded");
            WriteMaterial("Saved/TranslucencySmoke/Open.nmparticle", "VolumeOpen");
            const auto Receiver = [&](const char* InSystem)
            {
                const GameCore::Entity Each = Placed("Receiver", Vector3(0.0, 200.0, 300.0), Vector3(), Vector3(1.0, 1.0, 1.0));
                ParticleSystemComponent System;
                std::snprintf(System.System, sizeof(System.System), "%s", InSystem);
                Each.AddComponent(System);
                return Each;
            };
            Sun.GetComponent<DirectionalLightComponent>()->Intensity = 0.02f;
            EngineCore::Image Occluded, Open;
            GameCore::Entity Under = Receiver("Saved/TranslucencySmoke/Occluded.nmparticle");
            const bool bOccluded = Capture(Occluded);
            World.DestroyEntity(Under);
            Under = Receiver("Saved/TranslucencySmoke/Open.nmparticle");
            const bool bOpen = Capture(Open);
            World.DestroyEntity(Under);
            World.DestroyEntity(Upper);
            Sun.GetComponent<DirectionalLightComponent>()->Intensity = SunInit.Intensity;
            Expect(bOccluded && bOpen, "the receiver under a casting particle is captured with Self Occlusion 1 and 0");
            if(bOccluded && bOpen)
            {
                const int Dark = Centre(Occluded, 97), Lit = Centre(Open, 97);
                std::printf("    self-occlusion: under a caster, the sky's light %d with it, %d without\n", Dark, Lit);
                Expect(Dark * 10 < Lit * 9, "Self Occlusion darkens a medium's sky light by the medium toward the sun");
            }

            /* Glass casting: a flat pane (a skylight) over the floor with Cast Translucent Shadow at
             * opacity 0.5. The floor under it is darker with translucent shadows on, never black. */
            const GameCore::Entity GlassFloor = Placed("GlassFloor", Vector3(0.0, -1.0, 300.0), Vector3(), Vector3(2000.0, 2.0, 2000.0));
            GlassFloor.AddComponent(Drawn(BOX, "OpaqueWhite"));
            const GameCore::Entity Skylight = Placed("Skylight", Vector3(0.0, 160.0, 300.0), Vector3(), Vector3(240.0, 2.0, 240.0));
            Skylight.AddComponent(Drawn(BOX, "GlassCaster"));
            EngineCore::Image GlassOn, GlassOff;
            Translucent->SetFromString("1");
            const bool bGlassOn = Capture(GlassOn);
            Translucent->SetFromString("0");
            const bool bGlassOff = Capture(GlassOff);
            Translucent->SetFromString("1");
            Expect(bGlassOn && bGlassOff, "the skylight is captured with translucent shadows on and off");
            if(bGlassOn && bGlassOff)
            {
                Dump(GlassOn, "glass-shadow-on");
                const int UnderOn = FloorGrey(GlassOn, 128), UnderOff = FloorGrey(GlassOff, 128);
                std::printf("    glass: floor under the skylight %d on, %d off\n", UnderOn, UnderOff);
                Expect(UnderOn < UnderOff * 85 / 100, "a Translucent material with Cast Translucent Shadow casts a see-through shadow");
                Expect(UnderOn > UnderOff * 20 / 100, "which lets light through");
            }
            World.DestroyEntity(Skylight);
            World.DestroyEntity(GlassFloor);
            Sun.GetComponent<GameCore::TransformComponent>()->Rotation = Vector3(35.0, 10.0, 0.0);
        }

        /* Refraction (LIT_TRANSLUCENCY.md, Phases 11-12): a backdrop red on the left and white on
         * the right, and a clear pane (Opacity 0, no specular) turned 35 degrees about its vertical
         * axis between it and the eye. Refracting at 1.33, Lit or Unlit, it moves the colour edge
         * behind it and nothing outside it; an opaque block in front of it does not bleed into the
         * pane's pixels beside it. */
        {
            const GameCore::Entity Left  = Placed("BackRed", Vector3(-250.0, 100.0, 150.0), Vector3(), Vector3(500.0, 800.0, 2.0));
            const GameCore::Entity Right = Placed("BackWhite", Vector3(250.0, 100.0, 150.0), Vector3(), Vector3(500.0, 800.0, 2.0));
            Left.AddComponent(Drawn(BOX, "OpaqueRed"));
            Right.AddComponent(Drawn(BOX, "OpaqueWhite"));
            /* Where row InY turns from red to white; -1 if it does not. */
            const auto EdgeAt = [&](const EngineCore::Image& InPixels, const int InY)
            {
                for(int X = 40; X < 216; ++X)
                {
                    int Value[3];
                    Rgb(InPixels, X, InY, Value);
                    if(Value[1] > 100 && Value[2] > 100) { return X; }   // white: green and blue up with red
                }
                return -1;
            };
            /* Near-black pixels round the pane's centre: the block, and anything bled from it. */
            const auto BlackAround = [&](const EngineCore::Image& InPixels)
            {
                int Count = 0;
                for(int Y = 100; Y < 156; ++Y)
                {
                    for(int X = 80; X < 176; ++X)
                    {
                        int Value[3];
                        Rgb(InPixels, X, Y, Value);
                        Count += Value[0] + Value[1] + Value[2] < 120 ? 1 : 0;
                    }
                }
                return Count;
            };

            EngineCore::Image Plain;
            const bool bPlain = Capture(Plain);
            for(const char* Each : { "RefractLit", "RefractUnlit" })
            {
                const GameCore::Entity Pane = Placed("Pane", Vector3(0.0, 100.0, 0.0), Vector3(0.0, 35.0, 0.0), Vector3(200.0, 200.0, 2.0));
                Pane.AddComponent(Drawn(BOX, Each));
                EngineCore::Image Bent;
                const bool bBent = Capture(Bent);
                Expect(bPlain && bBent, "the backdrop is captured without and with a refracting pane");
                if(bPlain && bBent)
                {
                    Dump(Bent, Each);
                    const int Before = EdgeAt(Plain, 128), After = EdgeAt(Bent, 128);
                    const int Above = EdgeAt(Plain, 30), AboveBent = EdgeAt(Bent, 30);
                    std::printf("    %s: the edge at x %d through the pane, %d without; above it %d and %d\n",
                                Each, After, Before, AboveBent, Above);
                    Expect(Before > 0 && After > 0 && std::abs(After - Before) >= 3, "a refracting pane moves the edge behind it");
                    Expect(Above > 0 && std::abs(AboveBent - Above) <= 1, "and nothing outside it");
                }

                /* A black block in front of the pane, beside the edge: through the pane the backdrop
                 * shows round it, never the block (it is nearer than the pane). */
                const GameCore::Entity Block = Placed("Block", Vector3(-25.0, 100.0, -150.0), Vector3(), Vector3(30.0, 30.0, 10.0));
                Block.AddComponent(Drawn(BOX, "OpaqueBlack"));
                EngineCore::Image Blocked;
                const bool bBlocked = Capture(Blocked);
                World.DestroyEntity(Pane);
                EngineCore::Image BlockOnly;
                const bool bBlockOnly = Capture(BlockOnly);
                World.DestroyEntity(Block);
                Expect(bBlocked && bBlockOnly, "the block is captured with and without the pane behind it");
                if(bBlocked && bBlockOnly)
                {
                    Dump(Blocked, (std::string(Each) + "-block").c_str());
                    const int With = BlackAround(Blocked), Without = BlackAround(BlockOnly);
                    std::printf("    %s: %d black pixels round the block with the pane, %d without\n", Each, With, Without);
                    Expect(Without > 50 && With <= Without + 24, "what is in front of a refracting pane does not bleed into it");
                }
            }
            World.DestroyEntity(Left);
            World.DestroyEntity(Right);
        }

        /* Far faces (LIT_TRANSLUCENCY.md, "Far faces"): a cube at opacity 0.5 over black. One-sided,
         * its faces away from the eye are culled and one layer covers the backdrop; Two Sided, both
         * draw, the far one under the near, and the centre is brighter by a second layer. */
        {
            const GameCore::Entity Back = Placed("Back", Vector3(0.0, 100.0, 300.0), Vector3(), Vector3(800.0, 800.0, 2.0));
            Back.AddComponent(Drawn(BOX, "OpaqueBlack"));
            int Grey[2] = {};
            const char* Names[2] = { "TransUnlitHalf", "TransUnlitHalfTwoSided" };
            bool bBoth = true;
            for(int Index = 0; Index < 2; ++Index)
            {
                const GameCore::Entity Cube = Placed("Cube", Vector3(0.0, 100.0, 0.0), Vector3(20.0, 30.0, 0.0), Vector3(150.0, 150.0, 150.0));
                Cube.AddComponent(Drawn(BOX, Names[Index]));
                EngineCore::Image Pixels;
                const bool bCaptured = Capture(Pixels);
                bBoth = bBoth && bCaptured;
                if(bCaptured)
                {
                    Dump(Pixels, Names[Index]);
                    int Value[3];
                    Rgb(Pixels, 128, 128, Value);
                    Grey[Index] = (Value[0] + Value[1] + Value[2]) / 3;
                }
                World.DestroyEntity(Cube);
            }
            Expect(bBoth, "the half-covering cube is captured one-sided and Two Sided");
            if(bBoth)
            {
                std::printf("    far faces: the cube's centre %d one-sided, %d Two Sided\n", Grey[0], Grey[1]);
                Expect(Grey[0] > 20 && Grey[1] - Grey[0] > 10, "a one-sided Translucent mesh culls its far faces; a Two Sided one draws them");
            }
            World.DestroyEntity(Back);
        }

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}
#endif // WITH_EDITOR

#if WITH_EDITOR
/*
 * Parallel draw recording on the GPU (NIMBLE_SMOKE_GPU=Vulkan;
 * MULTITHREADED_RENDERING.md, phase 3): a floor and a grid of boxes under a
 * cascaded sun, captured with [Rendering] ParallelRecording Off and On. The
 * chunks record the same draws in the same order into secondaries, so the
 * two images are the same byte for byte, and only the On capture counts
 * parallel passes.
 */
static void TestParallelRecordingOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[parallel recording on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[parallel recording on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    /* Workers before the renderer, so the gate sees somewhere to run chunks. */
    EngineCore::JobSystem Jobs(4);
    EngineCore::JobSystem* PreviousJobs = EngineCore::GetJobSystem();
    EngineCore::SetJobSystem(&Jobs);

    ConfigVar* ModeVar  = ConfigFindVarByPath("Rendering.ParallelRecording");
    ConfigVar* MinDraws = ConfigFindVarByPath("Rendering.ParallelMinDraws");
    Expect(ModeVar != nullptr && MinDraws != nullptr, "[Rendering] ParallelRecording and ParallelMinDraws are config vars");

    Renderer Rend;
    const bool bInit = ModeVar && MinDraws && Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakePerspective(Viewport, Vector3(0.0, 1400.0, -1600.0), Vector3(0.0, -1400.0, 1600.0),
                                                            50.0f, 10.0f, 8000.0f);
        Rend.SetViewOverride(&View);
        GameCore::World World;
        Rend.BindWorld(World);

        const auto Placed = [&World](const std::string& InName, const Vector3& InPosition, const Vector3& InRotation, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName.c_str());
            GameCore::TransformComponent Transform;
            Transform.Position = InPosition;
            Transform.Rotation = InRotation;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            return Each;
        };
        const auto Mesh = [](const char* InPath)
        {
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InPath);
            return Render;
        };

        Placed("Floor", Vector3(), Vector3(), Vector3(4000.0, 1.0, 4000.0)).AddComponent(Mesh("Engine/Content/Meshes/Plane.nmmesh"));
        for(int Z = 0; Z < 20; ++Z)
        {
            for(int X = 0; X < 20; ++X)
            {
                const double Height = 30.0 + 10.0 * ((X * 7 + Z * 3) % 9);
                Placed("Box" + std::to_string(Z * 20 + X), Vector3(-1520.0 + X * 160.0, Height * 0.5, -1520.0 + Z * 160.0),
                       Vector3(0.0, (X + Z) * 9.0, 0.0), Vector3(60.0, Height, 60.0))
                    .AddComponent(Mesh("Engine/Content/Meshes/Box.nmmesh"));
            }
        }
        DirectionalLightComponent SunInit;
        SunInit.ShadowTechnique = LST_Cascaded;
        SunInit.ShadowDistance  = 5000.0f;
        Placed("Sun", Vector3(), Vector3(50.0, 60.0, 0.0), Vector3(1.0, 1.0, 1.0)).AddComponent(SunInit);
        Placed("Sky", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(SkyLightComponent());

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            const uint64_t Request = Rend.RequestSceneCapture();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Passes = [&Rend]() { RenderStats Stats; Rend.GetStats(Stats); return Stats.ParallelPasses; };
        const auto Differing = [](const EngineCore::Image& InA, const EngineCore::Image& InB)
        {
            int Out = 0;
            for(int Y = 0; Y < int(InA.GetHeight()); ++Y)
            {
                Out += std::memcmp(InA.GetData() + InA.GetStride() * Y, InB.GetData() + InB.GetStride() * Y,
                                   InA.GetWidth() * BytesPerPixel(InA.GetFormat())) != 0 ? 1 : 0;
            }
            return Out;
        };

        EngineCore::Image Serial, Parallel, SerialAgain;
        ModeVar->SetString("Off");
        const uint64_t PassesBefore = Passes();
        const bool bSerial = Capture(Serial);
        const uint64_t PassesSerial = Passes();
        ModeVar->SetString("On");
        MinDraws->SetFromString("1");
        const bool bParallel = Capture(Parallel);
        const uint64_t PassesParallel = Passes();
        ModeVar->SetString("Off");
        const bool bSerialAgain = Capture(SerialAgain);
        std::printf("    parallel passes: %llu off, %llu on\n", (unsigned long long)(PassesSerial - PassesBefore),
                    (unsigned long long)(PassesParallel - PassesSerial));
        Expect(bSerial && bParallel && bSerialAgain, "the scene is captured with parallel recording off, on and off again");
        Expect(PassesSerial == PassesBefore, "Off records no stage in parallel");
        Expect(PassesParallel > PassesSerial, "On records the base pass and the shadows in parallel");
        if(bSerial && bParallel && bSerialAgain)
        {
            const int Rows = Differing(Serial, Parallel);
            std::printf("    rows differing serial vs parallel: %d, serial vs serial: %d\n", Rows, Differing(Serial, SerialAgain));
            Expect(Differing(Serial, SerialAgain) == 0, "the scene captures the same twice (the comparison means something)");
            Expect(Rows == 0, "recorded in parallel, the image is the same byte for byte");
        }

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    if(ModeVar)  { ModeVar->SetString("Auto"); }
    if(MinDraws) { MinDraws->SetFromString("256"); }
    EngineCore::SetJobSystem(PreviousJobs);
    SetFileProvider(PreviousProvider);
}
#endif // WITH_EDITOR

#if WITH_EDITOR
/*
 * The post process stack on the GPU (NIMBLE_SMOKE_GPU=Vulkan): a lit yard of
 * boxes captured plain and through each effect, compared pixel by pixel.
 * Bloom only adds light; FXAA touches edges and nothing else; Depth of Field
 * flattens edges; a volume the camera is far outside does nothing; Motion
 * Blur smears only when the camera moved since the last frame.
 */
static void TestPostProcessOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[post process on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[post process on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    /* Stacks as the editor saves them, beside the executable. */
    std::error_code Ignored;
    std::filesystem::create_directories("SmokeScratch/PostStacks", Ignored);
    const auto WriteStack = [](const char* InName, const char* InStack)
    {
        const std::string Path = std::string("SmokeScratch/PostStacks/") + InName + ".pp.nmgraph";
        std::ofstream Out(Path);
        Out << "<?xml version=\"1.0\"?>\n<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <gen_code>\n"
               "    <Output Extension=\".ppstack\"><![CDATA[" << InStack << "]]></Output>\n  </gen_code>\n</NodeGraph>\n";
        PostProcessStackCache::Invalidate(Path);
        return Path;
    };
    const std::string BloomStack  = WriteStack("Bloom", "Effect Bloom Threshold=0.85 Intensity=0.8 Radius=0.75 Levels=5 Tint=1,1,1\n");
    const std::string FxaaStack   = WriteStack("Fxaa", "Effect Antialiasing EdgeThreshold=0.1 Method=FXAA Quality=High\n");
    const std::string DofStack    = WriteStack("Dof", "Effect DepthOfField FocusDistance=50 FocusRange=10 MaxBlur=16 NearBlur=false\n");
    const std::string MotionStack = WriteStack("Motion", "Effect MotionBlur Intensity=1 MaxLength=48 Samples=12\n");
    const std::string AoStack     = WriteStack("Ao", "Effect AmbientOcclusion Intensity=1.5 Radius=80 Samples=16 Power=1.5 Blur=true\n");

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView Still = RenderView::MakePerspective(Viewport, Vector3(0.0, 500.0, -700.0), Vector3(0.0, -500.0, 700.0),
                                                             50.0f, 10.0f, 5000.0f);
        const RenderView Turned = RenderView::MakePerspective(Viewport, Vector3(0.0, 500.0, -700.0), Vector3(150.0, -500.0, 700.0),
                                                              50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&Still);
        GameCore::World World;
        Rend.BindWorld(World);

        const auto Placed = [&World](const char* InName, const Vector3& InPosition, const Vector3& InRotation, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InPosition;
            Transform.Rotation = InRotation;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            return Each;
        };
        const auto Mesh = [](const char* InPath)
        {
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InPath);
            return Render;
        };
        Placed("Floor", Vector3(), Vector3(), Vector3(3000.0, 1.0, 3000.0)).AddComponent(Mesh("Engine/Content/Meshes/Plane.nmmesh"));
        for(int Index = 0; Index < 5; ++Index)
        {
            Placed("Box", Vector3(-400.0 + 200.0 * Index, 60.0, 100.0 * (Index % 3)), Vector3(0.0, 25.0 * Index, 0.0),
                   Vector3(90.0, 120.0, 90.0)).AddComponent(Mesh("Engine/Content/Meshes/Box.nmmesh"));
        }
        DirectionalLightComponent SunInit;
        SunInit.bCastShadows = 0;
        Placed("Sun", Vector3(), Vector3(50.0, 30.0, 0.0), Vector3(1.0, 1.0, 1.0)).AddComponent(SunInit);
        Placed("Sky", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(SkyLightComponent());

        /* The volume: everywhere until it is given Bounds. */
        PostProcessComponent VolumeInit;
        VolumeInit.bEnabled = 0;
        const GameCore::Entity Volume = Placed("Volume", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0));
        Volume.AddComponent(VolumeInit);
        const auto UseStack = [&Volume](const std::string& InStack)
        {
            PostProcessComponent* Each = Volume.GetComponent<PostProcessComponent>();
            std::snprintf(Each->Stack, sizeof(Each->Stack), "%s", InStack.c_str());
            Each->bEnabled = InStack.empty() ? 0 : 1;
        };

        uint64_t Frame = 1;
        const auto Draw = [&](const RenderView& InView)
        {
            Rend.SetViewOverride(&InView);
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
        };
        /* Two frames at InFirst, then the capture's frame at InView. */
        const auto Capture = [&](EngineCore::Image& OutPixels, const RenderView& InFirst, const RenderView& InView)
        {
            Draw(InFirst);
            Draw(InFirst);
            const uint64_t Request = Rend.RequestSceneCapture();
            Draw(InView);
            Draw(InView);
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Luma = [](const EngineCore::Image& InPixels, const int InX, const int InY)
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            return int(Px[0]) + int(Px[1]) + int(Px[2]);
        };
        struct Difference { int Brighter = 0; int Darker = 0; int Changed = 0; };
        const auto Compare = [&](const EngineCore::Image& InA, const EngineCore::Image& InB)
        {
            Difference Out;
            for(int Y = 0; Y < 256; ++Y)
            {
                for(int X = 0; X < 256; ++X)
                {
                    const int Delta = Luma(InA, X, Y) - Luma(InB, X, Y);
                    Out.Brighter += Delta > 24 ? 1 : 0;
                    Out.Darker   += Delta < -24 ? 1 : 0;
                    Out.Changed  += Delta != 0 ? 1 : 0;
                }
            }
            return Out;
        };
        /* How much neighbouring pixels differ: what blur takes away. */
        const auto EdgeEnergy = [&](const EngineCore::Image& InPixels)
        {
            double Sum = 0.0;
            for(int Y = 0; Y < 255; ++Y)
            {
                for(int X = 0; X < 255; ++X)
                {
                    Sum += std::abs(Luma(InPixels, X + 1, Y) - Luma(InPixels, X, Y)) + std::abs(Luma(InPixels, X, Y + 1) - Luma(InPixels, X, Y));
                }
            }
            return Sum;
        };
        const char* DumpDir = std::getenv("NIMBLE_SMOKE_DUMP");
        const auto Dump = [&](const EngineCore::Image& InPixels, const char* InName)
        {
            if(!DumpDir || !DumpDir[0] || InPixels.GetWidth() == 0) { return; }
            std::ofstream File(std::string(DumpDir) + "/" + InName + ".ppm", std::ios::binary);
            File << "P6\n" << InPixels.GetWidth() << " " << InPixels.GetHeight() << "\n255\n";
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            for(int Y = 0; Y < int(InPixels.GetHeight()); ++Y)
            {
                for(int X = 0; X < int(InPixels.GetWidth()); ++X)
                {
                    const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * Y + X * BytesPerPixel(InPixels.GetFormat());
                    const char Bytes[3] = { char(bBgra ? Px[2] : Px[0]), char(Px[1]), char(bBgra ? Px[0] : Px[2]) };
                    File.write(Bytes, 3);
                }
            }
        };

        EngineCore::Image Plain, Bloomed, Smoothed, Focused, Outside, PlainTurned, Moving, Steady;
        UseStack(std::string());
        const bool bPlain = Capture(Plain, Still, Still);
        Expect(bPlain, "the plain scene is captured");
        Dump(Plain, "post-plain");

        UseStack(BloomStack);
        if(bPlain && Capture(Bloomed, Still, Still))
        {
            Dump(Bloomed, "post-bloom");
            const Difference Glow = Compare(Bloomed, Plain);
            std::printf("    bloom: %d brighter, %d darker\n", Glow.Brighter, Glow.Darker);
            Expect(Glow.Brighter > 300 && Glow.Darker < 20, "Bloom only adds light, around what is bright");
        }

        UseStack(FxaaStack);
        if(bPlain && Capture(Smoothed, Still, Still))
        {
            Dump(Smoothed, "post-fxaa");
            const Difference Edges = Compare(Smoothed, Plain);
            std::printf("    fxaa: %d pixels changed, edge energy %.0f -> %.0f\n", Edges.Changed, EdgeEnergy(Plain), EdgeEnergy(Smoothed));
            Expect(Edges.Changed > 30 && Edges.Changed < 256 * 256 / 6, "FXAA changes the pixels on edges, not the flat areas");
            Expect(EdgeEnergy(Smoothed) < EdgeEnergy(Plain), "and softens them");
        }

        UseStack(DofStack);
        if(bPlain && Capture(Focused, Still, Still))
        {
            Dump(Focused, "post-dof");
            std::printf("    depth of field: edge energy %.0f -> %.0f\n", EdgeEnergy(Plain), EdgeEnergy(Focused));
            Expect(EdgeEnergy(Focused) < EdgeEnergy(Plain) * 0.75, "Depth of Field blurs a scene far past its focus");
        }

        /* Ambient Occlusion: the floor round each box darkens; open floor stays. */
        EngineCore::Image Occluded;
        UseStack(AoStack);
        if(bPlain && Capture(Occluded, Still, Still))
        {
            Dump(Occluded, "post-ao");
            const Difference Creases = Compare(Occluded, Plain);
            std::printf("    ambient occlusion: %d darker, %d brighter, %d changed\n", Creases.Darker, Creases.Brighter, Creases.Changed);
            Expect(Creases.Darker > 150 && Creases.Brighter < 20, "Ambient Occlusion darkens creases and never brightens");
            Expect(Creases.Darker < 256 * 256 / 4, "but not the open floor");
        }

        /* The Bloom stack in a volume far from the camera, with a hard edge. */
        UseStack(BloomStack);
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(Vector3(-50.0, -50.0, -50.0), Vector3(50.0, 50.0, 50.0));
        Volume.GetComponent<GameCore::TransformComponent>()->Position = Vector3(0.0, 0.0, 3000.0);
        Volume.AddComponent(Bounds);
        World.RefreshSpatialIndex();
        if(bPlain && Capture(Outside, Still, Still))
        {
            const Difference Far = Compare(Outside, Plain);
            std::printf("    volume far away: %d pixels changed\n", Far.Changed);
            Expect(Far.Changed < 20, "a volume the camera is outside of does nothing");
        }
        Volume.GetComponent<PostProcessComponent>()->BlendDistance = 1.0e6f;
        EngineCore::Image Near;
        if(bPlain && Capture(Near, Still, Still))
        {
            const Difference Faded = Compare(Near, Plain);
            std::printf("    volume with a wide blend: %d brighter\n", Faded.Brighter);
            Expect(Faded.Brighter > 100, "a wide enough Blend Distance reaches the camera");
        }

        /* Motion Blur: everywhere again; a frame at Still then one at Turned. */
        World.DestroyEntity(Volume);
        const GameCore::Entity Blur = Placed("Blur", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0));
        PostProcessComponent BlurInit;
        std::snprintf(BlurInit.Stack, sizeof(BlurInit.Stack), "%s", MotionStack.c_str());
        Blur.AddComponent(BlurInit);
        Blur.GetComponent<PostProcessComponent>()->bEnabled = 0;
        const bool bPlainTurned = Capture(PlainTurned, Turned, Turned);
        Blur.GetComponent<PostProcessComponent>()->bEnabled = 1;
        if(bPlainTurned && Capture(Steady, Turned, Turned))
        {
            const Difference Still_ = Compare(Steady, PlainTurned);
            std::printf("    motion blur, camera still: %d pixels changed\n", Still_.Brighter + Still_.Darker);
            Expect(Still_.Brighter + Still_.Darker < 20, "Motion Blur leaves a still camera's frame alone");
        }
        /* One frame at Still, then the captured one at Turned: it moved. */
        Draw(Still);
        Draw(Still);
        const uint64_t Request = Rend.RequestSceneCapture();
        Draw(Turned);
        Draw(Turned);
        std::string Error;
        if(bPlainTurned && Request && Rend.TakeMaterialPreview(Request, Moving, Error))
        {
            Dump(Moving, "post-motion");
            const Difference Smear = Compare(Moving, PlainTurned);
            std::printf("    motion blur, camera turned: %d pixels changed, edge energy %.0f -> %.0f\n", Smear.Brighter + Smear.Darker,
                        EdgeEnergy(PlainTurned), EdgeEnergy(Moving));
            Expect(Smear.Brighter + Smear.Darker > 200, "Motion Blur smears the frame the camera turned in");
        }

        /* Velocity: the camera still, a box that moved since the frame before
         * smears; standing still at the same place, it does not. */
        const GameCore::Entity Mover = Placed("Mover", Vector3(-120.0, 60.0, -150.0), Vector3(), Vector3(90.0, 120.0, 90.0));
        Mover.AddComponent(Mesh("Engine/Content/Meshes/Box.nmmesh"));
        const auto MoveTo = [&](const double InX)
        {
            Mover.GetComponent<GameCore::TransformComponent>()->Position = Vector3(InX, 60.0, -150.0);
        };
        EngineCore::Image MoverStill, MoverMoving;
        MoveTo(0.0);
        const bool bMoverStill = Capture(MoverStill, Turned, Turned);
        MoveTo(-120.0);
        Draw(Turned);
        Draw(Turned);
        const uint64_t MoverRequest = Rend.RequestSceneCapture();
        MoveTo(0.0);
        Draw(Turned);
        Draw(Turned);
        if(bMoverStill && MoverRequest && Rend.TakeMaterialPreview(MoverRequest, MoverMoving, Error))
        {
            Dump(MoverStill, "post-velocity-still");
            Dump(MoverMoving, "post-velocity-moving");
            const Difference Smear = Compare(MoverMoving, MoverStill);
            std::printf("    motion blur, a box moved and the camera still: %d pixels changed\n", Smear.Brighter + Smear.Darker);
            Expect(Smear.Brighter + Smear.Darker > 150, "Motion Blur smears what moved, with the camera still (velocity)");
            Expect(Smear.Brighter + Smear.Darker < 256 * 256 / 4, "and only that");
        }

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    PostProcessStackCache::Invalidate(std::string());
    SetFileProvider(PreviousProvider);
}

/*
 * "stat frame" on the GPU: with Stats.Frame on, each stage of the default
 * scene's frame is timed with GPU timestamps (GDIWriteGPUTimestamp) and
 * averaged into RenderStats::GpuMs.
 */
static void TestStatFrameOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[stat frame on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[stat frame on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);
    ConfigVar* FrameVar = ConfigFindVarByPath("Stats.Frame");

    Renderer Rend;
    const bool bInit = FrameVar && Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakePerspective(Viewport, Vector3(0.0, 700.0, -600.0), Vector3(0.0, -700.0, 600.0),
                                                            50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&View);
        GameCore::World World;
        Rend.BindWorld(World);
        const auto Placed = [&World](const char* InName, const Vector3& InPosition, const Vector3& InRotation, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InPosition;
            Transform.Rotation = InRotation;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            return Each;
        };
        RenderComponent Floor;
        std::snprintf(Floor.Mesh, sizeof(Floor.Mesh), "%s", "Engine/Content/Meshes/Plane.nmmesh");
        Placed("Floor", Vector3(), Vector3(), Vector3(3000.0, 1.0, 3000.0)).AddComponent(Floor);
        RenderComponent Box;
        std::snprintf(Box.Mesh, sizeof(Box.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        Placed("Box", Vector3(0.0, 90.0, 0.0), Vector3(), Vector3(120.0, 120.0, 120.0)).AddComponent(Box);
        DirectionalLightComponent SunInit;
        SunInit.ShadowTechnique = LST_Standard;
        SunInit.ShadowDistance  = 3000.0f;
        Placed("Sun", Vector3(), Vector3(55.0, 90.0, 0.0), Vector3(1.0, 1.0, 1.0)).AddComponent(SunInit);
        Placed("Sky", Vector3(), Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(SkyLightComponent());

        uint64_t   Frame = 1;
        const auto Run = [&](const int InMilliseconds)
        {
            const auto Start = std::chrono::steady_clock::now();
            while(std::chrono::steady_clock::now() - Start < std::chrono::milliseconds(InMilliseconds))
            {
                Rend.BeginGameFrame();
                Rend.WaitForPreviousFrame();
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            }
            Rend.WaitForPreviousFrame();
        };

        Run(100);
        RenderStats Off;
        Rend.GetStats(Off);
        Expect(Off.GpuMs[RGT_Total] < 0.0f, "with Stats.Frame off, nothing is timed");

        FrameVar->SetBool(true);
        Run(800);
        RenderStats On;
        Rend.GetStats(On);
        std::printf("   ");
        for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
        {
            std::printf(" %s %.3f", GetRenderGpuTimerName(ERenderGpuTimer(Timer)), double(On.GpuMs[Timer]));
        }
        std::printf("\n");
        bool  bAllTimed = true;
        float Stages    = 0.0f;
        for(uint32_t Timer = 0; Timer < RGT_Total; ++Timer)
        {
            bAllTimed = bAllTimed && On.GpuMs[Timer] >= 0.0f;
            Stages   += std::max(On.GpuMs[Timer], 0.0f);
        }
        Expect(bAllTimed && On.GpuMs[RGT_Total] > 0.0f, "every stage of a lit, shadowed scene is timed on the GPU");
        Expect(On.GpuMs[RGT_Total] + 0.05f >= Stages, "and the stages fit inside the total");
        Expect(On.GpuMs[RGT_Shadows] > 0.0f && On.GpuMs[RGT_BasePass] > 0.0f, "the shadow and base passes take time");
        Expect(On.GpuMs[RGT_SceneCapture] >= 0.0f, "the scene captures are timed, if only as nothing");
        Expect(On.GpuMs[RGT_Particles] >= 0.0f, "the particle passes are timed, if only as nothing");

        FrameVar->SetBool(false);
        Run(100);
        RenderStats Hidden;
        Rend.GetStats(Hidden);
        Expect(Hidden.GpuMs[RGT_Total] < 0.0f, "turning it off clears the times");

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}

/*
 * Static draw lists on the GPU: a lit, shadowed scene made static draws the
 * same pixels as dynamic, and a static box moved by a tool still moves.
 */
static void TestStaticDrawListsOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[static draw lists on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[static draw lists on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakePerspective(Viewport, Vector3(0.0, 700.0, -600.0), Vector3(0.0, -700.0, 600.0),
                                                            50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&View);
        GameCore::World World;
        Rend.BindWorld(World);
        const auto Placed = [&World](const char* InName, const Vector3& InPosition, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InPosition;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            return Each;
        };
        RenderComponent FloorMesh;
        std::snprintf(FloorMesh.Mesh, sizeof(FloorMesh.Mesh), "%s", "Engine/Content/Meshes/Plane.nmmesh");
        const GameCore::Entity Floor = Placed("Floor", Vector3(), Vector3(3000.0, 1.0, 3000.0));
        Floor.AddComponent(FloorMesh);
        RenderComponent BoxMesh;
        std::snprintf(BoxMesh.Mesh, sizeof(BoxMesh.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        const GameCore::Entity Box = Placed("Box", Vector3(0.0, 90.0, 0.0), Vector3(120.0, 120.0, 120.0));
        Box.AddComponent(BoxMesh);
        DirectionalLightComponent SunInit;
        SunInit.ShadowTechnique = LST_Standard;
        SunInit.ShadowDistance  = 3000.0f;
        GameCore::Entity Sun = World.CreateEntity("Sun");
        GameCore::TransformComponent SunAt;
        SunAt.Rotation = Vector3(55.0, 90.0, 0.0);
        Sun.AddComponent(SunAt);
        Sun.AddComponent(SunInit);
        Placed("Sky", Vector3(), Vector3(1.0, 1.0, 1.0)).AddComponent(SkyLightComponent());

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Differing = [](const EngineCore::Image& InA, const EngineCore::Image& InB)
        {
            int Count = 0;
            for(int Y = 0; Y < 256; ++Y)
            {
                const uint8_t* A = InA.GetData() + InA.GetStride() * Y;
                const uint8_t* B = InB.GetData() + InB.GetStride() * Y;
                for(int X = 0; X < 256 * 4; X += 4)
                {
                    Count += (A[X] != B[X] || A[X + 1] != B[X + 1] || A[X + 2] != B[X + 2]) ? 1 : 0;
                }
            }
            return Count;
        };

        EngineCore::Image Dynamic, Static, Moved, MovedDynamic;
        const bool bDynamic = Capture(Dynamic);
        Floor.SetMobility(GameCore::EMobility::Static);
        Box.SetMobility(GameCore::EMobility::Static);
        const bool bStatic = Capture(Static);
        RenderStats Stats;
        Rend.GetStats(Stats);
        Expect(bDynamic && bStatic && Stats.StaticProxiesDrawn == 2, "the floor and box draw from static lists");
        Expect(bDynamic && bStatic && Differing(Dynamic, Static) == 0, "and the frame is pixel for pixel the dynamic one, shadows and all");

        Box.GetComponent<GameCore::TransformComponent>()->Position = Vector3(200.0, 90.0, 0.0);
        const bool bMoved = Capture(Moved);
        Box.SetMobility(GameCore::EMobility::Dynamic);
        Floor.SetMobility(GameCore::EMobility::Dynamic);
        const bool bMovedDynamic = Capture(MovedDynamic);
        std::printf("    moved static box: %d pixels changed; vs dynamic %d\n", Differing(Static, Moved), Differing(Moved, MovedDynamic));
        Expect(bMoved && Differing(Static, Moved) > 200, "a static box moved by a tool moves on screen");
        Expect(bMoved && bMovedDynamic && Differing(Moved, MovedDynamic) == 0, "exactly where a dynamic one would be");

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}

/*
 * The background gradient (Renderer::SetClearGradient): top colour at the top
 * of the view, bottom at the bottom, under what is drawn; SetClearColor back
 * to one colour.
 */
static void TestClearGradientOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[background gradient on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[background gradient on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = RenderView::MakePerspective(Viewport, Vector3(0.0, 300.0, -800.0), Vector3(0.0, -300.0, 800.0),
                                                            50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&View);
        GameCore::World World;
        Rend.BindWorld(World);
        /* A lit box in the middle, to show the gradient stays under it. */
        const GameCore::Entity Box = World.CreateEntity("Box");
        GameCore::TransformComponent At;
        At.Scale = Vector3(120.0, 120.0, 120.0);
        Box.AddComponent(At);
        RenderComponent Mesh;
        std::snprintf(Mesh.Mesh, sizeof(Mesh.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        Box.AddComponent(Mesh);
        World.CreateEntity("Sky").AddComponent(SkyLightComponent());

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto At_ = [](const EngineCore::Image& InPixels, const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };
        const auto Near = [](const int (&InRgb)[3], const int InR, const int InG, const int InB, const int InSlack)
        {
            return std::abs(InRgb[0] - InR) <= InSlack && std::abs(InRgb[1] - InG) <= InSlack && std::abs(InRgb[2] - InB) <= InSlack;
        };

        Rend.SetClearGradient(RENDER_DEFAULT_SCENE, EngineCore::Color(200, 60, 40), EngineCore::Color(20, 40, 160));
        EngineCore::Image Gradient, Solid;
        if(Capture(Gradient))
        {
            int Top[3], Bottom[3], Middle[3], Centre[3];
            At_(Gradient, 4, 0, Top);
            At_(Gradient, 4, 255, Bottom);
            At_(Gradient, 4, 128, Middle);
            At_(Gradient, 128, 128, Centre);
            std::printf("    top %d %d %d, middle %d %d %d, bottom %d %d %d, box %d %d %d\n", Top[0], Top[1], Top[2], Middle[0], Middle[1],
                        Middle[2], Bottom[0], Bottom[1], Bottom[2], Centre[0], Centre[1], Centre[2]);
            Expect(Near(Top, 200, 60, 40, 3) && Near(Bottom, 20, 40, 160, 3), "the top row is the top colour, the bottom row the bottom");
            Expect(Near(Middle, 110, 50, 100, 4), "half way down, half way between");
            Expect(!Near(Centre, 110, 50, 100, 30), "and a lit box draws over it");
        }
        Rend.SetClearColor(RENDER_DEFAULT_SCENE, EngineCore::Color(10, 120, 30));
        if(Capture(Solid))
        {
            int Top[3], Bottom[3];
            At_(Solid, 4, 0, Top);
            At_(Solid, 4, 255, Bottom);
            std::printf("    clear colour: top %d %d %d, bottom %d %d %d\n", Top[0], Top[1], Top[2], Bottom[0], Bottom[1], Bottom[2]);
            /* Within a step: display to linear (sRGB) in the half-float scene colour and back. */
            Expect(Near(Top, 10, 120, 30, 1) && Near(Bottom, 10, 120, 30, 1), "SetClearColor goes back to one colour");
        }

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}

/* RGBE8 textures, the Sky Light's environment and the Reflection Probe component (Null: no GPU). */
static void TestImageBasedLightingData()
{
    std::printf("[image-based lighting data]\n");
    for(const float Value : { 0.0f, 0.001f, 0.5f, 1.0f, 37.5f, 2500.0f })
    {
        const float In[3] = { Value, Value * 0.5f, Value * 0.25f };
        uint8_t Texel[4];
        EncodeRgbe(In, Texel);
        float Out[3];
        DecodeRgbe(Texel, Out);
        bool bClose = true;
        for(int Channel = 0; Channel < 3; ++Channel) { bClose = bClose && std::abs(Out[Channel] - In[Channel]) <= std::max(In[0] * 0.01f, 1e-6f); }
        Expect(bClose, "RGBE8 holds a colour to within 1% of its brightest channel, 0 to 2500");
    }

    TextureData Hdri;
    Hdri.Format = ETexturePixelFormat::RGBE8;
    Hdri.MipGen = ETextureMipGen::Box;
    Hdri.Pixels = Image(4, 2, EImageFormat::RGBA8);
    BuildTextureMips(Hdri);
    Expect(Hdri.Mips.empty(), "an RGBE8 texture gets no mips: its shader decodes it point sampled");
    Archive Writer = Archive::ForWriting();
    SerializeTexture(Writer, Hdri);
    TextureData Back;
    Archive Reader = Archive::ForReading(Writer.GetBytes());
    Expect(SerializeTexture(Reader, Back) && Back.Format == ETexturePixelFormat::RGBE8, "and saves and loads as RGBE8");

    const GameCore::ComponentSerializerInfo* SkyInfo = GameCore::FindComponentSerializer(SkyLightComponent::StaticType().ID);
    Expect(SkyInfo && SkyInfo->Version == 2, "SkyLightComponent saves as version 2 (Environment, its intensity and rotation, DrawBackground)");
    const GameCore::ComponentSerializerInfo* ProbeInfo = GameCore::FindComponentSerializer(ReflectionProbeComponent::StaticType().ID);
    Expect(ProbeInfo && ProbeInfo->Version == 1 && std::string(ReflectionProbeComponent::StaticType().Name) == "Reflection Probe",
           "Reflection Probe is a registered component");
    const ReflectionProbeComponent Defaults;
    Expect(Defaults.Frequency == SCF_Once && Defaults.bCaptureShadows && Defaults.Radius > 0.0f,
           "a probe captures once by default, with shadows, a sphere of influence");
    const GameCore::ComponentSerializerInfo* VolumeInfo = GameCore::FindComponentSerializer(GIVolumeComponent::StaticType().ID);
    Expect(VolumeInfo && VolumeInfo->Version == 1 && std::string(GIVolumeComponent::StaticType().Name) == "GI Volume",
           "GI Volume is a registered component (GLOBAL_ILLUMINATION.md, Phase 6)");

    Renderer Rend;
    if(Rend.Initialize(ERenderBackend::Null, WindowHandle()))
    {
        GameCore::World World;
        Rend.BindWorld(World);
        const GameCore::Entity Probe = World.CreateEntity("Probe");
        Probe.AddComponent(GameCore::TransformComponent());
        Probe.AddComponent(ReflectionProbeComponent());
        for(uint64_t Frame = 1; Frame < 4; ++Frame) { Rend.EndFrame(FrameInfo{Frame, 0.016, EngineCore::Size(64, 64)}); Rend.WaitForPreviousFrame(); }
        Expect(true, "a Reflection Probe on the Null backend (no HDR target, so no IBL) renders without trouble");
        Rend.UnbindWorld();
        Rend.Shutdown();
    }
}

/* The ToneMapping effect's .ppstack line, parsed and blended (Null: no GPU). */
static void TestToneMappingSettings()
{
    std::printf("[tone mapping settings]\n");
    PostProcessSettings Parsed;
    std::string Error;
    Expect(ParsePostProcessStack("Effect ToneMapping Exposure=-1.5 ToneMapper=Reinhard\n", Parsed, Error) && Parsed.bToneMapping
           && Parsed.Exposure == -1.5f && Parsed.ToneMapper == 1, "ToneMapping reads its exposure and operator");
    PostProcessSettings Aces;
    ParsePostProcessStack("Effect ToneMapping\n", Aces, Error);
    Expect(Aces.bToneMapping && Aces.Exposure == 0.0f && Aces.ToneMapper == 2, "and defaults to exposure 0 and ACES");
    Expect(!PostProcessSettings().bToneMapping && !Aces.IsEmpty(), "off unless a stack has the node; a stack with only it is not empty");

    PostProcessSettings Blend;
    Parsed.Exposure = 2.0f;
    BlendPostProcess(Blend, Parsed, 0.25f);
    Expect(Blend.bToneMapping && std::abs(Blend.Exposure - 0.5f) < 1e-5f && Blend.ToneMapper == 0,
           "a quarter in: a quarter of the exposure, the operator not yet (None)");
    BlendPostProcess(Blend, Parsed, 1.0f);
    Expect(Blend.Exposure == 2.0f && Blend.ToneMapper == 1, "all the way in: the stack's");
}

/* Auto exposure, screen-space reflections, volumetric fog and TAA: their .ppstack keys, and
 * how a volume fades each in (RENDERPIPELINE.md). */
static void TestFidelitySettings()
{
    std::printf("[auto exposure, reflections, fog and TAA settings]\n");
    PostProcessSettings S;
    std::string Error;
    const bool bParsed = ParsePostProcessStack(
        "Effect AutoExposure Key=0.12 MinEV=-3 MaxEV=2 SpeedUp=4 SpeedDown=0.5 CentreWeight=0.8\n"
        "Effect ScreenSpaceReflections Intensity=0.7 MaxRoughness=0.3 MaxDistance=900 Thickness=12 Steps=40\n"
        "Effect ScreenSpaceGI Intensity=0.6 Rays=8 Radius=450\n"
        "Effect VolumetricFog Density=0.0002 HeightFalloff=0.001 BaseHeight=50 MaxDistance=5000 Anisotropy=0.7 "
        "SunScattering=2 SkyScattering=0.5 LightScattering=3 Albedo=1,0.9,0.8 Steps=16 Shadows=false\n"
        "Effect Antialiasing Method=TAA Sharpness=0.5 Quality=High\n", S, Error);
    Expect(bParsed && S.bAutoExposure && S.AeKey == 0.12f && S.AeMinEV == -3.0f && S.AeMaxEV == 2.0f && S.AeSpeedUp == 4.0f
           && S.AeSpeedDown == 0.5f && S.AeCentreWeight == 0.8f, "AutoExposure reads its key, range, speeds and weighting");
    Expect(S.bScreenReflections && S.SsrIntensity == 0.7f && S.SsrMaxRoughness == 0.3f && S.SsrMaxDistance == 900.0f
           && S.SsrThickness == 12.0f && S.SsrSteps == 40.0f, "ScreenSpaceReflections reads its settings");
    Expect(S.bScreenGI && S.GiIntensity == 0.6f && S.GiRays == 8.0f && S.GiRadius == 450.0f, "ScreenSpaceGI reads its settings");
    Expect(S.bVolumetricFog && S.FogDensity == 0.0002f && S.FogBaseHeight == 50.0f && S.FogAnisotropy == 0.7f
           && S.FogAlbedo[1] == 0.9f && S.FogSteps == 16.0f && !S.bFogShadows, "VolumetricFog reads its settings and Albedo");
    Expect(S.bAntialiasing && S.AaMethod == 1 && S.AaSharpness == 0.5f, "Antialiasing reads Method TAA and its Sharpness");
    PostProcessSettings Fxaa;
    ParsePostProcessStack("Effect Antialiasing\n", Fxaa, Error);
    Expect(Fxaa.AaMethod == 0 && !PostProcessSettings().bAutoExposure && !PostProcessSettings().bVolumetricFog,
           "FXAA by default; the new effects off unless a stack has their node");

    PostProcessSettings Blend;
    BlendPostProcess(Blend, S, 0.25f);
    Expect(Blend.bAutoExposure && std::abs(Blend.AeMinEV + 0.75f) < 1e-5f && std::abs(Blend.AeMaxEV - 0.5f) < 1e-5f,
           "a quarter in: a quarter of auto exposure's range, from none");
    Expect(Blend.bScreenReflections && std::abs(Blend.SsrIntensity - 0.175f) < 1e-5f, "a quarter of the reflections' intensity");
    Expect(Blend.bVolumetricFog && std::abs(Blend.FogDensity - 0.00005f) < 1e-9f && !Blend.bFogShadows,
           "a quarter of the fog's density, its switches the stack's from the start (as from off)");
    BlendPostProcess(Blend, S, 1.0f);
    Expect(Blend.AeMinEV == -3.0f && Blend.FogDensity == 0.0002f && !Blend.bFogShadows && Blend.AaMethod == 1,
           "all the way in: the stack's, switches too");
}

/*
 * HDR and tone mapping on the GPU: an Unlit quad of display 0.9 under each
 * operator and an exposure, and the editor's gradient background, which must
 * come out of any tone map as authored.
 */
static void TestToneMappingOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[tone mapping on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[tone mapping on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);
    std::error_code Ignored;
    std::filesystem::create_directories("SmokeScratch/PostStacks", Ignored);
    const std::string Stack = "SmokeScratch/PostStacks/Tone.pp.nmgraph";
    const auto WriteStack = [&Stack](const char* InLine)
    {
        std::ofstream Out(Stack);
        Out << "<?xml version=\"1.0\"?>\n<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <gen_code>\n"
               "    <Output Extension=\".ppstack\"><![CDATA[" << InLine << "\n]]></Output>\n  </gen_code>\n</NodeGraph>\n";
        PostProcessStackCache::Invalidate(Stack);
    };

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        Rend.MountShaderCache("SandboxGame");
        const EngineCore::Size Viewport(256, 256);
        const RenderView Facing = RenderView::MakePerspective(Viewport, Vector3(0, 0, -400), Vector3(0, 0, 1), 50.0f, 5.0f, 5000.0f);
        Rend.SetViewOverride(&Facing);
        Rend.SetClearGradient(RENDER_DEFAULT_SCENE, EngineCore::Color(70, 90, 140, 255), EngineCore::Color(20, 22, 30, 255));
        GameCore::World World;
        Rend.BindWorld(World);

        const GameCore::Entity Quad = World.CreateEntity("Flat");
        GameCore::TransformComponent Transform;
        Transform.Scale = Vector3(100, 100, 100);
        Quad.AddComponent(Transform);
        RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        Render.SetMaterial(0, "Content/Materials/SolidColor.material.nmgraph");
        Render.SetMaterialParameter(0, "Color", 0.9f, 0.9f, 0.9f, 1.0f);
        Render.bCastShadows = 0;
        Quad.AddComponent(Render);

        const GameCore::Entity Volume = World.CreateEntity("Volume");
        Volume.AddComponent(GameCore::TransformComponent());
        PostProcessComponent Post;
        std::snprintf(Post.Stack, sizeof(Post.Stack), "%s", Stack.c_str());
        Post.bEnabled = 0;
        Volume.AddComponent(Post);

        uint64_t Frame = 1;
        const auto Capture = [&](int (&OutCentre)[3], int (&OutCorner)[3])
        {
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            EngineCore::Image Pixels;
            std::string Error;
            if(!(Request != 0 && Rend.TakeMaterialPreview(Request, Pixels, Error) && Pixels.GetWidth() == 256)) { return false; }
            const bool bBgra = Pixels.GetFormat() == EImageFormat::BGRA8;
            const auto At = [&](const int InX, const int InY, int (&OutRgb)[3])
            {
                const uint8_t* Px = Pixels.GetData() + Pixels.GetStride() * InY + InX * BytesPerPixel(Pixels.GetFormat());
                OutRgb[0] = bBgra ? Px[2] : Px[0]; OutRgb[1] = Px[1]; OutRgb[2] = bBgra ? Px[0] : Px[2];
            };
            At(128, 128, OutCentre);
            At(10, 30, OutCorner);
            return true;
        };
        const auto Near = [](const int InValue, const int InWant, const int InSlack) { return std::abs(InValue - InWant) <= InSlack; };

        int Centre[3], Corner[3], BaseCorner[3];
        Expect(Capture(Centre, BaseCorner), "the quad is captured with no tone map");
        std::printf("    none: centre %d, corner %d %d %d\n", Centre[0], BaseCorner[0], BaseCorner[1], BaseCorner[2]);
        Expect(Near(Centre[0], 230, 3), "no ToneMapping: an HDR scene comes out clamped, as an 8-bit one did (0.9 is 230)");

        Volume.GetComponent<PostProcessComponent>()->bEnabled = 1;
        struct Case { const char* Line; int Want; const char* What; };
        const Case Cases[] = {
            { "Effect ToneMapping ToneMapper=ACES", 202, "ACES compresses: 0.9 comes out near 202" },
            { "Effect ToneMapping Exposure=1 ToneMapper=ACES", 229, "an exposure of +1 doubles the light first: near 229" },
            { "Effect ToneMapping ToneMapper=Reinhard", 176, "Reinhard, x / (1 + x): near 176" },
        };
        for(const Case& Each : Cases)
        {
            WriteStack(Each.Line);
            const bool bCaptured = Capture(Centre, Corner);
            std::printf("    %s: centre %d, corner %d %d %d\n", Each.Line, Centre[0], Corner[0], Corner[1], Corner[2]);
            Expect(bCaptured && Near(Centre[0], Each.Want, 6), Each.What);
            Expect(bCaptured && Near(Corner[0], BaseCorner[0], 3) && Near(Corner[1], BaseCorner[1], 3) && Near(Corner[2], BaseCorner[2], 3),
                   "and the gradient background comes out of the tone map as authored");
        }
        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    PostProcessStackCache::Invalidate(std::string());
    SetFileProvider(PreviousProvider);
}

/*
 * Image-based lighting on the GPU: in a room of six coloured walls, a mirror
 * metal sphere with a Reflection Probe beside it shows the wall behind the
 * camera at its centre; with the probe off it shows the sky instead; and a
 * rough white sphere's top takes the green ceiling's light (its irradiance).
 */
static void TestImageBasedLightingOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[image-based lighting on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[image-based lighting on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        Rend.MountShaderCache("SandboxGame");
        const EngineCore::Size Viewport(256, 256);
        const RenderView Facing = RenderView::MakePerspective(Viewport, Vector3(0, 0, -400), Vector3(0, 0, 1), 50.0f, 5.0f, 5000.0f);
        Rend.SetViewOverride(&Facing);
        GameCore::World World;
        Rend.BindWorld(World);

        const auto Shape = [&World](const char* InName, const char* InMesh, const char* InMaterial, const Vector3& InAt, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InAt;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InMesh);
            Render.SetMaterial(0, InMaterial);
            Render.bCastShadows = 0;
            Each.AddComponent(Render);
            return Each;
        };
        struct Wall { const char* Name; Vector3 At; Vector3 Scale; int R, G, B; };
        const Wall Walls[6] = {
            { "WallPX", Vector3( 600, 0, 0), Vector3(10, 2000, 2000), 230, 30, 30 },
            { "WallNX", Vector3(-600, 0, 0), Vector3(10, 2000, 2000), 30, 220, 220 },
            { "WallPY", Vector3(0,  600, 0), Vector3(2000, 10, 2000), 30, 230, 30 },
            { "WallNY", Vector3(0, -600, 0), Vector3(2000, 10, 2000), 220, 30, 220 },
            { "WallPZ", Vector3(0, 0,  600), Vector3(2000, 2000, 10), 30, 30, 230 },
            { "WallNZ", Vector3(0, 0, -600), Vector3(2000, 2000, 10), 230, 220, 30 },
        };
        for(const Wall& Each : Walls)
        {
            Shape(Each.Name, "Engine/Content/Meshes/Box.nmmesh", "Content/Materials/SolidColor.material.nmgraph", Each.At, Each.Scale)
                .GetComponent<RenderComponent>()->SetMaterialParameter(0, "Color", float(Each.R) / 255.0f, float(Each.G) / 255.0f,
                                                                       float(Each.B) / 255.0f, 1.0f);
        }
        const GameCore::Entity Ball = Shape("Ball", "Engine/Content/Meshes/Sphere.nmmesh", "Content/Materials/PBRParams.material.nmgraph",
                                            Vector3(), Vector3(200, 200, 200));
        const auto SetBall = [&Ball](const float InMetallic, const float InRoughness)
        {
            RenderComponent* Render = Ball.GetComponent<RenderComponent>();
            Render->SetMaterialParameter(0, "Metallic", InMetallic, 0.0f, 0.0f, 0.0f);
            Render->SetMaterialParameter(0, "Roughness", InRoughness, 0.0f, 0.0f, 0.0f);
        };
        SetBall(1.0f, 0.0f);

        /* No sun: the ball is lit by images alone. */
        const GameCore::Entity Sun = World.CreateEntity("Dark");
        Sun.AddComponent(GameCore::TransformComponent());
        DirectionalLightComponent Dark;
        Dark.Intensity    = 0.0f;
        Dark.bCastShadows = 0;
        Sun.AddComponent(Dark);

        const GameCore::Entity ProbeEntity = World.CreateEntity("Probe");
        GameCore::TransformComponent ProbeAt;
        ProbeAt.Position = Vector3(0, 0, -200);
        ProbeEntity.AddComponent(ProbeAt);
        ReflectionProbeComponent Probe;
        Probe.Radius     = 3000.0f;
        Probe.Resolution = 64;
        ProbeEntity.AddComponent(Probe);

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            // Eight frames: a probe's capture is a face a frame ([Rendering] ProbeFacesPerFrame).
            for(int Each = 0; Each < 8; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto At = [](const EngineCore::Image& InPixels, const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };
        /* The ball's screen centre and its top: 100 units up at depth 400, in a 50-degree view. */
        const int TopY = 128 - int(100.0 / (400.0 * std::tan(25.0 * 3.14159265 / 180.0)) * 128.0) + 6;

        EngineCore::Image Mirror;
        int Centre[3] = {};
        const bool bMirror = Capture(Mirror);
        if(bMirror) { At(Mirror, 128, 128, Centre); }
        std::printf("    probe on, mirror ball centre %d %d %d\n", Centre[0], Centre[1], Centre[2]);
        Expect(bMirror && Centre[0] > 150 && Centre[1] > 140 && Centre[2] < 90,
               "a mirror metal ball reflects, from its Reflection Probe, the yellow wall behind the camera");

        ProbeEntity.GetComponent<ReflectionProbeComponent>()->bEnabled = 0;
        EngineCore::Image SkyOnly;
        int SkyCentre[3] = {};
        const bool bSky = Capture(SkyOnly);
        if(bSky) { At(SkyOnly, 128, 128, SkyCentre); }
        std::printf("    probe off, mirror ball centre %d %d %d\n", SkyCentre[0], SkyCentre[1], SkyCentre[2]);
        Expect(bSky && SkyCentre[2] >= SkyCentre[0] && SkyCentre[2] > SkyCentre[1] - 10 && SkyCentre[0] + SkyCentre[1] + SkyCentre[2] > 60,
               "with the probe off it reflects the sky (the Sky Light's colours) instead");

        ProbeEntity.GetComponent<ReflectionProbeComponent>()->bEnabled = 1;
        SetBall(0.0f, 1.0f);
        EngineCore::Image Matte;
        int Top[3] = {};
        const bool bMatte = Capture(Matte);
        if(bMatte) { At(Matte, 128, TopY, Top); }
        std::printf("    rough white ball, top (y %d) %d %d %d\n", TopY, Top[0], Top[1], Top[2]);
        Expect(bMatte && Top[1] > Top[0] + 30 && Top[1] > Top[2] + 30,
               "a rough white ball's top is lit green by the ceiling: the probe's diffuse irradiance, the right way up");

        /* An HDRI: the room gone, a Sky Light's equirectangular RGBE8 environment, blue above the horizon
         * and red below, as the background and in the mirror ball (the probe off). */
        for(const char* Name : { "WallPX", "WallNX", "WallPY", "WallNY", "WallPZ", "WallNZ" })
        {
            World.DestroyEntity(World.FindEntityByName(Name));
        }
        ProbeEntity.GetComponent<ReflectionProbeComponent>()->bEnabled = 0;
        SetBall(1.0f, 0.0f);
        std::error_code Ignored;
        std::filesystem::create_directories("SmokeScratch/Sky", Ignored);
        {
            TextureData Hdri;
            Hdri.Format = ETexturePixelFormat::RGBE8;
            Hdri.bSRGB  = false;
            Hdri.Filter = ETextureFilter::Nearest;
            Hdri.Pixels = Image(64, 32, EImageFormat::RGBA8);
            for(int Y = 0; Y < 32; ++Y)
            {
                for(int X = 0; X < 64; ++X)
                {
                    const float Blue[3] = { 0.05f, 0.1f, 2.0f };
                    const float Red[3]  = { 2.0f, 0.1f, 0.05f };
                    EncodeRgbe(Y < 16 ? Blue : Red, Hdri.Pixels.GetScanline(Y) + X * 4);
                }
            }
            Archive Writer = Archive::ForWriting();
            SerializeTexture(Writer, Hdri);
            SaveArchiveToFile("SmokeScratch/Sky/TwoTone.nmtexture", Writer);
            TextureAssetCache::Invalidate("SmokeScratch/Sky/TwoTone.nmtexture");
        }
        const GameCore::Entity SkyEntity = World.CreateEntity("Sky");
        SkyEntity.AddComponent(GameCore::TransformComponent());
        SkyLightComponent SkyLight;
        std::snprintf(SkyLight.Environment, sizeof(SkyLight.Environment), "%s", "SmokeScratch/Sky/TwoTone.nmtexture");
        SkyEntity.AddComponent(SkyLight);

        EngineCore::Image Hdr;
        int Above[3] = {}, Below[3] = {}, BallTop[3] = {}, BallBottom[3] = {};
        const bool bHdr = Capture(Hdr);
        if(bHdr)
        {
            At(Hdr, 128, 8, Above);
            At(Hdr, 128, 247, Below);
            At(Hdr, 128, TopY, BallTop);
            At(Hdr, 128, 255 - TopY, BallBottom);
        }
        std::printf("    HDRI: sky %d %d %d, ground %d %d %d; ball top %d %d %d, bottom %d %d %d\n", Above[0], Above[1], Above[2],
                    Below[0], Below[1], Below[2], BallTop[0], BallTop[1], BallTop[2], BallBottom[0], BallBottom[1], BallBottom[2]);
        Expect(bHdr && Above[2] > 200 && Above[0] < 120 && Below[0] > 200 && Below[2] < 120,
               "a Sky Light's HDRI is drawn behind the scene: its upper half above the horizon, its lower below");
        Expect(bHdr && BallTop[2] > BallTop[0] + 60 && BallBottom[0] > BallBottom[2] + 60,
               "and the mirror ball reflects it: the sky in its top, the ground in its bottom");

        if(const char* DumpDir = std::getenv("NIMBLE_SMOKE_DUMP"); DumpDir && DumpDir[0])
        {
            for(const auto& [Pixels, Name] : { std::make_pair(&Mirror, "ibl-mirror"), std::make_pair(&SkyOnly, "ibl-sky"), std::make_pair(&Matte, "ibl-matte"),
                                               std::make_pair(&Hdr, "ibl-hdri") })
            {
                std::ofstream File(std::string(DumpDir) + "/" + Name + ".ppm", std::ios::binary);
                File << "P6\n256 256\n255\n";
                for(int Y = 0; Y < 256; ++Y)
                {
                    for(int X = 0; X < 256; ++X)
                    {
                        int Rgb[3];
                        At(*Pixels, X, Y, Rgb);
                        const char Bytes[3] = { char(Rgb[0]), char(Rgb[1]), char(Rgb[2]) };
                        File.write(Bytes, 3);
                    }
                }
            }
        }
        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}

/*
 * Scene capture on the GPU: an omnidirectional probe in a room of six
 * coloured walls -- each face of its RenderTargetTex3D is its wall, and a
 * Reflect sphere shows the wall each part of it faces -- and a planar mirror
 * floor showing the box above it, upside down, where it should be.
 */
static void TestSceneCaptureOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[scene capture on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[scene capture on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        Rend.MountShaderCache("SandboxGame");
        const EngineCore::Size Viewport(256, 256);
        GameCore::World World;
        Rend.BindWorld(World);

        const auto Shape = [&World](const char* InName, const char* InMesh, const char* InMaterial, const Vector3& InAt, const Vector3& InScale)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InAt;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InMesh);
            Render.SetMaterial(0, InMaterial);
            Render.bCastShadows = 0;
            Each.AddComponent(Render);
            return Each;
        };
        const auto Paint = [](const GameCore::Entity& InEntity, const int InR, const int InG, const int InB)
        {
            InEntity.GetComponent<RenderComponent>()->SetMaterialParameter(0, "Color", float(InR) / 255.0f, float(InG) / 255.0f,
                                                                            float(InB) / 255.0f, 1.0f);
        };
        uint64_t Frame = 1;
        const auto Draw = [&](const RenderView& InView, const int InFrames)
        {
            Rend.SetViewOverride(&InView);
            for(int Each = 0; Each < InFrames; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
        };
        const auto Capture = [&](const RenderView& InView, EngineCore::Image& OutPixels)
        {
            Draw(InView, 2);
            const uint64_t Request = Rend.RequestSceneCapture();
            Draw(InView, 2);
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto At = [](const EngineCore::Image& InPixels, const int InX, const int InY, int (&OutRgb)[3])
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            OutRgb[0] = bBgra ? Px[2] : Px[0];
            OutRgb[1] = Px[1];
            OutRgb[2] = bBgra ? Px[0] : Px[2];
        };
        const auto Is = [](const int (&InRgb)[3], const int InR, const int InG, const int InB)
        {
            return std::abs(InRgb[0] - InR) <= 12 && std::abs(InRgb[1] - InG) <= 12 && std::abs(InRgb[2] - InB) <= 12;
        };

        /* The room: +X red, -X cyan, +Y green, -Y magenta, +Z blue, -Z yellow. */
        struct Wall { const char* Name; Vector3 At; Vector3 Scale; int R, G, B; };
        const Wall Walls[6] = {
            { "WallPX", Vector3( 600, 0, 0), Vector3(10, 2000, 2000), 230, 30, 30 },
            { "WallNX", Vector3(-600, 0, 0), Vector3(10, 2000, 2000), 30, 220, 220 },
            { "WallPY", Vector3(0,  600, 0), Vector3(2000, 10, 2000), 30, 230, 30 },
            { "WallNY", Vector3(0, -600, 0), Vector3(2000, 10, 2000), 220, 30, 220 },
            { "WallPZ", Vector3(0, 0,  600), Vector3(2000, 2000, 10), 30, 30, 230 },
            { "WallNZ", Vector3(0, 0, -600), Vector3(2000, 2000, 10), 230, 220, 30 },
        };
        std::vector<GameCore::Entity> Room;
        for(const Wall& Each : Walls)
        {
            Room.push_back(Shape(Each.Name, "Engine/Content/Meshes/Box.nmmesh", "Content/Materials/SolidColor.material.nmgraph", Each.At, Each.Scale));
            Paint(Room.back(), Each.R, Each.G, Each.B);
        }
        const GameCore::Entity Ball = Shape("Ball", "Engine/Content/Meshes/Sphere.nmmesh", "Content/Materials/ReflectCube.material.nmgraph",
                                            Vector3(), Vector3(200, 200, 200));
        SceneCaptureProbeComponent Probe;
        std::snprintf(Probe.Target, sizeof(Probe.Target), "%s", "Content/RenderTargets/TestCube.nmrt3d");
        Probe.Mode       = SCM_Omnidirectional;
        Probe.Resolution = 64;
        Probe.NearClip   = 1.0f;
        Probe.FarClip    = 3000.0f;
        Ball.AddComponent(Probe);

        const RenderView Facing = RenderView::MakePerspective(Viewport, Vector3(0, 0, -400), Vector3(0, 0, 1), 50.0f, 5.0f, 5000.0f);
        Draw(Facing, 3);

        /* The capture itself: each face's centre is its wall. */
        const uint64_t Readback = Rend.RequestRenderTargetReadback("Content/RenderTargets/TestCube.nmrt3d");
        Draw(Facing, 2);
        EngineCore::Image Strip;
        std::string Error;
        const bool bStrip = Readback != 0 && Rend.TakeMaterialPreview(Readback, Strip, Error);
        Expect(bStrip && Strip.GetWidth() == 64 * 6 && Strip.GetHeight() == 64, "the RenderTargetTex3D reads back as six 64-texel faces side by side");
        if(bStrip)
        {
            bool bFaces = true;
            for(int Face = 0; Face < 6; ++Face)
            {
                int Rgb[3];
                At(Strip, Face * 64 + 32, 32, Rgb);
                std::printf("    face %d centre %d %d %d\n", Face, Rgb[0], Rgb[1], Rgb[2]);
                bFaces = bFaces && Is(Rgb, Walls[Face].R, Walls[Face].G, Walls[Face].B);
            }
            Expect(bFaces, "each face, +X -X +Y -Y +Z -Z, looks at its wall");
        }

        /* The sphere: centre reflects -Z (behind the camera); its sides their walls. */
        EngineCore::Image Shot;
        if(Capture(Facing, Shot))
        {
            int Centre[3], Right[3], Left[3], Top[3], Bottom[3];
            At(Shot, 128, 128, Centre);
            At(Shot, 186, 128, Right);
            At(Shot, 70, 128, Left);
            At(Shot, 128, 70, Top);
            At(Shot, 128, 186, Bottom);
            std::printf("    sphere centre %d %d %d, right %d %d %d, left %d %d %d, top %d %d %d, bottom %d %d %d\n", Centre[0], Centre[1],
                        Centre[2], Right[0], Right[1], Right[2], Left[0], Left[1], Left[2], Top[0], Top[1], Top[2], Bottom[0], Bottom[1], Bottom[2]);
            Expect(Is(Centre, 230, 220, 30), "the sphere's centre reflects the wall behind the camera");
            Expect(Is(Right, 230, 30, 30) && Is(Left, 30, 220, 220), "its right side the +X wall, its left the -X");
            Expect(Is(Top, 30, 230, 30) && Is(Bottom, 220, 30, 220), "its top the ceiling, its bottom the floor");
        }

        /* Capture Shadows: off (the default), a capture prepares no shadow
         * maps, so a lit, shadowed room costs its faces nothing more. */
        {
            GameCore::Entity Sun = World.CreateEntity("CaptureSun");
            GameCore::TransformComponent SunAt;
            SunAt.Rotation = Vector3(50, 35, 0);
            Sun.AddComponent(SunAt);
            DirectionalLightComponent SunLight;
            SunLight.ShadowTechnique = LST_Standard;
            Sun.AddComponent(SunLight);
            /* A lit caster (the Unlit walls cast nothing), off every face's axis. */
            const GameCore::Entity Caster = Shape("CaptureCaster", "Engine/Content/Meshes/Box.nmmesh",
                                                  "Engine/Content/Materials/DefaultLit.material.nmgraph", Vector3(300, -450, 300), Vector3(60, 60, 60));
            Caster.GetComponent<RenderComponent>()->bCastShadows = 1;

            const auto DrawsPerFrame = [&]()
            {
                Draw(Facing, 2);
                RenderStats Before;
                Rend.GetStats(Before);
                Draw(Facing, 4);
                RenderStats After;
                Rend.GetStats(After);
                return double(After.DrawCalls - Before.DrawCalls) / double(std::max<uint64_t>(After.FramesRendered - Before.FramesRendered, 1));
            };
            Expect(Ball.GetComponent<SceneCaptureProbeComponent>()->bCaptureShadows == 0, "a probe captures without shadows by default");
            const double Without = DrawsPerFrame();
            Ball.GetComponent<SceneCaptureProbeComponent>()->bCaptureShadows = 1;
            const double With = DrawsPerFrame();
            Ball.GetComponent<SceneCaptureProbeComponent>()->bCaptureShadows = 0;
            std::printf("    capture shadows: %.0f draws a frame off, %.0f on\n", Without, With);
            Expect(With >= Without + 6.0, "Capture Shadows on redraws the caster into each face's shadow map; off skips it");

            const uint64_t Again = Rend.RequestRenderTargetReadback("Content/RenderTargets/TestCube.nmrt3d");
            Draw(Facing, 2);
            EngineCore::Image Unshadowed;
            std::string AgainError;
            int Centre[3] = {};
            const bool bRead = Again != 0 && Rend.TakeMaterialPreview(Again, Unshadowed, AgainError);
            if(bRead) { At(Unshadowed, 32, 32, Centre); }
            Expect(bRead && Is(Centre, 230, 30, 30), "and the faces still capture their walls (+X red) without shadow maps");

            World.DestroyEntity(Caster.GetID());
            World.DestroyEntity(Sun.GetID());
        }

        /* A planar mirror: the room gone, a red box above a mirror floor. */
        for(const GameCore::Entity& Each : Room) { World.DestroyEntity(Each); }
        World.DestroyEntity(Ball);
        const GameCore::Entity Floor = Shape("Mirror", "Engine/Content/Meshes/Plane.nmmesh", "Content/Materials/ReflectMirror.material.nmgraph",
                                             Vector3(), Vector3(800, 1, 800));
        SceneCaptureProbeComponent Mirror;
        std::snprintf(Mirror.Target, sizeof(Mirror.Target), "%s", "Content/RenderTargets/TestMirror.nmrt2d");
        Mirror.Mode    = SCM_PlanarMirror;
        Mirror.ExtentX = 400.0f;
        Mirror.ExtentZ = 400.0f;
        Floor.AddComponent(Mirror);
        const GameCore::Entity Box = Shape("Box", "Engine/Content/Meshes/Box.nmmesh", "Content/Materials/SolidColor.material.nmgraph",
                                           Vector3(160, 120, 150), Vector3(80, 80, 80));
        Paint(Box, 230, 30, 30);   // off centre: a left-right mix-up would put its reflection on the other side
        /* A slab under the mirror: between the reflected camera and the box, kept out by the oblique near plane. */
        const GameCore::Entity Under = Shape("Under", "Engine/Content/Meshes/Box.nmmesh", "Content/Materials/SolidColor.material.nmgraph",
                                             Vector3(0, -40, 0), Vector3(900, 20, 900));
        Paint(Under, 30, 230, 30);

        const Vector3 Eye(0, 350, -450);
        const RenderView Above = RenderView::MakePerspective(Viewport, Eye, Vector3(0, -350, 600), 60.0f, 5.0f, 5000.0f);
        const auto PixelOf = [&Above](const Vector3& InWorld, int& OutX, int& OutY)
        {
            const EngineCore::Vector4 Clip = (Above.Projection * Above.View) * EngineCore::Vector4(float(InWorld.GetX()), float(InWorld.GetY()),
                                                                                                  float(InWorld.GetZ()), 1.0f);
            OutX = int((Clip.GetX() / Clip.GetW() * 0.5f + 0.5f) * 256.0f);
            OutY = int((1.0f - (Clip.GetY() / Clip.GetW() * 0.5f + 0.5f)) * 256.0f);
        };
        EngineCore::Image Pool;
        if(Capture(Above, Pool))
        {
            int RX, RY, EX, EY, BX, BY;
            PixelOf(Vector3(160, -120, 150), RX, RY);  // the box's reflection, under the floor
            PixelOf(Vector3(-160, -120, 150), EX, EY); // its mirror image across the screen: nothing there
            PixelOf(Vector3(160, 120, 150), BX, BY);   // the box itself
            int Reflection[3], Empty[3], Itself[3];
            At(Pool, std::clamp(RX, 0, 255), std::clamp(RY, 0, 255), Reflection);
            At(Pool, std::clamp(EX, 0, 255), std::clamp(EY, 0, 255), Empty);
            At(Pool, std::clamp(BX, 0, 255), std::clamp(BY, 0, 255), Itself);
            std::printf("    mirror: box %d %d %d at %d,%d; reflection %d %d %d at %d,%d; empty %d %d %d\n", Itself[0], Itself[1], Itself[2],
                        BX, BY, Reflection[0], Reflection[1], Reflection[2], RX, RY, Empty[0], Empty[1], Empty[2]);
            Expect(Is(Itself, 230, 30, 30), "the box draws");
            Expect(Is(Reflection, 230, 30, 30), "and the mirror floor shows it below itself, where its reflection belongs, past the slab under it");
            Expect(!Is(Empty, 230, 30, 30), "on its own side, not the other");
        }

        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}

/*
 * Bloom past the 8-bit scene colour: an Opaque Lit quad with emissive 50
 * (Sandbox's HotGlow) clips to white in the scene, but its G-buffer keeps the
 * rest (EncodeGBufferEmissive), and Bloom spreads it -- far wider than the
 * 0-1 a clipped white could give.
 */
static void TestBloomHdrOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[bloom past 1 on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[bloom past 1 on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    std::error_code Ignored;
    std::filesystem::create_directories("SmokeScratch/PostStacks", Ignored);
    const std::string Stack = "SmokeScratch/PostStacks/HdrBloom.pp.nmgraph";
    {
        std::ofstream Out(Stack);
        Out << "<?xml version=\"1.0\"?>\n<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <gen_code>\n"
               "    <Output Extension=\".ppstack\"><![CDATA[Effect Bloom Threshold=0.9 Intensity=0.5 Radius=0.75 Levels=5 Tint=1,1,1\n"
               "]]></Output>\n  </gen_code>\n</NodeGraph>\n";
    }
    PostProcessStackCache::Invalidate(Stack);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        Rend.MountShaderCache("SandboxGame");
        const EngineCore::Size Viewport(256, 256);
        const RenderView Above = RenderView::MakePerspective(Viewport, Vector3(0.0, 800.0, 0.0), Vector3(0.0, -1.0, 0.001),
                                                             50.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&Above);
        GameCore::World World;
        Rend.BindWorld(World);

        /* A quad 60 units across, about 28 pixels, in the middle of the view. */
        const GameCore::Entity Quad = World.CreateEntity("HotGlow");
        GameCore::TransformComponent Transform;
        Transform.Scale = Vector3(60.0, 1.0, 60.0);
        Quad.AddComponent(Transform);
        RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Plane.nmmesh");
        std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", "Content/Materials/HotGlow.material.nmgraph");
        Quad.AddComponent(Render);

        const GameCore::Entity Volume = World.CreateEntity("Volume");
        Volume.AddComponent(GameCore::TransformComponent());
        PostProcessComponent Post;
        std::snprintf(Post.Stack, sizeof(Post.Stack), "%s", Stack.c_str());
        Post.bEnabled = 0;
        Volume.AddComponent(Post);

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            for(int Each = 0; Each < 2; ++Each)
            {
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
                Rend.WaitForPreviousFrame();
            }
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each)
            {
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
                Rend.WaitForPreviousFrame();
            }
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Luma = [](const EngineCore::Image& InPixels, const int InX, const int InY)
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            return int(Px[0]) + int(Px[1]) + int(Px[2]);
        };
        /* The mean brightening, as R + G + B, from InFrom to InTo pixels out from the centre. */
        const auto Ring = [&](const EngineCore::Image& InOn, const EngineCore::Image& InOff, const int InFrom, const int InTo)
        {
            double Sum = 0.0;
            int    Count = 0;
            for(int Y = 0; Y < 256; ++Y)
            {
                for(int X = 0; X < 256; ++X)
                {
                    const int Distance = std::max(std::abs(X - 128), std::abs(Y - 128));
                    if(Distance < InFrom || Distance >= InTo) { continue; }
                    Sum += Luma(InOn, X, Y) - Luma(InOff, X, Y);
                    ++Count;
                }
            }
            return Count ? Sum / Count : 0.0;
        };

        EngineCore::Image Off, On;
        const bool bOff = Capture(Off);
        Volume.GetComponent<PostProcessComponent>()->bEnabled = 1;
        const bool bOn = bOff && Capture(On);
        Expect(bOff && bOn, "the glowing quad is captured with and without Bloom");
        if(bOff && bOn)
        {
            if(const char* DumpDir = std::getenv("NIMBLE_SMOKE_DUMP"); DumpDir && DumpDir[0])
            {
                for(const auto& [Pixels, Name] : { std::make_pair(&Off, "bloom-hdr-off"), std::make_pair(&On, "bloom-hdr-on") })
                {
                    std::ofstream File(std::string(DumpDir) + "/" + Name + ".ppm", std::ios::binary);
                    File << "P6\n256 256\n255\n";
                    const bool bBgra = Pixels->GetFormat() == EImageFormat::BGRA8;
                    for(int Y = 0; Y < 256; ++Y)
                    {
                        for(int X = 0; X < 256; ++X)
                        {
                            const uint8_t* Px = Pixels->GetData() + Pixels->GetStride() * Y + X * BytesPerPixel(Pixels->GetFormat());
                            const char Bytes[3] = { char(bBgra ? Px[2] : Px[0]), char(Px[1]), char(bBgra ? Px[0] : Px[2]) };
                            File.write(Bytes, 3);
                        }
                    }
                }
            }
            const double Near = Ring(On, Off, 20, 30);
            const double Far  = Ring(On, Off, 40, 60);
            std::printf("    centre %d; glow 20-30 px out +%.1f, 40-60 px out +%.1f\n", Luma(Off, 128, 128), Near, Far);
            Expect(Luma(Off, 128, 128) >= 760, "emissive 50 clips to white in the scene colour");
            Expect(Near > 120.0 && Far > 20.0, "Bloom spreads the emissive the scene clipped, well past the quad");
        }
        Rend.UnbindWorld();
        Rend.Shutdown();
    }
    PostProcessStackCache::Invalidate(std::string());
    SetFileProvider(PreviousProvider);
}
#endif // WITH_EDITOR


/*
 * The Particle System component and the spawn bookkeeping
 * (ParticleSimulation::BuildParams), on the CPU: settings clamped, spawns
 * counted round the ring, the sort sized. RENDERPIPELINE.md, "Particles".
 */
static void TestParticleSystem()
{
    std::printf("[particle system]\n");

    size_t Count = 0;
    Expect(GameCore::FindComponentScript(ParticleSystemComponent::StaticType().ID, Count) && Count == 8,
           "Particle System exposes its system and overrides to Lua");
    const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(ParticleSystemComponent::StaticType().ID);
    Expect(Serializer && Serializer->Version == 3, "and is saved, version 3 (naming a system, aiming its beams)");

    ParticleEmitterDesc Wild;
    Wild.MaxParticles = 100000;
    Wild.SpawnRate    = -5.0f;
    Wild.Lifetime     = 0.0f;
    Wild.ColorCurve   = 0.0f;
    Wild.StartColor   = EngineCore::Color(255, 0, 0, 255);
    const ParticleEmitterSettings Clamped = Wild.ToSettings();
    Expect(Clamped.MaxParticles == PARTICLE_MAX_PER_SYSTEM && Clamped.SpawnRate == 0.0f && Clamped.Lifetime > 0.0f
           && Clamped.ColorCurve > 0.0f && Clamped.StartColor[0] == 1.0f && Clamped.StartColor[1] == 0.0f,
           "an emitter's settings are clamped to what the GPU can run, colours to 0-1");

    ParticleEmitterDesc Plain;
    Plain.SpawnRate = 40.0f;
    ParticleSystemComponent Off;
    Off.bEmitting = 0;
    Expect(Off.ToSettings(Plain, 0).SpawnRate == 0.0f, "not emitting spawns nothing");
    ParticleSystemComponent Tuned;
    Tuned.SpawnRateScale = 0.5f;
    Tuned.Tint           = EngineCore::Color(255, 0, 255, 255);
    const ParticleEmitterSettings Run = Tuned.ToSettings(Plain, 0);
    Expect(Run.SpawnRate == 20.0f && Run.StartColor[0] == 1.0f && Run.StartColor[1] == 0.0f && Run.EndColor[2] == 1.0f,
           "the component scales the spawn rate and tints every colour");
    Expect(Tuned.ToSettings(Plain, 0).Seed != Tuned.ToSettings(Plain, 1).Seed,
           "two emitters of one system are seeded apart");
    Plain.MaxParticles = 100;
    Plain.Lifetime     = 2.0f;
    Plain.LifetimeVariance = 0.0f;
    Expect(std::abs(Plain.GetSteadyStateCount() - 80.0f) < 1e-3f, "40 a second for 2 s holds 80 once it has run a while");
    Plain.SpawnRate = 1000.0f;
    Expect(Plain.GetSteadyStateCount() == 100.0f, "never more than Max Particles");

    ParticleEmitterGpu Emitter;
    Emitter.MaxParticles = 10;
    ParticleEmitterSettings Settings;
    Settings.SpawnRate = 25.0f;
    const ParticleCamera Camera;
    const ParticleParams First  = ParticleSimulation::BuildParams(Emitter, Settings, EngineCore::Vector3(1.0, 2.0, 3.0), Camera, 0.1f, 0.0);
    const ParticleParams Second = ParticleSimulation::BuildParams(Emitter, Settings, EngineCore::Vector3(1.0, 2.0, 3.0), Camera, 0.1f, 0.0);
    Expect(First.Spawn[0] == 0 && First.Spawn[1] == 2 && Second.Spawn[0] == 2 && Second.Spawn[1] == 3
           && First.Spawn[2] == 10 && First.Emitter[1] == 2.0f,
           "25 a second over 0.1 s: 2, then 3 with the half carried, each from where the last ended");
    const ParticleParams Hitch = ParticleSimulation::BuildParams(Emitter, Settings, EngineCore::Vector3(), Camera, 5.0f, 0.0);
    Expect(Hitch.Timing[0] <= 0.1f + 1e-6f && Hitch.Spawn[1] <= 3, "a hitch is clamped to a tenth of a second: no burst");
    Settings.SpawnRate = 1000.0f;
    const ParticleParams Flood = ParticleSimulation::BuildParams(Emitter, Settings, EngineCore::Vector3(), Camera, 0.1f, 0.0);
    Expect(Flood.Spawn[1] == 10 && Flood.Spawn[0] < 10 && Flood.Sort[0] == 16,
           "never more than the ring holds a frame, wrapping round it; the sort runs over the next power of two");
    Expect(First.Spawn[3] != Second.Spawn[3], "each frame seeds its spawns differently");
    Expect(Second.PrevEmitter[1] == 2.0f && First.PrevEmitter[1] == 2.0f,
           "spawns are spread from where the emitter was a frame ago (the first frame, where it is)");

    /* Bursts: Burst Count at once, again every Burst Interval, Burst Cycles times. */
    ParticleEmitterGpu Bursting;
    Bursting.MaxParticles = 100;
    ParticleEmitterSettings Burst;
    Burst.SpawnMode  = PSM_Burst;
    Burst.BurstCount = 20;
    uint32_t Spawned[6] = {};
    for(int Frame = 0; Frame < 6; ++Frame)
    {
        Spawned[Frame] = ParticleSimulation::BuildParams(Bursting, Burst, EngineCore::Vector3(), Camera, 0.1f, 0.0).Spawn[1];
    }
    Expect(Spawned[0] == 20 && Spawned[1] == 0 && Spawned[5] == 0, "a burst with no interval fires once");
    Burst.bEmitting = false;
    ParticleSimulation::BuildParams(Bursting, Burst, EngineCore::Vector3(), Camera, 0.1f, 0.0);
    Burst.bEmitting = true;
    Expect(ParticleSimulation::BuildParams(Bursting, Burst, EngineCore::Vector3(), Camera, 0.1f, 0.0).Spawn[1] == 20,
           "switched off and on, it fires again");

    ParticleEmitterGpu Repeating;
    Repeating.MaxParticles = 100;
    Burst.BurstInterval = 0.25f;
    Burst.BurstCycles   = 3;
    uint32_t Total = 0, Bursts = 0;
    for(int Frame = 0; Frame < 20; ++Frame)
    {
        const uint32_t Made = ParticleSimulation::BuildParams(Repeating, Burst, EngineCore::Vector3(), Camera, 0.1f, 0.0).Spawn[1];
        Total  += Made;
        Bursts += Made > 0 ? 1 : 0;
    }
    Expect(Total == 60 && Bursts == 3 && Repeating.BurstsFired == 3, "every 0.25 s, three times: 60 in three bursts, then none");
    Burst.BurstCount = 500;
    Burst.BurstInterval = 0.0f;
    ParticleEmitterGpu Small;
    Small.MaxParticles = 64;
    Expect(ParticleSimulation::BuildParams(Small, Burst, EngineCore::Vector3(), Camera, 0.1f, 0.0).Spawn[1] == 64,
           "a burst bigger than the ring fills it, no more");

    /* Strips: a ribbon's points from the oldest slot round the ring; a beam's from 0 to its end. */
    ParticleEmitterGpu Trail;
    Trail.MaxParticles = 10;
    ParticleEmitterSettings Ribbon;
    Ribbon.Type      = PT_Ribbon;
    Ribbon.SpawnRate = 30.0f;
    const ParticleParams RibbonParams = ParticleSimulation::BuildParams(Trail, Ribbon, EngineCore::Vector3(), Camera, 0.1f, 0.0);
    Expect(RibbonParams.Sort[1] == 3 && RibbonParams.Sort[2] == 10 && RibbonParams.Render[0] == PT_Ribbon,
           "a ribbon is strung from the next slot to spawn into -- the oldest -- round the whole ring");
    ParticleEmitterSettings Beam;
    Beam.Type         = PT_Beam;
    Beam.BeamSegments = 8;
    Beam.BeamNoise    = 5.0f;
    ParticleEmitterGpu Ray;
    Ray.MaxParticles = Beam.GetCapacity();
    const ParticleParams BeamParams = ParticleSimulation::BuildParams(Ray, Beam, EngineCore::Vector3(), Camera, 0.1f, 0.0);
    Expect(Beam.GetCapacity() == 9 && BeamParams.Sort[1] == 0 && BeamParams.Sort[2] == 9 && BeamParams.Spawn[1] == 0
           && BeamParams.BeamNoise[1] == 8.0f && BeamParams.Beam[1] == 200.0f && BeamParams.Beam[3] == 5.0f,
           "a beam holds its 9 points, spawns nothing, and is strung from slot 0 to its end");

    ParticleEmitterDesc BeamDesc;
    BeamDesc.Type         = PT_Beam;
    BeamDesc.BeamSegments = 1000;
    Expect(BeamDesc.ToSettings().BeamSegments == PARTICLE_BEAM_SEGMENTS_MAX && BeamDesc.GetSteadyStateCount() == 129.0f,
           "a beam's segments are clamped to 128; it holds a point more");
    ParticleEmitterDesc BurstDesc;
    BurstDesc.SpawnMode = PSM_Burst;
    BurstDesc.BurstCount = 30;
    BurstDesc.BurstInterval = 0.5f;
    BurstDesc.BurstCycles = 0;
    BurstDesc.Lifetime = 1.2f;
    BurstDesc.LifetimeVariance = 0.0f;
    Expect(BurstDesc.GetSteadyStateCount() == 90.0f, "bursts every 0.5 s living 1.2 s overlap three at a time: 90");
}

/*
 * Particle system assets (ParticleSystemAsset.h): .nmparticle text read and
 * written, its errors, and the cache a proxy finds them in.
 */
static void TestParticleSystemAsset()
{
    std::printf("[particle system asset]\n");

    ParticleSystemDesc System;
    std::string        Error;
    const std::string  Text =
        "# smoke and sparks\n"
        "NimbleParticleSystem 1\n"
        "\n"
        "Emitter Smoke Plume\n"
        "    Material Content/Materials/Smoke.material.nmgraph\n"
        "    SpawnRate 70\n"
        "    Velocity 10, 120, 0\n"
        "    StartColor 225,38,22,150\n"
        "    FaceCamera false\n"
        "    Sparkle 3\n"
        "Emitter Sparks\n"
        "    Enabled false\n"
        "    Offset 0,20,0\n";
    const bool bParsed = ParseParticleSystem(Text, System, Error);
    Expect(bParsed && System.Emitters.size() == 2, "a system of two emitters is read");
    if(bParsed && System.Emitters.size() == 2)
    {
        const ParticleEmitterDesc& Smoke = System.Emitters[0];
        Expect(std::strcmp(Smoke.Name, "Smoke Plume") == 0 && std::strcmp(Smoke.Material, "Content/Materials/Smoke.material.nmgraph") == 0
               && Smoke.SpawnRate == 70.0f && Smoke.Velocity.GetY() == 120.0 && Smoke.StartColor.G == 38 && Smoke.StartColor.A == 150
               && Smoke.Billboard == PB_Fixed && Smoke.Lifetime == ParticleEmitterDesc().Lifetime,
               "names run to the end of the line, settings are read (an old FaceCamera false as a Fixed billboard), unknown keys skipped, the rest at their defaults");
        Expect(System.Emitters[1].bEnabled == 0 && System.Emitters[1].Offset.GetY() == 20.0, "the second emitter's own settings");
    }

    const std::string Written = WriteParticleSystem(System);
    ParticleSystemDesc Again;
    Expect(ParseParticleSystem(Written, Again, Error) && WriteParticleSystem(Again) == Written,
           "written and read back, it writes the same bytes");
    Expect(Written.find("LifetimeVariance 0.25\n") != std::string::npos && Written.find("Sparkle") == std::string::npos
           && Written.find("Billboard Fixed\n") != std::string::npos && Written.find("\n    FaceCamera ") == std::string::npos,
           "every known setting is written, numbers in their shortest form, choices by name, old keys not");
    Expect(ParseParticleSystem("NimbleParticleSystem 1\nEmitter T\n    Type Ribbon\n    SpawnMode Burst\n    MeshOrientation AlignToVelocity\n",
                               System, Error)
           && System.Emitters[0].Type == PT_Ribbon && System.Emitters[0].SpawnMode == PSM_Burst
           && System.Emitters[0].MeshOrientation == PMO_AlignToVelocity, "choices are read by name");
    Expect(!ParseParticleSystem("NimbleParticleSystem 1\nEmitter T\n    Type Laser\n", System, Error) && Error.find("Type") != std::string::npos,
           "an unknown choice is refused");
    ParticleEmitterDesc Shapes;
    Shapes.Type = PT_Mesh;
    Expect(std::strcmp(Shapes.GetMeshPath(), PARTICLE_DEFAULT_SHAPE_MESH) == 0, "a Mesh emitter with no mesh draws a box");
    Shapes.Type = PT_Sprite;
    Expect(std::strcmp(Shapes.GetMeshPath(), PARTICLE_DEFAULT_MESH) == 0, "a Sprite emitter with no mesh, a quad");

    Expect(!ParseParticleSystem("Emitter A\n", System, Error) && Error.find("line 1") != std::string::npos,
           "a file without its NimbleParticleSystem line is refused");
    Expect(!ParseParticleSystem("NimbleParticleSystem 99\n", System, Error), "a newer format is refused");
    Expect(!ParseParticleSystem("NimbleParticleSystem 1\nSpawnRate 3\n", System, Error) && Error.find("line 2") != std::string::npos,
           "a setting before any Emitter is refused, naming its line");
    Expect(!ParseParticleSystem("NimbleParticleSystem 1\nEmitter A\n    StartColor 300,0,0,0\n", System, Error)
           && Error.find("StartColor") != std::string::npos && System.Emitters.empty(),
           "a colour out of 0-255 is refused, and nothing is kept");
    std::string Nine = "NimbleParticleSystem 1\n";
    for(int Each = 0; Each < 9; ++Each) { Nine += "Emitter E\n"; }
    Expect(!ParseParticleSystem(Nine, System, Error), "at most eight emitters");

    /* Curves and gradients: keys read in any order, kept sorted; evaluated straight between keys. */
    const bool bKeys = ParseParticleSystem("NimbleParticleSystem 1\nEmitter K\n    SizeMode Curve\n    SizeKeys 1:0 0:10 0.5:40\n"
                                           "    ColorMode Gradient\n    ColorKeys 0:255,0,0,255 1:0,0,255,0\n"
                                           "    Shape Cone\n    ShapeSurface true\n    ConeAngle 30\n    BoxSize 1,2,3\n",
                                           System, Error);
    Expect(bKeys && System.Emitters[0].SizeKeyCount == 3 && System.Emitters[0].SizeKeys[0].Time == 0.0f
           && System.Emitters[0].SizeKeys[1].Value == 40.0f && System.Emitters[0].ColorKeyCount == 2
           && System.Emitters[0].Shape == PS_Cone && System.Emitters[0].bShapeSurface == 1 && System.Emitters[0].ConeAngle == 30.0f,
           "size and colour keys, the shape and its settings are read; keys sorted by time");
    if(bKeys)
    {
        const ParticleEmitterDesc& Keyed = System.Emitters[0];
        float Color[4];
        float Size = 0.0f;
        Keyed.Evaluate(0.25f, Color, Size);
        Expect(std::abs(Size - 25.0f) < 1e-4f && std::abs(Color[0] - 0.75f) < 1e-3f && std::abs(Color[2] - 0.25f) < 1e-3f
               && std::abs(Color[3] - 0.75f) < 1e-3f, "a quarter of the way: size 25, three quarters red, alpha 0.75");
        Keyed.Evaluate(0.75f, Color, Size);
        Expect(std::abs(Size - 20.0f) < 1e-4f, "past the middle key it falls back toward 0");
        const ParticleEmitterSettings Baked = Keyed.ToSettings();
        Expect(Baked.bColorLut && Baked.bSizeLut && Baked.ColorLut[0][0] == 1.0f && Baked.ColorLut[PARTICLE_LUT_SIZE - 1][2] == 1.0f
               && Baked.SizeLut[0] == 10.0f && Baked.SizeLut[PARTICLE_LUT_SIZE - 1] == 0.0f,
               "and they are baked, birth to death, into the samples the GPU reads");
        const std::string KeyText = WriteParticleSystem(System);
        ParticleSystemDesc KeyAgain;
        Expect(KeyText.find("    SizeKeys 0:10 0.5:40 1:0\n") != std::string::npos
               && KeyText.find("    ColorKeys 0:255,0,0,255 1:0,0,255,0\n") != std::string::npos
               && ParseParticleSystem(KeyText, KeyAgain, Error) && WriteParticleSystem(KeyAgain) == KeyText,
               "keys are written time:value, and read back the same");
    }
    Expect(!ParseParticleSystem("NimbleParticleSystem 1\nEmitter K\n    SizeKeys 0:1 1.5:2\n", System, Error),
           "a key's time past 0-1 is refused");
    Expect(!ParseParticleSystem("NimbleParticleSystem 1\nEmitter K\n    SizeKeys 0:1 0.1:1 0.2:1 0.3:1 0.4:1 0.5:1 0.6:1 0.7:1 0.8:1\n",
                                System, Error), "and a ninth key");

    /* Modules: named, or inferred from a file written before them; one left out is at rest. */
    Expect(ParseParticleSystem("NimbleParticleSystem 1\nEmitter Old\n    Velocity 0,0,0\n    Drag 2\n    EmitterRadius 5\n"
                               "Emitter New\n    Modules Rotation Forces\n    Drag 2\n    EmitterRadius 5\n", System, Error)
           && System.Emitters[0].Modules == (PEM_Shape | PEM_Forces) && System.Emitters[1].Modules == (PEM_Rotation | PEM_Forces),
           "an emitter with no Modules line has those its settings use; one with a line, those it names");
    if(System.Emitters.size() == 2)
    {
        const ParticleEmitterSettings AtRest = System.Emitters[1].ToSettings();
        Expect(AtRest.EmitterRadius == 0.0f && AtRest.Velocity[1] == 0.0f && AtRest.Drag == 2.0f,
               "a module left out is at rest -- no shape, no velocity -- whatever its settings hold; one there runs");
        const std::string ModuleText = WriteParticleSystem(System);
        Expect(ModuleText.find("    Modules Shape Forces\n") != std::string::npos && ModuleText.find("    Modules Forces Rotation\n") != std::string::npos,
               "modules are written by name, in a fixed order");
    }
    Expect(!ParseParticleSystem("NimbleParticleSystem 1\nEmitter X\n    Modules Shape Gravity\n", System, Error), "an unknown module is refused");
    Expect(ParticleEmitterDesc().Modules == PEM_Velocity, "a fresh emitter has Velocity, nothing else optional");

    /* Collision and Flipbook: modules like the rest, at rest when left out. */
    const bool bNewKeys = ParseParticleSystem("NimbleParticleSystem 1\nEmitter C\n    Modules Collision Flipbook\n"
                                              "    CollisionResponse Kill\n    CollisionHeight 25\n    Bounce 0.7\n"
                                              "    FlipbookColumns 8\n    FlipbookRows 2\n    FlipbookFrames 12\n    FlipbookRate 30\n"
                                              "    FlipbookRandomStart true\n    CullDistance 5000\n    PauseOffscreen false\n", System, Error);
    Expect(bNewKeys && System.Emitters[0].HasModule(PEM_Collision) && System.Emitters[0].HasModule(PEM_Flipbook)
           && System.Emitters[0].CollisionResponse == PCR_Kill && System.Emitters[0].FlipbookColumns == 8
           && System.Emitters[0].CullDistance == 5000.0f && System.Emitters[0].bPauseOffscreen == 0,
           "collision, flipbook and culling settings are read, the modules by name");
    if(bNewKeys)
    {
        ParticleEmitterDesc Flipping = System.Emitters[0];
        const ParticleEmitterSettings On = Flipping.ToSettings();
        Expect(On.CollisionResponse == PCR_Kill && On.CollisionPlaneY == 25.0f && On.FlipbookColumns == 8 && On.FlipbookRows == 2
               && On.FlipbookFrames == 12 && On.FlipbookRate == 30.0f && On.bFlipbookRandomStart, "and reach the settings");
        Flipping.Modules = 0;
        const ParticleEmitterSettings Off = Flipping.ToSettings();
        Expect(Off.CollisionResponse == PCR_None && Off.FlipbookColumns == 0, "left out, no collision and no flipbook");
        Flipping.Modules = PEM_Flipbook;
        Flipping.Type    = PT_Ribbon;
        Expect(Flipping.ToSettings().FlipbookColumns == 0, "a ribbon has no flipbook");
        const std::string NewText = WriteParticleSystem(System);
        ParticleSystemDesc NewAgain;
        Expect(NewText.find("    Modules Collision Flipbook\n") != std::string::npos && ParseParticleSystem(NewText, NewAgain, Error)
               && WriteParticleSystem(NewAgain) == NewText, "and written back the same");
    }
    Expect(ParseParticleSystem("NimbleParticleSystem 1\nEmitter Ray\n    Type Beam\n    Velocity 0,100,0\n", System, Error)
           && System.Emitters[0].Modules == 0, "a beam written before modules has none: it uses none");

    /* Start to End, as before keys: no fade is no fade; fades seed a gradient's keys. */
    ParticleEmitterDesc Plain;
    Plain.FadeIn  = 0.0f;
    Plain.FadeOut = 0.0f;
    Plain.EndColor = EngineCore::Color(255, 255, 255, 255);
    const ParticleEmitterSettings PlainSettings = Plain.ToSettings();
    float PlainColors[PARTICLE_LUT_SIZE][4];
    float PlainSizes[PARTICLE_LUT_SIZE];
    PlainSettings.BakeStartEnd(PlainColors, PlainSizes);
    Expect(!PlainSettings.bColorLut && PlainColors[0][3] == 1.0f && PlainColors[PARTICLE_LUT_SIZE - 1][3] == 1.0f && PlainSizes[0] == 10.0f,
           "Start to End with no fades is opaque right to birth and death");
    ParticleEmitterDesc Fading;   // fades in over 0.1, out over 0.3, white to clear
    Fading.SeedColorKeys();
    Fading.SeedSizeKeys();
    Expect(Fading.ColorKeyCount == 4 && Fading.ColorKeys[0].Color.A == 0 && Fading.ColorKeys[1].Time == 0.1f
           && Fading.ColorKeys[1].Color.A > 200 && Fading.ColorKeys[3].Time == 1.0f && Fading.ColorKeys[3].Color.A == 0
           && Fading.SizeKeyCount == 2 && Fading.SizeKeys[1].Value == Fading.EndSize,
           "a gradient chosen over Start to End starts as its colours and fades; a curve as its sizes");
    ParticleEmitterDesc Graded;
    Graded.ColorMode = POL_Keys;
    Graded.ColorKeyCount = 1;
    Graded.ColorKeys[0].Color = EngineCore::Color(255, 255, 255, 255);
    ParticleSystemComponent Tinted;
    Tinted.Tint = EngineCore::Color(255, 0, 0, 255);
    const ParticleEmitterSettings TintedSettings = Tinted.ToSettings(Graded, 0);
    Expect(TintedSettings.bColorLut && TintedSettings.ColorLut[5][0] == 1.0f && TintedSettings.ColorLut[5][1] == 0.0f,
           "the component's tint reaches a gradient too");

    Expect(GetDefaultParticleSystem() && GetDefaultParticleSystem()->Emitters.size() == 1,
           "a component naming no system runs one emitter at its defaults");

    auto Stored = std::make_shared<ParticleSystemDesc>(Again);
    ParticleSystemCache::Store("Test/Stored.nmparticle", Stored);
    Expect(ParticleSystemCache::Find("Test/Stored.nmparticle") == Stored, "a stored system is found by its path");
    ParticleSystemCache::Invalidate("Test/Stored.nmparticle");
    Expect(ParticleSystemCache::Find("Test/Stored.nmparticle") == nullptr, "and once invalidated, read from its file -- here, none");
    ParticleSystemCache::Invalidate(std::string());
}

#if WITH_EDITOR
/*
 * OPT-IN, on a real GPU: NIMBLE_SMOKE_GPU=Vulkan or D3D12. The compute passes read
 * back (spawned, moved, sorted farthest first) and a plume drawn through the
 * instanced-mesh path: red where it is born, dark where it has aged.
 */
static void TestParticlesOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[particles on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[particles on the GPU: %s]\n", GetShaderPlatformName(Backend));
    using EngineCore::Vector3;

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    if(bInit)
    {
        Rend.MountShaderCache("SandboxGame");

        /* 1. The compute passes alone, from a render command: 50 a second along +Z for 11 steps of 0.1 s. */
        struct Readback
        {
            bool bSupported = false, bMade = false, bSimulated = true, bRead = false;
            std::vector<ParticleSample>         Particles;
            std::vector<ParticleInstanceSample> Instances;
        } Out;
        ENQUEUE_RENDER_COMMAND(ParticleCompute)([&Out](RenderContext& Ctx)
        {
            Out.bSupported = Ctx.SupportsParticles();
            ParticleEmitterGpu* Emitter = Ctx.CreateParticleEmitter(64);
            Out.bMade = Emitter != nullptr;
            if(!Emitter) { return; }
            ParticleEmitterSettings Settings;
            Settings.MaxParticles     = 64;
            Settings.SpawnRate        = 50.0f;
            Settings.Lifetime         = 100.0f;
            Settings.LifetimeVariance = 0.0f;
            Settings.Velocity[0] = 0.0f; Settings.Velocity[1] = 0.0f; Settings.Velocity[2] = 100.0f;
            Settings.StartColor[0] = 1.0f; Settings.StartColor[1] = 0.0f; Settings.StartColor[2] = 0.0f; Settings.StartColor[3] = 1.0f;
            Settings.EndColor[0] = 1.0f;   Settings.EndColor[1] = 0.0f;   Settings.EndColor[2] = 0.0f;   Settings.EndColor[3] = 1.0f;
            Settings.FadeIn = 0.0f;
            Settings.FadeOut = 0.0f;
            for(int Step = 0; Step < 11; ++Step)
            {
                Out.bSimulated = Ctx.SimulateParticles(*Emitter, Settings, Vector3(0.0, 0.0, 0.0), 0.1f) && Out.bSimulated;
            }
            Out.bRead = Ctx.ReadParticles(*Emitter, Out.Particles) && Ctx.ReadParticleInstances(*Emitter, Out.Instances);
            Ctx.ReleaseParticleEmitter(Emitter);
        });
        FlushRenderCommands();

        Expect(Out.bSupported && Out.bMade && Out.bSimulated && Out.bRead && Out.Particles.size() == 64 && Out.Instances.size() == 64,
               "the GPU runs compute: an emitter is made, simulated eleven times and read back");
        int    Alive = 0;
        bool   bOnPath = true;
        float  Oldest = 0.0f;
        for(const ParticleSample& Each : Out.Particles)
        {
            if(!Each.IsAlive()) { continue; }
            ++Alive;
            Oldest  = std::max(Oldest, Each.Age);
            bOnPath = bOnPath && std::abs(Each.Position[0]) < 1e-3f && std::abs(Each.Position[1]) < 1e-3f
                   && std::abs(Each.Position[2] - Each.Age * 100.0f) < 0.05f && Each.Age <= 1.1f + 1e-4f;
        }
        std::printf("    %d alive, the oldest %.3f s\n", Alive, Oldest);
        Expect(Alive == 55, "50 a second for 1.1 s: 55 particles, the rest of the ring unused");
        Expect(bOnPath && Oldest > 1.0f, "each has moved 100 units a second along +Z for as long as it has lived");

        bool  bSorted = true, bDeadLast = true, bRed = true;
        float Last    = 1e30f;
        for(size_t Slot = 0; Slot < Out.Instances.size(); ++Slot)
        {
            const ParticleInstanceSample& Each = Out.Instances[Slot];
            const bool bDead = Each.Transform[15] == 0.0f;
            if(Slot < 55)
            {
                bSorted = bSorted && !bDead && Each.Transform[14] <= Last + 1e-3f;
                Last    = Each.Transform[14];
                bRed    = bRed && Each.Color[0] > 0.99f && Each.Color[1] < 0.01f;
            }
            else
            {
                bDeadLast = bDeadLast && bDead;
            }
        }
        Expect(bSorted && bDeadLast, "the instances are sorted farthest first along the view, the unused slots after them");
        Expect(bRed && !Out.Instances.empty() && std::abs(Out.Instances[0].Transform[0] - 10.0f) < 1e-3f,
               "each instance is the particle's colour and size (10, the default) on the camera's axes");

        /* 1b. Each type and mode, from render commands: InSteps of 0.1 s, the emitter at InAt(step). */
        struct TypeRun
        {
            bool     bRan      = false;
            bool     bCounted  = false;
            uint32_t DrawCount = 0;      // what the draws draw: the live count the simulation wrote
            std::vector<ParticleSample>         Particles;
            std::vector<ParticleInstanceSample> Instances;
        };
        const auto RunType = [](const ParticleEmitterSettings& InSettings, const int InSteps,
                                const std::function<Vector3(int)>& InAt)
        {
            TypeRun Run;
            ENQUEUE_RENDER_COMMAND(ParticleTypes)([&Run, &InSettings, InSteps, &InAt](RenderContext& Ctx)
            {
                ParticleEmitterGpu* Emitter = Ctx.CreateParticleEmitter(InSettings.GetCapacity(), InSettings.Type);
                if(!Emitter) { return; }
                bool bOk = true;
                for(int Step = 0; Step < InSteps; ++Step)
                {
                    bOk = Ctx.SimulateParticles(*Emitter, InSettings, InAt(Step), 0.1f) && bOk;
                }
                Run.bRan = bOk && Ctx.ReadParticles(*Emitter, Run.Particles) && Ctx.ReadParticleInstances(*Emitter, Run.Instances);
                Run.bCounted = Ctx.ReadParticleDrawCount(*Emitter, Run.DrawCount);
                Ctx.ReleaseParticleEmitter(Emitter);
            });
            FlushRenderCommands();
            return Run;
        };
        const auto Still = [](int) { return Vector3(0.0, 0.0, 0.0); };
        const auto FirstLive = [](const TypeRun& InRun) -> const ParticleInstanceSample*
        {
            for(const ParticleInstanceSample& Each : InRun.Instances)
            {
                if(Each.Transform[15] != 0.0f) { return &Each; }
            }
            return nullptr;
        };
        const auto Near = [](const float InA, const float InB) { return std::abs(InA - InB) < 1e-2f; };

        ParticleEmitterSettings Moving;
        Moving.MaxParticles     = 32;
        Moving.SpawnRate        = 50.0f;
        Moving.Lifetime         = 100.0f;
        Moving.LifetimeVariance = 0.0f;
        Moving.Velocity[0] = 100.0f; Moving.Velocity[1] = 0.0f; Moving.Velocity[2] = 0.0f;

        Moving.Billboard = PB_Vertical;
        const TypeRun Vertical = RunType(Moving, 3, Still);
        const ParticleInstanceSample* V = FirstLive(Vertical);
        Expect(Vertical.bRan && V && Near(V->Transform[4], 0.0f) && Near(V->Transform[5], 10.0f) && Near(V->Transform[6], 0.0f)
               && Near(V->Transform[1], 0.0f), "a Vertical sprite stands up world +Y, turned only about it");

        Moving.Billboard       = PB_VelocityAligned;
        Moving.VelocityStretch = 0.05f;
        const TypeRun Aligned = RunType(Moving, 3, Still);
        const ParticleInstanceSample* A = FirstLive(Aligned);
        Expect(Aligned.bRan && A && Near(A->Transform[4], 15.0f) && Near(A->Transform[5], 0.0f) && Near(A->Transform[6], 0.0f),
               "a Velocity Aligned sprite lies along its motion, 10 long plus 100 a second times 0.05 s");

        Moving.Billboard       = PB_FaceCamera;
        Moving.bRandomRotation = true;
        Moving.RotationRate    = 90.0f;
        const TypeRun Spun = RunType(Moving, 3, Still);
        int Live = 0, Turned = 0;
        bool bSized = Spun.bRan;
        for(const ParticleInstanceSample& Each : Spun.Instances)
        {
            if(Each.Transform[15] == 0.0f) { continue; }
            ++Live;
            bSized  = bSized && Near(std::sqrt(Each.Transform[0] * Each.Transform[0] + Each.Transform[1] * Each.Transform[1]
                                               + Each.Transform[2] * Each.Transform[2]), 10.0f);
            Turned += Near(Each.Transform[0], 10.0f) ? 0 : 1;
        }
        Expect(Live == 15 && Turned >= Live - 1 && bSized, "sprites with Random Rotation are each turned in their plane, their size kept");
        Moving.bRandomRotation = false;
        Moving.RotationRate    = 0.0f;

        Moving.Type            = PT_Mesh;
        Moving.MeshOrientation = PMO_AlignToVelocity;
        const TypeRun Arrows = RunType(Moving, 3, Still);
        const ParticleInstanceSample* M = FirstLive(Arrows);
        Expect(Arrows.bRan && M && Near(M->Transform[8], 10.0f) && Near(M->Transform[9], 0.0f) && Near(M->Transform[10], 0.0f),
               "an Align to Velocity mesh points its +Z along its motion");
        Moving.MeshOrientation = PMO_RandomSpin;
        Moving.RotationRate    = 180.0f;
        const TypeRun Debris = RunType(Moving, 3, Still);
        const ParticleInstanceSample* D = FirstLive(Debris);
        const auto Column = [](const ParticleInstanceSample& InSample, const int InColumn)
        {
            return Vector3(InSample.Transform[InColumn * 4], InSample.Transform[InColumn * 4 + 1], InSample.Transform[InColumn * 4 + 2]);
        };
        bool bRigid = D != nullptr;
        if(D)
        {
            const Vector3 X = Column(*D, 0), Y = Column(*D, 1), Z = Column(*D, 2);
            bRigid = Near(float(X.Dot(Y)), 0.0f) && Near(float(Y.Dot(Z)), 0.0f) && Near(float(X.GetLength()), 10.0f)
                  && !Near(float(X.GetX()), 10.0f);
        }
        Expect(Debris.bRan && bRigid, "a Random Spin mesh is turned rigidly, its size kept");

        ParticleEmitterSettings Bang;
        Bang.MaxParticles = 64;
        Bang.SpawnMode    = PSM_Burst;
        Bang.BurstCount   = 20;
        Bang.Lifetime     = 100.0f;
        const TypeRun Burst = RunType(Bang, 4, Still);
        int BurstAlive = 0;
        float BurstOldest = 0.0f;
        for(const ParticleSample& Each : Burst.Particles)
        {
            if(Each.IsAlive()) { ++BurstAlive; BurstOldest = std::max(BurstOldest, Each.Age); }
        }
        Expect(Burst.bRan && BurstAlive == 20 && BurstOldest <= 0.4f + 1e-4f, "a burst of 20 makes 20 at once, then no more");
        Expect(Burst.bCounted && Burst.DrawCount == 20 && Burst.Instances.size() == 64,
               "and its draws draw those 20, not the 64 its ring holds (the count the sort wrote, drawn indirectly)");

        /* A ribbon behind an emitter moving along +X: segment after segment sharing their edges exactly. */
        ParticleEmitterSettings Trail;
        Trail.Type             = PT_Ribbon;
        Trail.MaxParticles     = 64;
        Trail.SpawnRate        = 50.0f;
        Trail.Lifetime         = 100.0f;
        Trail.LifetimeVariance = 0.0f;
        Trail.Velocity[1]      = 0.0f;
        const TypeRun Ribbon = RunType(Trail, 10, [](const int InStep) { return Vector3(30.0 * InStep, 0.0, 0.0); });
        int Segments = 0;
        bool bJoined = Ribbon.bRan, bWide = Ribbon.bRan;
        for(size_t Index = 0; Index + 1 < Ribbon.Instances.size(); ++Index)
        {
            const ParticleInstanceSample& Here = Ribbon.Instances[Index];
            const ParticleInstanceSample& Next = Ribbon.Instances[Index + 1];
            if(Here.Transform[15] == 0.0f) { continue; }
            ++Segments;
            bWide = bWide && Near(float(Column(Here, 0).GetLength()), 10.0f);
            if(Next.Transform[15] == 0.0f) { continue; }
            /* This segment's far left corner (L0 + (L1 - L0)) is the next one's near left corner (L0). */
            const Vector3 Far = Column(Here, 3) + Column(Here, 1);
            bJoined = bJoined && float((Far - Column(Next, 3)).GetLength()) < 1e-2f;
        }
        int RibbonAlive = 0;
        for(const ParticleSample& Each : Ribbon.Particles) { RibbonAlive += Each.IsAlive() ? 1 : 0; }
        std::printf("    ribbon: %d points, %d segments\n", RibbonAlive, Segments);
        Expect(RibbonAlive == 50 && Segments == RibbonAlive - 1, "a ribbon of 50 points is 49 segments");
        bool bPacked = Ribbon.bRan;
        for(size_t Index = 0; Index < Ribbon.Instances.size(); ++Index)
        {
            bPacked = bPacked && (Ribbon.Instances[Index].Transform[15] != 0.0f) == (Index < 49);
        }
        Expect(bPacked && Ribbon.bCounted && Ribbon.DrawCount == 49,
               "packed to the front in order, the slots after cleared, and its draws draw the 49");
        Expect(bJoined && bWide, "each segment starts where the last ended, as wide as the size, with no gap");
        bool bSpread = Ribbon.bRan && Ribbon.Particles.size() == 64;
        float LastX = -1e30f;
        for(int Slot = 0; bSpread && Slot < 50; ++Slot)
        {
            const float X = Ribbon.Particles[size_t(Slot)].Position[0];
            bSpread = bSpread && X >= LastX - 1e-3f;
            LastX   = X;
        }
        Expect(bSpread && LastX > 240.0f, "spawns are spread along the emitter's path, in the order they were born");

        /* Shapes: where each is born (velocity 0, so where it is), and which way out it goes. */
        ParticleEmitterSettings Shaped;
        Shaped.MaxParticles     = 256;
        Shaped.SpawnRate        = 2000.0f;
        Shaped.Lifetime         = 100.0f;
        Shaped.LifetimeVariance = 0.0f;
        Shaped.Velocity[1]      = 0.0f;
        const auto Spawned = [&](const ParticleEmitterSettings& InSettings)
        {
            std::vector<ParticleSample> Alive;
            for(const ParticleSample& Each : RunType(InSettings, 1, Still).Particles) { if(Each.IsAlive()) { Alive.push_back(Each); } }
            return Alive;
        };
        Shaped.Shape = PS_Box;
        Shaped.BoxSize[0] = 200.0f; Shaped.BoxSize[1] = 100.0f; Shaped.BoxSize[2] = 50.0f;
        bool bInside = true, bFills = false;
        const std::vector<ParticleSample> InBox = Spawned(Shaped);
        for(const ParticleSample& Each : InBox)
        {
            bInside = bInside && std::abs(Each.Position[0]) <= 100.01f && std::abs(Each.Position[1]) <= 50.01f && std::abs(Each.Position[2]) <= 25.01f;
            bFills  = bFills || std::abs(Each.Position[0]) > 60.0f;
        }
        Expect(InBox.size() == 200 && bInside && bFills, "a Box spawns within its size, all through it");
        Shaped.bShapeSurface = true;
        bool bOnFace = true;
        for(const ParticleSample& Each : Spawned(Shaped))
        {
            const float Face = std::max({ std::abs(Each.Position[0]) / 100.0f, std::abs(Each.Position[1]) / 50.0f, std::abs(Each.Position[2]) / 25.0f });
            bOnFace = bOnFace && std::abs(Face - 1.0f) < 1e-3f;
        }
        Expect(bOnFace, "on its surface, every one on a face");

        Shaped.Shape = PS_Ring;
        Shaped.EmitterRadius = 80.0f;
        bool bOnRing = true;
        for(const ParticleSample& Each : Spawned(Shaped))
        {
            bOnRing = bOnRing && std::abs(std::hypot(Each.Position[0], Each.Position[2]) - 80.0f) < 1e-2f && std::abs(Each.Position[1]) < 1e-3f;
        }
        Expect(bOnRing, "a Ring's surface is its circle, about +Y");
        Shaped.bShapeSurface = false;
        Shaped.RingWidth = 20.0f;
        bool bInRing = true;
        for(const ParticleSample& Each : Spawned(Shaped))
        {
            const float R = std::hypot(Each.Position[0], Each.Position[2]);
            bInRing = bInRing && R >= 70.0f - 1e-2f && R <= 90.0f + 1e-2f;
        }
        Expect(bInRing, "and with a width, a band 20 across it");

        Shaped.Shape = PS_Hemisphere;
        Shaped.EmitterRadius = 50.0f;
        Shaped.bShapeSurface = true;
        bool bDome = true;
        for(const ParticleSample& Each : Spawned(Shaped))
        {
            const float R = std::sqrt(Each.Position[0] * Each.Position[0] + Each.Position[1] * Each.Position[1] + Each.Position[2] * Each.Position[2]);
            bDome = bDome && std::abs(R - 50.0f) < 1e-2f && Each.Position[1] >= -1e-3f;
        }
        Expect(bDome, "a Hemisphere's surface is the upper half of the sphere's shell");

        Shaped.Shape      = PS_Cone;
        Shaped.ConeAngle  = 20.0f;
        Shaped.ShapeSpeed = 100.0f;
        bool bInCone = true, bOpens = false;
        for(const ParticleSample& Each : Spawned(Shaped))
        {
            const float Speed = std::sqrt(Each.Velocity[0] * Each.Velocity[0] + Each.Velocity[1] * Each.Velocity[1] + Each.Velocity[2] * Each.Velocity[2]);
            const float Angle = std::acos(std::clamp(Each.Velocity[1] / std::max(Speed, 1e-6f), -1.0f, 1.0f)) * 180.0f / 3.14159265f;
            bInCone = bInCone && std::abs(Speed - 100.0f) < 1e-2f && Angle <= 20.01f;
            bOpens  = bOpens || Angle > 12.0f;
            bInCone = bInCone && std::abs(std::hypot(Each.Position[0] - Each.Velocity[0] * Each.Age, Each.Position[2] - Each.Velocity[2] * Each.Age) - 50.0f) < 0.05f;
        }
        Expect(bInCone && bOpens, "a Cone sends each out at its Outward Speed within its angle of +Y, from its rim");

        Shaped.ConeAngle = 0.0f;
        Shaped.ShapeAxisX[0] = 0.0f; Shaped.ShapeAxisX[1] = 1.0f; Shaped.ShapeAxisX[2] = 0.0f;   // turned 90 degrees about +Z:
        Shaped.ShapeAxisY[0] = -1.0f; Shaped.ShapeAxisY[1] = 0.0f; Shaped.ShapeAxisY[2] = 0.0f;  // its +Y is the world's -X
        bool bTurnedCone = true;
        for(const ParticleSample& Each : Spawned(Shaped))
        {
            bTurnedCone = bTurnedCone && std::abs(Each.Velocity[0] + 100.0f) < 1e-2f && std::abs(Each.Position[0] - Each.Velocity[0] * Each.Age) < 1e-2f;
        }
        Expect(bTurnedCone, "a shape turns with its axes: the cone fires along the world's -X, its rim upright");

        /* Over life from keys, drawn: velocity along +Z puts each one's age in its depth. */
        ParticleEmitterDesc Keyed;
        Keyed.MaxParticles     = 64;
        Keyed.SpawnRate        = 40.0f;
        Keyed.Lifetime         = 1.0f;
        Keyed.LifetimeVariance = 0.0f;
        Keyed.Velocity         = Vector3(0.0, 0.0, 100.0);
        Keyed.ColorMode        = POL_Keys;
        Keyed.ColorKeyCount    = 2;
        Keyed.ColorKeys[0]     = ParticleColorKey{ 0.0f, EngineCore::Color(255, 0, 0, 255) };
        Keyed.ColorKeys[1]     = ParticleColorKey{ 1.0f, EngineCore::Color(0, 0, 255, 255) };
        Keyed.SizeMode         = POL_Keys;
        Keyed.SizeKeyCount     = 2;
        Keyed.SizeKeys[0]      = ParticleSizeKey{ 0.0f, 10.0f };
        Keyed.SizeKeys[1]      = ParticleSizeKey{ 1.0f, 30.0f };
        const TypeRun Graded = RunType(Keyed.ToSettings(), 8, Still);
        int Checked = 0;
        float ColorError = 0.0f, SizeError = 0.0f;
        for(const ParticleInstanceSample& Each : Graded.Instances)
        {
            if(Each.Transform[15] == 0.0f) { continue; }
            const float Age  = Each.Transform[14] / 100.0f;
            const float Size = float(Column(Each, 0).GetLength());
            ColorError = std::max({ ColorError, std::abs(Each.Color[0] - (1.0f - Age)), std::abs(Each.Color[2] - Age) });
            SizeError  = std::max(SizeError, std::abs(Size - (10.0f + 20.0f * Age)));
            ++Checked;
        }
        std::printf("    keys: %d checked, colour off by %.4f, size by %.3f at most\n", Checked, ColorError, SizeError);
        Expect(Graded.bRan && Checked > 20 && ColorError < 0.01f && SizeError < 0.05f,
               "each is drawn the gradient's colour and the curve's size at its age");

        /* Collision: falling from 100 onto a plane at 0 for 1.5 s -- bouncing, or dying there. */
        ParticleEmitterSettings Falling;
        Falling.MaxParticles     = 64;
        Falling.SpawnRate        = 40.0f;
        Falling.Lifetime         = 100.0f;
        Falling.LifetimeVariance = 0.0f;
        Falling.Velocity[0] = 0.0f; Falling.Velocity[1] = -200.0f; Falling.Velocity[2] = 0.0f;
        Falling.CollisionResponse = PCR_Bounce;
        Falling.CollisionPlaneY   = 0.0f;
        Falling.Bounce            = 0.5f;
        Falling.Friction          = 0.0f;
        const auto Above = [](int) { return Vector3(0.0, 100.0, 0.0); };
        const TypeRun Bouncing = RunType(Falling, 15, Above);
        int Bounced = 0, Fallen = 0;
        float Lowest = 1e30f;
        for(const ParticleSample& Each : Bouncing.Particles)
        {
            if(!Each.IsAlive()) { continue; }
            ++Fallen;
            Lowest  = std::min(Lowest, Each.Position[1]);
            Bounced += Each.Velocity[1] > 0.0f ? 1 : 0;
        }
        std::printf("    collision: %d alive, %d rising, lowest at %.2f\n", Fallen, Bounced, Lowest);
        Expect(Bouncing.bRan && Fallen == 60 && Lowest >= -1e-3f && Bounced > 0,
               "particles falling onto the plane never go below it, and bounce back up");
        Falling.CollisionResponse = PCR_Kill;
        const TypeRun Killed = RunType(Falling, 15, Above);
        int Survivors = 0;
        bool bAboveAll = true;
        for(const ParticleSample& Each : Killed.Particles)
        {
            if(!Each.IsAlive()) { continue; }
            ++Survivors;
            bAboveAll = bAboveAll && Each.Position[1] >= 0.0f;
        }
        Expect(Killed.bRan && Survivors < 30 && Survivors > 0 && bAboveAll && Killed.bCounted && Killed.DrawCount == uint32_t(Survivors),
               "with Kill they die at it: only those still falling live, and only they are drawn");

        /* Flipbook: each instance's frame, in its transform's bottom row, from its age. */
        ParticleEmitterSettings Book = Moving;
        Book.Type            = PT_Sprite;
        Book.Billboard       = PB_FaceCamera;
        Book.Lifetime        = 1.0f;
        Book.FlipbookColumns = 4;
        Book.FlipbookRows    = 2;
        Book.FlipbookFrames  = 8;
        Book.FlipbookCycles  = 1.0f;
        Book.Velocity[0] = 0.0f; Book.Velocity[1] = 0.0f; Book.Velocity[2] = 100.0f;   // depth is age
        const TypeRun OverLife = RunType(Book, 9, Still);
        bool bFramed = OverLife.bRan;
        int  Framed  = 0;
        for(const ParticleInstanceSample& Each : OverLife.Instances)
        {
            if(Each.Transform[15] == 0.0f) { continue; }
            const float Age = Each.Transform[14] / 100.0f;
            const float Expected = std::floor(std::min(Age, 0.9999f) * 8.0f);
            bFramed = bFramed && std::abs(Each.Transform[3] - Expected) < 0.5f + (std::abs(Age * 8.0f - std::round(Age * 8.0f)) < 1e-3f ? 1.0f : 0.0f)
                   && Each.Transform[7] == 4.0f && Each.Transform[11] == 2.0f;
            ++Framed;
        }
        Expect(bFramed && Framed > 20, "over its life, each is on the frame its age reaches, of 4 x 2");
        Book.FlipbookRate = 10.0f;
        const TypeRun AtRate = RunType(Book, 9, Still);
        bool bTimed = AtRate.bRan;
        for(const ParticleInstanceSample& Each : AtRate.Instances)
        {
            if(Each.Transform[15] == 0.0f) { continue; }
            const float Seconds = Each.Transform[14] / 100.0f;
            const float Expected = std::fmod(std::floor(Seconds * 10.0f), 8.0f);
            bTimed = bTimed && (std::abs(Each.Transform[3] - Expected) < 0.5f || std::abs(Seconds * 10.0f - std::round(Seconds * 10.0f)) < 1e-3f);
        }
        Expect(bTimed, "at a frame rate, 10 a second, looping round the 8");
        Book.FlipbookColumns = 0;
        const TypeRun NoBook = RunType(Book, 3, Still);
        const ParticleInstanceSample* Plain = FirstLive(NoBook);
        Expect(Plain && Plain->Transform[3] == 0.0f && Plain->Transform[7] == 0.0f && Plain->Transform[11] == 0.0f && Plain->Transform[15] == 1.0f,
               "without one, the bottom row is an affine transform's");

        /* A beam: its points from the emitter to its end; jittered in the middle only. */
        ParticleEmitterSettings Ray;
        Ray.Type         = PT_Beam;
        Ray.BeamSegments = 8;
        Ray.BeamEnd[0] = 0.0f; Ray.BeamEnd[1] = 200.0f; Ray.BeamEnd[2] = 0.0f;
        const TypeRun Straight = RunType(Ray, 1, Still);
        bool bOnLine = Straight.bRan && Straight.Particles.size() == 9;
        for(size_t Point = 0; bOnLine && Point < 9; ++Point)
        {
            const ParticleSample& Each = Straight.Particles[Point];
            bOnLine = Each.IsAlive() && Near(Each.Position[1], 25.0f * float(Point)) && Near(Each.Position[0], 0.0f);
        }
        Expect(bOnLine, "a beam's 9 points run evenly from the emitter to its end");
        Ray.BeamNoise = 10.0f;
        const TypeRun Jitter = RunType(Ray, 3, Still);
        float Stray = 0.0f;
        for(const ParticleSample& Each : Jitter.Particles)
        {
            Stray = std::max(Stray, std::sqrt(Each.Position[0] * Each.Position[0] + Each.Position[2] * Each.Position[2]));
        }
        const bool bEnds = Jitter.Particles.size() == 9 && Near(Jitter.Particles[0].Position[0], 0.0f)
                        && Near(Jitter.Particles[8].Position[0], 0.0f) && Near(Jitter.Particles[8].Position[1], 200.0f);
        std::printf("    beam: strays %.2f at most\n", Stray);
        Expect(Jitter.bRan && bEnds && Stray > 0.5f && Stray <= 10.0f + 1e-3f, "with noise, its middle strays up to 10 units; its ends stay");
        int BeamSegments = 0;
        for(const ParticleInstanceSample& Each : Jitter.Instances) { BeamSegments += Each.Transform[15] != 0.0f ? 1 : 0; }
        Expect(BeamSegments == 8 && Jitter.bCounted && Jitter.DrawCount == 8, "and it is strung as 8 segments, which its draws draw");

        /* 2. Drawn: a plume through the instanced-mesh path, seen side on, red at the bottom and dark at the top. */
        const EngineCore::Size Viewport(256, 256);
        const RenderView Side = RenderView::MakePerspective(Viewport, Vector3(0.0, 100.0, -600.0), Vector3(0.0, 0.0, 1.0),
                                                            40.0f, 10.0f, 5000.0f);
        Rend.SetViewOverride(&Side);
        GameCore::World World;
        Rend.BindWorld(World);

        const GameCore::Entity Plume = World.CreateEntity("Plume");
        GameCore::TransformComponent Transform;
        Plume.AddComponent(Transform);
        /* A system made in code, stored under a path as the editor's preview does. */
        auto PlumeSystem = std::make_shared<ParticleSystemDesc>();
        ParticleEmitterDesc& Puffs = PlumeSystem->Emitters.emplace_back();
        std::snprintf(Puffs.Material, sizeof(Puffs.Material), "%s", "Content/Materials/ParticleProbe.material.nmgraph");
        Puffs.MaxParticles     = 512;
        Puffs.SpawnRate        = 400.0f;
        Puffs.Lifetime         = 1.0f;
        Puffs.LifetimeVariance = 0.0f;
        Puffs.Velocity         = Vector3(0.0, 200.0, 0.0);
        Puffs.StartSize        = 14.0f;
        Puffs.EndSize          = 14.0f;
        Puffs.StartColor       = EngineCore::Color(255, 0, 0, 255);
        Puffs.EndColor         = EngineCore::Color(0, 0, 0, 255);
        ParticleSystemCache::Store("Test/Plume.nmparticle", PlumeSystem);

        ParticleSystemComponent Particles;
        std::snprintf(Particles.System, sizeof(Particles.System), "%s", "Test/Plume.nmparticle");
        Plume.AddComponent(Particles);

        /* Real time: the simulation follows the render thread's clock. */
        uint64_t Frame = 1;
        for(int Each = 0; Each < 90; ++Each)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const uint64_t Request = Rend.RequestSceneCapture();
        for(int Each = 0; Each < 2; ++Each)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
        }
        EngineCore::Image Pixels;
        std::string       Error;
        const bool bCaptured = Request != 0 && Rend.TakeMaterialPreview(Request, Pixels, Error) && Pixels.GetWidth() == 256;
        Expect(bCaptured, "the plume is captured");
        if(bCaptured)
        {
            const bool bBgra = Pixels.GetFormat() == EImageFormat::BGRA8;
            /* World y to a row: the camera at y 100 looks along +Z, 600 away. */
            const auto RowOf = [](const double InY)
            {
                const double Focal = 1.0 / std::tan(20.0 * 3.14159265 / 180.0);
                return int(std::lround(128.0 - (InY - 100.0) / 600.0 * Focal * 128.0));
            };
            const auto Red = [&](const int InRow)
            {
                int Best = 0;   // the reddest pixel across the plume's middle at that row
                for(int X = 120; X <= 136; ++X)
                {
                    const uint8_t* Px = Pixels.GetData() + Pixels.GetStride() * InRow + X * BytesPerPixel(Pixels.GetFormat());
                    Best = std::max(Best, int(bBgra ? Px[2] : Px[0]) - int(Px[1]));
                }
                return Best;
            };
            const int Born = Red(RowOf(20.0)), Middle = Red(RowOf(100.0)), Aged = Red(RowOf(180.0));
            std::printf("    red over green: %d at 20 units up, %d at 100, %d at 180\n", Born, Middle, Aged);
            Expect(Born > 150 && Born > Middle && Middle > Aged && Aged < 70,
                   "the plume is red where it is born and darkens as it rises and ages");
        }

        /* 3. Strips drawn: a ribbon rising at x 100 and a beam across at y 250, both red, the plume gone. */
        auto Strips = std::make_shared<ParticleSystemDesc>();
        ParticleEmitterDesc& Rising = Strips->Emitters.emplace_back();
        std::snprintf(Rising.Material, sizeof(Rising.Material), "%s", "Content/Materials/ParticleProbe.material.nmgraph");
        Rising.Type             = PT_Ribbon;
        Rising.Offset           = Vector3(100.0, 0.0, 0.0);
        Rising.MaxParticles     = 128;
        Rising.SpawnRate        = 60.0f;
        Rising.Lifetime         = 1.5f;
        Rising.LifetimeVariance = 0.0f;
        Rising.Velocity         = Vector3(0.0, 200.0, 0.0);
        Rising.StartSize        = 16.0f;
        Rising.EndSize          = 16.0f;
        Rising.StartColor       = EngineCore::Color(255, 0, 0, 255);
        Rising.EndColor         = EngineCore::Color(255, 0, 0, 255);
        Rising.FadeIn           = 0.0f;
        Rising.FadeOut          = 0.0f;
        ParticleEmitterDesc& Across = Strips->Emitters.emplace_back(Rising);
        Across.Type         = PT_Beam;
        Across.Offset       = Vector3(-150.0, 250.0, 0.0);
        Across.BeamEnd      = Vector3(300.0, 0.0, 0.0);
        Across.BeamSegments = 12;
        ParticleSystemCache::Store("Test/Strips.nmparticle", Strips);
        if(auto* Running = Plume.GetComponent<ParticleSystemComponent>())
        {
            std::snprintf(Running->System, sizeof(Running->System), "%s", "Test/Strips.nmparticle");
        }
        /* Two seconds or so: the ribbon (1.5 s at 200 a second) has risen all the way. */
        for(int Each = 0; Each < 120; ++Each)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const uint64_t StripRequest = Rend.RequestSceneCapture();
        for(int Each = 0; Each < 2; ++Each)
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
        }
        EngineCore::Image StripPixels;
        const bool bStrips = StripRequest != 0 && Rend.TakeMaterialPreview(StripRequest, StripPixels, Error) && StripPixels.GetWidth() == 256;
        Expect(bStrips, "the ribbon and beam are captured");
        if(bStrips)
        {
            const bool bBgra = StripPixels.GetFormat() == EImageFormat::BGRA8;
            const double Focal = 1.0 / std::tan(20.0 * 3.14159265 / 180.0);
            const auto Pixel = [&](const double InX, const double InY)   // world x, y at z 0 to a pixel's red over green
            {
                const int Column = int(std::lround(128.0 + InX / 600.0 * Focal * 128.0));
                const int Row    = int(std::lround(128.0 - (InY - 100.0) / 600.0 * Focal * 128.0));
                const uint8_t* Px = StripPixels.GetData() + StripPixels.GetStride() * Row + Column * BytesPerPixel(StripPixels.GetFormat());
                return int(bBgra ? Px[2] : Px[0]) - int(Px[1]);
            };
            const int RibbonLow = Pixel(100.0, 50.0), RibbonHigh = Pixel(100.0, 200.0), BeamLeft = Pixel(-100.0, 250.0);
            const int BeamMiddle = Pixel(0.0, 250.0), Between = Pixel(0.0, 150.0), PlumeWas = Pixel(0.0, 20.0);
            std::printf("    red over green: ribbon %d at y 50, %d at y 200; beam %d left, %d middle; %d between, %d where the plume was\n",
                        RibbonLow, RibbonHigh, BeamLeft, BeamMiddle, Between, PlumeWas);
            Expect(RibbonLow > 150 && RibbonHigh > 150, "the ribbon is drawn up its whole rise, facing the eye");
            Expect(BeamLeft > 150 && BeamMiddle > 150, "the beam is drawn across from its emitter to its end");
            Expect(Between < 60 && PlumeWas < 60, "and nothing else: the plume's system was swapped out");
        }
        ParticleSystemCache::Invalidate("Test/Strips.nmparticle");

        /* 4. A flipbook drawn: one big still sprite of the UV probe (red U, green V), whole and then as frame 0 of 2 x 2. */
        const auto RunFrames = [&](const int InCount)
        {
            for(int Each = 0; Each < InCount; ++Each)
            {
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
                Rend.WaitForPreviousFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        };
        const auto Capture = [&](EngineCore::Image& OutPixels)
        {
            const uint64_t Asked = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each)
            {
                Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
                Rend.WaitForPreviousFrame();
            }
            std::string Why;
            return Asked != 0 && Rend.TakeMaterialPreview(Asked, OutPixels, Why) && OutPixels.GetWidth() == 256;
        };
        const auto Channel = [](const EngineCore::Image& InPixels, const int InX, const int InY, const int InChannel)
        {
            const bool bBgra = InPixels.GetFormat() == EImageFormat::BGRA8;
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * InY + InX * BytesPerPixel(InPixels.GetFormat());
            const int Index = InChannel == 1 ? 1 : (bBgra ? 2 - InChannel : InChannel);
            return int(Px[Index]);
        };
        const auto Sheet = [&](const bool bInFlipbook)
        {
            auto Card = std::make_shared<ParticleSystemDesc>();
            ParticleEmitterDesc& One = Card->Emitters.emplace_back();
            std::snprintf(One.Material, sizeof(One.Material), "%s", "Content/Materials/UVProbe.material.nmgraph");
            One.Offset        = Vector3(0.0, 100.0, 0.0);   // the view's centre
            One.Modules       = bInFlipbook ? uint8_t(PEM_Flipbook) : uint8_t(0);
            One.SpawnMode     = PSM_Burst;
            One.BurstCount    = 1;
            One.MaxParticles  = 1;
            One.Lifetime      = 100.0f;
            One.LifetimeVariance = 0.0f;
            One.StartSize     = 240.0f;
            One.EndSize       = 240.0f;
            One.FadeIn        = 0.0f;
            One.FadeOut       = 0.0f;
            One.FlipbookColumns = 2;
            One.FlipbookRows    = 2;
            ParticleSystemCache::Store("Test/Sheet.nmparticle", Card);
            if(auto* Running = Plume.GetComponent<ParticleSystemComponent>())
            {
                std::snprintf(Running->System, sizeof(Running->System), "%s", "Test/Sheet.nmparticle");
                Running->bEmitting = 0;   // off and on: the burst starts over in the new system
            }
            RunFrames(2);
            if(auto* Running = Plume.GetComponent<ParticleSystemComponent>()) { Running->bEmitting = 1; }
            RunFrames(10);
            EngineCore::Image Pixels;
            /* Near the card's right and bottom edges (u and v about 0.92): 70 pixels is its half width. */
            int Right = -1, Bottom = -1, Left = -1;
            if(Capture(Pixels))
            {
                Right  = Channel(Pixels, 128 + 60, 128, 0);
                Bottom = Channel(Pixels, 128, 128 + 60, 1);
                Left   = Channel(Pixels, 128 - 60, 128, 0);
            }
            return std::array<int, 3>{ Right, Bottom, Left };
        };
        const std::array<int, 3> Whole = Sheet(false);
        const std::array<int, 3> Frame0 = Sheet(true);
        std::printf("    UV probe red at the right edge: %d whole, %d as frame 0 of 2 x 2; green at the bottom: %d, %d; red at the left: %d, %d\n",
                    Whole[0], Frame0[0], Whole[1], Frame0[1], Whole[2], Frame0[2]);
        Expect(Whole[0] > 200 && Whole[1] > 200 && Frame0[0] > 40 && Frame0[0] < Whole[0] - 40 && Frame0[1] > 40 && Frame0[1] < Whole[1] - 40
               && Frame0[2] < 40 && Whole[2] < 40,
               "a flipbook frame samples its cell: frame 0 of 2 x 2 reads only the texture's top-left quarter");
        ParticleSystemCache::Invalidate("Test/Sheet.nmparticle");

        /* 5. Culling: an emitter past its Cull Distance is not simulated; one no view draws pauses, and goes on when seen. */
        const auto Simulations = [&]()
        {
            RenderStats Stats;
            Rend.GetStats(Stats);
            return Stats.ParticleSimulations;
        };
        const auto Plumes = [&](const float InCullDistance, const bool bInPause)
        {
            auto Steady = std::make_shared<ParticleSystemDesc>();
            ParticleEmitterDesc& Puffs = Steady->Emitters.emplace_back();
            Puffs.CullDistance    = InCullDistance;
            Puffs.bPauseOffscreen = bInPause ? 1 : 0;
            ParticleSystemCache::Store("Test/Steady.nmparticle", Steady);
            if(auto* Running = Plume.GetComponent<ParticleSystemComponent>())
            {
                std::snprintf(Running->System, sizeof(Running->System), "%s", "Test/Steady.nmparticle");
            }
        };
        const auto Simulated = [&](const int InFrames)
        {
            const uint64_t Before = Simulations();
            RunFrames(InFrames);
            return Simulations() - Before;
        };
        Plumes(0.0f, true);
        RunFrames(5);
        const uint64_t Seen = Simulated(20);
        Plumes(300.0f, true);    // the camera is 600 away
        RunFrames(3);
        const uint64_t Far = Simulated(20);
        Plumes(0.0f, true);
        /* Turned round, with the frustum's box as its bounds, as a game camera's view has (WorldBridge's
         * PerspectiveBounds; MakePerspective's own are empty, which culls nothing): the emitter behind it is culled. */
        RenderView Away = RenderView::MakePerspective(Viewport, Vector3(0.0, 100.0, -600.0), Vector3(0.0, 0.0, -1.0),
                                                      40.0f, 10.0f, 5000.0f);
        Away.Bounds = GameCore::AABB(Vector3(-1900.0, -1800.0, -5600.0), Vector3(1900.0, 2000.0, -600.0));
        Rend.SetViewOverride(&Away);
        RunFrames(50);           // well past half a second unseen
        const uint64_t Unseen = Simulated(20);
        Plumes(0.0f, false);     // the same, never pausing
        const uint64_t Unpaused = Simulated(20);
        Plumes(0.0f, true);
        Rend.SetViewOverride(&Side);
        RunFrames(3);
        const uint64_t Again = Simulated(20);
        std::printf("    simulations in 20 frames: %llu seen, %llu past its cull distance, %llu unseen, %llu unseen and never pausing, %llu seen again\n",
                    static_cast<unsigned long long>(Seen), static_cast<unsigned long long>(Far), static_cast<unsigned long long>(Unseen),
                    static_cast<unsigned long long>(Unpaused), static_cast<unsigned long long>(Again));
        Expect(Seen >= 18 && Far == 0, "an emitter past its Cull Distance from the view is not simulated");
        Expect(Unseen == 0 && Unpaused >= 18 && Again >= 18,
               "one no view has drawn for half a second pauses (unless told not to), and goes on once seen");
        ParticleSystemCache::Invalidate("Test/Steady.nmparticle");
        Rend.UnbindWorld();
        Rend.Shutdown();
        ParticleSystemCache::Invalidate("Test/Plume.nmparticle");
    }
    SetFileProvider(PreviousProvider);
}
#endif // WITH_EDITOR

/*
 * PostProcessStack.h: a stack's text, volume weights, blending, and the cache
 * reading a graph's <gen_code>. Null backend.
 */
static void TestPostProcessStack()
{
    std::printf("[post process stack]\n");

    const char* Text =
        "# Post process stack Arch\n"
        "Effect DepthOfField FocusDistance=300 FocusRange=100 MaxBlur=12 NearBlur=false\n"
        "Effect Bloom Threshold=0.7 Intensity=2 Radius=0.5 Levels=4 Tint=1,0.5,0.25\n"
        "Effect Antialiasing EdgeThreshold=0.2 Method=FXAA Quality=High\n"
        "Material Stage=Before Order=1 Path=Content/Materials/Wave.material.nmgraph\n"
        "Effect Future Unknown=1\n";
    PostProcessSettings Stack;
    std::string Error;
    Expect(ParsePostProcessStack(Text, Stack, Error), "a stack's text parses (an unknown effect is ignored)");
    Expect(Stack.bDepthOfField && Stack.DofFocusDistance == 300.0f && Stack.DofMaxBlur == 12.0f && !Stack.bDofNearBlur,
           "Depth of Field's settings");
    Expect(Stack.bBloom && Stack.BloomIntensity == 2.0f && Stack.BloomLevels == 4.0f && Stack.BloomTint[1] == 0.5f && Stack.BloomTint[2] == 0.25f,
           "Bloom's settings, the tint a list");
    Expect(Stack.bAntialiasing && Stack.AaQuality == 2 && !Stack.bMotionBlur, "Antialiasing's; no Motion Blur node, no motion blur");
    Expect(Stack.MaterialCount == 1 && Stack.Materials[0].Stage == PPS_BeforeEffects && Stack.Materials[0].Order == 1.0f
           && std::strcmp(Stack.Materials[0].Path, "Content/Materials/Wave.material.nmgraph") == 0, "a material pass, its path the rest of the line");
    PostProcessSettings Occlusion;
    Expect(ParsePostProcessStack("Effect AmbientOcclusion Intensity=2 Radius=40 Samples=8 Power=1 Blur=false\n", Occlusion, Error)
           && Occlusion.bAmbientOcclusion && Occlusion.AoIntensity == 2.0f && Occlusion.AoRadius == 40.0f && Occlusion.AoSamples == 8.0f
           && Occlusion.AoPower == 1.0f && !Occlusion.bAoBlur && !Occlusion.IsEmpty(), "Ambient Occlusion's settings");
    PostProcessSettings OcclusionHalf;
    BlendPostProcess(OcclusionHalf, Occlusion, 0.25f);
    Expect(OcclusionHalf.bAmbientOcclusion && std::abs(OcclusionHalf.AoIntensity - 0.5f) < 1e-5f && OcclusionHalf.AoRadius == 40.0f
           && !OcclusionHalf.bAoBlur, "from off, its intensity scales with the weight; its other settings are the stack's");
    PostProcessSettings Broken;
    Expect(!ParsePostProcessStack("Effect Bloom Intensity=lots\n", Broken, Error) && Error.find("line 1") != std::string::npos,
           "a bad number names its line");

    Expect(GetPostProcessVolumeWeight(0.0f, 0.0f) == 1.0f && GetPostProcessVolumeWeight(0.0f, 100.0f) == 1.0f, "inside a volume: full weight");
    Expect(GetPostProcessVolumeWeight(50.0f, 100.0f) == 0.5f && GetPostProcessVolumeWeight(150.0f, 100.0f) == 0.0f,
           "outside: fading over the blend distance");
    Expect(GetPostProcessVolumeWeight(1.0f, 0.0f) == 0.0f, "no blend distance: a hard edge");

    /* Half way in from nothing: strengths half, settings the stack's, switches off. */
    PostProcessSettings Half;
    BlendPostProcess(Half, Stack, 0.4f);
    Expect(Half.bBloom && std::abs(Half.BloomIntensity - 0.8f) < 1e-5f && Half.BloomThreshold == 0.7f,
           "from off, Bloom's intensity scales with the weight; its threshold is the stack's");
    Expect(Half.bDepthOfField && std::abs(Half.DofMaxBlur - 4.8f) < 1e-5f, "and Depth of Field's blur");
    Expect(!Half.bAntialiasing && Half.MaterialCount == 0, "under half weight, no antialiasing and no material passes");

    /* A second, higher-priority volume over a full first: numbers blend toward it. */
    PostProcessSettings Base;
    BlendPostProcess(Base, Stack, 1.0f);
    PostProcessSettings Over;
    ParsePostProcessStack("Effect Bloom Threshold=0.9 Intensity=0 Radius=0.5 Levels=4 Tint=1,1,1\nEffect Antialiasing Quality=Low\n", Over, Error);
    BlendPostProcess(Base, Over, 0.75f);
    Expect(std::abs(Base.BloomIntensity - 0.5f) < 1e-5f && std::abs(Base.BloomThreshold - 0.85f) < 1e-5f,
           "an overlapping volume blends each number by its weight");
    Expect(Base.AaQuality == 0 && Base.bDepthOfField && Base.DofMaxBlur == 12.0f && Base.MaterialCount == 1,
           "switches go to it past half weight; what it leaves out stays");

    /* The cache: a graph file's <gen_code>, and a bare material path. */
    FilesystemProvider  Disk;
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Disk);
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimblePostStack";
    std::error_code Ignored;
    std::filesystem::create_directories(Root, Ignored);
    const std::string File = (Root / "Test.pp.nmgraph").string();
    {
        std::ofstream Out(File);
        Out << "<?xml version=\"1.0\"?>\n<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <gen_code>\n"
               "    <Output Extension=\".ppstack\"><![CDATA[Effect Bloom Intensity=3\n]]></Output>\n  </gen_code>\n</NodeGraph>\n";
    }
    PostProcessStackCache::Invalidate(File);
    const std::shared_ptr<const PostProcessSettings> Cached = PostProcessStackCache::Find(File);
    Expect(Cached && Cached->bBloom && Cached->BloomIntensity == 3.0f, "a stack graph's compiled stack is read from its <gen_code>");
    const std::shared_ptr<const PostProcessSettings> Single = PostProcessStackCache::Find("Content/Materials/Negative.material.nmgraph");
    Expect(Single && Single->MaterialCount == 1 && Single->Materials[0].Stage == PPS_AfterEffects && !Single->bBloom,
           "a material graph path is a stack of that one material, after the effects");
    Expect(PostProcessStackCache::Find("Nope/Missing.pp.nmgraph") == nullptr, "a missing stack is null");
    PostProcessStackCache::Invalidate(std::string());
    SetFileProvider(PreviousProvider);
    std::filesystem::remove_all(Root, Ignored);
}

/*
 * ShadowSetup.h: where each shadow map looks from, and its atlas tile. Pure
 * math -- no GPU.
 */
static void TestShadowSetup()
{
    std::printf("[shadow setup]\n");
    using EngineCore::Vector3;
    using EngineCore::Vector4;

    float Splits[RENDER_CASCADE_MAX] = {};
    ComputeCascadeSplits(10.0f, 4000.0f, 4, 0.75f, Splits);
    Expect(Splits[0] > 10.0f && Splits[0] < Splits[1] && Splits[1] < Splits[2] && Splits[2] < Splits[3] && Splits[3] == 4000.0f,
           "cascade splits rise and end at the far edge");
    float Uniform[RENDER_CASCADE_MAX] = {};
    ComputeCascadeSplits(0.0f, 400.0f, 4, 0.0f, Uniform);
    Expect(std::abs(Uniform[0] - 100.75f) < 0.5f && std::abs(Uniform[1] - 200.5f) < 0.5f, "lambda 0 splits uniformly");

    const RenderView Camera = RenderView::MakePerspective(EngineCore::Size(1280, 720), Vector3(0.0, -300.0, -800.0),
                                                          Vector3(0.0, 0.3, 1.0), 60.0f, 1.0f, 20000.0f);
    RenderLighting Lighting;
    Lighting.LightDirection       = Vector3(0.3, 1.0, 0.2);
    Lighting.bLightCastShadows    = 1;
    Lighting.LightShadowTechnique = LST_Cascaded;
    Lighting.LightCascadeCount    = 4;
    Lighting.LightShadowDistance  = 3000.0f;

    ShadowFrame Frame;
    BuildShadowFrame(Lighting, Camera, Frame);
    Expect(Frame.ViewCount == 0 && Frame.AtlasSize == 0, "no shadow views while [Shadows] is disabled (a hand-built RenderLighting's default)");

    Lighting.Shadows.bEnabled   = 1;
    Lighting.Shadows.Resolution = 2048;

    /* A spot and a point light that cast, and one point light that does not. */
    RenderLocalLight Spot;
    Spot.Type = RLL_Spot; Spot.Position = Vector3(0.0, -400.0, 0.0); Spot.Direction = Vector3(0.0, 1.0, 0.0);
    Spot.Range = 800.0f; Spot.OuterConeAngle = 30.0f; Spot.bCastShadows = 1;
    RenderLocalLight Point;
    Point.Type = RLL_Point; Point.Position = Vector3(300.0, -200.0, 100.0); Point.Range = 500.0f; Point.bCastShadows = 1;
    RenderLocalLight Plain = Point;
    Plain.bCastShadows = 0;
    Lighting.Locals[0] = Spot; Lighting.Locals[1] = Point; Lighting.Locals[2] = Plain;
    Lighting.LocalCount = 3;

    BuildShadowFrame(Lighting, Camera, Frame);
    Expect(Frame.AtlasSize == 2048 && Frame.DirectionalFirstView == 0 && Frame.CascadeCount == 4,
           "a cascaded sun takes the first four views");
    Expect(Frame.LocalFirstView[0] == 4 && Frame.LocalFirstView[1] == 5 && Frame.LocalFirstView[2] == -1 && Frame.ViewCount == 11,
           "the spot takes one view, the point light six, the plain light none");

    bool bCascadeTiles = true;
    for(uint32_t Index = 0; Index < 4; ++Index)
    {
        const ShadowView& View = Frame.Views[Index];
        bCascadeTiles &= View.Atlas == SA_Directional && View.TileSize == 1024 && View.TileX == (Index % 2) * 1024 && View.TileY == (Index / 2) * 1024;
    }
    Expect(bCascadeTiles, "the cascades fill the directional atlas's quarters");

    /* Local tiles: inside the atlas, and no two overlapping. */
    bool bInside = true, bApart = true;
    for(uint32_t A = 4; A < Frame.ViewCount; ++A)
    {
        const ShadowView& View = Frame.Views[A];
        bInside &= View.Atlas == SA_Local && View.TileX + View.TileSize <= 2048 && View.TileY + View.TileSize <= 2048;
        for(uint32_t B = A + 1; B < Frame.ViewCount; ++B)
        {
            const ShadowView& Other = Frame.Views[B];
            bApart &= View.TileX + View.TileSize <= Other.TileX || Other.TileX + Other.TileSize <= View.TileX
                   || View.TileY + View.TileSize <= Other.TileY || Other.TileY + Other.TileSize <= View.TileY;
        }
    }
    Expect(bInside && bApart, "local tiles fit the atlas and do not overlap");

    /* A point in front of each cascade's slice lands inside its own tile, at a depth in [0, 1]. */
    const auto ToAtlas = [](const ShadowView& InView, const Vector3& InWorld, double& OutU, double& OutV, double& OutDepth)
    {
        const Vector4 Clip = InView.WorldToAtlas * Vector4(InWorld.GetX(), InWorld.GetY(), InWorld.GetZ(), 1.0);
        OutU = Clip.GetX() / Clip.GetW(); OutV = Clip.GetY() / Clip.GetW(); OutDepth = Clip.GetZ() / Clip.GetW();
    };
    bool bInTile = true;
    double SliceNear = 1.0;
    for(uint32_t Index = 0; Index < 4; ++Index)
    {
        const double Mid = (SliceNear + Frame.CascadeSplits[Index]) * 0.5;
        SliceNear = Frame.CascadeSplits[Index];
        const Vector3 Forward = Vector3(0.0, 0.3, 1.0).GetNormalize();
        const Vector3 Point = Camera.Origin + Forward * Mid;
        double U, V, Depth;
        ToAtlas(Frame.Views[Index], Point, U, V, Depth);
        const ShadowView& View = Frame.Views[Index];
        bInTile &= U * 2048.0 >= View.TileX && U * 2048.0 <= View.TileX + View.TileSize
                && V * 2048.0 >= View.TileY && V * 2048.0 <= View.TileY + View.TileSize && Depth > 0.0 && Depth < 1.0;
    }
    Expect(bInTile, "each cascade maps a point of its slice into its own tile, depth inside [0, 1]");

    /* The spot's view: straight below the light is its tile's centre. */
    double U, V, Depth;
    ToAtlas(Frame.Views[4], Vector3(0.0, 0.0, 0.0), U, V, Depth);
    const ShadowView& SpotView = Frame.Views[4];
    Expect(std::abs(U * 2048.0 - (SpotView.TileX + SpotView.TileSize * 0.5)) < 1.0
           && std::abs(V * 2048.0 - (SpotView.TileY + SpotView.TileSize * 0.5)) < 1.0 && Depth > 0.0 && Depth < 1.0,
           "a spot light's axis lands at its tile's centre");

    /* The point light's faces: each axis direction lands in its own face. */
    bool bFaces = true;
    const Vector3 Axes[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
    for(int Face = 0; Face < 6; ++Face)
    {
        const ShadowView& View = Frame.Views[5 + Face];
        ToAtlas(View, Point.Position + Axes[Face] * 200.0, U, V, Depth);
        bFaces &= std::abs(U * 2048.0 - (View.TileX + View.TileSize * 0.5)) < 1.0
               && std::abs(V * 2048.0 - (View.TileY + View.TileSize * 0.5)) < 1.0;
    }
    Expect(bFaces, "a point light's six faces look along +X -X +Y -Y +Z -Z, in that order");

    /* Caster bounds: the spot's reaches the floor below it. */
    Expect(SpotView.WorldBounds.Contains(Vector3(0.0, 300.0, 0.0)) && !SpotView.WorldBounds.Contains(Vector3(0.0, -1000.0, 0.0)),
           "a spot view's bounds hold what it lights, not what is behind it");

    /* MaxShadowedLights 1: only the first casting light gets views. */
    Lighting.Shadows.MaxShadowedLights = 1;
    BuildShadowFrame(Lighting, Camera, Frame);
    Expect(Frame.LocalFirstView[0] == 4 && Frame.LocalFirstView[1] == -1 && Frame.ViewCount == 5, "MaxShadowedLights caps the casting lights");

    /* A plain (Standard) sun takes the whole atlas. */
    Lighting.LightShadowTechnique = LST_Standard;
    BuildShadowFrame(Lighting, Camera, Frame);
    Expect(Frame.CascadeCount == 1 && Frame.Views[0].TileSize == 2048, "a Standard sun is one view over the whole atlas");

    /* Inset shadows: the local atlas's spare tiles, after the lights'. */
    {
        RenderLighting Insets = Lighting;
        Insets.Shadows.MaxShadowedLights = 4;
        Insets.bInsetLight         = 1;
        Insets.InsetLightDirection = Vector3(0.3, -1.0, 0.2);
        InsetShadowCaster Casters[5];
        for(int Index = 0; Index < 5; ++Index)
        {
            Casters[Index].Centre = Vector3(100.0 * Index, 50.0, 200.0);
            Casters[Index].Radius = 40.0f + 10.0f * Index;
        }
        Casters[1].Radius = 0.0f;   // nothing to fit: skipped

        BuildShadowFrame(Insets, Camera, Frame);
        Expect(Frame.InsetCount == 0 && Frame.InsetFirstView == -1, "no casters offered, no inset views");

        BuildShadowFrame(Insets, Camera, Frame, Casters, 5);
        const int32_t AfterLights = Frame.LocalFirstView[1] + int32_t(SHADOW_POINT_FACES);   // the spot's one, the point light's six
        Expect(Frame.InsetFirstView == AfterLights && Frame.InsetCount == 4 && Frame.ViewCount == uint32_t(AfterLights) + 4,
               "insets follow the lights' views, up to four, an empty caster skipped");
        bool bOwners = true, bContain = true, bTiles = true, bSnapped = true;
        const int32_t Expected[4] = { 0, 2, 3, 4 };
        for(uint32_t Index = 0; Index < Frame.InsetCount; ++Index)
        {
            const ShadowView& View = Frame.Views[uint32_t(Frame.InsetFirstView) + Index];
            const InsetShadowCaster& Caster = Casters[View.Inset];
            bOwners &= View.Inset == Expected[Index] && !Frame.IsSunView(uint32_t(Frame.InsetFirstView) + Index);
            bTiles  &= View.Atlas == SA_Local && View.TileSize == 2048 / 6;
            /* The sphere's centre near the tile's middle, its edge just inside. */
            double U, V, Depth;
            ToAtlas(View, Caster.Centre, U, V, Depth);
            bSnapped &= std::abs(U * 2048.0 - (View.TileX + View.TileSize * 0.5)) < 2.0
                     && std::abs(V * 2048.0 - (View.TileY + View.TileSize * 0.5)) < 2.0 && std::abs(Depth - 0.5) < 0.01;
            const Vector3 Across = (Vector3(1.0, 0.0, 0.0) ^ Vector3(0.3, -1.0, 0.2)).GetNormalize();
            ToAtlas(View, Caster.Centre + Across * (double(Caster.Radius) * 0.97), U, V, Depth);
            bContain &= U * 2048.0 > View.TileX && U * 2048.0 < View.TileX + View.TileSize
                     && V * 2048.0 > View.TileY && V * 2048.0 < View.TileY + View.TileSize;
            /* Its depth spans the sphere: the far side lands just short of 1. */
            ToAtlas(View, Caster.Centre + Vector3(0.3, -1.0, 0.2).GetNormalize() * (double(Caster.Radius) * 0.97), U, V, Depth);
            bContain &= Depth > 0.95 && Depth < 1.0;
        }
        Expect(bOwners, "each inset view names its caster, in the order offered");
        Expect(bTiles, "each takes one local tile");
        Expect(bSnapped && bContain, "and holds its caster's sphere, centred, its depth across the sphere alone");

        bool bApartInsets = true;
        for(uint32_t A = Frame.CascadeCount; A < Frame.ViewCount; ++A)
        {
            for(uint32_t B = A + 1; B < Frame.ViewCount; ++B)
            {
                const ShadowView& One = Frame.Views[A];
                const ShadowView& Two = Frame.Views[B];
                bApartInsets &= One.TileX + One.TileSize <= Two.TileX || Two.TileX + Two.TileSize <= One.TileX
                             || One.TileY + One.TileSize <= Two.TileY || Two.TileY + Two.TileSize <= One.TileY;
            }
        }
        Expect(bApartInsets, "inset tiles never overlap a light's");

        /* Snapped: moving the caster less than a texel does not move its
         * map across the world (only along the light, which is depth). */
        InsetShadowCaster Nudged[1] = { Casters[0] };
        BuildShadowFrame(Insets, Camera, Frame, Casters, 1);
        const ShadowView Placed = Frame.Views[Frame.InsetFirstView];
        Nudged[0].Centre = Casters[0].Centre + Vector3(0.001, 0.0, 0.001);
        BuildShadowFrame(Insets, Camera, Frame, Nudged, 1);
        double PlacedU, PlacedV, NudgedU, NudgedV;
        ToAtlas(Placed, Casters[0].Centre, PlacedU, PlacedV, Depth);
        ToAtlas(Frame.Views[Frame.InsetFirstView], Casters[0].Centre, NudgedU, NudgedV, Depth);
        Expect(std::abs(PlacedU - NudgedU) * 2048.0 < 1e-3 && std::abs(PlacedV - NudgedV) * 2048.0 < 1e-3,
               "an inset view moves in whole texels");

        /* Each inset view's screen rectangle holds its caster's centre and is
         * less than the whole screen -- its prism, swept down and under the
         * camera, is clipped to the view. One wholly behind the eye is empty;
         * one round the eye is the whole screen. */
        BuildShadowFrame(Insets, Camera, Frame, Casters, 5);
        bool bRects = true;
        for(uint32_t Index = 0; Index < Frame.InsetCount; ++Index)
        {
            const ShadowView& View = Frame.Views[uint32_t(Frame.InsetFirstView) + Index];
            const Vector3& Centre = Casters[View.Inset].Centre;
            const Vector4 Clip = Camera.Projection * Camera.View * Vector4(Centre.GetX(), Centre.GetY(), Centre.GetZ(), 1.0);
            const double X = Clip.GetX() / Clip.GetW(), Y = Clip.GetY() / Clip.GetW();
            bRects &= X >= View.ScreenNdc[0] && X <= View.ScreenNdc[2] && Y >= View.ScreenNdc[1] && Y <= View.ScreenNdc[3]
                   && (View.ScreenNdc[2] - View.ScreenNdc[0]) * (View.ScreenNdc[3] - View.ScreenNdc[1]) < 4.0f;
        }
        Expect(bRects, "each inset view's screen rectangle holds its caster, and is less than the screen");
        /* The receivers cut the sweep: a floor under the casters stops their
         * prisms there, so each covers less screen -- still holding where its
         * shadow lands on that floor. */
        const GameCore::AABB Floor(Vector3(-3000.0, -20.0, -3000.0), Vector3(3000.0, 0.0, 3000.0));
        bool bFloored = true;
        for(uint32_t Index = 0; Index < Frame.InsetCount; ++Index)
        {
            const ShadowView& View = Frame.Views[uint32_t(Frame.InsetFirstView) + Index];
            InsetShadowCaster Raised = Casters[View.Inset];
            Raised.Centre = Raised.Centre + Vector3(0.0, 300.0, 0.0);   // above the floor
            float Open[4] = {}, Cut[4] = {};
            InsetScreenNdc(Raised, Vector3(0.3, -1.0, 0.2).GetNormalize(), 2.0, Camera, Open);
            InsetScreenNdc(Raised, Vector3(0.3, -1.0, 0.2).GetNormalize(), 2.0, Camera, Cut, &Floor);
            const Vector3 Down = Vector3(0.3, -1.0, 0.2).GetNormalize();
            const Vector3 Lands = Raised.Centre + Down * ((Raised.Centre.GetY() + 10.0) / -Down.GetY());
            const Vector4 Clip = Camera.Projection * Camera.View * Vector4(Lands.GetX(), Lands.GetY(), Lands.GetZ(), 1.0);
            const double X = Clip.GetX() / Clip.GetW(), Y = Clip.GetY() / Clip.GetW();
            bFloored &= (Cut[2] - Cut[0]) * (Cut[3] - Cut[1]) < (Open[2] - Open[0]) * (Open[3] - Open[1])
                     && X >= Cut[0] && X <= Cut[2] && Y >= Cut[1] && Y <= Cut[3];
        }
        Expect(bFloored, "the receivers' bounds stop a prism at the floor: less screen, its shadow on the floor still in it");
        float Missed[4] = {};
        const GameCore::AABB Elsewhere(Vector3(5000.0, 0.0, 5000.0), Vector3(6000.0, 10.0, 6000.0));
        InsetScreenNdc(Casters[0], Vector3(0.3, -1.0, 0.2).GetNormalize(), 2.0, Camera, Missed, &Elsewhere);
        Expect(Missed[0] > Missed[2], "a prism that meets no receiver shadows no pixel");

        float None[4] = {}, Whole[4] = {};
        InsetShadowCaster Behind;
        Behind.Centre = Camera.Origin + Vector3(0.0, 0.0, -50.0);   // the camera looks along +Z: this is behind it
        Behind.Radius = 20.0f;
        InsetScreenNdc(Behind, Vector3(0.0, 0.0, -1.0), 1.0, Camera, None);
        Expect(None[0] > None[2] && None[1] > None[3], "a prism wholly behind the eye shadows no pixel");
        InsetShadowCaster Around;
        Around.Centre = Camera.Origin;
        Around.Radius = 20.0f;
        InsetScreenNdc(Around, Vector3(0.3, -1.0, 0.2).GetNormalize(), 1.0, Camera, Whole);
        Expect(Whole[0] == -1.0f && Whole[1] == -1.0f && Whole[2] == 1.0f && Whole[3] == 1.0f,
               "a prism round the eye shadows the whole screen");

        Insets.Shadows.MaxInsetShadows = 1;
        BuildShadowFrame(Insets, Camera, Frame, Casters, 5);
        Expect(Frame.InsetCount == 1, "MaxInsetShadows caps them");
        Insets.Shadows.MaxInsetShadows = 4;

        /* Four point lights fill all 24 tiles: none left for insets. */
        RenderLighting Full = Insets;
        for(uint32_t Index = 0; Index < 4; ++Index)
        {
            Full.Locals[Index] = Point;
            Full.Locals[Index].Position = Vector3(300.0 * Index, -200.0, 100.0);
        }
        Full.LocalCount = 4;
        BuildShadowFrame(Full, Camera, Frame, Casters, 5);
        Expect(Frame.InsetCount == 0 && Frame.ViewCount == Frame.CascadeCount + 24, "insets get only the tiles the lights leave");

        Insets.bInsetLight = 0;
        BuildShadowFrame(Insets, Camera, Frame, Casters, 5);
        Expect(Frame.InsetCount == 0, "no inset light, no inset views");
    }

    /* An orthographic camera (the 2D view) still gets a sun view. */
    const RenderView Flat = RenderView::MakeOrthographic(EngineCore::Size(1280, 720), Vector3(0.0, 0.0, 0.0), -1.0f, 1000.0f);
    BuildShadowFrame(Lighting, Flat, Frame);
    Expect(Frame.DirectionalFirstView == 0 && Frame.Views[0].WorldBounds.Contains(Vector3(640.0, 360.0, 0.0)),
           "an orthographic camera's view is covered too");
}

/*
 * WorldBridge's light gathering and the perspective camera, through a
 * Renderer on the Null backend: nothing to look at, but what reaches the
 * render thread is checked by a proxy that records the scene's view.
 */
static void TestLightComponents()
{
    std::printf("[light components]\n");
    using EngineCore::Vector3;

    Expect(std::abs(LightForward(Vector3(0.0, 0.0, 0.0)).GetZ() - 1.0) < 1e-9, "a light with no rotation shines along +Z");
    const Vector3 Down = LightForward(Vector3(90.0, 0.0, 0.0));
    Expect(std::abs(Down.GetY() + 1.0) < 1e-6, "pitch 90 shines down (-Y; +Y is up)");
    const Vector3 Right = LightForward(Vector3(0.0, 90.0, 0.0));
    Expect(std::abs(Right.GetX() - 1.0) < 1e-6, "yaw 90 shines along +X, as the editor camera turns");

    size_t Count = 0;
    Expect(GameCore::FindComponentScript(PointLightComponent::StaticType().ID, Count) && Count == 10,
           "Point Light exposes its light and shadow fields to Lua");
    Expect(GameCore::FindComponentScript(SpotLightComponent::StaticType().ID, Count) && Count == 12,
           "Spot Light exposes its light and shadow fields to Lua");
}

#if WITH_EDITOR   /* main runs it in editor builds only: it uses the editor's Renderer API */
/*
 * The stages (RENDERPIPELINE.md), on the Null backend: each material lands in
 * the stage its Domain and Shading name; a mesh's sections take their slots,
 * falling back to their own material; culling pads by the slots' reach.
 */
static void TestRenderStages()
{
    std::printf("[render stages]\n");

    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreStages";
    std::error_code Ignored;
    std::filesystem::remove_all(Root, Ignored);
    std::filesystem::create_directories(Root / "Materials", Ignored);

    const auto WriteMaterial = [&](const char* InName, const char* InDomain, const char* InShading, const char* InExtra)
    {
        std::ofstream(Root / "Materials" / (std::string(InName) + ".material"))
            << "NimbleMaterial 3\nDomain " << InDomain << "\nShading " << InShading << "\nBlockSize 160\n" << InExtra;
    };
    WriteMaterial("LitOpaque",   "Opaque",      "Lit",   "");
    WriteMaterial("UnlitMasked", "Masked",      "Unlit", "");
    WriteMaterial("TransLit",    "Translucent", "Lit",   "");
    WriteMaterial("TransUnlit",  "Translucent", "Unlit", "");
    WriteMaterial("Glow",        "Translucent", "Unlit", "");    // the smoke mesh's Front section names it
    WriteMaterial("Post",        "PostProcess", "Unlit", "");
    WriteMaterial("Reach",       "Opaque",      "Lit",   "MaxOffset 5\nHasOffset 1\n");
    std::filesystem::create_directories(Root / "Props", Ignored);   // a graph in another folder: Props/Crate.material.nmgraph
    std::ofstream(Root / "Props" / "Crate.material") << "NimbleMaterial 3\nDomain Opaque\nShading Lit\nBlockSize 160\n";

    FilesystemProvider Provider(Root.generic_string());
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Provider);
    MeshAssetCache::Invalidate(std::string());
    MeshData Square = MakeSmokeMesh();   // Front (material Glow), Back (none)
    SaveMeshFile((Root / "Content" / "Square.nmmesh").generic_string(), Square);

    Expect(GetMaterialRenderStage(EMaterialDomain::Opaque, EMaterialShading::Lit) == ERenderStage::BasePass
           && GetMaterialRenderStage(EMaterialDomain::Masked, EMaterialShading::Unlit) == ERenderStage::Unlit
           && GetMaterialRenderStage(EMaterialDomain::Translucent, EMaterialShading::Lit) == ERenderStage::Translucent
           && GetMaterialRenderStage(EMaterialDomain::Translucent, EMaterialShading::Unlit) == ERenderStage::Translucent
           && GetMaterialRenderStage(EMaterialDomain::PostProcess, EMaterialShading::Lit) == ERenderStage::PostProcess,
           "a material's Domain and Shading name its stage");

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds for the stage test");
    if(bInit)
    {
        const EngineCore::Size Viewport(640, 360);
        const RenderView View = RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
        Rend.SetViewOverride(&View);

        GameCore::World World;
        const auto AddPrimitive = [&](const char* InName, const EngineCore::Vector3& InAt, const char* InMesh,
                                      std::initializer_list<const char*> InSlots)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent* Transform = Each.AddComponent<GameCore::TransformComponent>();
            Transform->Position = InAt;
            if(InMesh[0] == '\0') { Transform->Scale = EngineCore::Vector3(10.0, 10.0, 1.0); }   // a quad is 1 x 1
            RenderComponent Render;
            std::strcpy(Render.Mesh, InMesh);
            size_t Slot = 0;
            for(const char* Name : InSlots) { std::strcpy(Render.Materials[Slot++], Name); }
            Each.AddComponent(Render);
            return Each;
        };
        const EngineCore::Vector3 Inside(100.0, 100.0, 0.0);
        AddPrimitive("Lit",        Inside, "", { "Materials/LitOpaque.material.nmgraph" });   // Base Pass
        AddPrimitive("Crate",      Inside, "", { "Props/Crate.material.nmgraph" });           // Base Pass, from another folder
        AddPrimitive("Masked",     Inside, "", { "UnlitMasked" });             // Unlit (a bare name, as older worlds)
        AddPrimitive("Glass",      Inside, "", { "Materials/TransLit.material.nmgraph" });    // Translucent Lit
        AddPrimitive("Smoke",      Inside, "", { "TransUnlit" });              // Translucent Unlit
        AddPrimitive("Plain",      Inside, "", {});                            // Unlit
        AddPrimitive("Slotted",    Inside, "Content/Square.nmmesh", { "TransLit", "" });   // Translucent Lit; Back: Unlit
        AddPrimitive("Sections",   Inside, "Content/Square.nmmesh", { "", "" });           // Glow: Translucent Unlit; Back: Unlit
        AddPrimitive("BadSlot",    Inside, "Content/Square.nmmesh", { "Nope", "" });       // falls back to Glow; Back: Unlit
        PostProcessComponent Post;
        std::strcpy(Post.Stack, "Materials/Post.material.nmgraph");   // a material path: a one-material stack
        World.CreateEntity("Post").AddComponent(Post);
        Rend.BindWorld(World);

        uint64_t Frame = 500;
        const auto DrawFrame = [&]
        {
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
        };
        const auto ReadStages = [](uint32_t (&OutCounts)[size_t(ERenderStage::Count)])
        {
            ENQUEUE_RENDER_COMMAND(ReadStageCounts)([&OutCounts](RenderContext& InContext)
            {
                for(size_t Index = 0; Index < size_t(ERenderStage::Count); ++Index)
                {
                    OutCounts[Index] = InContext.GetStageDrawCount(ERenderStage(Index));
                }
            });
            FlushRenderCommands();
        };

        DrawFrame();   // extracts; the proxies draw from the next frame
        DrawFrame();
        uint32_t Counts[size_t(ERenderStage::Count)] = {};
        ReadStages(Counts);
        const auto At = [&Counts](const ERenderStage InStage) { return Counts[size_t(InStage)]; };
        Expect(At(ERenderStage::BasePass) == 2, "an Opaque Lit material draws in the Base Pass, by its graph's path from any folder");
        Expect(At(ERenderStage::Unlit) == 5, "Masked Unlit, a plain quad and three meshes' material-less sections draw in Unlit");
        Expect(At(ERenderStage::Translucent) == 5,
               "Translucent takes Lit and Unlit together: two quads, the section whose slot names one, and the sections "
               "falling back to their own material (an empty slot, a missing one)");
        Expect(At(ERenderStage::PostProcess) == 1, "a Post Process component's material is a PostProcess draw");
        Expect(At(ERenderStage::PrePass) == 0 && At(ERenderStage::Shadows) == 0, "PrePass and Shadows draw nothing yet");

        /* Culling: a mesh 3 units past the view's right edge, visible only
         * while one of its slots reaches 5 (World Position Offset). */
        Rend.UnbindWorld();
        GameCore::World Edge;
        const GameCore::Entity Reaching = Edge.CreateEntity("Reaching");
        Reaching.AddComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(643.0, 100.0, 0.0);
        RenderComponent Render;
        std::strcpy(Render.Mesh, "Content/Square.nmmesh");
        std::strcpy(Render.Materials[1], "Reach");
        Reaching.AddComponent(Render);
        Rend.BindWorld(Edge);
        DrawFrame();
        DrawFrame();
        RenderStats Stats;
        Rend.GetStats(Stats);
        Expect(Stats.ProxiesCulledLastFrame == 0, "a section's slot with World Position Offset pads the bounds past the edge");

        Reaching.GetComponent<RenderComponent>()->Materials[1][0] = '\0';
        DrawFrame();
        DrawFrame();
        Rend.GetStats(Stats);
        Expect(Stats.ProxiesCulledLastFrame == 1, "without it the same mesh is culled");

        Rend.UnbindWorld();
        Rend.SetViewOverride(nullptr);
    }
    Rend.Shutdown();

    MeshAssetCache::Invalidate(std::string());
    SetFileProvider(PreviousProvider);
    std::filesystem::remove_all(Root, Ignored);
}
#endif // WITH_EDITOR

/*
 * The engine's primitive meshes (Engine/Scripts/generate_primitive_meshes.py),
 * as staged beside this executable: they load, and each triangle faces the way
 * its normals do (the cross product of its edges points along them), as
 * imported meshes.
 */
static void TestEngineMeshes()
{
    std::printf("[engine meshes]\n");

    struct Expected { const char* Path; uint32_t Vertices; uint32_t Triangles; float MinY; float MaxY; };
    const Expected Meshes[] = {
        { "Engine/Content/Meshes/Box.nmmesh",    24,  12,  -0.5f, 0.5f },
        { "Engine/Content/Meshes/Sphere.nmmesh", 561, 960, -0.5f, 0.5f },
        { "Engine/Content/Meshes/Plane.nmmesh",  289, 512,  0.0f, 0.0f },
        { "Engine/Content/Meshes/Cylinder.nmmesh", 132, 128, -0.5f, 0.5f },
        { "Engine/Content/Meshes/Capsule.nmmesh",  594, 1024, -1.0f, 1.0f },
    };
    for(const Expected& Each : Meshes)
    {
        MeshData Mesh;
        const bool bLoaded = LoadMeshFile(Each.Path, Mesh);
        Expect(bLoaded, (std::string(Each.Path) + " loads").c_str());
        if(!bLoaded) { continue; }

        Expect(Mesh.GetVertexCount() == Each.Vertices && Mesh.GetTriangleCount() == Each.Triangles,
               (std::string(Each.Path) + " has its vertices and triangles").c_str());
        Expect(Mesh.BoundsMin[0] == -0.5f && Mesh.BoundsMax[0] == 0.5f && Mesh.BoundsMin[2] == -0.5f && Mesh.BoundsMax[2] == 0.5f
               && std::fabs(Mesh.BoundsMin[1] - Each.MinY) < 1e-5f && std::fabs(Mesh.BoundsMax[1] - Each.MaxY) < 1e-5f,
               (std::string(Each.Path) + " is one unit across, centred on the origin").c_str());
        Expect(Mesh.Sections.size() == 1 && Mesh.Sections[0].Material.empty() && Mesh.Sections[0].IndexCount == Mesh.Indices.size(),
               (std::string(Each.Path) + " is one section with no material (DefaultLit)").c_str());

        bool bUnitNormals = true, bUVsInRange = true, bFacing = true;
        for(uint32_t Vertex = 0; Vertex < Mesh.GetVertexCount(); ++Vertex)
        {
            const float* N = &Mesh.Normals[Vertex * 3];
            bUnitNormals &= std::fabs(N[0] * N[0] + N[1] * N[1] + N[2] * N[2] - 1.0f) < 1e-4f;
            const float* UV = &Mesh.UV0[Vertex * 2];
            bUVsInRange &= UV[0] >= 0.0f && UV[0] <= 1.0f && UV[1] >= 0.0f && UV[1] <= 1.0f;
        }
        for(size_t At = 0; At + 2 < Mesh.Indices.size(); At += 3)
        {
            const float* A = &Mesh.Positions[Mesh.Indices[At] * 3];
            const float* B = &Mesh.Positions[Mesh.Indices[At + 1] * 3];
            const float* C = &Mesh.Positions[Mesh.Indices[At + 2] * 3];
            const float AB[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
            const float AC[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
            const float Face[3] = { AB[1] * AC[2] - AB[2] * AC[1], AB[2] * AC[0] - AB[0] * AC[2], AB[0] * AC[1] - AB[1] * AC[0] };
            float Normal[3] = {};
            for(int Corner = 0; Corner < 3; ++Corner)
            {
                const float* N = &Mesh.Normals[Mesh.Indices[At + size_t(Corner)] * 3];
                for(int Axis = 0; Axis < 3; ++Axis) { Normal[Axis] += N[Axis]; }
            }
            const float Area = Face[0] * Face[0] + Face[1] * Face[1] + Face[2] * Face[2];
            bFacing &= Area < 1e-12f || Face[0] * Normal[0] + Face[1] * Normal[1] + Face[2] * Normal[2] > 0.0f;
        }
        Expect(bUnitNormals && bUVsInRange, (std::string(Each.Path) + " has unit normals and 0-1 UVs").c_str());

        bool bUV1InRange = Mesh.UV1.size() == size_t(Mesh.GetVertexCount()) * 2;
        for(size_t At = 0; bUV1InRange && At < Mesh.UV1.size(); ++At) { bUV1InRange &= Mesh.UV1[At] >= 0.0f && Mesh.UV1[At] <= 1.0f; }
        Expect(bUV1InRange, (std::string(Each.Path) + " has lightmap UVs (UV1) in 0-1").c_str());
        Expect(bFacing, (std::string(Each.Path) + " winds every triangle to face its normals").c_str());

        /* MeshAsset.h's frame: a unit tangent at right angles to the normal, and
         * on every triangle with UV area, along its +U with w * cross(N, T) along +V. */
        bool bTangentFrames = Mesh.Tangents.size() == size_t(Mesh.GetVertexCount()) * 4;
        for(size_t Vertex = 0; bTangentFrames && Vertex < Mesh.GetVertexCount(); ++Vertex)
        {
            const float* T = &Mesh.Tangents[Vertex * 4];
            const float* N = &Mesh.Normals[Vertex * 3];
            bTangentFrames &= std::fabs(T[0] * T[0] + T[1] * T[1] + T[2] * T[2] - 1.0f) < 1e-3f
                              && std::fabs(T[0] * N[0] + T[1] * N[1] + T[2] * N[2]) < 1e-3f && std::fabs(T[3]) == 1.0f;
        }
        for(size_t At = 0; bTangentFrames && At + 2 < Mesh.Indices.size(); At += 3)
        {
            const uint32_t I[3] = { Mesh.Indices[At], Mesh.Indices[At + 1], Mesh.Indices[At + 2] };
            float E1[3], E2[3];
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                E1[Axis] = Mesh.Positions[I[1] * 3 + Axis] - Mesh.Positions[I[0] * 3 + Axis];
                E2[Axis] = Mesh.Positions[I[2] * 3 + Axis] - Mesh.Positions[I[0] * 3 + Axis];
            }
            const float Du1 = Mesh.UV0[I[1] * 2] - Mesh.UV0[I[0] * 2], Dv1 = Mesh.UV0[I[1] * 2 + 1] - Mesh.UV0[I[0] * 2 + 1];
            const float Du2 = Mesh.UV0[I[2] * 2] - Mesh.UV0[I[0] * 2], Dv2 = Mesh.UV0[I[2] * 2 + 1] - Mesh.UV0[I[0] * 2 + 1];
            const float Det = Du1 * Dv2 - Du2 * Dv1;
            if(std::fabs(Det) < 1e-9f) { continue; }
            float AlongU[3], AlongV[3];
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                AlongU[Axis] = (E1[Axis] * Dv2 - E2[Axis] * Dv1) / Det;
                AlongV[Axis] = (E2[Axis] * Du1 - E1[Axis] * Du2) / Det;
            }
            for(const uint32_t Corner : I)
            {
                const float* T = &Mesh.Tangents[Corner * 4];
                const float* N = &Mesh.Normals[Corner * 3];
                const float B[3] = { T[3] * (N[1] * T[2] - N[2] * T[1]), T[3] * (N[2] * T[0] - N[0] * T[2]), T[3] * (N[0] * T[1] - N[1] * T[0]) };
                bTangentFrames &= T[0] * AlongU[0] + T[1] * AlongU[1] + T[2] * AlongU[2] > 0.0f
                                  && B[0] * AlongV[0] + B[1] * AlongV[1] + B[2] * AlongV[2] > 0.0f;
            }
        }
        Expect(bTangentFrames, (std::string(Each.Path) + " has tangents along +U, their bitangents along +V (MTAN)").c_str());
    }

    MeshData Plane;
    if(LoadMeshFile("Engine/Content/Meshes/Plane.nmmesh", Plane))
    {
        Expect(Plane.Normals[1] == 1.0f, "the Plane faces up (+Y)");
    }
}

#if WITH_EDITOR   /* main runs it in editor builds only: it uses the editor's Renderer API */
/* Null backend: one draw per section, mesh bounds, quad fallback. */
static void TestMeshDrawing()
{
    std::printf("[mesh drawing]\n");

    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "NimbleRenderCoreMeshDraw";
    std::error_code Ignored;
    std::filesystem::remove_all(Root, Ignored);
    FilesystemProvider Provider(Root.generic_string());
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Provider);
    MeshAssetCache::Invalidate(std::string());

    MeshData Square = MakeSmokeMesh();
    SaveMeshFile((Root / "Content" / "Square.nmmesh").generic_string(), Square);

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null, {}) succeeds for the mesh test");
    if(bInit)
    {
        const EngineCore::Size Viewport(640, 360);
        const RenderView View = RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
        Rend.SetViewOverride(&View);

        GameCore::World World;
        const GameCore::Entity Meshed = World.CreateEntity("Meshed");
        Meshed.AddComponent<GameCore::TransformComponent>();
        RenderComponent Render;
        std::strcpy(Render.Mesh, "Content/Square.nmmesh");
        Meshed.AddComponent(Render);
        Rend.BindWorld(World);

        uint64_t Frame = 400;
        auto DrawFrame = [&]() -> uint64_t
        {
            RenderStats Before;
            Rend.GetStats(Before);
            Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport});
            Rend.WaitForPreviousFrame();
            RenderStats After;
            Rend.GetStats(After);
            return After.DrawCalls - Before.DrawCalls;
        };

        DrawFrame();   // extracts; the proxy draws from the next frame
        Expect(DrawFrame() == 2, "a two-section mesh draws once per section");

        std::strcpy(Meshed.GetComponent<RenderComponent>()->Mesh, "Content/Missing.nmmesh");
        DrawFrame();
        Expect(DrawFrame() == 1, "a mesh that cannot be loaded draws the quad instead");

        Meshed.GetComponent<RenderComponent>()->Mesh[0] = '\0';
        DrawFrame();
        Expect(DrawFrame() == 1, "no mesh draws the quad");

        Rend.UnbindWorld();
        Rend.SetViewOverride(nullptr);
    }
    Rend.Shutdown();

    MeshAssetCache::Invalidate(std::string());
    SetFileProvider(PreviousProvider);
    std::filesystem::remove_all(Root, Ignored);
}
#endif // WITH_EDITOR

#if WITH_EDITOR   /* main runs it in editor builds only: it uses the editor's Renderer API */
/*
 * Occlusion culling (RENDERPIPELINE.md, "Occlusion culling"): a wall marked
 * Occluder, a box behind it and a box beside it, seen in perspective. On the
 * Null backend every query answers "visible", so this checks what is queried;
 * with NIMBLE_SMOKE_GPU set it checks what the GPU finds, and that culling
 * changes no pixel.
 */
struct OcclusionScene
{
    GameCore::World  World;
    GameCore::Entity Wall, Hidden, Beside;

    OcclusionScene()
    {
        const auto Placed = [this](const char* InName, const EngineCore::Vector3& InAt, const EngineCore::Vector3& InScale, const bool bInOccluder)
        {
            const GameCore::Entity Each = World.CreateEntity(InName);
            GameCore::TransformComponent Transform;
            Transform.Position = InAt;
            Transform.Scale    = InScale;
            Each.AddComponent(Transform);
            RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
            Render.bOccluder = bInOccluder ? 1 : 0;
            Each.AddComponent(Render);
            return Each;
        };
        /* From (0, 100, -800) along +Z: the wall at depth 800 hides x within
         * 200 of the middle; the box beside it, at depth 1100, is clear of it. */
        Wall   = Placed("Wall",   EngineCore::Vector3(0.0, 100.0, 0.0),   EngineCore::Vector3(400.0, 400.0, 20.0), true);
        Hidden = Placed("Hidden", EngineCore::Vector3(0.0, 100.0, 300.0), EngineCore::Vector3(100.0, 100.0, 100.0), false);
        Beside = Placed("Beside", EngineCore::Vector3(420.0, 100.0, 300.0), EngineCore::Vector3(100.0, 100.0, 100.0), false);
    }

    static RenderView MakeView(const EngineCore::Size InViewport, const EngineCore::Vector3& InEye = EngineCore::Vector3(0.0, 100.0, -800.0))
    {
        return RenderView::MakePerspective(InViewport, InEye, EngineCore::Vector3(0.0, 0.0, 1.0), 50.0f, 10.0f, 5000.0f);
    }
};

static void TestOcclusionCulling()
{
    std::printf("[occlusion culling]\n");
    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(ERenderBackend::Null, WindowHandle());
    Expect(bInit, "Renderer::Initialize(Null) for occlusion culling");
    if(bInit)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = OcclusionScene::MakeView(Viewport);
        Rend.SetViewOverride(&View);
        OcclusionScene Scene;
        Rend.BindWorld(Scene.World);

        uint64_t Frame = 1;
        const auto Stats = [&]()
        {
            for(int Each = 0; Each < 3; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            RenderStats Out;
            Rend.GetStats(Out);
            return Out;
        };

        RenderStats On = Stats();
        Expect(On.OcclusionQueriesLastFrame == 2, "both boxes are queried; the occluder is not");
        Expect(On.ProxiesOccludedLastFrame == 0, "and nothing is hidden where every query answers visible");

        ConfigVar* Enabled = ConfigFindVarByPath("Rendering.OcclusionCulling");
        Expect(Enabled != nullptr, "[Rendering] OcclusionCulling is a setting");
        if(Enabled)
        {
            Enabled->SetBool(false);
            Expect(Stats().OcclusionQueriesLastFrame == 0, "off, nothing is queried");
            Enabled->SetBool(true);
        }

        /* A box round the eye could be cut by the near plane: drawn, never queried. */
        const RenderView Inside = OcclusionScene::MakeView(Viewport, EngineCore::Vector3(0.0, 100.0, 300.0));
        Rend.SetViewOverride(&Inside);
        Expect(Stats().OcclusionQueriesLastFrame == 1, "a box the eye is in is not queried");

        const RenderView Flat = RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -1000.0), -1.0f, 5000.0f);
        Rend.SetViewOverride(&Flat);
        Expect(Stats().OcclusionQueriesLastFrame == 0, "an orthographic view queries nothing");

        Rend.SetViewOverride(&View);
        Scene.Wall.GetComponent<RenderComponent>()->bOccluder = 0;
        Expect(Stats().OcclusionQueriesLastFrame == 0, "with no occluder in view, nothing is queried: there is nothing to hide behind");
        Scene.Beside.GetComponent<RenderComponent>()->bOccluder = 1;
        Expect(Stats().OcclusionQueriesLastFrame == 2, "an entity no longer an occluder is queried like any other");

        Rend.UnbindWorld();
        Rend.SetViewOverride(nullptr);
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}

static void TestOcclusionCullingOnGpu()
{
    const char* Gpu = std::getenv("NIMBLE_SMOKE_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[occlusion culling on the GPU] skipped: set NIMBLE_SMOKE_GPU=Vulkan or D3D12\n");
        return;
    }
    const ERenderBackend Backend = std::strcmp(Gpu, "D3D12") == 0 ? ERenderBackend::D3D12 : ERenderBackend::Vulkan;
    std::printf("[occlusion culling on the GPU: %s]\n", GetShaderPlatformName(Backend));

    FilesystemProvider  Disk;
    LayeredFileProvider Layers(&Disk, false);
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Layers);

    Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, WindowHandle());
    Expect(bInit, "Renderer::Initialize on the GPU with no window");
    ConfigVar* Enabled = ConfigFindVarByPath("Rendering.OcclusionCulling");
    if(bInit && Enabled)
    {
        const EngineCore::Size Viewport(256, 256);
        const RenderView View = OcclusionScene::MakeView(Viewport);
        Rend.SetViewOverride(&View);
        OcclusionScene Scene;
        Rend.BindWorld(Scene.World);

        uint64_t Frame = 1;
        const auto Capture = [&](EngineCore::Image& OutPixels, RenderStats& OutStats)
        {
            for(int Each = 0; Each < 3; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each) { Rend.EndFrame(FrameInfo{Frame++, 0.016, Viewport}); Rend.WaitForPreviousFrame(); }
            Rend.GetStats(OutStats);
            std::string Error;
            return Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == 256;
        };
        const auto Differing = [](const EngineCore::Image& InA, const EngineCore::Image& InB)
        {
            int Count = 0;
            for(int Y = 0; Y < 256; ++Y)
            {
                const uint8_t* A = InA.GetData() + InA.GetStride() * Y;
                const uint8_t* B = InB.GetData() + InB.GetStride() * Y;
                for(int X = 0; X < 256 * 4; X += 4)
                {
                    Count += (A[X] != B[X] || A[X + 1] != B[X + 1] || A[X + 2] != B[X + 2]) ? 1 : 0;
                }
            }
            return Count;
        };

        EngineCore::Image Culled, Unculled, Revealed, RevealedUnculled;
        RenderStats CulledStats, UnculledStats, RevealedStats, Ignored;
        const bool bCulled = Capture(Culled, CulledStats);
        std::printf("    behind the wall: %llu of %llu queried boxes occluded\n",
                    (unsigned long long)CulledStats.ProxiesOccludedLastFrame, (unsigned long long)CulledStats.OcclusionQueriesLastFrame);
        Expect(bCulled && CulledStats.OcclusionQueriesLastFrame == 2, "both boxes are queried");
        Expect(bCulled && CulledStats.ProxiesOccludedLastFrame == 1, "the box behind the wall is found hidden and not drawn; the one beside it is drawn");

        Enabled->SetBool(false);
        const bool bUnculled = Capture(Unculled, UnculledStats);
        Enabled->SetBool(true);
        std::printf("    culled vs not: %d pixels differ\n", Differing(Culled, Unculled));
        Expect(bCulled && bUnculled && UnculledStats.OcclusionQueriesLastFrame == 0 && Differing(Culled, Unculled) == 0,
               "and the frame is pixel for pixel the one drawn without occlusion culling");

        /* The wall moved aside: the box shows again, a frame late at most. */
        Scene.Wall.GetComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(-3000.0, 100.0, 0.0);
        const bool bRevealed = Capture(Revealed, RevealedStats);
        Enabled->SetBool(false);
        const bool bRevealedUnculled = Capture(RevealedUnculled, Ignored);
        Enabled->SetBool(true);
        std::printf("    wall moved aside: %d pixels changed; vs unculled %d\n", Differing(Culled, Revealed), Differing(Revealed, RevealedUnculled));
        Expect(bRevealed && RevealedStats.ProxiesOccludedLastFrame == 0 && Differing(Culled, Revealed) > 200,
               "with the wall moved aside, the box is drawn again");
        Expect(bRevealed && bRevealedUnculled && Differing(Revealed, RevealedUnculled) == 0, "exactly as it is without occlusion culling");

        Rend.UnbindWorld();
        Rend.SetViewOverride(nullptr);
        Rend.Shutdown();
    }
    SetFileProvider(PreviousProvider);
}
#endif // WITH_EDITOR

int main()
{
    // Unbuffered, so a run piped to a file shows how far it got if a GPU check hangs.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("RenderCore smoke test\n");

    Threads::SetMainThread();
    Memory::SetCollectorThread();

    TestModuleName();
    TestScriptBindings();
    TestMaterialRuntime();
#if WITH_EDITOR
    TestRenderComponentSerializer();
    TestRenderComponentMaterials();
#endif

    RenderThread Thread;
    const bool bStarted = Thread.Start(ERenderBackend::Null, WindowHandle());
    Expect(bStarted, "RenderThread::Start(Null, {}) succeeds with no window and no GPU");

    if(bStarted)
    {
        TestThreadLifecycle(Thread);
        TestOrdering();
        TestInlineExecution();
        TestMarkers(Thread);
        TestHandBuiltScene(Thread);

        Thread.Stop();
        Expect(!Thread.IsRunning(), "RenderThread::Stop() leaves IsRunning() false");
        Expect(Threads::GetLiveThreadCount() == 0, "no thread is left unjoined after Stop()");
    }

    TestRendererWithWorld();
    TestCustomRenderPasses();
    TestStatUnit();
    TestStaticDrawLists();
    TestShadowSetup();
    TestLightComponents();
    TestParticleSystem();
    TestParticleSystemAsset();
    TestPostProcessStack();
#if WITH_EDITOR
    TestMultipleScenes();
    TestSplitscreenViews();
    TestDebugLines();
    TestUIDrawList();
    TestUISubmit();
    TestMaterials();
    TestMaterialsOnGpu();
    TestShadowsOnGpu();
    TestTranslucencyOnGpu();
    TestParallelRecordingOnGpu();
    TestPostProcessOnGpu();
    TestBloomHdrOnGpu();
    TestParticlesOnGpu();
    TestStatFrameOnGpu();
    TestStaticDrawListsOnGpu();
    TestOcclusionCulling();
    TestOcclusionCullingOnGpu();
    TestClearGradientOnGpu();
    TestSceneCaptureOnGpu();
    TestToneMappingOnGpu();
    TestImageBasedLightingOnGpu();
    TestUIOnGpu();
    TestMeshAssets();
    TestTextureAssets();
    TestTextureCompression();
    TestTextureMips();
    TestTextureStreamingFiles();
    TestTextureStreamer();
    TestTextureMemory();
    TestToneMappingSettings();
    TestFidelitySettings();
    TestImageBasedLightingData();
    TestMeshDrawing();
    TestEngineMeshes();
    TestRenderStages();
#endif

    const uint64_t Leaked = Memory::ReportLeaks();
    Expect(Leaked == 0, "no allocations leaked across the whole run -- every command, extraction "
                         "batch and proxy was freed on the render thread");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

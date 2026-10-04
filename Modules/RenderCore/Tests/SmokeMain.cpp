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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/MeshAsset.h"
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
#include "Private/RenderCore/RenderThreadInternal.h"
#include "Private/RenderCore/ShadowSetup.h"

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
    Expect(Render != nullptr && RenderCount == 10,
           "Render exposes Visible, Mesh, slots by index, material parameters and CastShadows");
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
    const std::string Text = FormatStatUnit(Sample);
    Expect(Text == "Frame:    16.67 ms\nGame:      2.50 ms\nRender:    4.25 ms\nFPS:       60.0", "the overlay's text, a line each");

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
            FrameVar->SetBool(false);
        }
        Rend.Shutdown();
    }

    float Times[RGT_Count] = { 0.01f, 0.5f, -1.0f, 1.25f, 0.2f, 0.05f, 0.75f, 2.8f };
    const std::string FrameText = FormatStatFrame(Times, true);
    Expect(FrameText == "GPU            ms\nPrePass      0.01\nBasePass     0.50\nAO              -\nShadows      1.25\n"
                        "Lighting     0.20\nForward      0.05\nPostProcess  0.75\nTotal        2.80", "stat frame's text, a line per stage");
    Expect(FormatStatFrame(Times, false) == "GPU: not timed on this backend", "and a line saying so where the GPU is not timed");
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
#endif // WITH_EDITOR

#if WITH_EDITOR
// ============================================================================
// Debug lines: queued on the game thread, drawn once, then gone
// ============================================================================

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
}

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
            << "# a test material\nNimbleMaterial 2\nDomain Translucent\nShading Unlit\nBlockSize 48\n"
               "Param Tint vec3 32 1 0.5 0.25 0\n";
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

/* .material v2, the material block, permutation names, the shader cache and
 * the layered provider it mounts on -- no renderer needed. */
static void TestMaterialRuntime()
{
    std::printf("[material runtime]\n");

    // --- .material v2 -------------------------------------------------------------
    MaterialDescription Desc;
    std::string Error;
    const char* Wave = "# generated\nNimbleMaterial 2\nDomain Masked\nShading Lit\nTwoSided 1\nClipValue 0.25\n"
                       "MaxOffset 2\nHasOffset 1\nBlockSize 48\nParam Color vec3 32 0.2 0.6 1 0\n"
                       "Param Height float 44 0.5 0 0 0\nSomeFutureLine 1 2 3\n";
    Expect(ParseMaterialDescription(Wave, Desc, Error), "a version 2 .material parses");
    Expect(Desc.Domain == EMaterialDomain::Masked && Desc.Shading == EMaterialShading::Lit && Desc.bTwoSided
           && Desc.bHasOffset && Desc.MaxOffset == 2.0f && Desc.ClipValue == 0.25f && Desc.BlockSize == 48,
           "its header lines are read");
    const MaterialParameter* Height = Desc.FindParameter("Height");
    Expect(Desc.Parameters.size() == 2 && Height && Height->Offset == 44 && Height->Type == "float" && Height->Default[0] == 0.5f,
           "its parameters keep their offsets and defaults");

    std::vector<uint8_t> Block;
    BuildMaterialBlock(Desc, EngineCore::Color(255, 0, 0, 255), Block);
    float Read[12] = {};
    if(Block.size() == 48) { std::memcpy(Read, Block.data(), sizeof(Read)); }
    Expect(Block.size() == 48 && Read[0] == 1.0f && Read[1] == 0.0f && Read[3] == 1.0f && Read[4] == 0.25f && Read[5] == 1.0f
           && Read[8] == 0.2f && Read[10] == 1.0f && Read[11] == 0.5f,
           "the block is the tint, clip value and two-sided flag, then each default at its offset");

    Expect(!ParseMaterialDescription("NimbleMaterial 1\nParam Tint 0 vec3 1 1 1 0\n", Desc, Error)
           && Error.find("recompile") != std::string::npos, "a version 1 .material is refused, asking for a recompile");
    Expect(!ParseMaterialDescription("NimbleMaterial 2\nBlockSize 32\nParam X vec4 32 0 0 0 0\n", Desc, Error),
           "a parameter outside the block is refused");
    Expect(!ParseMaterialDescription("NimbleMaterial 2\nDomain Sideways\n", Desc, Error), "an unknown domain is refused");

    // --- Texture lines: what a Texture Sample writes -----------------------------------
    const char* Textured = "NimbleMaterial 2\nBlockSize 32\n"
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
    Expect(!ParseMaterialDescription("NimbleMaterial 2\nTexture 0 Default Default Content/A.nmtexture\n", Desc, Error)
           && !ParseMaterialDescription("NimbleMaterial 2\nTexture 8 Default Default Content/A.nmtexture\n", Desc, Error),
           "slot 0 (a PostProcess scene's) and slots past 7 are refused");
    Expect(!ParseMaterialDescription("NimbleMaterial 2\nTexture 1 Blurry Default Content/A.nmtexture\n", Desc, Error)
           && !ParseMaterialDescription("NimbleMaterial 2\nTexture 1 Default Default\n", Desc, Error),
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

    /* One the build cooked, when this runs from the runtime directory. */
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
        DrawFrame();

        const std::vector<std::string> Layers_ = Layers.GetLayerNames();
        Expect(std::find(Layers_.begin(), Layers_.end(), GetShaderCacheLayerName("Engine")) != Layers_.end()
               && std::find(Layers_.begin(), Layers_.end(), GetShaderCacheLayerName("SandboxGame")) != Layers_.end(),
               "the Engine and SandboxGame caches are mounted");

        /* The centre pixel of each preview, as R, G, B; and its quarter
         * points -- top-left, top-right, bottom-left, bottom-right. */
        struct Check { const char* Material; uint64_t Request; int Rgb[3]; bool bOk; int Quarter[4][3]; };
        Check Checks[] = { { "Engine/Content/Materials/DefaultLit.material.nmgraph", 0, {}, false, {} },
                           { "Materials/Glow.material.nmgraph", 0, {}, false, {} },
                           { "Wave", 0, {}, false, {} },       { "Negative", 0, {}, false, {} },
                           { "Textured", 0, {}, false, {} } };
        for(Check& Each : Checks)
        {
            Each.Request = Rend.RequestMaterialPreview(Each.Material, 64, EngineCore::Color(255, 255, 255, 255));
        }
        DrawFrame();
        DrawFrame();
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

        /* Mesh previews: the engine's primitives, as the mesh viewer and the
         * material editor draw them. */
        const auto Preview = [&](Renderer::MeshPreview InPreview, EngineCore::Image& OutPixels, std::string& OutError)
        {
            InPreview.Width  = 96;
            InPreview.Height = 64;
            const uint64_t Request = Rend.RequestMeshPreview(InPreview);
            DrawFrame();
            DrawFrame();
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
            Box.Material = "Textured";
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
            Box.Sections = { "Textured" };
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
    Expect(Info && Info->Version == 3, "RenderComponent saves as version 3 (material parameters, CastShadows)");
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

    /* The lights: version 2 dropped the sun's Direction (it is the
     * Transform's forward now) and added shadows; point and spot are new. */
    const GameCore::ComponentSerializerInfo* SunInfo = GameCore::FindComponentSerializer(DirectionalLightComponent::StaticType().ID);
    Expect(SunInfo && SunInfo->Version == 2, "DirectionalLightComponent saves as version 2");
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
        Expect(!Has(SunOut, "Direction") && Has(SunOut, "ShadowTechnique") && Has(SunOut, "CascadeCount"),
               "version 2 writes no Direction, and the shadow fields");
        RecordingArchive SunOld;
        SunOld.bLoading = true;
        SunOld.Version  = 1;
        SunInfo->Fn(SunOld, &Sun);
        Expect(Has(SunOld, "Direction") && !Has(SunOld, "ShadowTechnique"), "a version 1 save's Direction is read (and dropped)");
    }
    const GameCore::ComponentSerializerInfo* PointInfo = GameCore::FindComponentSerializer(PointLightComponent::StaticType().ID);
    const GameCore::ComponentSerializerInfo* SpotInfo  = GameCore::FindComponentSerializer(SpotLightComponent::StaticType().ID);
    Expect(PointInfo && PointInfo->Version == 1 && SpotInfo && SpotInfo->Version == 1, "Point and Spot Light save as version 1");
    const GameCore::ComponentSerializerInfo* CameraInfo = GameCore::FindComponentSerializer(CameraComponent::StaticType().ID);
    Expect(CameraInfo && CameraInfo->Version == 2, "CameraComponent saves as version 2 (Projection, FovY)");

    const GameCore::ComponentSerializerInfo* PostInfo = GameCore::FindComponentSerializer(PostProcessComponent::StaticType().ID);
    Expect(PostInfo && PostInfo->Version == 2, "PostProcessComponent saves as version 2 (Stack, BlendDistance)");
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
        Expect(HasName(PostOut, "Stack") && HasName(PostOut, "BlendDistance") && !HasName(PostOut, "Material"),
               "version 2 writes Stack and BlendDistance");
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
            Expect(Near(Top, 10, 120, 30, 0) && Near(Bottom, 10, 120, 30, 0), "SetClearColor goes back to one colour");
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
    Expect(GameCore::FindComponentScript(PointLightComponent::StaticType().ID, Count) && Count == 9,
           "Point Light exposes its light and shadow fields to Lua");
    Expect(GameCore::FindComponentScript(SpotLightComponent::StaticType().ID, Count) && Count == 11,
           "Spot Light exposes its light and shadow fields to Lua");
}

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
            << "NimbleMaterial 2\nDomain " << InDomain << "\nShading " << InShading << "\nBlockSize 32\n" << InExtra;
    };
    WriteMaterial("LitOpaque",   "Opaque",      "Lit",   "");
    WriteMaterial("UnlitMasked", "Masked",      "Unlit", "");
    WriteMaterial("TransLit",    "Translucent", "Lit",   "");
    WriteMaterial("TransUnlit",  "Translucent", "Unlit", "");
    WriteMaterial("Glow",        "Translucent", "Unlit", "");    // the smoke mesh's Front section names it
    WriteMaterial("Post",        "PostProcess", "Unlit", "");
    WriteMaterial("Reach",       "Opaque",      "Lit",   "MaxOffset 5\nHasOffset 1\n");
    std::filesystem::create_directories(Root / "Props", Ignored);   // a graph in another folder: Props/Crate.material.nmgraph
    std::ofstream(Root / "Props" / "Crate.material") << "NimbleMaterial 2\nDomain Opaque\nShading Lit\nBlockSize 32\n";

    FilesystemProvider Provider(Root.generic_string());
    IFileProvider* PreviousProvider = GetFileProvider();
    SetFileProvider(&Provider);
    MeshAssetCache::Invalidate(std::string());
    MeshData Square = MakeSmokeMesh();   // Front (material Glow), Back (none)
    SaveMeshFile((Root / "Content" / "Square.nmmesh").generic_string(), Square);

    Expect(GetMaterialRenderStage(EMaterialDomain::Opaque, EMaterialShading::Lit) == ERenderStage::BasePass
           && GetMaterialRenderStage(EMaterialDomain::Masked, EMaterialShading::Unlit) == ERenderStage::Unlit
           && GetMaterialRenderStage(EMaterialDomain::Translucent, EMaterialShading::Lit) == ERenderStage::TranslucentLit
           && GetMaterialRenderStage(EMaterialDomain::Translucent, EMaterialShading::Unlit) == ERenderStage::TranslucentUnlit
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
        Expect(At(ERenderStage::TranslucentLit) == 2, "Translucent Lit takes a quad and the section whose slot names one");
        Expect(At(ERenderStage::TranslucentUnlit) == 3,
               "Translucent Unlit takes a quad and the sections falling back to their own material (an empty slot, a missing one)");
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
        Expect(bFacing, (std::string(Each.Path) + " winds every triangle to face its normals").c_str());
    }

    MeshData Plane;
    if(LoadMeshFile("Engine/Content/Meshes/Plane.nmmesh", Plane))
    {
        Expect(Plane.Normals[1] == 1.0f, "the Plane faces up (+Y)");
    }
}

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

int main()
{
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
    TestStatUnit();
    TestStaticDrawLists();
    TestShadowSetup();
    TestLightComponents();
    TestPostProcessStack();
#if WITH_EDITOR
    TestMultipleScenes();
    TestDebugLines();
    TestMaterials();
    TestMaterialsOnGpu();
    TestShadowsOnGpu();
    TestPostProcessOnGpu();
    TestBloomHdrOnGpu();
    TestStatFrameOnGpu();
    TestStaticDrawListsOnGpu();
    TestClearGradientOnGpu();
    TestSceneCaptureOnGpu();
    TestMeshAssets();
    TestTextureAssets();
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

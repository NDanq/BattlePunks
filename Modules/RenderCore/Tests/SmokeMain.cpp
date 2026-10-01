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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreVersion.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/RenderThread.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/RenderCore/TextureAsset.h"

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
    Expect(Render != nullptr && RenderCount == 6, "Render exposes Width, Height, Tint, Visible, Material and Mesh");

    bool bProxyExposed = false;
    for(size_t Index = 0; Index < RenderCount; ++Index)
    {
        if(std::strstr(Render[Index].Name, "Proxy") != nullptr) { bProxyExposed = true; }
    }
    Expect(!bProxyExposed, "the render proxy handle is not exposed");

    size_t CameraCount = 0;
    GameCore::FindComponentScript(CameraComponent::StaticType().ID, CameraCount);
    Expect(CameraCount == 3, "Camera exposes ZNear, ZFar and Active");
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
    Init.Width  = 2.0f;
    Init.Height = 4.0f;
    Init.Tint   = EngineCore::Color(10, 20, 30);

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
    RenderComponent Prim;
    Prim.Width  = 1.0f;
    Prim.Height = 1.0f;
    E1.AddComponent(Prim);

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
        Quad.AddComponent<GameCore::TransformComponent>();
        RenderComponent Prim;
        Prim.Width = 100.0f;
        Prim.Height = 100.0f;
        std::strcpy(Prim.Material, "SmokeGlow");
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
            << "# a test material\nNimbleMaterial 1\nParam Tint 0 vec3 1 0.5 0.25 0\n";
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
#if WITH_EDITOR
    TestDebugLines();
    TestMaterials();
    TestMeshAssets();
    TestTextureAssets();
    TestMeshDrawing();
#endif

    const uint64_t Leaked = Memory::ReportLeaks();
    Expect(Leaked == 0, "no allocations leaked across the whole run -- every command, extraction "
                         "batch and proxy was freed on the render thread");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

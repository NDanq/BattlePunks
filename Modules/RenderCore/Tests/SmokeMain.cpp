/*
 * RenderCoreSmokeTest -- links RenderCore and NOTHING else (see
 * CMakeLists.txt's comment: it must NOT link GDI::GDI, which is the
 * structural check that no GDI header has leaked into Public/).
 *
 * Through step 3 (proxies, scene, the quad primitive). The World-driven
 * cases, the test-defined proxy type and teardown/leak checks arrive with
 * step 4 (WorldBridge + the Renderer facade); see DESIGN.md's "Build order"
 * and this module's own "Smoke test" section for the full list. Written in
 * GameCoreSmokeTest's Expect()-counter style.
 */

#include <atomic>
#include <cstdio>
#include <cstring>

#include "Public/Core/CoreThread.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreVersion.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/RenderThread.h"
#include "Public/RenderCore/RenderView.h"

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

    const bool bRegistered = RegisterProxyType(PrimitiveComponent::StaticType().ID, GetPrimitiveComponentFactory());
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
    PrimitiveComponent Init;
    Init.Width  = 2.0f;
    Init.Height = 4.0f;
    Init.Tint   = EngineCore::Color(10, 20, 30);

    const GameCore::EntityID Owner(1, 0);
    CreateProxyFn Factory = FindProxyFactory(PrimitiveComponent::StaticType().ID);
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
    PrimitiveComponent Updated = Init;
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

    UnregisterProxyType(PrimitiveComponent::StaticType().ID);
    Expect(FindProxyFactory(PrimitiveComponent::StaticType().ID) == nullptr,
           "UnregisterProxyType clears the factory table slot");
}

int main()
{
    std::printf("RenderCore smoke test\n");

    Threads::SetMainThread();

    TestModuleName();

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

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

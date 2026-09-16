/*
 * GameCoreSmokeTest -- links GameCore and NOTHING else (see CMakeLists.txt's
 * comment: it must NOT link Lua::Lua, which is the structural check that
 * lua.h has not leaked into Public/).
 *
 * Written in Modules/Core/Tests/SmokeMain.cpp's style: an Expect(cond, "what")
 * counter and a nonzero exit status, which is exactly what add_test() checks.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/SpatialIndex.h"
#include "Public/GameCore/World.h"

using namespace GameCore;
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

namespace
{
    /* Installed so CORE_CHECK returns rather than aborts -- Core's own smoke
     * test installs the same kind of counting handler for the same reason. */
    int GHandlerCalls = 0;
    void TestLogHandler(const LogVerbosity /*Verbosity*/, const char* /*Message*/) { ++GHandlerCalls; }
}

// ============================================================================
// Component registry
// ============================================================================

struct TestOnlyComponent
{
    GAMECORE_COMPONENT_BODY()
    int Value = 0;
};
GAMECORE_DEFINE_COMPONENT(TestOnlyComponent, "TestOnly")

static void TestComponentRegistry()
{
    std::printf("[registry]\n");

    const ComponentTypeID TransformID1 = TransformComponent::StaticType().ID;
    const ComponentTypeID TransformID2 = TransformComponent::StaticType().ID;
    Expect(TransformID1 == TransformID2, "StaticType().ID is stable across calls");

    const ComponentTypeInfo* Found = FindComponentType("Transform");
    Expect(Found != nullptr && Found->ID == TransformID1, "FindComponentType(\"Transform\") agrees with StaticType()");

    const ComponentTypeID TestOnlyID = TestOnlyComponent::StaticType().ID;
    Expect(TestOnlyID != TransformID1, "a locally-defined component gets a distinct id");

    Expect(sizeof(TransformComponent) == 64, "TransformComponent is 64 bytes (one cache line) under WITH_SIMD");
}

// ============================================================================
// Entities
// ============================================================================

static void TestEntities()
{
    std::printf("[entities]\n");

    World W;
    const EntityID A = W.CreateEntity();
    const EntityID B = W.CreateEntity();
    Expect(W.GetEntityCount() == 2, "two entities created");
    Expect(W.IsAlive(A) && W.IsAlive(B), "both are alive");

    W.DestroyEntity(A);
    Expect(!W.IsAlive(A), "A is dead after DestroyEntity");
    Expect(W.GetEntityCount() == 1, "count drops to 1");

    const EntityID C = W.CreateEntity();
    Expect(C.Index == A.Index, "the recycled index comes back");
    Expect(C.Generation > A.Generation, "the recycled slot has a higher generation");
    Expect(!W.IsAlive(A), "the OLD handle still reads dead even though its index is reused");
    Expect(W.IsAlive(C), "the NEW handle is alive");
}

// ============================================================================
// Components + owner header
// ============================================================================

static void TestComponents()
{
    std::printf("[components]\n");

    World W;
    const EntityID E1 = W.CreateEntity("Alice");
    const EntityID E2 = W.CreateEntity("Bob");

    TransformComponent* T1 = W.AddComponent<TransformComponent>(E1);
    Expect(T1 != nullptr, "AddComponent succeeds");
    Expect(W.HasComponent<TransformComponent>(E1), "HasComponent true after Add");

    T1->Position = Vector3(1.0, 2.0, 3.0);

    Expect(W.GetComponent<TransformComponent>(E2) == nullptr, "GetComponent on an entity without one is null");

    const EntityID Dead = EntityID(9999, 0);
    Expect(W.GetComponent<TransformComponent>(Dead) == nullptr, "GetComponent on a dead entity is null");

    TransformComponent* T2 = W.AddComponent<TransformComponent>(E2);
    T2->Position = Vector3(4.0, 5.0, 6.0);

    Expect(T1->Owner.OwningWorld == &W, "owner header's World is set");
    Expect(T1->Owner.Entity == E1, "owner header's Entity is set");
    Expect(std::strcmp(T1->Owner.GetName(), "Alice") == 0, "GetSibling<Name> through the owner header finds the right entity's name");
    Expect(std::strcmp(T2->Owner.GetName(), "Bob") == 0, "same, for the second entity");

    Expect(W.RemoveComponent<TransformComponent>(E1), "RemoveComponent succeeds");
    Expect(!W.HasComponent<TransformComponent>(E1), "HasComponent false after Remove");
    Expect(W.GetComponent<TransformComponent>(E2) != nullptr, "the survivor's component is intact after swap-remove");
    Expect(W.GetComponent<TransformComponent>(E2)->Position.GetX() == 4.0, "the survivor's DATA is intact after swap-remove");

    int VisitCount = 0;
    W.Each<TransformComponent>([&](Entity /*E*/, TransformComponent& /*T*/) { ++VisitCount; });
    Expect(VisitCount == 1, "Each visits exactly the live set");
}

// ============================================================================
// Tick phases
// ============================================================================

namespace
{
    struct PhaseOrderContext { std::string Order; };

    void AppendPhaseTag(World& /*W*/, float /*Dt*/, void* InUserData)
    {
        static_cast<PhaseOrderContext*>(InUserData)->Order += 'X';
    }

    void SystemA(World&, float, void* InUserData) { static_cast<PhaseOrderContext*>(InUserData)->Order += 'A'; }
    void SystemB(World&, float, void* InUserData) { static_cast<PhaseOrderContext*>(InUserData)->Order += 'B'; }

    EntityID GDestroyTarget;
    bool     GSeenAliveInPrePhysics  = false;
    bool     GSeenDeadInPostPhysics  = false;

    void DestroyerSystem(World& W, float, void* /*InUserData*/)
    {
        GSeenAliveInPrePhysics = W.IsAlive(GDestroyTarget);
        W.DestroyEntity(GDestroyTarget);
    }

    void ObserverSystem(World& W, float, void* /*InUserData*/)
    {
        GSeenDeadInPostPhysics = !W.IsAlive(GDestroyTarget);
    }
}

static void TestTickPhases()
{
    std::printf("[phases]\n");

    {
        World W;
        PhaseOrderContext Ctx;
        W.RegisterSystem("PostUpdateTag", TP_PostUpdate, &AppendPhaseTag, &Ctx);
        W.RegisterSystem("PrePhysicsTag", TP_PrePhysics, &AppendPhaseTag, &Ctx);
        W.Tick(0.016f);
        Expect(Ctx.Order.size() == 2, "two systems registered across two phases both ran");
        Expect(Ctx.Order[0] == 'X' && Ctx.Order[1] == 'X', "phase order is honoured regardless of registration order");
    }

    {
        World W;
        PhaseOrderContext Ctx;
        W.RegisterSystem("A", TP_PrePhysics, &SystemA, &Ctx);
        W.RegisterSystem("B", TP_PrePhysics, &SystemB, &Ctx);
        W.TickPhase(TP_PrePhysics, 0.016f);
        Expect(Ctx.Order == "AB", "two systems in one phase run in registration order");
    }

    {
        World W;
        GDestroyTarget = W.CreateEntity();
        GSeenAliveInPrePhysics = false;
        GSeenDeadInPostPhysics = false;
        W.RegisterSystem("Destroyer", TP_PrePhysics, &DestroyerSystem, nullptr);
        W.RegisterSystem("Observer",  TP_PostPhysics, &ObserverSystem, nullptr);
        W.Tick(0.016f);
        Expect(GSeenAliveInPrePhysics, "a PrePhysics system sees the entity alive before it destroys it");
        Expect(GSeenDeadInPostPhysics, "a PostPhysics system sees the entity dead in the SAME tick");
    }
}

// ============================================================================
// Delegates
// ============================================================================

namespace
{
    struct SumContext { int Sum = 0; void* LastUser = nullptr; };
    void AddOne(int InValue, void* InUserData) { auto* Ctx = static_cast<SumContext*>(InUserData); Ctx->Sum += InValue; Ctx->LastUser = InUserData; }

    int GReentrantCheckCalls = 0;
    void ReentrantCallback(int, void* InUserData)
    {
        auto* Delegate = static_cast<TMulticastDelegate<int>*>(InUserData);
        Delegate->Broadcast(1);   // re-entrant broadcast of the SAME delegate -- must be refused, not crash
    }

    int GAddedDuringBroadcast = 0;
    void AddDuringBroadcastCallback(int, void* InUserData) { ++GAddedDuringBroadcast; }
}

static void TestDelegates()
{
    std::printf("[delegates]\n");

    {
        TMulticastDelegate<int> D;
        SumContext Ctx1, Ctx2;
        const DelegateHandle H1 = D.Add(&AddOne, &Ctx1);
        D.Add(&AddOne, &Ctx2);
        D.Broadcast(5);
        Expect(Ctx1.Sum == 5 && Ctx2.Sum == 5, "Broadcast reaches every binding with the right args and user pointer");

        Expect(D.Remove(H1), "Remove by handle succeeds");
        Expect(!D.Remove(H1), "removing the same handle twice is a no-op, not a crash");
        D.Broadcast(5);
        Expect(Ctx1.Sum == 5 && Ctx2.Sum == 10, "only the removed binding stopped firing");
    }

    {
        TMulticastDelegate<int> D;
        SumContext Ctx1, Ctx2, Ctx3;
        D.Add(&AddOne, &Ctx1);
        D.Add(&AddOne, &Ctx2);
        D.Add(&AddOne, &Ctx3);
        Expect(D.RemoveAll(&Ctx2) == 1, "RemoveAll(user) removes only that user's binding");
        D.Broadcast(1);
        Expect(Ctx1.Sum == 1 && Ctx2.Sum == 0 && Ctx3.Sum == 1, "the other two still fire");
    }

    {
        TMulticastDelegate<int> D;
        D.Add(&ReentrantCallback, &D);
        GHandlerCalls = 0;
        D.Broadcast(1);   // triggers the re-entrant broadcast, which CORE_CHECKs and returns
        Expect(GHandlerCalls >= 1, "a re-entrant broadcast of the SAME delegate trips the check handler");
    }

    {
        TMulticastDelegate<int> D;
        struct SelfRemoveCtx { TMulticastDelegate<int>* Self; DelegateHandle Handle; };
        static SelfRemoveCtx Ctx;
        Ctx.Self = &D;
        GAddedDuringBroadcast = 0;
        auto SelfRemove = [](int, void* InUserData)
        {
            auto* C = static_cast<SelfRemoveCtx*>(InUserData);
            C->Self->Remove(C->Handle);
            C->Self->Add(&AddDuringBroadcastCallback, nullptr);
        };
        Ctx.Handle = D.Add(SelfRemove, &Ctx);
        D.Broadcast(0);
        Expect(GAddedDuringBroadcast == 0, "an Add during a broadcast is not called THIS broadcast");
        D.Broadcast(0);
        Expect(GAddedDuringBroadcast == 1, "...but IS called the next one, and the self-removed binding is gone");
    }

    {
        TMulticastDelegate<int> D;
        SumContext Ctx;
        {
            TScopedBinding<int> Scoped(D, &AddOne, &Ctx);
            D.Broadcast(1);
            Expect(Ctx.Sum == 1, "TScopedBinding's binding fires while in scope");
        }
        D.Broadcast(1);
        Expect(Ctx.Sum == 1, "TScopedBinding unbound on scope exit");
    }

    {
        TDelegate<bool, int> D;
        D.Bind([](int InValue, void*) -> bool { return InValue > 0; });
        bool Result = false;
        Expect(D.ExecuteIfBound(5, &Result) && Result, "TDelegate<bool,...> round-trips a bool return");
        D.Unbind();
        Expect(!D.ExecuteIfBound(5, &Result), "ExecuteIfBound on an unbound TDelegate returns false");
    }
}

// ============================================================================
// Event bus
// ============================================================================

namespace
{
    struct EventCaptureContext { EventID Event = EVENT_INVALID; EventArgs Args; int Calls = 0; };
    void CaptureEvent(EventID InEvent, const EventArgs& InArgs, void* InUserData)
    {
        auto* Ctx = static_cast<EventCaptureContext*>(InUserData);
        Ctx->Event = InEvent;
        Ctx->Args  = InArgs;
        ++Ctx->Calls;
    }
}

static void TestEventBus()
{
    std::printf("[event bus]\n");

    EventBus Bus;
    const EventID ID1 = Bus.GetEventID("Foo");
    const EventID ID2 = Bus.GetEventID("Foo");
    Expect(ID1 == ID2, "GetEventID is stable for the same name");
    Expect(Bus.FindEventID("Foo") == ID1, "FindEventID agrees with GetEventID");
    Expect(Bus.FindEventID("Bar") == EVENT_INVALID, "FindEventID on an unregistered name is EVENT_INVALID");

    EventCaptureContext ByID, ByName;
    Bus.Subscribe(ID1, &CaptureEvent, &ByID);
    const DelegateHandle H2 = Bus.Subscribe("Foo", &CaptureEvent, &ByName);

    EventArgs Args;
    Args.Args[0] = EventArg::Make(42.0);
    Args.Count   = 1;
    Bus.Emit(ID1, Args);
    Expect(ByID.Calls == 1 && ByName.Calls == 1, "emit by id reaches subscribers registered both ways");
    Bus.Emit("Foo", Args);
    Expect(ByID.Calls == 2, "emit by name reaches the same subscriber list as emit by id");

    std::string LongString(200, 'x');
    EventArg StringArg = EventArg::Make(LongString.c_str());
    Expect(std::strlen(StringArg.String) == EventArg::STRING_MAX - 1, "a long string argument is truncated, never overrun");

    Expect(Bus.Unsubscribe(ID1, H2), "Unsubscribe by handle succeeds");

    EventCaptureContext MultiA, MultiB;
    const EventID IDBar = Bus.GetEventID("Bar");
    Bus.Subscribe(ID1, &CaptureEvent, &MultiA);
    Bus.Subscribe(IDBar, &CaptureEvent, &MultiA);
    Bus.Subscribe(IDBar, &CaptureEvent, &MultiB);
    Expect(Bus.UnsubscribeAll(&MultiA) == 2, "UnsubscribeAll(user) spans every event, not just one");
}

// ============================================================================
// Spatial index
// ============================================================================

static AABB MakeBox(const double MinX, const double MinY, const double MinZ,
                     const double MaxX, const double MaxY, const double MaxZ)
{
    return AABB(Vector3(MinX, MinY, MinZ), Vector3(MaxX, MaxY, MaxZ));
}

namespace
{
    struct QueryCollector { std::vector<EntityID> Hits; };
    bool CollectHit(const EntityID InEntity, void* InUserData)
    {
        static_cast<QueryCollector*>(InUserData)->Hits.push_back(InEntity);
        return true;
    }
}

static void TestSpatialIndex()
{
    std::printf("[spatial index]\n");

    SpatialIndex Index;
    std::vector<int32_t> Proxies;
    std::vector<AABB>    Boxes;
    std::vector<EntityID> Entities;

    unsigned Seed = 12345u;
    const auto NextRand = [&]() -> double { Seed = Seed * 1103515245u + 12345u; return double((Seed >> 8) % 10000) / 10.0; };

    for(uint32_t Index_ = 0; Index_ < 200; ++Index_)
    {
        const double X = NextRand(), Y = NextRand(), Z = NextRand();
        const AABB Box = MakeBox(X, Y, Z, X + 1.0, Y + 1.0, Z + 1.0);
        const EntityID E(Index_, 0);
        const int32_t Proxy = Index.Insert(E, Box);
        Proxies.push_back(Proxy);
        Boxes.push_back(Box);
        Entities.push_back(E);
    }
    Expect(Index.Validate(), "tree invariants hold after 200 inserts");
    Expect(Index.GetProxyCount() == 200, "proxy count matches insert count");

    bool bAllQueriesMatchBruteForce = true;
    for(int Q = 0; Q < 50; ++Q)
    {
        const double X = NextRand(), Y = NextRand(), Z = NextRand();
        const AABB QueryBox = MakeBox(X, Y, Z, X + 5.0, Y + 5.0, Z + 5.0);

        QueryCollector Collector;
        Index.QueryBounds(QueryBox, &CollectHit, &Collector);

        size_t BruteForceCount = 0;
        for(size_t I = 0; I < Boxes.size(); ++I)
        {
            if(Boxes[I].Overlaps(QueryBox)) { ++BruteForceCount; }
        }
        /* The tree tests against FAT boxes (see SpatialIndex.h), so it may
         * report a hit the tight brute-force box comparison would not -- never
         * the other way around. */
        if(Collector.Hits.size() < BruteForceCount) { bAllQueriesMatchBruteForce = false; }
    }
    Expect(bAllQueriesMatchBruteForce, "QueryBounds never MISSES what a tight brute-force overlap scan finds");

    // Move within the fat margin: no change reported.
    const bool bEscaped = Index.Move(Proxies[0], Boxes[0].Fattened(0.0));   // still fully inside its own fat box
    Expect(!bEscaped, "a move that stays inside the fat box reports no re-insertion");
    Expect(Index.Validate(), "tree still valid after a no-op move");

    // Move far enough to escape.
    const AABB FarBox = MakeBox(9000.0, 9000.0, 9000.0, 9001.0, 9001.0, 9001.0);
    const bool bEscaped2 = Index.Move(Proxies[0], FarBox);
    Expect(bEscaped2, "a move past the fat margin reports a re-insertion");
    Expect(Index.Validate(), "tree still valid after a real move");

    QueryCollector AfterMove;
    Index.QueryBounds(FarBox, &CollectHit, &AfterMove);
    bool bFoundAtNewPosition = false;
    for(const EntityID& E : AfterMove.Hits) { if(E == Entities[0]) { bFoundAtNewPosition = true; } }
    Expect(bFoundAtNewPosition, "the moved entity is found at its NEW position");

    QueryCollector AtOldPosition;
    Index.QueryBounds(Boxes[0], &CollectHit, &AtOldPosition);
    bool bStillAtOldPosition = false;
    for(const EntityID& E : AtOldPosition.Hits) { if(E == Entities[0]) { bStillAtOldPosition = true; } }
    Expect(!bStillAtOldPosition, "the moved entity is gone from its OLD position");

    // Raycast: three boxes on a line, ray should hit the nearest.
    {
        SpatialIndex RayIndex;
        RayIndex.Insert(EntityID(1, 0), MakeBox(10, -1, -1, 11, 1, 1));
        RayIndex.Insert(EntityID(2, 0), MakeBox(20, -1, -1, 21, 1, 1));
        RayIndex.Insert(EntityID(3, 0), MakeBox(30, -1, -1, 31, 1, 1));

        RaycastHit Hit;
        const bool bHit = RayIndex.Raycast(Vector3(0, 0, 0), Vector3(1, 0, 0), 100.0f, Hit);
        Expect(bHit, "a ray fired down the line of boxes hits something");
        Expect(Hit.Entity == EntityID(1, 0), "the NEAREST box is reported, not just any hit");

        RaycastHit MissHit;
        const bool bMissed = RayIndex.Raycast(Vector3(0, 0, 0), Vector3(0, 1, 0), 100.0f, MissHit);
        Expect(!bMissed, "a ray aimed away from every box reports no hit");
    }

    Index.Remove(Proxies[1]);
    Expect(Index.GetProxyCount() == 199, "Remove drops the proxy count");
    Expect(Index.Validate(), "tree still valid after a remove");

    Index.Rebuild();
    Expect(Index.Validate(), "tree still valid after a full rebuild");
    Expect(Index.GetProxyCount() == 199, "Rebuild preserves the live proxy set");
}

static void TestWorldSpatialIntegration()
{
    std::printf("[world + spatial]\n");

    World W;
    const EntityID E1 = W.CreateEntity();
    BoundsComponent* B1 = W.AddComponent<BoundsComponent>(E1);
    B1->Local = MakeBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);

    Expect(B1->ProxyID == -1, "a freshly-added Bounds has no proxy yet");
    W.RefreshSpatialIndex();
    Expect(W.GetComponent<BoundsComponent>(E1)->ProxyID >= 0, "the first refit inserts the leaf");

    // Add a Transform AFTER the Bounds -- the "may not have its Transform yet" case.
    const EntityID E2 = W.CreateEntity();
    BoundsComponent* B2 = W.AddComponent<BoundsComponent>(E2);
    B2->Local = MakeBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);
    TransformComponent* T2 = W.AddComponent<TransformComponent>(E2);
    T2->Position = Vector3(100.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    Expect(W.GetComponent<BoundsComponent>(E2)->World.Min.GetX() > 99.0, "a Bounds added before its Transform is placed correctly once both exist");

    QueryCollector NearOrigin;
    W.QueryBounds(MakeBox(-10, -10, -10, 10, 10, 10), &CollectHit, &NearOrigin);
    bool bFoundE1 = false, bFoundE2 = false;
    for(const EntityID& E : NearOrigin.Hits) { if(E == E1) bFoundE1 = true; if(E == E2) bFoundE2 = true; }
    Expect(bFoundE1 && !bFoundE2, "a region query near the origin finds E1 but not the far-away E2");

    // Move the transform and refresh: the entity should relocate in the tree.
    T2->Position = Vector3(0.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    QueryCollector NearOriginAfterMove;
    W.QueryBounds(MakeBox(-10, -10, -10, 10, 10, 10), &CollectHit, &NearOriginAfterMove);
    bool bFoundE2AfterMove = false;
    for(const EntityID& E : NearOriginAfterMove.Hits) { if(E == E2) bFoundE2AfterMove = true; }
    Expect(bFoundE2AfterMove, "after moving the Transform and refreshing, the entity shows up in its new region");

    // Static bounds ignore a Transform change entirely.
    const EntityID E3 = W.CreateEntity();
    BoundsComponent* B3 = W.AddComponent<BoundsComponent>(E3);
    B3->Local    = MakeBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);
    B3->bStatic  = 1;
    TransformComponent* T3 = W.AddComponent<TransformComponent>(E3);
    T3->Position = Vector3(500.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    const AABB StaticWorldBoxBefore = W.GetComponent<BoundsComponent>(E3)->World;
    T3->Position = Vector3(-500.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    const AABB StaticWorldBoxAfter = W.GetComponent<BoundsComponent>(E3)->World;
    Expect(StaticWorldBoxBefore.Min.GetX() == StaticWorldBoxAfter.Min.GetX(), "a static Bounds never re-fits, regardless of Transform changes");

    // EachInBounds only visits hits that actually have T.
    int TransformHits = 0;
    W.EachInBounds<TransformComponent>(MakeBox(-1000, -1000, -1000, 1000, 1000, 1000),
        [&](Entity, TransformComponent&) { ++TransformHits; });
    Expect(TransformHits == 2, "EachInBounds only visits entities that have the requested component (E1 has no Transform)");

    // Destroying an entity removes its leaf before anything can query it.
    const EntityID DestroyedID = E1;
    W.DestroyEntity(DestroyedID);
    QueryCollector AfterDestroy;
    W.QueryBounds(MakeBox(-10, -10, -10, 10, 10, 10), &CollectHit, &AfterDestroy);
    bool bStillFound = false;
    for(const EntityID& E : AfterDestroy.Hits) { if(E == DestroyedID) bStillFound = true; }
    Expect(!bStillFound, "DestroyEntity removes the leaf; a destroyed entity is never returned by a query");
}

// ============================================================================
// Scripting
// ============================================================================

static void TestScripting()
{
    std::printf("[scripting]\n");

    World W;
    ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "ScriptEngine::Initialize succeeds");
    Scripts.BindWorld(&W);

    Expect(Scripts.RunString(
        "local e = World.CreateEntity(\"Scripted\")\n"
        "local t = e:AddComponent(\"Transform\")\n"
        "t:SetPosition(1, 2, 3)\n",
        "test-basic"), "RunString executes a basic entity/component script");

    const EntityID Found = W.FindEntityByName("Scripted");
    Expect(Found.IsSet(), "an entity created from Lua is findable from C++");
    TransformComponent* T = W.GetComponent<TransformComponent>(Found);
    Expect(T != nullptr && T->Position.GetX() == 1.0, "a component set from Lua is readable from C++");

    // A registered class ticks through the phase system.
    Expect(Scripts.RunString(
        "TickStarts = 0\n"
        "TickCalls = 0\n"
        "Script.Register(\"Counter\", {\n"
        "    OnStart = function(self) TickStarts = TickStarts + 1 end,\n"
        "    OnTick  = function(self, dt) TickCalls = TickCalls + 1 end,\n"
        "})\n",
        "test-class"), "RunString registers a script class");

    const EntityID Counted = W.CreateEntity();
    Expect(Scripts.InstantiateScript(Counted, "Counter"), "InstantiateScript succeeds for a registered class");

    W.Tick(0.016f);
    W.Tick(0.016f);
    W.Tick(0.016f);

    Scripts.RunString("assert(TickStarts == 1, 'expected OnStart once, got ' .. TickStarts)", "assert-starts");
    Scripts.RunString("assert(TickCalls == 3, 'expected OnTick 3 times, got ' .. TickCalls)", "assert-ticks");

    // A script that errors logs one error and the loop stays alive.
    const uint32_t ErrorsBefore = Scripts.GetErrorCount();
    Expect(Scripts.RunString(
        "Script.Register(\"Broken\", { OnTick = function(self, dt) error('boom') end })",
        "test-broken"), "registering a class that will error still succeeds");
    const EntityID BrokenEntity = W.CreateEntity();
    Scripts.InstantiateScript(BrokenEntity, "Broken");
    W.Tick(0.016f);
    W.Tick(0.016f);   // second tick must not re-raise -- bErrored gates it
    Expect(Scripts.GetErrorCount() == ErrorsBefore + 1, "exactly one error is logged, not one per tick");

    // OnDestroy runs and the instance ref is released.
    Expect(Scripts.RunString(
        "Destroyed = false\n"
        "Script.Register(\"Notify\", { OnDestroy = function(self) Destroyed = true end })",
        "test-destroy"), "registering a destroy-notifying class succeeds");
    const EntityID NotifyEntity = W.CreateEntity();
    Scripts.InstantiateScript(NotifyEntity, "Notify");
    W.DestroyEntity(NotifyEntity);
    Scripts.RunString("assert(Destroyed == true, 'OnDestroy did not run')", "assert-destroyed");

    // A proxy held across a component removal errors cleanly rather than aliasing.
    Expect(Scripts.RunString(
        "local e = World.CreateEntity()\n"
        "local t = e:AddComponent(\"Transform\")\n"
        "e:RemoveComponent(\"Transform\")\n"
        "local ok, err = pcall(function() t:GetPosition() end)\n"
        "assert(ok == false, 'expected the stale proxy to error')\n",
        "test-stale-proxy"), "a proxy held across RemoveComponent errors instead of aliasing");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
    Expect(!Scripts.IsInitialized(), "ScriptEngine reports not-initialized after Shutdown");
}

int main()
{
    std::printf("GameCore smoke test\n");

    SetLogHandler(&TestLogHandler);

    TestComponentRegistry();
    TestEntities();
    TestComponents();
    TestTickPhases();
    TestDelegates();
    TestEventBus();
    TestSpatialIndex();
    TestWorldSpatialIntegration();
    TestScripting();

    SetLogHandler(nullptr);

    const uint64_t Leaked = Memory::ReportLeaks();
    Expect(Leaked == 0, "no allocations leaked across the whole run");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

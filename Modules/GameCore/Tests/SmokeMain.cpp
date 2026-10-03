/*
 * GameCoreSmokeTest -- links GameCore and NOTHING else (see CMakeLists.txt's
 * comment: it must NOT link Lua::Lua, which is the structural check that
 * lua.h has not leaked into Public/).
 *
 * Written in Modules/Core/Tests/SmokeMain.cpp's style: an Expect(cond, "what")
 * counter and a nonzero exit status, which is exactly what add_test() checks.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ArchiveTextDump.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/SpatialIndex.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/GameCore/WorldSerializer.h"

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

#if WITH_EDITOR
// ============================================================================
// Editor-visible properties
// ============================================================================

struct PropertyProbeComponent
{
    GAMECORE_COMPONENT_BODY()
    float   Speed   = 1.5f;
    uint8_t bArmed  = 0;
    char    Label[16] = "probe";
    int32_t Slots[4]  = {};
};
GAMECORE_DEFINE_COMPONENT(PropertyProbeComponent, "TestPropertyProbe")

GAMECORE_DEFINE_COMPONENT_PROPERTIES(PropertyProbeComponent)
{
    Props.Field("Speed", &PropertyProbeComponent::Speed);
    Props.Category("Motion");
    Props.Flag("Armed",  &PropertyProbeComponent::bArmed, PF_ReadOnly);
    Props.Category("Text");
    Props.Field("Label", &PropertyProbeComponent::Label);
    Props.Category(nullptr);
    Props.Field("Rate",  &PropertyProbeComponent::Speed);
    Props.Field("ThisNameIsFarTooLongToBeStoredInThePropertyTable", &PropertyProbeComponent::Speed);
    Props.Category("Lists");
    Props.Array("Slots", &PropertyProbeComponent::Slots);
    Props.Category("ThisCategoryIsFarTooLongToBeStored");
    Props.Field("Spare", &PropertyProbeComponent::Speed);
}

/* A struct array: its element's members as entries after it. */
struct PropertyProbePair
{
    char    Key[8] = {};
    uint8_t Kind = 0;
    float   Values[2] = {};
};

struct StructArrayProbeComponent
{
    GAMECORE_COMPONENT_BODY()
    float             Before = 0.0f;
    PropertyProbePair Pairs[3] = {};
};
GAMECORE_DEFINE_COMPONENT(StructArrayProbeComponent, "TestStructArrayProbe")

GAMECORE_DEFINE_COMPONENT_PROPERTIES(StructArrayProbeComponent)
{
    Props.Field("Before", &StructArrayProbeComponent::Before);
    Props.Category("Pairs");
    Props.StructArray("Pairs", &StructArrayProbeComponent::Pairs, [](auto& Member)
    {
        Member.Field("Key",  &PropertyProbePair::Key);
        Member.Field("Kind", &PropertyProbePair::Kind);
        Member.Element("Second", &PropertyProbePair::Values, 1);
        Member.Element("Missing", &PropertyProbePair::Values, 2);   // out of range: rejected
    });
}

static void TestStructArrayProperties()
{
    std::printf("[struct array properties]\n");

    size_t Count = 0;
    const ComponentPropertyEntry* Entries = FindComponentProperties(StructArrayProbeComponent::StaticType().ID, Count);
    Expect(Entries != nullptr && Count == 5, "a field, the array, and three members; the out-of-range element is rejected");
    if(!Entries || Count != 5) { return; }

    const ComponentPropertyEntry& Pairs = Entries[1];
    Expect(Pairs.IsStructArray() && !Pairs.IsMember() && Pairs.ArrayCount == 3 && Pairs.Size == sizeof(PropertyProbePair)
           && Pairs.Stride == sizeof(PropertyProbePair) && Pairs.Offset == offsetof(StructArrayProbeComponent, Pairs),
           "StructArray registers a Struct array of the element's size and stride");
    Expect(!Entries[0].IsMember() && Entries[2].Parent == 1 && Entries[3].Parent == 1 && Entries[4].Parent == 1,
           "each member names its array as Parent");
    Expect(Entries[2].Type == EPropertyType::FixedString && Entries[2].Offset == offsetof(PropertyProbePair, Key)
           && Entries[3].Type == EPropertyType::UInt8 && Entries[4].Type == EPropertyType::Float
           && Entries[4].Offset == offsetof(PropertyProbePair, Values) + sizeof(float),
           "a member's offset is from the start of an element");
    Expect(std::strcmp(Entries[3].Category, "Pairs") == 0, "members keep the category");

    const ComponentPropertyEntry Second = Pairs.GetMember(2, Entries[4]);
    Expect(!Second.IsMember() && Second.Offset == offsetof(StructArrayProbeComponent, Pairs) + 2 * sizeof(PropertyProbePair)
                                                  + offsetof(PropertyProbePair, Values) + sizeof(float),
           "GetMember places a member of one element in the component");

    /* A tool writes through it. */
    World W;
    const Entity E = W.CreateEntity("StructArrayProbe");
    E.AddComponent<StructArrayProbeComponent>();
    auto* Bytes = static_cast<uint8_t*>(E.GetComponentByType(StructArrayProbeComponent::StaticType().ID));
    const float Written = 4.5f;
    std::memcpy(Bytes + Second.Offset, &Written, sizeof(Written));
    Expect(E.GetComponent<StructArrayProbeComponent>()->Pairs[2].Values[1] == 4.5f, "written by offset, read by the type");
}

static void TestComponentProperties()
{
    std::printf("[properties]\n");

    size_t Count = 0;
    const ComponentPropertyEntry* Entries = FindComponentProperties(PropertyProbeComponent::StaticType().ID, Count);
    Expect(Entries != nullptr && Count == 6, "six properties registered; the over-long name is rejected");
    if(Entries && Count == 6)
    {
        Expect(!Entries[0].HasCategory(), "a property before any Category has none");
        Expect(std::strcmp(Entries[1].Category, "Motion") == 0 && std::strcmp(Entries[2].Category, "Text") == 0,
               "Category files the properties after it, until the next");
        Expect(!Entries[3].HasCategory(), "Category(nullptr) ends the category");
        Expect(std::strcmp(Entries[4].Category, "Lists") == 0 && std::strcmp(Entries[4].GetElement(1).Category, "Lists") == 0,
               "an array, and each of its elements, keep the category");
        Expect(!Entries[5].HasCategory() && std::strcmp(Entries[5].Name, "Spare") == 0,
               "an over-long category is rejected, and the properties after it have none");

        Expect(std::strcmp(Entries[0].Name, "Speed") == 0, "registration order is kept");
        Expect(Entries[0].Type == EPropertyType::Float, "a float field is typed Float");
        Expect(Entries[0].Offset == offsetof(PropertyProbeComponent, Speed), "Speed's offset matches offsetof");
        Expect(!Entries[0].IsReadOnly(), "a field is writable by default");

        Expect(Entries[1].Type == EPropertyType::Flag && Entries[1].IsReadOnly(), "Flag with PF_ReadOnly");
        Expect(Entries[1].Offset == offsetof(PropertyProbeComponent, bArmed), "bArmed's offset matches offsetof");

        Expect(Entries[2].Type == EPropertyType::FixedString && Entries[2].Size == 16, "a char[16] is a 16-byte FixedString");
        Expect(!Entries[2].IsArray() && Entries[2].GetSpan() == 16, "a field is not an array; its span is its size");

        const ComponentPropertyEntry& Slots = Entries[4];
        Expect(Slots.IsArray() && Slots.ArrayCount == 4 && Slots.Type == EPropertyType::Int32 && Slots.Size == sizeof(int32_t)
               && Slots.Stride == sizeof(int32_t) && Slots.Offset == offsetof(PropertyProbeComponent, Slots),
               "Array registers one entry: element type, size and stride, the count, the first element's offset");
        Expect(Slots.GetSpan() == sizeof(int32_t) * 4, "an array's span is the whole array");
        const ComponentPropertyEntry Third = Slots.GetElement(2);
        Expect(!Third.IsArray() && Third.Offset == offsetof(PropertyProbeComponent, Slots) + 2 * sizeof(int32_t)
               && Third.Type == EPropertyType::Int32 && std::strcmp(Third.Name, "Slots") == 0,
               "GetElement is that element as a single value");
    }

    /* A tool writes through the offset alone, with no knowledge of the type. */
    World W;
    const Entity E = W.CreateEntity("PropertyProbe");
    E.AddComponent<PropertyProbeComponent>();
    if(Entries && Count >= 1)
    {
        auto* Bytes = static_cast<uint8_t*>(E.GetComponentByType(PropertyProbeComponent::StaticType().ID));
        const float NewSpeed = 9.0f;
        std::memcpy(Bytes + Entries[0].Offset, &NewSpeed, sizeof(NewSpeed));
        Expect(E.GetComponent<PropertyProbeComponent>()->Speed == 9.0f, "a write through the offset lands in the live component");
    }

    /* Built-ins, and what they deliberately leave out. */
    FindComponentProperties(TransformComponent::StaticType().ID, Count);
    Expect(Count == 3, "Transform exposes Position, Rotation, Scale");

    Entries = FindComponentProperties(BoundsComponent::StaticType().ID, Count);
    bool bProxyHidden = true, bWorldReadOnly = false;
    for(size_t Index = 0; Entries && Index < Count; ++Index)
    {
        if(Entries[Index].Offset == offsetof(BoundsComponent, ProxyID)) { bProxyHidden = false; }
        if(std::strcmp(Entries[Index].Name, "World") == 0) { bWorldReadOnly = Entries[Index].IsReadOnly(); }
    }
    Expect(bProxyHidden, "Bounds does not expose ProxyID");
    Expect(bWorldReadOnly, "Bounds' World box is read-only");

    /* Defaults are the default-constructed instance, never zeroes. */
    PropertyProbeComponent ProbeDefaults;
    ProbeDefaults.Speed = -1.0f;
    Expect(GetComponentPropertyDefaults(PropertyProbeComponent::StaticType().ID, &ProbeDefaults)
           && ProbeDefaults.Speed == 1.5f && std::strcmp(ProbeDefaults.Label, "probe") == 0,
           "GetComponentPropertyDefaults writes the default-constructed instance");

    BoundsComponent BoundsDefaults;
    BoundsDefaults.ProxyID = 7;
    Expect(GetComponentPropertyDefaults(BoundsComponent::StaticType().ID, &BoundsDefaults) && BoundsDefaults.ProxyID == -1,
           "Bounds defaults keep ProxyID at -1, not a zeroed 0");

    int32_t Untouched = 42;
    Expect(!GetComponentPropertyDefaults(TestOnlyComponent::StaticType().ID, &Untouched) && Untouched == 42,
           "a type with no property list has no defaults, and the buffer is left alone");

    Expect(IsComponentAddable(TransformComponent::StaticType().ID), "Transform is addable");
    Expect(!IsComponentAddable(TestOnlyComponent::StaticType().ID), "a type with no property list is not addable");

    Expect(FindComponentProperties(TestOnlyComponent::StaticType().ID, Count) == nullptr && Count == 0,
           "a component with no property list reports none");
}
#endif // WITH_EDITOR

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
// Entity members -- the name, stored on the entity, not in a pool
// ============================================================================

static void TestEntityMembers()
{
    std::printf("[entity members]\n");

    World W;
    const Entity A = W.CreateEntity("Alpha");
    const Entity B = W.CreateEntity();
    Expect(std::strcmp(A.GetName(), "Alpha") == 0, "CreateEntity(name) sets the name");
    Expect(B.GetName() != nullptr && B.GetName()[0] == '\0', "an unnamed entity reads \"\", never null");

    /* Must survive the slot table growing (members are in a deque). */
    const char* Held = A.GetName();
    for(int Index = 0; Index < 200; ++Index) { W.CreateEntity(); }
    Expect(Held == A.GetName() && std::strcmp(Held, "Alpha") == 0,
           "a GetName() pointer survives other entities being created");

    Expect(B.SetName("Beta"), "SetName on a live entity");
    Expect(W.FindEntityByName("Beta") == B.GetID(), "FindEntityByName sees a rename");
    Expect(!W.FindEntityByName("").IsSet(), "FindEntityByName(\"\") matches nothing, not every unnamed entity");
    Expect(B.SetName(nullptr) && B.GetName()[0] == '\0', "SetName(nullptr) clears");

    /* Destroying clears everything; the recycled slot starts clean. */
    W.DestroyEntity(A);
    Expect(!W.FindEntityByName("Alpha").IsSet(), "a destroyed entity is not found by name");
    Expect(A.GetName()[0] == '\0' && !A.SetName("Ghost"), "a dead handle has no members and takes none");
    const Entity Recycled = W.CreateEntity();
    Expect(Recycled.GetID().Index == A.GetID().Index, "(the slot is recycled)");
    Expect(Recycled.GetName()[0] == '\0', "the recycled slot has no name");

    /* GUIDs: set, unique, findable, kept only while the entity lives. */
    const EntityGuid RecycledGuid = Recycled.GetGuid();
    Expect(RecycledGuid.IsSet() && B.GetGuid().IsSet() && RecycledGuid != B.GetGuid(), "every entity gets its own GUID");
    Expect(RecycledGuid != A.GetGuid() && !A.GetGuid().IsSet(), "a recycled slot gets a new GUID; a dead handle reads none");
    Expect(W.FindEntityByGuid(B.GetGuid()) == B.GetID(), "FindEntityByGuid finds a live entity");

    const EntityGuid Chosen = EntityGuid::Generate();
    const Entity Placed = W.CreateEntity("Placed", Chosen);
    Expect(Placed.GetGuid() == Chosen, "CreateEntity(name, guid) keeps the GUID it is given");
    const Entity Clash = W.CreateEntity("Clash", Chosen);
    Expect(Clash.GetGuid().IsSet() && Clash.GetGuid() != Chosen, "a GUID already in use is replaced with a fresh one");
    W.DestroyEntity(Placed);
    Expect(!W.FindEntityByGuid(Chosen).IsSet(), "a destroyed entity's GUID is no longer found");

    char Text[EntityGuid::STRING_BUFFER];
    Chosen.ToString(Text);
    EntityGuid Parsed;
    Expect(std::strlen(Text) == 32 && EntityGuid::Parse(Text, Parsed) && Parsed == Chosen, "a GUID round-trips through its text");
    Expect(!EntityGuid::Parse("not-a-guid", Parsed) && !EntityGuid::Parse("0123", Parsed), "malformed GUID text is refused");
}

// ============================================================================
// Components + owner header
// ============================================================================

static void TestComponents()
{
    std::printf("[components]\n");

    World W;
    const Entity E1 = W.CreateEntity("Alice");
    const Entity E2 = W.CreateEntity("Bob");

    TransformComponent* T1 = E1.AddComponent<TransformComponent>();
    Expect(T1 != nullptr, "AddComponent succeeds");
    Expect(E1.HasComponent<TransformComponent>(), "HasComponent true after Add");

    T1->Position = Vector3(1.0, 2.0, 3.0);

    Expect(E2.GetComponent<TransformComponent>() == nullptr, "GetComponent on an entity without one is null");

    const EntityID Dead = EntityID(9999, 0);
    Expect(W.GetEntity(Dead).GetComponent<TransformComponent>() == nullptr, "GetComponent on a dead entity is null");

    TransformComponent* T2 = E2.AddComponent<TransformComponent>();
    T2->Position = Vector3(4.0, 5.0, 6.0);

    Expect(T1->Owner.OwningWorld == &W, "owner header's World is set");
    Expect(T1->Owner.Entity == E1, "owner header's Entity is set");
    Expect(std::strcmp(T1->Owner.GetName(), "Alice") == 0, "the owner header finds the right entity's name");
    Expect(std::strcmp(T2->Owner.GetName(), "Bob") == 0, "same, for the second entity");

    Expect(E1.RemoveComponent<TransformComponent>(), "RemoveComponent succeeds");
    Expect(!E1.HasComponent<TransformComponent>(), "HasComponent false after Remove");
    Expect(E2.GetComponent<TransformComponent>() != nullptr, "the survivor's component is intact after swap-remove");
    Expect(E2.GetComponent<TransformComponent>()->Position.GetX() == 4.0, "the survivor's DATA is intact after swap-remove");

    int VisitCount = 0;
    W.Each<TransformComponent>([&](Entity /*E*/, TransformComponent& /*T*/) { ++VisitCount; });
    Expect(VisitCount == 1, "Each visits exactly the live set");

    // The handle CreateEntity returns attaches components directly.
    const Entity Handle = W.CreateEntity("Carol");
    Expect(Handle.IsAlive() && Handle.GetWorld() == &W, "CreateEntity returns a live handle bound to this World");

    TransformComponent HandleInit;
    HandleInit.Position = Vector3(7.0, 8.0, 9.0);
    TransformComponent* Attached = Handle.AddComponent(HandleInit);
    Expect(Attached != nullptr && Attached->Position.GetX() == 7.0f, "Entity::AddComponent deduces T and copies the init values");
    Expect(Attached->Owner.Entity == Handle.GetID(), "the attached component's owner is the handle's entity");
    Expect(Handle.AddComponent<BoundsComponent>() != nullptr, "Entity::AddComponent<T>() with no init adds a default component");
    Expect(Handle.AddComponent(HandleInit) == nullptr, "a second component of the same type is still refused");

    const EntityID AsId = Handle;
    Expect(AsId == Handle.GetID() && W.IsAlive(Handle), "an Entity converts implicitly to its EntityID");

    Expect(Entity().AddComponent<TransformComponent>() == nullptr, "a default (unbound) handle adds nothing");
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
    const Entity E1 = W.CreateEntity();
    BoundsComponent* B1 = E1.AddComponent<BoundsComponent>();
    B1->Local = MakeBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);

    Expect(B1->ProxyID == -1, "a freshly-added Bounds has no proxy yet");
    W.RefreshSpatialIndex();
    Expect(E1.GetComponent<BoundsComponent>()->ProxyID >= 0, "the first refit inserts the leaf");

    // Add a Transform AFTER the Bounds -- the "may not have its Transform yet" case.
    const Entity E2 = W.CreateEntity();
    BoundsComponent* B2 = E2.AddComponent<BoundsComponent>();
    B2->Local = MakeBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);
    TransformComponent* T2 = E2.AddComponent<TransformComponent>();
    T2->Position = Vector3(100.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    Expect(E2.GetComponent<BoundsComponent>()->World.Min.GetX() > 99.0, "a Bounds added before its Transform is placed correctly once both exist");

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
    const Entity E3 = W.CreateEntity();
    BoundsComponent* B3 = E3.AddComponent<BoundsComponent>();
    B3->Local    = MakeBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5);
    B3->bStatic  = 1;
    TransformComponent* T3 = E3.AddComponent<TransformComponent>();
    T3->Position = Vector3(500.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    const AABB StaticWorldBoxBefore = E3.GetComponent<BoundsComponent>()->World;
    T3->Position = Vector3(-500.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    const AABB StaticWorldBoxAfter = E3.GetComponent<BoundsComponent>()->World;
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

// ============================================================================
// Component script bindings -- what a component chooses to expose to Lua.
// ============================================================================

/* One field of every supported shape, a read-only one, an unlisted one, and
 * custom methods. */
struct ScriptProbeComponent
{
    GAMECORE_COMPONENT_BODY()
    float               Speed  = 1.5f;
    int32_t             Count  = 3;
    uint8_t             Level  = 7;
    uint8_t             bArmed = 0;
    EngineCore::Color   Tint{10, 20, 30};
    char                Label[16] = "probe";
    EngineCore::Vector3 Offset{4.0, 5.0, 6.0};
    int32_t             Secret = 99;   // deliberately not bound
};
GAMECORE_DEFINE_COMPONENT(ScriptProbeComponent, "TestScriptProbe")
GAMECORE_DEFINE_COMPONENT_SCRIPT(ScriptProbeComponent)
{
    Bind.Field("Speed", &ScriptProbeComponent::Speed);
    Bind.Field("Count", &ScriptProbeComponent::Count);
    Bind.Field("Level", &ScriptProbeComponent::Level);
    Bind.Flag("Armed",  &ScriptProbeComponent::bArmed);
    Bind.Field("Tint",  &ScriptProbeComponent::Tint);
    Bind.Field("Label", &ScriptProbeComponent::Label);
    Bind.ReadOnly("Offset", &ScriptProbeComponent::Offset);

    Bind.Method("Scaled", [](ScriptProbeComponent& C, ScriptCall& Call)
    {
        Call.PushNumber(C.Speed * Call.GetNumber(1));
    });

    /* Several results, including an entity. */
    Bind.Method("Describe", [](ScriptProbeComponent& C, ScriptCall& Call)
    {
        Call.PushString(C.Label);
        Call.PushNumber(C.Count);
        Call.PushEntity(Call.GetOwner());
    });
}

/* One binding stored, one rejected: a name that would have to be truncated. */
struct ScriptRejectComponent
{
    GAMECORE_COMPONENT_BODY()
    float A = 0.0f;
};
GAMECORE_DEFINE_COMPONENT(ScriptRejectComponent, "TestScriptReject")
GAMECORE_DEFINE_COMPONENT_SCRIPT(ScriptRejectComponent)
{
    Bind.Field("A", &ScriptRejectComponent::A);
    Bind.Field("ThisBindingNameIsFarTooLongToStoreUnchanged", &ScriptRejectComponent::A);
}

static void TestComponentScripting()
{
    std::printf("Component script bindings\n");

    size_t RejectCount = 0;
    FindComponentScript(ScriptRejectComponent::StaticType().ID, RejectCount);
    Expect(RejectCount == 1, "an over-long binding name is rejected, not truncated");

    World W;
    ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "ScriptEngine::Initialize succeeds");
    Scripts.BindWorld(&W);

    const Entity Probe = W.CreateEntity("Probe");
    ScriptProbeComponent* C = Probe.AddComponent<ScriptProbeComponent>();
    Expect(C != nullptr, "the probe component is added");

    Expect(Scripts.RunString(
        "local c = World.FindEntity('Probe'):GetComponent('TestScriptProbe')\n"
        "assert(c:GetSpeed() == 1.5, 'float get')\n"
        "c:SetSpeed(4)\n"
        "assert(c:GetCount() == 3, 'int32 get')\n"
        "c:SetCount(12)\n"
        "assert(c:GetLevel() == 7, 'uint8 get')\n"
        "c:SetLevel(200)\n"
        "assert(c:GetArmed() == false, 'flag reads as a boolean')\n"
        "c:SetArmed(true)\n"
        "local r, g, b, a = c:GetTint()\n"
        "assert(r == 10 and g == 20 and b == 30 and a == 255, 'color get')\n"
        "c:SetTint(1, 2, 3)\n"
        "assert(c:GetLabel() == 'probe', 'fixed string get')\n"
        "c:SetLabel('renamed')\n"
        "local x, y, z = c:GetOffset()\n"
        "assert(x == 4 and y == 5 and z == 6, 'vector get')\n"
        "assert(c.SetOffset == nil, 'a read-only field has no setter')\n"
        "assert(c.GetSecret == nil and c.SetSecret == nil, 'an unlisted field is invisible')\n"
        "assert(c:Scaled(2) == 8, 'a custom method reads args and returns a value')\n"
        "local label, count, owner = c:Describe()\n"
        "assert(label == 'renamed' and count == 12 and owner:GetName() == 'Probe', 'several results')\n"
        "assert(c:GetEntity():GetName() == 'Probe', 'GetEntity on a bound component')\n",
        "test-component-bindings"), "fields and methods work from Lua");

    Expect(C->Speed == 4.0f && C->Count == 12 && C->Level == 200 && C->bArmed == 1,
           "scalar and flag writes from Lua reach the component");
    Expect(C->Tint.R == 1 && C->Tint.G == 2 && C->Tint.B == 3 && C->Tint.A == 255,
           "a color write with no alpha defaults it to opaque");
    Expect(std::strcmp(C->Label, "renamed") == 0, "a fixed-string write reaches the component");
    Expect(C->Secret == 99, "the unlisted field is untouched");

    // Errors are raised to Lua cleanly, with the method named, and nothing is half-applied.
    Expect(Scripts.RunString(
        "local c = World.FindEntity('Probe'):GetComponent('TestScriptProbe')\n"
        "local ok, err = pcall(function() c:Scaled('x') end)\n"
        "assert(not ok and string.find(err, \"bad argument #1 to 'Scaled'\", 1, true), tostring(err))\n"
        "ok = pcall(function() c:SetLevel(300) end)\n"
        "assert(not ok, 'a uint8 field rejects 300')\n"
        "assert(c:GetLevel() == 200, 'and keeps its value')\n",
        "test-component-binding-errors"), "binding errors surface as Lua errors");

    // The built-in components keep their Lua API on the new mechanism.
    Expect(Scripts.RunString(
        "local e = World.CreateEntity('Builtins')\n"
        "local t = e:AddComponent('Transform')\n"
        "t:SetPosition(1, 2, 3)\n"
        "t:Translate(1, 1, 1)\n"
        "local x, y, z = t:GetPosition()\n"
        "assert(x == 2 and y == 3 and z == 4, 'Set/GetPosition and Translate')\n"
        "assert(not pcall(function() t:Translate(1, 'nope', 1) end), 'a bad Translate errors')\n"
        "x = t:GetPosition()\n"
        "assert(x == 2, 'and moves nothing')\n"
        "t:SetScale(2, 2, 2)\n"
        "assert(t:GetScale() == 2, 'Set/GetScale')\n"
        "assert(e:GetComponent('Name') == nil, 'the name is not a component')\n"
        "assert(e:GetName() == 'Builtins', 'GetName')\n"
        "assert(e:SetName('Renamed') == true, 'SetName')\n"
        "assert(World.FindEntity('Renamed') ~= nil, 'FindEntity sees SetName')\n"
        "assert(#e:GetGuid() == 32 and e:GetGuid():match('^%x+$'), 'GetGuid is 32 hex digits')\n"
        "local b = e:AddComponent('Bounds')\n"
        "b:SetLocal(-1, -1, -1, 1, 1, 1)\n"
        "assert(b:GetLocal() == -1, 'Bounds:GetLocal')\n"
        "b:SetStatic(true)\n"
        "assert(b:GetStatic() == true, 'Bounds static flag')\n"
        "assert(b.SetWorld == nil, 'Bounds World is read-only')\n"
        "assert(b.GetProxyID == nil and b.SetProxyID == nil, 'ProxyID is not exposed')\n"
        "assert(select('#', b:GetWorld()) == 6, 'GetWorld returns six numbers')\n"
        "assert(t:GetEntity():GetName() == 'Renamed', 'Transform:GetEntity')\n"
        "local u = e:AddComponent('TestOnly')\n"
        "assert(u:GetEntity():GetName() == 'Renamed', 'an unbound component still has GetEntity')\n"
        "assert(u.GetValue == nil, 'and nothing else')\n",
        "test-builtin-bindings"), "the built-in component API is unchanged");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
}

/* Writes a script under the temp directory; returns a forward-slash path. */
static std::string WriteTestScript(const char* InName, const char* InSource)
{
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke";
    std::error_code Ignored;
    std::filesystem::create_directories(Dir, Ignored);
    const std::filesystem::path File = Dir / InName;
    std::ofstream(File, std::ios::binary | std::ios::trunc) << InSource;
    return File.generic_string();
}

namespace
{
    double GOrderProbeSawX = -1.0;

    /* Registered before BindWorld; must still see the same-phase script write. */
    void OrderProbeSystem(World& InWorld, float /*InDeltaSeconds*/, void* /*InUserData*/)
    {
        const EntityID Ordered = InWorld.FindEntityByName("Ordered");
        const TransformComponent* T = InWorld.GetEntity(Ordered).GetComponent<TransformComponent>();
        GOrderProbeSawX = T ? double(T->Position.GetX()) : -1.0;
    }
}

namespace
{
    /* An instance of a one-entity prefab named InName running InScript, with
     * a Transform if bInTransform. */
    PrefabInstance SpawnScripted(World& InWorld, const char* InName, const char* InScript, const bool bInTransform = false)
    {
        World Scratch;
        const Entity Root = Scratch.CreateEntity(InName);
        if(bInTransform) { Root.AddComponent<TransformComponent>(); }
        PrefabAsset Asset;
        CapturePrefab(Scratch, Root.GetID(), Asset);
        Asset.SetScriptPath(InScript);
        return InWorld.SpawnPrefab(Asset);
    }
}

static void TestScripting()
{
    std::printf("[scripting]\n");

    World W;
    W.RegisterSystem("Test.OrderProbe", TP_PrePhysics, &OrderProbeSystem);

    ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "ScriptEngine::Initialize succeeds");
    Scripts.BindWorld(&W);
    Expect(W.HasScriptDriver(), "BindWorld installs the World's script driver");

    Expect(Scripts.RunString(
        "local e = World.CreateEntity(\"Scripted\")\n"
        "local t = e:AddComponent(\"Transform\")\n"
        "t:SetPosition(1, 2, 3)\n",
        "test-basic"), "RunString executes a basic entity/component script");

    const EntityID Found = W.FindEntityByName("Scripted");
    Expect(Found.IsSet(), "an entity created from Lua is findable from C++");
    TransformComponent* T = W.GetEntity(Found).GetComponent<TransformComponent>();
    Expect(T != nullptr && T->Position.GetX() == 1.0, "a component set from Lua is readable from C++");

    // A script file drives a prefab instance through the World's script driver.
    const std::string CounterPath = WriteTestScript("Counter.lua",
        "CounterLoads = (CounterLoads or 0) + 1\n"
        "TickStarts = TickStarts or 0\n"
        "TickCalls = TickCalls or 0\n"
        "return {\n"
        "    OnStart = function(self) TickStarts = TickStarts + 1 end,\n"
        "    OnTick  = function(self, dt) TickCalls = TickCalls + 1 end,\n"
        "}\n");

    const PrefabInstance Counted = SpawnScripted(W, "Counted", CounterPath.c_str());
    const InstanceScriptState* CountedState = W.GetInstanceScriptState(Counted.GetID());
    Expect(CountedState && CountedState->bPendingInstantiate, "the script is pending until the next tick");

    W.Tick(0.016f);
    W.Tick(0.016f);
    W.Tick(0.016f);

    Expect(Scripts.RunString("assert(TickStarts == 1, 'expected OnStart once, got ' .. TickStarts)", "assert-starts"),
           "OnStart runs once");
    Expect(Scripts.RunString("assert(TickCalls == 3, 'expected OnTick 3 times, got ' .. TickCalls)", "assert-ticks"),
           "OnTick runs every tick");
    CountedState = W.GetInstanceScriptState(Counted.GetID());
    Expect(CountedState && !CountedState->bPendingInstantiate && CountedState->InstanceRef != -2,
           "the instance is live after the first tick");

    // A second instance on the same path shares the loaded class, not the instance.
    SpawnScripted(W, "CountedToo", CounterPath.c_str());
    W.Tick(0.016f);
    Expect(Scripts.RunString("assert(CounterLoads == 1, 'the file ran ' .. CounterLoads .. ' times')", "assert-cache"),
           "a script file is loaded once per bind, whatever uses it");
    Expect(Scripts.RunString("assert(TickStarts == 2, 'expected a second OnStart, got ' .. TickStarts)", "assert-second"),
           "each instance gets its own script instance");

    // Scripts tick before every registered system in their phase.
    const std::string OrderedPath = WriteTestScript("Ordered.lua",
        "return { OnTick = function(self, dt)\n"
        "    local t = self.Entity:GetComponent('Transform')\n"
        "    local x = t:GetPosition()\n"
        "    t:SetPosition(x + 1, 0, 0)\n"
        "end }\n");
    SpawnScripted(W, "Ordered", OrderedPath.c_str(), true);
    W.Tick(0.016f);
    Expect(GOrderProbeSawX == 1.0, "a script ticks before a system registered earlier in the same phase");

    // A script that errors logs one error and the loop stays alive.
    const std::string BrokenPath = WriteTestScript("Broken.lua",
        "return { OnTick = function(self, dt) error('boom') end }\n");
    uint32_t ErrorsBefore = Scripts.GetErrorCount();
    const PrefabInstance Broken = SpawnScripted(W, "Broken", BrokenPath.c_str());
    W.Tick(0.016f);
    W.Tick(0.016f);   // second tick must not re-raise -- bErrored gates it
    Expect(Scripts.GetErrorCount() == ErrorsBefore + 1, "exactly one error is logged, not one per tick");
    Expect(W.GetInstanceScriptState(Broken.GetID()) && W.GetInstanceScriptState(Broken.GetID())->bErrored,
           "the script is marked errored");

    // A missing file and a file that returns no class: one error per path, not per instance.
    const std::string NotAClassPath = WriteTestScript("NotAClass.lua", "return 42\n");
    const std::string MissingPath   = (std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "DoesNotExist.lua").generic_string();
    ErrorsBefore = Scripts.GetErrorCount();
    const PrefabInstance MissingA  = SpawnScripted(W, "MissingA", MissingPath.c_str());
    const PrefabInstance MissingB  = SpawnScripted(W, "MissingB", MissingPath.c_str());
    const PrefabInstance NotAClass = SpawnScripted(W, "NotAClass", NotAClassPath.c_str());
    W.Tick(0.016f);
    Expect(Scripts.GetErrorCount() == ErrorsBefore + 2, "a missing file and a non-table return log one error each");
    Expect(W.GetInstanceScriptState(MissingA.GetID())->bErrored && W.GetInstanceScriptState(MissingB.GetID())->bErrored
           && W.GetInstanceScriptState(NotAClass.GetID())->bErrored, "and every instance using them is marked errored");

    // OnDestroy runs and the instance ref is released.
    const std::string NotifyPath = WriteTestScript("Notify.lua",
        "return { OnDestroy = function(self) Destroyed = (Destroyed or 0) + 1 end }\n");
    const PrefabInstance Notify = SpawnScripted(W, "Notify", NotifyPath.c_str());
    W.Tick(0.016f);
    Notify.Destroy();
    Expect(Scripts.RunString("assert(Destroyed == 1, 'OnDestroy ran ' .. tostring(Destroyed) .. ' times')", "assert-destroyed"),
           "destroying the instance runs OnDestroy once");

    // A proxy held across a component removal errors cleanly rather than aliasing.
    Expect(Scripts.RunString(
        "local e = World.CreateEntity()\n"
        "local t = e:AddComponent(\"Transform\")\n"
        "e:RemoveComponent(\"Transform\")\n"
        "local ok, err = pcall(function() t:GetPosition() end)\n"
        "assert(ok == false, 'expected the stale proxy to error')\n",
        "test-stale-proxy"), "a proxy held across RemoveComponent errors instead of aliasing");

    // Unbind releases every instance but keeps the paths; a rebind restarts them.
    const std::string RebindPath = WriteTestScript("Rebind.lua",
        "return {\n"
        "    OnStart   = function(self) RebindStarts = (RebindStarts or 0) + 1 end,\n"
        "    OnDestroy = function(self) RebindDestroys = (RebindDestroys or 0) + 1 end,\n"
        "}\n");
    const PrefabInstance Rebound = SpawnScripted(W, "Rebound", RebindPath.c_str());
    W.Tick(0.016f);
    Scripts.UnbindWorld();
    Expect(!W.HasScriptDriver(), "UnbindWorld removes the driver");
    Expect(W.GetInstanceScriptState(Rebound.GetID()) && W.GetInstanceScriptState(Rebound.GetID())->bPendingInstantiate,
           "the script survives an unbind, pending again");
    Scripts.BindWorld(&W);
    W.Tick(0.016f);
    Expect(Scripts.RunString("assert(RebindStarts == 2 and RebindDestroys == 1, "
                             "'starts ' .. tostring(RebindStarts) .. ', destroys ' .. tostring(RebindDestroys))",
                             "assert-rebind"), "unbind runs OnDestroy; the next bind starts the script again");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
    Expect(!Scripts.IsInitialized(), "ScriptEngine reports not-initialized after Shutdown");
}

// ============================================================================
// Archive -- the byte layer, on its own, before anything built on it.
// ============================================================================

/* The archive has ONE operator (<<) for both directions, so a round trip reads
 * with the same operator it wrote with -- which is the whole design, and the
 * reason this test reads identically to the code that produced the bytes. */
static void TestArchiveRoundTrip()
{
    std::printf("Archive: round trip\n");

    uint32_t U32 = 0xDEADBEEFu;
    int32_t  I32 = -12345;
    float    F   = 3.5f;
    double   D   = -1.25;
    bool     B   = true;
    std::string S = "hello archive";
    char     Fixed[16] = "fixed";

    Archive Out = Archive::ForWriting();
    Out << U32 << I32 << F << D << B << S;
    Out.SerializeFixedString(Fixed, sizeof(Fixed));
    Expect(!Out.HasError(), "writing leaves the archive healthy");

    Archive In = Archive::ForReading(Out.GetBytes());

    uint32_t RU32 = 0;
    int32_t  RI32 = 0;
    float    RF   = 0.0f;
    double   RD   = 0.0;
    bool     RB   = false;
    std::string RS;
    char     RFixed[16] = {};

    In << RU32 << RI32 << RF << RD << RB << RS;
    In.SerializeFixedString(RFixed, sizeof(RFixed));

    Expect(!In.HasError(), "reading back leaves the archive healthy");
    Expect(RU32 == U32, "uint32 round-trips");
    Expect(RI32 == I32, "a NEGATIVE int32 round-trips");
    Expect(RF == F, "float round-trips exactly");
    Expect(RD == D, "double round-trips exactly");
    Expect(RB == B, "bool round-trips");
    Expect(RS == S, "std::string round-trips");
    Expect(std::strcmp(RFixed, "fixed") == 0, "a fixed char buffer round-trips");

    // Reading past the end must SET THE ERROR, not return garbage -- the whole
    // reason the flag is sticky.
    uint32_t Overrun = 0;
    In << Overrun;
    Expect(In.HasError(), "reading past the end sets the error flag");
}

static void TestArchiveChunks()
{
    std::printf("Archive: chunks and forward compatibility\n");

    const uint32_t TAG_AAAA = MakeArchiveTag('A','A','A','A');
    const uint32_t TAG_BBBB = MakeArchiveTag('B','B','B','B');

    Archive Out = Archive::ForWriting();
    {
        uint16_t Version = 3;
        Out.OpenChunk(TAG_AAAA, Version);
        uint32_t Payload = 111;
        Out << Payload;
        // A nested chunk, to prove the size back-patching survives nesting.
        uint16_t InnerVersion = 1;
        Out.OpenChunk(TAG_BBBB, InnerVersion);
        uint32_t Inner = 222;
        Out << Inner;
        Out.CloseChunk();
        Out.CloseChunk();
    }
    {
        uint16_t Version = 1;
        Out.OpenChunk(TAG_BBBB, Version);
        uint32_t Payload = 333;
        Out << Payload;
        Out.CloseChunk();
    }
    Expect(!Out.HasError(), "writing nested chunks leaves the archive healthy");

    // A reader that knows both chunks.
    {
        Archive In = Archive::ForReading(Out.GetBytes());

        uint16_t Version = 0;
        Expect(In.OpenChunk(TAG_AAAA, Version), "the first chunk opens");
        Expect(Version == 3, "the chunk's version comes back");
        uint32_t Payload = 0;
        In << Payload;
        Expect(Payload == 111, "the chunk's payload reads");

        uint16_t InnerVersion = 0;
        Expect(In.OpenChunk(TAG_BBBB, InnerVersion), "the nested chunk opens");
        uint32_t Inner = 0;
        In << Inner;
        Expect(Inner == 222, "the nested chunk's payload reads");
        In.CloseChunk();
        In.CloseChunk();

        Expect(In.OpenChunk(TAG_BBBB, Version), "the sibling chunk opens after the first closes");
        uint32_t Second = 0;
        In << Second;
        Expect(Second == 333, "the sibling chunk's payload reads");
        In.CloseChunk();
        Expect(In.IsAtEnd(), "the buffer is exhausted");
    }

    // THE FORWARD-COMPATIBILITY CLAIM, tested rather than asserted: a reader
    // that does not care about the first chunk skips it by size and still finds
    // the second one correctly. This is what lets an older build load a newer
    // build's save.
    {
        Archive In = Archive::ForReading(Out.GetBytes());

        uint16_t Version = 0;
        Expect(!In.OpenChunk(TAG_BBBB, Version),
               "probing for the wrong tag returns false WITHOUT consuming anything");
        Expect(In.PeekChunkTag() == TAG_AAAA, "the cursor did not move on a failed probe");

        // Open it, read NOTHING of its payload, and close it.
        Expect(In.OpenChunk(TAG_AAAA, Version), "the unwanted chunk still opens");
        In.CloseChunk();

        Expect(In.OpenChunk(TAG_BBBB, Version),
               "the NEXT chunk is found correctly after skipping an unread payload");
        uint32_t Second = 0;
        In << Second;
        Expect(Second == 333, "and its payload is intact");
        In.CloseChunk();
    }

    // A chunk whose size escapes the buffer is corruption and must be caught,
    // not trusted -- otherwise every following read is nonsense.
    {
        std::vector<uint8_t> Corrupt = Out.GetBytes();
        Expect(Corrupt.size() > 10, "there are bytes to corrupt");
        Corrupt[6] = 0xFF; Corrupt[7] = 0xFF; Corrupt[8] = 0xFF; Corrupt[9] = 0x7F;

        Archive In = Archive::ForReading(Corrupt);
        uint16_t Version = 0;
        In.OpenChunk(TAG_AAAA, Version);
        Expect(In.HasError(), "a chunk size past the end of the buffer is rejected");
    }
}

// ============================================================================
// WorldSerializer -- a whole scene out and back into a FRESH World.
// ============================================================================

/* A component holding an entity reference, to exercise IEntityRemapper -- the
 * built-in components have no such field, and the remap is the single most
 * load-bearing piece of the design (EntityID is a per-run slot handle). */
struct LinkComponent
{
    GAMECORE_COMPONENT_BODY()

    EntityID Target;
    int32_t  Payload = 0;
};
GAMECORE_DEFINE_COMPONENT(LinkComponent, "TestLink")
GAMECORE_DEFINE_COMPONENT_SERIALIZER(LinkComponent, 1)
{
    Ar.Entity("Target", C.Target);
    Ar.Field("Payload", C.Payload);
}

/* Registered as a type but given NO serializer, to prove an unserializable
 * component does not fail the save or the load. */
struct OpaqueComponent
{
    GAMECORE_COMPONENT_BODY()
    int32_t Whatever = 7;
};
GAMECORE_DEFINE_COMPONENT(OpaqueComponent, "TestOpaque")

static void TestWorldSerialization()
{
    std::printf("WorldSerializer: round trip into a fresh World\n");

    std::vector<uint8_t> Saved;
    EntityID             SavedAlice;
    EntityID             SavedBob;
    EntityGuid           SavedAliceGuid;

    {
        World W;

        const Entity Alice = W.CreateEntity("Alice");
        const Entity Bob   = W.CreateEntity("Bob");
        SavedAlice     = Alice;
        SavedBob       = Bob;
        SavedAliceGuid = Alice.GetGuid();

        TransformComponent AliceTransform;
        AliceTransform.Position = Vector3(1.5, 2.5, 3.5);
        AliceTransform.Scale    = Vector3(2.0, 2.0, 2.0);
        Alice.AddComponent(AliceTransform);

        BoundsComponent Bounds;
        Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
        Alice.AddComponent(Bounds);

        /* Bob points AT Alice, so the remap has something real to resolve. */
        LinkComponent Link;
        Link.Target  = Alice;
        Link.Payload = 42;
        Bob.AddComponent(Link);

        Bob.AddComponent<OpaqueComponent>();

        /* A destroyed entity must not appear in the save, and its hole in the
         * slot table must not break the dense index mapping. */
        const EntityID Doomed = W.CreateEntity("Doomed");
        W.DestroyEntity(Doomed);

        Archive Out = Archive::ForWriting();
        Expect(SerializeWorld(Out, W), "SerializeWorld succeeds when saving");
        Expect(!Out.HasError(), "the save archive is healthy");
        Saved = Out.GetBytes();
        Expect(!Saved.empty(), "the save produced bytes");
    }

    // A FRESH World -- the point of the test. Nothing from the old one survives.
    {
        World W;

        Archive In = Archive::ForReading(Saved);
        Expect(SerializeWorld(In, W), "SerializeWorld succeeds when loading");
        Expect(!In.HasError(), "the load archive is healthy");

        Expect(W.GetEntityCount() == 2, "exactly the two live entities came back (not the destroyed one)");

        const EntityID Alice = W.FindEntityByName("Alice");
        const EntityID Bob   = W.FindEntityByName("Bob");
        Expect(Alice.IsSet(), "Alice came back, by name");
        Expect(Bob.IsSet(),   "Bob came back, by name");
        Expect(!W.FindEntityByName("Doomed").IsSet(), "the destroyed entity did NOT come back");

        const Entity AliceHandle = W.GetEntity(Alice);
        Expect(AliceHandle.GetGuid() == SavedAliceGuid, "Alice kept her GUID through the binary round trip");

        TransformComponent* Transform = W.GetEntity(Alice).GetComponent<TransformComponent>();
        Expect(Transform != nullptr, "Alice's Transform came back");
        if(Transform)
        {
            Expect(Transform->Position.GetX() == 1.5f, "Position.X survived");
            Expect(Transform->Position.GetY() == 2.5f, "Position.Y survived");
            Expect(Transform->Position.GetZ() == 3.5f, "Position.Z survived");
            Expect(Transform->Scale.GetX()    == 2.0f, "Scale survived");
            Expect(Transform->Owner.OwningWorld == &W,
                   "the restored component's Owner points at the NEW World");
            Expect(Transform->Owner.Entity == Alice, "the restored component's Owner names the new entity");
        }

        /* THE REMAP UNDER TEST. The handles are new -- the file could not have
         * contained them -- and the cross-entity reference still resolves. */
        LinkComponent* Link = W.GetEntity(Bob).GetComponent<LinkComponent>();
        Expect(Link != nullptr, "Bob's Link came back");
        if(Link)
        {
            Expect(Link->Payload == 42, "the Link's scalar field survived");
            Expect(Link->Target == Alice, "the entity REFERENCE resolved to the new Alice");
            Expect(!(Link->Target == SavedAlice) || Alice == SavedAlice,
                   "the reference points at a live handle, not the saved bytes");
        }

        /* The type with no serializer: the save warned and skipped it, and the
         * load did not fail. Its absence is the correct outcome. */
        Expect(!W.GetEntity(Bob).HasComponent<OpaqueComponent>(),
               "a component type with no registered serializer is skipped, not faked");

        /* THE BOUNDS FIX-UP. ProxyID is deliberately not saved, so the loader's
         * RefreshSpatialIndex is what must have given this one its leaf. */
        BoundsComponent* Bounds = W.GetEntity(Alice).GetComponent<BoundsComponent>();
        Expect(Bounds != nullptr, "Alice's Bounds came back");
        if(Bounds)
        {
            Expect(Bounds->ProxyID >= 0, "the restored Bounds got a spatial leaf from the loader's refit");

            /* And the tree actually contains it -- a ProxyID alone could be a
             * number nobody inserted. */
            int Hits = 0;
            W.QueryBounds(AABB(Vector3(-10.0, -10.0, -10.0), Vector3(10.0, 10.0, 10.0)),
                          [](EntityID, void* InUserData) -> bool
                          {
                              ++(*static_cast<int*>(InUserData));
                              return true;
                          }, &Hits);
            Expect(Hits >= 1, "a spatial query finds the restored entity");
        }
    }
}

#if WITH_EDITOR
// ============================================================================
// WorldPackage -- the editor's text world: .nmworld + one .nmentity per entity
// ============================================================================

namespace
{
    std::string ReadFileText(const std::filesystem::path& InPath)
    {
        std::ifstream In(InPath, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }

    void WriteFileText(const std::filesystem::path& InPath, const std::string& InText)
    {
        std::ofstream(InPath, std::ios::binary | std::ios::trunc) << InText;
    }

    std::string GuidFileName(const EntityGuid& InGuid)
    {
        char Text[EntityGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return std::string(Text) + ".nmentity";
    }

    size_t CountEntityFiles(const std::filesystem::path& InFolder)
    {
        size_t Count = 0;
        for(const auto& Item : std::filesystem::directory_iterator(InFolder))
        {
            if(Item.path().extension() == ".nmentity") { ++Count; }
        }
        return Count;
    }
}

static void TestWorldPackage()
{
    std::printf("WorldPackage: text worlds\n");

    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "Package";
    std::error_code Ignored;
    std::filesystem::remove_all(Dir, Ignored);
    std::filesystem::create_directories(Dir, Ignored);

    const std::string WorldPath  = (Dir / "Level.nmworld").generic_string();
    const std::filesystem::path Folder = Dir / "Level";

    World Original;
    const Entity Alice = Original.CreateEntity("Alice");
    const Entity Bob   = Original.CreateEntity("Bob");
    const Entity Carol = Original.CreateEntity();

    TransformComponent AliceTransform;
    AliceTransform.Position = Vector3(1.5, -2.25, 0.1);   // 0.1 is not exact in binary: the text must still round-trip it
    AliceTransform.Rotation = Vector3(0.0, 45.0, 0.0);
    Alice.AddComponent(AliceTransform);
    BoundsComponent AliceBounds;
    AliceBounds.Local   = AABB(Vector3(-1.0, -2.0, -3.0), Vector3(1.0, 2.0, 3.0));
    AliceBounds.bStatic = 1;
    Alice.AddComponent(AliceBounds);

    LinkComponent Link;
    Link.Target  = Alice;
    Link.Payload = -7;
    Bob.AddComponent(Link);
    Bob.AddComponent<OpaqueComponent>();   // no serializer: skipped, not fatal

    Carol.AddComponent<TransformComponent>();

    // --- Save ---------------------------------------------------------------
    Expect(SaveWorldPackage(WorldPath.c_str(), Original), "SaveWorldPackage succeeds");
    Expect(DetectWorldFileFormat(WorldPath.c_str()) == EWorldFileFormat::Text, "a saved package reads as text");
    Expect(std::filesystem::is_directory(Folder), "the entities go in a sibling folder named after the world");
    Expect(CountEntityFiles(Folder) == 3, "one .nmentity per entity");
    Expect(std::filesystem::exists(Folder / GuidFileName(Alice.GetGuid())), "entity files are named by GUID");

    const std::string AliceText = ReadFileText(Folder / GuidFileName(Alice.GetGuid()));
    Expect(AliceText.find("Name=\"Alice\"") != std::string::npos
           && AliceText.find("<Component Type=\"Bounds\"") < AliceText.find("<Component Type=\"Transform\""),
           "an entity file is readable XML, components sorted by type name");

    // --- Load into a fresh World ------------------------------------------------
    {
        World Loaded;
        EWorldFileFormat Format = EWorldFileFormat::Unknown;
        Expect(LoadWorldFile(WorldPath.c_str(), Loaded, &Format) && Format == EWorldFileFormat::Text, "LoadWorldFile reads it back");
        Expect(Loaded.GetEntityCount() == 3, "every entity came back");

        const Entity A = Loaded.GetEntity(Loaded.FindEntityByGuid(Alice.GetGuid()));
        const Entity B = Loaded.GetEntity(Loaded.FindEntityByGuid(Bob.GetGuid()));
        const Entity C = Loaded.GetEntity(Loaded.FindEntityByGuid(Carol.GetGuid()));
        Expect(A.IsAlive() && B.IsAlive() && C.IsAlive(), "each entity kept its GUID");
        Expect(std::strcmp(A.GetName(), "Alice") == 0 && C.GetName()[0] == '\0', "names came back, unnamed stays unnamed");

        const TransformComponent* T = A.GetComponent<TransformComponent>();
        Expect(T && T->Position.GetX() == 1.5f && T->Position.GetY() == -2.25f && T->Position.GetZ() == 0.1f
               && T->Rotation.GetY() == 45.0f && T->Scale.GetX() == 1.0f, "float values round-trip exactly through text");
        const BoundsComponent* Bounds = A.GetComponent<BoundsComponent>();
        Expect(Bounds && Bounds->bStatic == 1 && Bounds->Local.Max.GetZ() == 3.0f && Bounds->ProxyID >= 0,
               "Bounds came back, and the load gave it a spatial leaf");

        const LinkComponent* L = B.GetComponent<LinkComponent>();
        Expect(L && L->Payload == -7 && L->Target == A.GetID(), "an entity reference resolves by GUID to the loaded entity");
        Expect(!B.HasComponent<OpaqueComponent>(), "a type with no serializer is not saved");

        /* Saving what was loaded gives the same bytes: deterministic output. */
        const std::string SecondPath = (Dir / "Second.nmworld").generic_string();
        Expect(SaveWorldPackage(SecondPath.c_str(), Loaded), "the loaded world saves");
        Expect(ReadFileText(Dir / "Second" / GuidFileName(Alice.GetGuid())) == AliceText,
               "a load/save round trip reproduces an entity file byte for byte");
    }

    // --- Resave: unchanged files untouched; a deleted entity's file removed ------
    const auto AliceStamp = std::filesystem::last_write_time(Folder / GuidFileName(Alice.GetGuid()));
    const EntityGuid BobGuid = Bob.GetGuid();
    Original.DestroyEntity(Bob);
    WriteFileText(Folder / "Notes.txt", "not an entity file");
    Expect(SaveWorldPackage(WorldPath.c_str(), Original), "a second save succeeds");
    Expect(!std::filesystem::exists(Folder / GuidFileName(BobGuid)), "a deleted entity's file is removed");
    Expect(std::filesystem::exists(Folder / "Notes.txt"), "a file that is not an entity file is left alone");
    Expect(std::filesystem::last_write_time(Folder / GuidFileName(Alice.GetGuid())) == AliceStamp,
           "an unchanged entity file is not rewritten");

    // --- Load: unlisted files are ignored; missing and unknown fields tolerated --
    {
        const EntityGuid Stray = EntityGuid::Generate();
        WriteFileText(Folder / GuidFileName(Stray),
                      "<?xml version=\"1.0\"?>\n<Entity Guid=\"x\" Name=\"Stray\"/>\n");

        /* Carol's Transform with Scale removed and a field no build knows. */
        const std::filesystem::path CarolFile = Folder / GuidFileName(Carol.GetGuid());
        char CarolGuid[EntityGuid::STRING_BUFFER];
        Carol.GetGuid().ToString(CarolGuid);
        WriteFileText(CarolFile,
                      std::string("<?xml version=\"1.0\"?>\n<Entity Guid=\"") + CarolGuid + "\" Name=\"Carol\">\n"
                      "  <Component Type=\"Transform\" Version=\"1\">\n"
                      "    <Position X=\"4\" Y=\"5\" Z=\"6\" />\n"
                      "    <Wobble Value=\"9\" />\n"
                      "  </Component>\n"
                      "</Entity>\n");

        World Loaded;
        Expect(LoadWorldFile(WorldPath.c_str(), Loaded), "a hand-edited package still loads");
        Expect(Loaded.GetEntityCount() == 2 && !Loaded.FindEntityByName("Stray").IsSet(),
               "an entity file the manifest does not list is ignored");
        const TransformComponent* T = Loaded.GetEntity(Loaded.FindEntityByName("Carol")).GetComponent<TransformComponent>();
        Expect(T && T->Position.GetZ() == 6.0f && T->Scale.GetX() == 1.0f,
               "a missing field keeps its default; an unknown one is skipped");

        Expect(SaveWorldPackage(WorldPath.c_str(), Original), "saving again succeeds");
        Expect(!std::filesystem::exists(Folder / GuidFileName(Stray)), "the next save removes the unlisted file");
    }

    // --- A failed write leaves the old world exactly as it was --------------------
    {
        const std::string ManifestBefore = ReadFileText(WorldPath);
        const std::filesystem::path AliceFile = Folder / GuidFileName(Alice.GetGuid());
        const std::string AliceBefore = ReadFileText(AliceFile);

        /* A directory where Alice's temporary would go makes that write fail. */
        std::filesystem::create_directories(Folder / (GuidFileName(Alice.GetGuid()) + ".tmp"), Ignored);
        Alice.SetName("Renamed");
        const Entity Extra = Original.CreateEntity("Extra");

        Expect(!SaveWorldPackage(WorldPath.c_str(), Original), "a save whose write fails reports failure");
        Expect(ReadFileText(WorldPath) == ManifestBefore && ReadFileText(AliceFile) == AliceBefore,
               "the manifest and entity files are untouched");
        Expect(!std::filesystem::exists(Folder / GuidFileName(Extra.GetGuid()))
               && !std::filesystem::exists(Folder / (GuidFileName(Extra.GetGuid()) + ".tmp")),
               "no new entity file or temporary is left behind");

        std::filesystem::remove(Folder / (GuidFileName(Alice.GetGuid()) + ".tmp"), Ignored);
        Expect(SaveWorldPackage(WorldPath.c_str(), Original), "once the obstacle is gone the save succeeds");
    }

    // --- Cook: text in, binary out, same world ---------------------------------------
    {
        const std::string CookedPath = (Dir / "Cooked" / "Level.nmworld").generic_string();
        Expect(CookWorldFile(WorldPath.c_str(), CookedPath.c_str()), "CookWorldFile succeeds");
        Expect(DetectWorldFileFormat(CookedPath.c_str()) == EWorldFileFormat::Binary, "the cooked file is binary");

        World Cooked;
        EWorldFileFormat Format = EWorldFileFormat::Unknown;
        Expect(LoadWorldFile(CookedPath.c_str(), Cooked, &Format) && Format == EWorldFileFormat::Binary,
               "LoadWorldFile reads the cooked world");
        Expect(Cooked.GetEntityCount() == Original.GetEntityCount(), "the cooked world has every entity");
        const Entity A = Cooked.GetEntity(Cooked.FindEntityByGuid(Alice.GetGuid()));
        const TransformComponent* T = A.GetComponent<TransformComponent>();
        Expect(std::strcmp(A.GetName(), "Renamed") == 0 && T && T->Position.GetZ() == 0.1f,
               "cooked values, names and GUIDs match the source");
    }

    Expect(DetectWorldFileFormat((Dir / "Missing.nmworld").generic_string().c_str()) == EWorldFileFormat::Unknown,
           "a missing file is Unknown");
    World Nothing;
    Expect(!LoadWorldFile((Dir / "Missing.nmworld").generic_string().c_str(), Nothing), "loading a missing file fails");
}
#endif // WITH_EDITOR

static void TestArchiveTextDump()
{
    std::printf("ArchiveTextDump\n");

    World W;
    W.CreateEntity("Dumped").AddComponent<TransformComponent>();

    Archive Out = Archive::ForWriting();
    Expect(SerializeWorld(Out, W), "a World saves for the dump");

    std::string Text;
    Expect(DumpArchiveToText(Out, Text), "DumpArchiveToText succeeds");
    Expect(Text.find("WRLD") != std::string::npos, "the dump names the WRLD chunk");
    Expect(Text.find("CMPT") != std::string::npos, "the dump names a CMPT chunk");

    /* An empty archive is a legal thing to dump, and a crash here would be in
     * the one tool a developer reaches for when a save is already broken. */
    Archive Empty = Archive::ForWriting();
    std::string EmptyText;
    Expect(DumpArchiveToText(Empty, EmptyText), "dumping an empty archive succeeds");
}

// ============================================================================
// Versioning -- one Serialize body reading both older and newer saves.
// ============================================================================

static void TestChunkVersion()
{
    std::printf("Archive: chunk versions\n");

    const uint32_t TAG_OUTER = MakeArchiveTag('O','U','T','R');
    const uint32_t TAG_INNER = MakeArchiveTag('I','N','N','R');

    Archive Out = Archive::ForWriting();
    Expect(Out.GetChunkVersion() == 0, "no chunk open: version 0");

    uint16_t OuterVersion = 5;
    Out.OpenChunk(TAG_OUTER, OuterVersion);
    Expect(Out.GetChunkVersion() == 5, "saving: the version being written");
    uint16_t InnerVersion = 2;
    Out.OpenChunk(TAG_INNER, InnerVersion);
    Expect(Out.GetChunkVersion() == 2, "saving: the innermost chunk's version");
    Out.CloseChunk();
    Expect(Out.GetChunkVersion() == 5, "saving: back to the parent's after close");
    Out.CloseChunk();

    Archive In = Archive::ForReading(Out.GetBytes());
    uint16_t Read = 0;
    In.OpenChunk(TAG_OUTER, Read);
    Expect(In.GetChunkVersion() == 5, "loading: the file's version");
    In.OpenChunk(TAG_INNER, Read);
    Expect(In.GetChunkVersion() == 2, "loading: the innermost chunk's version");
    In.CloseChunk();
    Expect(In.GetChunkVersion() == 5, "loading: back to the parent's after close");
    In.CloseChunk();
    Expect(In.GetChunkVersion() == 0, "loading: 0 once every chunk is closed");
}

/* Registered at version 2: B was added in v2, with a non-zero default so an old
 * save visibly leaves it at the default rather than at zero. */
struct VersionedComponent
{
    GAMECORE_COMPONENT_BODY()
    int32_t A = 0;
    int32_t B = 77;
};
GAMECORE_DEFINE_COMPONENT(VersionedComponent, "TestVersioned")
GAMECORE_DEFINE_COMPONENT_SERIALIZER(VersionedComponent, 2)
{
    Ar.Field("A", C.A);
    if(Ar.GetVersion() >= 2) { Ar.Field("B", C.B); }
}

/* A save as a build with a different VersionedComponent would write it: two
 * entities, each with A = 10+i and optionally B = 20+i and a trailing field
 * this build has never heard of. Hand-built, because this build's registry can
 * only ever write version 2 -- which is the point. Mirrors WorldSerializer's
 * format (DESIGN.md). */
static std::vector<uint8_t> BuildVersionedSave(const uint16_t InDataVersion, const bool bWriteB,
                                               const bool bWriteExtra)
{
    Archive Ar = Archive::ForWriting();

    uint16_t WorldVersion = 1;
    Ar.OpenChunk(MakeArchiveTag('W','R','L','D'), WorldVersion);
    uint32_t EntityCount = 2;
    Ar << EntityCount;
    for(uint32_t Index = 0; Index < EntityCount; ++Index)
    {
        uint32_t SavedIndex = Index;
        Ar << SavedIndex;

        uint64_t    GuidHi = 0;
        uint64_t    GuidLo = Index + 1;   // any distinct, set GUID
        std::string Name;
        Ar << GuidHi << GuidLo << Name;
    }
    for(uint32_t Index = 0; Index < EntityCount; ++Index)
    {
        uint32_t NoParent = 0xFFFFFFFFu;
        Ar << NoParent;
    }
    uint32_t NoInstances = 0;
    Ar << NoInstances;

    uint16_t ComponentVersion = InDataVersion;
    Ar.OpenChunk(MakeArchiveTag('C','M','P','T'), ComponentVersion);
    std::string TypeName = "TestVersioned";
    Ar << TypeName;
    uint32_t InstanceCount = 2;
    Ar << InstanceCount;

    for(uint32_t Index = 0; Index < InstanceCount; ++Index)
    {
        uint32_t Owner = Index;
        Ar << Owner;

        uint16_t DataVersion = InDataVersion;
        Ar.OpenChunk(MakeArchiveTag('D','A','T','A'), DataVersion);
        int32_t A = int32_t(10 + Index);
        Ar << A;
        if(bWriteB)
        {
            int32_t B = int32_t(20 + Index);
            Ar << B;
        }
        if(bWriteExtra)
        {
            uint64_t FutureField = 0xDEADBEEFCAFEull;
            Ar << FutureField;
        }
        Ar.CloseChunk();
    }

    Ar.CloseChunk();
    Ar.CloseChunk();
    return Ar.GetBytes();
}

/* Loads InBytes into a fresh World and returns each VersionedComponent's
 * (A, B), sorted by A so the order of the pool does not matter. */
static std::vector<std::pair<int32_t, int32_t>> LoadVersioned(const std::vector<uint8_t>& InBytes, bool& OutbOk)
{
    World W;
    Archive In = Archive::ForReading(InBytes);
    OutbOk = SerializeWorld(In, W) && !In.HasError();

    std::vector<std::pair<int32_t, int32_t>> Values;
    W.Each<VersionedComponent>([&Values](Entity, VersionedComponent& InComponent)
    {
        Values.emplace_back(InComponent.A, InComponent.B);
    });
    std::sort(Values.begin(), Values.end());
    return Values;
}

static void TestComponentVersioning()
{
    std::printf("WorldSerializer: component versioning\n");

    // Current version, both ways through SerializeWorld.
    {
        std::vector<uint8_t> Saved;
        {
            World W;
            VersionedComponent Init;
            Init.A = 1;
            Init.B = 2;
            W.CreateEntity().AddComponent(Init);

            Archive Out = Archive::ForWriting();
            Expect(SerializeWorld(Out, W), "a v2 component saves");
            Saved = Out.GetBytes();
        }

        bool bOk = false;
        const auto Values = LoadVersioned(Saved, bOk);
        Expect(bOk, "a v2 save loads");
        Expect(Values.size() == 1 && Values[0].first == 1 && Values[0].second == 2,
               "v2 round trip keeps the field added in v2");
    }

    // An OLDER save: v1 had no B. The v2 body must skip reading it.
    {
        bool bOk = false;
        const auto Values = LoadVersioned(BuildVersionedSave(1, false, false), bOk);
        Expect(bOk, "a v1 save loads into a v2 build");
        Expect(Values.size() == 2, "both v1 instances load");
        if(Values.size() == 2)
        {
            Expect(Values[0].first == 10 && Values[1].first == 11,
                   "v1: A reads correctly, and the second instance is not desynchronised");
            Expect(Values[0].second == 77 && Values[1].second == 77,
                   "v1: the field v1 lacked keeps its default, not zero");
        }
    }

    // A NEWER save: v3 appended a field this build does not know.
    {
        bool bOk = false;
        const auto Values = LoadVersioned(BuildVersionedSave(3, true, true), bOk);
        Expect(bOk, "a v3 save loads into a v2 build");
        Expect(Values.size() == 2, "both v3 instances load");
        if(Values.size() == 2)
        {
            Expect(Values[0] == std::make_pair(10, 20) && Values[1] == std::make_pair(11, 21),
                   "v3: known fields read, the unknown trailing field is skipped");
        }
    }
}

// ============================================================================
// TransformMath
// ============================================================================

namespace
{
    double RotationDistance(const Rotation3& InA, const Rotation3& InB)
    {
        double Worst = 0.0;
        for(int Row = 0; Row < 3; ++Row)
        {
            for(int Col = 0; Col < 3; ++Col) { Worst = std::max(Worst, std::abs(InA.M[Row][Col] - InB.M[Row][Col])); }
        }
        return Worst;
    }

    bool Near(const double InA, const double InB, const double InTolerance = 1e-3)
    {
        return std::abs(InA - InB) < InTolerance;
    }
}

static void TestTransformMath()
{
    std::printf("[transform math]\n");
    using namespace TransformMath;

    /* EulerToRotation is the rotation part of GetLocalMatrix. */
    TransformComponent T;
    T.Rotation = Vector3(20.0, -35.0, 110.0);
    const Matrix4x4 Local = T.GetLocalMatrix();
    const Rotation3 R     = EulerToRotation(T.Rotation);
    double Worst = 0.0;
    for(int Row = 0; Row < 3; ++Row)
    {
        for(int Col = 0; Col < 3; ++Col) { Worst = std::max(Worst, std::abs(double(Local.Data[Row][Col]) - R.M[Row][Col])); }
    }
    Expect(Worst < 1e-5, "EulerToRotation matches TransformComponent::GetLocalMatrix");

    /* 90 about Z turns +X into +Y (right-handed, column vectors). */
    const Vector3 X = AxisAngle(Vector3(0.0, 0.0, 1.0), 90.0) * Vector3(1.0, 0.0, 0.0);
    Expect(Near(X.GetX(), 0.0) && Near(X.GetY(), 1.0), "AxisAngle: 90 about Z maps X to Y");

    /* Round trips, generic and near the second solution. */
    const Vector3 Cases[] = { Vector3(0.0, 0.0, 0.0), Vector3(10.0, 20.0, 30.0), Vector3(-170.0, 60.0, 95.0),
                              Vector3(0.0, 0.0, 45.0), Vector3(33.0, -80.0, -120.0) };
    bool bAllRoundTrip = true;
    for(const Vector3& Case : Cases)
    {
        const Vector3 Back = RotationToEuler(EulerToRotation(Case), Case);
        bAllRoundTrip = bAllRoundTrip && Near(Back.GetX(), Case.GetX()) && Near(Back.GetY(), Case.GetY()) && Near(Back.GetZ(), Case.GetZ());
    }
    Expect(bAllRoundTrip, "RotationToEuler(EulerToRotation(e), e) == e");

    /* A different starting guess still gives the same rotation. */
    const Vector3 Other = RotationToEuler(EulerToRotation(Vector3(10.0, 20.0, 30.0)), Vector3(0.0, 0.0, 0.0));
    Expect(RotationDistance(EulerToRotation(Other), EulerToRotation(Vector3(10.0, 20.0, 30.0))) < 1e-5,
           "RotationToEuler is an equivalent rotation from any starting guess");

    /* Unwrapping: past 180 from 179 reads 181, not -179. */
    const Vector3 Past = RotationToEuler(AxisAngle(Vector3(0.0, 0.0, 1.0), 181.0), Vector3(0.0, 0.0, 179.0));
    Expect(Near(Past.GetZ(), 181.0) && Near(Past.GetX(), 0.0) && Near(Past.GetY(), 0.0), "RotationToEuler unwraps toward InNear");

    /* Gimbal lock: Y at 90 keeps X and puts the rest in Z. */
    const Vector3 Locked(25.0, 90.0, 40.0);
    const Vector3 LockedBack = RotationToEuler(EulerToRotation(Locked), Vector3(25.0, 90.0, 0.0));
    Expect(Near(LockedBack.GetX(), 25.0) && RotationDistance(EulerToRotation(LockedBack), EulerToRotation(Locked)) < 1e-5,
           "RotationToEuler at +90 gimbal lock keeps X and the rotation");
    const Vector3 LockedNeg(25.0, -90.0, 40.0);
    const Vector3 LockedNegBack = RotationToEuler(EulerToRotation(LockedNeg), Vector3(25.0, -90.0, 0.0));
    Expect(RotationDistance(EulerToRotation(LockedNegBack), EulerToRotation(LockedNeg)) < 1e-5,
           "RotationToEuler at -90 gimbal lock keeps the rotation");

    /* Composing a world-space turn onto an existing rotation. */
    const Rotation3 Composed = AxisAngle(Vector3(0.0, 0.0, 1.0), 30.0) * EulerToRotation(Vector3(0.0, 0.0, 15.0));
    const Vector3   Sum      = RotationToEuler(Composed, Vector3(0.0, 0.0, 15.0));
    Expect(Near(Sum.GetZ(), 45.0) && Near(Sum.GetX(), 0.0) && Near(Sum.GetY(), 0.0), "a Z turn composes onto a Z rotation");
}

static void TestRefreshEntityBounds()
{
    std::printf("[refresh entity bounds]\n");

    World W;
    const Entity E = W.CreateEntity("Static");
    TransformComponent* T = E.AddComponent<TransformComponent>();
    BoundsComponent*    B = E.AddComponent<BoundsComponent>();
    B->Local   = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
    B->bStatic = 1;
    W.RefreshSpatialIndex();

    T->Position = Vector3(100.0, 0.0, 0.0);
    W.RefreshSpatialIndex();
    Expect(B->World.Center().GetX() < 1.0, "RefreshSpatialIndex leaves a static Bounds where it was");

    W.RefreshEntityBounds(E.GetID());
    Expect(Near(B->World.Center().GetX(), 100.0), "RefreshEntityBounds refits a static Bounds");
}

// ============================================================================
// Hierarchy -- parents, children and world transforms
// ============================================================================

namespace
{
    bool NearVector(const Vector3& InA, const Vector3& InB, const double InTolerance = 1e-3)
    {
        return Near(InA.GetX(), InB.GetX(), InTolerance) && Near(InA.GetY(), InB.GetY(), InTolerance)
            && Near(InA.GetZ(), InB.GetZ(), InTolerance);
    }

    Entity MakePlaced(World& InWorld, const char* InName, const Vector3& InPosition,
                      const Vector3& InRotation = Vector3(0.0, 0.0, 0.0), const Vector3& InScale = Vector3(1.0, 1.0, 1.0))
    {
        const Entity E = InWorld.CreateEntity(InName);
        TransformComponent T;
        T.Position = InPosition;
        T.Rotation = InRotation;
        T.Scale    = InScale;
        E.AddComponent(T);
        return E;
    }

    std::vector<EntityID> ChildrenOf(World& InWorld, const Entity& InParent)
    {
        std::vector<EntityID> Out;
        for(EntityID Child = InParent.GetFirstChild(); Child.IsSet(); Child = InWorld.GetEntity(Child).GetNextSibling())
        {
            Out.push_back(Child);
        }
        return Out;
    }

    int GDestroyedCount = 0;
    void CountDestroyed(EntityID, void*) { ++GDestroyedCount; }
}

static void TestHierarchyLinks()
{
    std::printf("[hierarchy links]\n");

    World W;
    const Entity Root = W.CreateEntity("Root");
    const Entity A    = W.CreateEntity("A");
    const Entity B    = W.CreateEntity("B");
    const Entity C    = W.CreateEntity("C");

    Expect(!A.HasParent() && Root.GetChildCount() == 0, "a new entity is a root with no children");

    Expect(A.SetParent(Root) && B.SetParent(Root) && C.SetParent(Root), "SetParent links children");
    Expect(A.GetParent() == Root.GetID() && Root.GetChildCount() == 3, "parent and count are recorded");
    Expect((ChildrenOf(W, Root) == std::vector<EntityID>{ A, B, C }), "children walk in the order they were parented");

    Expect(B.ClearParent() && !B.HasParent(), "ClearParent detaches");
    Expect((ChildrenOf(W, Root) == std::vector<EntityID>{ A, C }), "a middle child unlinks cleanly");
    Expect(C.SetParent(A) && (ChildrenOf(W, Root) == std::vector<EntityID>{ A })
           && (ChildrenOf(W, A) == std::vector<EntityID>{ C }), "reparenting moves the child between lists");

    /* No cycles. */
    const int HandlerBefore = GHandlerCalls;
    Expect(!Root.SetParent(C), "an entity cannot be parented to its own descendant");
    Expect(!A.SetParent(A), "nor to itself");
    Expect(GHandlerCalls > HandlerBefore, "a refused cycle is warned about");
    Expect(!Root.HasParent(), "and the refused link changes nothing");

    const Entity Dead = W.CreateEntity("Dead");
    W.DestroyEntity(Dead);
    Expect(!A.SetParent(Dead), "a dead parent is refused");

    /* Destroying a parent destroys its subtree, outside a tick at once. */
    const DelegateHandle Counting = W.OnEntityDestroyed().Add(&CountDestroyed);
    GDestroyedCount = 0;
    W.DestroyEntity(Root);
    Expect(!Root.IsAlive() && !A.IsAlive() && !C.IsAlive(), "destroying a parent destroys its descendants");
    Expect(B.IsAlive(), "a detached former child survives");
    Expect(GDestroyedCount == 3, "each destroyed entity is announced once");

    /* Inside a tick, the subtree goes at the phase flush. */
    const Entity P2 = W.CreateEntity("P2");
    const Entity K2 = W.CreateEntity("K2");
    K2.SetParent(P2);
    struct Ctx { EntityID Target; bool bAliveDuringTick = false; } TickCtx{ P2 };
    W.RegisterSystem("DestroyParent", TP_PrePhysics, [](World& InWorld, float, void* InUserData)
    {
        Ctx& C2 = *static_cast<Ctx*>(InUserData);
        if(!InWorld.IsAlive(C2.Target)) { return; }
        const EntityID Child = InWorld.GetEntity(C2.Target).GetFirstChild();
        InWorld.DestroyEntity(C2.Target);
        C2.bAliveDuringTick = InWorld.IsAlive(Child);
    }, &TickCtx);
    W.TickPhase(TP_PrePhysics, 0.016f);
    Expect(TickCtx.bAliveDuringTick, "a child is still alive after its parent's deferred destroy is queued");
    Expect(!P2.IsAlive() && !K2.IsAlive(), "the flush destroys parent and child together");
    W.UnregisterAllSystems(&TickCtx);
    W.OnEntityDestroyed().Remove(Counting);
}

static void TestHierarchyTransforms()
{
    std::printf("[hierarchy transforms]\n");

    World W;
    const Entity Parent = MakePlaced(W, "Parent", Vector3(10.0, 0.0, 0.0), Vector3(0.0, 0.0, 90.0), Vector3(2.0, 2.0, 2.0));
    const Entity Child  = MakePlaced(W, "Child", Vector3(1.0, 0.0, 0.0));

    Expect(NearVector(Child.GetWorldPose().Position, Vector3(1.0, 0.0, 0.0)), "an unparented entity's world pose is its Transform");

    /* Without keeping the world transform the local values stay put. */
    Child.SetParent(Parent, false);
    const TransformPose World1 = Child.GetWorldPose();
    Expect(NearVector(Child.GetComponent<TransformComponent>()->Position, Vector3(1.0, 0.0, 0.0)), "SetParent(..., false) keeps the local Transform");
    Expect(NearVector(World1.Position, Vector3(10.0, 2.0, 0.0)), "the child is scaled, rotated and offset by its parent");
    Expect(Near(World1.Rotation.GetZ(), 90.0) && NearVector(World1.Scale, Vector3(2.0, 2.0, 2.0)), "rotation and scale compose");

    /* The matrix agrees with the pose. */
    const Vector4 Origin = Child.GetWorldMatrix() * Vector4(0.0, 0.0, 0.0, 1.0);   // w = 1: a point
    Expect(NearVector(Vector3(Origin.GetX(), Origin.GetY(), Origin.GetZ()), Vector3(10.0, 2.0, 0.0)),
           "GetWorldMatrix maps the local origin to the world position");

    /* Moving the parent carries the child. */
    Parent.GetComponent<TransformComponent>()->Position = Vector3(0.0, 0.0, 5.0);
    Expect(NearVector(Child.GetWorldPose().Position, Vector3(0.0, 2.0, 5.0)), "a moved parent carries its child");

    /* Keeping the world transform: detaching leaves it where it was. */
    const TransformPose Before = Child.GetWorldPose();
    Child.ClearParent(true);
    const TransformPose After = Child.GetWorldPose();
    Expect(NearVector(Before.Position, After.Position) && NearVector(Before.Rotation, After.Rotation)
           && NearVector(Before.Scale, After.Scale), "ClearParent(true) keeps the world pose");

    /* And attaching does too. */
    Child.SetParent(Parent, true);
    const TransformPose Reattached = Child.GetWorldPose();
    Expect(NearVector(Reattached.Position, After.Position) && NearVector(Reattached.Rotation, After.Rotation)
           && NearVector(Reattached.Scale, After.Scale), "SetParent(true) keeps the world pose");
    Expect(NearVector(Child.GetComponent<TransformComponent>()->Scale, Vector3(1.0, 1.0, 1.0)),
           "the local Transform is rewritten relative to the parent");

    /* SetWorldPose writes the local that lands there. */
    TransformPose Target;
    Target.Position = Vector3(-3.0, 4.0, 1.0);
    Target.Rotation = Vector3(0.0, 0.0, 30.0);
    Target.Scale    = Vector3(1.0, 1.0, 1.0);
    Expect(Child.SetWorldPose(Target), "SetWorldPose succeeds with a Transform");
    const TransformPose Landed = Child.GetWorldPose();
    Expect(NearVector(Landed.Position, Target.Position) && NearVector(Landed.Rotation, Target.Rotation)
           && NearVector(Landed.Scale, Target.Scale), "and the entity lands exactly there");

    /* A Transform-less link passes its parent through. */
    const Entity Group = W.CreateEntity("Group");
    Group.SetParent(Parent, false);
    const Entity Leaf = MakePlaced(W, "Leaf", Vector3(0.0, 0.0, 0.0));
    Leaf.SetParent(Group, false);
    Expect(NearVector(Leaf.GetWorldPose().Position, Parent.GetWorldPose().Position), "an entity without a Transform passes its parent's through");
    Expect(!Group.SetWorldPose(Target), "SetWorldPose needs a Transform");

    /* Bounds follow the world matrix, and RefreshEntityBounds the subtree. */
    BoundsComponent Bounds;
    Bounds.Local   = AABB(Vector3(-0.5, -0.5, -0.5), Vector3(0.5, 0.5, 0.5));
    Bounds.bStatic = 1;
    Leaf.AddComponent(Bounds);
    W.RefreshSpatialIndex();
    Expect(NearVector(Leaf.GetComponent<BoundsComponent>()->World.Center(), Parent.GetWorldPose().Position),
           "a child's Bounds is fitted in world space");
    Parent.GetComponent<TransformComponent>()->Position = Vector3(50.0, 0.0, 0.0);
    W.RefreshEntityBounds(Parent);
    Expect(NearVector(Leaf.GetComponent<BoundsComponent>()->World.Center(), Vector3(50.0, 0.0, 0.0)),
           "RefreshEntityBounds on a parent refits its descendants, static ones included");
}

static void TestHierarchyScripting()
{
    std::printf("[hierarchy scripting]\n");

    World W;
    ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "ScriptEngine::Initialize succeeds");
    Scripts.BindWorld(&W);

    Expect(Scripts.RunString(
        "local p = World.CreateEntity(\"LuaParent\")\n"
        "p:AddComponent(\"Transform\"):SetPosition(10, 0, 0)\n"
        "local a = World.CreateEntity(\"LuaA\")\n"
        "local b = World.CreateEntity(\"LuaB\")\n"
        "local ta = a:AddComponent(\"Transform\")\n"
        "ta:SetPosition(1, 0, 0)\n"
        "assert(not a:GetParent():IsValid(), 'no parent yet')\n"
        "assert(a:SetParent(p, false), 'SetParent')\n"
        "assert(b:SetParent(p), 'SetParent, keeping the world pose')\n"
        "assert(a:GetParent():GetName() == 'LuaParent', 'GetParent')\n"
        "assert(p:GetChildCount() == 2, 'GetChildCount')\n"
        "assert(p:GetChild(2):GetName() == 'LuaB' and not p:GetChild(3):IsValid(), 'GetChild')\n"
        "local kids = p:GetChildren()\n"
        "assert(#kids == 2 and kids[1]:GetName() == 'LuaA', 'GetChildren')\n"
        "local x = ta:GetWorldPosition()\n"
        "assert(math.abs(x - 11) < 1e-4, 'GetWorldPosition composes the parent')\n"
        "ta:SetWorldPosition(20, 0, 0)\n"
        "local lx = ta:GetPosition()\n"
        "assert(math.abs(lx - 10) < 1e-4, 'SetWorldPosition writes the local')\n"
        "assert(not p:SetParent(a), 'a cycle is refused')\n"
        "assert(a:SetParent(nil), 'nil detaches')\n"
        "assert(p:GetChildCount() == 1, 'and the count drops')\n",
        "test-hierarchy"), "Lua: SetParent, GetParent, children and world positions");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
}

static void TestHierarchySerialization()
{
    std::printf("[hierarchy serialization]\n");

    std::vector<uint8_t> Saved;
    EntityGuid ParentGuid, ChildGuid, GrandGuid;
    {
        World W;
        /* The child is created FIRST, so its parent comes later in the save. */
        const Entity Child  = MakePlaced(W, "Child", Vector3(1.0, 2.0, 3.0));
        const Entity Parent = MakePlaced(W, "Parent", Vector3(10.0, 0.0, 0.0), Vector3(0.0, 0.0, 90.0));
        const Entity Grand  = MakePlaced(W, "Grand", Vector3(0.0, 1.0, 0.0));
        Child.SetParent(Parent, false);
        Grand.SetParent(Child, false);
        ParentGuid = Parent.GetGuid();
        ChildGuid  = Child.GetGuid();
        GrandGuid  = Grand.GetGuid();

        Archive Out = Archive::ForWriting();
        Expect(SerializeWorld(Out, W), "a parented world saves");
        Saved = Out.GetBytes();
    }
    {
        World W;
        Archive In = Archive::ForReading(Saved);
        Expect(SerializeWorld(In, W), "and loads");
        const Entity Parent = W.GetEntity(W.FindEntityByGuid(ParentGuid));
        const Entity Child  = W.GetEntity(W.FindEntityByGuid(ChildGuid));
        const Entity Grand  = W.GetEntity(W.FindEntityByGuid(GrandGuid));
        Expect(Child.GetParent() == Parent.GetID() && Grand.GetParent() == Child.GetID() && !Parent.HasParent(),
               "binary: parents come back, including one saved after its child");
        Expect(NearVector(Child.GetComponent<TransformComponent>()->Position, Vector3(1.0, 2.0, 3.0)),
               "binary: local Transforms are not rewritten by the load");
        Expect(NearVector(Grand.GetWorldPose().Position, Vector3(7.0, 1.0, 3.0)), "binary: the world pose is the saved one");
    }

#if WITH_EDITOR
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "Hierarchy";
    std::error_code Ignored;
    std::filesystem::remove_all(Dir, Ignored);
    std::filesystem::create_directories(Dir, Ignored);
    const std::string WorldPath = (Dir / "Level.nmworld").generic_string();
    {
        World W;
        const Entity Child  = MakePlaced(W, "Child", Vector3(1.0, 2.0, 3.0));
        const Entity Parent = MakePlaced(W, "Parent", Vector3(10.0, 0.0, 0.0), Vector3(0.0, 0.0, 90.0));
        Child.SetParent(Parent, false);
        ParentGuid = Parent.GetGuid();
        ChildGuid  = Child.GetGuid();
        Expect(SaveWorldPackage(WorldPath.c_str(), W), "a parented world saves as text");

        char ParentText[EntityGuid::STRING_BUFFER];
        ParentGuid.ToString(ParentText);
        const std::string ChildFile  = ReadFileText(Dir / "Level" / GuidFileName(ChildGuid));
        const std::string ParentFile = ReadFileText(Dir / "Level" / GuidFileName(ParentGuid));
        Expect(ChildFile.find(std::string("Parent=\"") + ParentText + "\"") != std::string::npos,
               "text: the child's file names its parent by GUID");
        Expect(ParentFile.find("Parent=") == std::string::npos, "text: a root's file has no Parent attribute");
    }
    {
        World W;
        Expect(LoadWorldFile(WorldPath.c_str(), W), "and loads as text");
        const Entity Parent = W.GetEntity(W.FindEntityByGuid(ParentGuid));
        const Entity Child  = W.GetEntity(W.FindEntityByGuid(ChildGuid));
        Expect(Child.GetParent() == Parent.GetID(), "text: the parent comes back");
        Expect(NearVector(Child.GetWorldPose().Position, Vector3(8.0, 1.0, 3.0)), "text: the world pose is the saved one");
    }
#endif
}

// ============================================================================
// Prefabs -- the asset, capture, spawning, instances and both file formats
// ============================================================================

namespace
{
    /* Root (Transform away from the origin, a Link to its grandchild),
     * Mesh under it, Tip under Mesh (a Link back to Root and a Link to an
     * entity outside the subtree). Returns the root. */
    Entity BuildPrefabSource(World& InWorld, EntityID& OutOutside)
    {
        const Entity Outside = InWorld.CreateEntity("Outside");
        OutOutside = Outside;

        const Entity Root = MakePlaced(InWorld, "Root", Vector3(100.0, 0.0, 0.0), Vector3(0.0, 0.0, 30.0));
        const Entity Mesh = MakePlaced(InWorld, "Mesh", Vector3(0.0, 10.0, 0.0));
        const Entity Tip  = MakePlaced(InWorld, "Tip",  Vector3(1.0, 0.0, 0.0));
        Mesh.SetParent(Root, false);
        Tip.SetParent(Mesh, false);

        LinkComponent ToTip;
        ToTip.Target  = Tip;
        ToTip.Payload = 7;
        Root.AddComponent(ToTip);

        LinkComponent ToRoot;
        ToRoot.Target  = Root;
        ToRoot.Payload = 9;
        Tip.AddComponent(ToRoot);
        Mesh.AddComponent<OpaqueComponent>();   // no serializer: left out, warned

        BoundsComponent Bounds;
        Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
        Mesh.AddComponent(Bounds);
        return Root;
    }

    int GSpawnedCount   = 0;
    int GUnspawnedCount = 0;
    void CountSpawned(PrefabInstanceID, void*)   { ++GSpawnedCount; }
    void CountUnspawned(PrefabInstanceID, void*) { ++GUnspawnedCount; }
}

static void TestPrefabCaptureAndSpawn()
{
    std::printf("[prefab capture and spawn]\n");

    PrefabAsset Asset;
    Asset.SetName("Turret");
    Asset.SetScriptPath("Scripts/Turret.lua");
    Asset.SetTickMode(EPrefabTickMode::EventsOnly);
    {
        World Source;
        EntityID Outside;
        const Entity Root = BuildPrefabSource(Source, Outside);
        const int HandlerBefore = GHandlerCalls;
        Expect(CapturePrefab(Source, Root, Asset), "CapturePrefab succeeds");
        Expect(GHandlerCalls > HandlerBefore, "a component without a serializer is warned about");
        Expect(!CapturePrefab(Source, EntityID(), Asset), "capturing a dead root fails");
    }

    Expect(Asset.GetEntityCount() == 3, "the whole subtree is captured, and nothing else");
    Expect(std::strcmp(Asset.GetEntityName(0), "Root") == 0 && Asset.GetEntityParentId(0) == PREFAB_NO_LOCAL_ID,
           "the root comes first, with no parent");
    Expect(Asset.GetEntityParentId(1) == Asset.GetEntityLocalId(0) && Asset.GetEntityParentId(2) == Asset.GetEntityLocalId(1),
           "parents come before children");
    Expect(Asset.GetNextLocalId() == 4, "ids 1..3 are used; the next is 4");
    Expect(Asset.FindLocalId("Tip") == Asset.GetEntityLocalId(2) && Asset.FindLocalId("Nope") == PREFAB_NO_LOCAL_ID,
           "FindLocalId looks up a template by name");
    Expect(std::strcmp(Asset.GetName(), "Turret") == 0 && std::strcmp(Asset.GetScriptPath(), "Scripts/Turret.lua") == 0
           && Asset.GetTickMode() == EPrefabTickMode::EventsOnly, "capture keeps the name, script and tick mode");
    Expect(Asset.GetEntityComponentCount(1) == 2, "Mesh keeps Bounds and Transform, not the unserializable one");
    Expect(std::strcmp(Asset.GetEntityComponentType(1, 0), "Bounds") == 0, "components are sorted by type name");

    /* Copies share; the first change clones. */
    PrefabAsset Copy = Asset;
    Copy.SetName("Changed");
    Expect(std::strcmp(Asset.GetName(), "Turret") == 0 && std::strcmp(Copy.GetName(), "Changed") == 0,
           "changing a copy leaves the original alone");

    World W;
    const DelegateHandle Spawned   = W.OnInstanceSpawned().Add(&CountSpawned);
    const DelegateHandle Unspawned = W.OnInstanceDestroyed().Add(&CountUnspawned);
    GSpawnedCount = GUnspawnedCount = 0;

    TransformPose At;
    At.Position = Vector3(5.0, 0.0, 0.0);
    const PrefabInstance Instance = W.SpawnPrefab(Asset, At);
    Expect(Instance.IsAlive() && W.GetInstanceCount() == 1 && GSpawnedCount == 1, "SpawnPrefab makes a live instance, announced once");
    Expect(W.GetEntityCount() == 3, "one entity per template");
    Expect(std::strcmp(Instance.GetName(), "Turret") == 0 && Instance.GetTickMode() == EPrefabTickMode::EventsOnly,
           "the instance takes the prefab's name and tick mode");

    const Entity Root = Instance.GetRoot();
    const Entity Mesh = Instance.FindEntity("Mesh");
    const Entity Tip  = Instance.FindEntity("Tip");
    Expect(Root.IsAlive() && Mesh.IsAlive() && Tip.IsAlive(), "FindEntity finds each template's entity by name");
    Expect(Root.GetInstance() == Instance.GetID() && Tip.GetPrefabLocalId() == Asset.FindLocalId("Tip"),
           "each entity knows its instance and its template");
    Expect(Mesh.GetParent() == Root.GetID() && Tip.GetParent() == Mesh.GetID(), "the tree is rebuilt");

    const TransformComponent* RootTransform = Root.GetComponent<TransformComponent>();
    Expect(RootTransform && NearVector(RootTransform->Position, Vector3(5.0, 0.0, 0.0))
           && NearVector(RootTransform->Rotation, Vector3(0.0, 0.0, 0.0)),
           "the root stands at the placement: the captured root Transform is identity");
    Expect(NearVector(Tip.GetWorldPose().Position, Vector3(6.0, 10.0, 0.0)), "children sit relative to the placed root");

    const LinkComponent* RootLink = Root.GetComponent<LinkComponent>();
    const LinkComponent* TipLink  = Tip.GetComponent<LinkComponent>();
    Expect(RootLink && RootLink->Target == Tip.GetID() && RootLink->Payload == 7, "a reference down the tree points at the new entity");
    Expect(TipLink && TipLink->Target == Root.GetID() && TipLink->Payload == 9, "and one up the tree too");
    Expect(Mesh.GetComponent<BoundsComponent>() && Mesh.GetComponent<BoundsComponent>()->ProxyID >= 0,
           "Bounds are fitted at spawn");

    /* GUIDs are derived from the instance's, so they repeat. */
    Expect(Root.GetGuid() == DerivePrefabEntityGuid(Instance.GetGuid(), Root.GetPrefabLocalId()), "entity GUIDs derive from the instance's");
    {
        World Other;
        const PrefabInstance Again = Other.SpawnPrefab(Asset, TransformPose(), nullptr, Instance.GetGuid());
        Expect(Again.FindEntity("Tip").GetGuid() == Tip.GetGuid(), "the same instance GUID spawns the same entity GUIDs");
    }
    Expect(W.FindInstanceByGuid(Instance.GetGuid()) == Instance.GetID(), "FindInstanceByGuid");

    /* Membership: entities stay inside their instance; the root may attach. */
    const int HandlerBefore = GHandlerCalls;
    Expect(!Tip.ClearParent(), "an instance's entity cannot be detached from it");
    const Entity Loose = W.CreateEntity("Loose");
    Expect(!Tip.SetParent(Loose), "nor moved under a loose entity");
    Expect(GHandlerCalls > HandlerBefore, "the refusal is warned about");
    Expect(Tip.SetParent(Root), "it can move within the instance");
    Expect(Root.SetParent(Loose), "the root can be attached to anything");
    Expect(Root.ClearParent(), "and detached again");

    /* A second instance, then destroying the first. */
    const PrefabInstance Second = W.SpawnPrefab(Asset, TransformPose(), "Second");
    Expect(W.GetInstanceCount() == 2 && std::strcmp(Second.GetName(), "Second") == 0, "a second instance, with its own name");
    Expect(Second.FindEntity("Tip") != Tip, "with its own entities");

    W.DestroyEntity(Tip);
    Expect(!Instance.FindEntity("Tip").IsValid() || !Instance.FindEntity("Tip").IsAlive(), "a destroyed member is no longer found");

    Instance.Destroy();
    Expect(!Instance.IsAlive() && !Root.IsAlive() && !Mesh.IsAlive(), "destroying an instance destroys its entities");
    Expect(GUnspawnedCount == 1 && W.GetInstanceCount() == 1, "announced once; the other instance is untouched");
    Expect(W.GetEntityCount() == 4, "the second instance's three, and the loose one");

    /* Destroying the root is destroying the instance -- deferred in a tick. */
    struct Ctx { EntityID Root; bool bAliveDuringTick = false; PrefabInstanceID Instance; } TickCtx{ Second.GetRoot(), false, Second };
    W.RegisterSystem("DestroyRoot", TP_PrePhysics, [](World& InWorld, float, void* InUserData)
    {
        Ctx& C = *static_cast<Ctx*>(InUserData);
        InWorld.DestroyEntity(C.Root);
        C.bAliveDuringTick = InWorld.IsInstanceAlive(C.Instance);
    }, &TickCtx);
    W.TickPhase(TP_PrePhysics, 0.016f);
    W.UnregisterAllSystems(&TickCtx);
    Expect(TickCtx.bAliveDuringTick && !Second.IsAlive() && W.GetInstanceCount() == 0,
           "destroying the root in a tick ends the instance at the flush");

    /* An empty prefab spawns nothing. */
    Expect(!W.SpawnPrefab(PrefabAsset()).IsValid(), "an empty prefab spawns nothing");

    W.OnInstanceSpawned().Remove(Spawned);
    W.OnInstanceDestroyed().Remove(Unspawned);
}

static void TestPrefabRecapture()
{
    std::printf("[prefab recapture]\n");

    PrefabAsset Asset;
    {
        World Source;
        EntityID Outside;
        CapturePrefab(Source, BuildPrefabSource(Source, Outside), Asset);
    }

    /* The editor's loop: spawn, edit, capture back. Ids survive. */
    World W;
    const PrefabInstance Instance = W.SpawnPrefab(Asset);
    const Entity Mesh = Instance.FindEntity("Mesh");
    Mesh.SetName("Barrel");
    const Entity Added = MakePlaced(W, "Muzzle", Vector3(0.0, 2.0, 0.0));
    Added.SetParent(Mesh, false);

    PrefabAsset Edited = Asset;
    Expect(CapturePrefab(W, Instance.GetRoot(), Edited), "capturing an instance back");
    Expect(Edited.GetEntityCount() == 4, "the new entity is in it");
    Expect(Edited.FindLocalId("Barrel") == Asset.FindLocalId("Mesh"), "a renamed entity keeps its id");
    Expect(Edited.FindLocalId("Muzzle") == 4 && Edited.GetNextLocalId() == 5, "a new entity gets the next id");
    Expect(Asset.GetEntityCount() == 3, "the asset it was copied from is unchanged");

    /* Without adopting, a second capture gives the new entity yet another id. */
    PrefabAsset Again = Edited;
    CapturePrefab(W, Instance.GetRoot(), Again);
    Expect(Again.FindLocalId("Muzzle") == 5, "unadopted, a new entity is renumbered by each capture");

    /* Adopting: it joins the instance with the id it got, and keeps it. */
    PrefabAsset Adopted = Edited;
    Expect(CapturePrefab(W, Instance.GetRoot(), Adopted, true), "capturing with adoption");
    const uint32_t MuzzleId = Adopted.FindLocalId("Muzzle");
    Expect(Added.GetInstance() == Instance.GetID() && Added.GetPrefabLocalId() == MuzzleId, "the new entity joined the instance with its id");
    PrefabAsset Stable = Adopted;
    CapturePrefab(W, Instance.GetRoot(), Stable, true);
    Expect(Stable.FindLocalId("Muzzle") == MuzzleId && Stable.GetNextLocalId() == Adopted.GetNextLocalId(),
           "after adoption, recapturing keeps every id");
    Expect(Instance.FindEntity(MuzzleId).GetID() == Added.GetID(), "the instance finds it by its id");

    /* AdoptIntoInstance's refusals. */
    const Entity Loose = MakePlaced(W, "Loose", Vector3());
    Expect(!W.AdoptIntoInstance(Instance.GetID(), Loose.GetID(), 99), "an entity outside the root's subtree is refused");
    Loose.SetParent(Instance.GetRoot(), false);
    Expect(!W.AdoptIntoInstance(Instance.GetID(), Loose.GetID(), MuzzleId), "a taken id is refused");
    Expect(!W.AdoptIntoInstance(Instance.GetID(), Loose.GetID(), PREFAB_NO_LOCAL_ID), "an unset id is refused");
    Expect(!W.AdoptIntoInstance(Instance.GetID(), Added.GetID(), 99), "a member is refused");
    Expect(W.AdoptIntoInstance(Instance.GetID(), Loose.GetID(), 99), "a loose child of the root joins");
}

static void TestPrefabFiles()
{
    std::printf("[prefab files]\n");

    PrefabAsset Asset;
    Asset.SetName("Turret");
    Asset.SetScriptPath("Scripts/Turret.lua");
    {
        World Source;
        EntityID Outside;
        CapturePrefab(Source, BuildPrefabSource(Source, Outside), Asset);
    }

    /* Binary, in memory. */
    {
        Archive Out = Archive::ForWriting();
        Expect(SerializePrefab(Out, Asset) && !Out.HasError(), "SerializePrefab saves");
        PrefabAsset Back;
        Archive In = Archive::ForReading(Out.GetBytes());
        Expect(SerializePrefab(In, Back) && !In.HasError(), "and loads");
        Expect(Back.GetEntityCount() == 3 && std::strcmp(Back.GetName(), "Turret") == 0
               && std::strcmp(Back.GetScriptPath(), "Scripts/Turret.lua") == 0 && Back.GetNextLocalId() == 4,
               "binary: the tree, name, script and next id round-trip");

        World W;
        const PrefabInstance Spawned = W.SpawnPrefab(Back);
        const LinkComponent* Link = Spawned.FindEntity("Tip").GetComponent<LinkComponent>();
        Expect(Link && Link->Target == Spawned.GetRoot().GetID(), "binary: references still resolve at spawn");
    }

    /* A truncated archive is refused. */
    {
        Archive Out = Archive::ForWriting();
        SerializePrefab(Out, Asset);
        std::vector<uint8_t> Bytes = Out.GetBytes();
        Bytes.resize(Bytes.size() / 2);
        PrefabAsset Broken;
        Archive In = Archive::ForReading(Bytes);
        Expect(!SerializePrefab(In, Broken) && Broken.IsEmpty(), "a truncated prefab is refused, the target untouched");
    }

#if WITH_EDITOR
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "Prefabs";
    std::error_code Ignored;
    std::filesystem::remove_all(Dir, Ignored);
    std::filesystem::create_directories(Dir, Ignored);
    const std::string TextPath   = (Dir / "Turret.nmprefab").generic_string();
    const std::string CookedPath = (Dir / "Cooked" / "Turret.nmprefab").generic_string();

    Expect(SavePrefabFile(TextPath.c_str(), Asset), "SavePrefabFile writes text");
    Expect(DetectWorldFileFormat(TextPath.c_str()) == EWorldFileFormat::Text, "a saved prefab reads as text");
    const std::string Text = ReadFileText(TextPath);
    Expect(Text.find("<Prefab Format=\"1\" Name=\"Turret\" NextId=\"4\">") != std::string::npos
           && Text.find("<Script Path=\"Scripts/Turret.lua\"") != std::string::npos
           && Text.find("<Tick Mode=\"Tick\"") != std::string::npos, "the header is readable XML");
    Expect(Text.find("<Entity Id=\"3\" Name=\"Tip\" Parent=\"2\">") != std::string::npos, "entities name their parent by id");
    Expect(Text.find("<Target Entity=\"1\"") != std::string::npos, "references are written as local ids");

    PrefabAsset Loaded;
    Expect(LoadPrefabFile(TextPath.c_str(), Loaded) && Loaded.GetEntityCount() == 3, "LoadPrefabFile reads text");
    {
        World W;
        const PrefabInstance Spawned = W.SpawnPrefab(Loaded);
        const LinkComponent* Link = Spawned.GetRoot().GetComponent<LinkComponent>();
        Expect(Link && Link->Target == Spawned.FindEntity("Tip").GetID() && Link->Payload == 7, "text: references resolve");
        Expect(NearVector(Spawned.FindEntity("Mesh").GetComponent<TransformComponent>()->Position, Vector3(0.0, 10.0, 0.0)),
               "text: values round-trip");
    }

    /* Deterministic: saving what was loaded writes the same bytes. */
    const auto Stamp = std::filesystem::last_write_time(TextPath);
    Expect(SavePrefabFile(TextPath.c_str(), Loaded) && ReadFileText(TextPath) == Text, "a re-save is byte-identical");
    Expect(std::filesystem::last_write_time(TextPath) == Stamp, "and an unchanged file is not rewritten");

    Expect(CookPrefabFile(TextPath.c_str(), CookedPath.c_str()), "CookPrefabFile writes binary");
    PrefabAsset Cooked;
    Expect(DetectWorldFileFormat(CookedPath.c_str()) == EWorldFileFormat::Binary
           && LoadPrefabFile(CookedPath.c_str(), Cooked) && Cooked.GetEntityCount() == 3, "and the cooked file loads");

    /* Hand-written files: out of tree order is fixed; two roots are refused. */
    const std::string Shuffled = (Dir / "Shuffled.nmprefab").generic_string();
    WriteFileText(Shuffled,
        "<Prefab Format=\"1\" Name=\"Shuffled\">\n"
        "  <Entity Id=\"2\" Name=\"Child\" Parent=\"1\" />\n"
        "  <Entity Id=\"1\" Name=\"Root\" />\n"
        "</Prefab>\n");
    PrefabAsset Fixed;
    Expect(LoadPrefabFile(Shuffled.c_str(), Fixed) && Fixed.GetEntityLocalId(0) == 1 && Fixed.GetNextLocalId() == 3,
           "a child listed before its parent is put in tree order");

    const std::string TwoRoots = (Dir / "TwoRoots.nmprefab").generic_string();
    WriteFileText(TwoRoots,
        "<Prefab Format=\"1\">\n"
        "  <Entity Id=\"1\" Name=\"A\" />\n"
        "  <Entity Id=\"2\" Name=\"B\" />\n"
        "</Prefab>\n");
    PrefabAsset Refused;
    Expect(!LoadPrefabFile(TwoRoots.c_str(), Refused), "two roots are refused");

    /* The registry. */
    ClearPrefabCache();
    const PrefabAsset* First = FindOrLoadPrefab(TextPath.c_str());
    Expect(First && First == FindOrLoadPrefab(TextPath.c_str()), "FindOrLoadPrefab loads a path once");
    Expect(std::strcmp(First->GetName(), "Turret") == 0, "the loaded prefab");
    Expect(!FindOrLoadPrefab((Dir / "Missing.nmprefab").generic_string().c_str()), "a missing file is nullptr");
    {
        World W;
        const PrefabInstance Spawned = W.SpawnPrefab(TextPath.c_str());
        Expect(Spawned.IsAlive() && std::strcmp(Spawned.GetPrefabPath(), TextPath.c_str()) == 0, "SpawnPrefab by path records it");

        PrefabAsset Renamed = *First;
        Renamed.SetName("Renamed");
        SavePrefabFile(TextPath.c_str(), Renamed);
        Expect(ReloadPrefab(TextPath.c_str()) && FindOrLoadPrefab(TextPath.c_str()) == First
               && std::strcmp(First->GetName(), "Renamed") == 0, "ReloadPrefab updates the entry in place");
        Expect(std::strcmp(Spawned.GetPrefab()->GetName(), "Turret") == 0, "a live instance keeps the version it was spawned from");
    }

    /* From Lua. */
    {
        World W;
        ScriptEngine Scripts;
        Scripts.Initialize();
        Scripts.BindWorld(&W);
        const std::string Source =
            "local root = World.SpawnPrefab('" + TextPath + "', 3, 4, 0)\n"
            "assert(root:IsValid(), 'SpawnPrefab returns the root')\n"
            "local x, y = root:GetComponent('Transform'):GetPosition()\n"
            "assert(x == 3 and y == 4, 'placed where asked')\n"
            "assert(root:GetChildCount() == 1, 'with its children')\n"
            "assert(not World.SpawnPrefab('Missing.nmprefab'):IsValid(), 'a missing prefab gives an invalid Entity')\n";
        Expect(Scripts.RunString(Source.c_str(), "test-spawn-prefab"), "Lua: World.SpawnPrefab");
        Expect(W.GetInstanceCount() == 1, "Lua spawned one instance");
        Scripts.UnbindWorld();
        Scripts.Shutdown();
    }
    ClearPrefabCache();
#endif
}

// ============================================================================
// Prefab instance scripts -- per-phase functions, tick modes, overlaps
// ============================================================================

namespace
{
    /* A one-entity prefab (or more, through InSetup) driven by InScriptPath. */
    PrefabAsset MakeScriptedPrefab(const std::string& InScriptPath, const EPrefabTickMode InMode,
                                   void (*InSetup)(World&, const Entity&) = nullptr)
    {
        World Source;
        const Entity Root = MakePlaced(Source, "Root", Vector3(0.0, 0.0, 0.0));
        if(InSetup) { InSetup(Source, Root); }
        PrefabAsset Asset;
        Asset.SetName("Scripted");
        CapturePrefab(Source, Root, Asset);
        Asset.SetScriptPath(InScriptPath.c_str());
        Asset.SetTickMode(InMode);
        return Asset;
    }

    /* Runs InCheck (Lua asserts) and reports it as one check. */
    void ExpectLua(ScriptEngine& InScripts, const char* InCheck, const char* What)
    {
        Expect(InScripts.RunString(InCheck, What), What);
    }

    void AddCollider(World& InWorld, const Entity& InParent, const char* InName)
    {
        const Entity Collider = MakePlaced(InWorld, InName, Vector3(0.0, 0.0, 0.0));
        Collider.SetParent(InParent, false);
        BoundsComponent Bounds;
        Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
        Collider.AddComponent(Bounds);
        Collider.AddComponent<CollisionComponent>();
    }
}

static void TestInstanceScripts()
{
    std::printf("[instance scripts]\n");

    const std::string PhasesPath = WriteTestScript("InstancePhases.lua",
        "P = P or { Start = 0, Pre = 0, Post = 0, Destroy = 0 }\n"
        "return {\n"
        "    OnStart = function(self)\n"
        "        P.Start = P.Start + 1\n"
        "        P.RootName = self.Root:GetName()\n"
        "        P.SameRoot = self.Entity:GetID() == self.Root:GetID()\n"
        "        P.InstanceName = self.Instance:GetName()\n"
        "        P.Mode = self.Instance:GetTickMode()\n"
        "    end,\n"
        "    OnPrePhysics = function(self, dt) P.Pre = P.Pre + 1; P.Dt = dt end,\n"
        "    OnPostUpdate = function(self, dt) P.Post = P.Post + 1 end,\n"
        "    OnDestroy = function(self) P.Destroy = P.Destroy + 1 end,\n"
        "}\n");

    World W;
    ScriptEngine Scripts;
    Scripts.Initialize();
    Scripts.BindWorld(&W);

    const PrefabInstance Instance = W.SpawnPrefab(MakeScriptedPrefab(PhasesPath, EPrefabTickMode::Tick), TransformPose(), "Turret");
    Expect(W.GetInstanceScriptState(Instance) && W.GetInstanceScriptState(Instance)->bPendingInstantiate,
           "a spawned instance's script waits for the next phase");

    W.Tick(0.25f);
    W.Tick(0.25f);
    ExpectLua(Scripts, "assert(P.Start == 1 and P.Pre == 2 and P.Post == 2 and P.Dt == 0.25)",
              "Lua: OnStart once, each defined phase function every tick, with dt");
    ExpectLua(Scripts, "assert(P.RootName == 'Root' and P.SameRoot and P.InstanceName == 'Turret' and P.Mode == 'Tick')",
              "Lua: self.Root / self.Entity are the root, self.Instance the instance");
    Expect(W.GetTickingInstanceCount(TP_PrePhysics) == 1 && W.GetTickingInstanceCount(TP_PostUpdate) == 1
           && W.GetTickingInstanceCount(TP_Physics) == 0 && W.GetTickingInstanceCount(TP_PostPhysics) == 0,
           "an instance is listed only in the phases it defines");

    /* A phase switched off. */
    Expect(Instance.SetPhaseEnabled(TP_PostUpdate, false) && !Instance.IsPhaseEnabled(TP_PostUpdate), "SetPhaseEnabled");
    W.Tick(0.25f);
    ExpectLua(Scripts, "assert(P.Pre == 3 and P.Post == 2)", "Lua: a disabled phase's function does not run");
    Instance.SetPhaseEnabled(TP_PostUpdate, true);

    /* EventsOnly: the script stays; no phase function runs. */
    Instance.SetTickMode(EPrefabTickMode::EventsOnly);
    Expect(W.GetTickingInstanceCount(TP_PrePhysics) == 0, "EventsOnly takes it off every phase list");
    W.Tick(0.25f);
    ExpectLua(Scripts, "assert(P.Pre == 3 and P.Start == 1 and P.Destroy == 0)", "Lua: EventsOnly keeps the script, runs no phase");

    /* Off releases it; leaving Off starts it again. */
    Instance.SetTickMode(EPrefabTickMode::Off);
    ExpectLua(Scripts, "assert(P.Destroy == 1)", "Lua: Off releases the script (OnDestroy)");
    Expect(W.GetInstanceScriptState(Instance) && !W.GetInstanceScriptState(Instance)->bStarted, "and its state is reset");
    Instance.SetTickMode(EPrefabTickMode::Tick);
    W.Tick(0.25f);
    ExpectLua(Scripts, "assert(P.Start == 2 and P.Pre == 4)", "Lua: back to Tick starts it afresh");

    /* Destroying the instance releases it. */
    Instance.Destroy();
    ExpectLua(Scripts, "assert(P.Destroy == 2)", "Lua: destroying the instance calls OnDestroy");
    Expect(W.GetTickingInstanceCount(TP_PrePhysics) == 0, "and takes it off the lists");

    /* Off at spawn: no script at all, and its Bounds become static. */
    const PrefabInstance Static = W.SpawnPrefab(MakeScriptedPrefab(PhasesPath, EPrefabTickMode::Off,
        [](World&, const Entity& InRoot)
        {
            BoundsComponent Bounds;
            Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
            InRoot.AddComponent(Bounds);
        }));
    W.Tick(0.25f);
    ExpectLua(Scripts, "assert(P.Start == 2)", "Lua: an Off prefab never starts its script");
    Expect(Static.GetRoot().GetComponent<BoundsComponent>()->bStatic == 1, "an Off prefab's Bounds are static");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
}

static void TestInstanceScriptDetails()
{
    std::printf("[instance script details]\n");

    World W;
    ScriptEngine Scripts;
    Scripts.Initialize();
    Scripts.BindWorld(&W);

    /* OnTick + TickPhase still works, standing for that phase. */
    const std::string LegacyPath = WriteTestScript("InstanceLegacy.lua",
        "L = L or 0\n"
        "return { TickPhase = TickPhase.PostPhysics, OnTick = function(self, dt) L = L + 1 end }\n");
    W.SpawnPrefab(MakeScriptedPrefab(LegacyPath, EPrefabTickMode::Tick));
    W.Tick(0.1f);
    Expect(W.GetTickingInstanceCount(TP_PostPhysics) == 1 && W.GetTickingInstanceCount(TP_PrePhysics) == 0,
           "OnTick files the instance under its class's TickPhase");
    ExpectLua(Scripts, "assert(L == 1)", "Lua: OnTick runs once a tick");

    /* A mode change from inside a tick waits for the flush. */
    const std::string SelfPausePath = WriteTestScript("InstancePause.lua",
        "S = S or 0\n"
        "return {\n"
        "    OnPrePhysics = function(self, dt)\n"
        "        S = S + 1\n"
        "        self.Instance:SetPhaseEnabled(TickPhase.PrePhysics, false)\n"
        "    end,\n"
        "    OnPhysics = function(self, dt) S = S + 10 end,\n"
        "}\n");
    const PrefabInstance Pausing = W.SpawnPrefab(MakeScriptedPrefab(SelfPausePath, EPrefabTickMode::Tick));
    W.Tick(0.1f);
    W.Tick(0.1f);
    ExpectLua(Scripts, "assert(S == 21)", "Lua: SetPhaseEnabled from inside a tick applies at the flush");
    Expect(!Pausing.IsPhaseEnabled(TP_PrePhysics) && Pausing.IsPhaseEnabled(TP_Physics), "and only to that phase");

    /* A raising phase function is reported once and never called again. */
    const std::string BrokenPath = WriteTestScript("InstanceBroken.lua",
        "B = B or 0\n"
        "return { OnPrePhysics = function(self, dt) B = B + 1; error('boom') end }\n");
    const PrefabInstance Broken = W.SpawnPrefab(MakeScriptedPrefab(BrokenPath, EPrefabTickMode::Tick));
    const int HandlerBefore = GHandlerCalls;
    W.Tick(0.1f);
    W.Tick(0.1f);
    ExpectLua(Scripts, "assert(B == 1)", "Lua: a raising instance script is not called again");
    Expect(GHandlerCalls > HandlerBefore && W.GetInstanceScriptState(Broken)->bErrored, "and is reported and marked errored");

    /* Overlaps: the instance hears about each of its colliders, as "mine",
     * but not about two of its own touching. */
    const std::string SensorPath = WriteTestScript("InstanceSensor.lua",
        "O = O or {}\n"
        "return {\n"
        "    OnOverlapBegin = function(self, other, mine) O[#O + 1] = mine:GetName() .. '>' .. other:GetName() end,\n"
        "}\n");
    const PrefabInstance Sensor = W.SpawnPrefab(MakeScriptedPrefab(SensorPath, EPrefabTickMode::EventsOnly,
        [](World& InWorld, const Entity& InRoot)
        {
            AddCollider(InWorld, InRoot, "Sensor");
            AddCollider(InWorld, InRoot, "Body");
        }), TransformPose(), nullptr);
    const Entity Intruder = MakePlaced(W, "Intruder", Vector3(0.0, 0.0, 0.0));
    {
        BoundsComponent Bounds;
        Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
        Intruder.AddComponent(Bounds);
        Intruder.AddComponent<CollisionComponent>();
    }
    W.Tick(0.1f);
    ExpectLua(Scripts,
        "table.sort(O)\n"
        "assert(#O == 2, 'two reports, got ' .. #O)\n"
        "assert(O[1] == 'Body>Intruder' and O[2] == 'Sensor>Intruder', O[1] .. ' / ' .. tostring(O[2]))\n",
        "Lua: OnOverlapBegin(self, other, mine) for each collider, not between the instance's own");
    (void)Sensor;

    /* Unbinding releases instance scripts; binding again starts them afresh. */
    const std::string CountPath = WriteTestScript("InstanceRebind.lua",
        "R = R or { Start = 0, Destroy = 0 }\n"
        "return { OnStart = function(self) R.Start = R.Start + 1 end, OnDestroy = function(self) R.Destroy = R.Destroy + 1 end }\n");
    W.SpawnPrefab(MakeScriptedPrefab(CountPath, EPrefabTickMode::EventsOnly));
    W.Tick(0.1f);
    Scripts.UnbindWorld();
    Scripts.BindWorld(&W);   // the same lua_State: globals survive
    ExpectLua(Scripts, "assert(R.Start == 1 and R.Destroy == 1)", "Lua: UnbindWorld releases instance scripts");
    W.Tick(0.1f);
    ExpectLua(Scripts, "assert(R.Start == 2)", "Lua: the next bind starts them afresh");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
}

// ============================================================================
// Prefab instances in world files
// ============================================================================

namespace
{
    /* Root, Barrel (a Link to Root), Muzzle (Bounds) -- saved as InPath. */
    PrefabAsset WriteTurretPrefab(const std::string& InPath)
    {
        World Source;
        const Entity Root   = MakePlaced(Source, "Root", Vector3(0.0, 0.0, 0.0));
        const Entity Barrel = MakePlaced(Source, "Barrel", Vector3(0.0, 5.0, 0.0));
        const Entity Muzzle = MakePlaced(Source, "Muzzle", Vector3(0.0, 2.0, 0.0));
        Barrel.SetParent(Root, false);
        Muzzle.SetParent(Barrel, false);
        LinkComponent Link;
        Link.Target  = Root;
        Link.Payload = 3;
        Barrel.AddComponent(Link);
        BoundsComponent Bounds;
        Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
        Muzzle.AddComponent(Bounds);

        PrefabAsset Asset;
        Asset.SetName("Turret");
        CapturePrefab(Source, Root, Asset);
#if WITH_EDITOR
        SavePrefabFile(InPath.c_str(), Asset);
#else
        (void)InPath;
#endif
        return Asset;
    }

    struct TurretLevel
    {
        PrefabInstance Edited, Untouched, Lost, InMemory;
        EntityID       Ground, Flag;
    };

    TurretLevel BuildTurretLevel(World& W, const std::string& InPrefabPath, const PrefabAsset& InAsset)
    {
        TurretLevel L;
        const Entity Ground = MakePlaced(W, "Ground", Vector3(0.0, 0.0, 0.0));
        L.Ground = Ground;

        TransformPose At;
        At.Position = Vector3(100.0, 0.0, 0.0);
        L.Edited    = W.SpawnPrefab(InPrefabPath.c_str(), At, "TurretA");
        L.Untouched = W.SpawnPrefab(InPrefabPath.c_str(), TransformPose(), "TurretB");
        L.Lost      = W.SpawnPrefab(InPrefabPath.c_str(), TransformPose(), "TurretC");
        L.InMemory  = W.SpawnPrefab(InAsset, TransformPose(), "Loose");

        const Entity Barrel = L.Edited.FindEntity("Barrel");
        const Entity Muzzle = L.Edited.FindEntity("Muzzle");
        Barrel.SetName("Gun");
        Barrel.GetComponent<TransformComponent>()->Position = Vector3(0.0, 7.0, 0.0);
        Barrel.GetComponent<LinkComponent>()->Target = Ground;
        Muzzle.RemoveComponent<BoundsComponent>();
        Muzzle.AddComponent<CollisionComponent>();
        L.Edited.SetTickMode(EPrefabTickMode::EventsOnly);
        L.Edited.SetPhaseEnabled(TP_PostUpdate, false);
        L.Edited.GetRoot().SetParent(Ground, false);

        const Entity Flag = MakePlaced(W, "Flag", Vector3(0.0, 1.0, 0.0));
        Flag.SetParent(Barrel, false);
        L.Flag = Flag;

        L.Lost.FindEntity("Muzzle").Destroy();
        return L;
    }
}

static void TestPrefabWorldBinary()
{
    std::printf("[prefab instances: binary worlds]\n");

    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "PrefabWorlds";
    std::error_code Ignored;
    std::filesystem::create_directories(Dir, Ignored);
    const std::string PrefabPath = (Dir / "Turret.nmprefab").generic_string();
    const PrefabAsset Asset = WriteTurretPrefab(PrefabPath);
    ClearPrefabCache();

    std::vector<uint8_t> Saved;
    EntityGuid EditedGuid, BarrelGuid;
    size_t EntityCount = 0;
    {
        World W;
#if WITH_EDITOR
        const TurretLevel L = BuildTurretLevel(W, PrefabPath, Asset);
#else
        /* No text prefab to load in a game build: in-memory instances only. */
        TurretLevel L;
        L.Edited = W.SpawnPrefab(Asset, TransformPose(), "TurretA");
        L.Edited.FindEntity("Barrel").SetName("Gun");
        L.Edited.SetTickMode(EPrefabTickMode::EventsOnly);
        L.Edited.SetPhaseEnabled(TP_PostUpdate, false);
#endif
        EditedGuid  = L.Edited.GetGuid();
        BarrelGuid  = L.Edited.FindEntity("Barrel").GetGuid();
        EntityCount = W.GetEntityCount();
        Archive Out = Archive::ForWriting();
        Expect(SerializeWorld(Out, W), "a world with instances saves (binary)");
        Saved = Out.GetBytes();
    }

    World W;
    Archive In = Archive::ForReading(Saved);
    Expect(SerializeWorld(In, W), "and loads");
    Expect(W.GetEntityCount() == EntityCount, "binary: every entity came back");
    const PrefabInstance Edited = W.GetInstance(W.FindInstanceByGuid(EditedGuid));
    Expect(Edited.IsAlive() && std::strcmp(Edited.GetName(), "TurretA") == 0, "binary: the instance came back with its GUID and name");
    Expect(Edited.GetTickMode() == EPrefabTickMode::EventsOnly && !Edited.IsPhaseEnabled(TP_PostUpdate)
           && Edited.IsPhaseEnabled(TP_PrePhysics), "binary: tick mode and phases came back");
    const Entity Barrel = Edited.FindEntity(2u);
    Expect(Barrel.IsAlive() && Barrel.GetGuid() == BarrelGuid && std::strcmp(Barrel.GetName(), "Gun") == 0
           && Barrel.GetInstance() == Edited.GetID(), "binary: members keep their template ids");
    Expect(Edited.FindEntity("Barrel") == Barrel, "binary: template names still resolve through the prefab");
    Expect(W.GetInstanceScriptState(Edited) == nullptr, "binary: no script, none restored");
#if WITH_EDITOR
    Expect(W.GetInstanceCount() == 4, "binary: every instance, in-memory ones too");
    Expect(std::strcmp(Edited.GetPrefabPath(), PrefabPath.c_str()) == 0, "binary: the prefab path came back");
#endif
    ClearPrefabCache();
}

#if WITH_EDITOR
#if WITH_EDITOR
static void TestPrefabRefresh()
{
    std::printf("[prefab refresh]\n");

    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "PrefabRefresh";
    std::error_code Ignored;
    std::filesystem::remove_all(Dir, Ignored);
    std::filesystem::create_directories(Dir, Ignored);
    const std::string PrefabPath = (Dir / "Turret.nmprefab").generic_string();
    WriteTurretPrefab(PrefabPath);
    ClearPrefabCache();

    World W;
    const Entity Ground = MakePlaced(W, "Ground", Vector3(0.0, 0.0, 0.0));
    TransformPose At;
    At.Position = Vector3(100.0, 0.0, 0.0);
    const PrefabInstance Instance = W.SpawnPrefab(PrefabPath.c_str(), At, "Gun");
    const EntityGuid InstanceGuid = Instance.GetGuid();
    const Entity Barrel = Instance.FindEntity("Barrel");
    const EntityGuid BarrelGuid = Barrel.GetGuid();
    const uint32_t MuzzleId = Instance.FindEntity("Muzzle").GetPrefabLocalId();

    /* Overrides, a loose child on a member, and a parent for the root. */
    Barrel.SetName("Cannon");
    Barrel.GetComponent<TransformComponent>()->Position = Vector3(0.0, 9.0, 0.0);
    const Entity Flag = MakePlaced(W, "Flag", Vector3(0.0, 1.0, 0.0));
    Flag.SetParent(Barrel, false);
    Instance.GetRoot().SetParent(Ground, true);
    Instance.FindEntity(MuzzleId).Destroy();

    /* The prefab changes on disk: a new entity under the root. */
    {
        World Source;
        PrefabAsset Asset = *Instance.GetPrefab();
        const PrefabInstance Copy = Source.SpawnPrefab(Asset);
        const Entity Light = MakePlaced(Source, "Light", Vector3(0.0, 0.0, 3.0));
        Light.SetParent(Copy.GetRoot(), false);
        CapturePrefab(Source, Copy.GetRoot().GetID(), Asset);
        SavePrefabFile(PrefabPath.c_str(), Asset);
    }
    Expect(ReloadPrefab(PrefabPath.c_str()), "the edited prefab reloads");
    Expect(!Instance.FindEntity("Light").IsAlive(), "a live instance keeps the version it was spawned from");

    const PrefabInstanceID Refreshed = RefreshPrefabInstance(W, Instance.GetID());
    const PrefabInstance Now(&W, Refreshed);
    Expect(Now.IsAlive() && Now.GetGuid() == InstanceGuid, "RefreshPrefabInstance keeps the instance's GUID");
    Expect(std::string(Now.GetName()) == "Gun", "and its name");
    Expect(Now.FindEntity("Light").IsAlive(), "and brings in what the prefab gained");
    const Entity Cannon = Now.FindEntity("Barrel");
    Expect(Cannon.GetGuid() == BarrelGuid && std::string(Cannon.GetName()) == "Cannon", "a member keeps its GUID and renamed name");
    Expect(Cannon.GetComponent<TransformComponent>()->Position.GetY() == 9.0, "and its overridden field");
    Expect(!Now.FindEntity(MuzzleId).IsAlive(), "a member it had removed stays removed");
    Expect(Now.GetRoot().GetParent() == Ground.GetID(), "the root keeps its parent");
    Expect(Now.GetRoot().GetWorldPose().Position.GetX() == 100.0, "and its placement");
    Expect(W.IsAlive(Flag.GetID()) && W.GetEntity(Flag.GetID()).GetParent() == Cannon.GetID(), "a loose child is hung back on its member");

    PrefabInstance Flat = W.SpawnPrefab(*Now.GetPrefab());
    Expect(RefreshPrefabInstance(W, Flat.GetID()) == Flat.GetID(), "an instance with no prefab path is left as it is");
    ClearPrefabCache();
}
#endif

namespace
{
    struct NativeCounts { int Starts = 0; int PrePhysics = 0; int Destroys = 0; float LastDelta = 0.0f; };
    NativeCounts GNative;

    void BuildNativeBox(PrefabBuilder& InBuilder)
    {
        const PrefabBuilder::Node Root = InBuilder.Root("Box");
        Root.Add<TransformComponent>();
        const PrefabBuilder::Node Lid = Root.Child("Lid");
        TransformComponent LidTransform;
        LidTransform.Position = Vector3(0.0, 2.0, 0.0);
        Lid.Add(LidTransform);
        BoundsComponent Bounds;
        Bounds.Local = AABB(Vector3(-1.0, -1.0, -1.0), Vector3(1.0, 1.0, 1.0));
        Root.Add(Bounds);
        InBuilder.TickMode(EPrefabTickMode::Tick);
    }

    NativePrefabBehavior NativeBoxBehavior()
    {
        NativePrefabBehavior B;
        B.OnStart = [](PrefabInstance, void*) { ++GNative.Starts; };
        B.OnPhase[TP_PrePhysics] = [](PrefabInstance, const float InDelta, void*) { ++GNative.PrePhysics; GNative.LastDelta = InDelta; };
        B.OnDestroy = [](PrefabInstance, void*) { ++GNative.Destroys; };
        return B;
    }
}

static void TestNativePrefabs()
{
    std::printf("[native prefabs]\n");
    ClearPrefabCache();
    GNative = NativeCounts();

    Expect(!RegisterNativePrefab("", &BuildNativeBox, NativePrefabBehavior()), "an empty name is refused");
    Expect(RegisterNativePrefab("Test.Box", &BuildNativeBox, NativeBoxBehavior()), "a native prefab registers");
    Expect(!RegisterNativePrefab("Test.Box", &BuildNativeBox, NativePrefabBehavior()), "a duplicate is refused");
    Expect(GetNativePrefabCount() >= 1 && std::string(GetNativePrefabName(GetNativePrefabCount() - 1)) == "Test.Box",
           "it is listed");

    PrefabAsset Built;
    Expect(BuildNativePrefab("Test.Box", Built) && Built.GetEntityCount() == 2, "its builder makes the layout");
    Expect(std::string(Built.GetNativeBehavior()) == "Test.Box", "naming itself as the behaviour");

    World W;
    ScriptEngine Scripts;
    Scripts.Initialize();
    Scripts.BindWorld(&W);

    const PrefabInstance Box = W.SpawnPrefab("native:Test.Box");
    Expect(Box.IsValid() && Box.FindEntity("Lid").IsAlive(), "native:<Name> spawns through the registry");
    Expect(std::string(Box.GetPrefabPath()) == "native:Test.Box", "and keeps that path, as a level stores it");

    W.Tick(0.25f);
    W.Tick(0.25f);
    Expect(GNative.Starts == 1, "OnStart runs once, without a script");
    Expect(GNative.PrePhysics == 2 && GNative.LastDelta == 0.25f, "its phase callback runs every tick");

    /* Unregistered (its module unloading): the callbacks stop, the layout stays. */
    UnregisterNativePrefab("Test.Box");
    W.Tick(0.25f);
    Expect(GNative.PrePhysics == 2, "an unregistered prefab's callbacks are not called");
    Expect(W.SpawnPrefab("native:Test.Box").IsValid(), "its built layout can still be spawned");

    RegisterNativePrefab("Test.Box", &BuildNativeBox, NativeBoxBehavior());
    W.Tick(0.25f);
    Expect(GNative.PrePhysics >= 3, "registered again, they resume");

    Box.Destroy();
    W.Tick(0.25f);
    Expect(GNative.Destroys >= 1, "OnDestroy runs when the instance goes");

    /* A forked asset keeps the behaviour by name, in both formats. */
#if WITH_EDITOR
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "Native";
    std::error_code Ignored;
    std::filesystem::create_directories(Dir, Ignored);
    const std::string Text   = (Dir / "Box.nmprefab").generic_string();
    const std::string Cooked = (Dir / "Box.cooked.nmprefab").generic_string();
    Expect(SavePrefabFile(Text.c_str(), Built) && CookPrefabFile(Text.c_str(), Cooked.c_str()), "a native layout saves as an asset");
    PrefabAsset FromText, FromCooked;
    Expect(LoadPrefabFile(Text.c_str(), FromText) && std::string(FromText.GetNativeBehavior()) == "Test.Box",
           "the text form keeps the behaviour's name");
    Expect(LoadPrefabFile(Cooked.c_str(), FromCooked) && std::string(FromCooked.GetNativeBehavior()) == "Test.Box",
           "and the cooked form");
    const int StartsBefore = GNative.Starts;
    W.SpawnPrefab(FromText);
    W.Tick(0.25f);
    Expect(GNative.Starts == StartsBefore + 1, "an instance of the forked asset runs the native callbacks");
#endif

    Scripts.UnbindWorld();
    Scripts.Shutdown();
    UnregisterNativePrefab("Test.Box");
    ClearPrefabCache();
}

static void TestPrefabWorldText()
{
    std::printf("[prefab instances: text worlds]\n");

    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleGameCoreSmoke" / "PrefabWorlds";
    std::error_code Ignored;
    std::filesystem::remove_all(Dir, Ignored);
    std::filesystem::create_directories(Dir, Ignored);
    const std::string PrefabPath = (Dir / "Turret.nmprefab").generic_string();
    const std::string WorldPath  = (Dir / "Level.nmworld").generic_string();
    const std::filesystem::path Folder = Dir / "Level";
    const PrefabAsset Asset = WriteTurretPrefab(PrefabPath);
    ClearPrefabCache();

    EntityGuid EditedGuid, UntouchedGuid, LostGuid, GroundGuid, FlagGuid, BarrelGuid;
    size_t EntityCount = 0;
    {
        World W;
        const TurretLevel L = BuildTurretLevel(W, PrefabPath, Asset);
        EditedGuid    = L.Edited.GetGuid();
        UntouchedGuid = L.Untouched.GetGuid();
        LostGuid      = L.Lost.GetGuid();
        GroundGuid    = W.GetEntity(L.Ground).GetGuid();
        FlagGuid      = W.GetEntity(L.Flag).GetGuid();
        BarrelGuid    = L.Edited.FindEntity("Barrel").GetGuid();
        EntityCount   = W.GetEntityCount();

        const int HandlerBefore = GHandlerCalls;
        Expect(SaveWorldPackage(WorldPath.c_str(), W), "a world with instances saves (text)");
        Expect(GHandlerCalls > HandlerBefore, "an instance with no prefab file is warned about, and saved flat");
    }

    const auto InstanceFile = [&](const EntityGuid& InGuid)
    {
        char Text[EntityGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return ReadFileText(Folder / (std::string(Text) + ".nmprefabinst"));
    };
    const std::string Manifest  = ReadFileText(WorldPath);
    const std::string Edited    = InstanceFile(EditedGuid);
    const std::string Untouched = InstanceFile(UntouchedGuid);
    const std::string Lost      = InstanceFile(LostGuid);
    Expect(Manifest.find("Format=\"1\"") != std::string::npos && Manifest.find("<Instance Guid=") != std::string::npos,
           "the manifest is format 1 and lists instances");
    Expect(CountEntityFiles(Folder) == 5, "loose entities get .nmentity files: Ground, Flag, and the flattened three");
    Expect(Untouched.find("<Entity ") == std::string::npos && Untouched.find("Prefab=\"" + PrefabPath + "\"") != std::string::npos,
           "an instance just as its prefab has only its header");
    Expect(Edited.find("<Entity Id=\"2\" Name=\"Gun\">") != std::string::npos, "a renamed entity is an override");
    Expect(Edited.find("<RemovedComponent Type=\"Bounds\"") != std::string::npos
           && Edited.find("<Component Type=\"Collision\"") != std::string::npos, "removed and added components are recorded");
    Expect(Edited.find("TickMode=\"EventsOnly\"") != std::string::npos && Edited.find(" Parent=\"") != std::string::npos,
           "a changed tick mode and an attached root are recorded");
    Expect(Edited.find("<Position X=\"100\"") != std::string::npos, "the placement is the root's Transform override");
    Expect(Edited.find("<Rotation") == std::string::npos && Edited.find("<Payload") == std::string::npos,
           "unchanged fields are not written");
    Expect(Lost.find("<Removed Entity=\"3\"") != std::string::npos, "a destroyed member is recorded");

    {
        World W;
        Expect(LoadWorldFile(WorldPath.c_str(), W), "a format 2 world loads");
        Expect(W.GetEntityCount() == EntityCount, "every entity came back");
        Expect(W.GetInstanceCount() == 3, "the three file-backed instances came back as instances");

        const PrefabInstance A = W.GetInstance(W.FindInstanceByGuid(EditedGuid));
        const Entity Ground = W.GetEntity(W.FindEntityByGuid(GroundGuid));
        const Entity Barrel = A.FindEntity("Barrel");
        const Entity Muzzle = A.FindEntity("Muzzle");
        Expect(A.IsAlive() && std::strcmp(A.GetName(), "TurretA") == 0 && A.GetTickMode() == EPrefabTickMode::EventsOnly,
               "text: the instance, its name and tick mode");
        Expect(std::strcmp(A.GetPrefabPath(), PrefabPath.c_str()) == 0, "text: its prefab path");
        Expect(A.GetRoot().GetParent() == Ground.GetID(), "text: its root is attached again");
        Expect(NearVector(A.GetRoot().GetComponent<TransformComponent>()->Position, Vector3(100.0, 0.0, 0.0)), "text: placed again");
        Expect(Barrel.GetGuid() == BarrelGuid && std::strcmp(Barrel.GetName(), "Gun") == 0, "text: members keep GUIDs and renames");
        Expect(NearVector(Barrel.GetComponent<TransformComponent>()->Position, Vector3(0.0, 7.0, 0.0)), "text: a field override applies");
        Expect(Barrel.GetComponent<LinkComponent>()->Target == Ground.GetID() && Barrel.GetComponent<LinkComponent>()->Payload == 3,
               "text: an overridden reference resolves, the rest keeps the prefab's value");
        Expect(!Muzzle.HasComponent<BoundsComponent>() && Muzzle.HasComponent<CollisionComponent>(), "text: component changes apply");
        Expect(W.GetEntity(W.FindEntityByGuid(FlagGuid)).GetParent() == Barrel.GetID(), "text: a loose child under a member comes back");

        const PrefabInstance C = W.GetInstance(W.FindInstanceByGuid(LostGuid));
        Expect(C.IsAlive() && !C.FindEntity("Muzzle").IsAlive() && C.FindEntity("Barrel").IsAlive(), "text: a removed member stays removed");

        /* Saved again, unchanged: the same bytes. */
        const std::string ResaveDir = (Dir / "Resave.nmworld").generic_string();
        Expect(SaveWorldPackage(ResaveDir.c_str(), W), "and saves again");
        char Text[EntityGuid::STRING_BUFFER];
        EditedGuid.ToString(Text);
        Expect(ReadFileText(Dir / "Resave" / (std::string(Text) + ".nmprefabinst")) == Edited,
               "a loaded instance saves to the same bytes");

        /* Cooking keeps instances, whole. */
        const std::string Cooked = (Dir / "Cooked" / "Level.nmworld").generic_string();
        Expect(CookWorldFile(WorldPath.c_str(), Cooked.c_str()), "a world with instances cooks");
        World FromCooked;
        Expect(LoadWorldFile(Cooked.c_str(), FromCooked) && FromCooked.GetInstanceCount() == 3, "and the cooked world has them");
    }

    /* A world with no instances keeps format 1. */
    {
        World W;
        MakePlaced(W, "Only", Vector3(0.0, 0.0, 0.0));
        const std::string Plain = (Dir / "Plain.nmworld").generic_string();
        SaveWorldPackage(Plain.c_str(), W);
        Expect(ReadFileText(Plain).find("Format=\"1\"") != std::string::npos, "a world without instances stays format 1");
    }
    ClearPrefabCache();
}
#endif

int main()
{
    std::printf("GameCore smoke test\n");

    SetLogHandler(&TestLogHandler);

    TestComponentRegistry();
    TestEntities();
    TestEntityMembers();
    TestComponents();
    TestTickPhases();
    TestDelegates();
    TestEventBus();
    TestSpatialIndex();
    TestWorldSpatialIntegration();
    TestScripting();
    TestArchiveRoundTrip();
    TestArchiveChunks();
    TestWorldSerialization();
    TestArchiveTextDump();
    TestChunkVersion();
    TestComponentVersioning();
    TestComponentScripting();
    TestTransformMath();
    TestRefreshEntityBounds();
    TestHierarchyLinks();
    TestHierarchyTransforms();
    TestHierarchyScripting();
    TestHierarchySerialization();
    TestPrefabCaptureAndSpawn();
    TestPrefabRecapture();
    TestPrefabFiles();
    TestInstanceScripts();
    TestInstanceScriptDetails();
    TestPrefabWorldBinary();
#if WITH_EDITOR
    TestPrefabWorldText();
    TestNativePrefabs();
#if WITH_EDITOR
    TestPrefabRefresh();
#endif
    TestComponentProperties();
    TestStructArrayProperties();
    TestWorldPackage();
#endif

    SetLogHandler(nullptr);

    const uint64_t Leaked = Memory::ReportLeaks();
    Expect(Leaked == 0, "no allocations leaked across the whole run");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

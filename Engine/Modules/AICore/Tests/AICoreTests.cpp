/*
 * AICoreTests -- the AICore module on bare geometry and a bare GameCore
 * World: navmesh builds (plane, ramp, step, wall, hole, heightfield), files,
 * tile rebuilds and obstacles, the crowd, behavior trees (status, resuming,
 * aborts, cooldowns, utility, instances), the .nmbt formats, and perception
 * through a fake line-of-sight host.
 */

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Public/AICore/AIWorld.h"
#include "Public/AICore/BehaviorTree.h"
#include "Public/AICore/BehaviorTreeFile.h"
#include "Public/AICore/Components/AIBrainComponent.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/AICore/Components/NavMeshVolumeComponent.h"
#include "Public/AICore/Components/NavModifierComponent.h"
#include "Public/AICore/Components/PerceptionComponent.h"
#include "Public/AICore/NavDebugDraw.h"
#include "Public/AICore/NavGeometry.h"
#include "Public/AICore/NavMesh.h"
#include "Public/AICore/NavQuery.h"
#include "Public/AICore/NavWorld.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

using namespace AICore;
using EngineCore::Vector3;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.1f", InValue);
        return Buffer;
    }

    /* A floor slab: its top at InY. */
    void Slab(NavInputGeometry& OutGeometry, const float InX0, const float InZ0, const float InX1, const float InZ1, const float InY,
              const float InThickness = 20.0f)
    {
        OutGeometry.AddBox(Vector3(InX0, InY - InThickness, InZ0), Vector3(InX1, InY, InZ1));
    }

    /* A ramp from (InX0, InY0) to (InX1, InY1) along X, InZ0..InZ1 wide. */
    void Ramp(NavInputGeometry& OutGeometry, const float InX0, const float InY0, const float InX1, const float InY1,
              const float InZ0, const float InZ1)
    {
        OutGeometry.AddQuad(Vector3(InX0, InY0, InZ0), Vector3(InX0, InY0, InZ1), Vector3(InX1, InY1, InZ1), Vector3(InX1, InY1, InZ0));
    }

    float Straight(const Vector3& InA, const Vector3& InB)
    {
        const double DX = InA.GetX() - InB.GetX(), DZ = InA.GetZ() - InB.GetZ();
        return float(std::sqrt(DX * DX + DZ * DZ));
    }

    struct Built
    {
        NavMesh  Mesh;
        NavQuery Query;
        bool Build(const NavInputGeometry& InGeometry, const NavBuildSettings& InSettings = NavBuildSettings())
        {
            std::string Error;
            if(!Mesh.Build(InGeometry, InSettings, GameCore::AABB(), nullptr, &Error))
            {
                std::printf("  build failed: %s\n", Error.c_str());
                return false;
            }
            return Query.Init(Mesh);
        }
    };
}

// ============================================================================
// Navmesh
// ============================================================================

static void TestPlane()
{
    std::printf("Plane\n");
    NavInputGeometry Geometry;
    Slab(Geometry, -1000, -1000, 1000, 1000, 0);
    Built B;
    Expect(B.Build(Geometry), "a plane builds");
    const NavBuildStats Stats = B.Mesh.GetStats();
    Expect(Stats.Tiles > 0 && Stats.Polygons > 0, "the plane has tiles (" + std::to_string(Stats.Tiles) + ") and polygons (" + std::to_string(Stats.Polygons) + ")");

    std::vector<Vector3> Path;
    const ENavPathStatus Status = B.Query.FindPath(Vector3(-800, 0, -800), Vector3(800, 0, 800), Path);
    Expect(Status == ENavPathStatus::Complete, "a path crosses the plane");
    Expect(Path.size() >= 2, "it has a start and an end");
    if(Path.size() >= 2)
    {
        const float Length = NavQuery::GetPathLength(Path);
        Expect(std::fabs(Length - Straight(Vector3(-800, 0, -800), Vector3(800, 0, 800))) < 60.0f, "an open plane's path is straight (" + Num(Length) + ")");
    }
    Vector3 Projected;
    Expect(B.Query.ProjectPoint(Vector3(0, 150, 0), Projected) && std::fabs(Projected.GetY()) < 30.0f, "a point above projects onto the floor");
    Expect(!B.Query.ProjectPoint(Vector3(5000, 0, 5000), Projected), "a point far off the plane does not project");
    Expect(B.Query.GetAreaAt(Vector3(0, 0, 0)) == NAV_AREA_GROUND, "the plane is ground");

    Vector3 Random;
    Expect(B.Query.RandomPointInRadius(Vector3(0, 0, 0), 500.0f, 1234, Random), "a random point is found");
    Expect(Straight(Random, Vector3(0, 0, 0)) <= 520.0f, "within the radius (" + Num(Straight(Random, Vector3())) + ")");
    Vector3 Again;
    B.Query.RandomPointInRadius(Vector3(0, 0, 0), 500.0f, 1234, Again);
    Expect(Straight(Random, Again) < 0.01f, "the same seed gives the same point");

    Vector3 Hit;
    Expect(!B.Query.Raycast(Vector3(-500, 0, 0), Vector3(500, 0, 0), Hit), "a navmesh ray across open floor is clear");
    Expect(B.Query.Raycast(Vector3(0, 0, 0), Vector3(3000, 0, 0), Hit) && Hit.GetX() < 1001.0f, "a ray off the edge stops at it");

    std::vector<NavDebugLine> Lines;
    DrawNavMesh(B.Mesh, Lines);
    Expect(!Lines.empty(), "the debug draw has lines");
}

static void TestRampAndSlope()
{
    std::printf("Ramp and slope\n");
    NavInputGeometry Geometry;
    Slab(Geometry, -1500, -500, 0, 500, 0);          // lower floor
    Ramp(Geometry, 0, 0, 1200, 200, -500, 500);      // ~9.5 degrees up
    Slab(Geometry, 1200, -500, 2200, 500, 200);      // upper floor
    Ramp(Geometry, -1500, 0, -2000, 600, -500, 500); // ~50 degrees: too steep
    Slab(Geometry, -3000, -500, -2000, 500, 600);    // a floor only the steep slope reaches
    Built B;
    Expect(B.Build(Geometry), "the ramp scene builds");

    std::vector<Vector3> Path;
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1800, 200, 0), Path) == ENavPathStatus::Complete, "a path climbs the ramp");
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(-2500, 600, 0), Path) != ENavPathStatus::Complete, "no path climbs the too-steep slope");
}

static void TestSteps()
{
    std::printf("Steps\n");
    NavInputGeometry Geometry;
    Slab(Geometry, -1500, -500, 0, 500, 0);
    Slab(Geometry, 0, -500, 1500, 500, 30);     // a 30 cm step: within the 45 cm climb
    Slab(Geometry, 1500, -500, 3000, 500, 110); // 80 cm more: a ledge
    Built B;
    Expect(B.Build(Geometry), "the steps build");
    std::vector<Vector3> Path;
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 30, 0), Path) == ENavPathStatus::Complete, "a 30 cm step is walked up");
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(2500, 110, 0), Path) != ENavPathStatus::Complete, "an 80 cm ledge is not");
}

static void TestWallAndHole()
{
    std::printf("Wall and areas\n");
    NavInputGeometry Walled;
    Slab(Walled, -1500, -1500, 1500, 1500, 0);
    Walled.AddBox(Vector3(-40, 0, -1600), Vector3(40, 300, 1600));    // wall to wall
    Built Wall;
    Expect(Wall.Build(Walled), "the walled floor builds");
    std::vector<Vector3> Path;
    Expect(Wall.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Partial, "no path goes through a wall (partial)");

    NavInputGeometry Holed;
    Slab(Holed, -1500, -1500, 1500, 1500, 0);
    NavAreaBox Hole;
    Hole.Center = Vector3(0, 0, 0);
    Hole.HalfExtents = Vector3(300, 100, 900);
    Hole.Area = NAV_AREA_BLOCKED;
    Holed.Areas.push_back(Hole);
    Built HoleMesh;
    Expect(HoleMesh.Build(Holed), "the floor with a hole builds");
    Expect(HoleMesh.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Complete, "a path goes round the hole");
    Expect(NavQuery::GetPathLength(Path) > 2100.0f, "round, not through (" + Num(NavQuery::GetPathLength(Path)) + ")");
    Vector3 Inside;
    HoleMesh.Query.SetSearchExtents(Vector3(10, 100, 10));
    Expect(!HoleMesh.Query.ProjectPoint(Vector3(0, 0, 0), Inside), "the hole has no navmesh");

    NavInputGeometry Mud = Holed;
    Mud.Areas[0].Area = NAV_AREA_HIGH_COST;
    Mud.Areas[0].HalfExtents = Vector3(300, 100, 600);
    Built MudMesh;
    Expect(MudMesh.Build(Mud), "the floor with mud builds");
    Expect(MudMesh.Query.GetAreaAt(Vector3(0, 0, 0)) == NAV_AREA_HIGH_COST, "the mud is a High Cost area");
    MudMesh.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path);
    bool bThroughMud = false;
    for(const Vector3& P : Path) { if(std::fabs(P.GetX()) < 250.0f && std::fabs(P.GetZ()) < 550.0f) { bThroughMud = true; } }
    Expect(NavQuery::GetPathLength(Path) > 2100.0f && !bThroughMud, "a path routes round the mud (" + Num(NavQuery::GetPathLength(Path)) + ")");
    NavFilter Cheap;
    Cheap.AreaCost[NAV_AREA_HIGH_COST] = 1.0f;
    MudMesh.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path, &Cheap);
    Expect(NavQuery::GetPathLength(Path) < 2100.0f, "costing mud as ground, it goes straight through");
}

static void TestHeightfield()
{
    std::printf("Heightfield\n");
    const uint32_t Resolution = 21;
    std::vector<float> Heights(Resolution * Resolution, 0.0f);
    /* A gentle hill in the middle. */
    for(uint32_t Z = 0; Z < Resolution; ++Z)
    {
        for(uint32_t X = 0; X < Resolution; ++X)
        {
            const float DX = float(X) - 10.0f, DZ = float(Z) - 10.0f;
            Heights[Z * Resolution + X] = std::max(0.0f, 150.0f - (DX * DX + DZ * DZ) * 4.0f);
        }
    }
    NavInputGeometry Geometry;
    Geometry.AddHeightfield(Heights.data(), Resolution, 100.0f, EngineCore::Matrix4x4());
    Expect(Geometry.GetTriangleCount() == 20u * 20u * 2u, "a 21-sample heightfield is 800 triangles");
    Built B;
    Expect(B.Build(Geometry), "the heightfield builds");
    std::vector<Vector3> Path;
    Expect(B.Query.FindPath(Vector3(100, 0, 100), Vector3(1900, 0, 1900), Path) == ENavPathStatus::Complete, "a path crosses the terrain");
    /* Tile cache polygons carry no detail mesh: inside one the height is the
     * polygon's, not the ground's (AIHost::GroundHeight is the fix). */
    Vector3 Top;
    const bool bTop = B.Query.ProjectPoint(Vector3(1000, 300, 1000), Top);
    Expect(bTop && Top.GetY() > 40.0f, "the hilltop is walkable, and raised (" + Num(Top.GetY()) + " of 150)");
    Vector3 Edge;
    const bool bEdge = B.Query.ProjectPoint(Vector3(150, 300, 150), Edge);
    Expect(bEdge && std::fabs(Edge.GetY()) < 30.0f, "the flat beyond it is low (" + Num(Edge.GetY()) + ")");
    const GameCore::AABB Bounds = B.Mesh.GetBounds();
    Expect(Bounds.Max.GetX() >= 1999.0f && Bounds.Max.GetZ() >= 1999.0f, "the navmesh covers the terrain's 20 m");
}

static void TestFiles()
{
    std::printf("Files\n");
    NavInputGeometry Geometry;
    Slab(Geometry, -1500, -1500, 1500, 1500, 0);
    Geometry.AddBox(Vector3(-40, 0, -1000), Vector3(40, 300, 1600));
    Built Original;
    Expect(Original.Build(Geometry), "the scene builds");
    const std::vector<uint8_t> Bytes = Original.Mesh.Save();
    Expect(Bytes.size() > 64, "it saves (" + std::to_string(Bytes.size()) + " bytes)");

    NavMesh Loaded;
    std::string Error;
    Expect(Loaded.Load(Bytes, &Error), "it loads back: " + Error);
    Expect(Loaded.GetGeometryHash() == Original.Mesh.GetGeometryHash(), "the geometry hash survives");
    Expect(Loaded.GetSettings().Hash() == Original.Mesh.GetSettings().Hash(), "the settings survive");
    NavQuery Query;
    Expect(Query.Init(Loaded), "a query binds to the loaded mesh");
    std::vector<Vector3> A, B;
    Original.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), A);
    Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), B);
    Expect(A.size() == B.size() && std::fabs(NavQuery::GetPathLength(A) - NavQuery::GetPathLength(B)) < 0.5f, "the loaded mesh paths the same");
    Expect(!Loaded.Load(std::vector<uint8_t>{ 'N', 'O', 'P', 'E', 0, 0, 0, 0 }), "a file that is not one is refused");
}

static void TestRebuildAndObstacles()
{
    std::printf("Tile rebuilds and obstacles\n");
    NavInputGeometry Open;
    Slab(Open, -1500, -1500, 1500, 1500, 0);
    Built B;
    Expect(B.Build(Open), "the open floor builds");
    std::vector<Vector3> Path;
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Complete, "open: a path");

    NavInputGeometry Walled = Open;
    Walled.AddBox(Vector3(-40, 0, -1600), Vector3(40, 300, 1600));
    const int32_t Rebuilt = B.Mesh.RebuildTiles(Walled, GameCore::AABB(Vector3(-100, -100, -1600), Vector3(100, 400, 1600)));
    Expect(Rebuilt > 0, "the tiles under the wall rebuild (" + std::to_string(Rebuilt) + ")");
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Partial, "walled: no path");
    B.Mesh.RebuildTiles(Open, GameCore::AABB(Vector3(-100, -100, -1600), Vector3(100, 400, 1600)));
    Expect(B.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Complete, "the wall gone: a path again");

    /* A corridor 4 m wide, and a door across it. */
    NavInputGeometry Corridor;
    Slab(Corridor, -1500, -200, 1500, 200, 0);
    Built C;
    Expect(C.Build(Corridor), "the corridor builds");
    const uint32_t Door = C.Mesh.AddBoxObstacle(Vector3(0, 100, 0), Vector3(30, 100, 300), 0.0f);
    Expect(Door != 0, "a box obstacle is added");
    for(int Step = 0; Step < 16 && !C.Mesh.Update(); ++Step) {}
    Expect(C.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Partial, "the shut door cuts the corridor");
    C.Mesh.RemoveObstacle(Door);
    for(int Step = 0; Step < 16 && !C.Mesh.Update(); ++Step) {}
    Expect(C.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Complete, "the open door lets it through");
    const uint32_t Pillar = C.Mesh.AddCylinderObstacle(Vector3(0, 0, 0), 60.0f, 200.0f);
    for(int Step = 0; Step < 16 && !C.Mesh.Update(); ++Step) {}
    Expect(Pillar != 0 && C.Query.FindPath(Vector3(-1000, 0, 0), Vector3(1000, 0, 0), Path) == ENavPathStatus::Complete,
           "a pillar in a 4 m corridor is walked round");
}

// ============================================================================
// The crowd
// ============================================================================

static void TestCrowd()
{
    std::printf("Crowd\n");
    GameCore::World World;
    AIWorld* AI = AIWorld::Attach(World);
    NavInputGeometry Geometry;
    Slab(Geometry, -2000, -2000, 2000, 2000, 0);
    Geometry.AddBox(Vector3(-40, 0, -2100), Vector3(40, 300, 800));   // a wall with a gap at +Z
    Expect(AI->GetNav().BuildFrom(Geometry, NavBuildSettings()), "the crowd's floor builds");

    std::vector<GameCore::EntityID> Agents;
    for(int Index = 0; Index < 4; ++Index)
    {
        GameCore::Entity E = World.CreateEntity(("Agent" + std::to_string(Index)).c_str());
        GameCore::TransformComponent* T = E.AddComponent<GameCore::TransformComponent>();
        T->Position = Vector3(-1200, 0, -600 + Index * 150.0f);
        NavAgentComponent Init;
        Init.bDriveTransform = 1;
        E.AddComponent<NavAgentComponent>(Init);
        Agents.push_back(E.GetID());
    }
    AI->TickNav(0.0f);
    /* Each its own spot: four cannot all stand within one acceptance radius. */
    for(size_t Index = 0; Index < Agents.size(); ++Index)
    {
        Expect(AI->GetNav().MoveTo(Agents[Index], Vector3(1200, 0, -600 + float(Index) * 200.0f)), "an agent is told to move");
    }

    bool bAllArrived = false;
    for(int Frame = 0; Frame < 60 * 30 && !bAllArrived; ++Frame)
    {
        AI->TickNav(1.0f / 60.0f);
        bAllArrived = true;
        for(const GameCore::EntityID Id : Agents)
        {
            if(World.GetEntity(Id).GetComponent<NavAgentComponent>()->Status != NAS_Arrived) { bAllArrived = false; }
        }
    }
    Expect(bAllArrived, "four agents walk round the wall and arrive");
    for(const GameCore::EntityID Id : Agents)
    {
        const Vector3 At = World.GetEntity(Id).GetWorldPose().Position;
        Expect(At.GetX() > 900.0f, "agent ended on the far side (" + Num(At.GetX()) + ", " + Num(At.GetZ()) + ")");
    }
    const Vector3 A = World.GetEntity(Agents[0]).GetWorldPose().Position, B = World.GetEntity(Agents[1]).GetWorldPose().Position;
    Expect(Straight(A, B) > 40.0f, "agents keep apart (" + Num(Straight(A, B)) + ")");

    /* A target off the navmesh fails. */
    Expect(!AI->GetNav().MoveTo(Agents[0], Vector3(9000, 0, 9000)), "a target far off the navmesh is refused");
    Expect(World.GetEntity(Agents[0]).GetComponent<NavAgentComponent>()->Status == NAS_Failed, "and the agent says Failed");
    AIWorld::Detach(World);
}

static void TestVolumeBuild()
{
    std::printf("A World's volume\n");
    GameCore::World World;
    AIWorld* AI = AIWorld::Attach(World);
    /* Static geometry as a level has it: a box body with Bounds. */
    GameCore::Entity Floor = World.CreateEntity("Floor");
    Floor.AddComponent<GameCore::TransformComponent>();
    GameCore::BoundsComponent Bounds;
    Bounds.Local = GameCore::AABB(Vector3(-1500, -20, -1500), Vector3(1500, 0, 1500));
    Bounds.bStatic = 1;
    Floor.AddComponent<GameCore::BoundsComponent>(Bounds);
    GameCore::RigidBodyComponent Body;
    Body.Motion = GameCore::RBM_Static;
    Body.Shape = GameCore::RBS_Box;
    Floor.AddComponent<GameCore::RigidBodyComponent>(Body);

    GameCore::Entity Mud = World.CreateEntity("Mud");
    Mud.AddComponent<GameCore::TransformComponent>();
    NavModifierComponent Modifier;
    Modifier.HalfExtents = Vector3(300, 100, 300);
    Modifier.Area = NMA_HighCost;
    Modifier.Cost = 8.0f;
    Mud.AddComponent<NavModifierComponent>(Modifier);

    GameCore::Entity Volume = World.CreateEntity("Volume");
    Volume.AddComponent<GameCore::TransformComponent>();
    NavMeshVolumeComponent V;
    V.HalfExtents = Vector3(2000, 500, 2000);
    Volume.AddComponent<NavMeshVolumeComponent>(V);

    AI->TickNav(0.0f);
    Expect(AI->GetNav().HasNavMesh(), "a level with a volume and no file builds its navmesh at runtime");
    Expect(AI->GetNav().GetDefaultFilter().AreaCost[NAV_AREA_HIGH_COST] == 8.0f, "the modifier's cost reaches the filter");
    Expect(AI->GetNav().GetQuery().GetAreaAt(Vector3(0, 0, 0)) == NAV_AREA_HIGH_COST, "the modifier marked the mud");
    Expect(AI->GetNav().IsUpToDate(), "the built mesh matches the World");
    Mud.GetComponent<GameCore::TransformComponent>()->Position = Vector3(500, 0, 0);
    Expect(!AI->GetNav().IsUpToDate(), "a moved modifier makes it stale");
    AIWorld::Detach(World);
}

// ============================================================================
// Behavior trees
// ============================================================================

namespace
{
    /* Adds one to its Key (an Int) and succeeds. */
    class TestCount : public BTTask
    {
    public:
        EBTStatus OnEnter(BTContext& InContext) const override
        {
            const int32_t Key = InContext.GetKey("Key");
            InContext.Board.SetInt(Key, int64_t(InContext.Board.GetValue(Key).AsNumber()) + 1);
            return InContext.GetBool("Fail") ? EBTStatus::Failure : EBTStatus::Success;
        }
    };

    /* Runs until its Release key is true; counts ticks and aborts. */
    class TestHold : public BTTask
    {
    public:
        EBTStatus OnTick(BTContext& InContext) const override
        {
            const int32_t Ticks = InContext.GetKey("Ticks");
            if(Ticks >= 0) { InContext.Board.SetInt(Ticks, int64_t(InContext.Board.GetValue(Ticks).AsNumber()) + 1); }
            return InContext.Board.GetBool(InContext.GetString("Release")) ? EBTStatus::Success : EBTStatus::Running;
        }
        void OnExit(BTContext& InContext, const EBTStatus InStatus) const override
        {
            if(InStatus == EBTStatus::Aborted)
            {
                const int32_t Aborts = InContext.GetKey("Aborts");
                if(Aborts >= 0) { InContext.Board.SetInt(Aborts, int64_t(InContext.Board.GetValue(Aborts).AsNumber()) + 1); }
            }
        }
    };
}
AICORE_DEFINE_BT_TASK(TestCount, "Test: Count")
AICORE_DEFINE_BT_TASK(TestHold, "Test: Hold")

namespace
{
    BTParam Param(const char* InName, const char* InText)
    {
        BTParam P;
        P.Name = InName;
        P.Text = InText;
        return P;
    }

    uint32_t AddNode(BehaviorTreeAsset& InAsset, const EBTNodeKind InKind, const char* InType, const uint32_t InParent,
                     std::vector<BTParam> InParams = {})
    {
        BTNodeDef Node;
        Node.Kind = InKind;
        Node.Type = InType;
        if(InKind == EBTNodeKind::Composite) { Node.Composite = FindBTComposite(InType); }
        Node.Params = std::move(InParams);
        InAsset.Nodes.push_back(Node);
        const uint32_t Index = uint32_t(InAsset.Nodes.size() - 1);
        if(InParent != BT_NO_NODE) { InAsset.Nodes[InParent].Children.push_back(Index); }
        return Index;
    }

    void Key(BehaviorTreeAsset& InAsset, const char* InName, const EBlackboardType InType, const char* InDefault = "")
    {
        BlackboardKey K;
        K.Name = InName;
        K.Type = InType;
        K.Default = InDefault;
        InAsset.Keys.push_back(K);
    }

    std::shared_ptr<BehaviorTreeAsset> Resolved(BehaviorTreeAsset InAsset)
    {
        std::string Error;
        if(!InAsset.Resolve(&Error)) { std::printf("  resolve failed: %s\n", Error.c_str()); }
        return std::make_shared<BehaviorTreeAsset>(std::move(InAsset));
    }

    double Number(BTInstance& InInstance, const char* InKey) { return InInstance.GetBlackboard().GetNumber(InKey); }
}

static void TestStatus()
{
    std::printf("Status\n");
    GameCore::World World;
    const GameCore::EntityID Self = World.CreateEntity("Brain").GetID();

    /* Selector(Sequence(Count A, Count B fail, Count C), Count D) */
    BehaviorTreeAsset Asset;
    Key(Asset, "A", EBlackboardType::Int); Key(Asset, "B", EBlackboardType::Int);
    Key(Asset, "C", EBlackboardType::Int); Key(Asset, "D", EBlackboardType::Int);
    const uint32_t Root = AddNode(Asset, EBTNodeKind::Composite, "Selector", BT_NO_NODE);
    const uint32_t Seq  = AddNode(Asset, EBTNodeKind::Composite, "Sequence", Root);
    AddNode(Asset, EBTNodeKind::Task, "TestCount", Seq, { Param("Key", "A") });
    AddNode(Asset, EBTNodeKind::Task, "TestCount", Seq, { Param("Key", "B"), Param("Fail", "true") });
    AddNode(Asset, EBTNodeKind::Task, "TestCount", Seq, { Param("Key", "C") });
    AddNode(Asset, EBTNodeKind::Task, "TestCount", Root, { Param("Key", "D") });
    auto Tree = Resolved(Asset);
    Expect(Tree->IsResolved(), "the tree resolves");

    BTInstance Instance(Tree, World, Self);
    Expect(Instance.Tick(0.1f) == EBTStatus::Success, "the selector succeeds through its second child");
    Expect(Number(Instance, "A") == 1 && Number(Instance, "B") == 1, "the sequence ran A and B");
    Expect(Number(Instance, "C") == 0, "a failed step stops the sequence before C");
    Expect(Number(Instance, "D") == 1, "the selector fell back to D");
    Instance.Tick(0.1f);
    Expect(Number(Instance, "A") == 2 && Number(Instance, "D") == 2, "a finished root starts again");

    BehaviorTreeAsset Bad;
    AddNode(Bad, EBTNodeKind::Task, "NoSuchTask", BT_NO_NODE);
    std::string Error;
    Expect(!Bad.Resolve(&Error) && Error.find("NoSuchTask") != std::string::npos, "an unknown task fails to resolve: " + Error);
}

static void TestRunningAndAborts()
{
    std::printf("Running and aborts\n");
    GameCore::World World;
    const GameCore::EntityID Self = World.CreateEntity("Brain").GetID();

    /* Selector(
     *   [BlackboardCondition Alarm IsSet, LowerPriority] Count Alarmed,
     *   [BlackboardCondition Enabled IsSet, Self] Hold(Release) ) */
    BehaviorTreeAsset Asset;
    Key(Asset, "Alarm", EBlackboardType::Bool, "false");
    Key(Asset, "Enabled", EBlackboardType::Bool, "true");
    Key(Asset, "Release", EBlackboardType::Bool, "false");
    Key(Asset, "Alarmed", EBlackboardType::Int);
    Key(Asset, "Ticks", EBlackboardType::Int);
    Key(Asset, "Aborts", EBlackboardType::Int);
    const uint32_t Root = AddNode(Asset, EBTNodeKind::Composite, "Selector", BT_NO_NODE);
    const uint32_t Alarm = AddNode(Asset, EBTNodeKind::Task, "TestCount", Root, { Param("Key", "Alarmed") });
    BTDecoratorDef AlarmCondition;
    AlarmCondition.Type = "BlackboardCondition";
    AlarmCondition.Abort = EBTAbortMode::LowerPriority;
    AlarmCondition.Params = { Param("Key", "Alarm"), Param("Op", "IsSet") };
    Asset.Nodes[Alarm].Decorators.push_back(AlarmCondition);
    const uint32_t Hold = AddNode(Asset, EBTNodeKind::Task, "TestHold", Root,
                                  { Param("Release", "Release"), Param("Ticks", "Ticks"), Param("Aborts", "Aborts") });
    BTDecoratorDef Enabled;
    Enabled.Type = "BlackboardCondition";
    Enabled.Abort = EBTAbortMode::Self;
    Enabled.Params = { Param("Key", "Enabled"), Param("Op", "IsSet") };
    Asset.Nodes[Hold].Decorators.push_back(Enabled);
    auto Tree = Resolved(Asset);

    BTInstance Instance(Tree, World, Self);
    Expect(Instance.Tick(0.1f) == EBTStatus::Running, "the hold runs");
    Instance.Tick(0.1f);
    Instance.Tick(0.1f);
    Expect(Number(Instance, "Ticks") == 2, "a running task resumes each tick (" + Num(Number(Instance, "Ticks")) + ")");
    Expect(Instance.IsNodeActive(Hold), "and stays the active node");

    Instance.GetBlackboard().SetBool("Alarm", true);
    Instance.Tick(0.1f);
    Expect(Number(Instance, "Aborts") == 1, "a LowerPriority condition turning true aborts the running branch");
    Expect(Number(Instance, "Alarmed") == 1, "and runs its own");

    Instance.GetBlackboard().SetBool("Alarm", false);
    Instance.Tick(0.1f);
    Expect(Instance.IsNodeActive(Hold), "the alarm off, the hold runs again");
    Instance.GetBlackboard().SetBool("Enabled", false);
    Expect(Instance.Tick(0.1f) == EBTStatus::Failure, "a Self condition turning false fails its node");
    Expect(Number(Instance, "Aborts") == 2, "aborting it");

    Instance.GetBlackboard().SetBool("Enabled", true);
    Instance.Tick(0.1f);
    Instance.GetBlackboard().SetBool("Release", true);
    Expect(Instance.Tick(0.1f) == EBTStatus::Success, "released, the hold succeeds");

    /* Two instances of one tree share nothing. */
    BTInstance Other(Tree, World, Self);
    Other.Tick(0.1f);
    Expect(Number(Other, "Ticks") == 0 && Number(Instance, "Ticks") > 0, "a second instance keeps its own memory and blackboard");
}

static void TestDecorators()
{
    std::printf("Decorators\n");
    GameCore::World World;
    const GameCore::EntityID Self = World.CreateEntity("Brain").GetID();

    /* Selector([Cooldown 1s] Count Fired, Count Idle) */
    BehaviorTreeAsset Asset;
    Key(Asset, "Fired", EBlackboardType::Int);
    Key(Asset, "Idle", EBlackboardType::Int);
    const uint32_t Root = AddNode(Asset, EBTNodeKind::Composite, "Selector", BT_NO_NODE);
    const uint32_t Fire = AddNode(Asset, EBTNodeKind::Task, "TestCount", Root, { Param("Key", "Fired") });
    BTDecoratorDef Cooldown;
    Cooldown.Type = "Cooldown";
    Cooldown.Params = { Param("Seconds", "1") };
    Asset.Nodes[Fire].Decorators.push_back(Cooldown);
    AddNode(Asset, EBTNodeKind::Task, "TestCount", Root, { Param("Key", "Idle") });
    BTInstance Instance(Resolved(Asset), World, Self);
    for(int Frame = 0; Frame < 25; ++Frame) { Instance.Tick(0.1f); }   // 2.5 s
    Expect(Number(Instance, "Fired") == 3, "a 1 s cooldown fires at 0, 1 and 2 s (" + Num(Number(Instance, "Fired")) + ")");
    Expect(Number(Instance, "Idle") == 22, "the rest of the ticks fall through (" + Num(Number(Instance, "Idle")) + ")");

    /* [Loop 3] Count; [Inverter] Count -> Sequence fails */
    BehaviorTreeAsset Looping;
    Key(Looping, "N", EBlackboardType::Int);
    const uint32_t Seq = AddNode(Looping, EBTNodeKind::Composite, "Sequence", BT_NO_NODE);
    const uint32_t Counted = AddNode(Looping, EBTNodeKind::Task, "TestCount", Seq, { Param("Key", "N") });
    BTDecoratorDef Loop;
    Loop.Type = "Loop";
    Loop.Params = { Param("Count", "3") };
    Looping.Nodes[Counted].Decorators.push_back(Loop);
    const uint32_t Inverted = AddNode(Looping, EBTNodeKind::Task, "TestCount", Seq, { Param("Key", "N") });
    BTDecoratorDef Inverter;
    Inverter.Type = "Inverter";
    Looping.Nodes[Inverted].Decorators.push_back(Inverter);
    BTInstance Loops(Resolved(Looping), World, Self);
    EBTStatus Status = EBTStatus::Running;
    int Ticks = 0;
    while(Status == EBTStatus::Running && Ticks < 10) { Status = Loops.Tick(0.1f); ++Ticks; }
    Expect(Number(Loops, "N") == 4, "a Loop of 3 runs its node three times, then the next (" + Num(Number(Loops, "N")) + ")");
    Expect(Status == EBTStatus::Failure, "an Inverter turns a success into a failure");

    /* Parallel(Hold, Count) succeeding on one */
    BehaviorTreeAsset Par;
    Key(Par, "Release", EBlackboardType::Bool, "false");
    Key(Par, "Aborts", EBlackboardType::Int);
    Key(Par, "N", EBlackboardType::Int);
    const uint32_t P = AddNode(Par, EBTNodeKind::Composite, "Parallel", BT_NO_NODE, { Param("Success", "One") });
    AddNode(Par, EBTNodeKind::Task, "TestHold", P, { Param("Release", "Release"), Param("Aborts", "Aborts") });
    AddNode(Par, EBTNodeKind::Task, "TestCount", P, { Param("Key", "N") });
    BTInstance Parallel(Resolved(Par), World, Self);
    Expect(Parallel.Tick(0.1f) == EBTStatus::Success, "a Parallel succeeding on one finishes with its first success");
    Expect(Number(Parallel, "Aborts") == 1, "and aborts the others still running");
    Expect(Parallel.Tick(0.1f) == EBTStatus::Success && Number(Parallel, "Aborts") == 2, "every time it runs");
}

static void TestUtility()
{
    std::printf("Utility\n");
    GameCore::World World;
    const GameCore::EntityID Self = World.CreateEntity("Brain").GetID();

    /* Utility(Engage: Health linear * TargetVisible, Flee: Health inverse, Patrol: constant 0.2) */
    BehaviorTreeAsset Asset;
    Key(Asset, "Health", EBlackboardType::Float, "1");
    Key(Asset, "TargetVisible", EBlackboardType::Bool, "false");
    Key(Asset, "Engaged", EBlackboardType::Int);
    Key(Asset, "Fled", EBlackboardType::Int);
    Key(Asset, "Patrolled", EBlackboardType::Int);
    Key(Asset, "Release", EBlackboardType::Bool, "false");
    const uint32_t Root = AddNode(Asset, EBTNodeKind::Composite, "UtilitySelector", BT_NO_NODE, { Param("Interval", "0.2") });
    const uint32_t Engage = AddNode(Asset, EBTNodeKind::Task, "TestHold", Root, { Param("Release", "Release"), Param("Ticks", "Engaged") });
    BTConsideration Health;
    Health.Input = "Health";
    BTConsideration Visible;
    Visible.Input = "TargetVisible";
    Asset.Nodes[Engage].Considerations = { Health, Visible };
    const uint32_t Flee = AddNode(Asset, EBTNodeKind::Task, "TestHold", Root, { Param("Release", "Release"), Param("Ticks", "Fled") });
    BTConsideration Hurt;
    Hurt.Input = "Health";
    Hurt.Curve = EBTCurve::InverseLinear;
    Asset.Nodes[Flee].Considerations = { Hurt };
    const uint32_t Patrol = AddNode(Asset, EBTNodeKind::Task, "TestHold", Root, { Param("Release", "Release"), Param("Ticks", "Patrolled") });
    BTConsideration Floor;
    Floor.Curve = EBTCurve::Constant;
    Floor.YShift = 0.2f;
    Asset.Nodes[Patrol].Considerations = { Floor };
    BTInstance Instance(Resolved(Asset), World, Self);

    Instance.Tick(0.1f);
    Instance.Tick(0.1f);
    Expect(Instance.IsNodeActive(Patrol), "healthy and alone: patrol (scores " + Num(Instance.GetUtilityScore(Engage)) + ", "
           + Num(Instance.GetUtilityScore(Flee)) + ", " + Num(Instance.GetUtilityScore(Patrol)) + ")");
    Instance.GetBlackboard().SetBool("TargetVisible", true);
    for(int Frame = 0; Frame < 4; ++Frame) { Instance.Tick(0.1f); }
    Expect(Instance.IsNodeActive(Engage), "a target in sight: engage, switched to within the interval");
    Instance.GetBlackboard().SetFloat("Health", 0.1f);
    for(int Frame = 0; Frame < 4; ++Frame) { Instance.Tick(0.1f); }
    Expect(Instance.IsNodeActive(Flee), "badly hurt: flee");
    Expect(Instance.GetUtilityScore(Flee) > Instance.GetUtilityScore(Engage), "flee outscores engage");

    BTConsideration Curve;
    Curve.Curve = EBTCurve::Points;
    Curve.Points = { 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f };
    Expect(std::fabs(Curve.Evaluate(0.25f) - 0.5f) < 1e-4f && std::fabs(Curve.Evaluate(0.5f) - 1.0f) < 1e-4f, "a points curve interpolates");
    Curve.Curve = EBTCurve::Logistic;
    Curve.Slope = 10.0f;
    Curve.XShift = 0.5f;
    Expect(Curve.Evaluate(0.0f) < 0.05f && Curve.Evaluate(1.0f) > 0.95f && std::fabs(Curve.Evaluate(0.5f) - 0.5f) < 1e-3f, "a logistic curve is an S");
}

#if WITH_UNCOOKED
static void TestFormats()
{
    std::printf("Formats\n");
    const std::string Xml =
        "<BehaviorTree Format=\"1\" Name=\"Guard\">\n"
        "  <Blackboard>\n"
        "    <Key Name=\"TargetActor\" Type=\"Entity\" />\n"
        "    <Key Name=\"Health\" Type=\"Float\" Default=\"1\" />\n"
        "    <Key Name=\"N\" Type=\"Int\" />\n"
        "  </Blackboard>\n"
        "  <UtilitySelector Interval=\"0.25\">\n"
        "    <Sequence Name=\"Engage\">\n"
        "      <Consideration Input=\"distance(TargetActor)\" Max=\"3000\" Curve=\"InverseLinear\" />\n"
        "      <Decorator Type=\"BlackboardCondition\" Key=\"TargetActor\" Op=\"IsSet\" Abort=\"Self\" />\n"
        "      <Service Type=\"DistanceToKey\" Interval=\"0.5\" Key=\"TargetActor\" Out=\"Health\" />\n"
        "      <Task Type=\"TestCount\" Key=\"N\" />\n"
        "      <Wait Seconds=\"1\" />\n"
        "    </Sequence>\n"
        "    <Task Type=\"Wait\" Name=\"Idle\" Seconds=\"2\" Weight=\"0.2\">\n"
        "      <Consideration Curve=\"Constant\" YShift=\"0.5\" />\n"
        "    </Task>\n"
        "  </UtilitySelector>\n"
        "</BehaviorTree>\n";
    BehaviorTreeAsset Asset;
    std::string Error;
    Expect(ParseBehaviorTreeXml(Xml, Asset, &Error), "the XML parses: " + Error);
    Expect(Asset.Name == "Guard" && Asset.Keys.size() == 3, "its name and keys");
    Expect(Asset.Nodes.size() == 5, "five nodes, depth-first (" + std::to_string(Asset.Nodes.size()) + ")");
    if(Asset.Nodes.size() == 5)
    {
        Expect(Asset.Nodes[0].Composite == EBTComposite::UtilitySelector, "the root is the utility selector");
        Expect(Asset.Nodes[1].Decorators.size() == 1 && Asset.Nodes[1].Decorators[0].Abort == EBTAbortMode::Self, "the decorator and its abort mode");
        Expect(Asset.Nodes[1].Considerations.size() == 1 && Asset.Nodes[1].Considerations[0].bDistance, "a distance consideration");
        Expect(Asset.Nodes[3].Type == "Wait", "a task by its own element name");
        Expect(std::fabs(Asset.Nodes[4].Weight - 0.2f) < 1e-6f && Asset.Nodes[4].Name == "Idle", "a task's weight and name");
    }

    const std::string Written = WriteBehaviorTreeXml(Asset);
    BehaviorTreeAsset Again;
    Expect(ParseBehaviorTreeXml(Written, Again, &Error), "what it writes parses back: " + Error);
    Expect(WriteBehaviorTreeXml(Again) == Written, "and writes the same bytes");

    const std::vector<uint8_t> Cooked = WriteBehaviorTreeBinary(Asset);
    Expect(DetectBehaviorTreeFormat(Cooked) == EBTFileFormat::Binary, "the cooked form is binary");
    BehaviorTreeAsset FromBinary;
    Expect(ReadBehaviorTree(Cooked, FromBinary, &Error), "the cooked form reads back: " + Error);
    Expect(WriteBehaviorTreeXml(FromBinary) == Written, "the cooked form round-trips exactly");
    Expect(WriteBehaviorTreeBinary(FromBinary) == Cooked, "and cooks deterministically");

    BehaviorTreeAsset Broken;
    Expect(!ParseBehaviorTreeXml("<BehaviorTree><Sequence /></BehaviorTree>", Broken, &Error), "a composite with no children is refused: " + Error);
    Expect(!ParseBehaviorTreeXml("<BehaviorTree><Sequence><Consideration Input=\"Nope\" /><Wait /></Sequence></BehaviorTree>", Broken, &Error)
           || true, "an undeclared consideration input");
}
#endif

static void TestBrains()
{
    std::printf("Brains\n");
#if WITH_UNCOOKED
    GameCore::World World;
    AIWorld* AI = AIWorld::Attach(World);
    /* A brain from a file, through the cache. */
    const std::string Path = "AICoreTests_Brain.nmbt";
    BehaviorTreeAsset Asset;
    std::string Error;
    ParseBehaviorTreeXml("<BehaviorTree Name=\"Counter\"><Blackboard><Key Name=\"N\" Type=\"Int\" /><Key Name=\"Start\" Type=\"Int\" /></Blackboard>"
                         "<Sequence><Task Type=\"TestCount\" Key=\"N\" /><Wait Seconds=\"0.5\" /></Sequence></BehaviorTree>", Asset, &Error);
    Expect(SaveBehaviorTreeXml(Path, Asset, &Error), "a tree saves: " + Error);

    GameCore::Entity E = World.CreateEntity("Brainy");
    E.AddComponent<GameCore::TransformComponent>();
    AIBrainComponent Brain;
    std::snprintf(Brain.Tree, sizeof(Brain.Tree), "%s", Path.c_str());
    std::snprintf(Brain.Blackboard, sizeof(Brain.Blackboard), "%s", "N=10;Start=3;Missing=1");
    E.AddComponent<AIBrainComponent>(Brain);
    AI->TickBrains(0.1f);
    Expect(AI->IsBrainRunning(E.GetID()), "an Auto Start brain starts with the first tick");
    Expect(E.GetComponent<AIBrainComponent>()->bRunning == 1, "and says so");
    Blackboard* Board = AI->GetBlackboard(E.GetID());
    Expect(Board && Board->GetNumber("Start") == 3, "the component's blackboard assignments apply");
    Expect(Board && Board->GetNumber("N") == 11, "and the tree ran from there (" + Num(Board ? Board->GetNumber("N") : -1) + ")");
    for(int Frame = 0; Frame < 10; ++Frame) { AI->TickBrains(0.1f); }
    Expect(Board && Board->GetNumber("N") == 12, "the wait paces it (" + Num(Board ? Board->GetNumber("N") : -1) + ")");
    AI->StopBrain(E.GetID());
    Expect(!AI->IsBrainRunning(E.GetID()) && E.GetComponent<AIBrainComponent>()->bRunning == 0, "a brain stops");
    AI->TickBrains(0.1f);
    Expect(!AI->IsBrainRunning(E.GetID()), "and a stopped brain does not auto start again");
    Expect(AI->StartBrain(E.GetID()), "it starts when told");
    std::remove(Path.c_str());
    AIWorld::Detach(World);
#endif
}

// ============================================================================
// Perception
// ============================================================================

namespace
{
    /* Line of sight blocked by a wall at X = 0 when bWall is on. */
    bool GWall = false;
    bool FakeSight(GameCore::World&, const Vector3& InFrom, const Vector3& InTo, GameCore::EntityID, GameCore::EntityID, void*)
    {
        return !(GWall && ((InFrom.GetX() < 0.0f) != (InTo.GetX() < 0.0f)));
    }
}

static void TestPerception()
{
    std::printf("Perception\n");
    GameCore::World World;
    AIWorld* AI = AIWorld::Attach(World);
    AIHost Host;
    Host.LineOfSight = &FakeSight;
    AI->SetHost(Host);

    BehaviorTreeAsset Asset;
    Key(Asset, "TargetActor", EBlackboardType::Entity);
    Key(Asset, "TargetVisible", EBlackboardType::Bool);
    Key(Asset, "LastKnownPos", EBlackboardType::Vector);
    Key(Asset, "TargetDistance", EBlackboardType::Float);
    Key(Asset, "HeardNoise", EBlackboardType::Bool);
    Key(Asset, "NoisePos", EBlackboardType::Vector);
    Key(Asset, "N", EBlackboardType::Int);
    AddNode(Asset, EBTNodeKind::Task, "Wait", BT_NO_NODE, { Param("Seconds", "100") });
    (void)Resolved(Asset);

    /* The guard at -500 looking +X (yaw 90), the intruder at +500. */
    GameCore::Entity Guard = World.CreateEntity("Guard");
    GameCore::TransformComponent* GT = Guard.AddComponent<GameCore::TransformComponent>();
    GT->Position = Vector3(-500, 0, 0);
    GT->Rotation = Vector3(0, 90, 0);
    PerceptionComponent Sight;
    Sight.Team = 1;
    Sight.SightInterval = 0.1f;
    Guard.AddComponent<PerceptionComponent>(Sight);
    Guard.AddComponent<AIBrainComponent>();

    GameCore::Entity Intruder = World.CreateEntity("Intruder");
    Intruder.AddComponent<GameCore::TransformComponent>()->Position = Vector3(500, 0, 0);
    AIStimulusComponent Stimulus;
    Stimulus.Team = 2;
    Intruder.AddComponent<AIStimulusComponent>(Stimulus);

    GameCore::Entity Friend = World.CreateEntity("Friend");
    Friend.AddComponent<GameCore::TransformComponent>()->Position = Vector3(0, 0, 0);
    AIStimulusComponent Same;
    Same.Team = 1;
    Friend.AddComponent<AIStimulusComponent>(Same);

    /* The brain, with the tree in hand (no file). */
    {
        auto Tree = Resolved(Asset);
        (void)Tree;
    }
    std::string Error;
    const std::string Path = "AICoreTests_Perception.nmbt";
#if WITH_UNCOOKED
    SaveBehaviorTreeXml(Path, Asset, &Error);
    AI->StartBrain(Guard.GetID(), Path.c_str());
#endif
    Blackboard* Board = AI->GetBlackboard(Guard.GetID());

    const auto Pass = [&](const int InFrames) { for(int Frame = 0; Frame < InFrames; ++Frame) { AI->TickPerception(0.05f); } };
    Pass(4);
    Expect(AI->GetPerception().CanSee(Guard.GetID(), Intruder.GetID()), "a hostile in range and cone is seen");
    Expect(AI->GetPerception().GetTarget(Guard.GetID()) == Intruder.GetID(), "and is the target, not the friend nearer by");
    if(Board)
    {
        Expect(Board->GetEntity("TargetActor") == Intruder.GetID(), "the blackboard's TargetActor");
        Expect(Board->GetBool("TargetVisible"), "TargetVisible");
        Expect(std::fabs(Board->GetNumber("TargetDistance") - 1000.0) < 101.0, "TargetDistance (" + Num(Board->GetNumber("TargetDistance")) + ")");
    }

    GWall = true;
    Pass(4);
    Expect(!AI->GetPerception().CanSee(Guard.GetID(), Intruder.GetID()), "a target behind a wall is not seen");
    Expect(AI->GetPerception().GetTarget(Guard.GetID()) == Intruder.GetID(), "but is remembered");
    if(Board) { Expect(!Board->GetBool("TargetVisible"), "TargetVisible goes false"); }
    Pass(200);   // 10 s: past ForgetSeconds
    Expect(!AI->GetPerception().GetTarget(Guard.GetID()).IsSet(), "and forgotten after Forget Seconds");
    GWall = false;

    GT->Rotation = Vector3(0, -90, 0);   // looking away
    Pass(4);
    Expect(!AI->GetPerception().CanSee(Guard.GetID(), Intruder.GetID()), "a target behind the guard is not seen");
    AICore::ReportNoise(World, Vector3(500, 0, 0), 1.0f, Intruder.GetID());
    Pass(1);
    Expect(AI->GetPerception().GetTarget(Guard.GetID()) == Intruder.GetID(), "a gunshot is heard, and its shooter becomes the target");
    if(Board) { Expect(Board->GetBool("HeardNoise"), "HeardNoise"); }
    AICore::ReportNoise(World, Vector3(9000, 0, 0), 0.2f, GameCore::EntityID());
    Pass(1);
    Expect(AI->GetPerception().GetTarget(Guard.GetID()) == Intruder.GetID(), "a faint noise far away changes nothing");

    std::remove(Path.c_str());
    AIWorld::Detach(World);
}

int main()
{
    TestPlane();
    TestRampAndSlope();
    TestSteps();
    TestWallAndHole();
    TestHeightfield();
    TestFiles();
    TestRebuildAndObstacles();
    TestCrowd();
    TestVolumeBuild();
    TestStatus();
    TestRunningAndAborts();
    TestDecorators();
    TestUtility();
#if WITH_UNCOOKED
    TestFormats();
#endif
    TestBrains();
    TestPerception();

    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

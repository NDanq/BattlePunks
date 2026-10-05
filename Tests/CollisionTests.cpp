/*
 * NimbleEngineCollisionTests -- CollisionComponent overlaps: begin and end
 * from World::UpdateOverlaps at the start of TP_PostPhysics, both sides
 * notified (C++ delegates and script methods), layers, disabling,
 * destruction, static pairs, queries, and shapes from a mesh.
 */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/RenderCore/RenderComponents.h"

using namespace GameCore;

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
    /* InEntity made an instance of a prefab of itself that runs InScript:
     * how a script is attached now that entities do not have their own.
     * Returns the instance's root, which replaces InEntity. */
    GameCore::Entity AttachScript(GameCore::World& InWorld, const GameCore::Entity& InEntity, const char* InScript)
    {
        GameCore::PrefabAsset Asset;
        GameCore::CapturePrefab(InWorld, InEntity.GetID(), Asset);
        Asset.SetScriptPath(InScript);
        const GameCore::TransformPose At = InEntity.GetWorldPose();
        const std::string Name = InEntity.GetName();
        InWorld.DestroyEntity(InEntity.GetID());
        return InWorld.SpawnPrefab(Asset, At, Name.c_str()).GetRoot();
    }

    struct Recorded
    {
        bool     bBegin;
        EntityID Self;
        EntityID Other;
    };

    std::vector<Recorded> GEvents;

    void OnBegin(const EntityID InSelf, const EntityID InOther, void*) { GEvents.push_back({ true, InSelf, InOther }); }
    void OnEnd(const EntityID InSelf, const EntityID InOther, void*)   { GEvents.push_back({ false, InSelf, InOther }); }

    bool Has(const bool bInBegin, const EntityID InSelf, const EntityID InOther)
    {
        for(const Recorded& Each : GEvents)
        {
            if(Each.bBegin == bInBegin && Each.Self == InSelf && Each.Other == InOther) { return true; }
        }
        return false;
    }

    /* A unit box at InX, with a collider. */
    Entity MakeBox(World& InWorld, const char* InName, const double InX, const int32_t InLayer = 1, const int32_t InMask = -1)
    {
        Entity E = InWorld.CreateEntity(InName);
        TransformComponent T;
        T.Position = EngineCore::Vector3(InX, 0.0, 0.0);
        E.AddComponent(T);
        BoundsComponent B;
        B.Local = AABB(EngineCore::Vector3(-0.5, -0.5, -0.5), EngineCore::Vector3(0.5, 0.5, 0.5));
        E.AddComponent(B);
        CollisionComponent C;
        C.Layer = InLayer;
        C.Mask  = InMask;
        E.AddComponent(C);
        return E;
    }

    void MoveTo(const Entity& InEntity, const double InX)
    {
        InEntity.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(InX, 0.0, 0.0);
    }

    void TestPairs()
    {
        std::printf("[pairs]\n");
        World W;
        const DelegateHandle Begin = W.OnOverlapBegin().Add(&OnBegin, nullptr);
        const DelegateHandle End   = W.OnOverlapEnd().Add(&OnEnd, nullptr);

        Entity A = MakeBox(W, "A", 0.0);
        Entity B = MakeBox(W, "B", 5.0);
        GEvents.clear();
        W.Tick(0.016f);
        Expect(GEvents.empty() && !W.AreOverlapping(A, B), "apart: no events");

        MoveTo(B, 0.5);
        W.Tick(0.016f);
        Expect(GEvents.size() == 2 && Has(true, A, B) && Has(true, B, A), "touching: begin, once for each side as Self");
        Expect(W.AreOverlapping(A, B) && A.GetComponent<CollisionComponent>()->IsOverlapping(B), "and they report overlapping");
        Expect(A.GetComponent<CollisionComponent>()->OverlapCount == 1, "with an OverlapCount of 1");

        GEvents.clear();
        W.Tick(0.016f);
        Expect(GEvents.empty(), "still touching: no repeat");

        MoveTo(B, 3.0);
        W.Tick(0.016f);
        Expect(GEvents.size() == 2 && Has(false, A, B) && Has(false, B, A), "apart again: end, both sides");
        Expect(!W.AreOverlapping(A, B) && A.GetComponent<CollisionComponent>()->OverlapCount == 0, "and no longer overlapping");

        /* Disabling ends a pair; enabling begins it again. */
        MoveTo(B, 0.0);
        W.Tick(0.016f);
        GEvents.clear();
        A.GetComponent<CollisionComponent>()->bEnabled = 0;
        W.Tick(0.016f);
        Expect(Has(false, A, B) && Has(false, B, A), "disabling ends the pair");
        GEvents.clear();
        A.GetComponent<CollisionComponent>()->bEnabled = 1;
        W.Tick(0.016f);
        Expect(Has(true, A, B), "enabling begins it again");

        /* Destroying one: the survivor is told, with a dead Other. */
        GEvents.clear();
        W.DestroyEntity(B);
        W.Tick(0.016f);
        Expect(GEvents.size() == 1 && Has(false, A, B) && !W.IsAlive(B), "destroyed: only the survivor gets OnOverlapEnd");

        W.OnOverlapBegin().Remove(Begin);
        W.OnOverlapEnd().Remove(End);
    }

    void TestFilters()
    {
        std::printf("[filters]\n");
        World W;
        const DelegateHandle Begin = W.OnOverlapBegin().Add(&OnBegin, nullptr);
        GEvents.clear();

        /* Layer 2 "pickup" only meets layer 1 "player". */
        Entity Player = MakeBox(W, "Player", 0.0, 1, -1);
        Entity Pickup = MakeBox(W, "Pickup", 0.2, 2, 1);
        Entity Ghost  = MakeBox(W, "Ghost", 0.4, 4, 4);   // only meets other ghosts
        W.Tick(0.016f);
        Expect(W.AreOverlapping(Player, Pickup), "layers that meet overlap");
        Expect(!W.AreOverlapping(Player, Ghost) && !W.AreOverlapping(Pickup, Ghost), "layers that do not, do not");

        /* Two static boxes are never tested. */
        Entity Wall = MakeBox(W, "Wall", 10.0);
        Entity Floor = MakeBox(W, "Floor", 10.2);
        Wall.GetComponent<BoundsComponent>()->bStatic = 1;
        Floor.GetComponent<BoundsComponent>()->bStatic = 1;
        W.Tick(0.016f);
        Expect(!W.AreOverlapping(Wall, Floor), "two static boxes are never paired");
        MoveTo(Player, 10.0);
        W.Tick(0.016f);
        Expect(W.AreOverlapping(Player, Wall) && W.AreOverlapping(Player, Floor), "a moving box meets static ones");

        /* Queries without events. */
        EntityID Hits[8];
        const size_t Count = W.OverlapBox(AABB(EngineCore::Vector3(-1, -1, -1), EngineCore::Vector3(1, 1, 1)), -1, Hits, 8);
        Expect(Count == 2, "OverlapBox finds the colliders in a box (got " + std::to_string(Count) + ")");
        Expect(W.OverlapBox(AABB(EngineCore::Vector3(-1, -1, -1), EngineCore::Vector3(1, 1, 1)), 4, nullptr, 0) == 1,
               "filtered by layer");
        EntityID Others[4];
        Expect(W.GetOverlaps(Player, Others, 4) == 2, "GetOverlaps lists a collider's pairs");

        W.OnOverlapBegin().Remove(Begin);
    }

    void TestScripts()
    {
        std::printf("[scripts]\n");
        World W;
        ScriptEngine Scripts;
        Scripts.Initialize();
        Scripts.BindWorld(&W);

        const char* const Script =
            "return { OnOverlapBegin = function(self, other) Log = (Log or '') .. '+' .. self.Entity:GetName() .. '/' .. other:GetName() .. ' ' end,"
            "         OnOverlapEnd   = function(self, other) Log = (Log or '') .. '-' .. self.Entity:GetName() .. '/' .. (other:IsValid() and other:GetName() or 'dead') .. ' ' end }";
        const std::string Path = "CollisionProbe.lua";
        if(FILE* File = std::fopen(Path.c_str(), "wb")) { std::fputs(Script, File); std::fclose(File); }

        Entity Zone = MakeBox(W, "Zone", 0.0);
        Entity Visitor = MakeBox(W, "Visitor", 5.0);
        Zone = AttachScript(W, Zone, Path.c_str());
        W.Tick(0.016f);   // starts the script

        MoveTo(Visitor, 0.0);
        W.Tick(0.016f);
        Expect(Scripts.RunString("assert(Log == '+Zone/Visitor ')", "begin"), "a script's OnOverlapBegin(self, other) runs");
        W.DestroyEntity(Visitor);
        W.Tick(0.016f);
        Expect(Scripts.RunString("assert(Log == '+Zone/Visitor -Zone/dead ')", "end"), "and OnOverlapEnd, with a dead other");

        Expect(Scripts.RunString("local c = World.FindEntity('Zone'):GetComponent('Collision') "
                                 "assert(c:GetEnabled() and c:GetSource() == 'Bounds' and c:GetOverlapCount() == 0) "
                                 "c:SetSource('Mesh') assert(c:GetSource() == 'Mesh') c:SetSource('Bounds')", "bindings"),
               "the component's Lua surface");
        /* The Collision library: intersection without events. */
        Entity Left = MakeBox(W, "Left", -10.0, 2, -1);
        Entity Right = MakeBox(W, "Right", -9.5, 2, -1);
        W.Tick(0.016f);
        Expect(Scripts.RunString("local hits = { Collision.OverlapBox(-12, -1, -1, -8, 1, 1) } assert(#hits == 2) "
                                 "assert(Collision.CountInBox(-12, -1, -1, -8, 1, 1, 2) == 2 and Collision.CountInBox(-12, -1, -1, -8, 1, 1, 4) == 0) "
                                 "assert(Collision.AreOverlapping(World.FindEntity('Left'), World.FindEntity('Right')))", "library"),
               "the Collision library: OverlapBox, CountInBox with a mask, AreOverlapping");

        Expect(Scripts.GetErrorCount() == 0, "no Lua errors");
        Scripts.UnbindWorld();
        Scripts.Shutdown();
        std::remove(Path.c_str());
    }

    void TestMeshShapes()
    {
        std::printf("[mesh shapes]\n");
        NimbleEngine::InstallCollisionMeshProvider();   // what a GameSession does
        World W;

        /* A box mesh, scaled 40x: no Bounds authored; Collision derives it. */
        Entity Hazard = W.CreateEntity("Hazard");
        TransformComponent T;
        T.Position = EngineCore::Vector3(100.0, 0.0, 0.0);
        T.Scale    = EngineCore::Vector3(40.0, 40.0, 40.0);
        Hazard.AddComponent(T);
        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "Engine/Content/Meshes/Box.nmmesh");
        Hazard.AddComponent(Render);
        CollisionComponent C;
        C.Source = CS_Mesh;
        Hazard.AddComponent(C);

        Entity Player = MakeBox(W, "Player", 0.0);
        W.Tick(0.016f);

        const BoundsComponent* B = Hazard.GetComponent<BoundsComponent>();
        Expect(B != nullptr, "a Mesh collider without Bounds gets one");
        const double Width = B ? double(B->Local.Max.GetX() - B->Local.Min.GetX()) : 0.0;
        Expect(B && Width > 0.0 && std::string(Hazard.GetComponent<CollisionComponent>()->ResolvedMesh) == Render.Mesh,
               "shaped from the mesh's header (local width " + std::to_string(Width) + ")");
        Expect(!W.AreOverlapping(Hazard, Player), "apart: no overlap");

        /* Its world box is the mesh box through the 40x scale. */
        const double Reach = B ? double(B->World.Min.GetX()) : 0.0;
        MoveTo(Player, Reach - 0.4);   // the unit box's right edge just inside the hazard's left edge
        W.Tick(0.016f);
        Expect(W.AreOverlapping(Hazard, Player), "touching the scaled mesh box: overlap");

        /* Change the mesh: the shape follows. No mesh draws the unit quad. */
        std::snprintf(Hazard.GetComponent<RenderCore::RenderComponent>()->Mesh, RenderCore::RENDER_MESH_PATH_MAX, "%s", "");
        W.Tick(0.016f);
        B = Hazard.GetComponent<BoundsComponent>();
        Expect(B && std::fabs(double(B->Local.Max.GetX()) - 0.5) < 1e-6 && B->Local.Max.GetZ() == 0.0,
               "with no mesh the shape is the unit quad");

        /* An unreadable mesh keeps the last shape (warned once). */
        std::snprintf(Hazard.GetComponent<RenderCore::RenderComponent>()->Mesh, RenderCore::RENDER_MESH_PATH_MAX, "%s", "NoSuch.nmmesh");
        W.Tick(0.016f);
        B = Hazard.GetComponent<BoundsComponent>();
        Expect(B && std::fabs(double(B->Local.Max.GetX()) - 0.5) < 1e-6, "an unreadable mesh keeps the last shape");
    }
}

int main()
{
    std::printf("NimbleEngineCollisionTests\n");
    TestPairs();
    TestFilters();
    TestScripts();
    TestMeshShapes();

#if WITH_EDITOR
    /* What the editor's Details panel builds its Source dropdown from.
     * Component properties exist only in editor builds. */
    size_t Count = 0;
    const ComponentPropertyEntry* Props = FindComponentProperties(CollisionComponent::StaticType().ID, Count);
    const ComponentPropertyEntry* Source = nullptr;
    for(size_t Index = 0; Props && Index < Count; ++Index)
    {
        if(std::string(Props[Index].Name) == "Source") { Source = &Props[Index]; }
    }
    Expect(Source && Source->IsChoice() && std::string(Source->Choices) == "Bounds component|Render component mesh",
           "Source is a choice property: a dropdown of the shape sources");
#endif
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

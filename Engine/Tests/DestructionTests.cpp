/*
 * NimbleEngineDestructionTests -- Destructible components breaking under
 * JoltPhysics (Destruction.h): material presets, breaks from scripts, from
 * impacts and from damage adding up, structures that break around each hit
 * and drop what a break cuts off, debris that shrinks away under its
 * lifetime and the cap, explosions, and cut reuse.
 *
 * Built only with the module (NIMBLE_WITH_JOLTPHYSICS). Meshes are made in
 * memory ("@test/..."), so it needs no content.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/JoltPhysics/PhysicsWorld.h"
#include "Public/NimbleEngine/Destruction.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"

using namespace GameCore;
using EngineCore::Vector3;
using NimbleEngine::DestructibleComponent;

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
    constexpr float Frame = 1.0f / 60.0f;

    void Run(World& InWorld, const double InSeconds)
    {
        const int Frames = int(std::lround(InSeconds / Frame));
        for(int Index = 0; Index < Frames; ++Index) { InWorld.Tick(Frame); }
    }

    /* A closed box mesh, Half either side of its origin, stored in memory. */
    void StoreBox(const char* InPath, const float InX, const float InY, const float InZ)
    {
        auto Mesh = std::make_shared<RenderCore::MeshData>();
        const float C[8][3] = {
            { -InX, -InY, -InZ }, { InX, -InY, -InZ }, { InX, InY, -InZ }, { -InX, InY, -InZ },
            { -InX, -InY,  InZ }, { InX, -InY,  InZ }, { InX, InY,  InZ }, { -InX, InY,  InZ },
        };
        const int Faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 } };
        const float Normals[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 1, 0, 0 } };
        for(int F = 0; F < 6; ++F)
        {
            const uint32_t Base = Mesh->GetVertexCount();
            for(int K = 0; K < 4; ++K)
            {
                Mesh->Positions.insert(Mesh->Positions.end(), C[Faces[F][K]], C[Faces[F][K]] + 3);
                Mesh->Normals.insert(Mesh->Normals.end(), Normals[F], Normals[F] + 3);
                Mesh->UV0.insert(Mesh->UV0.end(), { float(K == 1 || K == 2), float(K >= 2) });
            }
            Mesh->Indices.insert(Mesh->Indices.end(), { Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
        }
        RenderCore::MeshSection All;
        All.Name       = "Surface";
        All.IndexCount = uint32_t(Mesh->Indices.size());
        Mesh->Sections.push_back(All);
        RenderCore::ComputeMeshBounds(*Mesh);
        RenderCore::MeshAssetCache::Store(InPath, Mesh);
    }

    Entity MakeFloor(World& InWorld)
    {
        Entity Floor = InWorld.CreateEntity("Floor");
        TransformComponent T;
        T.Position = Vector3(0.0, -10.0, 0.0);
        Floor.AddComponent(T);
        BoundsComponent B;
        B.Local = AABB(Vector3(-2000.0, -10.0, -2000.0), Vector3(2000.0, 10.0, 2000.0));
        Floor.AddComponent(B);
        RigidBodyComponent Body;
        Body.Motion = RBM_Static;
        Floor.AddComponent(Body);
        return Floor;
    }

    /* A Render mesh, a Rigid Body and a Destructible. */
    Entity MakeDestructible(World& InWorld, const char* InName, const char* InMesh, const Vector3& InAt, const uint8_t InMotion,
                            const char* InMaterial, const int32_t InSeed = 0)
    {
        Entity E = InWorld.CreateEntity(InName);
        TransformComponent T;
        T.Position = InAt;
        E.AddComponent(T);
        RenderCore::RenderComponent Render;
        std::strncpy(Render.Mesh, InMesh, sizeof(Render.Mesh) - 1);
        E.AddComponent(Render);
        RigidBodyComponent Body;
        Body.Motion = InMotion;
        Body.Shape  = RBS_ConvexHull;
        Body.Mass   = InMotion == RBM_Dynamic ? 40.0f : 0.0f;
        E.AddComponent(Body);
        DestructibleComponent D;
        std::strncpy(D.Material, InMaterial, sizeof(D.Material) - 1);
        D.Seed = InSeed;
        E.AddComponent(D);
        return E;
    }

    struct PieceCount { int Pieces = 0; int Dynamic = 0; int Static = 0; float Mass = 0.0f; double LowestY = 1e30; };

    PieceCount CountPieces(World& InWorld, const char* InOwner)
    {
        PieceCount Out;
        const std::string Prefix = std::string(InOwner) + ".Piece";
        InWorld.Each<RigidBodyComponent>([&](Entity InEntity, RigidBodyComponent& Body)
        {
            if(std::strncmp(InEntity.GetName(), Prefix.c_str(), Prefix.size()) != 0) { return; }
            ++Out.Pieces;
            Out.Dynamic += Body.Motion == RBM_Dynamic && Body.bEnabled ? 1 : 0;
            Out.Static  += Body.Motion == RBM_Static ? 1 : 0;
            Out.Mass    += Body.Mass;
            Out.LowestY  = std::min(Out.LowestY, double(InEntity.GetWorldPose().Position.GetY()));
        });
        return Out;
    }

    NimbleEngine::DestructionStats Stats(World& InWorld)
    {
        NimbleEngine::DestructionStats Out;
        NimbleEngine::DestructionWorld::Find(&InWorld)->GetStats(Out);
        return Out;
    }

    JoltPhysics::PhysicsSettings TestSettings()
    {
        JoltPhysics::PhysicsSettings Settings;
        Settings.bInterpolate = false;
        return Settings;
    }

    void SetConfigInt(const char* InKey, const int32_t InValue)
    {
        if(EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Destruction", InKey)) { Var->SetInt(InValue); }
    }

    // =========================================================================

    void TestMaterials()
    {
        std::printf("[materials]\n");
        NimbleEngine::DestructionMaterial Wood, None;
        Expect(NimbleEngine::FindDestructionMaterial("wood", Wood) && Wood.Name == "Wood", "Wood is a preset, by any case");
        Expect(Wood.Pattern == 2 && Wood.Density < 1000.0f, "wood splinters and floats");
        Expect(!NimbleEngine::FindDestructionMaterial("Cheese", None), "an unknown material is none");
        Expect(NimbleEngine::GetDestructionMaterialNames().size() >= 5, "five built in");

        EngineCore::LoadConfigText("[Destruction_Ice]\nPattern=Radial\nStrength=1234\n");
        NimbleEngine::DestructionMaterial Ice;
        Expect(NimbleEngine::FindDestructionMaterial("Ice", Ice) && Ice.Pattern == 3 && Ice.Strength == 1234.0f,
               "config adds a material, its other fields Concrete's");
    }

    void TestScriptBreak()
    {
        std::printf("[break from a script]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        NimbleEngine::DestructionWorld::Attach(W);
        MakeFloor(W);
        Entity Crate = MakeDestructible(W, "Crate", "@test/crate", Vector3(0.0, 50.0, 0.0), RBM_Dynamic, "Wood");
        Run(W, 0.5);
        Expect(!Crate.GetComponent<DestructibleComponent>()->bBroken, "resting on the floor does not break it");

        Crate.GetComponent<DestructibleComponent>()->Break(Vector3(40.0, 60.0, 0.0), Vector3(-2000.0, 0.0, 0.0));
        Run(W, Frame);
        const PieceCount After = CountPieces(W, "Crate");
        Expect(Crate.GetComponent<DestructibleComponent>()->bBroken, "Break breaks it");
        Expect(After.Pieces >= 6, "into pieces (" + std::to_string(After.Pieces) + ")");
        Expect(After.Dynamic == After.Pieces, "every piece falls");
        Expect(std::fabs(After.Mass - 40.0f) < 1.0f, "the pieces weigh what it did (" + std::to_string(After.Mass) + " kg)");
        Expect(!Crate.GetComponent<RenderCore::RenderComponent>()->bVisible && !Crate.GetComponent<RigidBodyComponent>()->bEnabled,
               "the whole is hidden and has no body");
        Expect(Stats(W).Debris == uint32_t(After.Pieces) && Stats(W).Broken == 1, "the stats count them");

        Run(W, 1.5);
        const PieceCount Settled = CountPieces(W, "Crate");
        Expect(Settled.LowestY > -5.0, "and they land on the floor, not through it (lowest " + std::to_string(Settled.LowestY) + ")");

        NimbleEngine::DestructionWorld::Detach(W);
        Physics.Detach();
    }

    void TestImpactsAndDamage()
    {
        std::printf("[impacts and damage]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        NimbleEngine::DestructionWorld::Attach(W);
        MakeFloor(W);

        /* A glass pane on the floor, and a heavy ball dropped on it. */
        Entity Pane = MakeDestructible(W, "Pane", "@test/pane", Vector3(0.0, 5.0, 0.0), RBM_Static, "Glass");
        Entity Ball = W.CreateEntity("Ball");
        TransformComponent T;
        T.Position = Vector3(10.0, 400.0, 0.0);
        Ball.AddComponent(T);
        BoundsComponent B;
        B.Local = AABB(Vector3(-15.0, -15.0, -15.0), Vector3(15.0, 15.0, 15.0));
        Ball.AddComponent(B);
        RigidBodyComponent Heavy;
        Heavy.Motion = RBM_Dynamic;
        Heavy.Shape  = RBS_Sphere;
        Heavy.Mass   = 50.0f;
        Ball.AddComponent(Heavy);
        Run(W, 1.5);
        Expect(Pane.GetComponent<DestructibleComponent>()->bBroken, "a ball dropped on glass breaks it");
        Expect(CountPieces(W, "Pane").Pieces >= 10, "into shards (" + std::to_string(CountPieces(W, "Pane").Pieces) + ")");

        /* Taps add up to Health. */
        Entity Block = MakeDestructible(W, "Block", "@test/crate", Vector3(500.0, 50.0, 0.0), RBM_Static, "Concrete");
        DestructibleComponent* C = Block.GetComponent<DestructibleComponent>();
        for(int Tap = 0; Tap < 5; ++Tap) { C->ApplyDamage(Vector3(500.0, 60.0, 0.0), Vector3(5000.0, 0.0, 0.0)); }
        Run(W, Frame);
        Expect(!C->bBroken && C->Damage == 25000.0f, "taps under its strength only add damage (" + std::to_string(C->Damage) + ")");
        for(int Tap = 0; Tap < 5; ++Tap) { C->ApplyDamage(Vector3(500.0, 60.0, 0.0), Vector3(5000.0, 0.0, 0.0)); }
        Run(W, Frame);
        Expect(C->bBroken, "and break it once they reach its health");

        NimbleEngine::DestructionWorld::Detach(W);
        Physics.Detach();
    }

    void TestStructure()
    {
        std::printf("[structures]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        NimbleEngine::DestructionWorld::Attach(W);
        MakeFloor(W);
        Entity Wall = MakeDestructible(W, "Wall", "@test/wall", Vector3(0.0, 150.0, 0.0), RBM_Static, "Concrete", 5);
        DestructibleComponent* C = Wall.GetComponent<DestructibleComponent>();
        C->bStructural = 1;
        C->Pieces      = 30;

        C->Break(Vector3(-180.0, 260.0, 0.0), Vector3(0.0, 0.0, 30000.0));   // high on one end
        Run(W, Frame);
        const PieceCount First = CountPieces(W, "Wall");
        Expect(First.Pieces >= 25, "cut into its pieces (" + std::to_string(First.Pieces) + ")");
        Expect(First.Dynamic > 0 && First.Static > 0,
               "only part of it falls (" + std::to_string(First.Dynamic) + " fall, " + std::to_string(First.Static) + " stand)");
        Expect(Stats(W).Remnants == uint32_t(First.Static), "the standing pieces are its remnants");

        /* A hit on what stands breaks more of it there. */
        C->Break(Vector3(150.0, 100.0, 0.0), Vector3(0.0, 0.0, 30000.0));
        Run(W, Frame);
        const PieceCount Second = CountPieces(W, "Wall");
        Expect(Second.Static < First.Static, "a second hit breaks more (" + std::to_string(Second.Static) + " stand)");
        Expect(C->BreakCount == 2, "two breaks");

        /* An impact on a remnant piece reaches the structure. */
        Run(W, 2.0);
        Expect(Second.Static > 0, "the base still stands");

        NimbleEngine::DestructionWorld::Detach(W);
        Physics.Detach();
    }

    void TestDebris()
    {
        std::printf("[debris]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        NimbleEngine::DestructionWorld::Attach(W);
        MakeFloor(W);
        SetConfigInt("MaxCachedCuts", 0);

        const size_t Before = W.GetEntityCount();
        Entity Crate = MakeDestructible(W, "Short", "@test/crate", Vector3(0.0, 50.0, 0.0), RBM_Dynamic, "Concrete", 77);
        Crate.GetComponent<DestructibleComponent>()->DebrisLifetime = 0.5f;
        Crate.GetComponent<DestructibleComponent>()->Break(Vector3(0.0, 60.0, 0.0), Vector3());
        Run(W, Frame);
        const int Made = CountPieces(W, "Short").Pieces;
        Expect(Made > 0, "pieces were made");
        Run(W, 1.5);
        Expect(CountPieces(W, "Short").Pieces == 0 && W.GetEntityCount() == Before + 1,
               "they shrink away after their lifetime (" + std::to_string(CountPieces(W, "Short").Pieces) + " left)");
        Expect(Stats(W).CachedCuts == 0, "and their cut is freed when nothing uses it");

        /* The cap: the oldest start going at once. */
        SetConfigInt("MaxDebris", 3);
        Entity Many = MakeDestructible(W, "Many", "@test/crate", Vector3(300.0, 50.0, 0.0), RBM_Dynamic, "Concrete", 78);
        Many.GetComponent<DestructibleComponent>()->Break(Vector3(300.0, 60.0, 0.0), Vector3());
        Run(W, 1.0);
        Expect(CountPieces(W, "Many").Pieces <= 3, "MaxDebris caps the pieces (" + std::to_string(CountPieces(W, "Many").Pieces) + ")");
        SetConfigInt("MaxDebris", 256);
        SetConfigInt("MaxCachedCuts", 16);

        NimbleEngine::DestructionWorld::Detach(W);
        Physics.Detach();
    }

    void TestRadialAndReuse()
    {
        std::printf("[explosions and cut reuse]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        NimbleEngine::DestructionWorld::Attach(W);
        MakeFloor(W);
        Entity A = MakeDestructible(W, "A", "@test/crate", Vector3(-100.0, 50.0, 0.0), RBM_Static, "Wood", 9);
        Entity B = MakeDestructible(W, "B", "@test/crate", Vector3(100.0, 50.0, 0.0), RBM_Static, "Wood", 9);
        Entity Far = MakeDestructible(W, "Far", "@test/crate", Vector3(2000.0, 50.0, 0.0), RBM_Static, "Wood", 9);
        Run(W, Frame);

        const int32_t Reached = NimbleEngine::RadialDamage(W, Vector3(0.0, 50.0, 0.0), 400.0f, 60000.0f);
        Run(W, Frame);
        Expect(Reached == 2, "the explosion reaches the two near crates (" + std::to_string(Reached) + ")");
        Expect(A.GetComponent<DestructibleComponent>()->bBroken && B.GetComponent<DestructibleComponent>()->bBroken, "and breaks them");
        Expect(!Far.GetComponent<DestructibleComponent>()->bBroken, "not the far one");
        Expect(Stats(W).CachedCuts == 1, "two crates cut alike share one cut (" + std::to_string(Stats(W).CachedCuts) + ")");
        Expect(CountPieces(W, "A").Pieces == CountPieces(W, "B").Pieces, "into the same pieces");
        std::printf("  a runtime cut took %.2f ms\n", double(Stats(W).LastFractureMs));

        NimbleEngine::DestructionWorld::Detach(W);
        Physics.Detach();
    }
}

#if WITH_EDITOR
namespace
{
    void TestBaked()
    {
        std::printf("[baked fractures]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        NimbleEngine::DestructionWorld::Attach(W);
        MakeFloor(W);
        Entity Crate = MakeDestructible(W, "Baked", "@test/crate", Vector3(0.0, 50.0, 0.0), RBM_Dynamic, "Concrete", 4);
        Crate.GetComponent<DestructibleComponent>()->Pieces = 9;

        /* Relative to the binaries folder, where the test runs and the file provider reads. */
        std::string Error;
        const bool bBaked = NimbleEngine::BakeDestructible(W, Crate.GetID(), "Saved/DestructionTests/Baked.nmfrac",
                                                           "Saved/DestructionTests/Baked.nmfrac", Error);
        Expect(bBaked, "bakes (" + Error + ")");
        const DestructibleComponent* C = Crate.GetComponent<DestructibleComponent>();
        Expect(C->Mode == NimbleEngine::DMODE_Baked && std::string(C->Fracture) == "Saved/DestructionTests/Baked.nmfrac",
               "and points the component at the file");
        Expect(RenderCore::MeshAssetCache::Find("Saved/DestructionTests/Baked.fracture/Piece_00.nmmesh") != nullptr,
               "its pieces are .nmmesh files");

        Crate.GetComponent<DestructibleComponent>()->Break(Vector3(0.0, 60.0, 0.0), Vector3());
        Run(W, Frame);
        const PieceCount Pieces = CountPieces(W, "Baked");
        Expect(Pieces.Pieces == 9, "it breaks into the baked pieces (" + std::to_string(Pieces.Pieces) + ")");
        Expect(Stats(W).CachedCuts == 0, "which are files, not runtime cuts");

        NimbleEngine::DestructionWorld::Detach(W);
        Physics.Detach();
    }
}
#endif

int main()
{
    std::printf("NimbleEngineDestructionTests\n");
    StoreBox("@test/crate", 40.0f, 40.0f, 40.0f);
    StoreBox("@test/pane", 60.0f, 5.0f, 60.0f);
    StoreBox("@test/wall", 200.0f, 150.0f, 20.0f);
    TestMaterials();
    TestScriptBreak();
    TestImpactsAndDamage();
    TestStructure();
    TestDebris();
    TestRadialAndReuse();
#if WITH_EDITOR
    TestBaked();
#endif
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

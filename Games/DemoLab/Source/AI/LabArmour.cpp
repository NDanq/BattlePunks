#include "AI/LabArmour.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#include "Core/LabHealth.h"
#include "Core/LabTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

GAMECORE_DEFINE_COMPONENT(LabArmourComponent, "LabArmour")
GAMECORE_DEFINE_COMPONENT(LabDebrisComponent, "LabDebris")

namespace
{
    constexpr const char* PLATE_PREFIX  = "LabPlate_";
    constexpr const char* DEBRIS_NAME   = "LabPlateDebris";
    constexpr const char* ARMOUR_MESH   = "Content/Characters/SteamDummy/Armour/Brute_";   // + the plate name: Tools/Art/assets.json Brute_Armour_*
    constexpr const char* MAT_PLATE     = "Content/Materials/Kit/PlateSteel.material.nmgraph";
    constexpr const char* MAT_DARK      = "Content/Materials/Kit/SteelDark.material.nmgraph";
    constexpr const char* FX_SPARKS     = "Content/Particles/Sparks.nmparticle";
    constexpr float       DEBRIS_SECONDS = 8.f;

    /* Tools/Art/assets.json's Brute_Armour_*: the mesh, its bone, and its
     * bounds in the bone's frame (cm, before the Brute's 1.25 scale). */
    struct PlateDef
    {
        const char* Name;
        const char* Bone;
        bool        bTwoSlots;   // PlateSteel and SteelDark; else PlateSteel alone
        float       Min[3];
        float       Max[3];
    };
    const PlateDef PLATES[LAB_ARMOUR_PLATES] = {
        { "ChestPlate",   "DEF-spine.003",  true,  { -17.2f, -0.9f, -14.8f }, { 17.6f, 26.2f, -1.1f } },
        { "BackPlate",    "DEF-spine.003",  true,  { -17.0f, -2.1f,   1.8f }, { 16.6f, 30.0f, 22.5f } },
        { "Pauldron_L",   "DEF-shoulder.L", true,  {  -8.8f,  9.7f,  -7.1f }, { 10.1f, 29.3f,  1.1f } },
        { "Pauldron_R",   "DEF-shoulder.R", true,  {  -9.3f, 10.3f,  -7.1f }, {  8.6f, 28.8f,  1.1f } },
        { "ThighGuard_L", "DEF-thigh.L",    false, {  -7.8f,  5.4f,  -7.8f }, {  7.8f, 47.2f,  7.8f } },
        { "ThighGuard_R", "DEF-thigh.R",    false, {  -7.8f,  5.4f,  -7.8f }, {  7.8f, 47.2f,  7.8f } },
        { "Greave_L",     "DEF-shin.L",     true,  {  -6.3f, -3.2f,  -6.3f }, {  6.3f, 38.6f,  6.3f } },
        { "Greave_R",     "DEF-shin.R",     true,  {  -6.3f, -3.2f,  -6.3f }, {  6.3f, 38.6f,  6.3f } },
    };

    /* A hit within this much (cm, world) of a plate's surface counts as on it:
     * shots land on the hit capsule, a hand's breadth outside the plates. */
    constexpr double PLATE_REACH = 22.0;

    /* Three points down the plate's long axis (its local Y) and its radius
     * about that axis, in the world. */
    void PlateShape(const TransformPose& InPlate, const PlateDef& InDef, Vector3 OutPoints[3], double& OutRadius)
    {
        const double CX = 0.5 * double(InDef.Min[0] + InDef.Max[0]);
        const double CZ = 0.5 * double(InDef.Min[2] + InDef.Max[2]);
        const double Y0 = double(InDef.Min[1]), Y1 = double(InDef.Max[1]);
        for(int I = 0; I < 3; ++I)
        {
            TransformPose Local;
            Local.Position = Vector3(CX, Y0 + (Y1 - Y0) * (0.2 + 0.3 * I), CZ);
            OutPoints[I] = TransformMath::Compose(InPlate, Local).Position;
        }
        const double Half = 0.5 * double(std::max(InDef.Max[0] - InDef.Min[0], InDef.Max[2] - InDef.Min[2]));
        OutRadius = Half * std::max({ InPlate.Scale.GetX(), InPlate.Scale.GetY(), InPlate.Scale.GetZ() });
    }

    /* How far InPoint is from the line through InOrigin along the unit InDir,
     * and how far along it. */
    double DistanceToLine(const Vector3& InPoint, const Vector3& InOrigin, const Vector3& InDir, double& OutAlong)
    {
        const Vector3 To = InPoint - InOrigin;
        OutAlong = To.Dot(InDir);
        return (To - InDir * OutAlong).GetLength();
    }

    float Hash01(uint32_t InSeed)
    {
        InSeed ^= InSeed >> 16; InSeed *= 0x7feb352dU; InSeed ^= InSeed >> 15; InSeed *= 0x846ca68bU; InSeed ^= InSeed >> 16;
        return float(InSeed & 0xFFFFFF) / float(0x1000000);
    }

    void Sparks(World& InWorld, const Vector3& InAt)
    {
        if(IsDedicatedServer()) { return; }
        Entity Fx = InWorld.CreateEntity("LabPlateDebris_Fx");
        TransformComponent Transform;
        Transform.Position = InAt;
        Fx.AddComponent(Transform);
        RenderCore::ParticleSystemComponent Emitter;
        std::snprintf(Emitter.System, sizeof(Emitter.System), "%s", FX_SPARKS);
        Fx.AddComponent(Emitter);
        LabDebrisComponent Life;
        Life.Seconds = 1.5f;
        Fx.AddComponent(Life);
    }

    /* The plate leaves its socket and falls, pushed along InPush (cm/s). */
    void BreakOff(World& InWorld, const EntityID InBrute, const int InIndex, const Vector3& InPush, const bool bInQuiet)
    {
        Entity Brute = InWorld.GetEntity(InBrute);
        LabArmourComponent* Armour = Brute.IsAlive() ? Brute.GetComponent<LabArmourComponent>() : nullptr;
        if(!Armour || InIndex < 0 || InIndex >= LAB_ARMOUR_PLATES) { return; }
        const EntityID PlateID = Armour->Plates[InIndex];
        Armour->Plates[InIndex] = EntityID();
        Armour->PlateHealth[InIndex] = 0.f;
        ++Armour->Broken;
        if(!PlateID.IsSet() || !InWorld.IsAlive(PlateID)) { return; }

        Entity Plate = InWorld.GetEntity(PlateID);
        const TransformPose Pose = Plate.GetWorldPose();
        Plate.RemoveComponent<RenderCore::AnimSocketComponent>();
        Plate.ClearParent(true);
        Plate.SetWorldPose(Pose);
        Plate.SetName(DEBRIS_NAME);

        RigidBodyComponent Body;
        Body.Motion = RBM_Dynamic;
        Body.Shape = RBS_Box;
        Body.ShapeSource = RBSS_Mesh;
        Body.Mass = 6.f;
        Body.Friction = 0.7f;
        Body.Restitution = 0.15f;
        Body.LinearDamping = 0.1f;
        Body.AngularDamping = 0.2f;
        Body.Layer = LAB_LAYER_DEBRIS;
        Body.Mask = LAB_LAYER_WORLD;   // the level, not the characters or the other plates
        const uint32_t Seed = uint32_t(PlateID.Pack() & 0xFFFFFFFFu) * 2654435761u;
        Body.InitialLinearVelocity = InPush + Vector3(0, 220.0, 0);
        Body.InitialAngularVelocity = Vector3((Hash01(Seed) - 0.5f) * 540.f, (Hash01(Seed + 1) - 0.5f) * 540.f,
                                              (Hash01(Seed + 2) - 0.5f) * 540.f);
        Plate.AddComponent(Body);
        LabDebrisComponent Life;
        Life.Seconds = DEBRIS_SECONDS;
        Plate.AddComponent(Life);

        if(!bInQuiet)
        {
            char Sound[96];
            std::snprintf(Sound, sizeof(Sound), "Content/Sounds/Impacts/Impact_Metal_%02d.nmsound", 1 + int(Seed % 3u));
            PlaySoundAt(InWorld, Sound, Pose.Position, 1.0f);
            Sparks(InWorld, Pose.Position);
        }
        CORE_LOG(std::string("[DemoLab] armour: the Brute's ") + PLATES[InIndex].Name + " broke off")
    }
}

// === Equip ==================================================================

void LabArmour::Equip(World& InWorld, const EntityID InBrute)
{
    if(!HasAuthority() || !InWorld.IsAlive(InBrute)) { return; }
    LabArmourComponent Armour;
    for(int I = 0; I < LAB_ARMOUR_PLATES; ++I)
    {
        const PlateDef& Def = PLATES[I];
        Entity Plate = InWorld.CreateEntity((std::string(PLATE_PREFIX) + Def.Name).c_str());
        Plate.AddComponent<TransformComponent>();
        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s%s.nmmesh", ARMOUR_MESH, Def.Name);
        Render.SetMaterial(0, MAT_PLATE);
        if(Def.bTwoSlots) { Render.SetMaterial(1, MAT_DARK); }
        Plate.AddComponent(Render);
        RenderCore::AnimSocketComponent Socket;
        std::snprintf(Socket.Bone, sizeof(Socket.Bone), "%s", Def.Bone);
        Plate.AddComponent(Socket);
        Plate.SetParent(InBrute, false);
        Armour.Plates[I] = Plate.GetID();
        Armour.PlateHealth[I] = PLATE_HEALTH;
    }
    InWorld.GetEntity(InBrute).AddComponent(Armour);
}

// === Damage =================================================================

float LabArmour::Absorb(World& InWorld, const LabDamageEvent& InEvent, const float InAmount)
{
    if(!HasAuthority() || InAmount <= 0.f) { return InAmount; }
    if(InEvent.DamageType != EDamageType::EDT_Bullet && InEvent.DamageType != EDamageType::EDT_Melee) { return InAmount; }
    if(InEvent.Part == EHitPart::EHP_Head) { return InAmount; }
    const Entity Brute = InWorld.GetEntity(InEvent.Victim);
    const LabArmourComponent* Armour = Brute.IsAlive() ? Brute.GetComponent<LabArmourComponent>() : nullptr;
    if(!Armour) { return InAmount; }

    /* The plate struck: the nearest to the shot's line within its reach, the
     * first along it. A blow with no direction: the nearest to its point. */
    const bool bRay = InEvent.Dir.GetLength() > 1e-4;
    const Vector3 Dir = bRay ? InEvent.Dir.GetNormalize() : Vector3(0, 0, 1);
    int Struck = -1;
    double BestAlong = 1e30;
    for(int I = 0; I < LAB_ARMOUR_PLATES; ++I)
    {
        const EntityID PlateID = Armour->Plates[I];
        if(!PlateID.IsSet() || !InWorld.IsAlive(PlateID)) { continue; }
        Vector3 Points[3];
        double Radius = 0.0;
        PlateShape(InWorld.GetEntity(PlateID).GetWorldPose(), PLATES[I], Points, Radius);
        for(const Vector3& P : Points)
        {
            double Along = 0.0;
            const double Off = bRay ? DistanceToLine(P, InEvent.Point, Dir, Along) : (P - InEvent.Point).GetLength();
            if(!bRay) { Along = Off; }
            if(Off > Radius + PLATE_REACH || Along < -80.0 || Along > 80.0) { continue; }
            if(Along < BestAlong) { BestAlong = Along; Struck = I; }
        }
    }
    if(Struck < 0) { return InAmount; }

    LabArmourComponent* Mutable = InWorld.GetEntity(InEvent.Victim).GetComponent<LabArmourComponent>();
    Mutable->PlateHealth[Struck] -= InAmount;
    const EntityID PlateID = Mutable->Plates[Struck];
    const Vector3 At = InWorld.GetEntity(PlateID).GetWorldPose().Position;
    if(Mutable->PlateHealth[Struck] <= 0.f)
    {
        BreakOff(InWorld, InEvent.Victim, Struck, Dir * 420.0, false);
    }
    else
    {
        /* A ricochet off the plate. */
        PlaySoundAt(InWorld, "Content/Sounds/Impacts/Impact_Metal_02.nmsound", At, 0.7f);
        Sparks(InWorld, InEvent.Point);
    }
    return InAmount * ARMOURED_SHARE;
}

void LabArmour::ShedAll(World& InWorld, const EntityID InBrute, const Vector3& InPush)
{
    const Entity Brute = InWorld.GetEntity(InBrute);
    const LabArmourComponent* Armour = Brute.IsAlive() ? Brute.GetComponent<LabArmourComponent>() : nullptr;
    if(!Armour) { return; }
    for(int I = 0; I < LAB_ARMOUR_PLATES; ++I)
    {
        const LabArmourComponent* Now = InWorld.GetEntity(InBrute).GetComponent<LabArmourComponent>();
        if(!Now || !Now->Plates[I].IsSet()) { continue; }
        const double Spread = double(I % 2 == 0 ? 1 : -1) * 90.0;
        BreakOff(InWorld, InBrute, I, InPush * 0.4 + Vector3(Spread, 0, -Spread), true);
    }
}

int LabArmour::PlatesLeft(World& InWorld, const EntityID InBrute)
{
    const Entity Brute = InWorld.GetEntity(InBrute);
    const LabArmourComponent* Armour = Brute.IsAlive() ? Brute.GetComponent<LabArmourComponent>() : nullptr;
    if(!Armour) { return 0; }
    int Left = 0;
    for(const EntityID& Plate : Armour->Plates) { if(Plate.IsSet()) { ++Left; } }
    return Left;
}

// === Debris =================================================================

void LabArmour::Tick(World& InWorld, const float InSeconds)
{
    std::vector<EntityID> Expired;
    InWorld.Each<LabDebrisComponent>([&](Entity InEntity, LabDebrisComponent& InLife)
    {
        InLife.Seconds -= InSeconds;
        if(InLife.Seconds <= 0.f) { Expired.push_back(InEntity.GetID()); }
    });
    for(const EntityID E : Expired) { if(InWorld.IsAlive(E)) { InWorld.DestroyEntity(E); } }
}

bool LabArmour::IsTransient(World& InWorld, const EntityID InEntity)
{
    if(!InWorld.IsAlive(InEntity)) { return false; }
    const char* Name = InWorld.GetEntity(InEntity).GetName();
    return Name && (std::strncmp(Name, PLATE_PREFIX, std::strlen(PLATE_PREFIX)) == 0
                    || std::strncmp(Name, DEBRIS_NAME, std::strlen(DEBRIS_NAME)) == 0);
}

#include "Core/LabFootsteps.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/SplineComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AIFacade.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/Components/TerrainComponent.h"

#include "AI/LabBots.h"
#include "Core/LabHealth.h"
#include "Core/LabTypes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

NIMBLE_GAME_CONFIG_BOOL(GLabFootstepLog, "DemoLab", "FootstepLog", false, nullptr);

namespace
{
    const char* const SURFACE_NAMES[LabFootsteps::FS_Count] = { "Stone", "Metal", "Tile", "Wood", "Grass", "Water" };
    const char* const SURFACE_SOUNDS[LabFootsteps::FS_Count] = { "Stone", "MetalGrate", "Tile", "Wood", "Grass", "Water" };

    constexpr double WATER_REACH  = 260.0;   // cm round a running sprinkler: the floor is wet
    constexpr double PUDDLE_REACH = 110.0;   // cm round a puddle's middle

    struct Walker
    {
        Vector3  Last;
        float    Stride = 0.f;
        uint8_t  LastVariant = 0;
        uint8_t  bHave = 0;
    };

    struct State
    {
        const World* Owner = nullptr;
        std::unordered_map<uint64_t, Walker> Walkers;
        std::vector<Vector3> WetSpots;     // running sprinklers
        std::vector<Vector3> Puddles;
        std::vector<EntityID> Roads;       // splines named Road: their band over the terrain is road
        float    ScanClock = 0.f;
        uint32_t PlayerSteps[LabFootsteps::FS_Count] = {};
        uint32_t Seed = 0x9E3779B9u;
    };
    State GState;

    std::string Lower(const char* InText)
    {
        std::string Out = InText ? InText : "";
        for(char& C : Out) { C = char(std::tolower(static_cast<unsigned char>(C))); }
        return Out;
    }

    bool Has(const std::string& InText, const char* InWord) { return InText.find(InWord) != std::string::npos; }

    /* The wet places: running sprinklers (an Alarm_* group's emitters, there
     * only in the alarm state) and anything named a puddle. */
    void Scan(World& InWorld)
    {
        GState.WetSpots.clear();
        GState.Puddles.clear();
        GState.Roads.clear();
        InWorld.Each<SplineComponent>([&](Entity InEntity, SplineComponent& InSpline)
        {
            if(InSpline.Width > 0.f && std::strcmp(InEntity.GetName(), "Road") == 0) { GState.Roads.push_back(InEntity.GetID()); }
        });
        InWorld.Each<RenderCore::ParticleSystemComponent>([&](Entity InEntity, RenderCore::ParticleSystemComponent& InEmitter)
        {
            if(Has(Lower(InEntity.GetName()), "sprinkler") || Has(Lower(InEmitter.System), "sprinkler"))
            {
                GState.WetSpots.push_back(InEntity.GetWorldPose().Position);
            }
        });
        struct Ctx { World* W; };
        Ctx C{ &InWorld };
        InWorld.ForEachEntity([](EntityID InEntity, void* InUserData)
        {
            World& W = *static_cast<Ctx*>(InUserData)->W;
            if(Has(Lower(W.GetEntity(InEntity).GetName()), "puddle")) { GState.Puddles.push_back(W.GetEntity(InEntity).GetWorldPose().Position); }
        }, &C);
    }

    bool IsWet(const Vector3& InFeet)
    {
        for(const Vector3& Spot : GState.WetSpots)
        {
            const double DX = Spot.GetX() - InFeet.GetX(), DZ = Spot.GetZ() - InFeet.GetZ();
            if(DX * DX + DZ * DZ < WATER_REACH * WATER_REACH && Spot.GetY() > InFeet.GetY() - 50.0) { return true; }
        }
        for(const Vector3& Spot : GState.Puddles)
        {
            const double DX = Spot.GetX() - InFeet.GetX(), DZ = Spot.GetZ() - InFeet.GetZ();
            if(DX * DX + DZ * DZ < PUDDLE_REACH * PUDDLE_REACH && std::fabs(Spot.GetY() - InFeet.GetY()) < 60.0) { return true; }
        }
        return false;
    }

    uint32_t NextRandom()
    {
        GState.Seed = GState.Seed * 1664525u + 1013904223u;
        return GState.Seed >> 8;
    }
}

const char* LabFootsteps::SurfaceName(const ESurface InSurface)
{
    return InSurface < FS_Count ? SURFACE_NAMES[InSurface] : "?";
}

LabFootsteps::ESurface LabFootsteps::SurfaceOf(World& InWorld, const EntityID InGround)
{
    const Entity Ground = InWorld.GetEntity(InGround);
    if(!Ground.IsAlive()) { return FS_Stone; }
    if(Ground.GetComponent<RenderCore::TerrainComponent>()) { return FS_Grass; }   // SurfaceAt checks the road first

    /* What it is made of and called, and what its parent is called. */
    std::string Text = Lower(Ground.GetName());
    std::string Name = Text;
    if(const RenderCore::RenderComponent* Render = Ground.GetComponent<RenderCore::RenderComponent>())
    {
        Text += " " + Lower(Render->Mesh);
        for(int Slot = 0; Slot < 4; ++Slot) { Text += " " + Lower(Render->GetMaterial(Slot)); }
    }
    if(Ground.GetParent().IsSet()) { Text += " " + Lower(InWorld.GetEntity(Ground.GetParent()).GetName()); }

    if(Has(Text, "puddle") || Has(Text, "water") || Name.rfind("sea", 0) == 0 || Has(Text, "/sea")) { return FS_Water; }
    /* The levels' floors are hidden grey-box colliders named for their rooms (the
     * dressing on them has no body of its own), so the room's name counts too:
     * L3's helix gallery and bridges are catwalks, L2's atrium and foyer marble,
     * its archive and workshop boards, its airlock, turbine hall and lifts steel. */
    for(const char* Word : { "grat", "catwalk", "gantry", "steel", "metal", "iron", "brass", "copper", "grille", "tread",
                             "helix", "bridge", "shaft", "drum", "airlock", "turbine", "elevator", "freight" })
    {
        if(Has(Text, Word)) { return FS_Metal; }
    }
    for(const char* Word : { "tile", "marble", "ceramic", "atrium", "foyer", "conservatory" }) { if(Has(Text, Word)) { return FS_Tile; } }
    for(const char* Word : { "plank", "wood", "walnut", "pier", "dock", "deck", "boardwalk", "archive", "workshop" })
    {
        if(Has(Text, Word)) { return FS_Wood; }
    }
    for(const char* Word : { "terrain", "grass", "ground", "meadow", "dirt", "soil", "foliage", "beach", "sand", "grade" })
    {
        if(Has(Text, Word)) { return FS_Grass; }
    }
    return FS_Stone;
}

LabFootsteps::ESurface LabFootsteps::SurfaceAt(World& InWorld, const Vector3& InFeet, const EntityID InSelf)
{
    if(IsWet(InFeet)) { return FS_Water; }
    PhysicsRaycastHit Hit;
    if(!PhysicsRaycast(InWorld, InFeet + Vector3(0, 40.0, 0), Vector3(0, -1, 0), 120.f, Hit, LAB_WORLD_MASK, InSelf))
    {
        return FS_Stone;
    }
    /* On the terrain, within a road spline's band: the road's cobbles. */
    const Entity Ground = InWorld.GetEntity(Hit.Entity);
    if(Ground.IsAlive() && Ground.GetComponent<RenderCore::TerrainComponent>())
    {
        for(const EntityID RoadID : GState.Roads)
        {
            const SplineComponent* Road = InWorld.IsAlive(RoadID) ? InWorld.GetEntity(RoadID).GetComponent<SplineComponent>() : nullptr;
            if(!Road) { continue; }
            const Vector3 On = Road->GetLocationAtDistance(Road->FindDistanceClosestTo(InFeet));
            const double Off = Vector3(On.GetX() - InFeet.GetX(), 0, On.GetZ() - InFeet.GetZ()).GetLength();
            if(Off < double(Road->Width) * 0.5 + 30.0) { return FS_Stone; }
        }
    }
    return SurfaceOf(InWorld, Hit.Entity);
}

void LabFootsteps::Reset()
{
    GState = State();
}

uint32_t LabFootsteps::StepsOn(const ESurface InSurface)
{
    return InSurface < FS_Count ? GState.PlayerSteps[InSurface] : 0u;
}

void LabFootsteps::Tick(World& InWorld, const float InSeconds)
{
    if(GState.Owner != &InWorld)
    {
        GState = State();
        GState.Owner = &InWorld;
        GState.ScanClock = 0.f;
    }
    GState.ScanClock -= InSeconds;
    if(GState.ScanClock <= 0.f) { GState.ScanClock = 2.f; Scan(InWorld); }

    const EntityID Local = GetLocalAvatar(InWorld, 0);
    struct Step { EntityID Who; Vector3 Feet; float Volume; bool bRunning; bool bPlayer; };
    std::vector<Step> Steps;
    InWorld.Each<CharacterMoverComponent>([&](Entity InEntity, CharacterMoverComponent& InMover)
    {
        Walker& W = GState.Walkers[InEntity.GetID().Pack()];
        const Vector3 Feet = InEntity.GetWorldPose().Position;
        const LabHealthComponent* Health = InEntity.GetComponent<LabHealthComponent>();
        const bool bUp = !Health || (!Health->bDead && !Health->bDown);
        if(!W.bHave || !bUp || !InMover.bGrounded)
        {
            W.Last = Feet;
            W.bHave = 1;
            if(!InMover.bGrounded) { W.Stride = 0.f; }   // a landing starts a new stride
            return;
        }
        const double Moved = Vector3(Feet.GetX() - W.Last.GetX(), 0, Feet.GetZ() - W.Last.GetZ()).GetLength();
        W.Last = Feet;
        if(Moved > 150.0) { return; }   // a teleport, a checkpoint

        const float Speed = std::max(std::sqrt(InMover.VelX * InMover.VelX + InMover.VelZ * InMover.VelZ),
                                     float(Moved) / std::max(InSeconds, 1e-3f));
        if(Speed < 30.f) { return; }
        const bool bRunning = Speed > InMover.WalkSpeed * 1.2f;
        const double Scale = InEntity.GetWorldPose().Scale.GetX();
        const double Stride = (InMover.bCrouched ? 70.0 : bRunning ? 150.0 : 100.0) * std::max(0.5, Scale);
        W.Stride += float(Moved);
        if(double(W.Stride) < Stride) { return; }
        W.Stride = 0.f;
        float Volume = InMover.bCrouched ? 0.3f : bRunning ? 1.0f : 0.65f;
        if(Scale > 1.1) { Volume = std::min(1.f, Volume * 1.4f); }   // the Brute
        Steps.push_back({ InEntity.GetID(), Feet, Volume, bRunning, InEntity.GetID() == Local });
    });

    for(const Step& S : Steps)
    {
        const ESurface Surface = SurfaceAt(InWorld, S.Feet, S.Who);
        Walker& W = GState.Walkers[S.Who.Pack()];
        uint8_t Variant = uint8_t(1 + NextRandom() % 4u);
        if(Variant == W.LastVariant) { Variant = uint8_t(1 + Variant % 4u); }
        W.LastVariant = Variant;
        char Sound[96];
        std::snprintf(Sound, sizeof(Sound), "Content/Sounds/Footsteps/Step_%s_%02d.nmsound", SURFACE_SOUNDS[Surface], int(Variant));
        PlaySoundAt(InWorld, Sound, S.Feet, S.Volume);
        if(!S.bPlayer) { continue; }
        ++GState.PlayerSteps[Surface];
        /* Running is heard: the Watchmen come to look (their Investigate branch). */
        if(S.bRunning && HasAuthority()) { ReportAINoise(InWorld, S.Feet, 0.25f, S.Who); }
        if(GLabFootstepLog.GetBool())
        {
            char Line[160];
            std::snprintf(Line, sizeof(Line), "[DemoLab] footstep %s at (%.0f, %.0f, %.0f)", SURFACE_NAMES[Surface],
                          S.Feet.GetX(), S.Feet.GetY(), S.Feet.GetZ());
            CORE_LOG(Line)
        }
    }
}

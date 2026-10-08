#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/UI/UISystem.h"

#include "Private/LightLab/LightLabAvatar.h"
#include "Private/LightLab/LightLabGpuTest.h"
#include "Private/LightLab/LightLabPhysics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/*
 * LightLabGame: the lights and shadows demo, built as a game module DLL
 * loaded by name by the engine's launcher (built as LightLab) and the editor's
 * Play-in-Editor. The player flies; the lights do the rest.
 *
 * Demonstrates every light RenderCore has, each with the shadow technique it
 * suits: a cascaded directional sun, a sky light, two Standard-shadowed spot
 * lights (one sweeping), a Modulated point light orbiting the centre pillar,
 * and an unshadowed coloured point light. Post process volumes give a soft
 * glow and FXAA everywhere, and depth of field and motion blur round the
 * arch. T cycles the sun's technique, F toggles [Shadows] Enabled through its
 * config var, and R resets the level (every LightLab level).
 *
 * A second world, BakedLighting (the editor plays it while it is open; a
 * game, with [Game] StartLevel=BakedLighting), is baked lighting's demo: a
 * Cornell-style room whose static walls, boxes and lights are baked (Build >
 * Bake Lightmaps), with a dynamic ball swinging through it lit by the light
 * probes. L switches between the bake and live lighting, through [Rendering]
 * BakedLighting, in either world.
 *
 * Each is a [World_*] in Config/DefaultEngine.ini naming its mode:
 *
 *   LightLabYardMode   Content/Levels/LightLab.nmworld -- the yard, the lights
 *                      and the Player, as the editor saved them: flies the
 *                      Player and animates the lights.
 *   LightLabBakedMode  Content/Levels/BakedLighting.nmworld: flies the Player
 *                      and swings the dynamic ball (Swinger).
 *   LightLabGpuTestMode Content/Levels/GpuTest.nmworld (Tools/make_gpu_test_level.py):
 *                      a town of occluding blocks, PBR props, shadowed lights
 *                      and a herd of foxes (LightLabGpuTest.h); flies the
 *                      Player, or with [LightLab] Benchmark flies a fixed path
 *                      and records it.
 *   LightLabPhysics    PhysicsLab's mode (LightLabPhysics.cpp): stacks,
 *                      dominoes, ramps, mesh colliders, joints and a jump
 *                      pad, simulated by the JoltPhysics module.
 *
 * Both turn the InsetLattice (inset shadows' demo cage) and drift it round
 * where it was placed, so its own shadow map follows a moving caster.
 * LightLab's yard also has the animation demo (LightLab.Animals): a fox on
 * an anim state -- a walk/run blend space, a sit, a masked head layer -- and
 * a walking man carrying a lantern on an Anim Socket, its light pulsing at
 * his footstep notifies (Content/Animation, from Art/Animation's glTFs).
 *   LightLabGameMode   what they share: mouse capture, the T/F/L toggles, a
 *                      periodic report.
 *   LightLabGameModule registers the four modes.
 *   LightLabAvatar     multiplayer: each connected player is an orb the others
 *                      see (LightLabServer, LightLab -connect=host:port).
 *
 * Coordinates are the engine's: left-handed, +X right, +Y UP, +Z forward. The
 * floor is at y=0. A rotation's positive pitch (X) tips the nose down.
 */

namespace
{
    constexpr double DegToRad = 3.14159265358979323846 / 180.0;

    /* Just short of straight along world Y, where the view basis vanishes
     * (EditorCamera.cpp's MaxPitch, for the same reason). */
    constexpr double MaxPitch = 89.0;

    /* Boost triples the fly speed ([LightLab] FlySpeed). */
    constexpr double BoostMultiplier = 3.0;

    /* Seconds between the mode's log reports. */
    constexpr double ReportIntervalSeconds = 5.0;
}

/* The pause menu's Settings page sets these (and [Shadows] Enabled,
 * [Rendering] BakedLighting) through its UI graph's Config nodes; an .ini
 * or the console can too. */
NIMBLE_CONFIG_FLOAT(GLightLabLookSensitivity, "LightLab", "LookSensitivity", 1.0f, nullptr);   // times the Look axes
NIMBLE_CONFIG_FLOAT(GLightLabFlySpeed, "LightLab", "FlySpeed", 600.0f, nullptr);               // world units a second

/* The GpuTest level's: a CSV path makes it fly its benchmark path, record each
 * frame there and quit (LightLabBenchmark); HerdCount makes its herd that many
 * foxes (-1: as placed). Tools/gpu_test_perf.py sets both. */
NIMBLE_CONFIG_STRING(GLightLabBenchmark, "LightLab", "Benchmark", "", nullptr);
NIMBLE_CONFIG_INT(GLightLabHerdCount, "LightLab", "HerdCount", -1, nullptr);
/* With Benchmark: Shots, a folder for each leg's middle frame (an editor
 * build); Freeze, every animation paused and the herd still, so two runs'
 * shots compare pixel for pixel. */
NIMBLE_CONFIG_STRING(GLightLabBenchmarkShots, "LightLab", "BenchmarkShots", "", nullptr);
NIMBLE_CONFIG_BOOL(GLightLabBenchmarkFreeze, "LightLab", "BenchmarkFreeze", false, nullptr);

// === Native prefabs =========================================================
// The level's shapes and its player, declared as prefabs ("native:LightLab.Block"):
// LightLabLevel::OnLoad places them, and the editor lists them under World >
// Native Prefabs. Each shape is a unit mesh, sized by its placement's Scale.

namespace
{
    /* A unit cube (Box.nmmesh, centred), drawn with DefaultLit. Casts. */
    void BuildBlock(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Block");
        Root.Add<GameCore::TransformComponent>();

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        Root.Add(Render);
    }

    /* A unit-diameter sphere (Sphere.nmmesh). Casts. */
    void BuildBall(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Ball");
        Root.Add<GameCore::TransformComponent>();

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Sphere.nmmesh");
        Root.Add(Render);
    }

    /* The ground: a unit plane facing up (+Y). Receives, never casts -- it
     * has nothing beneath it to shadow, and leaving it out keeps it out of
     * every shadow pass. */
    void BuildFloor(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Floor");
        Root.Add<GameCore::TransformComponent>();

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Plane.nmmesh");
        Render.bCastShadows = 0;
        Root.Add(Render);
    }

    /* The flying view: WorldBridge uses the first active CameraComponent, and
     * LightLab.Fly steers it from the Input component (LightLabInput.cpp). */
    void BuildPlayer(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Player");
        Root.Add<GameCore::TransformComponent>();

        RenderCore::CameraComponent Camera;
        Camera.Projection = RenderCore::CP_Perspective;
        Camera.FovY       = 70.0f;
        Camera.ZNear      = 5.0f;
        Camera.ZFar       = 20000.0f;
        Camera.bActive    = 1;
        Root.Add(Camera);

        Root.Add<NimbleEngine::InputComponent>();
    }
}

GAMECORE_DEFINE_NATIVE_PREFAB(Block,  "LightLab.Block",  &BuildBlock)
GAMECORE_DEFINE_NATIVE_PREFAB(Ball,   "LightLab.Ball",   &BuildBall)
GAMECORE_DEFINE_NATIVE_PREFAB(Floor,  "LightLab.Floor",  &BuildFloor)
GAMECORE_DEFINE_NATIVE_PREFAB(Player, "LightLab.Player", &BuildPlayer)

/* LightLabFlyContext (LightLabPhysics.h) is what LightLab.Fly's void* points
 * at. Yaw and Pitch live there rather than being read back from the
 * Transform each frame, so the clamp and wrap are the system's alone. */

/* What LightLab.Swing's void* points at: the dynamic ball, circling where it
 * was placed, in and out of the baked shadows. */
struct LightLabSwingContext
{
    GameCore::EntityID  Swinger;
    EngineCore::Vector3 Center;
    double              Radius         = 170.0;
    double              ElapsedSeconds = 0.0;
};

/* What LightLab.TurnLattice's void* points at: the inset-shadow lattice,
 * turning and drifting round where it was placed. */
struct LightLabLatticeContext
{
    GameCore::EntityID  Lattice;
    EngineCore::Vector3 Center;
    EngineCore::Vector3 Rotation;
    double              ElapsedSeconds = 0.0;
};

/* What LightLab.Animals' void* points at: the animation demo (RenderCore's
 * ANIMATION.md). The fox laps an ellipse, walking then running -- its
 * anim state's Speed is its ground speed, so the blend space follows it --
 * stops, sits (a one-shot that ends on its own) and sets off again; Look
 * brings in its masked head layer while it walks. The man walks a circle,
 * a lantern in his hand on an Anim Socket, its light pulsing at each Step
 * notify his walk fires. */
struct LightLabAnimalsContext
{
    GameCore::EntityID  Fox;
    GameCore::EntityID  Man;
    GameCore::EntityID  Lantern;

    /* Each loop starts where its entity was placed: its east end. */
    EngineCore::Vector3 FoxCenter;
    double              FoxRadiusX = 240.0, FoxRadiusZ = 100.0;
    double              FoxAngle   = 0.0;
    double              FoxSpeed   = 0.0;    // world units a second, along the path
    int                 FoxPhase   = 0;      // 0 walk, 1 run, 2 stopping, 3 sitting
    double              FoxPhaseSeconds = 0.0;

    EngineCore::Vector3 ManCenter;
    double              ManRadius = 110.0;
    double              ManAngle  = 0.0;

    double              LanternIntensity = 6.0;   // as placed
    double              LanternPulse     = 0.0;   // 1 at a footstep, fading
};

/* What LightLab.AnimateLights' void* points at. */
struct LightLabAnimateContext
{
    GameCore::EntityID  SweepSpot;
    GameCore::EntityID  OrbitLight;
    double              ElapsedSeconds = 0.0;

    /* The sweeping spot's resting yaw, and how far either side it swings. */
    double              SweepBaseYaw   = 0.0;
    double              SweepAmplitude = 40.0;

    /* The point light circles this (the centre pillar) at OrbitRadius. */
    EngineCore::Vector3 OrbitCenter;
    double              OrbitRadius    = 350.0;

    /* The ribbon demo's emitter flies round this, so its trail shows. */
    GameCore::EntityID  TrailEmitter;
    EngineCore::Vector3 TrailCenter;
    double              TrailRadius    = 120.0;
};

namespace
{
    std::string Num(uint64_t InValue)   { return std::to_string(InValue); }

    /* Two decimals, without dragging <iomanip> in for one call site. */
    std::string Fixed2(double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.2f", InValue);
        return std::string(Buffer);
    }

    const char* DescribeTechnique(const uint8_t InTechnique)
    {
        switch(InTechnique)
        {
            case RenderCore::LST_Standard:  return "Standard";
            case RenderCore::LST_Modulated: return "Modulated";
            case RenderCore::LST_Cascaded:  return "Cascaded";
            default:                        return "Unknown";
        }
    }

    /* The view direction for a (Pitch, Yaw, 0) rotation, as LightForward
     * computes it: (sin Y cos P, -sin P, cos Y cos P). Positive pitch looks
     * down (-Y). */
    EngineCore::Vector3 FlyForward(const double InPitch, const double InYaw)
    {
        const double P = InPitch * DegToRad;
        const double Y = InYaw * DegToRad;
        return EngineCore::Vector3(std::sin(Y) * std::cos(P), -std::sin(P), std::cos(Y) * std::cos(P));
    }

    /* PrePhysics system: free flight from the Player's Input component.
     * LookX/LookY turn (degrees this frame), the Move axes translate along the
     * view, across it and along world up, faster with Boost. Registered after
     * the engine's input resolve, so it sees this frame's input. */
    void FlySystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<LightLabFlyContext*>(InUserData);
        if(!Context.bEnabled)
        {
            return;
        }
        const GameCore::Entity Player = InWorld.GetEntity(Context.Player);
        GameCore::TransformComponent* Transform = Player.GetComponent<GameCore::TransformComponent>();
        const NimbleEngine::InputComponent* Input = Player.GetComponent<NimbleEngine::InputComponent>();
        if(!Transform || !Input)
        {
            return;
        }

        /* Mouse right turns right (+yaw); mouse down (window y grows) looks
         * down, which is POSITIVE pitch. */
        const double Look = double(GLightLabLookSensitivity.GetFloat());
        Context.Yaw   = std::fmod(Context.Yaw + double(Input->GetAxis("LookX")) * Look, 360.0);
        Context.Pitch = std::clamp(Context.Pitch + double(Input->GetAxis("LookY")) * Look, -MaxPitch, MaxPitch);

        /* EditorCamera's basis: Right = Up(0,1,0) ^ Forward, which is +X for
         * a view along +Z. World up is +Y. */
        const EngineCore::Vector3 Forward = FlyForward(Context.Pitch, Context.Yaw);
        const EngineCore::Vector3 Right   = (EngineCore::Vector3(0.0, 1.0, 0.0) ^ Forward).GetNormalize();
        const EngineCore::Vector3 Up(0.0, 1.0, 0.0);

        const double Speed = double(GLightLabFlySpeed.GetFloat()) * (Input->IsDown("Boost") ? BoostMultiplier : 1.0);
        const EngineCore::Vector3 Velocity = Forward * double(Input->GetAxis("MoveForward"))
                                           + Right   * double(Input->GetAxis("MoveRight"))
                                           + Up      * double(Input->GetAxis("MoveUp"));

        Transform->Position = Transform->Position + Velocity * (Speed * double(InDeltaSeconds));
        Transform->Rotation = EngineCore::Vector3(Context.Pitch, Context.Yaw, 0.0);
    }

    /* PrePhysics system: swings the sweeping spot's yaw back and forth and
     * carries the modulated point light round the centre pillar, so their
     * shadows visibly move. */
    void AnimateLightsSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<LightLabAnimateContext*>(InUserData);
        Context.ElapsedSeconds += double(InDeltaSeconds);
        const double T = Context.ElapsedSeconds;

        if(GameCore::TransformComponent* Spot = InWorld.GetEntity(Context.SweepSpot).GetComponent<GameCore::TransformComponent>())
        {
            const double Yaw = Context.SweepBaseYaw + Context.SweepAmplitude * std::sin(T * 0.5);
            Spot->Rotation.SetY(float(Yaw));
        }

        if(GameCore::TransformComponent* Orbit = InWorld.GetEntity(Context.OrbitLight).GetComponent<GameCore::TransformComponent>())
        {
            /* One lap every ~10 seconds, at a fixed height. */
            const double Angle = T * 0.6;
            Orbit->Position.SetX(float(Context.OrbitCenter.GetX() + Context.OrbitRadius * std::cos(Angle)));
            Orbit->Position.SetZ(float(Context.OrbitCenter.GetZ() + Context.OrbitRadius * std::sin(Angle)));
        }

        if(GameCore::TransformComponent* Trail = InWorld.GetEntity(Context.TrailEmitter).GetComponent<GameCore::TransformComponent>())
        {
            /* A lap every ~3 seconds, rising and falling as it goes: a ribbon behind it. */
            const double Angle = T * 2.1;
            Trail->Position.SetX(float(Context.TrailCenter.GetX() + Context.TrailRadius * std::cos(Angle)));
            Trail->Position.SetY(float(Context.TrailCenter.GetY() + 40.0 * std::sin(Angle * 2.0)));
            Trail->Position.SetZ(float(Context.TrailCenter.GetZ() + Context.TrailRadius * std::sin(Angle)));
        }
    }

    /* A heading's yaw (TransformComponent Y): the forward of FlyForward. */
    double YawOf(const double InX, const double InZ)
    {
        return std::atan2(InX, InZ) / DegToRad;
    }

    /* InCurrent toward InTarget by at most InStep. */
    double Approach(const double InCurrent, const double InTarget, const double InStep)
    {
        return InCurrent < InTarget ? std::min(InCurrent + InStep, InTarget) : std::max(InCurrent - InStep, InTarget);
    }

    /* "AnimNotify" (entity, name): the man's footsteps pulse his lantern. */
    void OnAnimNotify(GameCore::EventID, const GameCore::EventArgs& InArgs, void* InUserData)
    {
        auto& Context = *static_cast<LightLabAnimalsContext*>(InUserData);
        if(InArgs.Count >= 2 && InArgs.Args[0].Entity == Context.Man.Pack() && std::strcmp(InArgs.Args[1].String, "Step") == 0)
        {
            Context.LanternPulse = 1.0;
        }
    }

    /* PrePhysics system, before the animation tick (PostUpdate) reads the
     * parameters it sets: moves the fox and the man and drives their
     * animations. Walk 6 s, run 6 s, stop, sit while Survey plays, again. */
    void AnimalsSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<LightLabAnimalsContext*>(InUserData);
        const double Delta = double(InDeltaSeconds);

        const GameCore::Entity Fox = InWorld.GetEntity(Context.Fox);
        GameCore::TransformComponent*   FoxTransform = Fox.GetComponent<GameCore::TransformComponent>();
        RenderCore::AnimationComponent* FoxAnimation = Fox.GetComponent<RenderCore::AnimationComponent>();
        if(FoxTransform && FoxAnimation)
        {
            Context.FoxPhaseSeconds += Delta;
            double Target = 0.0;
            switch(Context.FoxPhase)
            {
                case 0: Target = 65.0;  if(Context.FoxPhaseSeconds > 6.0) { Context.FoxPhase = 1; Context.FoxPhaseSeconds = 0.0; } break;
                case 1: Target = 160.0; if(Context.FoxPhaseSeconds > 6.0) { Context.FoxPhase = 2; Context.FoxPhaseSeconds = 0.0; } break;
                case 2:
                    Target = 0.0;
                    if(Context.FoxSpeed <= 0.0)
                    {
                        FoxAnimation->SetTrigger("Sit");
                        Context.FoxPhase = 3;
                        Context.FoxPhaseSeconds = 0.0;
                    }
                    break;
                default: Target = 0.0;  if(Context.FoxPhaseSeconds > 4.0) { Context.FoxPhase = 0; Context.FoxPhaseSeconds = 0.0; } break;
            }
            Context.FoxSpeed = Approach(Context.FoxSpeed, Target, 90.0 * Delta);

            /* Along the ellipse at that speed: the angle by the arc length it covers. */
            const double SinA = std::sin(Context.FoxAngle), CosA = std::cos(Context.FoxAngle);
            const double ArcPerRadian = std::max(std::hypot(Context.FoxRadiusX * SinA, Context.FoxRadiusZ * CosA), 1.0);
            Context.FoxAngle += Context.FoxSpeed * Delta / ArcPerRadian;
            FoxTransform->Position = Context.FoxCenter + EngineCore::Vector3(Context.FoxRadiusX * std::cos(Context.FoxAngle), 0.0,
                                                                            Context.FoxRadiusZ * std::sin(Context.FoxAngle));
            /* The fox's clips face -Z, as the man's do: the heading's yaw, turned round. */
            FoxTransform->Rotation.SetY(float(YawOf(-Context.FoxRadiusX * std::sin(Context.FoxAngle),
                                                    Context.FoxRadiusZ * std::cos(Context.FoxAngle)) + 180.0));
            FoxAnimation->SetFloat("Speed", float(Context.FoxSpeed));
            FoxAnimation->SetFloat("Look", float(Approach(FoxAnimation->GetFloat("Look"), Context.FoxPhase == 0 ? 1.0 : 0.0, 1.5 * Delta)));
        }

        /* The man: a walk's pace round his circle. His clip faces -Z, so his yaw is the heading's, turned round. */
        if(GameCore::TransformComponent* ManTransform = InWorld.GetEntity(Context.Man).GetComponent<GameCore::TransformComponent>())
        {
            Context.ManAngle += 90.0 * Delta / Context.ManRadius;
            ManTransform->Position = Context.ManCenter + EngineCore::Vector3(Context.ManRadius * std::cos(Context.ManAngle), 0.0,
                                                                            Context.ManRadius * std::sin(Context.ManAngle));
            ManTransform->Rotation.SetY(float(YawOf(-std::sin(Context.ManAngle), std::cos(Context.ManAngle)) + 180.0));
        }

        if(RenderCore::PointLightComponent* Light = InWorld.GetEntity(Context.Lantern).GetComponent<RenderCore::PointLightComponent>())
        {
            Context.LanternPulse = std::max(0.0, Context.LanternPulse - 4.0 * Delta);
            Light->Intensity = float(Context.LanternIntensity * (1.0 + 0.6 * Context.LanternPulse));
        }
    }

    /* AnimFox, AnimMan and AnimLantern, if InWorld has them, into InOut; false if not. */
    bool FindAnimals(GameCore::World& InWorld, LightLabAnimalsContext& InOut)
    {
        InOut = LightLabAnimalsContext();
        InOut.Fox     = InWorld.FindEntityByName("AnimFox");
        InOut.Man     = InWorld.FindEntityByName("AnimMan");
        InOut.Lantern = InWorld.FindEntityByName("AnimLantern");
        const GameCore::TransformComponent* Fox = InWorld.GetEntity(InOut.Fox).GetComponent<GameCore::TransformComponent>();
        const GameCore::TransformComponent* Man = InWorld.GetEntity(InOut.Man).GetComponent<GameCore::TransformComponent>();
        if(!Fox && !Man)
        {
            return false;
        }
        if(Fox) { InOut.FoxCenter = Fox->Position - EngineCore::Vector3(InOut.FoxRadiusX, 0.0, 0.0); }
        if(Man) { InOut.ManCenter = Man->Position - EngineCore::Vector3(InOut.ManRadius, 0.0, 0.0); }
        if(const RenderCore::PointLightComponent* Light = InWorld.GetEntity(InOut.Lantern).GetComponent<RenderCore::PointLightComponent>())
        {
            InOut.LanternIntensity = double(Light->Intensity);
        }
        return true;
    }

    /* PrePhysics system: carries the Swinger round its starting point, one lap
     * every ~12 seconds, bobbing as it goes. */
    void SwingSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<LightLabSwingContext*>(InUserData);
        Context.ElapsedSeconds += double(InDeltaSeconds);
        GameCore::TransformComponent* Transform = InWorld.GetEntity(Context.Swinger).GetComponent<GameCore::TransformComponent>();
        if(!Transform)
        {
            return;
        }
        const double Angle = Context.ElapsedSeconds * 0.52;
        Transform->Position = EngineCore::Vector3(Context.Center.GetX() + Context.Radius * std::cos(Angle),
                                                  Context.Center.GetY() + 40.0 * std::sin(Angle * 2.0),
                                                  Context.Center.GetZ() + Context.Radius * std::sin(Angle));
    }

    /* InPath into InWorld, by its source path (GameCore::ResolveContentWorld:
     * cooked here or ahead outside the editor). False, logged, if there is
     * none or it cannot be read. */
    /* PrePhysics system: turns the InsetLattice a full turn every 15 seconds
     * about its height, rocking it 10 degrees, and drifts it round a 25-unit
     * circle every 9 seconds, bobbing 10 -- slow enough to follow its
     * shadow's bars, fast enough to cross many shadow-map texels. */
    void TurnLatticeSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<LightLabLatticeContext*>(InUserData);
        Context.ElapsedSeconds += double(InDeltaSeconds);
        GameCore::TransformComponent* Transform = InWorld.GetEntity(Context.Lattice).GetComponent<GameCore::TransformComponent>();
        if(!Transform)
        {
            return;
        }
        const double Time  = Context.ElapsedSeconds;
        const double Drift = Time * (2.0 * 3.14159265358979323846 / 9.0);
        Transform->Position = EngineCore::Vector3(Context.Center.GetX() + 25.0 * std::cos(Drift),
                                                  Context.Center.GetY() + 10.0 * std::sin(Drift * 2.0),
                                                  Context.Center.GetZ() + 25.0 * std::sin(Drift));
        Transform->Rotation = EngineCore::Vector3(Context.Rotation.GetX() + 10.0 * std::sin(Time * 0.7),
                                                  Context.Rotation.GetY() + Time * 24.0,
                                                  Context.Rotation.GetZ());
    }

    /* InsetLattice, if InWorld has one, into InOut; false if not (nothing to turn). */
    bool FindLattice(GameCore::World& InWorld, LightLabLatticeContext& InOut)
    {
        InOut = LightLabLatticeContext();
        InOut.Lattice = InWorld.FindEntityByName("InsetLattice");
        const GameCore::TransformComponent* Placed = InWorld.GetEntity(InOut.Lattice).GetComponent<GameCore::TransformComponent>();
        if(!InOut.Lattice.IsSet() || !Placed)
        {
            return false;
        }
        InOut.Center   = Placed->Position;
        InOut.Rotation = Placed->Rotation;
        return true;
    }

}

void LightLabStartFly(GameCore::World& InWorld, const GameCore::EntityID InPlayer, LightLabFlyContext& InContext)
{
    InContext = LightLabFlyContext();
    InContext.Player = InPlayer;
    if(const GameCore::TransformComponent* View = InWorld.GetEntity(InPlayer).GetComponent<GameCore::TransformComponent>())
    {
        InContext.Pitch = std::clamp(double(View->Rotation.GetX()), -MaxPitch, MaxPitch);
        InContext.Yaw   = double(View->Rotation.GetY());
    }
    InWorld.RegisterSystem("LightLab.Fly", GameCore::TP_PrePhysics, &FlySystem, &InContext);
}

/*
 * The pause menu: Escape (or Start) opens it and pauses the World. What it
 * does is its UI graph, Content/UI/PauseMenu.ui.nmgraph (the document's
 * Graph; UI/README.md, "UI graphs"): Resume and Back close it, unpause and
 * take the mouse back; its Settings page's check boxes and sliders, bound
 * two-way to LightLab.* data store keys, are filled from the config vars as
 * it opens and set them as they change -- the vars T, F and L, an .ini or the
 * console set. Quit is the engine's Game.Quit.
 */
namespace
{
    const char* const PauseMenuPath = "Content/UI/PauseMenu.nmui";
}

/* What every LightLab world shares: mouse capture, the T/F/L toggles, the
 * pause menu, reset and a periodic report. Each world's mode (below) adds its
 * own systems in OnLoad. */
class LightLabGameMode : public NimbleEngine::IGameMode
{
public:
    /* The world is loaded: its Player (flown) and Sun (T cycles it) by name. */
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        GameCore::World& World = InSession.GetWorld();
        PlayerEntity = World.FindEntityByName("Player");
        SunEntity    = World.FindEntityByName("Sun");
        if(!PlayerEntity.IsSet())
        {
            CORE_LOG_ERROR("[LightLab] the world has no entity named Player; nothing to fly")
        }
        return true;
    }

    void OnStart() override
    {
        /* Mouse-look needs the cursor held; the pause menu lets it go, a click takes it back. */
        Session->SetMouseCaptured(true);

        CORE_LOG("[LightLab] fly with WASD + mouse, E/Q up and down, Shift to boost; "
                 "T cycles the sun's shadow technique, F toggles shadows, L switches baked and live lighting, "
                 "R resets the level, Escape pauses")
    }

    /* The GpuTest level's benchmark, recorded each tick; null: none. */
    void SetBenchmark(LightLabBenchmark* InBenchmark) { Benchmark = InBenchmark; }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        if(Benchmark) { Benchmark->Record(*Session, InFrame); }

        /* Concurrent with the render thread: no renderer or window access. */
        GameCore::World& World = Session->GetWorld();
        ElapsedSeconds += InFrame.DeltaSeconds;
        TicksSinceReport++;

        /* Last frame's resolve: OnTick runs before World::Tick. Each edge is
         * still seen exactly once. */
        if(const NimbleEngine::InputComponent* Input = World.GetEntity(PlayerEntity).GetComponent<NimbleEngine::InputComponent>())
        {
            if(Input->WasPressed("CycleTechnique"))
            {
                CycleSunTechnique(World);
            }

            if(Input->WasPressed("ToggleShadows"))
            {
                ToggleShadows();
            }

            if(Input->WasPressed("ToggleBakedLighting"))
            {
                ToggleBakedLighting();
            }

            if(Input->WasPressed("Pause"))
            {
                OpenPauseMenu();
            }
            else if(!Session->IsMouseCaptured() && Input->WasPressed("CaptureMouse"))
            {
                Session->SetMouseCaptured(true);
            }

            /* R: the level again, as it loaded -- the World is rebuilt at the
             * next frame's ApplyPendingLevel, never inside its own tick. */
            if(Input->WasPressed("ResetLevel") && Session->GetLevel())
            {
                CORE_LOG(std::string("[LightLab] resetting '") + Session->GetLevel()->GetName() + "'")
                Session->RequestLevel(*Session->GetLevel());
                return;
            }
        }

        /* Connected to a server: tell it where this camera is (LightLabAvatar.h). */
        LightLabAvatar::Update(World, PlayerEntity, InFrame.DeltaSeconds);

        if(ElapsedSeconds - LastReportSeconds >= ReportIntervalSeconds)
        {
            const GameCore::TransformComponent* Player =
                World.GetEntity(PlayerEntity).GetComponent<GameCore::TransformComponent>();

            RenderCore::RenderStats RenderStats;
            Session->GetRenderer().GetStats(RenderStats);

            const double Seconds = ElapsedSeconds - LastReportSeconds;
            CORE_LOG("[LightLab] t=" + Fixed2(ElapsedSeconds) + "s"
                     + " fps=" + Fixed2(double(TicksSinceReport) / Seconds)
                     + (Player ? " player=(" + Fixed2(Player->Position.GetX()) + ", "
                                 + Fixed2(Player->Position.GetY()) + ", "
                                 + Fixed2(Player->Position.GetZ()) + ")"
                               : std::string())
                     + " frames_rendered=" + Num(RenderStats.FramesRendered)
                     + " draw_calls=" + Num(RenderStats.DrawCalls)
                     + (NimbleEngine::GetNetMode() != NimbleEngine::ENetMode::Standalone
                            ? " remote_players=" + Num(LightLabAvatar::CountRemote(World)) : std::string()))

            LastReportSeconds = ElapsedSeconds;
            TicksSinceReport  = 0;
        }
    }

    void OnUnload() override
    {
        CORE_LOG("[LightLab] stopping after " + Fixed2(ElapsedSeconds) + "s")
        Session = nullptr;
    }

    /* Server: a client joined; its player gets an orb the others see. */
    void OnClientConnected(const int32_t InClient, const GameCore::EntityID InPlayer) override
    {
        LightLabAvatar::Add(Session->GetWorld(), InPlayer);
        CORE_LOG("[LightLab] client " + std::to_string(InClient) + " joined")
    }

    void OnClientDisconnected(const int32_t InClient, const GameCore::EntityID /*InPlayer*/) override
    {
        CORE_LOG("[LightLab] client " + std::to_string(InClient) + " left")
    }

protected:
    /* Paused under the menu, the cursor free for it. Without the menu (a
     * Client build with nothing cooked) Escape only frees the mouse, as it
     * used to: a pause nothing can resume would be a hang. */
    void OpenPauseMenu()
    {
        Session->SetMouseCaptured(false);
        if(Session->GetUI().Open(PauseMenuPath) == UI::UI_DOCUMENT_INVALID)
        {
            CORE_LOG_WARN(std::string("[LightLab] no pause menu (") + PauseMenuPath + "); the mouse is free, a click takes it back")
            return;
        }
        Session->SetPaused(true);
    }

    /* Standard -> Modulated -> Cascaded -> Standard on the sun. */
    void CycleSunTechnique(GameCore::World& InWorld)
    {
        RenderCore::DirectionalLightComponent* Sun =
            InWorld.GetEntity(SunEntity).GetComponent<RenderCore::DirectionalLightComponent>();
        if(!Sun)
        {
            return;
        }

        Sun->ShadowTechnique = uint8_t((Sun->ShadowTechnique + 1) % (RenderCore::LST_Cascaded + 1));
        CORE_LOG(std::string("[LightLab] sun shadow technique: ") + DescribeTechnique(Sun->ShadowTechnique))
    }

    /* [Shadows] Enabled through the config registry, the same var the .ini
     * and the console set; WorldBridge reads it each frame. */
    void ToggleShadows()
    {
        EngineCore::ConfigVar* Enabled = EngineCore::ConfigFindVarByPath("Shadows.Enabled");
        if(!Enabled)
        {
            CORE_LOG_ERROR("[LightLab] no Shadows.Enabled config var to toggle")
            return;
        }

        const bool bNowEnabled = !Enabled->GetBool();
        Enabled->SetFromString(bNowEnabled ? "1" : "0");
        CORE_LOG(std::string("[LightLab] shadows ") + (bNowEnabled ? "enabled" : "disabled"))
    }

    /* [Rendering] BakedLighting, as ToggleShadows does [Shadows] Enabled:
     * draw with the world's bake (Build > Bake Lightmaps), or every light live. */
    void ToggleBakedLighting()
    {
        EngineCore::ConfigVar* Baked = EngineCore::ConfigFindVarByPath("Rendering.BakedLighting");
        if(!Baked)
        {
            CORE_LOG_ERROR("[LightLab] no Rendering.BakedLighting config var to toggle")
            return;
        }

        const bool bNowBaked = !Baked->GetBool();
        Baked->SetFromString(bNowBaked ? "1" : "0");
        CORE_LOG(std::string("[LightLab] lighting: ") + (bNowBaked ? "baked" : "lit live"))
    }

    /* Stashed in OnLoad. */
    NimbleEngine::GameSession* Session = nullptr;

    /* Found by name in OnLoad. */
    GameCore::EntityID PlayerEntity;
    GameCore::EntityID SunEntity;

    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    uint64_t TicksSinceReport  = 0;

    LightLabBenchmark* Benchmark = nullptr;
};

/* Content/Levels/LightLab.nmworld -- the yard, the lights and the Player, as
 * the editor saved them: flies the Player and animates the lights. */
class LightLabYardMode final : public LightLabGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        LightLabGameMode::OnLoad(InSession);
        GameCore::World& World = InSession.GetWorld();

        AnimateContext.SweepSpot    = World.FindEntityByName("SweepSpot");
        AnimateContext.OrbitLight   = World.FindEntityByName("OrbitLight");
        AnimateContext.TrailEmitter = World.FindEntityByName("TrailDemo");

        /* Start from where the world placed them. */
        if(const GameCore::TransformComponent* Sweep = World.GetEntity(AnimateContext.SweepSpot).GetComponent<GameCore::TransformComponent>())
        {
            AnimateContext.SweepBaseYaw = double(Sweep->Rotation.GetY());
        }
        if(const GameCore::TransformComponent* Orbit = World.GetEntity(AnimateContext.OrbitLight).GetComponent<GameCore::TransformComponent>())
        {
            /* It circles the centre pillar at the height and distance it was placed. */
            const EngineCore::Vector3 Start = Orbit->Position;
            AnimateContext.OrbitCenter = EngineCore::Vector3(0.0, Start.GetY(), 0.0);
            AnimateContext.OrbitRadius = std::max(std::hypot(double(Start.GetX()), double(Start.GetZ())), 1.0);
        }
        if(const GameCore::TransformComponent* Trail = World.GetEntity(AnimateContext.TrailEmitter).GetComponent<GameCore::TransformComponent>())
        {
            /* It circles a point TrailRadius toward -X of where it was placed (its circle's east end). */
            AnimateContext.TrailCenter = Trail->Position - EngineCore::Vector3(AnimateContext.TrailRadius, 0.0, 0.0);
        }

        LightLabStartFly(World, PlayerEntity, FlyContext);
        World.RegisterSystem("LightLab.AnimateLights", GameCore::TP_PrePhysics, &AnimateLightsSystem, &AnimateContext);
        if(FindLattice(World, LatticeContext))
        {
            World.RegisterSystem("LightLab.TurnLattice", GameCore::TP_PrePhysics, &TurnLatticeSystem, &LatticeContext);
        }
        if(FindAnimals(World, AnimalsContext))
        {
            World.RegisterSystem("LightLab.Animals", GameCore::TP_PrePhysics, &AnimalsSystem, &AnimalsContext);
            World.GetEvents().Subscribe("AnimNotify", &OnAnimNotify, &AnimalsContext);
        }
        return true;
    }

private:
    LightLabFlyContext     FlyContext;
    LightLabAnimateContext AnimateContext;
    LightLabLatticeContext LatticeContext;
    LightLabAnimalsContext AnimalsContext;
};

/* Baked lighting's demo room (Content/Levels/BakedLighting.nmworld, made by
 * Tools/make_baked_lighting_level.py, then Build > Bake Lightmaps): flies the
 * Player and swings the dynamic ball (Swinger). */
class LightLabBakedMode final : public LightLabGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        LightLabGameMode::OnLoad(InSession);
        GameCore::World& World = InSession.GetWorld();

        SwingContext.Swinger = World.FindEntityByName("Swinger");
        if(const GameCore::TransformComponent* Ball = World.GetEntity(SwingContext.Swinger).GetComponent<GameCore::TransformComponent>())
        {
            SwingContext.Center = Ball->Position;
        }

        LightLabStartFly(World, PlayerEntity, FlyContext);
        World.RegisterSystem("LightLab.Swing", GameCore::TP_PrePhysics, &SwingSystem, &SwingContext);
        if(FindLattice(World, LatticeContext))
        {
            World.RegisterSystem("LightLab.TurnLattice", GameCore::TP_PrePhysics, &TurnLatticeSystem, &LatticeContext);
        }
        return true;
    }

private:
    LightLabFlyContext     FlyContext;
    LightLabSwingContext   SwingContext;
    LightLabLatticeContext LatticeContext;
};

/* The GPU test world (Content/Levels/GpuTest.nmworld): its herd walks;
 * the Player flies, or follows the benchmark's path ([LightLab] Benchmark). */
class LightLabGpuTestMode final : public LightLabGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        LightLabGameMode::OnLoad(InSession);
        GameCore::World& World = InSession.GetWorld();

        const bool bFreeze = GLightLabBenchmarkFreeze.GetBool();
        if(LightLabFindHerd(World, GLightLabHerdCount.GetInt(), HerdContext) && !bFreeze)
        {
            World.RegisterSystem("LightLab.Herd", GameCore::TP_PrePhysics, &LightLabHerdSystem, &HerdContext);
        }
        if(bFreeze)
        {
            LightLabFreezeAnimation(World);
        }
        CORE_LOG("[LightLab] GpuTest: a herd of " + std::to_string(HerdContext.Members.size()) + " foxes")

        const std::string Csv = GLightLabBenchmark.GetString();
        if(!Csv.empty() && BenchmarkRun.Begin(PlayerEntity, Csv, GLightLabBenchmarkShots.GetString()))
        {
            World.RegisterSystem("LightLab.Benchmark", GameCore::TP_PrePhysics, &LightLabBenchmark::System, &BenchmarkRun);
            SetBenchmark(&BenchmarkRun);
        }
        else
        {
            LightLabStartFly(World, PlayerEntity, FlyContext);
        }
        return true;
    }

private:
    LightLabFlyContext  FlyContext;
    LightLabHerdContext HerdContext;
    LightLabBenchmark   BenchmarkRun;
};

/* Registers the modes the project's world list names (Config/DefaultEngine.ini). */
class LightLabGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "LightLabGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        NimbleEngine::RegisterGameMode<LightLabYardMode>("LightLab");
        NimbleEngine::RegisterGameMode<LightLabBakedMode>("BakedLighting");
        NimbleEngine::RegisterGameMode<LightLabGpuTestMode>("GpuTest");
        LightLabPhysics::RegisterGameMode();
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds this image's functions, and it is about to go. */
        LightLabPhysics::UnregisterGameMode();
        NimbleEngine::UnregisterGameMode("GpuTest");
        NimbleEngine::UnregisterGameMode("BakedLighting");
        NimbleEngine::UnregisterGameMode("LightLab");
    }
};

NIMBLE_IMPLEMENT_GAME_MODULE(LightLabGameModule, "LightLabGame")

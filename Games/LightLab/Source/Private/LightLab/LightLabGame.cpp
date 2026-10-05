#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/UI/UISystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
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
 * config var.
 *
 * A second level, BakedLighting (the editor plays it while its world is open,
 * ILevel::GetWorldFile; a game, with [Game] StartLevel=BakedLighting), is baked
 * lighting's demo: a Cornell-style room whose static walls, boxes and lights
 * are baked (Build > Bake Lightmaps), with a dynamic ball swinging through it
 * lit by the light probes. L switches between the bake and live lighting,
 * through [Rendering] BakedLighting, in either level.
 *
 *   LightLabLevel      loads Content/Levels/LightLab.nmworld -- the yard, the lights and
 *                      the Player, as the editor saved them -- or, without one,
 *                      builds the same scene in code; registers the fly and
 *                      light-animation systems, owns the mode.
 *   LightLabBakedLevel loads Content/Levels/BakedLighting.nmworld; flies the
 *                      Player and swings the dynamic ball (Swinger).
 *   LightLabGameMode   mouse capture, the T/F/L toggles, a periodic report.
 *   LightLabGameModule owns and registers both levels.
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

    /* The level as the editor saves it. Its entities are found by name:
     * Player, Sun, SweepSpot and OrbitLight drive the systems below. */
    const char* const LevelFile = "Content/Levels/LightLab.nmworld";

    /* Baked lighting's demo, made by Tools/make_baked_lighting_level.py and
     * baked in the editor. Player, Sun and Swinger are found by name. */
    const char* const BakedLevelFile = "Content/Levels/BakedLighting.nmworld";

    const char* const BoxMesh    = "Engine/Content/Meshes/Box.nmmesh";
    const char* const SphereMesh = "Engine/Content/Meshes/Sphere.nmmesh";
    const char* const PlaneMesh  = "Engine/Content/Meshes/Plane.nmmesh";
    const char* const CylinderMesh = "Engine/Content/Meshes/Cylinder.nmmesh";

    /* A prefab from a file (Content/Prefabs): a Cylinder shaft on a Box base
     * under a Box capital, 440 high, standing on its root. */
    const char* const ColumnPrefab = "Content/Prefabs/Column.nmprefab";
}

/* An editor build only: when set, the level is built in code (not loaded) and
 * written to this path as a text world before the run starts -- how
 * Content/Levels/LightLab.nmworld was made. Relative to the executable. */
NIMBLE_CONFIG_STRING(GLightLabExportLevel, "LightLab", "ExportLevel", "", nullptr);

/* The pause menu's Settings page sets these (and [Shadows] Enabled,
 * [Rendering] BakedLighting) through its UI graph's Config nodes; an .ini
 * or the console can too. */
NIMBLE_CONFIG_FLOAT(GLightLabLookSensitivity, "LightLab", "LookSensitivity", 1.0f, nullptr);   // times the Look axes
NIMBLE_CONFIG_FLOAT(GLightLabFlySpeed, "LightLab", "FlySpeed", 600.0f, nullptr);               // world units a second

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

    GameCore::TransformPose Placed(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InScale,
                                   const EngineCore::Vector3& InRotation = EngineCore::Vector3(0.0, 0.0, 0.0))
    {
        GameCore::TransformPose Pose;
        Pose.Position = InPosition;
        Pose.Rotation = InRotation;
        Pose.Scale    = InScale;
        return Pose;
    }
}

GAMECORE_DEFINE_NATIVE_PREFAB(Block,  "LightLab.Block",  &BuildBlock)
GAMECORE_DEFINE_NATIVE_PREFAB(Ball,   "LightLab.Ball",   &BuildBall)
GAMECORE_DEFINE_NATIVE_PREFAB(Floor,  "LightLab.Floor",  &BuildFloor)
GAMECORE_DEFINE_NATIVE_PREFAB(Player, "LightLab.Player", &BuildPlayer)

/* What LightLab.Fly's void* points at. Yaw and Pitch live here rather than
 * being read back from the Transform each frame, so the clamp and wrap are
 * the system's alone. */
struct LightLabFlyContext
{
    GameCore::EntityID Player;
    double             Yaw   = 0.0;
    double             Pitch = 0.0;
};

/* What LightLab.Swing's void* points at: the dynamic ball, circling where it
 * was placed, in and out of the baked shadows. */
struct LightLabSwingContext
{
    GameCore::EntityID  Swinger;
    EngineCore::Vector3 Center;
    double              Radius         = 170.0;
    double              ElapsedSeconds = 0.0;
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

    /* InPath into InWorld -- in a game build, its cooked twin in
     * Content/Levels/Cooked/. False, logged, if there is none or it cannot be read. */
    bool LoadWorld(const char* InPath, GameCore::World& InWorld)
    {
        std::string Path = InPath;
#if !WITH_EDITOR
        const size_t Slash = Path.find_last_of('/');
        Path = Path.substr(0, Slash + 1) + "Cooked/" + Path.substr(Slash + 1);
#endif
        if(GameCore::DetectWorldFileFormat(Path.c_str()) == GameCore::EWorldFileFormat::Unknown)
        {
            CORE_LOG("[LightLab] no " + Path)
            return false;
        }
        if(!GameCore::LoadWorldFile(Path.c_str(), InWorld))
        {
            CORE_LOG_WARN("[LightLab] could not read " + Path)
            return false;
        }
        CORE_LOG("[LightLab] loaded " + Path)
        return true;
    }

    /* An entity with a Transform at InPosition/InRotation and one light
     * component. Lights are made here rather than as native prefabs: each is
     * configured differently, which a placement cannot express. */
    template<typename TLight>
    GameCore::Entity SpawnLight(GameCore::World& InWorld, const char* InName, const EngineCore::Vector3& InPosition,
                                const EngineCore::Vector3& InRotation, const TLight& InLight)
    {
        const GameCore::Entity Light = InWorld.CreateEntity(InName);

        GameCore::TransformComponent Transform;
        Transform.Position = InPosition;
        Transform.Rotation = InRotation;
        Light.AddComponent(Transform);
        Light.AddComponent(InLight);
        return Light;
    }
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

class LightLabGameMode : public NimbleEngine::IGameMode
{
public:
    LightLabGameMode(const GameCore::EntityID InPlayer, const GameCore::EntityID InSun)
        : PlayerEntity(InPlayer), SunEntity(InSun) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        return true;
    }

    void OnStart() override
    {
        /* Mouse-look needs the cursor held; the pause menu lets it go, a click takes it back. */
        Session->SetMouseCaptured(true);

        CORE_LOG("[LightLab] fly with WASD + mouse, E/Q up and down, Shift to boost; "
                 "T cycles the sun's shadow technique, F toggles shadows, L switches baked and live lighting, "
                 "Escape pauses")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
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
        }

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
                     + " draw_calls=" + Num(RenderStats.DrawCalls))

            LastReportSeconds = ElapsedSeconds;
            TicksSinceReport  = 0;
        }
    }

    void OnUnload() override
    {
        CORE_LOG("[LightLab] stopping after " + Fixed2(ElapsedSeconds) + "s")
        Session = nullptr;
    }

private:
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

    /* From the level. */
    GameCore::EntityID PlayerEntity;
    GameCore::EntityID SunEntity;

    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    uint64_t TicksSinceReport  = 0;
};

/* The scene: a floor, a yard of blocks and balls, every kind of light, and
 * the Player. Registers the fly and animation systems and owns the mode. */
class LightLabLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "LightLab"; }
    const char* GetWorldFile() const override { return LevelFile; }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        (void)InSession;

        /* GetWorld(), not InSession.GetWorld() (Level.h). */
        GameCore::World& World = GetWorld();

        /* The level object outlives a run (a second Play-in-Editor loads it
         * again), so last run's system state goes first. */
        FlyContext     = LightLabFlyContext();
        AnimateContext = LightLabAnimateContext();

        const std::string Export = GLightLabExportLevel.GetString();
        if(!Export.empty() || !LoadLevelFile(World))
        {
            SpawnGeometry(World);
            SpawnLights(World);
            SpawnPostProcess(World);
            SpawnPlayer(World);
            if(!Export.empty())
            {
                ExportLevel(World, Export);
            }
        }

        /* The scene's named entities, wherever they came from. */
        const GameCore::EntityID Player = World.FindEntityByName("Player");
        SunEntity                       = World.FindEntityByName("Sun");
        AnimateContext.SweepSpot        = World.FindEntityByName("SweepSpot");
        AnimateContext.OrbitLight       = World.FindEntityByName("OrbitLight");
        AnimateContext.TrailEmitter     = World.FindEntityByName("TrailDemo");
        if(!Player.IsSet())
        {
            CORE_LOG_ERROR("[LightLab] the level has no entity named Player; nothing to fly")
        }

        /* Start from where the level placed them. */
        if(const GameCore::TransformComponent* View = World.GetEntity(Player).GetComponent<GameCore::TransformComponent>())
        {
            FlyContext.Pitch = std::clamp(double(View->Rotation.GetX()), -MaxPitch, MaxPitch);
            FlyContext.Yaw   = double(View->Rotation.GetY());
        }
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

        FlyContext.Player = Player;
        World.RegisterSystem("LightLab.Fly", GameCore::TP_PrePhysics, &FlySystem, &FlyContext);
        World.RegisterSystem("LightLab.AnimateLights", GameCore::TP_PrePhysics, &AnimateLightsSystem, &AnimateContext);

        /* Here, not a member initialiser: needs the Player and the Sun. */
        Mode.emplace(Player, SunEntity);

        return true;
    }

    NimbleEngine::IGameMode* GetGameMode() override
    {
        return Mode ? &*Mode : nullptr;
    }

private:
    /* Content/Levels/LightLab.nmworld into InWorld -- in a game build, its
     * cooked twin in Content/Levels/Cooked/, as BrickBreaker does. False (and
     * InWorld emptied) if there is none or it cannot be read, so the level
     * is built in code instead. */
    bool LoadLevelFile(GameCore::World& InWorld)
    {
        std::string Path = LevelFile;
#if !WITH_EDITOR
        const size_t Slash = Path.find_last_of('/');
        Path = Path.substr(0, Slash + 1) + "Cooked/" + Path.substr(Slash + 1);
#endif
        if(GameCore::DetectWorldFileFormat(Path.c_str()) == GameCore::EWorldFileFormat::Unknown)
        {
            CORE_LOG("[LightLab] no " + Path + "; building the level in code")
            return false;
        }
        if(GameCore::LoadWorldFile(Path.c_str(), InWorld))
        {
            CORE_LOG("[LightLab] loaded " + Path)
            return true;
        }

        CORE_LOG_WARN("[LightLab] could not read " + Path + "; building the level in code")
        std::vector<GameCore::EntityID> Partial;
        InWorld.ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
        }, &Partial);
        for(const GameCore::EntityID Each : Partial)
        {
            InWorld.DestroyEntity(Each);
        }
        return false;
    }

    /* [LightLab] ExportLevel: InWorld as a text world at InPath. */
    void ExportLevel(GameCore::World& InWorld, const std::string& InPath)
    {
#if WITH_EDITOR
        InWorld.RefreshSpatialIndex();
        if(GameCore::SaveWorldPackage(InPath.c_str(), InWorld))
        {
            CORE_LOG("[LightLab] exported the level to " + InPath)
        }
        else
        {
            CORE_LOG_ERROR("[LightLab] could not export the level to " + InPath)
        }
#else
        (void)InWorld;
        CORE_LOG_WARN("[LightLab] ExportLevel needs an editor build (" + InPath + " not written)")
#endif
    }

    /* A named entity with a Transform and a Render component drawing
     * InMesh -- plain, not a native prefab instance, so a saved level does
     * not need this module loaded to show it. */
    static void SpawnShape(GameCore::World& InWorld, const char* InName, const char* InMesh, const bool bInCasts,
                           const GameCore::TransformPose& InPose, const char* InMaterial = "")
    {
        const GameCore::Entity Shape = InWorld.CreateEntity(InName);
        GameCore::TransformComponent Transform;
        Transform.Position = InPose.Position;
        Transform.Rotation = InPose.Rotation;
        Transform.Scale    = InPose.Scale;
        Shape.AddComponent(Transform);

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InMesh);
        std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", InMaterial);
        Render.bCastShadows = bInCasts ? 1 : 0;
        Shape.AddComponent(Render);
    }

    /*
     * A road flare standing in the yard: a red stick, a glowing ball for
     * the flame (emissive far past 1, so Bloom spreads it), a red light it
     * casts, and its smoke -- a Particle System of soft puffs, born red and
     * blackening as they rise and spread (RenderCore's
     * ParticleSystemComponent; Content/Particles/FlareSmoke.nmparticle,
     * Content/Materials/Flare*.material.nmgraph).
     */
    static void SpawnFlare(GameCore::World& InWorld)
    {
        const EngineCore::Vector3 Base(-150.0, 0.0, -750.0);
        SpawnShape(InWorld, "FlareStick", CylinderMesh, true,
                   Placed(Base + EngineCore::Vector3(0.0, 70.0, 0.0), EngineCore::Vector3(16.0, 140.0, 16.0)),
                   "Content/Materials/FlareBody.material.nmgraph");
        SpawnShape(InWorld, "FlareFlame", SphereMesh, false,
                   Placed(Base + EngineCore::Vector3(0.0, 152.0, 0.0), EngineCore::Vector3(30.0, 30.0, 30.0)),
                   "Content/Materials/FlareFlame.material.nmgraph");

        RenderCore::PointLightComponent Light;
        Light.Color        = EngineCore::Color(255, 70, 35);
        Light.Intensity    = 12.0f;
        Light.Radius       = 700.0f;
        Light.bCastShadows = 0;
        SpawnLight(InWorld, "FlareLight", Base + EngineCore::Vector3(0.0, 175.0, 0.0), EngineCore::Vector3(0.0, 0.0, 0.0), Light);

        const GameCore::Entity Smoke = InWorld.CreateEntity("FlareSmoke");
        GameCore::TransformComponent Transform;
        Transform.Position = Base + EngineCore::Vector3(0.0, 168.0, 0.0);
        Smoke.AddComponent(Transform);
        RenderCore::ParticleSystemComponent Particles;
        std::snprintf(Particles.System, sizeof(Particles.System), "%s", "Content/Particles/FlareSmoke.nmparticle");
        Smoke.AddComponent(Particles);
    }

    /* A Particle System running InSystem at InPosition, named InName. */
    static RenderCore::ParticleSystemComponent* SpawnParticles(GameCore::World& InWorld, const char* InName,
                                                               const EngineCore::Vector3& InPosition, const char* InSystem)
    {
        const GameCore::Entity Entity = InWorld.CreateEntity(InName);
        GameCore::TransformComponent Transform;
        Transform.Position = InPosition;
        Entity.AddComponent(Transform);
        RenderCore::ParticleSystemComponent Particles;
        std::snprintf(Particles.System, sizeof(Particles.System), "%s", InSystem);
        return Entity.AddComponent(Particles);
    }

    /*
     * A sample of each particle type across the front of the yard
     * (Content/Particles/; RenderCore's RENDERPIPELINE.md, "Particles"):
     * sparks bursting in front of the left pillars, debris bursting by the ball,
     * a laser strung between the right pillars (its Beam Target, set here,
     * overrides the asset's own end), and a ribbon trailing a point that
     * AnimateLights flies round in a circle.
     */
    static void SpawnParticleDemos(GameCore::World& InWorld)
    {
        SpawnParticles(InWorld, "SparksDemo", EngineCore::Vector3(-650.0, 0.0, -820.0), "Content/Particles/Sparks.nmparticle");
        SpawnParticles(InWorld, "DebrisDemo", EngineCore::Vector3(420.0, 0.0, -680.0), "Content/Particles/Debris.nmparticle");
        if(RenderCore::ParticleSystemComponent* Laser =
               SpawnParticles(InWorld, "LaserDemo", EngineCore::Vector3(425.0, 260.0, -1000.0), "Content/Particles/Laser.nmparticle"))
        {
            Laser->bUseBeamTarget = 1;
            Laser->BeamTarget     = EngineCore::Vector3(775.0, 260.0, -1000.0);
        }
        SpawnParticles(InWorld, "TrailDemo", EngineCore::Vector3(-300.0, 140.0, -600.0), "Content/Particles/Trail.nmparticle");
    }

    /* The flying view, as LightLab.Player: above and behind the yard,
     * looking down into it along +Z. */
    static void SpawnPlayer(GameCore::World& InWorld)
    {
        const GameCore::Entity Player = InWorld.CreateEntity("Player");
        GameCore::TransformComponent Transform;
        Transform.Position = EngineCore::Vector3(0.0, 900.0, -1900.0);
        Transform.Rotation = EngineCore::Vector3(25.0, 0.0, 0.0);
        Player.AddComponent(Transform);

        RenderCore::CameraComponent Camera;
        Camera.Projection = RenderCore::CP_Perspective;
        Camera.FovY       = 70.0f;
        Camera.ZNear      = 5.0f;
        Camera.ZFar       = 20000.0f;
        Player.AddComponent(Camera);

        Player.AddComponent<NimbleEngine::InputComponent>();
    }

    /* Post process volumes (Content/PostProcess): a soft glow and FXAA
     * everywhere, and round the arch, depth of field and motion blur that
     * blend in as the camera nears it. */
    static void SpawnPostProcess(GameCore::World& InWorld)
    {
        const GameCore::Entity Global = InWorld.CreateEntity("PostProcessGlobal");
        GameCore::TransformComponent Anywhere;
        Global.AddComponent(Anywhere);
        RenderCore::PostProcessComponent Everywhere;
        std::snprintf(Everywhere.Stack, sizeof(Everywhere.Stack), "%s", "Content/PostProcess/Global.pp.nmgraph");
        Global.AddComponent(Everywhere);

        const GameCore::Entity Arch = InWorld.CreateEntity("PostProcessArch");
        GameCore::TransformComponent AtArch;
        AtArch.Position = EngineCore::Vector3(-350.0, 200.0, 400.0);
        Arch.AddComponent(AtArch);
        GameCore::BoundsComponent Volume;
        Volume.Local = GameCore::AABB(EngineCore::Vector3(-300.0, -200.0, -250.0), EngineCore::Vector3(300.0, 250.0, 250.0));
        Arch.AddComponent(Volume);
        RenderCore::PostProcessComponent UnderArch;
        std::snprintf(UnderArch.Stack, sizeof(UnderArch.Stack), "%s", "Content/PostProcess/Arch.pp.nmgraph");
        UnderArch.Priority      = 1;
        UnderArch.BlendDistance = 300.0f;
        Arch.AddComponent(UnderArch);
    }

    /* Sizes are world units; a block of height H stands on the floor at
     * y = H/2. The floor receives but never casts. */
    void SpawnGeometry(GameCore::World& InWorld)
    {
        SpawnShape(InWorld, "Floor", PlaneMesh, false,
                   Placed(EngineCore::Vector3(0.0, 0.0, 0.0), EngineCore::Vector3(6000.0, 1.0, 6000.0)));

        /* The centre pillar the orbiting point light circles. */
        SpawnShape(InWorld, "CentrePillar", BoxMesh, true,
                   Placed(EngineCore::Vector3(0.0, 300.0, 0.0), EngineCore::Vector3(120.0, 600.0, 120.0)));

        /* A long back wall and a side wall: big receivers for the sun's
         * cascades and the spots. */
        SpawnShape(InWorld, "BackWall", BoxMesh, true,
                   Placed(EngineCore::Vector3(0.0, 150.0, 800.0), EngineCore::Vector3(1600.0, 300.0, 60.0)));
        SpawnShape(InWorld, "SideWall", BoxMesh, true,
                   Placed(EngineCore::Vector3(-900.0, 125.0, 100.0), EngineCore::Vector3(60.0, 250.0, 900.0)));

        /* Crates of a few sizes, one turned. */
        SpawnShape(InWorld, "Crate", BoxMesh, true,
                   Placed(EngineCore::Vector3(420.0, 100.0, -300.0), EngineCore::Vector3(200.0, 200.0, 200.0)));
        SpawnShape(InWorld, "Crate", BoxMesh, true,
                   Placed(EngineCore::Vector3(-380.0, 75.0, -420.0), EngineCore::Vector3(150.0, 150.0, 150.0)));
        SpawnShape(InWorld, "Crate", BoxMesh, true,
                   Placed(EngineCore::Vector3(600.0, 50.0, 380.0), EngineCore::Vector3(300.0, 100.0, 150.0),
                          EngineCore::Vector3(0.0, 30.0, 0.0)));

        /* A row of thin pillars toward the front, for long sun shadows. */
        for(int Index = 0; Index < 5; ++Index)
        {
            const double X = -800.0 + 400.0 * double(Index);
            SpawnShape(InWorld, "Pillar", BoxMesh, true,
                       Placed(EngineCore::Vector3(X, 200.0, -1000.0), EngineCore::Vector3(50.0, 400.0, 50.0)));
        }

        /* An arch: two posts and a lintel, for a shadow with a hole in it. */
        SpawnShape(InWorld, "ArchPost", BoxMesh, true,
                   Placed(EngineCore::Vector3(-500.0, 175.0, 400.0), EngineCore::Vector3(60.0, 350.0, 60.0)));
        SpawnShape(InWorld, "ArchPost", BoxMesh, true,
                   Placed(EngineCore::Vector3(-200.0, 175.0, 400.0), EngineCore::Vector3(60.0, 350.0, 60.0)));
        SpawnShape(InWorld, "ArchLintel", BoxMesh, true,
                   Placed(EngineCore::Vector3(-350.0, 380.0, 400.0), EngineCore::Vector3(420.0, 60.0, 80.0)));

        /* Balls held off the floor, so each shadow lands apart from its caster. */
        SpawnShape(InWorld, "Ball", SphereMesh, true,
                   Placed(EngineCore::Vector3(-300.0, 260.0, -50.0), EngineCore::Vector3(200.0, 200.0, 200.0)));
        SpawnShape(InWorld, "Ball", SphereMesh, true,
                   Placed(EngineCore::Vector3(280.0, 320.0, 120.0), EngineCore::Vector3(140.0, 140.0, 140.0)),
                   "Content/Materials/Glow.material.nmgraph");   // emissive: what Bloom spreads
        SpawnShape(InWorld, "Ball", SphereMesh, true,
                   Placed(EngineCore::Vector3(420.0, 260.0, -300.0), EngineCore::Vector3(90.0, 90.0, 90.0)));
        SpawnShape(InWorld, "Ball", SphereMesh, true,
                   Placed(EngineCore::Vector3(150.0, 60.0, -600.0), EngineCore::Vector3(120.0, 120.0, 120.0)));

        SpawnFlare(InWorld);
        SpawnParticleDemos(InWorld);

        /* A colonnade down the right of the yard: instances of a prefab file,
         * so a saved level keeps them as instances (.nmprefabinst) of it. */
        for(int Index = 0; Index < 4; ++Index)
        {
            const double Z = -450.0 + 350.0 * double(Index);
            if(!InWorld.SpawnPrefab(ColumnPrefab, Placed(EngineCore::Vector3(1000.0, 0.0, Z), EngineCore::Vector3(1.0, 1.0, 1.0)),
                                    "Column").IsValid())
            {
                CORE_LOG_WARN(std::string("[LightLab] could not spawn ") + ColumnPrefab)
                break;
            }
        }
    }

    void SpawnLights(GameCore::World& InWorld)
    {
        /* The sun, low enough for long shadows. Rotation (pitch, yaw, 0):
         * pitch -50 points it 50 degrees below the horizon. */
        RenderCore::DirectionalLightComponent Sun;
        Sun.Color           = EngineCore::Color(255, 244, 225);
        Sun.Intensity       = 2.0f;   // under the default 3, so the white floor does not clip and the spots read
        Sun.bCastShadows    = 1;
        Sun.ShadowTechnique = RenderCore::LST_Cascaded;
        Sun.ShadowDistance  = 5000.0f;
        Sun.CascadeCount    = 4;
        SpawnLight(InWorld, "Sun", EngineCore::Vector3(0.0, 2000.0, 0.0),
                               EngineCore::Vector3(50.0, 35.0, 0.0), Sun);

        /* Ambient fill, so the shadowed side is not black. */
        RenderCore::SkyLightComponent Sky;
        Sky.SkyColor    = EngineCore::Color(120, 140, 175);
        Sky.GroundColor = EngineCore::Color(70, 60, 50);
        Sky.Intensity   = 0.5f;
        SpawnLight(InWorld, "Sky", EngineCore::Vector3(0.0, 0.0, 0.0), EngineCore::Vector3(0.0, 0.0, 0.0), Sky);

        /* A warm, fixed spot from the left, down onto the arch and the big ball. */
        RenderCore::SpotLightComponent WarmSpot;
        WarmSpot.Color           = EngineCore::Color(255, 200, 140);
        WarmSpot.Intensity       = 14.0f;
        WarmSpot.Range           = 1600.0f;
        WarmSpot.InnerConeAngle  = 18.0f;
        WarmSpot.OuterConeAngle  = 28.0f;
        WarmSpot.bCastShadows    = 1;
        WarmSpot.ShadowTechnique = RenderCore::LST_Standard;
        SpawnLight(InWorld, "WarmSpot", EngineCore::Vector3(-900.0, 800.0, -300.0),
                   EngineCore::Vector3(50.0, 45.0, 0.0), WarmSpot);

        /* A cool spot from the front right, sweeping across the crates. */
        RenderCore::SpotLightComponent SweepSpot;
        SweepSpot.Color           = EngineCore::Color(190, 215, 255);
        SweepSpot.Intensity       = 14.0f;
        SweepSpot.Range           = 1800.0f;
        SweepSpot.InnerConeAngle  = 14.0f;
        SweepSpot.OuterConeAngle  = 22.0f;
        SweepSpot.bCastShadows    = 1;
        SweepSpot.ShadowTechnique = RenderCore::LST_Standard;
        const double SweepBaseYaw = -20.0;
        SpawnLight(InWorld, "SweepSpot", EngineCore::Vector3(700.0, 900.0, -900.0),
                   EngineCore::Vector3(50.0, SweepBaseYaw, 0.0), SweepSpot);

        /* An orange point light orbiting the centre pillar. Modulated: it
         * lights unshadowed, and its shadow tints the result blue. */
        RenderCore::PointLightComponent OrbitLight;
        OrbitLight.Color           = EngineCore::Color(255, 170, 90);
        OrbitLight.Intensity       = 10.0f;
        OrbitLight.Radius          = 900.0f;
        OrbitLight.bCastShadows    = 1;
        OrbitLight.ShadowTechnique = RenderCore::LST_Modulated;
        OrbitLight.ShadowTint      = EngineCore::Color(110, 135, 200);   // multiplies what it shades: a light tint is a soft shadow
        const EngineCore::Vector3 OrbitCenter(0.0, 250.0, 0.0);
        SpawnLight(InWorld, "OrbitLight", OrbitCenter + EngineCore::Vector3(350.0, 0.0, 0.0),
                   EngineCore::Vector3(0.0, 0.0, 0.0), OrbitLight);

        /* A dim magenta glow in the corner by the walls. No shadow: the
         * cheap kind of light, for contrast. */
        RenderCore::PointLightComponent Glow;
        Glow.Color        = EngineCore::Color(220, 80, 255);
        Glow.Intensity    = 4.0f;
        Glow.Radius       = 600.0f;
        Glow.bCastShadows = 0;
        SpawnLight(InWorld, "Glow", EngineCore::Vector3(-750.0, 120.0, 650.0), EngineCore::Vector3(0.0, 0.0, 0.0), Glow);

    }

    GameCore::EntityID     SunEntity;
    LightLabFlyContext     FlyContext;
    LightLabAnimateContext AnimateContext;

    std::optional<LightLabGameMode> Mode;
};

/* Owns the level, so it lives exactly as long as its code. */
/* Baked lighting's demo room (Content/Levels/BakedLighting.nmworld): no
 * scene in code -- without the file there is nothing to show. */
class LightLabBakedLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "BakedLighting"; }
    const char* GetWorldFile() const override { return BakedLevelFile; }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        (void)InSession;
        GameCore::World& World = GetWorld();
        FlyContext   = LightLabFlyContext();
        SwingContext = LightLabSwingContext();

        if(!LoadWorld(BakedLevelFile, World))
        {
            CORE_LOG_ERROR("[LightLab] the BakedLighting level needs Content/Levels/BakedLighting.nmworld "
                           "(Tools/make_baked_lighting_level.py, then Build > Bake Lightmaps)")
            return false;
        }

        const GameCore::EntityID Player = World.FindEntityByName("Player");
        if(const GameCore::TransformComponent* View = World.GetEntity(Player).GetComponent<GameCore::TransformComponent>())
        {
            FlyContext.Pitch = std::clamp(double(View->Rotation.GetX()), -MaxPitch, MaxPitch);
            FlyContext.Yaw   = double(View->Rotation.GetY());
        }
        FlyContext.Player = Player;

        SwingContext.Swinger = World.FindEntityByName("Swinger");
        if(const GameCore::TransformComponent* Ball = World.GetEntity(SwingContext.Swinger).GetComponent<GameCore::TransformComponent>())
        {
            SwingContext.Center = Ball->Position;
        }

        World.RegisterSystem("LightLab.Fly", GameCore::TP_PrePhysics, &FlySystem, &FlyContext);
        World.RegisterSystem("LightLab.Swing", GameCore::TP_PrePhysics, &SwingSystem, &SwingContext);
        Mode.emplace(Player, World.FindEntityByName("Sun"));
        return true;
    }

    NimbleEngine::IGameMode* GetGameMode() override
    {
        return Mode ? &*Mode : nullptr;
    }

private:
    LightLabFlyContext              FlyContext;
    LightLabSwingContext            SwingContext;
    std::optional<LightLabGameMode> Mode;
};

class LightLabGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "LightLabGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        /* [Game] StartLevel resolves levels by name. */
        NimbleEngine::RegisterLevel(Level);
        NimbleEngine::RegisterLevel(BakedLevel);
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds a bare pointer, and this image is about to go. */
        NimbleEngine::UnregisterLevel(Level);
        NimbleEngine::UnregisterLevel(BakedLevel);
    }

    NimbleEngine::ILevel* GetStartLevel() override { return &Level; }

private:
    LightLabLevel      Level;
    LightLabBakedLevel BakedLevel;
};

NIMBLE_IMPLEMENT_GAME_MODULE(LightLabGameModule, "LightLabGame")

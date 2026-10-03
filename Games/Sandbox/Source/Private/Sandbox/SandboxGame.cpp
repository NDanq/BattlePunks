#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/Sandbox/BrickBreaker.h"
#include "Public/Sandbox/SandboxScripting.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <optional>
#include <vector>
#include <string>

/*
 * SandboxGame: the example game, built as a game module DLL loaded by name by
 * the engine's launcher (nimble_add_game_launcher) and the editor's Play-in-Editor.
 *
 * Demonstrates: a C++ entity ("Marker") steered by the player through an
 * Input component (SandboxInput.cpp's map) and wrapped by a PostPhysics
 * system; colliders -- Marker, pickups, a mesh-shaped hazard answered by a
 * C++ overlap delegate (OnHazardTouched); a Lua entity ("Scripted") read back from C++; an
 * OnEntityCreated bind/unbind; drawing via RenderComponent and a Camera
 * entity; and a timed save/load cycle. The game's own component and script
 * library -- what scripts and script graphs can call -- are in
 * SandboxScripting.cpp.
 *
 *   SandboxLevel      spawns Marker and Camera, registers the wrap system,
 *                     owns the mode and state.
 *   SandboxGameMode   moves Marker, reports, decides when to quit.
 *   SandboxGameState  elapsed time and tick counts.
 *   SandboxGameModule owns and registers the level, and BrickBreaker's
 *                     (BrickBreaker.cpp), the start level.
 */

/* A game-owned config var, in the same registry and .ini as the engine's.
 * Unlike [Application] MaxFrames, this is game logic requesting quit. */
NIMBLE_CONFIG_INT(GSandboxRunSeconds, "Sandbox", "RunSeconds", 0, nullptr);

/* Beside the executable for the demo; a real game would use appGetAppDataDir. */
static const char* const GSandboxSavePath = "Saves/Quick.nmsg";

// === Native prefabs =========================================================
// The level's C++ pieces, declared as prefabs ("native:Sandbox.Marker"):
// SandboxLevel::OnLoad places them, and the editor lists them under World >
// Native Prefabs. Where each stands, and how big, is its placement.

namespace
{
    /* The player: the unit quad (bounds to match, for picking and spatial
     * queries), steered through its Input component by Sandbox.DriveMarker
     * (SandboxInput.cpp's "Default" map), its Bounds box its collider on the
     * player's layer, and a script graph that counts its jumps. */
    void BuildMarker(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Marker");
        Root.Add<GameCore::TransformComponent>();
        Root.Add<RenderCore::RenderComponent>();

        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0));
        Root.Add(Bounds);

        Root.Add<NimbleEngine::InputComponent>();

        GameCore::CollisionComponent Collision;
        Collision.Layer = SandboxLayers::Player;
        Root.Add(Collision);

        InBuilder.Script("Scripts/JumpCounter.script.nmgraph");
    }

    /* A hazard shaped by its MESH: no Bounds is authored -- the Collision
     * reads the box mesh's bounds and adds one (Source = Mesh). */
    void BuildHazard(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Hazard");
        Root.Add<GameCore::TransformComponent>();

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        Root.Add(Render);

        GameCore::CollisionComponent Collision;
        Collision.Source = GameCore::CS_Mesh;
        Collision.Layer  = SandboxLayers::Hazard;
        Collision.Mask   = SandboxLayers::Player;
        Root.Add(Collision);
    }

    /* The view: WorldBridge uses the first active CameraComponent. */
    void BuildCamera(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Camera");
        Root.Add<GameCore::TransformComponent>();
        Root.Add<RenderCore::CameraComponent>();
    }

    GameCore::TransformPose Placed(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InScale)
    {
        GameCore::TransformPose Pose;
        Pose.Position = InPosition;
        Pose.Scale    = InScale;
        return Pose;
    }
}

GAMECORE_DEFINE_NATIVE_PREFAB(Marker, "Sandbox.Marker", &BuildMarker)
GAMECORE_DEFINE_NATIVE_PREFAB(Hazard, "Sandbox.Hazard", &BuildHazard)
GAMECORE_DEFINE_NATIVE_PREFAB(Camera, "Sandbox.Camera", &BuildCamera)

/* Save/load demo flags. File scope: a load rebuilds the mode and resets the
 * state, so flags there would loop the demo forever. */
static bool GSandboxHasSaved  = false;
static bool GSandboxHasLoaded = false;

/* What Sandbox.DriveMarker's and Sandbox.WrapMarker's void* points at. */
struct SandboxMarkerSystemContext
{
    GameCore::EntityID Entity;
    int                ViewportWidth = 0;

    /* Marker drifts right on its own until the player first steers it, so an
     * unattended run (RunSeconds) still sweeps up the pickups. */
    bool               bPlayerSteering = false;
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

    /* PrePhysics system: moves Marker from its Input component -- MoveX and
     * MoveY, faster with Boost. Registered after the scripts (the default
     * order), which saw the same input: the engine resolved it first. */
    void DriveMarkerSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<SandboxMarkerSystemContext*>(InUserData);
        const GameCore::Entity Marker = InWorld.GetEntity(Context.Entity);
        GameCore::TransformComponent* Transform = Marker.GetComponent<GameCore::TransformComponent>();
        const NimbleEngine::InputComponent* Input = Marker.GetComponent<NimbleEngine::InputComponent>();
        if(!Transform)
        {
            return;
        }

        const double X = Input ? double(Input->GetAxis("MoveX")) : 0.0;
        const double Y = Input ? double(Input->GetAxis("MoveY")) : 0.0;
        if(X != 0.0 || Y != 0.0)
        {
            Context.bPlayerSteering = true;
        }

        const double Speed = Input && Input->IsDown("Boost") ? 480.0 : 240.0;
        const EngineCore::Vector3 Velocity = Context.bPlayerSteering ? EngineCore::Vector3(X * Speed, Y * Speed, 0.0)
                                                                     : EngineCore::Vector3(120.0, 0.0, 0.0);
        EngineCore::Vector3 Position = Transform->Position + Velocity * double(InDeltaSeconds);

        /* Kept on screen vertically; Sandbox.WrapMarker handles x. */
        Position.SetY(std::max(16.0, std::min(704.0, double(Position.GetY()))));
        Transform->Position = Position;
    }

    /* PostPhysics system: wraps Marker's X at the viewport width. State comes
     * through InUserData (SandboxMarkerSystemContext). */
    void WrapMarkerSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        const auto& Context = *static_cast<const SandboxMarkerSystemContext*>(InUserData);

        if(GameCore::TransformComponent* Transform = InWorld.GetEntity(Context.Entity).GetComponent<GameCore::TransformComponent>())
        {
            const double X = Transform->Position.GetX();
            if(Context.ViewportWidth > 0 && X >= double(Context.ViewportWidth))
            {
                Transform->Position.SetX(X - double(Context.ViewportWidth));
            }
        }
    }

    /* OnOverlapBegin subscriber (World's collision delegate), bound in
     * SandboxLevel::OnLoad and unbound in OnUnload: touching the hazard costs
     * a point. Self is each side of the pair in turn, so this only answers
     * when Self is the hazard. */
    void OnHazardTouched(const GameCore::EntityID InSelf, const GameCore::EntityID InOther, void* InUserData)
    {
        auto* World = static_cast<GameCore::World*>(InUserData);
        if(std::strcmp(World->GetEntity(InSelf).GetName(), "Hazard") != 0)
        {
            return;
        }
        SandboxScripting::SetScore(std::max(0, SandboxScripting::GetScore() - 1));
        CORE_LOG("[Sandbox] " + std::string(World->GetEntity(InOther).GetName()) + " touched the hazard: score "
                 + std::to_string(SandboxScripting::GetScore()))
    }

    /* OnEntityCreated subscriber, bound in SandboxLevel::OnLoad and unbound in
     * OnUnload. Logs entities from both C++ and Scripts/Main.lua. */
    void OnEntityCreatedLogger(GameCore::EntityID InEntity, void* InUserData)
    {
        auto* World = static_cast<GameCore::World*>(InUserData);
        CORE_LOG("[Sandbox] OnEntityCreated: " + std::string(World->GetEntity(InEntity).GetName()))
    }
}

/* Owned by SandboxLevel, used by SandboxGameMode. OnReset zeroes everything. */
class SandboxGameState : public NimbleEngine::GameState
{
public:
    void OnReset() override
    {
        ElapsedSeconds    = 0.0;
        LastReportSeconds = 0.0;
        TicksThisSecond   = 0;
        SandboxScripting::SetScore(0);
    }

    /* Saves exactly the fields OnReset touches; keep the two in step. New
     * fields go at the end behind Ar.GetChunkVersion(). */
    uint16_t GetSerialVersion() const override { return 2; }

    void Serialize(EngineCore::Archive& Ar) override
    {
        Ar << ElapsedSeconds;
        Ar << LastReportSeconds;
        Ar << TicksThisSecond;

        /* 2: the script library's Score, so a restored pickup is not scored
         * a second time. */
        if(Ar.GetChunkVersion() >= 2)
        {
            int32_t Score = SandboxScripting::GetScore();
            Ar << Score;
            SandboxScripting::SetScore(Score);
        }
    }

    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    uint64_t TicksThisSecond   = 0;
};

/* The mode calls back into the level in OnPostLoadSave. */
class SandboxLevel;

class SandboxGameMode : public NimbleEngine::IGameMode
{
public:
    SandboxGameMode(SandboxLevel& InLevel, GameCore::EntityID InMarker, SandboxGameState& InState)
        : OwningLevel(&InLevel), MarkerEntity(InMarker), State(&InState) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;

        RunSeconds = double(GSandboxRunSeconds.GetInt());

        /* Nothing spawned here: the level builds the scene, the mode is rules. */
        return true;
    }

    /* The startup script has run, so "Scripted" exists now (not in OnLoad). */
    void OnStart() override
    {
        const uint32_t Limit = Session->GetHost().GetFrameRateLimit();

        const GameCore::EntityID Scripted = Session->GetWorld().FindEntityByName("Scripted");

        CORE_LOG("[Sandbox] game starting; frame limit "
                 + (Limit > 0 ? Num(Limit) + " fps" : std::string("unlimited"))
                 + (RunSeconds > 0.0
                    ? "; will quit after " + Fixed2(RunSeconds) + "s"
                    : "; running until the window closes")
                 + (Scripted.IsSet()
                    ? "; the startup script's entity is present"
                    : "; no scripted entity (no startup script ran)"))
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        /* Concurrent with the render thread: no renderer or window access.
         * The session has already checked this is the game thread. */
        State->ElapsedSeconds += InFrame.DeltaSeconds;
        SandboxScripting::SetElapsedSeconds(State->ElapsedSeconds);   // Sandbox.GetElapsed() in scripts
        State->TicksThisSecond++;

        /* Marker moves in Sandbox.DriveMarker, from its Input component. */
        GameCore::World& World = Session->GetWorld();

        /* The engine finishes this frame, then exits the loop. */
        if(RunSeconds > 0.0 && State->ElapsedSeconds >= RunSeconds && Session)
        {
            CORE_LOG("[Sandbox] RunSeconds reached; asking the engine to quit")
            Session->RequestQuit();
        }

        /* Save/load demo. Loading two seconds after the save makes Marker
         * visibly jump back. */
        if(!GSandboxHasSaved && State->ElapsedSeconds >= 2.0)
        {
            GSandboxHasSaved = true;
            SavedMarkerX = 0.0;
            if(const GameCore::TransformComponent* Marker =
                   World.GetEntity(MarkerEntity).GetComponent<GameCore::TransformComponent>())
            {
                SavedMarkerX = Marker->Position.GetX();
            }

            CORE_LOG("[Sandbox] saving at t=" + Fixed2(State->ElapsedSeconds)
                     + "s marker_x=" + Fixed2(SavedMarkerX))

            if(!NimbleEngine::SaveGameToFile(*Session, GSandboxSavePath))
            {
                CORE_LOG_ERROR("[Sandbox] the save failed")
            }
        }

        if(GSandboxHasSaved && !GSandboxHasLoaded && State->ElapsedSeconds >= 4.0)
        {
            GSandboxHasLoaded = true;

            CORE_LOG("[Sandbox] loading at t=" + Fixed2(State->ElapsedSeconds)
                     + "s (expect elapsed and marker_x to jump back to the saved values)")

            /* Deferred to the next frame, so safe from inside OnTick. */
            if(!NimbleEngine::LoadGameFromFile(*Session, GSandboxSavePath))
            {
                CORE_LOG_ERROR("[Sandbox] the load failed")
            }
        }

        if(State->ElapsedSeconds - State->LastReportSeconds >= 1.0)
        {
            const GameCore::TransformComponent* MarkerTransform =
                World.GetEntity(MarkerEntity).GetComponent<GameCore::TransformComponent>();
            const double MarkerX = MarkerTransform ? MarkerTransform->Position.GetX() : 0.0;

            /* Created by Scripts/Main.lua; read purely through the World. */
            const GameCore::EntityID ScriptedEntity = World.FindEntityByName("Scripted");
            std::string ScriptedPart;
            if(GameCore::TransformComponent* Scripted = World.GetEntity(ScriptedEntity).GetComponent<GameCore::TransformComponent>())
            {
                ScriptedPart = " scripted_x=" + Fixed2(Scripted->Position.GetX());
            }

            RenderCore::RenderStats RenderStats;
            Session->GetRenderer().GetStats(RenderStats);

            CORE_LOG("[Sandbox] t=" + Fixed2(State->ElapsedSeconds) + "s"
                     + " frame=" + Num(InFrame.FrameNumber)
                     + " fps=" + Num(State->TicksThisSecond)
                     + " dt=" + Fixed2(InFrame.DeltaSeconds * 1000.0) + "ms"
                     + " viewport=" + Num(uint64_t(InFrame.ViewportPixels.Width))
                     + "x" + Num(uint64_t(InFrame.ViewportPixels.Height))
                     + " marker_x=" + Fixed2(MarkerX)
                     + DescribeSteering()
                     + ScriptedPart
                     + " score=" + std::to_string(SandboxScripting::GetScore())
                     + " frames_rendered=" + Num(RenderStats.FramesRendered)
                     + " draw_calls=" + Num(RenderStats.DrawCalls))

            State->LastReportSeconds = State->ElapsedSeconds;
            State->TicksThisSecond   = 0;
        }
    }

    /* Re-derives MarkerEntity by name: the restored Marker is a new entity. */
    void OnPostLoadSave() override;

    /* " steering=player" once the player has moved Marker, else " steering=auto". */
    std::string DescribeSteering() const;

    void OnUnload() override
    {
        /* Nothing to unbind: the level owns the delegate binding. */
        CORE_LOG("[Sandbox] game stopping after " + Fixed2(State->ElapsedSeconds) + "s")
        Session = nullptr;
    }

private:
    /* Stashed in OnLoad for RequestQuit() and GetWorld(). */
    NimbleEngine::GameSession* Session = nullptr;

    double RunSeconds = 0.0;

    /* For OnMarkerRestored: the level also holds a Marker handle. */
    SandboxLevel* OwningLevel = nullptr;

    /* From the level; re-derived in OnPostLoadSave. */
    GameCore::EntityID MarkerEntity;
    SandboxGameState*  State = nullptr;

    /* Marker X at save time, logged on restore. */
    double SavedMarkerX = 0.0;
};

/* The scene: spawns Marker and Camera, registers the wrap system, binds the
 * entity logger, and owns the mode and state. */
class SandboxLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "Sandbox"; }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        (void)InSession;

        /* GetWorld(), not InSession.GetWorld() (Level.h). */
        GameCore::World& World = GetWorld();

        /* First, not last: OnUnload pairs with OnLoad being called (Level.h). */
        bLoaded = true;

        /* Removed in OnUnload (Delegate.h's RELOAD note). */
        EntityCreatedHandle = World.OnEntityCreated().Add(&OnEntityCreatedLogger, &World);

        /* The level is prefab instances (the native prefabs above). Off y=0:
         * +Y runs up from the bottom edge in the ortho view, so a quad at y=0
         * would hang half off screen. MarkerEntity keeps only the root's id;
         * the handle's World* is per-load. */
        const GameCore::Entity Marker = World.SpawnPrefab("native:Sandbox.Marker",
            Placed(EngineCore::Vector3(0.0, 360.0, 0.0), EngineCore::Vector3(32.0, 32.0, 1.0))).GetRoot();   // the unit quad, 32 x 32
        MarkerEntity = Marker;

        World.SpawnPrefab("native:Sandbox.Hazard",
                          Placed(EngineCore::Vector3(1200.0, 360.0, 0.0), EngineCore::Vector3(40.0, 40.0, 40.0)));
        HazardHandle = World.OnOverlapBegin().Add(&OnHazardTouched, &World);

        /* Centres the view on Marker's path. */
        World.SpawnPrefab("native:Sandbox.Camera");

        /* ViewportWidth is read once; the wrap boundary need only be rough. */
        SystemContext.Entity        = MarkerEntity;
        SystemContext.ViewportWidth = 1280;
        World.RegisterSystem("Sandbox.DriveMarker", GameCore::TP_PrePhysics,
                             &DriveMarkerSystem, &SystemContext);
        World.RegisterSystem("Sandbox.WrapMarker", GameCore::TP_PostPhysics,
                             &WrapMarkerSystem, &SystemContext);

        /* Here, not a member initialiser: needs MarkerEntity. */
        Mode.emplace(*this, MarkerEntity, State);

        return true;
    }

    /* Empties the World before a restore, or the scene is doubled (Level.h).
     * Systems and delegate bindings belong to the World and stay. */
    void OnPrepareForLoadedGame() override
    {
        GameCore::World& World = GetWorld();

        /* Collect, then destroy: ForEachEntity forbids destroying inside it. */
        std::vector<GameCore::EntityID> Doomed;
        World.ForEachEntity([](GameCore::EntityID InEntity, void* InUserData)
        {
            static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
        }, &Doomed);

        for(const GameCore::EntityID Entity : Doomed)
        {
            World.DestroyEntity(Entity);
        }

        /* Unset until OnMarkerRestored, which makes the system a no-op. */
        SystemContext.Entity = GameCore::InvalidEntityID();
        MarkerEntity         = GameCore::InvalidEntityID();

        CORE_LOG("[Sandbox] cleared " + std::to_string(Doomed.size())
                 + " entity/entities before restoring the save")
    }

    void OnUnload() override
    {
        /* Pairs with OnLoad's Add; ~World warns about leftover bindings. */
        if(OwnedWorldIsLive())
        {
            GetWorld().OnEntityCreated().Remove(EntityCreatedHandle);
            GetWorld().OnOverlapBegin().Remove(HazardHandle);
        }
    }

    NimbleEngine::IGameMode* GetGameMode() override
    {
        return Mode ? &*Mode : nullptr;
    }

    NimbleEngine::GameState* GetGameState() override { return &State; }

    /* Re-points the wrap system at the restored Marker (from the mode's
     * OnPostLoadSave). Otherwise Marker silently stops wrapping. */
    bool IsPlayerSteering() const { return SystemContext.bPlayerSteering; }

    void OnMarkerRestored(const GameCore::EntityID InMarker)
    {
        MarkerEntity         = InMarker;
        SystemContext.Entity = InMarker;
    }

private:
    /* OnLoad ran; guards OnUnload, since GetWorld() CORE_CHECKs. */
    bool OwnedWorldIsLive() const { return bLoaded; }

    bool                       bLoaded = false;
    GameCore::EntityID         MarkerEntity;
    GameCore::DelegateHandle   EntityCreatedHandle;
    GameCore::DelegateHandle   HazardHandle;
    SandboxMarkerSystemContext SystemContext;

    SandboxGameState                 State;
    std::optional<SandboxGameMode>   Mode;
};

/* Out-of-line: needs SandboxLevel's definition. */
std::string SandboxGameMode::DescribeSteering() const
{
    return OwningLevel->IsPlayerSteering() ? " steering=player" : " steering=auto";
}

/* Out-of-line: needs SandboxLevel's definition. */
void SandboxGameMode::OnPostLoadSave()
{
    const GameCore::EntityID Restored = Session->GetWorld().FindEntityByName("Marker");

    CORE_LOG(std::string("[Sandbox] save restored; re-deriving Marker (old handle ")
             + (Session->GetWorld().IsAlive(MarkerEntity) ? "still alive" : "now dead")
             + ", new handle " + (Restored.IsSet() ? "found" : "MISSING") + ")")

    MarkerEntity = Restored;

    /* The wrap system's copy is stale too. */
    OwningLevel->OnMarkerRestored(Restored);
}

/* Owns the level, so it lives exactly as long as its code. */
class SandboxGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "SandboxGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        /* Saves and [Game] StartLevel resolve levels by name. */
        NimbleEngine::RegisterLevel(Level);
        NimbleEngine::RegisterLevel(BrickBreaker::GetLevel());
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds a bare pointer, and this image is about to go. */
        NimbleEngine::UnregisterLevel(BrickBreaker::GetLevel());
        NimbleEngine::UnregisterLevel(Level);
    }

    /* BrickBreaker (BrickBreaker.cpp); [Game] StartLevel=Sandbox for the demo below. */
    NimbleEngine::ILevel* GetStartLevel() override { return &BrickBreaker::GetLevel(); }

private:
    SandboxLevel Level;
};

NIMBLE_IMPLEMENT_GAME_MODULE(SandboxGameModule, "SandboxGame")

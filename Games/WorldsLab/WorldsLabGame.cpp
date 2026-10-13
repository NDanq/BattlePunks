#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"

#include <cstdio>
#include <string>
#include <vector>

/*
 * WorldsLabGame: the game, built as a module library loaded by name by
 * the WorldsLab launcher (the engine's) and the editor's Play-in-Editor.
 *
 *   WorldsLabGameMode    the rules for Content/Levels/WorldsLab.nmworld:
 *                             places the native prefabs, ticked once per
 *                             frame; owns the state.
 *   WorldsLabGameState   what a save keeps, reset on every level start.
 *   WorldsLabExploreMode the visual-style worlds' mode (WorldsLabExplore.cpp):
 *                             walk and fly through Content/Levels/<Style>/.
 *   WorldsLabGameModule  registers the modes. Config/DefaultEngine.ini's
 *                             [World_WorldsLab] names it (Project
 *                             Settings > Worlds), and [Game] StartLevel that.
 *
 * Games/Sandbox/SandboxGame.cpp shows the rest: input, systems, collision,
 * scripting, saves.
 */

// === Native prefabs =========================================================
// The level's C++ pieces ("native:WorldsLab.Camera"): the level places
// them, and the editor lists them under World > Native Prefabs.

namespace
{
    /* The view: the renderer uses the first active CameraComponent. */
    void BuildCamera(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Camera");
        Root.Add<GameCore::TransformComponent>();
        Root.Add<RenderCore::CameraComponent>();
    }

    /* The player: the unit quad, with bounds to match for picking. */
    void BuildPlayer(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Player");
        Root.Add<GameCore::TransformComponent>();
        Root.Add<RenderCore::RenderComponent>();

        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0));
        Root.Add(Bounds);
    }

    GameCore::TransformPose Placed(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InScale)
    {
        GameCore::TransformPose Pose;
        Pose.Position = InPosition;
        Pose.Scale    = InScale;
        return Pose;
    }
}

GAMECORE_DEFINE_NATIVE_PREFAB(Camera, "WorldsLab.Camera", &BuildCamera)
GAMECORE_DEFINE_NATIVE_PREFAB(Player, "WorldsLab.Player", &BuildPlayer)

/* WorldsLabExplore.cpp: the visual-style worlds' walk-and-fly mode. */
void WorldsLabRegisterExplore();
void WorldsLabUnregisterExplore();

/* WorldsLabRange.cpp: the Shooting Range's mode and its shooter's movement. */
void WorldsLabRegisterRange();
void WorldsLabUnregisterRange();
void WorldsLabRegisterPvP();
void WorldsLabUnregisterPvP();
/* WorldsLabArena.cpp, the Toon Arena (AICore's demo), registers itself
 * (NIMBLE_REGISTER_GAME_MODE); the rest still register here. */

// === State ==================================================================

/* Owned by the mode. OnReset puts back the starting
 * values; Serialize saves the same fields. Keep the two in step. */
class WorldsLabGameState : public NimbleEngine::GameState
{
public:
    void OnReset() override
    {
        ElapsedSeconds = 0.0;
    }

    /* A new field goes at the end, behind Ar.GetChunkVersion() >= 2, with
     * this bumped to 2. */
    uint16_t GetSerialVersion() const override { return 1; }

    void Serialize(EngineCore::Archive& Ar) override
    {
        Ar << ElapsedSeconds;
    }

    double ElapsedSeconds = 0.0;
};

// === Mode ===================================================================

/* The rules, for Content/Levels/WorldsLab.nmworld ([World_WorldsLab]
 * in Config/DefaultEngine.ini names this mode). The world is loaded before
 * OnLoad: place what it does not hold, register systems. */
class WorldsLabGameMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        GameCore::World& World = InSession.GetWorld();

        /* Centred in the default 1280 x 720 view; +Y runs down from the top.
         * Place these in the world in the editor instead and delete them here. */
        World.SpawnPrefab("native:WorldsLab.Camera");
        World.SpawnPrefab("native:WorldsLab.Player",
                          Placed(EngineCore::Vector3(640.0, 360.0, 0.0), EngineCore::Vector3(32.0, 32.0, 1.0)));
        return true;
    }

    /* The startup script has run and the state is reset. */
    void OnStart() override
    {
        CORE_LOG("[WorldsLab] game starting")
    }

    /* On the game thread, concurrent with the render thread: no renderer or
     * window access. */
    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        State.ElapsedSeconds += InFrame.DeltaSeconds;
    }

    void OnUnload() override
    {
        char Elapsed[32];
        std::snprintf(Elapsed, sizeof(Elapsed), "%.2f", State.ElapsedSeconds);
        CORE_LOG("[WorldsLab] game stopping after " + std::string(Elapsed) + "s")
        Session = nullptr;
    }

    NimbleEngine::GameState* GetGameState() override { return &State; }

    /* Empties the World before a save is restored, or the scene is doubled. */
    void OnPrepareForLoadedGame() override
    {
        GameCore::World& World = Session->GetWorld();

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
    }

private:
    /* Stashed in OnLoad: GetWorld(), RequestQuit(). */
    NimbleEngine::GameSession* Session = nullptr;
    WorldsLabGameState    State;
};

// === Module =================================================================

/* Registers the mode; the project's world list names it. */
class WorldsLabGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "WorldsLabGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        NimbleEngine::RegisterGameMode<WorldsLabGameMode>("WorldsLab");
        WorldsLabRegisterExplore();
        WorldsLabRegisterRange();
        WorldsLabRegisterPvP();
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds this image's functions, and it is about to go. */
        NimbleEngine::UnregisterGameMode("WorldsLab");
        WorldsLabUnregisterExplore();
        WorldsLabUnregisterRange();
        WorldsLabUnregisterPvP();
    }
};

NIMBLE_IMPLEMENT_GAME_MODULE(WorldsLabGameModule, "WorldsLabGame")

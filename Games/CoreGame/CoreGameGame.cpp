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
 * CoreGameGame: the game, built as a module library loaded by name by
 * the CoreGame launcher (the engine's) and the editor's Play-in-Editor.
 *
 *   CoreGameGameMode    the rules for Content/Levels/CoreGame.nmworld:
 *                             places the native prefabs, ticked once per
 *                             frame; owns the state.
 *   CoreGameGameState   what a save keeps, reset on every level start.
 *   CoreGameGameModule  the module. Each mode registers itself
 *                             (NIMBLE_REGISTER_GAME_MODE); Config/DefaultEngine.ini's
 *                             [World_CoreGame] names it (Project Settings >
 *                             Worlds), and [Game] StartLevel that.
 *
 * Games/Sandbox/SandboxGame.cpp shows the rest: input, systems, collision,
 * scripting, saves.
 */

// === Native prefabs =========================================================
// The level's C++ pieces ("native:CoreGame.Camera"): the level places
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

GAMECORE_DEFINE_NATIVE_PREFAB(Camera, "CoreGame.Camera", &BuildCamera)
GAMECORE_DEFINE_NATIVE_PREFAB(Player, "CoreGame.Player", &BuildPlayer)

// === State ==================================================================

/* Owned by the mode. OnReset puts back the starting
 * values; Serialize saves the same fields. Keep the two in step. */
class CoreGameGameState : public NimbleEngine::GameState
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

/* The rules, for Content/Levels/CoreGame.nmworld ([World_CoreGame]
 * in Config/DefaultEngine.ini names this mode). The world is loaded before
 * OnLoad: place what it does not hold, register systems. */
class CoreGameGameMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GameCore::World& World = InSession.GetWorld();

        /* Centred in the default 1280 x 720 view; +Y runs down from the top.
         * Place these in the world in the editor instead and delete them here. */
        World.SpawnPrefab("native:CoreGame.Camera");
        World.SpawnPrefab("native:CoreGame.Player",
                          Placed(EngineCore::Vector3(640.0, 360.0, 0.0), EngineCore::Vector3(32.0, 32.0, 1.0)));
        return true;
    }

    /* The startup script has run and the state is reset. */
    void OnStart() override
    {
        CORE_LOG("[CoreGame] game starting")
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
        CORE_LOG("[CoreGame] game stopping after " + std::string(Elapsed) + "s")
    }

    NimbleEngine::GameState* GetGameState() override { return &State; }

    /* Empties the World before a save is restored, or the scene is doubled. */
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
    }

private:
    /* GetSession() and GetWorld() are the engine's to set (IGameMode): from
     * before OnLoad until after OnUnload. Keep this level's state in members. */
    CoreGameGameState    State;
};

/* By name, as the module loads; the project's world list names it. */
NIMBLE_REGISTER_GAME_MODE(CoreGameGameMode, "CoreGame")

// === Module =================================================================

/* The module: modes, native prefabs and input maps register themselves. */
class CoreGameGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "CoreGameGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override { return true; }
    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override {}
};

NIMBLE_IMPLEMENT_GAME_MODULE(CoreGameGameModule, "CoreGameGame")

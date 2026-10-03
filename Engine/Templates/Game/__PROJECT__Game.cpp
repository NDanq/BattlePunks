#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

/*
 * @PROJECT_NAME@Game: the game, built as a module library loaded by name by
 * the @PROJECT_NAME@ launcher (the engine's) and the editor's Play-in-Editor.
 *
 *   @PROJECT_NAME@Level       builds the scene from native prefabs; owns the
 *                             mode and the state.
 *   @PROJECT_NAME@GameMode    the rules: ticked once per frame.
 *   @PROJECT_NAME@GameState   what a save keeps, reset on every level start.
 *   @PROJECT_NAME@GameModule  registers the level; [Game] StartLevel names it.
 *
 * Games/Sandbox/SandboxGame.cpp shows the rest: input, systems, collision,
 * scripting, saves.
 */

// === Native prefabs =========================================================
// The level's C++ pieces ("native:@PROJECT_NAME@.Camera"): the level places
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

GAMECORE_DEFINE_NATIVE_PREFAB(Camera, "@PROJECT_NAME@.Camera", &BuildCamera)
GAMECORE_DEFINE_NATIVE_PREFAB(Player, "@PROJECT_NAME@.Player", &BuildPlayer)

// === State ==================================================================

/* Owned by the level, used by the mode. OnReset puts back the starting
 * values; Serialize saves the same fields. Keep the two in step. */
class @PROJECT_NAME@GameState : public NimbleEngine::GameState
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

/* The rules. Nothing is spawned here: the level builds the scene. */
class @PROJECT_NAME@GameMode : public NimbleEngine::IGameMode
{
public:
    explicit @PROJECT_NAME@GameMode(@PROJECT_NAME@GameState& InState) : State(&InState) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        return true;
    }

    /* The startup script has run and the state is reset. */
    void OnStart() override
    {
        CORE_LOG("[@PROJECT_NAME@] game starting")
    }

    /* On the game thread, concurrent with the render thread: no renderer or
     * window access. */
    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        State->ElapsedSeconds += InFrame.DeltaSeconds;
    }

    void OnUnload() override
    {
        char Elapsed[32];
        std::snprintf(Elapsed, sizeof(Elapsed), "%.2f", State->ElapsedSeconds);
        CORE_LOG("[@PROJECT_NAME@] game stopping after " + std::string(Elapsed) + "s")
        Session = nullptr;
    }

private:
    /* Stashed in OnLoad: GetWorld(), RequestQuit(). */
    NimbleEngine::GameSession* Session = nullptr;
    @PROJECT_NAME@GameState*   State   = nullptr;
};

// === Level ==================================================================

/* The scene: places the native prefabs, owns the mode and the state. */
class @PROJECT_NAME@Level : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "@PROJECT_NAME@"; }

    bool OnLoad(NimbleEngine::GameSession& /*InSession*/) override
    {
        /* GetWorld(), not InSession.GetWorld() (Level.h). */
        GameCore::World& World = GetWorld();

        /* Centred in the default 1280 x 720 view; +Y runs down from the top. */
        World.SpawnPrefab("native:@PROJECT_NAME@.Camera");
        World.SpawnPrefab("native:@PROJECT_NAME@.Player",
                          Placed(EngineCore::Vector3(640.0, 360.0, 0.0), EngineCore::Vector3(32.0, 32.0, 1.0)));

        Mode.emplace(State);
        return true;
    }

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

    NimbleEngine::IGameMode* GetGameMode() override { return Mode ? &*Mode : nullptr; }

    NimbleEngine::GameState* GetGameState() override { return &State; }

private:
    @PROJECT_NAME@GameState                 State;
    std::optional<@PROJECT_NAME@GameMode>   Mode;
};

// === Module =================================================================

/* Owns the level, so it lives exactly as long as its code. */
class @PROJECT_NAME@GameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "@PROJECT_NAME@Game"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        /* Saves and [Game] StartLevel resolve levels by name. */
        NimbleEngine::RegisterLevel(Level);
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds a bare pointer, and this image is about to go. */
        NimbleEngine::UnregisterLevel(Level);
    }

    NimbleEngine::ILevel* GetStartLevel() override { return &Level; }

private:
    @PROJECT_NAME@Level Level;
};

NIMBLE_IMPLEMENT_GAME_MODULE(@PROJECT_NAME@GameModule, "@PROJECT_NAME@Game")

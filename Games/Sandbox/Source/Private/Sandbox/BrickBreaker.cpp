#include "Public/Sandbox/BrickBreaker.h"

#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/Sandbox/SandboxScripting.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/*
 * BrickBreaker: the game framework (Engine/Source/GAMEFRAMEWORK.md) as a game.
 * Three prefabs, each driven a different way, and one set of rules:
 *
 *   Paddle   native C++ prefab "native:BrickBreaker.Paddle" (below): the
 *            player. Its PlayerState holds the score and lives; its callbacks
 *            move it from its Input and launch the ball.
 *   Brick    Content/Prefabs/Brick.nmprefab, driven by Scripts/Brick.script.nmgraph:
 *            counts its Brick component's Hits down; at 0 it scores through
 *            Game.AddScore, says "BrickBroken", and goes. Higher rows take
 *            more hits.
 *   Ball     Content/Prefabs/Ball.nmprefab, driven by Scripts/Ball.lua: moves only
 *            while the phase is Playing, bounces, and costs a life through
 *            Game.AddLives when it falls out.
 *
 *   BrickBreakerGameMode    the rules: which phase follows which, what a lost
 *                           life or the last brick means.
 *   BrickBreakerGameState   the phase (the engine's half) and bricks left.
 *
 * Phases: WaitingToStart -> Serving (the ball rides the paddle; Jump
 * launches) -> Playing -> Serving again after a lost life, GameOver after the
 * last (Jump restarts), or RoundCleared once every brick is gone (then the
 * next round, a little faster).
 *
 * THE LAYOUT is Content/Levels/BrickBreaker.nmworld ([BrickBreaker] LevelFile): the
 * camera, the paddle, the ball and the first wall, editable in the editor.
 * Every later wall repeats the first one's bricks, hits included. Without the
 * file (or with LevelFile empty) the level builds the same layout in code
 * (SpawnDefaultLayout, DefaultWall), which is also what wrote the file.
 *
 * Controls are Sandbox's "Default" map (SandboxInput.cpp): MoveX steers, Jump
 * launches. [BrickBreaker] AutoPlay=1 plays by itself, for unattended runs;
 * AutoPlay=2 launches and restarts but never steers, so it loses every ball.
 */

NIMBLE_CONFIG_INT(GBrickBreakerAutoPlay, "BrickBreaker", "AutoPlay", 0, nullptr);
NIMBLE_CONFIG_INT(GBrickBreakerRunSeconds, "BrickBreaker", "RunSeconds", 0, nullptr);
NIMBLE_CONFIG_STRING(GBrickBreakerLevelFile, "BrickBreaker", "LevelFile", "Content/Levels/BrickBreaker.nmworld", nullptr);

// === The Brick component ====================================================

GAMECORE_DEFINE_COMPONENT(SandboxBrickComponent, "Brick")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(SandboxBrickComponent, 1)
{
    Ar.Field("Hits", C.Hits);
}

/* brick:GetHits() / SetHits(n); Components/Brick in a script graph. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(SandboxBrickComponent)
{
    Bind.Field("Hits", &SandboxBrickComponent::Hits);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(SandboxBrickComponent)
{
    Props.Field("Hits", &SandboxBrickComponent::Hits);
}
#endif

namespace
{
    // === The field ==============================================================
    // Pixels, +Y up from the bottom edge: the Sandbox camera's orthographic
    // view of 1280 x 720 (RenderView.h).

    constexpr double FieldWidth  = 1280.0;

    constexpr double PaddleY      = 50.0;
    constexpr double PaddleWidth  = 128.0;
    constexpr double PaddleHeight = 16.0;
    constexpr double PaddleSpeed  = 900.0;   // pixels per second at full MoveX

    constexpr double BallSize = 14.0;

    constexpr int    BrickColumns = 10;
    constexpr int    BrickRows    = 5;
    constexpr double BrickWidth   = 110.0;
    constexpr double BrickHeight  = 28.0;
    constexpr double BrickGap     = 8.0;
    constexpr double BrickTop     = 640.0;   // the top row's centre; rows step down from it

    constexpr int    StartingLives = 3;
    constexpr double RoundClearedSeconds = 1.5;

    const char* const BrickPrefab = "Content/Prefabs/Brick.nmprefab";
    const char* const BallPrefab  = "Content/Prefabs/Ball.nmprefab";

    /* Phases this game adds to the engine's WaitingToStart and GameOver. */
    namespace Phases
    {
        constexpr const char* Serving      = "Serving";
        constexpr const char* Playing      = "Playing";
        constexpr const char* RoundCleared = "RoundCleared";
    }

    GameCore::TransformPose Placed(const double InX, const double InY, const double InWidth, const double InHeight)
    {
        GameCore::TransformPose Pose;
        Pose.Position = EngineCore::Vector3(InX, InY, 0.0);
        Pose.Scale    = EngineCore::Vector3(InWidth, InHeight, 1.0);
        return Pose;
    }

    /* A unit box with some depth, so colliders at z=0 meet face to face. */
    GameCore::BoundsComponent UnitBounds()
    {
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, -1.0), EngineCore::Vector3(0.5, 0.5, 1.0));
        return Bounds;
    }

    // === The paddle: a native prefab with C++ behaviour =========================

    void BuildPaddle(GameCore::PrefabBuilder& InBuilder)
    {
        const GameCore::PrefabBuilder::Node Root = InBuilder.Root("Paddle");
        Root.Add<GameCore::TransformComponent>();
        Root.Add<RenderCore::RenderComponent>();   // no mesh: the unit quad, scaled by the placement
        Root.Add(UnitBounds());
        Root.Add<NimbleEngine::InputComponent>();

        GameCore::CollisionComponent Collision;
        Collision.Layer = SandboxLayers::Paddle;
        Collision.Mask  = SandboxLayers::Ball;
        Root.Add(Collision);

        /* What makes the paddle the player: the mode hears of it
         * (OnPlayerAdded) and scripts find it (Game.GetPlayer(0)). */
        NimbleEngine::PlayerStateComponent Player;
        std::snprintf(Player.PlayerName, sizeof(Player.PlayerName), "%s", "Player 1");
        Player.PlayerIndex = 0;
        Player.Lives       = StartingLives;
        Root.Add(Player);
    }

    /* AutoPlay's steering: under the ball. MoveX in -1..1, like the stick it
     * stands in for. */
    double AutoSteer(GameCore::World& InWorld, const double InPaddleX)
    {
        const GameCore::Entity Ball = InWorld.GetEntity(InWorld.FindEntityByName("Ball"));
        const GameCore::TransformComponent* BallTransform = Ball.GetComponent<GameCore::TransformComponent>();
        if(!BallTransform)
        {
            return 0.0;
        }
        /* Off centre by a little, so the ball leaves at an angle. */
        const double Target = BallTransform->Position.GetX() - PaddleWidth * 0.2;
        return std::max(-1.0, std::min(1.0, (Target - InPaddleX) / 40.0));
    }

    /* TP_PrePhysics, after the engine resolved the Input component and before
     * the ball's script moves in TP_Physics. */
    void PaddlePrePhysics(const GameCore::PrefabInstance InSelf, const float InDeltaSeconds, void* /*InUserData*/)
    {
        GameCore::World* World = InSelf.GetWorld();
        NimbleEngine::GameSession* Session = World ? NimbleEngine::FindGameSession(*World) : nullptr;
        const GameCore::Entity Root = InSelf.GetRoot();
        GameCore::TransformComponent* Transform = Root.GetComponent<GameCore::TransformComponent>();
        const NimbleEngine::InputComponent* Input = Root.GetComponent<NimbleEngine::InputComponent>();
        if(!Session || !Transform || !Input)
        {
            return;
        }

        const bool bAutoPlay = GBrickBreakerAutoPlay.GetInt() != 0;
        const NimbleEngine::GameState* State = Session->GetGameState();

        /* AutoPlay 2 launches but never steers: every ball is lost. */
        double Move = double(Input->GetAxis("MoveX"));
        if(GBrickBreakerAutoPlay.GetInt() == 1 && Move == 0.0 && State->IsPhase(Phases::Playing))
        {
            Move = AutoSteer(*World, Transform->Position.GetX());
        }

        const double HalfWidth = PaddleWidth * 0.5;
        const double X = Transform->Position.GetX() + Move * PaddleSpeed * double(InDeltaSeconds);
        Transform->Position.SetX(std::max(HalfWidth, std::min(FieldWidth - HalfWidth, X)));

        /* Jump asks for the next phase; the mode's rules decide. */
        const bool bJump = Input->WasPressed("Jump");
        if(State->IsPhase(Phases::Serving) && (bJump || (bAutoPlay && State->GetPhaseSeconds() > 1.0)))
        {
            Session->SetPhase(Phases::Playing);
        }
        else if(State->IsPhase(NimbleEngine::GamePhases::GameOver) && (bJump || (bAutoPlay && State->GetPhaseSeconds() > 2.0)))
        {
            Session->SetPhase(Phases::Serving);
        }
    }

    GameCore::NativePrefabBehavior PaddleBehavior()
    {
        GameCore::NativePrefabBehavior Behavior;
        Behavior.OnPhase[GameCore::TP_PrePhysics] = &PaddlePrePhysics;
        return Behavior;
    }
}

GAMECORE_DEFINE_NATIVE_PREFAB_BEHAVIOR(Paddle, "BrickBreaker.Paddle", &BuildPaddle, PaddleBehavior())

namespace
{
    // === State and rules ========================================================

    /* The phase, its timer and the round are the engine's (GameState); this is
     * what BrickBreaker adds. */
    class BrickBreakerGameState : public NimbleEngine::GameState
    {
    public:
        void OnReset() override { BricksRemaining = 0; }

        uint16_t GetSerialVersion() const override { return 1; }
        void Serialize(EngineCore::Archive& Ar) override { Ar << BricksRemaining; }

        int32_t BricksRemaining = 0;
    };

    /* One brick of a wall: where it stands (its size is its scale) and how
     * many hits it takes. */
    struct WallBrick
    {
        GameCore::TransformPose Pose;
        int32_t                 Hits = 1;
    };

    /* The wall the level builds without a level file: BrickRows x
     * BrickColumns, centred, 1 hit on the bottom row and one more per row up. */
    std::vector<WallBrick> DefaultWall()
    {
        std::vector<WallBrick> Wall;
        const double Left = (FieldWidth - (BrickColumns * BrickWidth + (BrickColumns - 1) * BrickGap)) * 0.5
                            + BrickWidth * 0.5;
        for(int Row = 0; Row < BrickRows; ++Row)
        {
            for(int Column = 0; Column < BrickColumns; ++Column)
            {
                WallBrick Brick;
                Brick.Pose = Placed(Left + Column * (BrickWidth + BrickGap), BrickTop - Row * (BrickHeight + BrickGap),
                                    BrickWidth, BrickHeight);
                Brick.Hits = BrickRows - Row;   // row 0 is the top
                Wall.push_back(Brick);
            }
        }
        return Wall;
    }

    class BrickBreakerGameMode : public NimbleEngine::IGameMode
    {
    public:
        explicit BrickBreakerGameMode(BrickBreakerGameState& InState) : State(&InState) {}

        bool OnLoad(NimbleEngine::GameSession& InSession) override
        {
            Session    = &InSession;
            bRestored  = false;
            Elapsed    = 0.0;
            RunSeconds = double(GBrickBreakerRunSeconds.GetInt());

            /* From the brick's script graph (Events.Emit("BrickBroken")). */
            BrickBrokenHandle = InSession.GetWorld().GetEvents().Subscribe("BrickBroken", &OnBrickBroken, this);

            /* The World is as the level made it (before any restore): its
             * bricks are the wall every round rebuilds. */
            Wall = CaptureWall();
            bLevelHasWall = !Wall.empty();
            if(!bLevelHasWall)
            {
                Wall = DefaultWall();
            }
            return true;
        }

        /* The save brought its own bricks. */
        void OnPostLoadSave() override { bRestored = true; }

        void OnStart() override
        {
            /* A save or the level file brought this round's bricks already. */
            if(bRestored || bLevelHasWall)
            {
                State->BricksRemaining = CountBricks();
            }
            else
            {
                SpawnBricks();
            }
            CORE_LOG("[BrickBreaker] " + std::to_string(State->BricksRemaining) + " bricks; MoveX steers, Jump launches"
                     + (GBrickBreakerAutoPlay.GetInt() != 0 ? " (AutoPlay on)" : ""))
            Session->SetPhase(Phases::Serving);
        }

        void OnTick(const NimbleEngine::FrameContext& InFrame) override
        {
            Elapsed += InFrame.DeltaSeconds;
            if(RunSeconds > 0.0 && Elapsed >= RunSeconds)
            {
                CORE_LOG("[BrickBreaker] RunSeconds reached; asking the engine to quit")
                Session->RequestQuit();
                RunSeconds = 0.0;
            }

            /* A pause on the empty field, then the next round. */
            if(State->IsPhase(Phases::RoundCleared) && State->GetPhaseSeconds() >= RoundClearedSeconds)
            {
                State->SetRound(State->GetRound() + 1);
                SpawnBricks();
                Session->SetPhase(Phases::Serving);
            }
        }

        void OnUnload() override
        {
            if(Session && Session->HasWorld())
            {
                Session->GetWorld().GetEvents().Unsubscribe(Session->GetWorld().GetEvents().FindEventID("BrickBroken"),
                                                            BrickBrokenHandle);
            }
            Session = nullptr;
        }

        // === Rules ==============================================================

        bool CanEnterPhase(const char* InFrom, const char* InTo) override
        {
            /* The ball is only launched from the paddle, and a round is only
             * cleared by playing it. */
            if(std::strcmp(InTo, Phases::Playing) == 0)      { return std::strcmp(InFrom, Phases::Serving) == 0; }
            if(std::strcmp(InTo, Phases::RoundCleared) == 0) { return std::strcmp(InFrom, Phases::Playing) == 0; }
            return true;
        }

        void OnPhaseChanged(const char* InPrevious, const char* InCurrent) override
        {
            /* GameOver -> Serving is a new game. */
            if(std::strcmp(InPrevious, NimbleEngine::GamePhases::GameOver) == 0 && std::strcmp(InCurrent, Phases::Serving) == 0)
            {
                RestartGame();
            }

            if(std::strcmp(InCurrent, NimbleEngine::GamePhases::GameOver) == 0)
            {
                CORE_LOG("[BrickBreaker] GAME OVER -- final score " + std::to_string(PlayerScore())
                         + " in round " + std::to_string(State->GetRound() + 1) + "; Jump to play again")
            }
            else if(std::strcmp(InCurrent, Phases::RoundCleared) == 0)
            {
                CORE_LOG("[BrickBreaker] round " + std::to_string(State->GetRound() + 1) + " cleared")
            }
        }

        void OnPlayerAdded(const GameCore::EntityID InPlayer) override
        {
            Player = InPlayer;
        }

        void OnScoreChanged(GameCore::EntityID /*InPlayer*/, const int32_t InScore) override
        {
            CORE_LOG("[BrickBreaker] score " + std::to_string(InScore))
        }

        /* The ball's script costs a life (Game.AddLives) when it falls out. */
        void OnLivesChanged(GameCore::EntityID /*InPlayer*/, const int32_t InLives) override
        {
            CORE_LOG("[BrickBreaker] ball lost; " + std::to_string(InLives) + (InLives == 1 ? " life" : " lives") + " left")
            Session->SetPhase(InLives > 0 ? Phases::Serving : NimbleEngine::GamePhases::GameOver);
        }

    private:
        static void OnBrickBroken(GameCore::EventID, const GameCore::EventArgs&, void* InUserData)
        {
            auto* Mode = static_cast<BrickBreakerGameMode*>(InUserData);
            Mode->State->BricksRemaining = std::max(0, Mode->State->BricksRemaining - 1);
            if(Mode->State->BricksRemaining == 0)
            {
                Mode->Session->SetPhase(Phases::RoundCleared);   // refused unless Playing
            }
        }

        int32_t PlayerScore() const
        {
            const NimbleEngine::PlayerStateComponent* P =
                Session->GetWorld().GetEntity(Player).GetComponent<NimbleEngine::PlayerStateComponent>();
            return P ? P->Score : 0;
        }

        /* The wall, as the level laid it out. */
        void SpawnBricks()
        {
            GameCore::World& World = Session->GetWorld();
            int32_t Spawned = 0;
            for(const WallBrick& Brick : Wall)
            {
                const GameCore::Entity Root = World.SpawnPrefab(BrickPrefab, Brick.Pose).GetRoot();
                SandboxBrickComponent* Hits = Root.GetComponent<SandboxBrickComponent>();
                if(!Hits)
                {
                    CORE_LOG_ERROR(std::string("[BrickBreaker] could not spawn ") + BrickPrefab + " (or it has no Brick component)")
                    break;
                }
                Hits->Hits = Brick.Hits;
                ++Spawned;
            }
            State->BricksRemaining = Spawned;
        }

        /* The bricks in the World now: placement and hits. */
        std::vector<WallBrick> CaptureWall() const
        {
            std::vector<WallBrick> Captured;
            GameCore::World& World = Session->GetWorld();
            for(const GameCore::PrefabInstanceID Instance : FindBricks())
            {
                const GameCore::Entity Root = World.GetInstance(Instance).GetRoot();
                WallBrick Brick;
                Brick.Pose = Root.GetWorldPose();
                if(const SandboxBrickComponent* Hits = Root.GetComponent<SandboxBrickComponent>())
                {
                    Brick.Hits = Hits->Hits;
                }
                Captured.push_back(Brick);
            }
            return Captured;
        }

        /* Live instances of the brick prefab. */
        std::vector<GameCore::PrefabInstanceID> FindBricks() const
        {
            struct Context
            {
                GameCore::World*                        World;
                std::vector<GameCore::PrefabInstanceID> Found;
            } Ctx{ &Session->GetWorld(), {} };

            Ctx.World->ForEachInstance([](const GameCore::PrefabInstanceID InInstance, void* InUserData)
            {
                auto& C = *static_cast<Context*>(InUserData);
                if(std::strcmp(C.World->GetInstance(InInstance).GetPrefabPath(), BrickPrefab) == 0)
                {
                    C.Found.push_back(InInstance);
                }
            }, &Ctx);
            return Ctx.Found;
        }

        int32_t CountBricks() const { return int32_t(FindBricks().size()); }

        /* Score and lives back to the start, a fresh wall. Inside the paddle's
         * PrePhysics: the old bricks go at the phase flush, the new ones are
         * already there. */
        void RestartGame()
        {
            GameCore::World& World = Session->GetWorld();
            for(const GameCore::PrefabInstanceID Brick : FindBricks())
            {
                World.DestroyPrefabInstance(Brick);
            }

            /* Setting up, not scoring: written directly, no hooks. */
            if(NimbleEngine::PlayerStateComponent* P = World.GetEntity(Player).GetComponent<NimbleEngine::PlayerStateComponent>())
            {
                P->Score = 0;
                P->Lives = StartingLives;
            }
            State->SetRound(0);
            SpawnBricks();
            CORE_LOG("[BrickBreaker] new game")
        }

        NimbleEngine::GameSession* Session = nullptr;
        BrickBreakerGameState*     State   = nullptr;
        GameCore::EntityID         Player;
        GameCore::DelegateHandle   BrickBrokenHandle;
        std::vector<WallBrick>     Wall;                    // every round's bricks
        bool                       bLevelHasWall = false;   // the level file placed the first
        bool                       bRestored  = false;
        double                     Elapsed    = 0.0;
        double                     RunSeconds = 0.0;
    };

    // === The level ==============================================================

    class BrickBreakerLevel : public NimbleEngine::ILevel
    {
    public:
        BrickBreakerLevel() : Mode(State) {}

        const char* GetName() const override { return "BrickBreaker"; }
        const char* GetWorldFile() const override { return GBrickBreakerLevelFile.GetString(); }

        /* The scene is all the level's own: not Sandbox's Scripts/Main.lua. */
        const char* GetStartupScript() const override { return ""; }

        bool OnLoad(NimbleEngine::GameSession& /*InSession*/) override
        {
            /* An editor build reads the text level; a game, its cooked twin
             * in Content/Levels/Cooked/. */
            std::string Path = GBrickBreakerLevelFile.GetString();
#if !WITH_EDITOR
            const size_t Slash = Path.find_last_of('/');
            Path = Path.substr(0, Slash == std::string::npos ? 0 : Slash + 1) + "Cooked/"
                   + Path.substr(Slash == std::string::npos ? 0 : Slash + 1);
#endif
            if(!Path.empty() && GameCore::DetectWorldFileFormat(Path.c_str()) != GameCore::EWorldFileFormat::Unknown)
            {
                if(GameCore::LoadWorldFile(Path.c_str(), GetWorld()))
                {
                    CORE_LOG("[BrickBreaker] layout from " + Path)
                    return true;
                }
                CORE_LOG_ERROR("[BrickBreaker] could not load " + Path)
                return false;
            }
            CORE_LOG("[BrickBreaker] no level file" + (Path.empty() ? std::string() : " at " + Path)
                     + "; building the default layout")
            return SpawnDefaultLayout();
        }

        /* What Content/Levels/BrickBreaker.nmworld holds, in code: camera, paddle,
         * ball. The mode adds the default wall. */
        bool SpawnDefaultLayout()
        {
            GameCore::World& World = GetWorld();
            World.SpawnPrefab("native:Sandbox.Camera");
            World.SpawnPrefab("native:BrickBreaker.Paddle", Placed(FieldWidth * 0.5, PaddleY, PaddleWidth, PaddleHeight));

            if(!World.SpawnPrefab(BallPrefab, Placed(FieldWidth * 0.5, PaddleY + 20.0, BallSize, BallSize)).IsValid())
            {
                CORE_LOG_ERROR(std::string("[BrickBreaker] could not spawn ") + BallPrefab)
                return false;
            }
            return true;
        }

        /* A restore brings its own copy of everything (Level.h). */
        void OnPrepareForLoadedGame() override
        {
            std::vector<GameCore::EntityID> Doomed;
            GetWorld().ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
            {
                static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
            }, &Doomed);
            for(const GameCore::EntityID Entity : Doomed)
            {
                GetWorld().DestroyEntity(Entity);
            }
        }

        NimbleEngine::IGameMode* GetGameMode() override  { return &Mode; }
        NimbleEngine::GameState* GetGameState() override { return &State; }

    private:
        BrickBreakerGameState State;
        BrickBreakerGameMode  Mode;
    };

    BrickBreakerLevel GLevel;
}

NimbleEngine::ILevel& BrickBreaker::GetLevel()
{
    return GLevel;
}

#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderTypes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

/*
 * WorldsLabGrassLab: the GrassLab world (Tools/GrassLab/make_grasslab_level.py),
 * which shows custom render passes (Engine/Modules/RenderCore/RENDERPIPELINE.md,
 * "Custom render passes") at work. A ball rolls through procedural grass and
 * the grass bends away from it, then springs back:
 *
 *   GrassTrailPass  BeforeScene. Two half-float targets ping-pong a top-down
 *                   map of the field: each frame the last one eases back to rest
 *                   and the ball, where it touches the ground, presses into it
 *                   (GrassLabTrailUpdate.fsh); then it is copied into the render
 *                   target asset GrassTrail.nmrt2d (GrassLabTrailPublish.fsh),
 *                   which the grass material's World Position Offset samples --
 *                   the same frame, as the scene draws after it.
 *   BallXRayPass    AfterForward. The ball's mesh group (its RenderComponent's
 *                   CustomPassGroups) into a mask with its own depth, then, where
 *                   the scene's depth is nearer -- grass in front of it -- its
 *                   silhouette over the scene, tinted (GrassLabBallXRay.fsh).
 *   TrailStatsPass  BeforeScene. A compute shader adds up how far the ball has
 *                   rolled, in a buffer only the GPU writes (GrassLabTrailStats.csh);
 *                   GrassLabStats=1 reads it back now and then and logs it.
 *
 * The grass's LOD rings are centred on the ball (the Field spawner's LODFocus):
 * full clumps within 9 m of it, fewer and simpler past that, none past
 * GrassLabCullRadius (GrassLabRings=0: round the camera, nothing culled).
 *
 * The ball rolls a figure of eight on its own (GrassLabAutoPlay, the default);
 * with it off, WASD (or the left stick) push it, relative to the camera, which
 * follows it either way. [WorldsLab] GrassLabTrail=0 and GrassLabXRay=0 turn
 * the passes off (the console: WorldsLab.GrassLabTrail 0), to compare.
 *
 * FIELD is the square the trail covers; make_grasslab_materials.py's FIELD is
 * the same, in the grass material: keep them so.
 */

NIMBLE_CONFIG_BOOL(GGrassLabAutoPlay, "WorldsLab", "GrassLabAutoPlay", true,  nullptr);
NIMBLE_CONFIG_BOOL(GGrassLabTrail,    "WorldsLab", "GrassLabTrail",    true,  nullptr);
NIMBLE_CONFIG_BOOL(GGrassLabXRay,     "WorldsLab", "GrassLabXRay",     true,  nullptr);
NIMBLE_CONFIG_BOOL(GGrassLabStats,    "WorldsLab", "GrassLabStats",    false, nullptr);
NIMBLE_CONFIG_FLOAT(GGrassLabSpeed,   "WorldsLab", "GrassLabSpeed",    450.0f, nullptr);   // the autoplay's pace, cm/s
/* The grass's LOD rings round the ball, not the camera (the Field spawner's LODFocus): full clumps
 * near it, fewer and simpler further out, none past GrassLabCullRadius. */
NIMBLE_CONFIG_BOOL(GGrassLabRings,           "WorldsLab", "GrassLabRings",          true,    nullptr);
NIMBLE_CONFIG_FLOAT(GGrassLabCullRadius,     "WorldsLab", "GrassLabCullRadius",     3200.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GGrassLabCameraHeight,   "WorldsLab", "GrassLabCameraHeight",   520.0f, nullptr);   // the follow camera, above the ball
NIMBLE_CONFIG_FLOAT(GGrassLabCameraDistance, "WorldsLab", "GrassLabCameraDistance", 900.0f, nullptr);   // and back from it
NIMBLE_CONFIG_FLOAT(GGrassLabRecovery, "WorldsLab", "GrassLabRecovery", 1.4f, nullptr);    // seconds for the grass to come back most of the way

namespace
{
    constexpr float  FieldMin[2]  = { -2400.0f, -2400.0f };
    constexpr float  FieldSize[2] = { 4800.0f, 4800.0f };
    constexpr char   TrailTarget[] = "Content/RenderTargets/GrassTrail.nmrt2d";
    constexpr uint32_t TrailTexels = 512;
    constexpr double Pi = 3.14159265358979323846;

    /* What the passes see of the ball: the game thread's, a frame at a time (Extract). */
    struct BallState
    {
        float    Position[3] = {};
        float    Velocity[3] = {};
        float    Radius      = 50.0f;
        float    Touch       = 0.0f;   // 1 on the ground, 0 a ball's radius above it
        bool     bValid      = false;
        bool     bTrail      = true;
        bool     bXRay       = true;
        bool     bStats      = false;
        uint32_t StatsAsk    = 0;      // bumped when the game wants the stats read back
    };

    /* The game thread's side of a pass: the mode's state, put each frame. */
    struct BallFeed
    {
        RenderCore::RenderPassMailbox<BallState> Mailbox;
    };

    /* The stats read back on the render thread, for the game thread to log. */
    struct StatsResult
    {
        float    Distance = 0.0f;
        float    Frames   = 0.0f;
        uint32_t Answer   = 0;   // the StatsAsk it answers
    };

    void FillBall(RenderCore::RenderPassParams& OutParams, const BallState& InBall)
    {
        OutParams.Matrix[0] = InBall.Position[0];
        OutParams.Matrix[1] = InBall.Position[1];
        OutParams.Matrix[2] = InBall.Position[2];
        OutParams.Matrix[3] = InBall.Radius;
    }

    class GrassTrailPass final : public RenderCore::CustomRenderPass
    {
    public:
        explicit GrassTrailPass(std::shared_ptr<BallFeed> InFeed) : Feed(std::move(InFeed)) {}

        const char* GetName() const override { return "GrassTrail"; }

        void Execute(RenderCore::RenderPassContext& InContext) override
        {
            using namespace RenderCore;
            const BallState Ball = Feed->Mailbox.Get();
            const RenderPassTarget Published = InContext.AcquireAssetTarget(TrailTarget);
            if(!Ball.bValid || !Ball.bTrail || !Published.IsValid())
            {
                /* Off: the grass at rest, and the trail starts over when it is back on. */
                const float Rest[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                InContext.Clear(Published, Rest);
                bReset = true;
                return;
            }

            RenderPassTargetDesc Desc;
            Desc.Width  = TrailTexels;
            Desc.Height = TrailTexels;
            Desc.Format = ERenderPassFormat::RGBA16F;
            const RenderPassTarget Trails[2] = { InContext.AcquireTarget("TrailA", Desc), InContext.AcquireTarget("TrailB", Desc) };
            const RenderPassTarget Last = Trails[Current];
            const RenderPassTarget Next = Trails[1 - Current];

            /* The roll along the ground, 0..1: how much the push leans its way. */
            const float Speed = std::sqrt(Ball.Velocity[0] * Ball.Velocity[0] + Ball.Velocity[2] * Ball.Velocity[2]);
            const float Lean  = Speed > 1.0f ? std::min(Speed / 400.0f, 1.0f) / Speed : 0.0f;

            RenderPassParams Params;
            FillBall(Params, Ball);
            Params.Matrix[4]  = Ball.Velocity[0] * Lean;
            Params.Matrix[5]  = Ball.Velocity[2] * Lean;
            Params.Matrix[6]  = Ball.Touch;
            Params.Matrix[8]  = std::clamp(InContext.GetDeltaSeconds(), 0.0f, 0.1f);
            Params.Matrix[9]  = std::max(GGrassLabRecovery.GetFloat(), 0.05f);
            Params.Matrix[10] = 2.2f;                      // reach, in radii
            Params.Matrix[11] = bReset ? 1.0f : 0.0f;
            Params.A[0] = FieldMin[0];
            Params.A[1] = FieldMin[1];
            Params.A[2] = FieldSize[0];
            Params.A[3] = FieldSize[1];
            if(!InContext.DrawFullscreen("GrassLabTrailUpdate.fsh", Next, { InContext.AsTexture(Last) }, Params))
            {
                return;   // warned once: the shader is not cooked
            }
            InContext.DrawFullscreen("GrassLabTrailPublish.fsh", Published, { InContext.AsTexture(Next) }, Params);
            Current = 1 - Current;
            bReset  = false;
        }

    private:
        std::shared_ptr<BallFeed> Feed;
        int  Current = 0;      // render thread
        bool bReset  = true;
    };

    class BallXRayPass final : public RenderCore::CustomRenderPass
    {
    public:
        explicit BallXRayPass(std::shared_ptr<BallFeed> InFeed) : Feed(std::move(InFeed)) {}

        const char* GetName() const override { return "BallXRay"; }

        void Execute(RenderCore::RenderPassContext& InContext) override
        {
            using namespace RenderCore;
            if(!Feed->Mailbox.Get().bXRay || InContext.GetMeshGroupDrawCount(0) == 0)
            {
                return;
            }
            const RenderPassTarget Mask = InContext.AcquireTarget("Mask", RenderPassTargetDesc());
            if(!InContext.DrawMeshGroup(0, "GrassLabBallMask.fsh", Mask, ERenderPassDepth::Own, /*bInClear*/ true))
            {
                return;
            }
            RenderPassParams Params;
            /* Linear light: a warm glow, half through, its rim stronger. */
            Params.A[0] = 1.6f;
            Params.A[1] = 0.62f;
            Params.A[2] = 0.18f;
            Params.A[3] = 0.4f;
            Params.B[0] = 0.45f;
            InContext.DrawFullscreen("GrassLabBallXRay.fsh", RenderPassTarget(), { InContext.AsTexture(Mask), InContext.AsDepthTexture(Mask) },
                                     Params, ERenderPassBlend::AlphaBlend);
        }

    private:
        std::shared_ptr<BallFeed> Feed;
    };

    class TrailStatsPass final : public RenderCore::CustomRenderPass
    {
    public:
        TrailStatsPass(std::shared_ptr<BallFeed> InFeed, std::shared_ptr<RenderCore::RenderPassMailbox<StatsResult>> InResult)
            : Feed(std::move(InFeed)), Result(std::move(InResult)) {}

        const char* GetName() const override { return "TrailStats"; }

        void Execute(RenderCore::RenderPassContext& InContext) override
        {
            using namespace RenderCore;
            const BallState Ball = Feed->Mailbox.Get();
            if(!Ball.bValid || !InContext.SupportsCompute())
            {
                return;
            }
            const float Zeros[8] = {};
            const RenderPassBuffer Totals = InContext.AcquireBuffer("Totals", uint32_t(sizeof(Zeros)), Zeros);
            const float Params[4] = { Ball.Position[0], Ball.Position[1], Ball.Position[2], bStarted ? 0.0f : 1.0f };
            if(!InContext.Dispatch("GrassLabTrailStats.csh", Params, uint32_t(sizeof(Params)), { Totals }, 1))
            {
                return;
            }
            bStarted = true;
            /* Read back only when asked: it waits for the GPU. */
            if(Ball.bStats && Ball.StatsAsk != Answered)
            {
                std::vector<uint8_t> Bytes;
                if(InContext.ReadBuffer(Totals, Bytes) && Bytes.size() >= sizeof(Zeros))
                {
                    float Values[8];
                    std::memcpy(Values, Bytes.data(), sizeof(Values));
                    StatsResult Out;
                    Out.Distance = Values[4];
                    Out.Frames   = Values[5];
                    Out.Answer   = Ball.StatsAsk;
                    Result->Put(Out);
                }
                Answered = Ball.StatsAsk;
            }
        }

    private:
        std::shared_ptr<BallFeed> Feed;
        std::shared_ptr<RenderCore::RenderPassMailbox<StatsResult>> Result;
        uint32_t Answered = 0;
        bool     bStarted = false;
    };

    /* (Pitch, Yaw) in degrees looking along InDirection (WorldsLabExplore's ViewForward, inverted). */
    void LookAlong(const EngineCore::Vector3& InDirection, double& OutPitch, double& OutYaw)
    {
        const double X = InDirection.GetX(), Y = InDirection.GetY(), Z = InDirection.GetZ();
        OutYaw   = std::atan2(X, Z) * 180.0 / Pi;
        OutPitch = -std::atan2(Y, std::sqrt(X * X + Z * Z)) * 180.0 / Pi;
    }
}

class WorldsLabGrassLabMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GameCore::World& World = InSession.GetWorld();
        Player = World.FindEntityByName("Player");
        Ball   = World.FindEntityByName("Ball");
        Field  = World.FindEntityByName("Field");
        if(!Ball.IsSet())
        {
            CORE_LOG_ERROR("[GrassLab] the world has no entity named Ball; nothing rolls")
            return true;
        }
        if(const GameCore::TransformComponent* Where = World.GetEntity(Ball).GetComponent<GameCore::TransformComponent>())
        {
            BallRadius = float(Where->Scale.GetX()) * 0.5f;   // Sphere.nmmesh is 1 across
            Home       = Where->Position;
        }

        Feed    = std::make_shared<BallFeed>();
        Results = std::make_shared<RenderCore::RenderPassMailbox<StatsResult>>();
        Passes.push_back(RenderCore::RegisterCustomRenderPass(&World, std::make_shared<GrassTrailPass>(Feed), RenderCore::ERenderHook::BeforeScene));
        Passes.push_back(RenderCore::RegisterCustomRenderPass(&World, std::make_shared<TrailStatsPass>(Feed, Results),
                                                              RenderCore::ERenderHook::BeforeScene, 10));
        Passes.push_back(RenderCore::RegisterCustomRenderPass(&World, std::make_shared<BallXRayPass>(Feed), RenderCore::ERenderHook::AfterForward));

        AddSystem<&WorldsLabGrassLabMode::StepBall>("WorldsLab.GrassLabBall", GameCore::TP_PrePhysics);
        AddSystem<&WorldsLabGrassLabMode::StepCamera>("WorldsLab.GrassLabCamera", GameCore::TP_PostPhysics);
        return true;
    }

    void OnStart() override
    {
        CORE_LOG("[GrassLab] the ball rolls through the grass; WorldsLab.GrassLabAutoPlay 0 to push it with WASD, "
                 "GrassLabTrail / GrassLabXRay 0 to turn the passes off, GrassLabStats 1 to log the GPU's tally")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        if(!GGrassLabStats.GetBool() || !Feed)
        {
            return;
        }
        StatsClock += InFrame.DeltaSeconds;
        if(StatsClock >= 2.0)
        {
            StatsClock = 0.0;
            ++StatsAsk;
        }
        const StatsResult Got = Results->Get();
        if(Got.Answer != 0 && Got.Answer != LoggedAnswer)
        {
            LoggedAnswer = Got.Answer;
            const double Speed = std::sqrt(double(LastBall.Velocity[0]) * LastBall.Velocity[0]
                                         + double(LastBall.Velocity[2]) * LastBall.Velocity[2]);
            /* The scene's GPU time ("stat frame" must be on for the GPU to be timed). */
            RenderCore::RenderStats Render;
            Session->GetRenderer().GetStats(Render);
            char Line[320];
            std::snprintf(Line, sizeof(Line), "[GrassLab] stats (GPU): the ball has rolled %.1f m over %.0f frames; "
                          "now at (%.0f, %.0f, %.0f), %.0f cm/s, touching %.2f; scene GPU %.2f ms, render thread %.2f ms",
                          double(Got.Distance) / 100.0, double(Got.Frames), double(LastBall.Position[0]),
                          double(LastBall.Position[1]), double(LastBall.Position[2]), Speed, double(LastBall.Touch),
                          double(Render.GpuMs[RenderCore::RGT_Total]), double(Render.RenderThreadMs));
            CORE_LOG(std::string(Line))
        }
    }

    void OnUnload() override
    {
        for(const RenderCore::RenderPassHandle Each : Passes) { RenderCore::UnregisterCustomRenderPass(Each); }
        Passes.clear();
        /* The render thread may still hold the passes for a frame in flight: their
         * code is this module's, which may be unloaded next (a hot reload). */
        RenderCore::FlushRenderCommands();
        Feed.reset();
        Results.reset();
    }

private:
    /* The ball: pushed toward a point running round a figure of eight, or by the player. */
    void StepBall(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        const GameCore::Entity Entity = InWorld.GetEntity(Ball);
        GameCore::RigidBodyComponent* Body = Entity.GetComponent<GameCore::RigidBodyComponent>();
        const GameCore::TransformComponent* Where = Entity.GetComponent<GameCore::TransformComponent>();
        if(!Body || !Where)
        {
            return;
        }
        Time += double(InDeltaSeconds);
        const EngineCore::Vector3 Position = Where->Position;
        const EngineCore::Vector3 Velocity = Body->GetLinearVelocity();
        const double Mass = 25.0;

        double Want[2] = { 0.0, 0.0 };   // the acceleration asked for, along the ground
        if(GGrassLabAutoPlay.GetBool())
        {
            /* A lemniscate 32 m end to end; the point's pace is about GrassLabSpeed. */
            const double Scale = 1600.0;
            Phase += double(InDeltaSeconds) * double(GGrassLabSpeed.GetFloat()) / (Scale * 1.4);
            const double S = std::sin(Phase), C = std::cos(Phase);
            const double Denominator = 1.0 + S * S;
            const double TargetX = Home.GetX() + Scale * C / Denominator;
            const double TargetZ = Home.GetZ() + Scale * S * C / Denominator;
            Want[0] = 3.0 * (TargetX - Position.GetX()) - 2.2 * Velocity.GetX();
            Want[1] = 3.0 * (TargetZ - Position.GetZ()) - 2.2 * Velocity.GetZ();
        }
        else if(Player.IsSet())
        {
            if(const NimbleEngine::InputComponent* Input = InWorld.GetEntity(Player).GetComponent<NimbleEngine::InputComponent>())
            {
                const double Forward = Input->GetAxis("MoveForward");
                const double Right   = Input->GetAxis("MoveRight");
                const double Yaw = CameraYaw * Pi / 180.0;
                const double Push = 900.0;
                Want[0] = (std::sin(Yaw) * Forward + std::cos(Yaw) * Right) * Push;
                Want[1] = (std::cos(Yaw) * Forward - std::sin(Yaw) * Right) * Push;
            }
        }
        const double Limit = 1200.0;   // cm/s^2
        const double Size  = std::sqrt(Want[0] * Want[0] + Want[1] * Want[1]);
        const double Scale = Size > Limit ? Limit / Size : 1.0;
        Body->AddForce(EngineCore::Vector3(Want[0] * Scale * Mass, 0.0, Want[1] * Scale * Mass));

        /* Fallen off somewhere: back to the middle. */
        if(Position.GetY() < -2000.0)
        {
            Body->SetLinearVelocity(EngineCore::Vector3(0.0, 0.0, 0.0));
            if(GameCore::TransformComponent* Move = Entity.GetComponent<GameCore::TransformComponent>()) { Move->Position = Home; }
        }

        /* The grass's rings round the ball. */
        if(Field.IsSet())
        {
            if(RenderCore::ProceduralSpawnerComponent* Spawner = InWorld.GetEntity(Field).GetComponent<RenderCore::ProceduralSpawnerComponent>())
            {
                const bool bRings = GGrassLabRings.GetBool();
                Spawner->bLODFocus    = bRings ? 1 : 0;
                Spawner->LODFocus     = Position;
                Spawner->CullDistance = bRings ? std::max(GGrassLabCullRadius.GetFloat(), 0.0f) : 0.0f;
            }
        }

        /* For the passes: where it is, how it moves, whether it touches the ground. */
        float Ground = 0.0f;
        const bool bGround = NimbleEngine::SampleGround(InWorld, Position, Ground);
        const float Above  = float(Position.GetY()) - BallRadius - (bGround ? Ground : 0.0f);
        BallState State;
        State.Position[0] = float(Position.GetX());
        State.Position[1] = float(Position.GetY());
        State.Position[2] = float(Position.GetZ());
        State.Velocity[0] = float(Velocity.GetX());
        State.Velocity[1] = float(Velocity.GetY());
        State.Velocity[2] = float(Velocity.GetZ());
        State.Radius   = BallRadius;
        State.Touch    = std::clamp(1.0f - Above / BallRadius, 0.0f, 1.0f);
        State.bValid   = true;
        State.bTrail   = GGrassLabTrail.GetBool();
        State.bXRay    = GGrassLabXRay.GetBool();
        State.bStats   = GGrassLabStats.GetBool();
        State.StatsAsk = StatsAsk;
        Feed->Mailbox.Put(State);
        LastBall = State;
    }

    /* The camera: behind and above the ball, easing after it, slowly circling. */
    void StepCamera(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        GameCore::TransformComponent* View = Player.IsSet() ? InWorld.GetEntity(Player).GetComponent<GameCore::TransformComponent>() : nullptr;
        const GameCore::TransformComponent* Where = InWorld.GetEntity(Ball).GetComponent<GameCore::TransformComponent>();
        if(!View || !Where)
        {
            return;
        }
        const double Ease = 1.0 - std::exp(-double(InDeltaSeconds) * 2.5);
        Focus = bFocusSet ? Focus + (Where->Position - Focus) * Ease : Where->Position;
        bFocusSet = true;
        const double Around = Time * 0.12;
        const double Back = double(GGrassLabCameraDistance.GetFloat());
        const EngineCore::Vector3 Eye = Focus + EngineCore::Vector3(std::sin(Around) * Back, double(GGrassLabCameraHeight.GetFloat()),
                                                                    -std::cos(Around) * Back);
        double Pitch = 0.0, Yaw = 0.0;
        LookAlong(Focus - Eye, Pitch, Yaw);
        View->Position = Eye;
        View->Rotation = EngineCore::Vector3(Pitch, Yaw, 0.0);
        CameraYaw = Yaw;
    }

    GameCore::EntityID  Player;
    GameCore::EntityID  Ball;
    GameCore::EntityID  Field;
    EngineCore::Vector3 Home;
    EngineCore::Vector3 Focus;
    bool                bFocusSet  = false;
    float               BallRadius = 50.0f;
    double              Time       = 0.0;
    double              Phase      = 0.0;
    double              CameraYaw  = 0.0;
    double              StatsClock = 0.0;
    uint32_t            StatsAsk   = 1;
    uint32_t            LoggedAnswer = 0;
    BallState           LastBall;            // what the passes were last given

    std::shared_ptr<BallFeed>                                    Feed;
    std::shared_ptr<RenderCore::RenderPassMailbox<StatsResult>>  Results;
    std::vector<RenderCore::RenderPassHandle>                    Passes;
};

NIMBLE_REGISTER_GAME_MODE(WorldsLabGrassLabMode, "WorldsLabGrassLab")

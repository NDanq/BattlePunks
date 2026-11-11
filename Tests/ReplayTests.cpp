/*
 * NimbleEngineReplayTests -- GameCore's replays (Replay.h): what a recorder
 * keeps (keyframes, deltas, spawns, destroys, removed components, static
 * entities skipped), playing a clip into another World (puppets spawned,
 * existing entities bound by GUID, entity references resolved), seeking
 * both ways, Transform interpolation, Advance/loop, rolling clips, the
 * recorder driven by World::Tick, and the clip's file round trip.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/Replay.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"

using namespace GameCore;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

/* A gameplay component with an entity reference and a per-run field the
 * serializer skips: playback must resolve the first and leave the second. */
struct ReplayTestComponent
{
    GAMECORE_COMPONENT_BODY()
    int32_t  Score   = 0;
    EntityID Target;
    int32_t  RunOnly = 0;   // not serialized
};
GAMECORE_DEFINE_COMPONENT(ReplayTestComponent, "ReplayTest")
GAMECORE_DEFINE_COMPONENT_SERIALIZER(ReplayTestComponent, 1)
{
    Ar.Field("Score", C.Score);
    Ar.Entity("Target", C.Target);
}


/* Docs/Guides/ReplayGuide.md's C++ example, word for word: a kill cam. */
// --- guide: KillCam ----------------------------------------------------------
/* The last 5 seconds always in memory; Show() plays them back over the live
 * level, and Finish() hands the level back and starts recording again. */
class KillCam
{
public:
    void Begin(GameCore::World& InWorld)
    {
        TheWorld = &InWorld;
        StartRecording();
    }

    /* From gameplay code: a game mode, a script, any system. */
    void Show()
    {
        Recorder.reset();   // stops it; the clip stays
        Player = std::make_unique<GameCore::ReplayPlayer>(*TheWorld, Clip);
        Player->SetHold(true);   // the clip wins over the live level every frame
        Time = Clip.GetStartTime();
        bDone = false;
        TheWorld->RegisterSystem("KillCam", GameCore::TP_PostUpdate, &KillCam::Tick, this, GameCore::SO_BeforeScripts);
    }

    bool IsDone() const { return bDone; }

    void Finish()
    {
        TheWorld->UnregisterSystem(&KillCam::Tick, this);
        Player->DestroySpawnedEntities();   // puppets of what had died since
        Player.reset();
        StartRecording();
    }

private:
    void StartRecording()
    {
        GameCore::ReplayRecordSettings Settings;
        Settings.MaxDuration      = 5.0f;   // a rolling buffer
        Settings.SampleRate       = 30.0f;
        Settings.KeyframeInterval = 1.0f;   // short clips: fast seeks over small files
        Recorder = std::make_unique<GameCore::ReplayRecorder>(Clip, Settings);   // clears the clip
        Recorder->RecordOnly({ "Transform", "Render", "Animation" });
        Recorder->Start(*TheWorld);
    }

    /* Late in TP_PostUpdate, ahead of RenderCore.Animation: after every
     * gameplay system, so the replay has the last word on what is drawn. */
    static void Tick(GameCore::World&, const float InSeconds, void* InUserData)
    {
        KillCam& Self = *static_cast<KillCam*>(InUserData);
        Self.Time = std::min(Self.Time + double(InSeconds) * 0.5, Self.Clip.GetEndTime());   // half speed
        Self.Player->Seek(Self.Time);
        Self.bDone = Self.Time >= Self.Clip.GetEndTime();
    }

    GameCore::World*                          TheWorld = nullptr;
    GameCore::ReplayClip                      Clip;
    std::unique_ptr<GameCore::ReplayRecorder> Recorder;
    std::unique_ptr<GameCore::ReplayPlayer>   Player;
    double                                    Time  = 0.0;
    bool                                      bDone = false;
};
// --- end guide ---------------------------------------------------------------

namespace
{
    bool Near(const double A, const double B, const double Tolerance = 1e-3) { return std::fabs(A - B) <= Tolerance; }

    TransformComponent* TransformOf(World& InWorld, const EntityID InId)
    {
        return InWorld.IsAlive(InId) ? InWorld.GetEntity(InId).GetComponent<TransformComponent>() : nullptr;
    }

    double XOf(World& InWorld, const EntityID InId)
    {
        const TransformComponent* T = TransformOf(InWorld, InId);
        return T ? double(T->Position.GetX()) : -1.0e9;
    }

    /* The scripted recording most tests share, frames at 0.1 s steps:
     *   Mover   x = 10 * t, Score = frame / 4; its ReplayTest removed at frame 15
     *   Target  still; Mover's ReplayTest points at it
     *   Bullet  spawned at frame 5, destroyed at frame 8
     *   Wall    static: never recorded
     *   Child   parented to Mover, reparented to Target at frame 10 */
    struct Scene
    {
        EntityGuid Mover, Target, Bullet, Wall, Child;
    };

    Scene RecordScene(ReplayClip& OutClip, const ReplayRecordSettings& InSettings, const int InFrames = 20)
    {
        World Source;
        Scene Guids;

        Entity Mover  = Source.CreateEntity("Mover");
        Entity Target = Source.CreateEntity("Target");
        Entity Wall   = Source.CreateEntity("Wall");
        Entity Child  = Source.CreateEntity("Child");
        Mover.AddComponent<TransformComponent>();
        TransformComponent TargetPose;
        TargetPose.Position = EngineCore::Vector3(0.0f, 5.0f, 0.0f);
        Target.AddComponent(TargetPose);
        Wall.AddComponent<TransformComponent>();
        Wall.SetMobility(EMobility::Static);
        Child.AddComponent<TransformComponent>();
        Child.SetParent(Mover.GetID(), false);

        ReplayTestComponent Init;
        Init.Target  = Target.GetID();
        Init.RunOnly = 77;
        Mover.AddComponent(Init);

        Guids.Mover  = Mover.GetGuid();
        Guids.Target = Target.GetGuid();
        Guids.Wall   = Wall.GetGuid();
        Guids.Child  = Child.GetGuid();

        ReplayRecorder Recorder(OutClip, InSettings);
        EntityID Bullet;
        for(int Frame = 0; Frame < InFrames; ++Frame)
        {
            /* Frame / 10.0, not 0.1 * Frame: division rounds to the same
             * double as the literal, so Seek(0.7) lands on frame 7. */
            const double Time = Frame / 10.0;
            Mover.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(float(10.0 * Time), 0.0f, 0.0f);
            if(ReplayTestComponent* Test = Mover.GetComponent<ReplayTestComponent>()) { Test->Score = Frame / 4; }

            if(Frame == 5)
            {
                Entity B = Source.CreateEntity("Bullet");
                B.AddComponent<TransformComponent>();
                Bullet       = B.GetID();
                Guids.Bullet = B.GetGuid();
            }
            if(Frame == 8)  { Source.DestroyEntity(Bullet); }
            if(Frame == 10) { Child.SetParent(Target.GetID(), false); }
            if(Frame == 15) { Mover.RemoveComponent<ReplayTestComponent>(); }

            Recorder.Capture(Source, Time);
        }
        OutClip.LevelPath = "Levels/ReplayTest.nmworld";
        return Guids;
    }

    ReplayRecordSettings Settings(const float InKeyframeInterval = 0.5f)
    {
        ReplayRecordSettings S;
        S.KeyframeInterval = InKeyframeInterval;
        return S;
    }

    // ========================================================================

    void TestRecording()
    {
        std::printf("Recording\n");
        ReplayClip Clip;
        const Scene Guids = RecordScene(Clip, Settings());

        Expect(Clip.GetFrameCount() == 20, "one frame per capture");
        Expect(Clip.GetFrame(0).bKeyframe, "the first frame is a keyframe");
        Expect(Near(Clip.GetDuration(), 1.9), "duration spans the captures");

        size_t Keyframes = 0;
        for(size_t Index = 0; Index < Clip.GetFrameCount(); ++Index) { Keyframes += Clip.GetFrame(Index).bKeyframe ? 1 : 0; }
        Expect(Keyframes == 4, "a keyframe every 0.5 s: frames 0, 5, 10, 15 (got " + std::to_string(Keyframes) + ")");

        const ReplayFrame& First = Clip.GetFrame(0);
        bool bWall = false;
        for(const ReplayEntityState& E : First.Entities) { bWall |= (E.Guid == Guids.Wall); }
        Expect(!bWall, "a static entity is not recorded");
        Expect(First.Entities.size() == 3, "the keyframe holds Mover, Target and Child");

        /* Frame 1: only Mover moved (Score is still 0). */
        const ReplayFrame& Second = Clip.GetFrame(1);
        Expect(!Second.bKeyframe, "frame 1 is a delta");
        Expect(Second.Entities.size() == 1 && Second.Entities[0].Guid == Guids.Mover, "a delta holds only what changed");
        Expect(Second.Entities.size() == 1 && Second.Entities[0].Components.size() == 1
               && !(Second.Entities[0].Flags & REF_Header), "and only the component that changed");

        /* Frame 8 (a delta): the bullet goes. */
        bool bDestroyed = false;
        for(const EntityGuid& G : Clip.GetFrame(8).Destroyed) { bDestroyed |= (G == Guids.Bullet); }
        Expect(bDestroyed, "a destroyed entity is listed in the delta");

        /* Frame 16 (after the keyframe at 15 removed it): nothing to remove again. */
        bool bRemovedAt16 = false;
        for(const ReplayEntityState& E : Clip.GetFrame(16).Entities) { bRemovedAt16 |= !E.Removed.empty(); }
        Expect(!bRemovedAt16, "a removal is not repeated");

        Expect(Clip.FindFrame(-1.0) == 0, "before the start finds frame 0");
        Expect(Clip.FindFrame(0.55) == 5, "FindFrame takes the last frame at or before");
        Expect(Clip.FindFrame(99.0) == 19, "past the end finds the last frame");
        Expect(Clip.FindKeyframe(7) == 5, "frame 7 rebuilds from keyframe 5");
    }

    void TestPlaybackIntoEmptyWorld()
    {
        std::printf("Playback into an empty world\n");
        ReplayClip Clip;
        const Scene Guids = RecordScene(Clip, Settings());

        World Viewer;
        ReplayPlayer Player(Viewer, Clip);
        Player.SetInterpolation(false);
        Player.Seek(0.0);

        const EntityID Mover  = Player.FindEntity(Guids.Mover);
        const EntityID Target = Player.FindEntity(Guids.Target);
        Expect(Viewer.IsAlive(Mover) && Viewer.IsAlive(Target), "puppets are spawned for the recorded entities");
        Expect(Viewer.GetEntity(Mover).GetGuid() == Guids.Mover, "a puppet keeps the recorded GUID");
        Expect(std::string(Viewer.GetEntity(Mover).GetName()) == "Mover", "and the recorded name");
        Expect(!Viewer.FindEntityByGuid(Guids.Wall).IsSet(), "the static wall was never recorded, so never spawned");
        Expect(Viewer.GetEntityCount() == 3, "Mover, Target, Child");

        ReplayTestComponent* Test = Viewer.GetEntity(Mover).GetComponent<ReplayTestComponent>();
        Expect(Test && Test->Target == Target, "an entity reference resolves to the puppet");
        Expect(Test && Test->RunOnly == 0, "a field the serializer skips is not played");

        Player.Seek(0.7);
        Expect(Near(XOf(Viewer, Mover), 7.0), "seek forward: Mover at x=7");
        Test = Viewer.GetEntity(Mover).GetComponent<ReplayTestComponent>();
        Expect(Test && Test->Score == 1, "Score as recorded at frame 7");
        const EntityID Bullet = Player.FindEntity(Guids.Bullet);
        Expect(Viewer.IsAlive(Bullet), "the bullet exists at 0.7");

        Player.Seek(0.9);
        Expect(!Viewer.IsAlive(Bullet) && !Player.FindEntity(Guids.Bullet).IsSet(), "and is gone at 0.9");

        Player.Seek(0.6);
        Expect(Player.FindEntity(Guids.Bullet).IsSet(), "seeking back brings it back");
        Expect(Near(XOf(Viewer, Mover), 6.0), "seek back: Mover at x=6");

        const EntityID Child = Player.FindEntity(Guids.Child);
        Expect(Viewer.GetEntity(Child).GetParent() == Mover, "Child under Mover before frame 10");
        Player.Seek(1.2);
        Expect(Viewer.GetEntity(Child).GetParent() == Target, "and under Target after");
        Player.Seek(0.2);
        Expect(Viewer.GetEntity(Child).GetParent() == Mover, "a seek back reparents it again");

        Player.Seek(1.8);
        Expect(!Viewer.GetEntity(Mover).HasComponent<ReplayTestComponent>(), "a removed component is removed");
        Player.Seek(1.0);
        Test = Viewer.GetEntity(Mover).GetComponent<ReplayTestComponent>();
        Expect(Test && Test->Score == 2 && Test->Target == Target, "and comes back, with its state, on a seek back");

        Player.Seek(50.0);
        Expect(Near(Player.GetTime(), 1.9) && Near(XOf(Viewer, Mover), 19.0), "a seek past the end clamps to it");

        Player.DestroySpawnedEntities();
        Expect(Viewer.GetEntityCount() == 0, "DestroySpawnedEntities leaves the world as it found it");
        Player.Seek(0.3);
        Expect(Near(XOf(Viewer, Player.FindEntity(Guids.Mover)), 3.0), "and the player can play again after");
    }

    void TestPlaybackIntoLevel()
    {
        std::printf("Playback into the level\n");
        ReplayClip Clip;
        const Scene Guids = RecordScene(Clip, Settings());

        /* The level as loaded again: the same GUIDs, plus something the clip
         * never mentions. */
        World Level;
        const EntityID Mover = Level.CreateEntity("Mover", Guids.Mover).GetID();
        Level.GetEntity(Mover).AddComponent<TransformComponent>();
        ReplayTestComponent Local;
        Local.RunOnly = 5;
        Level.GetEntity(Mover).AddComponent(Local);
        const EntityID Wall  = Level.CreateEntity("Wall", Guids.Wall).GetID();
        const EntityID Other = Level.CreateEntity("Unrelated").GetID();

        ReplayPlayer Player(Level, Clip);
        Player.SetInterpolation(false);
        Player.Seek(0.4);

        Expect(Player.FindEntity(Guids.Mover) == Mover, "an entity the level has is bound, not duplicated");
        Expect(Near(XOf(Level, Mover), 4.0), "and plays");
        const ReplayTestComponent* Test = Level.GetEntity(Mover).GetComponent<ReplayTestComponent>();
        Expect(Test && Test->RunOnly == 5, "its per-run field is left as the level had it");
        Expect(Level.IsAlive(Wall) && Level.IsAlive(Other), "entities the clip never mentions are left alone");
        Expect(Level.GetEntityCount() == 5, "Target and Child were spawned beside them");

        /* The level's own systems move it between seeks. */
        TransformOf(Level, Mover)->Position = EngineCore::Vector3(-50.0f, 0.0f, 0.0f);
        Player.Seek(0.4);
        Expect(Near(XOf(Level, Mover), -50.0), "without hold, a seek to the same frame writes nothing");
        Player.SetHold(true);
        Player.Seek(0.4);
        Expect(Near(XOf(Level, Mover), 4.0), "with hold, every seek writes the whole frame back");
        Player.SetHold(false);

        Player.DestroySpawnedEntities();
        Expect(Level.IsAlive(Mover) && Level.GetEntityCount() == 3, "DestroySpawnedEntities keeps the level's own");
    }

    void TestInterpolation()
    {
        std::printf("Interpolation\n");
        ReplayClip Clip;
        const Scene Guids = RecordScene(Clip, Settings());

        World Viewer;
        ReplayPlayer Player(Viewer, Clip);
        Player.Seek(0.25);
        const EntityID Mover = Player.FindEntity(Guids.Mover);
        Expect(Near(XOf(Viewer, Mover), 2.5), "halfway between frames 2 and 3 blends to x=2.5 (got " + std::to_string(XOf(Viewer, Mover)) + ")");
        Expect(Near(TransformOf(Viewer, Player.FindEntity(Guids.Target))->Position.GetY(), 5.0), "a still entity stays put");

        Player.Seek(0.27);
        Expect(Near(XOf(Viewer, Mover), 2.7), "a second seek in the same span blends again");
        Player.Seek(0.2);
        Expect(Near(XOf(Viewer, Mover), 2.0), "landing on a frame exactly undoes the blend");

        Player.SetInterpolation(false);
        Player.Seek(0.29);
        Expect(Near(XOf(Viewer, Mover), 2.0), "without interpolation the state steps");

        /* Across a reparent the Child is not blended (different spaces). */
        Player.SetInterpolation(true);
        Player.Seek(0.95);
        const TransformComponent* Child = TransformOf(Viewer, Player.FindEntity(Guids.Child));
        Expect(Child && Near(Child->Position.GetX(), 0.0), "no blend across a reparent");
    }

    void TestAdvance()
    {
        std::printf("Advance\n");
        ReplayClip Clip;
        const Scene Guids = RecordScene(Clip, Settings());

        World Viewer;
        ReplayPlayer Player(Viewer, Clip);
        Player.Advance(0.5f);
        Expect(Near(Player.GetTime(), 0.0), "Advance does nothing while paused");

        Player.Play();
        Player.Advance(0.5f);
        Expect(Near(XOf(Viewer, Player.FindEntity(Guids.Mover)), 5.0), "playing advances the time");
        Player.SetRate(2.0f);
        Player.Advance(0.5f);
        Expect(Near(Player.GetTime(), 1.5), "at the rate set");
        Player.Advance(1.0f);
        Expect(Player.IsAtEnd() && !Player.IsPlaying() && Near(Player.GetTime(), 1.9), "stops at the end");

        Player.SetRate(-1.0f);
        Player.Play();
        Player.Advance(0.4f);
        Expect(Near(Player.GetTime(), 1.5) && Near(XOf(Viewer, Player.FindEntity(Guids.Mover)), 15.0), "plays backwards");

        Player.SetRate(1.0f);
        Player.SetLooping(true);
        Player.Seek(1.8);
        Player.Play();
        Player.Advance(0.3f);
        Expect(Player.IsPlaying() && Near(Player.GetTime(), 0.2), "loops back to the start (got " + std::to_string(Player.GetTime()) + ")");
    }

    void TestRolling()
    {
        std::printf("Rolling clips\n");
        ReplayClip Clip;
        ReplayRecordSettings S = Settings(0.5f);
        S.MaxDuration = 1.0f;
        const Scene Guids = RecordScene(Clip, S, 40);   // 3.9 s recorded

        Expect(Clip.GetFrame(0).bKeyframe, "a rolling clip still starts on a keyframe");
        Expect(Clip.GetDuration() >= 1.0 - 1e-6 && Clip.GetDuration() < 1.6, "it keeps about the last second (got " + std::to_string(Clip.GetDuration()) + ")");
        Expect(Near(Clip.GetEndTime(), 3.9), "ending at the newest frame");

        World Viewer;
        ReplayPlayer Player(Viewer, Clip);
        Player.SetInterpolation(false);
        Player.Seek(0.0);
        Expect(Near(XOf(Viewer, Player.FindEntity(Guids.Mover)), 10.0 * Clip.GetStartTime()), "and plays from where it starts");
    }

    void TestRecorderTicked()
    {
        std::printf("Recorder driven by the tick\n");
        World Source;
        Entity Mover = Source.CreateEntity("Mover");
        Mover.AddComponent<TransformComponent>();
        const EntityGuid Guid = Mover.GetGuid();

        struct Move { static void Run(World& InWorld, float InDelta, void* InUserData)
        {
            const EntityID Id = *static_cast<EntityID*>(InUserData);
            TransformComponent* T = InWorld.GetEntity(Id).GetComponent<TransformComponent>();
            T->Position = EngineCore::Vector3(float(T->Position.GetX()) + InDelta, 0.0f, 0.0f);
        } };
        EntityID MoverId = Mover.GetID();
        Source.RegisterSystem("Move", TP_PrePhysics, &Move::Run, &MoverId);

        ReplayClip Clip;
        ReplayRecordSettings S;
        S.SampleRate = 10.0f;
        ReplayRecorder Recorder(Clip, S);
        Recorder.Start(Source);
        Expect(Recorder.IsRecording() && Clip.GetFrameCount() == 1, "Start captures the first frame at once");

        for(int Tick = 0; Tick < 60; ++Tick) { Source.Tick(1.0f / 60.0f); }
        Expect(Clip.GetFrameCount() >= 10 && Clip.GetFrameCount() <= 12, "about 10 samples a second (got " + std::to_string(Clip.GetFrameCount()) + ")");
        Expect(Near(Clip.GetEndTime(), 1.0, 0.11), "stamped with the world's tick time");

        Recorder.Stop();
        const size_t Frames = Clip.GetFrameCount();
        for(int Tick = 0; Tick < 30; ++Tick) { Source.Tick(1.0f / 60.0f); }
        Expect(!Recorder.IsRecording() && Clip.GetFrameCount() == Frames, "Stop takes the system off the world");

        World Viewer;
        ReplayPlayer Player(Viewer, Clip);
        Player.Seek(Clip.GetEndTime());
        Expect(Near(XOf(Viewer, Player.FindEntity(Guid)), Clip.GetEndTime(), 0.02), "the ticked recording plays back");
        Source.UnregisterSystem(&Move::Run, &MoverId);
    }

    void TestFilters()
    {
        std::printf("Filters\n");
        World Source;
        Entity Keep = Source.CreateEntity("Keep");
        Entity Skip = Source.CreateEntity("Skip");
        Keep.AddComponent<TransformComponent>();
        Keep.AddComponent<ReplayTestComponent>();
        Skip.AddComponent<TransformComponent>();

        ReplayClip Clip;
        ReplayRecorder Recorder(Clip);
        Recorder.SetEntityFilter([](World& InWorld, const EntityID InEntity, void*)
        {
            return std::string(InWorld.GetEntity(InEntity).GetName()) != "Skip";
        }, nullptr);
        Recorder.ExcludeComponentType(ReplayTestComponent::StaticType().ID);
        Recorder.Capture(Source, 0.0);

        const ReplayFrame& Frame = Clip.GetFrame(0);
        Expect(Frame.Entities.size() == 1 && Frame.Entities[0].Guid == Keep.GetGuid(), "the entity filter decides who is recorded");
        Expect(Frame.Entities.size() == 1 && Frame.Entities[0].Components.size() == 1, "an excluded type is not recorded");
    }

    void TestFileRoundTrip()
    {
        std::printf("Clip file\n");
        ReplayClip Clip;
        const Scene Guids = RecordScene(Clip, Settings());

        EngineCore::Archive Out = EngineCore::Archive::ForWriting();
        Expect(Clip.Serialize(Out), "a clip saves");

        ReplayClip Loaded;
        EngineCore::Archive In = EngineCore::Archive::ForReading(Out.GetBytes());
        Expect(Loaded.Serialize(In), "and loads");
        Expect(Loaded.GetFrameCount() == Clip.GetFrameCount() && Loaded.GetTypeCount() == Clip.GetTypeCount(), "with every frame and type");
        Expect(Loaded.LevelPath == Clip.LevelPath, "and the level path");
        Expect(Loaded.GetPayloadBytes() == Clip.GetPayloadBytes(), "byte for byte");

        World Viewer;
        ReplayPlayer Player(Viewer, Loaded);
        Player.SetInterpolation(false);
        Player.Seek(0.6);
        Expect(Near(XOf(Viewer, Player.FindEntity(Guids.Mover)), 6.0) && Player.FindEntity(Guids.Bullet).IsSet(), "a loaded clip plays the same");

        /* Truncated and foreign data fail cleanly. */
        std::vector<uint8_t> Cut(Out.GetBytes().begin(), Out.GetBytes().begin() + std::ptrdiff_t(Out.GetBytes().size() / 2));
        ReplayClip Broken;
        EngineCore::Archive CutIn = EngineCore::Archive::ForReading(Cut);
        Expect(!Broken.Serialize(CutIn) && Broken.IsEmpty(), "a truncated clip fails and leaves the clip empty");

        ReplayClip Foreign;
        EngineCore::Archive Junk = EngineCore::Archive::ForReading(std::vector<uint8_t>(64, 0xAB));
        Expect(!Foreign.Serialize(Junk), "data that is not a clip fails");

        Expect(Clip.GetPayloadBytes() < 20 * 3 * 200, "deltas keep a clip small (" + std::to_string(Clip.GetPayloadBytes()) + " bytes)");
    }

    /* Docs/Guides/ReplayGuide.md's Lua example, word for word: an instant
     * replay on a goal, through the Replay script library. */
    const char* const INSTANT_REPLAY_LUA = R"lua(
-- InstantReplay.lua: keeps the last 10 seconds. On "Match.Goal" it saves
-- them and plays them back at half speed over the live level, then goes
-- back to recording when the replay ends.
return {
    OnStart = function(self)
        Replay.RecordOnly("Transform,Render,Animation")   -- what is seen, not gameplay state
        Replay.StartRecording(10, 30)                     -- the last 10 s, 30 samples a second
        self.OnGoal = Events.On("Match.Goal", function() self:ShowReplay() end)
        self.OnDone = Events.On(Replay.ReplayFinished, function() self:BackToPlay() end)
    end,

    OnDestroy = function(self)
        Events.Off(self.OnGoal)
        Events.Off(self.OnDone)
    end,

    ShowReplay = function(self)
        Replay.StopRecording()
        Replay.Save("Saved/Replays/LastGoal.nmreplay")
        Replay.Play(true)       -- hold: the clip wins over the live level every frame
        Replay.SetRate(0.5)
    end,

    BackToPlay = function(self)
        Replay.Stop(true)       -- and remove anything playback spawned
        Replay.SetRate(1)
        Replay.StartRecording(10, 30)
    end,
}
)lua";

    GameCore::Entity AttachScript(World& InWorld, const GameCore::Entity& InEntity, const char* InScript)
    {
        PrefabAsset Asset;
        CapturePrefab(InWorld, InEntity.GetID(), Asset);
        Asset.SetScriptPath(InScript);
        const TransformPose At = InEntity.GetWorldPose();
        const std::string Name = InEntity.GetName();
        InWorld.DestroyEntity(InEntity.GetID());
        return InWorld.SpawnPrefab(Asset, At, Name.c_str()).GetRoot();
    }



    /* World::UnregisterSystem from inside a bucket walk: the bucket must not
     * shift under the walk (it used to: the next system was skipped and the
     * walk read past the end). */
    struct WalkLog
    {
        std::vector<int> Ran;
        World*           W = nullptr;
    };
    void SysA(World& InWorld, float, void* InUserData);
    void SysB(World& InWorld, float, void* InUserData);
    void SysC(World& InWorld, float, void* InUserData);
    void SysA(World& InWorld, float, void* InUserData)
    {
        WalkLog& L = *static_cast<WalkLog*>(InUserData);
        L.Ran.push_back(1);
        InWorld.UnregisterSystem(&SysA, InUserData);   // itself
        InWorld.UnregisterSystem(&SysC, InUserData);   // a later one in the same bucket
    }
    void SysB(World&, float, void* InUserData) { static_cast<WalkLog*>(InUserData)->Ran.push_back(2); }
    void SysC(World&, float, void* InUserData) { static_cast<WalkLog*>(InUserData)->Ran.push_back(3); }

    struct StopFromPostUpdate
    {
        std::unique_ptr<ReplayRecorder> Recorder;
        int Ticks = 0;
        static void Run(World&, float, void* InUserData)
        {
            StopFromPostUpdate& Self = *static_cast<StopFromPostUpdate*>(InUserData);
            if(++Self.Ticks == 3) { Self.Recorder->Stop(); }
        }
    };

    void TestUnregisterDuringTick()
    {
        std::printf("Unregistering systems during a tick\n");
        {
            World W;
            WalkLog Log;
            W.RegisterSystem("A", TP_PrePhysics, &SysA, &Log);
            W.RegisterSystem("B", TP_PrePhysics, &SysB, &Log);
            W.RegisterSystem("C", TP_PrePhysics, &SysC, &Log);
            W.Tick(0.1f);
            Expect(Log.Ran == std::vector<int>({ 1, 2 }), "a system removing itself does not skip the next; one it removed does not run");
            Log.Ran.clear();
            W.Tick(0.1f);
            Expect(Log.Ran == std::vector<int>({ 2 }), "next tick only B is left");
            Expect(W.UnregisterAllSystems(&Log) == 1, "and the removed ones are really gone");
        }
        {
            /* The case that found it: a recorder stopped by a TP_PostUpdate system
             * registered before it (so in the same bucket, walked first). */
            World W;
            Entity Ball = W.CreateEntity("Ball");
            Ball.AddComponent<TransformComponent>();
            ReplayClip Clip;
            StopFromPostUpdate Stopper;
            W.RegisterSystem("Stopper", TP_PostUpdate, &StopFromPostUpdate::Run, &Stopper);
            ReplayRecordSettings Settings;
            Settings.SampleRate = 0.0f;   // every tick
            Stopper.Recorder = std::make_unique<ReplayRecorder>(Clip, Settings);
            Stopper.Recorder->Start(W);
            for(int Tick = 0; Tick < 6; ++Tick) { W.Tick(0.1f); }
            Expect(!Stopper.Recorder->IsRecording() && Clip.GetFrameCount() == 3,
                   "a recorder stopped mid-walk takes no more frames (got " + std::to_string(Clip.GetFrameCount()) + ")");
            W.UnregisterSystem(&StopFromPostUpdate::Run, &Stopper);
        }
    }

    void TestGuideKillCam()
    {
        std::printf("Guide: KillCam (C++)\n");
        World W;
        Entity Ball = W.CreateEntity("Ball");
        Ball.AddComponent<TransformComponent>();
        struct Roll { static void Run(World& InWorld, float InDelta, void* InUserData)
        {
            TransformComponent* T = InWorld.GetEntity(*static_cast<EntityID*>(InUserData)).GetComponent<TransformComponent>();
            if(T) { T->Position = EngineCore::Vector3(float(T->Position.GetX()) + 10.0f * InDelta, 0.0f, 0.0f); }
        } };
        EntityID BallId = Ball.GetID();
        W.RegisterSystem("Roll", TP_PrePhysics, &Roll::Run, &BallId);

        KillCam Cam;
        Cam.Begin(W);
        const float Dt = 1.0f / 30.0f;
        for(int Frame = 0; Frame < 240; ++Frame) { W.Tick(Dt); }   // 8 s: the buffer keeps about the last 5
        Expect(Near(XOf(W, BallId), 80.0, 0.5), "live: x=80 after 8 s");

        Cam.Show();
        W.Tick(Dt);
        Expect(XOf(W, BallId) > 15.0 && XOf(W, BallId) < 35.0, "the kill cam starts about 5 s back (x=" + std::to_string(XOf(W, BallId)) + ")");
        int Frames = 0;
        while(!Cam.IsDone() && Frames < 1000) { W.Tick(Dt); ++Frames; }
        Expect(Cam.IsDone() && Frames > 250, "it plays the buffer at half speed (" + std::to_string(Frames) + " frames)");
        Expect(Near(XOf(W, BallId), 80.0, 0.5), "and ends where the live game was");

        Cam.Finish();
        for(int Frame = 0; Frame < 30; ++Frame) { W.Tick(Dt); }
        Expect(XOf(W, BallId) > 85.0, "after Finish the live game moves the ball again");
        W.UnregisterSystem(&Roll::Run, &BallId);
    }

    void TestScriptLibrary()
    {
        std::printf("Replay script library (Lua)\n");
        const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleReplayTests";
        std::filesystem::create_directories(Dir);
        const std::string ScriptPath = (Dir / "InstantReplay.lua").generic_string();
        std::ofstream(ScriptPath, std::ios::binary | std::ios::trunc) << INSTANT_REPLAY_LUA;
        std::error_code Ignored;
        std::filesystem::remove("Saved/Replays/LastGoal.nmreplay", Ignored);

        {
            World W;
            ScriptEngine Scripts;
            Scripts.Initialize();
            Scripts.BindWorld(&W);

            /* Live gameplay: the ball rolls 10 units a second, always. */
            Entity Ball = W.CreateEntity("Ball");
            Ball.AddComponent<TransformComponent>();
            struct Roll { static void Run(World& InWorld, float InDelta, void* InUserData)
            {
                TransformComponent* T = InWorld.GetEntity(*static_cast<EntityID*>(InUserData)).GetComponent<TransformComponent>();
                if(T) { T->Position = EngineCore::Vector3(float(T->Position.GetX()) + 10.0f * InDelta, 0.0f, 0.0f); }
            } };
            EntityID BallId = Ball.GetID();
            W.RegisterSystem("Roll", TP_PrePhysics, &Roll::Run, &BallId);

            Entity Director = W.CreateEntity("Director");
            Director.AddComponent<TransformComponent>();
            AttachScript(W, Director, ScriptPath.c_str());

            const float Dt = 1.0f / 30.0f;
            for(int Frame = 0; Frame < 90; ++Frame) { W.Tick(Dt); }
            Expect(Scripts.RunString("assert(Replay.IsRecording())", "recording"), "OnStart started recording");
            Expect(Scripts.RunString("assert(math.abs(Replay.GetDuration() - 3) < 0.1)", "duration"), "three seconds recorded");
            Expect(Near(XOf(W, BallId), 30.0, 0.5), "the ball rolled to x=30 live");

            W.GetEvents().Emit("Match.Goal");
            Expect(std::filesystem::exists("Saved/Replays/LastGoal.nmreplay"), "the goal saved the clip");
            Expect(Scripts.RunString("assert(Replay.IsPlaying() and not Replay.IsRecording())", "playing"), "and plays it");

            W.Tick(Dt);
            Expect(XOf(W, BallId) < 2.0, "hold: the replay wins over the live roll (x=" + std::to_string(XOf(W, BallId)) + ")");
            for(int Frame = 0; Frame < 45; ++Frame) { W.Tick(Dt); }   // 1.5 s at half speed: 0.75 s in
            Expect(Near(XOf(W, BallId), 7.5, 0.6), "half speed: x=7.5 after 1.5 s (x=" + std::to_string(XOf(W, BallId)) + ")");

            for(int Frame = 0; Frame < 150; ++Frame) { W.Tick(Dt); }  // past the end
            Expect(Scripts.RunString("assert(Replay.IsRecording() and not Replay.IsPlaying())", "back"),
                   "ReplayFinished sent it back to recording");
            Expect(Scripts.GetErrorCount() == 0, "the script raised no Lua errors");

            ReplayClip Saved;
            Expect(LoadReplayClip("Saved/Replays/LastGoal.nmreplay", Saved) && Near(Saved.GetDuration(), 3.0, 0.1),
                   "the saved clip loads from C++");

            Expect(!Scripts.RunString("Replay.Stop() Replay.StopRecording() Replay.Load('nope.nmreplay') Replay.Play()", "empty"),
                   "Play with nothing loaded is a Lua error, not a crash");

            W.UnregisterSystem(&Roll::Run, &BallId);
            Scripts.UnbindWorld();
            Scripts.Shutdown();
        }
        Expect(true, "the World went with its replay session (no crash)");
        std::filesystem::remove("Saved/Replays/LastGoal.nmreplay", Ignored);
    }
}

int main()
{
    std::printf("Replay tests\n");

    TestRecording();
    TestPlaybackIntoEmptyWorld();
    TestPlaybackIntoLevel();
    TestInterpolation();
    TestAdvance();
    TestRolling();
    TestRecorderTicked();
    TestFilters();
    TestFileRoundTrip();
    TestScriptLibrary();
    TestGuideKillCam();
    TestUnregisterDuringTick();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

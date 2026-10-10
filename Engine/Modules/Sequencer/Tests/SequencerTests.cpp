/*
 * SequencerTests -- the Sequencer module without a renderer or a game: curve
 * sampling, the file formats both ways, and sequences played on a bare
 * GameCore World ticked by hand.
 */

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Public/Sequencer/SequenceAsset.h"
#include "Public/Sequencer/SequenceFile.h"
#include "Public/Sequencer/SequencePlayerComponent.h"
#include "Public/Sequencer/SequenceWorld.h"

using namespace Sequencer;

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

namespace
{
    bool Near(const double InA, const double InB, const double InTolerance = 1e-4)
    {
        return std::fabs(InA - InB) <= InTolerance;
    }

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.5f", InValue);
        return Buffer;
    }

    SequenceKey Key(const int32_t InFrame, const float InValue, const ESequenceInterp InInterp = ESequenceInterp::Linear)
    {
        SequenceKey Made;
        Made.Frame  = InFrame;
        Made.Value  = InValue;
        Made.Interp = InInterp;
        return Made;
    }

    SequenceEventKey EventKey(const int32_t InFrame, const char* InName, const bool bInFireOnSkip = false)
    {
        SequenceEventKey Made;
        Made.Frame       = InFrame;
        Made.Event       = InName;
        Made.bFireOnSkip = bInFireOnSkip;
        return Made;
    }

    /* Names of events heard, in order. */
    struct Heard
    {
        std::vector<std::string> Names;
        std::vector<GameCore::EventArgs> Args;
    };

    void Listen(GameCore::World& InWorld, Heard& InOut, const char* InName)
    {
        InWorld.GetEvents().Subscribe(InName, [](const GameCore::EventID InEvent, const GameCore::EventArgs& InArgs, void* InUserData)
        {
            (void)InEvent;
            Heard& H = *static_cast<Heard*>(InUserData);
            H.Args.push_back(InArgs);
            H.Names.emplace_back("?");
        }, &InOut);
    }

    /* A world with one "Mover" (Transform at 1,2,3) and the asset under
     * InPath: Mover's Position.X 0 -> 10 over frames 0..10 (linear), a
     * "Mid" event at 5, an "End" event at 10 (fire on skip). 10 fps. */
    std::shared_ptr<SequenceAsset> MoverAsset()
    {
        auto Asset = std::make_shared<SequenceAsset>();
        Asset->FrameRate = 10;
        Asset->Length    = 10;

        SequenceBinding Mover;
        Mover.Id   = "mover";
        Mover.Name = "Mover";
        SequenceTrack Transform;
        Transform.Type = ESequenceTrackType::Transform;
        Transform.Normalize();
        Transform.Channels[TC_PositionX].Keys = { Key(0, 0.0f), Key(10, 10.0f) };
        Mover.Tracks.push_back(Transform);
        Asset->Bindings.push_back(Mover);

        SequenceTrack Events;
        Events.Type = ESequenceTrackType::Event;
        Events.Events.push_back(EventKey(5, "Mid"));
        Events.Events.push_back(EventKey(10, "End", true));
        SequenceEventArg Who;
        Who.Type = SequenceEventArg::EType::Binding;
        Who.Text = "mover";
        Events.Events[0].Args.push_back(Who);
        Asset->Tracks.push_back(Events);
        Asset->Normalize();
        return Asset;
    }

    GameCore::EntityID MakeMover(GameCore::World& InWorld)
    {
        GameCore::Entity Mover = InWorld.CreateEntity("Mover");
        GameCore::TransformComponent Init;
        Init.Position = EngineCore::Vector3(1.0, 2.0, 3.0);
        Mover.AddComponent(Init);
        return Mover.GetID();
    }

    float MoverX(GameCore::World& InWorld, const GameCore::EntityID InMover)
    {
        return float(InWorld.GetEntity(InMover).GetComponent<GameCore::TransformComponent>()->Position.GetX());
    }

    GameCore::EntityID MakePlayer(GameCore::World& InWorld, const char* InPath, const bool bInAutoPlay, const bool bInRestore = false)
    {
        GameCore::Entity Player = InWorld.CreateEntity("Player");
        SequencePlayerComponent Init;
        std::snprintf(Init.Sequence, sizeof(Init.Sequence), "%s", InPath);
        Init.bAutoPlay     = bInAutoPlay ? 1 : 0;
        Init.bRestoreState = bInRestore ? 1 : 0;
        Player.AddComponent(Init);
        return Player.GetID();
    }
}

// === Curves ==================================================================

static void TestChannels()
{
    std::printf("Channels\n");

    SequenceChannel Empty;
    Expect(Near(Empty.Evaluate(3.0, 7.0f), 7.0), "an empty channel gives the default");

    SequenceChannel Linear;
    Linear.Keys = { Key(10, 0.0f), Key(20, 100.0f) };
    Expect(Near(Linear.Evaluate(0.0), 0.0),    "held before the first key");
    Expect(Near(Linear.Evaluate(15.0), 50.0),  "linear midway: " + Num(Linear.Evaluate(15.0)));
    Expect(Near(Linear.Evaluate(17.5), 75.0),  "linear at a fractional frame");
    Expect(Near(Linear.Evaluate(99.0), 100.0), "held after the last key");

    SequenceChannel Step;
    Step.Keys = { Key(0, 1.0f, ESequenceInterp::Constant), Key(10, 5.0f) };
    Expect(Near(Step.Evaluate(9.99), 1.0), "constant holds until the next key");
    Expect(Near(Step.Evaluate(10.0), 5.0), "constant takes the next key on its frame");

    SequenceChannel Eased;
    Eased.Keys = { Key(0, 0.0f, ESequenceInterp::CubicInOut), Key(10, 1.0f) };
    Expect(Near(Eased.Evaluate(5.0), 0.5), "cubic in-out is half way at half time");
    Expect(Eased.Evaluate(2.0) < 0.2f, "cubic in-out starts slow: " + Num(Eased.Evaluate(2.0)));

    /* Auto through a peak: flat at it, no overshoot past the keys. */
    SequenceChannel Smooth;
    Smooth.Keys = { Key(0, 0.0f, ESequenceInterp::Auto), Key(10, 10.0f, ESequenceInterp::Auto), Key(20, 0.0f, ESequenceInterp::Auto) };
    float Highest = -1.0f;
    for(int Frame = 0; Frame <= 200; ++Frame) { Highest = std::max(Highest, Smooth.Evaluate(Frame * 0.1)); }
    Expect(Near(Highest, 10.0, 1e-3), "auto never overshoots a peak: " + Num(Highest));
    Expect(Near(Smooth.Evaluate(10.0), 10.0), "auto passes through its keys");
    /* Through a rising middle key: eased out of the first, carried through the middle. */
    SequenceChannel Ramp;
    Ramp.Keys = { Key(0, 0.0f, ESequenceInterp::Auto), Key(10, 10.0f, ESequenceInterp::Auto), Key(20, 20.0f, ESequenceInterp::Auto) };
    Expect(Ramp.Evaluate(5.0) < 5.0f, "auto eases out of the first key: " + Num(Ramp.Evaluate(5.0)));
    const double SlopeIn  = (Ramp.Evaluate(10.0) - Ramp.Evaluate(9.9)) / 0.1;
    const double SlopeOut = (Ramp.Evaluate(10.1) - Ramp.Evaluate(10.0)) / 0.1;
    Expect(Near(SlopeIn, 1.0, 0.05) && Near(SlopeOut, 1.0, 0.05), "auto is smooth through a middle key: " + Num(SlopeIn) + " / " + Num(SlopeOut));

    /* Normalize: sorted, a frame's later key wins. */
    SequenceChannel Messy;
    Messy.Keys = { Key(20, 2.0f), Key(10, 1.0f), Key(20, 3.0f) };
    Messy.Normalize();
    Expect(Messy.Keys.size() == 2 && Messy.Keys[0].Frame == 10 && Near(Messy.Keys[1].Value, 3.0),
           "normalize sorts and keeps the later of two keys on one frame");

    Expect(Near(Ease(ESequenceInterp::BackOut, 1.0f), 1.0) && Ease(ESequenceInterp::BackOut, 0.6f) > 1.0f,
           "back-out overshoots and settles");

    ESequenceInterp Parsed = ESequenceInterp::Linear;
    Expect(ParseInterp("ElasticOut", Parsed) && Parsed == ESequenceInterp::ElasticOut, "interp names parse");
    Expect(!ParseInterp("Bouncy", Parsed), "an unknown interp name is refused");
}

// === Files ===================================================================

#if WITH_UNCOOKED
static const char* const FIXTURE = R"(<?xml version="1.0"?>
<Sequence version="1" fps="24" length="240">
  <Binding id="hale" kind="possess" guid="7f3a00000000000000000000000c2100" name="Inspector_Hale">
    <Track type="Transform">
      <Channel name="Position.X">
        <Key f="24" v="-4" interp="CubicInOut"/>
        <Key f="96" v="2.5"/>
      </Channel>
      <Channel name="Rotation.Y">
        <Key f="0" v="90" interp="Constant"/>
      </Channel>
    </Track>
    <Track type="Float" component="Sequence Player" field="PlayRate">
      <Key f="0" v="1"/>
    </Track>
    <Track type="Bool" component="Sequence Player" field="Loop">
      <Key f="0" v="true"/>
      <Key f="120" v="0"/>
    </Track>
  </Binding>
  <Binding id="guard" kind="spawn" prefab="Prefabs/Guard.nmprefab"/>
  <Track type="Event">
    <Key f="228" event="Combat.Start" arg0="cellar" arg1="@hale" arg2="3.5" arg3="true" fireOnSkip="true"/>
  </Track>
</Sequence>
)";

static void TestXml()
{
    std::printf("XML\n");

    SequenceAsset Asset;
    std::string Error;
    const bool bParsed = ParseSequenceXml(FIXTURE, Asset, &Error);
    Expect(bParsed, "the fixture parses: " + Error);
    if(!bParsed) { return; }

    Expect(Asset.FrameRate == 24 && Asset.Length == 240, "rate and length");
    Expect(Asset.Bindings.size() == 2, "two bindings");
    const SequenceBinding& Hale = Asset.Bindings[0];
    Expect(Hale.Id == "hale" && Hale.Name == "Inspector_Hale" && Hale.Kind == ESequenceBindingKind::Possess, "a possess binding");
    Expect(Hale.Guid.IsSet() && Hale.Guid.Hi == 0x7f3a000000000000ull, "its guid");
    Expect(Hale.Tracks.size() == 3, "three tracks on it");
    Expect(Hale.Tracks[0].Channels.size() == TC_Count, "a transform track has every channel");
    Expect(Hale.Tracks[0].Channels[TC_PositionX].Keys.size() == 2
           && Hale.Tracks[0].Channels[TC_PositionX].Keys[0].Interp == ESequenceInterp::CubicInOut
           && Hale.Tracks[0].Channels[TC_PositionX].Keys[1].Interp == ESequenceInterp::Auto, "keys and interps");
    Expect(Hale.Tracks[2].Type == ESequenceTrackType::Bool && Near(Hale.Tracks[2].Channels[0].Keys[0].Value, 1.0)
           && Near(Hale.Tracks[2].Channels[0].Keys[1].Value, 0.0), "bool keys read true/0");
    Expect(Asset.Bindings[1].Kind == ESequenceBindingKind::Spawn && Asset.Bindings[1].Prefab == "Prefabs/Guard.nmprefab", "a spawn binding");

    Expect(Asset.Tracks.size() == 1 && Asset.Tracks[0].Events.size() == 1, "one master event");
    const SequenceEventKey& Combat = Asset.Tracks[0].Events[0];
    Expect(Combat.bFireOnSkip && Combat.Args.size() == 4, "its flags and args");
    Expect(Combat.Args[0].Type == SequenceEventArg::EType::String && Combat.Args[1].Type == SequenceEventArg::EType::Binding
           && Combat.Args[2].Type == SequenceEventArg::EType::Number && Combat.Args[3].Type == SequenceEventArg::EType::Bool, "argument types inferred");
    Expect(Asset.Validate().empty(), "the fixture validates");

    /* Written and read back: the same, and writing again the same bytes. */
    const std::string Written = WriteSequenceXml(Asset);
    SequenceAsset Again;
    Expect(ParseSequenceXml(Written, Again, &Error), "written XML parses: " + Error);
    Expect(WriteSequenceXml(Again) == Written, "writing is deterministic");
    Expect(Again.Tracks[0].Events[0].Args[1].ToText() == "@hale", "a binding argument round-trips");

    SequenceAsset Bad;
    Expect(!ParseSequenceXml("<Sequence><Track type=\"Wobble\"/></Sequence>", Bad, &Error), "an unknown track type is refused");
    Expect(!ParseSequenceXml("<Sequence><Binding id=\"a\"><Track type=\"Transform\"><Channel name=\"Size.X\"/></Track></Binding></Sequence>", Bad, &Error),
           "an unknown channel is refused");
    Expect(!ParseSequenceXml("<Nope/>", Bad, &Error), "a document without <Sequence> is refused");

    /* Through the binary form. */
    const std::vector<uint8_t> Cooked = WriteSequenceBinary(Asset);
    Expect(DetectSequenceFileFormat(Cooked) == ESequenceFileFormat::Binary, "the cooked form is detected");
    SequenceAsset Read;
    Expect(ReadSequence(Cooked, Read, &Error), "the cooked form reads: " + Error);
    Expect(WriteSequenceXml(Read) == Written, "cooked and read back, it is the same asset");
    Expect(WriteSequenceBinary(Read) == Cooked, "cooking is deterministic");

    const std::vector<uint8_t> Text(Written.begin(), Written.end());
    Expect(DetectSequenceFileFormat(Text) == ESequenceFileFormat::Text, "XML is detected");

    std::vector<uint8_t> Damaged(Cooked.begin(), Cooked.begin() + long(Cooked.size() / 2));
    Expect(!ReadSequence(Damaged, Read, &Error), "a truncated cook is refused");
}
#endif

static void TestValidate()
{
    std::printf("Validate\n");
    SequenceAsset Asset;
    Asset.Length = 10;
    SequenceBinding A;
    A.Id = "a";
    A.Name = "A";
    Asset.Bindings.push_back(A);
    Asset.Bindings.push_back(A);   // twice
    SequenceTrack Float;
    Float.Type = ESequenceTrackType::Float;
    Asset.Tracks.push_back(Float);  // master, not an event
    SequenceTrack Events;
    Events.Type = ESequenceTrackType::Event;
    Events.Events.push_back(EventKey(20, "Late"));
    SequenceEventArg Missing;
    Missing.Type = SequenceEventArg::EType::Binding;
    Missing.Text = "ghost";
    Events.Events[0].Args.push_back(Missing);
    Asset.Tracks.push_back(Events);
    Asset.Normalize();

    const std::vector<std::string> Problems = Asset.Validate();
    const auto Mentions = [&](const char* InText)
    {
        for(const std::string& P : Problems) { if(P.find(InText) != std::string::npos) return true; }
        return false;
    };
    Expect(Mentions("declared twice"), "a repeated binding id");
    Expect(Mentions("no binding to animate"), "a master track that is not events");
    Expect(Mentions("past the end"), "a key past the end");
    Expect(Mentions("@ghost"), "an argument naming no binding");
}

// === Evaluating ==============================================================

static void TestInstance()
{
    std::printf("Instance\n");
    GameCore::World World;
    const GameCore::EntityID Mover = MakeMover(World);

    SequenceInstance Instance(World, MoverAsset(), "Test/Mover.nmseq");
    Expect(Instance.ResolveBinding(0) == Mover, "a binding resolves by name");

    Instance.Evaluate(2.5);
    Expect(Near(MoverX(World, Mover), 2.5), "position at frame 2.5: " + Num(MoverX(World, Mover)));
    Expect(Near(World.GetEntity(Mover).GetComponent<GameCore::TransformComponent>()->Position.GetY(), 2.0),
           "an unkeyed channel is left alone");
    Expect(Instance.GetSavedCount() == 1, "the transform was saved once");
    Instance.Evaluate(8.0);
    Expect(Instance.GetSavedCount() == 1, "and not again");

    Instance.RestoreState();
    Expect(Near(MoverX(World, Mover), 1.0), "restored to where it was: " + Num(MoverX(World, Mover)));

    /* Events over ranges. */
    Heard Mid, End;
    Listen(World, Mid, "Mid");
    Listen(World, End, "End");
    Instance.FireEvents(0.0, 4.9, true);
    Expect(Mid.Args.empty(), "nothing before its frame");
    Instance.FireEvents(4.9, 5.0, false);
    Expect(Mid.Args.size() == 1, "a key fires when the range reaches it");
    Expect(Mid.Args.size() == 1 && Mid.Args[0].Count == 1 && Mid.Args[0].Args[0].Type == GameCore::EventArg::ET_Entity
           && GameCore::EntityID::Unpack(Mid.Args[0].Args[0].Entity) == Mover, "a binding argument is its entity");
    Instance.FireEvents(5.0, 6.0, false);
    Expect(Mid.Args.size() == 1, "a range starting on a key excludes it");
    Instance.FireEvents(5.0, 6.0, true);
    Expect(Mid.Args.size() == 2, "unless the start is included");
    Instance.FireEvents(0.0, 10.0, true, true);
    Expect(Mid.Args.size() == 2 && End.Args.size() == 1, "skipping fires only fire-on-skip keys");

    /* An override wins over the name. */
    const GameCore::EntityID Other = MakeMover(World);
    Expect(Instance.SetBindingOverride("mover", Other), "an override is taken");
    Expect(!Instance.SetBindingOverride("nobody", Other), "an override for no binding is refused");
    Instance.Evaluate(10.0);
    Expect(Near(MoverX(World, Other), 10.0) && Near(MoverX(World, Mover), 1.0), "the override moves, the original does not");
}

static void TestFields()
{
    std::printf("Fields\n");
    GameCore::World World;
    /* A Sequence Player's own fields are a convenient float and flag. */
    const GameCore::EntityID Target = MakePlayer(World, "", false);
    World.GetEntity(Target).SetName("Target");

    auto Asset = std::make_shared<SequenceAsset>();
    Asset->Length = 10;
    SequenceBinding Binding;
    Binding.Id   = "t";
    Binding.Name = "Target";
    SequenceTrack Rate;
    Rate.Type      = ESequenceTrackType::Float;
    Rate.Component = "Sequence Player";
    Rate.Field     = "PlayRate";
    Rate.Normalize();
    Rate.Channels[0].Keys = { Key(0, 2.0f), Key(10, 4.0f) };
    SequenceTrack Loop;
    Loop.Type      = ESequenceTrackType::Bool;
    Loop.Component = "Sequence Player";
    Loop.Field     = "Loop";
    Loop.Normalize();
    Loop.Channels[0].Keys = { Key(0, 0.0f), Key(5, 1.0f) };
    SequenceTrack Bogus;
    Bogus.Type      = ESequenceTrackType::Float;
    Bogus.Component = "Sequence Player";
    Bogus.Field     = "NoSuchField";
    Bogus.Normalize();
    Bogus.Channels[0].Keys = { Key(0, 1.0f) };
    Binding.Tracks = { Rate, Loop, Bogus };
    Asset->Bindings.push_back(Binding);
    Asset->Normalize();

    SequenceInstance Instance(World, Asset);
    const auto Component = [&]() { return World.GetEntity(Target).GetComponent<SequencePlayerComponent>(); };

    Instance.Evaluate(5.0);
    Expect(Near(Component()->PlayRate, 3.0), "a float field is written: " + Num(Component()->PlayRate));
    Expect(Component()->bLoop == 1, "a flag is written on its key");
    Instance.Evaluate(4.0);
    Expect(Component()->bLoop == 0, "and stepped, not blended");

    Instance.RestoreState();
    Expect(Near(Component()->PlayRate, 1.0) && Component()->bLoop == 0, "both restored");
}

// === Players =================================================================

static void TestPlayers()
{
    std::printf("Players\n");
    SequenceAssetCache::Set("Test/Mover.nmseq", MoverAsset());

    GameCore::World World;
    SequenceWorld* Players = SequenceWorld::Attach(World);
    Expect(Players && SequenceWorld::Find(&World) == Players, "attached and found");

    const GameCore::EntityID Mover  = MakeMover(World);
    const GameCore::EntityID Player = MakePlayer(World, "Test/Mover.nmseq", true, true);

    Heard Started, Finished, Mid;
    Listen(World, Started, "SequenceStarted");
    Listen(World, Finished, "SequenceFinished");
    Listen(World, Mid, "Mid");

    /* AutoPlay: the first tick shows frame 0, no time passes. */
    World.Tick(0.25f);
    Expect(Players->IsPlaying(Player) && Started.Args.size() == 1, "auto-played on the first tick");
    Expect(Near(Players->GetTime(Player), 0.0) && Near(MoverX(World, Mover), 0.0), "showing frame 0");
    Expect(Players->GetDuration(Player) == 1.0, "its duration");

    World.Tick(0.25f);   // 2.5 frames
    Expect(Near(MoverX(World, Mover), 2.5), "advanced by a tick: " + Num(MoverX(World, Mover)));
    World.Tick(0.3f);    // 5.5
    Expect(Mid.Args.size() == 1, "the event between ticks fired");

    Expect(Players->Pause(Player) && !Players->IsPlaying(Player), "paused");
    World.Tick(0.3f);
    Expect(Near(Players->GetTime(Player), 0.55), "a paused player holds");
    Expect(Players->Resume(Player), "resumed");

    Expect(Players->Seek(Player, 0.2) && Near(MoverX(World, Mover), 2.0), "seek shows its frame now");
    World.Tick(0.4f);    // 2 -> 6: passes Mid again
    Expect(Mid.Args.size() == 2, "after a seek, events ahead fire again");

    World.Tick(1.0f);    // past the end
    Expect(Finished.Args.size() == 1 && Players->GetState(Player) == ESequencePlayState::Finished, "finished at the end");
    Expect(Near(MoverX(World, Mover), 1.0), "restore-state put the mover back: " + Num(MoverX(World, Mover)));

    /* Play again from the start; then stop. */
    Heard Stopped;
    Listen(World, Stopped, "SequenceStopped");
    Expect(Players->Play(Player), "played again");
    World.Tick(0.0f);
    World.Tick(0.5f);
    Expect(Near(MoverX(World, Mover), 5.0), "from the start: " + Num(MoverX(World, Mover)));
    Expect(Players->Stop(Player) && Stopped.Args.size() == 1, "stopped");
    Expect(Near(MoverX(World, Mover), 1.0) && Near(Players->GetTime(Player), 0.0), "stop restores and rewinds");

    /* Skip: fire-on-skip only, then finished. */
    Heard End;
    Listen(World, End, "End");
    const size_t MidBefore = Mid.Args.size();
    Players->Play(Player);
    World.Tick(0.0f);
    Expect(Players->Skip(Player), "skipped");
    Expect(End.Args.size() == 1 && Mid.Args.size() == MidBefore, "skip fired End, not Mid");
    Expect(Players->GetState(Player) == ESequencePlayState::Finished && Finished.Args.size() == 2, "and finished");

    SequenceWorld::Detach(World);
    Expect(SequenceWorld::Find(&World) == nullptr, "detached");
}

static void TestLoopAndPlaySequence()
{
    std::printf("Loop and PlaySequence\n");
    SequenceAssetCache::Set("Test/Mover.nmseq", MoverAsset());

    GameCore::World World;
    SequenceWorld* Players = SequenceWorld::Attach(World);
    const GameCore::EntityID Mover = MakeMover(World);

    Heard Mid, End, Finished;
    Listen(World, Mid, "Mid");
    Listen(World, End, "End");
    Listen(World, Finished, "SequenceFinished");

    const GameCore::EntityID Looping = Players->PlaySequence("Test/Mover.nmseq", true);
    Expect(Looping.IsSet(), "PlaySequence made a player");
    World.Tick(0.0f);
    World.Tick(0.8f);   // 8
    World.Tick(0.4f);   // 12 -> 2: End, then wraps
    Expect(End.Args.size() == 1 && Mid.Args.size() == 1, "events up to the end fired once");
    Expect(Near(MoverX(World, Mover), 2.0), "wrapped to frame 2: " + Num(MoverX(World, Mover)));
    World.Tick(0.4f);   // 6: Mid again
    Expect(Mid.Args.size() == 2 && Finished.Args.empty() && Players->IsPlaying(Looping), "still looping");
    Players->Stop(Looping);

    /* Not looping: finishes, and its entity goes. */
    const GameCore::EntityID Once = Players->PlaySequence("Test/Mover.nmseq");
    World.Tick(0.0f);
    World.Tick(2.0f);
    World.Tick(0.0f);   // the deferred destroy is flushed by the tick
    Expect(Finished.Args.size() == 1, "finished");
    Expect(!World.IsAlive(Once), "a PlaySequence player is destroyed when it finishes");

    Expect(!Players->PlaySequence("Test/Missing.nmseq").IsSet(), "a sequence that cannot load makes no player");

    SequenceWorld::Detach(World);
}

// === Phase 2-4 ===============================================================

#if WITH_UNCOOKED
static void TestSectionFiles()
{
    std::printf("Sections and markers\n");
    const char* const Text = R"(<Sequence version="1" fps="30" length="300">
  <Binding id="hero" name="Hero">
    <Track type="Animation" name="Body">
      <Section start="0" end="120" clip="Anims/Walk.nmanim" rate="1.25" loop="true"/>
      <Section start="100" end="300" clip="Anims/Talk.nmanim" blendIn="20" offset="15"/>
    </Track>
    <Track type="Audio" muted="true">
      <Section start="10" end="90" sound="Audio/Steps.nmsound" volume="0.5" blendIn="5" blendOut="10"/>
    </Track>
    <Track type="Float" component="Sequence Player" field="PlayRate">
      <Key f="0" v="1" interp="User" in="0" out="0.25"/>
      <Key f="30" v="2"/>
    </Track>
  </Binding>
  <Binding id="wide" name="CamWide"/>
  <Track type="CameraCut">
    <Section start="0" end="150" camera="wide"/>
    <Section start="150" end="300" camera="hero" blendIn="30"/>
  </Track>
  <Track type="Fade"><Key f="0" v="1"/><Key f="15" v="0"/></Track>
  <Track type="SubSequence"><Section start="200" end="300" sequence="Cine/Inner.nmseq"/></Track>
  <Marker f="150" name="Reveal"/>
</Sequence>)";
    SequenceAsset Asset;
    std::string Error;
    Expect(ParseSequenceXml(Text, Asset, &Error), "sections parse: " + Error);
    if(Asset.Bindings.size() != 2) { return; }
    const SequenceTrack& Body = Asset.Bindings[0].Tracks[0];
    Expect(Body.Type == ESequenceTrackType::Animation && Body.Name == "Body" && Body.Sections.size() == 2, "an animation track");
    Expect(Body.Sections[0].Asset == "Anims/Walk.nmanim" && Near(Body.Sections[0].PlayRate, 1.25) && Body.Sections[0].bLoop,
           "a clip section's fields");
    Expect(Body.Sections[1].BlendIn == 20 && Body.Sections[1].Offset == 15, "blend and offset");
    Expect(Asset.Bindings[0].Tracks[1].bMuted && Near(Asset.Bindings[0].Tracks[1].Sections[0].Volume, 0.5), "a muted audio track");
    const SequenceKey& User = Asset.Bindings[0].Tracks[2].Channels[0].Keys[0];
    Expect(User.Interp == ESequenceInterp::User && Near(User.OutTangent, 0.25), "a user key's tangents");
    Expect(Asset.Tracks.size() == 3 && Asset.Tracks[0].Sections[1].Asset == "hero", "a camera cut names a binding");
    Expect(Asset.Markers.size() == 1 && Asset.FindMarker("Reveal") == 0, "a marker");
    Expect(Asset.Validate().empty(), "it validates");
    const std::vector<std::string> Refs = Asset.GetReferencedAssets();
    Expect(Refs.size() == 4, "four referenced files (no camera ids): " + std::to_string(Refs.size()));

    const std::string Written = WriteSequenceXml(Asset);
    SequenceAsset Again;
    Expect(ParseSequenceXml(Written, Again, &Error) && WriteSequenceXml(Again) == Written, "sections round-trip as XML");
    SequenceAsset Cooked;
    Expect(ReadSequence(WriteSequenceBinary(Asset), Cooked, &Error) && WriteSequenceXml(Cooked) == Written, "and through the cook");

    /* A camera cut naming no binding, and a fade on a binding, are reported. */
    SequenceAsset Bad = Asset;
    Bad.Tracks[0].Sections[0].Asset = "nobody";
    SequenceTrack Fade;
    Fade.Type = ESequenceTrackType::Fade;
    Bad.Bindings[0].Tracks.push_back(Fade);
    Bad.Normalize();
    Expect(Bad.Validate().size() == 2, "a bad camera and a misplaced fade are reported");
}
#endif

static void TestUserTangents()
{
    std::printf("User tangents\n");
    SequenceChannel Curve;
    SequenceKey A = Key(0, 0.0f, ESequenceInterp::User);
    A.OutTangent = 2.0f;   // leaves steeply
    Curve.Keys = { A, Key(10, 10.0f, ESequenceInterp::Linear) };
    const double Slope = (Curve.Evaluate(0.1) - Curve.Evaluate(0.0)) / 0.1;
    Expect(Near(Slope, 2.0, 0.1), "a user key leaves at its tangent: " + Num(Slope));
    Expect(Near(Curve.Evaluate(10.0), 10.0), "and arrives at the next key");
}

namespace
{
    GameCore::EntityID MakeCamera(GameCore::World& InWorld, const char* InName, const EngineCore::Vector3& InAt, const float InYaw,
                                  const float InFov)
    {
        GameCore::Entity Camera = InWorld.CreateEntity(InName);
        GameCore::TransformComponent Transform;
        Transform.Position = InAt;
        Transform.Rotation = EngineCore::Vector3(0.0, InYaw, 0.0);
        Camera.AddComponent(Transform);
        RenderCore::CameraComponent Lens;
        Lens.Projection = RenderCore::CP_Perspective;
        Lens.FovY       = InFov;
        Camera.AddComponent(Lens);
        return Camera.GetID();
    }

    /* Two cameras, a cut at 10 that blends over 10 frames, a fade 0 -> 1. */
    std::shared_ptr<SequenceAsset> ShotAsset()
    {
        auto Asset = std::make_shared<SequenceAsset>();
        Asset->FrameRate = 10;
        Asset->Length    = 20;
        SequenceBinding A, B;
        A.Id = "a"; A.Name = "CamA";
        B.Id = "b"; B.Name = "CamB";
        Asset->Bindings = { A, B };
        SequenceTrack Cut;
        Cut.Type = ESequenceTrackType::CameraCut;
        SequenceSection First, Second;
        First.Start = 0;   First.End = 10;  First.Asset = "a";
        Second.Start = 10; Second.End = 20; Second.Asset = "b"; Second.BlendIn = 10;
        Cut.Sections = { First, Second };
        SequenceTrack Fade;
        Fade.Type = ESequenceTrackType::Fade;
        Fade.Normalize();
        Fade.Channels[0].Keys = { Key(0, 0.0f), Key(20, 1.0f) };
        Asset->Tracks = { Cut, Fade };
        Asset->Normalize();
        return Asset;
    }
}

static void TestShots()
{
    std::printf("Shots and fades\n");
    GameCore::World World;
    const GameCore::EntityID CamA = MakeCamera(World, "CamA", EngineCore::Vector3(0.0, 0.0, 0.0), 0.0f, 40.0f);
    const GameCore::EntityID CamB = MakeCamera(World, "CamB", EngineCore::Vector3(10.0, 0.0, 0.0), 90.0f, 80.0f);

    SequenceInstance Instance(World, ShotAsset(), "Test/Shot.nmseq");
    Instance.Evaluate(5.0);
    Expect(Instance.GetShot().bValid && Instance.GetShot().To == CamA && !Instance.GetShot().From.IsSet(), "the first shot");
    Expect(Near(Instance.GetFade(), 0.25), "the fade: " + Num(Instance.GetFade()));
    const GameCore::EntityID Camera = Instance.GetCameraEntity();
    Expect(Camera.IsSet(), "a camera entity shows the shot");

    Instance.Evaluate(15.0);
    const SequenceShot& Shot = Instance.GetShot();
    Expect(Shot.To == CamB && Shot.From == CamA && Near(Shot.Blend, 0.5), "half way through the blend");
    Expect(Near(Shot.Pose.Position.GetX(), 5.0, 1e-3) && Near(Shot.FovY, 60.0), "position and lens blended");
    Expect(Near(Shot.Pose.Rotation.GetY(), 45.0, 0.05), "rotation blended: " + Num(Shot.Pose.Rotation.GetY()));
    const RenderCore::CameraComponent* Lens = World.GetEntity(Camera).GetComponent<RenderCore::CameraComponent>();
    Expect(Instance.GetCameraEntity() == Camera && Lens && Near(Lens->FovY, 60.0), "the same camera entity, its lens moved");

    Instance.Evaluate(20.0);
    Expect(Instance.GetShot().bValid && Instance.GetShot().To == CamB, "the last frame keeps the last shot");

    Instance.Release();
    Expect(!World.IsAlive(Camera) && !Instance.GetShot().bValid, "released: the camera entity goes");

    SequenceInstanceOptions Editor;
    Editor.bCameraEntity = false;
    SequenceInstance Preview(World, ShotAsset(), "Test/Shot.nmseq", Editor);
    Preview.Evaluate(5.0);
    Expect(Preview.GetShot().bValid && !Preview.GetCameraEntity().IsSet(), "without bCameraEntity, the shot alone");
}

static void TestSubSequences()
{
    std::printf("Sub-sequences\n");
    auto Inner = std::make_shared<SequenceAsset>(*MoverAsset());   // x 0 -> 10 over 10 frames, Mid at 5
    SequenceAssetCache::Set("Test/Inner.nmseq", Inner);

    auto Outer = std::make_shared<SequenceAsset>();
    Outer->FrameRate = 10;
    Outer->Length    = 40;
    SequenceTrack Subs;
    Subs.Type = ESequenceTrackType::SubSequence;
    SequenceSection Section;
    Section.Start = 10; Section.End = 30; Section.Asset = "Test/Inner.nmseq"; Section.PlayRate = 0.5f;
    Subs.Sections = { Section };
    Outer->Tracks = { Subs };
    Outer->Normalize();

    GameCore::World World;
    const GameCore::EntityID Mover = MakeMover(World);
    Heard Mid;
    Listen(World, Mid, "Mid");

    SequenceInstance Instance(World, Outer, "Test/Outer.nmseq");
    Instance.Evaluate(20.0);   // (20 - 10) * 0.5 = frame 5 inside
    Expect(Near(MoverX(World, Mover), 5.0), "the inner sequence at its own frame: " + Num(MoverX(World, Mover)));
    Instance.FireEvents(0.0, 19.0, true);
    Expect(Mid.Args.empty(), "the inner event not yet");
    Instance.FireEvents(19.0, 21.0, false);
    Expect(Mid.Args.size() == 1, "the inner event, mapped into the outer's time");
    Instance.Evaluate(35.0);
    Expect(Near(MoverX(World, Mover), 5.0), "outside its section the inner one holds");
}

static void TestSounds()
{
    std::printf("Sounds\n");
    auto Asset = std::make_shared<SequenceAsset>();
    Asset->FrameRate = 10;
    Asset->Length    = 20;
    SequenceTrack Audio;
    Audio.Type = ESequenceTrackType::Audio;
    SequenceSection Ping;
    Ping.Start = 5; Ping.End = 15; Ping.Asset = "Test/Ping.nmsound"; Ping.Volume = 0.5f; Ping.BlendIn = 2;
    Audio.Sections = { Ping };
    Asset->Tracks = { Audio };
    Asset->Normalize();

    GameCore::World World;
    SequenceInstance Instance(World, Asset, "Test/Sound.nmseq");
    const auto Source = [&]() -> GameCore::AudioSourceComponent*
    {
        GameCore::AudioSourceComponent* Found = nullptr;
        World.Each<GameCore::AudioSourceComponent>([&Found](GameCore::Entity, GameCore::AudioSourceComponent& InSource) { Found = &InSource; });
        return Found;
    };

    Instance.UpdatePlayback(4.0, true);
    Expect(!Source(), "no sound before its section");
    Instance.UpdatePlayback(6.0, true);
    GameCore::AudioSourceComponent* Made = Source();
    Expect(Made && std::string(Made->Sound) == "Test/Ping.nmsound" && Made->bPendingPlay, "inside it, a source is asked to play");
    Expect(Made && Near(Made->Volume, 0.25) && Near(Made->PendingSeekSeconds, 0.1), "faded in, and seeked to its place");
    Made->bPendingPlay = 0; Made->PendingSeekSeconds = -1.0f;

    Instance.UpdatePlayback(7.0, false);
    Expect(Source()->bPendingPause, "paused with the sequence");
    Instance.UpdatePlayback(8.0, true);
    Expect(Source()->bPendingResume && Near(Source()->PendingSeekSeconds, 0.3), "resumed where the sequence is");
    Source()->PendingSeekSeconds = -1.0f;
    Instance.NoteJump();
    Instance.UpdatePlayback(12.0, true);
    Expect(Near(Source()->PendingSeekSeconds, 0.7), "a jump seeks it");
    Instance.UpdatePlayback(16.0, true);
    Expect(Source()->bPendingStop, "stopped past its section");

    Instance.Release();
    World.Tick(0.0f);
    Expect(!Source(), "released: the helper goes");
}

static void TestHeldValues()
{
    std::printf("Held values\n");
    GameCore::World World;
    const GameCore::EntityID Mover = MakeMover(World);
    World.GetEntity(Mover).AddComponent(GameCore::RigidBodyComponent());

    SequenceInstance Instance(World, MoverAsset(), "Test/Mover.nmseq");
    Instance.Evaluate(5.0);
    const auto Motion = [&]() { return World.GetEntity(Mover).GetComponent<GameCore::RigidBodyComponent>()->Motion; };
    Expect(Motion() == GameCore::RBM_Kinematic, "a moved rigid body is kinematic while held");
    Instance.DiscardState();
    Expect(Motion() == GameCore::RBM_Dynamic && Near(MoverX(World, Mover), 5.0), "let go: dynamic again, where it was left");
}

namespace
{
    struct FakeHost
    {
        GameCore::EntityID View;
        GameCore::EntityID Before;
        float Fade = -1.0f;
        bool  bCinematic = false;
        bool  bReplica = false;
        int   ViewCalls = 0;
    };

    SequenceHost MakeHost(FakeHost& InFake)
    {
        SequenceHost Host;
        Host.SetViewTarget = [](const GameCore::EntityID InCamera, void* InUser) { auto& F = *static_cast<FakeHost*>(InUser); F.View = InCamera; ++F.ViewCalls; };
        Host.GetViewTarget = [](void* InUser) { return static_cast<FakeHost*>(InUser)->Before; };
        Host.SetFade       = [](const float InAlpha, void* InUser) { static_cast<FakeHost*>(InUser)->Fade = InAlpha; };
        Host.SetCinematic  = [](const bool bInOn, void* InUser) { static_cast<FakeHost*>(InUser)->bCinematic = bInOn; };
        Host.IsReplica     = [](GameCore::EntityID, void* InUser) { return static_cast<FakeHost*>(InUser)->bReplica; };
        Host.UserData = &InFake;
        return Host;
    }
}

static void TestHost()
{
    std::printf("Host\n");
    SequenceAssetCache::Set("Test/Shot.nmseq", ShotAsset());
    GameCore::World World;
    MakeCamera(World, "CamA", EngineCore::Vector3(0.0, 0.0, 0.0), 0.0f, 40.0f);
    MakeCamera(World, "CamB", EngineCore::Vector3(10.0, 0.0, 0.0), 90.0f, 80.0f);
    const GameCore::EntityID Gameplay = MakeCamera(World, "PlayerCam", EngineCore::Vector3(0.0, 5.0, 0.0), 0.0f, 70.0f);

    FakeHost Fake;
    Fake.Before = Gameplay;
    SequenceWorld* Players = SequenceWorld::Attach(World);
    Players->SetHost(MakeHost(Fake));
    const GameCore::EntityID Player = MakePlayer(World, "Test/Shot.nmseq", true);
    World.GetEntity(Player).GetComponent<SequencePlayerComponent>()->bCinematic = 1;

    World.Tick(0.0f);
    SequenceInstance* Instance = Players->FindInstance(Player);
    Expect(Instance && Fake.View == Instance->GetCameraEntity() && Fake.View.IsSet(), "the view goes to the sequence's camera");
    Expect(Fake.bCinematic && Near(Fake.Fade, 0.0), "cinematic, the fade clear");
    World.Tick(1.0f);
    Expect(Near(Fake.Fade, 0.5), "the fade follows: " + Num(Fake.Fade));

    Expect(Players->SkipCinematics() == 1, "Back skips a skippable cinematic");
    Expect(Fake.View == Gameplay && !Fake.bCinematic && Fake.Fade < 0.0f, "finished: the view, input and screen given back");

    /* Markers: a seek by name. */
    SequenceAsset WithMarker = *ShotAsset();
    WithMarker.Markers.push_back({ 15, "Blend" });
    SequenceAssetCache::Set("Test/Shot.nmseq", std::make_shared<SequenceAsset>(WithMarker));
    Expect(Players->SeekToMarker(Player, "Blend") && Near(Players->GetTime(Player), 1.5), "seek to a marker");
    Expect(!Players->SeekToMarker(Player, "Nope"), "a marker it lacks");
    SequenceWorld::Detach(World);
}

static void TestReplica()
{
    std::printf("Replicas\n");
    SequenceAssetCache::Set("Test/Mover.nmseq", MoverAsset());

    GameCore::World Server, Client;
    SequenceWorld* ServerPlayers = SequenceWorld::Attach(Server);
    SequenceWorld* ClientPlayers = SequenceWorld::Attach(Client);
    FakeHost ClientHost;
    ClientHost.bReplica = true;
    ClientPlayers->SetHost(MakeHost(ClientHost));

    MakeMover(Server);
    const GameCore::EntityID ClientMover = MakeMover(Client);
    const GameCore::EntityID ServerPlayer = MakePlayer(Server, "Test/Mover.nmseq", false);
    const GameCore::EntityID ClientPlayer = MakePlayer(Client, "Test/Mover.nmseq", true);   // AutoPlay: ignored on a replica

    /* What replication would carry. */
    const auto Replicate = [&]()
    {
        const SequencePlayerComponent* From = Server.GetEntity(ServerPlayer).GetComponent<SequencePlayerComponent>();
        SequencePlayerComponent*       To   = Client.GetEntity(ClientPlayer).GetComponent<SequencePlayerComponent>();
        To->NetState = From->NetState; To->NetSerial = From->NetSerial; To->NetSeconds = From->NetSeconds;
    };

    Client.Tick(0.1f);
    Expect(ClientPlayers->GetState(ClientPlayer) == ESequencePlayState::Stopped, "a replica does not auto play");

    ServerPlayers->Play(ServerPlayer);
    Server.Tick(0.0f);
    Server.Tick(0.3f);
    ServerPlayers->Seek(ServerPlayer, 0.3);   // publish where it is
    Replicate();
    Client.Tick(0.0f);
    Expect(ClientPlayers->IsPlaying(ClientPlayer) && Near(ClientPlayers->GetTime(ClientPlayer), 0.3, 0.11), "the replica starts where the server is");
    Expect(Near(MoverX(Client, ClientMover), 3.0, 1.1), "and shows it");

    ServerPlayers->Pause(ServerPlayer);
    Replicate();
    Client.Tick(0.1f);
    Expect(ClientPlayers->GetState(ClientPlayer) == ESequencePlayState::Paused, "paused as the server is");

    ServerPlayers->Stop(ServerPlayer);
    Replicate();
    Client.Tick(0.1f);
    Expect(ClientPlayers->GetState(ClientPlayer) == ESequencePlayState::Stopped, "stopped as the server is");
    SequenceWorld::Detach(Client);
    SequenceWorld::Detach(Server);
}

// === Animation ===============================================================

/* Writes its rig with the editor-only Save*File: editor builds only. */
#if WITH_EDITOR
namespace
{
    /* Root, and an Arm 100 along X with a Hand 50 further: RenderCore's test rig. */
    RenderCore::SkeletonData ArmSkeleton()
    {
        RenderCore::SkeletonData Skeleton;
        Skeleton.Bones.resize(3);
        Skeleton.Bones[0].Name = "Root";
        Skeleton.Bones[1].Name = "Arm";
        Skeleton.Bones[1].Parent = 0;
        Skeleton.Bones[1].RefPose.Translation = { 100.0f, 0.0f, 0.0f };
        Skeleton.Bones[2].Name = "Hand";
        Skeleton.Bones[2].Parent = 1;
        Skeleton.Bones[2].RefPose.Translation = { 50.0f, 0.0f, 0.0f };
        return Skeleton;
    }

    /* The arm turned 0 -> InDegrees about Z over one second. */
    RenderCore::AnimClipData ArmClip(const float InDegrees)
    {
        RenderCore::AnimClipData Clip;
        Clip.SampleRate = 30.0f;
        Clip.FrameCount = 31;
        Clip.bLoop      = false;
        Clip.RootBone   = "Root";
        RenderCore::AnimTrack Track;
        Track.Bone = "Arm";
        Track.Translations.push_back({ 100.0f, 0.0f, 0.0f });
        Track.Scales.push_back({ 1.0f, 1.0f, 1.0f });
        for(uint32_t Frame = 0; Frame < Clip.FrameCount; ++Frame)
        {
            const float Half = 0.5f * InDegrees * 0.0174532925f * float(Frame) / 30.0f;
            Track.Rotations.push_back({ 0.0f, 0.0f, std::sin(Half), std::cos(Half) });
        }
        Clip.Tracks.push_back(Track);
        return Clip;
    }
}

static void TestAnimationTrack()
{
    std::printf("Animation\n");
    const std::string Scratch = "SequencerTestScratch/";
    RenderCore::SkeletonData Skeleton = ArmSkeleton();
    RenderCore::AnimClipData Bend = ArmClip(90.0f), Still = ArmClip(0.0f);
    RenderCore::MeshData Mesh;
    Mesh.Positions = { 150.0f, 0.0f, 0.0f,   160.0f, 0.0f, 0.0f,   150.0f, 10.0f, 0.0f };
    Mesh.Normals   = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    Mesh.UV0       = { 0, 0,  1, 0,  0, 1 };
    Mesh.Indices   = { 0, 1, 2 };
    Mesh.Sections.push_back({ "Tri", "", 0, 3 });
    Mesh.Skeleton  = Scratch + "Arm.nmskel";
    std::vector<RenderCore::AnimMatrix> Model;
    Skeleton.ComputeRefPoseModel(Model);
    for(const RenderCore::AnimMatrix& Each : Model) { Mesh.InverseBind.push_back(RenderCore::AnimMath::Inverse(Each)); }
    Mesh.Joints  = { 2, 0, 0, 0,  2, 0, 0, 0,  2, 0, 0, 0 };
    Mesh.Weights = { 1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0 };
    RenderCore::ComputeMeshBounds(Mesh);
    Expect(RenderCore::SaveSkeletonFile(Scratch + "Arm.nmskel", Skeleton) && RenderCore::SaveAnimClipFile(Scratch + "Bend.nmanim", Bend)
           && RenderCore::SaveAnimClipFile(Scratch + "Still.nmanim", Still) && RenderCore::SaveMeshFile(Scratch + "Arm.nmmesh", Mesh),
           "the rig's assets are written");

    GameCore::World World;
    RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Attach(World);
    GameCore::Entity Actor = World.CreateEntity("Actor");
    Actor.AddComponent(GameCore::TransformComponent());
    RenderCore::RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", (Scratch + "Arm.nmmesh").c_str());
    Actor.AddComponent(Render);
    RenderCore::AnimationComponent Own;
    std::snprintf(Own.Animation, sizeof(Own.Animation), "%s", (Scratch + "Still.nmanim").c_str());
    Actor.AddComponent(Own);
    World.Tick(0.0f);

    /* Bend over frames 0..10 (10 fps: one second); Still blends in over 8..12. */
    auto Asset = std::make_shared<SequenceAsset>();
    Asset->FrameRate = 10;
    Asset->Length    = 20;
    SequenceBinding Binding;
    Binding.Id = "actor"; Binding.Name = "Actor";
    SequenceTrack Clips;
    Clips.Type = ESequenceTrackType::Animation;
    SequenceSection First, Second;
    First.Start = 0;  First.End = 10;  First.Asset = Scratch + "Bend.nmanim";
    Second.Start = 8; Second.End = 20; Second.Asset = Scratch + "Still.nmanim"; Second.BlendIn = 4;
    Clips.Sections = { First, Second };
    Binding.Tracks = { Clips };
    Asset->Bindings = { Binding };
    Asset->Normalize();

    const auto HandY = [&]() { return Animation->FindInstance(Actor)->GetModelPose()[2].M[1][3]; };
    SequenceInstance Instance(World, Asset, "Test/Anim.nmseq");
    Instance.Evaluate(5.0);
    RenderCore::AnimInstance* Pose = Animation->FindInstance(Actor);
    Expect(Pose && Pose->GetClip() == RenderCore::AnimClipAssetCache::Find(First.Asset) && Near(Pose->GetTime(), 0.5, 1e-3),
           "a section plays its clip at the sequence's time");
    Expect(Actor.GetComponent<RenderCore::AnimationComponent>()->bPaused == 1, "the component's own advance is held");
    const float HalfBent = HandY();
    World.Tick(0.5f);
    Expect(Near(HandY(), HalfBent, 1e-3), "a world tick does not move a clip the sequence holds");

    Instance.Evaluate(9.9);
    Expect(Pose->HasOverlayClip(), "where sections overlap, the next blends in over the first");
    Instance.Evaluate(15.0);
    Expect(!Pose->HasOverlayClip() && Pose->GetClip() == RenderCore::AnimClipAssetCache::Find(Second.Asset), "blended in, it plays alone");

    Instance.Release();
    Expect(Actor.GetComponent<RenderCore::AnimationComponent>()->bPaused == 0, "released: the component plays itself again");
    Expect(Animation->FindInstance(Actor)->GetClip() == RenderCore::AnimClipAssetCache::Find(Scratch + "Still.nmanim"),
           "its own clip back");
    RenderCore::AnimationSystem::Detach(World);
}
#endif // WITH_EDITOR

// === Post process ============================================================

static void TestPostProcess()
{
    std::printf("Post process\n");
    /* RenderCore's side: settings by name turn their effect on. */
    RenderCore::PostProcessSettings Settings;
    RenderCore::EPostProcessParam Param;
    Expect(RenderCore::FindPostProcessParam("DofFocusDistance", Param) && !RenderCore::FindPostProcessParam("Vignette", Param), "setting names");
    RenderCore::FindPostProcessParam("DofFocusDistance", Param);
    RenderCore::ApplyPostProcessParam(Settings, Param, 1234.0f);
    Expect(Settings.bDepthOfField && Near(Settings.DofFocusDistance, 1234.0), "a focus distance turns depth of field on");

    auto Asset = std::make_shared<SequenceAsset>();
    Asset->FrameRate = 10;
    Asset->Length    = 40;
    SequenceTrack Stacks;
    Stacks.Type = ESequenceTrackType::PostProcess;
    SequenceSection Section;
    Section.Start = 10; Section.End = 30; Section.Asset = "Test/Dream.pp.nmgraph"; Section.Volume = 0.8f; Section.BlendIn = 10;
    Stacks.Sections = { Section };
    SequenceTrack Exposure;
    Exposure.Type  = ESequenceTrackType::PostProcessSetting;
    Exposure.Field = "Exposure";
    Exposure.Normalize();
    Exposure.Channels[0].Keys = { Key(0, -1.0f), Key(20, 1.0f) };
    Asset->Tracks = { Stacks, Exposure };
    Asset->Normalize();
    Expect(Asset->Validate().empty(), "post process tracks validate");

#if WITH_UNCOOKED
    const std::string Xml = WriteSequenceXml(*Asset);
    Expect(Xml.find("stack=\"Test/Dream.pp.nmgraph\"") != std::string::npos && Xml.find("weight=\"0.8") != std::string::npos
           && Xml.find("setting=\"Exposure\"") != std::string::npos, "written as stack, weight and setting");
    SequenceAsset Again;
    std::string Error;
    Expect(ParseSequenceXml(Xml, Again, &Error) && WriteSequenceXml(Again) == Xml, "and read back");
#endif

    SequenceAsset Bad = *Asset;
    Bad.Tracks[1].Field = "Vignette";
    Expect(Bad.Validate().size() == 1, "an unknown setting is reported");

    GameCore::World World;
    SequenceInstance Instance(World, Asset, "Test/Post.nmseq");
    Instance.Evaluate(15.0);
    const SequencePostProcess& Post = Instance.GetPostProcess();
    Expect(Post.Stacks.size() == 1 && Near(Post.Stacks[0].Weight, 0.4), "a stack at its weight times its fade: " +
           (Post.Stacks.empty() ? std::string("none") : Num(Post.Stacks[0].Weight)));
    Expect(Post.Settings.size() == 1 && Post.Settings[0].Name == "Exposure" && Near(Post.Settings[0].Value, 0.5), "a setting keyed");
    Instance.Evaluate(35.0);
    Expect(Instance.GetPostProcess().Stacks.empty() && Instance.GetPostProcess().Settings.size() == 1, "past its section the stack goes");

    /* A player lays it over the World's post process, and takes it off when it stops. */
    SequenceAssetCache::Set("Test/Post.nmseq", Asset);
    SequenceWorld* Players = SequenceWorld::Attach(World);
    const GameCore::EntityID Player = MakePlayer(World, "Test/Post.nmseq", true);
    World.Tick(0.0f);
    World.Tick(1.5f);
    const RenderCore::PostProcessOverride Pushed = RenderCore::GetPostProcessOverride(&World);
    Expect(Pushed.Stacks.size() == 1 && Pushed.Stacks[0].Path == "Test/Dream.pp.nmgraph" && Pushed.Params.size() == 1
           && Pushed.Params[0].Param == RenderCore::EPostProcessParam::Exposure, "the World's override is the sequence's");
    RenderCore::PostProcessSettings Laid;
    RenderCore::ApplyPostProcessOverride(&World, Laid);
    Expect(Laid.bToneMapping && Near(Laid.Exposure, 0.5), "applied, the exposure is the key's: " + Num(Laid.Exposure));
    Players->Stop(Player);
    Expect(RenderCore::GetPostProcessOverride(&World).IsEmpty(), "stopped, the override is gone");
    SequenceWorld::Detach(World);
}

// === Sound mix and subtitles =================================================

static std::shared_ptr<SequenceAsset> MixAsset()
{
    auto Asset = std::make_shared<SequenceAsset>();
    Asset->FrameRate = 10;
    Asset->Length    = 40;
    SequenceTrack Duck;
    Duck.Type  = ESequenceTrackType::BusVolume;
    Duck.Field = "Music";
    Duck.Normalize();
    Duck.Channels[0].Keys = { Key(0, 1.0f), Key(20, 0.2f) };
    SequenceTrack Lines;
    Lines.Type = ESequenceTrackType::Subtitle;
    SequenceSection First;
    First.Start = 5; First.End = 20; First.Speaker = "Captain"; First.Asset = "Hold fast!";
    SequenceSection Second;
    Second.Start = 15; Second.End = 30; Second.Asset = "(wind howls)";
    Lines.Sections = { First, Second };
    SequenceTrack Voice;
    Voice.Type = ESequenceTrackType::Audio;
    SequenceSection Line;
    Line.Start = 5; Line.End = 20; Line.Asset = "Test/Line.nmsound"; Line.Bus = 4; Line.Spatial = 0;
    Voice.Sections = { Line };
    Asset->Tracks = { Duck, Lines, Voice };
    Asset->Normalize();
    return Asset;
}

static void TestSoundMix()
{
    std::printf("Sound mix and subtitles\n");
    Expect(SEQUENCE_BUS_COUNT == 6 && FindSequenceBus("Voice") == 4 && std::string(SequenceBusName(1)) == "Music" && FindSequenceBus("Loud") < 0,
           "bus names");
    const std::shared_ptr<SequenceAsset> Asset = MixAsset();
    Expect(Asset->Validate().empty(), "bus volume and subtitle tracks validate");
    SequenceAsset Bad = *Asset;
    Bad.Tracks[0].Field = "Loud";
    Expect(Bad.Validate().size() == 1, "an unknown bus is reported");

#if WITH_UNCOOKED
    const std::string Xml = WriteSequenceXml(*Asset);
    Expect(Xml.find("bus=\"Music\"") != std::string::npos && Xml.find("speaker=\"Captain\"") != std::string::npos
           && Xml.find("text=\"Hold fast!\"") != std::string::npos && Xml.find("bus=\"Voice\"") != std::string::npos
           && Xml.find("spatial=\"2D\"") != std::string::npos, "written as bus, speaker, text and spatial");
    SequenceAsset Again;
    std::string Error;
    Expect(ParseSequenceXml(Xml, Again, &Error) && WriteSequenceXml(Again) == Xml, "and read back");
#endif
    SequenceAsset Cooked;
    Expect(ReadSequenceBinary(WriteSequenceBinary(*Asset), Cooked) && Cooked.Tracks.size() == 3
           && Cooked.Tracks[1].Sections[0].Speaker == "Captain" && Cooked.Tracks[2].Sections[0].Bus == 4
           && Cooked.Tracks[2].Sections[0].Spatial == 0, "cooked and read back");

    GameCore::World World;
    SequenceInstance Instance(World, Asset, "Test/Mix.nmseq");
    Instance.Evaluate(10.0);
    Expect(Instance.GetBusGains().size() == 1 && Instance.GetBusGains()[0].Bus == "Music" && Near(Instance.GetBusGains()[0].Gain, 0.6),
           "the music ducked: " + (Instance.GetBusGains().empty() ? std::string("none") : Num(Instance.GetBusGains()[0].Gain)));
    Expect(Instance.GetSubtitle().Speaker == "Captain" && Instance.GetSubtitle().Text == "Hold fast!", "the line under the playhead");
    Instance.Evaluate(17.0);
    Expect(Instance.GetSubtitle().Speaker.empty() && Instance.GetSubtitle().Text == "(wind howls)", "the later-started line wins");
    Instance.Evaluate(35.0);
    Expect(Instance.GetSubtitle().IsEmpty(), "past them, none");

    /* A section's bus and 2D/3D reach its source. */
    Instance.UpdatePlayback(10.0, true);
    GameCore::AudioSourceComponent* Made = nullptr;
    World.Each<GameCore::AudioSourceComponent>([&Made](GameCore::Entity, GameCore::AudioSourceComponent& InSource) { Made = &InSource; });
    Expect(Made && Made->ForceBus == 5 && Made->ForceSpatial == 1, "the source forced to Voice, 2D");
    Instance.SetAudioEnabled(false);
    World.Tick(0.0f);
    Made = nullptr;
    World.Each<GameCore::AudioSourceComponent>([&Made](GameCore::Entity, GameCore::AudioSourceComponent& InSource) { Made = &InSource; });
    Expect(!Made, "audio off: the helper goes");
    Instance.Release();

    /* At twice the rate, a second in is two seconds into the sound. */
    auto Fast = std::make_shared<SequenceAsset>(*Asset);
    Fast->Tracks[2].Sections[0].PlayRate = 2.0f;
    SequenceInstance Quick(World, Fast, "Test/Fast.nmseq");
    Quick.UpdatePlayback(15.0, true);
    Made = nullptr;
    World.Each<GameCore::AudioSourceComponent>([&Made](GameCore::Entity, GameCore::AudioSourceComponent& InSource) { Made = &InSource; });
    Expect(Made && Near(Made->Pitch, 2.0) && Near(Made->PendingSeekSeconds, 2.0),
           "a fast section seeks in the sound's own seconds: " + (Made ? Num(Made->PendingSeekSeconds) : std::string("none")));
    Quick.Release();
    World.Tick(0.0f);

    /* Sounds in a World of their own (the editor's preview). */
    GameCore::World SoundWorld;
    SequenceInstanceOptions Options;
    Options.SoundWorld = &SoundWorld;
    SequenceInstance Apart(World, Asset, "Test/Mix.nmseq", Options);
    Apart.UpdatePlayback(10.0, true);
    int InLevel = 0, InSounds = 0;
    World.Each<GameCore::AudioSourceComponent>([&InLevel](GameCore::Entity, GameCore::AudioSourceComponent&) { ++InLevel; });
    SoundWorld.Each<GameCore::AudioSourceComponent>([&InSounds](GameCore::Entity, GameCore::AudioSourceComponent&) { ++InSounds; });
    Expect(InLevel == 0 && InSounds == 1, "helpers made in the sound world, none in the level");
    Apart.Release();

    /* Players: gains multiplied to the host, the subtitle shown unless off. */
    struct MixHost
    {
        std::vector<std::pair<std::string, float>> Gains;
        std::string Speaker, Text;
    } Fake;
    SequenceHost Host;
    Host.SetBusGain  = [](const char* InBus, const float InGain, void* InUser) { static_cast<MixHost*>(InUser)->Gains.emplace_back(InBus, InGain); };
    Host.SetSubtitle = [](const char* InSpeaker, const char* InText, void* InUser)
    {
        static_cast<MixHost*>(InUser)->Speaker = InSpeaker;
        static_cast<MixHost*>(InUser)->Text    = InText;
    };
    Host.UserData = &Fake;
    SequenceAssetCache::Set("Test/Mix.nmseq", Asset);
    GameCore::World Level;
    SequenceWorld* Players = SequenceWorld::Attach(Level);
    Players->SetHost(Host);
    const GameCore::EntityID A = MakePlayer(Level, "Test/Mix.nmseq", true);
    const GameCore::EntityID B = MakePlayer(Level, "Test/Mix.nmseq", true);
    Level.Tick(0.0f);
    Level.Tick(1.0f);
    Expect(!Fake.Gains.empty() && Fake.Gains.back().first == "Music" && Near(Fake.Gains.back().second, 0.36),
           "two players' ducks multiply: " + (Fake.Gains.empty() ? std::string("none") : Num(Fake.Gains.back().second)));
    Expect(Fake.Speaker == "Captain" && Fake.Text == "Hold fast!", "the subtitle reaches the host");
    SequenceWorld::SetSubtitlesEnabled(false);
    Level.Tick(0.1f);
    Expect(Fake.Text.empty() && !SequenceWorld::AreSubtitlesEnabled(), "subtitles off: none shown");
    SequenceWorld::SetSubtitlesEnabled(true);
    Players->Stop(A);
    Players->Stop(B);
    Expect(!Fake.Gains.empty() && Near(Fake.Gains.back().second, 1.0) && Fake.Text.empty(), "stopped: the bus put back, the line cleared");
    SequenceWorld::Detach(Level);
}

int main()
{
    TestChannels();
#if WITH_UNCOOKED
    TestXml();
#endif
    TestValidate();
    TestInstance();
    TestFields();
    TestPlayers();
    TestLoopAndPlaySequence();
#if WITH_UNCOOKED
    TestSectionFiles();
#endif
    TestUserTangents();
    TestShots();
    TestSubSequences();
    TestSounds();
    TestHeldValues();
    TestHost();
    TestReplica();
#if WITH_EDITOR
    TestAnimationTrack();
#endif
    TestPostProcess();
    TestSoundMix();

    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

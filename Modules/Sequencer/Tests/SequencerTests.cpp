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

    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

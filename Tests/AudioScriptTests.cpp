/*
 * NimbleEngineAudioScriptTests -- sound driven from scripts: the Audio script
 * library and the Audio Source component from Lua, and a script graph -- the
 * PhysicsLab buggy's engine sound, pitched by its RPM and loud with its
 * throttle -- compiled and run on a prefab.
 *
 * Built only with the module (NIMBLE_WITH_AUDIO). On an offline device, so
 * it runs headless.
 */

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Public/Audio/AudioDevice.h"
#include "Public/Audio/AudioWorld.h"
#include "Public/Audio/SoundAsset.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/Schema.h"

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
    const char* const LoopSound = "AudioScriptTests_EngineLoop.nmsound";
    const char* const BlipSound = "AudioScriptTests_Blip.nmsound";

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.3f", InValue);
        return Buffer;
    }

    /* A 16-bit mono WAV of a sine, in memory. */
    std::vector<uint8_t> MakeWav(const double InSeconds)
    {
        const uint32_t Rate = 48000, Frames = uint32_t(InSeconds * Rate), Data = Frames * 2;
        std::vector<uint8_t> Out;
        const auto U32 = [&Out](const uint32_t V) { for(int I = 0; I < 4; ++I) { Out.push_back(uint8_t(V >> (8 * I))); } };
        const auto U16 = [&Out](const uint16_t V) { Out.push_back(uint8_t(V)); Out.push_back(uint8_t(V >> 8)); };
        const auto Tag = [&Out](const char* T) { Out.insert(Out.end(), T, T + 4); };
        Tag("RIFF"); U32(36 + Data); Tag("WAVE");
        Tag("fmt "); U32(16); U16(1); U16(1); U32(Rate); U32(Rate * 2); U16(2); U16(16);
        Tag("data"); U32(Data);
        for(uint32_t Frame = 0; Frame < Frames; ++Frame)
        {
            U16(uint16_t(int16_t(std::lround(0.5 * 32767.0 * std::sin(2.0 * 3.14159265358979 * 110.0 * Frame / Rate)))));
        }
        return Out;
    }

    void WriteSound(const char* InPath, const double InSeconds, const bool bInLoop)
    {
        Audio::SoundData Data;
        Data.Source         = MakeWav(InSeconds);
        Data.Settings.bLoop = bInLoop;
        Audio::SaveSoundFile(InPath, Data);
        Audio::SoundAssetCache::Invalidate(InPath);
    }

    /* InEntity made an instance of a prefab of itself that runs InScript (as
     * NodeGraphTests does it). */
    GameCore::Entity AttachScript(GameCore::World& InWorld, const GameCore::Entity& InEntity, const char* InScript)
    {
        GameCore::PrefabAsset Asset;
        GameCore::CapturePrefab(InWorld, InEntity.GetID(), Asset);
        Asset.SetScriptPath(InScript);
        const GameCore::TransformPose At = InEntity.GetWorldPose();
        const std::string Name = InEntity.GetName();
        InWorld.DestroyEntity(InEntity.GetID());
        return InWorld.SpawnPrefab(Asset, At, Name.c_str()).GetRoot();
    }

    void Tick(GameCore::World& InWorld, const int InFrames = 1)
    {
        std::vector<float> Mix(2400 * 2);
        for(int Index = 0; Index < InFrames; ++Index)
        {
            InWorld.Tick(0.05f);
            Audio::AudioDevice::Get().Render(Mix.data(), 2400);
        }
    }

    void TestLua()
    {
        std::printf("Lua\n");
        GameCore::World World;
        Audio::AudioWorld Sound;
        Expect(Sound.Attach(World), "the World has audio");

        GameCore::ScriptEngine Scripts;
        Expect(Scripts.Initialize(), "ScriptEngine::Initialize");
        Scripts.BindWorld(&World);

        GameCore::Entity Car = World.CreateEntity("Car");
        Car.AddComponent(GameCore::TransformComponent());
        GameCore::AudioSourceComponent Source;
        Source.SetSound(LoopSound);
        Source.bAutoPlay = 0;
        Car.AddComponent(Source);
        Tick(World);
        Expect(!Car.GetComponent<GameCore::AudioSourceComponent>()->IsPlaying(), "not AutoPlay: silent until asked");

        const std::string Lua = std::string(R"(
            local Engine = World.FindEntity("Car"):GetComponent("Audio Source")
            if Audio.IsAvailable() then
                Engine:SetPitch(1.5)
                Engine:SetVolume(0.5)
                Engine:Play()
            end
            Audio.SetBusVolume("SFX", 0.25)
            Audio.PlaySoundAt(")") + BlipSound + R"(", 0, 0, 100, 1.0)
            Events.On("SoundFinished", function(Source)
                World.FindEntity("Car"):GetComponent("Audio Source"):SetVolume(0.75)
            end)
        )";
        Expect(Scripts.RunString(Lua.c_str(), "audio-lua"), "the Lua runs");
        Tick(World);

        const auto* Engine = Car.GetComponent<GameCore::AudioSourceComponent>();
        Audio::AudioVoiceState State;
        Expect(Engine->IsPlaying(), "Play from Lua starts it");
        Expect(Sound.GetVoiceState(Car.GetID(), State) && std::fabs(State.Pitch - 1.5f) < 1e-4f,
               "SetPitch from Lua reaches the voice: " + Num(State.Pitch));
        Expect(std::fabs(Audio::AudioDevice::Get().GetBusVolume(Audio::EAudioBus::SFX) - 0.25f) < 1e-6f,
               "Audio.SetBusVolume sets the bus");
        Expect(std::fabs(NimbleEngine::GetBusVolume("sfx") - 0.25f) < 1e-6f, "and the facade reads it back");
        Audio::AudioDevice::Get().SetBusVolume(Audio::EAudioBus::SFX, 1.0f);

        Audio::AudioWorldStats Stats;
        Sound.GetStats(Stats);
        Expect(Stats.OneShots == 1, "Audio.PlaySoundAt plays a one-shot");

        // A short source that ends: SoundFinished reaches the Lua handler.
        GameCore::Entity Blip = World.CreateEntity("Blip");
        Blip.AddComponent(GameCore::TransformComponent());
        GameCore::AudioSourceComponent Short;
        Short.SetSound(BlipSound);
        Blip.AddComponent(Short);
        Tick(World, 10);
        Expect(std::fabs(Car.GetComponent<GameCore::AudioSourceComponent>()->Volume - 0.75f) < 1e-6f,
               "SoundFinished runs the Lua handler");
        Expect(Scripts.GetErrorCount() == 0, "no Lua errors");

        Scripts.UnbindWorld();
        Scripts.Shutdown();
        Sound.Detach();
    }

    using namespace NodeGraph;

    NodeGuid Id(const uint64_t InNumber)
    {
        NodeGuid Guid;
        Guid.Hi = 0x617564696f746573ull;
        Guid.Lo = InNumber;
        return Guid;
    }

    Node& AddNode(Graph& InGraph, const uint64_t InId, const char* InType)
    {
        Node Added;
        Added.Id   = Id(InId);
        Added.Type = InType;
        InGraph.Nodes.push_back(Added);
        return InGraph.Nodes.back();
    }

    void Connect(Graph& InGraph, const uint64_t InFrom, const char* InFromPin, const uint64_t InTo, const char* InToPin)
    {
        InGraph.Links.push_back({ PinRef{ Id(InFrom), InFromPin }, PinRef{ Id(InTo), InToPin } });
    }

    /*
     * The PhysicsLab buggy's engine sound, as a script graph on its prefab:
     * every tick its Audio Source's Pitch follows the engine (0.6 idle to 2.0
     * at MaxRPM) and its Volume the throttle (0.55 coasting to 1.0).
     */
    Graph MakeEngineSoundGraph()
    {
        Graph G;
        G.SchemaName = "Script";
        G.Name       = "EngineSound";
        G.Properties["TickPhase"] = "PostPhysics";
        G.Variables.push_back({ "MaxRPM", "number", "6000" });

        AddNode(G, 1, "Event.OnTick");
        AddNode(G, 2, "Entity.Self");
        AddNode(G, 3, "Component.Vehicle.Get");
        AddNode(G, 4, "Component.Vehicle.GetEngineRPM");
        AddNode(G, 5, "Variable.Get").Properties["Variable"] = "MaxRPM";
        AddNode(G, 6, "Math.Divide");
        AddNode(G, 7, "Math.Clamp");
        Node& Pitch = AddNode(G, 8, "Math.Lerp");
        Pitch.Constants["A"] = "0.6";
        Pitch.Constants["B"] = "2";
        AddNode(G, 9, "Component.Audio Source.Get");
        AddNode(G, 10, "Component.Audio Source.SetPitch");
        AddNode(G, 11, "Component.Vehicle.GetForward");
        AddNode(G, 12, "Math.Abs");
        Node& Volume = AddNode(G, 13, "Math.Lerp");
        Volume.Constants["A"] = "0.55";
        Volume.Constants["B"] = "1";
        AddNode(G, 14, "Component.Audio Source.SetVolume");

        Connect(G, 2, "Entity", 3, "Entity");
        Connect(G, 3, "Vehicle", 4, "Target");
        Connect(G, 4, "EngineRPM", 6, "A");
        Connect(G, 5, "Value", 6, "B");
        Connect(G, 6, "Out", 7, "X");
        Connect(G, 7, "Out", 8, "T");
        Connect(G, 2, "Entity", 9, "Entity");

        Connect(G, 1, "Then", 10, "Exec");
        Connect(G, 9, "Audio Source", 10, "Target");
        Connect(G, 8, "Out", 10, "Pitch");

        Connect(G, 3, "Vehicle", 11, "Target");
        Connect(G, 11, "Forward", 12, "X");
        Connect(G, 12, "Out", 13, "T");
        Connect(G, 10, "Then", 14, "Exec");
        Connect(G, 9, "Audio Source", 14, "Target");
        Connect(G, 13, "Out", 14, "Volume");
        return G;
    }

    void TestGraph()
    {
        std::printf("Script graph\n");
        RefreshSchemas();   // after the Audio library and components registered

        const Schema* Script = FindSchema("Script");
        Expect(Script && Script->FindNode("Library.Audio.PlaySoundAt"), "the Audio library has graph nodes");
        Expect(Script && Script->FindNode("Library.Audio.OnSoundFinished"), "and its event");
        Expect(Script && Script->FindNode("Component.Audio Source.FadeTo"), "Audio Source's methods are nodes");

        const Graph G = MakeEngineSoundGraph();
        CompileResult Compiled;
        CompileGraph(G, Compiled);
        Expect(!Compiled.HasErrors(), "the engine sound graph compiles");
        if(Compiled.HasErrors()) { std::printf("%s\n", FormatDiagnostics(G, Compiled).c_str()); return; }

        const std::string Path = (std::filesystem::temp_directory_path() / "NimbleEngineSound.lua").generic_string();
        std::ofstream(Path, std::ios::binary | std::ios::trunc) << Compiled.Code;

        GameCore::World World;
        Audio::AudioWorld Sound;
        Sound.Attach(World);
        GameCore::ScriptEngine Scripts;
        Scripts.Initialize();
        Scripts.BindWorld(&World);

        GameCore::Entity Buggy = World.CreateEntity("Buggy");
        Buggy.AddComponent(GameCore::TransformComponent());
        Buggy.AddComponent(GameCore::VehicleComponent());
        GameCore::AudioSourceComponent Engine;
        Engine.SetSound(LoopSound);
        Buggy.AddComponent(Engine);
        Buggy = AttachScript(World, Buggy, Path.c_str());

        // No physics here: the engine's numbers, as JoltPhysics would write them.
        auto* Vehicle = Buggy.GetComponent<GameCore::VehicleComponent>();
        Vehicle->EngineRPM = 0.0f;
        Vehicle->Forward   = 0.0f;
        Tick(World, 2);
        Audio::AudioVoiceState Idle;
        Expect(Sound.GetVoiceState(Buggy.GetID(), Idle) && Idle.bPlaying, "the engine sound plays");
        Expect(std::fabs(Idle.Pitch - 0.6f) < 1e-3f && std::fabs(Idle.Volume - 0.55f) < 1e-3f,
               "idle: pitch " + Num(Idle.Pitch) + ", volume " + Num(Idle.Volume));

        Vehicle->EngineRPM = 3000.0f;
        Vehicle->Forward   = 1.0f;
        Tick(World, 2);
        Audio::AudioVoiceState Half;
        Sound.GetVoiceState(Buggy.GetID(), Half);
        Expect(std::fabs(Half.Pitch - 1.3f) < 1e-3f && std::fabs(Half.Volume - 1.0f) < 1e-3f,
               "half revs, full throttle: pitch " + Num(Half.Pitch) + ", volume " + Num(Half.Volume));

        Vehicle->EngineRPM = 9000.0f;   // over MaxRPM: clamped
        Tick(World, 2);
        Audio::AudioVoiceState Redline;
        Sound.GetVoiceState(Buggy.GetID(), Redline);
        Expect(std::fabs(Redline.Pitch - 2.0f) < 1e-3f, "redline: pitch " + Num(Redline.Pitch));
        Expect(Scripts.GetErrorCount() == 0, "the generated script raised no Lua errors");

        Scripts.UnbindWorld();
        Scripts.Shutdown();
        Sound.Detach();
        std::filesystem::remove(Path);
    }
}

int main()
{
    Audio::AudioDeviceSettings Offline;
    Offline.bOffline = true;
    Offline.Channels = 2;
    if(!Audio::AudioDevice::Get().Initialize(Offline))
    {
        std::printf("no offline audio device\n");
        return 1;
    }
    WriteSound(LoopSound, 0.5, true);
    WriteSound(BlipSound, 0.2, false);

    TestLua();
    TestGraph();

    std::remove(LoopSound);
    std::remove(BlipSound);
    Audio::AudioDevice::Get().Shutdown();

    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

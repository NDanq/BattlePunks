/*
 * AudioTests -- the Audio module on an offline device (no sound card: the
 * mix advances only as the test renders it), so it runs headless and every
 * check reads the exact samples the speakers would have been given.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Public/Audio/AudioDevice.h"
#include "Public/Audio/AudioWorld.h"
#include "Public/Audio/SoundAsset.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"

#include <chrono>
#include <thread>

using namespace Audio;

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
    AudioDeviceSettings OfflineSettings()
    {
        AudioDeviceSettings Settings;
        Settings.bOffline   = true;
        Settings.SampleRate = 48000;
        Settings.Channels   = 2;
        return Settings;
    }

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.4f", InValue);
        return Buffer;
    }

    /* The mix's next InSeconds, rendered; the RMS of each channel. */
    struct Levels
    {
        double Left  = 0.0;
        double Right = 0.0;
        double Both() const { return 0.5 * (Left + Right); }
    };

    Levels RenderLevels(const double InSeconds)
    {
        AudioDevice&   Device   = AudioDevice::Get();
        const uint32_t Channels = Device.GetChannels();
        const uint64_t Frames   = uint64_t(InSeconds * double(Device.GetSampleRate()));

        std::vector<float> Samples(size_t(Frames * Channels), 0.0f);
        const uint64_t Read = Device.Render(Samples.data(), Frames);

        double SumL = 0.0;
        double SumR = 0.0;
        for(uint64_t Frame = 0; Frame < Read; ++Frame)
        {
            const double L = Samples[size_t(Frame * Channels)];
            const double R = Samples[size_t(Frame * Channels + (Channels > 1 ? 1 : 0))];
            SumL += L * L;
            SumR += R * R;
        }
        Levels Out;
        if(Read > 0)
        {
            Out.Left  = std::sqrt(SumL / double(Read));
            Out.Right = std::sqrt(SumR / double(Read));
        }
        return Out;
    }

    void TestDevice()
    {
        std::printf("Device\n");
        AudioDevice& Device = AudioDevice::Get();

        Expect(Device.Initialize(OfflineSettings()), "an offline device starts");
        Expect(Device.IsInitialized() && Device.IsOffline(), "it says it is offline");
        Expect(Device.GetSampleRate() == 48000, "at the asked rate");
        Expect(Device.GetChannels() == 2, "in stereo");
        Expect(Device.Initialize(OfflineSettings()), "a second Initialize is a no-op that succeeds");

        Expect(RenderLevels(0.1).Both() < 1e-6, "nothing playing renders silence");
    }

    void TestBuses()
    {
        std::printf("Buses\n");
        AudioDevice& Device = AudioDevice::Get();

        EAudioBus Bus = EAudioBus::Master;
        Expect(FindBus("music", Bus) && Bus == EAudioBus::Music, "a bus is found by name, any case");
        Expect(!FindBus("Nope", Bus), "an unknown name is no bus");
        Expect(std::string(GetBusName(EAudioBus::Ambient)) == "Ambient", "and named back");

        Expect(Device.PlayTone(440.0f, 0.5f, 0.5f, EAudioBus::SFX), "a tone plays");
        Expect(Device.GetPlayingVoiceCount() == 1, "one voice playing");

        // A 0.5-amplitude sine has an RMS of 0.5/sqrt(2) -- what an unpanned
        // mono voice gives each channel of a stereo mix, as miniaudio pans
        // with constant gain.
        const Levels Full = RenderLevels(0.1);
        Expect(Full.Both() > 0.2, "the tone is heard, RMS " + Num(Full.Both()));
        Expect(std::fabs(Full.Left - Full.Right) < 1e-3, "centred: left " + Num(Full.Left) + ", right " + Num(Full.Right));

        Device.SetBusVolume(EAudioBus::SFX, 0.5f);
        Expect(std::fabs(Device.GetBusVolume(EAudioBus::SFX) - 0.5f) < 1e-6f, "the SFX bus is at half");
        const Levels Half = RenderLevels(0.1);
        Expect(std::fabs(Half.Both() / Full.Both() - 0.5) < 0.02, "and the tone with it: " + Num(Half.Both()));

        Device.SetBusMuted(EAudioBus::SFX, true);
        Expect(Device.IsBusMuted(EAudioBus::SFX), "SFX muted");
        Expect(RenderLevels(0.05).Both() < 1e-6, "silences it");
        Device.SetBusMuted(EAudioBus::SFX, false);

        Device.SetBusVolume(EAudioBus::Master, 0.0f);
        Expect(RenderLevels(0.05).Both() < 1e-6, "Master at zero silences every bus");
        Device.SetBusVolume(EAudioBus::Master, 1.0f);
        Device.SetBusVolume(EAudioBus::SFX, 1.0f);

        Device.SetBusVolume(EAudioBus::Music, 100.0f);
        Expect(Device.GetBusVolume(EAudioBus::Music) <= 4.0f, "a volume is clamped");
        Device.SetBusVolume(EAudioBus::Music, 1.0f);

        Device.SetPaused(true);
        Expect(RenderLevels(0.05).Both() < 1e-6, "paused, nothing is heard");
        Device.SetPaused(false);

        // 0.5 s in all: what is left of it, and then it has stopped itself.
        RenderLevels(0.5);
        Expect(Device.GetPlayingVoiceCount() == 0, "the tone stops after its length");
        Expect(RenderLevels(0.05).Both() < 1e-6, "and is silent");
    }

    /* A 16-bit PCM WAV of a sine at InHz, InSeconds long, InChannels wide
     * (each channel the same), with InSilence seconds of silence either side. */
    std::vector<uint8_t> MakeWav(const uint32_t InRate, const uint32_t InChannels, const double InSeconds,
                                 const double InHz, const float InAmplitude, const double InSilence = 0.0)
    {
        const uint32_t Silent = uint32_t(InSilence * InRate);
        const uint32_t Tone   = uint32_t(InSeconds * InRate);
        const uint32_t Frames = Silent + Tone + Silent;
        const uint32_t Data   = Frames * InChannels * 2;

        std::vector<uint8_t> Out;
        const auto U32 = [&Out](const uint32_t V) { for(int I = 0; I < 4; ++I) { Out.push_back(uint8_t(V >> (8 * I))); } };
        const auto U16 = [&Out](const uint16_t V) { Out.push_back(uint8_t(V)); Out.push_back(uint8_t(V >> 8)); };
        const auto Tag = [&Out](const char* T) { Out.insert(Out.end(), T, T + 4); };

        Tag("RIFF"); U32(36 + Data); Tag("WAVE");
        Tag("fmt "); U32(16); U16(1); U16(uint16_t(InChannels)); U32(InRate); U32(InRate * InChannels * 2);
        U16(uint16_t(InChannels * 2)); U16(16);
        Tag("data"); U32(Data);
        for(uint32_t Frame = 0; Frame < Frames; ++Frame)
        {
            double Value = 0.0;
            if(Frame >= Silent && Frame < Silent + Tone)
            {
                Value = InAmplitude * std::sin(2.0 * 3.14159265358979 * InHz * double(Frame - Silent) / InRate);
            }
            const int16_t Sample = int16_t(std::lround(Value * 32767.0));
            for(uint32_t Channel = 0; Channel < InChannels; ++Channel) { U16(uint16_t(Sample)); }
        }
        return Out;
    }

    void TestCook()
    {
        std::printf("Cook\n");

        const std::vector<uint8_t> Wav = MakeWav(44100, 2, 1.0, 440.0, 0.5);
        Expect(DetectSoundSourceFormat(Wav.data(), Wav.size()) == ESoundSourceFormat::Wav, "a WAV is known by its bytes");
        const uint8_t Ogg[] = { 'O', 'g', 'g', 'S', 0 };
        Expect(DetectSoundSourceFormat(Ogg, sizeof(Ogg)) == ESoundSourceFormat::Vorbis, "and an Ogg");

        SoundData Data;
        Data.Source = Wav;
        Expect(!Data.IsCooked(), "a new sound is not cooked");

        std::string Error;
        Expect(CookSound(Data, Error), "a stereo 44.1 kHz WAV cooks: " + Error);
        Expect(Data.IsCooked(), "and is then cooked");
        Expect(Data.SourceFormat == ESoundSourceFormat::Wav, "as a WAV");
        Expect(Data.Cooked.Mode == ESoundLoadMode::Decompressed, "1 s, Auto: Decompressed");
        Expect(Data.Cooked.Channels == 1, "ForceMono: one channel");
        Expect(Data.Cooked.SampleRate == 48000, "at the mix's rate");
        Expect(std::llabs(int64_t(Data.Cooked.Frames) - 48000) <= 64, "1 s of frames: " + std::to_string(Data.Cooked.Frames));
        Expect(Data.Cooked.Samples.size() == Data.Cooked.Frames, "a sample a frame");
        Expect(std::fabs(Data.Cooked.Peak - 0.5f) < 0.02f, "its peak, " + Num(Data.Cooked.Peak));
        Expect(Data.Cooked.Gain == 1.0f, "no gain without Normalize");
        Expect(Data.Cooked.Waveform.size() == WaveformBuckets, "a waveform");

        Data.Settings.bNormalize = true;
        Expect(!Data.IsCooked(), "a cook setting changed: no longer cooked");
        Data.Settings.Volume = 0.3f;
        Data.Settings.bNormalize = false;
        Expect(Data.IsCooked(), "a playback setting does not touch the cook");

        Data.Settings.bNormalize = true;
        Expect(CookSound(Data, Error) && std::fabs(Data.Cooked.Gain - 2.0f) < 0.1f, "Normalize: a gain of " + Num(Data.Cooked.Gain));

        Data.Settings.bForceMono = false;
        Data.Settings.SampleRate = 22050;
        Expect(CookSound(Data, Error) && Data.Cooked.Channels == 2 && Data.Cooked.SampleRate == 22050,
               "kept stereo, cooked to 22.05 kHz");

        Data.Settings.LoadMode = ESoundLoadMode::Compressed;
        Expect(CookSound(Data, Error) && Data.Cooked.PlaysSource() && Data.Cooked.Samples.empty(),
               "Compressed plays the source: no samples");
        Expect(Data.Cooked.SampleRate == 44100 && Data.Cooked.Channels == 2, "the source's own rate and channels");

        SoundData Trimmed;
        Trimmed.Source = MakeWav(48000, 1, 0.5, 220.0, 0.5, 0.25);
        Trimmed.Settings.bTrimSilence = true;
        Expect(CookSound(Trimmed, Error) && std::llabs(int64_t(Trimmed.Cooked.Frames) - 24000) < 200,
               "TrimSilence: 0.25 s cut from each end, " + std::to_string(Trimmed.Cooked.Frames) + " frames");

        SoundData Long;
        Long.Source = MakeWav(8000, 1, 12.0, 110.0, 0.5);
        Expect(CookSound(Long, Error) && Long.Cooked.Mode == ESoundLoadMode::Stream, "12 s, Auto: Stream");

        SoundData Bad;
        Bad.Source = { 1, 2, 3, 4, 5, 6, 7, 8 };
        Expect(!CookSound(Bad, Error) && !Error.empty(), "noise does not cook: " + Error);
    }

    void TestSoundFiles()
    {
        std::printf("Sound files\n");

        const std::string WavPath   = "AudioTests_Source.wav";
        const std::string SoundPath = "AudioTests_Sound.nmsound";
        {
            const std::vector<uint8_t> Wav = MakeWav(44100, 2, 12.0, 330.0, 0.5);
            std::ofstream File(WavPath, std::ios::binary);
            File.write(reinterpret_cast<const char*>(Wav.data()), std::streamsize(Wav.size()));
        }

        SoundData Imported;
        std::string Error;
        Expect(ImportSoundFile(WavPath, Imported, Error), "a 12 s WAV imports: " + Error);
        Expect(Imported.Cooked.Mode == ESoundLoadMode::Stream, "streamed");
        Expect(!Imported.Settings.bSpatial && Imported.Settings.Bus == EAudioBus::Music, "long: music, not spatial");
        Imported.Import.SourcePath = WavPath;
        Imported.Import.SourceTime = 1234;
        Expect(SaveSoundFile(SoundPath, Imported), "saved");

        SoundData Everything;
        Expect(LoadSoundFile(SoundPath, Everything), "and loaded");
        Expect(Everything.Source == Imported.Source, "the source byte for byte");
        Expect(Everything.IsCooked(), "still cooked");
        Expect(Everything.Import.SourcePath == WavPath && Everything.Import.SourceTime == 1234, "with its import info");
        Expect(!Everything.Settings.bSpatial && Everything.Settings.Bus == EAudioBus::Music, "and settings");

        SoundData Playback;
        Expect(LoadSoundFile(SoundPath, Playback, ESoundLoad::ForPlayback) && !Playback.Source.empty(),
               "a streamed sound loads its source for playback");

        Everything.Settings.LoadMode = ESoundLoadMode::Decompressed;
        Expect(!Everything.IsCooked(), "made Decompressed: stale");
        Expect(SaveSoundFile(SoundPath, Everything), "saving cooks it again");
        Expect(Everything.IsCooked() && Everything.Cooked.Mode == ESoundLoadMode::Decompressed, "as Decompressed");

        Expect(LoadSoundFile(SoundPath, Playback, ESoundLoad::ForPlayback), "loaded for playback");
        Expect(Playback.Source.empty(), "without its source");
        Expect(Playback.Cooked.Samples.size() == Everything.Cooked.Samples.size(), "with its samples");

        const uint64_t Generation = SoundAssetCache::GetGeneration();
        std::shared_ptr<const SoundData> Cached = SoundAssetCache::Find(SoundPath);
        Expect(Cached && Cached->Source.empty() && !Cached->Cooked.Samples.empty(), "the cache loads it for playback");
        Expect(SoundAssetCache::Find(SoundPath) == Cached, "once");
        SoundAssetCache::Invalidate(SoundPath);
        Expect(SoundAssetCache::GetGeneration() != Generation, "an Invalidate moves the generation on");
        Expect(SoundAssetCache::Find(SoundPath) != Cached, "and loads it afresh");
        Expect(!SoundAssetCache::Find("NoSuchSound.nmsound"), "a missing sound is null");

        std::remove(WavPath.c_str());
        std::remove(SoundPath.c_str());
    }

    /* A sound file from a generated WAV, cooked by InSettings' cook settings. */
    void WriteSound(const std::string& InPath, const double InSeconds, const SoundSettings& InSettings)
    {
        SoundData Data;
        Data.Source   = MakeWav(48000, 1, InSeconds, 300.0, 0.5);
        Data.Settings = InSettings;
        std::string Error;
        CookSound(Data, Error);
        SaveSoundFile(InPath, Data);
        SoundAssetCache::Invalidate(InPath);
    }

    GameCore::Entity MakeSource(GameCore::World& InWorld, const char* InSound, const EngineCore::Vector3& InAt)
    {
        GameCore::Entity E = InWorld.CreateEntity("Source");
        GameCore::TransformComponent T;
        T.Position = InAt;
        E.AddComponent(T);
        GameCore::AudioSourceComponent Source;
        Source.SetSound(InSound);
        E.AddComponent(Source);
        return E;
    }

    void SetPosition(GameCore::Entity InEntity, const EngineCore::Vector3& InAt)
    {
        InEntity.GetComponent<GameCore::TransformComponent>()->Position = InAt;
    }

    /* One frame: the World ticked (which updates its audio), then that much mixed. */
    Levels Frame(GameCore::World& InWorld, const double InSeconds = 0.05)
    {
        InWorld.Tick(float(InSeconds));
        return RenderLevels(InSeconds);
    }

    int GFinished = 0;

    void TestWorld()
    {
        std::printf("World\n");
        AudioDevice& Device = AudioDevice::Get();
        Device.Initialize(OfflineSettings());

        SoundSettings Loop;
        Loop.bLoop       = true;
        Loop.MinDistance = 100.0f;
        Loop.MaxDistance = 10000.0f;
        WriteSound("AudioTests_Loop.nmsound", 0.5, Loop);
        SoundSettings Once;
        WriteSound("AudioTests_Once.nmsound", 0.2, Once);

        GameCore::World World;
        AudioWorld Audio;
        Expect(Audio.Attach(World), "audio attaches to a World");
        Expect(AudioWorld::Find(World) == &Audio, "and is found by it");
        Expect(!AudioWorld().Attach(World), "a second one does not");

        GameCore::Entity Ears = World.CreateEntity("Ears");
        Ears.AddComponent(GameCore::TransformComponent());
        Ears.AddComponent(GameCore::AudioListenerComponent());

        // Right of a listener facing +Z: the right ear (left-handed, +X right).
        GameCore::Entity Engine = MakeSource(World, "AudioTests_Loop.nmsound", EngineCore::Vector3(300.0, 0.0, 0.0));
        Frame(World);
        const Levels Right = Frame(World, 0.1);
        Expect(Engine.GetComponent<GameCore::AudioSourceComponent>()->IsPlaying(), "AutoPlay starts it");
        Expect(Right.Right > Right.Left * 1.5, "to the right is heard on the right: L " + Num(Right.Left) + ", R " + Num(Right.Right));

        SetPosition(Engine, EngineCore::Vector3(-300.0, 0.0, 0.0));
        Frame(World);
        const Levels Left = Frame(World, 0.1);
        Expect(Left.Left > Left.Right * 1.5, "to the left, on the left: L " + Num(Left.Left) + ", R " + Num(Left.Right));

        // Turn the listener round: what was on the left is on the right.
        Ears.GetComponent<GameCore::TransformComponent>()->Rotation = EngineCore::Vector3(0.0, 180.0, 0.0);
        Frame(World);
        const Levels Turned = Frame(World, 0.1);
        Expect(Turned.Right > Turned.Left * 1.5, "the listener turned round hears it on the right");
        Ears.GetComponent<GameCore::TransformComponent>()->Rotation = EngineCore::Vector3();

        SetPosition(Engine, EngineCore::Vector3(0.0, 0.0, 200.0));
        Frame(World);
        const Levels Near = Frame(World, 0.1);
        SetPosition(Engine, EngineCore::Vector3(0.0, 0.0, 4000.0));
        Frame(World);
        const Levels Far = Frame(World, 0.1);
        Expect(Far.Both() < Near.Both() * 0.25, "farther is quieter: " + Num(Near.Both()) + " then " + Num(Far.Both()));

        auto* Source = Engine.GetComponent<GameCore::AudioSourceComponent>();
        Source->Pitch  = 2.0f;
        Source->Volume = 0.5f;
        Frame(World);
        AudioVoiceState State;
        Expect(Audio.GetVoiceState(Engine.GetID(), State) && std::fabs(State.Pitch - 2.0f) < 1e-4f
               && std::fabs(State.Volume - 0.5f) < 1e-4f, "Pitch and Volume reach the voice");

        Source->Stop();
        Frame(World);
        Expect(!Source->IsPlaying() && Frame(World).Both() < 1e-6, "Stop silences it");
        Source->Play();
        Frame(World);
        Expect(Source->IsPlaying() && Frame(World).Both() > 1e-4, "Play starts it again");
        Source->Pause();
        Frame(World);
        Expect(Source->IsPaused() && !Source->IsPlaying() && Frame(World).Both() < 1e-6, "Pause holds it");
        Source->Resume();
        Frame(World);
        Expect(Source->IsPlaying() && Frame(World).Both() > 1e-4, "Resume carries on");

        Audio.SetPaused(true);
        Frame(World);
        Expect(Frame(World).Both() < 1e-6, "a paused World is silent");
        Audio.SetPaused(false);
        Frame(World);
        Expect(Frame(World).Both() > 1e-4, "and heard again unpaused");

        // A sound that ends by itself says so.
        GFinished = 0;
        World.GetEvents().Subscribe("SoundFinished", [](GameCore::EventID, const GameCore::EventArgs&, void*) { ++GFinished; });
        GameCore::Entity Blip = MakeSource(World, "AudioTests_Once.nmsound", EngineCore::Vector3(0.0, 0.0, 100.0));
        for(int Index = 0; Index < 10; ++Index) { Frame(World); }
        Expect(GFinished == 1, "SoundFinished once its 0.2 s are done");
        Expect(!Blip.GetComponent<GameCore::AudioSourceComponent>()->IsPlaying(), "and it is no longer playing");

        AudioWorldStats Stats;
        Audio.GetStats(Stats);
        Expect(Stats.Sources == 2 && Stats.Playing == 1, "two sources, one playing");
        Engine.RemoveComponent<GameCore::AudioSourceComponent>();
        Frame(World);
        Audio.GetStats(Stats);
        Expect(Stats.Sources == 1 && Stats.Playing == 0, "a removed source takes its voice");
        Blip.Destroy();
        Frame(World);
        Audio.GetStats(Stats);
        Expect(Stats.Sources == 0, "a destroyed entity too");

        // One-shots.
        Expect(Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(0.0, 0.0, 100.0)), "a one-shot plays");
        Expect(Audio.PlaySound2D("AudioTests_Once.nmsound"), "a 2D one too");
        Expect(!Audio.PlaySound2D("NoSuchSound.nmsound"), "a missing sound does not");
        Audio.GetStats(Stats);
        Expect(Stats.OneShots == 2, "two one-shots playing");
        Expect(Frame(World).Both() > 1e-4, "and heard");
        for(int Index = 0; Index < 10; ++Index) { Frame(World); }
        Audio.GetStats(Stats);
        Expect(Stats.OneShots == 0, "freed once they end");
        for(uint32_t Index = 0; Index < AudioWorld::MaxOneShots + 10; ++Index) { Audio.PlaySound2D("AudioTests_Once.nmsound", 0.01f); }
        Audio.GetStats(Stats);
        Expect(Stats.OneShots == AudioWorld::MaxOneShots, "at most MaxOneShots: " + std::to_string(Stats.OneShots));
        Audio.StopAll();
        Audio.GetStats(Stats);
        Expect(Stats.OneShots == 0, "StopAll stops them");

        // Weighed by distance (the listener is at the origin; MaxDistance 5000).
        Expect(!Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(0.0, 0.0, 6000.0)),
               "a 3D one-shot beyond its MaxDistance is not started");
        for(uint32_t Index = 0; Index < AudioWorld::MaxOneShots; ++Index)
        {
            Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(0.0, 0.0, 3000.0));
        }
        Expect(!Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(0.0, 0.0, 4000.0)),
               "full: one farther than every playing one is dropped");
        Expect(Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(0.0, 0.0, 100.0)),
               "a nearer one replaces the farthest");
        Audio.GetStats(Stats);
        Expect(Stats.OneShots == AudioWorld::MaxOneShots, "still at most MaxOneShots");
        Audio.StopAll();

        Audio.Detach();
        Expect(!AudioWorld::Find(World), "detached");
        std::remove("AudioTests_Loop.nmsound");
        std::remove("AudioTests_Once.nmsound");
    }

    /* SPLITSCREEN: a listener per view, each sound heard from the nearest. */
    AudioListenerPose GListenerPoses[AudioDevice::AUDIO_LISTENERS_MAX];
    uint32_t          GListenerCount = 0;

    uint32_t TestListeners(GameCore::World&, AudioListenerPose* OutPoses, const uint32_t InMax, void*)
    {
        const uint32_t Count = std::min(GListenerCount, InMax);
        for(uint32_t Index = 0; Index < Count; ++Index) { OutPoses[Index] = GListenerPoses[Index]; }
        return Count;
    }

    void TestManyListeners()
    {
        std::printf("Listeners\n");
        AudioDevice& Device = AudioDevice::Get();
        Device.Initialize(OfflineSettings());
        SoundSettings Loop;
        Loop.bLoop       = true;
        Loop.MinDistance = 100.0f;
        Loop.MaxDistance = 10000.0f;
        WriteSound("AudioTests_Loop.nmsound", 0.5, Loop);
        SoundSettings Once;
        WriteSound("AudioTests_Once.nmsound", 0.2, Once);   // MaxDistance 5000

        GameCore::World World;
        AudioWorld Audio;
        Audio.Attach(World);
        Audio.SetListenersFallback(&TestListeners, nullptr);
        GListenerPoses[0].Position = EngineCore::Vector3(0.0, 0.0, 0.0);
        GListenerPoses[1].Position = EngineCore::Vector3(20000.0, 0.0, 0.0);

        GameCore::Entity Engine = MakeSource(World, "AudioTests_Loop.nmsound", EngineCore::Vector3(20300.0, 0.0, 0.0));
        GListenerCount = 1;
        Frame(World);
        const Levels One = Frame(World, 0.1);
        Expect(Audio.GetListenerCount() == 1 && Device.GetListenerCount() == 1, "one view: one listener");
        Expect(!Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(20000.0, 0.0, 100.0)),
               "far from the one listener, a one-shot is not started");

        GListenerCount = 2;
        Frame(World);
        const Levels Two = Frame(World, 0.1);
        Expect(Audio.GetListenerCount() == 2 && Device.GetListenerCount() == 2, "two views: two listeners");
        Expect(Audio.GetListener(1).Position.GetX() == 20000.0, "each where its view is");
        Expect(Two.Both() > One.Both() * 4.0 + 1e-6, "a sound near the second listener is heard from it: "
               + Num(One.Both()) + " then " + Num(Two.Both()));
        Expect(Two.Right > Two.Left * 1.5, "and placed as that listener hears it (to its right)");
        Expect(Audio.PlaySoundAt("AudioTests_Once.nmsound", EngineCore::Vector3(20000.0, 0.0, 100.0)),
               "a one-shot near the second listener starts: weighed by the nearest");
        Audio.StopAll();

        GListenerCount = 0;   // no view has a camera: the single-listener rule again
        Frame(World);
        Expect(Audio.GetListenerCount() == 1 && Device.GetListenerCount() == 1, "back to one listener");

        (void)Engine;
        Audio.Detach();
        std::remove("AudioTests_Loop.nmsound");
        std::remove("AudioTests_Once.nmsound");
    }

    /* Compressed and Stream play the source through the loading threads. */
    void TestLoadModes()
    {
        std::printf("Load modes\n");
        for(const ESoundLoadMode Mode : { ESoundLoadMode::Compressed, ESoundLoadMode::Stream })
        {
            const std::string Name = GetSoundLoadModeName(Mode);
            const std::string Path = "AudioTests_" + Name + ".nmsound";
            SoundSettings Settings;
            Settings.LoadMode = Mode;
            Settings.bLoop    = true;
            Settings.bSpatial = false;
            WriteSound(Path, 1.0, Settings);

            GameCore::World World;
            AudioWorld Audio;
            Audio.Attach(World);
            MakeSource(World, Path.c_str(), EngineCore::Vector3());

            // Async: it is heard once a loading thread has it ready.
            double Heard = 0.0;
            for(int Attempt = 0; Attempt < 100 && Heard < 1e-3; ++Attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                Heard = Frame(World).Both();
            }
            Expect(Heard > 0.1, Name + " is heard: " + Num(Heard));
            Audio.Detach();
            std::remove(Path.c_str());
        }
    }

    void TestShutdown()
    {
        std::printf("Shutdown\n");
        AudioDevice& Device = AudioDevice::Get();

        Device.PlayTone(220.0f, 10.0f);
        Device.Shutdown();
        Expect(!Device.IsInitialized(), "shut down, with a voice still playing");
        Device.Shutdown();
        Expect(!Device.IsInitialized(), "a second Shutdown is a no-op");

        float Sample[2] = { 1.0f, 1.0f };
        Expect(Device.Render(Sample, 1) == 0, "without a device nothing renders");
        Expect(!Device.PlayTone(440.0f, 1.0f), "or plays");
        Device.SetBusVolume(EAudioBus::UI, 0.25f);
        Expect(std::fabs(Device.GetBusVolume(EAudioBus::UI) - 0.25f) < 1e-6f, "a bus volume is kept for the next start");
        Device.SetBusVolume(EAudioBus::UI, 1.0f);

        Expect(Device.Initialize(OfflineSettings()), "and it starts again");
        Device.Shutdown();
    }
}

int main()
{
    TestDevice();
    TestBuses();
    TestCook();
    TestSoundFiles();
    TestWorld();
    TestManyListeners();
    TestLoadModes();
    TestShutdown();

    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

#include "Public/NimbleEngine/AudioFacade.h"

#include <cstdlib>
#include <string>

#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameConsole.h"

#if WITH_AUDIO
#include "Public/Audio/AudioDevice.h"
#include "Public/Audio/AudioWorld.h"
#endif

/* AudioFacade.h has the contract. */

namespace NimbleEngine
{
    bool IsAudioAvailable()
    {
#if WITH_AUDIO
        return Audio::AudioDevice::Get().IsInitialized();
#else
        return false;
#endif
    }

    bool StartAudio()
    {
#if WITH_AUDIO
        return Audio::AudioDevice::Get().Initialize();
#else
        return false;
#endif
    }

    void StopAudio()
    {
#if WITH_AUDIO
        Audio::AudioDevice::Get().Shutdown();
#endif
    }

    void SetAudioPaused(const bool bInPaused)
    {
#if WITH_AUDIO
        Audio::AudioDevice::Get().SetPaused(bInPaused);
#else
        (void)bInPaused;
#endif
    }

    bool PlayTestTone(const float InHz, const float InSeconds)
    {
#if WITH_AUDIO
        return Audio::AudioDevice::Get().PlayTone(InHz, InSeconds);
#else
        (void)InHz;
        (void)InSeconds;
        return false;
#endif
    }

    bool PlaySound2D(GameCore::World& InWorld, const char* InSound, const float InVolume)
    {
#if WITH_AUDIO
        Audio::AudioWorld* Sound = Audio::AudioWorld::Find(InWorld);
        return Sound && Sound->PlaySound2D(InSound, InVolume);
#else
        (void)InWorld; (void)InSound; (void)InVolume;
        return false;
#endif
    }

    bool PlaySoundAt(GameCore::World& InWorld, const char* InSound, const EngineCore::Vector3& InPosition, const float InVolume)
    {
#if WITH_AUDIO
        Audio::AudioWorld* Sound = Audio::AudioWorld::Find(InWorld);
        return Sound && Sound->PlaySoundAt(InSound, InPosition, InVolume);
#else
        (void)InWorld; (void)InSound; (void)InPosition; (void)InVolume;
        return false;
#endif
    }

    bool PlaySoundAttached(GameCore::World& InWorld, const char* InSound, const GameCore::EntityID InEntity, const float InVolume)
    {
#if WITH_AUDIO
        Audio::AudioWorld* Sound = Audio::AudioWorld::Find(InWorld);
        return Sound && Sound->PlaySoundAttached(InSound, InEntity, InVolume);
#else
        (void)InWorld; (void)InSound; (void)InEntity; (void)InVolume;
        return false;
#endif
    }

    void StopAllSounds(GameCore::World& InWorld)
    {
#if WITH_AUDIO
        if(Audio::AudioWorld* Sound = Audio::AudioWorld::Find(InWorld)) { Sound->StopAll(); }
#else
        (void)InWorld;
#endif
    }

    void SetBusVolume(const char* InBus, const float InVolume)
    {
#if WITH_AUDIO
        Audio::EAudioBus Bus;
        if(Audio::FindBus(InBus, Bus)) { Audio::AudioDevice::Get().SetBusVolume(Bus, InVolume); }
#else
        (void)InBus; (void)InVolume;
#endif
    }

    float GetBusVolume(const char* InBus)
    {
#if WITH_AUDIO
        Audio::EAudioBus Bus;
        return Audio::FindBus(InBus, Bus) ? Audio::AudioDevice::Get().GetBusVolume(Bus) : 0.0f;
#else
        (void)InBus;
        return 0.0f;
#endif
    }

    void SetBusMuted(const char* InBus, const bool bInMuted)
    {
#if WITH_AUDIO
        Audio::EAudioBus Bus;
        if(Audio::FindBus(InBus, Bus)) { Audio::AudioDevice::Get().SetBusMuted(Bus, bInMuted); }
#else
        (void)InBus; (void)bInMuted;
#endif
    }
}

namespace
{
    /* The library's, as script calls: the World is the script's own. */
    bool ScriptPlaySound2D(GameCore::World& InWorld, const char* InSound, const float InVolume)
    {
        return NimbleEngine::PlaySound2D(InWorld, InSound, InVolume);
    }

    bool ScriptPlaySoundAt(GameCore::World& InWorld, const char* InSound, const EngineCore::Vector3& InPosition, const float InVolume)
    {
        return NimbleEngine::PlaySoundAt(InWorld, InSound, InPosition, InVolume);
    }

    bool ScriptPlaySoundAttached(GameCore::World& InWorld, const char* InSound, const GameCore::EntityID InTarget, const float InVolume)
    {
        return NimbleEngine::PlaySoundAttached(InWorld, InSound, InTarget, InVolume);
    }

    void ScriptStopAll(GameCore::World& InWorld)
    {
        NimbleEngine::StopAllSounds(InWorld);
    }

    bool ScriptIsAvailable()
    {
        return NimbleEngine::IsAudioAvailable();
    }
}

/*
 * The "Audio" script library: AudioFacade.h for Lua and script graphs.
 * Always registered; without the module every call does nothing (and the
 * plays answer false).
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Audio)
{
    Lib.NativePure<&ScriptIsAvailable>("IsAvailable", "-> Available")
       .Tooltip("Sound is compiled in (NIMBLE_WITH_AUDIO) and an output device is running");

    Lib.Native<&ScriptPlaySound2D>("PlaySound2D", "Sound, Volume -> Played")
       .Tooltip("Play an .nmsound once, the same in both ears wherever the listener is (UI, stingers)");
    Lib.Native<&ScriptPlaySoundAt>("PlaySoundAt", "Sound, Position, Volume -> Played")
       .Tooltip("Play an .nmsound once at a point, heard in 3D (impacts, explosions)");
    Lib.Native<&ScriptPlaySoundAttached>("PlaySoundAttached", "Sound, Target, Volume -> Played")
       .Tooltip("Play an .nmsound once, following an entity until it ends");
    Lib.Native<&ScriptStopAll>("StopAll")
       .Tooltip("Stop every sound in this world: Audio Sources and one-shots");

    Lib.Native<&NimbleEngine::SetBusVolume>("SetBusVolume", "Bus, Volume")
       .Tooltip("Master, Music, SFX, UI, Voice or Ambient, 0..1 (to 4 to amplify); saved with the player's settings");
    Lib.NativePure<&NimbleEngine::GetBusVolume>("GetBusVolume", "Bus -> Volume");
    Lib.Native<&NimbleEngine::SetBusMuted>("SetBusMuted", "Bus, Muted");

    Lib.Event("SoundFinished", "Source:Entity").Tooltip("An Audio Source's sound ended by itself (not looping, not stopped)");
}

/* "audio": the device's state; "audio tone [hz] [seconds]" to hear it. */
NIMBLE_CONSOLE_COMMAND(audio, "[tone [hz] [seconds]]", "Show the audio device, or play a test tone")
{
    if(!Console.Args.empty() && Console.Args[0] == "tone")
    {
        const float Hz      = Console.Args.size() > 1 ? float(std::atof(Console.Args[1].c_str())) : 440.0f;
        const float Seconds = Console.Args.size() > 2 ? float(std::atof(Console.Args[2].c_str())) : 1.0f;
        if(NimbleEngine::PlayTestTone(Hz, Seconds))
        {
            Console.Print("audio: " + std::to_string(int(Hz)) + " Hz tone");
        }
        else
        {
            Console.PrintError("audio: no output device");
        }
        return;
    }
#if WITH_AUDIO
    const Audio::AudioDevice& Device = Audio::AudioDevice::Get();
    if(!Device.IsInitialized())
    {
        Console.Print("audio: no output device");
        return;
    }
    Console.Print("audio: " + std::to_string(Device.GetSampleRate()) + " Hz, " + std::to_string(Device.GetChannels())
                  + " channel(s)" + (Device.IsOffline() ? ", offline" : "") + (Device.IsPaused() ? ", paused" : "")
                  + ", " + std::to_string(Device.GetPlayingVoiceCount()) + " voice(s)");
#else
    Console.Print("audio: not compiled in (NIMBLE_WITH_AUDIO=OFF)");
#endif
}

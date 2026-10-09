#include "Public/Audio/AudioWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Public/Audio/SoundAsset.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/AudioListenerComponent.h"
#include "Public/GameCore/Components/AudioSourceComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/World.h"

#include "Private/Audio/AudioInternal.h"

/* AudioWorld.h has the contract. */

/* [Audio] LogPlays: every one-shot logged as it starts -- what, where, and
 * how loud -- for finding why a sound is (not) heard. */
NIMBLE_CONFIG_BOOL(GAudioLogPlays, "Audio", "LogPlays", false, nullptr);

namespace Audio
{
    namespace
    {
        using EngineCore::Vector3;

        constexpr float DegToRad = 3.14159265358979f / 180.0f;

        /* How a voice plays, from its sound's settings or, with Override,
         * its source's. */
        struct PlaySettings
        {
            bool              bLoop    = false;
            bool              bSpatial = true;
            EAudioBus         Bus      = EAudioBus::SFX;
            float             MinDistance   = 100.0f;
            float             MaxDistance   = 5000.0f;
            float             Rolloff       = 1.0f;
            float             DopplerFactor = 1.0f;
            ESoundAttenuation Attenuation   = ESoundAttenuation::Inverse;
            float             ConeInner = 360.0f;
            float             ConeOuter = 360.0f;
            float             ConeOuterGain = 0.0f;
        };

        PlaySettings SettingsOf(const SoundData& InData, const GameCore::AudioSourceComponent* InSource)
        {
            const SoundSettings& S = InData.Settings;
            PlaySettings Out;
            Out.bLoop         = S.bLoop;
            Out.bSpatial      = S.bSpatial;
            Out.Bus           = S.Bus;
            Out.MinDistance   = S.MinDistance;
            Out.MaxDistance   = S.MaxDistance;
            Out.Rolloff       = S.Rolloff;
            Out.DopplerFactor = S.DopplerFactor;
            Out.Attenuation   = S.Attenuation;
            Out.ConeInner     = S.ConeInnerAngle;
            Out.ConeOuter     = S.ConeOuterAngle;
            Out.ConeOuterGain = S.ConeOuterGain;
            if(InSource && InSource->bOverride)
            {
                Out.bLoop         = InSource->bLoop != 0;
                Out.bSpatial      = InSource->bSpatial != 0;
                Out.Bus           = InSource->Bus < uint8_t(EAudioBus::Count) ? EAudioBus(InSource->Bus) : EAudioBus::SFX;
                Out.MinDistance   = InSource->MinDistance;
                Out.MaxDistance   = InSource->MaxDistance;
                Out.Rolloff       = InSource->Rolloff;
                Out.DopplerFactor = InSource->DopplerFactor;
            }
            if(InSource && InSource->ForceBus > 0 && InSource->ForceBus <= uint8_t(EAudioBus::Count))
            {
                Out.Bus = EAudioBus(InSource->ForceBus - 1);
            }
            if(InSource && InSource->ForceSpatial > 0)
            {
                Out.bSpatial = InSource->ForceSpatial == 2;
            }
            return Out;
        }

        ma_attenuation_model ToMiniaudio(const ESoundAttenuation InModel)
        {
            switch(InModel)
            {
                case ESoundAttenuation::None:        return ma_attenuation_model_none;
                case ESoundAttenuation::Linear:      return ma_attenuation_model_linear;
                case ESoundAttenuation::Exponential: return ma_attenuation_model_exponential;
                default:                             return ma_attenuation_model_inverse;
            }
        }

        float Finite(const float InValue, const float InFallback)
        {
            return std::isfinite(InValue) ? InValue : InFallback;
        }

        /* An entity's forward (+Z) and up (+Y) in the World. */
        void AxesOf(const GameCore::TransformPose& InPose, Vector3& OutForward, Vector3& OutUp)
        {
            const GameCore::Rotation3 Turn = GameCore::TransformMath::EulerToRotation(InPose.Rotation);
            OutForward = Turn * Vector3(0.0, 0.0, 1.0);
            OutUp      = Turn * Vector3(0.0, 1.0, 0.0);
        }

        /* A playing sound: its data kept alive while miniaudio reads it. */
        struct Voice
        {
            std::string                      Path;
            std::shared_ptr<const SoundData> Data;
            EAudioBus                        Bus = EAudioBus::SFX;

            ma_audio_buffer_ref Buffer;   // a Decompressed sound's samples, read in place
            ma_sound            Sound;
            bool                bBuffer = false;
            bool                bSound  = false;

            /* The spatializer's velocity, from the last frame's position. */
            Vector3 LastPosition;
            bool    bHasLast = false;

            ~Voice()
            {
                if(bSound)  { ma_sound_uninit(&Sound); }
                if(bBuffer) { ma_audio_buffer_ref_uninit(&Buffer); }
            }

            bool Init(AudioDevice::Impl& InDevice, const std::string& InPath, std::shared_ptr<const SoundData> InData,
                      const EAudioBus InBus)
            {
                Path = InPath;
                Data = std::move(InData);
                Bus  = InBus;
                const CookedSound& Cooked = Data->Cooked;

                ma_result Result = MA_SUCCESS;
                if(Cooked.Mode == ESoundLoadMode::Decompressed)
                {
                    Result = ma_audio_buffer_ref_init(ma_format_s16, Cooked.Channels, Cooked.Samples.data(), Cooked.Frames, &Buffer);
                    if(Result != MA_SUCCESS) { return false; }
                    Buffer.sampleRate = Cooked.SampleRate;
                    bBuffer = true;
                    Result = ma_sound_init_from_data_source(&InDevice.Engine, &Buffer, 0, InDevice.BusGroup(InBus), &Sound);
                }
                else
                {
                    /* Decoded (once, shared) or streamed by the loading threads. */
                    const std::string Name = InDevice.MountSource(InPath, SoundAssetCache::GetGeneration(), Data);
                    const ma_uint32 Flags = MA_SOUND_FLAG_ASYNC
                                          | (Cooked.Mode == ESoundLoadMode::Stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE);
                    Result = ma_sound_init_from_file(&InDevice.Engine, Name.c_str(), Flags, InDevice.BusGroup(InBus), nullptr, &Sound);
                }
                if(Result != MA_SUCCESS)
                {
                    CORE_LOG_WARN("[Audio] cannot play '" + InPath + "': " + ma_result_description(Result))
                    return false;
                }
                bSound = true;
                ma_sound_set_positioning(&Sound, ma_positioning_absolute);
                return true;
            }

            void Apply(const PlaySettings& InSettings, const float InVolume, const float InPitch)
            {
                ma_sound_set_volume(&Sound, std::clamp(Finite(InVolume, 1.0f), 0.0f, 16.0f));
                ma_sound_set_pitch(&Sound, std::clamp(Finite(InPitch, 1.0f), 0.01f, 8.0f));
                ma_sound_set_looping(&Sound, InSettings.bLoop ? MA_TRUE : MA_FALSE);
                ma_sound_set_spatialization_enabled(&Sound, InSettings.bSpatial ? MA_TRUE : MA_FALSE);
                if(!InSettings.bSpatial) { return; }
                const float MinDistance = std::max(Finite(InSettings.MinDistance, 100.0f), 0.0f);
                ma_sound_set_attenuation_model(&Sound, ToMiniaudio(InSettings.Attenuation));
                ma_sound_set_min_distance(&Sound, MinDistance);
                ma_sound_set_max_distance(&Sound, std::max(Finite(InSettings.MaxDistance, 5000.0f), MinDistance));
                ma_sound_set_rolloff(&Sound, std::max(Finite(InSettings.Rolloff, 1.0f), 0.0f));
                ma_sound_set_doppler_factor(&Sound, std::max(Finite(InSettings.DopplerFactor, 1.0f), 0.0f));
                ma_sound_set_cone(&Sound, std::clamp(InSettings.ConeInner, 0.0f, 360.0f) * DegToRad,
                                  std::clamp(InSettings.ConeOuter, 0.0f, 360.0f) * DegToRad,
                                  std::clamp(InSettings.ConeOuterGain, 0.0f, 1.0f));
            }

            void Place(const GameCore::TransformPose& InPose, const float InDeltaSeconds)
            {
                Vector3 Forward, Up;
                AxesOf(InPose, Forward, Up);
                Vector3 Velocity;
                if(bHasLast && InDeltaSeconds > 0.0f)
                {
                    Velocity = (InPose.Position - LastPosition) * (1.0 / double(InDeltaSeconds));
                }
                LastPosition = InPose.Position;
                bHasLast     = true;
                ma_sound_set_position(&Sound, float(InPose.Position.GetX()), float(InPose.Position.GetY()), float(InPose.Position.GetZ()));
                ma_sound_set_direction(&Sound, float(Forward.GetX()), float(Forward.GetY()), float(Forward.GetZ()));
                ma_sound_set_velocity(&Sound, float(Velocity.GetX()), float(Velocity.GetY()), float(Velocity.GetZ()));
            }

            void Restart()
            {
                ma_sound_seek_to_pcm_frame(&Sound, 0);
                ma_sound_start(&Sound);
            }

            bool IsPlaying() { return bSound && ma_sound_is_playing(&Sound); }
            bool AtEnd()     { return bSound && ma_sound_at_end(&Sound); }
        };

        /* An Audio Source's voice and what it was asked. */
        struct SourceState
        {
            std::unique_ptr<Voice> V;
            bool bSeen        = false;   // AutoPlay considered
            bool bWantPlaying = false;   // started and not stopped or ended
            bool bUserPaused  = false;
            float Volume = 0.0f;         // as last applied
            float Pitch  = 0.0f;
        };

        struct OneShot
        {
            std::unique_ptr<Voice> V;
            GameCore::EntityID     Follow;            // set: follows it
            bool                   bSpatial = false;
        };

        std::vector<std::pair<const GameCore::World*, AudioWorld*>>& Attached()
        {
            static std::vector<std::pair<const GameCore::World*, AudioWorld*>> Entries;
            return Entries;
        }
    }

    struct AudioWorld::Impl
    {
        GameCore::World* World = nullptr;

        ListenerFallbackFn Fallback         = nullptr;
        void*              FallbackUserData = nullptr;

        std::unordered_map<uint64_t, SourceState> Sources;   // by EntityID::Pack()
        std::deque<OneShot>                       OneShots;  // oldest first

        AudioListenerPose Listener;
        Vector3           LastListenerPosition;
        bool              bHasListener = false;

        bool bPaused = false;

        static void TickSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
        {
            (void)InWorld;
            static_cast<AudioWorld*>(InUserData)->Update(InDeltaSeconds);
        }

        void UpdateListener(AudioDevice::Impl& InDevice, const float InDeltaSeconds)
        {
            AudioListenerPose Pose;
            bool bFound = false;
            World->Each<GameCore::AudioListenerComponent>([&](GameCore::Entity InEntity, GameCore::AudioListenerComponent& InListener)
            {
                if(bFound || !InListener.bActive) { return; }
                const GameCore::TransformPose World = InEntity.GetWorldPose();
                Pose.Position = World.Position;
                AxesOf(World, Pose.Forward, Pose.Up);
                bFound = true;
            });
            if(!bFound && Fallback)
            {
                bFound = Fallback(*World, Pose, FallbackUserData);
            }

            Vector3 Velocity;
            if(bHasListener && InDeltaSeconds > 0.0f)
            {
                Velocity = (Pose.Position - LastListenerPosition) * (1.0 / double(InDeltaSeconds));
            }
            LastListenerPosition = Pose.Position;
            bHasListener         = true;
            Listener             = Pose;

            (void)InDevice;
            AudioDevice::Get().SetListener(Pose.Position, Pose.Forward, Pose.Up, Velocity);
        }

        void UpdateSources(AudioDevice::Impl& InDevice, const float InDeltaSeconds)
        {
            /* Voices whose component or entity is gone. */
            for(auto It = Sources.begin(); It != Sources.end();)
            {
                const GameCore::EntityID Entity = GameCore::EntityID::Unpack(It->first);
                if(!World->IsAlive(Entity) || !World->GetEntity(Entity).GetComponent<GameCore::AudioSourceComponent>())
                {
                    It = Sources.erase(It);
                }
                else
                {
                    ++It;
                }
            }

            GameCore::EventBus& Events = World->GetEvents();
            World->Each<GameCore::AudioSourceComponent>([&](GameCore::Entity InEntity, GameCore::AudioSourceComponent& InSource)
            {
                SourceState& State = Sources[InEntity.GetID().Pack()];

                /* What it asks for this frame. */
                const bool bAutoStart = !State.bSeen && InSource.bAutoPlay;
                State.bSeen = true;
                if(bAutoStart || InSource.bPendingPlay) { State.bWantPlaying = true; State.bUserPaused = false; }
                if(InSource.bPendingStop)               { State.bWantPlaying = false; State.bUserPaused = false; }
                if(InSource.bPendingPause && State.bWantPlaying)  { State.bUserPaused = true; }
                if(InSource.bPendingResume)                       { State.bUserPaused = false; }

                /* Its voice: made, or remade for a new sound (or a reimport). */
                const std::string Path(InSource.Sound, strnlen(InSource.Sound, sizeof(InSource.Sound)));
                std::shared_ptr<const SoundData> Data = Path.empty() ? nullptr : SoundAssetCache::Find(Path);
                const PlaySettings Settings = Data ? SettingsOf(*Data, &InSource) : PlaySettings();

                bool bFresh = false;
                if(!Data)
                {
                    State.V.reset();
                }
                else if(!State.V || State.V->Data != Data || State.V->Path != Path || State.V->Bus != Settings.Bus)
                {
                    State.V = std::make_unique<Voice>();
                    if(!State.V->Init(InDevice, Path, Data, Settings.Bus))
                    {
                        State.V.reset();
                    }
                    bFresh = true;
                }

                Voice* V = State.V.get();
                if(V)
                {
                    State.Volume = Data->Settings.Volume * InSource.Volume * Data->Cooked.Gain;
                    State.Pitch  = Data->Settings.Pitch * InSource.Pitch;
                    V->Apply(Settings, State.Volume, State.Pitch);
                    V->Place(InEntity.GetWorldPose(), InDeltaSeconds);

                    if(InSource.PendingSeekSeconds >= 0.0f)
                    {
                        ma_sound_seek_to_pcm_frame(&V->Sound, ma_uint64(double(InSource.PendingSeekSeconds) * Data->Cooked.SampleRate));
                    }
                    if(InSource.PendingFadeSeconds >= 0.0f)
                    {
                        ma_sound_set_fade_in_milliseconds(&V->Sound, -1.0f, InSource.PendingFadeGain,
                                                          ma_uint64(double(InSource.PendingFadeSeconds) * 1000.0));
                    }

                    const bool bShouldPlay = State.bWantPlaying && !State.bUserPaused && !bPaused;
                    if(InSource.bPendingPlay || (bFresh && bShouldPlay))
                    {
                        V->Restart();
                        if(!bShouldPlay) { ma_sound_stop(&V->Sound); }
                    }
                    else if(bShouldPlay && !V->IsPlaying() && !V->AtEnd())
                    {
                        ma_sound_start(&V->Sound);   // resumed
                    }
                    else if(!bShouldPlay && V->IsPlaying())
                    {
                        ma_sound_stop(&V->Sound);    // paused or stopped
                    }
                    if(InSource.bPendingStop)
                    {
                        ma_sound_seek_to_pcm_frame(&V->Sound, 0);
                    }

                    /* Ended by itself. */
                    if(State.bWantPlaying && !Settings.bLoop && V->AtEnd())
                    {
                        State.bWantPlaying = false;
                        State.bUserPaused  = false;
                        GameCore::EventArgs Args;
                        Args.Args[0] = GameCore::EventArg::Make(InEntity.GetID());
                        Args.Count   = 1;
                        Events.Emit("SoundFinished", Args);
                    }
                }

                InSource.bPendingPlay = InSource.bPendingStop = InSource.bPendingPause = InSource.bPendingResume = 0;
                InSource.PendingFadeSeconds = -1.0f;
                InSource.PendingSeekSeconds = -1.0f;

                InSource.bPlaying        = V && State.bWantPlaying && !State.bUserPaused ? 1 : 0;
                InSource.bPaused         = V && State.bWantPlaying && State.bUserPaused ? 1 : 0;
                InSource.PlaybackSeconds = 0.0f;
                InSource.LengthSeconds   = Data ? float(Data->Cooked.GetDuration()) : 0.0f;
                if(V)
                {
                    float Cursor = 0.0f;
                    ma_sound_get_cursor_in_seconds(&V->Sound, &Cursor);
                    InSource.PlaybackSeconds = Cursor;
                }
            });
        }

        void UpdateOneShots(const float InDeltaSeconds)
        {
            for(auto It = OneShots.begin(); It != OneShots.end();)
            {
                Voice& V = *It->V;
                if(V.AtEnd())
                {
                    It = OneShots.erase(It);
                    continue;
                }
                if(It->Follow.IsSet())
                {
                    if(!World->IsAlive(It->Follow))
                    {
                        It = OneShots.erase(It);
                        continue;
                    }
                    V.Place(World->GetEntity(It->Follow).GetWorldPose(), InDeltaSeconds);
                }
                ++It;
            }
        }

        /* [Audio] LogPlays: InWhat ("play", "cull", "drop") InSound. */
        static void LogPlay(const char* InWhat, const char* InSound, const PlaySettings& InSettings, const Vector3& InAt,
                            const double InDistance, const float InVolume)
        {
            if(!GAudioLogPlays.GetBool()) { return; }
            char Line[320];
            std::snprintf(Line, sizeof(Line), "[Audio] %s '%s' %s at %.0f %.0f %.0f, %.0f from the listener, volume %.2f",
                          InWhat, InSound, InSettings.bSpatial ? "3D" : "2D", double(InAt.GetX()), double(InAt.GetY()),
                          double(InAt.GetZ()), InDistance, double(InVolume));
            CORE_LOG(Line)
        }

        bool PlayOneShot(const char* InSound, const bool bInSpatial, const GameCore::TransformPose* InAt,
                         const GameCore::EntityID InFollow, const float InVolume)
        {
            AudioDevice::Impl* Device = GetDeviceImpl();
            if(!Device || !InSound || !*InSound) { return false; }
            std::shared_ptr<const SoundData> Data = SoundAssetCache::Find(InSound);
            if(!Data) { return false; }

            PlaySettings Settings = SettingsOf(*Data, nullptr);
            Settings.bLoop = false;   // a one-shot that looped would never be freed
            if(!bInSpatial) { Settings.bSpatial = false; }

            /* Where it plays, to weigh it against the others: a 3D sound
             * beyond its MaxDistance is not started -- a herd's far
             * footsteps would only push out the near ones. */
            Vector3 At = InAt ? InAt->Position : Vector3();
            if(!InAt && InFollow.IsSet() && World->IsAlive(InFollow)) { At = World->GetEntity(InFollow).GetWorldPose().Position; }
            const double Distance = Settings.bSpatial ? double((At - Listener.Position).GetLength()) : 0.0;
            if(Settings.bSpatial && Distance > double(Settings.MaxDistance))
            {
                LogPlay("cull", InSound, Settings, At, Distance, InVolume);
                return false;
            }

            /* Full: the farthest makes way for a nearer one; else this one is
             * dropped. 2D one-shots count as at the listener. */
            if(OneShots.size() >= MaxOneShots)
            {
                auto Farthest = OneShots.end();
                double FarthestDistance = -1.0;
                for(auto It = OneShots.begin(); It != OneShots.end(); ++It)
                {
                    const double Each = It->bSpatial && It->V->bHasLast
                                      ? double((It->V->LastPosition - Listener.Position).GetLength()) : 0.0;
                    if(Each > FarthestDistance) { FarthestDistance = Each; Farthest = It; }
                }
                if(Farthest == OneShots.end() || Distance >= FarthestDistance)
                {
                    LogPlay("drop", InSound, Settings, At, Distance, InVolume);
                    return false;
                }
                OneShots.erase(Farthest);
            }

            OneShot Shot;
            Shot.V = std::make_unique<Voice>();
            if(!Shot.V->Init(*Device, InSound, Data, Settings.Bus)) { return false; }
            Shot.Follow   = InFollow;
            Shot.bSpatial = Settings.bSpatial;
            Shot.V->Apply(Settings, Data->Settings.Volume * InVolume * Data->Cooked.Gain, Data->Settings.Pitch);
            if(InAt) { Shot.V->Place(*InAt, 0.0f); }
            else if(InFollow.IsSet() && World->IsAlive(InFollow)) { Shot.V->Place(World->GetEntity(InFollow).GetWorldPose(), 0.0f); }
            ma_sound_start(&Shot.V->Sound);
            if(bPaused) { ma_sound_stop(&Shot.V->Sound); }
            LogPlay("play", InSound, Settings, At, Distance, InVolume);
            OneShots.push_back(std::move(Shot));
            return true;
        }
    };

    AudioWorld::AudioWorld()
        : P(new Impl())
    {
    }

    AudioWorld::~AudioWorld()
    {
        Detach();
        delete P;
    }

    bool AudioWorld::Attach(GameCore::World& InWorld)
    {
        if(P->World)
        {
            CORE_LOG_WARN("[Audio] AudioWorld::Attach: already attached")
            return false;
        }
        if(!AudioDevice::Get().IsInitialized())
        {
            static bool bWarned = false;
            if(!bWarned)
            {
                CORE_LOG_WARN("[Audio] no output device: Worlds will be silent")
                bWarned = true;
            }
            return false;
        }
        if(Find(InWorld))
        {
            CORE_LOG_WARN("[Audio] AudioWorld::Attach: that World already has audio")
            return false;
        }
        P->World = &InWorld;
        InWorld.RegisterSystem("Audio.Update", GameCore::TP_PostUpdate, &Impl::TickSystem, this);
        Attached().emplace_back(&InWorld, this);
        return true;
    }

    void AudioWorld::Detach()
    {
        if(!P->World) { return; }
        P->World->UnregisterSystem(&Impl::TickSystem, this);
        P->Sources.clear();
        P->OneShots.clear();
        auto& Entries = Attached();
        Entries.erase(std::remove_if(Entries.begin(), Entries.end(),
                                     [this](const auto& InEntry) { return InEntry.second == this; }), Entries.end());
        P->World        = nullptr;
        P->bHasListener = false;
        P->bPaused      = false;
    }

    bool AudioWorld::IsAttached() const
    {
        return P->World != nullptr;
    }

    GameCore::World* AudioWorld::GetWorld() const
    {
        return P->World;
    }

    void AudioWorld::SetListenerFallback(const ListenerFallbackFn InFn, void* InUserData)
    {
        P->Fallback         = InFn;
        P->FallbackUserData = InUserData;
    }

    void AudioWorld::Update(const float InDeltaSeconds)
    {
        AudioDevice::Impl* Device = GetDeviceImpl();
        if(!P->World || !Device) { return; }
        P->UpdateListener(*Device, InDeltaSeconds);
        P->UpdateSources(*Device, InDeltaSeconds);
        P->UpdateOneShots(InDeltaSeconds);
    }

    void AudioWorld::SetPaused(const bool bInPaused)
    {
        if(P->bPaused == bInPaused) { return; }
        P->bPaused = bInPaused;
        /* Sources follow at the next Update (bShouldPlay); one-shots now. */
        for(OneShot& Shot : P->OneShots)
        {
            if(bInPaused) { ma_sound_stop(&Shot.V->Sound); }
            else if(!Shot.V->AtEnd()) { ma_sound_start(&Shot.V->Sound); }
        }
        if(bInPaused)
        {
            for(auto& Entry : P->Sources)
            {
                if(Entry.second.V) { ma_sound_stop(&Entry.second.V->Sound); }
            }
        }
    }

    bool AudioWorld::IsPaused() const
    {
        return P->bPaused;
    }

    bool AudioWorld::PlaySound2D(const char* InSound, const float InVolume)
    {
        return P->World && P->PlayOneShot(InSound, false, nullptr, GameCore::InvalidEntityID(), InVolume);
    }

    bool AudioWorld::PlaySoundAt(const char* InSound, const EngineCore::Vector3& InPosition, const float InVolume)
    {
        GameCore::TransformPose At;
        At.Position = InPosition;
        return P->World && P->PlayOneShot(InSound, true, &At, GameCore::InvalidEntityID(), InVolume);
    }

    bool AudioWorld::PlaySoundAttached(const char* InSound, const GameCore::EntityID InEntity, const float InVolume)
    {
        return P->World && InEntity.IsSet() && P->World->IsAlive(InEntity)
               && P->PlayOneShot(InSound, true, nullptr, InEntity, InVolume);
    }

    void AudioWorld::StopAll()
    {
        P->OneShots.clear();
        for(auto& Entry : P->Sources)
        {
            Entry.second.bWantPlaying = false;
            Entry.second.bUserPaused  = false;
            if(Entry.second.V)
            {
                ma_sound_stop(&Entry.second.V->Sound);
                ma_sound_seek_to_pcm_frame(&Entry.second.V->Sound, 0);
            }
        }
    }

    void AudioWorld::GetStats(AudioWorldStats& OutStats) const
    {
        OutStats = AudioWorldStats();
        for(auto& Entry : P->Sources)
        {
            if(!Entry.second.V) { continue; }
            ++OutStats.Sources;
            if(Entry.second.V->IsPlaying()) { ++OutStats.Playing; }
        }
        for(OneShot& Shot : P->OneShots)
        {
            if(Shot.V->IsPlaying()) { ++OutStats.OneShots; }
        }
    }

    bool AudioWorld::GetVoiceState(const GameCore::EntityID InEntity, AudioVoiceState& OutState) const
    {
        OutState = AudioVoiceState();
        const auto Found = P->Sources.find(InEntity.Pack());
        if(Found == P->Sources.end() || !Found->second.V) { return false; }
        Voice& V = *Found->second.V;
        OutState.bPlaying = V.IsPlaying();
        OutState.Volume   = Found->second.Volume;
        OutState.Pitch    = Found->second.Pitch;
        ma_sound_get_cursor_in_seconds(&V.Sound, &OutState.Seconds);
        return true;
    }

    AudioListenerPose AudioWorld::GetListener() const
    {
        return P->Listener;
    }

    AudioWorld* AudioWorld::Find(const GameCore::World& InWorld)
    {
        for(const auto& Entry : Attached())
        {
            if(Entry.first == &InWorld) { return Entry.second; }
        }
        return nullptr;
    }

    bool PlayPreview(const char* InSound)
    {
        AudioDevice::Impl* Device = GetDeviceImpl();
        if(!Device || !InSound || !*InSound) { return false; }
        Device->Preview.reset();

        std::shared_ptr<const SoundData> Data = SoundAssetCache::Find(InSound);
        if(!Data) { return false; }

        PlaySettings Settings = SettingsOf(*Data, nullptr);
        Settings.bSpatial = false;
        auto Preview = std::make_shared<Voice>();
        if(!Preview->Init(*Device, InSound, Data, Settings.Bus)) { return false; }
        Preview->Apply(Settings, Data->Settings.Volume * Data->Cooked.Gain, Data->Settings.Pitch);
        ma_sound_start(&Preview->Sound);
        Device->Preview = std::move(Preview);
        return true;
    }

    void StopPreview()
    {
        if(AudioDevice::Impl* Device = GetDeviceImpl()) { Device->Preview.reset(); }
    }

    const char* GetPreviewSound()
    {
        AudioDevice::Impl* Device = GetDeviceImpl();
        Voice* Preview = Device ? static_cast<Voice*>(Device->Preview.get()) : nullptr;
        if(!Preview || Preview->AtEnd()) { return ""; }
        return Preview->Path.c_str();
    }
}

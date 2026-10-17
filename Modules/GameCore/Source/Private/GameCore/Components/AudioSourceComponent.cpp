#include "Public/GameCore/Components/AudioSourceComponent.h"

#include <cstring>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* AudioSourceComponent: registration, its methods, and what it exposes to
 * saves, Lua and the editor. The playing is Engine/Modules/Audio. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(AudioSourceComponent, "Audio Source")

    namespace
    {
        constexpr const char* GBusNames[] = { "Master", "Music", "SFX", "UI", "Voice", "Ambient" };
        constexpr size_t      GBusCount   = sizeof(GBusNames) / sizeof(GBusNames[0]);
    }

    void AudioSourceComponent::Play()
    {
        bPendingPlay = 1;
        bPendingStop = 0;
        bPendingPause = 0;
        bPendingResume = 0;
    }

    void AudioSourceComponent::Stop()
    {
        bPendingStop = 1;
        bPendingPlay = 0;
        bPendingPause = 0;
        bPendingResume = 0;
    }

    void AudioSourceComponent::Pause()
    {
        bPendingPause  = 1;
        bPendingResume = 0;
    }

    void AudioSourceComponent::Resume()
    {
        bPendingResume = 1;
        bPendingPause  = 0;
    }

    void AudioSourceComponent::FadeTo(const float InGain, const float InSeconds)
    {
        PendingFadeGain    = InGain < 0.0f ? 0.0f : (InGain > 1.0f ? 1.0f : InGain);
        PendingFadeSeconds = InSeconds < 0.0f ? 0.0f : InSeconds;
    }

    void AudioSourceComponent::Seek(const float InSeconds)
    {
        PendingSeekSeconds = InSeconds < 0.0f ? 0.0f : InSeconds;
    }

    void AudioSourceComponent::SetSound(const char* InPath)
    {
        std::memset(Sound, 0, sizeof(Sound));
        if(InPath) { std::strncpy(Sound, InPath, sizeof(Sound) - 1); }
    }

    const char* AudioSourceComponent::GetBusName() const
    {
        return Bus < GBusCount ? GBusNames[Bus] : "SFX";
    }

    void AudioSourceComponent::SetBusName(const char* InName)
    {
        if(!InName) { return; }
        for(size_t Index = 0; Index < GBusCount; ++Index)
        {
            if(std::strcmp(InName, GBusNames[Index]) == 0) { Bus = uint8_t(Index); }
        }
    }

    /* The authored fields; requests and playback state are this run's. */
    /* Version 2: ForceBus, ForceSpatial. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(AudioSourceComponent, 2)
    {
        Ar.Field("Sound",         C.Sound);
        Ar.Field("Volume",        C.Volume);
        Ar.Field("Pitch",         C.Pitch);
        Ar.Flag("AutoPlay",       C.bAutoPlay);
        Ar.Flag("Override",       C.bOverride);
        Ar.Flag("Loop",           C.bLoop);
        Ar.Flag("Spatial",        C.bSpatial);
        Ar.Field("Bus",           C.Bus);
        Ar.Field("MinDistance",   C.MinDistance);
        Ar.Field("MaxDistance",   C.MaxDistance);
        Ar.Field("Rolloff",       C.Rolloff);
        Ar.Field("DopplerFactor", C.DopplerFactor);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("ForceBus",     C.ForceBus);
            Ar.Field("ForceSpatial", C.ForceSpatial);
        }
    }

    /* Get/Set for the authored fields; Play, Stop, Pause, Resume, FadeTo(Gain,
     * Seconds), Seek(Seconds); IsPlaying, IsPaused, GetPlaybackSeconds,
     * GetLengthSeconds as of the last frame; GetBus / SetBus by name. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(AudioSourceComponent)
    {
        Bind.Field("Sound",         &AudioSourceComponent::Sound);
        Bind.Field("Volume",        &AudioSourceComponent::Volume);
        Bind.Field("Pitch",         &AudioSourceComponent::Pitch);
        Bind.Flag("AutoPlay",       &AudioSourceComponent::bAutoPlay);
        Bind.Flag("Override",       &AudioSourceComponent::bOverride);
        Bind.Flag("Loop",           &AudioSourceComponent::bLoop);
        Bind.Flag("Spatial",        &AudioSourceComponent::bSpatial);
        Bind.Field("MinDistance",   &AudioSourceComponent::MinDistance);
        Bind.Field("MaxDistance",   &AudioSourceComponent::MaxDistance);
        Bind.Field("Rolloff",       &AudioSourceComponent::Rolloff);
        Bind.Field("DopplerFactor", &AudioSourceComponent::DopplerFactor);
        Bind.Field("ForceBus",      &AudioSourceComponent::ForceBus);
        Bind.Field("ForceSpatial",  &AudioSourceComponent::ForceSpatial);

        Bind.Native<&AudioSourceComponent::Play>("Play");
        Bind.Native<&AudioSourceComponent::Stop>("Stop");
        Bind.Native<&AudioSourceComponent::Pause>("Pause");
        Bind.Native<&AudioSourceComponent::Resume>("Resume");
        Bind.Native<&AudioSourceComponent::FadeTo>("FadeTo", "Gain, Seconds");
        Bind.Native<&AudioSourceComponent::Seek>("Seek", "Seconds");
        Bind.NativePure<&AudioSourceComponent::IsPlaying>("IsPlaying", "-> Playing");
        Bind.NativePure<&AudioSourceComponent::IsPaused>("IsPaused", "-> Paused");
        Bind.NativePure<&AudioSourceComponent::GetPlaybackSeconds>("GetPlaybackSeconds", "-> Seconds");
        Bind.NativePure<&AudioSourceComponent::GetLengthSeconds>("GetLengthSeconds", "-> Seconds");
        Bind.NativePure<&AudioSourceComponent::GetBusName>("GetBus", "-> Bus");
        Bind.Native<&AudioSourceComponent::SetBusName>("SetBus", "Bus");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(AudioSourceComponent)
    {
        Props.TypeCategory("Audio");
        Props.Field("Sound",     &AudioSourceComponent::Sound, PF_AssetSound);
        Props.Field("Volume",    &AudioSourceComponent::Volume);
        Props.Field("Pitch",     &AudioSourceComponent::Pitch);
        Props.Flag("Auto Play",  &AudioSourceComponent::bAutoPlay);
        Props.Choice("Force Bus",     &AudioSourceComponent::ForceBus, "The Sound's|Master|Music|SFX|UI|Voice|Ambient");
        Props.Choice("Force 2D / 3D", &AudioSourceComponent::ForceSpatial, "The Sound's|2D|3D");
        Props.Category("Override");
        Props.Flag("Override",   &AudioSourceComponent::bOverride);
        Props.Flag("Loop",       &AudioSourceComponent::bLoop);
        Props.Flag("Spatial",    &AudioSourceComponent::bSpatial);
        Props.Choice("Bus",      &AudioSourceComponent::Bus, "Master|Music|SFX|UI|Voice|Ambient");   // EAudioSourceBus
        Props.Field("Min Distance",   &AudioSourceComponent::MinDistance);
        Props.Field("Max Distance",   &AudioSourceComponent::MaxDistance);
        Props.Field("Rolloff",        &AudioSourceComponent::Rolloff);
        Props.Field("Doppler Factor", &AudioSourceComponent::DopplerFactor);
        Props.Category("State");
        Props.Flag("Playing",          &AudioSourceComponent::bPlaying, PF_ReadOnly);
        Props.Field("Playback Seconds", &AudioSourceComponent::PlaybackSeconds, PF_ReadOnly);
        Props.Field("Length Seconds",   &AudioSourceComponent::LengthSeconds, PF_ReadOnly);
    }
#endif // WITH_EDITOR
}

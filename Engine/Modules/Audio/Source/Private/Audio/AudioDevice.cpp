#include "Public/Audio/AudioDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"

#include "Public/Audio/SoundAsset.h"

#include "Private/Audio/AudioInternal.h"

namespace
{
    void OnBusVolumeChanged(const EngineCore::ConfigVar& InVar);
}

/* [Audio]: the device (read when it starts) and the bus volumes (applied as
 * they change, and saved with the player's settings). README.md. */
NIMBLE_CONFIG_BOOL(GAudioOffline, "Audio", "Offline", false, nullptr);             // no output device: silent, mixed only on demand
NIMBLE_CONFIG_INT(GAudioSampleRate, "Audio", "SampleRate", 48000, nullptr);        // 0: the device's own
NIMBLE_CONFIG_INT(GAudioChannels, "Audio", "Channels", 0, nullptr);                // 0: the device's own
NIMBLE_CONFIG_FLOAT(GAudioUnitsPerMeter, "Audio", "UnitsPerMeter", 100.0f, nullptr);
NIMBLE_CONFIG_INT(GAudioLoadThreads, "Audio", "LoadThreads", 2, nullptr);

NIMBLE_CONFIG_FLOAT(GAudioMasterVolume, "Audio", "MasterVolume", 1.0f, &OnBusVolumeChanged);
NIMBLE_CONFIG_FLOAT(GAudioMusicVolume, "Audio", "MusicVolume", 1.0f, &OnBusVolumeChanged);
NIMBLE_CONFIG_FLOAT(GAudioSFXVolume, "Audio", "SFXVolume", 1.0f, &OnBusVolumeChanged);
NIMBLE_CONFIG_FLOAT(GAudioUIVolume, "Audio", "UIVolume", 1.0f, &OnBusVolumeChanged);
NIMBLE_CONFIG_FLOAT(GAudioVoiceVolume, "Audio", "VoiceVolume", 1.0f, &OnBusVolumeChanged);
NIMBLE_CONFIG_FLOAT(GAudioAmbientVolume, "Audio", "AmbientVolume", 1.0f, &OnBusVolumeChanged);

namespace
{
    using Audio::EAudioBus;

    constexpr const char* GBusNames[size_t(EAudioBus::Count)] = { "Master", "Music", "SFX", "UI", "Voice", "Ambient" };

    EngineCore::ConfigVar& BusVolumeVar(const EAudioBus InBus)
    {
        switch(InBus)
        {
            case EAudioBus::Music:   return GAudioMusicVolume;
            case EAudioBus::SFX:     return GAudioSFXVolume;
            case EAudioBus::UI:      return GAudioUIVolume;
            case EAudioBus::Voice:   return GAudioVoiceVolume;
            case EAudioBus::Ambient: return GAudioAmbientVolume;
            default:                 return GAudioMasterVolume;
        }
    }

    float ClampVolume(const float InVolume)
    {
        return std::isfinite(InVolume) ? std::clamp(InVolume, 0.0f, 4.0f) : 1.0f;
    }

    void OnBusVolumeChanged(const EngineCore::ConfigVar& InVar)
    {
        Audio::AudioDevice::Impl* P = Audio::GetDeviceImpl();
        if(!P) { return; }
        for(size_t Index = 0; Index < size_t(EAudioBus::Count); ++Index)
        {
            if(&BusVolumeVar(EAudioBus(Index)) == &InVar)
            {
                P->ApplyBus(EAudioBus(Index));
                return;
            }
        }
    }

    bool ValidBus(const EAudioBus InBus)
    {
        return InBus < EAudioBus::Count;
    }
}

namespace Audio
{
    const char* GetBusName(const EAudioBus InBus)
    {
        return ValidBus(InBus) ? GBusNames[size_t(InBus)] : "";
    }

    bool FindBus(const char* InName, EAudioBus& OutBus)
    {
        if(!InName) { return false; }
        const std::string Name(InName);
        for(size_t Index = 0; Index < size_t(EAudioBus::Count); ++Index)
        {
            const std::string Candidate(GBusNames[Index]);
            if(Candidate.size() == Name.size()
               && std::equal(Name.begin(), Name.end(), Candidate.begin(),
                             [](const char A, const char B) { return std::tolower((unsigned char)A) == std::tolower((unsigned char)B); }))
            {
                OutBus = EAudioBus(Index);
                return true;
            }
        }
        return false;
    }

    AudioDeviceSettings AudioDeviceSettings::FromConfig()
    {
        AudioDeviceSettings Settings;
        Settings.bOffline      = GAudioOffline.GetBool();
        Settings.SampleRate    = uint32_t(std::clamp(GAudioSampleRate.GetInt(), 0, 384000));
        Settings.Channels      = uint32_t(std::clamp(GAudioChannels.GetInt(), 0, 8));
        Settings.UnitsPerMeter = GAudioUnitsPerMeter.GetFloat() > 0.0f ? GAudioUnitsPerMeter.GetFloat() : 100.0f;
        Settings.LoadThreads   = uint32_t(std::clamp(GAudioLoadThreads.GetInt(), 1, 8));
        return Settings;
    }

    namespace
    {
        /* An open source: what it reads, and where it is. */
        struct VfsFile
        {
            std::shared_ptr<const SoundData> Data;
            size_t                           Cursor = 0;
        };

        SoundVfs& VfsOf(ma_vfs* InVfs) { return *static_cast<SoundVfs*>(InVfs); }

        ma_result VfsOpen(ma_vfs* InVfs, const char* InPath, const ma_uint32 InMode, ma_vfs_file* OutFile)
        {
            if(!InPath || !OutFile || (InMode & MA_OPEN_MODE_WRITE) != 0) { return MA_INVALID_ARGS; }
            SoundVfs& Vfs = VfsOf(InVfs);
            std::lock_guard<std::mutex> Guard(Vfs.Lock);
            const auto Found = Vfs.Sources.find(InPath);
            if(Found == Vfs.Sources.end()) { return MA_DOES_NOT_EXIST; }
            *OutFile = new VfsFile{ Found->second, 0 };
            return MA_SUCCESS;
        }

        ma_result VfsOpenW(ma_vfs*, const wchar_t*, ma_uint32, ma_vfs_file*)
        {
            return MA_NOT_IMPLEMENTED;   // every name is ASCII (MountSource)
        }

        ma_result VfsClose(ma_vfs*, ma_vfs_file InFile)
        {
            delete static_cast<VfsFile*>(InFile);
            return MA_SUCCESS;
        }

        ma_result VfsRead(ma_vfs*, ma_vfs_file InFile, void* OutBytes, const size_t InSize, size_t* OutRead)
        {
            auto& File = *static_cast<VfsFile*>(InFile);
            const std::vector<uint8_t>& Source = File.Data->Source;
            const size_t Count = std::min(InSize, Source.size() - std::min(File.Cursor, Source.size()));
            if(Count > 0) { std::memcpy(OutBytes, Source.data() + File.Cursor, Count); }
            File.Cursor += Count;
            if(OutRead) { *OutRead = Count; }
            return Count == 0 && InSize > 0 ? MA_AT_END : MA_SUCCESS;
        }

        ma_result VfsWrite(ma_vfs*, ma_vfs_file, const void*, size_t, size_t*)
        {
            return MA_NOT_IMPLEMENTED;
        }

        ma_result VfsSeek(ma_vfs*, ma_vfs_file InFile, const ma_int64 InOffset, const ma_seek_origin InOrigin)
        {
            auto& File = *static_cast<VfsFile*>(InFile);
            const ma_int64 Size = ma_int64(File.Data->Source.size());
            ma_int64 Base = 0;
            if(InOrigin == ma_seek_origin_current) { Base = ma_int64(File.Cursor); }
            if(InOrigin == ma_seek_origin_end)     { Base = Size; }
            const ma_int64 Target = Base + InOffset;
            if(Target < 0 || Target > Size) { return MA_BAD_SEEK; }
            File.Cursor = size_t(Target);
            return MA_SUCCESS;
        }

        ma_result VfsTell(ma_vfs*, ma_vfs_file InFile, ma_int64* OutCursor)
        {
            *OutCursor = ma_int64(static_cast<VfsFile*>(InFile)->Cursor);
            return MA_SUCCESS;
        }

        ma_result VfsInfo(ma_vfs*, ma_vfs_file InFile, ma_file_info* OutInfo)
        {
            OutInfo->sizeInBytes = ma_uint64(static_cast<VfsFile*>(InFile)->Data->Source.size());
            return MA_SUCCESS;
        }
    }

    SoundVfs::SoundVfs()
    {
        Callbacks.onOpen  = &VfsOpen;
        Callbacks.onOpenW = &VfsOpenW;
        Callbacks.onClose = &VfsClose;
        Callbacks.onRead  = &VfsRead;
        Callbacks.onWrite = &VfsWrite;
        Callbacks.onSeek  = &VfsSeek;
        Callbacks.onTell  = &VfsTell;
        Callbacks.onInfo  = &VfsInfo;
    }

    std::string AudioDevice::Impl::MountSource(const std::string& InPath, const uint64_t InGeneration,
                                               const std::shared_ptr<const SoundData>& InData)
    {
        std::string Name = "nmsound:" + std::to_string(InGeneration) + ":" + InPath;
        std::lock_guard<std::mutex> Guard(Vfs.Lock);
        Vfs.Sources.emplace(Name, InData);
        return Name;
    }

    void AudioDevice::Impl::ApplyBus(const EAudioBus InBus)
    {
        const float Volume = bBusMuted[size_t(InBus)] ? 0.0f : ClampVolume(BusVolumeVar(InBus).GetFloat());
        if(InBus == EAudioBus::Master)
        {
            ma_engine_set_volume(&Engine, Volume);
        }
        else if(ma_sound_group* Group = BusGroup(InBus))
        {
            ma_sound_group_set_volume(Group, Volume);
        }
    }

    void AudioDevice::Impl::ReapTones()
    {
        for(auto It = Tones.begin(); It != Tones.end();)
        {
            ToneVoice& Tone = **It;
            if(Tone.bInitialized && ma_sound_at_end(&Tone.Sound))
            {
                ma_sound_uninit(&Tone.Sound);
                ma_waveform_uninit(&Tone.Wave);
                It = Tones.erase(It);
            }
            else
            {
                ++It;
            }
        }
    }

    AudioDevice& AudioDevice::Get()
    {
        /* Function-local, so it exists whenever first asked for; Shutdown()
         * does the real teardown, while every thread is still alive. */
        static AudioDevice Instance;
        return Instance;
    }

    AudioDevice::~AudioDevice()
    {
        Shutdown();
    }

    bool AudioDevice::Initialize(const AudioDeviceSettings& InSettings)
    {
        if(P) { return true; }

        P = EngineCore::Memory::Alloc<Impl>();
        P->Settings = InSettings;

        /* Decoded to the mix's format, so a decoded sound never converts as it
         * plays. Its job threads decode and stream (README.md, Threads). */
        ma_resource_manager_config ResourceConfig = ma_resource_manager_config_init();
        ResourceConfig.decodedFormat     = ma_format_f32;
        ResourceConfig.decodedSampleRate = InSettings.SampleRate;
        ResourceConfig.jobThreadCount    = InSettings.LoadThreads;
        ResourceConfig.pVFS              = &P->Vfs;

        ma_result Result = ma_resource_manager_init(&ResourceConfig, &P->ResourceManager);
        if(Result != MA_SUCCESS)
        {
            CORE_LOG_ERROR(std::string("[Audio] could not start the resource manager: ") + ma_result_description(Result))
            EngineCore::Memory::Dealloc(P);
            P = nullptr;
            return false;
        }

        ma_engine_config EngineConfig = ma_engine_config_init();
        EngineConfig.pResourceManager = &P->ResourceManager;
        EngineConfig.listenerCount    = 1;
        EngineConfig.sampleRate       = InSettings.SampleRate;
        EngineConfig.channels         = InSettings.Channels;
        if(InSettings.bOffline)
        {
            /* No device: Render() pulls the mix. noDevice needs both set. */
            EngineConfig.noDevice   = MA_TRUE;
            EngineConfig.channels   = InSettings.Channels != 0 ? InSettings.Channels : 2;
            EngineConfig.sampleRate = InSettings.SampleRate != 0 ? InSettings.SampleRate : 48000;
        }

        Result = ma_engine_init(&EngineConfig, &P->Engine);
        if(Result != MA_SUCCESS)
        {
            CORE_LOG_WARN(std::string("[Audio] no output device (") + ma_result_description(Result) + "); the game will be silent")
            ma_resource_manager_uninit(&P->ResourceManager);
            EngineCore::Memory::Dealloc(P);
            P = nullptr;
            return false;
        }

        /* The World's axes: left-handed, +Y up, +Z forward (RenderView.h).
         * Doppler at the speed of sound in world units. miniaudio has no
         * setter for handedness; the mix reads the field each time, and the
         * direction it flipped at init is set below anyway. */
        ma_spatializer_listener* Listener = &P->Engine.listeners[0];
        Listener->config.handedness = ma_handedness_left;
        ma_spatializer_listener_set_speed_of_sound(Listener, 343.3f * InSettings.UnitsPerMeter);
        ma_engine_listener_set_world_up(&P->Engine, 0, 0.0f, 1.0f, 0.0f);
        ma_engine_listener_set_position(&P->Engine, 0, 0.0f, 0.0f, 0.0f);
        ma_engine_listener_set_direction(&P->Engine, 0, 0.0f, 0.0f, 1.0f);

        for(size_t Index = size_t(EAudioBus::Master) + 1; Index < size_t(EAudioBus::Count); ++Index)
        {
            Result = ma_sound_group_init(&P->Engine, 0, nullptr, &P->Buses[Index]);
            if(Result != MA_SUCCESS)
            {
                CORE_LOG_ERROR(std::string("[Audio] could not make the ") + GBusNames[Index] + " bus: " + ma_result_description(Result))
                for(size_t Made = size_t(EAudioBus::Master) + 1; Made < Index; ++Made)
                {
                    ma_sound_group_uninit(&P->Buses[Made]);
                }
                ma_engine_uninit(&P->Engine);
                ma_resource_manager_uninit(&P->ResourceManager);
                EngineCore::Memory::Dealloc(P);
                P = nullptr;
                return false;
            }
        }
        for(size_t Index = 0; Index < size_t(EAudioBus::Count); ++Index)
        {
            P->ApplyBus(EAudioBus(Index));
        }

        if(InSettings.bOffline)
        {
            CORE_LOG("[Audio] offline, " + std::to_string(GetSampleRate()) + " Hz, " + std::to_string(GetChannels()) + " channel(s)")
        }
        else
        {
            ma_device* Device = ma_engine_get_device(&P->Engine);
            CORE_LOG("[Audio] '" + std::string(Device ? Device->playback.name : "?") + "', "
                     + std::to_string(GetSampleRate()) + " Hz, " + std::to_string(GetChannels()) + " channel(s), "
                     + (Device ? ma_get_backend_name(Device->pContext->backend) : "?"))
        }
        return true;
    }

    void AudioDevice::Shutdown()
    {
        if(!P) { return; }

        for(std::unique_ptr<ToneVoice>& Tone : P->Tones)
        {
            if(Tone->bInitialized)
            {
                ma_sound_uninit(&Tone->Sound);
                ma_waveform_uninit(&Tone->Wave);
            }
        }
        P->Tones.clear();
        P->Preview.reset();

        /* Stops the device first, so its thread is gone before the groups. */
        ma_engine_stop(&P->Engine);
        for(size_t Index = size_t(EAudioBus::Master) + 1; Index < size_t(EAudioBus::Count); ++Index)
        {
            ma_sound_group_uninit(&P->Buses[Index]);
        }
        ma_engine_uninit(&P->Engine);
        ma_resource_manager_uninit(&P->ResourceManager);

        EngineCore::Memory::Dealloc(P);
        P = nullptr;
        CORE_LOG("[Audio] shut down")
    }

    bool AudioDevice::IsInitialized() const
    {
        return P != nullptr;
    }

    bool AudioDevice::IsOffline() const
    {
        return P && P->Settings.bOffline;
    }

    uint32_t AudioDevice::GetSampleRate() const
    {
        return P ? ma_engine_get_sample_rate(&P->Engine) : 0;
    }

    uint32_t AudioDevice::GetChannels() const
    {
        return P ? ma_engine_get_channels(&P->Engine) : 0;
    }

    float AudioDevice::GetUnitsPerMeter() const
    {
        return P ? P->Settings.UnitsPerMeter : AudioDeviceSettings().UnitsPerMeter;
    }

    void AudioDevice::SetBusVolume(const EAudioBus InBus, const float InVolume)
    {
        if(!ValidBus(InBus)) { return; }
        /* The var's callback applies it, to a running device. */
        BusVolumeVar(InBus).SetFloat(ClampVolume(InVolume));
    }

    float AudioDevice::GetBusVolume(const EAudioBus InBus) const
    {
        return ValidBus(InBus) ? ClampVolume(BusVolumeVar(InBus).GetFloat()) : 0.0f;
    }

    void AudioDevice::SetBusMuted(const EAudioBus InBus, const bool bInMuted)
    {
        if(!P || !ValidBus(InBus)) { return; }
        P->bBusMuted[size_t(InBus)] = bInMuted;
        P->ApplyBus(InBus);
    }

    bool AudioDevice::IsBusMuted(const EAudioBus InBus) const
    {
        return P && ValidBus(InBus) && P->bBusMuted[size_t(InBus)];
    }

    void AudioDevice::SetPaused(const bool bInPaused)
    {
        if(!P || P->bPaused == bInPaused) { return; }
        P->bPaused = bInPaused;
        if(P->Settings.bOffline) { return; }   // Render() honours it instead
        if(bInPaused) { ma_engine_stop(&P->Engine); }
        else          { ma_engine_start(&P->Engine); }
    }

    bool AudioDevice::IsPaused() const
    {
        return P && P->bPaused;
    }

    void AudioDevice::SetListener(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InForward,
                                  const EngineCore::Vector3& InUp, const EngineCore::Vector3& InVelocity)
    {
        if(!P) { return; }
        ma_engine_listener_set_position(&P->Engine, 0, float(InPosition.GetX()), float(InPosition.GetY()), float(InPosition.GetZ()));
        ma_engine_listener_set_direction(&P->Engine, 0, float(InForward.GetX()), float(InForward.GetY()), float(InForward.GetZ()));
        ma_engine_listener_set_world_up(&P->Engine, 0, float(InUp.GetX()), float(InUp.GetY()), float(InUp.GetZ()));
        ma_engine_listener_set_velocity(&P->Engine, 0, float(InVelocity.GetX()), float(InVelocity.GetY()), float(InVelocity.GetZ()));
    }

    bool AudioDevice::PlayTone(const float InHz, const float InSeconds, const float InVolume, const EAudioBus InBus)
    {
        if(!P || !(InHz > 0.0f) || !(InSeconds > 0.0f)) { return false; }
        P->ReapTones();

        std::unique_ptr<ToneVoice> Tone = std::make_unique<ToneVoice>();
        const ma_waveform_config WaveConfig = ma_waveform_config_init(ma_format_f32, 1, GetSampleRate(),
                                                                      ma_waveform_type_sine, 1.0, double(InHz));
        if(ma_waveform_init(&WaveConfig, &Tone->Wave) != MA_SUCCESS) { return false; }

        if(ma_sound_init_from_data_source(&P->Engine, &Tone->Wave, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                          P->BusGroup(InBus), &Tone->Sound) != MA_SUCCESS)
        {
            ma_waveform_uninit(&Tone->Wave);
            return false;
        }
        Tone->bInitialized = true;

        ma_sound_set_volume(&Tone->Sound, ClampVolume(InVolume));
        /* The waveform never ends; the sound stops itself after InSeconds. */
        const ma_uint64 Frames = ma_uint64(double(InSeconds) * double(GetSampleRate()));
        ma_sound_set_stop_time_in_pcm_frames(&Tone->Sound, ma_engine_get_time_in_pcm_frames(&P->Engine) + Frames);
        ma_sound_start(&Tone->Sound);

        P->Tones.push_back(std::move(Tone));
        return true;
    }

    uint64_t AudioDevice::Render(float* OutSamples, const uint64_t InFrames)
    {
        if(!P || !P->Settings.bOffline || !OutSamples || InFrames == 0) { return 0; }
        if(P->bPaused)
        {
            std::fill(OutSamples, OutSamples + InFrames * GetChannels(), 0.0f);
            return InFrames;
        }
        ma_uint64 Read = 0;
        ma_engine_read_pcm_frames(&P->Engine, OutSamples, InFrames, &Read);
        return Read;
    }

    uint32_t AudioDevice::GetPlayingVoiceCount() const
    {
        if(!P) { return 0; }
        uint32_t Count = 0;
        for(const std::unique_ptr<ToneVoice>& Tone : P->Tones)
        {
            if(Tone->bInitialized && ma_sound_is_playing(&Tone->Sound)) { ++Count; }
        }
        return Count;
    }
}

#ifndef AUDIO_AUDIOINTERNAL_H
#define AUDIO_AUDIOINTERNAL_H

/*
 * The one place miniaudio.h is included outside MiniaudioImpl.c: the device's
 * state, for the module's own units (AudioWorld, SoundAsset). Never from a
 * Public header.
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "miniaudio.h"

#include "Public/Audio/AudioDevice.h"

namespace Audio
{
    struct SoundData;

    /*
     * The resource manager's file system: sound sources by name, served from
     * memory -- a Compressed or Stream sound's bytes are already in its
     * .nmsound (SoundData::Source), so miniaudio's loading threads decode and
     * stream them from there. Callbacks first: miniaudio is handed a pointer
     * to this and calls through its first member.
     */
    struct SoundVfs
    {
        ma_vfs_callbacks Callbacks;

        std::mutex                                                        Lock;
        std::unordered_map<std::string, std::shared_ptr<const SoundData>> Sources;

        SoundVfs();
    };

    /* A PlayTone voice: its generator must outlive the sound reading it. */
    struct ToneVoice
    {
        ma_waveform Wave;
        ma_sound    Sound;
        bool        bInitialized = false;
    };

    struct AudioDevice::Impl
    {
        AudioDeviceSettings Settings;

        ma_resource_manager ResourceManager;
        ma_engine           Engine;

        /* Index EAudioBus::Master is unused: Master is the engine's endpoint
         * (ma_engine_set_volume), every other bus a group playing into it. */
        ma_sound_group Buses[size_t(EAudioBus::Count)];
        bool           bBusMuted[size_t(EAudioBus::Count)] = {};

        bool bPaused = false;

        std::vector<std::unique_ptr<ToneVoice>> Tones;

        SoundVfs Vfs;

        /* PlayPreview's voice (AudioWorld.cpp's type, erased): let go first
         * at Shutdown, while the engine it plays in is still there. */
        std::shared_ptr<void> Preview;

        /*
         * InData's source, mounted under a name for ma_sound_init_from_file:
         * "nmsound:<generation>:<path>", so a reimported sound (a new
         * generation) is decoded afresh rather than found in miniaudio's cache.
         * Mounted until the device shuts down. Any thread.
         */
        std::string MountSource(const std::string& InPath, uint64_t InGeneration, const std::shared_ptr<const SoundData>& InData);

        /* The group a voice on InBus plays into; null for Master (the engine
         * itself). */
        ma_sound_group* BusGroup(const EAudioBus InBus)
        {
            return InBus == EAudioBus::Master || InBus >= EAudioBus::Count ? nullptr : &Buses[size_t(InBus)];
        }

        /* Applies InBus' [Audio] volume and its mute. */
        void ApplyBus(EAudioBus InBus);

        /* Tones that have finished, uninitialized and freed. */
        void ReapTones();
    };

    /* The running device's state, or null when it is not initialized. */
    inline AudioDevice::Impl* GetDeviceImpl()
    {
        AudioDevice& Device = AudioDevice::Get();
        return Device.IsInitialized() ? Device.GetImpl() : nullptr;
    }
}

#endif //AUDIO_AUDIOINTERNAL_H

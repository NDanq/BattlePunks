#ifndef AUDIO_AUDIODEVICE_H
#define AUDIO_AUDIODEVICE_H

#include <cstdint>

#include "Public/Audio/AudioAPI.h"
#include "Public/Math/Vector.h"

namespace Audio
{
    /*
     * The mix's buses. Every voice plays into one; each bus has a volume and
     * a mute, and all but Master play into Master. [Audio] <Bus>Volume holds
     * each volume (0..1), so a player's choice is saved in their User.ini
     * and the console can set one ("Audio.MusicVolume 0.3").
     */
    enum class EAudioBus : uint8_t
    {
        Master = 0,
        Music,
        SFX,
        UI,
        Voice,
        Ambient,
        Count
    };

    /* "Master", "Music", ... -- what scripts and the editor name a bus by. */
    AUDIO_API const char* GetBusName(EAudioBus InBus);

    /* Case-insensitive; false (OutBus untouched) for no bus of that name. */
    AUDIO_API bool FindBus(const char* InName, EAudioBus& OutBus);

    /*
     * How the device starts. FromConfig() reads [Audio] (README.md).
     */
    struct AudioDeviceSettings
    {
        /* No output device: nothing is heard, and the mix advances only when
         * Render() is called -- for tests and headless tools. */
        bool bOffline = false;

        /* The mix's rate and channel count; 0 takes the device's own. */
        uint32_t SampleRate = 48000;
        uint32_t Channels   = 0;

        /* World units in a metre (the World's are centimetres), so doppler
         * uses the real speed of sound and a source's distances read in world
         * units. */
        float UnitsPerMeter = 100.0f;

        /* Threads decoding and streaming sounds off the game thread. */
        uint32_t LoadThreads = 2;

        AUDIO_API static AudioDeviceSettings FromConfig();
    };

    /*
     * The process' one output device and mixer, on miniaudio's engine.
     *
     * THREADS. miniaudio owns two kinds: the device's real-time thread, which
     * pulls the mix (spatialization, resampling, buses) whenever the hardware
     * wants more; and the resource manager's job threads, which decode and
     * stream sound files. Neither runs engine code. The game thread changes
     * voices through calls that are safe against both (positions, volumes,
     * start and stop are atomics in miniaudio), so nothing is queued.
     *
     * Initialize and Shutdown are game thread only, as is everything here
     * unless it says otherwise. Without a device -- none present, or it failed
     * to open -- Initialize logs and returns false and every call after it is
     * a quiet no-op: a machine with no sound card runs the game silently.
     */
    class AUDIO_API AudioDevice
    {
    public:
        static AudioDevice& Get();

        AudioDevice(const AudioDevice&)            = delete;
        AudioDevice& operator=(const AudioDevice&) = delete;

        /* True if running, now or already (InSettings is then ignored). */
        bool Initialize(const AudioDeviceSettings& InSettings);
        bool Initialize() { return Initialize(AudioDeviceSettings::FromConfig()); }

        /* Every voice stopped, the device closed. Idempotent. Every World's
         * audio must be detached first (AudioWorld). */
        void Shutdown();

        bool IsInitialized() const;
        bool IsOffline() const;

        uint32_t GetSampleRate() const;
        uint32_t GetChannels() const;
        float    GetUnitsPerMeter() const;

        /* InVolume clamped to 0..4 (above 1 amplifies); also written to its
         * [Audio] var, so it is saved with the player's settings. */
        void  SetBusVolume(EAudioBus InBus, float InVolume);
        float GetBusVolume(EAudioBus InBus) const;
        void  SetBusMuted(EAudioBus InBus, bool bInMuted);
        bool  IsBusMuted(EAudioBus InBus) const;

        /* A gain over the bus's volume (0..4, 1 none), never saved: what a
         * cutscene ducks the music or mutes the ambience with (Sequencer's
         * Bus Volume tracks). The player's volume stays theirs. */
        void  SetBusGain(EAudioBus InBus, float InGain);
        float GetBusGain(EAudioBus InBus) const;

        /*
         * Pausing stops the device pulling the mix: every voice holds where it
         * is, and resumes from there. For a game losing focus or a debugger
         * break, not for a paused game -- the game's World pauses its own
         * voices, and its menus keep playing.
         */
        void SetPaused(bool bInPaused);
        bool IsPaused() const;

        /* Listener 0: where the mix is heard from. InForward and InUp need not
         * be normalized; InVelocity is in world units a second. */
        void SetListener(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InForward,
                         const EngineCore::Vector3& InUp, const EngineCore::Vector3& InVelocity);

        /*
         * SPLITSCREEN (SPLITSCREEN.md): up to AUDIO_LISTENERS_MAX listeners, one
         * per player's view; each 3D sound is heard from the one nearest it
         * (miniaudio's closest-listener spatialization), so a player hears
         * what is near their own camera. 1 by default; those past the count
         * are switched off. SetListener(InIndex, ...) places listener InIndex.
         */
        static constexpr uint32_t AUDIO_LISTENERS_MAX = 4;
        void     SetListenerCount(uint32_t InCount);
        uint32_t GetListenerCount() const;
        void SetListener(uint32_t InIndex, const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InForward,
                         const EngineCore::Vector3& InUp, const EngineCore::Vector3& InVelocity);

        /*
         * A sine of InHz on InBus for InSeconds, not spatialized: to check the
         * output reaches the speakers ("audio tone" on the console). False
         * without a device.
         */
        bool PlayTone(float InHz, float InSeconds, float InVolume = 0.25f, EAudioBus InBus = EAudioBus::SFX);

        /*
         * Offline only: mixes InFrames frames into OutSamples (interleaved,
         * GetChannels() per frame) and advances every voice by that much.
         * Returns the frames written; 0 when not offline.
         */
        uint64_t Render(float* OutSamples, uint64_t InFrames);

        /* Total voices playing now, on every bus. */
        uint32_t GetPlayingVoiceCount() const;

        struct Impl;
        Impl* GetImpl() const { return P; }

    private:
        AudioDevice() = default;
        ~AudioDevice();

        Impl* P = nullptr;
    };
}

#endif //AUDIO_AUDIODEVICE_H

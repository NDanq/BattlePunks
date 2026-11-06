#ifndef AUDIO_AUDIOWORLD_H
#define AUDIO_AUDIOWORLD_H

#include <cstdint>

#include "Public/Audio/AudioAPI.h"
#include "Public/Audio/AudioDevice.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

namespace Audio
{
    /* Where a World is heard from. Forward and Up need not be normalized. */
    struct AudioListenerPose
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Forward = EngineCore::Vector3(0.0, 0.0, 1.0);
        EngineCore::Vector3 Up      = EngineCore::Vector3(0.0, 1.0, 0.0);
    };

    /* The listener when the World has no active Audio Listener: true with
     * OutPose filled (the engine gives the active camera), false for none. */
    using ListenerFallbackFn = bool (*)(GameCore::World& InWorld, AudioListenerPose& OutPose, void* InUserData);

    /* SPLITSCREEN: the listeners, one per player's view, into OutPoses (up to
     * InMax); how many it filled, 0 for none. More than one wins over the
     * level's Audio Listener, a one-view idea. */
    using ListenersFallbackFn = uint32_t (*)(GameCore::World& InWorld, AudioListenerPose* OutPoses, uint32_t InMax, void* InUserData);

    struct AudioWorldStats
    {
        uint32_t Sources  = 0;   // Audio Source components with a voice
        uint32_t Playing  = 0;   // of them, playing now
        uint32_t OneShots = 0;   // PlaySound* voices playing
    };

    /* What one Audio Source's voice is doing, for tests and "stat audio". */
    struct AudioVoiceState
    {
        bool  bPlaying = false;
        float Volume   = 0.0f;   // as applied: the sound's, the source's and its normalize gain
        float Pitch    = 0.0f;   // as applied, before doppler
        float Seconds  = 0.0f;   // its cursor
    };

    /*
     * One World's sound. Attach registers "Audio.Update" in TP_PostUpdate --
     * after physics and scripts have moved everything -- and from then on,
     * every frame:
     *
     *   1. The listener: the first active Audio Listener's world pose, else
     *      the fallback's (the camera), else the origin. SPLITSCREEN: with a
     *      listeners fallback giving more than one (a view each), those --
     *      each sound heard from the nearest; one-shots are weighed by the
     *      nearest too.
     *   2. Each Audio Source: a voice made for its Sound (remade when Sound
     *      changes or the sound is reimported, SoundAssetCache's generation);
     *      AutoPlay started once; what its methods recorded (Play, Stop,
     *      Pause, Resume, FadeTo, Seek) done and cleared; its volume, pitch,
     *      loop and 3D settings applied; its world position, forward and
     *      velocity given to the spatializer; IsPlaying written back. One that
     *      ends emits the World event SoundFinished (Self).
     *   3. One-shots (PlaySound2D / PlaySoundAt / PlaySoundAttached) follow
     *      their entity, if any, and are freed when they end.
     *
     * Components removed and entities destroyed take their voices with them.
     *
     * One-shots are weighed by distance: a 3D one beyond its sound's
     * MaxDistance from the listener is not started, and at MaxOneShots a new
     * one replaces the farthest playing only if it is nearer (else it is
     * dropped) -- so a crowd's far footsteps never push out the near ones.
     * [Audio] LogPlays logs each one played, culled or dropped.
     *
     * NimbleEngine's GameSession attaches one to each level's World (so Play-
     * in-Editor is heard and the edited world is not); a test or tool may
     * attach its own. Detach (or destruction) before the World goes, and
     * before the device shuts down. Needs a running AudioDevice: Attach fails
     * without one. Game thread only.
     */
    class AUDIO_API AudioWorld
    {
    public:
        static constexpr uint32_t MaxOneShots = 64;

        AudioWorld();
        ~AudioWorld();   // Detach()es

        AudioWorld(const AudioWorld&)            = delete;
        AudioWorld& operator=(const AudioWorld&) = delete;

        /* False, logged once, if already attached or the device is not running. */
        bool Attach(GameCore::World& InWorld);

        /* Every voice stopped and freed, the system unregistered. Idempotent. */
        void Detach();

        bool             IsAttached() const;
        GameCore::World* GetWorld() const;

        void SetListenerFallback(ListenerFallbackFn InFn, void* InUserData);
        void SetListenersFallback(ListenersFallbackFn InFn, void* InUserData);

        /* The frame's work, as the TP_PostUpdate system does it; a test may
         * call it directly instead of ticking the World. */
        void Update(float InDeltaSeconds);

        /* Every voice of this World held where it is (the game paused), or
         * resumed. The device and other Worlds play on. */
        void SetPaused(bool bInPaused);
        bool IsPaused() const;

        /* A sound once, not spatialized; InVolume times the sound's own.
         * False if it cannot be loaded. */
        bool PlaySound2D(const char* InSound, float InVolume = 1.0f);
        /* A sound once at InPosition, heard in 3D by the sound's settings. */
        bool PlaySoundAt(const char* InSound, const EngineCore::Vector3& InPosition, float InVolume = 1.0f);
        /* A sound once, following InEntity until it ends (or the entity goes). */
        bool PlaySoundAttached(const char* InSound, GameCore::EntityID InEntity, float InVolume = 1.0f);

        /* Every voice of this World stopped: sources and one-shots. */
        void StopAll();

        void GetStats(AudioWorldStats& OutStats) const;
        bool GetVoiceState(GameCore::EntityID InEntity, AudioVoiceState& OutState) const;

        /* The listener used last frame (listener 0), and SPLITSCREEN's others. */
        AudioListenerPose GetListener() const;
        AudioListenerPose GetListener(uint32_t InIndex) const;
        uint32_t          GetListenerCount() const;

        /* The AudioWorld attached to InWorld, or nullptr. */
        static AudioWorld* Find(const GameCore::World& InWorld);

        struct Impl;

    private:
        Impl* P = nullptr;
    };

    /*
     * A sound heard on its own, in no World: the editor's preview (Content
     * Browser double-click). One at a time -- another replaces it; by its
     * sound's settings, but never spatialized. False without a device or a
     * sound that cannot be loaded. Game thread only.
     */
    AUDIO_API bool PlayPreview(const char* InSound);
    AUDIO_API void StopPreview();
    /* The sound previewing now, or "" -- "" too once it has ended. */
    AUDIO_API const char* GetPreviewSound();
}

#endif //AUDIO_AUDIOWORLD_H

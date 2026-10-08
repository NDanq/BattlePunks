#ifndef GAMECORE_AUDIOSOURCECOMPONENT_H
#define GAMECORE_AUDIOSOURCECOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    enum : size_t { AUDIO_SOUND_PATH_MAX = 128 };   // including the NUL

    /* The bus a source plays into, as the Audio module's EAudioBus. */
    enum EAudioSourceBus : uint8_t
    {
        ASB_Master  = 0,
        ASB_Music   = 1,
        ASB_SFX     = 2,
        ASB_UI      = 3,
        ASB_Voice   = 4,
        ASB_Ambient = 5,
    };

    /*
     * "Audio Source": the entity plays a sound (an .nmsound) where it is --
     * heard in 3D from the Audio Listener (or the active camera), moving with
     * it, pitched by its speed (doppler). Played by the Audio module
     * (Engine/Modules/Audio/README.md).
     *
     * Here, not in Audio, so a level or prefab that carries one still loads,
     * edits and saves in a build with sound compiled out (NIMBLE_WITH_AUDIO=
     * OFF); it is silent there, and the engine warns when a level has any.
     *
     * THE SOUND'S SETTINGS. A sound asset carries its own defaults -- loop,
     * 3D or not, bus, distances. The source plays by those unless Override is
     * on, when it plays by its own fields below instead. Volume and Pitch
     * always scale the sound's.
     *
     * PLAYING. AutoPlay starts it as the level starts (or as the component is
     * added). Play, Stop, Pause, Resume and FadeTo only record what is asked;
     * the Audio module does it next frame and clears it. IsPlaying is as of
     * the last frame. A sound that ends (not looping) emits the World event
     * SoundFinished (Self).
     *
     * Changing Sound restarts it, from the new sound, if it was playing.
     * Volume, Pitch and the overridden settings apply as they change.
     */
    struct GAMECORE_API AudioSourceComponent
    {
        GAMECORE_COMPONENT_BODY()

        char Sound[AUDIO_SOUND_PATH_MAX] = {};   // runtime path of an .nmsound; "": silent

        float Volume = 1.0f;   // times the sound's; 0..4
        float Pitch  = 1.0f;   // times the sound's; a rate, 2 an octave up

        uint8_t bAutoPlay = 1;
        uint8_t bOverride = 0;   // play by the fields below, not the sound's own settings
        uint8_t bLoop     = 0;
        uint8_t bSpatial  = 1;

        uint8_t Bus       = ASB_SFX;   // EAudioSourceBus
        uint8_t Reserved0 = 0;
        uint8_t Reserved1 = 0;
        uint8_t Reserved2 = 0;

        float MinDistance   = 100.0f;    // world units: full volume within
        float MaxDistance   = 5000.0f;   // world units: no quieter beyond
        float Rolloff       = 1.0f;
        float DopplerFactor = 1.0f;      // 0 none

        // === Engine-written: never saved ====================================

        /* What the methods below recorded, for the Audio module's next frame. */
        uint8_t bPendingPlay   = 0;
        uint8_t bPendingStop   = 0;
        uint8_t bPendingPause  = 0;
        uint8_t bPendingResume = 0;

        float PendingFadeGain    = 1.0f;
        float PendingFadeSeconds = -1.0f;   // < 0: no fade asked
        float PendingSeekSeconds = -1.0f;   // < 0: no seek asked

        /* As of the last frame. */
        uint8_t bPlaying   = 0;
        uint8_t bPaused    = 0;
        uint8_t Reserved3  = 0;
        uint8_t Reserved4  = 0;
        float   PlaybackSeconds = 0.0f;
        float   LengthSeconds   = 0.0f;

        /* From the start (from where it was, if paused). */
        void Play();
        void Stop();
        void Pause();
        void Resume();
        /* Its fade gain (0..1, separate from Volume) to InGain over InSeconds. */
        void FadeTo(float InGain, float InSeconds);
        void Seek(float InSeconds);

        bool  IsPlaying() const { return bPlaying != 0; }
        bool  IsPaused() const  { return bPaused != 0; }
        float GetPlaybackSeconds() const { return PlaybackSeconds; }
        float GetLengthSeconds() const   { return LengthSeconds; }

        /* Sound as text, for scripts; Set restarts it if it was playing. */
        const char* GetSound() const { return Sound; }
        void        SetSound(const char* InPath);

        /* "Master", "Music", "SFX", "UI", "Voice" or "Ambient". Set ignores
         * an unknown name. */
        const char* GetBusName() const;
        void        SetBusName(const char* InName);
    };
}

#endif //GAMECORE_AUDIOSOURCECOMPONENT_H

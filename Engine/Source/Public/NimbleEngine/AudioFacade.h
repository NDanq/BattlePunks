#ifndef NIMBLEENGINE_AUDIOFACADE_H
#define NIMBLEENGINE_AUDIOFACADE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * What the engine and a game ask of sound, without the Audio module's
 * headers. Always compiled: with the module compiled out (NIMBLE_WITH_AUDIO=
 * OFF) or no output device, IsAudioAvailable() is false and every call here
 * does nothing -- so game code needs no #if.
 *
 * Sounds themselves are GameCore's components (Audio Source, Audio Listener)
 * and the one-shots below. The "Audio" script library is this, for Lua and
 * script graphs (Engine/Modules/Audio/README.md):
 *
 *     Audio.PlaySoundAt("Content/Sounds/Hit.nmsound", x, y, z, 1.0)
 *     Audio.SetBusVolume("Music", 0.5)
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    /* The module is compiled in and the output device is running. */
    NE_API bool IsAudioAvailable();

    /*
     * Opens the output device from [Audio] (or offline, [Audio] Offline=true).
     * Idempotent; the Application calls it as it starts (not headless), the
     * editor as it opens. False, logged, without the module or a device --
     * the game then runs silently.
     */
    NE_API bool StartAudio();

    /* Closes it, after every World's audio is gone. Idempotent. */
    NE_API void StopAudio();

    /* Holds the whole mix where it is (the window lost focus), or resumes. */
    NE_API void SetAudioPaused(bool bInPaused);

    /* A plain sine on the SFX bus: "audio tone [hz] [seconds]" on the console. */
    NE_API bool PlayTestTone(float InHz, float InSeconds);

    /* A sound (an .nmsound's runtime path) once in InWorld: heard the same
     * everywhere; at InPosition in 3D; or following InEntity. InVolume times
     * the sound's own. False without audio for InWorld, or a sound that
     * cannot be loaded. */
    NE_API bool PlaySound2D(GameCore::World& InWorld, const char* InSound, float InVolume = 1.0f);
    NE_API bool PlaySoundAt(GameCore::World& InWorld, const char* InSound, const EngineCore::Vector3& InPosition, float InVolume = 1.0f);
    NE_API bool PlaySoundAttached(GameCore::World& InWorld, const char* InSound, GameCore::EntityID InEntity, float InVolume = 1.0f);

    /* Every sound of InWorld stopped: sources and one-shots. */
    NE_API void StopAllSounds(GameCore::World& InWorld);

    /* A bus by name -- Master, Music, SFX, UI, Voice, Ambient -- at InVolume
     * (0..4, saved with the player's settings), or muted. */
    NE_API void  SetBusVolume(const char* InBus, float InVolume);
    NE_API float GetBusVolume(const char* InBus);
    NE_API void  SetBusMuted(const char* InBus, bool bInMuted);
}

#endif //NIMBLEENGINE_AUDIOFACADE_H

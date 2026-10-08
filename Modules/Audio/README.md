# Audio

Sound for a GameCore `World`, on [miniaudio](https://miniaud.io) 0.11.25.
Entities with an **Audio Source** component play a sound where they are. The
sound is heard in 3D from the **Audio Listener**, or from the active camera
when there is no listener. Both components belong to GameCore, and this module
plays them. It also owns the output device and mixer (`AudioDevice`), the
`.nmsound` asset (`SoundAsset`), and each World's voices (`AudioWorld`).

`Audio` is written for this repository and uses `nimble_add_module()`. It
vendors miniaudio, with the `stb_vorbis` miniaudio ships for `.ogg`, under
`External/`. Both are MIT-0 / public domain.

---

## Position in the tree

```
Host exe --> NimbleEngine --> Audio --> GameCore --> EngineCore
                                `----> miniaudio (STATIC, inside Audio only)
```

`NimbleEngine` links it **PRIVATE**. A game reaches sound in three ways, none
of which needs an Audio header:

- GameCore's components;
- the `Audio` script library;
- `NimbleEngine/AudioFacade.h`.

So switching the module off cannot break a game's include or link line. The
editor links it for sound import, and engine code includes it under
`#if WITH_AUDIO`.

---

## Turning it on and off

It is **on by default**, one switch per configure: `NIMBLE_WITH_AUDIO`. It
becomes `WITH_AUDIO=0/1` on every target. A project that wants no sound turns
it off in its config file:

```ini
; Config/DefaultEngine.ini
[Modules]
Audio=false
```

In the editor the same switch is **Project Settings > Project > Modules >
Sound (Audio)**. You can also pass `-DNIMBLE_WITH_AUDIO=OFF` when you
configure.

**Off**, nothing of miniaudio is compiled.

- **The components stay.** Audio Source and Audio Listener are GameCore's, so
  levels and prefabs that carry them load, edit and save unchanged. They are
  silent.
- **It says so.** A level that has any Audio Sources warns as it loads
  (`WarnInactiveModuleComponents`).
- **Calls do nothing.** `IsAudioAvailable()` is false, and the facade and
  the script library's calls do nothing (a play returns false).

---

## Threads

The module starts no threads of its own. miniaudio runs two kinds:

- **The device's real-time thread** pulls the mix whenever the hardware wants
  more: every voice, spatialized, resampled, through its bus into Master.
  This is PipeWire, PulseAudio, ALSA or JACK on Linux, WASAPI on Windows, and
  Core Audio on macOS.
- **The resource manager's job threads** (`[Audio] LoadThreads`, default 2)
  decode Compressed sounds and stream Stream sounds. They read the sound's
  bytes straight out of its `.nmsound`, through an in-memory file system
  (`SoundVfs`).

The game thread drives everything else: `AudioWorld` runs in `TP_PostUpdate`,
after physics and scripts. Each frame it sets each voice's position,
direction, velocity, volume and pitch, and starts and stops voices.
miniaudio's setters are atomic, so nothing is queued to the mixer. No engine
code runs on the audio threads.

---

## The device: `AudioDevice`

There is one per process. The Application starts it (except when headless)
and so does the editor, through `NimbleEngine::StartAudio()`. It reads
`[Audio]`:

| Key | Default | |
|---|---|---|
| `Offline` | false | No output device: silent, and mixed only when `Render()` is called. For tests |
| `SampleRate` | 48000 | The mix's rate; 0 uses the device's |
| `Channels` | 0 | The mix's channel count; 0 uses the device's |
| `UnitsPerMeter` | 100 | World units in a metre, for doppler's speed of sound |
| `LoadThreads` | 2 | Decoding and streaming threads |
| `MasterVolume`, `MusicVolume`, `SFXVolume`, `UIVolume`, `VoiceVolume`, `AmbientVolume` | 1 | The buses (0..4), applied as they change and saved with the player's settings |

**Buses.** Master, with Music, SFX, UI, Voice and Ambient playing into it.
Each bus has a volume and a mute.

**Pausing the device** (`SetPaused`) holds the whole mix. That is for a lost
window. A paused game pauses only its World's voices (`AudioWorld::SetPaused`),
so its menus can still play.

**No device is not an error.** If there is no sound card, or the device
fails to open, the module logs it once and the game runs silently.

On the console:

- `audio` shows the device.
- `audio tone [hz] [seconds]` plays a sine, to check the output reaches the
  speakers.

---

## Sounds: `.nmsound`

A sound keeps the file it was imported from, whole, plus what the game
plays, which is cooked from that file:

```
NSND v1
  SHDR v1    source format; cook settings; playback defaults
  SDAT v1    the cook: key, load mode, channels, rate, frames, peak, gain, waveform, samples
  SSRC v1    the imported file's bytes (optional)
  ISRC v1    import info, for Reimport (optional)
```

### Importing

The Content Browser imports `.wav`, `.flac`, `.mp3` and `.ogg`: drop a file,
or use **Import...**. The editor console does the same:

```
import Tools/Audio/EngineLoop.wav Content/Sounds
```

A sound longer than 10 s imports as music: not spatial, on the Music bus.
Reimporting a sound keeps the settings an author gave it.

### Cooking

The source is the truth, as a texture's pixels are. Changing a cook setting
cooks the sound again from its source, and `SaveSoundFile` does this whenever
the cook key no longer matches. There are three load modes:

| Load mode | What the file holds | When |
|---|---|---|
| **Decompressed** | 16-bit PCM at the mix's rate: no decoding or resampling as it plays | Short, frequent sounds (Auto: under 2 s, or any WAV) |
| **Compressed** | The source as imported, decoded once on the loading threads the first time it plays and shared by every voice | Medium sounds (Auto: 2-10 s) |
| **Stream** | The source as imported, decoded a little at a time | Music and ambience (Auto: over 10 s) |

These cook settings apply to Decompressed sounds only:

- **ForceMono**: on by default, since a 3D source wants one channel.
- **SampleRate**: the rate the sound is cooked to.
- **TrimSilence**: cuts leading and trailing silence.

**Normalize** is different: it applies to every mode, as a gain on playback.

No platform needs its own cook. Every desktop target plays the same formats.

---

## In a World: `AudioWorld`

`GameSession` attaches one to each level's World (and to Play-in-Editor's),
before the level's `OnLoad`. A test or tool may attach its own. Every frame
it does three things:

1. **The listener.** It uses the first active **Audio Listener**'s world
   pose. With none, it uses the active camera. The World's axes are
   left-handed: +X right, +Y up, +Z forward.
2. **Each Audio Source.**
   - It makes a voice for the source's `Sound`, and remakes it when `Sound`
     changes or the sound is reimported.
   - AutoPlay starts the voice once.
   - It carries out what the component's methods recorded: `Play`, `Stop`,
     `Pause`, `Resume`, `FadeTo(Gain, Seconds)`, `Seek(Seconds)`.
   - It applies volume, pitch and the 3D settings.
   - It places the voice at the entity, with a velocity from the entity's
     motion for doppler.
   - It writes `IsPlaying`, `PlaybackSeconds` and `LengthSeconds` back to
     the component.
   - A sound that ends by itself emits the World event `SoundFinished`
     (Source).
3. **One-shots.** `PlaySound2D`, `PlaySoundAt` and `PlaySoundAttached` play a
   sound once, following its entity if one was given, and free the voice
   when it ends.
   - A 3D one-shot beyond its sound's MaxDistance from the listener is not
     started.
   - At most 64 play at once. A new one replaces the farthest playing only if
     it is nearer; otherwise it is dropped. A crowd's far footsteps never push
     out the near ones.
   - `[Audio] LogPlays=true` logs each one-shot played, culled or dropped.

A source plays by its sound's settings: loop, 3D or not, bus, distances,
attenuation and cone. With **Override** on, it plays by its own fields
instead. Its Volume and Pitch always scale the sound's.

**Footsteps.** An animation clip's notify can name a sound, played once at
its bone as the clip passes it (RenderCore's ANIMATION.md, "Notifies").
GameSession connects the level's animation system to its `AudioWorld`, so
each footstep is a 3D one-shot, weighed by distance like any other. The
LightLab foxes are the example: `Tools/add_footstep_notifies.py` gave their
walk and run clips a step where each paw lands.

`stat audio` shows the level's sources, voices, listener and device.

---

## From C++, Lua and script graphs

The `Audio` script library and the Audio Source component bindings are
written once in C++. Lua and the script graph both get them, because graphs
compile to Lua.

```cpp
// C++
#include "Public/NimbleEngine/AudioFacade.h"
NimbleEngine::PlaySoundAt(World, "Content/Sounds/Hit.nmsound", Position);
Entity.GetComponent<GameCore::AudioSourceComponent>()->FadeTo(0.0f, 2.0f);
NimbleEngine::SetBusVolume("Music", 0.5f);
```

```lua
-- Lua
local Engine = World.FindEntity("Buggy"):GetComponent("Audio Source")
Engine:SetPitch(1.5)
Audio.PlaySoundAt("Content/Sounds/Hit.nmsound", x, y, z, 1.0)
Events.On("SoundFinished", function(Source) print(Source:GetName()) end)
```

In a script graph:

- The Audio Source's fields and methods appear as nodes, for example
  **Audio Source: Set Pitch** and **Audio Source: Fade To**.
- So does the library, under **Library > Audio** (Play Sound At, Set Bus
  Volume, On Sound Finished, and the rest).
- LightLab's `Scripts/EngineSound.script.nmgraph` is a worked example. It
  drives the PhysicsLab buggy's engine from its Vehicle's `EngineRPM` and
  throttle.

| Library member | |
|---|---|
| `IsAvailable() -> bool` | Compiled in, and a device is running |
| `PlaySound2D(Sound, Volume) -> bool` | Once, the same in both ears |
| `PlaySoundAt(Sound, Position, Volume) -> bool` | Once, at a point, in 3D |
| `PlaySoundAttached(Sound, Target, Volume) -> bool` | Once, following an entity |
| `StopAll()` | Every sound in the World |
| `SetBusVolume(Bus, Volume)`, `GetBusVolume(Bus)`, `SetBusMuted(Bus, Muted)` | A bus by name |
| event `SoundFinished(Source)` | An Audio Source's sound ended by itself |

---

## Tests

All run offline, with no sound card:

- `Audio.Tests` covers the device, buses, cooking, files, the cache, an
  `AudioWorld`'s panning, distance, pitch, events, one-shots and the load
  modes.
- `NimbleEngine.AudioScriptTests` covers the library and the component from
  Lua, and the engine-sound graph compiled and run.
- `Editor.Import` covers sound import and reimport.

---

## Known limits

- **No shipping cook yet.** A game ships the sound's source (`SSRC`) beside
  its cook, as textures ship their pixels. When Cook Content exists, it
  should drop `SSRC` for Decompressed sounds; the runtime never reads it.
- **No encoder.** A long WAV stays a WAV. Import `.ogg` for music.
  Transcoding would mean vendoring libvorbis or libopus.
- **One listener.** If two Worlds play at once (the editor's and
  Play-in-Editor's), the one updated last places it.
- **No occlusion, reverb or HRTF.** Panning and attenuation only.

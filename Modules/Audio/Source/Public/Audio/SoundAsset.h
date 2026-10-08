#ifndef AUDIO_SOUNDASSET_H
#define AUDIO_SOUNDASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Audio/AudioAPI.h"
#include "Public/Audio/AudioDevice.h"

namespace EngineCore
{
    class Archive;
}

/*
 * .nmsound -- a sound: the file it was imported from, kept whole, and what
 * the game plays, cooked from it:
 *
 *   NSND v1                the file
 *     SHDR v1              the source's format; the cook settings; the
 *                          playback defaults (SoundSettings)
 *     SDAT v1              the cook: its key, load mode, channels, rate,
 *                          frames, peak, gain and waveform, then its samples
 *                          (Decompressed) -- or none, when it plays the
 *                          source as it is (Compressed, Stream)
 *     SSRC v1 (optional)   the imported file's bytes, as imported
 *     ISRC v1 (optional)   SoundImportInfo, for Reimport
 *
 * The source is the truth, as a texture's RGBA8 pixels are: changing a cook
 * setting cooks again from it (SaveSoundFile does, when the key no longer
 * matches). A game loads ForPlayback, which steps over SSRC unless the cook
 * plays the source itself. There is no cook step for shipping yet, so a game
 * ships the source too (README.md, Known limits).
 */
namespace Audio
{
    /*
     * How the game holds a sound:
     *
     *   Decompressed   cooked to 16-bit PCM at the mix's rate, in the file:
     *                  nothing to decode or resample as it plays. Short and
     *                  frequent sounds -- footsteps, impacts, clicks.
     *   Compressed     the source as imported, decoded once, on the loading
     *                  threads, the first time it plays; shared by every voice.
     *   Stream         the source as imported, decoded a little at a time as it
     *                  plays: long sounds -- music, ambience.
     *   Auto           chosen by length when cooked: under 2 s Decompressed,
     *                  over 10 s Stream, else Compressed (a WAV source, being
     *                  PCM already, is Decompressed rather than Compressed).
     */
    enum class ESoundLoadMode : uint8_t { Auto = 0, Decompressed = 1, Compressed = 2, Stream = 3 };

    /* Gain over distance, beyond MinDistance (miniaudio's models). */
    enum class ESoundAttenuation : uint8_t { None = 0, Inverse = 1, Linear = 2, Exponential = 3 };

    /* What the source file is; found from its bytes, not its name. */
    enum class ESoundSourceFormat : uint8_t { Unknown = 0, Wav = 1, Flac = 2, Mp3 = 3, Vorbis = 4 };

    AUDIO_API const char* GetSoundLoadModeName(ESoundLoadMode InMode);
    AUDIO_API const char* GetSoundAttenuationName(ESoundAttenuation InModel);
    AUDIO_API const char* GetSoundSourceFormatName(ESoundSourceFormat InFormat);

    /* The source's format from its first bytes; Unknown if none matches. */
    AUDIO_API ESoundSourceFormat DetectSoundSourceFormat(const uint8_t* InBytes, size_t InSize);

    struct SoundSettings
    {
        // === Cook: change one and the sound is cooked again ==================

        ESoundLoadMode LoadMode = ESoundLoadMode::Auto;

        /* Decompressed only: averaged to one channel. On for a spatial sound
         * -- a stereo source heard in 3D is two voices' work for one. */
        bool bForceMono = true;

        /* Decompressed only: the rate it is cooked to; 0 the mix's (48 kHz). */
        uint32_t SampleRate = 0;

        /* Decompressed only: leading and trailing silence (under -60 dB) cut. */
        bool bTrimSilence = false;

        /* Its peak brought to full scale, as a gain applied as it plays. */
        bool bNormalize = false;

        // === Playback defaults: an Audio Source starts from these ============

        float     Volume = 1.0f;   // 0..4
        float     Pitch  = 1.0f;   // a rate: 2 an octave up
        bool      bLoop  = false;
        EAudioBus Bus    = EAudioBus::SFX;

        /* Heard from where it plays. Off: the same in both ears wherever the
         * listener is (music, UI). */
        bool bSpatial = true;

        /* World units. Full volume within MinDistance; Attenuation from there
         * out to MaxDistance, then no quieter. */
        float             MinDistance   = 100.0f;
        float             MaxDistance   = 5000.0f;
        ESoundAttenuation Attenuation   = ESoundAttenuation::Inverse;
        float             Rolloff       = 1.0f;
        float             DopplerFactor = 1.0f;   // 0 none

        /* Degrees about the source's forward (+Z): full volume inside the
         * inner angle, ConeOuterGain outside the outer, blended between.
         * 360 and 360: no cone. */
        float ConeInnerAngle = 360.0f;
        float ConeOuterAngle = 360.0f;
        float ConeOuterGain  = 0.0f;

        /* The cook's settings only, for the cook key. */
        bool CookEquals(const SoundSettings& InOther) const
        {
            return LoadMode == InOther.LoadMode && bForceMono == InOther.bForceMono && SampleRate == InOther.SampleRate
                   && bTrimSilence == InOther.bTrimSilence && bNormalize == InOther.bNormalize;
        }
    };

    /* An imported sound's source and settings (optional 'ISRC' chunk), for
     * Reimport; ignored at run time. As RenderCore's AssetImportInfo. */
    struct SoundImportInfo
    {
        std::string SourcePath;    // project-relative if under the project root, else absolute
        int64_t     SourceTime = 0;   // the source's mtime at import, epoch seconds
        std::string Settings;      // the importer's options as "Key=Value" lines

        bool IsEmpty() const { return SourcePath.empty(); }
    };

    /* Peaks for drawing a sound: WaveformBuckets of them, 0..255, over its length. */
    constexpr uint32_t WaveformBuckets = 128;

    /* What CookSound made. */
    struct CookedSound
    {
        /* Of Source and the cook settings; 0 never cooked. */
        uint64_t Key = 0;

        /* Never Auto: what Auto chose. */
        ESoundLoadMode Mode = ESoundLoadMode::Decompressed;

        /* Of what plays: the samples below, or the source. */
        uint32_t Channels   = 0;
        uint32_t SampleRate = 0;
        uint64_t Frames     = 0;

        /* The loudest sample, 0..1, before Gain; and the gain applied as it
         * plays (Normalize), 1 without. */
        float Peak = 0.0f;
        float Gain = 1.0f;

        std::vector<uint8_t> Waveform;

        /* Decompressed: interleaved 16-bit PCM, Frames * Channels samples.
         * Empty otherwise -- the source is played. */
        std::vector<int16_t> Samples;

        bool   PlaysSource() const { return Mode == ESoundLoadMode::Compressed || Mode == ESoundLoadMode::Stream; }
        double GetDuration() const { return SampleRate > 0 ? double(Frames) / double(SampleRate) : 0.0; }
    };

    struct SoundData
    {
        SoundSettings Settings;

        /* The imported file, as it was; its format. Empty in a sound loaded
         * ForPlayback whose cook does not play it. */
        std::vector<uint8_t> Source;
        ESoundSourceFormat   SourceFormat = ESoundSourceFormat::Unknown;

        CookedSound Cooked;

        SoundImportInfo Import;

        /* Cooked from this Source with these settings. */
        AUDIO_API bool IsCooked() const;
    };

    /* What InData's Source and cook settings key to: a cook is current when
     * its Key is this. */
    AUDIO_API uint64_t ComputeSoundCookKey(const SoundData& InData);

    /*
     * Cooks InOutData.Source by its settings into InOutData.Cooked (decodes it
     * whatever the mode, for its length, peak and waveform). False, with
     * OutError, if the source cannot be decoded. Any thread; needs no device.
     */
    AUDIO_API bool CookSound(SoundData& InOutData, std::string& OutError);

    /*
     * A sound from a .wav, .flac, .mp3 or .ogg on disk: its bytes as Source,
     * the default settings -- not spatial, and on the Music bus, if it is
     * longer than 10 s -- and cooked. False, with OutError, if it cannot be
     * read or decoded.
     */
    AUDIO_API bool ImportSoundFile(const std::string& InSourceFile, SoundData& OutData, std::string& OutError);

    enum class ESoundLoad : uint8_t
    {
        Everything,    // the source too: the editor, to cook again
        ForPlayback    // the source only if the cook plays it
    };

    AUDIO_API bool SerializeSound(EngineCore::Archive& Ar, SoundData& InOutData, ESoundLoad InLoad = ESoundLoad::Everything);

    /* False, warned, if the file is missing or not a sound. */
    AUDIO_API bool LoadSoundFile(const std::string& InPath, SoundData& OutData, ESoundLoad InLoad = ESoundLoad::Everything);

    /* Cooks first when the cook is not current (CookSound); false, warned, if
     * that or the write fails. */
    AUDIO_API bool SaveSoundFile(const std::string& InPath, SoundData& InData);

    /*
     * Loaded sounds by path, loaded ForPlayback: shared by the game thread and
     * miniaudio's loading threads, thread-safe. A failed load is warned once
     * and remembered until Invalidate, which is how a reimport reaches a
     * playing game: GetGeneration() moves on, and a voice playing the old one
     * is remade from the new (AudioWorld).
     */
    class AUDIO_API SoundAssetCache
    {
    public:
        /* The sound, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<const SoundData> Find(const std::string& InPath);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);

        /* Moves on at every Invalidate. */
        static uint64_t GetGeneration();
    };
}

#endif //AUDIO_SOUNDASSET_H

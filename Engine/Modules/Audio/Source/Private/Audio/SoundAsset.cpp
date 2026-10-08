#include "Public/Audio/SoundAsset.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include "miniaudio.h"

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"

namespace Audio
{
    namespace
    {
        constexpr uint32_t TAG_SOUND         = EngineCore::MakeArchiveTag('N','S','N','D');
        constexpr uint32_t TAG_HEADER        = EngineCore::MakeArchiveTag('S','H','D','R');
        constexpr uint32_t TAG_COOKED        = EngineCore::MakeArchiveTag('S','D','A','T');
        constexpr uint32_t TAG_SOURCE        = EngineCore::MakeArchiveTag('S','S','R','C');
        constexpr uint32_t TAG_IMPORT_SOURCE = EngineCore::MakeArchiveTag('I','S','R','C');

        /* Part of every cook key: bump it when CookSound's output changes, and
         * every sound cooks again at its next save. */
        constexpr uint64_t COOK_VERSION = 1;

        /* The mix's rate (AudioDeviceSettings), what Decompressed cooks to by default. */
        constexpr uint32_t DEFAULT_COOK_RATE = 48000;

        /* Auto's lengths, seconds. */
        constexpr double AUTO_DECOMPRESS_BELOW = 2.0;
        constexpr double AUTO_STREAM_ABOVE     = 10.0;

        /* -60 dB: TrimSilence's threshold. */
        constexpr float SILENCE = 0.001f;

        /* Normalize never amplifies more than this (+24 dB). */
        constexpr float MAX_NORMALIZE_GAIN = 16.0f;

        /* A file's limits, against a damaged one: an hour at 192 kHz, 8 channels. */
        constexpr uint32_t MAX_CHANNELS = 8;
        constexpr uint32_t MAX_RATE     = 384000;
        constexpr uint64_t MAX_FRAMES   = uint64_t(3600) * 192000;

        /* A source decoded to float at its own rate and channels. */
        struct DecodedPcm
        {
            uint32_t           Channels   = 0;
            uint32_t           SampleRate = 0;
            std::vector<float> Samples;   // interleaved

            uint64_t Frames() const { return Channels > 0 ? Samples.size() / Channels : 0; }
        };

        bool Decode(const std::vector<uint8_t>& InSource, DecodedPcm& OutPcm, std::string& OutError)
        {
            if(InSource.empty())
            {
                OutError = "no source";
                return false;
            }

            const ma_decoder_config Config = ma_decoder_config_init(ma_format_f32, 0, 0);
            ma_decoder Decoder;
            const ma_result Result = ma_decoder_init_memory(InSource.data(), InSource.size(), &Config, &Decoder);
            if(Result != MA_SUCCESS)
            {
                OutError = std::string("cannot decode the source: ") + ma_result_description(Result);
                return false;
            }

            OutPcm            = DecodedPcm();
            OutPcm.Channels   = Decoder.outputChannels;
            OutPcm.SampleRate = Decoder.outputSampleRate;
            if(OutPcm.Channels == 0 || OutPcm.Channels > MAX_CHANNELS || OutPcm.SampleRate == 0 || OutPcm.SampleRate > MAX_RATE)
            {
                ma_decoder_uninit(&Decoder);
                OutError = "the source has an unsupported channel count or rate";
                return false;
            }

            /* In pieces: an MP3 or Vorbis length may be unknown until read. */
            constexpr ma_uint64 Chunk = 16384;
            for(;;)
            {
                const size_t At = OutPcm.Samples.size();
                OutPcm.Samples.resize(At + size_t(Chunk) * OutPcm.Channels);
                ma_uint64 Read = 0;
                const ma_result ReadResult = ma_decoder_read_pcm_frames(&Decoder, OutPcm.Samples.data() + At, Chunk, &Read);
                OutPcm.Samples.resize(At + size_t(Read) * OutPcm.Channels);
                if(Read < Chunk || ReadResult != MA_SUCCESS) { break; }
                if(OutPcm.Frames() > MAX_FRAMES)
                {
                    ma_decoder_uninit(&Decoder);
                    OutError = "the source is longer than an hour";
                    return false;
                }
            }
            ma_decoder_uninit(&Decoder);

            if(OutPcm.Frames() == 0)
            {
                OutError = "the source has no samples";
                return false;
            }
            return true;
        }

        void DownmixToMono(DecodedPcm& InOutPcm)
        {
            if(InOutPcm.Channels <= 1) { return; }
            const uint64_t Frames = InOutPcm.Frames();
            const float    Scale  = 1.0f / float(InOutPcm.Channels);
            for(uint64_t Frame = 0; Frame < Frames; ++Frame)
            {
                float Sum = 0.0f;
                for(uint32_t Channel = 0; Channel < InOutPcm.Channels; ++Channel)
                {
                    Sum += InOutPcm.Samples[size_t(Frame * InOutPcm.Channels + Channel)];
                }
                InOutPcm.Samples[size_t(Frame)] = Sum * Scale;
            }
            InOutPcm.Samples.resize(size_t(Frames));
            InOutPcm.Channels = 1;
        }

        bool Resample(DecodedPcm& InOutPcm, const uint32_t InRate, std::string& OutError)
        {
            if(InOutPcm.SampleRate == InRate) { return true; }

            const ma_resampler_config Config = ma_resampler_config_init(ma_format_f32, InOutPcm.Channels, InOutPcm.SampleRate,
                                                                        InRate, ma_resample_algorithm_linear);
            ma_resampler Resampler;
            if(ma_resampler_init(&Config, nullptr, &Resampler) != MA_SUCCESS)
            {
                OutError = "cannot resample the source";
                return false;
            }

            ma_uint64 Expected = 0;
            ma_resampler_get_expected_output_frame_count(&Resampler, InOutPcm.Frames(), &Expected);
            std::vector<float> Out(size_t(Expected + 64) * InOutPcm.Channels);

            ma_uint64 FramesIn  = InOutPcm.Frames();
            ma_uint64 FramesOut = Expected + 64;
            const ma_result Result = ma_resampler_process_pcm_frames(&Resampler, InOutPcm.Samples.data(), &FramesIn,
                                                                     Out.data(), &FramesOut);
            ma_resampler_uninit(&Resampler, nullptr);
            if(Result != MA_SUCCESS)
            {
                OutError = "cannot resample the source";
                return false;
            }

            Out.resize(size_t(FramesOut) * InOutPcm.Channels);
            InOutPcm.Samples    = std::move(Out);
            InOutPcm.SampleRate = InRate;
            return true;
        }

        void TrimSilence(DecodedPcm& InOutPcm)
        {
            const uint64_t Frames = InOutPcm.Frames();
            const auto Loud = [&InOutPcm](const uint64_t InFrame)
            {
                for(uint32_t Channel = 0; Channel < InOutPcm.Channels; ++Channel)
                {
                    if(std::fabs(InOutPcm.Samples[size_t(InFrame * InOutPcm.Channels + Channel)]) > SILENCE) { return true; }
                }
                return false;
            };

            uint64_t First = 0;
            while(First < Frames && !Loud(First)) { ++First; }
            uint64_t End = Frames;
            while(End > First && !Loud(End - 1)) { --End; }

            if(End <= First)
            {
                /* All silence: keep one frame, so it is still a sound. */
                InOutPcm.Samples.assign(InOutPcm.Channels, 0.0f);
                return;
            }
            InOutPcm.Samples.erase(InOutPcm.Samples.begin() + std::ptrdiff_t(End * InOutPcm.Channels), InOutPcm.Samples.end());
            InOutPcm.Samples.erase(InOutPcm.Samples.begin(), InOutPcm.Samples.begin() + std::ptrdiff_t(First * InOutPcm.Channels));
        }

        float PeakOf(const DecodedPcm& InPcm)
        {
            float Peak = 0.0f;
            for(const float Sample : InPcm.Samples)
            {
                Peak = std::max(Peak, std::fabs(Sample));
            }
            return std::min(Peak, 1.0f);
        }

        std::vector<uint8_t> WaveformOf(const DecodedPcm& InPcm)
        {
            std::vector<uint8_t> Out(WaveformBuckets, 0);
            const uint64_t Frames = InPcm.Frames();
            if(Frames == 0) { return Out; }
            for(uint32_t Bucket = 0; Bucket < WaveformBuckets; ++Bucket)
            {
                const uint64_t Begin = Frames * Bucket / WaveformBuckets;
                const uint64_t End   = std::max(Frames * (Bucket + 1) / WaveformBuckets, Begin + 1);
                float Peak = 0.0f;
                for(uint64_t Frame = Begin; Frame < End && Frame < Frames; ++Frame)
                {
                    for(uint32_t Channel = 0; Channel < InPcm.Channels; ++Channel)
                    {
                        Peak = std::max(Peak, std::fabs(InPcm.Samples[size_t(Frame * InPcm.Channels + Channel)]));
                    }
                }
                Out[Bucket] = uint8_t(std::lround(std::min(Peak, 1.0f) * 255.0f));
            }
            return Out;
        }

        int16_t ToS16(const float InSample)
        {
            const float Clamped = std::clamp(InSample, -1.0f, 1.0f);
            return int16_t(std::lround(Clamped * 32767.0f));
        }

        // === FNV-1a, for the cook key ========================================

        constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
        constexpr uint64_t FNV_PRIME  = 1099511628211ull;

        uint64_t Fnv(uint64_t InHash, const void* InData, const size_t InSize)
        {
            const auto* Bytes = static_cast<const uint8_t*>(InData);
            for(size_t Index = 0; Index < InSize; ++Index)
            {
                InHash = (InHash ^ Bytes[Index]) * FNV_PRIME;
            }
            return InHash;
        }

        template<typename T>
        uint64_t FnvValue(const uint64_t InHash, const T InValue)
        {
            return Fnv(InHash, &InValue, sizeof(InValue));
        }

        // === Serialization ===================================================

        bool OpenRequiredChunk(EngineCore::Archive& Ar, const uint32_t InTag, const uint16_t InVersion, const char* InWhat)
        {
            uint16_t Version = InVersion;
            if(!Ar.OpenChunk(InTag, Version))
            {
                Ar.SetError(InWhat);
                return false;
            }
            return true;
        }

        /* Guards a resize from an on-disk count (AssetFileCommon.h's). */
        bool CanHold(EngineCore::Archive& Ar, const uint64_t InCount, const size_t InElementBytes, const char* InWhat)
        {
            if(Ar.IsSaving()) { return true; }
            const uint64_t Remaining = uint64_t(Ar.GetSize() - Ar.GetCursor());
            if(InCount > Remaining / InElementBytes)
            {
                Ar.SetError(InWhat);
                return false;
            }
            return true;
        }

        template<typename TEnum>
        void SerializeEnum(EngineCore::Archive& Ar, TEnum& InOutValue)
        {
            uint8_t Raw = uint8_t(InOutValue);
            Ar << Raw;
            InOutValue = TEnum(Raw);
        }

        void SerializeHeader(EngineCore::Archive& Ar, SoundData& InOutData)
        {
            SoundSettings& S = InOutData.Settings;
            SerializeEnum(Ar, InOutData.SourceFormat);

            SerializeEnum(Ar, S.LoadMode);
            Ar << S.bForceMono;
            Ar << S.SampleRate;
            Ar << S.bTrimSilence;
            Ar << S.bNormalize;

            Ar << S.Volume;
            Ar << S.Pitch;
            Ar << S.bLoop;
            SerializeEnum(Ar, S.Bus);
            Ar << S.bSpatial;
            Ar << S.MinDistance;
            Ar << S.MaxDistance;
            SerializeEnum(Ar, S.Attenuation);
            Ar << S.Rolloff;
            Ar << S.DopplerFactor;
            Ar << S.ConeInnerAngle;
            Ar << S.ConeOuterAngle;
            Ar << S.ConeOuterGain;

            if(Ar.IsLoading() && !Ar.HasError())
            {
                if(InOutData.SourceFormat > ESoundSourceFormat::Vorbis || S.LoadMode > ESoundLoadMode::Stream
                   || S.Bus >= EAudioBus::Count || S.Attenuation > ESoundAttenuation::Exponential || S.SampleRate > MAX_RATE)
                {
                    Ar.SetError("sound: unknown format, load mode, bus or attenuation");
                }
            }
        }

        void SerializeCooked(EngineCore::Archive& Ar, CookedSound& InOutCooked)
        {
            Ar << InOutCooked.Key;
            SerializeEnum(Ar, InOutCooked.Mode);
            Ar << InOutCooked.Channels;
            Ar << InOutCooked.SampleRate;
            Ar << InOutCooked.Frames;
            Ar << InOutCooked.Peak;
            Ar << InOutCooked.Gain;

            uint32_t WaveformCount = uint32_t(InOutCooked.Waveform.size());
            Ar << WaveformCount;
            if(!CanHold(Ar, WaveformCount, 1, "sound: implausible waveform")) { return; }
            if(Ar.IsLoading()) { InOutCooked.Waveform.resize(WaveformCount); }
            if(WaveformCount > 0) { Ar.SerializeBytes(InOutCooked.Waveform.data(), WaveformCount); }

            uint64_t SampleCount = uint64_t(InOutCooked.Samples.size());
            Ar << SampleCount;
            if(!CanHold(Ar, SampleCount, sizeof(int16_t), "sound: implausible sample count")) { return; }
            if(Ar.IsLoading()) { InOutCooked.Samples.resize(size_t(SampleCount)); }
            /* Little-endian on every target the engine builds for, as the
             * Archive's own scalars are. */
            if(SampleCount > 0) { Ar.SerializeBytes(InOutCooked.Samples.data(), size_t(SampleCount) * sizeof(int16_t)); }

            if(Ar.IsLoading() && !Ar.HasError())
            {
                const bool bDecompressed = InOutCooked.Mode == ESoundLoadMode::Decompressed;
                if(InOutCooked.Mode == ESoundLoadMode::Auto || InOutCooked.Mode > ESoundLoadMode::Stream
                   || InOutCooked.Channels == 0 || InOutCooked.Channels > MAX_CHANNELS
                   || InOutCooked.SampleRate == 0 || InOutCooked.SampleRate > MAX_RATE
                   || (bDecompressed && SampleCount != InOutCooked.Frames * InOutCooked.Channels)
                   || (!bDecompressed && SampleCount != 0)
                   || !std::isfinite(InOutCooked.Gain) || InOutCooked.Gain < 0.0f)
                {
                    Ar.SetError("sound: the cook is inconsistent");
                }
            }
        }
    }

    const char* GetSoundLoadModeName(const ESoundLoadMode InMode)
    {
        switch(InMode)
        {
            case ESoundLoadMode::Auto:         return "Auto";
            case ESoundLoadMode::Decompressed: return "Decompressed";
            case ESoundLoadMode::Compressed:   return "Compressed";
            case ESoundLoadMode::Stream:       return "Stream";
        }
        return "";
    }

    const char* GetSoundAttenuationName(const ESoundAttenuation InModel)
    {
        switch(InModel)
        {
            case ESoundAttenuation::None:        return "None";
            case ESoundAttenuation::Inverse:     return "Inverse";
            case ESoundAttenuation::Linear:      return "Linear";
            case ESoundAttenuation::Exponential: return "Exponential";
        }
        return "";
    }

    const char* GetSoundSourceFormatName(const ESoundSourceFormat InFormat)
    {
        switch(InFormat)
        {
            case ESoundSourceFormat::Unknown: return "Unknown";
            case ESoundSourceFormat::Wav:     return "WAV";
            case ESoundSourceFormat::Flac:    return "FLAC";
            case ESoundSourceFormat::Mp3:     return "MP3";
            case ESoundSourceFormat::Vorbis:  return "Ogg Vorbis";
        }
        return "";
    }

    ESoundSourceFormat DetectSoundSourceFormat(const uint8_t* InBytes, const size_t InSize)
    {
        if(!InBytes || InSize < 4) { return ESoundSourceFormat::Unknown; }
        const auto Starts = [InBytes, InSize](const char* InMagic)
        {
            const size_t Length = std::strlen(InMagic);
            return InSize >= Length && std::memcmp(InBytes, InMagic, Length) == 0;
        };
        if(Starts("RIFF") || Starts("RIFX") || Starts("RF64")) { return ESoundSourceFormat::Wav; }
        if(Starts("fLaC"))                                    { return ESoundSourceFormat::Flac; }
        if(Starts("OggS"))                                    { return ESoundSourceFormat::Vorbis; }
        if(Starts("ID3") || (InBytes[0] == 0xFF && (InBytes[1] & 0xE0) == 0xE0)) { return ESoundSourceFormat::Mp3; }
        return ESoundSourceFormat::Unknown;
    }

    uint64_t ComputeSoundCookKey(const SoundData& InData)
    {
        const SoundSettings& S = InData.Settings;
        uint64_t Hash = FnvValue(FNV_OFFSET, COOK_VERSION);
        Hash = FnvValue(Hash, uint64_t(InData.Source.size()));
        Hash = Fnv(Hash, InData.Source.data(), InData.Source.size());
        Hash = FnvValue(Hash, uint8_t(S.LoadMode));
        Hash = FnvValue(Hash, uint8_t(S.bForceMono));
        Hash = FnvValue(Hash, S.SampleRate);
        Hash = FnvValue(Hash, uint8_t(S.bTrimSilence));
        Hash = FnvValue(Hash, uint8_t(S.bNormalize));
        return Hash == 0 ? 1 : Hash;   // 0 is "never cooked"
    }

    bool SoundData::IsCooked() const
    {
        return Cooked.Key != 0 && !Source.empty() && Cooked.Key == ComputeSoundCookKey(*this);
    }

    bool CookSound(SoundData& InOutData, std::string& OutError)
    {
        DecodedPcm Pcm;
        if(!Decode(InOutData.Source, Pcm, OutError)) { return false; }

        InOutData.SourceFormat = DetectSoundSourceFormat(InOutData.Source.data(), InOutData.Source.size());
        const SoundSettings& S = InOutData.Settings;

        ESoundLoadMode Mode = S.LoadMode;
        if(Mode == ESoundLoadMode::Auto)
        {
            const double Seconds = double(Pcm.Frames()) / double(Pcm.SampleRate);
            if(Seconds > AUTO_STREAM_ABOVE)                                                       { Mode = ESoundLoadMode::Stream; }
            else if(Seconds < AUTO_DECOMPRESS_BELOW || InOutData.SourceFormat == ESoundSourceFormat::Wav) { Mode = ESoundLoadMode::Decompressed; }
            else                                                                                  { Mode = ESoundLoadMode::Compressed; }
        }

        CookedSound Cooked;
        Cooked.Mode = Mode;

        if(Mode == ESoundLoadMode::Decompressed)
        {
            if(S.bForceMono) { DownmixToMono(Pcm); }
            if(!Resample(Pcm, S.SampleRate != 0 ? S.SampleRate : DEFAULT_COOK_RATE, OutError)) { return false; }
            if(S.bTrimSilence) { TrimSilence(Pcm); }

            Cooked.Samples.resize(Pcm.Samples.size());
            std::transform(Pcm.Samples.begin(), Pcm.Samples.end(), Cooked.Samples.begin(), &ToS16);
        }

        Cooked.Channels   = Pcm.Channels;
        Cooked.SampleRate = Pcm.SampleRate;
        Cooked.Frames     = Pcm.Frames();
        Cooked.Peak       = PeakOf(Pcm);
        Cooked.Gain       = S.bNormalize && Cooked.Peak > 0.0f ? std::min(1.0f / Cooked.Peak, MAX_NORMALIZE_GAIN) : 1.0f;
        Cooked.Waveform   = WaveformOf(Pcm);
        Cooked.Key        = ComputeSoundCookKey(InOutData);

        InOutData.Cooked = std::move(Cooked);
        return true;
    }

    bool ImportSoundFile(const std::string& InSourceFile, SoundData& OutData, std::string& OutError)
    {
        std::ifstream File(InSourceFile, std::ios::binary);
        if(!File)
        {
            OutError = "cannot open " + InSourceFile;
            return false;
        }
        std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        if(Bytes.empty())
        {
            OutError = InSourceFile + " is empty";
            return false;
        }

        SoundData Data;
        Data.Source = std::move(Bytes);
        if(!CookSound(Data, OutError)) { return false; }

        /* A long sound is music or ambience until the author says otherwise:
         * heard the same everywhere, on the Music bus. Cooked again only if
         * that changed a cook setting -- it does not. */
        if(Data.Cooked.GetDuration() > AUTO_STREAM_ABOVE)
        {
            Data.Settings.bSpatial = false;
            Data.Settings.Bus      = EAudioBus::Music;
        }

        OutData = std::move(Data);
        return true;
    }

    bool SerializeSound(EngineCore::Archive& Ar, SoundData& InOutData, const ESoundLoad InLoad)
    {
        if(!OpenRequiredChunk(Ar, TAG_SOUND, 1, "not a sound (no NSND chunk)")) { return false; }

        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "sound: no SHDR chunk"))
        {
            SerializeHeader(Ar, InOutData);
            Ar.CloseChunk();
        }
        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_COOKED, 1, "sound: no SDAT chunk"))
        {
            SerializeCooked(Ar, InOutData.Cooked);
            Ar.CloseChunk();
        }

        /* SSRC: optional, and stepped over for playback unless played. */
        const bool bWantSource = InLoad == ESoundLoad::Everything || InOutData.Cooked.PlaysSource();
        if(!Ar.HasError() && (Ar.IsSaving() ? !InOutData.Source.empty() : Ar.PeekChunkTag() == TAG_SOURCE))
        {
            uint16_t Version = 1;
            if(Ar.OpenChunk(TAG_SOURCE, Version))
            {
                if(Ar.IsSaving() || bWantSource)
                {
                    uint64_t Size = uint64_t(InOutData.Source.size());
                    Ar << Size;
                    if(CanHold(Ar, Size, 1, "sound: implausible source size"))
                    {
                        if(Ar.IsLoading()) { InOutData.Source.resize(size_t(Size)); }
                        if(Size > 0) { Ar.SerializeBytes(InOutData.Source.data(), size_t(Size)); }
                    }
                }
                Ar.CloseChunk();
            }
        }
        if(!Ar.HasError() && Ar.IsLoading() && InOutData.Cooked.PlaysSource() && InOutData.Source.empty())
        {
            Ar.SetError("sound: its cook plays the source, and there is none");
        }

        /* ISRC, as RenderCore's assets carry it. */
        if(!Ar.HasError() && (Ar.IsSaving() ? !InOutData.Import.IsEmpty() : Ar.PeekChunkTag() == TAG_IMPORT_SOURCE))
        {
            uint16_t Version = 1;
            if(Ar.OpenChunk(TAG_IMPORT_SOURCE, Version))
            {
                Ar << InOutData.Import.SourcePath;
                Ar << InOutData.Import.SourceTime;
                Ar << InOutData.Import.Settings;
                Ar.CloseChunk();
            }
        }

        Ar.CloseChunk();
        return !Ar.HasError();
    }

    bool LoadSoundFile(const std::string& InPath, SoundData& OutData, const ESoundLoad InLoad)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutData = SoundData();
        if(!SerializeSound(Ar, OutData, InLoad))
        {
            CORE_LOG_WARN("[Audio] cannot load sound " + InPath)
            OutData = SoundData();
            return false;
        }
        return true;
    }

    bool SaveSoundFile(const std::string& InPath, SoundData& InData)
    {
        if(!InData.IsCooked())
        {
            if(std::string Error; !CookSound(InData, Error))
            {
                CORE_LOG_WARN("[Audio] cannot save sound " + InPath + ": " + Error)
                return false;
            }
        }

        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeSound(Ar, InData))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }

    // === SoundAssetCache ======================================================

    namespace
    {
        struct SoundCacheState
        {
            std::mutex                                                        Lock;
            std::unordered_map<std::string, std::shared_ptr<const SoundData>> Sounds;   // null: failed
            std::atomic<uint64_t>                                             Generation{ 1 };
        };

        /* Function-local, so it exists before any static initialiser asks. */
        SoundCacheState& GetSoundCache()
        {
            static SoundCacheState State;
            return State;
        }
    }

    std::shared_ptr<const SoundData> SoundAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty())
        {
            return nullptr;
        }

        SoundCacheState& Cache = GetSoundCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Sounds.find(InPath);
            if(Found != Cache.Sounds.end())
            {
                return Found->second;
            }
        }

        /* Loaded outside the lock; if two threads race, the first stored wins. */
        auto Loaded = std::make_shared<SoundData>();
        std::shared_ptr<const SoundData> Result;
        if(LoadSoundFile(InPath, *Loaded, ESoundLoad::ForPlayback))
        {
            Result = std::move(Loaded);
        }
        else
        {
            CORE_LOG_WARN("[Audio] sound '" + InPath + "' cannot be loaded; it will be silent")
        }

        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Sounds.emplace(InPath, Result).first->second;
    }

    void SoundAssetCache::Invalidate(const std::string& InPath)
    {
        SoundCacheState& Cache = GetSoundCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(InPath.empty())
        {
            Cache.Sounds.clear();
        }
        else
        {
            Cache.Sounds.erase(InPath);
        }
        ++Cache.Generation;
    }

    uint64_t SoundAssetCache::GetGeneration()
    {
        return GetSoundCache().Generation.load();
    }
}

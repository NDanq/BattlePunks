#include "Public/Sequencer/SequenceFile.h"

#include <cctype>
#include <cstring>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreFileProvider.h"

/*
 * The cooked .nmseq: "NMSQ", a version, then chunks (CoreArchive) -- HEAD the
 * rate and length, BIND the bindings with their tracks, TRAK the tracks of no
 * binding, MARK the markers. A chunk a newer build adds is stepped over.
 *
 * Version 2 added key tangents, track names and mute, sections and markers;
 * version 1 files still read.
 */
namespace Sequencer
{
    namespace
    {
        constexpr char     MAGIC[4]       = { 'N', 'M', 'S', 'Q' };
        constexpr uint32_t BINARY_VERSION = 2;

        constexpr uint32_t Tag(const char InA, const char InB, const char InC, const char InD)
        {
            return uint32_t(uint8_t(InA)) | (uint32_t(uint8_t(InB)) << 8) | (uint32_t(uint8_t(InC)) << 16) | (uint32_t(uint8_t(InD)) << 24);
        }
        constexpr uint32_t TAG_HEADER   = Tag('H', 'E', 'A', 'D');
        constexpr uint32_t TAG_BINDINGS = Tag('B', 'I', 'N', 'D');
        constexpr uint32_t TAG_TRACKS   = Tag('T', 'R', 'A', 'K');
        constexpr uint32_t TAG_MARKERS  = Tag('M', 'A', 'R', 'K');

        /* Past these a count is damage, not data. */
        constexpr uint32_t COUNT_MAX = 1000000;

        bool SerializeCount(EngineCore::Archive& Ar, uint32_t& InOutCount, const char* InWhat)
        {
            Ar << InOutCount;
            if(Ar.IsLoading() && InOutCount > COUNT_MAX)
            {
                Ar.SetError((std::string("an impossible ") + InWhat + " count").c_str());
                return false;
            }
            return !Ar.HasError();
        }

        void SerializeChannel(EngineCore::Archive& Ar, SequenceChannel& InOutChannel, const uint32_t InVersion)
        {
            uint32_t Count = uint32_t(InOutChannel.Keys.size());
            if(!SerializeCount(Ar, Count, "key")) { return; }
            if(Ar.IsLoading()) { InOutChannel.Keys.resize(Count); }
            for(SequenceKey& Key : InOutChannel.Keys)
            {
                uint8_t Interp = uint8_t(Key.Interp);
                Ar << Key.Frame;
                Ar << Key.Value;
                Ar << Interp;
                Key.Interp = Interp < uint8_t(ESequenceInterp::Count) ? ESequenceInterp(Interp) : ESequenceInterp::Auto;
                if(InVersion >= 2)
                {
                    Ar << Key.InTangent;
                    Ar << Key.OutTangent;
                }
            }
        }

        void SerializeTrack(EngineCore::Archive& Ar, SequenceTrack& InOutTrack, const uint32_t InVersion)
        {
            uint8_t Type = uint8_t(InOutTrack.Type);
            Ar << Type;
            if(Ar.IsLoading() && Type >= uint8_t(ESequenceTrackType::Count))
            {
                Ar.SetError("an unknown track type");
                return;
            }
            InOutTrack.Type = ESequenceTrackType(Type);
            Ar << InOutTrack.Component;
            Ar << InOutTrack.Field;

            uint32_t Channels = uint32_t(InOutTrack.Channels.size());
            if(!SerializeCount(Ar, Channels, "channel")) { return; }
            if(Ar.IsLoading()) { InOutTrack.Channels.resize(Channels); }
            for(SequenceChannel& Channel : InOutTrack.Channels) { SerializeChannel(Ar, Channel, InVersion); }

            uint32_t Events = uint32_t(InOutTrack.Events.size());
            if(!SerializeCount(Ar, Events, "event")) { return; }
            if(Ar.IsLoading()) { InOutTrack.Events.resize(Events); }
            for(SequenceEventKey& Event : InOutTrack.Events)
            {
                Ar << Event.Frame;
                Ar << Event.Event;
                Ar << Event.bFireOnSkip;
                uint32_t Args = uint32_t(Event.Args.size());
                if(!SerializeCount(Ar, Args, "argument")) { return; }
                if(Ar.IsLoading()) { Event.Args.resize(Args); }
                for(SequenceEventArg& Arg : Event.Args)
                {
                    uint8_t ArgType = uint8_t(Arg.Type);
                    Ar << ArgType;
                    Arg.Type = ArgType <= uint8_t(SequenceEventArg::EType::Binding) ? SequenceEventArg::EType(ArgType) : SequenceEventArg::EType::String;
                    Ar << Arg.Number;
                    Ar << Arg.bValue;
                    Ar << Arg.Text;
                }
            }

            if(InVersion < 2) { return; }
            Ar << InOutTrack.Name;
            Ar << InOutTrack.bMuted;
            uint32_t Sections = uint32_t(InOutTrack.Sections.size());
            if(!SerializeCount(Ar, Sections, "section")) { return; }
            if(Ar.IsLoading()) { InOutTrack.Sections.resize(Sections); }
            for(SequenceSection& Section : InOutTrack.Sections)
            {
                Ar << Section.Start;
                Ar << Section.End;
                Ar << Section.Asset;
                Ar << Section.PlayRate;
                Ar << Section.Volume;
                Ar << Section.Offset;
                Ar << Section.BlendIn;
                Ar << Section.BlendOut;
                Ar << Section.bLoop;
            }
        }

        void SerializeTracks(EngineCore::Archive& Ar, std::vector<SequenceTrack>& InOutTracks, const uint32_t InVersion)
        {
            uint32_t Count = uint32_t(InOutTracks.size());
            if(!SerializeCount(Ar, Count, "track")) { return; }
            if(Ar.IsLoading()) { InOutTracks.resize(Count); }
            for(SequenceTrack& Track : InOutTracks)
            {
                SerializeTrack(Ar, Track, InVersion);
                if(Ar.HasError()) { return; }
            }
        }

        void SerializeMarkers(EngineCore::Archive& Ar, std::vector<SequenceAsset::Marker>& InOutMarkers)
        {
            uint32_t Count = uint32_t(InOutMarkers.size());
            if(!SerializeCount(Ar, Count, "marker")) { return; }
            if(Ar.IsLoading()) { InOutMarkers.resize(Count); }
            for(SequenceAsset::Marker& Marker : InOutMarkers)
            {
                Ar << Marker.Frame;
                Ar << Marker.Name;
            }
        }

        void SerializeBindings(EngineCore::Archive& Ar, std::vector<SequenceBinding>& InOutBindings, const uint32_t InVersion)
        {
            uint32_t Count = uint32_t(InOutBindings.size());
            if(!SerializeCount(Ar, Count, "binding")) { return; }
            if(Ar.IsLoading()) { InOutBindings.resize(Count); }
            for(SequenceBinding& Binding : InOutBindings)
            {
                uint8_t Kind = uint8_t(Binding.Kind);
                Ar << Binding.Id;
                Ar << Binding.Name;
                Ar << Kind;
                Binding.Kind = Kind == uint8_t(ESequenceBindingKind::Spawn) ? ESequenceBindingKind::Spawn : ESequenceBindingKind::Possess;
                Ar << Binding.Guid.Hi;
                Ar << Binding.Guid.Lo;
                Ar << Binding.Prefab;
                SerializeTracks(Ar, Binding.Tracks, InVersion);
                if(Ar.HasError()) { return; }
            }
        }
    }

    ESequenceFileFormat DetectSequenceFileFormat(const std::vector<uint8_t>& InBytes)
    {
        if(InBytes.size() >= 4 && std::memcmp(InBytes.data(), MAGIC, 4) == 0) { return ESequenceFileFormat::Binary; }
        size_t At = 0;
        if(InBytes.size() >= 3 && InBytes[0] == 0xEF && InBytes[1] == 0xBB && InBytes[2] == 0xBF) { At = 3; }   // a UTF-8 BOM
        while(At < InBytes.size() && std::isspace(InBytes[At])) { ++At; }
        return At < InBytes.size() && InBytes[At] == '<' ? ESequenceFileFormat::Text : ESequenceFileFormat::Unknown;
    }

    std::vector<uint8_t> WriteSequenceBinary(const SequenceAsset& InAsset)
    {
        /* The archive serializes both ways through non-const references. */
        SequenceAsset Asset = InAsset;
        Asset.Normalize();

        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        char Magic[4];
        std::memcpy(Magic, MAGIC, 4);
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = BINARY_VERSION;
        Ar << Version;

        uint16_t ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_HEADER, ChunkVersion))
        {
            Ar << Asset.FrameRate;
            Ar << Asset.Length;
            Ar.CloseChunk();
        }
        ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_BINDINGS, ChunkVersion))
        {
            SerializeBindings(Ar, Asset.Bindings, BINARY_VERSION);
            Ar.CloseChunk();
        }
        ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_TRACKS, ChunkVersion))
        {
            SerializeTracks(Ar, Asset.Tracks, BINARY_VERSION);
            Ar.CloseChunk();
        }
        ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_MARKERS, ChunkVersion))
        {
            SerializeMarkers(Ar, Asset.Markers);
            Ar.CloseChunk();
        }
        return Ar.GetBytes();
    }

    bool ReadSequenceBinary(const std::vector<uint8_t>& InBytes, SequenceAsset& OutAsset, std::string* OutError)
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) { *OutError = InWhy; } return false; };
        if(DetectSequenceFileFormat(InBytes) != ESequenceFileFormat::Binary) { return Fail("is not a cooked .nmseq"); }

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        char Magic[4];
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = 0;
        Ar << Version;
        if(Version == 0 || Version > BINARY_VERSION)
        {
            return Fail("was cooked by a newer build (version " + std::to_string(Version) + ")");
        }

        SequenceAsset Asset;
        while(!Ar.HasError() && !Ar.IsAtEnd())
        {
            uint16_t ChunkVersion = 0;
            if(Ar.OpenChunk(TAG_HEADER, ChunkVersion))
            {
                Ar << Asset.FrameRate;
                Ar << Asset.Length;
                Ar.CloseChunk();
            }
            else if(Ar.OpenChunk(TAG_BINDINGS, ChunkVersion))
            {
                SerializeBindings(Ar, Asset.Bindings, Version);
                Ar.CloseChunk();
            }
            else if(Ar.OpenChunk(TAG_TRACKS, ChunkVersion))
            {
                SerializeTracks(Ar, Asset.Tracks, Version);
                Ar.CloseChunk();
            }
            else if(Ar.OpenChunk(TAG_MARKERS, ChunkVersion))
            {
                SerializeMarkers(Ar, Asset.Markers);
                Ar.CloseChunk();
            }
            else
            {
                /* A chunk a newer build added: stepped over. */
                const uint32_t Unknown = Ar.PeekChunkTag();
                if(Unknown == 0 || !Ar.OpenChunk(Unknown, ChunkVersion)) { break; }
                Ar.CloseChunk();
            }
        }
        if(Ar.HasError()) { return Fail("is damaged"); }
        Asset.Normalize();
        OutAsset = std::move(Asset);
        return true;
    }

    bool ReadSequence(const std::vector<uint8_t>& InBytes, SequenceAsset& OutAsset, std::string* OutError)
    {
        switch(DetectSequenceFileFormat(InBytes))
        {
            case ESequenceFileFormat::Binary:
                return ReadSequenceBinary(InBytes, OutAsset, OutError);
            case ESequenceFileFormat::Text:
#if WITH_UNCOOKED
                return ParseSequenceXml(std::string(InBytes.begin(), InBytes.end()), OutAsset, OutError);
#else
                if(OutError) { *OutError = "is XML; a Shipping game reads only the cooked form (Build > Cook Content)"; }
                return false;
#endif
            case ESequenceFileFormat::Unknown:
                break;
        }
        if(OutError) { *OutError = "is neither a .nmseq sequence nor a cooked one"; }
        return false;
    }

    bool LoadSequence(const std::string& InPath, SequenceAsset& OutAsset, std::string* OutError)
    {
        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        std::vector<uint8_t> Bytes;
        if(!Provider || !Provider->ReadAll(InPath, Bytes))
        {
            if(OutError) { *OutError = "cannot be read"; }
            return false;
        }
        return ReadSequence(Bytes, OutAsset, OutError);
    }
}

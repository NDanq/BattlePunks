#include "Public/Sequencer/SequenceAsset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "Public/Core/CoreLog.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Public/Sequencer/SequenceFile.h"

/* The asset's arithmetic: easing, sampling a channel, names, validation, and
 * the cache players load through. */
namespace Sequencer
{
    namespace
    {
        constexpr float PI = 3.14159265358979323846f;

        const char* const INTERP_NAMES[] =
        {
            "Constant", "Linear", "Auto", "QuadIn", "QuadOut", "QuadInOut",
            "CubicIn", "CubicOut", "CubicInOut", "BackOut", "ElasticOut", "User",
        };
        static_assert(sizeof(INTERP_NAMES) / sizeof(INTERP_NAMES[0]) == size_t(ESequenceInterp::Count), "one name each");

        const char* const TRACK_NAMES[] = { "Transform", "Float", "Bool", "Event", "Animation", "Audio", "CameraCut", "Fade", "SubSequence", "PostProcess", "PostProcessSetting", "BusVolume",
                                             "Subtitle" };
        static_assert(sizeof(TRACK_NAMES) / sizeof(TRACK_NAMES[0]) == size_t(ESequenceTrackType::Count), "one name each");

        const char* const CHANNEL_NAMES[] =
        {
            "Position.X", "Position.Y", "Position.Z",
            "Rotation.X", "Rotation.Y", "Rotation.Z",
            "Scale.X", "Scale.Y", "Scale.Z",
        };
        static_assert(sizeof(CHANNEL_NAMES) / sizeof(CHANNEL_NAMES[0]) == TC_Count, "one name each");

        /* The slope an Auto key leaves with, in value per frame: the line
         * through its neighbours, flat at either end and at a peak or a
         * trough, so a curve through keys never overshoots them. */
        float AutoSlope(const std::vector<SequenceKey>& InKeys, const size_t InIndex)
        {
            if(InIndex == 0 || InIndex + 1 >= InKeys.size())
            {
                return 0.0f;
            }
            const SequenceKey& Prev = InKeys[InIndex - 1];
            const SequenceKey& Here = InKeys[InIndex];
            const SequenceKey& Next = InKeys[InIndex + 1];
            if((Here.Value - Prev.Value) * (Next.Value - Here.Value) <= 0.0f)
            {
                return 0.0f;
            }
            return (Next.Value - Prev.Value) / float(Next.Frame - Prev.Frame);
        }
    }

    const char* InterpName(const ESequenceInterp InInterp)
    {
        const size_t Index = size_t(InInterp);
        return Index < size_t(ESequenceInterp::Count) ? INTERP_NAMES[Index] : "Auto";
    }

    bool ParseInterp(const char* InName, ESequenceInterp& OutInterp)
    {
        if(!InName) { return false; }
        for(size_t Index = 0; Index < size_t(ESequenceInterp::Count); ++Index)
        {
            if(std::strcmp(InName, INTERP_NAMES[Index]) == 0)
            {
                OutInterp = ESequenceInterp(Index);
                return true;
            }
        }
        return false;
    }

    const char* TrackTypeName(const ESequenceTrackType InType)
    {
        const size_t Index = size_t(InType);
        return Index < size_t(ESequenceTrackType::Count) ? TRACK_NAMES[Index] : "Transform";
    }

    bool ParseTrackType(const char* InName, ESequenceTrackType& OutType)
    {
        if(!InName) { return false; }
        for(size_t Index = 0; Index < size_t(ESequenceTrackType::Count); ++Index)
        {
            if(std::strcmp(InName, TRACK_NAMES[Index]) == 0)
            {
                OutType = ESequenceTrackType(Index);
                return true;
            }
        }
        return false;
    }

    const char* TransformChannelName(const size_t InChannel)
    {
        return InChannel < TC_Count ? CHANNEL_NAMES[InChannel] : "";
    }

    size_t FindTransformChannel(const char* InName)
    {
        if(!InName) { return TC_Count; }
        for(size_t Index = 0; Index < TC_Count; ++Index)
        {
            if(std::strcmp(InName, CHANNEL_NAMES[Index]) == 0) { return Index; }
        }
        return TC_Count;
    }

    bool IsSectionTrack(const ESequenceTrackType InType)
    {
        return InType == ESequenceTrackType::Animation || InType == ESequenceTrackType::Audio
            || InType == ESequenceTrackType::CameraCut || InType == ESequenceTrackType::SubSequence
            || InType == ESequenceTrackType::PostProcess || InType == ESequenceTrackType::Subtitle;
    }

    namespace
    {
        const char* const BUS_NAMES[SEQUENCE_BUS_COUNT] = { "Master", "Music", "SFX", "UI", "Voice", "Ambient" };
    }

    const char* SequenceBusName(const int32_t InBus)
    {
        return InBus >= 0 && InBus < SEQUENCE_BUS_COUNT ? BUS_NAMES[InBus] : "";
    }

    int32_t FindSequenceBus(const char* InName)
    {
        if(!InName) { return -1; }
        for(int32_t Index = 0; Index < SEQUENCE_BUS_COUNT; ++Index)
        {
            if(std::strcmp(InName, BUS_NAMES[Index]) == 0) { return Index; }
        }
        return -1;
    }

    float Ease(const ESequenceInterp InInterp, float InT)
    {
        InT = std::clamp(InT, 0.0f, 1.0f);
        switch(InInterp)
        {
            case ESequenceInterp::Constant:   return 0.0f;
            case ESequenceInterp::Linear:     return InT;
            case ESequenceInterp::Auto:
            case ESequenceInterp::User:       return InT;
            case ESequenceInterp::QuadIn:     return InT * InT;
            case ESequenceInterp::QuadOut:    return 1.0f - (1.0f - InT) * (1.0f - InT);
            case ESequenceInterp::QuadInOut:  return InT < 0.5f ? 2.0f * InT * InT : 1.0f - std::pow(-2.0f * InT + 2.0f, 2.0f) * 0.5f;
            case ESequenceInterp::CubicIn:    return InT * InT * InT;
            case ESequenceInterp::CubicOut:   return 1.0f - std::pow(1.0f - InT, 3.0f);
            case ESequenceInterp::CubicInOut: return InT < 0.5f ? 4.0f * InT * InT * InT : 1.0f - std::pow(-2.0f * InT + 2.0f, 3.0f) * 0.5f;
            case ESequenceInterp::BackOut:
            {
                const float C1 = 1.70158f, C3 = C1 + 1.0f;
                return 1.0f + C3 * std::pow(InT - 1.0f, 3.0f) + C1 * std::pow(InT - 1.0f, 2.0f);
            }
            case ESequenceInterp::ElasticOut:
                if(InT <= 0.0f || InT >= 1.0f) { return InT; }
                return std::pow(2.0f, -10.0f * InT) * std::sin((InT * 10.0f - 0.75f) * (2.0f * PI / 3.0f)) + 1.0f;
            case ESequenceInterp::Count:
                break;
        }
        return InT;
    }

    // === SequenceChannel ===================================================

    float SequenceChannel::Evaluate(const double InFrame, const float InDefault) const
    {
        if(Keys.empty())                    { return InDefault; }
        if(InFrame <= double(Keys.front().Frame)) { return Keys.front().Value; }
        if(InFrame >= double(Keys.back().Frame))  { return Keys.back().Value; }

        /* The last key at or before InFrame. */
        const auto After = std::upper_bound(Keys.begin(), Keys.end(), InFrame,
                                            [](const double InAt, const SequenceKey& InKey) { return InAt < double(InKey.Frame); });
        const size_t Index = size_t(After - Keys.begin()) - 1;
        const SequenceKey& A = Keys[Index];
        const SequenceKey& B = Keys[Index + 1];

        const float Span = float(B.Frame - A.Frame);
        const float T    = float((InFrame - double(A.Frame)) / double(Span));

        if(A.Interp == ESequenceInterp::Auto || A.Interp == ESequenceInterp::User)
        {
            /* Cubic Hermite, slopes scaled to the segment. */
            const float M0 = GetLeaveSlope(Index) * Span;
            const float M1 = GetArriveSlope(Index + 1) * Span;
            const float T2 = T * T, T3 = T2 * T;
            return (2.0f * T3 - 3.0f * T2 + 1.0f) * A.Value + (T3 - 2.0f * T2 + T) * M0
                 + (-2.0f * T3 + 3.0f * T2) * B.Value + (T3 - T2) * M1;
        }
        return A.Value + (B.Value - A.Value) * Ease(A.Interp, T);
    }

    float SequenceChannel::GetLeaveSlope(const size_t InIndex) const
    {
        if(InIndex >= Keys.size()) { return 0.0f; }
        return Keys[InIndex].Interp == ESequenceInterp::User ? Keys[InIndex].OutTangent : AutoSlope(Keys, InIndex);
    }

    float SequenceChannel::GetArriveSlope(const size_t InIndex) const
    {
        if(InIndex >= Keys.size()) { return 0.0f; }
        return Keys[InIndex].Interp == ESequenceInterp::User ? Keys[InIndex].InTangent : AutoSlope(Keys, InIndex);
    }

    void SequenceChannel::Normalize()
    {
        /* Stable, so of two keys on a frame the later one in the list wins. */
        std::stable_sort(Keys.begin(), Keys.end(), [](const SequenceKey& L, const SequenceKey& R) { return L.Frame < R.Frame; });
        std::vector<SequenceKey> Unique;
        Unique.reserve(Keys.size());
        for(const SequenceKey& Key : Keys)
        {
            if(!Unique.empty() && Unique.back().Frame == Key.Frame) { Unique.back() = Key; }
            else                                                    { Unique.push_back(Key); }
        }
        Keys.swap(Unique);
    }

    // === SequenceEventArg ==================================================

    SequenceEventArg SequenceEventArg::FromText(const std::string& InText)
    {
        SequenceEventArg Arg;
        if(InText == "true" || InText == "false")
        {
            Arg.Type   = EType::Bool;
            Arg.bValue = InText == "true";
            return Arg;
        }
        if(InText.size() > 1 && InText[0] == '@')
        {
            Arg.Type = EType::Binding;
            Arg.Text = InText.substr(1);
            return Arg;
        }
        if(!InText.empty())
        {
            char* End = nullptr;
            const double Number = std::strtod(InText.c_str(), &End);
            if(End && *End == '\0' && std::isfinite(Number))
            {
                Arg.Type   = SequenceEventArg::EType::Number;
                Arg.Number = Number;
                return Arg;
            }
        }
        Arg.Type = EType::String;
        Arg.Text = InText;
        return Arg;
    }

    std::string SequenceEventArg::ToText() const
    {
        switch(Type)
        {
            case EType::Number:
            {
                char Buffer[32];
                std::snprintf(Buffer, sizeof(Buffer), "%.9g", Number);
                return Buffer;
            }
            case EType::Bool:    return bValue ? "true" : "false";
            case EType::Binding: return "@" + Text;
            case EType::String:  break;
        }
        return Text;
    }

    // === SequenceTrack =====================================================

    void SequenceTrack::Normalize()
    {
        size_t Wanted = 0;
        switch(Type)
        {
            case ESequenceTrackType::Transform: Wanted = TC_Count; break;
            case ESequenceTrackType::Float:
            case ESequenceTrackType::Bool:
            case ESequenceTrackType::Fade:
            case ESequenceTrackType::PostProcessSetting:
            case ESequenceTrackType::BusVolume: Wanted = 1; break;
            default:                            Wanted = 0; break;
        }
        Channels.resize(Wanted);
        for(SequenceChannel& Channel : Channels)
        {
            Channel.Normalize();
            if(Type == ESequenceTrackType::Bool)
            {
                /* A flag has no in-between. */
                for(SequenceKey& Key : Channel.Keys)
                {
                    Key.Value  = Key.Value != 0.0f ? 1.0f : 0.0f;
                    Key.Interp = ESequenceInterp::Constant;
                }
            }
        }
        if(Type != ESequenceTrackType::Event)
        {
            Events.clear();
        }
        std::stable_sort(Events.begin(), Events.end(), [](const SequenceEventKey& L, const SequenceEventKey& R) { return L.Frame < R.Frame; });

        if(!IsSectionTrack(Type))
        {
            Sections.clear();
        }
        for(SequenceSection& Section : Sections)
        {
            if(Section.End < Section.Start) { std::swap(Section.Start, Section.End); }
            Section.Offset   = std::max(Section.Offset, 0);
            Section.BlendIn  = std::clamp(Section.BlendIn, 0, Section.GetLength());
            Section.BlendOut = std::clamp(Section.BlendOut, 0, Section.GetLength());
            if(Section.Bus < -1 || Section.Bus >= SEQUENCE_BUS_COUNT) { Section.Bus = -1; }
            if(Section.Spatial < -1 || Section.Spatial > 1)           { Section.Spatial = -1; }
            if(!(Section.PlayRate > 0.0f)) { Section.PlayRate = 1.0f; }
            if(!(Section.Volume >= 0.0f))  { Section.Volume = 1.0f; }
        }
        std::stable_sort(Sections.begin(), Sections.end(), [](const SequenceSection& L, const SequenceSection& R) { return L.Start < R.Start; });
        for(SequenceEventKey& Event : Events)
        {
            if(Event.Args.size() > SEQUENCE_EVENT_ARGS_MAX) { Event.Args.resize(SEQUENCE_EVENT_ARGS_MAX); }
        }
    }

    int32_t SequenceTrack::GetLastFrame() const
    {
        int32_t Last = -1;
        for(const SequenceChannel& Channel : Channels)
        {
            if(!Channel.Keys.empty()) { Last = std::max(Last, Channel.Keys.back().Frame); }
        }
        if(!Events.empty()) { Last = std::max(Last, Events.back().Frame); }
        for(const SequenceSection& Section : Sections) { Last = std::max(Last, Section.End); }
        return Last;
    }

    // === SequenceAsset =====================================================

    int32_t SequenceAsset::FindBinding(const std::string& InId) const
    {
        for(size_t Index = 0; Index < Bindings.size(); ++Index)
        {
            if(Bindings[Index].Id == InId) { return int32_t(Index); }
        }
        return -1;
    }

    int32_t SequenceAsset::FindMarker(const std::string& InName) const
    {
        for(size_t Index = 0; Index < Markers.size(); ++Index)
        {
            if(Markers[Index].Name == InName) { return int32_t(Index); }
        }
        return -1;
    }

    void SequenceAsset::Normalize()
    {
        std::stable_sort(Markers.begin(), Markers.end(), [](const Marker& L, const Marker& R) { return L.Frame < R.Frame; });
        FrameRate = std::clamp<uint32_t>(FrameRate, 1, 1000);
        if(Length < 0) { Length = 0; }
        for(SequenceBinding& Binding : Bindings)
        {
            for(SequenceTrack& Track : Binding.Tracks) { Track.Normalize(); }
        }
        for(SequenceTrack& Track : Tracks) { Track.Normalize(); }
    }

    std::vector<std::string> SequenceAsset::GetReferencedAssets() const
    {
        std::vector<std::string> Out;
        const auto Add = [&Out](const std::string& InPath)
        {
            if(!InPath.empty() && std::find(Out.begin(), Out.end(), InPath) == Out.end()) { Out.push_back(InPath); }
        };
        const auto FromTrack = [&Add](const SequenceTrack& InTrack)
        {
            if(InTrack.Type == ESequenceTrackType::CameraCut || InTrack.Type == ESequenceTrackType::Subtitle) { return; }   // not files
            for(const SequenceSection& Section : InTrack.Sections) { Add(Section.Asset); }
        };
        for(const SequenceBinding& Binding : Bindings)
        {
            if(Binding.Kind == ESequenceBindingKind::Spawn && Binding.Prefab.rfind("native:", 0) != 0) { Add(Binding.Prefab); }
            for(const SequenceTrack& Track : Binding.Tracks) { FromTrack(Track); }
        }
        for(const SequenceTrack& Track : Tracks) { FromTrack(Track); }
        return Out;
    }

    std::vector<std::string> SequenceAsset::Validate() const
    {
        std::vector<std::string> Problems;
        if(Length <= 0) { Problems.push_back("its length is 0 frames"); }

        std::unordered_set<std::string> Ids;
        for(const SequenceBinding& Binding : Bindings)
        {
            const std::string Who = "binding '" + Binding.Id + "'";
            if(Binding.Id.empty())                   { Problems.push_back("a binding has no id"); }
            else if(!Ids.insert(Binding.Id).second)  { Problems.push_back(Who + " is declared twice"); }

            if(Binding.Kind == ESequenceBindingKind::Possess && !Binding.Guid.IsSet() && Binding.Name.empty())
            {
                Problems.push_back(Who + " names no entity (no guid and no name)");
            }
            if(Binding.Kind == ESequenceBindingKind::Spawn && Binding.Prefab.empty())
            {
                Problems.push_back(Who + " spawns no prefab");
            }
            for(const SequenceTrack& Track : Binding.Tracks)
            {
                if((Track.Type == ESequenceTrackType::Float || Track.Type == ESequenceTrackType::Bool)
                   && (Track.Component.empty() || Track.Field.empty()))
                {
                    Problems.push_back(Who + ": a " + TrackTypeName(Track.Type) + " track names no component field");
                }
                if(Track.GetLastFrame() > Length)
                {
                    Problems.push_back(Who + ": a " + TrackTypeName(Track.Type) + " track has keys past the end");
                }
                if(Track.Type == ESequenceTrackType::CameraCut || Track.Type == ESequenceTrackType::Fade
                   || Track.Type == ESequenceTrackType::SubSequence || Track.Type == ESequenceTrackType::PostProcess
                   || Track.Type == ESequenceTrackType::PostProcessSetting || Track.Type == ESequenceTrackType::BusVolume
                   || Track.Type == ESequenceTrackType::Subtitle)
                {
                    Problems.push_back(Who + ": a " + TrackTypeName(Track.Type) + " track belongs to no binding (put it at the top level)");
                }
            }
        }
        for(const SequenceTrack& Track : Tracks)
        {
            if(Track.Type == ESequenceTrackType::Transform || Track.Type == ESequenceTrackType::Float
               || Track.Type == ESequenceTrackType::Bool || Track.Type == ESequenceTrackType::Animation)
            {
                Problems.push_back(std::string("a ") + TrackTypeName(Track.Type) + " track has no binding to animate");
            }
            if(Track.GetLastFrame() > Length)
            {
                Problems.push_back(std::string("a ") + TrackTypeName(Track.Type) + " track has keys past the end");
            }
            RenderCore::EPostProcessParam Param;
            if(Track.Type == ESequenceTrackType::PostProcessSetting && !RenderCore::FindPostProcessParam(Track.Field.c_str(), Param))
            {
                Problems.push_back("a PostProcessSetting track names '" + Track.Field + "', which is not a post process setting");
            }
            if(Track.Type == ESequenceTrackType::BusVolume && FindSequenceBus(Track.Field.c_str()) < 0)
            {
                Problems.push_back("a BusVolume track names '" + Track.Field + "', which is not a bus (Master, Music, SFX, UI, Voice, Ambient)");
            }
        }

        /* Sections: something to play, and a camera that is a binding. */
        const auto CheckSections = [&](const SequenceTrack& InTrack)
        {
            for(const SequenceSection& Section : InTrack.Sections)
            {
                const std::string Where = std::string(TrackTypeName(InTrack.Type)) + " section at frame " + std::to_string(Section.Start);
                if(Section.GetLength() == 0) { Problems.push_back(Where + " is empty"); }
                if(Section.Asset.empty())    { Problems.push_back(Where + " names nothing to play"); }
                else if(InTrack.Type == ESequenceTrackType::CameraCut && Ids.find(Section.Asset) == Ids.end())
                {
                    Problems.push_back(Where + " cuts to '" + Section.Asset + "', which is not a binding");
                }
            }
        };

        /* Every @id an event names is a binding. */
        const auto CheckEvents = [&](const SequenceTrack& InTrack)
        {
            for(const SequenceEventKey& Event : InTrack.Events)
            {
                if(Event.Event.empty()) { Problems.push_back("an event key at frame " + std::to_string(Event.Frame) + " has no name"); }
                for(const SequenceEventArg& Arg : Event.Args)
                {
                    if(Arg.Type == SequenceEventArg::EType::Binding && Ids.find(Arg.Text) == Ids.end())
                    {
                        Problems.push_back("event '" + Event.Event + "' names @" + Arg.Text + ", which is not a binding");
                    }
                }
            }
        };
        for(const SequenceBinding& Binding : Bindings)
        {
            for(const SequenceTrack& Track : Binding.Tracks) { CheckEvents(Track); CheckSections(Track); }
        }
        for(const SequenceTrack& Track : Tracks) { CheckEvents(Track); CheckSections(Track); }
        return Problems;
    }

    // === SequenceAssetCache ================================================

    namespace
    {
        struct CacheEntry
        {
            std::shared_ptr<const SequenceAsset> Asset;
            bool bFailed = false;
        };

        std::unordered_map<std::string, CacheEntry>& Cache()
        {
            static std::unordered_map<std::string, CacheEntry> Entries;
            return Entries;
        }
    }

    std::shared_ptr<const SequenceAsset> SequenceAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty()) { return nullptr; }
        CacheEntry& Entry = Cache()[InPath];
        if(Entry.Asset || Entry.bFailed) { return Entry.Asset; }

        auto Loaded = std::make_shared<SequenceAsset>();
        std::string Error;
        if(!LoadSequence(InPath, *Loaded, &Error))
        {
            Entry.bFailed = true;
            CORE_LOG_ERROR("[Sequencer] '" + InPath + "' " + Error)
            return nullptr;
        }
        for(const std::string& Problem : Loaded->Validate())
        {
            CORE_LOG_WARN("[Sequencer] '" + InPath + "': " + Problem)
        }
        Entry.Asset = std::move(Loaded);
        return Entry.Asset;
    }

    void SequenceAssetCache::Invalidate(const std::string& InPath)
    {
        Cache().erase(InPath);
    }

    void SequenceAssetCache::Clear()
    {
        Cache().clear();
    }

    void SequenceAssetCache::Set(const std::string& InPath, std::shared_ptr<const SequenceAsset> InAsset)
    {
        CacheEntry& Entry = Cache()[InPath];
        Entry.Asset   = std::move(InAsset);
        Entry.bFailed = !Entry.Asset;
    }
}

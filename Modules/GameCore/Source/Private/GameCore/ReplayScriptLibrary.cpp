#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Replay.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/GameCoreLog.h"

/*
 * The "Replay" script library (Replay.h, and Docs/Guides/ReplayGuide.md):
 * one recording and one playback per World, for Lua and script graphs.
 *
 *     Replay.RecordOnly("Transform,Render,Animation")   -- optional; "" records every serialized type
 *     Replay.StartRecording(20, 30)                      -- keep the last 20 s, 30 samples a second
 *     ...
 *     Replay.StopRecording()
 *     Replay.Play(true)                                  -- hold: the clip wins over the live level
 *     Replay.SetRate(0.25)                               -- slow motion; negative rewinds
 *     Events.On(Replay.ReplayFinished, function() Replay.Stop(true) end)
 *
 * Playback runs in a TP_PostUpdate system ahead of the phase's scripts (and
 * so ahead of RenderCore.Animation), after every gameplay system. The state
 * goes when the World does (World::OnWorldDestroyed).
 */

namespace GameCore
{
namespace
{
    constexpr const char* FINISHED_EVENT = "ReplayFinished";

    struct ReplaySession
    {
        World*                          TheWorld = nullptr;
        ReplayClip                      Clip;
        std::unique_ptr<ReplayRecorder> Recorder;
        std::unique_ptr<ReplayPlayer>   Player;
        std::vector<std::string>        Only;   // RecordOnly's types; empty for all
        double                          Time    = 0.0;
        float                           Rate    = 1.0f;
        bool                            bPlaying = false;
        bool                            bPaused  = false;
        bool                            bLoop    = false;
    };

    std::unordered_map<World*, std::unique_ptr<ReplaySession>>& Sessions()
    {
        static std::unordered_map<World*, std::unique_ptr<ReplaySession>> Map;
        return Map;
    }

    void PlaybackSystem(World& InWorld, float InSeconds, void* InUserData);

    /* The playback system stays registered for the World's life (idle when
     * nothing plays), registered once in SessionFor; Stop only drops the player. */
    void StopPlayback(ReplaySession& S, const bool bInDestroySpawned)
    {
        if(S.Player && bInDestroySpawned) { S.Player->DestroySpawnedEntities(); }
        S.Player.reset();
        S.bPlaying = false;
        S.bPaused  = false;
    }

    void OnWorldDestroyed(World* InWorld, void*)
    {
        auto Found = Sessions().find(InWorld);
        if(Found == Sessions().end()) { return; }
        ReplaySession& S = *Found->second;
        InWorld->UnregisterSystem(&PlaybackSystem, &S);
        StopPlayback(S, false);
        if(S.Recorder) { S.Recorder->Stop(); }
        Sessions().erase(Found);
    }

    ReplaySession& SessionFor(World& InWorld)
    {
        auto& Map = Sessions();
        auto Found = Map.find(&InWorld);
        if(Found != Map.end()) { return *Found->second; }
        auto Made = std::make_unique<ReplaySession>();
        Made->TheWorld = &InWorld;
        InWorld.OnWorldDestroyed().Add(&OnWorldDestroyed);
        ReplaySession& S = *Map.emplace(&InWorld, std::move(Made)).first->second;
        InWorld.RegisterSystem("Replay.Playback", TP_PostUpdate, &PlaybackSystem, &S, SO_BeforeScripts);
        return S;
    }

    void PlaybackSystem(World& InWorld, const float InSeconds, void* InUserData)
    {
        ReplaySession& S = *static_cast<ReplaySession*>(InUserData);
        if(!S.bPlaying || !S.Player || S.Clip.IsEmpty()) { return; }

        const double Start = S.Clip.GetStartTime(), End = S.Clip.GetEndTime(), Length = End - Start;
        bool bFinished = false;
        if(!S.bPaused)
        {
            S.Time += double(InSeconds) * double(S.Rate);
            if(S.Time > End || S.Time < Start)
            {
                if(S.bLoop && Length > 0.0)
                {
                    S.Time = Start + std::fmod(std::fmod(S.Time - Start, Length) + Length, Length);
                }
                else
                {
                    S.Time = std::clamp(S.Time, Start, End);
                    bFinished = true;
                }
            }
        }
        S.Player->Seek(S.Time);
        if(bFinished)
        {
            S.bPaused = true;   // held on the last frame shown, until Stop or another Play
            InWorld.GetEvents().Emit(FINISHED_EVENT);
        }
    }

    double Seconds(ScriptCall& Call, const int InArg, const double InDefault)
    {
        return Call.HasArg(InArg) ? Call.GetNumber(InArg) : InDefault;
    }
}
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Replay)
{
    using namespace GameCore;

    // --- Recording ----------------------------------------------------------

    Lib.Function("RecordOnly", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        S.Only.clear();
        std::string List = Call.GetString(1);
        size_t At = 0;
        while(At <= List.size())
        {
            const size_t Comma = List.find(',', At);
            std::string Name = List.substr(At, Comma == std::string::npos ? std::string::npos : Comma - At);
            Name.erase(0, Name.find_first_not_of(' '));
            Name.erase(Name.find_last_not_of(' ') + 1);
            if(!Name.empty()) { S.Only.push_back(Name); }
            if(Comma == std::string::npos) { break; }
            At = Comma + 1;
        }
    }, "Components:string").Tooltip("Record only these component types (comma-separated names); \"\" records every serialized type");

    Lib.Function("StartRecording", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        StopPlayback(S, false);
        ReplayRecordSettings Settings;
        Settings.MaxDuration = float(std::max(0.0, Seconds(Call, 1, 0.0)));
        const double Rate = Seconds(Call, 2, 0.0);
        if(Rate > 0.0) { Settings.SampleRate = float(Rate); }
        S.Recorder = std::make_unique<ReplayRecorder>(S.Clip, Settings);
        S.Recorder->RecordOnly(S.Only);
        S.Recorder->Start(Call.GetWorld());
    }, "MaxSeconds:number, SampleRate:number")
       .Tooltip("Starts a new recording. MaxSeconds > 0 keeps only the last that many seconds; SampleRate 0 is 30 a second");

    Lib.Function("StopRecording", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        if(S.Recorder) { S.Recorder->Stop(); }
        S.Recorder.reset();
    }).Tooltip("Stops recording; the clip stays, to play or save");

    Lib.Pure("IsRecording", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        Call.PushBool(S.Recorder && S.Recorder->IsRecording());
    }, "-> Recording:bool");

    Lib.Pure("GetDuration", [](ScriptCall& Call)
    {
        Call.PushNumber(SessionFor(Call.GetWorld()).Clip.GetDuration());
    }, "-> Seconds:number").Tooltip("The clip's length, in seconds");

    Lib.Pure("GetSizeBytes", [](ScriptCall& Call)
    {
        Call.PushInteger(int64_t(SessionFor(Call.GetWorld()).Clip.GetPayloadBytes()));
    }, "-> Bytes:integer").Tooltip("The clip's recorded state, in bytes");

    Lib.Function("Save", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        const char* Path = Call.GetString(1);
        Call.PushBool(Path && *Path && !S.Clip.IsEmpty() && SaveReplayClip(Path, S.Clip));
    }, "Path:string -> Ok:bool").Tooltip("Writes the clip to a file (Saved/Replays/Name.nmreplay)");

    Lib.Function("Load", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        StopPlayback(S, false);
        if(S.Recorder) { S.Recorder->Stop(); S.Recorder.reset(); }
        const char* Path = Call.GetString(1);
        const bool bLoaded = Path && *Path && LoadReplayClip(Path, S.Clip);
        if(!bLoaded) { S.Clip.Clear(); }
        Call.PushBool(bLoaded);
    }, "Path:string -> Ok:bool").Tooltip("Replaces the clip with a saved one; false leaves no clip");

    // --- Playback -----------------------------------------------------------

    Lib.Function("Play", [](ScriptCall& Call)
    {
        World& W = Call.GetWorld();
        ReplaySession& S = SessionFor(W);
        if(S.Recorder) { S.Recorder->Stop(); S.Recorder.reset(); }
        StopPlayback(S, false);
        if(S.Clip.IsEmpty()) { Call.Error("Replay.Play: nothing recorded or loaded"); return; }
        S.Player = std::make_unique<ReplayPlayer>(W, S.Clip);
        S.Player->SetHold(Call.HasArg(1) && Call.GetBool(1));
        S.Time     = S.Clip.GetStartTime();
        S.bPlaying = true;
        S.bPaused  = false;
        S.Player->Seek(S.Time);
    }, "Hold:bool").Tooltip("Plays the clip from its start. Hold: write every frame whole, to play over a live level");

    Lib.Function("Stop", [](ScriptCall& Call)
    {
        StopPlayback(SessionFor(Call.GetWorld()), Call.HasArg(1) && Call.GetBool(1));
    }, "DestroySpawned:bool").Tooltip("Stops playback; DestroySpawned removes the entities playback created");

    Lib.Function("Pause",  [](ScriptCall& Call) { SessionFor(Call.GetWorld()).bPaused = true; });
    Lib.Function("Resume", [](ScriptCall& Call) { SessionFor(Call.GetWorld()).bPaused = false; });

    Lib.Function("SetRate", [](ScriptCall& Call)
    {
        SessionFor(Call.GetWorld()).Rate = float(Call.GetNumber(1));
    }, "Rate:number").Tooltip("Replay seconds per second: 0.25 slow motion, -1 rewinds, 0 freezes");

    Lib.Function("SetLooping", [](ScriptCall& Call)
    {
        SessionFor(Call.GetWorld()).bLoop = Call.GetBool(1);
    }, "Loop:bool");

    Lib.Function("Seek", [](ScriptCall& Call)
    {
        ReplaySession& S = SessionFor(Call.GetWorld());
        if(S.Clip.IsEmpty()) { return; }
        S.Time = std::clamp(S.Clip.GetStartTime() + Call.GetNumber(1), S.Clip.GetStartTime(), S.Clip.GetEndTime());
        if(S.Player) { S.Player->Seek(S.Time); }
    }, "Seconds:number").Tooltip("Moves the playhead, in seconds from the clip's start");

    Lib.Pure("GetTime", [](ScriptCall& Call)
    {
        const ReplaySession& S = SessionFor(Call.GetWorld());
        Call.PushNumber(S.Clip.IsEmpty() ? 0.0 : S.Time - S.Clip.GetStartTime());
    }, "-> Seconds:number").Tooltip("The playhead, in seconds from the clip's start");

    Lib.Pure("IsPlaying", [](ScriptCall& Call)
    {
        const ReplaySession& S = SessionFor(Call.GetWorld());
        Call.PushBool(S.bPlaying && !S.bPaused);
    }, "-> Playing:bool");

    Lib.Event(FINISHED_EVENT).Tooltip("Playback reached the end (or the start, rewinding) without looping; it holds that frame");
}

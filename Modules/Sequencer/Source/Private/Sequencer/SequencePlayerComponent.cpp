#include "Public/Sequencer/SequencePlayerComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

#include "Public/Sequencer/SequenceWorld.h"

/* SequencePlayerComponent: registration, saves, the editor's fields, and the
 * "Sequence" script library that drives players from Lua and script graphs. */

namespace Sequencer
{
    GAMECORE_DEFINE_COMPONENT(SequencePlayerComponent, "Sequence Player")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SequencePlayerComponent, 1)
    {
        Ar.Field("Sequence",            C.Sequence);
        Ar.Field("PlayRate",            C.PlayRate);
        Ar.Flag("AutoPlay",             C.bAutoPlay);
        Ar.Flag("Loop",                 C.bLoop);
        Ar.Flag("RestoreState",         C.bRestoreState);
        Ar.Flag("DestroyWhenFinished",  C.bDestroyWhenFinished);
        Ar.Flag("Cinematic",            C.bCinematic);
        Ar.Flag("Skippable",            C.bSkippable);
    }

    /* The fields; playing goes through the Sequence library (below), which
     * takes the player entity, so a graph can drive one it holds. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(SequencePlayerComponent)
    {
        Bind.Field("Sequence",     &SequencePlayerComponent::Sequence);
        Bind.Field("PlayRate",     &SequencePlayerComponent::PlayRate);
        Bind.Flag("AutoPlay",      &SequencePlayerComponent::bAutoPlay);
        Bind.Flag("Loop",          &SequencePlayerComponent::bLoop);
        Bind.Flag("RestoreState",  &SequencePlayerComponent::bRestoreState);
        Bind.Flag("Cinematic",     &SequencePlayerComponent::bCinematic);
        Bind.Flag("Skippable",     &SequencePlayerComponent::bSkippable);
    }

    /* What a client's copy needs to follow the server's (SequenceWorld.h,
     * "Networking"): which sequence, how it plays, and where it is. */
    GAMECORE_DEFINE_COMPONENT_REPLICATION(SequencePlayerComponent)
    {
        Rep.Field("Sequence",   &SequencePlayerComponent::Sequence);
        Rep.Field("PlayRate",   &SequencePlayerComponent::PlayRate);
        Rep.Flag("Loop",        &SequencePlayerComponent::bLoop);
        Rep.Flag("Cinematic",   &SequencePlayerComponent::bCinematic);
        Rep.Field("NetState",   &SequencePlayerComponent::NetState);
        Rep.Field("NetSerial",  &SequencePlayerComponent::NetSerial);
        Rep.Field("NetSeconds", &SequencePlayerComponent::NetSeconds);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SequencePlayerComponent)
    {
        Props.TypeCategory("Animation");
        Props.Category("Sequence");
        Props.Field("Sequence",              &SequencePlayerComponent::Sequence, GameCore::PF_AssetSequence);
        Props.Field("Play Rate",             &SequencePlayerComponent::PlayRate);
        Props.Flag("Auto Play",              &SequencePlayerComponent::bAutoPlay);
        Props.Flag("Loop",                   &SequencePlayerComponent::bLoop);
        Props.Flag("Restore State",          &SequencePlayerComponent::bRestoreState);
        Props.Flag("Destroy When Finished",  &SequencePlayerComponent::bDestroyWhenFinished);
        Props.Flag("Cinematic",              &SequencePlayerComponent::bCinematic);
        Props.Flag("Skippable",              &SequencePlayerComponent::bSkippable);
    }
#endif // WITH_EDITOR
}

namespace
{
    Sequencer::SequenceWorld* Players(GameCore::ScriptCall& InCall)
    {
        Sequencer::SequenceWorld* System = Sequencer::SequenceWorld::Find(&InCall.GetWorld());
        if(!System)
        {
            InCall.Error("this world plays no sequences (no SequenceWorld is attached)");
        }
        return System;
    }

    /* Sequence.Play(path [, loop]) -> player: a new player entity, playing,
     * gone when it finishes. nil if the sequence cannot be loaded. */
    void ScriptPlay(GameCore::ScriptCall& InCall)
    {
        const char* Path  = InCall.GetString(1);
        const bool  bLoop = InCall.HasArg(2) && InCall.GetBool(2);
        if(InCall.HasError()) { return; }
        Sequencer::SequenceWorld* System = Players(InCall);
        if(!System) { return; }
        const GameCore::EntityID Player = System->PlaySequence(Path, bLoop);
        if(Player.IsSet()) { InCall.PushEntity(Player); }
        else               { InCall.PushNil(); }
    }

    /* The calls on one player: Sequence.Start(player), Sequence.Stop(player), ... */
    template<bool (Sequencer::SequenceWorld::*Method)(GameCore::EntityID)>
    void ScriptOnPlayer(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        Sequencer::SequenceWorld* System = Players(InCall);
        if(!System) { return; }
        InCall.PushBool((System->*Method)(Player));
    }

    void ScriptSeek(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player  = InCall.GetEntity(1);
        const double             Seconds = InCall.GetNumber(2);
        if(InCall.HasError()) { return; }
        Sequencer::SequenceWorld* System = Players(InCall);
        if(!System) { return; }
        InCall.PushBool(System->Seek(Player, Seconds));
    }

    void ScriptSeekToMarker(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player = InCall.GetEntity(1);
        const char*              Marker = InCall.GetString(2);
        if(InCall.HasError()) { return; }
        Sequencer::SequenceWorld* System = Players(InCall);
        if(!System) { return; }
        InCall.PushBool(System->SeekToMarker(Player, Marker));
    }

    void ScriptSkipCinematics(GameCore::ScriptCall& InCall)
    {
        Sequencer::SequenceWorld* System = Players(InCall);
        if(!System) { return; }
        InCall.PushInteger(int64_t(System->SkipCinematics()));
    }

    void ScriptSetSubtitles(GameCore::ScriptCall& InCall)
    {
        const bool bOn = InCall.GetBool(1);
        if(InCall.HasError()) { return; }
        Sequencer::SequenceWorld::SetSubtitlesEnabled(bOn);
    }

    void ScriptGetSubtitles(GameCore::ScriptCall& InCall)
    {
        InCall.PushBool(Sequencer::SequenceWorld::AreSubtitlesEnabled());
    }

    void ScriptBind(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player = InCall.GetEntity(1);
        const char*              Id     = InCall.GetString(2);
        const GameCore::EntityID Entity = InCall.GetEntity(3);
        if(InCall.HasError()) { return; }
        Sequencer::SequenceWorld* System = Players(InCall);
        if(!System) { return; }
        InCall.PushBool(System->Bind(Player, Id, Entity));
    }

    void ScriptIsPlaying(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        const Sequencer::SequenceWorld* System = Sequencer::SequenceWorld::Find(&InCall.GetWorld());
        InCall.PushBool(System && System->IsPlaying(Player));
    }

    void ScriptGetTime(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        const Sequencer::SequenceWorld* System = Sequencer::SequenceWorld::Find(&InCall.GetWorld());
        InCall.PushNumber(System ? System->GetTime(Player) : 0.0);
    }

    void ScriptGetDuration(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Player = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        const Sequencer::SequenceWorld* System = Sequencer::SequenceWorld::Find(&InCall.GetWorld());
        InCall.PushNumber(System ? System->GetDuration(Player) : 0.0);
    }
}

/*
 * Cutscenes from Lua and script graphs:
 *
 *     local p = Sequence.Play("Content/Cinematics/Intro.nmseq")
 *     Sequence.Bind(p, "hero", Self)
 *     Events.On("SequenceFinished", function(player, path) ... end)
 *
 * A player placed in the level (a Sequence Player component) is driven the
 * same way through its entity.
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Sequence)
{
    Lib.Function("Play", &ScriptPlay, "Path:string, Loop:bool -> Player:Entity")
       .Tooltip("Plays a sequence on a new player entity, destroyed when it finishes; nil if it cannot load");
    Lib.Function("Start", &ScriptOnPlayer<&Sequencer::SequenceWorld::Play>, "Player:Entity -> Started:bool")
       .Tooltip("Plays a Sequence Player from the start (a paused one resumes)");
    Lib.Function("Pause", &ScriptOnPlayer<&Sequencer::SequenceWorld::Pause>, "Player:Entity -> Done:bool")
       .Tooltip("Holds the playhead where it is");
    Lib.Function("Resume", &ScriptOnPlayer<&Sequencer::SequenceWorld::Resume>, "Player:Entity -> Done:bool")
       .Tooltip("Continues a paused player");
    Lib.Function("Stop", &ScriptOnPlayer<&Sequencer::SequenceWorld::Stop>, "Player:Entity -> Done:bool")
       .Tooltip("Back to the start; restores what it changed if Restore State is on, removes what it spawned");
    Lib.Function("Skip", &ScriptOnPlayer<&Sequencer::SequenceWorld::Skip>, "Player:Entity -> Done:bool")
       .Tooltip("Jumps to the end, firing only the events marked fire-on-skip, and finishes");
    Lib.Function("Seek", &ScriptSeek, "Player:Entity, Seconds:number -> Done:bool")
       .Tooltip("Moves the playhead and shows that moment now; no events fire");
    Lib.Function("SeekToMarker", &ScriptSeekToMarker, "Player:Entity, Marker:string -> Done:bool")
       .Tooltip("Seek, to one of the sequence's named markers");
    Lib.Function("SkipCinematics", &ScriptSkipCinematics, "-> Skipped:integer")
       .Tooltip("Skips every skippable cinematic playing, as the Back key does");
    Lib.Function("SetSubtitles", &ScriptSetSubtitles, "On:bool")
       .Tooltip("Shows sequences' subtitle tracks or not (every level; on by default)");
    Lib.Pure("GetSubtitles", &ScriptGetSubtitles, "-> On:bool")
       .Tooltip("Whether sequences' subtitles are shown");
    Lib.Function("Bind", &ScriptBind, "Player:Entity, Binding:string, Entity:Entity -> Done:bool")
       .Tooltip("Plays one of the sequence's bindings on this entity instead");
    Lib.Pure("IsPlaying", &ScriptIsPlaying, "Player:Entity -> Playing:bool")
       .Tooltip("Whether the player is advancing (not paused, stopped or finished)");
    Lib.Pure("GetTime", &ScriptGetTime, "Player:Entity -> Seconds:number")
       .Tooltip("Where the playhead is");
    Lib.Pure("GetDuration", &ScriptGetDuration, "Player:Entity -> Seconds:number")
       .Tooltip("How long the player's sequence is");

    Lib.Event("SequenceStarted", "Player:Entity, Sequence:string")
       .Tooltip("A player started from the beginning");
    Lib.Event("SequenceFinished", "Player:Entity, Sequence:string")
       .Tooltip("A player reached the end, or was skipped to it");
    Lib.Event("SequenceStopped", "Player:Entity, Sequence:string")
       .Tooltip("A player was stopped before the end");
}

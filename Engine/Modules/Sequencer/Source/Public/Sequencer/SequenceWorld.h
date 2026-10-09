#ifndef SEQUENCER_SEQUENCEWORLD_H
#define SEQUENCER_SEQUENCEWORLD_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/EntityID.h"
#include "Public/Sequencer/SequenceAsset.h"
#include "Public/Sequencer/SequenceInstance.h"
#include "Public/Sequencer/SequencerAPI.h"

namespace GameCore { class World; }

namespace Sequencer
{
    /* Where a player is. */
    enum class ESequencePlayState : uint8_t
    {
        Stopped = 0,   // never played, or stopped: at the start, holding nothing
        Playing,
        Paused,
        Finished,      // reached the end (not looping); holds the last frame
    };

    /*
     * What sequences ask of the game around them, that this module cannot
     * reach itself. NimbleEngine's GameSession fills it in for a level; each
     * may be null, and a World without a host plays everything else.
     */
    struct SequenceHost
    {
        /* The camera entity the game is seen through (GameSession::
         * SetViewTarget); unset gives the view back. And what it was. */
        void               (*SetViewTarget)(GameCore::EntityID InCamera, void* InUserData) = nullptr;
        GameCore::EntityID (*GetViewTarget)(void* InUserData) = nullptr;

        /* The screen to black: 0 clear .. 1 black; below 0, no fade at all. */
        void (*SetFade)(float InAlpha, void* InUserData) = nullptr;

        /* Cinematic mode: letterboxed, players' input held back, the Back key
         * skipping (SkipCinematics). */
        void (*SetCinematic)(bool bInOn, void* InUserData) = nullptr;

        /* Whether InPlayer is a client's copy of a server's player, which
         * follows the server rather than playing on its own. */
        bool (*IsReplica)(GameCore::EntityID InPlayer, void* InUserData) = nullptr;

        /* A mix bus (SequenceBusName: "Music", "SFX", ...) at InGain, 1 being
         * as the player set it: the Bus Volume tracks. Told 1 again when no
         * player wants it any more. */
        void (*SetBusGain)(const char* InBus, float InGain, void* InUserData) = nullptr;

        /* The subtitle line to show; both "" for none. Never called with a
         * line while subtitles are off (SetSubtitlesEnabled). */
        void (*SetSubtitle)(const char* InSpeaker, const char* InText, void* InUserData) = nullptr;

        void* UserData = nullptr;
    };

    /*
     * A World's sequence players: one per entity with a Sequence Player
     * component, kept beside the components as AnimationSystem keeps
     * AnimInstances.
     *
     * Ticks in TP_PostPhysics: after gameplay scripts and physics, so a
     * sequence has the last word on what it binds, and before the animation
     * system (TP_PostUpdate), which then sees this frame's values. A world
     * that does not tick -- the editor's, outside Play -- plays nothing.
     *
     * Emits on the World's EventBus, each with (player entity, sequence
     * path): SequenceStarted, SequenceFinished (reached the end, or skipped
     * to it), SequenceStopped.
     *
     * THE SCREEN. Of the players with a shot, the one started last is seen
     * through; when none has one the view goes back to what it was. The
     * fade is the darkest any player asks for. Cinematic mode is on while
     * any cinematic player plays or is paused. Post process: every player's
     * stacks and settings, earlier-started first, laid over the volumes
     * (RenderCore's PostProcessOverride for this World). Bus gains: every
     * player's multiplied. The subtitle: the started-last player's that has
     * a line, unless subtitles are off.
     *
     * NETWORKING. A server's player writes its playback into the component
     * (NetState, NetSerial, NetSeconds), which replicates. A client's copy
     * (SequenceHost::IsReplica) does not auto play; it follows those fields
     * -- started, paused, seeked, stopped as the server's was -- and plays
     * locally from there, a round trip behind. Spawn bindings spawn on each
     * machine and never replicate.
     */
    class SEQUENCER_API SequenceWorld
    {
    public:
        /* InWorld's, made on the first Attach; each Attach needs a Detach. */
        static SequenceWorld* Attach(GameCore::World& InWorld);
        static void           Detach(GameCore::World& InWorld);

        /* Null if InWorld has none. */
        static SequenceWorld* Find(const GameCore::World* InWorld);

        /* What the game does for sequences (copied); clear with {}. */
        void SetHost(const SequenceHost& InHost);

        /* A new entity with a Sequence Player for InPath, playing now and
         * destroyed when it finishes. Unset if InPath cannot be loaded. */
        GameCore::EntityID PlaySequence(const char* InPath, bool bInLoop = false);

        // === One player, by its entity =======================================
        // False when InPlayer has no Sequence Player (or, for Play, its
        // sequence cannot be loaded).

        /* From the start; a paused player resumes instead. */
        bool Play(GameCore::EntityID InPlayer);
        bool Pause(GameCore::EntityID InPlayer);
        bool Resume(GameCore::EntityID InPlayer);
        /* Back to the start, holding nothing: restored if bRestoreState,
         * spawned entities destroyed. */
        bool Stop(GameCore::EntityID InPlayer);
        /* Jumps the playhead (seconds, clamped) and shows that frame now. No
         * events fire. A stopped player is left paused there. */
        bool Seek(GameCore::EntityID InPlayer, double InSeconds);
        /* Seek, to a marker by name. False if the sequence has none so named. */
        bool SeekToMarker(GameCore::EntityID InPlayer, const char* InMarker);
        /* Jumps to the end: fireOnSkip events on the way, then it finishes. */
        bool Skip(GameCore::EntityID InPlayer);
        /* Subtitle tracks shown or not, for every World (a player's
         * setting; on by default). Not saved here -- the game keeps it. */
        static void SetSubtitlesEnabled(bool bInOn);
        static bool AreSubtitlesEnabled();

        /* SequenceInstance::SetBindingOverride; kept across Play and Stop. */
        bool Bind(GameCore::EntityID InPlayer, const char* InBindingId, GameCore::EntityID InEntity);

        /* Skips every cinematic, skippable player that is playing: the Back
         * key in cinematic mode. How many it skipped. */
        size_t SkipCinematics();

        ESequencePlayState GetState(GameCore::EntityID InPlayer) const;
        bool   IsPlaying(GameCore::EntityID InPlayer) const { return GetState(InPlayer) == ESequencePlayState::Playing; }
        /* Seconds; 0 when it has no sequence. */
        double GetTime(GameCore::EntityID InPlayer) const;
        double GetDuration(GameCore::EntityID InPlayer) const;
        /* Its instance while it has one (tests, tools); null otherwise. */
        SequenceInstance* FindInstance(GameCore::EntityID InPlayer);

        /* What the host was last told. */
        GameCore::EntityID GetViewCamera() const { return AppliedCamera; }
        float              GetFade() const       { return AppliedFade; }
        bool               IsCinematic() const   { return bAppliedCinematic; }
        const SequencePostProcess& GetPostProcess() const { return AppliedPostProcess; }
        const std::vector<SequenceBusGain>& GetBusGains() const { return AppliedBusGains; }
        const SequenceSubtitle&    GetSubtitle() const    { return AppliedSubtitle; }

        /* Every player advanced by InDeltaSeconds; the system calls it. */
        void Tick(float InDeltaSeconds);

    private:
        explicit SequenceWorld(GameCore::World& InWorld);
        ~SequenceWorld();

        static void TickSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

        struct Player
        {
            std::unique_ptr<SequenceInstance> Instance;
            ESequencePlayState State = ESequencePlayState::Stopped;
            double Frame = 0.0;
            /* Events from here on the next advance: Play includes the frame
             * it starts on, a jump does not. */
            bool   bIncludeFrame = false;
            bool   bFresh        = false;   // just played: the next tick shows Frame, no time passes
            bool   bAutoPlayed   = false;
            bool   bRestoreState = false;   // the component's, as of the last Play
            uint64_t StartOrder  = 0;       // which started last, for the view
            int32_t  SeenSerial  = 0;       // a replica's: the server's last start/seek/skip it followed
            uint8_t  SeenState   = 0;
            std::unordered_map<std::string, GameCore::EntityID> Overrides;
        };

        Player* FindPlayer(GameCore::EntityID InPlayer);
        const Player* FindPlayer(GameCore::EntityID InPlayer) const;
        /* InPlayer's state, made if it has the component; null otherwise. */
        Player* GetPlayer(GameCore::EntityID InPlayer);

        /* The instance for the component's sequence, (re)made if the path
         * changed or there is none; false if it cannot be loaded. */
        bool Prepare(GameCore::EntityID InPlayer, Player& InOutPlayer);
        void Advance(GameCore::EntityID InPlayer, Player& InOutPlayer, float InDeltaSeconds);
        void Finish(GameCore::EntityID InPlayer, Player& InOutPlayer);
        /* Restored if it asked (else its saves forgotten), everything it
         * made or holds let go, instance kept. */
        void Release(Player& InOutPlayer);
        void Emit(const char* InEvent, GameCore::EntityID InPlayer, const Player& InPlayerState);
        /* The component's Net* fields from InPlayerState (a server's). */
        void Publish(GameCore::EntityID InPlayer, const Player& InPlayerState, bool bInJumped);
        /* A replica made to match the server's fields. */
        void Follow(GameCore::EntityID InPlayer, Player& InOutPlayer);
        bool IsReplica(GameCore::EntityID InPlayer) const;
        /* Tells the host what the players now want of the screen. */
        void ApplyToHost();

        GameCore::World& World;
        int32_t          Attachments = 0;
        uint64_t         StartCounter = 0;
        SequenceHost     Host;
        std::unordered_map<uint64_t, Player> Players;   // by EntityID::Pack()

        GameCore::EntityID AppliedCamera;
        GameCore::EntityID ViewBefore;        // what the host showed before we took it
        bool               bViewTaken        = false;
        float              AppliedFade       = -1.0f;
        bool               bAppliedCinematic = false;
        SequencePostProcess AppliedPostProcess;
        std::vector<SequenceBusGain> AppliedBusGains;   // only those not 1
        SequenceSubtitle    AppliedSubtitle;
    };
}

#endif //SEQUENCER_SEQUENCEWORLD_H

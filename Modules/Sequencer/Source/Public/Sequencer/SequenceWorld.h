#ifndef SEQUENCER_SEQUENCEWORLD_H
#define SEQUENCER_SEQUENCEWORLD_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/PrefabInstanceID.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Sequencer/SequenceAsset.h"
#include "Public/Sequencer/SequencerAPI.h"

namespace GameCore { class World; }

namespace Sequencer
{
    /*
     * One sequence applied to one World: its bindings resolved to entities,
     * its tracks written to them at a frame, its events fired over a range,
     * and the values it overwrote kept so they can be put back.
     *
     * A player owns one while it plays; the editor's preview owns one too,
     * so what the editor shows is what the game plays. Game thread only.
     *
     * Bindings resolve on first use: an override (SetBindingOverride), else
     * the entity with the binding's Guid, else the first with its Name; a
     * Spawn binding spawns its prefab. A binding that resolves to nothing is
     * warned about once and its tracks skipped.
     */
    class SEQUENCER_API SequenceInstance
    {
    public:
        SequenceInstance(GameCore::World& InWorld, std::shared_ptr<const SequenceAsset> InAsset, std::string InPath = std::string());
        /* Destroys what it spawned; restores nothing (RestoreState first if
         * that is wanted). */
        ~SequenceInstance();

        SequenceInstance(const SequenceInstance&)            = delete;
        SequenceInstance& operator=(const SequenceInstance&) = delete;

        const SequenceAsset& GetAsset() const { return *Asset; }
        const std::string&   GetPath() const  { return Path; }

        /* InEntity plays binding InId from now on (an unset id clears the
         * override). False if the asset has no such binding. A binding that
         * already resolved is resolved again, its changes so far kept. */
        bool SetBindingOverride(const std::string& InId, GameCore::EntityID InEntity);

        /* The entity binding InIndex resolves to (resolving it now); unset
         * if it resolves to nothing. */
        GameCore::EntityID ResolveBinding(size_t InIndex);

        /* Every Transform, Float and Bool track written at InFrame
         * (fractional, clamped to 0..Length). */
        void Evaluate(double InFrame);

        /*
         * Event keys in (InFrom, InTo] -- [InFrom, InTo] with bInIncludeFrom
         * -- emitted on the World's EventBus in frame order. bInSkipOnly: only
         * those marked fireOnSkip. InTo below InFrom fires nothing.
         */
        void FireEvents(double InFrom, double InTo, bool bInIncludeFrom, bool bInSkipOnly = false);

        /* Every value Evaluate overwrote, back as it was before the first
         * write; then forgotten, so the next Evaluate saves afresh. */
        void RestoreState();

        /* Forgets the saved values without writing them: what the sequence
         * left becomes what is. */
        void DiscardState();

        /* Destroys the entities Spawn bindings made; they resolve again
         * (spawning anew) on next use. */
        void ReleaseSpawned();

        /* How many values are saved for RestoreState. */
        size_t GetSavedCount() const { return Saved.size(); }

    private:
        /* A track bound to what it writes, worked out once per resolve. */
        struct BoundTrack
        {
            const SequenceTrack*      Track = nullptr;
            GameCore::ComponentTypeID Type  = GameCore::COMPONENT_TYPE_INVALID;   // Float, Bool
            uint32_t                  Offset = 0;
            uint8_t                   FieldType = 0;                              // EScriptFieldType
            bool                      bUsable = false;
        };

        struct BindingState
        {
            GameCore::EntityID         Entity;
            GameCore::EntityID         Override;
            GameCore::PrefabInstanceID Spawned;
            std::vector<BoundTrack>    Tracks;
            bool bResolved = false;
            bool bWarned   = false;
        };

        /* One value as it was. A Transform is kept whole. */
        struct SavedValue
        {
            GameCore::EntityID        Entity;
            GameCore::ComponentTypeID Type = GameCore::COMPONENT_TYPE_INVALID;
            uint32_t                  Offset = 0;    // UINT32_MAX: the Transform pose
            uint32_t                  Size   = 0;
            uint8_t                   Bytes[8] = {};
            GameCore::TransformPose   Pose;
        };

        /* (entity, component type, offset): one save per value. */
        using SavedKey = std::tuple<uint64_t, GameCore::ComponentTypeID, uint32_t>;

        void BindTracks(BindingState& InOutState, const SequenceBinding& InBinding);
        void WriteTransform(GameCore::EntityID InEntity, const SequenceTrack& InTrack, double InFrame);
        void WriteField(GameCore::EntityID InEntity, const BoundTrack& InBound, double InFrame);
        void Save(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, uint32_t InOffset, uint32_t InSize, const void* InNow);
        void SaveTransform(GameCore::EntityID InEntity, const GameCore::TransformPose& InNow);

        GameCore::World&                     World;
        std::shared_ptr<const SequenceAsset> Asset;
        std::string                          Path;
        std::vector<BindingState>            Bindings;
        std::vector<SavedValue>              Saved;
        std::map<SavedKey, size_t>           SavedIndex;
    };

    /* Where a player is. */
    enum class ESequencePlayState : uint8_t
    {
        Stopped = 0,   // never played, or stopped: at the start, holding nothing
        Playing,
        Paused,
        Finished,      // reached the end (not looping); holds the last frame
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
     */
    class SEQUENCER_API SequenceWorld
    {
    public:
        /* InWorld's, made on the first Attach; each Attach needs a Detach. */
        static SequenceWorld* Attach(GameCore::World& InWorld);
        static void           Detach(GameCore::World& InWorld);

        /* Null if InWorld has none. */
        static SequenceWorld* Find(const GameCore::World* InWorld);

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
        /* Jumps to the end: fireOnSkip events on the way, then it finishes. */
        bool Skip(GameCore::EntityID InPlayer);
        /* SequenceInstance::SetBindingOverride; kept across Play and Stop. */
        bool Bind(GameCore::EntityID InPlayer, const char* InBindingId, GameCore::EntityID InEntity);

        ESequencePlayState GetState(GameCore::EntityID InPlayer) const;
        bool   IsPlaying(GameCore::EntityID InPlayer) const { return GetState(InPlayer) == ESequencePlayState::Playing; }
        /* Seconds; 0 when it has no sequence. */
        double GetTime(GameCore::EntityID InPlayer) const;
        double GetDuration(GameCore::EntityID InPlayer) const;

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
        /* Restored if it asked (else its saves forgotten), spawned entities gone, instance kept. */
        void Release(Player& InOutPlayer);
        void Emit(const char* InEvent, GameCore::EntityID InPlayer, const Player& InPlayerState);

        GameCore::World& World;
        int32_t          Attachments = 0;
        std::unordered_map<uint64_t, Player> Players;   // by EntityID::Pack()
    };
}

#endif //SEQUENCER_SEQUENCEWORLD_H

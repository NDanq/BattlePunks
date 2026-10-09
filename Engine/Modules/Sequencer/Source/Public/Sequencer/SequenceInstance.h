#ifndef SEQUENCER_SEQUENCEINSTANCE_H
#define SEQUENCER_SEQUENCEINSTANCE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/PrefabInstanceID.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Sequencer/SequenceAsset.h"
#include "Public/Sequencer/SequencerAPI.h"

namespace GameCore { class World; }
namespace RenderCore { struct AnimClipData; }

namespace Sequencer
{
    /* What an instance may make in its World. A game's plays make all of
     * it; the editor's preview none, so previewing never adds entities to
     * the level being edited. */
    struct SequenceInstanceOptions
    {
        bool    bSpawn        = true;   // Spawn bindings spawn their prefab
        bool    bAudio        = true;   // Audio sections play, through helper entities with an Audio Source
        bool    bCameraEntity = true;   // CameraCut tracks drive a camera entity of their own (GetCameraEntity)
        int32_t Depth         = 0;      // sub-sequence nesting; past 8 a SubSequence plays nothing
    };

    /* What the CameraCut track wants seen now: a camera binding's view, or
     * a blend between two. */
    struct SequenceShot
    {
        bool                    bValid = false;
        GameCore::EntityID      From;            // the camera blended from; unset outside a blend
        GameCore::EntityID      To;              // the section's camera
        float                   Blend = 1.0f;    // 0 From .. 1 To
        GameCore::TransformPose Pose;            // world space
        float                   FovY  = 60.0f;
        float                   ZNear = -1.0f;
        float                   ZFar  = 1000.0f;
    };

    /*
     * One sequence applied to one World: its bindings resolved to entities,
     * its tracks written to them at a frame, its sounds kept in step, its
     * events fired over a range, and the values it overwrote kept so they
     * can be put back.
     *
     * A player owns one while it plays; the editor's preview owns one too,
     * so what the editor shows is what the game plays. Game thread only.
     *
     * Bindings resolve on first use: an override (SetBindingOverride), else
     * the entity with the binding's Guid, else the first with its Name; a
     * Spawn binding spawns its prefab. A binding that resolves to nothing is
     * warned about once and its tracks skipped.
     *
     * WHAT IT HOLDS. Some values are the sequence's only while it plays and
     * always go back on Release, whatever the player's Restore State: an
     * Animation track's control of its entity's clip, and a bound rigid
     * body's motion (kinematic while a Transform track moves it).
     */
    class SEQUENCER_API SequenceInstance
    {
    public:
        SequenceInstance(GameCore::World& InWorld, std::shared_ptr<const SequenceAsset> InAsset, std::string InPath = std::string(),
                         const SequenceInstanceOptions& InOptions = SequenceInstanceOptions());
        /* Release, without restoring (RestoreState first if that is wanted). */
        ~SequenceInstance();

        SequenceInstance(const SequenceInstance&)            = delete;
        SequenceInstance& operator=(const SequenceInstance&) = delete;

        const SequenceAsset&           GetAsset() const   { return *Asset; }
        const std::string&             GetPath() const    { return Path; }
        const SequenceInstanceOptions& GetOptions() const { return Options; }

        /* InEntity plays binding InId from now on (an unset id clears the
         * override). False if the asset has no such binding. A binding that
         * already resolved is resolved again, its changes so far kept. */
        bool SetBindingOverride(const std::string& InId, GameCore::EntityID InEntity);

        /* The entity binding InIndex resolves to (resolving it now); unset
         * if it resolves to nothing. */
        GameCore::EntityID ResolveBinding(size_t InIndex);

        /* Every continuous track at InFrame (fractional, clamped to
         * 0..Length): transforms, fields, clips, the fade and the shot, and
         * sub-sequences'. */
        void Evaluate(double InFrame);

        /* Sounds (and sub-sequences' sounds) in step with InFrame: playing
         * when bInPlaying and inside their sections, paused when not, stopped
         * outside. Call after Evaluate. */
        void UpdatePlayback(double InFrame, bool bInPlaying);

        /* The next UpdatePlayback seeks every playing sound: the playhead
         * jumped (a seek, a loop). */
        void NoteJump();

        /* Every sound stopped. */
        void StopPlayback();

        /*
         * Event keys in (InFrom, InTo] -- [InFrom, InTo] with bInIncludeFrom
         * -- emitted on the World's EventBus in frame order, sub-sequences'
         * too. bInSkipOnly: only those marked fireOnSkip. InTo below InFrom
         * fires nothing.
         */
        void FireEvents(double InFrom, double InTo, bool bInIncludeFrom, bool bInSkipOnly = false);

        /* Every value Evaluate overwrote, back as it was before the first
         * write; then forgotten, so the next Evaluate saves afresh. */
        void RestoreState();

        /* Forgets the saved values without writing them: what the sequence
         * left becomes what is -- except what it holds, which goes back. */
        void DiscardState();

        /* Lets go of everything it made or holds: sounds stopped and their
         * helpers destroyed, clips handed back, rigid bodies' motion back,
         * its camera entity and spawned entities destroyed. Saved values
         * stay (RestoreState / DiscardState). Evaluating again takes it all
         * up afresh. */
        void Release();

        /* Only the spawned entities; they resolve again (spawning anew) on
         * next use. */
        void ReleaseSpawned();

        /* How many values are saved for RestoreState. */
        size_t GetSavedCount() const { return Saved.size(); }

        /* The Fade track's value at the last Evaluate (0 clear .. 1 black);
         * -1 with no Fade track. */
        float GetFade() const { return Fade; }

        /* The CameraCut track's shot at the last Evaluate. */
        const SequenceShot& GetShot() const { return Shot; }

        /* The camera entity it drives to show the shot (bCameraEntity);
         * unset when there is no shot. */
        GameCore::EntityID GetCameraEntity() const;

    private:
        struct SoundVoice
        {
            enum class EState : uint8_t { Idle, Playing, Paused };
            GameCore::EntityID Helper;
            EState             State = EState::Idle;
        };

        /* A track bound to what it writes, worked out once per resolve. */
        struct BoundTrack
        {
            const SequenceTrack*      Track = nullptr;
            GameCore::ComponentTypeID Type  = GameCore::COMPONENT_TYPE_INVALID;   // Float, Bool
            uint32_t                  Offset = 0;
            uint8_t                   FieldType = 0;                              // EScriptFieldType
            bool                      bUsable = false;

            /* Animation: whether it has the entity's clip, and what it found. */
            bool    bControlling = false;
            uint8_t SavedPaused  = 0;
            std::vector<std::shared_ptr<const RenderCore::AnimClipData>> Clips;   // by section; loaded on use

            /* Audio. */
            std::vector<SoundVoice> Voices;   // by section

            /* SubSequence. */
            std::vector<std::unique_ptr<SequenceInstance>> Subs;   // by section; made on use
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
            bool                      bHeld = false; // goes back on Release whatever is asked
        };

        /* (entity, component type, offset): one save per value. */
        using SavedKey = std::tuple<uint64_t, GameCore::ComponentTypeID, uint32_t>;

        void BindTracks(BindingState& InOutState, const SequenceBinding& InBinding);
        void EvaluateTrack(GameCore::EntityID InEntity, BoundTrack& InOutBound, double InFrame);
        void WriteTransform(GameCore::EntityID InEntity, const SequenceTrack& InTrack, double InFrame);
        void WriteField(GameCore::EntityID InEntity, const BoundTrack& InBound, double InFrame);
        void WriteAnimation(GameCore::EntityID InEntity, BoundTrack& InOutBound, double InFrame);
        void HandBackAnimation(GameCore::EntityID InEntity, BoundTrack& InOutBound);
        void UpdateSounds(GameCore::EntityID InOwner, BoundTrack& InOutBound, double InFrame, bool bInPlaying);
        void EvaluateShot(const SequenceTrack& InTrack, double InFrame);
        void EvaluateSubs(BoundTrack& InOutBound, double InFrame);
        SequenceInstance* GetSub(BoundTrack& InOutBound, size_t InSection);
        double SubFrame(const SequenceSection& InSection, const SequenceInstance& InSub, double InFrame) const;
        void HoldRigidBody(GameCore::EntityID InEntity);
        void Save(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, uint32_t InOffset, uint32_t InSize, const void* InNow,
                  bool bInHeld = false);
        void SaveTransform(GameCore::EntityID InEntity, const GameCore::TransformPose& InNow);
        void WriteSaved(const SavedValue& InValue);

        /* Every bound track, binding ones with their entity, then master
         * ones with none. */
        template<typename F> void ForEachTrack(F&& InFn);

        GameCore::World&                     World;
        std::shared_ptr<const SequenceAsset> Asset;
        std::string                          Path;
        SequenceInstanceOptions              Options;
        std::vector<BindingState>            Bindings;
        std::vector<BoundTrack>              MasterTracks;
        std::vector<SavedValue>              Saved;
        std::map<SavedKey, size_t>           SavedIndex;

        float              Fade = -1.0f;
        SequenceShot       Shot;
        GameCore::EntityID CameraEntity;
        bool               bJumped = false;

        /* False once destroyed: a FireEvents handler may destroy the
         * instance whose events it is hearing. */
        std::shared_ptr<bool> Alive;
    };
}

#endif //SEQUENCER_SEQUENCEINSTANCE_H

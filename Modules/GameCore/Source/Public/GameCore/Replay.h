#ifndef GAMECORE_REPLAY_H
#define GAMECORE_REPLAY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * Replays: a World's state recorded over time and played back into a World.
 *
 *     ReplayClip     Clip;
 *     ReplayRecorder Recorder(Clip);
 *     Recorder.Start(World);           // samples at the end of every TP_PostUpdate
 *     ...                              // play
 *     Recorder.Stop();
 *     SaveReplayClip("Saved/Replays/Last.nmreplay", Clip);
 *
 *     // later, into the same level loaded fresh, with gameplay not ticking:
 *     ReplayPlayer Player(World, Clip);
 *     Player.Play();
 *     Player.Advance(DeltaSeconds);    // every frame
 *
 * STATE, NOT INPUT. A frame is what each recorded entity's components held,
 * written through their serializers (ComponentSerializer.h) -- never a
 * re-simulation from recorded input, which would need the whole game, its
 * physics and its scripts to be deterministic. Playback writes the state
 * into a World; nothing has to run for it to look right, and the host
 * should keep the game's own systems and scripts from fighting it (don't
 * tick gameplay, or tick only what draws).
 *
 * WHAT IS RECORDED. Every live entity that is not Static (unless
 * bRecordStatic) and passes the entity filter, with its name, parent and
 * mobility, and every component it has whose type has a serializer and is
 * not excluded. Entities are named by EntityGuid, components by type NAME,
 * entity references inside a component by GUID -- so a clip plays into a
 * different run, where every EntityID and ComponentTypeID may differ.
 * Per-run fields a serializer skips are never touched on playback; the
 * live component keeps them.
 *
 * KEYFRAMES AND DELTAS. A keyframe holds every recorded entity whole; the
 * frames between hold only what changed since the frame before, compared
 * by the serialized bytes: entities that appeared or changed, components
 * set or removed, entities destroyed. A seek rebuilds from the nearest
 * keyframe at or before it; playing forward applies one delta at a time.
 *
 * INTERPOLATION. Between two frames the player blends TransformComponent's
 * Position, Scale and (per axis, the short way) Rotation, so a clip sampled
 * at 20-30 Hz plays smoothly at any frame rate. Every other field steps.
 *
 * ROLLING. ReplayRecordSettings::MaxDuration > 0 keeps only the last that
 * many seconds (dropping whole keyframe spans from the front): a kill cam or
 * an instant replay that records all match long in bounded memory.
 *
 * NOT RECORDED: events, sounds, particles, anything outside components;
 * prefab instance membership and scripts (a puppet the player spawns is a
 * plain entity). Play back into the level as it was when recording began:
 * an entity the clip never mentions is left alone.
 *
 * Game thread only, like World.
 */

namespace GameCore
{
    class World;

    /* One component's state: the serializer's bytes, in a versioned chunk
     * (so a clip from an older build loads through the serializer's own
     * version checks). Type is an index into the clip's type table. */
    struct ReplayComponentState
    {
        uint16_t             Type = 0;
        std::vector<uint8_t> Bytes;
    };

    enum EReplayEntityFlags : uint8_t
    {
        REF_None   = 0,
        REF_Header = 1 << 0,   // Header is set: name, parent and mobility (new, or changed)
    };

    /* One entity in a frame. In a keyframe, or for an entity that just
     * appeared, the whole entity; in a delta, what changed. */
    struct ReplayEntityState
    {
        EntityGuid                        Guid;
        uint8_t                           Flags = REF_None;
        std::vector<uint8_t>              Header;
        std::vector<ReplayComponentState> Components;   // set: added or changed
        std::vector<uint16_t>             Removed;      // type indices removed (deltas only)
    };

    struct ReplayFrame
    {
        double                         Time      = 0.0;   // seconds, the recorder's clock
        bool                           bKeyframe = false;
        std::vector<ReplayEntityState> Entities;
        std::vector<EntityGuid>        Destroyed;   // deltas only
    };

    /* A recording. Plain data: copy it, save it, play it any number of times. */
    class GAMECORE_API ReplayClip
    {
    public:
        /* Free text for the host -- by convention the level it was recorded
         * in, so a viewer can load it before playing. Saved with the clip. */
        std::string LevelPath;

        void Clear();
        bool IsEmpty() const { return Frames.empty(); }

        size_t             GetFrameCount() const { return Frames.size(); }
        const ReplayFrame& GetFrame(size_t InIndex) const { return Frames[InIndex]; }

        /* 0 when empty. Time of the first and last frame. */
        double GetStartTime() const;
        double GetEndTime() const;
        double GetDuration() const { return GetEndTime() - GetStartTime(); }

        /* The last frame at or before InTime; 0 before the first. NO_FRAME when empty. */
        enum : size_t { NO_FRAME = ~size_t(0) };
        size_t FindFrame(double InTime) const;
        /* The keyframe the frame at InIndex rebuilds from. */
        size_t FindKeyframe(size_t InIndex) const;

        size_t      GetTypeCount() const { return TypeNames.size(); }
        const char* GetTypeName(size_t InIndex) const { return TypeNames[InIndex].c_str(); }
        /* The table index for InName, added if new. */
        uint16_t    InternType(const char* InName);

        /* Bytes held by the frames' payloads: for a memory budget, roughly. */
        size_t GetPayloadBytes() const;

        /* Both directions, as one 'RPLY' chunk. A load replaces the clip;
         * false (and the clip cleared) if the archive is bad. */
        bool Serialize(EngineCore::Archive& Ar);

    private:
        friend class ReplayRecorder;

        std::vector<std::string> TypeNames;
        std::vector<ReplayFrame> Frames;
    };

    /* Through SaveArchiveToFile / LoadArchiveFromFile. */
    GAMECORE_API bool SaveReplayClip(const std::string& InPath, ReplayClip& InClip);
    GAMECORE_API bool LoadReplayClip(const std::string& InPath, ReplayClip& OutClip);

    // === Recording ==========================================================

    struct ReplayRecordSettings
    {
        /* Start()'s samples a second; <= 0 samples every tick. Capture()
         * ignores it: a call is a frame. */
        float SampleRate       = 30.0f;
        /* Seconds between keyframes: shorter seeks faster, longer is smaller. */
        float KeyframeInterval = 2.0f;
        /* > 0 keeps only about the last this many seconds (at least this
         * many: whole keyframe spans are dropped). 0 keeps everything. */
        float MaxDuration      = 0.0f;
        /* Static entities never move in play, so they are skipped. */
        bool  bRecordStatic    = false;
    };

    /* True to record InEntity. Called for each live candidate every sample. */
    using ReplayEntityFilterFn = bool (*)(World& InWorld, EntityID InEntity, void* InUserData);

    class GAMECORE_API ReplayRecorder
    {
    public:
        /* Appends to OutClip, which must outlive the recorder. A clip that
         * already holds frames is cleared first. */
        explicit ReplayRecorder(ReplayClip& OutClip, const ReplayRecordSettings& InSettings = ReplayRecordSettings());
        ~ReplayRecorder();

        ReplayRecorder(const ReplayRecorder&)            = delete;
        ReplayRecorder& operator=(const ReplayRecorder&) = delete;

        void SetEntityFilter(ReplayEntityFilterFn InFn, void* InUserData);
        /* Never record InType (per-run bookkeeping a game serializes for
         * saves but that playback must not overwrite). */
        void ExcludeComponentType(ComponentTypeID InType);
        /* Excludes every registered type but these, by name ("Transform",
         * "Render", "Animation"): what is seen, not gameplay state -- the
         * usual choice for playing over a live level. Empty changes nothing. */
        void RecordOnly(const std::vector<std::string>& InTypeNames);

        /* One frame now, stamped InTime (seconds, increasing). */
        void Capture(World& InWorld, double InTime);

        /* Registers a TP_PostUpdate system on InWorld that captures at
         * SampleRate, stamped with the World's accumulated tick time. Stop()
         * (or the destructor) unregisters it; InWorld must outlive that. */
        void Start(World& InWorld);
        void Stop();
        bool IsRecording() const { return Attached != nullptr; }

        const ReplayRecordSettings& GetSettings() const { return Settings; }

    private:
        struct EntitySnapshot
        {
            std::vector<uint8_t>                            Header;
            std::unordered_map<uint16_t, std::vector<uint8_t>> Components;
            std::vector<uint16_t>                           Order;   // component type indices, as captured
        };

        static void TickSystem(World& InWorld, float InDeltaSeconds, void* InUserData);
        void TrimToMaxDuration();

        ReplayClip&          Clip;
        ReplayRecordSettings Settings;

        ReplayEntityFilterFn Filter         = nullptr;
        void*                FilterUserData = nullptr;
        std::vector<uint8_t> Excluded;   // by ComponentTypeID, 1 = excluded

        std::vector<EntityGuid>                                    PreviousOrder;
        std::unordered_map<EntityGuid, EntitySnapshot, EntityGuidHash> Previous;
        double LastKeyframeTime = 0.0;

        World* Attached       = nullptr;
        double Clock          = 0.0;
        double SinceSample    = 0.0;
    };

    // === Playback ===========================================================

    class GAMECORE_API ReplayPlayer
    {
    public:
        /* Plays InClip into InWorld; both must outlive the player. Nothing
         * is written until the first Seek or Advance. */
        ReplayPlayer(World& InWorld, const ReplayClip& InClip);
        ~ReplayPlayer();

        ReplayPlayer(const ReplayPlayer&)            = delete;
        ReplayPlayer& operator=(const ReplayPlayer&) = delete;

        /* InWorld shows the clip at InTime (clamped to the clip). */
        void Seek(double InTime);

        /* If playing, moves the time by InDeltaSeconds * rate and seeks;
         * pauses on reaching either end. */
        void Advance(float InDeltaSeconds);

        void Play()  { bPlaying = true; }
        void Pause() { bPlaying = false; }
        bool IsPlaying() const { return bPlaying; }
        /* Negative plays backwards. */
        void  SetRate(float InRate) { Rate = InRate; }
        float GetRate() const { return Rate; }
        void SetLooping(bool bInLoop) { bLoop = bInLoop; }

        void SetInterpolation(bool bInEnabled) { bInterpolate = bInEnabled; }

        /* HOLD: every Seek writes every recorded entity whole, not only what
         * changed -- for playing over a World whose own systems still run
         * (a live level), so the clip wins each frame wherever they moved
         * things. Seek from a system that runs after them (late in
         * TP_PostUpdate, before RenderCore.Animation: SO_BeforeScripts). */
        void SetHold(bool bInHold) { bHold = bInHold; }

        double GetTime() const { return Time; }
        bool   IsAtEnd() const;

        /* The live entity playing InGuid, InvalidEntityID() if none (yet). */
        EntityID FindEntity(const EntityGuid& InGuid) const;

        /* Destroys every entity the player spawned (puppets of entities the
         * world did not have), e.g. before handing the World back to play. */
        void DestroySpawnedEntities();

    private:
        struct EntityState
        {
            std::vector<uint8_t>                               Header;
            std::unordered_map<uint16_t, std::vector<uint8_t>> Components;
        };
        struct Dirty
        {
            bool                  bAll    = false;
            bool                  bHeader = false;
            std::vector<uint16_t> Types;
        };

        void ApplyFrameToState(size_t InIndex);
        void MarkDirty(const EntityGuid& InGuid, bool bInHeader, uint16_t InType);
        void FlushToWorld();
        void Interpolate(size_t InFrame, double InTime);
        EntityID Resolve(const EntityGuid& InGuid) const;
        void ApplyComponent(EntityID InEntity, uint16_t InType, const std::vector<uint8_t>& InBytes);

        World&            TargetWorld;
        const ReplayClip& Clip;

        /* The clip's type index -> this build's ComponentTypeID; INVALID to skip. */
        std::vector<ComponentTypeID> Types;
        uint16_t                     TransformType = 0xFFFF;   // the clip's index for Transform

        std::unordered_map<EntityGuid, EntityState, EntityGuidHash> State;
        std::unordered_map<EntityGuid, Dirty, EntityGuidHash>       DirtySet;
        std::vector<EntityGuid>                                     Removed;
        std::unordered_map<EntityGuid, EntityID, EntityGuidHash>    Bound;
        std::unordered_set<EntityGuid, EntityGuidHash>              Spawned;
        std::vector<EntityGuid>                                     Interpolated;

        size_t Current      = ReplayClip::NO_FRAME;
        double Time         = 0.0;
        float  Rate         = 1.0f;
        bool   bPlaying     = false;
        bool   bLoop        = false;
        bool   bInterpolate = true;
        bool   bHold        = false;
        bool   bStarted     = false;
    };
}

#endif //GAMECORE_REPLAY_H

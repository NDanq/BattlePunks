#ifndef RENDERCORE_ANIMATIONSYSTEM_H
#define RENDERCORE_ANIMATIONSYSTEM_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Matrix.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace GameCore
{
    class Entity;
    class World;
}

/*
 * A world's animation (ANIMATION.md): an AnimInstance for each Animation
 * component, kept beside the components rather than in them (they are POD,
 * and copied whole every frame).
 *
 * GAME THREAD. A world bound to a renderer has one (WorldBridge attaches
 * it); it ticks in TP_PostUpdate, after gameplay has set parameters and
 * before the frame's extraction reads the poses. A world that does not tick
 * -- the editor's, outside Play -- is synced instead: each new or changed
 * component evaluated where it starts, and left there.
 *
 * After each advance, an entity with an Anim Socket is moved onto its bone
 * of its parent's pose (AnimSocketComponent.h).
 *
 * Each notify a tick passes is emitted on the world's EventBus as
 * "AnimNotify" with (entity, notify name) -- Lua: Events.On("AnimNotify", ...)
 * -- and plays its particle system, if it names one, at its bone. A state
 * that plays once emits "AnimEnded" (entity, state name, layer) as it ends.
 */
namespace RenderCore
{
    struct MeshData;

    class RENDERCORE_API AnimationSystem
    {
    public:
        /* InWorld's system, made on the first Attach; each Attach needs a Detach. */
        static AnimationSystem* Attach(GameCore::World& InWorld);
        static void             Detach(GameCore::World& InWorld);

        /* Null if InWorld has none. */
        static AnimationSystem* Find(const GameCore::World* InWorld);

        /*
         * How IK finds the ground (AnimInstance's IK probes): the nearest hit
         * of a ray from InFrom along InDirection within InLength, not on
         * InIgnore (the character itself), its point and normal. RenderCore
         * has no physics: NimbleEngine sets one on the world's physics
         * (PhysicsFacade.h); none set, feet are as animated. Game thread.
         */
        using GroundQueryFn = bool (*)(GameCore::World& InWorld, GameCore::EntityID InIgnore, const EngineCore::Vector3& InFrom,
                                       const EngineCore::Vector3& InDirection, float InLength, EngineCore::Vector3& OutPoint,
                                       EngineCore::Vector3& OutNormal);
        static void          SetGroundQuery(GroundQueryFn InQuery);
        static GroundQueryFn GetGroundQuery();

        /* Every Animation component's instance made, rebound to what its
         * fields and mesh now name, and evaluated if it changed. No time passes. */
        void Sync();

        /* Sync, then each unpaused instance advances by InDeltaSeconds times
         * its PlayRate; notifies are emitted. */
        void Tick(float InDeltaSeconds);

        /* Before frame InFrame's extraction (WorldBridge; once per frame
         * however many scenes show the world): a world that did not Tick
         * since the last frame -- the editor's -- is synced, and with
         * bInPreview advanced by InDeltaSeconds so its animations play in
         * the viewport ([Animation] PreviewInEditor). A preview emits
         * nothing and writes to no component. */
        void PrepareFrame(uint64_t InFrame, float InDeltaSeconds, bool bInPreview);
        /* InEntity's instance; null if it has no Animation component, or no
         * skeleton to pose. */
        AnimInstance*       FindInstance(GameCore::EntityID InEntity);
        const AnimInstance* FindInstance(GameCore::EntityID InEntity) const;

        /* InEntity's bone InBone in the world (its entity's transform after
         * the bone's model-space pose); false if there is no such bone. */
        bool GetBoneWorldPose(GameCore::EntityID InEntity, const char* InBone, GameCore::TransformPose& OutPose) const;

        /*
         * Who plays a notify's Sound (AnimNotify::Sound) where it fires -- at
         * its bone, else its entity. RenderCore plays no sound itself:
         * NimbleEngine's GameSession sets this to the level's AudioWorld.
         * Unset (the default, or without the Audio module), a notify's sound
         * is ignored. Called during Tick, on the game thread.
         */
        using SoundSinkFn = void (*)(const char* InSound, const EngineCore::Vector3& InAt, void* InUserData);
        void SetSoundSink(SoundSinkFn InFn, void* InUserData) { SoundSink = InFn; SoundSinkUserData = InUserData; }

        /* Bumped whenever an instance is made or rebound: the editor's
         * pose-dependent views (sockets) look again. */
        uint64_t GetGeneration() const { return Generation; }

        /* Where frame InFrame is seen from (WorldBridge, per view), and
         * (AddView) what it sees -- InViewProjection, GL clip space: the next
         * ticks spend less on what is far from all of them, or outside every
         * one -- the budget's [Animation] config. None: everything at full
         * rate. A view given by origin alone sees everything. */
        void AddView(uint64_t InFrame, const EngineCore::Vector3& InOrigin, const EngineCore::Matrix4x4& InViewProjection);
        void AddViewOrigin(uint64_t InFrame, const EngineCore::Vector3& InOrigin);

        /* What the last tick did. */
        struct Stats
        {
            uint32_t Updated = 0;    // updated this tick (Reduced among them)
            uint32_t Reduced = 0;    // updated after skipping ticks, their time saved up
            uint32_t Skipped = 0;    // not updated this tick: far
            uint32_t Frozen  = 0;    // updated without a pose: farther
            uint32_t Offscreen = 0;  // outside every view: at the slowest rate (among Reduced and Skipped)
            bool     bParallel = false;

            /* Wall time: the whole tick (sync, gather, update, events,
             * sockets), and the update alone -- the poses, across the job
             * system when parallel. */
            float    Milliseconds       = 0.0f;
            float    UpdateMilliseconds = 0.0f;
        };
        const Stats& GetLastStats() const { return LastStats; }

    private:
        explicit AnimationSystem(GameCore::World& InWorld);
        ~AnimationSystem();

        /* Deleted outright: an exported class gets its implicit copies
         * instantiated, and Entries' unique_ptrs cannot copy (MSVC's C2280). */
        AnimationSystem(const AnimationSystem&)            = delete;
        AnimationSystem& operator=(const AnimationSystem&) = delete;

        static void TickSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

        /* Tick and Preview: bInGame emits notifies and writes back to components. */
        void Advance(float InDeltaSeconds, bool bInGame);

        /* Each Anim Socket's entity onto its bone of its parent's pose. */
        void UpdateSockets();

        /* A notify's particle system, started at its bone: an entity that
         * emits a moment, and is destroyed when its particles are gone. */
        struct PendingEffect
        {
            GameCore::EntityID Owner;
            std::string        System;
            std::string        Bone;
        };
        struct LiveEffect
        {
            GameCore::EntityID Entity;
            double             StopAt    = 0.0;
            double             DestroyAt = 0.0;
        };
        void SpawnEffect(const PendingEffect& InEffect);
        void UpdateEffects();

        struct Entry
        {
            std::unique_ptr<AnimInstance>        Instance;
            std::string                          AnimationPath;
            std::shared_ptr<const MeshData>      Mesh;
            std::shared_ptr<const SkeletonData>  Skeleton;
            std::shared_ptr<const AnimClipData>  Clip;
            std::shared_ptr<const AnimStateData> State;
            uint64_t                             ClipGeneration = 0;   // an anim state's clips, as loaded
            std::string                          RetargetPath;         // the component's, or else the anim state's
            std::shared_ptr<const RetargetData>  Retarget;             // null: none, or not for the mesh's skeleton
            uint64_t                             SkeletonGeneration = 0;   // with a retarget: its source skeletons, as loaded
            uint64_t                             Seen = 0;
            float                                Saved   = 0.0f;       // time saved up while skipped
            int32_t                              Skipped = 0;
        };

        /* Whether any view sees InEntity's character: a sphere round its mesh's
         * box and its bones now, in the world, against each view's frustum. */
        bool IsSeen(const GameCore::Entity& InEntity, const Entry& InEntry, const AnimInstance& InInstance) const;

        GameCore::World&                     World;
        int                                  Attachments = 0;
        std::unordered_map<uint64_t, Entry>  Entries;   // by packed EntityID
        uint64_t                             SyncSerial = 0;
        uint64_t                             Generation = 0;
        bool                                 bTicked    = false;
        double                               Clock      = 0.0;       // game seconds advanced
        std::vector<EngineCore::Vector3>     ViewOrigins;              // the last frame's views
        struct ViewFrustum
        {
            float Planes[6][4] = {};   // xyz inward normal (unit), w: inside where dot(n, p) + w >= 0
            bool  bHas = false;        // false: a view by origin alone, which sees everything
        };
        std::vector<ViewFrustum>             ViewFrustums;             // by view, as ViewOrigins
        uint64_t                             ViewFrame  = ~0ull;
        Stats                                LastStats;
        std::vector<PendingEffect>           Pending;
        SoundSinkFn                          SoundSink         = nullptr;
        void*                                SoundSinkUserData = nullptr;
        std::vector<LiveEffect>              Effects;
        uint64_t                             PreparedFrame = ~0ull;
    };
}

#endif //RENDERCORE_ANIMATIONSYSTEM_H

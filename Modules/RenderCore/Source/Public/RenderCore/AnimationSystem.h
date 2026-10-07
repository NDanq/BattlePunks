#ifndef RENDERCORE_ANIMATIONSYSTEM_H
#define RENDERCORE_ANIMATIONSYSTEM_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace GameCore
{
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
 * Each notify a tick passes is emitted on the world's EventBus as
 * "AnimNotify" with (entity, notify name) -- Lua: Events.On("AnimNotify", ...).
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

        /* Bumped whenever an instance is made or rebound: the editor's
         * pose-dependent views (sockets) look again. */
        uint64_t GetGeneration() const { return Generation; }

    private:
        explicit AnimationSystem(GameCore::World& InWorld);
        ~AnimationSystem();

        static void TickSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

        /* Tick and Preview: bInGame emits notifies and writes back to components. */
        void Advance(float InDeltaSeconds, bool bInGame);

        struct Entry
        {
            std::unique_ptr<AnimInstance>        Instance;
            std::string                          AnimationPath;
            std::shared_ptr<const MeshData>      Mesh;
            std::shared_ptr<const SkeletonData>  Skeleton;
            std::shared_ptr<const AnimClipData>  Clip;
            std::shared_ptr<const AnimStateData> State;
            uint64_t                             ClipGeneration = 0;   // an anim state's clips, as loaded
            uint64_t                             Seen = 0;
        };

        GameCore::World&                     World;
        int                                  Attachments = 0;
        std::unordered_map<uint64_t, Entry>  Entries;   // by packed EntityID
        uint64_t                             SyncSerial = 0;
        uint64_t                             Generation = 0;
        bool                                 bTicked    = false;
        uint64_t                             PreparedFrame = ~0ull;
    };
}

#endif //RENDERCORE_ANIMATIONSYSTEM_H

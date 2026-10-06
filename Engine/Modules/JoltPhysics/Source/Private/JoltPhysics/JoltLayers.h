#ifndef JOLTPHYSICS_JOLTLAYERS_H
#define JOLTPHYSICS_JOLTLAYERS_H

#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

/*
 * A Rigid Body's Layer and Mask, and whether it moves, packed into Jolt's
 * object layer (32 bits, JPH_OBJECT_LAYER_BITS in CMakeLists.txt):
 *
 *   bit 31      moving (Dynamic or Kinematic)
 *   bits 15-30  Mask  -- what it collides with
 *   bits 0-14   Layer -- what it is
 *
 * Two bodies collide when each one's Layer is in the other's Mask -- the rule
 * CollisionComponent uses -- and at least one moves. Jolt's broad phase keeps
 * two trees, one for what never moves and one for what does.
 */
namespace JoltPhysics::Layers
{
    constexpr uint32_t MovingBit = 0x80000000u;
    constexpr uint32_t LayerBits = 0x7FFFu;
    constexpr uint32_t MaskBits  = 0xFFFFu;
    constexpr int      MaskShift = 15;

    namespace BroadPhase
    {
        static constexpr JPH::BroadPhaseLayer NonMoving(0);
        static constexpr JPH::BroadPhaseLayer Moving(1);
        static constexpr JPH::uint            Count = 2;
    }

    inline JPH::ObjectLayer Make(const int32_t InLayer, const int32_t InMask, const bool bInMoving)
    {
        return JPH::ObjectLayer((bInMoving ? MovingBit : 0u)
                                | ((uint32_t(InMask) & MaskBits) << MaskShift)
                                | (uint32_t(InLayer) & LayerBits));
    }

    inline bool     IsMoving(const JPH::ObjectLayer InLayer) { return (uint32_t(InLayer) & MovingBit) != 0; }
    inline uint32_t LayerOf(const JPH::ObjectLayer InLayer)  { return uint32_t(InLayer) & LayerBits; }
    inline uint32_t MaskOf(const JPH::ObjectLayer InLayer)   { return (uint32_t(InLayer) >> MaskShift) & MaskBits; }

    class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
    {
    public:
        JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhase::Count; }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer InLayer) const override
        {
            return IsMoving(InLayer) ? BroadPhase::Moving : BroadPhase::NonMoving;
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(const JPH::BroadPhaseLayer InLayer) const override
        {
            return InLayer == BroadPhase::Moving ? "Moving" : "NonMoving";
        }
#endif
    };

    class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(const JPH::ObjectLayer InLayer, const JPH::BroadPhaseLayer InBroadPhase) const override
        {
            return IsMoving(InLayer) || InBroadPhase == BroadPhase::Moving;
        }
    };

    class ObjectPairs final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(const JPH::ObjectLayer InA, const JPH::ObjectLayer InB) const override
        {
            if(!IsMoving(InA) && !IsMoving(InB)) { return false; }
            return (LayerOf(InA) & MaskOf(InB)) != 0 && (LayerOf(InB) & MaskOf(InA)) != 0;
        }
    };

    /* A query's mask: bodies whose Layer is in it. */
    class QueryMask final : public JPH::ObjectLayerFilter
    {
    public:
        explicit QueryMask(const int32_t InMask) : Mask(uint32_t(InMask) & LayerBits) {}

        bool ShouldCollide(const JPH::ObjectLayer InLayer) const override
        {
            return (LayerOf(InLayer) & Mask) != 0;
        }

    private:
        uint32_t Mask;
    };
}

#endif //JOLTPHYSICS_JOLTLAYERS_H

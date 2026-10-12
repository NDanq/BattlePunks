#include "Public/NimbleEngine/LagCompensation.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/* LagCompensation.h: the server's second of positions, and tests against it. */

namespace NimbleEngine
{
    GAMECORE_DEFINE_COMPONENT(LagCompensatedComponent, "LagCompensated")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(LagCompensatedComponent, 1)
    {
        Ar.Field("Radius", C.Radius);
        Ar.Field("Bottom", C.Bottom);
        Ar.Field("Top",    C.Top);
        Ar.Flag("Enabled", C.bEnabled);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(LagCompensatedComponent)
    {
        Bind.Field("Radius", &LagCompensatedComponent::Radius);
        Bind.Field("Bottom", &LagCompensatedComponent::Bottom);
        Bind.Field("Top",    &LagCompensatedComponent::Top);
        Bind.Flag("Enabled", &LagCompensatedComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(LagCompensatedComponent)
    {
        Props.Field("Radius", &LagCompensatedComponent::Radius);
        Props.Field("Bottom", &LagCompensatedComponent::Bottom);
        Props.Field("Top",    &LagCompensatedComponent::Top);
        Props.Flag("Enabled", &LagCompensatedComponent::bEnabled);
    }
#endif

    namespace
    {
        using EngineCore::Vector3;

        constexpr uint32_t KeptMs = 1000;

        struct Sample
        {
            uint32_t TimeMs = 0;
            Vector3  Position;
        };

        /* Per World, per entity (EntityID::Pack()): oldest first. */
        using History = std::unordered_map<uint64_t, std::deque<Sample>>;
        std::unordered_map<const GameCore::World*, History> GHistories;

        Vector3 WorldPositionOf(GameCore::World& InWorld, const GameCore::EntityID InEntity)
        {
            return InWorld.GetEntity(InEntity).GetWorldPose().Position;
        }

        /* InEntity at InTimeMs: between the samples either side, else the nearest end. */
        Vector3 PositionAt(GameCore::World& InWorld, const GameCore::EntityID InEntity, const uint32_t InTimeMs)
        {
            if(InTimeMs == 0) { return WorldPositionOf(InWorld, InEntity); }
            const auto World = GHistories.find(&InWorld);
            if(World == GHistories.end()) { return WorldPositionOf(InWorld, InEntity); }
            const auto Found = World->second.find(InEntity.Pack());
            if(Found == World->second.end() || Found->second.empty()) { return WorldPositionOf(InWorld, InEntity); }

            const std::deque<Sample>& H = Found->second;
            if(int32_t(InTimeMs - H.front().TimeMs) <= 0) { return H.front().Position; }
            if(int32_t(InTimeMs - H.back().TimeMs) >= 0)  { return H.back().Position; }
            for(size_t Index = 1; Index < H.size(); ++Index)
            {
                if(int32_t(InTimeMs - H[Index].TimeMs) > 0) { continue; }
                const Sample& A = H[Index - 1];
                const Sample& B = H[Index];
                const double Span  = double(B.TimeMs - A.TimeMs);
                const double Alpha = Span > 0.0 ? double(InTimeMs - A.TimeMs) / Span : 1.0;
                return A.Position + (B.Position - A.Position) * float(Alpha);
            }
            return H.back().Position;
        }

        /* InEntity is InIgnore, or under it (a fighter's own gun). */
        bool IsIgnored(GameCore::World& InWorld, GameCore::EntityID InEntity, const GameCore::EntityID InIgnore)
        {
            if(!InIgnore.IsSet()) { return false; }
            for(int Depth = 0; InEntity.IsSet() && Depth < 64; ++Depth)
            {
                if(InEntity == InIgnore) { return true; }
                InEntity = InWorld.GetEntity(InEntity).GetParent();
            }
            return false;
        }

        /* The unit ray against a standing capsule at InBase: the distance along it, or < 0. */
        double RayCapsule(const Vector3& InOrigin, const Vector3& InDir, const Vector3& InBase, const LagCompensatedComponent& InShape)
        {
            const double R   = std::max(1.0, double(InShape.Radius));
            const double Lo  = double(InShape.Bottom) + R;
            const double Hi  = std::max(Lo, double(InShape.Top) - R);
            const double Seg = Hi - Lo;

            /* Closest approach of the ray to the capsule's axis, a vertical segment from A. */
            const double AX = InBase.GetX(), AY = InBase.GetY() + Lo, AZ = InBase.GetZ();
            const double WX = double(InOrigin.GetX()) - AX, WY = double(InOrigin.GetY()) - AY, WZ = double(InOrigin.GetZ()) - AZ;
            const double DX = InDir.GetX(), DY = InDir.GetY(), DZ = InDir.GetZ();
            const double B   = DY;
            const double D   = DX * WX + DY * WY + DZ * WZ;
            const double Den = 1.0 - B * B;
            double T = Den > 1e-6 ? (B * WY - D) / Den : -D;
            double S = std::clamp(WY + T * B, 0.0, Seg);
            T = std::max(0.0, (S - WY) * B - D);   // the ray's nearest point to that axis point
            S = std::clamp(WY + T * B, 0.0, Seg);

            const double PX = WX + DX * T, PY = WY + DY * T - S, PZ = WZ + DZ * T;
            const double Dist2 = PX * PX + PY * PY + PZ * PZ;
            if(Dist2 > R * R) { return -1.0; }
            return std::max(0.0, T - std::sqrt(R * R - Dist2));
        }
    }

    bool LagCompensatedRaycastAt(GameCore::World& InWorld, const uint32_t InTimeMs, const GameCore::EntityID InIgnore,
                                 const Vector3& InOrigin, const Vector3& InDirection, const float InMaxDistance,
                                 LagCompensatedHit& OutHit, const int32_t InWorldMask)
    {
        OutHit = LagCompensatedHit();
        const float Length = float(InDirection.GetLength());
        if(Length < 1e-6f || InMaxDistance <= 0.0f) { return false; }
        const Vector3 Dir = InDirection * (1.0f / Length);

        /* The world as it is: how far the ray gets. */
        bool   bHit    = false;
        double Nearest = InMaxDistance;
        PhysicsRaycastHit World;
        if(PhysicsRaycast(InWorld, InOrigin, Dir, InMaxDistance, World, InWorldMask) && !IsIgnored(InWorld, World.Entity, InIgnore))
        {
            bHit           = true;
            Nearest        = World.Distance;
            OutHit.Entity  = World.Entity;
            OutHit.Distance = World.Distance;
            OutHit.Point   = World.Point;
        }

        /* Then each compensated entity, where it stood then. */
        InWorld.Each<LagCompensatedComponent>([&](GameCore::Entity InEntity, LagCompensatedComponent& InShape)
        {
            if(!InShape.bEnabled || IsIgnored(InWorld, InEntity.GetID(), InIgnore)) { return; }
            const Vector3 Base = PositionAt(InWorld, InEntity.GetID(), InTimeMs);
            const double  T    = RayCapsule(InOrigin, Dir, Base, InShape);
            if(T < 0.0 || T >= Nearest) { return; }
            bHit                = true;
            Nearest             = T;
            OutHit.Entity       = InEntity.GetID();
            OutHit.Distance     = float(T);
            OutHit.Point        = InOrigin + Dir * float(T);
            OutHit.bCompensated = true;
            OutHit.Height       = OutHit.Point.GetY() - Base.GetY();
        });
        return bHit;
    }

    bool LagCompensatedRaycast(GameCore::World& InWorld, const GameCore::RpcContext& InCall, const Vector3& InOrigin,
                               const Vector3& InDirection, const float InMaxDistance, LagCompensatedHit& OutHit, const int32_t InWorldMask)
    {
        return LagCompensatedRaycastAt(InWorld, InCall.bRemote ? InCall.ViewTimeMs : 0u, InCall.Entity, InOrigin, InDirection,
                                       InMaxDistance, OutHit, InWorldMask);
    }

    bool GetLagCompensatedPosition(GameCore::World& InWorld, const GameCore::EntityID InEntity, const uint32_t InTimeMs,
                                   Vector3& OutPosition)
    {
        if(!InWorld.IsAlive(InEntity) || !InWorld.GetEntity(InEntity).HasComponent<LagCompensatedComponent>()) { return false; }
        OutPosition = PositionAt(InWorld, InEntity, InTimeMs);
        return true;
    }

    void RecordLagCompensation(GameCore::World& InWorld, const uint32_t InTimeMs)
    {
        if(InTimeMs == 0) { return; }
        History& Kept = GHistories[&InWorld];

        /* Who is still here: anyone not recorded this frame is dropped. */
        std::unordered_map<uint64_t, bool> Seen;
        InWorld.Each<LagCompensatedComponent>([&](GameCore::Entity InEntity, LagCompensatedComponent& InShape)
        {
            if(!InShape.bEnabled) { return; }
            const uint64_t Key = InEntity.GetID().Pack();
            std::deque<Sample>& H = Kept[Key];
            if(!H.empty() && H.back().TimeMs == InTimeMs) { H.back().Position = WorldPositionOf(InWorld, InEntity.GetID()); }
            else                                          { H.push_back({ InTimeMs, WorldPositionOf(InWorld, InEntity.GetID()) }); }
            /* A second, and the sample just before it, to blend from. */
            while(H.size() > 2 && InTimeMs - H[1].TimeMs > KeptMs) { H.pop_front(); }
            Seen[Key] = true;
        });
        for(auto It = Kept.begin(); It != Kept.end();)
        {
            if(Seen.count(It->first) == 0) { It = Kept.erase(It); }
            else                           { ++It; }
        }
    }

    void ClearLagCompensation(const GameCore::World& InWorld)
    {
        GHistories.erase(&InWorld);
    }
}

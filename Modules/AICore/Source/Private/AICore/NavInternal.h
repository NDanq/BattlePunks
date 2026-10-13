#ifndef AICORE_NAVINTERNAL_H
#define AICORE_NAVINTERNAL_H

#include <cmath>
#include <cstdint>
#include <cstring>

#include "DetourNavMeshQuery.h"
#include "Public/AICore/NavTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

/* What AICore's navigation sources share, and nothing outside them sees:
 * conversions to Recast's float[3], the query filter, hashing. */
namespace AICore
{
    namespace NavDetail
    {
        inline void ToFloat3(const EngineCore::Vector3& InVector, float (&OutFloats)[3])
        {
            OutFloats[0] = float(InVector.GetX());
            OutFloats[1] = float(InVector.GetY());
            OutFloats[2] = float(InVector.GetZ());
        }

        inline EngineCore::Vector3 FromFloat3(const float* InFloats)
        {
            return EngineCore::Vector3(InFloats[0], InFloats[1], InFloats[2]);
        }

        /* InMatrix applied to a point: rotation, scale and translation. */
        inline EngineCore::Vector3 TransformPoint(const EngineCore::Matrix4x4& InMatrix, const float InX, const float InY, const float InZ)
        {
            const EngineCore::Vector4 Result = InMatrix * EngineCore::Vector4(InX, InY, InZ, 1.0);
            return EngineCore::Vector3(Result.GetX(), Result.GetY(), Result.GetZ());
        }

        inline float Distance2D(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB)
        {
            const double DX = double(InA.GetX()) - double(InB.GetX());
            const double DZ = double(InA.GetZ()) - double(InB.GetZ());
            return float(std::sqrt(DX * DX + DZ * DZ));
        }

        inline float Distance3D(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB)
        {
            const double DX = double(InA.GetX()) - double(InB.GetX());
            const double DY = double(InA.GetY()) - double(InB.GetY());
            const double DZ = double(InA.GetZ()) - double(InB.GetZ());
            return float(std::sqrt(DX * DX + DY * DY + DZ * DZ));
        }

        /* FNV-1a, a byte at a time: stable across builds and platforms. */
        inline void HashBytes(uint64_t& InOutHash, const void* InBytes, const size_t InCount)
        {
            const uint8_t* Bytes = static_cast<const uint8_t*>(InBytes);
            for(size_t Index = 0; Index < InCount; ++Index)
            {
                InOutHash ^= Bytes[Index];
                InOutHash *= 1099511628211ull;
            }
        }
        constexpr uint64_t HASH_SEED = 1469598103934665603ull;

        /* Detour's filter, from ours. DT_VIRTUAL_QUERYFILTER (the Recast
         * build's PUBLIC define) makes its cost virtual, which is what a
         * per-team or per-agent cost would override; today the area table
         * is all it needs. */
        class NavQueryFilter : public dtQueryFilter
        {
        public:
            explicit NavQueryFilter(const NavFilter* InFilter)
            {
                const NavFilter Default;
                const NavFilter& Filter = InFilter ? *InFilter : Default;
                setIncludeFlags(Filter.IncludeFlags);
                setExcludeFlags(Filter.ExcludeFlags);
                for(int Area = 0; Area < int(NAV_AREA_COUNT) && Area < DT_MAX_AREAS; ++Area)
                {
                    setAreaCost(Area, Filter.AreaCost[Area] > 0.0f ? Filter.AreaCost[Area] : 1.0f);
                }
            }
        };
    }
}

#endif //AICORE_NAVINTERNAL_H

#ifndef JOLTPHYSICS_JOLTCONVERT_H
#define JOLTPHYSICS_JOLTCONVERT_H

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Vector.h"

/*
 * The World's terms <-> Jolt's. Lengths: world units / UnitsPerMeter is
 * metres. Rotations: TransformComponent's Euler degrees (Rz * Ry * Rx,
 * TransformMath.h) <-> a Jolt quaternion, through the same 3x3 matrix, so a
 * body turns exactly as its Transform draws. Angular quantities are degrees
 * per second in the World, radians per second in Jolt.
 *
 * Both sides use column vectors, so the matrix crosses unchanged.
 */
namespace JoltPhysics::Convert
{
    inline JPH::Vec3 ToVec3(const EngineCore::Vector3& InV, const double InScale = 1.0)
    {
        return JPH::Vec3(float(InV.GetX() * InScale), float(InV.GetY() * InScale), float(InV.GetZ() * InScale));
    }

    inline EngineCore::Vector3 FromVec3(const JPH::Vec3 InV, const double InScale = 1.0)
    {
        return EngineCore::Vector3(double(InV.GetX()) * InScale, double(InV.GetY()) * InScale, double(InV.GetZ()) * InScale);
    }

    inline JPH::RVec3 ToRVec3(const EngineCore::Vector3& InV, const double InScale)
    {
        return JPH::RVec3(JPH::Real(InV.GetX() * InScale), JPH::Real(InV.GetY() * InScale), JPH::Real(InV.GetZ() * InScale));
    }

    inline EngineCore::Vector3 FromRVec3(const JPH::RVec3 InV, const double InScale)
    {
        return EngineCore::Vector3(double(InV.GetX()) * InScale, double(InV.GetY()) * InScale, double(InV.GetZ()) * InScale);
    }

    inline JPH::Quat ToQuat(const GameCore::Rotation3& InR)
    {
        const JPH::Mat44 M(JPH::Vec4(float(InR.M[0][0]), float(InR.M[1][0]), float(InR.M[2][0]), 0.0f),
                           JPH::Vec4(float(InR.M[0][1]), float(InR.M[1][1]), float(InR.M[2][1]), 0.0f),
                           JPH::Vec4(float(InR.M[0][2]), float(InR.M[1][2]), float(InR.M[2][2]), 0.0f),
                           JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return M.GetQuaternion().Normalized();
    }

    inline JPH::Quat EulerToQuat(const EngineCore::Vector3& InDegrees)
    {
        return ToQuat(GameCore::TransformMath::EulerToRotation(InDegrees));
    }

    inline GameCore::Rotation3 ToRotation(const JPH::Quat InQ)
    {
        const JPH::Mat44 M = JPH::Mat44::sRotation(InQ);
        GameCore::Rotation3 R;
        for(int Row = 0; Row < 3; ++Row)
        {
            for(int Col = 0; Col < 3; ++Col)
            {
                R.M[Row][Col] = double(M(JPH::uint(Row), JPH::uint(Col)));
            }
        }
        return R;
    }

    /* InQ as Euler degrees, the equivalent nearest InNear (the Transform's
     * current angles), so a body's angles do not jump between equivalents. */
    inline EngineCore::Vector3 QuatToEuler(const JPH::Quat InQ, const EngineCore::Vector3& InNear)
    {
        return GameCore::TransformMath::RotationToEuler(ToRotation(InQ), InNear);
    }

    inline double DegToRad(const double InDegrees) { return InDegrees * GameCore::TransformMath::Pi / 180.0; }
    inline double RadToDeg(const double InRadians) { return InRadians * 180.0 / GameCore::TransformMath::Pi; }
}

#endif //JOLTPHYSICS_JOLTCONVERT_H

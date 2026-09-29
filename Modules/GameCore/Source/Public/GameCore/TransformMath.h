#ifndef GAMECORE_TRANSFORMMATH_H
#define GAMECORE_TRANSFORMMATH_H

#include <cmath>

#include "Public/Math/Vector.h"

/*
 * Rotation helpers for TransformComponent's Euler angles, in double precision
 * (Matrix4x4 is float under WITH_SIMD, which is too coarse to round-trip
 * angles through). Header-only: nothing here needs to cross the DLL boundary.
 *
 * CONVENTIONS match TransformComponent::GetLocalMatrix and Matrix4x4:
 * column vectors (v' = M * v), right-handed rotations, Euler degrees applied
 * X first, then Y, then Z -- R = Rz * Ry * Rx.
 *
 * The editor's rotate gizmo is the first user: it composes a drag's rotation
 * with the entity's, as matrices, and turns the result back into angles close
 * to the ones the entity started with (see RotationToEuler's InNear).
 */
namespace GameCore
{
    struct Rotation3
    {
        double M[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };

        Rotation3 operator*(const Rotation3& InRight) const
        {
            Rotation3 Out;
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 3; ++Col)
                {
                    Out.M[Row][Col] = M[Row][0] * InRight.M[0][Col]
                                    + M[Row][1] * InRight.M[1][Col]
                                    + M[Row][2] * InRight.M[2][Col];
                }
            }
            return Out;
        }

        EngineCore::Vector3 operator*(const EngineCore::Vector3& InV) const
        {
            const double X = InV.GetX(), Y = InV.GetY(), Z = InV.GetZ();
            return EngineCore::Vector3(M[0][0] * X + M[0][1] * Y + M[0][2] * Z,
                                       M[1][0] * X + M[1][1] * Y + M[1][2] * Z,
                                       M[2][0] * X + M[2][1] * Y + M[2][2] * Z);
        }

        /* Column InAxis: where the rotation takes the unit X (0), Y (1) or Z (2). */
        EngineCore::Vector3 GetAxis(const int InAxis) const
        {
            return EngineCore::Vector3(M[0][InAxis], M[1][InAxis], M[2][InAxis]);
        }
    };

    namespace TransformMath
    {
        constexpr double Pi = 3.14159265358979323846;

        inline double ToRadians(const double InDegrees) { return InDegrees * Pi / 180.0; }
        inline double ToDegrees(const double InRadians) { return InRadians * 180.0 / Pi; }

        /* InDegrees shifted by whole turns to lie within 180 of InNear. */
        inline double UnwrapNear(const double InDegrees, const double InNear)
        {
            return InDegrees + 360.0 * std::round((InNear - InDegrees) / 360.0);
        }

        /* InDegrees about InAxis (any length; zero gives the identity). */
        inline Rotation3 AxisAngle(const EngineCore::Vector3& InAxis, const double InDegrees)
        {
            Rotation3 R;
            const double X = InAxis.GetX(), Y = InAxis.GetY(), Z = InAxis.GetZ();
            const double Length = std::sqrt(X * X + Y * Y + Z * Z);
            if(Length <= 0.0)
            {
                return R;
            }
            const double U = X / Length, V = Y / Length, W = Z / Length;
            const double C = std::cos(ToRadians(InDegrees));
            const double S = std::sin(ToRadians(InDegrees));
            const double T = 1.0 - C;

            R.M[0][0] = C + U * U * T;     R.M[0][1] = U * V * T - W * S; R.M[0][2] = U * W * T + V * S;
            R.M[1][0] = U * V * T + W * S; R.M[1][1] = C + V * V * T;     R.M[1][2] = V * W * T - U * S;
            R.M[2][0] = U * W * T - V * S; R.M[2][1] = V * W * T + U * S; R.M[2][2] = C + W * W * T;
            return R;
        }

        /* Rz * Ry * Rx -- TransformComponent::Rotation as a matrix. */
        inline Rotation3 EulerToRotation(const EngineCore::Vector3& InDegrees)
        {
            return AxisAngle(EngineCore::Vector3(0.0, 0.0, 1.0), InDegrees.GetZ())
                 * AxisAngle(EngineCore::Vector3(0.0, 1.0, 0.0), InDegrees.GetY())
                 * AxisAngle(EngineCore::Vector3(1.0, 0.0, 0.0), InDegrees.GetX());
        }

        /*
         * The Euler angles (degrees, Rz * Ry * Rx) of a pure rotation, picked
         * to be as close as possible to InNear: of the two solutions every
         * rotation has -- (x, y, z) and (x + 180, 180 - y, z + 180) -- the one
         * nearer InNear, each angle then shifted by whole turns to within 180
         * of InNear's. So a gizmo drag from 179 degrees reads 181, not -179.
         *
         * At gimbal lock (Y at +-90) only x - z or x + z is determined; X
         * keeps InNear's value and Z takes the rest.
         */
        inline EngineCore::Vector3 RotationToEuler(const Rotation3& InR, const EngineCore::Vector3& InNear)
        {
            const double NearX = InNear.GetX(), NearY = InNear.GetY(), NearZ = InNear.GetZ();
            const double SinY  = std::fmax(-1.0, std::fmin(1.0, -InR.M[2][0]));

            if(std::abs(SinY) > 0.999999)
            {
                const double Y = SinY > 0.0 ? 90.0 : -90.0;
                const double X = NearX;
                double Z;
                if(SinY > 0.0)
                {
                    /* R01 = sin(x - z), R02 = cos(x - z) */
                    Z = X - ToDegrees(std::atan2(InR.M[0][1], InR.M[0][2]));
                }
                else
                {
                    /* R01 = -sin(x + z), R02 = -cos(x + z) */
                    Z = ToDegrees(std::atan2(-InR.M[0][1], -InR.M[0][2])) - X;
                }
                return EngineCore::Vector3(X, UnwrapNear(Y, NearY), UnwrapNear(Z, NearZ));
            }

            const double X1 = ToDegrees(std::atan2(InR.M[2][1], InR.M[2][2]));
            const double Y1 = ToDegrees(std::asin(SinY));
            const double Z1 = ToDegrees(std::atan2(InR.M[1][0], InR.M[0][0]));

            const double Ax = UnwrapNear(X1, NearX),         Ay = UnwrapNear(Y1, NearY),         Az = UnwrapNear(Z1, NearZ);
            const double Bx = UnwrapNear(X1 + 180.0, NearX), By = UnwrapNear(180.0 - Y1, NearY), Bz = UnwrapNear(Z1 + 180.0, NearZ);

            const double DistA = std::abs(Ax - NearX) + std::abs(Ay - NearY) + std::abs(Az - NearZ);
            const double DistB = std::abs(Bx - NearX) + std::abs(By - NearY) + std::abs(Bz - NearZ);
            return DistA <= DistB ? EngineCore::Vector3(Ax, Ay, Az) : EngineCore::Vector3(Bx, By, Bz);
        }
    }
}

#endif //GAMECORE_TRANSFORMMATH_H

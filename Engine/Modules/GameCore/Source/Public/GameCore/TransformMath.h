#ifndef GAMECORE_TRANSFORMMATH_H
#define GAMECORE_TRANSFORMMATH_H

#include <cmath>

#include "Public/Math/Vector.h"

/*
 * Double-precision rotation helpers for TransformComponent's Euler angles
 * (float Matrix4x4 is too coarse to round-trip angles), and TransformPose:
 * how a parent's transform carries its children.
 *
 * Conventions match GetLocalMatrix: column vectors, right-handed, Euler
 * degrees applied X, Y, then Z (R = Rz * Ry * Rx).
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
         * Euler degrees of a pure rotation, nearest InNear: picks between
         * (x, y, z) and (x + 180, 180 - y, z + 180), then shifts each angle by
         * whole turns, so a drag from 179 reads 181, not -179.
         *
         * At gimbal lock (Y at +-90) X keeps InNear's value; Z takes the rest.
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

        /* The transpose -- the inverse of a pure rotation. */
        inline Rotation3 Transposed(const Rotation3& InR)
        {
            Rotation3 Out;
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 3; ++Col)
                {
                    Out.M[Row][Col] = InR.M[Col][Row];
                }
            }
            return Out;
        }
    }

    /*
     * A position, rotation (Euler degrees) and scale: a TransformComponent's
     * fields without its owner header, for an entity's place in the world.
     *
     * Parenting composes these, not matrices (see DESIGN.md's hierarchy
     * section): rotations multiply, scales multiply per axis, and the
     * child's position is scaled, rotated and offset by the parent's. A
     * parent with non-uniform scale therefore stretches its child along the
     * CHILD's axes, never shears it -- the renderer only takes TRS.
     */
    struct TransformPose
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;   // Euler degrees, Rz * Ry * Rx
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};
    };

    namespace TransformMath
    {
        /* InParent then InChild, InChild given relative to InParent. */
        inline TransformPose Compose(const TransformPose& InParent, const TransformPose& InChild)
        {
            const Rotation3 ParentR = EulerToRotation(InParent.Rotation);
            TransformPose Out;
            Out.Position = InParent.Position + ParentR * (InParent.Scale * InChild.Position);
            Out.Rotation = RotationToEuler(ParentR * EulerToRotation(InChild.Rotation), InParent.Rotation + InChild.Rotation);
            Out.Scale    = InParent.Scale * InChild.Scale;
            return Out;
        }

        /*
         * Compose's inverse: the pose that, composed under InParent, gives
         * InWorld. Euler angles nearest InNearRotation. An axis of InParent's
         * scale at zero cannot be divided out; that axis keeps InWorld's
         * value.
         */
        inline TransformPose Relative(const TransformPose& InParent, const TransformPose& InWorld,
                                      const EngineCore::Vector3& InNearRotation)
        {
            const auto SafeDivide = [](const double InValue, const double InBy)
            {
                return std::abs(InBy) > 1e-9 ? InValue / InBy : InValue;
            };
            const Rotation3 InverseR = Transposed(EulerToRotation(InParent.Rotation));
            const EngineCore::Vector3 Unrotated = InverseR * (InWorld.Position - InParent.Position);

            TransformPose Out;
            Out.Position = EngineCore::Vector3(SafeDivide(Unrotated.GetX(), InParent.Scale.GetX()),
                                               SafeDivide(Unrotated.GetY(), InParent.Scale.GetY()),
                                               SafeDivide(Unrotated.GetZ(), InParent.Scale.GetZ()));
            Out.Rotation = RotationToEuler(InverseR * EulerToRotation(InWorld.Rotation), InNearRotation);
            Out.Scale    = EngineCore::Vector3(SafeDivide(InWorld.Scale.GetX(), InParent.Scale.GetX()),
                                               SafeDivide(InWorld.Scale.GetY(), InParent.Scale.GetY()),
                                               SafeDivide(InWorld.Scale.GetZ(), InParent.Scale.GetZ()));
            return Out;
        }
    }
}

#endif //GAMECORE_TRANSFORMMATH_H

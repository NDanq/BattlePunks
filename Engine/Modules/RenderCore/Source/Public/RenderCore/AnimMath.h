#ifndef RENDERCORE_ANIMMATH_H
#define RENDERCORE_ANIMMATH_H

#include <cmath>

/*
 * The float math animation runs on: a bone's local transform as translation,
 * rotation (a unit quaternion) and scale, and the 3 x 4 affine matrices poses
 * are composed and skinned with. Header-only; no allocation.
 *
 * Conventions: column vectors (a matrix applies on the left, M * v), and an
 * AnimMatrix is the top three rows of a 4 x 4 whose last row is 0 0 0 1 --
 * row-major, so row i is what the skinning shader dots a vertex with
 * (Shaders/Skinning.glsl). A transform applies scale, then rotation, then
 * translation: T * R * S, as glTF and TransformComponent both do.
 */
namespace RenderCore
{
    struct AnimVec3
    {
        float X = 0.0f, Y = 0.0f, Z = 0.0f;
    };

    struct AnimQuat
    {
        float X = 0.0f, Y = 0.0f, Z = 0.0f, W = 1.0f;
    };

    struct AnimTransform
    {
        AnimVec3 Translation;
        AnimQuat Rotation;
        AnimVec3 Scale{1.0f, 1.0f, 1.0f};
    };

    struct AnimMatrix
    {
        float M[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
    };

    namespace AnimMath
    {
        inline AnimVec3 Add(const AnimVec3& A, const AnimVec3& B)   { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
        inline AnimVec3 Sub(const AnimVec3& A, const AnimVec3& B)   { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
        inline AnimVec3 Scale(const AnimVec3& A, const float S)     { return { A.X * S, A.Y * S, A.Z * S }; }
        inline AnimVec3 Mul(const AnimVec3& A, const AnimVec3& B)   { return { A.X * B.X, A.Y * B.Y, A.Z * B.Z }; }
        inline AnimVec3 Lerp(const AnimVec3& A, const AnimVec3& B, const float T)
        {
            return { A.X + (B.X - A.X) * T, A.Y + (B.Y - A.Y) * T, A.Z + (B.Z - A.Z) * T };
        }

        inline float Dot(const AnimQuat& A, const AnimQuat& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W; }

        inline AnimQuat Normalize(const AnimQuat& Q)
        {
            const float Length = std::sqrt(Dot(Q, Q));
            if(Length < 1e-12f) { return AnimQuat(); }
            const float Inv = 1.0f / Length;
            return { Q.X * Inv, Q.Y * Inv, Q.Z * Inv, Q.W * Inv };
        }

        /* Mul(A, B) rotates by B first, then A -- as matrices do. */
        inline AnimQuat Mul(const AnimQuat& A, const AnimQuat& B)
        {
            return { A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y,
                     A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X,
                     A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W,
                     A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z };
        }

        inline AnimQuat Conjugate(const AnimQuat& Q) { return { -Q.X, -Q.Y, -Q.Z, Q.W }; }

        inline AnimVec3 Rotate(const AnimQuat& Q, const AnimVec3& V)
        {
            /* v + 2w(q x v) + 2 q x (q x v) */
            const AnimVec3 U{ Q.X, Q.Y, Q.Z };
            const AnimVec3 T{ 2.0f * (U.Y * V.Z - U.Z * V.Y), 2.0f * (U.Z * V.X - U.X * V.Z), 2.0f * (U.X * V.Y - U.Y * V.X) };
            return { V.X + Q.W * T.X + (U.Y * T.Z - U.Z * T.Y),
                     V.Y + Q.W * T.Y + (U.Z * T.X - U.X * T.Z),
                     V.Z + Q.W * T.Z + (U.X * T.Y - U.Y * T.X) };
        }

        /* Normalised linear interpolation along the shorter arc: what blending
         * poses wants (commutative and cheap); Slerp for sampling keys. */
        inline AnimQuat Nlerp(const AnimQuat& A, const AnimQuat& B, const float T)
        {
            const float Sign = Dot(A, B) < 0.0f ? -1.0f : 1.0f;
            return Normalize({ A.X + (B.X * Sign - A.X) * T, A.Y + (B.Y * Sign - A.Y) * T,
                               A.Z + (B.Z * Sign - A.Z) * T, A.W + (B.W * Sign - A.W) * T });
        }

        inline AnimQuat Slerp(const AnimQuat& A, const AnimQuat& B, const float T)
        {
            float Cos = Dot(A, B);
            AnimQuat To = B;
            if(Cos < 0.0f) { Cos = -Cos; To = { -B.X, -B.Y, -B.Z, -B.W }; }
            if(Cos > 0.9995f) { return Nlerp(A, To, T); }
            const float Angle = std::acos(Cos);
            const float Sin   = std::sin(Angle);
            const float WA    = std::sin((1.0f - T) * Angle) / Sin;
            const float WB    = std::sin(T * Angle) / Sin;
            return { A.X * WA + To.X * WB, A.Y * WA + To.Y * WB, A.Z * WA + To.Z * WB, A.W * WA + To.W * WB };
        }

        inline AnimTransform Lerp(const AnimTransform& A, const AnimTransform& B, const float T)
        {
            AnimTransform Out;
            Out.Translation = Lerp(A.Translation, B.Translation, T);
            Out.Rotation    = Nlerp(A.Rotation, B.Rotation, T);
            Out.Scale       = Lerp(A.Scale, B.Scale, T);
            return Out;
        }

        /* Parent * Child, as transforms (exact without non-uniform scale under rotation). */
        inline AnimTransform Compose(const AnimTransform& Parent, const AnimTransform& Child)
        {
            AnimTransform Out;
            Out.Translation = Add(Parent.Translation, Rotate(Parent.Rotation, Mul(Parent.Scale, Child.Translation)));
            Out.Rotation    = Normalize(Mul(Parent.Rotation, Child.Rotation));
            Out.Scale       = Mul(Parent.Scale, Child.Scale);
            return Out;
        }

        inline AnimMatrix ToMatrix(const AnimTransform& InT)
        {
            const AnimQuat& Q = InT.Rotation;
            const float XX = Q.X * Q.X, YY = Q.Y * Q.Y, ZZ = Q.Z * Q.Z;
            const float XY = Q.X * Q.Y, XZ = Q.X * Q.Z, YZ = Q.Y * Q.Z;
            const float WX = Q.W * Q.X, WY = Q.W * Q.Y, WZ = Q.W * Q.Z;
            AnimMatrix Out;
            Out.M[0][0] = (1.0f - 2.0f * (YY + ZZ)) * InT.Scale.X;
            Out.M[0][1] = (2.0f * (XY - WZ))        * InT.Scale.Y;
            Out.M[0][2] = (2.0f * (XZ + WY))        * InT.Scale.Z;
            Out.M[0][3] = InT.Translation.X;
            Out.M[1][0] = (2.0f * (XY + WZ))        * InT.Scale.X;
            Out.M[1][1] = (1.0f - 2.0f * (XX + ZZ)) * InT.Scale.Y;
            Out.M[1][2] = (2.0f * (YZ - WX))        * InT.Scale.Z;
            Out.M[1][3] = InT.Translation.Y;
            Out.M[2][0] = (2.0f * (XZ - WY))        * InT.Scale.X;
            Out.M[2][1] = (2.0f * (YZ + WX))        * InT.Scale.Y;
            Out.M[2][2] = (1.0f - 2.0f * (XX + YY)) * InT.Scale.Z;
            Out.M[2][3] = InT.Translation.Z;
            return Out;
        }

        inline AnimMatrix Mul(const AnimMatrix& A, const AnimMatrix& B)
        {
            AnimMatrix Out;
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 4; ++Col)
                {
                    Out.M[Row][Col] = A.M[Row][0] * B.M[0][Col] + A.M[Row][1] * B.M[1][Col] + A.M[Row][2] * B.M[2][Col]
                                    + (Col == 3 ? A.M[Row][3] : 0.0f);
                }
            }
            return Out;
        }

        inline AnimVec3 TransformPoint(const AnimMatrix& A, const AnimVec3& P)
        {
            return { A.M[0][0] * P.X + A.M[0][1] * P.Y + A.M[0][2] * P.Z + A.M[0][3],
                     A.M[1][0] * P.X + A.M[1][1] * P.Y + A.M[1][2] * P.Z + A.M[1][3],
                     A.M[2][0] * P.X + A.M[2][1] * P.Y + A.M[2][2] * P.Z + A.M[2][3] };
        }

        /* The general affine inverse; the identity for a singular matrix. */
        inline AnimMatrix Inverse(const AnimMatrix& A)
        {
            const float a = A.M[0][0], b = A.M[0][1], c = A.M[0][2];
            const float d = A.M[1][0], e = A.M[1][1], f = A.M[1][2];
            const float g = A.M[2][0], h = A.M[2][1], i = A.M[2][2];
            const float Det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            if(std::fabs(Det) < 1e-20f) { return AnimMatrix(); }
            const float Inv = 1.0f / Det;
            AnimMatrix Out;
            Out.M[0][0] = (e * i - f * h) * Inv; Out.M[0][1] = (c * h - b * i) * Inv; Out.M[0][2] = (b * f - c * e) * Inv;
            Out.M[1][0] = (f * g - d * i) * Inv; Out.M[1][1] = (a * i - c * g) * Inv; Out.M[1][2] = (c * d - a * f) * Inv;
            Out.M[2][0] = (d * h - e * g) * Inv; Out.M[2][1] = (b * g - a * h) * Inv; Out.M[2][2] = (a * e - b * d) * Inv;
            for(int Row = 0; Row < 3; ++Row)
            {
                Out.M[Row][3] = -(Out.M[Row][0] * A.M[0][3] + Out.M[Row][1] * A.M[1][3] + Out.M[Row][2] * A.M[2][3]);
            }
            return Out;
        }

        /* A rotation matrix (orthonormal columns) as a unit quaternion. */
        inline AnimQuat FromRotation(const float R[3][3])
        {
            AnimQuat Q;
            const float Trace = R[0][0] + R[1][1] + R[2][2];
            if(Trace > 0.0f)
            {
                const float S = std::sqrt(Trace + 1.0f) * 2.0f;
                Q.W = 0.25f * S;
                Q.X = (R[2][1] - R[1][2]) / S;
                Q.Y = (R[0][2] - R[2][0]) / S;
                Q.Z = (R[1][0] - R[0][1]) / S;
            }
            else if(R[0][0] > R[1][1] && R[0][0] > R[2][2])
            {
                const float S = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
                Q.W = (R[2][1] - R[1][2]) / S;
                Q.X = 0.25f * S;
                Q.Y = (R[0][1] + R[1][0]) / S;
                Q.Z = (R[0][2] + R[2][0]) / S;
            }
            else if(R[1][1] > R[2][2])
            {
                const float S = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
                Q.W = (R[0][2] - R[2][0]) / S;
                Q.X = (R[0][1] + R[1][0]) / S;
                Q.Y = 0.25f * S;
                Q.Z = (R[1][2] + R[2][1]) / S;
            }
            else
            {
                const float S = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
                Q.W = (R[1][0] - R[0][1]) / S;
                Q.X = (R[0][2] + R[2][0]) / S;
                Q.Y = (R[1][2] + R[2][1]) / S;
                Q.Z = 0.25f * S;
            }
            return Normalize(Q);
        }

        /* Back to translation, rotation and scale (no shear assumed; a mirror
         * goes into a negative X scale). */
        inline AnimTransform ToTransform(const AnimMatrix& A)
        {
            AnimTransform Out;
            Out.Translation = { A.M[0][3], A.M[1][3], A.M[2][3] };
            float Columns[3];
            for(int Col = 0; Col < 3; ++Col)
            {
                Columns[Col] = std::sqrt(A.M[0][Col] * A.M[0][Col] + A.M[1][Col] * A.M[1][Col] + A.M[2][Col] * A.M[2][Col]);
            }
            const float Det = A.M[0][0] * (A.M[1][1] * A.M[2][2] - A.M[1][2] * A.M[2][1])
                            - A.M[0][1] * (A.M[1][0] * A.M[2][2] - A.M[1][2] * A.M[2][0])
                            + A.M[0][2] * (A.M[1][0] * A.M[2][1] - A.M[1][1] * A.M[2][0]);
            if(Det < 0.0f) { Columns[0] = -Columns[0]; }
            Out.Scale = { Columns[0], Columns[1], Columns[2] };
            float R[3][3];
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 3; ++Col)
                {
                    R[Row][Col] = std::fabs(Columns[Col]) > 1e-12f ? A.M[Row][Col] / Columns[Col] : (Row == Col ? 1.0f : 0.0f);
                }
            }
            Out.Rotation = FromRotation(R);
            return Out;
        }
    }
}

#endif //RENDERCORE_ANIMMATH_H

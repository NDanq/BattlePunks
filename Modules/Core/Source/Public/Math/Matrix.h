#ifndef CORECLIENT_MATRIX_H
#define CORECLIENT_MATRIX_H

#ifndef MATRIX_H
#define MATRIX_H

#include "Public/Math/Vector.h"
#include "Public/Core/CoreLog.h"

#ifndef DEG_TO_RAD
#define DEG_TO_RAD(X) ((X) * 3.14159265358979f / 180.0f)
#endif

namespace EngineCore
{
class Matrix4x4
{
    // Elem selects the storage type.  SIMD path uses float so each row fits
    // exactly in one __m128 register.  Scalar path keeps Real (double).
#ifdef WITH_SIMD
    using Elem = float;
#else
    using Elem = Real;
#endif

public:
#ifdef WITH_SIMD
    alignas(16) Elem Data[4][4];
#else
    Elem Data[4][4];
#endif

    Matrix4x4()
    {
        InitIdentity();
    }

    inline Matrix4x4 InitIdentity()
    {
#ifdef WITH_SIMD
        _mm_storeu_ps(Data[0], _mm_set_ps(0.f, 0.f, 0.f, 1.f));
        _mm_storeu_ps(Data[1], _mm_set_ps(0.f, 0.f, 1.f, 0.f));
        _mm_storeu_ps(Data[2], _mm_set_ps(0.f, 1.f, 0.f, 0.f));
        _mm_storeu_ps(Data[3], _mm_set_ps(1.f, 0.f, 0.f, 0.f));
#else
        for(int i = 0; i < 4; ++i)
            for(int j = 0; j < 4; ++j)
                Data[i][j] = (i == j) ? 1.0f : 0.0f;
#endif
        return *this;
    }

    inline Matrix4x4 InitTranslation(const Vector3& InTranslation)
    {
        Data[0][0] = 1.0f; Data[0][1] = 0.0f; Data[0][2] = 0.0f; Data[0][3] = InTranslation.GetX();
        Data[1][0] = 0.0f; Data[1][1] = 1.0f; Data[1][2] = 0.0f; Data[1][3] = InTranslation.GetY();
        Data[2][0] = 0.0f; Data[2][1] = 0.0f; Data[2][2] = 1.0f; Data[2][3] = InTranslation.GetZ();
        Data[3][0] = 0.0f; Data[3][1] = 0.0f; Data[3][2] = 0.0f; Data[3][3] = 1.0f;
        return *this;
    }

    /*
     * InAngle degrees about InRotationAxis (any length; zero gives identity).
     * Right-handed, column vectors: +Z turns +X toward +Y. Rodrigues' formula,
     * R = cI + s[k]x + (1 - c)kk^T.
     */
    inline Matrix4x4 InitRotation(const float& InAngle, const Vector3& InRotationAxis)
    {
        const Real Length = sqrt(InRotationAxis.GetX() * InRotationAxis.GetX()
                               + InRotationAxis.GetY() * InRotationAxis.GetY()
                               + InRotationAxis.GetZ() * InRotationAxis.GetZ());
        if(Length <= 0.0)
        {
            return InitIdentity();
        }
        const Real U = InRotationAxis.GetX() / Length;
        const Real V = InRotationAxis.GetY() / Length;
        const Real W = InRotationAxis.GetZ() / Length;

        const double Angle = double(InAngle) * 3.14159265358979323846 / 180.0;
        const Real   C     = Real(cos(Angle));
        const Real   S     = Real(sin(Angle));
        const Real   T     = 1 - C;

        Data[0][0] = C + U * U * T;
        Data[0][1] = U * V * T - W * S;
        Data[0][2] = U * W * T + V * S;
        Data[0][3] = 0.0f;

        Data[1][0] = U * V * T + W * S;
        Data[1][1] = C + V * V * T;
        Data[1][2] = V * W * T - U * S;
        Data[1][3] = 0.0f;

        Data[2][0] = U * W * T - V * S;
        Data[2][1] = V * W * T + U * S;
        Data[2][2] = C + W * W * T;
        Data[2][3] = 0.0f;

        Data[3][0] = 0.0f; Data[3][1] = 0.0f; Data[3][2] = 0.0f; Data[3][3] = 1.0f;
        return *this;
    }

    inline Matrix4x4 InitScale(const Vector3& InScale)
    {
        Data[0][0] = InScale.GetX(); Data[0][1] = 0.0f;           Data[0][2] = 0.0f;           Data[0][3] = 0.0f;
        Data[1][0] = 0.0f;           Data[1][1] = InScale.GetY(); Data[1][2] = 0.0f;           Data[1][3] = 0.0f;
        Data[2][0] = 0.0f;           Data[2][1] = 0.0f;           Data[2][2] = InScale.GetZ(); Data[2][3] = 0.0f;
        Data[3][0] = 0.0f;           Data[3][1] = 0.0f;           Data[3][2] = 0.0f;           Data[3][3] = 1.0f;
        return *this;
    }

    inline Matrix4x4 LookAt(const Vector3& Position, const Vector3& Direction, const Vector3& Up)
    {
        Matrix4x4 Orient, Translation;
        const Vector3 D = Direction.GetNormalize();
        const Vector3 R = (Up ^ D).GetNormalize();
        const Vector3 U = D ^ R;

        Orient.Data[0][0] = R.GetX(); Orient.Data[0][1] = R.GetY(); Orient.Data[0][2] = R.GetZ(); Orient.Data[0][3] = 0.f;
        Orient.Data[1][0] = U.GetX(); Orient.Data[1][1] = U.GetY(); Orient.Data[1][2] = U.GetZ(); Orient.Data[1][3] = 0.f;
        Orient.Data[2][0] = D.GetX(); Orient.Data[2][1] = D.GetY(); Orient.Data[2][2] = D.GetZ(); Orient.Data[2][3] = 0.f;
        Orient.Data[3][0] = 0.f;      Orient.Data[3][1] = 0.f;      Orient.Data[3][2] = 0.f;      Orient.Data[3][3] = 1.f;

        Translation.InitTranslation(Position * -1.f);
        return Orient * Translation;
    }

    inline Matrix4x4 InitOrthographicView(const float& InWidth, const float& InHeight, const float& InZNear, const float& InZFar)
    {
        const float ZRange = InZFar - InZNear;
        Data[0][0] = 1.0f / InWidth;  Data[0][1] = 0.0f;             Data[0][2] = 0.0f;               Data[0][3] = 0.0f;
        Data[1][0] = 0.0f;            Data[1][1] = 1.0f / InHeight;  Data[1][2] = 0.0f;               Data[1][3] = 0.0f;
        Data[2][0] = 0.0f;            Data[2][1] = 0.0f;             Data[2][2] = -2.0f / ZRange;     Data[2][3] = (-InZFar - InZNear) / ZRange;
        Data[3][0] = 0.0f;            Data[3][1] = 0.0f;             Data[3][2] = 0.0f;               Data[3][3] = 1.0f;
        return *this;
    }

    inline Matrix4x4 InitOrthographicView(const float& InTop, const float& InBottom, const float& InLeft, const float& InRight, const float& InZNear, const float& InZFar)
    {
        const float ZRange = InZFar - InZNear;
        const float Width  = InRight - InLeft;
        const float Height = InTop - InBottom;
        Data[0][0] = 2.0f / Width;   Data[0][1] = 0.0f;            Data[0][2] = 0.0f;            Data[0][3] = -(InRight + InLeft) / Width;
        Data[1][0] = 0.0f;           Data[1][1] = 2.0f / Height;   Data[1][2] = 0.0f;            Data[1][3] = -(InTop + InBottom) / Height;
        Data[2][0] = 0.0f;           Data[2][1] = 0.0f;            Data[2][2] = -2.0f / ZRange;  Data[2][3] = -(InZFar + InZNear) / ZRange;
        Data[3][0] = 0.0f;           Data[3][1] = 0.0f;            Data[3][2] = 0.0f;            Data[3][3] = 1.0f;
        return *this;
    }

    inline Matrix4x4 InitCamera(const Vector3& Target, const Vector3& Up)
    {
        const Vector3 N = Target.GetNormalize();
        const Vector3 U = (Up ^ N).GetNormalize();
        const Vector3 V = N ^ U;

        Data[0][0] = U.GetX(); Data[0][1] = U.GetY(); Data[0][2] = U.GetZ(); Data[0][3] = 0.0f;
        Data[1][0] = V.GetX(); Data[1][1] = V.GetY(); Data[1][2] = V.GetZ(); Data[1][3] = 0.0f;
        Data[2][0] = N.GetX(); Data[2][1] = N.GetY(); Data[2][2] = N.GetZ(); Data[2][3] = 0.0f;
        Data[3][0] = 0.0f;     Data[3][1] = 0.0f;     Data[3][2] = 0.0f;     Data[3][3] = 1.0f;
        return *this;
    }

    static Matrix4x4 Inverse(const Matrix4x4& InMat)
    {
        float Determinant = GetDeterminant(InMat.Data, 4);
        if(Determinant == 0)
        {
            CORE_CHECK(0, "Determinant doesn't exist");
            return {};
        }
        Elem AdjoinMat[4][4] = {};
        GetAdjoint(InMat.Data, AdjoinMat);
        Matrix4x4 InverseMat;
        for(int i = 0; i < 4; i++)
            for(int j = 0; j < 4; j++)
                InverseMat.Data[i][j] = AdjoinMat[i][j] / Determinant;
        return InverseMat;
    }

    // Matrix * Matrix â€” SIMD: FMA row-linear combination.
    // For each row i of the result: C.row[i] = sum_k( A[i][k] * B.row[k] )
    Matrix4x4 operator*(const Matrix4x4& Right) const
    {
#ifdef WITH_SIMD
        Matrix4x4 Ret;
        const __m128 B0 = _mm_loadu_ps(Right.Data[0]);
        const __m128 B1 = _mm_loadu_ps(Right.Data[1]);
        const __m128 B2 = _mm_loadu_ps(Right.Data[2]);
        const __m128 B3 = _mm_loadu_ps(Right.Data[3]);
        for(int i = 0; i < 4; ++i)
        {
            const __m128 A = _mm_loadu_ps(Data[i]);
            // Broadcast each element of row A and accumulate with FMA.
            // _MM_SHUFFLE(n,n,n,n) broadcasts lane n to all four lanes.
            __m128 Sum = _mm_fmadd_ps(_mm_shuffle_ps(A, A, _MM_SHUFFLE(0, 0, 0, 0)), B0,
                         _mm_fmadd_ps(_mm_shuffle_ps(A, A, _MM_SHUFFLE(1, 1, 1, 1)), B1,
                         _mm_fmadd_ps(_mm_shuffle_ps(A, A, _MM_SHUFFLE(2, 2, 2, 2)), B2,
                          _mm_mul_ps( _mm_shuffle_ps(A, A, _MM_SHUFFLE(3, 3, 3, 3)), B3))));
            _mm_storeu_ps(Ret.Data[i], Sum);
        }
        return Ret;
#else
        Matrix4x4 Ret;
        for(unsigned int i = 0; i < 4; i++)
            for(unsigned int j = 0; j < 4; j++)
                Ret.Data[i][j] = Data[i][0] * Right.Data[0][j]
                               + Data[i][1] * Right.Data[1][j]
                               + Data[i][2] * Right.Data[2][j]
                               + Data[i][3] * Right.Data[3][j];
        return Ret;
#endif
    }

    // Matrix * Vector3 (direction transform, w=0).
    // SIMD: four dp_ps dot products packed into one Vector3 via unpack+movelh.
    Vector3 operator*(const Vector3& Right) const
    {
#ifdef WITH_SIMD
        const __m128 V  = _mm_loadu_ps(Right.GetData()); // {x, y, z, 0}
        // 0x71: multiply x,y,z (mask 0111), store to x lane (mask 0001)
        const __m128 R0 = _mm_dp_ps(_mm_loadu_ps(Data[0]), V, 0x71);
        const __m128 R1 = _mm_dp_ps(_mm_loadu_ps(Data[1]), V, 0x71);
        const __m128 R2 = _mm_dp_ps(_mm_loadu_ps(Data[2]), V, 0x71);
        // Pack {d0,0,0,0}, {d1,0,0,0}, {d2,0,0,0} â†’ {d0, d1, d2, 0}
        const __m128 Lo     = _mm_unpacklo_ps(R0, R1);            // {d0, d1, 0, 0}
        const __m128 Hi     = _mm_unpacklo_ps(R2, _mm_setzero_ps()); // {d2,  0, 0, 0}
        const __m128 Result = _mm_movelh_ps(Lo, Hi);              // {d0, d1, d2, 0}
        Vector3 Ret;
        _mm_storeu_ps(Ret.GetData(), Result);
        return Ret;
#else
        const Real VectorData[4] = { Right.GetX(), Right.GetY(), Right.GetZ(), 0.0 };
        Real ReturnData[4]       = { 0.0, 0.0, 0.0, 0.0 };
        for(unsigned int i = 0; i < 4; i++)
            for(unsigned int j = 0; j < 4; j++)
                ReturnData[i] += Data[i][j] * VectorData[j];
        return { ReturnData[0], ReturnData[1], ReturnData[2] };
#endif
    }

    // Matrix * Vector4.
    // SIMD: four dp_ps dot products packed into one Vector4 via unpack+movelh.
    Vector4 operator*(const Vector4& Right) const
    {
#ifdef WITH_SIMD
        const __m128 V  = _mm_loadu_ps(Right.GetData());
        // 0xF1: multiply all 4 components (mask 1111), store to x lane (mask 0001)
        const __m128 R0 = _mm_dp_ps(_mm_loadu_ps(Data[0]), V, 0xF1);
        const __m128 R1 = _mm_dp_ps(_mm_loadu_ps(Data[1]), V, 0xF1);
        const __m128 R2 = _mm_dp_ps(_mm_loadu_ps(Data[2]), V, 0xF1);
        const __m128 R3 = _mm_dp_ps(_mm_loadu_ps(Data[3]), V, 0xF1);
        // Pack {d0,0,0,0}, {d1,0,0,0}, {d2,0,0,0}, {d3,0,0,0} â†’ {d0,d1,d2,d3}
        const __m128 Lo     = _mm_unpacklo_ps(R0, R1); // {d0, d1, 0, 0}
        const __m128 Hi     = _mm_unpacklo_ps(R2, R3); // {d2, d3, 0, 0}
        const __m128 Result = _mm_movelh_ps(Lo, Hi);   // {d0, d1, d2, d3}
        Vector4 Ret;
        _mm_storeu_ps(Ret.GetData(), Result);
        return Ret;
#else
        const Real VectorData[4] = { Right.GetX(), Right.GetY(), Right.GetZ(), Right.GetW() };
        Real ReturnData[4]       = { 0.0, 0.0, 0.0, 0.0 };
        for(unsigned int i = 0; i < 4; i++)
            for(unsigned int j = 0; j < 4; j++)
                ReturnData[i] += Data[i][j] * VectorData[j];
        return { ReturnData[0], ReturnData[1], ReturnData[2], ReturnData[3] };
#endif
    }

    Elem* GetData() { return Data[0]; }

    void ToDataF(float** InData) const
    {
        for(unsigned int i = 0; i < 4; ++i)
            for(unsigned int j = 0; j < 4; ++j)
                InData[i][j] = float(Data[i][j]);
    }

    // Transpose â€” SIMD: in-register 4x4 float transpose via _MM_TRANSPOSE4_PS.
    [[nodiscard]] Matrix4x4 Transpose() const
    {
#ifdef WITH_SIMD
        __m128 R0 = _mm_loadu_ps(Data[0]);
        __m128 R1 = _mm_loadu_ps(Data[1]);
        __m128 R2 = _mm_loadu_ps(Data[2]);
        __m128 R3 = _mm_loadu_ps(Data[3]);
        _MM_TRANSPOSE4_PS(R0, R1, R2, R3);
        Matrix4x4 Ret;
        _mm_storeu_ps(Ret.Data[0], R0);
        _mm_storeu_ps(Ret.Data[1], R1);
        _mm_storeu_ps(Ret.Data[2], R2);
        _mm_storeu_ps(Ret.Data[3], R3);
        return Ret;
#else
        Matrix4x4 Ret;
        for(unsigned int i = 0; i < 4; i++)
            for(unsigned int j = 0; j < 4; j++)
                Ret.Data[i][j] = Data[j][i];
        return Ret;
#endif
    }

    [[nodiscard]] Vector4 ToVector4() const
    {
        const Real X = Data[0][0] + Data[0][1] + Data[0][2] + Data[0][3];
        const Real Y = Data[1][0] + Data[1][1] + Data[1][2] + Data[1][3];
        const Real Z = Data[2][0] + Data[2][1] + Data[2][2] + Data[2][3];
        const Real W = Data[3][0] + Data[3][1] + Data[3][2] + Data[3][3];
        return { X, Y, Z, W };
    }

private:
    static inline void GetCoFactor(const Elem InData[4][4], Elem TmpData[4][4], int P, int Q, int N)
    {
        int i = 0, j = 0;
        for(int Row = 0; Row < N; Row++)
        {
            for(int Col = 0; Col < N; Col++)
            {
                if(Row != P && Col != Q)
                {
                    TmpData[i][j++] = InData[Row][Col];
                }
                if(j == N - 1)
                {
                    j = 0;
                    i++;
                }
            }
        }
    }

    static inline float GetDeterminant(const Elem InData[4][4], int N)
    {
        if(N == 1) return InData[0][0];
        Elem Tmp[4][4] = {};
        float Det  = 0.f;
        float Sign = 1.f;
        for(int Idx = 0; Idx < N; Idx++)
        {
            GetCoFactor(InData, Tmp, 0, Idx, N);
            Det  += Sign * InData[0][Idx] * GetDeterminant(Tmp, N - 1);
            Sign  = -Sign;
        }
        return Det;
    }

    static inline void GetAdjoint(const Elem InData[4][4], Elem Adj[4][4])
    {
        Elem Tmp[4][4] = {};
        for(int i = 0; i < 4; i++)
        {
            for(int j = 0; j < 4; j++)
            {
                GetCoFactor(InData, Tmp, i, j, 4);
                const float Sign = ((i + j) % 2 == 0) ? 1.f : -1.f;
                Adj[j][i] = Sign * GetDeterminant(Tmp, 3);
            }
        }
    }
};


}

#ifndef CORE_NO_GLOBAL_ALIASES
using EngineCore::Matrix4x4;
#endif

// See the ABI-guard note in Vector.h -- same reasoning, same failure mode.
#ifdef WITH_SIMD
static_assert(sizeof(EngineCore::Matrix4x4) == 64  && alignof(EngineCore::Matrix4x4) == 16, "Matrix4x4 SIMD layout changed (WITH_SIMD mismatch?)");
#else
static_assert(sizeof(EngineCore::Matrix4x4) == 128, "Matrix4x4 scalar layout changed (WITH_SIMD mismatch?)");
#endif

#endif // MATRIX_H

#endif //CORECLIENT_MATRIX_H

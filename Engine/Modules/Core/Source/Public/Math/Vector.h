#ifndef CORECLIENT_VECTOR_H
#define CORECLIENT_VECTOR_H

#ifndef VECTOR_H
#define VECTOR_H

#include <cmath>

#include "Public/Core/CoreTypes.h"

#ifdef WITH_SIMD
#include <immintrin.h>
#endif

namespace EngineCore
{
// ---------------------------------------------------------------------------
// Vector2
// SIMD path: alignas(16) double[2], operations use __m128d (SSE2).
// Scalar path: Real[2].  Public interface is identical either way.
// ---------------------------------------------------------------------------
class Vector2
{
public:
    Vector2()
    {
#ifdef WITH_SIMD
        _mm_storeu_pd(Data, _mm_setzero_pd());
#else
        Data[0] = Data[1] = 0.0;
#endif
    }

    Vector2(Real InX, Real InY)
    {
#ifdef WITH_SIMD
        // _mm_set_pd(hi, lo): first arg → element[1], second → element[0]
        _mm_storeu_pd(Data, _mm_set_pd(InY, InX));
#else
        Data[0] = InX;
        Data[1] = InY;
#endif
    }

    Real GetX() const { return Data[0]; }
    Real GetY() const { return Data[1]; }

    void SetX(Real InX) { Data[0] = InX; }
    void SetY(Real InY) { Data[1] = InY; }

    Real* GetData() { return Data; }

    Real GetLength() const
    {
#ifdef WITH_SIMD
        __m128d V   = _mm_loadu_pd(Data);
        __m128d Sq  = _mm_mul_pd(V, V);
        __m128d Shf = _mm_shuffle_pd(Sq, Sq, 1); // swap lanes: {y², x²}
        __m128d Sum = _mm_add_pd(Sq, Shf);        // {x²+y², ...}
        return _mm_cvtsd_f64(_mm_sqrt_sd(Sum, Sum));
#else
        return sqrt(Data[0] * Data[0] + Data[1] * Data[1]);
#endif
    }

    Vector2 Normalize()
    {
#ifdef WITH_SIMD
        __m128d V    = _mm_loadu_pd(Data);
        __m128d Sq   = _mm_mul_pd(V, V);
        __m128d Shf  = _mm_shuffle_pd(Sq, Sq, 1);
        __m128d Sum  = _mm_add_pd(Sq, Shf);
        __m128d Len  = _mm_sqrt_sd(Sum, Sum);
        __m128d LenB = _mm_unpacklo_pd(Len, Len); // broadcast to both lanes
        _mm_storeu_pd(Data, _mm_div_pd(V, LenB));
#else
        const Real Length = GetLength();
        Data[0] /= Length;
        Data[1] /= Length;
#endif
        return *this;
    }

    Vector2 GetNormalize() const
    {
#ifdef WITH_SIMD
        __m128d V    = _mm_loadu_pd(Data);
        __m128d Sq   = _mm_mul_pd(V, V);
        __m128d Shf  = _mm_shuffle_pd(Sq, Sq, 1);
        __m128d Sum  = _mm_add_pd(Sq, Shf);
        __m128d Len  = _mm_sqrt_sd(Sum, Sum);
        __m128d LenB = _mm_unpacklo_pd(Len, Len);
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_div_pd(V, LenB));
        return Result;
#else
        const Real Length = GetLength();
        return Vector2(Data[0] / Length, Data[1] / Length);
#endif
    }

    Real Dot(const Vector2& Other) const
    {
#ifdef WITH_SIMD
        __m128d A   = _mm_loadu_pd(Data);
        __m128d B   = _mm_loadu_pd(Other.Data);
        __m128d Mul = _mm_mul_pd(A, B);
        __m128d Shf = _mm_shuffle_pd(Mul, Mul, 1);
        __m128d Sum = _mm_add_pd(Mul, Shf);
        return _mm_cvtsd_f64(Sum);
#else
        return Data[0] * Other.Data[0] + Data[1] * Other.Data[1];
#endif
    }

    static Vector2 Lerp(const Vector2& InStart, const Vector2& InEnd, const float Step)
    {
#ifdef WITH_SIMD
        const double InvStep = 1.0 - Step;
        __m128d S  = _mm_loadu_pd(InStart.Data);
        __m128d E  = _mm_loadu_pd(InEnd.Data);
        __m128d IS = _mm_set1_pd(InvStep);
        __m128d T  = _mm_set1_pd(static_cast<double>(Step));
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_add_pd(_mm_mul_pd(S, IS), _mm_mul_pd(E, T)));
        return Result;
#else
        const float InvStep = 1.0f - Step;
        return Vector2((InvStep * InStart.Data[0]) + (Step * InEnd.Data[0]),
                       (InvStep * InStart.Data[1]) + (Step * InEnd.Data[1]));
#endif
    }

    Vector2 operator+(const Real Scalar) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_add_pd(_mm_loadu_pd(Data), _mm_set1_pd(Scalar)));
        return Result;
#else
        return Vector2(Data[0] + Scalar, Data[1] + Scalar);
#endif
    }

    Vector2 operator+(const Vector2& Other) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_add_pd(_mm_loadu_pd(Data), _mm_loadu_pd(Other.Data)));
        return Result;
#else
        return Vector2(Data[0] + Other.Data[0], Data[1] + Other.Data[1]);
#endif
    }

    Vector2 operator-(const Real Scalar) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_sub_pd(_mm_loadu_pd(Data), _mm_set1_pd(Scalar)));
        return Result;
#else
        return Vector2(Data[0] - Scalar, Data[1] - Scalar);
#endif
    }

    Vector2 operator-(const Vector2& Other) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_sub_pd(_mm_loadu_pd(Data), _mm_loadu_pd(Other.Data)));
        return Result;
#else
        return Vector2(Data[0] - Other.Data[0], Data[1] - Other.Data[1]);
#endif
    }

    Vector2 operator*(const Real Scalar) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_mul_pd(_mm_loadu_pd(Data), _mm_set1_pd(Scalar)));
        return Result;
#else
        return Vector2(Data[0] * Scalar, Data[1] * Scalar);
#endif
    }

    Vector2 operator*(const Vector2& Other) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_mul_pd(_mm_loadu_pd(Data), _mm_loadu_pd(Other.Data)));
        return Result;
#else
        return Vector2(Data[0] * Other.Data[0], Data[1] * Other.Data[1]);
#endif
    }

    Vector2 operator/(const Real Scalar) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_div_pd(_mm_loadu_pd(Data), _mm_set1_pd(Scalar)));
        return Result;
#else
        return Vector2(Data[0] / Scalar, Data[1] / Scalar);
#endif
    }

    Vector2 operator/(const Vector2& Other) const
    {
#ifdef WITH_SIMD
        Vector2 Result;
        _mm_storeu_pd(Result.Data, _mm_div_pd(_mm_loadu_pd(Data), _mm_loadu_pd(Other.Data)));
        return Result;
#else
        return Vector2(Data[0] / Other.Data[0], Data[1] / Other.Data[1]);
#endif
    }

private:
#ifdef WITH_SIMD
    alignas(16) double Data[2];
#else
    Real Data[2];
#endif
};

// ---------------------------------------------------------------------------
// Vector3
// SIMD path: alignas(16) float[4] with w=0 padding, operations use __m128
//   (SSE4.1 dp_ps for dot/length, shuffle-based cross product).
//   GetData() returns float* under SIMD — callers expecting Real* (e.g. for
//   direct OpenGL upload) should use the scalar accessors instead.
// Scalar path: Real[3], unchanged behaviour.
// ---------------------------------------------------------------------------
class Vector3
{
public:
    Vector3()
    {
#ifdef WITH_SIMD
        _mm_storeu_ps(Data, _mm_setzero_ps());
#else
        Data[0] = Data[1] = Data[2] = 0.f;
#endif
    }

    Vector3(Real InX, Real InY, Real InZ)
    {
#ifdef WITH_SIMD
        // _mm_set_ps(e3, e2, e1, e0): high-to-low argument order
        _mm_storeu_ps(Data, _mm_set_ps(0.f,
                                        static_cast<float>(InZ),
                                        static_cast<float>(InY),
                                        static_cast<float>(InX)));
#else
        Data[0] = InX;
        Data[1] = InY;
        Data[2] = InZ;
#endif
    }

#ifdef WITH_SIMD
    float GetX() const { return Data[0]; }
    float GetY() const { return Data[1]; }
    float GetZ() const { return Data[2]; }
    void SetX(float InX) { Data[0] = InX; }
    void SetY(float InY) { Data[1] = InY; }
    void SetZ(float InZ) { Data[2] = InZ; }
    float* GetData() { return Data; }
    const float* GetData() const { return Data; }
#else
    Real GetX() const { return Data[0]; }
    Real GetY() const { return Data[1]; }
    Real GetZ() const { return Data[2]; }
    void SetX(float InX) { Data[0] = InX; }
    void SetY(float InY) { Data[1] = InY; }
    void SetZ(float InZ) { Data[2] = InZ; }
    Real* GetData() { return Data; }
#endif

    Real GetLength() const
    {
#ifdef WITH_SIMD
        __m128 V   = _mm_loadu_ps(Data);
        __m128 Dot = _mm_dp_ps(V, V, 0x71); // x²+y²+z² → x lane
        return _mm_cvtss_f32(_mm_sqrt_ss(Dot));
#else
        return sqrt(Data[0]*Data[0] + Data[1]*Data[1] + Data[2]*Data[2]);
#endif
    }

    Vector3 Normalize()
    {
#ifdef WITH_SIMD
        __m128 V    = _mm_loadu_ps(Data);
        __m128 Dot  = _mm_dp_ps(V, V, 0x71);
        __m128 Len  = _mm_sqrt_ss(Dot);
        __m128 LenB = _mm_shuffle_ps(Len, Len, _MM_SHUFFLE(0, 0, 0, 0)); // broadcast
        _mm_storeu_ps(Data, _mm_div_ps(V, LenB));
#else
        const Real Length = GetLength();
        Data[0] /= Length;
        Data[1] /= Length;
        Data[2] /= Length;
#endif
        return *this;
    }

    Vector3 GetNormalize() const
    {
#ifdef WITH_SIMD
        __m128 V    = _mm_loadu_ps(Data);
        __m128 Dot  = _mm_dp_ps(V, V, 0x71);
        __m128 Len  = _mm_sqrt_ss(Dot);
        __m128 LenB = _mm_shuffle_ps(Len, Len, _MM_SHUFFLE(0, 0, 0, 0));
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_div_ps(V, LenB));
        return Result;
#else
        const Real Length = GetLength();
        return Vector3(Data[0] / Length, Data[1] / Length, Data[2] / Length);
#endif
    }

    Real Dot(const Vector3& Other) const
    {
#ifdef WITH_SIMD
        __m128 A = _mm_loadu_ps(Data);
        __m128 B = _mm_loadu_ps(Other.Data);
        return _mm_cvtss_f32(_mm_dp_ps(A, B, 0x71));
#else
        return Data[0]*Other.Data[0] + Data[1]*Other.Data[1] + Data[2]*Other.Data[2];
#endif
    }

    Vector3 operator+(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        __m128 V = _mm_loadu_ps(Data);
        // Scalar only into x,y,z so w stays 0
        __m128 S = _mm_set_ps(0.f, static_cast<float>(Scalar),
                                   static_cast<float>(Scalar),
                                   static_cast<float>(Scalar));
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_add_ps(V, S));
        return Result;
#else
        return Vector3(Data[0]+Scalar, Data[1]+Scalar, Data[2]+Scalar);
#endif
    }

    Vector3 operator+(const Vector3& Other) const
    {
#ifdef WITH_SIMD
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_add_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector3(Data[0]+Other.Data[0], Data[1]+Other.Data[1], Data[2]+Other.Data[2]);
#endif
    }

    Vector3 operator-(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        __m128 V = _mm_loadu_ps(Data);
        __m128 S = _mm_set_ps(0.f, static_cast<float>(Scalar),
                                   static_cast<float>(Scalar),
                                   static_cast<float>(Scalar));
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_sub_ps(V, S));
        return Result;
#else
        return Vector3(Data[0]-Scalar, Data[1]-Scalar, Data[2]-Scalar);
#endif
    }

    // Fixed: original had + instead of - for Y and Z components.
    Vector3 operator-(const Vector3& Other) const
    {
#ifdef WITH_SIMD
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_sub_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector3(Data[0]-Other.Data[0], Data[1]-Other.Data[1], Data[2]-Other.Data[2]);
#endif
    }

    Vector3 operator*(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_mul_ps(_mm_loadu_ps(Data),
                                               _mm_set1_ps(static_cast<float>(Scalar))));
        return Result;
#else
        return Vector3(Data[0]*Scalar, Data[1]*Scalar, Data[2]*Scalar);
#endif
    }

    Vector3 operator*(const Vector3& Other) const
    {
#ifdef WITH_SIMD
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_mul_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector3(Data[0]*Other.Data[0], Data[1]*Other.Data[1], Data[2]*Other.Data[2]);
#endif
    }

    Vector3 operator/(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_div_ps(_mm_loadu_ps(Data),
                                               _mm_set1_ps(static_cast<float>(Scalar))));
        return Result;
#else
        return Vector3(Data[0]/Scalar, Data[1]/Scalar, Data[2]/Scalar);
#endif
    }

    Vector3 operator/(const Vector3& Other) const
    {
#ifdef WITH_SIMD
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_div_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector3(Data[0]/Other.Data[0], Data[1]/Other.Data[1], Data[2]/Other.Data[2]);
#endif
    }

    // Cross product.  Shuffle pattern: cross(A,B) = A.yzx*B.zxy - A.zxy*B.yzx
    Vector3 operator^(const Vector3& Other) const
    {
#ifdef WITH_SIMD
        __m128 A = _mm_loadu_ps(Data);
        __m128 B = _mm_loadu_ps(Other.Data);
        __m128 A_yzx = _mm_shuffle_ps(A, A, _MM_SHUFFLE(3, 0, 2, 1));
        __m128 B_zxy = _mm_shuffle_ps(B, B, _MM_SHUFFLE(3, 1, 0, 2));
        __m128 A_zxy = _mm_shuffle_ps(A, A, _MM_SHUFFLE(3, 1, 0, 2));
        __m128 B_yzx = _mm_shuffle_ps(B, B, _MM_SHUFFLE(3, 0, 2, 1));
        Vector3 Result;
        _mm_storeu_ps(Result.Data, _mm_sub_ps(_mm_mul_ps(A_yzx, B_zxy),
                                               _mm_mul_ps(A_zxy, B_yzx)));
        return Result;
#else
        return Vector3(Data[1]*Other.Data[2] - Data[2]*Other.Data[1],
                       Data[2]*Other.Data[0] - Data[0]*Other.Data[2],
                       Data[0]*Other.Data[1] - Data[1]*Other.Data[0]);
#endif
    }

private:
#ifdef WITH_SIMD
    alignas(16) float Data[4]; // Data[3] = 0 (padding, never exposed)
#else
    Real Data[3];
#endif
};

// ---------------------------------------------------------------------------
// Vector4
// SIMD path: alignas(16) float[4], operations use __m128.
//   GetData() returns float* under SIMD.
// Scalar path: Real[4], unchanged behaviour.
// ---------------------------------------------------------------------------
class Vector4
{
public:
    Vector4()
    {
#ifdef WITH_SIMD
        _mm_storeu_ps(Data, _mm_setzero_ps());
#else
        Data[0] = Data[1] = Data[2] = Data[3] = 0.f;
#endif
    }

    Vector4(Real InX, Real InY, Real InZ, Real InW)
    {
#ifdef WITH_SIMD
        _mm_storeu_ps(Data, _mm_set_ps(static_cast<float>(InW),
                                        static_cast<float>(InZ),
                                        static_cast<float>(InY),
                                        static_cast<float>(InX)));
#else
        Data[0] = InX;
        Data[1] = InY;
        Data[2] = InZ;
        Data[3] = InW;
#endif
    }

#ifdef WITH_SIMD
    float GetX() const { return Data[0]; }
    float GetY() const { return Data[1]; }
    float GetZ() const { return Data[2]; }
    float GetW() const { return Data[3]; }
    void SetX(Real InX) { Data[0] = static_cast<float>(InX); }
    void SetY(Real InY) { Data[1] = static_cast<float>(InY); }
    void SetZ(Real InZ) { Data[2] = static_cast<float>(InZ); }
    void SetW(Real InW) { Data[3] = static_cast<float>(InW); }
    const float* GetData() const { return Data; }
    float*       GetData()       { return Data; }
#else
    Real GetX() const { return Data[0]; }
    Real GetY() const { return Data[1]; }
    Real GetZ() const { return Data[2]; }
    Real GetW() const { return Data[3]; }
    void SetX(Real InX) { Data[0] = InX; }
    void SetY(Real InY) { Data[1] = InY; }
    void SetZ(Real InZ) { Data[2] = InZ; }
    void SetW(Real InW) { Data[3] = InW; }
    const Real* GetData() const { return Data; }
    Real*       GetData()       { return Data; }
#endif

    Vector4 operator+(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_add_ps(_mm_loadu_ps(Data),
                                               _mm_set1_ps(static_cast<float>(Scalar))));
        return Result;
#else
        return Vector4(Data[0]+Scalar, Data[1]+Scalar, Data[2]+Scalar, Data[3]+Scalar);
#endif
    }

    Vector4 operator+(const Vector4& Other) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_add_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector4(Data[0]+Other.Data[0], Data[1]+Other.Data[1],
                       Data[2]+Other.Data[2], Data[3]+Other.Data[3]);
#endif
    }

    Vector4 operator-(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_sub_ps(_mm_loadu_ps(Data),
                                               _mm_set1_ps(static_cast<float>(Scalar))));
        return Result;
#else
        return Vector4(Data[0]-Scalar, Data[1]-Scalar, Data[2]-Scalar, Data[3]-Scalar);
#endif
    }

    Vector4 operator-(const Vector4& Other) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_sub_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector4(Data[0]-Other.Data[0], Data[1]-Other.Data[1],
                       Data[2]-Other.Data[2], Data[3]-Other.Data[3]);
#endif
    }

    Vector4 operator*(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_mul_ps(_mm_loadu_ps(Data),
                                               _mm_set1_ps(static_cast<float>(Scalar))));
        return Result;
#else
        return Vector4(Data[0]*Scalar, Data[1]*Scalar, Data[2]*Scalar, Data[3]*Scalar);
#endif
    }

    Vector4 operator*(const Vector4& Other) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_mul_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector4(Data[0]*Other.Data[0], Data[1]*Other.Data[1],
                       Data[2]*Other.Data[2], Data[3]*Other.Data[3]);
#endif
    }

    Vector4 operator/(const Real& Scalar) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_div_ps(_mm_loadu_ps(Data),
                                               _mm_set1_ps(static_cast<float>(Scalar))));
        return Result;
#else
        return Vector4(Data[0]/Scalar, Data[1]/Scalar, Data[2]/Scalar, Data[3]/Scalar);
#endif
    }

    Vector4 operator/(const Vector4& Other) const
    {
#ifdef WITH_SIMD
        Vector4 Result;
        _mm_storeu_ps(Result.Data, _mm_div_ps(_mm_loadu_ps(Data), _mm_loadu_ps(Other.Data)));
        return Result;
#else
        return Vector4(Data[0]/Other.Data[0], Data[1]/Other.Data[1],
                       Data[2]/Other.Data[2], Data[3]/Other.Data[3]);
#endif
    }

private:
#ifdef WITH_SIMD
    alignas(16) float Data[4];
#else
    Real Data[4];
#endif
};


}

/*
 * Re-exported to global scope so the ~185 existing unqualified uses of
 * Vector2/3/4 and Matrix4x4 across GDI and the app keep compiling untouched.
 * A consumer that wants a clean global namespace (a game engine embedding
 * EngineCore) configures with -DCORE_COMPAT_GLOBAL_ALIASES=OFF and qualifies.
 *
 * The option is PUBLIC on the target for the same reason WITH_SIMD is: it
 * changes which names exist at global scope, so it must be identical in every
 * translation unit.
 */
#ifndef CORE_NO_GLOBAL_ALIASES
using EngineCore::Vector2;
using EngineCore::Vector3;
using EngineCore::Vector4;
#endif

// ---------------------------------------------------------------------------
// ABI guard. WITH_SIMD changes the SIZE and ALIGNMENT of every type below, and
// it is now owned PUBLIC by the EngineCore target so that EngineCore, GDI and
// CoreClient cannot disagree. If a stale option() is ever left behind in a
// downstream CMakeLists, these fire at compile time -- which is the only
// diagnostic there is. A mismatch otherwise corrupts memory silently.
//
// Note Vector2 is 16 bytes on BOTH paths; only the alignment tells them apart,
// so the alignof assertions are load-bearing, not decoration.
// ---------------------------------------------------------------------------
#ifdef WITH_SIMD
static_assert(sizeof(EngineCore::Vector2)  == 16 && alignof(EngineCore::Vector2)  == 16, "Vector2 SIMD layout changed (WITH_SIMD mismatch?)");
static_assert(sizeof(EngineCore::Vector3)  == 16 && alignof(EngineCore::Vector3)  == 16, "Vector3 SIMD layout changed (WITH_SIMD mismatch?)");
static_assert(sizeof(EngineCore::Vector4)  == 16 && alignof(EngineCore::Vector4)  == 16, "Vector4 SIMD layout changed (WITH_SIMD mismatch?)");
#else
static_assert(sizeof(EngineCore::Vector2)  == 16 && alignof(EngineCore::Vector2)  == alignof(double), "Vector2 scalar layout changed (WITH_SIMD mismatch?)");
static_assert(sizeof(EngineCore::Vector3)  == 24, "Vector3 scalar layout changed (WITH_SIMD mismatch?)");
static_assert(sizeof(EngineCore::Vector4)  == 32, "Vector4 scalar layout changed (WITH_SIMD mismatch?)");
#endif

#endif // VECTOR_H

#endif //CORECLIENT_VECTOR_H

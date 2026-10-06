#include "Private/RenderCore/ShadowSetup.h"

#include <algorithm>
#include <cmath>

#include "Public/RenderCore/Components/LightShadow.h"

/* The shadow views of a frame (ShadowSetup.h). */

namespace RenderCore
{
    namespace
    {
        using EngineCore::Matrix4x4;
        using EngineCore::Vector3;
        using EngineCore::Vector4;

        constexpr double SHADOW_PI = 3.14159265358979323846;

        /* An up that is never parallel to InForward. */
        Vector3 UpFor(const Vector3& InForward)
        {
            return std::abs(InForward.GetY()) > 0.99 ? Vector3(0.0, 0.0, 1.0) : Vector3(0.0, 1.0, 0.0);
        }

        Matrix4x4 LookAlong(const Vector3& InEye, const Vector3& InForward)
        {
            return Matrix4x4().LookAt(InEye, InForward, UpFor(InForward));
        }

        /* View x in [-InHalfX, InHalfX], y likewise, z in [InNear, InFar] to GL clip space. */
        Matrix4x4 Orthographic(const double InHalfX, const double InHalfY, const double InNear, const double InFar)
        {
            Matrix4x4 P;
            const double Range = InFar - InNear;
            P.Data[0][0] = float(1.0 / InHalfX);
            P.Data[1][1] = float(1.0 / InHalfY);
            P.Data[2][2] = float(2.0 / Range);
            P.Data[2][3] = float(-(InFar + InNear) / Range);
            P.Data[3][3] = 1.0f;
            return P;
        }

        /* A square frustum InFovDegrees wide, z in [InNear, InFar] to GL clip space. */
        Matrix4x4 Perspective(const double InFovDegrees, const double InNear, const double InFar)
        {
            Matrix4x4 P;
            const double Focal = 1.0 / std::tan(InFovDegrees * 0.5 * SHADOW_PI / 180.0);
            const double Range = InFar - InNear;
            P.Data[0][0] = float(Focal);
            P.Data[1][1] = float(Focal);
            P.Data[2][2] = float((InFar + InNear) / Range);
            P.Data[2][3] = float(-2.0 * InFar * InNear / Range);
            P.Data[3][2] = 1.0f;
            P.Data[3][3] = 0.0f;
            return P;
        }

        /* GL clip space to (u, v, depth) * w inside a tile of an InAtlasSize
         * atlas. The atlas is stored top-down (GDI.h) and the tile drawn at
         * rows InTileY.. from its top, so NDC y = +1 is v = V0. */
        Matrix4x4 TileMatrix(const uint32_t InTileX, const uint32_t InTileY, const uint32_t InTileSize, const uint32_t InAtlasSize)
        {
            const double Atlas = double(InAtlasSize);
            const double U0 = InTileX / Atlas, V0 = InTileY / Atlas, Extent = InTileSize / Atlas;
            Matrix4x4 T;
            T.Data[0][0] = float(0.5 * Extent); T.Data[0][3] = float(U0 + 0.5 * Extent);
            T.Data[1][1] = float(-0.5 * Extent); T.Data[1][3] = float(V0 + 0.5 * Extent);
            T.Data[2][2] = 0.5f;                T.Data[2][3] = 0.5f;
            return T;
        }

        /* The world box a view and projection see: their clip cube's corners, unprojected. */
        GameCore::AABB FrustumBounds(const Matrix4x4& InView, const Matrix4x4& InProjection)
        {
            const Matrix4x4 Inverse = Matrix4x4::Inverse(InProjection * InView);
            GameCore::AABB Bounds;
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const Vector4 Clip((Corner & 1) ? 1.0 : -1.0, (Corner & 2) ? 1.0 : -1.0, (Corner & 4) ? 1.0 : -1.0, 1.0);
                const Vector4 World = Inverse * Clip;
                const double  W     = std::abs(double(World.GetW())) > 1e-12 ? double(World.GetW()) : 1e-12;
                const Vector3 Point(World.GetX() / W, World.GetY() / W, World.GetZ() / W);
                Bounds = GameCore::AABB::Union(Bounds, GameCore::AABB(Point, Point));
            }
            return Bounds;
        }

        /* Fills InOutView's matrices and bounds from a view, projection and tile. */
        void FinishView(ShadowView& InOutView, const Matrix4x4& InView, const Matrix4x4& InProjection, const uint32_t InAtlasSize)
        {
            InOutView.View         = InView;
            InOutView.Projection   = InProjection;
            InOutView.WorldToAtlas = TileMatrix(InOutView.TileX, InOutView.TileY, InOutView.TileSize, InAtlasSize) * InProjection * InView;
            InOutView.WorldBounds  = FrustumBounds(InView, InProjection);
        }

        /*
         * The camera's frustum corner at view depth InDepth, toward NDC
         * (InX, InY). Reads the projection rather than unprojecting a depth,
         * so it does not care which clip-depth convention built it:
         * RenderView's perspective has w = view z, its orthographic w = 1.
         */
        Vector3 CameraPoint(const RenderView& InCamera, const double InX, const double InY, const double InDepth)
        {
            const Matrix4x4& P = InCamera.Projection;
            const Matrix4x4& V = InCamera.View;
            const bool bPerspective = P.Data[3][2] != 0.0f;

            double ViewX, ViewY;
            if(bPerspective)
            {
                ViewX = (InX * InDepth - P.Data[0][2] * InDepth) / P.Data[0][0];
                ViewY = (InY * InDepth - P.Data[1][2] * InDepth) / P.Data[1][1];
            }
            else
            {
                ViewX = (InX - P.Data[0][3]) / P.Data[0][0];
                ViewY = (InY - P.Data[1][3]) / P.Data[1][1];
            }

            /* LookAt's rows are right, up and forward; the eye is Origin. */
            const Vector3 Right(V.Data[0][0], V.Data[0][1], V.Data[0][2]);
            const Vector3 Up(V.Data[1][0], V.Data[1][1], V.Data[1][2]);
            const Vector3 Forward(V.Data[2][0], V.Data[2][1], V.Data[2][2]);
            return InCamera.Origin + Right * ViewX + Up * ViewY + Forward * InDepth;
        }

        /* An orthographic view along InDirection holding the camera's slice
         * from InNear to InFar, snapped to whole texels of InTileSize. */
        void FitDirectional(ShadowView& InOutView, const RenderView& InCamera, const Vector3& InDirection,
                            const double InNear, const double InFar, const double InCasterReach, const uint32_t InAtlasSize)
        {
            Vector3 Corners[8];
            Vector3 Centre(0.0, 0.0, 0.0);
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                Corners[Corner] = CameraPoint(InCamera, (Corner & 1) ? 1.0 : -1.0, (Corner & 2) ? 1.0 : -1.0, (Corner & 4) ? InFar : InNear);
                Centre = Centre + Corners[Corner];
            }
            Centre = Centre * (1.0 / 8.0);

            /* A sphere, not the box: its size does not change as the camera turns. */
            double Radius = 0.0;
            for(const Vector3& Corner : Corners)
            {
                Radius = std::max(Radius, double((Corner - Centre).GetLength()));
            }
            Radius = std::max(1.0, std::ceil(Radius * 16.0) / 16.0);

            /* Move the centre in whole texels, in the light's own plane. */
            const Matrix4x4 Orient = LookAlong(Vector3(0.0, 0.0, 0.0), InDirection);
            const double TexelWorld = 2.0 * Radius / double(InOutView.TileSize);
            Vector3 LightSpace = Orient * Centre;
            LightSpace = Vector3(std::floor(LightSpace.GetX() / TexelWorld) * TexelWorld,
                                 std::floor(LightSpace.GetY() / TexelWorld) * TexelWorld,
                                 LightSpace.GetZ());
            const Vector3 Snapped = Orient.Transpose() * LightSpace;

            /* Casters between the sun and the slice reach InCasterReach further back. */
            const Matrix4x4 View       = LookAlong(Snapped, InDirection);
            const Matrix4x4 Projection = Orthographic(Radius, Radius, -Radius - InCasterReach, Radius);
            FinishView(InOutView, View, Projection, InAtlasSize);
        }

        /* An orthographic view along InDirection holding InCaster's sphere and
         * no more: its depth runs across the sphere alone, so a receiver past
         * the far plane is behind the caster (Shadows.glsl holds it there).
         * Snapped to whole texels in the light's plane, so a moving caster's
         * shadow does not crawl. */
        void FitInset(ShadowView& InOutView, const Vector3& InDirection, const InsetShadowCaster& InCaster, const uint32_t InAtlasSize)
        {
            const double Radius = std::max(1.0, std::ceil(double(InCaster.Radius) * 16.0) / 16.0);

            const Matrix4x4 Orient = LookAlong(Vector3(0.0, 0.0, 0.0), InDirection);
            const double TexelWorld = 2.0 * Radius / double(InOutView.TileSize);
            Vector3 LightSpace = Orient * InCaster.Centre;
            LightSpace = Vector3(std::floor(LightSpace.GetX() / TexelWorld) * TexelWorld,
                                 std::floor(LightSpace.GetY() / TexelWorld) * TexelWorld,
                                 LightSpace.GetZ());
            const Vector3 Snapped = Orient.Transpose() * LightSpace;

            /* Snapping moves the centre up to a texel: a texel more each way. */
            const double Half = Radius + TexelWorld;
            FinishView(InOutView, LookAlong(Snapped, InDirection), Orthographic(Half, Half, -Radius, Radius), InAtlasSize);
        }

        /* The six cube faces' directions, +X -X +Y -Y +Z -Z (Shadows.glsl picks them in this order). */
        const Vector3 POINT_FACES[SHADOW_POINT_FACES] = {
            Vector3( 1.0, 0.0, 0.0), Vector3(-1.0, 0.0, 0.0),
            Vector3( 0.0, 1.0, 0.0), Vector3( 0.0, -1.0, 0.0),
            Vector3( 0.0, 0.0, 1.0), Vector3( 0.0, 0.0, -1.0),
        };
    }

    void InsetScreenNdc(const InsetShadowCaster& InCaster, const Vector3& InDirection, const double InPad,
                        const RenderView& InCamera, float (&OutNdc)[4], const GameCore::AABB* InReceivers)
    {
        /*
         * The prism is the sphere's box swept along InDirection to past the far
         * plane: a convex hull whose faces are among the box's faces at each
         * end and its edges swept. Each of those quads is clipped to the
         * view's clip volume (-w <= x, y, z <= w) in homogeneous space, so
         * what lies behind the eye or off screen drops out, and what is left
         * is projected. Interior quads only add inner points: still a bound.
         *
         * InReceivers shortens the sweep: a receiver point in the prism is
         * within Half of the sweep at some t, so lies in the box swept from the
         * first t whose box overlaps InReceivers to the last. None: no pixel.
         */
        const double Half  = double(InCaster.Radius) + std::max(InPad, 0.0);
        const double Reach = double(std::abs(InCamera.ZFar)) + (InCaster.Centre - InCamera.Origin).GetLength();
        double TStart = 0.0, TEnd = Reach;
        if(InReceivers && !InReceivers->IsEmpty())
        {
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                const auto Of = [Axis](const Vector3& InV) { return double(Axis == 0 ? InV.GetX() : Axis == 1 ? InV.GetY() : InV.GetZ()); };
                const double C = Of(InCaster.Centre), D = Of(InDirection);
                const double Lo = Of(InReceivers->Min) - Half - C, Hi = Of(InReceivers->Max) + Half - C;   // the box overlaps on this axis while D t is in [Lo, Hi]
                if(std::abs(D) < 1e-12)
                {
                    if(Lo > 0.0 || Hi < 0.0) { TEnd = -1.0; }
                    continue;
                }
                TStart = std::max(TStart, std::min(Lo / D, Hi / D));
                TEnd   = std::min(TEnd, std::max(Lo / D, Hi / D));
            }
            if(TStart > TEnd)
            {
                OutNdc[0] = 1.0f; OutNdc[1] = 1.0f; OutNdc[2] = -1.0f; OutNdc[3] = -1.0f;
                return;
            }
        }
        const Vector3 Start = InDirection * TStart;
        const Vector3 Sweep = InDirection * TEnd;
        const Matrix4x4 ToClip = InCamera.Projection * InCamera.View;

        struct Clip4 { double V[4]; };
        Clip4 Corners[16];
        for(int Corner = 0; Corner < 16; ++Corner)
        {
            const Vector3 Point = InCaster.Centre + Vector3((Corner & 1) ? Half : -Half, (Corner & 2) ? Half : -Half, (Corner & 4) ? Half : -Half)
                                + ((Corner & 8) ? Sweep : Start);
            const Vector4 At = ToClip * Vector4(Point.GetX(), Point.GetY(), Point.GetZ(), 1.0);
            Corners[Corner] = { { double(At.GetX()), double(At.GetY()), double(At.GetZ()), double(At.GetW()) } };
        }

        /* Sutherland-Hodgman: InOutPoly (InOutCount points) cut to InInside(point) >= 0, linear in the point. */
        const auto ClipBy = [](Clip4 (&InOutPoly)[24], int& InOutCount, const auto& InInside)
        {
            Clip4 Kept[24];
            int KeptCount = 0;
            for(int Index = 0; Index < InOutCount; ++Index)
            {
                const Clip4& From = InOutPoly[Index];
                const Clip4& To   = InOutPoly[(Index + 1) % InOutCount];
                const double DFrom = InInside(From), DTo = InInside(To);
                if(DFrom >= 0.0 && KeptCount < 24) { Kept[KeptCount++] = From; }
                if((DFrom >= 0.0) != (DTo >= 0.0) && KeptCount < 24)
                {
                    const double T = DFrom / (DFrom - DTo);
                    Clip4 Cut;
                    for(int C = 0; C < 4; ++C) { Cut.V[C] = From.V[C] + (To.V[C] - From.V[C]) * T; }
                    Kept[KeptCount++] = Cut;
                }
            }
            InOutCount = KeptCount;
            for(int Index = 0; Index < InOutCount; ++Index) { InOutPoly[Index] = Kept[Index]; }
        };

        double Lo[2] = { 1e30, 1e30 }, Hi[2] = { -1e30, -1e30 };
        bool bAny = false;
        const auto Plane = [](const Clip4& InPoint, const int InPlane)
        {
            const double Axis = InPoint.V[InPlane / 2];
            return InPoint.V[3] + ((InPlane & 1) ? -Axis : Axis);   // w + x, w - x, w + y, ...
        };
        const auto ClipQuad = [&](const int InA, const int InB, const int InC, const int InD)
        {
            Clip4 Poly[24] = { Corners[InA], Corners[InB], Corners[InC], Corners[InD] };
            int Count = 4;
            for(int PlaneIndex = 0; PlaneIndex < 6 && Count > 0; ++PlaneIndex)
            {
                ClipBy(Poly, Count, [&Plane, PlaneIndex](const Clip4& InPoint) { return Plane(InPoint, PlaneIndex); });
            }
            for(int Index = 0; Index < Count; ++Index)
            {
                const double W = Poly[Index].V[3];
                if(W <= 1e-12) { continue; }
                for(int Axis = 0; Axis < 2; ++Axis)
                {
                    Lo[Axis] = std::min(Lo[Axis], Poly[Index].V[Axis] / W);
                    Hi[Axis] = std::max(Hi[Axis], Poly[Index].V[Axis] / W);
                }
                bAny = true;
            }
        };

        for(int End = 0; End < 2; ++End)                 // the box's faces, at the caster and swept
        {
            const int Base = End * 8;
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                const int U = 1 << ((Axis + 1) % 3), V = 1 << ((Axis + 2) % 3);
                for(int Side = 0; Side < 2; ++Side)
                {
                    const int A = Base + (Side << Axis);
                    ClipQuad(A, A + U, A + U + V, A + V);
                }
            }
        }
        for(int Corner = 0; Corner < 8; ++Corner)          // its 12 edges, swept
        {
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                if(Corner & (1 << Axis)) { continue; }
                const int Other = Corner | (1 << Axis);
                ClipQuad(Corner, Other, Other + 8, Corner + 8);
            }
        }

        if(bAny)
        {
            OutNdc[0] = float(std::clamp(Lo[0], -1.0, 1.0)); OutNdc[1] = float(std::clamp(Lo[1], -1.0, 1.0));
            OutNdc[2] = float(std::clamp(Hi[0], -1.0, 1.0)); OutNdc[3] = float(std::clamp(Hi[1], -1.0, 1.0));
            return;
        }

        /* No face crosses the view: the prism misses it, or holds all of it. */
        const Matrix4x4 FromClip = Matrix4x4::Inverse(ToClip);
        const Vector4 Middle = FromClip * Vector4(0.0, 0.0, 0.0, 1.0);
        const double  MiddleW = std::abs(double(Middle.GetW())) > 1e-12 ? double(Middle.GetW()) : 1e-12;
        const Vector3 Inside = Vector3(Middle.GetX() / MiddleW, Middle.GetY() / MiddleW, Middle.GetZ() / MiddleW) - InCaster.Centre;
        double TMin = TStart, TMax = TEnd;   // some point of the sweep within Half of it on every axis
        for(int Axis = 0; Axis < 3; ++Axis)
        {
            const double P = Axis == 0 ? Inside.GetX() : Axis == 1 ? Inside.GetY() : Inside.GetZ();
            const double D = Axis == 0 ? InDirection.GetX() : Axis == 1 ? InDirection.GetY() : InDirection.GetZ();
            if(std::abs(D) < 1e-12)
            {
                if(std::abs(P) > Half) { TMax = -1.0; }
                continue;
            }
            const double T0 = (P - Half) / D, T1 = (P + Half) / D;
            TMin = std::max(TMin, std::min(T0, T1));
            TMax = std::min(TMax, std::max(T0, T1));
        }
        const bool bHoldsView = TMin <= TMax;
        OutNdc[0] = bHoldsView ? -1.0f : 1.0f; OutNdc[1] = bHoldsView ? -1.0f : 1.0f;
        OutNdc[2] = bHoldsView ? 1.0f : -1.0f; OutNdc[3] = bHoldsView ? 1.0f : -1.0f;
    }

    void ComputeCascadeSplits(const float InNear, const float InFar, const uint32_t InCount, const float InLambda, float* OutSplits)
    {
        const double Near   = std::max(double(InNear), 1.0);
        const double Far    = std::max(double(InFar), Near + 1.0);
        const double Lambda = std::clamp(double(InLambda), 0.0, 1.0);
        for(uint32_t Index = 0; Index < InCount; ++Index)
        {
            const double Fraction    = double(Index + 1) / double(InCount);
            const double Logarithmic = Near * std::pow(Far / Near, Fraction);
            const double Uniform     = Near + (Far - Near) * Fraction;
            OutSplits[Index] = float(Lambda * Logarithmic + (1.0 - Lambda) * Uniform);
        }
        if(InCount > 0)
        {
            OutSplits[InCount - 1] = float(Far);
        }
    }

    void BuildShadowFrame(const RenderLighting& InLighting, const RenderView& InCamera, ShadowFrame& OutFrame,
                          const InsetShadowCaster* InCasters, const uint32_t InCasterCount, const GameCore::AABB* InReceivers)
    {
        OutFrame.ViewCount            = 0;
        OutFrame.InsetFirstView       = -1;
        OutFrame.InsetCount           = 0;
        OutFrame.AtlasSize            = 0;
        OutFrame.DirectionalFirstView = -1;
        OutFrame.CascadeCount         = 0;
        for(int32_t& Each : OutFrame.LocalFirstView) { Each = -1; }

        const RenderShadowSettings& Settings = InLighting.Shadows;
        if(!Settings.bEnabled)
        {
            return;
        }
        const uint32_t Atlas = uint32_t(std::clamp(Settings.Resolution, 256, 8192));
        OutFrame.AtlasSize = Atlas;

        // --- The sun -------------------------------------------------------------
        const double DirectionLength = InLighting.LightDirection.GetLength();
        if(InLighting.bLightCastShadows && InLighting.LightIntensity > 0.0f && DirectionLength > 1e-9)
        {
            const Vector3 Direction = InLighting.LightDirection * (1.0 / DirectionLength);
            const bool    bPerspective = InCamera.Projection.Data[3][2] != 0.0f;

            /* The camera's depth range, clipped to the shadow distance. An
             * orthographic camera's starts at its own ZNear (it may be negative). */
            const double Distance = std::max(1.0, double(InLighting.LightShadowDistance) * double(std::max(Settings.DistanceScale, 0.0f)));
            const double Near     = bPerspective ? std::max(1.0, double(InCamera.ZNear)) : double(InCamera.ZNear);
            const double Far      = std::max(Near + 1.0, std::min(double(InCamera.ZFar), Near + Distance));
            const double Reach    = std::max(Distance, 2000.0);

            const bool     bCascaded = InLighting.LightShadowTechnique == LST_Cascaded;
            const uint32_t Count     = bCascaded ? uint32_t(std::clamp(InLighting.LightCascadeCount, 1, int32_t(RENDER_CASCADE_MAX))) : 1u;

            float Splits[RENDER_CASCADE_MAX] = {};
            if(bPerspective)
            {
                ComputeCascadeSplits(float(Near), float(Far), Count, InLighting.LightCascadeSplitLambda, Splits);
            }
            else
            {
                /* No perspective, nothing to gain from logarithmic slices. */
                for(uint32_t Index = 0; Index < Count; ++Index)
                {
                    Splits[Index] = float(Near + (Far - Near) * double(Index + 1) / double(Count));
                }
            }

            OutFrame.DirectionalFirstView = int32_t(OutFrame.ViewCount);
            OutFrame.CascadeCount         = Count;
            const uint32_t Tile = Count == 1 ? Atlas : Atlas / 2;
            double SliceNear = Near;
            for(uint32_t Index = 0; Index < Count; ++Index)
            {
                ShadowView& View = OutFrame.Views[OutFrame.ViewCount++];
                View.Atlas    = SA_Directional;
                View.Inset    = -1;
                View.TileSize = Tile;
                View.TileX    = (Index % 2) * Tile;
                View.TileY    = (Index / 2) * Tile;
                FitDirectional(View, InCamera, Direction, SliceNear, Splits[Index], Reach, Atlas);
                OutFrame.CascadeSplits[Index] = Splits[Index];
                SliceNear = Splits[Index];
            }
        }

        // --- Point and spot lights, in the local atlas's tiles -------------------
        const uint32_t TileSize  = Atlas / SHADOW_LOCAL_ATLAS_COLUMNS;
        const uint32_t TileCount = SHADOW_LOCAL_ATLAS_COLUMNS * SHADOW_LOCAL_ATLAS_ROWS;
        const uint32_t MaxLights = uint32_t(std::clamp(Settings.MaxShadowedLights, 0, int32_t(RENDER_SHADOWED_LOCAL_LIGHT_MAX)));
        uint32_t NextTile = 0;
        uint32_t Shadowed = 0;
        const uint32_t LocalCount = std::min<uint32_t>(InLighting.LocalCount, RENDER_LOCAL_LIGHT_MAX);
        for(uint32_t LightIndex = 0; LightIndex < LocalCount && Shadowed < MaxLights; ++LightIndex)
        {
            const RenderLocalLight& Light = InLighting.Locals[LightIndex];
            if(!Light.bCastShadows || Light.Range <= 1.0f || Light.Intensity <= 0.0f)
            {
                continue;
            }
            const bool     bSpot = Light.Type == RLL_Spot;
            const uint32_t Faces = bSpot ? 1u : uint32_t(SHADOW_POINT_FACES);
            if(NextTile + Faces > TileCount)
            {
                break;
            }

            const double Far  = double(Light.Range);
            const double Near = std::max(1.0, Far * 0.005);
            OutFrame.LocalFirstView[LightIndex] = int32_t(OutFrame.ViewCount);
            for(uint32_t Face = 0; Face < Faces; ++Face)
            {
                ShadowView& View = OutFrame.Views[OutFrame.ViewCount++];
                View.Atlas    = SA_Local;
                View.Inset    = -1;
                View.TileSize = TileSize;
                View.TileX    = (NextTile % SHADOW_LOCAL_ATLAS_COLUMNS) * TileSize;
                View.TileY    = (NextTile / SHADOW_LOCAL_ATLAS_COLUMNS) * TileSize;
                ++NextTile;

                const Vector3 Forward = bSpot ? Light.Direction.GetNormalize() : POINT_FACES[Face];
                const double  Fov     = bSpot ? std::clamp(2.0 * double(Light.OuterConeAngle) + 2.0, 2.0, 170.0) : 90.0;
                FinishView(View, LookAlong(Light.Position, Forward), Perspective(Fov, Near, Far), Atlas);
            }
            ++Shadowed;
        }

        // --- Inset shadows, in the local atlas's tiles the lights left -------
        const double InsetLength = InLighting.InsetLightDirection.GetLength();
        if(!InLighting.bInsetLight || !InCasters || InsetLength <= 1e-9)
        {
            return;
        }
        const Vector3  InsetDirection = InLighting.InsetLightDirection * (1.0 / InsetLength);
        const uint32_t MaxInsets      = uint32_t(std::clamp(Settings.MaxInsetShadows, 0, int32_t(RENDER_INSET_SHADOW_MAX)));
        for(uint32_t Caster = 0; Caster < InCasterCount && OutFrame.InsetCount < MaxInsets && NextTile < TileCount; ++Caster)
        {
            if(!(InCasters[Caster].Radius > 0.0f))
            {
                continue;
            }
            if(OutFrame.InsetCount == 0)
            {
                OutFrame.InsetFirstView = int32_t(OutFrame.ViewCount);
            }
            ShadowView& View = OutFrame.Views[OutFrame.ViewCount++];
            View.Atlas    = SA_Local;
            View.TileSize = TileSize;
            View.TileX    = (NextTile % SHADOW_LOCAL_ATLAS_COLUMNS) * TileSize;
            View.TileY    = (NextTile / SHADOW_LOCAL_ATLAS_COLUMNS) * TileSize;
            View.Inset    = int32_t(Caster);
            ++NextTile;
            FitInset(View, InsetDirection, InCasters[Caster], Atlas);

            /* Where on screen it can shadow: a receiver's sample point moves
             * by its normal offset (Shadows.glsl, at most twice the larger of
             * the normal bias and a texel and a half), and the filter reaches
             * a couple of texels more. */
            const double Texel = 2.0 / std::abs(double(View.Projection.Data[0][0])) / double(View.TileSize);
            const double Pad   = Texel + 2.0 * std::max(double(InLighting.InsetShadowNormalBias), 1.5 * Texel) + 2.0 * Texel;
            InsetScreenNdc(InCasters[Caster], InsetDirection, Pad, InCamera, View.ScreenNdc, InReceivers);
            ++OutFrame.InsetCount;
        }
    }
}

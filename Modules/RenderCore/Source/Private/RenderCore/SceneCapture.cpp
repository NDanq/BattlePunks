#include "Private/RenderCore/SceneCapture.h"

#include <algorithm>
#include <cmath>

#include "Public/Math/Matrix.h"
#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"

namespace RenderCore
{
    namespace
    {
        using EngineCore::Matrix4x4;
        using EngineCore::Vector3;

        /* The world box a view can see: its frustum's eight corners. */
        GameCore::AABB FrustumBounds(const RenderView& InView)
        {
            const Matrix4x4 Inverse = Matrix4x4::Inverse(InView.Projection * InView.View);
            GameCore::AABB Box;
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const float X = (Corner & 1) ? 1.0f : -1.0f;
                const float Y = (Corner & 2) ? 1.0f : -1.0f;
                const float Z = (Corner & 4) ? 1.0f : 0.0f;   // RenderView's depth: near 0, far 1
                const EngineCore::Vector4 Point = Inverse * EngineCore::Vector4(X, Y, Z, 1.0f);
                if(std::abs(Point.GetW()) < 1e-9f) { return GameCore::AABB(); }
                const Vector3 World(Point.GetX() / Point.GetW(), Point.GetY() / Point.GetW(), Point.GetZ() / Point.GetW());
                Box = GameCore::AABB::Union(Box, GameCore::AABB(World, World));
            }
            return Box;
        }

        /*
         * An oblique near plane (Lengyel): the projection's depth row replaced so the near plane is InPlane,
         * a world plane (a, b, c, d) with a*x + b*y + c*z + d >= 0 on the kept side. Everything behind the
         * mirror is clipped by the rasteriser, no shader change. Every backend clips as GL does (z in [-w, w];
         * GDI remaps to its own range), so the plane goes to z = -w and the far corner opposite it stays at w.
         * Shaders reconstruct positions through Scene_InvViewProjection, which follows the new matrix.
         */
        void ClipNearToPlane(RenderView& InOutView, const float (&InPlane)[4])
        {
            const Matrix4x4 InverseView = Matrix4x4::Inverse(InOutView.View);
            const Matrix4x4 InverseProjection = Matrix4x4::Inverse(InOutView.Projection);
            float Plane[4], ClipPlane[4], Corner[4];
            for(int Column = 0; Column < 4; ++Column)   // the plane in view space: inverse-transpose of the view
            {
                Plane[Column] = 0.0f;
                for(int Row = 0; Row < 4; ++Row) { Plane[Column] += float(InverseView.Data[Row][Column]) * InPlane[Row]; }
            }
            for(int Column = 0; Column < 4; ++Column)   // and in clip space, to find the far corner opposite it
            {
                ClipPlane[Column] = 0.0f;
                for(int Row = 0; Row < 4; ++Row) { ClipPlane[Column] += float(InverseProjection.Data[Row][Column]) * Plane[Row]; }
            }
            const float FarCorner[4] = { ClipPlane[0] < 0.0f ? -1.0f : 1.0f, ClipPlane[1] < 0.0f ? -1.0f : 1.0f, 1.0f, 1.0f };
            for(int Row = 0; Row < 4; ++Row)
            {
                Corner[Row] = 0.0f;
                for(int Column = 0; Column < 4; ++Column) { Corner[Row] += float(InverseProjection.Data[Row][Column]) * FarCorner[Column]; }
            }
            float CornerW = 0.0f, CornerPlane = 0.0f;
            for(int Each = 0; Each < 4; ++Each)
            {
                CornerW     += float(InOutView.Projection.Data[3][Each]) * Corner[Each];
                CornerPlane += Plane[Each] * Corner[Each];
            }
            if(std::abs(CornerPlane) < 1e-9f) { return; }
            const float Scale = 2.0f * CornerW / CornerPlane;
            for(int Each = 0; Each < 4; ++Each)
            {
                InOutView.Projection.Data[2][Each] = Plane[Each] * Scale - float(InOutView.Projection.Data[3][Each]);
            }
        }

        Vector3 Reflect(const Vector3& InDirection, const Vector3& InNormal)
        {
            return InDirection - InNormal * (2.0 * InDirection.Dot(InNormal));
        }
    }

    void CubeFaceBasis(const uint32_t InFace, Vector3& OutForward, Vector3& OutUp)
    {
        static const Vector3 Forwards[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0),
                                             Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
        OutForward = Forwards[InFace % 6];
        OutUp      = (InFace % 6 == 2 || InFace % 6 == 3) ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
    }

    const char* GetCubeFaceName(const uint32_t InFace)
    {
        static const char* const Names[6] = { "Face +X", "Face -X", "Face +Y", "Face -Y", "Face +Z", "Face -Z" };
        return Names[InFace % 6];
    }

    uint32_t BuildCaptureViews(const SceneCaptureRequest& InRequest, const RenderView& InMain, const uint32_t InWidth,
                               const uint32_t InHeight, RenderView (&OutViews)[6])
    {
        const EngineCore::Size Pixels{ int(InWidth), int(InHeight) };
        if(InRequest.Mode == SCM_Omnidirectional)
        {
            const float Near = std::max(InRequest.NearClip, 0.01f);
            const float Far  = std::max(InRequest.FarClip, Near + 1.0f);
            const Vector3 Reach(Far, Far, Far);
            for(uint32_t Face = 0; Face < 6; ++Face)
            {
                Vector3 Forward, Up;
                CubeFaceBasis(Face, Forward, Up);
                /* MakePerspective for the projection (its forward only must not be Y), then the face's own LookAt. */
                RenderView View = RenderView::MakePerspective(Pixels, InRequest.Position, Vector3(0, 0, 1), 90.0f, Near, Far);
                View.View   = Matrix4x4().LookAt(InRequest.Position, Forward, Up);
                View.Bounds = GameCore::AABB(InRequest.Position - Reach, InRequest.Position + Reach);
                OutViews[Face] = View;
            }
            return 6;
        }

        /* A planar mirror: the camera reflected in the plane. */
        const Vector3 Normal = InRequest.Normal.GetNormalize();
        if((InMain.Origin - InRequest.Position).Dot(Normal) <= 0.0)
        {
            return 0;   // seen from behind: nothing to reflect
        }
        const Vector3 Up(InMain.View.Data[1][0], InMain.View.Data[1][1], InMain.View.Data[1][2]);
        const Vector3 Forward(InMain.View.Data[2][0], InMain.View.Data[2][1], InMain.View.Data[2][2]);
        const Vector3 Eye = InMain.Origin - Normal * (2.0 * (InMain.Origin - InRequest.Position).Dot(Normal));

        RenderView View  = InMain;
        View.Origin         = Eye;
        View.ViewportPixels = Pixels;
        View.View           = Matrix4x4().LookAt(Eye, Reflect(Forward, Normal), Reflect(Up, Normal));
        View.Bounds         = FrustumBounds(View);   // before the oblique plane, which tilts the far plane too
        const float Plane[4] = { float(Normal.GetX()), float(Normal.GetY()), float(Normal.GetZ()),
                                 float(-Normal.Dot(InRequest.Position)) };
        ClipNearToPlane(View, Plane);   // what is behind the mirror (the floor it lies on) stays out of the reflection
        OutViews[0] = View;
        return 1;
    }
}

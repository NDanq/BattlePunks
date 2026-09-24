#ifndef RENDERCORE_RENDERVIEW_H
#define RENDERCORE_RENDERVIEW_H

#include <cmath>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

/*
 * What the render thread sees the scene through: a view/projection pair, and
 * the world-space box those matrices actually cover -- so RenderScene::Render
 * can cull a proxy against Bounds without asking GDI anything.
 *
 * POD, built entirely on the game thread (WorldBridge, in a later build-order
 * step) and sent to the render thread by value inside a command -- the same
 * "assembled on the right thread, then handed down as a value" shape
 * NimbleEngine::FrameContext already uses for the same reason: GDICamera's
 * own accessors are cached reads with no cross-thread contract, so nothing
 * here is computed FROM a live GDI object, only INTO one, on the render
 * thread, by RenderContext.
 */
namespace RenderCore
{
    struct RenderView
    {
        EngineCore::Matrix4x4 View;
        EngineCore::Matrix4x4 Projection;
        EngineCore::Vector3   Origin;
        EngineCore::Size      ViewportPixels;

        /* World-space box this view can see. IsEmpty() (GameCore::AABB's own
         * default-constructed state) means "cull nothing" -- the state a
         * RenderView has before MakeOrthographic ever runs, and the safest
         * default for a caller that builds one by hand for a test. */
        GameCore::AABB Bounds;

        float ZNear = -1.0f;
        float ZFar  = 1000.0f;

        /*
         * Reproduces GDICamera::UpdateDimensions exactly (GDIView.h):
         * InitOrthographicView(0, Height, 0, Width, ZNear, ZFar) and
         * LookAt(Origin, (0,0,1), (0,1,0)) -- the same view every GDIView in
         * this engine has always used, so the Basic shaders see nothing new.
         *
         * Bounds spans (Origin.x, Origin.y, Origin.z+ZNear) to
         * (Origin.x+Width, Origin.y+Height, Origin.z+ZFar) -- the same box
         * the orthographic projection above actually covers.
         */
        static RenderView MakeOrthographic(const EngineCore::Size InViewport, const EngineCore::Vector3& InOrigin,
                                            const float InZNear, const float InZFar)
        {
            RenderView Result;
            Result.Origin         = InOrigin;
            Result.ViewportPixels = InViewport;
            Result.ZNear          = InZNear;
            Result.ZFar           = InZFar;

            const float Width  = float(InViewport.Width);
            const float Height = float(InViewport.Height);

            Result.Projection.InitOrthographicView(0.0f, Height, 0.0f, Width, InZNear, InZFar);
            Result.View.InitTranslation(InOrigin * -1.0);
            Result.View = EngineCore::Matrix4x4().LookAt(InOrigin, EngineCore::Vector3(0, 0, 1), EngineCore::Vector3(0, 1, 0));

            Result.Bounds = GameCore::AABB(
                EngineCore::Vector3(InOrigin.GetX(), InOrigin.GetY(), InOrigin.GetZ() + InZNear),
                EngineCore::Vector3(InOrigin.GetX() + Width, InOrigin.GetY() + Height, InOrigin.GetZ() + InZFar));

            return Result;
        }

        /*
         * A perspective view for tools (the editor viewport) -- nothing in the
         * game path builds one. Same conventions as MakeOrthographic, so a
         * scene looks the same way up in both:
         *
         *   View       LookAt(InEye, InForward, (0,1,0)) -- view-space +Z is
         *              InForward, exactly as the orthographic view's is.
         *   Projection negates Y, as InitOrthographicView(0, Height, ...)
         *              does, so world +Y runs DOWN the screen -- the game's
         *              pixel-space convention. Screen-up is world -Y.
         *   Depth      near -> 0, far -> w: inside both the [-w,w] and [0,w]
         *              clip conventions, so every backend keeps the geometry.
         *
         * Built by hand into Data rather than as a Matrix4x4::InitPerspective,
         * so the vendored Core subtree needs no edit. Row-major with the
         * translation column in [i][3], matching every Init* in Matrix.h.
         *
         * Bounds is left empty -- "cull nothing" -- since a frustum is not an
         * AABB and a loose box would buy little for an editor scene.
         * InForward must not be parallel to world Y (the caller clamps pitch).
         */
        static RenderView MakePerspective(const EngineCore::Size InViewport, const EngineCore::Vector3& InEye,
                                          const EngineCore::Vector3& InForward, const float InFovYDegrees,
                                          const float InZNear, const float InZFar)
        {
            RenderView Result;
            Result.Origin         = InEye;
            Result.ViewportPixels = InViewport;
            Result.ZNear          = InZNear;
            Result.ZFar           = InZFar;

            const float Aspect = InViewport.Height > 0 ? float(InViewport.Width) / float(InViewport.Height) : 1.0f;
            const float FocalY = 1.0f / std::tan(InFovYDegrees * 0.5f * 3.14159265f / 180.0f);
            const float FocalX = FocalY / Aspect;
            const float Range  = InZFar - InZNear;

            EngineCore::Matrix4x4& P = Result.Projection;
            P.Data[0][0] = FocalX; P.Data[0][1] = 0.0f;    P.Data[0][2] = 0.0f;            P.Data[0][3] = 0.0f;
            P.Data[1][0] = 0.0f;   P.Data[1][1] = -FocalY; P.Data[1][2] = 0.0f;            P.Data[1][3] = 0.0f;
            P.Data[2][0] = 0.0f;   P.Data[2][1] = 0.0f;    P.Data[2][2] = InZFar / Range;  P.Data[2][3] = -InZFar * InZNear / Range;
            P.Data[3][0] = 0.0f;   P.Data[3][1] = 0.0f;    P.Data[3][2] = 1.0f;            P.Data[3][3] = 0.0f;

            Result.View = EngineCore::Matrix4x4().LookAt(InEye, InForward, EngineCore::Vector3(0, 1, 0));
            return Result;
        }
    };
}

#endif //RENDERCORE_RENDERVIEW_H

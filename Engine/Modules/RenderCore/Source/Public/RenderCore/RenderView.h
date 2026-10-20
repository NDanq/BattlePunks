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

        /* SPLITSCREEN (RenderTypes.h's RENDER_VIEWS_MAX): which of its scene's
         * views this is, and where its ViewportPixels sit in the window
         * (physical pixels, row 0 the top). ViewportPixels stays the view's
         * own size, so its aspect and every pass see just this view. */
        uint32_t              ViewIndex = 0;
        int32_t               ViewportX = 0;
        int32_t               ViewportY = 0;

        /* OCCLUSION REVEAL (RENDERPIPELINE.md, "Revealing players"): up to
         * RENDER_REVEAL_MAX points this view keeps in sight -- its players.
         * What draws marked to reveal them (RenderComponent::bRevealPlayers)
         * nearer the eye than a point, inside a circle of RevealRadius world
         * units round it on screen, is dithered away. */
        EngineCore::Vector3   RevealPoints[4];   // RENDER_REVEAL_MAX
        uint32_t              RevealCount  = 0;
        float                 RevealRadius = 0.0f;

        /* World-space box this view can see. IsEmpty() (GameCore::AABB's own
         * default-constructed state) means "cull nothing" -- the state a
         * RenderView has before MakeOrthographic ever runs, and the safest
         * default for a caller that builds one by hand for a test. */
        GameCore::AABB Bounds;

        float ZNear = -1.0f;
        float ZFar  = 1000.0f;

        /*
         * THE ENGINE'S AXES: left-handed, +X right, +Y UP, +Z into the screen
         * (away from the viewer).
         *
         * One pixel per unit, Origin at the BOTTOM-LEFT of the viewport:
         * InitOrthographicView(Height, 0, 0, Width, ZNear, ZFar) and
         * LookAt(Origin, (0,0,1), (0,1,0)). GDI's own GDICamera keeps its
         * Y-down 2D view; RenderCore does not use it.
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

            Result.Projection.InitOrthographicView(Height, 0.0f, 0.0f, Width, InZNear, InZFar);
            Result.View.InitTranslation(InOrigin * -1.0);
            Result.View = EngineCore::Matrix4x4().LookAt(InOrigin, EngineCore::Vector3(0, 0, 1), EngineCore::Vector3(0, 1, 0));

            Result.Bounds = GameCore::AABB(
                EngineCore::Vector3(InOrigin.GetX(), InOrigin.GetY(), InOrigin.GetZ() + InZNear),
                EngineCore::Vector3(InOrigin.GetX() + Width, InOrigin.GetY() + Height, InOrigin.GetZ() + InZFar));

            return Result;
        }

        /*
         * A perspective view for tools. Same conventions as MakeOrthographic:
         *
         *   View       LookAt(InEye, InForward, (0,1,0)); view +Z is InForward.
         *   Projection keeps Y, so world +Y runs UP the screen.
         *   Depth      near -> 0, far -> w: valid for [-w,w] and [0,w] clip.
         *
         * Built by hand (row-major, translation in [i][3]) so the vendored
         * Core subtree needs no edit. Bounds is empty (cull nothing).
         * InForward must not be parallel to world Y.
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
            P.Data[1][0] = 0.0f;   P.Data[1][1] = FocalY;  P.Data[1][2] = 0.0f;            P.Data[1][3] = 0.0f;
            P.Data[2][0] = 0.0f;   P.Data[2][1] = 0.0f;    P.Data[2][2] = InZFar / Range;  P.Data[2][3] = -InZFar * InZNear / Range;
            P.Data[3][0] = 0.0f;   P.Data[3][1] = 0.0f;    P.Data[3][2] = 1.0f;            P.Data[3][3] = 0.0f;

            Result.View = EngineCore::Matrix4x4().LookAt(InEye, InForward, EngineCore::Vector3(0, 1, 0));
            return Result;
        }
    };
}

#endif //RENDERCORE_RENDERVIEW_H

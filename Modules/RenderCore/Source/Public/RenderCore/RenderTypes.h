#ifndef RENDERCORE_RENDERTYPES_H
#define RENDERCORE_RENDERTYPES_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/RenderCoreAPI.h"   // the WITH_EDITOR check; no RENDERCORE_API is used here

/*
 * The small POD vocabulary shared across every RenderCore header: the
 * backend enum, what a frame looks like from the outside, the transform
 * shape a proxy is fed, and a stats snapshot. Header-only -- nothing here has
 * an out-of-line member, so nothing is RENDERCORE_API; see RenderCoreAPI.h.
 */
namespace RenderCore
{
    /*
     * Mirrors GDI's ERenderPlatform (GDIDefinitions.h) value-for-value.
     * RenderCoreInternal.h static_asserts every pair; this header exists so
     * that public RenderCore headers need no GDI include at all -- GDI stays
     * entirely inside RenderCore.dll, never in Public/.
     *
     * NULL IS ZERO, deliberately reordered from GDI's own ERP_OpenGL = 0.
     * A zero-initialized or defaulted backend value should be the one that
     * needs no window and no GPU, not the one that needs a live WGL context --
     * GDIDefinitions.h carries the matching reorder, and DESIGN.md records
     * why this is the one deliberate edit inside the vendored GDI subtree.
     */
    enum class ERenderBackend : uint8_t
    {
        Null   = 0,
        OpenGL = 1,
        Vulkan = 2,
        Metal  = 3,
        D3D12  = 4,
    };

    /*
     * What Application::EndFrame hands the Renderer facade. A value, like
     * NimbleEngine::FrameContext, which RenderCore cannot include -- GameCore
     * and RenderCore are both linked PUBLIC beneath NimbleEngine, and neither
     * may depend on the engine layer above it.
     */
    struct FrameInfo
    {
        uint64_t         FrameNumber  = 0;
        double           DeltaSeconds = 0.0;
        EngineCore::Size ViewportPixels;
    };

    /*
     * The same Position/Rotation/Scale triple GameCore::TransformComponent
     * holds, restated here rather than depending on GameCore::Components.h --
     * a proxy needs the numbers, not the component's storage or its
     * ComponentOwner header.
     */
    struct RenderTransform
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;              // Euler degrees, applied Z * Y * X -- matches TransformComponent::GetLocalMatrix
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};

        /* Translation * (Rz * Ry * Rx) * Scale -- identical composition to
         * TransformComponent::GetLocalMatrix(), restated here so a proxy
         * never has to reach back into GameCore::Components.h for it. */
        EngineCore::Matrix4x4 ToMatrix() const
        {
            EngineCore::Matrix4x4 T; T.InitTranslation(Position);
            EngineCore::Matrix4x4 S; S.InitScale(Scale);

            EngineCore::Matrix4x4 Rx; Rx.InitRotation(float(Rotation.GetX()), EngineCore::Vector3(1.0, 0.0, 0.0));
            EngineCore::Matrix4x4 Ry; Ry.InitRotation(float(Rotation.GetY()), EngineCore::Vector3(0.0, 1.0, 0.0));
            EngineCore::Matrix4x4 Rz; Rz.InitRotation(float(Rotation.GetZ()), EngineCore::Vector3(0.0, 0.0, 1.0));

            return T * (Rz * Ry * Rx) * S;
        }
    };

#if WITH_EDITOR
    /* A tool overlay segment for Renderer::DrawDebugLine: drawn after every
     * proxy, unculled, ThicknessPixels wide. Trivially copyable. */
    struct DebugLine
    {
        EngineCore::Vector3 Start;
        EngineCore::Vector3 End;
        EngineCore::Color   Color{255, 255, 255};
        float               ThicknessPixels = 2.0f;
    };
#endif // WITH_EDITOR

    /*
     * A snapshot of RenderThread's live counters (each an atomic there),
     * copied out by RenderThread::GetStats() / Renderer::GetStats() for a
     * host to log or display. Not itself updated atomically as a whole --
     * a caller sees a consistent snapshot of no single instant, which is
     * fine for a per-second log line and wrong for anything stricter.
     */
    struct RenderStats
    {
        uint64_t FramesRendered         = 0;
        uint64_t CommandsExecuted       = 0;
        uint64_t CommandsExecutedInline = 0;   // ran on the calling thread because it WAS the render thread -- see RenderCommand.h
        uint64_t DrawCalls              = 0;
        uint64_t ProxiesLive            = 0;
        uint64_t ProxiesCulledLastFrame = 0;
        uint32_t PeakQueueDepth         = 0;

        /* "stat unit": each averaged over the last half second (0 until the
         * first half second has passed). Frame is the game thread's time
         * from one EndFrame to the next; Game Thread is that less the time it
         * waited on the render thread or presented; Render Thread is its time
         * running a frame's commands, less the swap. */
        float FrameMs        = 0.0f;
        float GameThreadMs   = 0.0f;
        float RenderThreadMs = 0.0f;
    };
}

#endif //RENDERCORE_RENDERTYPES_H

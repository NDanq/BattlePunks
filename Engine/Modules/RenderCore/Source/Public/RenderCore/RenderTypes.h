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
    /*
     * What a scene's view shows (Renderer::SetViewMode; the editor's Show >
     * View Mode): its picture as lit; Unlit -- base colour and emissive as
     * they are, no light at all; or one of the render targets the frame drew
     * (Renderer::GetViewableTargets), stretched over the view.
     */
    enum class ERenderViewMode : uint8_t
    {
        Lit          = 0,
        Unlit        = 1,
        RenderTarget = 2,
    };

    /* What a material preview shows (Renderer::RequestMaterialPreview): the
     * material lit by its studio light, or one of its surface's channels as
     * drawn -- base colour, or emissive divided by EMISSIVE_PREVIEW_SCALE (so
     * past 1 survives 8 bits) -- display space, what the bake measures
     * materials by. Scene_View's values. */
    enum class EMaterialPreviewChannel : uint8_t
    {
        Lit       = 0,
        BaseColor = 2,
        Emissive  = 3,
    };
    constexpr float EMISSIVE_PREVIEW_SCALE = 16.0f;

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
    /* "stat frame": the GPU spans a scene's frame is timed in, in order. */
    enum ERenderGpuTimer : uint32_t
    {
        RGT_PrePass = 0,
        RGT_BasePass,            // the G-buffer
        RGT_AmbientOcclusion,
        RGT_Shadows,
        RGT_Lighting,
        RGT_Forward,             // Unlit and the translucent stages
        RGT_PostProcess,
        RGT_Total,               // the whole scene, start to end of its stages
        RGT_SceneCapture,        // every Scene Capture Probe's views, before the scene's stages (not in Total)
        RGT_Particles,           // every particle emitter's compute passes, before the captures (not in Total)
        RGT_Count
    };

    /* "BasePass", "Total", ... */
    inline const char* GetRenderGpuTimerName(const ERenderGpuTimer InTimer)
    {
        static const char* const Names[RGT_Count] = { "PrePass", "BasePass", "AO", "Shadows", "Lighting", "Forward", "PostProcess", "Total", "Captures", "Particles" };
        return InTimer < RGT_Count ? Names[InTimer] : "";
    }

    struct RenderStats
    {
        uint64_t FramesRendered         = 0;
        uint64_t CommandsExecuted       = 0;
        uint64_t CommandsExecutedInline = 0;   // ran on the calling thread because it WAS the render thread -- see RenderCommand.h
        uint64_t DrawCalls              = 0;
        uint64_t ParticleSimulations    = 0;   // emitters simulated, every frame each ran (not culled or paused offscreen)
        uint64_t ProxiesLive            = 0;
        uint64_t ProxiesCulledLastFrame = 0;
        uint32_t PeakQueueDepth         = 0;

        /* Static draw lists (RENDERPIPELINE.md): static proxies drawn from
         * their kept lists last frame, and how many lists were (re)built
         * over the scene's lifetime -- steady when nothing static changes. */
        uint64_t StaticProxiesDrawn     = 0;
        uint64_t StaticListBuilds       = 0;

        /* Occlusion culling (RENDERPIPELINE.md): proxies in view not drawn
         * last frame because the answer before found them hidden, and how
         * many boxes were queried. The GPU's own skips are not counted. */
        uint64_t ProxiesOccludedLastFrame  = 0;
        uint64_t OcclusionQueriesLastFrame = 0;

        /* "stat unit": each averaged over the last half second (0 until the
         * first half second has passed). Frame is the game thread's time
         * from one EndFrame to the next; Game Thread is that less the time it
         * waited on the render thread or presented; Render Thread is its time
         * running a frame's commands, less the swap. Wait, Present and Idle
         * are the rest of Frame: the game thread waiting on the render thread,
         * presenting (blocked on the swapchain), and between frames (the
         * host's loop or frame timer). */
        float FrameMs        = 0.0f;
        float GameThreadMs   = 0.0f;
        float RenderThreadMs = 0.0f;
        float WaitMs         = 0.0f;
        float PresentMs      = 0.0f;
        float IdleMs         = 0.0f;

        /* "stat frame" (Stats.Frame on, and a backend that can time the GPU):
         * the default scene's GPU time per span, averaged over half a second;
         * -1 where it was not timed (off, or the span did not run). */
        float GpuMs[RGT_Count] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    };
}

#endif //RENDERCORE_RENDERTYPES_H

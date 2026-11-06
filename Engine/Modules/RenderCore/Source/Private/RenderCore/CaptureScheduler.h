#ifndef RENDERCORE_CAPTURESCHEDULER_H
#define RENDERCORE_CAPTURESCHEDULER_H

#include <cstdint>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderLighting.h"

/*
 * THE CAPTURE BUDGET (GLOBAL_ILLUMINATION.md, "Capture budget"): how much of
 * a frame GI Volume probe updates and Reflection Probe captures may take, and
 * when a capture that has converged may stop. Pure bookkeeping -- no GDI --
 * so the smoke test checks it on the Null backend; RenderScene drives it.
 *
 * Work is counted in units: a cube face of side S is CaptureFaceUnits(S)
 * (1 for a GI probe's 16 x 16 face; a face's draws cost about the same
 * whatever its size, its pixels only a little). CaptureBudget learns the
 * milliseconds a unit takes from the Captures span's GPU time -- frames in
 * flight late, matched by the GDI block's serial -- and from the render
 * thread's time recording the faces, and hands each frame the units that
 * fit [Rendering] CaptureBudgetMs at the dearer of the two. A cost each frame's
 * captures pay once (the shared shadow maps) is folded into the unit's,
 * which settles where the whole span fills the budget.
 */
namespace RenderCore
{
    /* A cube face of side InFace, in units: 1 + its pixels / 65536. */
    RENDERCORE_API float CaptureFaceUnits(uint32_t InFace);

    class RENDERCORE_API CaptureBudget
    {
    public:
        /* What a unit is taken to cost before any frame has been timed. */
        static constexpr float DEFAULT_UNIT_MS = 0.1f;

        /* The units this frame may spend for InBudgetMs: at least InMinimum. */
        float GetAllowance(float InBudgetMs, float InMinimum = 1.0f) const;

        /* The units the frame of GDI block InSerial spent. */
        void Record(uint64_t InSerial, float InUnits);

        /* The Captures span of block InSerial took InGpuMs: a unit's cost,
         * smoothed, if that frame spent any. */
        void Measured(uint64_t InSerial, float InGpuMs);

        /* The render thread took InCpuMs for InUnits this frame: a unit's CPU cost,
         * smoothed. The allowance is for whichever of the two is dearer. */
        void MeasuredCpu(float InUnits, float InCpuMs);

        float GetUnitMs() const { return UnitMs; }
        float GetUnitCpuMs() const { return UnitCpuMs; }
        bool  HasMeasured() const { return bMeasured; }
        void  Reset();

    private:
        static constexpr uint32_t RING = 8;
        uint64_t Serials[RING] = {};
        float    Units[RING]   = {};
        float    UnitMs        = DEFAULT_UNIT_MS;
        float    UnitCpuMs     = 0.0f;
        bool     bMeasured     = false;
    };

    /*
     * Whether a capture has converged: rounds of updates (InTotal each) since
     * what it sees last changed. Observe each frame with the signature of
     * what it sees; Updated after each update; IsConverged once InRounds
     * rounds are in -- a GI Volume's first round lights with the new direct
     * light, each after it adds a bounce.
     */
    struct RENDERCORE_API CaptureConvergence
    {
        uint64_t Signature   = 0;
        uint64_t SinceChange = 0;
        bool     bKnown      = false;

        /* True if InSignature differs from the last (or is the first). */
        bool Observe(uint64_t InSignature);
        void Updated(uint64_t InCount = 1) { SinceChange += InCount; }
        bool IsConverged(uint64_t InTotal, uint32_t InRounds) const;
        void Reset() { *this = CaptureConvergence(); }
    };

    /*
     * What lights a capture, as a hash: the sun's direction in steps of
     * InSunStepDegrees (so a TimeOfDay sun moving less than that changes
     * nothing), its colour, intensity and shadow; the sky's colours and
     * HDRI; every local light (order-free: WorldBridge sorts them by distance
     * to the camera); whether shadows are on. Intensities to 1 %.
     */
    RENDERCORE_API uint64_t CaptureLightingSignature(const RenderLighting& InLighting, float InSunStepDegrees);
}

#endif //RENDERCORE_CAPTURESCHEDULER_H

#ifndef RENDERCORE_PARALLELRECORDING_H
#define RENDERCORE_PARALLELRECORDING_H

#include <cstdint>
#include <string>

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * Parallel draw recording's gate (MULTITHREADED_RENDERING.md, "The gate"):
 * whether a stage's draws may be recorded on several threads, decided from
 *
 *   - what the backend offers (GDIGetParallelRecordingSupport: Vulkan
 *     secondary command buffers, D3D12 command lists, Metal's parallel
 *     encoder; not OpenGL; Null runs the chunks serially),
 *   - the job system's workers (EngineCore::GetJobSystem(), the host's pool),
 *   - [Rendering] ParallelRecording = Auto | On | Off, and
 *   - [Rendering] ParallelMinDraws, the draws a stage needs before Auto
 *     splits it.
 *
 * The render thread evaluates it once GDI is up, and again when either
 * setting changes, and logs the verdict; the Base Pass and Shadows read it
 * (RenderContext's IssueInParallel).
 */

namespace RenderCore
{
    enum class EParallelRecordingMode : uint8_t { Off = 0, On = 1, Auto = 2 };

    struct ParallelRecordingState
    {
        EParallelRecordingMode Mode = EParallelRecordingMode::Off;
        bool        bBackend              = false;   // GDI could record in parallel
        bool        bSerialOnly           = false;   // ... but runs every chunk on the calling thread (Null)
        bool        bInheritedQueries     = false;   // GDIParallelRecordingSupport's, for the log
        bool        bInheritedPredication = false;
        uint32_t    Workers               = 0;       // the job system's workers; 0 without one
        uint32_t    ActiveWorkers         = 0;       // ... of them taking jobs (a power profile parks the rest)
        uint32_t    MinDraws              = 0;       // [Rendering] ParallelMinDraws
        uint32_t    MaxThreads            = 0;       // [Rendering] ParallelMaxThreads; 0 no cap
        bool        bEnabled              = false;   // Mode not Off, the backend can, and there is somewhere to run chunks
        std::string Summary;                         // the verdict, a line for the log
    };

    /* Reads the config, GDI and the job system now: on the render thread once
     * GDI is allocated (RenderThread does), or in a test. */
    RENDERCORE_API ParallelRecordingState EvaluateParallelRecording();

    /* Whether a stage of InDraws draws should be split: never when disabled;
     * On, always; Auto, from MinDraws draws. */
    RENDERCORE_API bool ShouldRecordInParallel(const ParallelRecordingState& InState, uint32_t InDraws);

    /* The verdict the render thread latched at startup (a default, disabled
     * state before it has); RenderStats reports it. */
    RENDERCORE_API ParallelRecordingState GetParallelRecordingState();

    /* The render thread's, once a frame: if [Rendering] ParallelRecording or
     * ParallelMinDraws changed since, evaluates the gate again, latches and
     * logs it, and returns true. */
    RENDERCORE_API bool TakeParallelRecordingChange();
    RENDERCORE_API void                   SetParallelRecordingState(const ParallelRecordingState& InState);
}

#endif //RENDERCORE_PARALLELRECORDING_H

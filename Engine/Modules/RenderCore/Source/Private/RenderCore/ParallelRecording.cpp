#include "Public/RenderCore/ParallelRecording.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreJob.h"
#include "Public/Core/CoreLog.h"
#include "Public/GDI/GDI.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

namespace
{
    /* Set by either var's change (the console, an ini), taken by the render
     * thread (TakeParallelRecordingChange), which evaluates the gate again. */
    std::atomic<bool> GParallelRecordingChanged{false};
    void OnParallelRecordingChanged(const EngineCore::ConfigVar&) { GParallelRecordingChanged.store(true); }
}

/* Auto | On | Off. ParallelMinDraws: Phase 1 left a draw at 1.5-6 us of the
 * render thread, so a stage of a few hundred draws already costs more than a
 * pass's fork and join (MULTITHREADED_RENDERING.md, phase 3). */
NIMBLE_CONFIG_STRING(GRenderParallelRecording, "Rendering", "ParallelRecording", "Auto", &OnParallelRecordingChanged);
NIMBLE_CONFIG_INT(GRenderParallelMinDraws,     "Rendering", "ParallelMinDraws",  256,    &OnParallelRecordingChanged);
/* The most threads a stage's chunks spread over, the render thread included;
 * 0 is every active job worker plus it. PowerSaver's 2 keeps the fan-out
 * narrow and the same frame to frame (SCALABILITY.md). */
NIMBLE_CONFIG_INT(GRenderParallelMaxThreads,   "Rendering", "ParallelMaxThreads", 0,     &OnParallelRecordingChanged);

namespace RenderCore
{
    namespace
    {
        std::mutex             GStateLock;
        ParallelRecordingState GState;

        /* "Auto", "On", "Off" in any case; 1/0 and true/false too. Anything
         * else is Auto, said once. */
        EParallelRecordingMode ParseMode(std::string InText)
        {
            for(char& Each : InText) { Each = char(std::tolower(static_cast<unsigned char>(Each))); }
            if(InText == "off" || InText == "0" || InText == "false") { return EParallelRecordingMode::Off; }
            if(InText == "on" || InText == "1" || InText == "true")   { return EParallelRecordingMode::On; }
            if(InText != "auto")
            {
                CORE_LOG_WARN("[RenderCore] [Rendering] ParallelRecording='" + InText + "' is not Auto, On or Off; using Auto")
            }
            return EParallelRecordingMode::Auto;
        }

        const char* ModeName(const EParallelRecordingMode InMode)
        {
            switch(InMode)
            {
                case EParallelRecordingMode::Off: return "Off";
                case EParallelRecordingMode::On:  return "On";
                default:                          return "Auto";
            }
        }
    }

    ParallelRecordingState EvaluateParallelRecording()
    {
        ParallelRecordingState Out;
        const char* ModeText = GRenderParallelRecording.GetString();
        Out.Mode     = ParseMode(ModeText ? std::string(ModeText) : std::string("Auto"));
        Out.MinDraws = uint32_t(std::max(GRenderParallelMinDraws.GetInt(), 0));
        Out.MaxThreads = uint32_t(std::max(GRenderParallelMaxThreads.GetInt(), 0));

        const GDIParallelRecordingSupport Support = GDIGetParallelRecordingSupport();
        Out.bBackend              = Support.bSupported;
        Out.bSerialOnly           = Support.bSerialOnly;
        Out.bInheritedQueries     = Support.bInheritedQueries;
        Out.bInheritedPredication = Support.bInheritedPredication;

        const EngineCore::JobSystem* Jobs = EngineCore::GetJobSystem();
        Out.Workers       = Jobs ? Jobs->GetWorkerCount() : 0;
        Out.ActiveWorkers = Jobs ? Jobs->GetActiveWorkerCount() : 0;

        /* Chunks need somewhere to run: the pool's workers, or the calling
         * thread where the backend runs them serially anyway. */
        Out.bEnabled = Out.Mode != EParallelRecordingMode::Off && Out.bBackend && (Out.Workers > 0 || Out.bSerialOnly);

        std::string Line = "[RenderCore] parallel draw recording: ";
        if(Out.bEnabled)
        {
            Line += "available -- ";
            Line += Support.Mechanism;
            if(!Out.bSerialOnly)
            {
                Line += std::string(" (inherited queries ") + (Out.bInheritedQueries ? "yes" : "no")
                      + ", inherited predication " + (Out.bInheritedPredication ? "yes" : "no") + ")";
                Line += ", " + std::to_string(Out.ActiveWorkers) + " of " + std::to_string(Out.Workers) + " job worker(s) active";
                if(Out.MaxThreads > 0)
                {
                    Line += ", chunks over at most " + std::to_string(Out.MaxThreads) + " threads ([Rendering] ParallelMaxThreads)";
                }
            }
            Line += std::string("; [Rendering] ParallelRecording=") + ModeName(Out.Mode);
            if(Out.Mode == EParallelRecordingMode::Auto)
            {
                Line += ", for stages of " + std::to_string(Out.MinDraws) + "+ draws";
            }
            Line += " (the Base Pass and Shadows).";
        }
        else if(Out.Mode == EParallelRecordingMode::Off)
        {
            Line += "off ([Rendering] ParallelRecording=Off)";
        }
        else if(!Out.bBackend)
        {
            Line += std::string("unavailable -- ") + (Support.Reason && *Support.Reason ? Support.Reason : "the backend cannot");
        }
        else
        {
            Line += "unavailable -- no job system to run chunks on (the host registers one: CoreJob.h)";
        }
        Out.Summary = std::move(Line);
        return Out;
    }

    bool ShouldRecordInParallel(const ParallelRecordingState& InState, const uint32_t InDraws)
    {
        if(!InState.bEnabled || InDraws == 0)
        {
            return false;
        }
        return InState.Mode == EParallelRecordingMode::On || InDraws >= InState.MinDraws;
    }

    bool TakeParallelRecordingChange()
    {
        /* A power profile parking or waking job workers changes the verdict's
         * numbers without touching either var (Scalability.h's Jobs.*). */
        const EngineCore::JobSystem* Jobs = EngineCore::GetJobSystem();
        bool bWorkersMoved = false;
        if(Jobs)
        {
            std::lock_guard<std::mutex> Lock(GStateLock);
            bWorkersMoved = GState.Workers > 0 && GState.ActiveWorkers != Jobs->GetActiveWorkerCount();
        }
        if(!GParallelRecordingChanged.exchange(false) && !bWorkersMoved)
        {
            return false;
        }
        const ParallelRecordingState State = EvaluateParallelRecording();
        SetParallelRecordingState(State);
        CORE_LOG(State.Summary)
        return true;
    }

    ParallelRecordingState GetParallelRecordingState()
    {
        std::lock_guard<std::mutex> Lock(GStateLock);
        return GState;
    }

    void SetParallelRecordingState(const ParallelRecordingState& InState)
    {
        std::lock_guard<std::mutex> Lock(GStateLock);
        GState = InState;
    }
}

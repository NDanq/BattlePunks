#include "Public/RenderCore/ParallelRecording.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreJob.h"
#include "Public/Core/CoreLog.h"
#include "Public/GDI/GDI.h"

#include <algorithm>
#include <cctype>
#include <mutex>

NIMBLE_CONFIG_STRING(GRenderParallelRecording, "Rendering", "ParallelRecording", "Auto", nullptr);   // Auto | On | Off
NIMBLE_CONFIG_INT(GRenderParallelMinDraws,     "Rendering", "ParallelMinDraws",  1000,   nullptr);

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

        const GDIParallelRecordingSupport Support = GDIGetParallelRecordingSupport();
        Out.bBackend              = Support.bSupported;
        Out.bSerialOnly           = Support.bSerialOnly;
        Out.bInheritedQueries     = Support.bInheritedQueries;
        Out.bInheritedPredication = Support.bInheritedPredication;

        const EngineCore::JobSystem* Jobs = EngineCore::GetJobSystem();
        Out.Workers = Jobs ? Jobs->GetWorkerCount() : 0;

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
                Line += ", " + std::to_string(Out.Workers) + " job worker(s)";
            }
            Line += std::string("; [Rendering] ParallelRecording=") + ModeName(Out.Mode);
            if(Out.Mode == EParallelRecordingMode::Auto)
            {
                Line += ", for stages of " + std::to_string(Out.MinDraws) + "+ draws";
            }
            Line += ". No stage records in parallel yet (MULTITHREADED_RENDERING.md).";
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

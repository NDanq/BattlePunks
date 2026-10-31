#include "Public/GameCore/Scalability.h"

#include <cctype>
#include <cmath>
#include <cstring>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreJob.h"
#include "Public/GameCore/GameCoreLog.h"

using namespace EngineCore;

namespace GameCore
{
namespace Scalability
{
namespace
{
    void OnDetailModeChanged(const ConfigVar& InVar);
    void OnPowerProfileChanged(const ConfigVar& InVar);
    void OnJobsChanged(const ConfigVar& InVar);
}
}
}

/* [Scalability] -- see Scalability.h. Strings, so an .ini and the console
 * read as words; a value that parses as nothing keeps the last good one. */
NIMBLE_CONFIG_STRING(GScalabilityDetailMode,   "Scalability", "DetailMode",      "High", &GameCore::Scalability::OnDetailModeChanged);
NIMBLE_CONFIG_STRING(GScalabilityPowerProfile, "Scalability", "PowerProfile",    "Auto", &GameCore::Scalability::OnPowerProfileChanged);
NIMBLE_CONFIG_FLOAT (GScalabilityAutoPoll,     "Scalability", "AutoPollSeconds", 5.0f,   nullptr);

/* [Jobs] -- the job pool's occupancy, which the power profiles set. The pool's
 * threads are made once (CoreJob.h); these park and wake them, live.
 *   WorkerFraction    the share of workers taking jobs, 0-1 (1 all of them)
 *   MinActiveWorkers  never fewer than this awake (capped at the pool)
 *   IdleWaitMs        how often an idle worker wakes to look at the queue */
NIMBLE_CONFIG_FLOAT(GJobsWorkerFraction,   "Jobs", "WorkerFraction",   1.0f, &GameCore::Scalability::OnJobsChanged);
NIMBLE_CONFIG_INT  (GJobsMinActiveWorkers, "Jobs", "MinActiveWorkers", 2,    &GameCore::Scalability::OnJobsChanged);
NIMBLE_CONFIG_INT  (GJobsIdleWaitMs,       "Jobs", "IdleWaitMs",       2,    &GameCore::Scalability::OnJobsChanged);

namespace GameCore
{
namespace Scalability
{
namespace
{
    /*
     * THE ENGINE'S TABLES. High is the engine's shipped defaults, so a project
     * that never mentions scalability renders exactly as it did. A project's
     * [Scalability_<Tier>] line replaces the matching row; and a row whose
     * var the project's own file already sets ([Shadows] Resolution=4096) is
     * skipped -- the engine's per-tier guess never trumps a project's choice,
     * only the project's own tier sections do.
     */
    struct DetailRow
    {
        const char* Path;
        const char* Values[DETAIL_MODE_COUNT];   // Low, Medium, High, Ultra
    };
    const DetailRow GDetailRows[] = {
        { "Shadows.Resolution",                { "1024",  "2048",  "2048",  "4096" } },
        { "Shadows.Filter",                    { "0",     "1",     "1",     "2" } },
        { "Shadows.MaxShadowedLights",         { "1",     "2",     "4",     "4" } },
        { "Shadows.MaxInsetShadows",           { "0",     "2",     "4",     "4" } },
        { "Shadows.DistanceScale",             { "0.5",   "0.75",  "1",     "1.5" } },
        { "Shadows.ContactShadows",            { "false", "false", "false", "true" } },
        { "Shadows.TranslucentShadows",        { "false", "true",  "true",  "true" } },
        { "Rendering.ScreenSpaceGI",           { "false", "false", "true",  "true" } },
        { "Rendering.StochasticReflections",   { "false", "false", "true",  "true" } },
        { "Rendering.SubsurfaceScattering",    { "false", "true",  "true",  "true" } },
        { "Rendering.DynamicGI",               { "false", "true",  "true",  "true" } },
        { "Rendering.GIProbesPerFrame",        { "1",     "1",     "1",     "2" } },
        { "Rendering.TextureStreamingPoolMB",  { "256",   "384",   "512",   "1024" } },
        { "Rendering.TextureStreamingMipBias", { "1",     "0",     "0",     "0" } },
        { "Rendering.Anisotropy",              { "2",     "4",     "8",     "16" } },
        { "Rendering.AllowAmbientOcclusion",   { "false", "true",  "true",  "true" } },
        { "Rendering.AllowScreenSpaceReflections", { "false", "false", "true", "true" } },
        { "Rendering.AllowVolumetricFog",      { "false", "true",  "true",  "true" } },
        { "Rendering.AllowDepthOfField",       { "false", "false", "true",  "true" } },
        { "Rendering.AllowMotionBlur",         { "false", "true",  "true",  "true" } },
        { "Rendering.AllowBloom",              { "true",  "true",  "true",  "true" } },
        { "Animation.FullRateDistance",        { "1500",  "2000",  "3000",  "5000" } },
        { "Animation.FreezeDistance",          { "8000",  "12000", "20000", "30000" } },
        { "Destruction.MaxDebris",             { "64",    "128",   "256",   "512" } },
    };

    struct PowerRow
    {
        const char* Path;
        const char* Saver;
        const char* Max;
    };
    const PowerRow GPowerRows[] = {
        { "Jobs.WorkerFraction",                 "0.5",  "1" },     // half the pool parked
        { "Jobs.IdleWaitMs",                     "8",    "2" },     // fewer idle wake-ups
        { "Rendering.ParallelMaxThreads",        "2",    "0" },     // narrow, steady recording fan-out
        { "Display.PaceSpinMs",                  "0",    "2" },     // sleep the whole wait, never spin
        { "Display.FrameRateLimit",              "30",   "0" },
        { "Rendering.FramesInFlight",            "2",    "3" },     // at device creation
        { "Rendering.SwapchainExtraImages",      "1",    "2" },     // at swapchain (re)creation
        { "Rendering.TextureStreamingUploadMB",  "4",    "16" },
        { "Rendering.TextureStreamingMaxLoads",  "4",    "16" },
    };

    PowerSourceFn GPowerQuery    = nullptr;
    EPowerSource  GPowerSource   = EPowerSource::Unknown;
    EPowerProfile GPowerResolved = EPowerProfile::PowerMax;
    EDetailMode   GDetail        = EDetailMode::High;
    uint32_t      GSerial        = 0;
    double        GPollClock     = 0.0;
    bool          GInitialized   = false;

    bool EqualsNoCase(const char* InA, const char* InB)
    {
        for(; *InA && *InB; ++InA, ++InB)
        {
            if(std::tolower(static_cast<unsigned char>(*InA)) != std::tolower(static_cast<unsigned char>(*InB))) { return false; }
        }
        return *InA == *InB;
    }

    /* 0 Auto, 1 Saver, 2 Max; -1 for anything else. */
    int ParseProfileSetting(const char* InText)
    {
        if(!InText) { return -1; }
        if(EqualsNoCase(InText, "Auto")) { return 0; }
        if(EqualsNoCase(InText, "PowerSaver") || EqualsNoCase(InText, "Saver") || EqualsNoCase(InText, "Power-Saver")) { return 1; }
        if(EqualsNoCase(InText, "PowerMax") || EqualsNoCase(InText, "Max") || EqualsNoCase(InText, "Power-Max")) { return 2; }
        return -1;
    }

    bool IsMobilePlatform()
    {
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
        return true;
#else
        return false;
#endif
    }

    EPowerProfile ResolveProfile(const int InSetting)
    {
        if(InSetting == 1) { return EPowerProfile::PowerSaver; }
        if(InSetting == 2) { return EPowerProfile::PowerMax; }
        return IsMobilePlatform() || GPowerSource == EPowerSource::OnBattery ? EPowerProfile::PowerSaver : EPowerProfile::PowerMax;
    }

    /* A section's Section.Key lines laid over a table's: replace a row, or add one. */
    struct SectionCollect
    {
        const char*            Section;
        std::vector<Override>* Out;
    };
    void CollectSection(const char* InSection, const char* InKey, const char* InValue, void* InUserData)
    {
        SectionCollect& Collect = *static_cast<SectionCollect*>(InUserData);
        if(!EqualsNoCase(InSection, Collect.Section)) { return; }   // the prefix also visits the rest: claimed, unused
        for(Override& Existing : *Collect.Out)
        {
            if(EqualsNoCase(Existing.Path.c_str(), InKey)) { Existing.Value = InValue; return; }
        }
        Collect.Out->push_back({ InKey, InValue });
    }

    /* Whether the project's own file sets Path's var -- then the engine's row yields. */
    bool ProjectSets(const char* InPath)
    {
        const char* Dot = std::strchr(InPath, '.');
        if(!Dot) { return false; }
        return ConfigGetProjectValue(std::string(InPath, size_t(Dot - InPath)).c_str(), Dot + 1);
    }

    std::vector<ConfigOverlayValue> ToOverlay(const std::vector<Override>& InOverrides)
    {
        std::vector<ConfigOverlayValue> Out;
        Out.reserve(InOverrides.size());
        for(const Override& Each : InOverrides) { Out.push_back({ Each.Path, Each.Value }); }
        return Out;
    }

    void ApplyDetail()
    {
        EDetailMode Parsed;
        if(!ParseDetailMode(GScalabilityDetailMode.GetString(), Parsed))
        {
            GAMECORE_LOG_WARN(std::string("Scalability: '") + GScalabilityDetailMode.GetString()
                              + "' is not a detail mode (Low, Medium, High, Ultra); staying at " + DetailModeName(GDetail))
            return;
        }
        GDetail = Parsed;
        const int Changed = ConfigSetOverlay(EConfigOverlay::Detail, ToOverlay(ResolveDetailOverrides(GDetail)));
        ++GSerial;
        GAMECORE_LOG(std::string("Scalability: detail ") + DetailModeName(GDetail) + " (" + std::to_string(Changed) + " values changed)")
    }

    void ApplyPower()
    {
        const int Setting = ParseProfileSetting(GScalabilityPowerProfile.GetString());
        if(Setting < 0)
        {
            GAMECORE_LOG_WARN(std::string("Scalability: '") + GScalabilityPowerProfile.GetString()
                              + "' is not a power profile (Auto, PowerSaver, PowerMax); staying at " + PowerProfileName(GPowerResolved))
            return;
        }
        if(Setting == 0 && GPowerQuery) { GPowerSource = GPowerQuery(); }
        GPowerResolved = ResolveProfile(Setting);
        const int Changed = ConfigSetOverlay(EConfigOverlay::Power, ToOverlay(ResolvePowerOverrides(GPowerResolved)));
        ++GSerial;
        GAMECORE_LOG(std::string("Scalability: power ") + PowerProfileName(GPowerResolved)
                     + (Setting == 0 ? std::string(" (Auto, ") + PowerSourceName(GPowerSource) + ")" : std::string())
                     + " (" + std::to_string(Changed) + " values changed)")
    }

    void OnDetailModeChanged(const ConfigVar& /*InVar*/)
    {
        if(GInitialized) { ApplyDetail(); }
    }

    void OnPowerProfileChanged(const ConfigVar& /*InVar*/)
    {
        if(GInitialized) { ApplyPower(); }
    }

    /* Reads only its own var's siblings in [Jobs] once a load is done -- a
     * mid-file callback may see a sibling's old value, and the next one
     * fixes it. */
    void OnJobsChanged(const ConfigVar& /*InVar*/)
    {
        JobSystem* Jobs = GetJobSystem();
        if(!Jobs) { return; }
        const uint32_t Pool     = Jobs->GetWorkerCount();
        const float    Fraction = GJobsWorkerFraction.GetFloat();
        uint32_t       Active   = Fraction >= 1.0f ? Pool : uint32_t(std::ceil(float(Pool) * (Fraction > 0.0f ? Fraction : 0.0f)));
        const int      Min      = GJobsMinActiveWorkers.GetInt();
        if(Min > 0 && Active < uint32_t(Min)) { Active = uint32_t(Min); }
        if(Active < 1) { Active = 1; }
        Jobs->SetActiveWorkerLimit(Active >= Pool ? 0 : Active);
        Jobs->SetIdleWaitMs(GJobsIdleWaitMs.GetInt() > 0 ? uint32_t(GJobsIdleWaitMs.GetInt()) : 1u);
    }
}

void SetPowerSourceQuery(const PowerSourceFn InFn)
{
    GPowerQuery = InFn;
}

void Initialize()
{
    GInitialized = true;
    /* Power first: Detail sits above it, and either order gives the same
     * values -- this one logs in the order they stack. */
    ApplyPower();
    ApplyDetail();
    OnJobsChanged(GJobsWorkerFraction);
}

void Tick(const double InDeltaSeconds)
{
    if(!GInitialized || !GPowerQuery || ParseProfileSetting(GScalabilityPowerProfile.GetString()) != 0) { return; }
    const float Poll = GScalabilityAutoPoll.GetFloat();
    if(Poll <= 0.0f) { return; }

    GPollClock += InDeltaSeconds;
    if(GPollClock < double(Poll)) { return; }
    GPollClock = 0.0;

    const EPowerSource Source = GPowerQuery();
    if(Source == GPowerSource) { return; }
    GPowerSource = Source;
    if(ResolveProfile(0) != GPowerResolved) { ApplyPower(); }
}

EDetailMode GetDetailMode()
{
    return GDetail;
}

void SetDetailMode(const EDetailMode InMode)
{
    GScalabilityDetailMode.SetString(DetailModeName(InMode));
}

EPowerProfile GetPowerProfile()
{
    return GPowerResolved;
}

const char* GetPowerProfileSetting()
{
    switch(ParseProfileSetting(GScalabilityPowerProfile.GetString()))
    {
        case 1:  return "PowerSaver";
        case 2:  return "PowerMax";
        default: return "Auto";
    }
}

bool SetPowerProfileSetting(const char* InText)
{
    const int Setting = ParseProfileSetting(InText);
    if(Setting < 0) { return false; }
    static const char* const Names[] = { "Auto", "PowerSaver", "PowerMax" };
    GScalabilityPowerProfile.SetString(Names[Setting]);
    return true;
}

EPowerSource GetPowerSource()
{
    return GPowerSource;
}

const char* PowerProfileName(const EPowerProfile InProfile)
{
    return InProfile == EPowerProfile::PowerSaver ? "PowerSaver" : "PowerMax";
}

const char* PowerSourceName(const EPowerSource InSource)
{
    switch(InSource)
    {
        case EPowerSource::NoBattery: return "no battery";
        case EPowerSource::Plugged:   return "plugged in";
        case EPowerSource::OnBattery: return "on battery";
        default:                      return "unknown power source";
    }
}

uint32_t GetSerial()
{
    return GSerial;
}

std::vector<Override> ResolveDetailOverrides(const EDetailMode InMode)
{
    const uint8_t Tier = uint8_t(InMode) < DETAIL_MODE_COUNT ? uint8_t(InMode) : uint8_t(EDetailMode::High);
    std::vector<Override> Out;
    for(const DetailRow& Row : GDetailRows)
    {
        if(!ProjectSets(Row.Path)) { Out.push_back({ Row.Path, Row.Values[Tier] }); }
    }
    const std::string Section = std::string("Scalability_") + DetailModeName(EDetailMode(Tier));
    SectionCollect Collect{ Section.c_str(), &Out };
    ConfigForEachEntry("Scalability_", &CollectSection, &Collect);
    return Out;
}

std::vector<Override> ResolvePowerOverrides(const EPowerProfile InProfile)
{
    const bool bSaver = InProfile == EPowerProfile::PowerSaver;
    std::vector<Override> Out;
    for(const PowerRow& Row : GPowerRows)
    {
        if(!ProjectSets(Row.Path)) { Out.push_back({ Row.Path, bSaver ? Row.Saver : Row.Max }); }
    }
    SectionCollect Collect{ bSaver ? "PowerProfile_Saver" : "PowerProfile_Max", &Out };
    ConfigForEachEntry("PowerProfile_", &CollectSection, &Collect);
    return Out;
}
}
}

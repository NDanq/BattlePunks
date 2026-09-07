/*
 * The hot-reload test fixture: a real module, built three times.
 *
 * WHY THREE BUILDS OF ONE FILE
 *
 * CTest runs one binary, so the smoke test cannot invoke a compiler to
 * "rebuild" a module mid-run. Instead the build produces three variants that
 * differ only in a preprocessor define, and the test copies one over another at
 * the watched path and ticks:
 *
 *   A      the initially loaded build
 *   B      the "rebuild" -- identical code, different EXAMPLE_MODULE_VARIANT_STRING
 *   STALE  forges a wrong ABI version, for the refusal and rollback tests
 *
 * WHY THIS LIVES UNDER Core/Tests/ RATHER THAN Engine/Modules/
 *
 * Modules/Editor and friends are placeholders for real engine modules; a test
 * fixture squatting one of those names is a name somebody has to take back
 * later. A top-level Modules/ExampleModule would only build under the
 * engine-level aggregate, which would make that aggregate mandatory for
 * something it should not be mandatory for -- `cmake -S Engine/Modules/Core` is
 * the cheapest way for CI to run this, and it has to keep covering the feature
 * end to end.
 *
 * WHAT IT PROVES, AND HOW
 *
 * Four independent observations, each of which fails loudly if the design is
 * wrong rather than degrading quietly:
 *
 *   GetVersionString()  changes across a reload -> different code is executing.
 *   StartupCount        survives a reload       -> the host-owned blob round-trips.
 *   CORE_LOG in OnStartup reaches a handler the EXE installed
 *                                               -> CORE_SHARED produced exactly
 *                                                  one GLogHandler, which is the
 *                                                  most important structural
 *                                                  claim in the whole design.
 *   TickCount           accumulates across a reload
 *                                               -> the blob carries more than
 *                                                  one field correctly.
 */

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreModule.h"

#include <cstring>

#ifndef EXAMPLE_MODULE_VARIANT_STRING
    #define EXAMPLE_MODULE_VARIANT_STRING "?"
#endif

namespace
{
    constexpr uint32_t EXAMPLE_STATE_VERSION = 1;

    /*
     * Trivially copyable, no pointers, size asserted. This is the shape every
     * module's reload payload should have -- see WHAT MAY GO IN THE BLOB in
     * CoreModule.h. The static_assert is the reminder that changing this struct
     * without bumping EXAMPLE_STATE_VERSION is a silent reinterpretation, which
     * is the one failure the (version, size) gate cannot catch on its own.
     */
    struct ExampleState
    {
        uint32_t Magic;          // a second, cheap corruption guard
        uint32_t StartupCount;   // incremented on every OnStartup, cold or not
        uint32_t TickCount;      // accumulates ACROSS reloads
        uint32_t LastVariant;    // first char of the variant that last wrote this
    };
    static_assert(sizeof(ExampleState) == 16, "bump EXAMPLE_STATE_VERSION when this changes");

    constexpr uint32_t EXAMPLE_STATE_MAGIC = 0x4558484Du;   // 'EXHM'

    /* The live instance's counters, published through a module-only export so
     * the test can observe them without a test-shaped virtual on IModule. */
    ExampleState GLiveState = {};
}

class ExampleHotModule : public EngineCore::IModule
{
public:
    const char* GetName() const override { return "ExampleHotModule"; }

    const char* GetVersionString() const override { return EXAMPLE_MODULE_VARIANT_STRING; }

    bool OnStartup(const EngineCore::ModuleReloadState* InPrev) override
    {
        ExampleState Restored = {};

        /*
         * InPrev is null on a cold start, non-null after a reload -- and after
         * a ROLLBACK, where the blob was written by this same build's own
         * OnShutdown moments ago. Both paths land here and both must work.
         *
         * A failed Read is not an error: it means the previous build wrote a
         * different layout, and a clean cold start is exactly the right answer.
         */
        if(InPrev && InPrev->Read(EXAMPLE_STATE_VERSION, &Restored, sizeof(Restored)))
        {
            if(Restored.Magic != EXAMPLE_STATE_MAGIC)
            {
                CORE_LOG_WARN("ExampleHotModule: reload blob failed its magic check; starting cold");
                Restored = ExampleState();
            }
        }

        Restored.Magic       = EXAMPLE_STATE_MAGIC;
        Restored.StartupCount += 1;
        GLiveState            = Restored;

        // If the executable's handler receives this, there is exactly one
        // GLogHandler in the process and CORE_SHARED did its job.
        CORE_LOG(std::string("ExampleHotModule ") + EXAMPLE_MODULE_VARIANT_STRING + " startup");
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& OutState) override
    {
        GLiveState.Magic       = EXAMPLE_STATE_MAGIC;
        GLiveState.LastVariant = uint32_t(EXAMPLE_MODULE_VARIANT_STRING[0]);

        OutState.Write(EXAMPLE_STATE_VERSION, &GLiveState, sizeof(GLiveState));
    }

    void OnTick(float /*InDeltaSeconds*/) override
    {
        GLiveState.TickCount += 1;
    }
};

/*
 * Module-only exports. Not part of the IModule contract -- they exist so the
 * test can read the LIVE instance's counters through
 * ModuleManager::GetModuleSymbol without adding a test-shaped virtual to the
 * interface every real module would then have to implement.
 */
extern "C" NIMBLE_MODULE_API uint32_t ExampleHotModuleGetStartupCount()
{
    return GLiveState.StartupCount;
}

extern "C" NIMBLE_MODULE_API uint32_t ExampleHotModuleGetTickCount()
{
    return GLiveState.TickCount;
}

extern "C" NIMBLE_MODULE_API uint32_t ExampleHotModuleGetLastVariant()
{
    return GLiveState.LastVariant;
}

#if defined(EXAMPLE_MODULE_FORCE_BAD_ABI)
/*
 * The forged stale plugin.
 *
 * It goes through the REAL macro rather than four hand-written extern "C"
 * bodies, so the forgery cannot drift away from the genuine export shape as
 * that shape evolves. The only lie is the version integer, which is exactly
 * the lie the host's load gate exists to catch.
 */
NIMBLE_IMPLEMENT_MODULE_VERSIONED(ExampleHotModule, "ExampleHotModule",
                                  ::EngineCore::NIMBLE_MODULE_ABI_VERSION ^ 0xFFu)
#else
NIMBLE_IMPLEMENT_MODULE(ExampleHotModule, "ExampleHotModule")
#endif

/*
 * RenderCoreSmokeTest -- links RenderCore and NOTHING else (see
 * CMakeLists.txt's comment: it must NOT link GDI::GDI, which is the
 * structural check that no GDI header has leaked into Public/).
 *
 * Step 1 (skeleton) only. The real cases -- thread lifecycle, command
 * ordering, markers, proxy lifecycle through a World, a test-defined proxy
 * type, teardown/leaks -- arrive with the build-order steps that implement
 * the surface they exercise; see DESIGN.md's "Build order" and this module's
 * own "Smoke test" section for the full list. Written in GameCoreSmokeTest's
 * Expect()-counter style so the later cases drop in without a rewrite.
 */

#include <cstdio>
#include <cstring>

#include "Public/RenderCore/RenderCoreVersion.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

int main()
{
    std::printf("RenderCore smoke test\n");

    Expect(std::strcmp(RenderCore::GetModuleName(), "RenderCore") == 0,
           "GetModuleName() names this module, proving the skeleton actually links against RenderCore.dll");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}

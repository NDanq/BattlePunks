#ifndef LIGHTLAB_GPUTEST_H
#define LIGHTLAB_GPUTEST_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }
namespace NimbleEngine { class GameSession; struct FrameContext; }

/*
 * The GPU test world (Content/Levels/GpuTest.nmworld, Tools/make_gpu_test_level.py):
 * the herd that walks its courtyards, and the benchmark that flies through it.
 */

/* LightLab.Herd: every HerdFox prefab instance walks a small loop round where
 * it was placed -- walk, run, stop, sit, again -- each on its own clock and
 * its own way round, so a crowd is never in step. */
struct LightLabHerdContext
{
    struct Member
    {
        GameCore::EntityID  Root;            // the instance's root: moved
        GameCore::EntityID  Body;            // its Animation: driven
        EngineCore::Vector3 Center;
        double              Radius   = 110.0;
        double              Angle    = 0.0;
        double              Turn     = 1.0;  // +1 anticlockwise, -1 clockwise
        double              Speed    = 0.0;
        int                 Phase    = 0;    // 0 walk, 1 run, 2 stopping, 3 sitting
        double              PhaseSeconds = 0.0;
    };
    std::vector<Member> Members;
};

/* The world's HerdFox instances into InOut, after making their count
 * InCount (-1: as placed) -- the extra spawned round the courtyards, the
 * surplus destroyed. False if there are none. */
bool LightLabFindHerd(GameCore::World& InWorld, int32_t InCount, LightLabHerdContext& InOut);

/* Every Animation component paused where it starts: the same poses every
 * run, so two benchmark runs' frames compare pixel for pixel. */
void LightLabFreezeAnimation(GameCore::World& InWorld);
void LightLabHerdSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

/*
 * [LightLab] Benchmark=<file.csv>: instead of flying, the Player follows a
 * fixed path -- up the main avenue, along the cross street, round a
 * courtyard, then over the roofs -- a frame at a time (the same views
 * whatever the frame rate), and each frame's timings and counts are written
 * to the file: RenderStats ("stat unit", "stat frame": half-second
 * averages), the occlusion and draw counts, and the animation tick. The run
 * quits at the end of the path. Tools/gpu_test_perf.py runs and reads it.
 */
class LightLabBenchmark
{
public:
    ~LightLabBenchmark() { Close(); }

    /* Opens InCsvPath; false (logged) if it cannot. InShotsDir (an editor
     * build; "" none): the middle frame of each leg saved there as
     * <leg>.ppm, read back from the scene (Renderer::RequestSceneCapture). */
    bool Begin(GameCore::EntityID InPlayer, const std::string& InCsvPath, const std::string& InShotsDir = std::string());
    bool IsActive() const { return File != nullptr; }

    /* PrePhysics system: places the Player for this frame. */
    static void System(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

    /* The game mode's OnTick: the last frame's row; quits when the path is done. */
    void Record(NimbleEngine::GameSession& InSession, const NimbleEngine::FrameContext& InFrame);

private:
    void Close();

    GameCore::EntityID Player;
    std::FILE*         File      = nullptr;
    uint64_t           Frame     = 0;    // path frames placed
    uint64_t           LastDrawCalls = 0;
    bool               bQuit     = false;
    std::string        ShotsDir;
    uint64_t           ShotRequest = 0;   // a capture on its way; 0: none
    std::string        ShotName;
};

#endif // LIGHTLAB_GPUTEST_H

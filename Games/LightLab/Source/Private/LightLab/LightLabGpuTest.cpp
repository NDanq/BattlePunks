#include "Private/LightLab/LightLabGpuTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/Renderer.h"

namespace
{
    constexpr double Pi = 3.14159265358979323846;

    const char* const HerdFoxPrefab = "Content/Prefabs/HerdFox.nmprefab";

    /* The town's grid (make_gpu_test_level.py): 4 x 4 blocks, 2200 apart. */
    constexpr int    TownBlocks = 4;
    constexpr double TownPitch  = 2200.0;

    double Approach(const double InFrom, const double InTo, const double InStep)
    {
        return InFrom < InTo ? std::min(InFrom + InStep, InTo) : std::max(InFrom - InStep, InTo);
    }

    /* A cheap, repeatable 0..1 from an integer. */
    double Hash01(uint32_t InValue)
    {
        InValue ^= InValue >> 16; InValue *= 0x7feb352dU;
        InValue ^= InValue >> 15; InValue *= 0x846ca68bU;
        InValue ^= InValue >> 16;
        return double(InValue) / double(0xFFFFFFFFU);
    }

    struct HerdGather
    {
        GameCore::World*                   World = nullptr;
        std::vector<GameCore::PrefabInstanceID> Instances;
    };
}

bool LightLabFindHerd(GameCore::World& InWorld, const int32_t InCount, LightLabHerdContext& InOut)
{
    InOut = LightLabHerdContext();
    const auto Gather = [&InWorld]()
    {
        HerdGather Found;
        Found.World = &InWorld;
        InWorld.ForEachInstance([](const GameCore::PrefabInstanceID InInstance, void* InUserData)
        {
            auto& Into = *static_cast<HerdGather*>(InUserData);
            const char* Path = Into.World->GetInstance(InInstance).GetPrefabPath();
            if(Path && std::strcmp(Path, HerdFoxPrefab) == 0) { Into.Instances.push_back(InInstance); }
        }, &Found);
        return Found.Instances;
    };
    std::vector<GameCore::PrefabInstanceID> Herd = Gather();

    /* As many as asked: the surplus gone, the rest round the courtyards. */
    if(InCount >= 0 && size_t(InCount) < Herd.size())
    {
        for(size_t Index = size_t(InCount); Index < Herd.size(); ++Index) { InWorld.DestroyPrefabInstance(Herd[Index]); }
        Herd.resize(size_t(InCount));
    }
    else if(InCount >= 0 && size_t(InCount) > Herd.size())
    {
        const double Half = (TownBlocks - 1) / 2.0;
        for(uint32_t Extra = 0; Herd.size() < size_t(InCount); ++Extra)
        {
            const uint32_t Block = Extra % uint32_t(TownBlocks * TownBlocks);
            const double   Ring  = 160.0 + 70.0 * double((Extra / (TownBlocks * TownBlocks)) % 5);
            const double   Angle = 2.0 * Pi * Hash01(Extra * 2654435761U);
            GameCore::TransformPose At;
            At.Position = EngineCore::Vector3((double(Block % TownBlocks) - Half) * TownPitch + Ring * std::cos(Angle), 0.0,
                                              (double(Block / TownBlocks) - Half) * TownPitch + Ring * std::sin(Angle));
            At.Rotation = EngineCore::Vector3(0.0, 360.0 * Hash01(Extra + 7U), 0.0);
            const std::string Name = "HerdFox_Extra" + std::to_string(Extra);
            const GameCore::PrefabInstance Spawned = InWorld.SpawnPrefab(HerdFoxPrefab, At, Name.c_str());
            if(!Spawned.IsValid())
            {
                CORE_LOG_WARN(std::string("[LightLab] could not spawn ") + HerdFoxPrefab)
                break;
            }
            Herd.push_back(Spawned.GetID());
        }
    }

    for(size_t Index = 0; Index < Herd.size(); ++Index)
    {
        LightLabHerdContext::Member Each;
        const GameCore::PrefabInstance Instance = InWorld.GetInstance(Herd[Index]);
        Each.Root = Instance.GetRoot().GetID();
        Each.Body = Instance.FindEntity("Body").GetID();
        const GameCore::TransformComponent* Placed = InWorld.GetEntity(Each.Root).GetComponent<GameCore::TransformComponent>();
        if(!Placed || !InWorld.GetEntity(Each.Body).GetComponent<RenderCore::AnimationComponent>())
        {
            continue;
        }
        /* Its loop starts where it stands: the loop's east end. */
        Each.Center       = Placed->Position - EngineCore::Vector3(Each.Radius, 0.0, 0.0);
        Each.Turn         = Hash01(uint32_t(Index) * 977U) < 0.5 ? -1.0 : 1.0;
        Each.Phase        = int(Hash01(uint32_t(Index) * 31U + 5U) * 4.0) % 4;
        Each.PhaseSeconds = 6.0 * Hash01(uint32_t(Index) * 131U + 3U);
        InOut.Members.push_back(Each);
    }
    return !InOut.Members.empty();
}

void LightLabFreezeAnimation(GameCore::World& InWorld)
{
    InWorld.Each<RenderCore::AnimationComponent>([](const GameCore::Entity&, RenderCore::AnimationComponent& InComponent)
    {
        InComponent.bPaused = true;
    });
}

/* LightLab.Animals' fox, for every member: walk 6 s, run 6 s, stop, sit 4 s. */
void LightLabHerdSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
{
    auto& Context = *static_cast<LightLabHerdContext*>(InUserData);
    const double Delta = double(InDeltaSeconds);
    for(LightLabHerdContext::Member& Each : Context.Members)
    {
        GameCore::TransformComponent*   Transform = InWorld.GetEntity(Each.Root).GetComponent<GameCore::TransformComponent>();
        RenderCore::AnimationComponent* Animation = InWorld.GetEntity(Each.Body).GetComponent<RenderCore::AnimationComponent>();
        if(!Transform || !Animation)
        {
            continue;
        }
        Each.PhaseSeconds += Delta;
        double Target = 0.0;
        switch(Each.Phase)
        {
            case 0: Target = 65.0;  if(Each.PhaseSeconds > 6.0) { Each.Phase = 1; Each.PhaseSeconds = 0.0; } break;
            case 1: Target = 160.0; if(Each.PhaseSeconds > 6.0) { Each.Phase = 2; Each.PhaseSeconds = 0.0; } break;
            case 2:
                if(Each.Speed <= 0.0)
                {
                    Animation->SetTrigger("Sit");
                    Each.Phase = 3;
                    Each.PhaseSeconds = 0.0;
                }
                break;
            default: if(Each.PhaseSeconds > 4.0) { Each.Phase = 0; Each.PhaseSeconds = 0.0; } break;
        }
        Each.Speed  = Approach(Each.Speed, Target, 90.0 * Delta);
        Each.Angle += Each.Turn * Each.Speed * Delta / Each.Radius;

        const double CosA = std::cos(Each.Angle), SinA = std::sin(Each.Angle);
        Transform->Position = Each.Center + EngineCore::Vector3(Each.Radius * CosA, 0.0, Each.Radius * SinA);
        /* Facing along the loop: its heading's yaw (the Body turns the clips round). */
        const double HeadingX = -SinA * Each.Turn, HeadingZ = CosA * Each.Turn;
        Transform->Rotation.SetY(float(std::atan2(HeadingX, HeadingZ) * 180.0 / Pi));
        Animation->SetFloat("Speed", float(Each.Speed));
    }
}

// === The benchmark =========================================================

namespace
{
    /* One leg of the path: Frames frames, the camera at Place(t) for t 0..1. */
    struct PathLeg
    {
        const char* Name;
        uint32_t    Frames;
        void      (*Place)(double InT, EngineCore::Vector3& OutPosition, double& OutPitch, double& OutYaw);
    };

    EngineCore::Vector3 Lerp(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const double InT)
    {
        return InA + (InB - InA) * InT;
    }

    /* The town's south square is at z -5000 and the Player starts 700 behind it. */
    const PathLeg Path[] = {
        /* Not recorded: shaders, pipelines and the first frames' allocations. */
        { "Warmup", 240, [](double, EngineCore::Vector3& P, double& Pitch, double& Yaw)
            { P = EngineCore::Vector3(0.0, 180.0, -5600.0); Pitch = 3.0; Yaw = 0.0; } },
        /* Street level up the main avenue: the blocks' walls hide their courtyards. */
        { "Avenue", 900, [](double T, EngineCore::Vector3& P, double& Pitch, double& Yaw)
            { P = Lerp(EngineCore::Vector3(0.0, 180.0, -5600.0), EngineCore::Vector3(0.0, 180.0, 4200.0), T); Pitch = 3.0; Yaw = 0.0; } },
        /* Across the town along the middle street, looking along it. */
        { "CrossStreet", 600, [](double T, EngineCore::Vector3& P, double& Pitch, double& Yaw)
            { P = Lerp(EngineCore::Vector3(-4600.0, 180.0, 0.0), EngineCore::Vector3(4600.0, 180.0, 0.0), T); Pitch = 3.0; Yaw = 90.0; } },
        /* Round inside one courtyard (block 2,1), looking at its herd. */
        { "Courtyard", 600, [](double T, EngineCore::Vector3& P, double& Pitch, double& Yaw)
            {
                const double Angle = 2.0 * Pi * T;
                P = EngineCore::Vector3(1100.0 + 380.0 * std::cos(Angle), 260.0, -1100.0 + 380.0 * std::sin(Angle));
                /* Toward the courtyard's middle. */
                Yaw = std::atan2(-std::cos(Angle), -std::sin(Angle)) * 180.0 / Pi; Pitch = 18.0;
            } },
        /* High over the roofs: almost nothing is hidden. */
        { "Overview", 600, [](double T, EngineCore::Vector3& P, double& Pitch, double& Yaw)
            { P = Lerp(EngineCore::Vector3(-1500.0, 4200.0, -7500.0), EngineCore::Vector3(1500.0, 4200.0, -2500.0), T); Pitch = 38.0; Yaw = 0.0; } },
    };

    /* The leg frame InFrame falls in, and how far along it; false past the end. */
    bool FindLeg(uint64_t InFrame, const PathLeg*& OutLeg, double& OutT)
    {
        for(const PathLeg& Leg : Path)
        {
            if(InFrame < Leg.Frames)
            {
                OutLeg = &Leg;
                OutT   = Leg.Frames > 1 ? double(InFrame) / double(Leg.Frames - 1) : 0.0;
                return true;
            }
            InFrame -= Leg.Frames;
        }
        return false;
    }
}

bool LightLabBenchmark::Begin(const GameCore::EntityID InPlayer, const std::string& InCsvPath, const std::string& InShotsDir)
{
    Close();
    Player   = InPlayer;
    Frame    = 0;
    bQuit    = false;
    ShotsDir = InShotsDir;
    ShotRequest = 0;
    File   = std::fopen(InCsvPath.c_str(), "w");
    if(!File)
    {
        CORE_LOG_ERROR("[LightLab] benchmark: cannot write " + InCsvPath)
        return false;
    }
    std::fprintf(File, "frame,leg,wall_s,dt_ms,frame_ms,game_ms,render_ms,"
                       "gpu_prepass,gpu_basepass,gpu_ao,gpu_shadows,gpu_lighting,gpu_forward,gpu_post,gpu_total,gpu_capture,gpu_particles,"
                       "draw_calls,proxies,culled,occluded,queries,static_drawn,"
                       "anim_ms,anim_update_ms,anim_updated,anim_reduced,anim_skipped,anim_frozen,anim_offscreen,anim_parallel,"
                       "wait_ms,queue_ms,"
                       "cpu_prepass,cpu_basepass,cpu_ao,cpu_shadows,cpu_lighting,cpu_forward,cpu_post,cpu_total,cpu_capture,cpu_particles,"
                       "draws_prepass,draws_basepass,draws_ao,draws_shadows,draws_lighting,draws_forward,draws_post,draws_total,"
                       "draws_capture,draws_particles\n");
    CORE_LOG("[LightLab] benchmark: recording to " + InCsvPath)
    return true;
}

void LightLabBenchmark::Close()
{
    if(File) { std::fclose(File); }
    File = nullptr;
}

void LightLabBenchmark::System(GameCore::World& InWorld, const float /*InDeltaSeconds*/, void* InUserData)
{
    auto& Self = *static_cast<LightLabBenchmark*>(InUserData);
    const PathLeg* Leg = nullptr;
    double         T   = 0.0;
    if(!FindLeg(Self.Frame, Leg, T))
    {
        Self.bQuit = true;
        return;
    }
    EngineCore::Vector3 Position;
    double Pitch = 0.0, Yaw = 0.0;
    Leg->Place(T, Position, Pitch, Yaw);
    if(GameCore::TransformComponent* Transform = InWorld.GetEntity(Self.Player).GetComponent<GameCore::TransformComponent>())
    {
        Transform->Position = Position;
        Transform->Rotation = EngineCore::Vector3(Pitch, Yaw, 0.0);
    }
    ++Self.Frame;
}

void LightLabBenchmark::Record(NimbleEngine::GameSession& InSession, const NimbleEngine::FrameContext& InFrame)
{
    if(!File)
    {
        return;
    }
    /* A shot on its way: written when it is in (a frame or two). */
#if WITH_EDITOR
    if(ShotRequest != 0)
    {
        EngineCore::Image Pixels;
        std::string       Error;
        if(InSession.GetRenderer().TakeMaterialPreview(ShotRequest, Pixels, Error))
        {
            ShotRequest = 0;
            const std::string Path = ShotsDir + "/" + ShotName + ".ppm";
            if(std::FILE* Shot = Pixels.GetWidth() > 0 ? std::fopen(Path.c_str(), "wb") : nullptr)
            {
                std::fprintf(Shot, "P6\n%d %d\n255\n", int(Pixels.GetWidth()), int(Pixels.GetHeight()));
                for(int Y = 0; Y < int(Pixels.GetHeight()); ++Y)
                {
                    const uint8_t* Row = Pixels.GetData() + size_t(Pixels.GetStride()) * size_t(Y);
                    for(int X = 0; X < int(Pixels.GetWidth()); ++X) { std::fwrite(Row + X * 4, 1, 3, Shot); }
                }
                std::fclose(Shot);
                CORE_LOG("[LightLab] benchmark: saved " + Path)
            }
            else
            {
                CORE_LOG_WARN("[LightLab] benchmark: no shot for " + ShotName + (Error.empty() ? std::string() : ": " + Error))
            }
        }
        return;   // the frames round a shot stall: not recorded
    }
#endif
    if(bQuit)
    {
        CORE_LOG("[LightLab] benchmark: done after " + std::to_string(Frame) + " frames")
        Close();
        InSession.RequestQuit();
        return;
    }

    /* The frame placed last tick, now drawn (OnTick runs before World::Tick). */
    const PathLeg* Leg = nullptr;
    double         T   = 0.0;
    if(Frame == 0 || !FindLeg(Frame - 1, Leg, T))
    {
        return;
    }
#if WITH_EDITOR
    /* The middle frame of each recorded leg, when shots are asked for. */
    if(!ShotsDir.empty() && std::strcmp(Leg->Name, "Warmup") != 0 && T >= 0.5 && T - 1.0 / double(Leg->Frames) < 0.5)
    {
        ShotName    = Leg->Name;
        ShotRequest = InSession.GetRenderer().RequestSceneCapture();
    }
#endif
    RenderCore::RenderStats Stats;
    InSession.GetRenderer().GetStats(Stats);
    const uint64_t DrawCalls = Stats.DrawCalls - std::min(LastDrawCalls, Stats.DrawCalls);
    LastDrawCalls = Stats.DrawCalls;

    RenderCore::AnimationSystem::Stats Anim;
    if(const RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&InSession.GetWorld()))
    {
        Anim = Animation->GetLastStats();
    }

    /* Seconds since the Unix epoch: lines a row up with what else was sampled then (the GPU's clock). */
    const double Wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(File, "%llu,%s,%.3f,%.4f,%.4f,%.4f,%.4f,", (unsigned long long)(Frame - 1), Leg->Name, Wall, InFrame.DeltaSeconds * 1000.0,
                 Stats.FrameMs, Stats.GameThreadMs, Stats.RenderThreadMs);
    for(uint32_t Timer = 0; Timer < RenderCore::RGT_Count; ++Timer) { std::fprintf(File, "%.4f,", Stats.GpuMs[Timer]); }
    std::fprintf(File, "%llu,%llu,%llu,%llu,%llu,%llu,", (unsigned long long)DrawCalls, (unsigned long long)Stats.ProxiesLive,
                 (unsigned long long)Stats.ProxiesCulledLastFrame, (unsigned long long)Stats.ProxiesOccludedLastFrame,
                 (unsigned long long)Stats.OcclusionQueriesLastFrame, (unsigned long long)Stats.StaticProxiesDrawn);
    std::fprintf(File, "%.4f,%.4f,%u,%u,%u,%u,%u,%d,", Anim.Milliseconds, Anim.UpdateMilliseconds, Anim.Updated, Anim.Reduced,
                 Anim.Skipped, Anim.Frozen, Anim.Offscreen, Anim.bParallel ? 1 : 0);
    /* The render thread's side (MULTITHREADED_RENDERING.md, phase 0): its wait,
     * queueing, and per-stage time and draws. */
    std::fprintf(File, "%.4f,%.4f,", Stats.WaitMs, Stats.QueueCpuMs);
    for(uint32_t Timer = 0; Timer < RenderCore::RGT_Count; ++Timer) { std::fprintf(File, "%.4f,", Stats.CpuMs[Timer]); }
    for(uint32_t Timer = 0; Timer < RenderCore::RGT_Count; ++Timer)
    {
        std::fprintf(File, Timer + 1 < RenderCore::RGT_Count ? "%d," : "%d\n", int(Stats.StageDraws[Timer]));
    }
}

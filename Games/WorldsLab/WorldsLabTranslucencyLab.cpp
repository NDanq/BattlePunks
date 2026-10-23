#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

/*
 * TranslucencyLab (Engine/Modules/RenderCore/LIT_TRANSLUCENCY.md; the level from
 * Tools/TranslucencyLab/make_translucencylab.py): the camera held at one of four
 * fixed views, so a phase's captures compare frame to frame. With [Rendering]
 * FixedStep the particles are the same frame each run too.
 */

/* 0 the whole lab, 1 the glass, 2 the plume, 3 the water. */
NIMBLE_CONFIG_INT(GTranslucencyLabView, "WorldsLab", "TranslucencyLabView", 0, nullptr);
/* > 0, with -Stats.Frame=1: at that frame, log the scene's GPU time per span once. */
NIMBLE_CONFIG_INT(GTranslucencyLabStats, "WorldsLab", "TranslucencyLabStats", 0, nullptr);
/* How many times to log them, 120 frames apart (the perf report's samples: Tools/TranslucencyLab/perf.ps1). */
NIMBLE_CONFIG_INT(GTranslucencyLabStatsCount, "WorldsLab", "TranslucencyLabStatsCount", 1, nullptr);
/* 1: the plume's smoke lit per particle (SmokePerParticle) rather than per pixel (Smoke, Volume). */
NIMBLE_CONFIG_BOOL(GTranslucencyLabPerParticle, "WorldsLab", "TranslucencyLabPerParticle", false, nullptr);

using EngineCore::Vector3;

namespace
{
    struct LabView { const char* Name; Vector3 Eye; Vector3 Look; };
    const LabView VIEWS[] = {
        { "lab",   Vector3(400.0, 650.0, -2300.0),  Vector3(400.0, 150.0, 200.0) },
        { "glass", Vector3(-480.0, 220.0, -900.0),  Vector3(-480.0, 170.0, 100.0) },
        { "plume", Vector3(1500.0, 360.0, -750.0),  Vector3(1500.0, 330.0, 250.0) },
        { "water", Vector3(500.0, 330.0, -800.0),   Vector3(500.0, 60.0, 150.0) },
    };
    const char* const SPAN_NAMES[] = { "PrePass", "BasePass", "AO", "Shadows", "Lighting", "Forward", "Post", "Total",
                                       "Captures", "Particles" };
}

class WorldsLabTranslucencyLabMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Owner = &InSession;
        GameCore::World& World = InSession.GetWorld();
        const GameCore::EntityID Camera = World.FindEntityByName("Camera");
        GameCore::TransformComponent* View = Camera.IsSet() ? World.GetEntity(Camera).GetComponent<GameCore::TransformComponent>() : nullptr;
        if(!View)
        {
            CORE_LOG_ERROR("[TranslucencyLab] the world has no entity named Camera; the view stays where it is")
            return true;
        }
        const int Index = std::clamp(GTranslucencyLabView.GetInt(), 0, int(std::size(VIEWS)) - 1);
        const LabView& Pick = VIEWS[Index];
        const Vector3 Dir = Pick.Look - Pick.Eye;
        const double Flat = std::sqrt(double(Dir.GetX()) * Dir.GetX() + double(Dir.GetZ()) * Dir.GetZ());
        View->Position = Pick.Eye;
        View->Rotation = Vector3(std::atan2(-double(Dir.GetY()), std::max(Flat, 1.0)) * 180.0 / 3.14159265358979,
                                 std::atan2(double(Dir.GetX()), double(Dir.GetZ())) * 180.0 / 3.14159265358979, 0.0);
        CORE_LOG(std::string("[TranslucencyLab] view ") + std::to_string(Index) + " (" + Pick.Name
                 + "); WorldsLab.TranslucencyLabView 0-3 picks another")

        /* The plume lit per particle instead of per pixel (LIT_TRANSLUCENCY.md, Phase 6). */
        if(GTranslucencyLabPerParticle.GetBool())
        {
            const GameCore::EntityID Plume = World.FindEntityByName("Plume");
            if(RenderCore::ParticleSystemComponent* System = Plume.IsSet()
                   ? World.GetEntity(Plume).GetComponent<RenderCore::ParticleSystemComponent>() : nullptr)
            {
                std::snprintf(System->System, sizeof(System->System), "%s", "Content/Particles/TranslucencyLab/PlumePerParticle.nmparticle");
                CORE_LOG("[TranslucencyLab] the plume is lit per particle")
            }
        }
        return true;
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        (void)InFrame;
        const int At    = GTranslucencyLabStats.GetInt();
        const int Count = std::max(GTranslucencyLabStatsCount.GetInt(), 1);
        ++Frames;
        if(At <= 0 || !Owner || Frames < At || (Frames - At) % 120 != 0 || (Frames - At) / 120 >= Count) { return; }
        RenderCore::RenderStats Render;
        Owner->GetRenderer().GetStats(Render);
        std::string Line = "[TranslucencyLab] GPU ms at frame " + std::to_string(Frames) + ":";
        for(int Span = 0; Span < int(std::size(SPAN_NAMES)) && Span < int(RenderCore::RGT_Count); ++Span)
        {
            char Each[48];
            std::snprintf(Each, sizeof(Each), " %s %.3f", SPAN_NAMES[Span], double(Render.GpuMs[Span]));
            Line += Each;
        }
        CORE_LOG(Line)
    }

private:
    NimbleEngine::GameSession* Owner  = nullptr;
    int                        Frames = 0;
};

NIMBLE_REGISTER_GAME_MODE(WorldsLabTranslucencyLabMode, "WorldsLabTranslucencyLab")

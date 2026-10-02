#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/Renderer.h"
#include "SandboxScripting.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>
#include <string>

/*
 * SandboxGame: the example game, built as a game module DLL loaded by name by
 * SandboxMain.cpp's launcher and the editor's Play-in-Editor.
 *
 * Demonstrates: a C++ entity ("Marker") moved by the mode and wrapped by a
 * PostPhysics system; a Lua entity ("Scripted") read back from C++; an
 * OnEntityCreated bind/unbind; drawing via RenderComponent and a Camera
 * entity; and a timed save/load cycle. The game's own component and script
 * library -- what scripts and script graphs can call -- are in
 * SandboxScripting.cpp.
 *
 *   SandboxLevel      spawns Marker and Camera, registers the wrap system,
 *                     owns the mode and state.
 *   SandboxGameMode   moves Marker, reports, decides when to quit.
 *   SandboxGameState  elapsed time and tick counts.
 *   SandboxGameModule owns and registers the level.
 */

/* A game-owned config var, in the same registry and .ini as the engine's.
 * Unlike [Application] MaxFrames, this is game logic requesting quit. */
NIMBLE_CONFIG_INT(GSandboxRunSeconds, "Sandbox", "RunSeconds", 0, nullptr);

/* Beside the executable for the demo; a real game would use appGetAppDataDir. */
static const char* const GSandboxSavePath = "Saves/Quick.nmsg";

/* Save/load demo flags. File scope: a load rebuilds the mode and resets the
 * state, so flags there would loop the demo forever. */
static bool GSandboxHasSaved  = false;
static bool GSandboxHasLoaded = false;

/* What Sandbox.WrapMarker's void* points at. */
struct SandboxMarkerSystemContext
{
    GameCore::EntityID Entity;
    int                ViewportWidth = 0;
};

namespace
{
    std::string Num(uint64_t InValue)   { return std::to_string(InValue); }

    /* Two decimals, without dragging <iomanip> in for one call site. */
    std::string Fixed2(double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.2f", InValue);
        return std::string(Buffer);
    }

    /* PostPhysics system: wraps Marker's X at the viewport width. State comes
     * through InUserData (SandboxMarkerSystemContext). */
    void WrapMarkerSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        const auto& Context = *static_cast<const SandboxMarkerSystemContext*>(InUserData);

        if(GameCore::TransformComponent* Transform = InWorld.GetEntity(Context.Entity).GetComponent<GameCore::TransformComponent>())
        {
            const double X = Transform->Position.GetX();
            if(Context.ViewportWidth > 0 && X >= double(Context.ViewportWidth))
            {
                Transform->Position.SetX(X - double(Context.ViewportWidth));
            }
        }
    }

    /* OnEntityCreated subscriber, bound in SandboxLevel::OnLoad and unbound in
     * OnUnload. Logs entities from both C++ and Scripts/Main.lua. */
    void OnEntityCreatedLogger(GameCore::EntityID InEntity, void* InUserData)
    {
        auto* World = static_cast<GameCore::World*>(InUserData);
        CORE_LOG("[Sandbox] OnEntityCreated: " + std::string(World->GetEntity(InEntity).GetName()))
    }
}

/* Owned by SandboxLevel, used by SandboxGameMode. OnReset zeroes everything. */
class SandboxGameState : public NimbleEngine::GameState
{
public:
    void OnReset() override
    {
        ElapsedSeconds    = 0.0;
        LastReportSeconds = 0.0;
        TicksThisSecond   = 0;
        SandboxScripting::SetScore(0);
    }

    /* Saves exactly the fields OnReset touches; keep the two in step. New
     * fields go at the end behind Ar.GetChunkVersion(). */
    uint16_t GetSerialVersion() const override { return 2; }

    void Serialize(EngineCore::Archive& Ar) override
    {
        Ar << ElapsedSeconds;
        Ar << LastReportSeconds;
        Ar << TicksThisSecond;

        /* 2: the script library's Score, so a restored pickup is not scored
         * a second time. */
        if(Ar.GetChunkVersion() >= 2)
        {
            int32_t Score = SandboxScripting::GetScore();
            Ar << Score;
            SandboxScripting::SetScore(Score);
        }
    }

    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    uint64_t TicksThisSecond   = 0;
};

/* The mode calls back into the level in OnPostLoadSave. */
class SandboxLevel;

class SandboxGameMode : public NimbleEngine::IGameMode
{
public:
    SandboxGameMode(SandboxLevel& InLevel, GameCore::EntityID InMarker, SandboxGameState& InState)
        : OwningLevel(&InLevel), MarkerEntity(InMarker), State(&InState) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;

        /* So OnTick can assert it runs on the game thread. */
        GameThread = EngineCore::appThreadID();

        RunSeconds = double(GSandboxRunSeconds.GetInt());

        /* Nothing spawned here: the level builds the scene, the mode is rules. */
        return true;
    }

    /* The startup script has run, so "Scripted" exists now (not in OnLoad). */
    void OnStart() override
    {
        const uint32_t Limit = Session->GetHost().GetFrameRateLimit();

        const GameCore::EntityID Scripted = Session->GetWorld().FindEntityByName("Scripted");

        CORE_LOG("[Sandbox] game starting on thread " + Num(uint64_t(GameThread))
                 + "; frame limit "
                 + (Limit > 0 ? Num(Limit) + " fps" : std::string("unlimited"))
                 + (RunSeconds > 0.0
                    ? "; will quit after " + Fixed2(RunSeconds) + "s"
                    : "; running until the window closes")
                 + (Scripted.IsSet()
                    ? "; the startup script's entity is present"
                    : "; no scripted entity (no startup script ran)"))
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        /* Concurrent with the render thread: no renderer or window access. */
        CORE_CHECK(EngineCore::appThreadID() == GameThread,
                   "[Sandbox] OnTick ran on a thread other than the one OnStartup saw")

        State->ElapsedSeconds += InFrame.DeltaSeconds;
        SandboxScripting::SetElapsedSeconds(State->ElapsedSeconds);   // Sandbox.GetElapsed() in scripts
        State->TicksThisSecond++;

        GameCore::World& World = Session->GetWorld();
        if(GameCore::TransformComponent* Marker = World.GetEntity(MarkerEntity).GetComponent<GameCore::TransformComponent>())
        {
            const double SpeedPixelsPerSecond = 120.0;
            Marker->Position.SetX(Marker->Position.GetX() + SpeedPixelsPerSecond * InFrame.DeltaSeconds);
            /* Sandbox.WrapMarker wraps it later this frame, in World::Tick. */
        }

        /* The engine finishes this frame, then exits the loop. */
        if(RunSeconds > 0.0 && State->ElapsedSeconds >= RunSeconds && Session)
        {
            CORE_LOG("[Sandbox] RunSeconds reached; asking the engine to quit")
            Session->RequestQuit();
        }

        /* Save/load demo. Loading two seconds after the save makes Marker
         * visibly jump back. */
        if(!GSandboxHasSaved && State->ElapsedSeconds >= 2.0)
        {
            GSandboxHasSaved = true;
            SavedMarkerX = 0.0;
            if(const GameCore::TransformComponent* Marker =
                   World.GetEntity(MarkerEntity).GetComponent<GameCore::TransformComponent>())
            {
                SavedMarkerX = Marker->Position.GetX();
            }

            CORE_LOG("[Sandbox] saving at t=" + Fixed2(State->ElapsedSeconds)
                     + "s marker_x=" + Fixed2(SavedMarkerX))

            if(!NimbleEngine::SaveGameToFile(*Session, GSandboxSavePath))
            {
                CORE_LOG_ERROR("[Sandbox] the save failed")
            }
        }

        if(GSandboxHasSaved && !GSandboxHasLoaded && State->ElapsedSeconds >= 4.0)
        {
            GSandboxHasLoaded = true;

            CORE_LOG("[Sandbox] loading at t=" + Fixed2(State->ElapsedSeconds)
                     + "s (expect elapsed and marker_x to jump back to the saved values)")

            /* Deferred to the next frame, so safe from inside OnTick. */
            if(!NimbleEngine::LoadGameFromFile(*Session, GSandboxSavePath))
            {
                CORE_LOG_ERROR("[Sandbox] the load failed")
            }
        }

        if(State->ElapsedSeconds - State->LastReportSeconds >= 1.0)
        {
            const GameCore::TransformComponent* MarkerTransform =
                World.GetEntity(MarkerEntity).GetComponent<GameCore::TransformComponent>();
            const double MarkerX = MarkerTransform ? MarkerTransform->Position.GetX() : 0.0;

            /* Created by Scripts/Main.lua; read purely through the World. */
            const GameCore::EntityID ScriptedEntity = World.FindEntityByName("Scripted");
            std::string ScriptedPart;
            if(GameCore::TransformComponent* Scripted = World.GetEntity(ScriptedEntity).GetComponent<GameCore::TransformComponent>())
            {
                ScriptedPart = " scripted_x=" + Fixed2(Scripted->Position.GetX());
            }

            RenderCore::RenderStats RenderStats;
            Session->GetRenderer().GetStats(RenderStats);

            CORE_LOG("[Sandbox] t=" + Fixed2(State->ElapsedSeconds) + "s"
                     + " frame=" + Num(InFrame.FrameNumber)
                     + " fps=" + Num(State->TicksThisSecond)
                     + " dt=" + Fixed2(InFrame.DeltaSeconds * 1000.0) + "ms"
                     + " viewport=" + Num(uint64_t(InFrame.ViewportPixels.Width))
                     + "x" + Num(uint64_t(InFrame.ViewportPixels.Height))
                     + " marker_x=" + Fixed2(MarkerX)
                     + ScriptedPart
                     + " score=" + std::to_string(SandboxScripting::GetScore())
                     + " frames_rendered=" + Num(RenderStats.FramesRendered)
                     + " draw_calls=" + Num(RenderStats.DrawCalls))

            State->LastReportSeconds = State->ElapsedSeconds;
            State->TicksThisSecond   = 0;
        }
    }

    /* Re-derives MarkerEntity by name: the restored Marker is a new entity. */
    void OnPostLoadSave() override;

    void OnUnload() override
    {
        /* Nothing to unbind: the level owns the delegate binding. */
        CORE_LOG("[Sandbox] game stopping after " + Fixed2(State->ElapsedSeconds) + "s")
        Session = nullptr;
    }

private:
    /* Stashed in OnLoad for RequestQuit() and GetWorld(). */
    NimbleEngine::GameSession* Session = nullptr;

    EngineCore::ThreadID GameThread = 0;

    double RunSeconds = 0.0;

    /* For OnMarkerRestored: the level also holds a Marker handle. */
    SandboxLevel* OwningLevel = nullptr;

    /* From the level; re-derived in OnPostLoadSave. */
    GameCore::EntityID MarkerEntity;
    SandboxGameState*  State = nullptr;

    /* Marker X at save time, logged on restore. */
    double SavedMarkerX = 0.0;
};

/* The scene: spawns Marker and Camera, registers the wrap system, binds the
 * entity logger, and owns the mode and state. */
class SandboxLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "Sandbox"; }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        (void)InSession;

        /* GetWorld(), not InSession.GetWorld() (Level.h). */
        GameCore::World& World = GetWorld();

        /* First, not last: OnUnload pairs with OnLoad being called (Level.h). */
        bLoaded = true;

        /* Removed in OnUnload (Delegate.h's RELOAD note). */
        EntityCreatedHandle = World.OnEntityCreated().Add(&OnEntityCreatedLogger, &World);

        /* MarkerEntity keeps only the id; the handle's World* is per-load. */
        const GameCore::Entity Marker = World.CreateEntity("Marker");
        MarkerEntity = Marker;
        {
            /* Off y=0: +Y runs down from the top edge in the ortho view, so a
             * quad at y=0 would hang half off screen. */
            GameCore::TransformComponent MarkerTransform;
            MarkerTransform.Position = EngineCore::Vector3(0.0, 360.0, 0.0);
            Marker.AddComponent(MarkerTransform);
        }
        /* WorldBridge draws it every frame; nothing else is needed. */
        RenderCore::RenderComponent Render;
        Render.Width  = 32.0f;
        Render.Height = 32.0f;
        Render.Tint   = EngineCore::Color(255, 200, 40);
        Marker.AddComponent(Render);

        /* The quad's extent in entity space, derived from the RenderComponent
         * so picking and spatial queries match what is drawn. */
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(
            EngineCore::Vector3(-0.5 * Render.Width, -0.5 * Render.Height, 0.0),
            EngineCore::Vector3( 0.5 * Render.Width,  0.5 * Render.Height, 0.0));
        Marker.AddComponent(Bounds);

        /* Centres the view on Marker's path. WorldBridge uses the first active
         * CameraComponent. */
        const GameCore::Entity CameraEntity = World.CreateEntity("Camera");
        CameraEntity.AddComponent<GameCore::TransformComponent>();
        RenderCore::CameraComponent Camera;
        CameraEntity.AddComponent(Camera);

        /* ViewportWidth is read once; the wrap boundary need only be rough. */
        SystemContext.Entity        = MarkerEntity;
        SystemContext.ViewportWidth = 1280;
        World.RegisterSystem("Sandbox.WrapMarker", GameCore::TP_PostPhysics,
                             &WrapMarkerSystem, &SystemContext);

        /* Here, not a member initialiser: needs MarkerEntity. */
        Mode.emplace(*this, MarkerEntity, State);

        return true;
    }

    /* Empties the World before a restore, or the scene is doubled (Level.h).
     * Systems and delegate bindings belong to the World and stay. */
    void OnPrepareForLoadedGame() override
    {
        GameCore::World& World = GetWorld();

        /* Collect, then destroy: ForEachEntity forbids destroying inside it. */
        std::vector<GameCore::EntityID> Doomed;
        World.ForEachEntity([](GameCore::EntityID InEntity, void* InUserData)
        {
            static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
        }, &Doomed);

        for(const GameCore::EntityID Entity : Doomed)
        {
            World.DestroyEntity(Entity);
        }

        /* Unset until OnMarkerRestored, which makes the system a no-op. */
        SystemContext.Entity = GameCore::InvalidEntityID();
        MarkerEntity         = GameCore::InvalidEntityID();

        CORE_LOG("[Sandbox] cleared " + std::to_string(Doomed.size())
                 + " entity/entities before restoring the save")
    }

    void OnUnload() override
    {
        /* Pairs with OnLoad's Add; ~World warns about leftover bindings. */
        if(OwnedWorldIsLive())
        {
            GetWorld().OnEntityCreated().Remove(EntityCreatedHandle);
        }
    }

    NimbleEngine::IGameMode* GetGameMode() override
    {
        return Mode ? &*Mode : nullptr;
    }

    NimbleEngine::GameState* GetGameState() override { return &State; }

    /* Re-points the wrap system at the restored Marker (from the mode's
     * OnPostLoadSave). Otherwise Marker silently stops wrapping. */
    void OnMarkerRestored(const GameCore::EntityID InMarker)
    {
        MarkerEntity         = InMarker;
        SystemContext.Entity = InMarker;
    }

private:
    /* OnLoad ran; guards OnUnload, since GetWorld() CORE_CHECKs. */
    bool OwnedWorldIsLive() const { return bLoaded; }

    bool                       bLoaded = false;
    GameCore::EntityID         MarkerEntity;
    GameCore::DelegateHandle   EntityCreatedHandle;
    SandboxMarkerSystemContext SystemContext;

    SandboxGameState                 State;
    std::optional<SandboxGameMode>   Mode;
};

/* Out-of-line: needs SandboxLevel's definition. */
void SandboxGameMode::OnPostLoadSave()
{
    const GameCore::EntityID Restored = Session->GetWorld().FindEntityByName("Marker");

    CORE_LOG(std::string("[Sandbox] save restored; re-deriving Marker (old handle ")
             + (Session->GetWorld().IsAlive(MarkerEntity) ? "still alive" : "now dead")
             + ", new handle " + (Restored.IsSet() ? "found" : "MISSING") + ")")

    MarkerEntity = Restored;

    /* The wrap system's copy is stale too. */
    OwningLevel->OnMarkerRestored(Restored);
}

/* Owns the level, so it lives exactly as long as its code. */
class SandboxGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "SandboxGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        /* Saves and [Game] StartLevel resolve levels by name. */
        NimbleEngine::RegisterLevel(Level);
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds a bare pointer, and this image is about to go. */
        NimbleEngine::UnregisterLevel(Level);
    }

    NimbleEngine::ILevel* GetStartLevel() override { return &Level; }

private:
    SandboxLevel Level;
};

NIMBLE_IMPLEMENT_GAME_MODULE(SandboxGameModule, "SandboxGame")

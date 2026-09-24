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

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>
#include <string>

/*
 * SandboxGame: the example game's code, built as a game module DLL
 * (NimbleEngine/GameModule.h) that SandboxMain.cpp's launcher -- and, next,
 * the editor's Play-in-Editor -- load by name. Nothing here knows which
 * program is running it.
 *
 * Sandbox: the example game. It demonstrates a C++ entity ("Marker") moved by
 * the mode and wrapped by a PostPhysics system; a Lua entity ("Scripted") from
 * Scripts/Main.lua, read back from C++; an OnEntityCreated binding with its
 * unbind; drawing through a PrimitiveComponent and a Camera entity, with no
 * OnRender anywhere; and a save/load cycle on a timer.
 *
 *   SandboxLevel      the entities: spawns Marker and Camera, registers the
 *                     wrap system, owns the mode and the state.
 *   SandboxGameMode   the rules: moves Marker, reports, decides when to quit.
 *   SandboxGameState  the progress: elapsed time and tick counts.
 *   SandboxGameModule the module: owns the level, registers it, names it as
 *                     the start level.
 */

/* A GAME-OWNED config var, reaching the same registry and the same .ini as the
 * engine's own -- which is the point. Distinct from [Application] MaxFrames:
 * that counts iterations for engine bring-up, this is game logic that ends the
 * run by asking the engine to stop. */
NIMBLE_CONFIG_INT(GSandboxRunSeconds, "Sandbox", "RunSeconds", 0, nullptr);

/* Where the save/load demonstration in SandboxGameMode::OnTick writes to.
 * Relative, so it lands beside the executable (next to the Config/ and Scripts/
 * this game already stages); SaveArchiveToFile creates the directory. A real
 * game would put this under EngineCore::appGetAppDataDir instead -- a save is
 * user data, not something that belongs in an install directory. */
static const char* const GSandboxSavePath = "Saves/Quick.nmsg";

/*
 * One-shot flags for the save/load demo, at file scope because a load rebuilds
 * the mode (OnLoad's Mode.emplace) and resets the state. A flag on either reads
 * false straight after the load it recorded, and the game saves and loads
 * forever. State that must outlive a level needs something that outlives a
 * level.
 */
static bool GSandboxHasSaved  = false;
static bool GSandboxHasLoaded = false;

/* The blob Sandbox.WrapMarker's void* points at -- one struct rather than
 * relying on raw pointer arithmetic into a bag of bytes, which is exactly the
 * kind of "assumes no padding between members" trap a POD closure with a real
 * type declaration avoids. Named at file scope so both the system function
 * below and SandboxGame's own member can agree on its layout. */
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

    /*
     * The PostPhysics system that wraps the Marker's X at the viewport width.
     * A plain function pointer registered with World::RegisterSystem, per
     * GameCore's "function pointer + void*, not a virtual interface" rule --
     * the marker's id and the current viewport width are closed over via
     * InUserData, the only channel a raw SystemFn has.
     */
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

    /*
     * World::OnEntityCreated's C++ subscriber, bound in SandboxLevel::OnLoad
     * and unbound in its OnUnload -- the "whatever OnLoad binds, OnUnload
     * unbinds" rule Delegate.h states, demonstrated explicitly rather than
     * left implicit. Logs the new entity's name so the log shows BOTH
     * entities this file and Scripts/Main.lua create, from one delegate.
     */
    void OnEntityCreatedLogger(GameCore::EntityID InEntity, void* InUserData)
    {
        auto* World = static_cast<GameCore::World*>(InUserData);
        CORE_LOG("[Sandbox] OnEntityCreated: " + std::string(World->GetEntity(InEntity).GetName()))
    }
}

/*
 * What this run is tracking. Owned by SandboxLevel, read and written by
 * SandboxGameMode -- the split GameState.h describes, at its smallest useful
 * size. OnReset is what makes restarting the level a real operation rather
 * than a fiction: everything here goes back to zero without the mode being
 * rebuilt.
 */
class SandboxGameState : public NimbleEngine::GameState
{
public:
    void OnReset() override
    {
        ElapsedSeconds    = 0.0;
        LastReportSeconds = 0.0;
        TicksThisSecond   = 0;
    }

    /*
     * Both directions in one body. ElapsedSeconds only counts up, so resuming
     * at the saved time rather than 0 shows the restore worked. Saves exactly
     * the fields OnReset touches; keep the two lists in step. A new field
     * goes at the end behind Ar.GetChunkVersion() (see GameState::Serialize).
     */
    void Serialize(EngineCore::Archive& Ar) override
    {
        Ar << ElapsedSeconds;
        Ar << LastReportSeconds;
        Ar << TicksThisSecond;
    }

    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    uint64_t TicksThisSecond   = 0;
};

/* Declared ahead of the mode because the mode calls back into the level in
 * OnPostLoadSave -- both handles onto the Marker go stale on a load, and the
 * level owns one of them. Defined below, after the mode it constructs. */
class SandboxLevel;

class SandboxGameMode : public NimbleEngine::IGameMode
{
public:
    SandboxGameMode(SandboxLevel& InLevel, GameCore::EntityID InMarker, SandboxGameState& InState)
        : OwningLevel(&InLevel), MarkerEntity(InMarker), State(&InState) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;

        /* Recorded so OnTick can assert the engine's game-thread-only promise
         * from the outside, rather than assume it. */
        GameThread = EngineCore::appThreadID();

        RunSeconds = double(GSandboxRunSeconds.GetInt());

        /* NOTHING IS SPAWNED HERE. SandboxLevel::OnLoad already built the
         * scene -- Marker, Camera, the wrap system -- and handed this mode the
         * one entity its rules actually need. That is the whole distinction
         * between a level and a mode; see IGameMode's class comment. */
        return true;
    }

    /*
     * The startup script has run by now -- Scripts/Main.lua, per
     * Config/DefaultEngine.ini's [Scripting] StartupScript -- so "Scripted"
     * exists here and did NOT during OnLoad. That ordering is the whole
     * reason this phase is separate; see IGameMode's lifecycle comment.
     */
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
        /* Runs CONCURRENTLY with the render thread draining the previous
         * frame. Nothing here can touch the renderer or the window, which is
         * the point of the context being a value. */
        CORE_CHECK(EngineCore::appThreadID() == GameThread,
                   "[Sandbox] OnTick ran on a thread other than the one OnStartup saw")

        State->ElapsedSeconds += InFrame.DeltaSeconds;
        State->TicksThisSecond++;

        GameCore::World& World = Session->GetWorld();
        if(GameCore::TransformComponent* Marker = World.GetEntity(MarkerEntity).GetComponent<GameCore::TransformComponent>())
        {
            const double SpeedPixelsPerSecond = 120.0;
            Marker->Position.SetX(Marker->Position.GetX() + SpeedPixelsPerSecond * InFrame.DeltaSeconds);
            /* Wrapping itself happens in Sandbox.WrapMarker, TP_PostPhysics --
             * this only advances the position; World::Tick (called by
             * NimbleEngine.cpp's Simulate, right after this method returns)
             * is what actually runs that system this same frame. */
        }

        /* The game deciding it is done. The engine finishes the frame in
         * flight and leaves the loop at the top. */
        if(RunSeconds > 0.0 && State->ElapsedSeconds >= RunSeconds && Session)
        {
            CORE_LOG("[Sandbox] RunSeconds reached; asking the engine to quit")
            Session->RequestQuit();
        }

        /* The save/load demo, on a timer so a plain run exercises it. The load
         * comes two seconds after the save, so the log shows Marker jump back
         * rather than a position indistinguishable from the current one. */
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

            /* "Scripted" is created by Scripts/Main.lua, run once at startup
             * (see Config/DefaultEngine.ini's [Scripting] StartupScript) --
             * absent until that script has actually run, and this reads it
             * purely through World, with no dependency on Lua itself. Proves
             * C++ can read what a script wrote to the same World. */
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
                     + " frames_rendered=" + Num(RenderStats.FramesRendered)
                     + " draw_calls=" + Num(RenderStats.DrawCalls))

            State->LastReportSeconds = State->ElapsedSeconds;
            State->TicksThisSecond   = 0;
        }
    }

    /* Re-derives MarkerEntity by name -- the restored Marker is a new entity,
     * so the old handle reads dead. Defined below SandboxLevel, which it calls. */
    void OnPostLoadSave() override;

    void OnUnload() override
    {
        /* NOTHING TO UNBIND HERE. The OnEntityCreated subscription belongs to
         * SandboxLevel, which bound it and unbinds it in its own OnUnload --
         * the mode never touched a World delegate. */
        CORE_LOG("[Sandbox] game stopping after " + Fixed2(State->ElapsedSeconds) + "s")
        Session = nullptr;
    }

private:
    /* Stashed in OnLoad so OnStart/OnTick can call RequestQuit() and reach
     * World. The session, never the host: this mode runs unchanged whichever
     * program is running the game. */
    NimbleEngine::GameSession* Session = nullptr;

    EngineCore::ThreadID GameThread = 0;

    double RunSeconds = 0.0;

    /* The level that owns this mode, for the one callback that goes the other
     * way: both it and this class hold a handle onto Marker, and a load
     * invalidates both. */
    SandboxLevel* OwningLevel = nullptr;

    /* Handed in by the level at construction -- the mode does not look them
     * up, because it did not create them. RE-DERIVED in OnPostLoadSave, which
     * is the one case where the mode does have to look one up: a restored
     * entity is a different entity. */
    GameCore::EntityID MarkerEntity;
    SandboxGameState*  State = nullptr;

    /* The marker position at the moment of the save, so the log can show it
     * coming back. Per-run, so a member here is right. */
    double SavedMarkerX = 0.0;
};

/*
 * WHAT CONTAINS THE SCENE. Spawns Marker and Camera, registers the wrap
 * system, subscribes the entity logger, and owns both the mode and the state
 * that act on all of it.
 *
 * Everything here used to live in SandboxGameMode::OnLoad. The move is the
 * whole point of this iteration: unloading this level destroys its World and
 * with it every entity below, and the mode survives that as pure logic with
 * nothing of its own to clean up.
 */
class SandboxLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "Sandbox"; }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        (void)InSession;

        /* GetWorld(), not InSession.GetWorld(): they are the same object during
         * OnLoad, but this one stays correct regardless of what else the
         * engine has loaded -- Level.h says so. */
        GameCore::World& World = GetWorld();

        /* First, not last: OnUnload pairs with OnLoad being called (Level.h). */
        bLoaded = true;

        /* The bind/unbind pair, spelled out: Remove(EntityCreatedHandle) in
         * OnUnload is what makes this safe rather than merely convenient --
         * see Delegate.h's RELOAD note. */
        EntityCreatedHandle = World.OnEntityCreated().Add(&OnEntityCreatedLogger, &World);

        /* The handle attaches components directly. MarkerEntity keeps just the
         * id: the handle's World* belongs to this load, while the id is what
         * the mode and the wrap system are handed. */
        const GameCore::Entity Marker = World.CreateEntity("Marker");
        MarkerEntity = Marker;
        {
            /* Lifted off y=0 deliberately. The view is orthographic with its
             * origin at the camera entity's own position -- (0,0,0) here --
             * and +Y runs DOWN the window (RenderView::MakeOrthographic), so
             * y=0 is the TOP edge, and a 32x32 quad centred there would hang
             * half out of the viewport. Roughly mid-height keeps the whole
             * quad on screen as it sweeps. */
            GameCore::TransformComponent MarkerTransform;
            MarkerTransform.Position = EngineCore::Vector3(0.0, 360.0, 0.0);
            Marker.AddComponent(MarkerTransform);
        }
        /* Drawn automatically, every frame, by RenderCore::Renderer's
         * WorldBridge -- adding this component is the whole of what this
         * file does to make Marker visible; nothing here calls DrawQuad or
         * touches a RenderProxy directly. */
        RenderCore::PrimitiveComponent Primitive;
        Primitive.Width  = 32.0f;
        Primitive.Height = 32.0f;
        Primitive.Tint   = EngineCore::Color(255, 200, 40);
        Marker.AddComponent(Primitive);

        /* The quad's own extent, in entity space: a centred Width x Height,
         * flat in Z like the quad. Derived from the Primitive rather than
         * written out, so the two cannot drift -- they did, when this was a
         * hard-coded +-0.5 cube on a 32 x 32 quad and spatial queries, picking
         * and the editor's selection outline all saw a 1-unit Marker. The
         * refit carries Local through the Transform, Scale included, the same
         * way the proxy draws the quad. */
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(
            EngineCore::Vector3(-0.5 * Primitive.Width, -0.5 * Primitive.Height, 0.0),
            EngineCore::Vector3( 0.5 * Primitive.Width,  0.5 * Primitive.Height, 0.0));
        Marker.AddComponent(Bounds);

        /* A camera entity so the view is centred on where Marker actually
         * moves rather than the default (0, 1, -1) origin -- WorldBridge's
         * per-frame view build reads whichever active CameraComponent it
         * finds first, and its position from this entity's own Transform. */
        const GameCore::Entity CameraEntity = World.CreateEntity("Camera");
        CameraEntity.AddComponent<GameCore::TransformComponent>();
        RenderCore::CameraComponent Camera;
        CameraEntity.AddComponent(Camera);

        /* Closed over via a small POD blob rather than two separate void*
         * parameters -- SystemFn's contract is exactly one void*, matching
         * every other seam in GameCore (ComponentRemovedFn, EventFn).
         *
         * ViewportWidth is set once here from the window rather than every
         * frame from the FrameContext: the wrap boundary only has to be
         * roughly right, and reading it here keeps the mode from needing to
         * reach into the level's system state at all. */
        SystemContext.Entity        = MarkerEntity;
        SystemContext.ViewportWidth = 1280;
        World.RegisterSystem("Sandbox.WrapMarker", GameCore::TP_PostPhysics,
                             &WrapMarkerSystem, &SystemContext);

        /* Constructed HERE, not as a member initialiser, because it needs
         * MarkerEntity -- which only exists once the World above is built. */
        Mode.emplace(*this, MarkerEntity, State);

        return true;
    }

    /*
     * Clears the World before a save is restored into it; otherwise the scene
     * is doubled (Level.h). Every entity, not just Marker and Camera -- the
     * startup script's "Scripted" is here too, and the save has its own copy.
     * Outside a tick DestroyEntity is immediate, so the World is empty when the
     * restore starts. The wrap system and the delegate binding belong to the
     * World, not an entity, and stay; OnUnload still unbinds once.
     */
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
        /* The other half of the bind/unbind pair from OnLoad. The World is
         * still alive at this point (Level.h guarantees it), which is exactly
         * what makes this possible -- ~World logs a warning for any built-in
         * delegate still holding entries, and this is what keeps it quiet. */
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

    /*
     * Re-points the wrap system at the restored Marker. Without it the system
     * silently finds no Transform and Marker never wraps -- which looks like a
     * renderer bug. Called from the mode's OnPostLoadSave, which is already
     * re-deriving the same handle, rather than being a second ordered hook.
     */
    void OnMarkerRestored(const GameCore::EntityID InMarker)
    {
        MarkerEntity         = InMarker;
        SystemContext.Entity = InMarker;
    }

private:
    /* True once OnLoad has run; guards OnUnload, since GetWorld() CORE_CHECKs.
     * A flag of its own, because OnPrepareForLoadedGame clears MarkerEntity and
     * OnUnload must still unbind after a load. */
    bool OwnedWorldIsLive() const { return bLoaded; }

    bool                       bLoaded = false;
    GameCore::EntityID         MarkerEntity;
    GameCore::DelegateHandle   EntityCreatedHandle;
    SandboxMarkerSystemContext SystemContext;

    SandboxGameState                 State;
    std::optional<SandboxGameMode>   Mode;
};

/* Out-of-line, because it reaches into SandboxLevel -- which was only a forward
 * declaration where this method is declared. */
void SandboxGameMode::OnPostLoadSave()
{
    const GameCore::EntityID Restored = Session->GetWorld().FindEntityByName("Marker");

    CORE_LOG(std::string("[Sandbox] save restored; re-deriving Marker (old handle ")
             + (Session->GetWorld().IsAlive(MarkerEntity) ? "still alive" : "now dead")
             + ", new handle " + (Restored.IsSet() ? "found" : "MISSING") + ")")

    MarkerEntity = Restored;

    /* The level's wrap system closes over the marker's handle too, and its copy
     * is just as stale as the one above was -- see SandboxLevel::OnMarkerRestored
     * for what goes wrong without this. */
    OwningLevel->OnMarkerRestored(Restored);
}

/*
 * The module the host loads. It owns the one level, so the level lives exactly
 * as long as the code that implements it; the host has unloaded the level
 * (GameSession::UnloadLevel) before it unloads this.
 */
class SandboxGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "SandboxGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        /* Required before any load: a save names its level by string, and the
         * host resolves [Game] StartLevel the same way. */
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

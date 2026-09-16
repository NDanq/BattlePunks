#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

#include <cstdint>
#include <cstdio>
#include <string>

/*
 * Sandbox: the example game. CMakeLists.txt says why it exists; this is what it
 * currently demonstrates -- the IGame seam, and now GameCore's World/Entity/
 * delegate surface: a C++-owned entity ("Marker") ticked by a registered
 * PostPhysics system, a C++ subscriber on World's OnEntityCreated delegate
 * with the bind/unbind pair shown explicitly, and a Lua-owned entity
 * ("Scripted", created by Scripts/Main.lua) whose position this file reads
 * back every second to show the two sides agreeing on one World.
 *
 * No drawing: IGame is simulation-only until render proxies land. When drawing
 * arrives it arrives as another IGame method and this file grows one.
 *
 * NO LOG HANDLER IS INSTALLED, deliberately. Without one, CoreLog's Fatal path
 * writes to stderr and abort()s; installing one turns every CORE_CHECK and
 * GDI_CHECK into print-and-continue, which is the harder failure to debug while
 * the engine is this young. A shipping game will want a handler.
 */

/* A GAME-OWNED config var, reaching the same registry and the same .ini as the
 * engine's own -- which is the point. Distinct from [Application] MaxFrames:
 * that counts iterations for engine bring-up, this is game logic that ends the
 * run by asking the engine to stop. */
NIMBLE_CONFIG_INT(GSandboxRunSeconds, "Sandbox", "RunSeconds", 0, nullptr);

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

        if(GameCore::TransformComponent* Transform = InWorld.GetComponent<GameCore::TransformComponent>(Context.Entity))
        {
            const double X = Transform->Position.GetX();
            if(Context.ViewportWidth > 0 && X >= double(Context.ViewportWidth))
            {
                Transform->Position.SetX(X - double(Context.ViewportWidth));
            }
        }
    }

    /*
     * World::OnEntityCreated's C++ subscriber, bound in OnStartup and
     * unbound in OnShutdown -- the "whatever OnStartup binds, OnShutdown
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

class SandboxGame : public NimbleEngine::IGame
{
public:
    bool OnStartup(NimbleEngine::Application& InApp) override
    {
        App = &InApp;

        /* Recorded so OnTick can assert the engine's game-thread-only promise
         * from the outside, rather than assume it. */
        GameThread = EngineCore::appThreadID();

        RunSeconds = double(GSandboxRunSeconds.GetInt());

        const uint32_t Limit = InApp.GetFrameRateLimit();

        CORE_LOG("[Sandbox] game starting on thread " + Num(uint64_t(GameThread))
                 + "; frame limit "
                 + (Limit > 0 ? Num(Limit) + " fps" : std::string("unlimited"))
                 + (RunSeconds > 0.0
                    ? "; will quit after " + Fixed2(RunSeconds) + "s"
                    : "; running until the window closes"))

        GameCore::World& World = InApp.GetWorld();

        /* The bind/unbind pair, spelled out: Remove(EntityCreatedHandle) in
         * OnShutdown is what makes this safe rather than merely convenient --
         * see Delegate.h's RELOAD note. */
        EntityCreatedHandle = World.OnEntityCreated().Add(&OnEntityCreatedLogger, &World);

        MarkerEntity = World.CreateEntity("Marker");
        World.AddComponent<GameCore::TransformComponent>(MarkerEntity);
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, -0.5), EngineCore::Vector3(0.5, 0.5, 0.5));
        World.AddComponent<GameCore::BoundsComponent>(MarkerEntity, Bounds);

        /* Closed over via a small POD blob rather than two separate void*
         * parameters -- SystemFn's contract is exactly one void*, matching
         * every other seam in GameCore (ComponentRemovedFn, EventFn). */
        SystemContext.Entity        = MarkerEntity;
        SystemContext.ViewportWidth = 0;   // filled in on the first OnTick, once a real viewport is known
        World.RegisterSystem("Sandbox.WrapMarker", GameCore::TP_PostPhysics, &WrapMarkerSystem, &SystemContext);

        return true;
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        /* Runs CONCURRENTLY with the render thread draining the previous
         * frame. Nothing here can touch the renderer or the window, which is
         * the point of the context being a value. */
        CORE_CHECK(EngineCore::appThreadID() == GameThread,
                   "[Sandbox] OnTick ran on a thread other than the one OnStartup saw")

        ElapsedSeconds += InFrame.DeltaSeconds;
        TicksThisSecond++;

        SystemContext.ViewportWidth = InFrame.ViewportPixels.Width > 0 ? InFrame.ViewportPixels.Width : 1;

        GameCore::World& World = App->GetWorld();
        if(GameCore::TransformComponent* Marker = World.GetComponent<GameCore::TransformComponent>(MarkerEntity))
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
        if(RunSeconds > 0.0 && ElapsedSeconds >= RunSeconds && App)
        {
            CORE_LOG("[Sandbox] RunSeconds reached; asking the engine to quit")
            App->RequestQuit();
        }

        if(ElapsedSeconds - LastReportSeconds >= 1.0)
        {
            const double MarkerX = World.GetComponent<GameCore::TransformComponent>(MarkerEntity)
                                  ? World.GetComponent<GameCore::TransformComponent>(MarkerEntity)->Position.GetX()
                                  : 0.0;

            /* "Scripted" is created by Scripts/Main.lua, run once at startup
             * (see Config/DefaultEngine.ini's [Scripting] StartupScript) --
             * absent until that script has actually run, and this reads it
             * purely through World, with no dependency on Lua itself. Proves
             * C++ can read what a script wrote to the same World. */
            const GameCore::EntityID ScriptedEntity = World.FindEntityByName("Scripted");
            std::string ScriptedPart;
            if(GameCore::TransformComponent* Scripted = World.GetComponent<GameCore::TransformComponent>(ScriptedEntity))
            {
                ScriptedPart = " scripted_x=" + Fixed2(Scripted->Position.GetX());
            }

            CORE_LOG("[Sandbox] t=" + Fixed2(ElapsedSeconds) + "s"
                     + " frame=" + Num(InFrame.FrameNumber)
                     + " fps=" + Num(TicksThisSecond)
                     + " dt=" + Fixed2(InFrame.DeltaSeconds * 1000.0) + "ms"
                     + " viewport=" + Num(uint64_t(InFrame.ViewportPixels.Width))
                     + "x" + Num(uint64_t(InFrame.ViewportPixels.Height))
                     + " marker_x=" + Fixed2(MarkerX)
                     + ScriptedPart)

            LastReportSeconds = ElapsedSeconds;
            TicksThisSecond   = 0;
        }
    }

    void OnShutdown() override
    {
        CORE_LOG("[Sandbox] game stopping after " + Fixed2(ElapsedSeconds) + "s")

        /* The other half of the bind/unbind pair from OnStartup -- see
         * Delegate.h's RELOAD note on why this is not optional even for a
         * binding that lives exactly as long as the engine's own World. */
        if(App)
        {
            App->GetWorld().OnEntityCreated().Remove(EntityCreatedHandle);
        }

        App = nullptr;
    }

private:
    /* Stashed in OnStartup so OnTick can call RequestQuit() and reach World. */
    NimbleEngine::Application* App = nullptr;

    EngineCore::ThreadID GameThread = 0;

    double   RunSeconds        = 0.0;
    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    uint64_t TicksThisSecond   = 0;

    GameCore::EntityID     MarkerEntity;
    GameCore::DelegateHandle EntityCreatedHandle;

    SandboxMarkerSystemContext SystemContext;
};

int main(int /*argc*/, char** /*argv*/)
{
    {
        SandboxGame Game;
        NimbleEngine::Application App;
        App.Exec(Game);
    }

    /*
     * AFTER both are destroyed, so their members are not counted, and explicit
     * rather than a static destructor -- CoreGarbageCollector.h explains why.
     * This is also what proves the drain loop's `delete Command`, and the
     * virtual destructor under it, actually work.
     */
    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();

    if(Leaked != 0)
    {
        std::fprintf(stderr, "Sandbox: %llu allocation(s) leaked\n",
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }

    return 0;
}

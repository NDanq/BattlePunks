#ifndef NIMBLEEDITOR_PLAYSESSION_H
#define NIMBLEEDITOR_PLAYSESSION_H

#if WITH_EDITOR

#include <cstdint>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/NimbleEngine/GameSession.h"

namespace GameCore     { class ScriptEngine; class World; }
namespace NimbleEngine { class GameModuleHost; }
namespace RenderCore   { class Renderer; }

/*
 * One Play-in-Editor run: the editor as a game host.
 *
 * What Application does for a game's own launcher, minus the window and the
 * loop -- the editor already has both. Start loads the game module named by
 * [Game] Module (GameModuleHost), makes a ScriptEngine, and runs the module's
 * start level in a GameSession that borrows the editor's Renderer. Tick is one
 * frame of it, called by EditorContext::RenderFrame before the render fence,
 * exactly where Application::RunFrame ticks its session. Stop unwinds in the
 * reverse order: level, scripts, module.
 *
 * The game module is loaded per run and unloaded at Stop, so a rebuilt game
 * DLL is picked up by the next Play without restarting the editor -- the
 * module manager loads a shadow copy, so the build can overwrite the original
 * while the editor runs.
 *
 * The Renderer must NOT be bound to a World when Start runs: the session binds
 * its level's World itself (GameSession::LoadLevel), and unbinds it at Stop.
 *
 * Private to the editor: EditorContext owns it and is the only caller.
 */
class PlaySession final : public NimbleEngine::IGameHost
{
public:
    explicit PlaySession(RenderCore::Renderer& InRenderer);
    ~PlaySession() override;   // Stop()s

    PlaySession(const PlaySession&)            = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    /* False, with the reason logged, if the module, the scripts or the level
     * would not start; everything started so far is stopped again. */
    bool Start();

    /* Null-safe and idempotent. */
    void Stop();

    bool IsRunning() const { return Session != nullptr; }

    /* One frame: a requested level swap (a save restore is one), then the
     * mode and the World. InDeltaSeconds is wall time since the last Tick. */
    void Tick(double InDeltaSeconds, EngineCore::Size InViewportPixels);

    /* The level's World; null when no level is loaded (a failed swap). */
    GameCore::World* GetWorld() const;

    /* See GameSession::GetLoadCount -- changes when the World is replaced. */
    uint32_t GetLoadCount() const { return Session ? Session->GetLoadCount() : 0; }

    /* The next Tick will replace the World (GameSession::HasPendingLevel). */
    bool IsLevelSwapPending() const { return Session && Session->HasPendingLevel(); }

    /* The game asked to end (GameSession::RequestQuit), or a level swap
     * failed. The editor stops the run rather than quitting. */
    bool IsQuitRequested() const { return Session && Session->IsQuitRequested(); }

    /* "SandboxGame / Sandbox", for the status bar. */
    std::string Describe() const;

    // === NimbleEngine::IGameHost ==========================================

    /* Recorded for the game to read back, not enforced: the editor's viewport
     * sets the pace (its frame timer), and a game running slower or faster
     * than it asked for in the editor is the expected trade. */
    void     SetFrameRateLimit(uint32_t InFramesPerSecond) override { FrameRateLimit = InFramesPerSecond; }
    uint32_t GetFrameRateLimit() const override                    { return FrameRateLimit; }

private:
    RenderCore::Renderer& Rendering;

    NimbleEngine::GameModuleHost* Game    = nullptr;
    GameCore::ScriptEngine*       Scripts = nullptr;
    NimbleEngine::GameSession*    Session = nullptr;

    uint32_t FrameRateLimit = 0;
    uint64_t FrameNumber    = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PLAYSESSION_H

#ifndef NIMBLEEDITOR_PLAYSESSION_H
#define NIMBLEEDITOR_PLAYSESSION_H

#if WITH_EDITOR

#include <cstdint>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/InputRouter.h"

namespace GameCore     { class ScriptEngine; class World; }
namespace NimbleEngine { class GameModuleHost; }
namespace RenderCore   { class Renderer; }

/*
 * One Play-in-Editor run: Application's job minus the window and loop. Start
 * loads [Game] Module, makes a ScriptEngine and runs the start level in a
 * GameSession using the editor's Renderer; Tick runs where RunFrame would;
 * Stop unwinds level, scripts, module.
 *
 * The module is reloaded per run (from a shadow copy), so rebuilds are picked
 * up on the next Play. The Renderer must be unbound when Start runs.
 */
class PlaySession final : public NimbleEngine::IGameHost
{
public:
    explicit PlaySession(RenderCore::Renderer& InRenderer);
    ~PlaySession() override;   // Stop()s

    PlaySession(const PlaySession&)            = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    /* False, logged, on failure; partial startup is undone. */
    /* InOpenWorld: the editor's open world (runtime path); the level that
     * loads it plays if the game has one, else [Game] StartLevel. */
    bool Start(const std::string& InOpenWorld = std::string());

    /* Null-safe and idempotent. */
    void Stop();

    bool IsRunning() const { return Session != nullptr; }

    /* Pending level swap, then the mode and World. */
    void Tick(double InDeltaSeconds, EngineCore::Size InViewportPixels);

    /* The level's World; null when no level is loaded (a failed swap). */
    GameCore::World* GetWorld() const;

    /* See GameSession::GetLoadCount -- changes when the World is replaced. */
    uint32_t GetLoadCount() const { return Session ? Session->GetLoadCount() : 0; }

    /* The next Tick will replace the World (GameSession::HasPendingLevel). */
    bool IsLevelSwapPending() const { return Session && Session->HasPendingLevel(); }

    /* The editor stops the run rather than quitting. */
    bool IsQuitRequested() const { return Session && Session->IsQuitRequested(); }

    /* "SandboxGame / Sandbox", for the status bar. */
    std::string Describe() const;

    // === NimbleEngine::IGameHost ==========================================

    /* Recorded, not enforced: the viewport's timer sets the pace. */
    void     SetFrameRateLimit(uint32_t InFramesPerSecond) override { FrameRateLimit = InFramesPerSecond; }
    uint32_t GetFrameRateLimit() const override                    { return FrameRateLimit; }

    /* What Tick latched: the viewport's SDL input, as the launcher's. */
    const NimbleEngine::InputState& GetInput() const override { return Input.GetState(); }
    /* The viewport's SDL relative mode; logged, since it only engages while
     * the viewport holds the native input focus (ViewportSDLInput). */
    void SetMouseCaptured(bool bInCaptured) override;
    bool IsMouseCaptured() const override           { return Input.GetWindow() ? Input.IsMouseCaptured() : bHostMouseCaptured; }
    /* The viewport's SDL window's; kept for one attached later. */
    void SetTextInputActive(bool bInActive) override { Input.SetTextInputActive(bInActive); }
    std::string GetClipboardText() const override;
    void        SetClipboardText(const std::string& InUtf8) override;

    /* The game wants the mouse captured but there is no SDL window to do it
     * (the Qt fallback): the viewport holds the cursor itself. */
    bool WantsViewportMouseHold() const { return !Input.GetWindow() && bHostMouseCaptured; }

    // === Input ===============================================================

    /* The router the viewport's SDL window feeds (EditorContext attaches it),
     * or, without SDL, the viewport's Qt events through GetPending(). Tick
     * latches it once a frame, before the game's frame. */
    NimbleEngine::InputRouter& GetInputRouter() { return Input; }

    /* Attaches the router to the viewport's SDL window. A capture the game
     * asked for before there was one (IGameMode::OnStart runs inside Start)
     * is applied to it now. */
    void AttachInputWindow(GameWindow* InWindow);

    /* While the editor holds the view (ejected), the game sees nothing held:
     * what arrives is dropped instead of latched. */
    void SetInputBlocked(bool bInBlocked) { bInputBlocked = bInBlocked; }

private:
    RenderCore::Renderer& Rendering;

    NimbleEngine::GameModuleHost* Game    = nullptr;
    GameCore::ScriptEngine*       Scripts = nullptr;
    NimbleEngine::GameSession*    Session = nullptr;

    NimbleEngine::InputRouter Input;
    bool                      bInputBlocked = false;
    bool                      bHostMouseCaptured = false;   // SetMouseCaptured without an SDL window

    uint32_t FrameRateLimit = 0;
    uint64_t FrameNumber    = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PLAYSESSION_H

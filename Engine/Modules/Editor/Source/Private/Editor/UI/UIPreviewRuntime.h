#ifndef NIMBLEEDITOR_UIPREVIEWRUNTIME_H
#define NIMBLEEDITOR_UIPREVIEWRUNTIME_H

#if WITH_EDITOR

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameCore { class ScriptEngine; class World; }
namespace UI { class UISystem; }

/*
 * What the UI Designer's Preview runs a document's logic in: a scratch World
 * and script engine of its own, so actions and the UI graph run as in a game
 * -- Native:UI.*, Lua:, Event:, Graph: -- against the preview's UISystem.
 *
 * Not a game: the Game library answers defaults there instead of raising
 * (NimbleEngine::SetScriptPreviewWorld), Game.* native actions do nothing,
 * and every config var a graph changes (Config.SetBool...) is put back when
 * the preview stops. Config.Save does nothing in the editor.
 */
class UIPreviewRuntime
{
public:
    UIPreviewRuntime();
    ~UIPreviewRuntime();
    UIPreviewRuntime(const UIPreviewRuntime&)            = delete;
    UIPreviewRuntime& operator=(const UIPreviewRuntime&) = delete;

    /* InSystem runs its documents' logic from now; close and reopen them after. */
    bool Start(UI::UISystem& InSystem);

    /* Closes InSystem's documents (their OnClose runs), lets the engine go and
     * puts back the config vars. Safe when not started. */
    void Stop(UI::UISystem& InSystem);

    bool IsRunning() const { return Scripts != nullptr; }

    /* Script errors since Start: the status bar says when there are new ones. */
    uint32_t GetErrorCount() const;

    /* The next document opened reads InGraphPath again (a saved graph). */
    void ForgetGraph(const std::string& InGraphPath);

private:
    struct SavedVar { std::string Section, Name, Value; };

    std::unique_ptr<GameCore::World>        World;
    std::unique_ptr<GameCore::ScriptEngine> Scripts;
    std::vector<SavedVar>                   Config;
    uint32_t                                ErrorsAtStart = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIPREVIEWRUNTIME_H

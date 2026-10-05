#ifndef UI_UISYSTEM_H
#define UI_UISYSTEM_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/Renderer.h"
#include "Public/UI/UIAction.h"
#include "Public/UI/UIAPI.h"
#include "Public/UI/UIBinding.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UIFont.h"
#include "Public/UI/UIInput.h"

namespace GameCore { class ScriptEngine; class World; }

/*
 * The open documents of one viewport, and everything they share: the data
 * store, the fonts, the action queue. A game session owns one (NimbleEngine's
 * GameSession::GetUI); the editor's designer owns one per tab.
 *
 * A frame, game thread:
 *
 *     ProcessInput   pointer and navigation to the documents, top layer
 *                    first; actions queued; what was consumed returned
 *     RunQueuedActions
 *     Tick           hot reload, bindings, animations (reserved)
 *     Update         layout and paint what changed, and give the renderer
 *                    the merged list -- only when it changed
 *
 * Documents draw by Layer, then by when they opened; a UIOnly document takes
 * every input from those under it.
 */
namespace UI
{
    using UIDocumentHandle = uint32_t;
    constexpr UIDocumentHandle UI_DOCUMENT_INVALID = 0;

    class UI_API UISystem
    {
    public:
        UISystem();
        ~UISystem();
        UISystem(const UISystem&)            = delete;
        UISystem& operator=(const UISystem&) = delete;

        // === Wiring =============================================================

        /* Where Update submits (null: nowhere -- tests, the editor's own paths). */
        void SetRenderer(RenderCore::Renderer* InRenderer, RenderCore::RenderSceneID InScene = RenderCore::RENDER_DEFAULT_SCENE);
        /* What Lua: actions and documents' Script run in (null: logged, skipped). */
        void SetScriptEngine(GameCore::ScriptEngine* InScripts);
        /* Whose EventBus Event: actions emit on (null: logged, skipped). */
        void SetWorld(GameCore::World* InWorld);
        /* Handed to native actions (UIActionContext::HostData): the game session. */
        void SetHostData(void* InHostData) { HostData = InHostData; }
        void* GetHostData() const { return HostData; }

        /* False: actions are logged, not run -- the editor's preview. */
        void SetExecuteActions(bool bInExecute) { bExecuteActions = bInExecute; }

        // === Documents ==========================================================

        /* Opens the document at InPath (UIAssetCache) over the others of its
         * Layer, or returns it if it is open. A persistent one stays through
         * CloseAll(false) -- a level change. UI_DOCUMENT_INVALID if it does not load. */
        UIDocumentHandle Open(const std::string& InPath, bool bInPersistent = false);

        /* Opens InAsset, not from a file (the editor; tests). InPath names it
         * (its graph's self.Document). bInFollowCache: rebuilt when the cache's
         * copy of InPath moves -- false for the designer, whose asset is the
         * one being edited, not the file. */
        UIDocumentHandle OpenAsset(std::shared_ptr<const UIDocumentAsset> InAsset, const std::string& InPath, bool bInPersistent = false,
                                   bool bInFollowCache = true);

        bool Close(UIDocumentHandle InHandle);
        bool Close(const std::string& InPath);
        void CloseAll(bool bInPersistentToo);

        bool IsOpen(const std::string& InPath) const;
        UIDocumentInstance* Find(UIDocumentHandle InHandle) const;
        UIDocumentInstance* Find(const std::string& InPath) const;
        UIDocumentHandle    FindHandle(const std::string& InPath) const;
        size_t              GetDocumentCount() const;

        // === Data store =========================================================

        void SetValue(const std::string& InKey, const UIValue& InValue);
        const UIValue* GetValue(const std::string& InKey) const;
        UIDataStore& GetDataStore() { return Store; }

        // === Frame ==============================================================

        UIInputResult ProcessInput(const UIInputFrame& InFrame);
        void RunQueuedActions();

        /* Queues InActions as fired by InSender of InDocument (either may be null). */
        void QueueActions(const UIActionList& InActions, UIDocumentInstance* InDocument, UIWidget* InSender);

        void Tick(float InDeltaSeconds);

        /* Lays out and paints for InViewportPixels what changed; submits the
         * merged list to the renderer when it changed. */
        void Update(const UIVec2& InViewportPixels);

        /* Every document's list merged, bottom first (what Update submits),
         * and a count of its rebuilds: a host composing its own frame (the
         * editor's designer) redraws only when it moves. */
        const RenderCore::UIDrawList& GetDrawList() const { return Merged; }
        uint64_t GetDrawListSerial() const { return MergedSerial; }

        /* Lists the renderer has been given: for tests and "stat". */
        uint64_t GetSubmitCount() const { return SubmitCount; }

        /* Clears what the renderer shows (Update would submit it again). */
        void ClearRenderer();

        UIFontCache& GetFonts() { return Fonts; }

        /* An Include made or dropped its document: its bindings applied and its
         * UI graph made (OnOpen), or its UI graph let go (OnClose). */
        void AttachEmbedded(UIDocumentInstance& InEmbedded);
        void DetachEmbedded(UIDocumentInstance& InEmbedded);
        /* An embedded document given a new prefix (a ListView's reused row): its
         * bindings read again, and its graph's self.Prefix follows. */
        void RebindEmbedded(UIDocumentInstance& InEmbedded, const std::string& InPrefix);

        /* The clipboard a text field copies to and pastes from: the host's
         * (GameSession: the window's; the designer: Qt's). Without hooks, one
         * kept here. */
        void SetClipboard(std::function<std::string()> InGet, std::function<void(const std::string&)> InSet);
        std::string GetClipboardText() const;
        void        SetClipboardText(const std::string& InText);

        /* Each action run, logged or not ("Native:Game.Quit"), the newest last; up to 64. */
        const std::vector<std::string>& GetActionLog() const { return ActionLog; }

    private:
        struct Document;
        struct QueuedAction;

        Document* FindDocument(UIDocumentHandle InHandle) const;
        void      SortDocuments();
        void      RunAction(const QueuedAction& InAction);
        void      Rebuild(Document& InDocument, std::shared_ptr<const UIDocumentAsset> InAsset);
        void      RunScriptOnce(const UIDocumentAsset& InAsset);

        /* A document's UI graph (Settings.Graph) instanced in Scripts, its
         * OnOpen queued; and let go, OnClose run first if bInCallClose. The
         * Tree forms: it and the documents embedded in it. */
        void      CreateGraphObject(UIDocumentInstance& InDoc);
        void      ReleaseGraphObject(UIDocumentInstance& InDoc, bool bInCallClose);
        void      CreateTreeGraphObjects(UIDocumentInstance& InRoot);
        void      ReleaseTreeGraphObjects(UIDocumentInstance& InRoot, bool bInCallClose);

        std::vector<int32_t> TickingScratch;   // graph objects with an OnTick, this frame

        std::vector<std::unique_ptr<Document>> Documents;   // draw order: bottom first
        UIDocumentHandle         NextHandle = 1;
        uint64_t                 NextOrder  = 1;

        UIDataStore              Store;
        UIFontCache              Fonts;
        std::vector<QueuedAction> Queue;
        std::vector<std::string> ActionLog;
        std::vector<std::string> ScriptsRun;   // per ScriptEngine: a document's Script runs once

        RenderCore::Renderer*     Renderer = nullptr;
        RenderCore::RenderSceneID Scene    = RenderCore::RENDER_DEFAULT_SCENE;
        GameCore::ScriptEngine*   Scripts  = nullptr;
        GameCore::World*          World    = nullptr;
        void*                     HostData = nullptr;
        bool                      bExecuteActions = true;

        std::function<std::string()>             ClipboardGet;
        std::function<void(const std::string&)>  ClipboardSet;
        std::string                              ClipboardOwn;

        RenderCore::UIDrawList Merged;
        bool                   bMergedDirty = true;
        bool                   bSubmitted   = false;   // the renderer holds Merged
        uint64_t               MergedSerial = 0;
        uint64_t               SubmitCount  = 0;
        UIVec2                 Viewport;
    };

    /* The session Lua's UI library acts on (UIScriptLibrary.cpp); null none. */
    UI_API void      SetScriptUISystem(UISystem* InSystem);
    UI_API UISystem* GetScriptUISystem();
}

#endif //UI_UISYSTEM_H

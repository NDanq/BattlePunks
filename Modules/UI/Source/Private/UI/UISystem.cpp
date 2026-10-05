#include "Public/UI/UISystem.h"

#include <algorithm>
#include <set>

#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/UI/UILog.h"

namespace UI
{
    namespace
    {
        UISystem* GScriptUISystem = nullptr;
        constexpr size_t ACTION_LOG_MAX = 64;

        const char* KindName(const EUIActionKind InKind)
        {
            switch(InKind)
            {
                case EUIActionKind::Native: return "Native";
                case EUIActionKind::Lua:    return "Lua";
                case EUIActionKind::Event:  return "Event";
            }
            return "?";
        }
    }

    void SetScriptUISystem(UISystem* InSystem) { GScriptUISystem = InSystem; }
    UISystem* GetScriptUISystem() { return GScriptUISystem; }

    struct UISystem::Document
    {
        UIDocumentHandle                    Handle = UI_DOCUMENT_INVALID;
        std::unique_ptr<UIDocumentInstance> Instance;
        std::string                         Path;
        int32_t                             Layer = 0;
        uint64_t                            Order = 0;
        bool                                bPersistent = false;
        uint64_t                            AssetGeneration = 0;

        /* The last hit test, kept while the pointer and the painted
         * rectangles stay put: a still mouse over a still UI costs nothing. */
        UIVec2                              HitPointer;
        uint64_t                            HitPaintSerial = 0;
        int32_t                             Hit = -1;
    };

    /* An action fired, held by handles and names: run after input, by which
     * time its document may have closed (or reloaded). */
    struct UISystem::QueuedAction
    {
        UIAction            Action;
        UIDocumentHandle    Document = UI_DOCUMENT_INVALID;
        UIDocumentInstance* Instance = nullptr;   // compared, never followed, until Document is found
        int32_t             Sender   = -1;
        std::string         SenderName;
        double              SenderValue = 0.0;
    };

    UISystem::UISystem() = default;

    UISystem::~UISystem()
    {
        /* Not the renderer: it may already be gone. A host clears it (ClearRenderer). */
        if(GScriptUISystem == this) GScriptUISystem = nullptr;
    }

    void UISystem::SetRenderer(RenderCore::Renderer* InRenderer, const RenderCore::RenderSceneID InScene)
    {
        Renderer   = InRenderer;
        Scene      = InScene;
        bSubmitted = false;
        bMergedDirty = true;
        Fonts.MarkAllForUpload();
    }

    void UISystem::SetScriptEngine(GameCore::ScriptEngine* InScripts)
    {
        if(InScripts != Scripts) ScriptsRun.clear();
        Scripts = InScripts;
    }

    void UISystem::SetWorld(GameCore::World* InWorld)
    {
        World = InWorld;
    }

    // === Documents ================================================================

    UIDocumentHandle UISystem::Open(const std::string& InPath, const bool bInPersistent)
    {
        if(const UIDocumentHandle Existing = FindHandle(InPath)) return Existing;
        std::shared_ptr<const UIDocumentAsset> Asset = UIAssetCache::Find(InPath);
        if(!Asset) return UI_DOCUMENT_INVALID;
        return OpenAsset(Asset, InPath, bInPersistent);
    }

    UIDocumentHandle UISystem::OpenAsset(std::shared_ptr<const UIDocumentAsset> InAsset, const std::string& InPath, const bool bInPersistent)
    {
        if(!InAsset) return UI_DOCUMENT_INVALID;
        auto Made = std::make_unique<Document>();
        Made->Instance = std::make_unique<UIDocumentInstance>(InAsset, InPath);
        if(!Made->Instance->IsValid()) return UI_DOCUMENT_INVALID;

        Made->Handle          = NextHandle++;
        Made->Path            = InPath;
        Made->Layer           = InAsset->Settings.Layer;
        Made->Order           = NextOrder++;
        Made->bPersistent     = bInPersistent;
        Made->AssetGeneration = InPath.empty() ? 0 : UIAssetCache::GetGeneration(InPath);
        Made->Instance->SetFonts(&Fonts);
        Made->Instance->SetSystem(this);
        Made->Instance->ApplyBindings(Store);

        RunScriptOnce(*InAsset);
        UIDocumentInstance* Instance = Made->Instance.get();
        const UIDocumentHandle Handle = Made->Handle;
        Documents.push_back(std::move(Made));
        SortDocuments();
        bMergedDirty = true;

        UIActionList OnOpen;
        if(OnOpen.Parse(InAsset->Settings.OnOpen)) { QueueActions(OnOpen, Instance, nullptr); }
        return Handle;
    }

    bool UISystem::Close(const UIDocumentHandle InHandle)
    {
        for(auto It = Documents.begin(); It != Documents.end(); ++It)
        {
            if((*It)->Handle != InHandle) continue;
            UIActionList OnClose;
            const bool bOnClose = OnClose.Parse((*It)->Instance->GetAsset().Settings.OnClose);
            Documents.erase(It);
            bMergedDirty = true;
            if(bOnClose) { QueueActions(OnClose, nullptr, nullptr); }
            return true;
        }
        return false;
    }

    bool UISystem::Close(const std::string& InPath)
    {
        const UIDocumentHandle Handle = FindHandle(InPath);
        return Handle != UI_DOCUMENT_INVALID && Close(Handle);
    }

    void UISystem::CloseAll(const bool bInPersistentToo)
    {
        std::vector<UIDocumentHandle> Closing;
        for(const auto& Each : Documents)
        {
            if(bInPersistentToo || !Each->bPersistent) Closing.push_back(Each->Handle);
        }
        for(const UIDocumentHandle Each : Closing) { Close(Each); }
    }

    bool UISystem::IsOpen(const std::string& InPath) const
    {
        return FindHandle(InPath) != UI_DOCUMENT_INVALID;
    }

    UISystem::Document* UISystem::FindDocument(const UIDocumentHandle InHandle) const
    {
        for(const auto& Each : Documents)
        {
            if(Each->Handle == InHandle) return Each.get();
        }
        return nullptr;
    }

    UIDocumentInstance* UISystem::Find(const UIDocumentHandle InHandle) const
    {
        Document* Found = FindDocument(InHandle);
        return Found ? Found->Instance.get() : nullptr;
    }

    UIDocumentInstance* UISystem::Find(const std::string& InPath) const
    {
        return Find(FindHandle(InPath));
    }

    UIDocumentHandle UISystem::FindHandle(const std::string& InPath) const
    {
        if(InPath.empty()) return UI_DOCUMENT_INVALID;
        for(const auto& Each : Documents)
        {
            if(Each->Path == InPath) return Each->Handle;
        }
        return UI_DOCUMENT_INVALID;
    }

    size_t UISystem::GetDocumentCount() const
    {
        return Documents.size();
    }

    void UISystem::SortDocuments()
    {
        std::stable_sort(Documents.begin(), Documents.end(), [](const std::unique_ptr<Document>& InA, const std::unique_ptr<Document>& InB)
        {
            return InA->Layer != InB->Layer ? InA->Layer < InB->Layer : InA->Order < InB->Order;
        });
    }

    void UISystem::RunScriptOnce(const UIDocumentAsset& InAsset)
    {
        const std::string& Script = InAsset.Settings.Script;
        if(Script.empty()) return;
        if(!Scripts)
        {
            /* The editor's preview runs no scripts, and says so once. */
            if(bExecuteActions) { UI_LOG_WARN("a document's Script '" + Script + "' did not run: no script engine") }
            return;
        }
        if(std::find(ScriptsRun.begin(), ScriptsRun.end(), Script) != ScriptsRun.end()) return;
        ScriptsRun.push_back(Script);
        Scripts->RunFile(Script.c_str());
    }

    void UISystem::Rebuild(Document& InDocument, std::shared_ptr<const UIDocumentAsset> InAsset)
    {
        /* Hot reload: a new instance, the focus kept by name. */
        const UIDocumentInstance& Old = *InDocument.Instance;
        const std::string Focus = Old.GetFocused() >= 0 ? Old.GetWidget(uint32_t(Old.GetFocused()))->Name : std::string();

        auto Made = std::make_unique<UIDocumentInstance>(InAsset, InDocument.Path);
        if(!Made->IsValid()) return;   // logged; the old one stays
        Made->SetFonts(&Fonts);
        Made->SetSystem(this);
        Made->ApplyBindings(Store);
        if(UIWidget* Focused = Made->FindWidget(Focus)) { Made->SetFocus(int32_t(Focused->GetIndex())); }
        InDocument.Instance = std::move(Made);
        InDocument.Layer    = InAsset->Settings.Layer;
        SortDocuments();
        bMergedDirty = true;
        UI_LOG("reloaded '" + InDocument.Path + "'")
    }

    // === Data store ===============================================================

    void UISystem::SetValue(const std::string& InKey, const UIValue& InValue)
    {
        Store.SetValue(InKey, InValue);
    }

    const UIValue* UISystem::GetValue(const std::string& InKey) const
    {
        return Store.Find(InKey);
    }

    // === Input ====================================================================

    UIInputResult UISystem::ProcessInput(const UIInputFrame& InFrame)
    {
        UIInputResult Result;
        bool bPointerTaken = false;   // a document above has the pointer
        bool bBlocked      = false;   // a UIOnly document above takes everything

        for(size_t At = Documents.size(); At-- > 0;)
        {
            Document&           Entry = *Documents[At];
            UIDocumentInstance& Doc   = *Entry.Instance;
            const EUIInputMode Mode = Doc.GetAsset().Settings.InputMode;
            if(bBlocked || Mode == EUIInputMode::GameOnly)
            {
                Doc.SetHovered(-1);
                Doc.SetPressed(-1);
                continue;
            }

            // --- The pointer ------------------------------------------------
            int32_t Hit = -1;
            if(InFrame.bHasMouse && !bPointerTaken)
            {
                if(Entry.HitPaintSerial != Doc.GetPaintSerial() || Entry.HitPointer != InFrame.MousePixels)
                {
                    Entry.Hit            = Doc.HitTest(InFrame.MousePixels);
                    Entry.HitPointer     = InFrame.MousePixels;
                    Entry.HitPaintSerial = Doc.GetPaintSerial();
                }
                Hit = Entry.Hit;
            }
            const int32_t Interactive = Hit >= 0 ? Doc.FindInteractive(Hit) : -1;
            if(Hit >= 0)
            {
                bPointerTaken      = true;
                Result.bOverWidget = true;
                Result.bConsumeMouse = true;
            }
            Doc.SetHovered(Interactive);

            UIWidget* Target = Interactive >= 0 ? Doc.GetWidget(uint32_t(Interactive)) : nullptr;
            if(InFrame.MousePressed[0] && Target && Target->IsEnabledInHierarchy())
            {
                Doc.SetPressed(Interactive);
                Target->OnPointer({ UIPointerEvent::PE_Press, InFrame.MousePixels });
            }
            if(InFrame.MouseReleased[0] && Doc.GetPressed() >= 0)
            {
                /* A click is a press and a release on the same widget. */
                UIWidget* Pressed = Doc.GetWidget(uint32_t(Doc.GetPressed()));
                Pressed->OnPointer({ UIPointerEvent::PE_Release, InFrame.MousePixels });
                if(Doc.GetPressed() == Interactive && Pressed->IsEnabledInHierarchy())
                {
                    Pressed->OnPointer({ UIPointerEvent::PE_Click, InFrame.MousePixels });
                }
                Doc.SetPressed(-1);
                Result.bConsumeMouse = true;
            }
            if(Doc.GetPressed() >= 0) { Result.bConsumeMouse = true; }   // held: captured

            // --- Navigation: a menu's, or any document holding focus ---------
            const bool bNavigates = Mode == EUIInputMode::UIOnly || Doc.GetFocused() >= 0;
            if(bNavigates)
            {
                const bool bDirection = InFrame.bNavUp || InFrame.bNavDown || InFrame.bNavLeft || InFrame.bNavRight;
                if(bDirection || InFrame.bNext || InFrame.bPrevious)
                {
                    if(Doc.GetFocused() < 0)
                    {
                        Doc.SetFocus(Doc.FindNextFocus(InFrame.bPrevious ? -1 : +1));
                    }
                    else if(InFrame.bNext || InFrame.bPrevious)
                    {
                        Doc.SetFocus(Doc.FindNextFocus(InFrame.bPrevious ? -1 : +1));
                    }
                    else
                    {
                        const int Direction = InFrame.bNavUp ? 0 : InFrame.bNavDown ? 1 : InFrame.bNavLeft ? 2 : 3;
                        const int32_t Next = Doc.FindFocusInDirection(Doc.GetFocused(), Direction);
                        if(Next >= 0) Doc.SetFocus(Next);
                    }
                }
                if(InFrame.bAccept && Doc.GetFocused() >= 0)
                {
                    UIWidget* Focused = Doc.GetWidget(uint32_t(Doc.GetFocused()));
                    if(Focused->IsInteractive() && Focused->IsEnabledInHierarchy())
                    {
                        Focused->OnPointer({ UIPointerEvent::PE_Click, UIVec2() });
                    }
                }
                if(InFrame.bBack)
                {
                    UIActionList OnBack;
                    if(OnBack.Parse(Doc.GetAsset().Settings.OnBack) && !OnBack.IsEmpty()) { QueueActions(OnBack, &Doc, nullptr); }
                }
                const bool bUsed = bDirection || InFrame.bNext || InFrame.bPrevious || InFrame.bAccept || InFrame.bBack;
                if(bUsed || Mode == EUIInputMode::UIOnly)
                {
                    Result.bConsumeKeyboard = true;
                    Result.bConsumeGamepad  = true;
                }
            }

            if(Mode == EUIInputMode::UIOnly)
            {
                Result.bConsumeMouse    = true;
                Result.bConsumeKeyboard = true;
                Result.bConsumeGamepad  = true;
                Result.bShowCursor      = true;
                bBlocked = true;
            }
        }
        return Result;
    }

    // === Actions ==================================================================

    void UISystem::QueueActions(const UIActionList& InActions, UIDocumentInstance* InDocument, UIWidget* InSender)
    {
        UIDocumentHandle Handle = UI_DOCUMENT_INVALID;
        for(const auto& Each : Documents)
        {
            if(Each->Instance.get() == InDocument) { Handle = Each->Handle; break; }
        }
        for(const UIAction& Action : InActions.Actions)
        {
            QueuedAction Queued;
            Queued.Action   = Action;
            Queued.Document = Handle;
            Queued.Instance = InDocument;
            if(InSender)
            {
                Queued.Sender      = int32_t(InSender->GetIndex());
                Queued.SenderName  = InSender->Name;
                Queued.SenderValue = InSender->GetActionValue();
            }
            Queue.push_back(std::move(Queued));
        }
    }

    void UISystem::RunQueuedActions()
    {
        /* Actions may queue more (OnOpen of a document an action opens): a few rounds. */
        for(int Round = 0; Round < 8 && !Queue.empty(); ++Round)
        {
            std::vector<QueuedAction> Running;
            Running.swap(Queue);
            for(const QueuedAction& Each : Running) { RunAction(Each); }
        }
    }

    void UISystem::RunAction(const QueuedAction& InAction)
    {
        const UIAction& Action = InAction.Action;
        GameCore::EventArgs Args = Action.Args;
        for(int At = 0; At < Args.Count && At < 4; ++At)
        {
            if(Action.Tokens[At] == UIAction::AT_Self)  { Args.Args[At] = GameCore::EventArg::Make(InAction.SenderName.c_str()); }
            if(Action.Tokens[At] == UIAction::AT_Value) { Args.Args[At] = GameCore::EventArg::Make(InAction.SenderValue); }
        }

        const std::string Text = std::string(KindName(Action.Kind)) + ":" + Action.Target;
        if(ActionLog.size() >= ACTION_LOG_MAX) { ActionLog.erase(ActionLog.begin()); }
        ActionLog.push_back(Text);
        if(!bExecuteActions)
        {
            UI_LOG("preview: " + Text)
            return;
        }

        /* Its document and sender, if both are still the ones that fired it. */
        UIDocumentInstance* Instance = nullptr;
        if(Document* Found = FindDocument(InAction.Document))
        {
            if(Found->Instance.get() == InAction.Instance) Instance = Found->Instance.get();
        }
        UIWidget* Sender = Instance && InAction.Sender >= 0 ? Instance->GetWidget(uint32_t(InAction.Sender)) : nullptr;

        static std::set<std::string> Warned;
        const auto WarnOnce = [](const std::string& InMessage)
        {
            if(Warned.insert(InMessage).second) { UI_LOG_WARN(InMessage) }
        };

        switch(Action.Kind)
        {
            case EUIActionKind::Native:
            {
                const UINativeAction Fn = FindUIAction(Action.Target.c_str());
                if(!Fn)
                {
                    WarnOnce("no native action '" + Action.Target + "' (UI_DEFINE_ACTIONS)");
                    return;
                }
                UIActionContext Context;
                Context.System   = this;
                Context.Document = Instance;
                Context.Sender   = Sender;
                Context.Args     = &Args;
                Context.HostData = HostData;
                Fn(Context);
                return;
            }
            case EUIActionKind::Lua:
                if(!Scripts) { WarnOnce("'" + Text + "' did not run: no script engine"); return; }
                Scripts->CallFunction(Action.Target.c_str(), Args);   // logs its own failure
                return;
            case EUIActionKind::Event:
                if(!World) { WarnOnce("'" + Text + "' did not emit: no World"); return; }
                World->GetEvents().Emit(Action.Target.c_str(), Args);
                return;
        }
    }

    // === Frame ====================================================================

    void UISystem::Tick(const float InDeltaSeconds)
    {
        for(const auto& Each : Documents)
        {
            Document& Doc = *Each;
            if(!Doc.Path.empty())
            {
                const uint64_t Generation = UIAssetCache::GetGeneration(Doc.Path);
                if(Generation != Doc.AssetGeneration)
                {
                    Doc.AssetGeneration = Generation;
                    if(std::shared_ptr<const UIDocumentAsset> Asset = UIAssetCache::Find(Doc.Path)) { Rebuild(Doc, Asset); }
                }
            }
            Doc.Instance->ApplyBindings(Store);
            Doc.Instance->GetAnimator().Tick(InDeltaSeconds);
        }
    }

    void UISystem::Update(const UIVec2& InViewportPixels)
    {
        Viewport = InViewportPixels;
        for(const auto& Each : Documents)
        {
            UIDocumentInstance& Doc = *Each->Instance;
            Doc.Layout(InViewportPixels);
            if(Doc.Paint()) { bMergedDirty = true; }
        }

        if(bMergedDirty)
        {
            /* Rebuilt only on a change; the vectors keep their capacity. */
            Merged.Reset();
            for(const auto& Each : Documents) { Merged.Append(Each->Instance->GetDrawList()); }
            bMergedDirty = false;
            ++MergedSerial;

            if(Renderer && (!Merged.IsEmpty() || bSubmitted))
            {
                Fonts.Upload(*Renderer);   // before the list that names them
                Renderer->SubmitUIDrawList(Scene, Merged);
                bSubmitted = !Merged.IsEmpty();
                ++SubmitCount;
            }
        }
        else if(Renderer)
        {
            Fonts.Upload(*Renderer);
        }
    }

    void UISystem::ClearRenderer()
    {
        if(Renderer && bSubmitted)
        {
            Renderer->SubmitUIDrawList(Scene, RenderCore::UIDrawList());
        }
        bSubmitted   = false;
        bMergedDirty = true;
    }
}

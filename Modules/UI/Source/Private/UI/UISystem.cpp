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
                case EUIActionKind::Graph:  return "Graph";
            }
            return "?";
        }
    }

    void SetScriptUISystem(UISystem* InSystem) { GScriptUISystem = InSystem; }
    UISystem* GetScriptUISystem() { return GScriptUISystem; }

    namespace
    {
        /* While a system calls into Lua, the UI library acts on it -- not on
         * whichever session set the global last (the editor's Preview beside
         * Play-in-Editor). Restored after. */
        struct ScopedScriptUISystem
        {
            explicit ScopedScriptUISystem(UISystem* InSystem) : Previous(GScriptUISystem) { GScriptUISystem = InSystem; }
            ~ScopedScriptUISystem() { GScriptUISystem = Previous; }
            UISystem* Previous;
        };
    }

    struct UISystem::Document
    {
        UIDocumentHandle                    Handle = UI_DOCUMENT_INVALID;
        std::unique_ptr<UIDocumentInstance> Instance;
        std::string                         Path;
        int32_t                             Layer = 0;
        uint64_t                            Order = 0;
        bool                                bPersistent = false;
        uint64_t                            AssetGeneration = 0;

        bool                                bClosing    = false;
        bool                                bFollowCache = true;   // rebuilt when UIAssetCache's copy moves

        /* The last hit test, kept while the pointer and the painted
         * rectangles stay put: a still mouse over a still UI costs nothing. */
        UIVec2                              HitPointer;
        UIVec2                              DragPointer;   // where the pressed widget last heard of the pointer
        uint64_t                            HitPaintSerial = 0;
        UIWidgetRef                         Hit;   // into embedded documents
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
        for(const auto& Each : Documents) { ReleaseTreeGraphObjects(*Each->Instance, false); }
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
        if(InScripts == Scripts) return;
        /* Each document's logic lives in the engine: made again in the new one. */
        for(const auto& Each : Documents) { ReleaseTreeGraphObjects(*Each->Instance, false); }
        ScriptsRun.clear();
        Scripts = InScripts;
        for(const auto& Each : Documents) { CreateTreeGraphObjects(*Each->Instance); }
    }

    void UISystem::CreateGraphObject(UIDocumentInstance& InDoc)
    {
        const std::string& GraphPath = InDoc.GetAsset().Settings.Graph;
        if(GraphPath.empty() || InDoc.GetGraphObject()) return;
        if(!Scripts)
        {
            /* The editor's preview runs no logic; a game without scripts says so. */
            if(bExecuteActions) { UI_LOG_WARN("'" + InDoc.GetPath() + "': its Graph '" + GraphPath + "' did not run: no script engine") }
            return;
        }
        /* self.Document, and self.Prefix: an embedded document's binding keys (the Key node). */
        static const char* const FIELDS[] = { "Document", "Prefix" };
        const GameCore::EventArg Values[] = { GameCore::EventArg::Make(InDoc.GetPath().c_str()),
                                              GameCore::EventArg::Make(InDoc.GetBindingPrefix().c_str()) };
        ScopedScriptUISystem Scope(this);   // the class's chunk runs as it loads
        const GameCore::ScriptEngine::Object Object = Scripts->CreateObject(GraphPath.c_str(), FIELDS, Values, 2);   // logged if it does not load
        if(!Object) return;
        InDoc.SetGraphObject(Object, Scripts->HasObjectMethod(Object, "OnTick"));

        /* OnOpen runs with the other actions: after this frame's input, before it is painted. */
        if(Scripts->HasObjectMethod(Object, "OnOpen"))
        {
            UIActionList OnOpen;
            OnOpen.Parse("Graph:OnOpen");
            QueueActions(OnOpen, &InDoc, nullptr);
        }
    }

    void UISystem::ReleaseGraphObject(UIDocumentInstance& InDoc, const bool bInCallClose)
    {
        const GameCore::ScriptEngine::Object Object = InDoc.GetGraphObject();
        if(!Object) return;
        InDoc.SetGraphObject(0, false);
        if(!Scripts) return;
        if(bInCallClose && bExecuteActions)
        {
            ScopedScriptUISystem Scope(this);
            Scripts->CallObjectMethod(Object, "OnClose");   // absent: nothing
        }
        Scripts->ReleaseObject(Object);
    }

    void UISystem::CreateTreeGraphObjects(UIDocumentInstance& InRoot)
    {
        InRoot.ForEachInstance([this](UIDocumentInstance& InEach) { CreateGraphObject(InEach); });
    }

    void UISystem::ReleaseTreeGraphObjects(UIDocumentInstance& InRoot, const bool bInCallClose)
    {
        /* Embedded documents first: their OnClose before their host's. */
        InRoot.ForEachInstance([this, bInCallClose](UIDocumentInstance& InEach) { ReleaseGraphObject(InEach, bInCallClose); }, true);
    }

    void UISystem::AttachEmbedded(UIDocumentInstance& InEmbedded)
    {
        InEmbedded.ApplyBindings(Store);
        CreateTreeGraphObjects(InEmbedded);
    }

    void UISystem::DetachEmbedded(UIDocumentInstance& InEmbedded)
    {
        ReleaseTreeGraphObjects(InEmbedded, true);
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

    UIDocumentHandle UISystem::OpenAsset(std::shared_ptr<const UIDocumentAsset> InAsset, const std::string& InPath, const bool bInPersistent,
                                         const bool bInFollowCache)
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
        Made->bFollowCache    = bInFollowCache;
        Made->AssetGeneration = InPath.empty() ? 0 : UIAssetCache::GetGeneration(InPath);
        Made->Instance->SetFonts(&Fonts);
        Made->Instance->SetSystem(this);
        Made->Instance->ApplyBindings(Store);

        RunScriptOnce(*InAsset);
        UIDocumentInstance* Instance = Made->Instance.get();
        const UIDocumentHandle Handle = Made->Handle;
        Document& Opened = *Made;
        Documents.push_back(std::move(Made));
        SortDocuments();
        bMergedDirty = true;
        CreateTreeGraphObjects(*Opened.Instance);

        UIActionList OnOpen;
        if(OnOpen.Parse(InAsset->Settings.OnOpen)) { QueueActions(OnOpen, Instance, nullptr); }
        return Handle;
    }

    bool UISystem::Close(const UIDocumentHandle InHandle)
    {
        /* Its logic's OnClose first: it may open or close others (so the
         * document is found again after), not this one again (bClosing). */
        Document* Closing = FindDocument(InHandle);
        if(!Closing || Closing->bClosing) return false;
        Closing->bClosing = true;
        ReleaseTreeGraphObjects(*Closing->Instance, true);

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
        ScopedScriptUISystem Scope(this);
        Scripts->RunFile(Script.c_str());
    }

    void UISystem::Rebuild(Document& InDocument, std::shared_ptr<const UIDocumentAsset> InAsset)
    {
        /* Hot reload: a new instance, the focus kept by name. */
        const UIDocumentInstance& Old = *InDocument.Instance;
        const std::string Focus = Old.GetFocused() >= 0 ? Old.GetWidget(uint32_t(Old.GetFocused()))->Name : std::string();

        auto Made = std::make_unique<UIDocumentInstance>(InAsset, InDocument.Path);
        if(!Made->IsValid()) return;   // logged; the old one stays

        /* Its logic too, read again: a saved graph is picked up with its document. */
        ReleaseTreeGraphObjects(*InDocument.Instance, true);
        if(Scripts && !InAsset->Settings.Graph.empty()) { Scripts->ForgetClass(InAsset->Settings.Graph.c_str()); }
        Made->SetFonts(&Fonts);
        Made->SetSystem(this);
        Made->ApplyBindings(Store);
        if(UIWidget* Focused = Made->FindWidget(Focus)) { Made->SetFocus(int32_t(Focused->GetIndex())); }
        InDocument.Instance = std::move(Made);
        InDocument.Hit            = UIWidgetRef();   // it pointed into the old instance
        InDocument.HitPaintSerial = ~uint64_t(0);
        InDocument.Layer    = InAsset->Settings.Layer;
        CreateTreeGraphObjects(*InDocument.Instance);
        SortDocuments();
        bMergedDirty = true;
        UI_LOG("reloaded '" + InDocument.Path + "'")
    }

    // === Clipboard ================================================================

    void UISystem::SetClipboard(std::function<std::string()> InGet, std::function<void(const std::string&)> InSet)
    {
        ClipboardGet = std::move(InGet);
        ClipboardSet = std::move(InSet);
    }

    std::string UISystem::GetClipboardText() const
    {
        return ClipboardGet ? ClipboardGet() : ClipboardOwn;
    }

    void UISystem::SetClipboardText(const std::string& InText)
    {
        if(ClipboardSet) ClipboardSet(InText);
        else             ClipboardOwn = InText;
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
                Doc.SetTreeHovered(UIWidgetRef());
                Doc.SetTreePressed(UIWidgetRef());
                continue;
            }

            // --- The pointer, into embedded documents (Include) -------------
            UIWidgetRef Hit;
            if(InFrame.bHasMouse && !bPointerTaken)
            {
                if(Entry.HitPaintSerial != Doc.GetPaintSerial() || Entry.HitPointer != InFrame.MousePixels)
                {
                    Entry.Hit            = Doc.HitTestTree(InFrame.MousePixels);
                    Entry.HitPointer     = InFrame.MousePixels;
                    Entry.HitPaintSerial = Doc.GetPaintSerial();
                }
                Hit = Entry.Hit;
            }
            const UIWidgetRef Interactive = Hit.IsValid() ? UIDocumentInstance::FindInteractiveTree(Hit) : UIWidgetRef();
            if(Hit.IsValid())
            {
                bPointerTaken      = true;
                Result.bOverWidget = true;
                Result.bConsumeMouse = true;
            }
            Doc.SetTreeHovered(Interactive);

            /* The wheel: the widget under the pointer, then up its parents (and out of an
             * embedded document to its host's), until one uses it. */
            if(InFrame.Wheel != 0.0f && Hit.IsValid())
            {
                for(UIWidgetRef Up = Hit; Up.Doc;)
                {
                    UIWidget* Each = Up.Doc->GetWidget(uint32_t(Up.Index));
                    if(Each->IsEnabledInHierarchy() && Each->OnWheel(InFrame.Wheel)) { Result.bConsumeMouse = true; break; }
                    const int32_t Parent = Up.Doc->GetParent(uint32_t(Up.Index));
                    if(Parent >= 0)                    { Up.Index = Parent; }
                    else if(Up.Doc->GetParentInstance()) { Up = UIWidgetRef{ Up.Doc->GetParentInstance(), int32_t(Up.Doc->GetHostIndex()) }; }
                    else                               { break; }
                }
            }

            UIWidget* Target = Interactive.IsValid() ? Interactive.Doc->GetWidget(uint32_t(Interactive.Index)) : nullptr;
            const UIWidgetRef Held = Doc.GetTreePressed();
            if(InFrame.MousePressed[0] && Target && Target->IsEnabledInHierarchy())
            {
                Doc.SetTreePressed(Interactive);
                if(Target->bFocusable) { Doc.SetTreeFocus(Interactive); }   // a click focuses what takes focus
                Entry.DragPointer = InFrame.MousePixels;
                Target->OnPointer({ UIPointerEvent::PE_Press, InFrame.MousePixels });
            }
            else if(Held.IsValid() && InFrame.bHasMouse && InFrame.MouseDown[0] && !InFrame.MouseReleased[0]
                    && InFrame.MousePixels != Entry.DragPointer)
            {
                /* Held and moved: the pressed widget hears it, over it or not (a drag). */
                Entry.DragPointer = InFrame.MousePixels;
                Held.Doc->GetWidget(uint32_t(Held.Index))->OnPointer({ UIPointerEvent::PE_Move, InFrame.MousePixels });
            }
            const UIWidgetRef Released = Doc.GetTreePressed();
            if(InFrame.MouseReleased[0] && Released.IsValid())
            {
                /* A click is a press and a release on the same widget. */
                UIWidget* Pressed = Released.Doc->GetWidget(uint32_t(Released.Index));
                Pressed->OnPointer({ UIPointerEvent::PE_Release, InFrame.MousePixels });
                if(Released == Interactive && Pressed->IsEnabledInHierarchy())
                {
                    Pressed->OnPointer({ UIPointerEvent::PE_Click, InFrame.MousePixels });
                }
                Doc.SetTreePressed(UIWidgetRef());
                Result.bConsumeMouse = true;
            }
            if(Doc.GetTreePressed().IsValid()) { Result.bConsumeMouse = true; }   // held: captured

            // --- Navigation: a menu's, or any document holding focus ---------
            const UIWidgetRef Focus = Doc.GetTreeFocus();
            UIWidget* Focused = Focus.IsValid() ? Focus.Doc->GetWidget(uint32_t(Focus.Index)) : nullptr;
            const bool bNavigates = Mode == EUIInputMode::UIOnly || Focused;

            /* A focused text field takes typing and editing keys; Back leaves it. */
            UIInputFrame Nav = InFrame;
            if(Focused && Focused->WantsTextInput() && Focused->IsEnabledInHierarchy())
            {
                Result.bWantsTextInput  = true;
                Result.bConsumeKeyboard = true;
                if(InFrame.bBack) { Doc.SetTreeFocus(UIWidgetRef()); }   // and not the document's OnBack
                else              { Focused->OnTextInput(InFrame); }
                Nav.bNavLeft = Nav.bNavRight = Nav.bAccept = Nav.bBack = false;   // edit keys, not navigation
            }
            if(bNavigates)
            {
                const bool bDirection = Nav.bNavUp || Nav.bNavDown || Nav.bNavLeft || Nav.bNavRight;
                if(bDirection || Nav.bNext || Nav.bPrevious)
                {
                    const UIWidgetRef Now = Doc.GetTreeFocus();
                    if(!Now.IsValid() || Nav.bNext || Nav.bPrevious)
                    {
                        Doc.SetTreeFocus(Doc.FindTreeNextFocus(Nav.bPrevious ? -1 : +1));
                    }
                    else
                    {
                        /* The focused widget first (a slider takes left and right), then focus moves. */
                        const int Direction = Nav.bNavUp ? 0 : Nav.bNavDown ? 1 : Nav.bNavLeft ? 2 : 3;
                        UIWidget* Current = Now.Doc->GetWidget(uint32_t(Now.Index));
                        if(!(Current->IsEnabledInHierarchy() && Current->OnNavigate(Direction)))
                        {
                            const UIWidgetRef Next = Doc.FindTreeFocusInDirection(Now, Direction);
                            if(Next.IsValid()) Doc.SetTreeFocus(Next);
                        }
                    }
                }
                const UIWidgetRef Accepting = Doc.GetTreeFocus();
                if(Nav.bAccept && Accepting.IsValid())
                {
                    UIWidget* Each = Accepting.Doc->GetWidget(uint32_t(Accepting.Index));
                    if(Each->IsInteractive() && Each->IsEnabledInHierarchy())
                    {
                        Each->OnPointer({ UIPointerEvent::PE_Click, UIVec2() });
                    }
                }
                if(Nav.bBack)
                {
                    UIActionList OnBack;
                    if(OnBack.Parse(Doc.GetAsset().Settings.OnBack) && !OnBack.IsEmpty()) { QueueActions(OnBack, &Doc, nullptr); }
                }
                const bool bUsed = bDirection || Nav.bNext || Nav.bPrevious || Nav.bAccept || Nav.bBack;
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
        /* The document it is in -- an embedded one's root's. */
        UIDocumentHandle Handle = UI_DOCUMENT_INVALID;
        const UIDocumentInstance* Root = InDocument ? InDocument->GetRootInstance() : nullptr;
        for(const auto& Each : Documents)
        {
            if(Each->Instance.get() == Root) { Handle = Each->Handle; break; }
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
        ScopedScriptUISystem Scope(this);
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
            if(InAction.Instance && Found->Instance->ContainsInstance(InAction.Instance)) Instance = InAction.Instance;
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
            case EUIActionKind::Graph:
            {
                /* The firing document's own graph instance (an embedded one's, its own): self:Handler(value, sender). */
                const GameCore::ScriptEngine::Object Object = Instance ? Instance->GetGraphObject() : 0;
                if(!Object || !Scripts)
                {
                    if(Instance) { WarnOnce("'" + Text + "' did not run: '" + Instance->GetPath() + "' has no UI graph running"); }
                    return;
                }
                if(!Scripts->HasObjectMethod(Object, Action.Target.c_str()))
                {
                    WarnOnce("'" + Text + "': " + Instance->GetAsset().Settings.Graph + " has no handler " + Action.Target);
                    return;
                }
                GameCore::EventArgs GraphArgs;
                GraphArgs.Args[0] = GameCore::EventArg::Make(InAction.SenderValue);
                GraphArgs.Args[1] = GameCore::EventArg::Make(InAction.SenderName.c_str());
                GraphArgs.Count   = 2;
                Scripts->CallObjectMethod(Object, Action.Target.c_str(), GraphArgs);   // logs its own failure
                return;
            }
        }
    }

    // === Frame ====================================================================

    void UISystem::Tick(const float InDeltaSeconds)
    {
        for(const auto& Each : Documents)
        {
            Document& Doc = *Each;
            if(!Doc.Path.empty() && Doc.bFollowCache)
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

        /* Embedded documents made, reloaded or dropped (Include); the focused widget's tick (a caret's blink). */
        for(size_t At = 0; At < Documents.size(); ++At)
        {
            UIDocumentInstance& Doc = *Documents[At]->Instance;
            Doc.ForEachInstance([](UIDocumentInstance& InEach)
            {
                for(const uint32_t Host : InEach.GetEmbedHosts()) { InEach.GetWidget(Host)->TickEmbedded(); }
            });
            const UIWidgetRef Focus = Doc.GetTreeFocus();
            if(Focus.IsValid() && Focus.Doc->GetWidget(uint32_t(Focus.Index))->OnFocusedTick(InDeltaSeconds)) { Focus.Doc->MarkPaintDirty(); }
        }

        /* Graphs that tick; by handle, as OnTick may open or close documents. */
        if(Scripts && bExecuteActions)
        {
            ScopedScriptUISystem Scope(this);
            GameCore::EventArgs Delta;
            Delta.Args[0] = GameCore::EventArg::Make(double(InDeltaSeconds));
            Delta.Count   = 1;
            std::vector<GameCore::ScriptEngine::Object>& Ticking = TickingScratch;
            Ticking.clear();
            for(const auto& Each : Documents)
            {
                Each->Instance->ForEachInstance([&Ticking](UIDocumentInstance& InEach) { if(InEach.GetGraphTicks()) Ticking.push_back(InEach.GetGraphObject()); });
            }
            for(const GameCore::ScriptEngine::Object Object : Ticking) { Scripts->CallObjectMethod(Object, "OnTick", Delta); }
        }
    }

    void UISystem::Update(const UIVec2& InViewportPixels)
    {
        Viewport = InViewportPixels;

        /* Actions queued since input -- a document opened this frame queues
         * its graph's OnOpen -- run before anything is painted, so a menu's
         * first frame already shows what OnOpen put in the store. */
        if(!Queue.empty())
        {
            RunQueuedActions();
            for(const auto& Each : Documents) { Each->Instance->ApplyBindings(Store); }
        }
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

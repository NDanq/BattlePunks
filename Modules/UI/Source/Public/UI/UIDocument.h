#ifndef UI_UIDOCUMENT_H
#define UI_UIDOCUMENT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/UIDrawList.h"
#include "Public/UI/UIAnimation.h"
#include "Public/UI/UIAPI.h"
#include "Public/UI/UIBinding.h"
#include "Public/UI/UITypes.h"
#include "Public/UI/UIWidget.h"

/*
 * A UI document: the asset -- what a .nmui holds, plain data, shared and
 * never changed once loaded -- and an instance of it: the widgets, laid out,
 * painted and taking input. Like a prefab and its instances.
 *
 * THE ASSET is a flat list of widgets in document order (each after its
 * parent, its subtree contiguous after it), each its type and its properties
 * as text, exactly as the XML has them. Styles are applied when an instance
 * is made (the editor's view) or once for all by the cook.
 *
 * THE INSTANCE makes every widget in one block, in that order, and keeps
 * what changes per frame -- each widget's rectangle, its flags -- in arrays
 * beside them, so layout, paint and hit tests walk memory in order. It lays
 * out only when something moved (UPF_Layout properties, the viewport), paints
 * only when something changed, and keeps its draw list between: an unchanged
 * document costs nothing per frame.
 */
namespace UI
{
    class UIFontCache;
    class UISystem;
    class UIDocumentInstance;

    /* A widget of a document tree: an instance (the root, or one an Include
     * embeds) and its index there. */
    struct UIWidgetRef
    {
        UIDocumentInstance* Doc   = nullptr;
        int32_t             Index = -1;

        bool IsValid() const { return Doc && Index >= 0; }
        bool operator==(const UIWidgetRef& InOther) const { return Doc == InOther.Doc && Index == InOther.Index; }
        bool operator!=(const UIWidgetRef& InOther) const { return !(*this == InOther); }
    };

    struct UIPropertyText
    {
        std::string Name;
        std::string Value;
    };

    struct UI_API UIWidgetDesc
    {
        std::string                 Type;          // an XML element name: "Button"
        int32_t                     Parent = -1;   // an index into the asset's Widgets; -1 the root
        std::vector<UIPropertyText> Properties;    // as written; Name and Class among them

        const std::string* Find(const std::string& InName) const;
        void               Set(const std::string& InName, const std::string& InValue);
        bool               Remove(const std::string& InName);
        std::string        GetName() const;   // its Name property, or ""
    };

    /* <Style Name="MenuButton" Type="Button" .../>: properties a widget takes
     * by naming it in Class. Type "" applies to any widget. */
    struct UIStyle
    {
        std::string                 Name;
        std::string                 Type;
        std::vector<UIPropertyText> Properties;
    };

    /* The root <UI> element's attributes. */
    struct UI_API UIDocumentSettings
    {
        uint32_t     Format        = 1;
        UIVec2       ReferenceSize{ 1920.0f, 1080.0f };
        EUIScaleMode ScaleMode     = EUIScaleMode::FitHeight;
        EUIInputMode InputMode     = EUIInputMode::GameAndUI;
        int32_t      Layer         = 0;     // higher draws over and takes input first
        std::string  Script;                // a .lua run once when it first opens: its handlers
        std::string  Graph;                 // a .ui.nmgraph instanced per open document (Graph: actions)
        std::string  DefaultFont;           // "" the engine's
        std::string  OnOpen;                // actions, as text
        std::string  OnClose;
        std::string  OnBack;                // Back (Escape, gamepad East) while it has input

        /* The attributes in order, as text, and set by name: the XML and the editor's. */
        std::vector<UIPropertyText> ToText() const;
        bool SetFromText(const std::string& InName, const std::string& InValue);
    };

    struct UI_API UIDocumentAsset
    {
        static constexpr uint32_t FORMAT = 1;

        UIDocumentSettings        Settings;
        std::vector<UIStyle>      Styles;
        std::vector<UIWidgetDesc> Widgets;    // document order; [0] the root
        /* RESERVED sections (<Animations>, <VisualStates>), their XML as
         * written: kept through load, save and cook, not run (UIAnimation.h). */
        std::vector<std::string>       Reserved;
        std::vector<UIAnimationAsset>  Animations;   // the <Animations> read, where they parse

        /* -1 if no widget has InName. */
        int32_t FindWidget(const std::string& InName) const;
        /* One past the last widget of InIndex's subtree. */
        int32_t GetSubtreeEnd(int32_t InIndex) const;
        std::vector<int32_t> GetChildren(int32_t InIndex) const;

        /* InIndex's properties with its styles under them (Class, in order;
         * those matching its type), as an instance takes them. */
        std::vector<UIPropertyText> GetEffectiveProperties(int32_t InIndex) const;

        /* Every widget's styles written into it and Styles emptied: the cook's. */
        void ApplyStyles();

        /* Whether this can be instanced: types known, parents in order, child
         * counts allowed. False with OutError. */
        bool Validate(std::string* OutError = nullptr) const;

        // === Editing (the designer's) ==========================================
        // Each keeps document order: a subtree stays contiguous after its root.

        /* Whether InParent may take one more child of type InType (its type's MaxChildren). */
        bool CanTakeChild(int32_t InParent) const;

        /* InIndex's subtree, the first's Parent -1 and the rest relative to it. */
        std::vector<UIWidgetDesc> CopyWidgets(int32_t InIndex) const;

        /* InWidgets (a CopyWidgets fragment) as InParent's child before its
         * child InBefore, or last when InBefore is -1. The fragment's root
         * index; -1 if InParent cannot take it. */
        int32_t InsertWidgets(int32_t InParent, int32_t InBefore, const std::vector<UIWidgetDesc>& InWidgets);

        /* InIndex and its subtree gone; not the root. */
        bool RemoveWidget(int32_t InIndex);

        /* InIndex's subtree to InNewParent, before its child InBefore (-1
         * last). Its new index; -1 if refused (the root, into itself, or a
         * parent with no room). */
        int32_t MoveWidget(int32_t InIndex, int32_t InNewParent, int32_t InBefore);
    };

    /*
     * Loaded documents by path, shared by every instance. Invalidate (the
     * editor's save) bumps the path's generation; UISystem rebuilds the
     * instances of a document whose generation moved.
     */
    class UI_API UIAssetCache
    {
    public:
        /* Null, logged, if the file is missing or does not load. */
        static std::shared_ptr<const UIDocumentAsset> Find(const std::string& InPath);
        static void     Invalidate(const std::string& InPath);   // "" all
        static uint64_t GetGeneration(const std::string& InPath);
    };

    struct UIInputFrame;

    class UI_API UIDocumentInstance
    {
    public:
        explicit UIDocumentInstance(std::shared_ptr<const UIDocumentAsset> InAsset, const std::string& InPath = std::string());
        ~UIDocumentInstance();
        UIDocumentInstance(const UIDocumentInstance&)            = delete;
        UIDocumentInstance& operator=(const UIDocumentInstance&) = delete;

        const UIDocumentAsset& GetAsset() const { return *Asset; }
        const std::shared_ptr<const UIDocumentAsset>& GetAssetPtr() const { return Asset; }
        const std::string& GetPath() const { return Path; }

        /* The system its widgets' actions go to; null outside one (the editor's design view). */
        void      SetSystem(class UISystem* InSystem) { System = InSystem; }
        UISystem* GetSystem() const { return System; }

        /* False if the asset could not be instanced (logged): it then has no widgets. */
        bool IsValid() const { return !Widgets.empty(); }

        // === Widgets ============================================================
        // Index i is the asset's widget i.

        uint32_t  GetWidgetCount() const { return uint32_t(Widgets.size()); }
        UIWidget* GetWidget(uint32_t InIndex) const { return InIndex < Widgets.size() ? Widgets[InIndex] : nullptr; }
        UIWidget* GetRoot() const { return Widgets.empty() ? nullptr : Widgets[0]; }
        UIWidget* FindWidget(const std::string& InName) const;
        int32_t   GetParent(uint32_t InIndex) const      { return InIndex < Widgets.size() ? Parents[InIndex] : -1; }
        int32_t   GetFirstChild(uint32_t InIndex) const  { return InIndex < Widgets.size() ? FirstChildren[InIndex] : -1; }
        int32_t   GetNextSibling(uint32_t InIndex) const { return InIndex < Widgets.size() ? NextSiblings[InIndex] : -1; }
        /* Its children in paint order: document order, by ZOrder in a Canvas. */
        const std::vector<uint32_t>& GetPaintOrder(uint32_t InIndex) const { return PaintOrders[InIndex]; }

        // === Properties ========================================================

        /* Sets InWidget's InName, relaying out or repainting as it needs.
         * False if it has no such property or InValue does not fit. */
        bool    SetProperty(UIWidget& InWidget, const std::string& InName, const UIValue& InValue);
        /* As SetProperty, from text; {Bind:...} text binds instead (UIBinding.h). */
        bool    SetPropertyText(UIWidget& InWidget, const std::string& InName, const std::string& InText);
        UIValue GetProperty(const UIWidget& InWidget, const std::string& InName) const;

        /* An input widget's own change (a check box ticked, a slider dragged):
         * sets InName keeping its binding, and a whole-value binding writes
         * the new value back to the data store -- two-way, so the game and
         * anything else bound to the key see it. False if nothing changed. */
        bool    SetInputValue(UIWidget& InWidget, const std::string& InName, const UIValue& InValue);

        /* An embedded document's marks reach its root: one layout, one paint. */
        void MarkLayoutDirty() { bLayoutDirty = true; bPaintDirty = true; if(ParentInstance) ParentInstance->MarkLayoutDirty(); }
        void MarkPaintDirty()  { bPaintDirty = true; if(ParentInstance) ParentInstance->MarkPaintDirty(); }
        bool IsLayoutDirty() const { return bLayoutDirty; }
        bool IsPaintDirty()  const { return bPaintDirty; }

        // === Layout and paint ==================================================

        void SetFonts(UIFontCache* InFonts) { Fonts = InFonts; MarkLayoutDirty(); }
        UIFontCache* GetFonts() const { return Fonts; }

        /* Lays out for a viewport of InPixels if anything moved; true if it did. */
        bool Layout(const UIVec2& InViewportPixels);

        /* Repaints the draw list if anything changed; true if it did. */
        bool Paint();

        const RenderCore::UIDrawList& GetDrawList() const { return DrawList; }

        /* Bumped by every Paint that changed the list. */
        uint64_t GetPaintSerial() const { return PaintSerial; }

        float  GetScale()      const { return Scale; }
        UIVec2 GetLayoutSize() const { return LayoutSize; }   // the root's, layout units
        UIVec2 GetViewport()   const { return Viewport; }

        /* Its rectangle in layout units (document space), and in pixels as
         * last painted (render transforms included). */
        const UIRect& GetRect(uint32_t InIndex) const { return Rects[InIndex]; }
        const UIRect& GetPixelRect(uint32_t InIndex) const { return PixelRects[InIndex]; }
        const UIVec2& GetDesiredSize(uint32_t InIndex) const { return Desired[InIndex]; }

        /* Shown: neither it nor an ancestor Hidden, Collapsed or on a hidden
         * Switcher page. */
        bool IsShown(uint32_t InIndex) const;

        /* For UILayoutContext. */
        UIVec2 Measure(UILayoutContext& InContext, uint32_t InIndex);
        void   Arrange(UILayoutContext& InContext, uint32_t InIndex, const UIRect& InRect);

        // === Input =============================================================

        /* The deepest widget under InPixels that takes hits (its Visibility),
         * topmost first; -1 none. bInDesign: every shown widget (the editor). */
        int32_t HitTest(const UIVec2& InPixels, bool bInDesign = false) const;

        /* InIndex or its nearest ancestor that IsInteractive; -1 none. */
        int32_t FindInteractive(int32_t InIndex) const;

        int32_t GetHovered() const { return Hovered; }
        int32_t GetPressed() const { return Pressed; }
        int32_t GetFocused() const { return Focused; }

        /* Moves focus to InIndex (-1 none) if it is focusable and enabled; repaints. */
        bool SetFocus(int32_t InIndex);

        /* The next focusable widget in document order (InStep +1 or -1), wrapping. */
        int32_t FindNextFocus(int32_t InStep) const;

        /* The focusable widget nearest InFrom in a direction (0 up, 1 down, 2
         * left, 3 right): its Nav* name if set, else the nearest by geometry. */
        int32_t FindFocusInDirection(int32_t InFrom, int InDirection) const;

        /* UISystem's pointer routing: new hover and press states, repainting
         * the widgets whose look changed. */
        void SetHovered(int32_t InIndex);
        void SetPressed(int32_t InIndex);

        // === Bindings ==========================================================

        const std::vector<UIBinding>& GetBindings() const { return Bindings; }

        /* Each binding's value from InStore, if the store moved since the last
         * call. True if any property changed. */
        bool ApplyBindings(const UIDataStore& InStore);

        // === Embedding (Include) ===============================================
        /*
         * A document shown inside a widget of another (UIInclude): laid out in
         * its host's rectangle at its root's scale, painted into its root's
         * list, its dirty marks its root's. Its {Bind:Key} reads InPrefix + Key
         * ({Bind:/Key} the key as written); its UI graph is its own.
         */
        void SetEmbedded(UIDocumentInstance* InParent, uint32_t InHost, const std::string& InPrefix);
        UIDocumentInstance* GetParentInstance() const { return ParentInstance; }
        uint32_t            GetHostIndex() const      { return HostIndex; }
        const std::string&  GetBindingPrefix() const  { return BindingPrefix; }
        /* A new prefix: its bindings read again on the next ApplyBindings (a reused list row). */
        void                SetBindingPrefix(const std::string& InPrefix) { BindingPrefix = InPrefix; BindingsVersion = 0; MarkPaintDirty(); }
        UIDocumentInstance* GetRootInstance();
        /* How deep it is embedded: 0 a root. */
        int                 GetEmbedDepth() const;
        /* InOther is this instance or one embedded in it, at any depth. */
        bool                ContainsInstance(const UIDocumentInstance* InOther) const;

        /* This instance, then each embedded one in document order (InVisit
         * after them instead, with bInPostOrder). */
        template<typename TVisit>
        void ForEachInstance(TVisit&& InVisit, const bool bInPostOrder = false)
        {
            if(!bInPostOrder) InVisit(*this);
            for(const uint32_t Host : EmbedHosts)
            {
                const UIWidget& W = *Widgets[Host];
                for(uint32_t At = 0; At < W.GetEmbeddedCount(); ++At)
                {
                    if(UIDocumentInstance* Inner = W.GetEmbeddedAt(At)) Inner->ForEachInstance(InVisit, bInPostOrder);
                }
            }
            if(bInPostOrder) InVisit(*this);
        }
        const std::vector<uint32_t>& GetEmbedHosts() const { return EmbedHosts; }

        /* The key a binding names: InKey after its prefix, or as written after a '/'. */
        std::string ResolveKey(const std::string& InKey) const;

        /* For UIInclude: its root measured, arranged and painted as part of the host's. */
        UIVec2 MeasureEmbedded(const UILayoutContext& InOuter);
        void   ArrangeEmbedded(const UILayoutContext& InOuter, const UIRect& InRect);
        void   PaintEmbedded(UIPaintContext& InOuter);

        // --- The whole tree (call on the root) ------------------------------

        /* The deepest widget under InPixels, into embedded documents; and the
         * interactive widget it belongs to, out of them again if need be. */
        UIWidgetRef HitTestTree(const UIVec2& InPixels);
        static UIWidgetRef FindInteractiveTree(UIWidgetRef InHit);

        /* The focused widget anywhere in the tree; focus there (and nowhere
         * else); the next focusable in document order, embedded ones at their
         * host's place; the nearest one in a direction. */
        UIWidgetRef GetTreeFocus();
        bool        SetTreeFocus(UIWidgetRef InFocus);
        UIWidgetRef FindTreeNextFocus(int32_t InStep);
        UIWidgetRef FindTreeFocusInDirection(UIWidgetRef InFrom, int InDirection);

        /* Hover and press, in at most one instance of the tree. */
        void        SetTreeHovered(UIWidgetRef InHover);
        void        SetTreePressed(UIWidgetRef InPressed);
        UIWidgetRef GetTreePressed();

        /* Its UI graph's script object (UISystem's bookkeeping). */
        int32_t GetGraphObject() const { return GraphObject; }
        bool    GetGraphTicks() const  { return bGraphTicks; }
        void    SetGraphObject(int32_t InObject, bool bInTicks) { GraphObject = InObject; bGraphTicks = bInTicks; }

        // === Animation (RESERVED: UIAnimation.h) ==============================

        /* False, warned once: animations do not play yet. */
        bool PlayAnimation(const std::string& InName);
        UIAnimator& GetAnimator() { return Animator; }

    private:
        void Build();
        void SortPaintOrder(uint32_t InIndex);
        /* Its own Visibility, and its parent showing it (IsChildShown). */
        bool IsSelfShown(uint32_t InIndex) const;
        void KeepFocusShown();
        void PaintWidget(uint32_t InIndex, UIPaintContext& InContext);
        int32_t HitTestWidget(uint32_t InIndex, const UIVec2& InPixels, const RenderCore::UIClipRect& InClip, bool bInDesign) const;
        bool ApplyProperty(UIWidget& InWidget, int32_t InProperty, const UIValue& InValue);

        std::shared_ptr<const UIDocumentAsset> Asset;
        std::string                            Path;
        UISystem*                              System = nullptr;

        // The widgets, made in Block in document order.
        std::unique_ptr<uint8_t[]> Block;
        std::vector<UIWidget*>     Widgets;

        // Per widget, by index.
        std::vector<int32_t>               Parents, FirstChildren, NextSiblings;
        std::vector<std::vector<uint32_t>> PaintOrders;
        std::vector<UIRect>                Rects;
        std::vector<UIRect>                PixelRects;
        std::vector<UIVec2>                Desired;
        std::vector<uint8_t>               Measured;   // this layout pass

        std::vector<UIBinding> Bindings;
        uint64_t               BindingsVersion = 0;

        UIFontCache*           Fonts = nullptr;
        RenderCore::UIDrawList DrawList;
        uint64_t               PaintSerial  = 0;
        bool                   bLayoutDirty = true;
        bool                   bPaintDirty  = true;
        float                  Scale        = 1.0f;
        UIVec2                 LayoutSize;
        UIVec2                 Viewport;

        int32_t Hovered = -1, Pressed = -1, Focused = -1;

        UIAnimator Animator;
        bool       bWarnedAnimation = false;

        // Embedding (Include): its host, and the documents it hosts.
        UIDocumentInstance*   ParentInstance = nullptr;
        uint32_t              HostIndex      = 0;
        std::string           BindingPrefix;
        std::vector<uint32_t> EmbedHosts;          // its widgets that host a document
        int32_t               GraphObject = 0;     // its UI graph's script object (UISystem's); 0 none
        bool                  bGraphTicks = false;
    };
}

#endif //UI_UIDOCUMENT_H

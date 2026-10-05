#ifndef UI_UIWIDGET_H
#define UI_UIWIDGET_H

#include <cstdint>
#include <string>

#include "Public/RenderCore/UIDrawList.h"
#include "Public/UI/UIAction.h"
#include "Public/UI/UIAPI.h"
#include "Public/UI/UIProperty.h"
#include "Public/UI/UITypes.h"

/*
 * A widget: one element of a document. Behaviour is virtual -- how big it
 * wants to be, where its children go, what it paints, what a pointer does --
 * and its per-frame state (where it ended up, whether it is shown) lives in
 * its document's arrays, not here (UIDocumentInstance). Widgets are made by
 * their document, in one block, in document order; never new one yourself.
 *
 * SLOT PROPERTIES. Where a widget goes is its own properties, read by its
 * parent according to what the parent is:
 *
 *   in a Canvas:  Anchors (min X, min Y, max X, max Y, 0-1 of the parent),
 *                 Position, Size, Pivot, AutoSize, Margin, ZOrder. On an axis
 *                 whose anchors meet, the widget is Size (or its desired size,
 *                 AutoSize) at the anchor plus Position, less Pivot of its
 *                 size. On an axis whose anchors differ it stretches between
 *                 them, inset by Margin.
 *   in a box, an Overlay, a Border or a Button:
 *                 Margin (space round it), HAlign, VAlign (Fill stretches),
 *                 and, along a box's axis, Fill: 0 its desired size, else a
 *                 weight of the space left over.
 */
namespace UI
{
    class UIDocumentInstance;
    class UIFontCache;

    /* What Measure and Arrange are given. */
    struct UILayoutContext
    {
        UIDocumentInstance* Document = nullptr;
        UIFontCache*        Fonts    = nullptr;   // null: text measures empty
        float               Scale    = 1.0f;      // physical pixels per layout unit

        /* InIndex's desired size (UIDocumentInstance::Measure). */
        UI_API UIVec2 Measure(uint32_t InIndex);
        /* Places InIndex, layout units in document space. */
        UI_API void   Arrange(uint32_t InIndex, const UIRect& InRect);
    };

    /* What Paint is given: a draw list in physical pixels and the mapping to it. */
    struct UI_API UIPaintContext
    {
        RenderCore::UIDrawList* List    = nullptr;
        UIFontCache*            Fonts   = nullptr;
        UIDocumentInstance*     Document = nullptr;
        float                   Scale   = 1.0f;   // the document's: physical pixels per layout unit (font sizes, margins)
        UIVec2                  PixelScale{ 1.0f, 1.0f };   // per axis, render transforms included
        UIVec2                  Offset;           // pixels, added after PixelScale
        float                   Opacity = 1.0f;   // the widget's, times its ancestors'

        /* A layout rectangle in pixels, edges snapped to whole pixels. */
        UIRect ToPixels(const UIRect& InLayout) const;

        /* InBrush over InLayout. */
        void DrawBrush(const UIBrush& InBrush, const UIRect& InLayout) const;
    };

    struct UIPointerEvent
    {
        enum EKind : uint8_t { PE_Enter, PE_Leave, PE_Press, PE_Release, PE_Click };
        EKind Kind = PE_Enter;
        UIVec2 Pixels;
    };

    class UI_API UIWidget
    {
    public:
        UIWidget() = default;
        virtual ~UIWidget() = default;
        UIWidget(const UIWidget&)            = delete;
        UIWidget& operator=(const UIWidget&) = delete;

        // === Properties (UI_DEFINE_ABSTRACT_WIDGET(UIWidget) registers them) ===

        std::string   Name;
        std::string   Class;                  // the styles it took (editor; a cooked file has them applied)
        EUIVisibility Visibility = EUIVisibility::Visible;
        bool          bEnabled   = true;
        float         Opacity    = 1.0f;
        UIVec2        RenderTranslate;        // drawn moved; layout and siblings unaffected
        UIVec2        RenderScale{ 1.0f, 1.0f };   // drawn scaled about its centre
        bool          bClipChildren = false;
        bool          bFocusable    = false;
        bool          bInitialFocus = false;
        std::string   NavUp, NavDown, NavLeft, NavRight;   // a widget's Name; "" finds the nearest

        // Slot (see above).
        UIMargin  Anchors;                    // 0,0,0,0: the parent's top-left
        UIVec2    Position;
        UIVec2    Size{ 100.0f, 100.0f };
        UIVec2    Pivot;
        bool      bAutoSize = false;
        int32_t   ZOrder    = 0;
        UIMargin  Margin;
        EUIHAlign HAlign = EUIHAlign::Fill;
        EUIVAlign VAlign = EUIVAlign::Fill;
        float     Fill   = 0.0f;

        // === Behaviour ===========================================================

        /* Its desired size, layout units, children measured through InContext. */
        virtual UIVec2 MeasureContent(UILayoutContext& InContext);

        /* Places its children inside InRect (its own, arranged). */
        virtual void ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect);

        /* Paints itself into InRect (layout units); its children paint after. */
        virtual void Paint(UIPaintContext& InContext, const UIRect& InRect) const;

        /* Whether pointer input stops here (a button), rather than at an ancestor. */
        virtual bool IsInteractive() const { return false; }

        /* Whether it is hit itself where it is shown (its Visibility allowing):
         * something drawn, or interactive. Layout containers are not, so a
         * HUD's full-screen root lets clicks through to the game. */
        virtual bool TakesHits() const { return false; }

        /* Whether its child InChild (a document index) is shown: drawn, hit
         * and focusable. All of them, but a Switcher's inactive pages. */
        virtual bool IsChildShown(uint32_t /*InChild*/) const { return true; }

        /* Hover, press, release, click: UIDocumentInstance's pointer routing.
         * An interactive widget queues its actions here (QueueActions). */
        virtual void OnPointer(const UIPointerEvent& /*InEvent*/) {}

        /* InActions to its document's UISystem, fired by this widget. */
        void QueueActions(const UIActionList& InActions);

        /* A slider's or a check box's value, for $value; 0 by default. */
        virtual double GetActionValue() const { return 0.0; }

        /* A property was set: drop anything cached from it (a Text's glyphs). */
        virtual void OnPropertyChanged(const UIPropertyDesc& /*InProperty*/) {}

        /* UIVisualState.h's: records InState; transitions are not played yet. */
        void GoToState(const char* InState);
        const std::string& GetVisualState() const { return VisualState; }

        // === Document ============================================================

        const UIWidgetType* GetType()     const { return Type; }
        UIDocumentInstance* GetDocument() const { return Document; }
        uint32_t            GetIndex()    const { return Index; }

        /* Whether pointer input or focus would act on it now. */
        bool IsHovered() const;
        bool IsPressed() const;
        bool IsFocused() const;
        bool IsEnabledInHierarchy() const;

    private:
        friend class UIDocumentInstance;
        const UIWidgetType* Type     = nullptr;
        UIDocumentInstance* Document = nullptr;
        uint32_t            Index    = 0;
        std::string         VisualState;
    };
}

#endif //UI_UIWIDGET_H

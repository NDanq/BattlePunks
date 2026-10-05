#ifndef UI_UIWIDGETS_H
#define UI_UIWIDGETS_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/UI/UIWidget.h"

/*
 * The built-in widgets. Each is registered in UIWidgets.cpp, which is the
 * list of its properties as XML and the editor see them.
 *
 *   Canvas         children placed by anchors (the usual root)
 *   Border         a brush behind one child, inset by Padding (a panel)
 *   Image          a brush, sized to its texture unless told otherwise
 *   Text           a string in a font; {Bind:Key} in it is filled from the data store
 *   Button         Border's look per state (normal, hovered, pressed, disabled) and OnClick
 *   HorizontalBox  children in a row
 *   VerticalBox    children in a column
 *   Overlay        children stacked over each other
 *   Switcher       pages, one shown at a time (a settings menu's tabs)
 *   ProgressBar    a fill over a background, Value 0-1
 *   Spacer         empty space of a size
 */
namespace UI
{
    class UIFont;

    class UI_API UICanvas : public UIWidget
    {
    public:
        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect) override;
    };

    /* The shared part of Border and Button: one child inside Padding. */
    class UI_API UIContentWidget : public UIWidget
    {
    public:
        UIMargin Padding;

        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect) override;
    };

    class UI_API UIBorder : public UIContentWidget
    {
    public:
        UIBrush Background;

        void Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        bool TakesHits() const override { return Background.DrawAs != EUIDrawAs::None; }
    };

    class UI_API UIImage : public UIWidget
    {
    public:
        UIBrush    Brush;
        UIVec2     DesiredSize;     // 0,0: the texture's size
        EUIStretch Stretch = EUIStretch::Fill;

        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        bool   TakesHits() const override { return Brush.DrawAs != EUIDrawAs::None; }
    };

    class UI_API UIText : public UIWidget
    {
    public:
        std::string Text;
        std::string Font;                 // a .ttf; "" the default (UIFontCache)
        float       FontSize = 24.0f;     // layout units
        UIColor     Color;
        EUIJustify  Justify = EUIJustify::Left;
        bool        bWrap   = false;
        float       LineHeight = 1.0f;    // times the font's
        UIVec2      ShadowOffset;
        UIColor     ShadowColor{ 0.0f, 0.0f, 0.0f, 0.0f };

        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        void   OnPropertyChanged(const UIPropertyDesc& InProperty) override;

    private:
        /* The glyphs, laid out in pixels from the text's top-left; rebuilt
         * when the text, font, size, scale or wrap width changes. */
        void EnsureGlyphs(UIFontCache* InFonts, float InScale, float InWrapPixels) const;

        mutable std::vector<RenderCore::UIVertex> Glyphs;
        mutable UIVec2      GlyphSize;            // pixels
        mutable std::string GlyphTexture;
        mutable float       GlyphScale = 0.0f;    // what they were laid out for; 0 none
        mutable float       GlyphWrap  = -1.0f;
        mutable uint64_t    GlyphFontGeneration = 0;
        mutable bool        bGlyphsValid = false;
    };

    class UI_API UIButton : public UIContentWidget
    {
    public:
        UIBrush      Normal;
        UIBrush      Hovered;
        UIBrush      Pressed;
        UIBrush      Disabled;
        UIActionList OnClick;
        UIActionList OnPressed;
        UIActionList OnReleased;
        UIActionList OnHovered;
        UIActionList OnUnhovered;

        UIButton();
        void Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        bool IsInteractive() const override { return true; }
        bool TakesHits() const override { return true; }
        void OnPointer(const UIPointerEvent& InEvent) override;
    };

    /* HorizontalBox and VerticalBox: children along one axis. */
    class UI_API UIBoxWidget : public UIWidget
    {
    public:
        float Spacing = 0.0f;

        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect) override;

    protected:
        bool bVertical = false;
    };

    class UI_API UIHorizontalBox : public UIBoxWidget
    {
    public:
        UIHorizontalBox() { bVertical = false; }
    };

    class UI_API UIVerticalBox : public UIBoxWidget
    {
    public:
        UIVerticalBox() { bVertical = true; }
    };

    class UI_API UIOverlay : public UIWidget
    {
    public:
        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect) override;
    };

    /*
     * Pages: its children, all laid out over each other in its rectangle,
     * one shown at a time -- the others are not drawn, hit or focused. Sized
     * by its largest page, so switching does not move what is around it.
     * ActiveIndex binds ({Bind:Settings.Page}); UI.ShowPage picks one by name.
     */
    class UI_API UISwitcher : public UIWidget
    {
    public:
        int32_t ActiveIndex = 0;   // its children in document order; clamped

        bool IsChildShown(uint32_t InChild) const override;

        /* The page shown, a document index; -1 with no pages. */
        int32_t GetActiveChild() const;
        int32_t GetPageCount() const;

        /* Shows the page named InPage (a child's Name), or numbered (0 first).
         * False, nothing changed, if it has no such page. */
        bool ShowPage(const std::string& InPage);
    };

    class UI_API UIProgressBar : public UIWidget
    {
    public:
        float            Value = 0.5f;   // 0-1
        UIBrush          Background;
        UIBrush          FillBrush;
        EUIFillDirection FillDirection = EUIFillDirection::LeftToRight;

        UIProgressBar();
        void Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        bool TakesHits() const override { return true; }
    };

    class UI_API UISpacer : public UIWidget
    {
    public:
        UIVec2 MeasureContent(UILayoutContext& InContext) override;
    };

    /* Where InChild goes in InSlot (a box, an Overlay, a Border's content):
     * its Margin, HAlign and VAlign against its desired size. */
    UI_API UIRect AlignInSlot(const UIWidget& InChild, const UIVec2& InDesired, const UIRect& InSlot);
}

#endif //UI_UIWIDGETS_H

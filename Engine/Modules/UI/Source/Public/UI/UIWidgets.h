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
 *   CheckBox       a box ticked on and off, its child a label beside it
 *   Slider         a thumb dragged along a track, Value from Min to Max
 *   ScrollBox      its one child, longer than it, scrolled by wheel, bar or focus
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

    /*
     * A box that ticks on and off, clicked or Accepted, with its one child (a
     * label) beside it -- the label clicks it too. Checked binds two-way:
     * Checked="{Bind:Settings.Shadows}" shows the store's value, and a click
     * writes the new one back. OnChanged fires after, $value 1 or 0.
     */
    class UI_API UICheckBox : public UIContentWidget
    {
    public:
        bool         bChecked = false;
        float        BoxSize  = 32.0f;   // layout units, square
        float        Spacing  = 16.0f;   // between the box and the label
        UIBrush      Box;
        UIBrush      BoxHovered;         // hovered or focused
        UIBrush      Check;              // the tick, inset in the box
        UIActionList OnChanged;

        UICheckBox();
        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect) override;
        void   Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        bool   IsInteractive() const override { return true; }
        bool   TakesHits() const override { return true; }
        void   OnPointer(const UIPointerEvent& InEvent) override;
        double GetActionValue() const override { return bChecked ? 1.0 : 0.0; }
    };

    /*
     * A thumb along a horizontal track: pressed or dragged it jumps to the
     * pointer, and while focused Left and Right step it (by Step, or a
     * twentieth of the range). Value snaps to Min + n * Step and binds
     * two-way, as a CheckBox's Checked. OnChanged fires on each change, $value
     * the new value. It asks a box for its Size.
     */
    class UI_API UISlider : public UIWidget
    {
    public:
        float        Value = 0.5f;
        float        Min   = 0.0f;
        float        Max   = 1.0f;
        float        Step  = 0.0f;           // 0: continuous
        float        TrackHeight = 8.0f;     // layout units
        UIVec2       ThumbSize{ 20.0f, 36.0f };
        UIBrush      Track;
        UIBrush      FillBrush;              // the track up to the thumb
        UIBrush      Thumb;
        UIBrush      ThumbHovered;           // hovered, focused or dragged
        UIActionList OnChanged;

        UISlider();
        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   Paint(UIPaintContext& InContext, const UIRect& InRect) const override;
        bool   IsInteractive() const override { return true; }
        bool   TakesHits() const override { return true; }
        void   OnPointer(const UIPointerEvent& InEvent) override;
        bool   OnNavigate(int InDirection) override;
        double GetActionValue() const override { return double(Value); }

        /* InValue clamped to Min-Max and snapped to Step. */
        float Snap(double InValue) const;

        /* 0-1: where Value is along the range. */
        float GetFraction() const;

    private:
        /* Sets Value as the player's input (two-way) and fires OnChanged if it moved. */
        void ChangeTo(double InValue);
    };

    /*
     * Scrolling, shared by ScrollBox and ListView: an offset into content
     * longer than the view, a bar to drag, and the arithmetic between them.
     */
    struct UI_API UIScrollState
    {
        float ContentLength = 0.0f;   // layout units, along the axis
        float ViewLength    = 0.0f;

        float MaxOffset() const { return ContentLength > ViewLength ? ContentLength - ViewLength : 0.0f; }
        bool  CanScroll() const { return MaxOffset() > 0.0f; }

        /* The thumb's start and length along a track of InTrack, at InOffset. */
        void Thumb(float InOffset, float InTrack, float& OutStart, float& OutLength) const;

        /* The offset that puts the thumb's start at InThumbStart. */
        float OffsetForThumb(float InThumbStart, float InTrack) const;

        /* The offset closest to InOffset that shows [InStart, InStart + InLength). */
        float Reveal(float InOffset, float InStart, float InLength) const;
    };

    /*
     * Its one child at full length, seen through its own rectangle: the wheel,
     * the bar (dragged, or clicked beside the thumb for a page) or focus moving
     * to something out of view scrolls it. It asks a box for its Size (Fill
     * stretches it). ScrollOffset binds two-way: {Bind:Menu.Scroll} remembers
     * where a list was. Children past the edge are clipped, so neither drawn
     * nor clicked.
     */
    class UI_API UIScrollBox : public UIWidget
    {
    public:
        float            ScrollOffset = 0.0f;   // layout units from the start; clamped
        float            ScrollStep   = 60.0f;  // per wheel notch
        EUIOrientation   Orientation  = EUIOrientation::Vertical;
        EUIScrollBarMode ScrollBar    = EUIScrollBarMode::Auto;
        float            BarWidth     = 8.0f;
        UIBrush          Track;
        UIBrush          Thumb;
        UIBrush          ThumbHovered;          // hovered or dragged
        UIActionList     OnScrolled;            // $value the offset

        UIScrollBox();
        UIVec2 MeasureContent(UILayoutContext& InContext) override;
        void   ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect) override;
        void   PaintOver(UIPaintContext& InContext, const UIRect& InRect) const override;
        UIVec2 GetChildOffset() const override;
        bool   IsInteractive() const override { return true; }
        bool   TakesHits() const override { return true; }
        bool   OnWheel(float InDelta) override;
        void   OnPointer(const UIPointerEvent& InEvent) override;
        void   OnDescendantFocused(uint32_t InIndex) override;
        double GetActionValue() const override { return double(ScrollOffset); }

        /* Scrolls to InOffset (clamped), as the player would: two-way, OnScrolled. False if it did not move. */
        bool ScrollTo(float InOffset);

        const UIScrollState& GetScroll() const { return Scroll; }

    private:
        bool   IsVertical() const { return Orientation == EUIOrientation::Vertical; }
        bool   ShowsBar() const;
        /* The bar's track, layout units. */
        UIRect TrackRect(const UIRect& InRect) const;

        UIScrollState Scroll;
        float         DragGrab = -1.0f;   // where on the thumb it was taken, pixels; < 0 not dragging
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

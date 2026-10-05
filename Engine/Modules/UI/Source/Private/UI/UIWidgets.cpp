#include "Public/UI/UIWidgets.h"

#include <algorithm>
#include <cmath>

#include "Public/UI/UIDocument.h"
#include "Public/UI/UIFont.h"
#include "Public/UI/UIInput.h"
#include "Public/UI/UILog.h"
#include "Public/UI/UISystem.h"
#include "Private/UI/UIInternal.h"

namespace UI
{
    namespace
    {
        /* The extent InChild asks of a Canvas on one axis: its size where its
         * anchors meet, past its position less its pivot. */
        float CanvasExtent(const float InAnchorMin, const float InAnchorMax, const float InPosition, const float InSize,
                           const float InPivot, const float InMarginA, const float InMarginB)
        {
            if(InAnchorMin == InAnchorMax)
            {
                return std::max(InPosition + InSize * (1.0f - InPivot), 0.0f);
            }
            return InMarginA + InMarginB;
        }

        /* One axis of a Canvas child: where it starts and how long it is. */
        void CanvasAxis(const float InParentStart, const float InParentLength, const float InAnchorMin, const float InAnchorMax,
                        const float InPosition, const float InSize, const float InPivot, const float InMarginA, const float InMarginB,
                        float& OutStart, float& OutLength)
        {
            const float A0 = InParentStart + InParentLength * InAnchorMin;
            const float A1 = InParentStart + InParentLength * InAnchorMax;
            if(InAnchorMin == InAnchorMax)
            {
                OutLength = InSize;
                OutStart  = A0 + InPosition - InPivot * InSize;
            }
            else
            {
                OutStart  = A0 + InMarginA;
                OutLength = std::max(A1 - InMarginB - OutStart, 0.0f);
            }
        }

        /* A box child's Margin along and across the axis. */
        float MainMargin(const UIWidget& InChild, const bool bInVertical)
        {
            return bInVertical ? InChild.Margin.Top + InChild.Margin.Bottom : InChild.Margin.Left + InChild.Margin.Right;
        }
        float CrossMargin(const UIWidget& InChild, const bool bInVertical)
        {
            return bInVertical ? InChild.Margin.Left + InChild.Margin.Right : InChild.Margin.Top + InChild.Margin.Bottom;
        }

        int32_t FirstChild(const UIWidget& InWidget)
        {
            return InWidget.GetDocument() ? InWidget.GetDocument()->GetFirstChild(InWidget.GetIndex()) : -1;
        }
    }

    UIRect AlignInSlot(const UIWidget& InChild, const UIVec2& InDesired, const UIRect& InSlot)
    {
        const float X = InSlot.X + InChild.Margin.Left, Y = InSlot.Y + InChild.Margin.Top;
        const float W = std::max(InSlot.W - InChild.Margin.Left - InChild.Margin.Right, 0.0f);
        const float H = std::max(InSlot.H - InChild.Margin.Top - InChild.Margin.Bottom, 0.0f);
        UIRect Out{ X, Y, W, H };
        switch(InChild.HAlign)
        {
            case EUIHAlign::Fill:   break;
            case EUIHAlign::Left:   Out.W = InDesired.X; break;
            case EUIHAlign::Center: Out.W = InDesired.X; Out.X = X + (W - InDesired.X) * 0.5f; break;
            case EUIHAlign::Right:  Out.W = InDesired.X; Out.X = X + W - InDesired.X; break;
        }
        switch(InChild.VAlign)
        {
            case EUIVAlign::Fill:   break;
            case EUIVAlign::Top:    Out.H = InDesired.Y; break;
            case EUIVAlign::Center: Out.H = InDesired.Y; Out.Y = Y + (H - InDesired.Y) * 0.5f; break;
            case EUIVAlign::Bottom: Out.H = InDesired.Y; Out.Y = Y + H - InDesired.Y; break;
        }
        return Out;
    }

    // === Canvas ===================================================================

    UIVec2 UICanvas::MeasureContent(UILayoutContext& InContext)
    {
        UIVec2 Size;
        UIDocumentInstance* Doc = GetDocument();
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            const UIVec2 Desired = InContext.Measure(uint32_t(Child));
            const UIVec2 Own = C.bAutoSize ? Desired : C.Size;
            Size.X = std::max(Size.X, CanvasExtent(C.Anchors.Left, C.Anchors.Right, C.Position.X, Own.X, C.Pivot.X, C.Margin.Left, C.Margin.Right));
            Size.Y = std::max(Size.Y, CanvasExtent(C.Anchors.Top, C.Anchors.Bottom, C.Position.Y, Own.Y, C.Pivot.Y, C.Margin.Top, C.Margin.Bottom));
        }
        return Size;
    }

    void UICanvas::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        UIDocumentInstance* Doc = GetDocument();
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            const UIVec2 Own = C.bAutoSize ? Doc->GetDesiredSize(uint32_t(Child)) : C.Size;
            UIRect Rect;
            CanvasAxis(InRect.X, InRect.W, C.Anchors.Left, C.Anchors.Right, C.Position.X, Own.X, C.Pivot.X,
                       C.Margin.Left, C.Margin.Right, Rect.X, Rect.W);
            CanvasAxis(InRect.Y, InRect.H, C.Anchors.Top, C.Anchors.Bottom, C.Position.Y, Own.Y, C.Pivot.Y,
                       C.Margin.Top, C.Margin.Bottom, Rect.Y, Rect.H);
            InContext.Arrange(uint32_t(Child), Rect);
        }
    }

    // === Border and Button ========================================================

    UIVec2 UIContentWidget::MeasureContent(UILayoutContext& InContext)
    {
        UIVec2 Size{ Padding.Left + Padding.Right, Padding.Top + Padding.Bottom };
        const int32_t Child = FirstChild(*this);
        if(Child >= 0)
        {
            const UIWidget& C = *GetDocument()->GetWidget(uint32_t(Child));
            const UIVec2 Desired = InContext.Measure(uint32_t(Child));
            Size.X += Desired.X + C.Margin.Left + C.Margin.Right;
            Size.Y += Desired.Y + C.Margin.Top + C.Margin.Bottom;
        }
        return Size;
    }

    void UIContentWidget::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        const int32_t Child = FirstChild(*this);
        if(Child < 0) return;
        const UIRect Inner{ InRect.X + Padding.Left, InRect.Y + Padding.Top,
                            std::max(InRect.W - Padding.Left - Padding.Right, 0.0f),
                            std::max(InRect.H - Padding.Top - Padding.Bottom, 0.0f) };
        const UIWidget& C = *GetDocument()->GetWidget(uint32_t(Child));
        InContext.Arrange(uint32_t(Child), AlignInSlot(C, GetDocument()->GetDesiredSize(uint32_t(Child)), Inner));
    }

    void UIBorder::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        InContext.DrawBrush(Background, InRect);
    }

    UIButton::UIButton()
    {
        bFocusable = true;
        Normal.Tint   = UIColor{ 0.18f, 0.20f, 0.24f, 0.92f };
        Hovered.Tint  = UIColor{ 0.28f, 0.32f, 0.38f, 0.95f };
        Pressed.Tint  = UIColor{ 0.12f, 0.13f, 0.16f, 0.95f };
        Disabled.Tint = UIColor{ 0.18f, 0.20f, 0.24f, 0.45f };
        Padding       = UIMargin{ 16.0f, 8.0f, 16.0f, 8.0f };
    }

    void UIButton::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        const UIBrush* Brush = &Normal;
        if(!IsEnabledInHierarchy())                { Brush = &Disabled; }
        else if(IsPressed() && IsHovered())        { Brush = &Pressed; }
        else if(IsHovered() || IsFocused())        { Brush = &Hovered; }
        InContext.DrawBrush(*Brush, InRect);
    }

    void UIButton::OnPointer(const UIPointerEvent& InEvent)
    {
        switch(InEvent.Kind)
        {
            case UIPointerEvent::PE_Enter:   QueueActions(OnHovered);   break;
            case UIPointerEvent::PE_Leave:   QueueActions(OnUnhovered); break;
            case UIPointerEvent::PE_Press:   QueueActions(OnPressed);   break;
            case UIPointerEvent::PE_Release: QueueActions(OnReleased);  break;
            case UIPointerEvent::PE_Click:   QueueActions(OnClick);     break;
            default: break;
        }
    }

    // === Image ====================================================================

    UIVec2 UIImage::MeasureContent(UILayoutContext& /*InContext*/)
    {
        if(DesiredSize.X > 0.0f || DesiredSize.Y > 0.0f) return DesiredSize;
        const UIVec2 Texture = GetTextureSize(Brush.Texture);
        return UIVec2{ Texture.X * Brush.UVRect.Right, Texture.Y * Brush.UVRect.Bottom };
    }

    void UIImage::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        if(Stretch == EUIStretch::Fill || Brush.Texture.empty() || Brush.DrawAs == EUIDrawAs::NineSlice)
        {
            InContext.DrawBrush(Brush, InRect);
            return;
        }
        const UIVec2 Texture = GetTextureSize(Brush.Texture);
        const float  SrcW = Texture.X * Brush.UVRect.Right, SrcH = Texture.Y * Brush.UVRect.Bottom;
        if(SrcW <= 0.0f || SrcH <= 0.0f || InRect.W <= 0.0f || InRect.H <= 0.0f)
        {
            InContext.DrawBrush(Brush, InRect);
            return;
        }

        UIRect  Rect  = InRect;
        UIBrush Drawn = Brush;
        if(Stretch == EUIStretch::None || Stretch == EUIStretch::Uniform)
        {
            /* Centred: its own size, or as big as fits keeping its shape. */
            const float Fit = Stretch == EUIStretch::None ? 1.0f / InContext.Scale : std::min(InRect.W / SrcW, InRect.H / SrcH);
            Rect.W = SrcW * Fit;
            Rect.H = SrcH * Fit;
            Rect.X = InRect.X + (InRect.W - Rect.W) * 0.5f;
            Rect.Y = InRect.Y + (InRect.H - Rect.H) * 0.5f;
        }
        else
        {
            /* UniformToFill: covers the rectangle, the overhang cut off in UV. */
            const float Cover = std::max(InRect.W / SrcW, InRect.H / SrcH);
            const float KeepU = InRect.W / (SrcW * Cover), KeepV = InRect.H / (SrcH * Cover);
            Drawn.UVRect.Left   += Brush.UVRect.Right * (1.0f - KeepU) * 0.5f;
            Drawn.UVRect.Top    += Brush.UVRect.Bottom * (1.0f - KeepV) * 0.5f;
            Drawn.UVRect.Right  *= KeepU;
            Drawn.UVRect.Bottom *= KeepV;
        }
        InContext.DrawBrush(Drawn, Rect);
    }

    // === Text =====================================================================

    void UIText::EnsureGlyphs(UIFontCache* InFonts, const float InScale, const float InWrapPixels) const
    {
        const uint64_t Generation = InFonts ? InFonts->GetGeneration() : 0;
        if(bGlyphsValid && GlyphScale == InScale && GlyphWrap == InWrapPixels && GlyphFontGeneration == Generation) return;

        Glyphs.clear();
        GlyphSize    = UIVec2();
        GlyphTexture.clear();
        GlyphScale   = InScale;
        GlyphWrap    = InWrapPixels;
        GlyphFontGeneration = Generation;
        bGlyphsValid = true;

        UIFont* Font = InFonts ? InFonts->Find(this->Font, int(std::lround(FontSize * InScale))) : nullptr;
        if(!Font) return;
        UITextLayout Layout;
        UIFontCache::Layout(*Font, Text, InWrapPixels, Justify, LineHeight, Layout);
        Glyphs       = std::move(Layout.Quads);
        GlyphSize    = Layout.Size;
        GlyphTexture = Font->GetTextureKey();
    }

    UIVec2 UIText::MeasureContent(UILayoutContext& InContext)
    {
        EnsureGlyphs(InContext.Fonts, InContext.Scale, bWrap ? GlyphWrap : 0.0f);
        const float Scale = InContext.Scale > 0.0f ? InContext.Scale : 1.0f;
        return UIVec2{ GlyphSize.X / Scale, GlyphSize.Y / Scale };
    }

    void UIText::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        const UIRect Pixels = InContext.ToPixels(InRect);
        const float  HeightBefore = GlyphSize.Y;
        EnsureGlyphs(InContext.Fonts, InContext.Scale, bWrap ? std::max(Pixels.W, 1.0f) : 0.0f);
        if(bWrap && GlyphSize.Y != HeightBefore && GetDocument())
        {
            /* Wrapped at its width, it is a new height: lay out again (once:
             * the next pass measures at this width). */
            GetDocument()->MarkLayoutDirty();
        }
        if(Glyphs.empty() || !InContext.List) return;

        /* The block in its rectangle by Justify, on whole pixels: crisp glyphs. */
        float X = Pixels.X;
        if(Justify == EUIJustify::Center)     { X += std::round((Pixels.W - GlyphSize.X) * 0.5f); }
        else if(Justify == EUIJustify::Right) { X += Pixels.W - GlyphSize.X; }
        const float Y = Pixels.Y;

        RenderCore::UIDrawList& List = *InContext.List;
        const uint32_t Texture = List.FindOrAddTexture(GlyphTexture);
        thread_local std::vector<RenderCore::UIVertex> Scratch;
        const auto Emit = [&](const float InX, const float InY, const uint32_t InColor)
        {
            Scratch.assign(Glyphs.begin(), Glyphs.end());
            for(RenderCore::UIVertex& Each : Scratch)
            {
                Each.X += InX;
                Each.Y += InY;
                Each.Color = InColor;
            }
            List.AddQuads(Scratch.data(), uint32_t(Scratch.size()), Texture);
        };
        if(ShadowColor.A > 0.0f && (ShadowOffset.X != 0.0f || ShadowOffset.Y != 0.0f))
        {
            Emit(X + std::round(ShadowOffset.X * InContext.Scale), Y + std::round(ShadowOffset.Y * InContext.Scale),
                 ShadowColor.Pack(InContext.Opacity));
        }
        Emit(X, Y, Color.Pack(InContext.Opacity));

        /* Solid fills after this batch with it, from the atlas's white texel. */
        if(const UIFont* Font = InContext.Fonts ? InContext.Fonts->Find(this->Font, int(std::lround(FontSize * InContext.Scale))) : nullptr)
        {
            List.SetSolidFill(Texture, Font->GetWhiteU(), Font->GetWhiteV());
        }
    }

    void UIText::OnPropertyChanged(const UIPropertyDesc& /*InProperty*/)
    {
        bGlyphsValid = false;
    }

    // === Boxes ====================================================================

    UIVec2 UIBoxWidget::MeasureContent(UILayoutContext& InContext)
    {
        UIDocumentInstance* Doc = GetDocument();
        float Main = 0.0f, Cross = 0.0f;
        int   Shown = 0;
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            if(C.Visibility == EUIVisibility::Collapsed) continue;
            const UIVec2 Desired = InContext.Measure(uint32_t(Child));
            Main  += (bVertical ? Desired.Y : Desired.X) + MainMargin(C, bVertical);
            Cross  = std::max(Cross, (bVertical ? Desired.X : Desired.Y) + CrossMargin(C, bVertical));
            ++Shown;
        }
        Main += Spacing * float(std::max(Shown - 1, 0));
        return bVertical ? UIVec2{ Cross, Main } : UIVec2{ Main, Cross };
    }

    void UIBoxWidget::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        UIDocumentInstance* Doc = GetDocument();

        /* What the fixed children take; the rest is shared by Fill weight. */
        float Fixed = 0.0f, Weights = 0.0f;
        int   Shown = 0;
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            if(C.Visibility == EUIVisibility::Collapsed) continue;
            const UIVec2 Desired = Doc->GetDesiredSize(uint32_t(Child));
            Fixed += MainMargin(C, bVertical) + (C.Fill > 0.0f ? 0.0f : (bVertical ? Desired.Y : Desired.X));
            Weights += std::max(C.Fill, 0.0f);
            ++Shown;
        }
        const float Length = bVertical ? InRect.H : InRect.W;
        const float Spare  = std::max(Length - Fixed - Spacing * float(std::max(Shown - 1, 0)), 0.0f);

        float At = bVertical ? InRect.Y : InRect.X;
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            if(C.Visibility == EUIVisibility::Collapsed)
            {
                InContext.Arrange(uint32_t(Child), UIRect{ At, At, 0.0f, 0.0f });
                continue;
            }
            const UIVec2 Desired = Doc->GetDesiredSize(uint32_t(Child));
            const float  Own = C.Fill > 0.0f && Weights > 0.0f ? Spare * C.Fill / Weights : (bVertical ? Desired.Y : Desired.X);

            /* The slot along the axis is exactly its share; across, alignment applies. */
            UIRect Slot = bVertical ? UIRect{ InRect.X, At, InRect.W, Own + MainMargin(C, true) }
                                    : UIRect{ At, InRect.Y, Own + MainMargin(C, false), InRect.H };
            UIRect Rect = AlignInSlot(C, Desired, Slot);
            if(bVertical) { Rect.Y = At + C.Margin.Top;  Rect.H = Own; }
            else          { Rect.X = At + C.Margin.Left; Rect.W = Own; }
            InContext.Arrange(uint32_t(Child), Rect);
            At += Own + MainMargin(C, bVertical) + Spacing;
        }
    }

    // === Overlay, ProgressBar, Spacer ============================================

    UIVec2 UIOverlay::MeasureContent(UILayoutContext& InContext)
    {
        return UIWidget::MeasureContent(InContext);
    }

    void UIOverlay::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        UIWidget::ArrangeChildren(InContext, InRect);
    }

    // === Switcher =================================================================

    int32_t UISwitcher::GetPageCount() const
    {
        int32_t Count = 0;
        const UIDocumentInstance* Doc = GetDocument();
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child))) { ++Count; }
        return Count;
    }

    int32_t UISwitcher::GetActiveChild() const
    {
        const UIDocumentInstance* Doc = GetDocument();
        const int32_t Count  = GetPageCount();
        if(!Doc || Count == 0) return -1;
        const int32_t Active = std::clamp(ActiveIndex, 0, Count - 1);
        int32_t Child = FirstChild(*this);
        for(int32_t Page = 0; Page < Active; ++Page) { Child = Doc->GetNextSibling(uint32_t(Child)); }
        return Child;
    }

    bool UISwitcher::IsChildShown(const uint32_t InChild) const
    {
        return int32_t(InChild) == GetActiveChild();
    }

    bool UISwitcher::ShowPage(const std::string& InPage)
    {
        UIDocumentInstance* Doc = GetDocument();
        if(!Doc || InPage.empty()) return false;

        int32_t Page = -1, At = 0;
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)), ++At)
        {
            if(Doc->GetWidget(uint32_t(Child))->Name == InPage) { Page = At; break; }
        }
        float Number = 0.0f;
        if(Page < 0 && ParseFloat(InPage, Number) && Number >= 0.0f && int32_t(Number) < At) { Page = int32_t(Number); }
        if(Page < 0) return false;
        return Doc->SetProperty(*this, "ActiveIndex", UIValue::Number(Page));
    }

    UIProgressBar::UIProgressBar()
    {
        Background.Tint = UIColor{ 0.1f, 0.1f, 0.1f, 0.75f };
        FillBrush.Tint  = UIColor{ 0.35f, 0.75f, 0.35f, 1.0f };
        Size = UIVec2{ 300.0f, 24.0f };
    }

    void UIProgressBar::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        InContext.DrawBrush(Background, InRect);
        const float T = std::clamp(Value, 0.0f, 1.0f);
        if(T <= 0.0f) return;
        UIRect Filled = InRect;
        switch(FillDirection)
        {
            case EUIFillDirection::LeftToRight: Filled.W = InRect.W * T; break;
            case EUIFillDirection::RightToLeft: Filled.W = InRect.W * T; Filled.X = InRect.X + InRect.W - Filled.W; break;
            case EUIFillDirection::TopToBottom: Filled.H = InRect.H * T; break;
            case EUIFillDirection::BottomToTop: Filled.H = InRect.H * T; Filled.Y = InRect.Y + InRect.H - Filled.H; break;
        }
        InContext.DrawBrush(FillBrush, Filled);
    }

    // === CheckBox =================================================================

    UICheckBox::UICheckBox()
    {
        bFocusable = true;
        Padding    = UIMargin{};
        Box.Tint              = UIColor{ 0.07f, 0.09f, 0.12f, 0.92f };
        Box.OutlineColor      = UIColor{ 1.0f, 1.0f, 1.0f, 0.35f };
        Box.OutlineThickness  = 2.0f;
        BoxHovered            = Box;
        BoxHovered.OutlineColor = UIColor{ 0.95f, 0.65f, 0.30f, 1.0f };
        Check.Tint            = UIColor{ 0.95f, 0.65f, 0.30f, 1.0f };
    }

    UIVec2 UICheckBox::MeasureContent(UILayoutContext& InContext)
    {
        UIVec2 Size{ Padding.Left + Padding.Right + BoxSize, Padding.Top + Padding.Bottom + BoxSize };
        const int32_t Child = FirstChild(*this);
        if(Child >= 0)
        {
            const UIWidget& C = *GetDocument()->GetWidget(uint32_t(Child));
            const UIVec2 Desired = InContext.Measure(uint32_t(Child));
            if(C.Visibility != EUIVisibility::Collapsed)
            {
                Size.X += Spacing + Desired.X + C.Margin.Left + C.Margin.Right;
                Size.Y  = std::max(Size.Y, Padding.Top + Padding.Bottom + Desired.Y + C.Margin.Top + C.Margin.Bottom);
            }
        }
        return Size;
    }

    void UICheckBox::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        const int32_t Child = FirstChild(*this);
        if(Child < 0) return;
        const float Left = Padding.Left + BoxSize + Spacing;
        const UIRect Inner{ InRect.X + Left, InRect.Y + Padding.Top,
                            std::max(InRect.W - Left - Padding.Right, 0.0f),
                            std::max(InRect.H - Padding.Top - Padding.Bottom, 0.0f) };
        const UIWidget& C = *GetDocument()->GetWidget(uint32_t(Child));
        InContext.Arrange(uint32_t(Child), AlignInSlot(C, GetDocument()->GetDesiredSize(uint32_t(Child)), Inner));
    }

    void UICheckBox::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        UIPaintContext Context = InContext;
        if(!IsEnabledInHierarchy()) Context.Opacity *= 0.45f;

        /* The box at the left, centred down the row. */
        const float  Inner = std::max(InRect.H - Padding.Top - Padding.Bottom, 0.0f);
        const UIRect BoxRect{ InRect.X + Padding.Left, InRect.Y + Padding.Top + (Inner - BoxSize) * 0.5f, BoxSize, BoxSize };
        Context.DrawBrush(IsHovered() || IsFocused() || IsPressed() ? BoxHovered : Box, BoxRect);
        if(bChecked)
        {
            const float Inset = std::round(BoxSize * 0.22f);
            Context.DrawBrush(Check, UIRect{ BoxRect.X + Inset, BoxRect.Y + Inset,
                                             std::max(BoxSize - Inset * 2.0f, 0.0f), std::max(BoxSize - Inset * 2.0f, 0.0f) });
        }
    }

    void UICheckBox::OnPointer(const UIPointerEvent& InEvent)
    {
        if(InEvent.Kind != UIPointerEvent::PE_Click || !GetDocument()) return;
        if(GetDocument()->SetInputValue(*this, "Checked", UIValue::Bool(!bChecked))) { QueueActions(OnChanged); }
    }

    // === Slider ===================================================================

    UISlider::UISlider()
    {
        bFocusable = true;
        Size = UIVec2{ 300.0f, 36.0f };
        Track.Tint     = UIColor{ 0.07f, 0.09f, 0.12f, 0.92f };
        Track.OutlineColor     = UIColor{ 1.0f, 1.0f, 1.0f, 0.20f };
        Track.OutlineThickness = 1.0f;
        FillBrush.Tint = UIColor{ 0.85f, 0.54f, 0.18f, 0.95f };
        Thumb.Tint     = UIColor{ 0.92f, 0.94f, 0.97f, 1.0f };
        ThumbHovered   = Thumb;
        ThumbHovered.Tint = UIColor{ 1.0f, 0.80f, 0.55f, 1.0f };
        ThumbHovered.OutlineColor     = UIColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        ThumbHovered.OutlineThickness = 2.0f;
    }

    UIVec2 UISlider::MeasureContent(UILayoutContext& /*InContext*/)
    {
        return UIVec2{ Size.X, std::max(Size.Y, std::max(ThumbSize.Y, TrackHeight)) };
    }

    float UISlider::Snap(double InValue) const
    {
        const double Low = std::min(Min, Max), High = std::max(Min, Max);
        double V = std::clamp(InValue, Low, High);
        if(Step > 0.0f)
        {
            /* In double, so Min + n * Step is the float nearest what was meant (0.25 + 22 * 0.05 is 1.35). */
            V = double(Min) + std::round((V - double(Min)) / double(Step)) * double(Step);
            V = std::clamp(V, Low, High);
        }
        return float(V);
    }

    float UISlider::GetFraction() const
    {
        if(Max == Min) return 0.0f;
        return std::clamp((Value - Min) / (Max - Min), 0.0f, 1.0f);
    }

    void UISlider::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        UIPaintContext Context = InContext;
        if(!IsEnabledInHierarchy()) Context.Opacity *= 0.45f;

        /* The thumb's centre travels between half a thumb in from each end: it never hangs out. */
        const float Half  = ThumbSize.X * 0.5f;
        const float Start = InRect.X + Half, Length = std::max(InRect.W - ThumbSize.X, 0.0f);
        const float At    = Start + Length * GetFraction();
        const float Mid   = InRect.Y + InRect.H * 0.5f;

        const UIRect TrackRect{ InRect.X, Mid - TrackHeight * 0.5f, InRect.W, TrackHeight };
        Context.DrawBrush(Track, TrackRect);
        if(At > InRect.X) { Context.DrawBrush(FillBrush, UIRect{ InRect.X, TrackRect.Y, At - InRect.X, TrackHeight }); }
        Context.DrawBrush(IsHovered() || IsFocused() || IsPressed() ? ThumbHovered : Thumb,
                          UIRect{ At - Half, Mid - ThumbSize.Y * 0.5f, ThumbSize.X, ThumbSize.Y });
    }

    void UISlider::ChangeTo(const double InValue)
    {
        if(GetDocument() && GetDocument()->SetInputValue(*this, "Value", UIValue::Number(Snap(InValue)))) { QueueActions(OnChanged); }
    }

    void UISlider::OnPointer(const UIPointerEvent& InEvent)
    {
        if(InEvent.Kind != UIPointerEvent::PE_Press && InEvent.Kind != UIPointerEvent::PE_Move) return;
        if(!GetDocument() || !IsEnabledInHierarchy()) return;

        /* The pointer along the travel, in the pixels it was last painted at (render transforms included). */
        const uint32_t Self   = GetIndex();
        const UIRect&  Pixels = GetDocument()->GetPixelRect(Self);
        const UIRect&  Layout = GetDocument()->GetRect(Self);
        const float    PerUnit = Layout.W > 0.0f ? Pixels.W / Layout.W : 1.0f;
        const float    Half    = ThumbSize.X * 0.5f * PerUnit;
        const float    Travel  = Pixels.W - Half * 2.0f;
        const double   T = Travel > 0.0f ? std::clamp(double(InEvent.Pixels.X - Pixels.X - Half) / double(Travel), 0.0, 1.0) : 0.0;
        ChangeTo(double(Min) + (double(Max) - double(Min)) * T);
    }

    bool UISlider::OnNavigate(const int InDirection)
    {
        if(InDirection != 2 && InDirection != 3) return false;
        const double By = Step > 0.0f ? double(Step) : (double(Max) - double(Min)) / 20.0;
        const double Sign = (InDirection == 3) == (Max >= Min) ? 1.0 : -1.0;
        ChangeTo(double(Value) + By * Sign);
        return true;   // held at an end too: left and right never leave a slider by surprise
    }

    // === Scrolling ================================================================

    void UIScrollState::Thumb(const float InOffset, const float InTrack, float& OutStart, float& OutLength) const
    {
        const float Content = std::max(ContentLength, 1.0f);
        OutLength = std::clamp(InTrack * ViewLength / Content, std::min(24.0f, InTrack), InTrack);
        const float Max = MaxOffset();
        OutStart = Max > 0.0f ? (InTrack - OutLength) * std::clamp(InOffset / Max, 0.0f, 1.0f) : 0.0f;
    }

    float UIScrollState::OffsetForThumb(const float InThumbStart, const float InTrack) const
    {
        float Start = 0.0f, Length = 0.0f;
        Thumb(0.0f, InTrack, Start, Length);
        const float Travel = InTrack - Length;
        return Travel > 0.0f ? std::clamp(InThumbStart / Travel, 0.0f, 1.0f) * MaxOffset() : 0.0f;
    }

    float UIScrollState::Reveal(const float InOffset, const float InStart, const float InLength) const
    {
        float Offset = InOffset;
        if(InStart < Offset)                              { Offset = InStart; }
        else if(InStart + InLength > Offset + ViewLength) { Offset = InStart + InLength - ViewLength; }
        return std::clamp(Offset, 0.0f, MaxOffset());
    }

    UIScrollBox::UIScrollBox()
    {
        bClipChildren = true;
        Size = UIVec2{ 400.0f, 300.0f };
        Track.Tint        = UIColor{ 1.0f, 1.0f, 1.0f, 0.08f };
        Thumb.Tint        = UIColor{ 1.0f, 1.0f, 1.0f, 0.35f };
        ThumbHovered.Tint = UIColor{ 1.0f, 1.0f, 1.0f, 0.6f };
    }

    UIVec2 UIScrollBox::MeasureContent(UILayoutContext& InContext)
    {
        const int32_t Child = FirstChild(*this);
        if(Child >= 0) { InContext.Measure(uint32_t(Child)); }   // measured, for ArrangeChildren
        return Size;
    }

    bool UIScrollBox::ShowsBar() const
    {
        return ScrollBar == EUIScrollBarMode::Always || (ScrollBar == EUIScrollBarMode::Auto && Scroll.CanScroll());
    }

    UIRect UIScrollBox::TrackRect(const UIRect& InRect) const
    {
        return IsVertical() ? UIRect{ InRect.X + InRect.W - BarWidth, InRect.Y, BarWidth, InRect.H }
                            : UIRect{ InRect.X, InRect.Y + InRect.H - BarWidth, InRect.W, BarWidth };
    }

    void UIScrollBox::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        const int32_t Child = FirstChild(*this);
        const UIVec2 Desired = Child >= 0 ? GetDocument()->GetDesiredSize(uint32_t(Child)) : UIVec2();
        const UIWidget* C = Child >= 0 ? GetDocument()->GetWidget(uint32_t(Child)) : nullptr;
        const float Along = C ? (IsVertical() ? Desired.Y + C->Margin.Top + C->Margin.Bottom : Desired.X + C->Margin.Left + C->Margin.Right) : 0.0f;
        Scroll.ViewLength    = IsVertical() ? InRect.H : InRect.W;
        Scroll.ContentLength = std::max(Along, Scroll.ViewLength);

        /* Room for the bar across the axis, when it shows. */
        const float Bar = ShowsBar() ? BarWidth : 0.0f;
        if(C)
        {
            const UIRect Content = IsVertical() ? UIRect{ InRect.X, InRect.Y, std::max(InRect.W - Bar, 0.0f), Scroll.ContentLength }
                                                : UIRect{ InRect.X, InRect.Y, Scroll.ContentLength, std::max(InRect.H - Bar, 0.0f) };
            UIRect Placed = AlignInSlot(*C, Desired, Content);
            if(IsVertical()) { Placed.Y = Content.Y + C->Margin.Top;  Placed.H = Desired.Y; }
            else             { Placed.X = Content.X + C->Margin.Left; Placed.W = Desired.X; }
            InContext.Arrange(uint32_t(Child), Placed);
        }
        /* Content that shrank: the offset comes back into range (not a player's change: not written back). */
        ScrollOffset = std::clamp(ScrollOffset, 0.0f, Scroll.MaxOffset());
    }

    UIVec2 UIScrollBox::GetChildOffset() const
    {
        const float Offset = std::clamp(ScrollOffset, 0.0f, Scroll.MaxOffset());
        return IsVertical() ? UIVec2{ 0.0f, Offset } : UIVec2{ Offset, 0.0f };
    }

    void UIScrollBox::PaintOver(UIPaintContext& InContext, const UIRect& InRect) const
    {
        if(!ShowsBar()) return;
        const UIRect TrackArea = TrackRect(InRect);
        InContext.DrawBrush(Track, TrackArea);
        const float Length = IsVertical() ? TrackArea.H : TrackArea.W;
        float Start = 0.0f, ThumbLength = 0.0f;
        Scroll.Thumb(ScrollOffset, Length, Start, ThumbLength);
        const UIRect ThumbArea = IsVertical() ? UIRect{ TrackArea.X, TrackArea.Y + Start, TrackArea.W, ThumbLength }
                                              : UIRect{ TrackArea.X + Start, TrackArea.Y, ThumbLength, TrackArea.H };
        InContext.DrawBrush(DragGrab >= 0.0f || IsHovered() ? ThumbHovered : Thumb, ThumbArea);
    }

    bool UIScrollBox::ScrollTo(const float InOffset)
    {
        const float Clamped = std::clamp(InOffset, 0.0f, Scroll.MaxOffset());
        if(!GetDocument() || !GetDocument()->SetInputValue(*this, "ScrollOffset", UIValue::Number(Clamped))) return false;
        QueueActions(OnScrolled);
        return true;
    }

    bool UIScrollBox::OnWheel(const float InDelta)
    {
        /* At an end, the wheel goes on to an outer scroll box. */
        return Scroll.CanScroll() && ScrollTo(ScrollOffset - InDelta * ScrollStep);
    }

    void UIScrollBox::OnPointer(const UIPointerEvent& InEvent)
    {
        if(!GetDocument() || !ShowsBar()) return;
        const UIRect& Pixels = GetDocument()->GetPixelRect(GetIndex());
        const UIRect& Layout = GetDocument()->GetRect(GetIndex());
        const float PerUnit = IsVertical() ? (Layout.H > 0.0f ? Pixels.H / Layout.H : 1.0f) : (Layout.W > 0.0f ? Pixels.W / Layout.W : 1.0f);
        const float Track   = IsVertical() ? Pixels.H : Pixels.W;
        const float Along   = IsVertical() ? InEvent.Pixels.Y - Pixels.Y : InEvent.Pixels.X - Pixels.X;
        const float Across  = IsVertical() ? InEvent.Pixels.X - (Pixels.X + Pixels.W - BarWidth * PerUnit)
                                           : InEvent.Pixels.Y - (Pixels.Y + Pixels.H - BarWidth * PerUnit);
        float Start = 0.0f, Length = 0.0f;
        Scroll.Thumb(ScrollOffset, Track, Start, Length);

        switch(InEvent.Kind)
        {
            case UIPointerEvent::PE_Press:
                if(Across < 0.0f) return;                       // on the content, not the bar
                if(Along >= Start && Along < Start + Length) { DragGrab = Along - Start; }
                else { ScrollTo(ScrollOffset + (Along < Start ? -Scroll.ViewLength : Scroll.ViewLength)); }   // a page
                GetDocument()->MarkPaintDirty();
                break;
            case UIPointerEvent::PE_Move:
                if(DragGrab >= 0.0f) { ScrollTo(Scroll.OffsetForThumb(Along - DragGrab, Track)); }
                break;
            case UIPointerEvent::PE_Release:
                if(DragGrab >= 0.0f) { DragGrab = -1.0f; GetDocument()->MarkPaintDirty(); }
                break;
            default:
                break;
        }
    }

    void UIScrollBox::OnDescendantFocused(const uint32_t InIndex)
    {
        if(!GetDocument() || !Scroll.CanScroll()) return;
        const UIRect& Self  = GetDocument()->GetRect(GetIndex());
        const UIRect& Shown = GetDocument()->GetRect(InIndex);
        const float Start  = IsVertical() ? Shown.Y - Self.Y : Shown.X - Self.X;
        const float Length = IsVertical() ? Shown.H : Shown.W;
        const float Offset = Scroll.Reveal(ScrollOffset, Start, Length);
        if(Offset != ScrollOffset) { ScrollTo(Offset); }
    }

    // === Grid =====================================================================

    void UIGrid::SizeTracks()
    {
        UIDocumentInstance* Doc = GetDocument();
        int32_t ColumnCount = 0, RowCount = 0;
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            if(C.Visibility == EUIVisibility::Collapsed) continue;
            ColumnCount = std::max(ColumnCount, std::max(C.Column, 0) + std::max(C.ColumnSpan, 1));
            RowCount    = std::max(RowCount, std::max(C.Row, 0) + std::max(C.RowSpan, 1));
        }
        Columns.assign(size_t(ColumnCount), 0.0f);
        Rows.assign(size_t(RowCount), 0.0f);

        /* One axis: single-track children set the sizes, then spanning ones add their surplus evenly. */
        const auto SizeAxis = [&](std::vector<float>& Tracks, const float Spacing, const bool bColumns)
        {
            float Largest = 0.0f;
            for(int Spanning = 0; Spanning < 2; ++Spanning)
            {
                for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
                {
                    const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
                    if(C.Visibility == EUIVisibility::Collapsed) continue;
                    const int32_t First = std::max(bColumns ? C.Column : C.Row, 0);
                    const int32_t Span  = std::max(bColumns ? C.ColumnSpan : C.RowSpan, 1);
                    if((Span > 1) != (Spanning == 1)) continue;
                    const UIVec2& Desired = Doc->GetDesiredSize(uint32_t(Child));
                    const float Need = bColumns ? Desired.X + C.Margin.Left + C.Margin.Right : Desired.Y + C.Margin.Top + C.Margin.Bottom;
                    Largest = std::max(Largest, (Need - Spacing * float(Span - 1)) / float(Span));
                    float Have = Spacing * float(Span - 1);
                    for(int32_t At = First; At < First + Span; ++At) { Have += Tracks[size_t(At)]; }
                    if(Need > Have)
                    {
                        const float Each = (Need - Have) / float(Span);
                        for(int32_t At = First; At < First + Span; ++At) { Tracks[size_t(At)] += Each; }
                    }
                }
            }
            if(bUniform) { std::fill(Tracks.begin(), Tracks.end(), Largest); }
        };
        SizeAxis(Columns, ColumnSpacing, true);
        SizeAxis(Rows, RowSpacing, false);
    }

    UIVec2 UIGrid::MeasureContent(UILayoutContext& InContext)
    {
        UIDocumentInstance* Doc = GetDocument();
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child))) { InContext.Measure(uint32_t(Child)); }
        SizeTracks();
        const auto Total = [](const std::vector<float>& InTracks, const float InSpacing)
        {
            float Sum = 0.0f;
            for(const float Each : InTracks) { Sum += Each; }
            return Sum + InSpacing * float(std::max(int(InTracks.size()) - 1, 0));
        };
        return UIVec2{ Total(Columns, ColumnSpacing), Total(Rows, RowSpacing) };
    }

    void UIGrid::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        UIDocumentInstance* Doc = GetDocument();
        SizeTracks();
        if(bUniform && !Columns.empty() && !Rows.empty())
        {
            /* Every cell the same, sharing the grid's rectangle. */
            const float W = std::max((InRect.W - ColumnSpacing * float(Columns.size() - 1)) / float(Columns.size()), 0.0f);
            const float H = std::max((InRect.H - RowSpacing * float(Rows.size() - 1)) / float(Rows.size()), 0.0f);
            std::fill(Columns.begin(), Columns.end(), W);
            std::fill(Rows.begin(), Rows.end(), H);
        }
        const auto Span = [](const std::vector<float>& InTracks, const float InSpacing, const int32_t InFirst, const int32_t InCount,
                             float& OutStart, float& OutLength)
        {
            OutStart = 0.0f;
            for(int32_t At = 0; At < InFirst && At < int32_t(InTracks.size()); ++At) { OutStart += InTracks[size_t(At)] + InSpacing; }
            OutLength = InSpacing * float(InCount - 1);
            for(int32_t At = InFirst; At < InFirst + InCount && At < int32_t(InTracks.size()); ++At) { OutLength += InTracks[size_t(At)]; }
        };
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
            if(C.Visibility == EUIVisibility::Collapsed)
            {
                InContext.Arrange(uint32_t(Child), UIRect{ InRect.X, InRect.Y, 0.0f, 0.0f });
                continue;
            }
            UIRect Cell;
            Span(Columns, ColumnSpacing, std::max(C.Column, 0), std::max(C.ColumnSpan, 1), Cell.X, Cell.W);
            Span(Rows, RowSpacing, std::max(C.Row, 0), std::max(C.RowSpan, 1), Cell.Y, Cell.H);
            Cell.X += InRect.X;
            Cell.Y += InRect.Y;
            InContext.Arrange(uint32_t(Child), AlignInSlot(C, Doc->GetDesiredSize(uint32_t(Child)), Cell));
        }
    }

    // === WrapBox ==================================================================

    UIVec2 UIWrapBox::Flow(UILayoutContext& InContext, const float InWidth, const UIRect* InArrange)
    {
        UIDocumentInstance* Doc = GetDocument();
        UIVec2  Size;
        bool    bAnyLine  = false;
        int32_t LineFirst = FirstChild(*this);
        while(LineFirst >= 0)
        {
            /* A line: as many as fit (at least one). */
            float   LineW = 0.0f, LineH = 0.0f;
            int32_t Next  = LineFirst, Count = 0;
            for(; Next >= 0; Next = Doc->GetNextSibling(uint32_t(Next)))
            {
                const UIWidget& C = *Doc->GetWidget(uint32_t(Next));
                if(C.Visibility == EUIVisibility::Collapsed) continue;
                const UIVec2& D = Doc->GetDesiredSize(uint32_t(Next));
                const float W = D.X + C.Margin.Left + C.Margin.Right, H = D.Y + C.Margin.Top + C.Margin.Bottom;
                const float With = LineW + (Count > 0 ? ItemSpacing : 0.0f) + W;
                if(Count > 0 && InWidth > 0.0f && With > InWidth) break;
                LineW = With;
                LineH = std::max(LineH, H);
                ++Count;
            }
            const float Top = bAnyLine ? Size.Y + LineSpacing : 0.0f;
            if(InArrange)
            {
                float X = InArrange->X;
                if(LineAlign == EUIJustify::Center)     { X += (InArrange->W - LineW) * 0.5f; }
                else if(LineAlign == EUIJustify::Right) { X += InArrange->W - LineW; }
                for(int32_t Child = LineFirst; Child != Next; Child = Doc->GetNextSibling(uint32_t(Child)))
                {
                    const UIWidget& C = *Doc->GetWidget(uint32_t(Child));
                    const UIVec2& D = Doc->GetDesiredSize(uint32_t(Child));
                    if(C.Visibility == EUIVisibility::Collapsed)
                    {
                        InContext.Arrange(uint32_t(Child), UIRect{ X, InArrange->Y + Top, 0.0f, 0.0f });
                        continue;
                    }
                    const float W = D.X + C.Margin.Left + C.Margin.Right;
                    InContext.Arrange(uint32_t(Child), AlignInSlot(C, D, UIRect{ X, InArrange->Y + Top, W, LineH }));
                    X += W + ItemSpacing;
                }
            }
            if(Count > 0)
            {
                Size.X   = std::max(Size.X, LineW);
                Size.Y   = Top + LineH;
                bAnyLine = true;
            }
            LineFirst = Next;
        }
        return Size;
    }

    UIVec2 UIWrapBox::MeasureContent(UILayoutContext& InContext)
    {
        UIDocumentInstance* Doc = GetDocument();
        for(int32_t Child = FirstChild(*this); Child >= 0; Child = Doc->GetNextSibling(uint32_t(Child))) { InContext.Measure(uint32_t(Child)); }
        return Flow(InContext, WrapWidth, nullptr);
    }

    void UIWrapBox::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        if(InRect.W != WrapWidth)
        {
            /* Its height depends on this width: measured again (UIDocumentInstance::Layout's next pass). */
            WrapWidth = InRect.W;
            GetDocument()->MarkLayoutDirty();
        }
        Flow(InContext, InRect.W, &InRect);
    }

    // === TextInput ================================================================

    namespace
    {
        constexpr float CARET_BLINK_SECONDS = 0.53f;

        size_t CountCodepoints(const std::string& InText)
        {
            size_t Count = 0;
            for(size_t At = 0; At < InText.size(); At = NextUtf8(InText, At)) { ++Count; }
            return Count;
        }
    }

    UITextInput::UITextInput()
    {
        bFocusable = true;
        Size = UIVec2{ 360.0f, 48.0f };
        Background.Tint                = UIColor{ 0.07f, 0.09f, 0.12f, 0.92f };
        Background.OutlineColor        = UIColor{ 1.0f, 1.0f, 1.0f, 0.3f };
        Background.OutlineThickness    = 1.0f;
        FocusedBrush                   = Background;
        FocusedBrush.OutlineColor      = UIColor{ 0.95f, 0.65f, 0.30f, 1.0f };
        FocusedBrush.OutlineThickness  = 2.0f;
    }

    UIVec2 UITextInput::MeasureContent(UILayoutContext& /*InContext*/)
    {
        return UIVec2{ Size.X, std::max(Size.Y, FontSize * 1.3f + Padding.Top + Padding.Bottom) };
    }

    std::string UITextInput::Shown() const
    {
        return bPassword ? std::string(CountCodepoints(Text), '*') : Text;
    }

    size_t UITextInput::ShownOffset(const size_t InTextOffset) const
    {
        if(!bPassword) return std::min(InTextOffset, Text.size());
        size_t Count = 0;
        for(size_t At = 0; At < InTextOffset && At < Text.size(); At = NextUtf8(Text, At)) { ++Count; }
        return Count;
    }

    size_t UITextInput::TextOffsetForShown(const size_t InShownOffset) const
    {
        if(!bPassword) return std::min(InShownOffset, Text.size());
        size_t At = 0;
        for(size_t Count = 0; Count < InShownOffset && At < Text.size(); ++Count) { At = NextUtf8(Text, At); }
        return At;
    }

    void UITextInput::EnsureLayout(UIFontCache* InFonts, const float InScale) const
    {
        const std::string Now = Shown();
        const uint64_t Generation = InFonts ? InFonts->GetGeneration() : 0;
        if(Now == LaidOut && InScale == LaidOutScale && Generation == LaidOutGeneration) return;
        LaidOut = Now;
        LaidOutScale = InScale;
        LaidOutGeneration = Generation;
        Glyphs.clear();
        Carets.assign(Now.size() + 1, 0.0f);
        UIFont* Font = InFonts ? InFonts->Find(this->Font, int(std::lround(FontSize * InScale))) : nullptr;
        if(!Font) return;
        UITextLayout Layout;
        UIFontCache::Layout(*Font, Now, 0.0f, EUIJustify::Left, 1.0f, Layout);
        Glyphs       = std::move(Layout.Quads);
        GlyphTexture = Font->GetTextureKey();
        LineHeight   = Font->GetLineHeight();
        WhiteU       = Font->GetWhiteU();
        WhiteV       = Font->GetWhiteV();
        UIFontCache::MeasureCarets(*Font, Now, Carets);
    }

    void UITextInput::Paint(UIPaintContext& InContext, const UIRect& InRect) const
    {
        const bool bFocused = IsFocused();
        InContext.DrawBrush(bFocused ? FocusedBrush : Background, InRect);
        if(!InContext.List) return;

        const UIRect Inner = InContext.ToPixels(UIRect{ InRect.X + Padding.Left, InRect.Y + Padding.Top,
                                                        std::max(InRect.W - Padding.Left - Padding.Right, 0.0f),
                                                        std::max(InRect.H - Padding.Top - Padding.Bottom, 0.0f) });
        PixelScale = InContext.PixelScale.X;
        EnsureLayout(InContext.Fonts, InContext.Scale);
        RenderCore::UIDrawList& List = *InContext.List;
        const float Top = Inner.Y + std::round((Inner.H - LineHeight) * 0.5f);

        /* The caret in view: the text moved left as it runs past the right edge. */
        const float CaretX = Carets.empty() ? 0.0f : Carets[std::min(ShownOffset(Caret), Carets.size() - 1)];
        const float Width  = Carets.empty() ? 0.0f : Carets.back();
        if(CaretX - ScrollX > Inner.W - 2.0f) ScrollX = CaretX - Inner.W + 2.0f;
        if(CaretX < ScrollX)                  ScrollX = CaretX;
        ScrollX = std::clamp(ScrollX, 0.0f, std::max(Width - Inner.W + 2.0f, 0.0f));
        const float Left = Inner.X - ScrollX;
        TextLeft = Left;

        List.PushClip(Inner);
        if(bFocused && HasSelection() && !Carets.empty())
        {
            const float A = Carets[std::min(ShownOffset(SelectionStart()), Carets.size() - 1)];
            const float B = Carets[std::min(ShownOffset(SelectionEnd()), Carets.size() - 1)];
            List.AddRect(UIRect{ Left + A, Top, B - A, LineHeight }, SelectionColor.Pack(InContext.Opacity));
        }
        if(Text.empty() && !Placeholder.empty() && !bFocused)
        {
            /* The placeholder, in its own colour: laid out each paint (an empty field is cheap). */
            if(UIFont* Font = InContext.Fonts ? InContext.Fonts->Find(this->Font, int(std::lround(FontSize * InContext.Scale))) : nullptr)
            {
                UITextLayout Layout;
                UIFontCache::Layout(*Font, Placeholder, 0.0f, EUIJustify::Left, 1.0f, Layout);
                const uint32_t Packed = PlaceholderColor.Pack(InContext.Opacity);
                for(RenderCore::UIVertex& Each : Layout.Quads) { Each.X += Left; Each.Y += Top; Each.Color = Packed; }
                List.AddQuads(Layout.Quads.data(), uint32_t(Layout.Quads.size()), List.FindOrAddTexture(Font->GetTextureKey()));
            }
        }
        else if(!Glyphs.empty())
        {
            thread_local std::vector<RenderCore::UIVertex> Scratch;
            Scratch.assign(Glyphs.begin(), Glyphs.end());
            const uint32_t Packed = Color.Pack(InContext.Opacity);
            for(RenderCore::UIVertex& Each : Scratch) { Each.X += Left; Each.Y += Top; Each.Color = Packed; }
            const uint32_t Texture = List.FindOrAddTexture(GlyphTexture);
            List.AddQuads(Scratch.data(), uint32_t(Scratch.size()), Texture);
            List.SetSolidFill(Texture, WhiteU, WhiteV);
        }
        if(bFocused && Blink < CARET_BLINK_SECONDS)
        {
            const float Thick = std::max(std::round(InContext.Scale * 2.0f), 1.0f);
            List.AddRect(UIRect{ std::round(Left + CaretX), Top, Thick, LineHeight }, CaretColor.Pack(InContext.Opacity));
        }
        List.PopClip();
    }

    size_t UITextInput::CaretAtPixel(const float InX) const
    {
        /* The nearest boundary between characters. */
        if(Carets.empty()) return 0;
        const std::string Now = Shown();
        size_t Best = 0;
        float  BestDistance = std::abs(InX - Carets[0]);
        for(size_t At = NextUtf8(Now, 0); At <= Now.size(); At = At < Now.size() ? NextUtf8(Now, At) : Now.size() + 1)
        {
            const float Distance = std::abs(InX - Carets[std::min(At, Carets.size() - 1)]);
            if(Distance < BestDistance) { Best = At; BestDistance = Distance; }
        }
        return TextOffsetForShown(Best);
    }

    void UITextInput::Select(const size_t InAnchor, const size_t InCaret)
    {
        Anchor = std::min(InAnchor, Text.size());
        Caret  = std::min(InCaret, Text.size());
        Blink  = 0.0f;
        if(GetDocument()) GetDocument()->MarkPaintDirty();
    }

    bool UITextInput::Change(const std::string& InText, const size_t InCaret)
    {
        UIDocumentInstance* Doc = GetDocument();
        if(!Doc || InText == Text) { Select(InCaret, InCaret); return false; }
        Doc->SetInputValue(*this, "Text", UIValue::String(InText));   // two-way
        Select(InCaret, InCaret);
        QueueActions(OnChanged);
        return true;
    }

    bool UITextInput::DeleteSelection()
    {
        if(!HasSelection()) return false;
        const size_t Start = SelectionStart();
        return Change(Text.substr(0, Start) + Text.substr(SelectionEnd()), Start);
    }

    bool UITextInput::Insert(const std::string& InUtf8)
    {
        /* What may be typed: printable text; digits (and one '.', a leading '-') for numbers. */
        const std::string Before = Text.substr(0, SelectionStart()), After = Text.substr(SelectionEnd());
        std::string Accepted;
        size_t Count = CountCodepoints(Before) + CountCodepoints(After);
        bool bDot = (Before + After).find('.') != std::string::npos;
        for(size_t At = 0; At < InUtf8.size();)
        {
            const uint32_t Code = DecodeUtf8(InUtf8, At);
            if(Code < 0x20 || Code == 0x7F) continue;
            if(Mode != EUITextMode::Text)
            {
                const bool bDigit = Code >= '0' && Code <= '9';
                const bool bMinus = Code == '-' && Before.empty() && Accepted.empty();
                const bool bPoint = Code == '.' && Mode == EUITextMode::Number && !bDot;
                if(!bDigit && !bMinus && !bPoint) continue;
                if(bPoint) bDot = true;
            }
            if(MaxLength > 0 && Count >= size_t(MaxLength)) break;
            AppendUtf8(Accepted, Code);
            ++Count;
        }
        if(Accepted.empty() && !HasSelection()) return false;
        return Change(Before + Accepted + After, Before.size() + Accepted.size());
    }

    bool UITextInput::OnTextInput(const UIInputFrame& InFrame)
    {
        bool bUsed = false;
        const auto Move = [&](const size_t InTo)
        {
            Select(InFrame.bShift ? Anchor : InTo, InTo);
            bUsed = true;
        };

        /* In a frame's order of intent: select, move, delete, the clipboard, then what was typed. */
        if(InFrame.bSelectAll) { Select(0, Text.size()); bUsed = true; }
        for(int Each = 0; Each < InFrame.Left; ++Each)
        {
            Move(HasSelection() && !InFrame.bShift ? SelectionStart() : PrevUtf8(Text, Caret));
        }
        for(int Each = 0; Each < InFrame.Right; ++Each)
        {
            Move(HasSelection() && !InFrame.bShift ? SelectionEnd() : NextUtf8(Text, Caret));
        }
        if(InFrame.Home) Move(0);
        if(InFrame.End)  Move(Text.size());
        for(int Each = 0; Each < InFrame.Backspace; ++Each)
        {
            if(!DeleteSelection() && Caret > 0)
            {
                const size_t From = PrevUtf8(Text, Caret);
                Change(Text.substr(0, From) + Text.substr(Caret), From);
            }
            bUsed = true;
        }
        for(int Each = 0; Each < InFrame.Delete; ++Each)
        {
            if(!DeleteSelection() && Caret < Text.size())
            {
                Change(Text.substr(0, Caret) + Text.substr(NextUtf8(Text, Caret)), Caret);
            }
            bUsed = true;
        }
        if((InFrame.bCopy || InFrame.bCut) && HasSelection() && !bPassword)
        {
            if(UISystem* System = GetDocument() ? GetDocument()->GetSystem() : nullptr)
            {
                System->SetClipboardText(Text.substr(SelectionStart(), SelectionEnd() - SelectionStart()));
            }
            if(InFrame.bCut) DeleteSelection();
            bUsed = true;
        }
        if(InFrame.bPaste)
        {
            if(UISystem* System = GetDocument() ? GetDocument()->GetSystem() : nullptr)
            {
                std::string Pasted = System->GetClipboardText();
                Pasted = Pasted.substr(0, Pasted.find_first_of("\r\n"));   // one line
                Insert(Pasted);
            }
            bUsed = true;
        }
        if(InFrame.Text[0] != '\0' && !InFrame.bCtrl) { Insert(InFrame.Text); bUsed = true; }
        if(InFrame.bReturn)
        {
            /* Committed: Return keeps focus, for typing on. */
            if(Text != TextAtFocus) { QueueActions(OnCommit); TextAtFocus = Text; }
            bUsed = true;
        }
        return bUsed;
    }

    void UITextInput::OnPointer(const UIPointerEvent& InEvent)
    {
        if(!GetDocument()) return;
        const float X = InEvent.Pixels.X - TextLeft;
        switch(InEvent.Kind)
        {
            case UIPointerEvent::PE_Press:   bDragging = true; Select(CaretAtPixel(X), CaretAtPixel(X)); break;
            case UIPointerEvent::PE_Move:    if(bDragging) Select(Anchor, CaretAtPixel(X)); break;
            case UIPointerEvent::PE_Release: bDragging = false; break;
            default: break;
        }
    }

    void UITextInput::OnFocusChanged(const bool bInFocused)
    {
        if(bInFocused)
        {
            TextAtFocus = Text;
            Select(Text.size(), Text.size());
            return;
        }
        bDragging = false;
        if(Text != TextAtFocus) { QueueActions(OnCommit); TextAtFocus = Text; }
        Select(Caret, Caret);
    }

    bool UITextInput::OnFocusedTick(const float InDeltaSeconds)
    {
        const bool bWasOn = Blink < CARET_BLINK_SECONDS;
        Blink = std::fmod(Blink + InDeltaSeconds, CARET_BLINK_SECONDS * 2.0f);
        return bWasOn != (Blink < CARET_BLINK_SECONDS);
    }

    void UITextInput::OnPropertyChanged(const UIPropertyDesc& /*InProperty*/)
    {
        /* Text from a binding or a script: the caret stays inside it. */
        Caret  = std::min(Caret, Text.size());
        Anchor = std::min(Anchor, Text.size());
        LaidOutGeneration = ~uint64_t(0);
    }

    double UITextInput::GetActionValue() const
    {
        return Mode == EUITextMode::Text ? 0.0 : std::atof(Text.c_str());
    }

    // === Include ==================================================================

    namespace
    {
        constexpr int MAX_INCLUDE_DEPTH = 8;
    }

    UIInclude::UIInclude()
    {
        Size = UIVec2{ 400.0f, 100.0f };
    }

    UIInclude::~UIInclude()
    {
        /* Its system let its UI graph go already (closing its root does that first). */
        Embedded.reset();
    }

    void UIInclude::DropEmbedded(const bool bInTellSystem)
    {
        if(!Embedded) return;
        UISystem* System = GetDocument() ? GetDocument()->GetSystem() : nullptr;
        if(bInTellSystem && System) System->DetachEmbedded(*Embedded);
        Embedded.reset();
        if(GetDocument()) GetDocument()->MarkLayoutDirty();
    }

    void UIInclude::EnsureEmbedded()
    {
        UIDocumentInstance* Host = GetDocument();
        if(!Host || !bStale) return;
        bStale = false;
        DropEmbedded(true);
        bRefused = false;
        if(Document.empty()) return;

        /* Not itself, at any depth; and not without end. */
        bool bCycle = Host->GetEmbedDepth() + 1 >= MAX_INCLUDE_DEPTH;
        for(UIDocumentInstance* Up = Host; Up && !bCycle; Up = Up->GetParentInstance()) { bCycle = Up->GetPath() == Document; }
        if(bCycle)
        {
            bRefused = true;
            UI_LOG_WARN("'" + Host->GetPath() + "': Include '" + Name + "' of '" + Document + "' would include itself (or go "
                        + std::to_string(MAX_INCLUDE_DEPTH) + " deep): left empty")
            return;
        }
        std::shared_ptr<const UIDocumentAsset> Asset = UIAssetCache::Find(Document);   // logged if missing
        if(!Asset) return;
        Generation = UIAssetCache::GetGeneration(Document);
        Embedded = std::make_unique<UIDocumentInstance>(Asset, Document);
        if(!Embedded->IsValid()) { Embedded.reset(); return; }
        Embedded->SetEmbedded(Host, GetIndex(), Prefix);

        /* One focus per tree: an included document's InitialFocus yields to its host's. */
        if(Embedded->GetFocused() >= 0 && Host->GetRootInstance()->GetTreeFocus().Doc != Embedded.get()) { Embedded->SetFocus(-1); }
        if(UISystem* System = Host->GetSystem()) System->AttachEmbedded(*Embedded);
        Host->MarkLayoutDirty();
    }

    void UIInclude::TickEmbedded()
    {
        /* Saved again (hot reload): made again. */
        if(Embedded && !Document.empty() && UIAssetCache::GetGeneration(Document) != Generation) bStale = true;
        if(bStale && !bRefused) EnsureEmbedded();
    }

    UIVec2 UIInclude::MeasureContent(UILayoutContext& InContext)
    {
        EnsureEmbedded();
        return Embedded ? Embedded->MeasureEmbedded(InContext) : UIVec2();
    }

    void UIInclude::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        if(Embedded) Embedded->ArrangeEmbedded(InContext, InRect);
    }

    void UIInclude::Paint(UIPaintContext& InContext, const UIRect& /*InRect*/) const
    {
        if(Embedded) Embedded->PaintEmbedded(InContext);
    }

    void UIInclude::OnPropertyChanged(const UIPropertyDesc& /*InProperty*/)
    {
        bStale = true;
        bRefused = false;
    }

    UIVec2 UISpacer::MeasureContent(UILayoutContext& /*InContext*/)
    {
        return Size;
    }

    // === Registration =============================================================
    // Each type's properties after UIWidget's (UIWidget.cpp), in the order the
    // editor shows them.

    UI_DEFINE_WIDGET(UICanvas, UIWidget, "Canvas")
    {
        Type.Palette("Layout").Children(-1);
    }

    UI_DEFINE_ABSTRACT_WIDGET_OF(UIContentWidget, UIWidget, "ContentWidget")
    {
        Type.Children(1);
        Type.Category("Layout");
        Type.Field<&UIContentWidget::Padding>("Padding", UPF_Layout);
    }

    UI_DEFINE_WIDGET(UIBorder, UIContentWidget, "Border")
    {
        Type.Palette("Common").Children(1);
        Type.Category("Appearance");
        Type.Brush<&UIBorder::Background>("Background");
    }

    UI_DEFINE_WIDGET(UIImage, UIWidget, "Image")
    {
        Type.Palette("Common");
        Type.Category("Appearance");
        Type.Brush<&UIImage::Brush>("Brush");
        Type.Field<&UIImage::DesiredSize>("DesiredSize", UPF_Layout).Tooltip("Its size in a box; 0,0 the texture's");
        Type.Enum<&UIImage::Stretch>("Stretch", GetEnumNames(EUIStretch()));
    }

    UI_DEFINE_WIDGET(UIText, UIWidget, "Text")
    {
        Type.Palette("Common");
        Type.Category("Text");
        Type.Field<&UIText::Text>("Text", UPF_Layout | UPF_Bindable | UPF_Multiline).Tooltip("{Bind:Key} is filled from the data store");
        Type.Asset<&UIText::Font>("Font", EUIAssetKind::Font, UPF_Layout).Tooltip("A .ttf; empty is the engine's");
        Type.Field<&UIText::FontSize>("FontSize", UPF_Layout);
        Type.Field<&UIText::Color>("Color", UPF_Animatable | UPF_Bindable);
        Type.Enum<&UIText::Justify>("Justify", GetEnumNames(EUIJustify()), UPF_Layout);
        Type.Field<&UIText::bWrap>("Wrap", UPF_Layout).Tooltip("Breaks lines at its width");
        Type.Field<&UIText::LineHeight>("LineHeight", UPF_Layout);
        Type.Field<&UIText::ShadowOffset>("ShadowOffset");
        Type.Field<&UIText::ShadowColor>("ShadowColor");
    }

    UI_DEFINE_WIDGET(UIButton, UIContentWidget, "Button")
    {
        Type.Palette("Common").Children(1);
        Type.Category("Appearance");
        Type.Brush<&UIButton::Normal>("Normal");
        Type.Brush<&UIButton::Hovered>("Hovered");
        Type.Brush<&UIButton::Pressed>("Pressed");
        Type.Brush<&UIButton::Disabled>("Disabled");
        Type.Category("Events");
        Type.Action<&UIButton::OnClick>("OnClick");
        Type.Action<&UIButton::OnPressed>("OnPressed");
        Type.Action<&UIButton::OnReleased>("OnReleased");
        Type.Action<&UIButton::OnHovered>("OnHovered");
        Type.Action<&UIButton::OnUnhovered>("OnUnhovered");
    }

    UI_DEFINE_ABSTRACT_WIDGET_OF(UIBoxWidget, UIWidget, "BoxWidget")
    {
        Type.Children(-1);
        Type.Category("Layout");
        Type.Field<&UIBoxWidget::Spacing>("Spacing", UPF_Layout);
    }

    UI_DEFINE_WIDGET(UIHorizontalBox, UIBoxWidget, "HorizontalBox")
    {
        Type.Palette("Layout").Children(-1);
    }

    UI_DEFINE_WIDGET(UIVerticalBox, UIBoxWidget, "VerticalBox")
    {
        Type.Palette("Layout").Children(-1);
    }

    UI_DEFINE_WIDGET(UIOverlay, UIWidget, "Overlay")
    {
        Type.Palette("Layout").Children(-1);
    }

    UI_DEFINE_WIDGET(UISwitcher, UIWidget, "Switcher")
    {
        Type.Palette("Layout").Children(-1);
        Type.Category("Pages");
        Type.Field<&UISwitcher::ActiveIndex>("ActiveIndex", UPF_Bindable)
            .Tooltip("The page shown, 0 first (its children in order); UI.ShowPage picks one by name");
    }

    UI_DEFINE_WIDGET(UIProgressBar, UIWidget, "ProgressBar")
    {
        Type.Palette("Common");
        Type.Category("Progress");
        Type.Field<&UIProgressBar::Value>("Value", UPF_Bindable | UPF_Animatable).Tooltip("0-1");
        Type.Enum<&UIProgressBar::FillDirection>("FillDirection", GetEnumNames(EUIFillDirection()));
        Type.Category("Appearance");
        Type.Brush<&UIProgressBar::Background>("Background");
        Type.Brush<&UIProgressBar::FillBrush>("FillBrush");
    }

    UI_DEFINE_WIDGET(UICheckBox, UIContentWidget, "CheckBox")
    {
        Type.Palette("Common").Children(1);
        Type.Category("CheckBox");
        Type.Field<&UICheckBox::bChecked>("Checked", UPF_Bindable).Tooltip("{Bind:Key} is two-way: a click writes the store");
        Type.Field<&UICheckBox::BoxSize>("BoxSize", UPF_Layout);
        Type.Field<&UICheckBox::Spacing>("Spacing", UPF_Layout).Tooltip("Between the box and its label");
        Type.Category("Appearance");
        Type.Brush<&UICheckBox::Box>("Box");
        Type.Brush<&UICheckBox::BoxHovered>("BoxHovered");
        Type.Brush<&UICheckBox::Check>("Check");
        Type.Category("Events");
        Type.Action<&UICheckBox::OnChanged>("OnChanged").Tooltip("$value is 1 checked, 0 not");
    }

    UI_DEFINE_WIDGET(UISlider, UIWidget, "Slider")
    {
        Type.Palette("Common");
        Type.Category("Slider");
        Type.Field<&UISlider::Value>("Value", UPF_Bindable | UPF_Animatable).Tooltip("{Bind:Key} is two-way: a drag writes the store");
        Type.Field<&UISlider::Min>("Min");
        Type.Field<&UISlider::Max>("Max");
        Type.Field<&UISlider::Step>("Step").Tooltip("Values are Min + n * Step; 0 any");
        Type.Field<&UISlider::TrackHeight>("TrackHeight");
        Type.Field<&UISlider::ThumbSize>("ThumbSize", UPF_Layout);
        Type.Category("Appearance");
        Type.Brush<&UISlider::Track>("Track");
        Type.Brush<&UISlider::FillBrush>("FillBrush");
        Type.Brush<&UISlider::Thumb>("Thumb");
        Type.Brush<&UISlider::ThumbHovered>("ThumbHovered");
        Type.Category("Events");
        Type.Action<&UISlider::OnChanged>("OnChanged").Tooltip("$value is the new Value");
    }

    UI_DEFINE_WIDGET(UIScrollBox, UIWidget, "ScrollBox")
    {
        Type.Palette("Layout").Children(1);
        Type.Category("Scroll");
        Type.Field<&UIScrollBox::ScrollOffset>("ScrollOffset", UPF_Bindable).Tooltip("Layout units from the start; {Bind:Key} is two-way");
        Type.Field<&UIScrollBox::ScrollStep>("ScrollStep").Tooltip("Layout units per wheel notch");
        Type.Enum<&UIScrollBox::Orientation>("Orientation", GetEnumNames(EUIOrientation()), UPF_Layout);
        Type.Enum<&UIScrollBox::ScrollBar>("ScrollBar", GetEnumNames(EUIScrollBarMode()), UPF_Layout);
        Type.Field<&UIScrollBox::BarWidth>("BarWidth", UPF_Layout);
        Type.Category("Appearance");
        Type.Brush<&UIScrollBox::Track>("Track");
        Type.Brush<&UIScrollBox::Thumb>("Thumb");
        Type.Brush<&UIScrollBox::ThumbHovered>("ThumbHovered");
        Type.Category("Events");
        Type.Action<&UIScrollBox::OnScrolled>("OnScrolled").Tooltip("$value is the offset");
    }

    UI_DEFINE_WIDGET(UIGrid, UIWidget, "Grid")
    {
        Type.Palette("Layout").Children(-1);
        Type.Category("Layout");
        Type.Field<&UIGrid::RowSpacing>("RowSpacing", UPF_Layout);
        Type.Field<&UIGrid::ColumnSpacing>("ColumnSpacing", UPF_Layout);
        Type.Field<&UIGrid::bUniform>("Uniform", UPF_Layout).Tooltip("Every cell the size of the largest, filling the grid");
    }

    UI_DEFINE_WIDGET(UIWrapBox, UIWidget, "WrapBox")
    {
        Type.Palette("Layout").Children(-1);
        Type.Category("Layout");
        Type.Field<&UIWrapBox::ItemSpacing>("ItemSpacing", UPF_Layout);
        Type.Field<&UIWrapBox::LineSpacing>("LineSpacing", UPF_Layout);
        Type.Enum<&UIWrapBox::LineAlign>("LineAlign", GetEnumNames(EUIJustify()), UPF_Layout);
    }

    UI_DEFINE_WIDGET(UITextInput, UIWidget, "TextInput")
    {
        Type.Palette("Common");
        Type.Category("Text");
        Type.Field<&UITextInput::Text>("Text", UPF_Bindable).Tooltip("{Bind:Key} is two-way: typing writes the store");
        Type.Field<&UITextInput::Placeholder>("Placeholder").Tooltip("Shown while it is empty");
        Type.Field<&UITextInput::MaxLength>("MaxLength").Tooltip("Characters; 0 any");
        Type.Enum<&UITextInput::Mode>("Mode", GetEnumNames(EUITextMode()));
        Type.Field<&UITextInput::bPassword>("Password").Tooltip("Shown as '*', never copied");
        Type.Asset<&UITextInput::Font>("Font", EUIAssetKind::Font);
        Type.Field<&UITextInput::FontSize>("FontSize", UPF_Layout);
        Type.Field<&UITextInput::Color>("Color");
        Type.Field<&UITextInput::PlaceholderColor>("PlaceholderColor");
        Type.Field<&UITextInput::SelectionColor>("SelectionColor");
        Type.Field<&UITextInput::CaretColor>("CaretColor");
        Type.Category("Appearance");
        Type.Field<&UITextInput::Padding>("Padding", UPF_Layout);
        Type.Brush<&UITextInput::Background>("Background");
        Type.Brush<&UITextInput::FocusedBrush>("FocusedBrush");
        Type.Category("Events");
        Type.Action<&UITextInput::OnChanged>("OnChanged").Tooltip("Each edit; $value the number (Integer, Number modes)");
        Type.Action<&UITextInput::OnCommit>("OnCommit").Tooltip("Return, or leaving it, if it changed");
    }

    UI_DEFINE_WIDGET(UIInclude, UIWidget, "Include")
    {
        Type.Palette("Layout");
        Type.Category("Include");
        Type.Asset<&UIInclude::Document>("Document", EUIAssetKind::Document, UPF_Layout).Tooltip("A .nmui shown here");
        Type.Field<&UIInclude::Prefix>("Prefix", UPF_Layout).Tooltip("Its {Bind:Key} reads Prefix + Key ({Bind:/Key}: the key as written)");
    }

    UI_DEFINE_WIDGET(UISpacer, UIWidget, "Spacer")
    {
        Type.Palette("Layout");
    }
}

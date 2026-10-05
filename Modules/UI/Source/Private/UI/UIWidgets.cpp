#include "Public/UI/UIWidgets.h"

#include <algorithm>
#include <cmath>

#include "Public/UI/UIDocument.h"
#include "Public/UI/UIFont.h"
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

    UI_DEFINE_WIDGET(UISpacer, UIWidget, "Spacer")
    {
        Type.Palette("Layout");
    }
}

#include "Public/UI/UIWidget.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "Public/RenderCore/TextureAsset.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidgets.h"
#include "Private/UI/UIInternal.h"

namespace UI
{
    UIVec2 GetTextureSize(const std::string& InPath)
    {
        static std::mutex                              Lock;
        static std::unordered_map<std::string, UIVec2> Sizes;
        if(InPath.empty()) return UIVec2();

        std::lock_guard<std::mutex> Guard(Lock);
        const auto Found = Sizes.find(InPath);
        if(Found != Sizes.end()) return Found->second;

        UIVec2 Size;
        if(const std::shared_ptr<const RenderCore::TextureData> Texture = RenderCore::TextureAssetCache::Find(InPath))
        {
            Size = UIVec2{ float(Texture->Pixels.GetWidth()), float(Texture->Pixels.GetHeight()) };
        }
        Sizes[InPath] = Size;
        return Size;
    }

    RenderCore::UIClipRect IntersectClip(const RenderCore::UIClipRect& InA, const RenderCore::UIClipRect& InB)
    {
        if(InA.IsNone()) return InB;
        if(InB.IsNone()) return InA;
        RenderCore::UIClipRect Out;
        Out.X = std::max(InA.X, InB.X);
        Out.Y = std::max(InA.Y, InB.Y);
        Out.W = std::max(std::min(InA.X + InA.W, InB.X + InB.W) - Out.X, 0);
        Out.H = std::max(std::min(InA.Y + InA.H, InB.Y + InB.H) - Out.Y, 0);
        return Out;
    }

    // === Contexts =================================================================

    UIVec2 UILayoutContext::Measure(const uint32_t InIndex)
    {
        return Document ? Document->Measure(*this, InIndex) : UIVec2();
    }

    void UILayoutContext::Arrange(const uint32_t InIndex, const UIRect& InRect)
    {
        if(Document) { Document->Arrange(*this, InIndex, InRect); }
    }

    UIRect UIPaintContext::ToPixels(const UIRect& InLayout) const
    {
        const float X0 = std::round(InLayout.X * PixelScale.X + Offset.X);
        const float Y0 = std::round(InLayout.Y * PixelScale.Y + Offset.Y);
        const float X1 = std::round((InLayout.X + InLayout.W) * PixelScale.X + Offset.X);
        const float Y1 = std::round((InLayout.Y + InLayout.H) * PixelScale.Y + Offset.Y);
        return UIRect{ X0, Y0, X1 - X0, Y1 - Y0 };
    }

    void UIPaintContext::DrawBrush(const UIBrush& InBrush, const UIRect& InLayout) const
    {
        if(!List || InBrush.DrawAs == EUIDrawAs::None) return;

        const UIRect   Pixels = ToPixels(InLayout);
        const uint32_t Color  = InBrush.Tint.Pack(Opacity);
        const UIRect   UV{ InBrush.UVRect.Left, InBrush.UVRect.Top, InBrush.UVRect.Right, InBrush.UVRect.Bottom };

        if(InBrush.Texture.empty())
        {
            List->AddRect(Pixels, Color);
        }
        else if(InBrush.DrawAs == EUIDrawAs::NineSlice)
        {
            /* Margins in texture pixels: on screen they scale with the
             * document; in UV they are a share of the texture. */
            const UIVec2 TexSize = GetTextureSize(InBrush.Texture);
            const float  TexW = TexSize.X > 0.0f ? TexSize.X : 1.0f, TexH = TexSize.Y > 0.0f ? TexSize.Y : 1.0f;
            const float  Margins[4]   = { InBrush.Margin.Left * Scale, InBrush.Margin.Top * Scale,
                                          InBrush.Margin.Right * Scale, InBrush.Margin.Bottom * Scale };
            const float  UVMargins[4] = { InBrush.Margin.Left / TexW, InBrush.Margin.Top / TexH,
                                          InBrush.Margin.Right / TexW, InBrush.Margin.Bottom / TexH };
            List->AddNineSlice(Pixels, Margins, UV, UVMargins, Color, List->FindOrAddTexture(InBrush.Texture));
        }
        else
        {
            List->AddImage(Pixels, UV, Color, List->FindOrAddTexture(InBrush.Texture));
        }

        if(InBrush.OutlineThickness > 0.0f && InBrush.OutlineColor.A > 0.0f)
        {
            List->AddOutline(Pixels, std::max(1.0f, std::round(InBrush.OutlineThickness * Scale)), InBrush.OutlineColor.Pack(Opacity));
        }
    }

    // === UIWidget =================================================================

    UIVec2 UIWidget::MeasureContent(UILayoutContext& InContext)
    {
        /* Any container's default: its children over each other (an Overlay). */
        UIVec2 Size;
        if(!Document) return Size;
        for(int32_t Child = Document->GetFirstChild(Index); Child >= 0; Child = Document->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Document->GetWidget(uint32_t(Child));
            const UIVec2 Desired = InContext.Measure(uint32_t(Child));
            Size.X = std::max(Size.X, Desired.X + C.Margin.Left + C.Margin.Right);
            Size.Y = std::max(Size.Y, Desired.Y + C.Margin.Top + C.Margin.Bottom);
        }
        return Size;
    }

    void UIWidget::ArrangeChildren(UILayoutContext& InContext, const UIRect& InRect)
    {
        if(!Document) return;
        for(int32_t Child = Document->GetFirstChild(Index); Child >= 0; Child = Document->GetNextSibling(uint32_t(Child)))
        {
            const UIWidget& C = *Document->GetWidget(uint32_t(Child));
            InContext.Arrange(uint32_t(Child), AlignInSlot(C, Document->GetDesiredSize(uint32_t(Child)), InRect));
        }
    }

    void UIWidget::Paint(UIPaintContext& /*InContext*/, const UIRect& /*InRect*/) const
    {
    }

    void UIWidget::QueueActions(const UIActionList& InActions)
    {
        if(!InActions.IsEmpty() && Document && Document->GetSystem())
        {
            Document->GetSystem()->QueueActions(InActions, Document, this);
        }
    }

    void UIWidget::GoToState(const char* InState)
    {
        /* Through its document's group (UIVisualState.h); without one, the name only. */
        const std::string State = InState ? InState : "";
        if(Document && Document->GoToVisualState(Index, State)) return;
        VisualState = State;
    }

    bool UIWidget::IsHovered() const { return Document && Document->GetHovered() == int32_t(Index); }
    bool UIWidget::IsPressed() const { return Document && Document->GetPressed() == int32_t(Index); }
    bool UIWidget::IsFocused() const { return Document && Document->GetFocused() == int32_t(Index); }

    bool UIWidget::IsEnabledInHierarchy() const
    {
        if(!Document) return bEnabled;
        for(int32_t At = int32_t(Index); At >= 0; At = Document->GetParent(uint32_t(At)))
        {
            if(!Document->GetWidget(uint32_t(At))->bEnabled) return false;
        }
        return true;
    }

/* Every widget's properties: its own, then where it goes in its parent (Slot). */
UI_DEFINE_ABSTRACT_WIDGET(UIWidget, "Widget")
{
    Type.Category("Common");
    Type.Field<&UIWidget::Name>("Name").Tooltip("How actions, Lua and navigation find it ($self)");
    Type.Field<&UIWidget::Class>("Class", UPF_Layout).Tooltip("Styles it takes, by name, separated by spaces");
    Type.Enum<&UIWidget::Visibility>("Visibility", GetEnumNames(EUIVisibility()), UPF_Layout | UPF_Bindable);
    Type.Field<&UIWidget::bEnabled>("IsEnabled", UPF_Bindable);
    Type.Field<&UIWidget::Opacity>("Opacity", UPF_Animatable | UPF_Bindable);

    Type.Category("Render Transform");
    Type.Field<&UIWidget::RenderTranslate>("RenderTranslate", UPF_Animatable).Tooltip("Drawn moved, in layout units; layout is unaffected");
    Type.Field<&UIWidget::RenderScale>("RenderScale", UPF_Animatable).Tooltip("Drawn scaled about its centre; layout is unaffected");
    Type.Field<&UIWidget::bClipChildren>("ClipChildren").Tooltip("Children are cut off at its edges");

    Type.Category("Focus");
    Type.Field<&UIWidget::bFocusable>("Focusable");
    Type.Field<&UIWidget::bInitialFocus>("InitialFocus").Tooltip("Focused when its document opens");
    Type.Field<&UIWidget::NavUp>("NavUp").Tooltip("The widget Up moves focus to; empty finds the nearest");
    Type.Field<&UIWidget::NavDown>("NavDown");
    Type.Field<&UIWidget::NavLeft>("NavLeft");
    Type.Field<&UIWidget::NavRight>("NavRight");

    Type.Category("Slot");
    Type.Field<&UIWidget::Anchors>("Anchors", UPF_SlotCanvas | UPF_Layout).Tooltip("Min X, min Y, max X, max Y: where in the parent, 0-1");
    Type.Field<&UIWidget::Position>("Position", UPF_SlotCanvas | UPF_Layout | UPF_Animatable);
    Type.Field<&UIWidget::Size>("Size", UPF_SlotCanvas | UPF_Layout);
    Type.Field<&UIWidget::Pivot>("Pivot", UPF_SlotCanvas | UPF_Layout).Tooltip("The point of it at Position, 0-1 of its size");
    Type.Field<&UIWidget::bAutoSize>("AutoSize", UPF_SlotCanvas | UPF_Layout).Tooltip("Its desired size instead of Size");
    Type.Field<&UIWidget::ZOrder>("ZOrder", UPF_SlotCanvas | UPF_Layout).Tooltip("Higher draws over its siblings");
    Type.Field<&UIWidget::Margin>("Margin", UPF_SlotCanvas | UPF_SlotBox | UPF_SlotGrid | UPF_Layout);
    Type.Enum<&UIWidget::HAlign>("HAlign", GetEnumNames(EUIHAlign()), UPF_SlotBox | UPF_SlotGrid | UPF_Layout);
    Type.Enum<&UIWidget::VAlign>("VAlign", GetEnumNames(EUIVAlign()), UPF_SlotBox | UPF_SlotGrid | UPF_Layout);
    Type.Field<&UIWidget::Fill>("Fill", UPF_SlotBox | UPF_Layout).Tooltip("In a box: a share of the space left; 0 its desired size");
    Type.Field<&UIWidget::Row>("Row", UPF_SlotGrid | UPF_Layout).Tooltip("In a Grid: its row, 0 first");
    Type.Field<&UIWidget::Column>("Column", UPF_SlotGrid | UPF_Layout);
    Type.Field<&UIWidget::RowSpan>("RowSpan", UPF_SlotGrid | UPF_Layout).Tooltip("How many rows it covers");
    Type.Field<&UIWidget::ColumnSpan>("ColumnSpan", UPF_SlotGrid | UPF_Layout);
}
}

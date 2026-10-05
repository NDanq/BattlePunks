#include "Public/UI/UIDocument.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "Public/UI/UIFile.h"
#include "Public/UI/UIFont.h"
#include "Public/UI/UILog.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidgets.h"
#include "Private/UI/UIInternal.h"

namespace UI
{
    namespace
    {
        /* InClass split at spaces. */
        std::vector<std::string> SplitClasses(const std::string& InClass)
        {
            std::vector<std::string> Out;
            std::istringstream Stream(InClass);
            std::string Each;
            while(Stream >> Each) { Out.push_back(Each); }
            return Out;
        }

        const UIPropertyText* FindText(const std::vector<UIPropertyText>& InList, const std::string& InName)
        {
            for(const UIPropertyText& Each : InList)
            {
                if(Each.Name == InName) return &Each;
            }
            return nullptr;
        }

        template<typename TEnum>
        bool ParseEnumText(const std::string& InText, TEnum& OutValue)
        {
            const UIEnumNames Names = GetEnumNames(TEnum());
            for(uint32_t At = 0; At < Names.Count; ++At)
            {
                if(InText == Names.Names[At]) { OutValue = TEnum(At); return true; }
            }
            return false;
        }

        template<typename TEnum>
        std::string EnumText(const TEnum InValue)
        {
            const UIEnumNames Names = GetEnumNames(TEnum());
            return uint32_t(InValue) < Names.Count ? Names.Names[uint32_t(InValue)] : "";
        }
    }

    // === UIWidgetDesc =============================================================

    const std::string* UIWidgetDesc::Find(const std::string& InName) const
    {
        const UIPropertyText* Found = FindText(Properties, InName);
        return Found ? &Found->Value : nullptr;
    }

    void UIWidgetDesc::Set(const std::string& InName, const std::string& InValue)
    {
        for(UIPropertyText& Each : Properties)
        {
            if(Each.Name == InName) { Each.Value = InValue; return; }
        }
        Properties.push_back({ InName, InValue });
    }

    bool UIWidgetDesc::Remove(const std::string& InName)
    {
        for(auto It = Properties.begin(); It != Properties.end(); ++It)
        {
            if(It->Name == InName) { Properties.erase(It); return true; }
        }
        return false;
    }

    std::string UIWidgetDesc::GetName() const
    {
        const std::string* Name = Find("Name");
        return Name ? *Name : std::string();
    }

    // === UIDocumentSettings =======================================================

    std::vector<UIPropertyText> UIDocumentSettings::ToText() const
    {
        /* Format first, then only what differs from the default -- as widgets write. */
        const UIDocumentSettings Default;
        std::vector<UIPropertyText> Out;
        Out.push_back({ "Format", std::to_string(Format) });
        Out.push_back({ "ReferenceSize", FormatVec2(ReferenceSize) });
        Out.push_back({ "ScaleMode", EnumText(ScaleMode) });
        Out.push_back({ "InputMode", EnumText(InputMode) });
        if(Layer != Default.Layer)        Out.push_back({ "Layer", std::to_string(Layer) });
        if(!Script.empty())               Out.push_back({ "Script", Script });
        if(!DefaultFont.empty())          Out.push_back({ "DefaultFont", DefaultFont });
        if(!OnOpen.empty())               Out.push_back({ "OnOpen", OnOpen });
        if(!OnClose.empty())              Out.push_back({ "OnClose", OnClose });
        if(!OnBack.empty())               Out.push_back({ "OnBack", OnBack });
        return Out;
    }

    bool UIDocumentSettings::SetFromText(const std::string& InName, const std::string& InValue)
    {
        if(InName == "Format")        { float V = 0; if(!ParseFloat(InValue, V)) return false; Format = uint32_t(V); return true; }
        if(InName == "ReferenceSize") { UIVec2 V; if(!ParseVec2(InValue, V) || V.X <= 0 || V.Y <= 0) return false; ReferenceSize = V; return true; }
        if(InName == "ScaleMode")     return ParseEnumText(InValue, ScaleMode);
        if(InName == "InputMode")     return ParseEnumText(InValue, InputMode);
        if(InName == "Layer")         { float V = 0; if(!ParseFloat(InValue, V)) return false; Layer = int32_t(V); return true; }
        if(InName == "Script")        { Script = InValue; return true; }
        if(InName == "DefaultFont")   { DefaultFont = InValue; return true; }
        if(InName == "OnOpen")        { OnOpen = InValue; return true; }
        if(InName == "OnClose")       { OnClose = InValue; return true; }
        if(InName == "OnBack")        { OnBack = InValue; return true; }
        return false;
    }

    // === UIDocumentAsset ==========================================================

    int32_t UIDocumentAsset::FindWidget(const std::string& InName) const
    {
        if(InName.empty()) return -1;
        for(size_t At = 0; At < Widgets.size(); ++At)
        {
            const std::string* Name = Widgets[At].Find("Name");
            if(Name && *Name == InName) return int32_t(At);
        }
        return -1;
    }

    int32_t UIDocumentAsset::GetSubtreeEnd(const int32_t InIndex) const
    {
        if(InIndex < 0 || size_t(InIndex) >= Widgets.size()) return InIndex;
        int32_t End = InIndex + 1;
        while(size_t(End) < Widgets.size())
        {
            /* Still inside if an ancestor of End is InIndex. */
            int32_t Up = Widgets[size_t(End)].Parent;
            while(Up > InIndex) { Up = Widgets[size_t(Up)].Parent; }
            if(Up != InIndex) break;
            ++End;
        }
        return End;
    }

    std::vector<int32_t> UIDocumentAsset::GetChildren(const int32_t InIndex) const
    {
        std::vector<int32_t> Out;
        for(size_t At = size_t(std::max(InIndex, -1) + 1); At < Widgets.size(); ++At)
        {
            if(Widgets[At].Parent == InIndex) Out.push_back(int32_t(At));
        }
        return Out;
    }

    std::vector<UIPropertyText> UIDocumentAsset::GetEffectiveProperties(const int32_t InIndex) const
    {
        std::vector<UIPropertyText> Out;
        if(InIndex < 0 || size_t(InIndex) >= Widgets.size()) return Out;
        const UIWidgetDesc& Desc = Widgets[size_t(InIndex)];
        const UIWidgetType* Type = FindWidgetType(Desc.Type.c_str());

        const auto Put = [&Out](const UIPropertyText& InEach)
        {
            for(UIPropertyText& Existing : Out)
            {
                if(Existing.Name == InEach.Name) { Existing.Value = InEach.Value; return; }
            }
            Out.push_back(InEach);
        };

        /* Styles in Class order, then the widget's own over them. */
        if(const std::string* Class = Desc.Find("Class"))
        {
            for(const std::string& Name : SplitClasses(*Class))
            {
                for(const UIStyle& Style : Styles)
                {
                    if(Style.Name != Name) continue;
                    if(!Style.Type.empty())
                    {
                        const UIWidgetType* StyleType = FindWidgetType(Style.Type.c_str());
                        if(!Type || !StyleType || !Type->IsA(*StyleType)) continue;
                    }
                    for(const UIPropertyText& Each : Style.Properties) { Put(Each); }
                }
            }
        }
        for(const UIPropertyText& Each : Desc.Properties) { Put(Each); }
        return Out;
    }

    void UIDocumentAsset::ApplyStyles()
    {
        if(Styles.empty()) return;
        for(size_t At = 0; At < Widgets.size(); ++At)
        {
            std::vector<UIPropertyText> Effective = GetEffectiveProperties(int32_t(At));
            /* The widget's own order first, then what styles added. */
            std::vector<UIPropertyText> Ordered;
            for(const UIPropertyText& Own : Widgets[At].Properties)
            {
                if(const UIPropertyText* Found = FindText(Effective, Own.Name)) Ordered.push_back(*Found);
            }
            for(const UIPropertyText& Each : Effective)
            {
                if(!FindText(Ordered, Each.Name)) Ordered.push_back(Each);
            }
            Widgets[At].Properties = std::move(Ordered);
        }
        Styles.clear();
    }

    bool UIDocumentAsset::Validate(std::string* OutError) const
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) *OutError = InWhy; return false; };
        if(Widgets.empty()) return Fail("it has no root widget");

        std::vector<int32_t> Children(Widgets.size(), 0);
        for(size_t At = 0; At < Widgets.size(); ++At)
        {
            const UIWidgetDesc& Desc = Widgets[At];
            const UIWidgetType* Type = FindWidgetType(Desc.Type.c_str());
            if(!Type)            return Fail("widget " + std::to_string(At) + ": no widget type '" + Desc.Type + "'");
            if(Type->IsAbstract()) return Fail("widget " + std::to_string(At) + ": '" + Desc.Type + "' cannot be created");
            if(At == 0 && Desc.Parent != -1) return Fail("the first widget must be the root");
            if(At > 0 && (Desc.Parent < 0 || size_t(Desc.Parent) >= At))
            {
                return Fail("widget " + std::to_string(At) + " ('" + Desc.Type + "'): its parent must come before it");
            }
            if(At > 0)
            {
                const UIWidgetType* ParentType = FindWidgetType(Widgets[size_t(Desc.Parent)].Type.c_str());
                ++Children[size_t(Desc.Parent)];
                if(ParentType && ParentType->MaxChildren >= 0 && Children[size_t(Desc.Parent)] > ParentType->MaxChildren)
                {
                    return Fail("a " + ParentType->Name + " takes " + std::to_string(ParentType->MaxChildren)
                                + (ParentType->MaxChildren == 1 ? " child" : " children"));
                }
            }
        }
        /* Document order: each subtree contiguous after its root. */
        for(size_t At = 1; At < Widgets.size(); ++At)
        {
            const int32_t Parent = Widgets[At].Parent;
            if(GetSubtreeEnd(Parent) <= int32_t(At)) return Fail("widget " + std::to_string(At) + " is not with its parent's other children");
        }
        return true;
    }

    bool UIDocumentAsset::CanTakeChild(const int32_t InParent) const
    {
        if(InParent < 0 || size_t(InParent) >= Widgets.size()) return false;
        const UIWidgetType* Type = FindWidgetType(Widgets[size_t(InParent)].Type.c_str());
        if(!Type) return false;
        return Type->MaxChildren < 0 || int32_t(GetChildren(InParent).size()) < Type->MaxChildren;
    }

    std::vector<UIWidgetDesc> UIDocumentAsset::CopyWidgets(const int32_t InIndex) const
    {
        std::vector<UIWidgetDesc> Out;
        if(InIndex < 0 || size_t(InIndex) >= Widgets.size()) return Out;
        const int32_t End = GetSubtreeEnd(InIndex);
        for(int32_t At = InIndex; At < End; ++At)
        {
            UIWidgetDesc Each = Widgets[size_t(At)];
            Each.Parent = At == InIndex ? -1 : Each.Parent - InIndex;
            Out.push_back(std::move(Each));
        }
        return Out;
    }

    int32_t UIDocumentAsset::InsertWidgets(const int32_t InParent, const int32_t InBefore, const std::vector<UIWidgetDesc>& InWidgets)
    {
        if(InWidgets.empty() || !CanTakeChild(InParent)) return -1;
        if(InBefore >= 0 && (size_t(InBefore) >= Widgets.size() || Widgets[size_t(InBefore)].Parent != InParent)) return -1;

        const int32_t At    = InBefore >= 0 ? InBefore : GetSubtreeEnd(InParent);
        const int32_t Count = int32_t(InWidgets.size());
        /* Everything from At moves down by Count; so do parents pointing there. */
        for(UIWidgetDesc& Each : Widgets)
        {
            if(Each.Parent >= At) Each.Parent += Count;
        }
        std::vector<UIWidgetDesc> Placed = InWidgets;
        for(int32_t Each = 0; Each < Count; ++Each)
        {
            Placed[size_t(Each)].Parent = Each == 0 ? InParent : Placed[size_t(Each)].Parent + At;
        }
        Widgets.insert(Widgets.begin() + At, Placed.begin(), Placed.end());
        return At;
    }

    bool UIDocumentAsset::RemoveWidget(const int32_t InIndex)
    {
        if(InIndex <= 0 || size_t(InIndex) >= Widgets.size()) return false;
        const int32_t End   = GetSubtreeEnd(InIndex);
        const int32_t Count = End - InIndex;
        Widgets.erase(Widgets.begin() + InIndex, Widgets.begin() + End);
        for(UIWidgetDesc& Each : Widgets)
        {
            if(Each.Parent >= End) Each.Parent -= Count;
        }
        return true;
    }

    int32_t UIDocumentAsset::MoveWidget(const int32_t InIndex, int32_t InNewParent, int32_t InBefore)
    {
        if(InIndex <= 0 || size_t(InIndex) >= Widgets.size() || InNewParent < 0 || size_t(InNewParent) >= Widgets.size()) return -1;
        const int32_t End = GetSubtreeEnd(InIndex);
        if(InNewParent >= InIndex && InNewParent < End) return -1;   // into itself
        if(InBefore == InIndex) return InIndex;                    // already there

        /* Room is checked without it, if it is already the parent's. */
        const bool bSameParent = Widgets[size_t(InIndex)].Parent == InNewParent;
        if(!bSameParent && !CanTakeChild(InNewParent)) return -1;

        const std::vector<UIWidgetDesc> Moving = CopyWidgets(InIndex);
        RemoveWidget(InIndex);
        const int32_t Count = End - InIndex;
        if(InNewParent >= End) InNewParent -= Count;
        if(InBefore >= End)    InBefore    -= Count;
        return InsertWidgets(InNewParent, InBefore, Moving);
    }

    // === UIAssetCache =============================================================

    namespace
    {
        struct CacheEntry
        {
            std::shared_ptr<const UIDocumentAsset> Asset;
            uint64_t                               Generation = 1;
        };
        struct Cache
        {
            std::mutex                                  Lock;
            std::unordered_map<std::string, CacheEntry> Entries;
            uint64_t                                    AllGeneration = 0;
        };
        Cache& GetCache()
        {
            static Cache Instance;
            return Instance;
        }
    }

    std::shared_ptr<const UIDocumentAsset> UIAssetCache::Find(const std::string& InPath)
    {
        Cache& C = GetCache();
        {
            std::lock_guard<std::mutex> Guard(C.Lock);
            auto Found = C.Entries.find(InPath);
            if(Found != C.Entries.end() && Found->second.Asset) return Found->second.Asset;
        }

        auto Loaded = std::make_shared<UIDocumentAsset>();
        std::string Error;
        if(!LoadUIDocument(InPath, *Loaded, &Error))
        {
            UI_LOG_ERROR("'" + InPath + "': " + Error)
            return nullptr;
        }
        std::lock_guard<std::mutex> Guard(C.Lock);
        CacheEntry& Entry = C.Entries[InPath];
        Entry.Asset = Loaded;
        return Entry.Asset;
    }

    void UIAssetCache::Invalidate(const std::string& InPath)
    {
        Cache& C = GetCache();
        std::lock_guard<std::mutex> Guard(C.Lock);
        if(InPath.empty())
        {
            for(auto& Each : C.Entries) { Each.second.Asset.reset(); ++Each.second.Generation; }
            ++C.AllGeneration;
            return;
        }
        CacheEntry& Entry = C.Entries[InPath];
        Entry.Asset.reset();
        ++Entry.Generation;
    }

    uint64_t UIAssetCache::GetGeneration(const std::string& InPath)
    {
        Cache& C = GetCache();
        std::lock_guard<std::mutex> Guard(C.Lock);
        auto Found = C.Entries.find(InPath);
        return (Found != C.Entries.end() ? Found->second.Generation : 1) + C.AllGeneration;
    }

    // === UIDocumentInstance =======================================================

    UIDocumentInstance::UIDocumentInstance(std::shared_ptr<const UIDocumentAsset> InAsset, const std::string& InPath)
        : Asset(std::move(InAsset))
        , Path(InPath)
    {
        if(!Asset) { Asset = std::make_shared<UIDocumentAsset>(); }
        Build();
    }

    UIDocumentInstance::~UIDocumentInstance()
    {
        for(size_t At = Widgets.size(); At-- > 0;)
        {
            Widgets[At]->~UIWidget();
        }
    }

    void UIDocumentInstance::Build()
    {
        std::string Error;
        if(!Asset->Validate(&Error))
        {
            UI_LOG_ERROR("'" + (Path.empty() ? std::string("a document") : Path) + "' cannot be shown: " + Error)
            return;
        }

        /* One block, every widget at its aligned offset, in document order. */
        const size_t Count = Asset->Widgets.size();
        std::vector<const UIWidgetType*> Types(Count);
        std::vector<size_t> Offsets(Count);
        size_t Bytes = 0;
        for(size_t At = 0; At < Count; ++At)
        {
            Types[At] = FindWidgetType(Asset->Widgets[At].Type.c_str());
            const size_t Align = std::max<size_t>(Types[At]->Align, 1);
            Bytes = (Bytes + Align - 1) / Align * Align;
            Offsets[At] = Bytes;
            Bytes += Types[At]->Size;
            if(Align > alignof(std::max_align_t))
            {
                UI_LOG_ERROR("widget type '" + Types[At]->Name + "' needs more alignment than a document gives")
                return;
            }
        }
        Block.reset(new uint8_t[std::max<size_t>(Bytes, 1)]);
        Widgets.resize(Count);
        for(size_t At = 0; At < Count; ++At)
        {
            UIWidget* Made = Types[At]->Construct(Block.get() + Offsets[At]);
            Made->Type     = Types[At];
            Made->Document = this;
            Made->Index    = uint32_t(At);
            Widgets[At]    = Made;
        }

        Parents.assign(Count, -1);
        FirstChildren.assign(Count, -1);
        NextSiblings.assign(Count, -1);
        PaintOrders.assign(Count, {});
        Rects.assign(Count, UIRect());
        PixelRects.assign(Count, UIRect());
        Desired.assign(Count, UIVec2());
        Measured.assign(Count, 0);
        std::vector<int32_t> LastChild(Count, -1);
        for(size_t At = 1; At < Count; ++At)
        {
            const int32_t Parent = Asset->Widgets[At].Parent;
            Parents[At] = Parent;
            if(LastChild[size_t(Parent)] < 0) FirstChildren[size_t(Parent)] = int32_t(At);
            else                              NextSiblings[size_t(LastChild[size_t(Parent)])] = int32_t(At);
            LastChild[size_t(Parent)] = int32_t(At);
        }

        /* Properties: styles, then the widget's own. */
        for(size_t At = 0; At < Count; ++At)
        {
            for(const UIPropertyText& Each : Asset->GetEffectiveProperties(int32_t(At)))
            {
                if(!SetPropertyText(*Widgets[At], Each.Name, Each.Value))
                {
                    UI_LOG_WARN("'" + Path + "': " + Asset->Widgets[At].Type + " '" + Widgets[At]->Name + "': "
                                + (Types[At]->FindProperty(Each.Name.c_str()) < 0 ? "no property " : "a bad value for ")
                                + Each.Name + " (\"" + Each.Value + "\")")
                }
            }
        }
        for(size_t At = 0; At < Count; ++At) { SortPaintOrder(uint32_t(At)); }

        for(size_t At = 0; At < Count; ++At)
        {
            if(Widgets[At]->bInitialFocus && SetFocus(int32_t(At))) break;
        }
        MarkLayoutDirty();
    }

    void UIDocumentInstance::SortPaintOrder(const uint32_t InIndex)
    {
        std::vector<uint32_t>& Order = PaintOrders[InIndex];
        Order.clear();
        for(int32_t Child = FirstChildren[InIndex]; Child >= 0; Child = NextSiblings[size_t(Child)]) { Order.push_back(uint32_t(Child)); }
        if(dynamic_cast<const UICanvas*>(Widgets[InIndex]))
        {
            std::stable_sort(Order.begin(), Order.end(),
                             [this](const uint32_t InA, const uint32_t InB) { return Widgets[InA]->ZOrder < Widgets[InB]->ZOrder; });
        }
    }

    UIWidget* UIDocumentInstance::FindWidget(const std::string& InName) const
    {
        if(InName.empty()) return nullptr;
        for(UIWidget* Each : Widgets)
        {
            if(Each->Name == InName) return Each;
        }
        return nullptr;
    }

    // --- Properties --------------------------------------------------------------

    bool UIDocumentInstance::ApplyProperty(UIWidget& InWidget, const int32_t InProperty, const UIValue& InValue)
    {
        const UIPropertyDesc& Desc = InWidget.Type->Properties[size_t(InProperty)];
        if(!Desc.Set || !Desc.Set(InWidget, InValue)) return false;
        InWidget.OnPropertyChanged(Desc);
        KeepFocusShown();
        if(Desc.Flags & UPF_Layout)
        {
            MarkLayoutDirty();
            if(Desc.Name == "ZOrder" && InWidget.Index > 0) { SortPaintOrder(uint32_t(Parents[InWidget.Index])); }
        }
        else
        {
            MarkPaintDirty();
        }
        return true;
    }

    bool UIDocumentInstance::SetProperty(UIWidget& InWidget, const std::string& InName, const UIValue& InValue)
    {
        if(InWidget.Document != this) return false;
        const int32_t Property = InWidget.Type->FindProperty(InName.c_str());
        if(Property < 0) return false;
        /* A plain value replaces a binding. */
        Bindings.erase(std::remove_if(Bindings.begin(), Bindings.end(), [&](const UIBinding& InEach)
                       { return InEach.Widget == InWidget.Index && InEach.Property == Property; }), Bindings.end());
        return ApplyProperty(InWidget, Property, InValue);
    }

    bool UIDocumentInstance::SetPropertyText(UIWidget& InWidget, const std::string& InName, const std::string& InText)
    {
        if(InWidget.Document != this) return false;
        const int32_t Property = InWidget.Type->FindProperty(InName.c_str());
        if(Property < 0) return false;
        const UIPropertyDesc& Desc = InWidget.Type->Properties[size_t(Property)];

        Bindings.erase(std::remove_if(Bindings.begin(), Bindings.end(), [&](const UIBinding& InEach)
                       { return InEach.Widget == InWidget.Index && InEach.Property == Property; }), Bindings.end());

        bool bWhole = false;
        if((Desc.Flags & UPF_Bindable) && ParseBinding(InText, bWhole))
        {
            const bool bText = Desc.Type == EUIPropertyType::String;
            if(!bWhole && !bText) return false;   // interpolation needs a String
            UIBinding Binding;
            Binding.Widget   = InWidget.Index;
            Binding.Property = Property;
            Binding.Template = InText;
            Binding.bWhole   = bWhole;
            Bindings.push_back(Binding);
            BindingsVersion = 0;   // the next ApplyBindings fills it
            if(bText)
            {
                /* Shown without its values until the store has them. */
                ApplyProperty(InWidget, Property, UIValue::String(ResolveBindingText(InText, UIDataStore())));
            }
            return true;
        }

        UIValue Value;
        if(!ParsePropertyValue(Desc, InText, Value)) return false;
        return ApplyProperty(InWidget, Property, Value);
    }

    bool UIDocumentInstance::SetInputValue(UIWidget& InWidget, const std::string& InName, const UIValue& InValue)
    {
        if(InWidget.Document != this) return false;
        const int32_t Property = InWidget.Type->FindProperty(InName.c_str());
        if(Property < 0) return false;
        const UIPropertyDesc& Desc = InWidget.Type->Properties[size_t(Property)];
        const UIValue Before = Desc.Get(InWidget);
        if(!ApplyProperty(InWidget, Property, InValue)) return false;
        const UIValue After = Desc.Get(InWidget);
        if(After == Before) return false;

        if(System)
        {
            for(const UIBinding& Each : Bindings)
            {
                if(Each.Widget != InWidget.Index || Each.Property != Property || !Each.bWhole) continue;
                std::vector<std::string> Keys;
                bool bWhole = false;
                if(ParseBinding(Each.Template, bWhole, &Keys) && !Keys.empty()) { System->SetValue(Keys[0], After); }
            }
        }
        return true;
    }

    UIValue UIDocumentInstance::GetProperty(const UIWidget& InWidget, const std::string& InName) const
    {
        const int32_t Property = InWidget.Type ? InWidget.Type->FindProperty(InName.c_str()) : -1;
        if(Property < 0) return UIValue();
        const UIPropertyDesc& Desc = InWidget.Type->Properties[size_t(Property)];
        return Desc.Get ? Desc.Get(InWidget) : UIValue();
    }

    // --- Layout ------------------------------------------------------------------

    UIVec2 UIDocumentInstance::Measure(UILayoutContext& InContext, const uint32_t InIndex)
    {
        if(InIndex >= Widgets.size()) return UIVec2();
        if(Measured[InIndex]) return Desired[InIndex];
        UIWidget& W = *Widgets[InIndex];
        UIVec2 Size;
        if(W.Visibility != EUIVisibility::Collapsed)
        {
            Size = W.MeasureContent(InContext);
        }
        else
        {
            /* Still measured, so its subtree has sizes when it shows again. */
            for(int32_t Child = FirstChildren[InIndex]; Child >= 0; Child = NextSiblings[size_t(Child)]) { Measure(InContext, uint32_t(Child)); }
        }
        Desired[InIndex]  = Size;
        Measured[InIndex] = 1;
        return Size;
    }

    void UIDocumentInstance::Arrange(UILayoutContext& InContext, const uint32_t InIndex, const UIRect& InRect)
    {
        if(InIndex >= Widgets.size()) return;
        Rects[InIndex] = InRect;
        UIWidget& W = *Widgets[InIndex];
        if(W.Visibility == EUIVisibility::Collapsed)
        {
            for(int32_t Child = FirstChildren[InIndex]; Child >= 0; Child = NextSiblings[size_t(Child)])
            {
                Arrange(InContext, uint32_t(Child), UIRect{ InRect.X, InRect.Y, 0.0f, 0.0f });
            }
            return;
        }
        W.ArrangeChildren(InContext, InRect);
    }

    bool UIDocumentInstance::Layout(const UIVec2& InViewportPixels)
    {
        if(Widgets.empty()) return false;
        if(!bLayoutDirty && InViewportPixels == Viewport) return false;
        Viewport = InViewportPixels;

        const UIDocumentSettings& S = Asset->Settings;
        const float ByWidth  = InViewportPixels.X / std::max(S.ReferenceSize.X, 1.0f);
        const float ByHeight = InViewportPixels.Y / std::max(S.ReferenceSize.Y, 1.0f);
        switch(S.ScaleMode)
        {
            case EUIScaleMode::FitHeight: Scale = ByHeight; break;
            case EUIScaleMode::FitWidth:  Scale = ByWidth;  break;
            case EUIScaleMode::Fit:       Scale = std::min(ByWidth, ByHeight); break;
            case EUIScaleMode::Fill:      Scale = std::max(ByWidth, ByHeight); break;
            case EUIScaleMode::None:      Scale = 1.0f; break;
        }
        if(!(Scale > 0.0f) || !std::isfinite(Scale)) Scale = 1.0f;
        LayoutSize = UIVec2{ InViewportPixels.X / Scale, InViewportPixels.Y / Scale };

        bLayoutDirty = false;
        std::fill(Measured.begin(), Measured.end(), uint8_t(0));
        UILayoutContext Context;
        Context.Document = this;
        Context.Fonts    = Fonts;
        Context.Scale    = Scale;
        Measure(Context, 0);
        Arrange(Context, 0, UIRect{ 0.0f, 0.0f, LayoutSize.X, LayoutSize.Y });
        bPaintDirty = true;
        return true;
    }

    // --- Paint -------------------------------------------------------------------

    bool UIDocumentInstance::IsSelfShown(const uint32_t InIndex) const
    {
        const EUIVisibility V = Widgets[InIndex]->Visibility;
        if(V == EUIVisibility::Hidden || V == EUIVisibility::Collapsed) return false;
        const int32_t Parent = Parents[InIndex];
        return Parent < 0 || Widgets[size_t(Parent)]->IsChildShown(InIndex);
    }

    void UIDocumentInstance::PaintWidget(const uint32_t InIndex, UIPaintContext& InContext)
    {
        UIWidget& W = *Widgets[InIndex];
        if(!IsSelfShown(InIndex))
        {
            /* Not drawn; nor are its children, which keep no rectangle to hit. */
            PixelRects[InIndex] = UIRect();
            std::vector<uint32_t> Stack(PaintOrders[InIndex].begin(), PaintOrders[InIndex].end());
            while(!Stack.empty())
            {
                const uint32_t Each = Stack.back();
                Stack.pop_back();
                PixelRects[Each] = UIRect();
                Stack.insert(Stack.end(), PaintOrders[Each].begin(), PaintOrders[Each].end());
            }
            return;
        }

        UIPaintContext Context = InContext;
        Context.Opacity *= std::clamp(W.Opacity, 0.0f, 1.0f);

        /* A render transform: scaled about its centre, then moved; children too. */
        const UIRect& Rect = Rects[InIndex];
        if(W.RenderScale != UIVec2{ 1.0f, 1.0f } || W.RenderTranslate != UIVec2())
        {
            const float CX = Rect.X + Rect.W * 0.5f, CY = Rect.Y + Rect.H * 0.5f;
            Context.Offset.X += (CX * (1.0f - W.RenderScale.X) + W.RenderTranslate.X) * Context.PixelScale.X;
            Context.Offset.Y += (CY * (1.0f - W.RenderScale.Y) + W.RenderTranslate.Y) * Context.PixelScale.Y;
            Context.PixelScale.X *= W.RenderScale.X;
            Context.PixelScale.Y *= W.RenderScale.Y;
        }

        PixelRects[InIndex] = Context.ToPixels(Rect);
        W.Paint(Context, Rect);

        if(W.bClipChildren) { DrawList.PushClip(PixelRects[InIndex]); }
        for(const uint32_t Child : PaintOrders[InIndex]) { PaintWidget(Child, Context); }
        if(W.bClipChildren) { DrawList.PopClip(); }
    }

    bool UIDocumentInstance::Paint()
    {
        if(Widgets.empty() || !bPaintDirty) return false;
        /* Cleared first: a widget may ask for another pass while painting (wrapped text). */
        bPaintDirty = false;
        DrawList.Reset();
        UIPaintContext Context;
        Context.List       = &DrawList;
        Context.Fonts      = Fonts;
        Context.Document   = this;
        Context.Scale      = Scale;
        Context.PixelScale = UIVec2{ Scale, Scale };
        PaintWidget(0, Context);
        ++PaintSerial;
        return true;
    }

    bool UIDocumentInstance::IsShown(const uint32_t InIndex) const
    {
        for(int32_t At = int32_t(InIndex); At >= 0 && size_t(At) < Widgets.size(); At = Parents[size_t(At)])
        {
            if(!IsSelfShown(uint32_t(At))) return false;
        }
        return InIndex < Widgets.size();
    }

    // --- Input -------------------------------------------------------------------

    int32_t UIDocumentInstance::HitTestWidget(const uint32_t InIndex, const UIVec2& InPixels, const RenderCore::UIClipRect& InClip,
                                              const bool bInDesign) const
    {
        const UIWidget& W = *Widgets[InIndex];
        if(!IsSelfShown(InIndex)) return -1;
        if(W.Visibility == EUIVisibility::HitTestInvisible && !bInDesign) return -1;

        const UIRect& Pixels = PixelRects[InIndex];
        RenderCore::UIClipRect Clip = InClip;
        if(W.bClipChildren)
        {
            RenderCore::UIClipRect Own;
            Own.X = int32_t(Pixels.X); Own.Y = int32_t(Pixels.Y); Own.W = int32_t(Pixels.W); Own.H = int32_t(Pixels.H);
            Clip = IntersectClip(InClip, Own);
        }
        /* Topmost first: children in reverse paint order. */
        const std::vector<uint32_t>& Order = PaintOrders[InIndex];
        for(size_t At = Order.size(); At-- > 0;)
        {
            const int32_t Hit = HitTestWidget(Order[At], InPixels, Clip, bInDesign);
            if(Hit >= 0) return Hit;
        }
        if(!bInDesign && (W.Visibility == EUIVisibility::SelfHitTestInvisible || !W.TakesHits())) return -1;
        return Contains(Pixels, InPixels) && Contains(InClip, InPixels) ? int32_t(InIndex) : -1;
    }

    int32_t UIDocumentInstance::HitTest(const UIVec2& InPixels, const bool bInDesign) const
    {
        return Widgets.empty() ? -1 : HitTestWidget(0, InPixels, RenderCore::UIClipRect(), bInDesign);
    }

    int32_t UIDocumentInstance::FindInteractive(const int32_t InIndex) const
    {
        for(int32_t At = InIndex; At >= 0 && size_t(At) < Widgets.size(); At = Parents[size_t(At)])
        {
            if(Widgets[size_t(At)]->IsInteractive()) return At;
        }
        return -1;
    }

    bool UIDocumentInstance::SetFocus(const int32_t InIndex)
    {
        if(InIndex == Focused) return true;
        if(InIndex >= 0)
        {
            if(size_t(InIndex) >= Widgets.size()) return false;
            const UIWidget& W = *Widgets[size_t(InIndex)];
            if(!W.bFocusable || !W.IsEnabledInHierarchy() || !IsShown(uint32_t(InIndex))) return false;
        }
        Focused = InIndex;
        MarkPaintDirty();
        return true;
    }

    void UIDocumentInstance::KeepFocusShown()
    {
        /* A page switched away, or a widget collapsed, under the focus: the
         * focus goes to the first one still shown. */
        if(Focused >= 0 && !IsShown(uint32_t(Focused)))
        {
            Focused = -1;
            Focused = FindNextFocus(+1);
            MarkPaintDirty();
        }
    }

    int32_t UIDocumentInstance::FindNextFocus(const int32_t InStep) const
    {
        const int32_t Count = int32_t(Widgets.size());
        if(Count == 0) return -1;
        int32_t At = Focused;
        for(int32_t Tried = 0; Tried < Count; ++Tried)
        {
            At = At < 0 ? (InStep > 0 ? 0 : Count - 1) : (At + InStep + Count) % Count;
            const UIWidget& W = *Widgets[size_t(At)];
            if(W.bFocusable && W.IsEnabledInHierarchy() && IsShown(uint32_t(At))) return At;
        }
        return -1;
    }

    int32_t UIDocumentInstance::FindFocusInDirection(const int32_t InFrom, const int InDirection) const
    {
        if(InFrom < 0 || size_t(InFrom) >= Widgets.size()) return FindNextFocus(+1);
        const UIWidget& From = *Widgets[size_t(InFrom)];
        const std::string* Named[4] = { &From.NavUp, &From.NavDown, &From.NavLeft, &From.NavRight };
        if(InDirection >= 0 && InDirection < 4 && !Named[InDirection]->empty())
        {
            const UIWidget* Target = FindWidget(*Named[InDirection]);
            return Target ? int32_t(Target->Index) : -1;
        }

        /* The nearest focusable centre that way, sideways distance counting double. */
        const UIRect& A = PixelRects[size_t(InFrom)];
        const float AX = A.X + A.W * 0.5f, AY = A.Y + A.H * 0.5f;
        int32_t Best = -1;
        float   BestScore = 0.0f;
        for(size_t At = 0; At < Widgets.size(); ++At)
        {
            const UIWidget& W = *Widgets[At];
            if(int32_t(At) == InFrom || !W.bFocusable || !W.IsEnabledInHierarchy() || !IsShown(uint32_t(At))) continue;
            const UIRect& B = PixelRects[At];
            const float DX = B.X + B.W * 0.5f - AX, DY = B.Y + B.H * 0.5f - AY;
            float Along = 0.0f, Across = 0.0f;
            switch(InDirection)
            {
                case 0: Along = -DY; Across = DX; break;
                case 1: Along =  DY; Across = DX; break;
                case 2: Along = -DX; Across = DY; break;
                default: Along = DX; Across = DY; break;
            }
            if(Along <= 0.5f) continue;
            const float Score = Along + 2.0f * std::fabs(Across);
            if(Best < 0 || Score < BestScore) { Best = int32_t(At); BestScore = Score; }
        }
        return Best;
    }

    void UIDocumentInstance::SetHovered(const int32_t InIndex)
    {
        if(InIndex == Hovered) return;
        const int32_t Previous = Hovered;
        Hovered = InIndex;
        if(Previous >= 0 && size_t(Previous) < Widgets.size()) Widgets[size_t(Previous)]->OnPointer({ UIPointerEvent::PE_Leave, UIVec2() });
        if(Hovered >= 0 && size_t(Hovered) < Widgets.size())  Widgets[size_t(Hovered)]->OnPointer({ UIPointerEvent::PE_Enter, UIVec2() });
        MarkPaintDirty();
    }

    void UIDocumentInstance::SetPressed(const int32_t InIndex)
    {
        if(InIndex == Pressed) return;
        Pressed = InIndex;
        MarkPaintDirty();
    }

    // --- Bindings ----------------------------------------------------------------

    bool UIDocumentInstance::ApplyBindings(const UIDataStore& InStore)
    {
        if(Bindings.empty() || InStore.GetVersion() == BindingsVersion) return false;
        BindingsVersion = InStore.GetVersion();

        bool bChanged = false;
        for(const UIBinding& Each : Bindings)
        {
            UIWidget& W = *Widgets[Each.Widget];
            const UIPropertyDesc& Desc = W.Type->Properties[size_t(Each.Property)];
            UIValue Value;
            if(Each.bWhole)
            {
                Value = ResolveBindingValue(Each.Template, InStore);
                if(Value.IsNone()) continue;   // unset: the property keeps what it has
                if(Desc.Type == EUIPropertyType::String || Desc.Type == EUIPropertyType::Asset) Value = UIValue::String(Value.AsString());
            }
            else
            {
                Value = UIValue::String(ResolveBindingText(Each.Template, InStore));
            }
            /* Only a change costs anything: compared as the property holds it. */
            const UIValue Before = Desc.Get(W);
            if(Desc.Set(W, Value))
            {
                if(Desc.Get(W) != Before)
                {
                    W.OnPropertyChanged(Desc);
                    KeepFocusShown();
                    if(Desc.Flags & UPF_Layout) MarkLayoutDirty(); else MarkPaintDirty();
                    bChanged = true;
                }
            }
        }
        return bChanged;
    }

    // --- Animation ---------------------------------------------------------------

    bool UIDocumentInstance::PlayAnimation(const std::string& InName)
    {
        if(!bWarnedAnimation)
        {
            bWarnedAnimation = true;
            UI_LOG_WARN("'" + Path + "': animations do not play yet (asked for '" + InName + "')")
        }
        return Animator.Play(UIAnimationAsset()) != UI_ANIMATION_INVALID;
    }
}

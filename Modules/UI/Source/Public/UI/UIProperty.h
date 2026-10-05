#ifndef UI_UIPROPERTY_H
#define UI_UIPROPERTY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "Public/UI/UIAction.h"
#include "Public/UI/UIAPI.h"
#include "Public/UI/UITypes.h"

/*
 * The widget type registry: every widget class, its XML element name and
 * its properties, each read and written through a UIValue. XML, the cooked
 * binary, bindings, Lua and the editor's details panel all go through it, so
 * a property declared once is everywhere.
 *
 *     UI_DEFINE_WIDGET(UIButton, UIWidget, "Button")
 *     {
 *         Type.Palette("Common").Children(1);
 *         Type.Category("Appearance");
 *         Type.Brush<&UIButton::Normal>("Normal");
 *         Type.Category("Events");
 *         Type.Action<&UIButton::OnClick>("OnClick");
 *     }
 *
 * A type's properties start with its parent's. A struct property (a Brush)
 * flattens to dotted names: Normal.Texture, Normal.Tint, ...
 *
 * Properties are bound by member pointer, through generated accessors -- no
 * offsets, so a widget may be any class. Unlike ComponentProperties.h this is
 * in every build: a game reads documents and bindings set properties.
 */
namespace UI
{
    class UIWidget;

    enum class EUIPropertyType : uint8_t
    {
        Float, Int, Bool, Enum, Color, Vec2, Margin, String, Asset, Action
    };

    /* What an Asset property names, for the editor's picker. */
    enum class EUIAssetKind : uint8_t { None, Texture, Font, Script, Document };

    enum EUIPropertyFlags : uint32_t
    {
        UPF_None         = 0,
        UPF_ReadOnly     = 1u << 0,
        UPF_Animatable   = 1u << 1,   // a future UIAnimation track may drive it (Float, Color, Vec2)
        UPF_Bindable     = 1u << 2,   // takes {Bind:Key} (UIBinding.h)
        UPF_SlotCanvas   = 1u << 3,   // a slot property its parent reads when it is a Canvas
        UPF_SlotBox      = 1u << 4,   // ... when it is a box, an Overlay, a Border or a Button
        UPF_EditorHidden = 1u << 5,
        UPF_Layout       = 1u << 6,   // changing it moves things: a relayout, not just a repaint
        UPF_Multiline    = 1u << 7,   // a String the editor edits in a box
    };

    struct UIPropertyDesc
    {
        std::string     Name;          // "Opacity", "Normal.Tint"
        uint32_t        NameHash = 0;  // HashName(Name): what the cooked file stores
        EUIPropertyType Type     = EUIPropertyType::Float;
        uint32_t        Flags    = UPF_None;
        std::string     Category;
        UIEnumNames     Choices;       // Enum
        EUIAssetKind    AssetKind = EUIAssetKind::None;
        std::string     Tooltip;

        UIValue (*Get)(const UIWidget& InWidget) = nullptr;
        /* False if InValue cannot become this property's type. */
        bool    (*Set)(UIWidget& InWidget, const UIValue& InValue) = nullptr;
    };

    /* A property's value as XML writes it, and back. */
    UI_API std::string FormatPropertyValue(const UIPropertyDesc& InDesc, const UIValue& InValue);
    UI_API bool        ParsePropertyValue(const UIPropertyDesc& InDesc, const std::string& InText, UIValue& OutValue);

    struct UIWidgetType
    {
        std::string                 ClassName;    // "UIButton"
        std::string                 Name;         // "Button": the XML element
        uint32_t                    NameHash = 0;
        const UIWidgetType*         Parent   = nullptr;
        std::vector<UIPropertyDesc> Properties;   // the parent's first
        std::string                 Palette;      // the editor's palette group; "" not offered (abstract)
        int32_t                     MaxChildren = 0;   // -1 any
        size_t                      Size  = 0;
        size_t                      Align = 0;
        /* Constructs one in InMemory (Size, Align); null for an abstract type. */
        UIWidget*                 (*Construct)(void* InMemory) = nullptr;

        bool IsAbstract() const { return Construct == nullptr; }

        /* -1 if InName / InNameHash is not one of Properties. */
        UI_API int32_t FindProperty(const char* InName) const;
        UI_API int32_t FindPropertyByHash(uint32_t InNameHash) const;
        /* Whether this is InOther or derives from it. */
        UI_API bool IsA(const UIWidgetType& InOther) const;
    };

    /* Null if no type has that XML name or class name (now). */
    UI_API const UIWidgetType* FindWidgetType(const char* InName);
    UI_API const UIWidgetType* FindWidgetTypeByHash(uint32_t InNameHash);
    UI_API const UIWidgetType* FindWidgetTypeByClass(const char* InClassName);

    /* Every registered type, abstract ones too, in name order. Valid until
     * the next register or unregister. */
    UI_API std::vector<const UIWidgetType*> GetWidgetTypes();

    // === Registration =========================================================

    namespace Detail
    {
        template<typename T> struct MemberTraits;
        template<typename TClass, typename TField> struct MemberTraits<TField TClass::*>
        {
            using Class = TClass;
            using Field = TField;
        };

        // To and from a UIValue, by field type.
        inline UIValue ToValue(const float InValue)          { return UIValue::Number(InValue); }
        inline UIValue ToValue(const int32_t InValue)        { return UIValue::Number(InValue); }
        inline UIValue ToValue(const bool InValue)           { return UIValue::Bool(InValue); }
        inline UIValue ToValue(const UIColor& InValue)       { return UIValue::Color(InValue); }
        inline UIValue ToValue(const UIVec2& InValue)        { return UIValue::Vec2(InValue); }
        inline UIValue ToValue(const UIMargin& InValue)      { return UIValue::Margin(InValue); }
        inline UIValue ToValue(const std::string& InValue)   { return UIValue::String(InValue); }
        inline UIValue ToValue(const UIActionList& InValue)  { return UIValue::String(InValue.Source); }
        template<typename TEnum, typename = std::enable_if_t<std::is_enum<TEnum>::value>>
        inline UIValue ToValue(const TEnum InValue)          { return UIValue::Number(double(InValue)); }

        inline bool FromValue(const UIValue& In, float& Out)         { if(In.IsNone()) return false; Out = float(In.AsNumber()); return true; }
        inline bool FromValue(const UIValue& In, int32_t& Out)       { if(In.IsNone()) return false; Out = int32_t(In.AsNumber()); return true; }
        inline bool FromValue(const UIValue& In, bool& Out)          { if(In.IsNone()) return false; Out = In.AsBool(); return true; }
        inline bool FromValue(const UIValue& In, UIColor& Out)       { if(In.IsNone()) return false; Out = In.AsColor(); return true; }
        inline bool FromValue(const UIValue& In, UIVec2& Out)        { if(In.IsNone()) return false; Out = In.AsVec2(); return true; }
        inline bool FromValue(const UIValue& In, UIMargin& Out)      { if(In.IsNone()) return false; Out = In.AsMargin(); return true; }
        inline bool FromValue(const UIValue& In, std::string& Out)   { Out = In.AsString(); return true; }
        inline bool FromValue(const UIValue& In, UIActionList& Out)  { Out.Parse(In.AsString()); return true; }
        template<typename TEnum, typename = std::enable_if_t<std::is_enum<TEnum>::value>>
        inline bool FromValue(const UIValue& In, TEnum& Out)
        {
            if(In.Type != EUIValueType::Number && In.Type != EUIValueType::Bool) return false;
            Out = TEnum(int(In.AsNumber()));
            return true;
        }

        template<auto TMember>
        UIValue Get(const UIWidget& InWidget)
        {
            using Traits = MemberTraits<decltype(TMember)>;
            return ToValue(static_cast<const typename Traits::Class&>(InWidget).*TMember);
        }
        template<auto TMember>
        bool Set(UIWidget& InWidget, const UIValue& InValue)
        {
            using Traits = MemberTraits<decltype(TMember)>;
            return FromValue(InValue, static_cast<typename Traits::Class&>(InWidget).*TMember);
        }
        /* A struct property's field: (widget.*TOuter).*TInner. */
        template<auto TOuter, auto TInner>
        UIValue GetNested(const UIWidget& InWidget)
        {
            using Outer = MemberTraits<decltype(TOuter)>;
            return ToValue((static_cast<const typename Outer::Class&>(InWidget).*TOuter).*TInner);
        }
        template<auto TOuter, auto TInner>
        bool SetNested(UIWidget& InWidget, const UIValue& InValue)
        {
            using Outer = MemberTraits<decltype(TOuter)>;
            return FromValue(InValue, (static_cast<typename Outer::Class&>(InWidget).*TOuter).*TInner);
        }

        template<typename TField> constexpr EUIPropertyType TypeOf()
        {
            if constexpr(std::is_same<TField, float>::value)        return EUIPropertyType::Float;
            else if constexpr(std::is_same<TField, int32_t>::value) return EUIPropertyType::Int;
            else if constexpr(std::is_same<TField, bool>::value)    return EUIPropertyType::Bool;
            else if constexpr(std::is_same<TField, UIColor>::value) return EUIPropertyType::Color;
            else if constexpr(std::is_same<TField, UIVec2>::value)  return EUIPropertyType::Vec2;
            else if constexpr(std::is_same<TField, UIMargin>::value) return EUIPropertyType::Margin;
            else if constexpr(std::is_same<TField, UIActionList>::value) return EUIPropertyType::Action;
            else if constexpr(std::is_enum<TField>::value)          return EUIPropertyType::Enum;
            else                                                     return EUIPropertyType::String;
        }

        template<typename TClass>
        UIWidget* Construct(void* InMemory) { return new(InMemory) TClass(); }
    }

    /* What `Type` is inside UI_DEFINE_WIDGET. Records plain data; registered at Commit. */
    class UI_API UIWidgetTypeBinder
    {
    public:
        /* The palette group the editor offers it in; "" (the default) offers it nowhere. */
        UIWidgetTypeBinder& Palette(const char* InGroup) { Type.Palette = InGroup; return *this; }

        /* How many children it takes: 0 none (the default), 1, or -1 any. */
        UIWidgetTypeBinder& Children(const int32_t InMax) { Type.MaxChildren = InMax; return *this; }

        /* The group the next properties are shown under. */
        UIWidgetTypeBinder& Category(const char* InName) { CurrentCategory = InName; return *this; }

        /* Adds to the last property's flags. */
        UIWidgetTypeBinder& Flags(const uint32_t InFlags) { if(!Type.Properties.empty()) Type.Properties.back().Flags |= InFlags; return *this; }
        UIWidgetTypeBinder& Tooltip(const char* InText) { if(!Type.Properties.empty()) Type.Properties.back().Tooltip = InText; return *this; }

        /* A plain field: float, int32_t, bool, UIColor, UIVec2, UIMargin, std::string. */
        template<auto TMember>
        UIWidgetTypeBinder& Field(const char* InName, const uint32_t InFlags = UPF_None)
        {
            using TField = typename Detail::MemberTraits<decltype(TMember)>::Field;
            return Add(InName, Detail::TypeOf<TField>(), InFlags, &Detail::Get<TMember>, &Detail::Set<TMember>);
        }

        /* An enum field, with the names XML writes (GetEnumNames). */
        template<auto TMember>
        UIWidgetTypeBinder& Enum(const char* InName, const UIEnumNames InChoices, const uint32_t InFlags = UPF_None)
        {
            Add(InName, EUIPropertyType::Enum, InFlags, &Detail::Get<TMember>, &Detail::Set<TMember>);
            Type.Properties.back().Choices = InChoices;
            return *this;
        }

        /* A std::string naming an asset. */
        template<auto TMember>
        UIWidgetTypeBinder& Asset(const char* InName, const EUIAssetKind InKind, const uint32_t InFlags = UPF_None)
        {
            Add(InName, EUIPropertyType::Asset, InFlags, &Detail::Get<TMember>, &Detail::Set<TMember>);
            Type.Properties.back().AssetKind = InKind;
            return *this;
        }

        /* A UIActionList. */
        template<auto TMember>
        UIWidgetTypeBinder& Action(const char* InName)
        {
            return Add(InName, EUIPropertyType::Action, UPF_None, &Detail::Get<TMember>, &Detail::Set<TMember>);
        }

        /* A UIBrush, as InName.Texture, .Tint, .DrawAs, .Margin, .UVRect, .OutlineColor, .OutlineThickness. */
        template<auto TMember>
        UIWidgetTypeBinder& Brush(const char* InName, const uint32_t InFlags = UPF_None)
        {
            const std::string Prefix = std::string(InName) + ".";
            Add((Prefix + "Texture").c_str(), EUIPropertyType::Asset, InFlags,
                &Detail::GetNested<TMember, &UIBrush::Texture>, &Detail::SetNested<TMember, &UIBrush::Texture>);
            Type.Properties.back().AssetKind = EUIAssetKind::Texture;
            Add((Prefix + "Tint").c_str(), EUIPropertyType::Color, InFlags | UPF_Animatable | UPF_Bindable,
                &Detail::GetNested<TMember, &UIBrush::Tint>, &Detail::SetNested<TMember, &UIBrush::Tint>);
            Add((Prefix + "DrawAs").c_str(), EUIPropertyType::Enum, InFlags,
                &Detail::GetNested<TMember, &UIBrush::DrawAs>, &Detail::SetNested<TMember, &UIBrush::DrawAs>);
            Type.Properties.back().Choices = GetEnumNames(EUIDrawAs());
            Add((Prefix + "Margin").c_str(), EUIPropertyType::Margin, InFlags,
                &Detail::GetNested<TMember, &UIBrush::Margin>, &Detail::SetNested<TMember, &UIBrush::Margin>);
            Add((Prefix + "UVRect").c_str(), EUIPropertyType::Margin, InFlags,
                &Detail::GetNested<TMember, &UIBrush::UVRect>, &Detail::SetNested<TMember, &UIBrush::UVRect>);
            Add((Prefix + "OutlineColor").c_str(), EUIPropertyType::Color, InFlags | UPF_Animatable,
                &Detail::GetNested<TMember, &UIBrush::OutlineColor>, &Detail::SetNested<TMember, &UIBrush::OutlineColor>);
            Add((Prefix + "OutlineThickness").c_str(), EUIPropertyType::Float, InFlags,
                &Detail::GetNested<TMember, &UIBrush::OutlineThickness>, &Detail::SetNested<TMember, &UIBrush::OutlineThickness>);
            return *this;
        }

        // --- the macro's ------------------------------------------------------
        UIWidgetTypeBinder(const char* InClassName, const char* InParentClassName, const char* InName);

        template<typename TClass>
        void SetClass(const bool bInAbstract)
        {
            Type.Size  = sizeof(TClass);
            Type.Align = alignof(TClass);
            if(!bInAbstract) { Type.Construct = &Detail::Construct<TClass>; }
        }

        /* Registers what was declared; the handle for Unregister. */
        uint32_t Commit();

    private:
        UIWidgetTypeBinder& Add(const char* InName, EUIPropertyType InType, uint32_t InFlags,
                                UIValue (*InGet)(const UIWidget&), bool (*InSet)(UIWidget&, const UIValue&));

        UIWidgetType Type;
        std::string  ParentClassName;
        std::string  CurrentCategory = "Common";
    };

    UI_API void UnregisterWidgetType(uint32_t InHandle);
}

/*
 * Registers Class (deriving ParentClass) as the XML element XmlName while
 * this image is loaded. The body declares its properties through `Type`.
 * UI_DEFINE_ABSTRACT_WIDGET(_OF): properties only, never created -- UIWidget
 * itself, or a base its subclasses share (UIBoxWidget).
 */
#define UI_DEFINE_WIDGET_IMPL(Class, ParentClass, XmlName, bAbstract)                           \
    static void Class##_UIWidgetBody(::UI::UIWidgetTypeBinder& Type);                           \
    namespace                                                                                   \
    {                                                                                           \
        struct Class##_UIWidgetRegistrar                                                        \
        {                                                                                       \
            Class##_UIWidgetRegistrar()                                                         \
            {                                                                                   \
                ::UI::UIWidgetTypeBinder Type(#Class, ParentClass, XmlName);                    \
                Type.SetClass<Class>(bAbstract);                                                \
                Class##_UIWidgetBody(Type);                                                     \
                Handle = Type.Commit();                                                         \
            }                                                                                   \
            ~Class##_UIWidgetRegistrar() { ::UI::UnregisterWidgetType(Handle); }                \
            uint32_t Handle = 0;                                                                \
        };                                                                                      \
        Class##_UIWidgetRegistrar G##Class##_UIWidgetRegistrar;                                 \
    }                                                                                           \
    static void Class##_UIWidgetBody(::UI::UIWidgetTypeBinder& Type)

#define UI_DEFINE_WIDGET(Class, ParentClass, XmlName)                   UI_DEFINE_WIDGET_IMPL(Class, #ParentClass, XmlName, false)
#define UI_DEFINE_ABSTRACT_WIDGET(Class, XmlName)                       UI_DEFINE_WIDGET_IMPL(Class, "", XmlName, true)
#define UI_DEFINE_ABSTRACT_WIDGET_OF(Class, ParentClass, XmlName)       UI_DEFINE_WIDGET_IMPL(Class, #ParentClass, XmlName, true)

#endif //UI_UIPROPERTY_H

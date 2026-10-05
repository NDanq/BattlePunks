#ifndef UI_UITYPES_H
#define UI_UITYPES_H

#include <cstdint>
#include <string>

#include "Public/RenderCore/UIDrawList.h"
#include "Public/UI/UIAPI.h"

/*
 * The UI's value types: the small structs widget properties are made of,
 * the enums they choose from (each with the names XML writes), and UIValue,
 * the one variant a property is read and written through.
 *
 * Layout units: what a document is authored in -- pixels at its root's
 * ReferenceSize. The document scales them to the viewport (EUIScaleMode).
 */
namespace UI
{
    using UIRect = RenderCore::UIRect;

    struct UIVec2
    {
        float X = 0.0f, Y = 0.0f;

        bool operator==(const UIVec2& InOther) const { return X == InOther.X && Y == InOther.Y; }
        bool operator!=(const UIVec2& InOther) const { return !(*this == InOther); }
    };

    /* Left, top, right, bottom -- or, as Anchors, min X, min Y, max X, max Y. */
    struct UIMargin
    {
        float Left = 0.0f, Top = 0.0f, Right = 0.0f, Bottom = 0.0f;

        bool operator==(const UIMargin& InOther) const
        {
            return Left == InOther.Left && Top == InOther.Top && Right == InOther.Right && Bottom == InOther.Bottom;
        }
        bool operator!=(const UIMargin& InOther) const { return !(*this == InOther); }
    };

    /* Straight (not premultiplied) RGBA, 0-1, display space. */
    struct UIColor
    {
        float R = 1.0f, G = 1.0f, B = 1.0f, A = 1.0f;

        bool operator==(const UIColor& InOther) const
        {
            return R == InOther.R && G == InOther.G && B == InOther.B && A == InOther.A;
        }
        bool operator!=(const UIColor& InOther) const { return !(*this == InOther); }

        /* A vertex colour (RenderCore::PackUIColor), alpha times InOpacity. */
        UI_API uint32_t Pack(float InOpacity = 1.0f) const;
    };

    // === Enums ================================================================
    // Each with its XML names, in order: GetEnumNames(...) for the registry.

    enum class EUIVisibility : uint8_t
    {
        Visible,               // drawn and hit
        Hidden,                // not drawn, still laid out
        Collapsed,             // not drawn, takes no space
        HitTestInvisible,      // drawn; neither it nor its children are hit
        SelfHitTestInvisible,  // drawn; its children are hit, it is not
    };

    enum class EUIHAlign : uint8_t { Fill, Left, Center, Right };
    enum class EUIVAlign : uint8_t { Fill, Top, Center, Bottom };

    /* How a document's ReferenceSize meets the viewport. */
    enum class EUIScaleMode : uint8_t
    {
        FitHeight,   // scale by height; the width follows the aspect
        FitWidth,
        Fit,         // the smaller scale: the whole reference fits
        Fill,        // the larger: the reference covers the viewport
        None,        // layout units are pixels
    };

    /* Who sees the input over a document (UISystem::ProcessInput). */
    enum class EUIInputMode : uint8_t
    {
        GameOnly,    // never consumes
        GameAndUI,   // consumes the mouse over a widget it hits
        UIOnly,      // consumes everything; shows the cursor
    };

    enum class EUIDrawAs : uint8_t { None, Box, Image, NineSlice };
    enum class EUIStretch : uint8_t { Fill, None, Uniform, UniformToFill };
    enum class EUIJustify : uint8_t { Left, Center, Right };
    enum class EUIFillDirection : uint8_t { LeftToRight, RightToLeft, TopToBottom, BottomToTop };
    enum class EUIOrientation : uint8_t { Vertical, Horizontal };
    enum class EUIScrollBarMode : uint8_t { Auto, Always, Never };   // Auto: while there is more to see

    struct UIEnumNames
    {
        const char* const* Names = nullptr;
        uint32_t           Count = 0;
    };
    UI_API UIEnumNames GetEnumNames(EUIVisibility);
    UI_API UIEnumNames GetEnumNames(EUIHAlign);
    UI_API UIEnumNames GetEnumNames(EUIVAlign);
    UI_API UIEnumNames GetEnumNames(EUIScaleMode);
    UI_API UIEnumNames GetEnumNames(EUIInputMode);
    UI_API UIEnumNames GetEnumNames(EUIDrawAs);
    UI_API UIEnumNames GetEnumNames(EUIStretch);
    UI_API UIEnumNames GetEnumNames(EUIJustify);
    UI_API UIEnumNames GetEnumNames(EUIFillDirection);
    UI_API UIEnumNames GetEnumNames(EUIOrientation);
    UI_API UIEnumNames GetEnumNames(EUIScrollBarMode);

    /*
     * How a rectangle is painted: a colour, a texture, or a texture's nine
     * slices. Texture is a runtime-relative .nmtexture; "" is white.
     */
    struct UIBrush
    {
        std::string Texture;
        UIColor     Tint;
        EUIDrawAs   DrawAs = EUIDrawAs::Box;
        UIMargin    Margin;                       // NineSlice: the corners, in texture pixels
        UIMargin    UVRect{ 0.0f, 0.0f, 1.0f, 1.0f };   // X, Y, W, H of the texture used (a sprite sheet's cell)
        UIColor     OutlineColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        float       OutlineThickness = 0.0f;      // layout units, inside the rectangle
    };

    // === UIValue ==============================================================

    enum class EUIValueType : uint8_t { None, Number, Bool, String, Color, Vec2, Margin };

    /*
     * A property's value or a data store entry (UISystem::SetValue): one of
     * the types above. Numbers carry ints and enums too. Copies allocate only
     * for strings.
     */
    struct UI_API UIValue
    {
        EUIValueType Type = EUIValueType::None;
        float        F[4] = {};    // Number: F[0]; Color: RGBA; Vec2: X, Y; Margin: L, T, R, B
        bool         B    = false;
        std::string  S;

        static UIValue Number(double InValue);
        static UIValue Bool(bool InValue);
        static UIValue String(std::string InValue);
        static UIValue Color(const UIColor& InValue);
        static UIValue Vec2(const UIVec2& InValue);
        static UIValue Margin(const UIMargin& InValue);

        bool IsNone() const { return Type == EUIValueType::None; }

        /* Converting where it makes sense: a string parses, a bool is 0 or 1. */
        double      AsNumber() const;
        bool        AsBool()   const;
        UIColor     AsColor()  const;
        UIVec2      AsVec2()   const;
        UIMargin    AsMargin() const;
        /* As text: a whole number without a fraction, a colour as #RRGGBBAA. */
        std::string AsString() const;

        bool operator==(const UIValue& InOther) const;
        bool operator!=(const UIValue& InOther) const { return !(*this == InOther); }
    };

    // === Text forms ===========================================================
    // What XML writes. Each Parse is false, OutValue untouched, on bad text.

    UI_API std::string FormatFloat(float InValue);   // shortest form that reads back exactly
    UI_API bool        ParseFloat(const std::string& InText, float& OutValue);
    UI_API std::string FormatColor(const UIColor& InValue);   // #RRGGBBAA
    UI_API bool        ParseColor(const std::string& InText, UIColor& OutValue);   // #RGB, #RRGGBB, #RRGGBBAA
    UI_API std::string FormatVec2(const UIVec2& InValue);     // "x,y"
    UI_API bool        ParseVec2(const std::string& InText, UIVec2& OutValue);
    UI_API std::string FormatMargin(const UIMargin& InValue); // "l,t,r,b"; "a" or "h,v" when it can
    UI_API bool        ParseMargin(const std::string& InText, UIMargin& OutValue);   // "a", "h,v" or "l,t,r,b"

    /* FNV-1a, 32 bits: the hash names are stored and found by. */
    UI_API uint32_t HashName(const char* InName);
    inline uint32_t HashName(const std::string& InName) { return HashName(InName.c_str()); }
}

#endif //UI_UITYPES_H

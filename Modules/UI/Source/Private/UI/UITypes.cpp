#include "Public/UI/UITypes.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace UI
{
    namespace
    {
        const char* const VISIBILITY_NAMES[] = { "Visible", "Hidden", "Collapsed", "HitTestInvisible", "SelfHitTestInvisible" };
        const char* const HALIGN_NAMES[]     = { "Fill", "Left", "Center", "Right" };
        const char* const VALIGN_NAMES[]     = { "Fill", "Top", "Center", "Bottom" };
        const char* const SCALE_MODE_NAMES[] = { "FitHeight", "FitWidth", "Fit", "Fill", "None" };
        const char* const INPUT_MODE_NAMES[] = { "GameOnly", "GameAndUI", "UIOnly" };
        const char* const DRAW_AS_NAMES[]    = { "None", "Box", "Image", "NineSlice" };
        const char* const STRETCH_NAMES[]    = { "Fill", "None", "Uniform", "UniformToFill" };
        const char* const JUSTIFY_NAMES[]    = { "Left", "Center", "Right" };
        const char* const FILL_DIR_NAMES[]   = { "LeftToRight", "RightToLeft", "TopToBottom", "BottomToTop" };
        const char* const ORIENTATION_NAMES[] = { "Vertical", "Horizontal" };
        const char* const SCROLL_BAR_NAMES[]  = { "Auto", "Always", "Never" };

        template<size_t N>
        UIEnumNames Names(const char* const (&InNames)[N]) { return UIEnumNames{ InNames, uint32_t(N) }; }

        std::string Trim(const std::string& InText)
        {
            size_t Begin = 0, End = InText.size();
            while(Begin < End && std::isspace(static_cast<unsigned char>(InText[Begin]))) { ++Begin; }
            while(End > Begin && std::isspace(static_cast<unsigned char>(InText[End - 1]))) { --End; }
            return InText.substr(Begin, End - Begin);
        }

        /* Up to InMax comma-separated floats; their count, or -1 on bad text. */
        int ParseFloats(const std::string& InText, float* OutValues, const int InMax)
        {
            int    Count = 0;
            size_t Start = 0;
            while(Start <= InText.size())
            {
                const size_t Comma = InText.find(',', Start);
                const std::string Part = Trim(InText.substr(Start, Comma == std::string::npos ? std::string::npos : Comma - Start));
                if(Count >= InMax || !ParseFloat(Part, OutValues[Count])) { return -1; }
                ++Count;
                if(Comma == std::string::npos) { break; }
                Start = Comma + 1;
            }
            return Count;
        }

        int HexDigit(const char InChar)
        {
            if(InChar >= '0' && InChar <= '9') return InChar - '0';
            if(InChar >= 'a' && InChar <= 'f') return InChar - 'a' + 10;
            if(InChar >= 'A' && InChar <= 'F') return InChar - 'A' + 10;
            return -1;
        }
    }

    UIEnumNames GetEnumNames(EUIVisibility)    { return Names(VISIBILITY_NAMES); }
    UIEnumNames GetEnumNames(EUIHAlign)        { return Names(HALIGN_NAMES); }
    UIEnumNames GetEnumNames(EUIVAlign)        { return Names(VALIGN_NAMES); }
    UIEnumNames GetEnumNames(EUIScaleMode)     { return Names(SCALE_MODE_NAMES); }
    UIEnumNames GetEnumNames(EUIInputMode)     { return Names(INPUT_MODE_NAMES); }
    UIEnumNames GetEnumNames(EUIDrawAs)        { return Names(DRAW_AS_NAMES); }
    UIEnumNames GetEnumNames(EUIStretch)       { return Names(STRETCH_NAMES); }
    UIEnumNames GetEnumNames(EUIJustify)       { return Names(JUSTIFY_NAMES); }
    UIEnumNames GetEnumNames(EUIFillDirection) { return Names(FILL_DIR_NAMES); }
    UIEnumNames GetEnumNames(EUIOrientation)   { return Names(ORIENTATION_NAMES); }
    UIEnumNames GetEnumNames(EUIScrollBarMode) { return Names(SCROLL_BAR_NAMES); }

    uint32_t UIColor::Pack(const float InOpacity) const
    {
        return RenderCore::PackUIColor(R, G, B, A * InOpacity);
    }

    // === UIValue ==================================================================

    UIValue UIValue::Number(const double InValue)
    {
        UIValue V; V.Type = EUIValueType::Number; V.F[0] = float(InValue); return V;
    }
    UIValue UIValue::Bool(const bool InValue)
    {
        UIValue V; V.Type = EUIValueType::Bool; V.B = InValue; return V;
    }
    UIValue UIValue::String(std::string InValue)
    {
        UIValue V; V.Type = EUIValueType::String; V.S = std::move(InValue); return V;
    }
    UIValue UIValue::Color(const UIColor& InValue)
    {
        UIValue V; V.Type = EUIValueType::Color;
        V.F[0] = InValue.R; V.F[1] = InValue.G; V.F[2] = InValue.B; V.F[3] = InValue.A;
        return V;
    }
    UIValue UIValue::Vec2(const UIVec2& InValue)
    {
        UIValue V; V.Type = EUIValueType::Vec2; V.F[0] = InValue.X; V.F[1] = InValue.Y; return V;
    }
    UIValue UIValue::Margin(const UIMargin& InValue)
    {
        UIValue V; V.Type = EUIValueType::Margin;
        V.F[0] = InValue.Left; V.F[1] = InValue.Top; V.F[2] = InValue.Right; V.F[3] = InValue.Bottom;
        return V;
    }

    double UIValue::AsNumber() const
    {
        switch(Type)
        {
            case EUIValueType::Number: return F[0];
            case EUIValueType::Bool:   return B ? 1.0 : 0.0;
            case EUIValueType::String:
            {
                float Parsed = 0.0f;
                return ParseFloat(Trim(S), Parsed) ? Parsed : 0.0;
            }
            default: return F[0];
        }
    }

    bool UIValue::AsBool() const
    {
        switch(Type)
        {
            case EUIValueType::Bool:   return B;
            case EUIValueType::Number: return F[0] != 0.0f;
            case EUIValueType::String: return S == "true" || S == "1";
            default:                   return false;
        }
    }

    UIColor UIValue::AsColor() const
    {
        if(Type == EUIValueType::String)
        {
            UIColor Parsed;
            return ParseColor(Trim(S), Parsed) ? Parsed : UIColor();
        }
        if(Type == EUIValueType::Number)
        {
            return UIColor{ F[0], F[0], F[0], 1.0f };
        }
        return UIColor{ F[0], F[1], F[2], F[3] };
    }

    UIVec2 UIValue::AsVec2() const
    {
        if(Type == EUIValueType::String)
        {
            UIVec2 Parsed;
            return ParseVec2(S, Parsed) ? Parsed : UIVec2();
        }
        if(Type == EUIValueType::Number) { return UIVec2{ F[0], F[0] }; }
        return UIVec2{ F[0], F[1] };
    }

    UIMargin UIValue::AsMargin() const
    {
        if(Type == EUIValueType::String)
        {
            UIMargin Parsed;
            return ParseMargin(S, Parsed) ? Parsed : UIMargin();
        }
        if(Type == EUIValueType::Number) { return UIMargin{ F[0], F[0], F[0], F[0] }; }
        return UIMargin{ F[0], F[1], F[2], F[3] };
    }

    std::string UIValue::AsString() const
    {
        switch(Type)
        {
            case EUIValueType::None:   return std::string();
            case EUIValueType::Number: return FormatFloat(F[0]);
            case EUIValueType::Bool:   return B ? "true" : "false";
            case EUIValueType::String: return S;
            case EUIValueType::Color:  return FormatColor(AsColor());
            case EUIValueType::Vec2:   return FormatVec2(AsVec2());
            case EUIValueType::Margin: return FormatMargin(AsMargin());
        }
        return std::string();
    }

    bool UIValue::operator==(const UIValue& InOther) const
    {
        if(Type != InOther.Type) return false;
        switch(Type)
        {
            case EUIValueType::None:   return true;
            case EUIValueType::Number: return F[0] == InOther.F[0];
            case EUIValueType::Bool:   return B == InOther.B;
            case EUIValueType::String: return S == InOther.S;
            case EUIValueType::Vec2:   return F[0] == InOther.F[0] && F[1] == InOther.F[1];
            default:                   return F[0] == InOther.F[0] && F[1] == InOther.F[1] && F[2] == InOther.F[2] && F[3] == InOther.F[3];
        }
    }

    // === Text forms ===============================================================

    std::string FormatFloat(const float InValue)
    {
        /* Shortest round-trip, as XmlComponentArchive writes floats: deterministic, exact. */
        if(InValue == 0.0f) { return "0"; }
        char Buffer[32];
        const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), InValue);
        return std::string(Buffer, Result.ptr);
    }

    bool ParseFloat(const std::string& InText, float& OutValue)
    {
        if(InText.empty()) return false;
        /* strtof: from_chars for floats is missing from older standard libraries. */
        char* End = nullptr;
        const float Value = std::strtof(InText.c_str(), &End);
        if(End == InText.c_str() || *End != '\0' || !std::isfinite(Value)) return false;
        OutValue = Value;
        return true;
    }

    std::string FormatColor(const UIColor& InValue)
    {
        const auto Byte = [](const float InChannel)
        {
            const float Clamped = InChannel < 0.0f ? 0.0f : (InChannel > 1.0f ? 1.0f : InChannel);
            return unsigned(std::lround(Clamped * 255.0f));
        };
        char Buffer[16];
        std::snprintf(Buffer, sizeof(Buffer), "#%02X%02X%02X%02X", Byte(InValue.R), Byte(InValue.G), Byte(InValue.B), Byte(InValue.A));
        return Buffer;
    }

    bool ParseColor(const std::string& InText, UIColor& OutValue)
    {
        if(InText.size() < 2 || InText[0] != '#') return false;
        const std::string Hex = InText.substr(1);
        int Digits[8];
        for(size_t At = 0; At < Hex.size(); ++At)
        {
            if(At >= 8 || (Digits[At] = HexDigit(Hex[At])) < 0) return false;
        }
        float Channels[4] = { 0, 0, 0, 1 };
        if(Hex.size() == 3)
        {
            for(int At = 0; At < 3; ++At) { Channels[At] = float(Digits[At] * 17) / 255.0f; }
        }
        else if(Hex.size() == 6 || Hex.size() == 8)
        {
            for(size_t At = 0; At < Hex.size() / 2; ++At) { Channels[At] = float(Digits[At * 2] * 16 + Digits[At * 2 + 1]) / 255.0f; }
        }
        else
        {
            return false;
        }
        OutValue = UIColor{ Channels[0], Channels[1], Channels[2], Channels[3] };
        return true;
    }

    std::string FormatVec2(const UIVec2& InValue)
    {
        return FormatFloat(InValue.X) + "," + FormatFloat(InValue.Y);
    }

    bool ParseVec2(const std::string& InText, UIVec2& OutValue)
    {
        float Values[2];
        const int Count = ParseFloats(InText, Values, 2);
        if(Count == 1) { OutValue = UIVec2{ Values[0], Values[0] }; return true; }
        if(Count == 2) { OutValue = UIVec2{ Values[0], Values[1] }; return true; }
        return false;
    }

    std::string FormatMargin(const UIMargin& InValue)
    {
        if(InValue.Left == InValue.Right && InValue.Top == InValue.Bottom)
        {
            return InValue.Left == InValue.Top ? FormatFloat(InValue.Left)
                                               : FormatFloat(InValue.Left) + "," + FormatFloat(InValue.Top);
        }
        return FormatFloat(InValue.Left) + "," + FormatFloat(InValue.Top) + "," + FormatFloat(InValue.Right) + "," + FormatFloat(InValue.Bottom);
    }

    bool ParseMargin(const std::string& InText, UIMargin& OutValue)
    {
        float Values[4];
        switch(ParseFloats(InText, Values, 4))
        {
            case 1: OutValue = UIMargin{ Values[0], Values[0], Values[0], Values[0] }; return true;
            case 2: OutValue = UIMargin{ Values[0], Values[1], Values[0], Values[1] }; return true;
            case 4: OutValue = UIMargin{ Values[0], Values[1], Values[2], Values[3] }; return true;
            default: return false;
        }
    }

    uint32_t HashName(const char* InName)
    {
        uint32_t Hash = 2166136261u;
        for(const char* At = InName; At && *At; ++At)
        {
            Hash ^= uint8_t(*At);
            Hash *= 16777619u;
        }
        return Hash;
    }
}

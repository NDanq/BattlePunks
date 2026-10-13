#include "Public/AICore/Blackboard.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Blackboard: typed keys and their revisions. Blackboard.h has the rules. */
namespace AICore
{
    namespace
    {
        const char* const GTypeNames[] = { "Bool", "Int", "Float", "Vector", "Entity", "Name" };

        bool EqualsNoCase(const char* InA, const char* InB)
        {
            if(!InA || !InB) { return false; }
            while(*InA && *InB)
            {
                if(std::tolower(static_cast<unsigned char>(*InA)) != std::tolower(static_cast<unsigned char>(*InB))) { return false; }
                ++InA;
                ++InB;
            }
            return *InA == *InB;
        }

        std::string Trim(const std::string& InText)
        {
            size_t Start = 0, End = InText.size();
            while(Start < End && std::isspace(static_cast<unsigned char>(InText[Start]))) { ++Start; }
            while(End > Start && std::isspace(static_cast<unsigned char>(InText[End - 1]))) { --End; }
            return InText.substr(Start, End - Start);
        }

        bool ParseNumber(const std::string& InText, double& OutValue)
        {
            const std::string Text = Trim(InText);
            if(Text.empty()) { return false; }
            char* End = nullptr;
            OutValue = std::strtod(Text.c_str(), &End);
            return End && *End == '\0';
        }
    }

    const char* GetBlackboardTypeName(const EBlackboardType InType)
    {
        return InType < EBlackboardType::Count ? GTypeNames[size_t(InType)] : "Bool";
    }

    EBlackboardType FindBlackboardType(const char* InName)
    {
        for(size_t Index = 0; Index < size_t(EBlackboardType::Count); ++Index)
        {
            if(EqualsNoCase(InName, GTypeNames[Index])) { return EBlackboardType(Index); }
        }
        if(EqualsNoCase(InName, "Number")) { return EBlackboardType::Float; }
        if(EqualsNoCase(InName, "String")) { return EBlackboardType::Name; }
        return EBlackboardType::Count;
    }

    // === Values ===============================================================

    double BlackboardValue::AsNumber() const
    {
        switch(Type)
        {
            case EBlackboardType::Bool:   return bValue ? 1.0 : 0.0;
            case EBlackboardType::Int:    return double(IntValue);
            case EBlackboardType::Float:  return double(FloatValue);
            case EBlackboardType::Vector: return double(VectorValue.GetLength());
            case EBlackboardType::Entity: return EntityValue.IsSet() ? 1.0 : 0.0;
            case EBlackboardType::Name:   { double Value = 0.0; return ParseNumber(NameValue, Value) ? Value : (NameValue.empty() ? 0.0 : 1.0); }
            default:                      return 0.0;
        }
    }

    std::string BlackboardValue::ToString() const
    {
        char Buffer[96];
        switch(Type)
        {
            case EBlackboardType::Bool:   return bValue ? "true" : "false";
            case EBlackboardType::Int:    return std::to_string(IntValue);
            case EBlackboardType::Float:  std::snprintf(Buffer, sizeof(Buffer), "%g", double(FloatValue)); return Buffer;
            case EBlackboardType::Vector:
                std::snprintf(Buffer, sizeof(Buffer), "%g,%g,%g", double(VectorValue.GetX()), double(VectorValue.GetY()), double(VectorValue.GetZ()));
                return Buffer;
            case EBlackboardType::Entity:
                if(!EntityValue.IsSet()) { return "none"; }
                std::snprintf(Buffer, sizeof(Buffer), "#%u", EntityValue.Index);
                return Buffer;
            case EBlackboardType::Name:   return NameValue;
            default:                      return "";
        }
    }

    bool BlackboardValue::Parse(const std::string& InText)
    {
        const std::string Text = Trim(InText);
        double Number = 0.0;
        switch(Type)
        {
            case EBlackboardType::Bool:
                if(EqualsNoCase(Text.c_str(), "true") || Text == "1")  { bValue = true;  bSet = true; return true; }
                if(EqualsNoCase(Text.c_str(), "false") || Text == "0") { bValue = false; bSet = true; return true; }
                return false;
            case EBlackboardType::Int:
                if(!ParseNumber(Text, Number)) { return false; }
                IntValue = int64_t(std::llround(Number));
                bSet = true;
                return true;
            case EBlackboardType::Float:
                if(!ParseNumber(Text, Number)) { return false; }
                FloatValue = float(Number);
                bSet = true;
                return true;
            case EBlackboardType::Vector:
            {
                double X = 0.0, Y = 0.0, Z = 0.0;
                if(std::sscanf(Text.c_str(), "%lf , %lf , %lf", &X, &Y, &Z) != 3) { return false; }
                VectorValue = EngineCore::Vector3(X, Y, Z);
                bSet = true;
                return true;
            }
            case EBlackboardType::Name:
                NameValue = Text;
                bSet = true;
                return true;
            default:
                return false;
        }
    }

    bool BlackboardValue::Equals(const BlackboardValue& InOther) const
    {
        if(bSet != InOther.bSet) { return false; }
        switch(Type)
        {
            case EBlackboardType::Bool:   return bValue == InOther.bValue;
            case EBlackboardType::Int:    return IntValue == InOther.IntValue;
            case EBlackboardType::Float:  return FloatValue == InOther.FloatValue;
            case EBlackboardType::Vector:
                return VectorValue.GetX() == InOther.VectorValue.GetX() && VectorValue.GetY() == InOther.VectorValue.GetY()
                    && VectorValue.GetZ() == InOther.VectorValue.GetZ();
            case EBlackboardType::Entity: return EntityValue == InOther.EntityValue;
            case EBlackboardType::Name:   return NameValue == InOther.NameValue;
            default:                      return true;
        }
    }

    // === The board ============================================================

    void Blackboard::Declare(const std::vector<BlackboardKey>& InKeys)
    {
        Keys = InKeys;
        Values.assign(Keys.size(), BlackboardValue());
        Revisions.assign(Keys.size(), 1);
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            Values[Index].Type = Keys[Index].Type < EBlackboardType::Count ? Keys[Index].Type : EBlackboardType::Bool;
            if(!Keys[Index].Default.empty()) { Values[Index].Parse(Keys[Index].Default); }
        }
        ++Revision;
    }

    int32_t Blackboard::AddKey(const char* InName, const EBlackboardType InType)
    {
        if(const int32_t Existing = FindKey(InName); Existing >= 0) { return Existing; }
        BlackboardKey Key;
        Key.Name = InName ? InName : "";
        Key.Type = InType;
        Keys.push_back(Key);
        BlackboardValue Value;
        Value.Type = InType;
        Values.push_back(Value);
        Revisions.push_back(1);
        ++Revision;
        return int32_t(Keys.size() - 1);
    }

    int32_t Blackboard::FindKey(const char* InName) const
    {
        if(!InName) { return INVALID_KEY; }
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if(Keys[Index].Name == InName) { return int32_t(Index); }
        }
        return INVALID_KEY;
    }

    void Blackboard::Touch(const int32_t InKey)
    {
        ++Revision;
        Revisions[size_t(InKey)] = Revision;
    }

    void Blackboard::SetBool(const int32_t InKey, const bool bInValue)
    {
        if(!Valid(InKey)) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        BlackboardValue Next = V;
        Next.bSet = true;
        switch(V.Type)
        {
            case EBlackboardType::Bool:  Next.bValue = bInValue; break;
            case EBlackboardType::Int:   Next.IntValue = bInValue ? 1 : 0; break;
            case EBlackboardType::Float: Next.FloatValue = bInValue ? 1.0f : 0.0f; break;
            default: return;
        }
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::SetInt(const int32_t InKey, const int64_t InValue)
    {
        if(!Valid(InKey)) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        BlackboardValue Next = V;
        Next.bSet = true;
        switch(V.Type)
        {
            case EBlackboardType::Bool:  Next.bValue = InValue != 0; break;
            case EBlackboardType::Int:   Next.IntValue = InValue; break;
            case EBlackboardType::Float: Next.FloatValue = float(InValue); break;
            default: return;
        }
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::SetFloat(const int32_t InKey, const float InValue)
    {
        if(!Valid(InKey)) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        BlackboardValue Next = V;
        Next.bSet = true;
        switch(V.Type)
        {
            case EBlackboardType::Bool:  Next.bValue = InValue != 0.0f; break;
            case EBlackboardType::Int:   Next.IntValue = int64_t(std::llround(double(InValue))); break;
            case EBlackboardType::Float: Next.FloatValue = InValue; break;
            default: return;
        }
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::SetVector(const int32_t InKey, const EngineCore::Vector3& InValue)
    {
        if(!Valid(InKey) || Values[size_t(InKey)].Type != EBlackboardType::Vector) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        BlackboardValue Next = V;
        Next.bSet = true;
        Next.VectorValue = InValue;
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::SetEntity(const int32_t InKey, const GameCore::EntityID InValue)
    {
        if(!Valid(InKey) || Values[size_t(InKey)].Type != EBlackboardType::Entity) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        BlackboardValue Next = V;
        Next.bSet = InValue.IsSet();
        Next.EntityValue = InValue;
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::SetName(const int32_t InKey, const std::string& InValue)
    {
        if(!Valid(InKey)) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        if(V.Type != EBlackboardType::Name)
        {
            BlackboardValue Parsed = V;
            if(Parsed.Parse(InValue) && !Parsed.Equals(V)) { V = Parsed; Touch(InKey); }
            return;
        }
        BlackboardValue Next = V;
        Next.bSet = true;
        Next.NameValue = InValue;
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::SetValue(const int32_t InKey, const BlackboardValue& InValue)
    {
        if(!Valid(InKey)) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        BlackboardValue Next = InValue;
        Next.Type = V.Type;
        if(!Next.Equals(V)) { V = Next; Touch(InKey); }
    }

    void Blackboard::Clear(const int32_t InKey)
    {
        if(!Valid(InKey)) { return; }
        BlackboardValue& V = Values[size_t(InKey)];
        if(!V.bSet) { return; }
        const EBlackboardType Type = V.Type;
        V = BlackboardValue();
        V.Type = Type;
        Touch(InKey);
    }

    bool Blackboard::IsSet(const int32_t InKey) const
    {
        if(!Valid(InKey)) { return false; }
        const BlackboardValue& V = Values[size_t(InKey)];
        if(V.Type == EBlackboardType::Entity) { return V.bSet && V.EntityValue.IsSet(); }
        return V.bSet;
    }

    bool Blackboard::GetBool(const char* InKey, const bool bInDefault) const
    {
        const int32_t K = FindKey(InKey);
        return K >= 0 ? Values[size_t(K)].AsNumber() != 0.0 : bInDefault;
    }

    double Blackboard::GetNumber(const char* InKey, const double InDefault) const
    {
        const int32_t K = FindKey(InKey);
        return K >= 0 ? Values[size_t(K)].AsNumber() : InDefault;
    }

    EngineCore::Vector3 Blackboard::GetVector(const char* InKey) const
    {
        const int32_t K = FindKey(InKey);
        return K >= 0 ? Values[size_t(K)].VectorValue : EngineCore::Vector3();
    }

    GameCore::EntityID Blackboard::GetEntity(const char* InKey) const
    {
        const int32_t K = FindKey(InKey);
        return K >= 0 ? Values[size_t(K)].EntityValue : GameCore::EntityID();
    }

    std::string Blackboard::GetName(const char* InKey) const
    {
        const int32_t K = FindKey(InKey);
        return K >= 0 ? Values[size_t(K)].ToString() : std::string();
    }

    size_t Blackboard::ApplyAssignments(const char* InText)
    {
        if(!InText) { return 0; }
        size_t Applied = 0;
        const std::string Text = InText;
        size_t Start = 0;
        while(Start < Text.size())
        {
            size_t End = Text.find(';', Start);
            if(End == std::string::npos) { End = Text.size(); }
            const std::string Pair = Text.substr(Start, End - Start);
            Start = End + 1;
            const size_t Equals = Pair.find('=');
            if(Equals == std::string::npos) { continue; }
            const std::string Name = Trim(Pair.substr(0, Equals));
            const int32_t Key = FindKey(Name.c_str());
            if(Key < 0) { continue; }
            BlackboardValue Value = Values[size_t(Key)];
            if(Value.Parse(Pair.substr(Equals + 1)))
            {
                SetValue(Key, Value);
                ++Applied;
            }
        }
        return Applied;
    }
}

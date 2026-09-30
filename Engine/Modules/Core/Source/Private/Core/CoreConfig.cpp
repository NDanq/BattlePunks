#include "Public/Core/CoreConfig.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreString.h"

namespace EngineCore
{
namespace
{
    // The registry head -- see WHY AN INTRUSIVE LIST AND NOT std::vector in
    // CoreConfig.h. Constant-initialised into .bss; no constructor, no
    // ordering question.
    ConfigVar* GConfigHead = nullptr;

    /*
     * One (Section, Key) -> text-value record per key ever seen by
     * LoadConfigFile/LoadConfigText, kept so a config var declared AFTER a
     * file was loaded -- the normal case for a hot module -- still comes up
     * holding the file's value instead of silently sitting at its compiled
     * default. See WHY PARSED ENTRIES ARE RETAINED below.
     */
    struct ConfigEntry
    {
        std::string Section;
        std::string Key;
        std::string Value;
        bool        bClaimed = false;
    };

    /*
     * Leaked ON PURPOSE. A module DLL's ConfigVar destructors run at unload,
     * which can be during static destruction; a function-local static vector
     * could already be dead by then and the unlink path would touch it. Never
     * destroying it removes the ordering question entirely, and the leak is
     * one vector for the process lifetime.
     */
    std::vector<ConfigEntry>* GConfigEntries = nullptr;

    // --- string helpers, kept local to this file ---------------------------
    //
    // CoreString.h says outright that it is not the beginning of a string
    // class and that anything needing more probably wants a real parser --
    // this is that parser, so it carries its own case-fold and value parsing
    // rather than growing the shared header for one caller.

    bool EqualsIgnoreCase(const std::string& InA, const std::string& InB)
    {
        if(InA.size() != InB.size()) return false;
        for(size_t Idx = 0; Idx < InA.size(); ++Idx)
        {
            if(std::tolower(static_cast<unsigned char>(InA[Idx])) !=
               std::tolower(static_cast<unsigned char>(InB[Idx])))
            {
                return false;
            }
        }
        return true;
    }

    std::string StripQuotes(const std::string& InText)
    {
        if(InText.size() >= 2 && InText.front() == '"' && InText.back() == '"')
        {
            return InText.substr(1, InText.size() - 2);
        }
        return InText;
    }

    bool ParseBool(const std::string& InText, bool& OutValue)
    {
        const std::string T = Trim(InText);
        if(EqualsIgnoreCase(T, "true") || T == "1" || EqualsIgnoreCase(T, "yes") || EqualsIgnoreCase(T, "on"))
        {
            OutValue = true;
            return true;
        }
        if(EqualsIgnoreCase(T, "false") || T == "0" || EqualsIgnoreCase(T, "no") || EqualsIgnoreCase(T, "off"))
        {
            OutValue = false;
            return true;
        }
        return false;
    }

    /*
     * Decimal or 0x/0X hex, full-token only. Bare leading-zero octal is
     * deliberately NOT enabled: base-0 strtol would treat "0640" as octal 416,
     * and that silent reinterpretation is a worse trap than not supporting
     * octal at all.
     */
    bool ParseInt(const std::string& InText, int32_t& OutValue)
    {
        const std::string T = Trim(InText);
        if(T.empty()) return false;

        const char* Start  = T.c_str();
        const char* Digits = Start;
        if(*Digits == '+' || *Digits == '-') ++Digits;

        const int Base = (Digits[0] == '0' && (Digits[1] == 'x' || Digits[1] == 'X')) ? 16 : 10;

        errno = 0;
        char* EndPtr = nullptr;
        const long Parsed = std::strtol(Start, &EndPtr, Base);
        if(EndPtr == Start || *EndPtr != '\0') return false;   // did not consume the whole token
        if(errno == ERANGE) return false;
        if(Parsed < INT32_MIN || Parsed > INT32_MAX) return false;

        OutValue = static_cast<int32_t>(Parsed);
        return true;
    }

    bool ParseFloat(const std::string& InText, float& OutValue)
    {
        const std::string T = Trim(InText);
        if(T.empty()) return false;

        errno = 0;
        char* EndPtr = nullptr;
        const float Parsed = std::strtof(T.c_str(), &EndPtr);
        if(EndPtr == T.c_str() || *EndPtr != '\0') return false;
        if(errno == ERANGE) return false;

        OutValue = Parsed;
        return true;
    }

    // --- retained entry store -----------------------------------------------

    void RecordConfigEntry(const std::string& InSection, const std::string& InKey, const std::string& InValue)
    {
        if(!GConfigEntries)
        {
            GConfigEntries = new std::vector<ConfigEntry>();
        }

        for(ConfigEntry& Entry : *GConfigEntries)
        {
            if(EqualsIgnoreCase(Entry.Section, InSection) && EqualsIgnoreCase(Entry.Key, InKey))
            {
                Entry.Value    = InValue;
                Entry.bClaimed = false;   // a fresh value re-opens the claim question
                return;
            }
        }

        GConfigEntries->push_back(ConfigEntry{ InSection, InKey, InValue, false });
    }

    void ClaimConfigEntry(const std::string& InSection, const std::string& InKey)
    {
        if(!GConfigEntries) return;

        for(ConfigEntry& Entry : *GConfigEntries)
        {
            if(EqualsIgnoreCase(Entry.Section, InSection) && EqualsIgnoreCase(Entry.Key, InKey))
            {
                Entry.bClaimed = true;
                return;
            }
        }
    }

    // Whitespace-run tokeniser for the console line, with a double-quoted
    // final argument kept as one token. NOT EngineCore::Split(Text, ' ') --
    // Split keeps empty parts by contract (the GLSL preprocessor depends on
    // that), so two spaces between tokens would yield an empty middle token.
    std::vector<std::string> SplitCommandTokens(const std::string& InText)
    {
        std::vector<std::string> Tokens;
        size_t Idx = 0;
        while(Idx < InText.size())
        {
            while(Idx < InText.size() && std::isspace(static_cast<unsigned char>(InText[Idx]))) ++Idx;
            if(Idx >= InText.size()) break;

            if(InText[Idx] == '"')
            {
                const size_t Close = InText.find('"', Idx + 1);
                if(Close != std::string::npos)
                {
                    Tokens.push_back(InText.substr(Idx + 1, Close - Idx - 1));
                    Idx = Close + 1;
                    continue;
                }
                // Unterminated quote -- fall through and take the rest verbatim.
                Tokens.push_back(InText.substr(Idx + 1));
                break;
            }

            size_t End = Idx;
            while(End < InText.size() && !std::isspace(static_cast<unsigned char>(InText[End]))) ++End;
            Tokens.push_back(InText.substr(Idx, End - Idx));
            Idx = End;
        }
        return Tokens;
    }
}   // anonymous namespace

// ============================================================================
// ConfigVar
// ============================================================================

ConfigVar::ConfigVar(const EConfigType InType, const char* InSection, const char* InName,
                      const ConfigValue InDefault, const ConfigChangedFn InOnChanged)
    : Section(InSection ? InSection : "")
    , Name(InName ? InName : "")
    , Value(InDefault)
    , Default(InDefault)
    , OnChanged(InOnChanged)
    , Next(nullptr)
    , Type(InType)
    , bOverridden(false)
{
    ConfigRegisterVar(this);

    /*
     * WHY PARSED ENTRIES ARE RETAINED, AND CONSULTED HERE
     *
     * A hot module loaded at minute five declares its config vars long after
     * the .ini was read in main(). Without this, those vars would silently
     * sit at their compiled defaults forever -- the file would appear to be
     * ignored, which defeats the whole point of allowing config vars inside a
     * module. So the FIRST thing a newly-linked var does is ask whether a
     * previously-parsed file already has a value for it.
     */
    if(GConfigEntries)
    {
        for(ConfigEntry& Entry : *GConfigEntries)
        {
            if(EqualsIgnoreCase(Entry.Section, Section) && EqualsIgnoreCase(Entry.Key, Name))
            {
                Entry.bClaimed = true;
                SetFromString(Entry.Value);
                break;
            }
        }
    }
}

ConfigVar::~ConfigVar()
{
    ConfigUnregisterVar(this);
}

bool ConfigVar::GetBool() const
{
    CORE_CHECK(Type == EConfigType::CT_Bool, "CoreConfig: GetBool called on a non-bool config var")
    return Value.AsBool;
}

int32_t ConfigVar::GetInt() const
{
    CORE_CHECK(Type == EConfigType::CT_Int, "CoreConfig: GetInt called on a non-int config var")
    return Value.AsInt;
}

float ConfigVar::GetFloat() const
{
    CORE_CHECK(Type == EConfigType::CT_Float, "CoreConfig: GetFloat called on a non-float config var")
    return Value.AsFloat;
}

const char* ConfigVar::GetString() const
{
    CORE_CHECK(Type == EConfigType::CT_String, "CoreConfig: GetString called on a non-string config var")
    return Value.AsString;
}

bool ConfigVar::SetBool(const bool bInValue)
{
    CORE_CHECK(Type == EConfigType::CT_Bool, "CoreConfig: SetBool called on a non-bool config var")
    if(Value.AsBool == bInValue) return false;

    Value.AsBool = bInValue;
    bOverridden  = true;
    FireCallback();
    return true;
}

bool ConfigVar::SetInt(const int32_t InValue)
{
    CORE_CHECK(Type == EConfigType::CT_Int, "CoreConfig: SetInt called on a non-int config var")
    if(Value.AsInt == InValue) return false;

    Value.AsInt = InValue;
    bOverridden = true;
    FireCallback();
    return true;
}

bool ConfigVar::SetFloat(const float InValue)
{
    CORE_CHECK(Type == EConfigType::CT_Float, "CoreConfig: SetFloat called on a non-float config var")
    if(Value.AsFloat == InValue) return false;

    Value.AsFloat = InValue;
    bOverridden   = true;
    FireCallback();
    return true;
}

bool ConfigVar::SetString(const char* InValue)
{
    CORE_CHECK(Type == EConfigType::CT_String, "CoreConfig: SetString called on a non-string config var")
    if(!InValue) InValue = "";

    if(std::strcmp(Value.AsString, InValue) == 0) return false;

    const size_t Len = std::strlen(InValue);
    if(Len >= CONFIG_STRING_MAX)
    {
        CORE_LOG_WARN("CoreConfig: '" + GetQualifiedName() + "' value truncated to " +
                      std::to_string(CONFIG_STRING_MAX - 1) + " bytes")
    }

    std::strncpy(Value.AsString, InValue, CONFIG_STRING_MAX - 1);
    Value.AsString[CONFIG_STRING_MAX - 1] = '\0';
    bOverridden = true;
    FireCallback();
    return true;
}

bool ConfigVar::SetFromString(const std::string& InText, bool* OutbChanged)
{
    bool bParsed  = false;
    bool bChanged = false;

    switch(Type)
    {
        case EConfigType::CT_Bool:
        {
            bool Parsed = false;
            bParsed = ParseBool(InText, Parsed);
            if(bParsed) bChanged = SetBool(Parsed);
            break;
        }
        case EConfigType::CT_Int:
        {
            int32_t Parsed = 0;
            bParsed = ParseInt(InText, Parsed);
            if(bParsed) bChanged = SetInt(Parsed);
            break;
        }
        case EConfigType::CT_Float:
        {
            float Parsed = 0.0f;
            bParsed = ParseFloat(InText, Parsed);
            if(bParsed) bChanged = SetFloat(Parsed);
            break;
        }
        case EConfigType::CT_String:
        {
            bParsed  = true;
            bChanged = SetString(InText.c_str());
            break;
        }
    }

    if(OutbChanged) *OutbChanged = bChanged;
    return bParsed;
}

namespace
{
    std::string ConfigValueToString(const EConfigType InType, const ConfigValue& InValue)
    {
        switch(InType)
        {
            case EConfigType::CT_Bool:   return InValue.AsBool ? "true" : "false";
            case EConfigType::CT_Int:    return std::to_string(InValue.AsInt);
            case EConfigType::CT_Float:
            {
                char Buffer[64];
                std::snprintf(Buffer, sizeof(Buffer), "%g", InValue.AsFloat);
                return Buffer;
            }
            case EConfigType::CT_String: return InValue.AsString;
        }
        return "";
    }
}

std::string ConfigVar::ToString() const
{
    return ConfigValueToString(Type, Value);
}

std::string ConfigVar::DefaultToString() const
{
    return ConfigValueToString(Type, Default);
}

std::string ConfigVar::GetQualifiedName() const
{
    if(!Section || Section[0] == '\0') return Name;
    return std::string(Section) + "." + Name;
}

bool ConfigVar::ResetToDefault()
{
    switch(Type)
    {
        case EConfigType::CT_Bool:   return SetBool  (Default.AsBool);
        case EConfigType::CT_Int:    return SetInt   (Default.AsInt);
        case EConfigType::CT_Float:  return SetFloat (Default.AsFloat);
        case EConfigType::CT_String: return SetString(Default.AsString);
    }
    return false;
}

void ConfigVar::FireCallback() const
{
    if(OnChanged) OnChanged(*this);
}

// ============================================================================
// Registry
// ============================================================================

void ConfigRegisterVar(ConfigVar* InVar)
{
    if(!InVar) return;

    for(ConfigVar* V = GConfigHead; V; V = V->Next)
    {
        if(EqualsIgnoreCase(V->Section, InVar->Section) && EqualsIgnoreCase(V->Name, InVar->Name))
        {
            CORE_LOG_WARN("CoreConfig: duplicate config var '" + InVar->GetQualifiedName() + "'" +
                          " -- both instances are kept in sync by LoadConfigFile and the console, "
                          "but a direct Set call on one will not update the other")
            break;
        }
    }

    InVar->Next = GConfigHead;
    GConfigHead = InVar;
}

void ConfigUnregisterVar(ConfigVar* InVar)
{
    if(!InVar) return;

    if(GConfigHead == InVar)
    {
        GConfigHead = InVar->Next;
        return;
    }

    for(ConfigVar* V = GConfigHead; V; V = V->Next)
    {
        if(V->Next == InVar)
        {
            V->Next = InVar->Next;
            return;
        }
    }
}

ConfigVar* ConfigGetFirst()
{
    return GConfigHead;
}

int ConfigGetVarCount()
{
    int Count = 0;
    for(ConfigVar* V = GConfigHead; V; V = V->GetNext()) ++Count;
    return Count;
}

ConfigVar* ConfigFindVar(const char* InSection, const char* InName)
{
    const std::string SectionStr = InSection ? InSection : "";
    const std::string NameStr    = InName    ? InName    : "";

    for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
    {
        if(EqualsIgnoreCase(V->GetSection(), SectionStr) && EqualsIgnoreCase(V->GetName(), NameStr))
        {
            return V;
        }
    }
    return nullptr;
}

ConfigVar* ConfigFindVarByPath(const std::string& InQualifiedName, bool* OutbAmbiguous)
{
    if(OutbAmbiguous) *OutbAmbiguous = false;

    const size_t Dot = InQualifiedName.find('.');
    if(Dot != std::string::npos)
    {
        return ConfigFindVar(InQualifiedName.substr(0, Dot).c_str(), InQualifiedName.substr(Dot + 1).c_str());
    }

    ConfigVar* Found      = nullptr;
    int        MatchCount = 0;
    for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
    {
        if(EqualsIgnoreCase(V->GetName(), InQualifiedName))
        {
            if(!Found) Found = V;
            ++MatchCount;
        }
    }
    if(MatchCount > 1 && OutbAmbiguous) *OutbAmbiguous = true;
    return Found;
}

// ============================================================================
// Load
// ============================================================================

bool LoadConfigFile(const std::string& InPath, ConfigLoadStats* OutStats)
{
    std::string Text;

    IFileProvider* Provider = GetFileProvider();
    if(Provider)
    {
        if(!Provider->ReadAllText(InPath, Text))
        {
            CORE_LOG_WARN("CoreConfig: could not read '" + InPath + "' through the installed file provider")
            return false;
        }
    }
    else
    {
        std::vector<uint8_t> Bytes;
        if(!appFileReadAll(InPath, Bytes))
        {
            CORE_LOG_WARN("CoreConfig: could not read '" + InPath + "'")
            return false;
        }
        Text.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
    }

    return LoadConfigText(Text, OutStats);
}

bool LoadConfigText(const std::string& InText, ConfigLoadStats* OutStats)
{
    ConfigLoadStats Stats;
    std::string     CurrentSection;

    const std::vector<std::string> Lines = Split(InText, '\n');
    int LineNumber = 0;

    for(const std::string& RawLine : Lines)
    {
        ++LineNumber;
        ++Stats.LinesRead;

        // Trim eats a trailing carriage return too (std::isspace), so CRLF and
        // LF parse identically with no special-casing here.
        const std::string Line = Trim(RawLine);
        if(Line.empty()) continue;
        if(Line[0] == ';' || Line[0] == '#') continue;

        if(Line[0] == '[')
        {
            const size_t Close = Line.find(']');
            if(Close == std::string::npos)
            {
                CORE_LOG_WARN("CoreConfig: line " + std::to_string(LineNumber) + " is missing a closing bracket -- line skipped")
                ++Stats.MalformedLines;
                continue;
            }
            CurrentSection = Trim(Line.substr(1, Close - 1));
            if(Contains(CurrentSection, "."))
            {
                CORE_LOG_WARN("CoreConfig: line " + std::to_string(LineNumber) + ": section '" +
                              CurrentSection + "' contains a period, which the console cannot address")
            }
            continue;
        }

        const size_t Eq = Line.find('=');
        if(Eq == std::string::npos)
        {
            CORE_LOG_WARN("CoreConfig: line " + std::to_string(LineNumber) + " does not look like Key = Value -- line skipped")
            ++Stats.MalformedLines;
            continue;
        }

        const std::string Key = Trim(Line.substr(0, Eq));
        if(Key.empty())
        {
            CORE_LOG_WARN("CoreConfig: line " + std::to_string(LineNumber) + " has an empty key -- line skipped")
            ++Stats.MalformedLines;
            continue;
        }

        const std::string Value = StripQuotes(Trim(Line.substr(Eq + 1)));

        // Recorded BEFORE the registry walk, so a var that does not exist yet
        // (or ever) still leaves a trail for ConfigVar's constructor and for
        // ConfigWarnUnclaimedEntries to find.
        RecordConfigEntry(CurrentSection, Key, Value);

        bool bAnyMatch = false;
        for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
        {
            if(!EqualsIgnoreCase(V->GetSection(), CurrentSection) || !EqualsIgnoreCase(V->GetName(), Key))
            {
                continue;
            }
            bAnyMatch = true;

            bool bChanged = false;
            if(V->SetFromString(Value, &bChanged))
            {
                ++Stats.KeysApplied;
                if(bChanged) ++Stats.ValuesChanged;
            }
            else
            {
                CORE_LOG_ERROR("CoreConfig: line " + std::to_string(LineNumber) + ": '" + Value +
                               "' is not a valid value for '" + V->GetQualifiedName() + "'")
                ++Stats.BadValues;
            }
        }

        if(bAnyMatch)
        {
            ClaimConfigEntry(CurrentSection, Key);
        }
        else
        {
            // Log verbosity, not Warning -- a module that has not loaded yet
            // will legitimately claim this later. ConfigWarnUnclaimedEntries
            // is the deferred check that catches an actual typo.
            const std::string Qualified = CurrentSection.empty() ? Key : (CurrentSection + "." + Key);
            CORE_LOG("CoreConfig: line " + std::to_string(LineNumber) + ": '" + Qualified +
                    "' does not match a declared config var (yet)")
            ++Stats.UnknownKeys;
        }
    }

    if(OutStats) *OutStats = Stats;
    return Stats.MalformedLines == 0 && Stats.BadValues == 0;
}

void ConfigApplyAll()
{
    for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
    {
        V->FireCallback();
    }
}

void ConfigResetAllToDefaults()
{
    for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
    {
        V->ResetToDefault();
    }
}

void ConfigWarnUnclaimedEntries()
{
    if(!GConfigEntries) return;

    for(const ConfigEntry& Entry : *GConfigEntries)
    {
        if(Entry.bClaimed) continue;

        const std::string Qualified = Entry.Section.empty() ? Entry.Key : (Entry.Section + "." + Entry.Key);
        CORE_LOG_WARN("CoreConfig: '" + Qualified + "' was set in a config file but never matched a declared config var")
    }
}

// ============================================================================
// Console
// ============================================================================

bool ExecConfigCommand(const std::string& InCommand, std::string& OutResponse)
{
    OutResponse.clear();

    const std::vector<std::string> Tokens = SplitCommandTokens(InCommand);
    if(Tokens.empty())
    {
        return false;
    }

    const std::string Path         = Tokens[0];
    size_t            NextTokenIdx = 1;

    // "Section.Name = Value" -- a lone '=' token is sugar, drop it.
    if(NextTokenIdx < Tokens.size() && Tokens[NextTokenIdx] == "=")
    {
        ++NextTokenIdx;
    }

    bool       bAmbiguous = false;
    ConfigVar* Var        = ConfigFindVarByPath(Path, &bAmbiguous);

    if(bAmbiguous)
    {
        std::string Names;
        for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
        {
            if(EqualsIgnoreCase(V->GetName(), Path))
            {
                if(!Names.empty()) Names += ", ";
                Names += V->GetQualifiedName();
            }
        }
        OutResponse = "'" + Path + "' is ambiguous: " + Names;
        return false;
    }

    if(!Var)
    {
        OutResponse = "Unknown config variable '" + Path + "'";
        return false;
    }

    // --- get -----------------------------------------------------------
    if(NextTokenIdx >= Tokens.size())
    {
        OutResponse = Var->GetQualifiedName() + " = " + Var->ToString();

        // Report every other var sharing this key -- see A DUPLICATE KEY IS A
        // WARNING in CoreConfig.h.
        int DuplicateCount = 0;
        for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
        {
            if(V != Var && EqualsIgnoreCase(V->GetSection(), Var->GetSection()) && EqualsIgnoreCase(V->GetName(), Var->GetName()))
            {
                ++DuplicateCount;
            }
        }
        if(DuplicateCount > 0)
        {
            OutResponse += "  (" + std::to_string(DuplicateCount) + " duplicate" + (DuplicateCount > 1 ? "s" : "") + " registered)";
        }
        return true;
    }

    // --- set -----------------------------------------------------------
    if(Tokens.size() > NextTokenIdx + 1)
    {
        OutResponse = "Too many arguments; quote the value if it contains spaces";
        return false;
    }

    const std::string& ValueText = Tokens[NextTokenIdx];
    const std::string  PrevText  = Var->ToString();

    int  SetCount     = 0;
    int  ChangedCount = 0;
    bool bAnyParsed   = false;

    for(ConfigVar* V = GConfigHead; V; V = V->GetNext())
    {
        if(!EqualsIgnoreCase(V->GetSection(), Var->GetSection()) || !EqualsIgnoreCase(V->GetName(), Var->GetName()))
        {
            continue;
        }

        bool bChanged = false;
        if(V->SetFromString(ValueText, &bChanged))
        {
            bAnyParsed = true;
            ++SetCount;
            if(bChanged) ++ChangedCount;
        }
    }

    if(!bAnyParsed)
    {
        static const char* const TypeNames[] = { "bool", "int", "float", "string" };
        OutResponse = Var->GetQualifiedName() + " expects a " + TypeNames[static_cast<int>(Var->GetType())] +
                      ", got '" + ValueText + "'";
        return false;
    }

    CORE_LOG("CoreConfig: console set " + Var->GetQualifiedName() + " = " + Var->ToString())

    OutResponse = Var->GetQualifiedName() + " = " + Var->ToString();
    if(ChangedCount == 0)
    {
        OutResponse += "  (unchanged)";
    }
    else
    {
        OutResponse += "  (was " + PrevText + ")";
    }
    if(SetCount > 1)
    {
        OutResponse += "  [" + std::to_string(SetCount) + " vars]";
    }
    return true;
}
}

#ifndef NIMBLEENGINE_LEVELTRAVEL_H
#define NIMBLEENGINE_LEVELTRAVEL_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

/*
 * LEVEL TRAVEL: what a level change carries into the next level, besides the
 * level itself (Engine/Docs/Guides/GameModeGuide.md, "Changing level"):
 *
 *   Entry    the Player Start to come in at: one whose Entry is this, or
 *            whose entity is named this (PlayerStart.h, FindEntryPoint).
 *            "" the usual start.
 *   Values   a few key/value strings -- the travel state ("Alarm=1") the new
 *            level's mode and scripts read once it has loaded
 *            (GameSession::GetTravelValue, Game.GetTravelValue in Lua).
 *
 * Previous is set by the session: the name of the level travelled from ("" for
 * the first, or a level loaded directly).
 *
 * Header-only and plain: copy it, keep it, build it anywhere.
 *
 *     NimbleEngine::LevelTravel Travel;
 *     Travel.Entry = "FreightLift";
 *     Travel.Set("Alarm", "1");
 *     Session->OpenLevel("Lab", Travel);
 */
namespace NimbleEngine
{
    struct LevelTravel
    {
        std::string Entry;
        std::vector<std::pair<std::string, std::string>> Values;
        std::string Previous;

        /* InKey to InValue, replacing any it had. */
        void Set(const std::string& InKey, const std::string& InValue)
        {
            for(std::pair<std::string, std::string>& Each : Values)
            {
                if(Each.first == InKey) { Each.second = InValue; return; }
            }
            Values.emplace_back(InKey, InValue);
        }

        bool Has(const char* InKey) const { return Find(InKey) != nullptr; }

        /* InKey's value, or InDefault. */
        const char* Get(const char* InKey, const char* InDefault = "") const
        {
            const std::string* Found = Find(InKey);
            return Found ? Found->c_str() : InDefault;
        }

        /* InKey's value as a number ("1", "0.5"), or InDefault when absent or not a number. */
        double GetNumber(const char* InKey, const double InDefault = 0.0) const
        {
            const std::string* Found = Find(InKey);
            if(!Found || Found->empty()) { return InDefault; }
            char* End = nullptr;
            const double Value = std::strtod(Found->c_str(), &End);
            return (End && *End == '\0') ? Value : InDefault;
        }

        /*
         * Adds "Key=Value" pairs from InText, separated by ';' or ','
         * ("Alarm=1;Lift=B"): what Lua and the console pass. Spaces round
         * keys and values are trimmed; a pair without '=' is Key=1.
         */
        void SetFromText(const char* InText)
        {
            if(!InText) { return; }
            std::string Pair;
            for(const char* At = InText;; ++At)
            {
                if(*At == ';' || *At == ',' || *At == '\0')
                {
                    const size_t Equals = Pair.find('=');
                    const std::string Key   = Trim(Equals == std::string::npos ? Pair : Pair.substr(0, Equals));
                    const std::string Value = Equals == std::string::npos ? std::string("1") : Trim(Pair.substr(Equals + 1));
                    if(!Key.empty()) { Set(Key, Value); }
                    Pair.clear();
                    if(*At == '\0') { break; }
                    continue;
                }
                Pair += *At;
            }
        }

        /* "Key=Value;Key=Value", as SetFromText reads it. */
        std::string ToText() const
        {
            std::string Text;
            for(const std::pair<std::string, std::string>& Each : Values)
            {
                if(!Text.empty()) { Text += ';'; }
                Text += Each.first + "=" + Each.second;
            }
            return Text;
        }

    private:
        const std::string* Find(const char* InKey) const
        {
            if(!InKey) { return nullptr; }
            for(const std::pair<std::string, std::string>& Each : Values)
            {
                if(Each.first == InKey) { return &Each.second; }
            }
            return nullptr;
        }

        static std::string Trim(const std::string& InText)
        {
            const size_t First = InText.find_first_not_of(" \t");
            if(First == std::string::npos) { return std::string(); }
            const size_t Last = InText.find_last_not_of(" \t");
            return InText.substr(First, Last - First + 1);
        }
    };
}

#endif //NIMBLEENGINE_LEVELTRAVEL_H

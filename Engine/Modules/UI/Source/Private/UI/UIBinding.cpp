#include "Public/UI/UIBinding.h"

namespace UI
{
    namespace
    {
        constexpr const char  OPEN[]     = "{Bind:";
        constexpr size_t      OPEN_CHARS = sizeof(OPEN) - 1;

        /* Calls InEach(Key) for each {Bind:Key} in InText, and InText(literal) for the text between. */
        template<typename TKey, typename TLiteral>
        bool Walk(const std::string& InText, TKey&& InKey, TLiteral&& InLiteral)
        {
            bool   bAny = false;
            size_t At   = 0;
            while(At < InText.size())
            {
                const size_t Open = InText.find(OPEN, At);
                const size_t Close = Open == std::string::npos ? std::string::npos : InText.find('}', Open + OPEN_CHARS);
                if(Close == std::string::npos)
                {
                    InLiteral(InText.substr(At));
                    break;
                }
                InLiteral(InText.substr(At, Open - At));
                InKey(InText.substr(Open + OPEN_CHARS, Close - Open - OPEN_CHARS));
                bAny = true;
                At = Close + 1;
            }
            return bAny;
        }
    }

    bool UIDataStore::SetValue(const std::string& InKey, const UIValue& InValue)
    {
        auto Found = Values.find(InKey);
        if(Found != Values.end() && Found->second == InValue)
        {
            return false;
        }
        Values[InKey] = InValue;
        ++Version;
        return true;
    }

    const UIValue* UIDataStore::Find(const std::string& InKey) const
    {
        const auto Found = Values.find(InKey);
        return Found != Values.end() ? &Found->second : nullptr;
    }

    bool UIDataStore::Remove(const std::string& InKey)
    {
        if(Values.erase(InKey) == 0) return false;
        ++Version;
        return true;
    }

    void UIDataStore::Clear()
    {
        if(Values.empty()) return;
        Values.clear();
        ++Version;
    }

    bool ParseBinding(const std::string& InText, bool& bOutWhole, std::vector<std::string>* OutKeys)
    {
        if(InText.find(OPEN) == std::string::npos)
        {
            bOutWhole = false;
            return false;
        }
        size_t Keys = 0;
        bool   bLiteral = false;
        const bool bAny = Walk(InText,
            [&](const std::string& InKey) { ++Keys; if(OutKeys) OutKeys->push_back(InKey); },
            [&](const std::string& InLiteral) { if(!InLiteral.empty()) bLiteral = true; });
        bOutWhole = bAny && Keys == 1 && !bLiteral;
        return bAny;
    }

    std::string ResolveBindingText(const std::string& InText, const UIDataStore& InStore)
    {
        std::string Out;
        Out.reserve(InText.size());
        Walk(InText,
            [&](const std::string& InKey)
            {
                if(const UIValue* Value = InStore.Find(InKey)) { Out += Value->AsString(); }
            },
            [&](const std::string& InLiteral) { Out += InLiteral; });
        return Out;
    }

    UIValue ResolveBindingValue(const std::string& InText, const UIDataStore& InStore)
    {
        UIValue Out;
        Walk(InText,
            [&](const std::string& InKey)
            {
                if(const UIValue* Value = InStore.Find(InKey)) { Out = *Value; }
            },
            [](const std::string&) {});
        return Out;
    }
}

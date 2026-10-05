#include "Public/UI/UIProperty.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "Public/UI/UILog.h"

namespace UI
{
    namespace
    {
        /*
         * Registered types, each as declared (its own properties) and as
         * resolved (its parent's first). Entries never move -- an instance
         * holds a type's pointer -- and the resolved lists are rebuilt when a
         * registration changes, before the next lookup.
         */
        struct Entry
        {
            uint32_t     Handle = 0;
            UIWidgetType Declared;
            std::string  ParentClassName;
            UIWidgetType Resolved;
        };

        struct Registry
        {
            std::mutex                          Lock;
            std::vector<std::unique_ptr<Entry>> Entries;
            uint32_t                            NextHandle = 1;
            bool                                bDirty     = true;

            Entry* FindClass(const std::string& InClassName)
            {
                for(auto& Each : Entries)
                {
                    if(Each->Declared.ClassName == InClassName) return Each.get();
                }
                return nullptr;
            }

            /* InEntry resolved, its parents first; InDepth guards a cycle. */
            void Resolve(Entry& InEntry, std::vector<Entry*>& InDone, const int InDepth)
            {
                if(std::find(InDone.begin(), InDone.end(), &InEntry) != InDone.end()) return;

                InEntry.Resolved = InEntry.Declared;
                InEntry.Resolved.Parent = nullptr;
                if(!InEntry.ParentClassName.empty())
                {
                    Entry* Parent = FindClass(InEntry.ParentClassName);
                    if(!Parent || InDepth > 32)
                    {
                        UI_LOG_ERROR("widget type '" + InEntry.Declared.Name + "': its parent class '"
                                     + InEntry.ParentClassName + "' is not registered")
                    }
                    else
                    {
                        Resolve(*Parent, InDone, InDepth + 1);
                        InEntry.Resolved.Parent = &Parent->Resolved;
                        std::vector<UIPropertyDesc> Merged = Parent->Resolved.Properties;
                        for(UIPropertyDesc& Own : InEntry.Declared.Properties)
                        {
                            /* A redeclared property replaces the parent's in place. */
                            auto Same = std::find_if(Merged.begin(), Merged.end(),
                                                     [&Own](const UIPropertyDesc& Each) { return Each.NameHash == Own.NameHash; });
                            if(Same != Merged.end()) { *Same = Own; }
                            else                     { Merged.push_back(Own); }
                        }
                        InEntry.Resolved.Properties = std::move(Merged);
                    }
                }
                InDone.push_back(&InEntry);
            }

            void EnsureResolved()
            {
                if(!bDirty) return;
                std::vector<Entry*> Done;
                for(auto& Each : Entries) { Resolve(*Each, Done, 0); }
                bDirty = false;
            }
        };

        Registry& GetRegistry()
        {
            static Registry Instance;
            return Instance;
        }

        /* "UI::UIWidget" -> "UIWidget": a class is registered and found by its bare name. */
        std::string BareClassName(const char* InName)
        {
            const std::string Name = InName ? InName : "";
            const size_t Colons = Name.rfind("::");
            return Colons == std::string::npos ? Name : Name.substr(Colons + 2);
        }

        const char* EnumName(const UIEnumNames& InNames, const int InIndex)
        {
            return InIndex >= 0 && uint32_t(InIndex) < InNames.Count ? InNames.Names[InIndex] : nullptr;
        }
    }

    // === UIWidgetType ============================================================

    int32_t UIWidgetType::FindProperty(const char* InName) const
    {
        return FindPropertyByHash(HashName(InName));
    }

    int32_t UIWidgetType::FindPropertyByHash(const uint32_t InNameHash) const
    {
        for(size_t At = 0; At < Properties.size(); ++At)
        {
            if(Properties[At].NameHash == InNameHash) return int32_t(At);
        }
        return -1;
    }

    bool UIWidgetType::IsA(const UIWidgetType& InOther) const
    {
        for(const UIWidgetType* Each = this; Each; Each = Each->Parent)
        {
            if(Each == &InOther || Each->NameHash == InOther.NameHash) return true;
        }
        return false;
    }

    const UIWidgetType* FindWidgetType(const char* InName)
    {
        if(!InName) return nullptr;
        return FindWidgetTypeByHash(HashName(InName));
    }

    const UIWidgetType* FindWidgetTypeByHash(const uint32_t InNameHash)
    {
        Registry& R = GetRegistry();
        std::lock_guard<std::mutex> Guard(R.Lock);
        R.EnsureResolved();
        for(auto& Each : R.Entries)
        {
            if(Each->Resolved.NameHash == InNameHash) return &Each->Resolved;
        }
        return nullptr;
    }

    const UIWidgetType* FindWidgetTypeByClass(const char* InClassName)
    {
        if(!InClassName) return nullptr;
        Registry& R = GetRegistry();
        std::lock_guard<std::mutex> Guard(R.Lock);
        R.EnsureResolved();
        Entry* Found = R.FindClass(BareClassName(InClassName));
        return Found ? &Found->Resolved : nullptr;
    }

    std::vector<const UIWidgetType*> GetWidgetTypes()
    {
        Registry& R = GetRegistry();
        std::lock_guard<std::mutex> Guard(R.Lock);
        R.EnsureResolved();
        std::vector<const UIWidgetType*> Out;
        for(auto& Each : R.Entries) { Out.push_back(&Each->Resolved); }
        std::sort(Out.begin(), Out.end(), [](const UIWidgetType* InA, const UIWidgetType* InB) { return InA->Name < InB->Name; });
        return Out;
    }

    // === Binder ===================================================================

    UIWidgetTypeBinder::UIWidgetTypeBinder(const char* InClassName, const char* InParentClassName, const char* InName)
        : ParentClassName(BareClassName(InParentClassName))
    {
        Type.ClassName = BareClassName(InClassName);
        Type.Name      = InName;
        Type.NameHash  = HashName(InName);
    }

    UIWidgetTypeBinder& UIWidgetTypeBinder::Add(const char* InName, const EUIPropertyType InType, const uint32_t InFlags,
                                                UIValue (*InGet)(const UIWidget&), bool (*InSet)(UIWidget&, const UIValue&))
    {
        UIPropertyDesc Desc;
        Desc.Name     = InName;
        Desc.NameHash = HashName(InName);
        Desc.Type     = InType;
        Desc.Flags    = InFlags;
        Desc.Category = CurrentCategory;
        Desc.Get      = InGet;
        Desc.Set      = InSet;
        Type.Properties.push_back(std::move(Desc));
        return *this;
    }

    uint32_t UIWidgetTypeBinder::Commit()
    {
        Registry& R = GetRegistry();
        std::lock_guard<std::mutex> Guard(R.Lock);
        for(auto& Each : R.Entries)
        {
            if(Each->Declared.NameHash == Type.NameHash || Each->Declared.ClassName == Type.ClassName)
            {
                UI_LOG_ERROR("widget type '" + Type.Name + "' (" + Type.ClassName + ") is already registered; ignoring the second")
                return 0;
            }
        }
        auto Made = std::make_unique<Entry>();
        Made->Handle          = R.NextHandle++;
        Made->Declared        = Type;
        Made->ParentClassName = ParentClassName;
        R.Entries.push_back(std::move(Made));
        R.bDirty = true;
        return R.Entries.back()->Handle;
    }

    void UnregisterWidgetType(const uint32_t InHandle)
    {
        if(InHandle == 0) return;
        Registry& R = GetRegistry();
        std::lock_guard<std::mutex> Guard(R.Lock);
        for(auto It = R.Entries.begin(); It != R.Entries.end(); ++It)
        {
            if((*It)->Handle == InHandle)
            {
                R.Entries.erase(It);
                R.bDirty = true;
                return;
            }
        }
    }

    // === Text =====================================================================

    std::string FormatPropertyValue(const UIPropertyDesc& InDesc, const UIValue& InValue)
    {
        switch(InDesc.Type)
        {
            case EUIPropertyType::Float: return FormatFloat(float(InValue.AsNumber()));
            case EUIPropertyType::Int:   return std::to_string(int32_t(InValue.AsNumber()));
            case EUIPropertyType::Bool:  return InValue.AsBool() ? "true" : "false";
            case EUIPropertyType::Enum:
            {
                const char* Name = EnumName(InDesc.Choices, int(InValue.AsNumber()));
                return Name ? Name : std::to_string(int(InValue.AsNumber()));
            }
            case EUIPropertyType::Color:  return FormatColor(InValue.AsColor());
            case EUIPropertyType::Vec2:   return FormatVec2(InValue.AsVec2());
            case EUIPropertyType::Margin: return FormatMargin(InValue.AsMargin());
            case EUIPropertyType::String:
            case EUIPropertyType::Asset:
            case EUIPropertyType::Action: return InValue.AsString();
        }
        return InValue.AsString();
    }

    bool ParsePropertyValue(const UIPropertyDesc& InDesc, const std::string& InText, UIValue& OutValue)
    {
        switch(InDesc.Type)
        {
            case EUIPropertyType::Float:
            case EUIPropertyType::Int:
            {
                float Value = 0.0f;
                if(!ParseFloat(InText, Value)) return false;
                OutValue = UIValue::Number(InDesc.Type == EUIPropertyType::Int ? std::round(Value) : Value);
                return true;
            }
            case EUIPropertyType::Bool:
                if(InText == "true" || InText == "1")  { OutValue = UIValue::Bool(true);  return true; }
                if(InText == "false" || InText == "0") { OutValue = UIValue::Bool(false); return true; }
                return false;
            case EUIPropertyType::Enum:
                for(uint32_t At = 0; At < InDesc.Choices.Count; ++At)
                {
                    if(InText == InDesc.Choices.Names[At]) { OutValue = UIValue::Number(At); return true; }
                }
                return false;
            case EUIPropertyType::Color:
            {
                UIColor Value;
                if(!ParseColor(InText, Value)) return false;
                OutValue = UIValue::Color(Value);
                return true;
            }
            case EUIPropertyType::Vec2:
            {
                UIVec2 Value;
                if(!ParseVec2(InText, Value)) return false;
                OutValue = UIValue::Vec2(Value);
                return true;
            }
            case EUIPropertyType::Margin:
            {
                UIMargin Value;
                if(!ParseMargin(InText, Value)) return false;
                OutValue = UIValue::Margin(Value);
                return true;
            }
            case EUIPropertyType::Action:
            {
                UIActionList Probe;
                if(!Probe.Parse(InText)) return false;
                OutValue = UIValue::String(InText);
                return true;
            }
            case EUIPropertyType::String:
            case EUIPropertyType::Asset:
                OutValue = UIValue::String(InText);
                return true;
        }
        return false;
    }
}

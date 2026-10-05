#ifndef UI_UIBINDING_H
#define UI_UIBINDING_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/UI/UIAPI.h"
#include "Public/UI/UITypes.h"

/*
 * DATA BINDING, push model: the game (or Lua) puts values in the data store
 * by key -- UISystem::SetValue("Player0.Score", 10) -- and a property written
 *
 *     Text="{Bind:Player0.Score}"            the whole value
 *     Text="Score: {Bind:Player0.Score}"     interpolated, String properties only
 *
 * follows it. Bindable properties only (UPF_Bindable). A document re-applies
 * its bindings when the store's version moves, and sets a property only when
 * its value changed -- an unchanged HUD costs a compare per binding.
 *
 * TWO-WAY: an input widget's own change (a CheckBox ticked, a Slider
 * dragged) writes its whole-value binding's key back to the store
 * (UIDocumentInstance::SetInputValue).
 *
 * RESERVED for later: pull bindings ({Bind:Lua:expr}, a component's field)
 * and formats ({Bind:X|0.0}).
 */
namespace UI
{
    class UI_API UIDataStore
    {
    public:
        /* False if the value was already InValue (nothing changes). */
        bool SetValue(const std::string& InKey, const UIValue& InValue);

        /* Null if InKey was never set. */
        const UIValue* Find(const std::string& InKey) const;

        bool Remove(const std::string& InKey);
        void Clear();

        /* Bumped by every change. */
        uint64_t GetVersion() const { return Version; }

    private:
        std::unordered_map<std::string, UIValue> Values;
        uint64_t                                 Version = 1;
    };

    /* One bound property of one widget in a document instance. */
    struct UIBinding
    {
        uint32_t    Widget   = 0;    // the instance's index
        int32_t     Property = -1;   // its type's
        std::string Template;        // as written
        bool        bWhole   = false;
    };

    /* Whether InText binds anything; bWhole if it is exactly one {Bind:Key}. */
    UI_API bool ParseBinding(const std::string& InText, bool& bOutWhole, std::vector<std::string>* OutKeys = nullptr);

    /* InText with each {Bind:Key} replaced by the store's value as text ("" if unset). */
    UI_API std::string ResolveBindingText(const std::string& InText, const UIDataStore& InStore);

    /* The value of a whole-value binding; None if its key is unset. */
    UI_API UIValue ResolveBindingValue(const std::string& InText, const UIDataStore& InStore);
}

#endif //UI_UIBINDING_H

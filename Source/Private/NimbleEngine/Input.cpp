#include "Public/NimbleEngine/Input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Private/NimbleEngine/InputInternal.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"
#include "Public/GameWindow/GameWindowInput.h"

/*
 * Input.h's engine half: key names and ids, the snapshot, the map registry,
 * the InputComponent and its resolve step, and the "Input" script library.
 */

namespace NimbleEngine
{
namespace
{
    // === Keys =================================================================
    // An InputKey is a kind in the high byte and an index in the low one.

    enum EKind : uint16_t
    {
        IK_Key         = 0,
        IK_MouseButton = 1,
        IK_PadButton   = 2,
        IK_MouseAxis   = 3,
        IK_PadAxis     = 4,
    };

    constexpr InputKey MakeKey(const EKind InKind, const uint16_t InIndex) { return InputKey((uint16_t(InKind) << 8) | InIndex); }
    constexpr EKind    KindOf(const InputKey InKey)  { return EKind(InKey >> 8); }
    constexpr uint16_t IndexOf(const InputKey InKey) { return uint16_t(InKey & 0xFF); }

    static_assert(KC_MAX <= 256, "EKeyCode must fit the key slots");
    static_assert(MB_MAX <= 8, "EMouseButton must fit the mouse slots");

    /* SDL_GamepadButton's and SDL_GamepadAxis's order, which GameWindow's
     * gamepad events keep. */
    const char* const GPadButtons[] = {
        "South", "East", "West", "North", "Back", "Guide", "Start", "LeftStick", "RightStick",
        "LeftShoulder", "RightShoulder", "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
    };
    const char* const GPadAxes[]    = { "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger" };
    const char* const GMouseAxes[]  = { "DeltaX", "DeltaY", "WheelX", "WheelY" };

    constexpr uint16_t PAD_BUTTONS = uint16_t(sizeof(GPadButtons) / sizeof(GPadButtons[0]));
    constexpr uint16_t PAD_AXES    = uint16_t(sizeof(GPadAxes) / sizeof(GPadAxes[0]));
    static_assert(PAD_BUTTONS <= 32 && PAD_AXES <= 8, "pad slots in InputState");

    bool EqualsNoCase(const char* InA, const char* InB)
    {
        for(; *InA != '\0' && *InB != '\0'; ++InA, ++InB)
        {
            if(std::tolower(static_cast<unsigned char>(*InA)) != std::tolower(static_cast<unsigned char>(*InB))) { return false; }
        }
        return *InA == *InB;
    }

    /* "Gamepad.South" -> "South" when InName starts with InPrefix. */
    const char* AfterPrefix(const char* InName, const char* InPrefix)
    {
        const size_t Length = std::strlen(InPrefix);
        for(size_t Index = 0; Index < Length; ++Index)
        {
            if(std::tolower(static_cast<unsigned char>(InName[Index])) != std::tolower(static_cast<unsigned char>(InPrefix[Index]))) { return nullptr; }
        }
        return InName + Length;
    }

    int FindIn(const char* const* InNames, const uint16_t InCount, const char* InName)
    {
        for(uint16_t Index = 0; Index < InCount; ++Index)
        {
            if(EqualsNoCase(InNames[Index], InName)) { return Index; }
        }
        return -1;
    }

    /* InputState's slot for a button key; -1 for an analog key or a bad player. */
    int ButtonSlot(const InputKey InKey, const uint8_t InPlayer)
    {
        switch(KindOf(InKey))
        {
        case IK_Key:         return IndexOf(InKey) < KC_MAX ? int(IndexOf(InKey)) : -1;
        case IK_MouseButton: return IndexOf(InKey) < MB_MAX ? 256 + int(IndexOf(InKey)) : -1;
        case IK_PadButton:
            return IndexOf(InKey) < PAD_BUTTONS && InPlayer < INPUT_PLAYERS_MAX ? 264 + InPlayer * 32 + int(IndexOf(InKey)) : -1;
        default:             return -1;
        }
    }

    int PadAxisSlot(const InputKey InKey, const uint8_t InPlayer)
    {
        return KindOf(InKey) == IK_PadAxis && IndexOf(InKey) < PAD_AXES && InPlayer < INPUT_PLAYERS_MAX
             ? 4 + InPlayer * 8 + int(IndexOf(InKey)) : -1;
    }
}

InputKey FindInputKey(const char* InName)
{
    if(!InName || InName[0] == '\0') { return INPUT_KEY_INVALID; }

    if(const char* Rest = AfterPrefix(InName, "Gamepad."))
    {
        const int Button = FindIn(GPadButtons, PAD_BUTTONS, Rest);
        if(Button >= 0) { return MakeKey(IK_PadButton, uint16_t(Button)); }
        const int Axis = FindIn(GPadAxes, PAD_AXES, Rest);
        return Axis >= 0 ? MakeKey(IK_PadAxis, uint16_t(Axis)) : INPUT_KEY_INVALID;
    }
    if(const char* Rest = AfterPrefix(InName, "Mouse."))
    {
        const int Axis = FindIn(GMouseAxes, 4, Rest);
        if(Axis >= 0) { return MakeKey(IK_MouseAxis, uint16_t(Axis)); }
        EMouseButton Button;
        return FindMouseButtonByName(InName, Button) ? MakeKey(IK_MouseButton, Button) : INPUT_KEY_INVALID;
    }
    const EKeyCode Key = FindKeyByName(InName);
    return Key == KC_Unknown ? INPUT_KEY_INVALID : MakeKey(IK_Key, Key);
}

const char* GetInputKeyName(const InputKey InKey)
{
    /* Composed names live in a small rotating buffer: valid until a few more calls. */
    static char Buffers[4][48];
    static int  Next = 0;
    char* Out = Buffers[Next++ & 3];

    switch(KindOf(InKey))
    {
    case IK_Key:         return IndexOf(InKey) < KC_MAX ? GetKeyName(EKeyCode(IndexOf(InKey))) : "Unknown";
    case IK_MouseButton: return IndexOf(InKey) < MB_MAX ? GetMouseButtonName(EMouseButton(IndexOf(InKey))) : "Unknown";
    case IK_MouseAxis:   if(IndexOf(InKey) < 4) { std::snprintf(Out, 48, "Mouse.%s", GMouseAxes[IndexOf(InKey)]); return Out; } break;
    case IK_PadButton:   if(IndexOf(InKey) < PAD_BUTTONS) { std::snprintf(Out, 48, "Gamepad.%s", GPadButtons[IndexOf(InKey)]); return Out; } break;
    case IK_PadAxis:     if(IndexOf(InKey) < PAD_AXES) { std::snprintf(Out, 48, "Gamepad.%s", GPadAxes[IndexOf(InKey)]); return Out; } break;
    }
    return "Unknown";
}

bool IsAnalogInputKey(const InputKey InKey)
{
    return KindOf(InKey) == IK_MouseAxis || KindOf(InKey) == IK_PadAxis;
}

InputKey InputKeyFromKeyCode(const uint16_t InKeyCode)        { return MakeKey(IK_Key, InKeyCode); }
InputKey InputKeyFromMouseButton(const uint8_t InButton)      { return MakeKey(IK_MouseButton, InButton); }
InputKey InputKeyFromPadButton(const uint8_t InButton)        { return MakeKey(IK_PadButton, InButton); }
InputKey InputKeyFromPadAxis(const uint8_t InAxis)            { return MakeKey(IK_PadAxis, InAxis); }

// === InputState ===============================================================

bool InputState::IsDown(const InputKey InKey, const uint8_t InPlayer) const
{
    const int Slot = ButtonSlot(InKey, InPlayer);
    if(Slot >= 0) { return Buttons[Slot].bDown != 0; }
    return IsAnalogInputKey(InKey) && std::fabs(GetValue(InKey, InPlayer)) >= 0.5f;   // a trigger pulled half way
}

bool InputState::WasPressed(const InputKey InKey, const uint8_t InPlayer) const
{
    const int Slot = ButtonSlot(InKey, InPlayer);
    return Slot >= 0 && Buttons[Slot].Presses > 0;
}

bool InputState::WasReleased(const InputKey InKey, const uint8_t InPlayer) const
{
    const int Slot = ButtonSlot(InKey, InPlayer);
    return Slot >= 0 && Buttons[Slot].Releases > 0;
}

float InputState::GetValue(const InputKey InKey, const uint8_t InPlayer) const
{
    const int Slot = ButtonSlot(InKey, InPlayer);
    if(Slot >= 0) { return Buttons[Slot].bDown ? 1.0f : 0.0f; }
    if(KindOf(InKey) == IK_MouseAxis)
    {
        switch(IndexOf(InKey))
        {
        case 0:  return float(MouseDeltaX);
        case 1:  return float(MouseDeltaY);
        case 2:  return WheelX;
        case 3:  return WheelY;
        default: return 0.0f;
        }
    }
    const int Axis = PadAxisSlot(InKey, InPlayer);
    return Axis >= 0 ? Analog[Axis] : 0.0f;
}

bool InputState::IsPadConnected(const uint8_t InPlayer) const
{
    return InPlayer < INPUT_PLAYERS_MAX && PadConnected[InPlayer] != 0;
}

void InputState::Press(const InputKey InKey, const uint8_t InPlayer)
{
    const int Slot = ButtonSlot(InKey, InPlayer);
    if(Slot < 0 || Buttons[Slot].bDown) { return; }   // an OS repeat is not a press
    Buttons[Slot].bDown = 1;
    if(Buttons[Slot].Presses < 255) { ++Buttons[Slot].Presses; }
}

void InputState::Release(const InputKey InKey, const uint8_t InPlayer)
{
    const int Slot = ButtonSlot(InKey, InPlayer);
    if(Slot < 0 || !Buttons[Slot].bDown) { return; }
    Buttons[Slot].bDown = 0;
    if(Buttons[Slot].Releases < 255) { ++Buttons[Slot].Releases; }
}

void InputState::SetAnalog(const InputKey InKey, const float InValue, const uint8_t InPlayer)
{
    const int Axis = PadAxisSlot(InKey, InPlayer);
    if(Axis >= 0) { Analog[Axis] = InValue; }
}

void InputState::MoveMouse(const int InX, const int InY, const int InDeltaX, const int InDeltaY)
{
    MouseX = InX;
    MouseY = InY;
    MouseDeltaX += InDeltaX;
    MouseDeltaY += InDeltaY;
}

void InputState::AddWheel(const float InDeltaX, const float InDeltaY)
{
    WheelX += InDeltaX;
    WheelY += InDeltaY;
}

void InputState::SetFocus(const bool bInFocus) { bFocus = bInFocus ? 1 : 0; }

void InputState::SetPadConnected(const uint8_t InPlayer, const bool bInConnected)
{
    if(InPlayer >= INPUT_PLAYERS_MAX) { return; }
    PadConnected[InPlayer] = bInConnected ? 1 : 0;
    if(!bInConnected)
    {
        /* A pad pulled out mid-press must not leave its buttons held. */
        for(uint16_t Button = 0; Button < PAD_BUTTONS; ++Button) { Release(MakeKey(IK_PadButton, Button), InPlayer); }
        for(uint16_t Axis = 0; Axis < PAD_AXES; ++Axis) { SetAnalog(MakeKey(IK_PadAxis, Axis), 0.0f, InPlayer); }
    }
}

void InputState::ReleaseAll()
{
    for(Button& Each : Buttons)
    {
        if(Each.bDown)
        {
            Each.bDown = 0;
            if(Each.Releases < 255) { ++Each.Releases; }
        }
    }
    for(float& Each : Analog) { Each = 0.0f; }
}

void InputState::BeginFrame(const uint64_t InFrameNumber)
{
    for(Button& Each : Buttons) { Each.Presses = 0; Each.Releases = 0; }
    MouseDeltaX = MouseDeltaY = 0;
    WheelX = WheelY = 0.0f;
    FrameNumber = InFrameNumber;
}

// === Input maps ===============================================================

namespace
{
    struct Binding
    {
        InputKey Key   = INPUT_KEY_INVALID;
        float    Scale = 1.0f;
    };

    struct Entry
    {
        InputMapEntry                          Desc;
        Binding                                Bindings[INPUT_BINDINGS_MAX];
        uint8_t                                Count = 0;
        bool                                   bUnclamped = false;   // a mouse axis is bound
        std::unique_ptr<EngineCore::ConfigVar> Var;
    };

    struct Map
    {
        InputMapHandle     Handle = INPUT_MAP_INVALID;
        std::string        Name;
        std::string        Section;    // "Input_<Name>"; the config vars point at it
        std::vector<Entry> Actions;
        std::vector<Entry> Axes;
    };

    struct MapRegistry
    {
        std::vector<std::unique_ptr<Map>> Maps;   // by pointer: config vars hold Section's chars
        InputMapHandle                    NextHandle = 1;
    };

    MapRegistry& GetMaps()
    {
        static MapRegistry Instance;
        return Instance;
    }

    const Map* FindMap(const char* InName)
    {
        for(const std::unique_ptr<Map>& Each : GetMaps().Maps)
        {
            if(Each->Name == InName) { return Each.get(); }
        }
        return nullptr;
    }

    /* "D, Right, -A, Gamepad.LeftX*0.5" into InOut's bindings. Unknown keys are
     * skipped with a warning that names the map and entry. */
    void ParseBindings(const std::string& InMapName, Entry& InOut, const char* InText)
    {
        InOut.Count      = 0;
        InOut.bUnclamped = false;

        const std::string Text = InText ? InText : "";
        size_t Pos = 0;
        while(Pos <= Text.size())
        {
            size_t Comma = Text.find(',', Pos);
            if(Comma == std::string::npos) { Comma = Text.size(); }
            std::string Item = Text.substr(Pos, Comma - Pos);
            Pos = Comma + 1;

            const size_t First = Item.find_first_not_of(" \t");
            if(First == std::string::npos) { continue; }
            Item = Item.substr(First, Item.find_last_not_of(" \t") - First + 1);

            Binding Bound;
            if(Item[0] == '-') { Bound.Scale = -1.0f; Item = Item.substr(1); }
            const size_t Star = Item.find('*');
            if(Star != std::string::npos)
            {
                Bound.Scale *= float(std::atof(Item.c_str() + Star + 1));
                Item = Item.substr(0, Star);
                Item = Item.substr(0, Item.find_last_not_of(" \t") + 1);
            }

            Bound.Key = FindInputKey(Item.c_str());
            if(Bound.Key == INPUT_KEY_INVALID)
            {
                CORE_LOG_WARN("[Input] " + InMapName + "." + InOut.Desc.Name + ": no key named '" + Item + "'")
                continue;
            }
            if(InOut.Count >= INPUT_BINDINGS_MAX)
            {
                CORE_LOG_WARN("[Input] " + InMapName + "." + InOut.Desc.Name + ": more than "
                              + std::to_string(size_t(INPUT_BINDINGS_MAX)) + " keys; the rest are ignored")
                break;
            }
            InOut.bUnclamped = InOut.bUnclamped || KindOf(Bound.Key) == IK_MouseAxis;
            InOut.Bindings[InOut.Count++] = Bound;
        }
    }

    /* A config var changed (an .ini or the console): rebind its entry. */
    void OnBindingVarChanged(const EngineCore::ConfigVar& InVar)
    {
        for(const std::unique_ptr<Map>& Each : GetMaps().Maps)
        {
            for(std::vector<Entry>* List : { &Each->Actions, &Each->Axes })
            {
                for(Entry& Item : *List)
                {
                    if(Item.Var.get() == &InVar)
                    {
                        ParseBindings(Each->Name, Item, InVar.GetString());
                        return;
                    }
                }
            }
        }
    }

    int IndexIn(const std::vector<Entry>& InList, const char* InName)
    {
        for(size_t Index = 0; Index < InList.size(); ++Index)
        {
            if(std::strcmp(InList[Index].Desc.Name, InName) == 0) { return int(Index); }
        }
        return -1;
    }
}

InputMapHandle RegisterInputMap(const char* InName, const InputMapEntry* InEntries, const size_t InCount)
{
    const std::string Name = InName ? InName : "";
    if(Name.empty() || Name.size() >= INPUT_NAME_MAX || FindMap(Name.c_str()))
    {
        CORE_LOG_ERROR("[Input] input map '" + Name + "' is unnamed, too long, or already registered; refused")
        return INPUT_MAP_INVALID;
    }

    std::unique_ptr<Map> Added = std::make_unique<Map>();
    Added->Handle  = GetMaps().NextHandle++;
    Added->Name    = Name;
    Added->Section = "Input_" + Name;   // no period: the console addresses Section.Name

    for(size_t Index = 0; Index < InCount; ++Index)
    {
        const InputMapEntry& Desc = InEntries[Index];
        std::vector<Entry>& List = Desc.bAxis ? Added->Axes : Added->Actions;
        const size_t Max = Desc.bAxis ? size_t(INPUT_AXES_MAX) : size_t(INPUT_ACTIONS_MAX);
        if(List.size() >= Max || Desc.Name[0] == '\0' || IndexIn(List, Desc.Name) >= 0)
        {
            CORE_LOG_ERROR("[Input] " + Name + "." + Desc.Name + ": unnamed, repeated, or past the map's "
                           + std::to_string(Max) + (Desc.bAxis ? " axes" : " actions") + "; dropped")
            continue;
        }
        List.emplace_back();
        List.back().Desc = Desc;
    }

    /* Bindings from config where it says so ([Input_Default] Jump=...),
     * else the game's text: the var's value is whichever applies. */
    Map& Stored = *Added;
    GetMaps().Maps.push_back(std::move(Added));
    for(std::vector<Entry>* List : { &Stored.Actions, &Stored.Axes })
    {
        for(Entry& Item : *List)
        {
            Item.Var = std::make_unique<EngineCore::ConfigVar>(EngineCore::EConfigType::CT_String, Stored.Section.c_str(),
                                                               Item.Desc.Name, EngineCore::MakeConfigString(Item.Desc.Bindings),
                                                               &OnBindingVarChanged);
            ParseBindings(Stored.Name, Item, Item.Var->GetString());
        }
    }
    return Stored.Handle;
}

void UnregisterInputMap(const InputMapHandle InHandle)
{
    if(InHandle == INPUT_MAP_INVALID) { return; }
    std::vector<std::unique_ptr<Map>>& Maps = GetMaps().Maps;
    for(auto It = Maps.begin(); It != Maps.end(); ++It)
    {
        if((*It)->Handle == InHandle)
        {
            Maps.erase(It);   // the config vars unregister in their destructors
            return;
        }
    }
}

int FindInputAction(const char* InMap, const char* InAction)
{
    const Map* Found = FindMap(InMap ? InMap : "");
    return Found && InAction ? IndexIn(Found->Actions, InAction) : -1;
}

int FindInputAxis(const char* InMap, const char* InAxis)
{
    const Map* Found = FindMap(InMap ? InMap : "");
    return Found && InAxis ? IndexIn(Found->Axes, InAxis) : -1;
}

// === InputComponent ===========================================================

bool InputComponent::IsDown(const char* InAction) const
{
    const int Index = FindInputAction(Map, InAction);
    return Index >= 0 && (Down >> Index & 1u) != 0;
}

bool InputComponent::WasPressed(const char* InAction) const
{
    const int Index = FindInputAction(Map, InAction);
    return Index >= 0 && (Pressed >> Index & 1u) != 0;
}

bool InputComponent::WasReleased(const char* InAction) const
{
    const int Index = FindInputAction(Map, InAction);
    return Index >= 0 && (Released >> Index & 1u) != 0;
}

float InputComponent::GetAxis(const char* InAxis) const
{
    const int Index = FindInputAxis(Map, InAxis);
    return Index >= 0 ? Axes[Index] : 0.0f;
}

void ResolveInputComponents(GameCore::World& InWorld, const InputState& InState)
{
    GameCore::ComponentPool* Pool = InWorld.FindPool(InputComponent::StaticType().ID);
    if(!Pool) { return; }

    InputComponent* Components = static_cast<InputComponent*>(Pool->GetDenseData());
    for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
    {
        InputComponent& C = Components[Index];
        const uint32_t WasDown = C.Down;
        const Map* Bindings = C.bEnabled ? FindMap(C.Map) : nullptr;

        uint32_t NowDown = 0, Tapped = 0;
        for(size_t Action = 0; Bindings && Action < Bindings->Actions.size(); ++Action)
        {
            const Entry& Item = Bindings->Actions[Action];
            for(uint8_t Each = 0; Each < Item.Count; ++Each)
            {
                if(InState.IsDown(Item.Bindings[Each].Key, C.PlayerIndex))     { NowDown |= 1u << Action; }
                if(InState.WasPressed(Item.Bindings[Each].Key, C.PlayerIndex)) { Tapped  |= 1u << Action; }
            }
        }

        /* An edge is the action's, not a key's: a second key held down while
         * the first is still held does not press it again. A tap -- down and
         * up inside one frame -- is both. */
        C.Down     = NowDown;
        C.Pressed  = ~WasDown & (NowDown | Tapped);
        C.Released = (WasDown & ~NowDown) | (~WasDown & ~NowDown & Tapped);

        for(size_t Axis = 0; Axis < INPUT_AXES_MAX; ++Axis)
        {
            float Sum = 0.0f;
            const Entry* Item = Bindings && Axis < Bindings->Axes.size() ? &Bindings->Axes[Axis] : nullptr;
            for(uint8_t Each = 0; Item && Each < Item->Count; ++Each)
            {
                Sum += InState.GetValue(Item->Bindings[Each].Key, C.PlayerIndex) * Item->Bindings[Each].Scale;
            }
            C.Axes[Axis] = Item && Item->bUnclamped ? Sum : std::max(-1.0f, std::min(1.0f, Sum));
        }
    }
}

// === Script access ============================================================

namespace
{
    const InputState* GScriptInput = nullptr;
    IGameHost*        GScriptHost  = nullptr;

    const InputState& ScriptInput()
    {
        static const InputState Empty;
        return GScriptInput ? *GScriptInput : Empty;
    }

    bool  ScriptIsKeyDown(const char* InKey)       { return ScriptInput().IsDown(InKey); }
    bool  ScriptWasKeyPressed(const char* InKey)   { return ScriptInput().WasPressed(InKey); }
    bool  ScriptWasKeyReleased(const char* InKey)  { return ScriptInput().WasReleased(InKey); }
    float ScriptGetKeyValue(const char* InKey)     { return ScriptInput().GetValue(InKey); }
    bool  ScriptIsPadConnected(const int32_t InPlayer) { return ScriptInput().IsPadConnected(uint8_t(InPlayer)); }
    void  ScriptSetMouseCaptured(const bool bInCaptured) { if(GScriptHost) { GScriptHost->SetMouseCaptured(bInCaptured); } }
    bool  ScriptIsMouseCaptured()                        { return GScriptHost && GScriptHost->IsMouseCaptured(); }
}

void SetScriptInputState(const InputState* InState)
{
    GScriptInput = InState;
}

void SetScriptInputHost(IGameHost* InHost)
{
    GScriptHost = InHost;
}
}

using NimbleEngine::InputComponent;

GAMECORE_DEFINE_COMPONENT(InputComponent, "Input")

/* The authored half only; the rest is this frame's. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(InputComponent, 1)
{
    Ar.Field("Map",         C.Map);
    Ar.Field("PlayerIndex", C.PlayerIndex);
    Ar.Flag("Enabled",      C.bEnabled);
}

/* input:IsDown("Jump"), input:WasPressed, input:WasReleased, input:GetAxis("MoveX");
 * GetMap/SetMap, GetPlayerIndex/SetPlayerIndex, GetEnabled/SetEnabled. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(InputComponent)
{
    Bind.Field("Map",         &InputComponent::Map);
    Bind.Field("PlayerIndex", &InputComponent::PlayerIndex);
    Bind.Flag("Enabled",      &InputComponent::bEnabled);

    Bind.Native<&InputComponent::IsDown>("IsDown", "Action -> Down");
    Bind.Native<&InputComponent::WasPressed>("WasPressed", "Action -> Pressed");
    Bind.Native<&InputComponent::WasReleased>("WasReleased", "Action -> Released");
    Bind.Native<&InputComponent::GetAxis>("GetAxis", "Axis -> Value");
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(InputComponent)
{
    Props.Field("Map",         &InputComponent::Map);
    Props.Field("PlayerIndex", &InputComponent::PlayerIndex);
    Props.Flag("Enabled",      &InputComponent::bEnabled);
}
#endif

/* Raw device queries, for debug keys and UI: Input.IsKeyDown("F3"),
 * Input.GetMousePosition() -> x, y. Gameplay reads an InputComponent. */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Input)
{
    Lib.NativePure<&NimbleEngine::ScriptIsKeyDown>("IsKeyDown", "Key -> Down")
       .Tooltip("A key by name (\"Space\", \"Mouse.Left\", \"Gamepad.South\"), held this frame");
    Lib.NativePure<&NimbleEngine::ScriptWasKeyPressed>("WasKeyPressed", "Key -> Pressed");
    Lib.NativePure<&NimbleEngine::ScriptWasKeyReleased>("WasKeyReleased", "Key -> Released");
    Lib.NativePure<&NimbleEngine::ScriptGetKeyValue>("GetKeyValue", "Key -> Value")
       .Tooltip("0 or 1 for a button; the position of an axis (\"Gamepad.LeftX\", \"Mouse.DeltaX\")");
    Lib.NativePure<&NimbleEngine::ScriptIsPadConnected>("IsPadConnected", "Player -> Connected");
    Lib.Native<&NimbleEngine::ScriptSetMouseCaptured>("SetMouseCaptured", "Captured")
       .Tooltip("Hide and hold the cursor, for mouse-look; Mouse.DeltaX keeps coming");
    Lib.NativePure<&NimbleEngine::ScriptIsMouseCaptured>("IsMouseCaptured", "-> Captured");

    Lib.Pure("GetMousePosition", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::Point At = NimbleEngine::ScriptInput().GetMousePosition();
        Call.PushInteger(At.X);
        Call.PushInteger(At.Y);
    }, "-> X:integer, Y:integer").Tooltip("Physical pixels, window-relative");
    Lib.Pure("GetMouseDelta", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::Point Delta = NimbleEngine::ScriptInput().GetMouseDelta();
        Call.PushInteger(Delta.X);
        Call.PushInteger(Delta.Y);
    }, "-> X:integer, Y:integer").Tooltip("This frame's mouse motion, in pixels");
}

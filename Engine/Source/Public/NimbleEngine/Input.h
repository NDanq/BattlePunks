#ifndef NIMBLEENGINE_INPUT_H
#define NIMBLEENGINE_INPUT_H

#include <cstddef>
#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace GameCore { class World; }

/*
 * Input for games: SDL, through GameWindow, to a per-frame snapshot, to named
 * actions and axes on an InputComponent.
 *
 *   InputState      this frame's devices: keys, mouse, gamepads. Latched by the
 *                   host after the event pump; GameSession::GetInput().
 *   InputMap        named actions and axes bound to key names, declared by the
 *                   game (NIMBLE_DEFINE_INPUT_MAP) and overridable in config
 *                   ([Input_<Map>] Jump=Space, Gamepad.South).
 *   InputComponent  "Input": which map an entity reads, resolved into its
 *                   action bits and axis values at the START of TP_PrePhysics,
 *                   before that phase's scripts (World's SO_BeforeScripts), so
 *                   every script and system this frame sees this frame's input.
 *
 * KEYS ARE NAMES ("W", "Space", "Mouse.Left", "Mouse.DeltaX", "Gamepad.South",
 * "Gamepad.LeftX"), resolved once to an InputKey. GameWindow's key enum stays
 * private to the engine; a name works the same in C++, Lua, an .ini and the
 * editor.
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    /* An input source: a key, a mouse button or axis, a gamepad button or axis.
     * Opaque; from FindInputKey. */
    class IGameHost;

    using InputKey = uint16_t;
    constexpr InputKey INPUT_KEY_INVALID = 0xFFFF;

    enum : size_t
    {
        INPUT_PLAYERS_MAX  = 4,    // local players; keyboard and mouse are player 0's
        INPUT_NAME_MAX     = 32,   // including the NUL: maps, actions, axes
        INPUT_ACTIONS_MAX  = 32,   // per map; one bit each on an InputComponent
        INPUT_AXES_MAX     = 16,   // per map
        INPUT_BINDINGS_MAX = 8,    // keys per action or axis
        INPUT_TEXT_MAX     = 64,   // bytes of typed text a frame keeps, including the NUL
    };

    /* "Space" -> its key; INPUT_KEY_INVALID if no key has that name. Case-insensitive. */
    NE_API InputKey    FindInputKey(const char* InName);
    NE_API const char* GetInputKeyName(InputKey InKey);

    /* True for a key whose value is a position rather than up/down
     * (Mouse.DeltaX, Gamepad.LeftX). */
    NE_API bool IsAnalogInputKey(InputKey InKey);

    /*
     * One frame's devices. Edges are counts, so a key pressed and released
     * within one frame still reports WasPressed and WasReleased. A gamepad key
     * reads player InPlayer's pad; keyboard and mouse ignore the player.
     */
    class InputRouter;

    class NE_API InputState
    {
    public:
        bool  IsDown     (InputKey InKey, uint8_t InPlayer = 0) const;
        bool  WasPressed (InputKey InKey, uint8_t InPlayer = 0) const;   // this frame
        bool  WasReleased(InputKey InKey, uint8_t InPlayer = 0) const;

        /* Presses plus the OS's repeats while held, this frame: how many times
         * a text field's Backspace or arrow acts. Gameplay wants WasPressed. */
        int   GetPressesAndRepeats(InputKey InKey, uint8_t InPlayer = 0) const;

        /* 0 or 1 for a button; the position for an analog key (-1..1 for a
         * stick, pixels for Mouse.DeltaX, notches for Mouse.WheelY). */
        float GetValue(InputKey InKey, uint8_t InPlayer = 0) const;

        /* The same, by name. */
        bool  IsDown     (const char* InKeyName, uint8_t InPlayer = 0) const { return IsDown(FindInputKey(InKeyName), InPlayer); }
        bool  WasPressed (const char* InKeyName, uint8_t InPlayer = 0) const { return WasPressed(FindInputKey(InKeyName), InPlayer); }
        bool  WasReleased(const char* InKeyName, uint8_t InPlayer = 0) const { return WasReleased(FindInputKey(InKeyName), InPlayer); }
        float GetValue   (const char* InKeyName, uint8_t InPlayer = 0) const { return GetValue(FindInputKey(InKeyName), InPlayer); }

        /* Physical pixels, window-relative. The delta is this frame's motion,
         * and keeps coming while the mouse is captured (relative mode). */
        EngineCore::Point GetMousePosition() const { return { MouseX, MouseY }; }
        EngineCore::Point GetMouseDelta() const    { return { MouseDeltaX, MouseDeltaY }; }
        float             GetWheelDelta() const    { return WheelY; }

        /* This frame's typed text, UTF-8 after the keyboard layout and IME
         * (GameWindow's OnText). Only while the host's text input is on
         * (IGameHost::SetTextInputActive); "" otherwise. */
        const char* GetText() const      { return Text; }

        bool     HasFocus() const        { return bFocus != 0; }
        bool     IsPadConnected(uint8_t InPlayer) const;
        uint64_t GetFrameNumber() const  { return FrameNumber; }

        // --- host side ---------------------------------------------------------
        // Written by the engine's InputRouter (and tests); games only read.

        void Press  (InputKey InKey, uint8_t InPlayer = 0);
        void Release(InputKey InKey, uint8_t InPlayer = 0);
        void Repeat (InputKey InKey, uint8_t InPlayer = 0);   // an OS auto-repeat of a held key
        void AppendText(const char* InUtf8);                  // dropped past INPUT_TEXT_MAX
        void SetAnalog(InputKey InKey, float InValue, uint8_t InPlayer = 0);
        void MoveMouse(int InX, int InY, int InDeltaX, int InDeltaY);
        void AddWheel(float InDeltaX, float InDeltaY);
        void SetFocus(bool bInFocus);
        void SetPadConnected(uint8_t InPlayer, bool bInConnected);

        /* Everything up, without losing the edges: focus loss, so nothing sticks. */
        void ReleaseAll();

        /*
         * Taken by something in front of the game -- the UI over a button, a
         * menu -- so the game does not see it this frame: up, no edges, no
         * value. Unlike ReleaseAll there is no release edge: a key consumed
         * while held looks to the game as if it went up without one, and an
         * InputComponent reports that as released. GameSession consumes what
         * its UISystem took (INPUT.md, "The UI").
         */
        void Consume(InputKey InKey, uint8_t InPlayer = 0);
        void ConsumeKeyboard();       // and the typed text
        void ConsumeMouseButtons();   // and the wheel
        void ConsumeGamepad();        // every player's buttons and sticks
        void ConsumeAll();

        /* Starts the next frame: edges and per-frame motion back to zero, held
         * state kept. */
        void BeginFrame(uint64_t InFrameNumber);

        enum : size_t
        {
            BUTTON_SLOTS = 256 + 8 + INPUT_PLAYERS_MAX * 32,   // keys, mouse, pads
            ANALOG_SLOTS = 4 + INPUT_PLAYERS_MAX * 8,          // mouse axes, pad axes
        };

    private:
        friend class InputRouter;   // stamps the frame number it latches

        struct Button
        {
            uint8_t bDown    = 0;
            uint8_t Presses  = 0;
            uint8_t Releases = 0;
            uint8_t Repeats  = 0;
        };

        Button   Buttons[BUTTON_SLOTS];
        float    Analog[ANALOG_SLOTS] = {};
        int32_t  MouseX = 0, MouseY = 0, MouseDeltaX = 0, MouseDeltaY = 0;
        float    WheelX = 0.0f, WheelY = 0.0f;
        char     Text[INPUT_TEXT_MAX] = {};
        uint8_t  bFocus = 1;
        uint8_t  PadConnected[INPUT_PLAYERS_MAX] = {};
        uint64_t FrameNumber = 0;
    };

    // === Input maps ===========================================================

    /*
     * One action or axis: its name and its bindings as text, the form an .ini
     * uses too. An action is down while any of its keys is. An axis sums its
     * keys: "D, Right, -A, -Left, Gamepad.LeftX" -- a leading '-' negates, and
     * "*0.5" after a key scales it. The sum is clamped to -1..1 unless a mouse
     * axis is bound (mouse look wants pixels).
     */
    struct InputMapEntry
    {
        char Name[INPUT_NAME_MAX] = {};
        bool bAxis = false;
        char Bindings[128]        = {};   // EngineCore::CONFIG_STRING_MAX
    };

    using InputMapHandle = uint32_t;
    constexpr InputMapHandle INPUT_MAP_INVALID = 0;

    /* Called by the macro; refused (INPUT_MAP_INVALID, logged) for a name
     * already registered or an over-full map. Each entry also becomes a config
     * var, [Input_<Map>] <Name>, so an .ini or the console (Input_Default.Jump K)
     * can rebind it. */
    NE_API InputMapHandle RegisterInputMap(const char* InName, const InputMapEntry* InEntries, size_t InCount);
    NE_API void           UnregisterInputMap(InputMapHandle InHandle);

    /* Index of an action or axis in a map, for a hot loop; -1 if none. */
    NE_API int FindInputAction(const char* InMap, const char* InAction);
    NE_API int FindInputAxis(const char* InMap, const char* InAxis);

    /* What `Map` is inside NIMBLE_DEFINE_INPUT_MAP. Plain data until Commit(). */
    class InputMapBinder
    {
    public:
        void Action(const char* InName, const char* InBindings) { Add(InName, InBindings, false); }
        void Axis  (const char* InName, const char* InBindings) { Add(InName, InBindings, true); }

        InputMapHandle Commit(const char* InMapName) { return RegisterInputMap(InMapName, Entries, Count); }

    private:
        void Add(const char* InName, const char* InBindings, const bool bInAxis)
        {
            if(Count >= INPUT_ACTIONS_MAX + INPUT_AXES_MAX) { return; }   // RegisterInputMap reports the shortfall
            InputMapEntry& Out = Entries[Count++];
            Copy(Out.Name, sizeof(Out.Name), InName);
            Copy(Out.Bindings, sizeof(Out.Bindings), InBindings);
            Out.bAxis = bInAxis;
        }

        static void Copy(char* OutBuffer, const size_t InCapacity, const char* InText)
        {
            size_t Length = 0;
            while(InText && InText[Length] != '\0' && Length + 1 < InCapacity) { OutBuffer[Length] = InText[Length]; ++Length; }
            OutBuffer[Length] = '\0';
        }

        InputMapEntry Entries[INPUT_ACTIONS_MAX + INPUT_AXES_MAX];
        size_t        Count = 0;
    };

    // === The component ========================================================

    /*
     * "Input". Map and PlayerIndex are authored and saved; the rest is written
     * by the engine at the start of TP_PrePhysics and is this frame's.
     */
    struct NE_API InputComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Map[INPUT_NAME_MAX] = "Default";   // an input map's name
        uint8_t PlayerIndex = 0;                   // 0 has the keyboard and mouse
        uint8_t bEnabled    = 1;                   // disabled: every action up, every axis 0

        uint32_t Down     = 0;                     // one bit per action, by map index
        uint32_t Pressed  = 0;                     // went down this frame
        uint32_t Released = 0;                     // went up this frame
        float    Axes[INPUT_AXES_MAX] = {};

        bool  IsDown     (const char* InAction) const;
        bool  WasPressed (const char* InAction) const;
        bool  WasReleased(const char* InAction) const;
        float GetAxis    (const char* InAxis) const;
    };

    // === Host side ============================================================
    // GameSession does both; exported for other hosts and tests.

    /* Every InputComponent in InWorld from InState: the TP_PrePhysics
     * SO_BeforeScripts system a session registers. */
    NE_API void ResolveInputComponents(GameCore::World& InWorld, const InputState& InState);

    /* Whether InMap's action InAction went down this frame in InState (any of
     * its bindings), for a reader without an InputComponent -- the UI's
     * navigation. bOutFromGamepad, if given, says a gamepad binding did it. */
    NE_API bool WasInputActionPressed(const char* InMap, const char* InAction, const InputState& InState,
                                      uint8_t InPlayer = 0, bool* bOutFromGamepad = nullptr);

    /* What the "Input" script library reads; null reads nothing pressed. */
    NE_API void SetScriptInputState(const InputState* InState);

    /* Where the "Input" library's SetMouseCaptured goes; null ignores it. */
    NE_API void SetScriptInputHost(IGameHost* InHost);
}

/*
 * A game's bindings, at file scope in one .cpp. MapName is the name an
 * InputComponent's Map field uses ("Default"). Unregisters with its module.
 *
 *     NIMBLE_DEFINE_INPUT_MAP(Default)
 *     {
 *         Map.Action("Jump",  "Space, Gamepad.South");
 *         Map.Axis  ("MoveX", "D, Right, -A, -Left, Gamepad.LeftX");
 *     }
 */
#define NIMBLE_DEFINE_INPUT_MAP(MapName)                                                             \
    static void MapName##_InputMapBody(::NimbleEngine::InputMapBinder& Map);                         \
    namespace                                                                                         \
    {                                                                                                 \
        struct MapName##_InputMapRegistrar                                                            \
        {                                                                                             \
            MapName##_InputMapRegistrar()                                                             \
            {                                                                                         \
                ::NimbleEngine::InputMapBinder Map;                                                   \
                MapName##_InputMapBody(Map);                                                          \
                Handle = Map.Commit(#MapName);                                                        \
            }                                                                                         \
            ~MapName##_InputMapRegistrar() { ::NimbleEngine::UnregisterInputMap(Handle); }            \
            ::NimbleEngine::InputMapHandle Handle = ::NimbleEngine::INPUT_MAP_INVALID;                \
        };                                                                                            \
        MapName##_InputMapRegistrar G##MapName##_InputMapRegistrar;                                   \
    }                                                                                                 \
    static void MapName##_InputMapBody(::NimbleEngine::InputMapBinder& Map)

#endif //NIMBLEENGINE_INPUT_H

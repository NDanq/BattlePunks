# GameWindow

Window creation, event pumping and input delivery, on SDL3.

`GameWindow` is the first module written *for* this repository rather than
vendored into it. `Modules/Core` and `Modules/GDI` are git subtrees of
standalone repositories that must keep configuring on their own; GameWindow is
not, which makes it the intended first consumer of `nimble_add_module()` -- the
helper in `Engine/cmake/NimbleModule.cmake` that today, by its own comment,
"serves new modules only."

---

## Position in the tree

GameWindow sits between the host executable and GDI, and depends on
`EngineCore` alone:

```
Host exe --> GameWindow --> EngineCore
   \-------> GDI --------> EngineCore
```

**GameWindow does not link GDI, and GDI does not link GameWindow.**

GDI already owns its window-system seam -- `GDIPlatform::appGLCreateContext`,
`GDIPlatform::appVulkanCreateSurface` -- and takes an
`EngineCore::WindowHandle` into `InitializeGDI(ERenderPlatform, WindowHandle)`.
GameWindow's job ends at producing that handle. Coupling the two would put SDL
inside GDI's Vulkan and OpenGL backends, which is precisely what
`GDIVulkanPlatform.h` says an SDL host is supposed to avoid:

> An SDL host swaps these two functions for `SDL_Vulkan_GetInstanceExtensions`
> and `SDL_Vulkan_CreateSurface` and changes nothing else in the backend.

---

## File layout

```
Engine/Modules/GameWindow/
|-- CMakeLists.txt
|-- README.md                       (this file)
|-- Source/
|   |-- Public/GameWindow/
|   |   |-- GameWindowAPI.h         GAMEWINDOW_API three-state export macro
|   |   |-- GameWindowLog.h         GW_LOG / GW_LOG_WARN / GW_LOG_ERROR over CoreLog
|   |   |-- GameWindowTypes.h       EWindowMode, EWindowFlags, WindowDesc
|   |   |-- GameWindowInput.h       EKeyCode, EMouseButton, EModifierKeys, event structs
|   |   |-- GameWindowDelegates.h   the POD callback table
|   |   |-- GameWindow.h            class GameWindow
|   |   `-- GameWindowSystem.h      Initialize / Shutdown / PumpEvents / registry
|   `-- Private/GameWindow/
|       |-- GameWindow.cpp
|       |-- GameWindowSystem.cpp
|       |-- GameWindowDispatch.cpp  SDL_Event -> delegate translation (the whole switch)
|       |-- GameWindowInputSDL.cpp  scancode / button translation tables
|       `-- Platform/
|           `-- GameWindowNativeWindows.cpp   SDL_Window* -> HWND
`-- Tests/
    `-- SmokeMain.cpp
```

Include paths are Source-relative -- `#include "Public/GameWindow/GameWindow.h"`
-- matching Core and GDI exactly.

---

## Public API

### `GameWindowTypes.h`

```cpp
enum EWindowMode : uint8_t
{
    WM_Windowed = 0,
    WM_FullscreenDesktop,     // borderless at the desktop's current mode
    WM_FullscreenExclusive,   // a real mode switch; needs a DisplayMode
    WM_MAX
};

enum EWindowFlags : uint32_t
{
    WF_None        = 0,
    WF_Resizable   = 1u << 0,
    WF_Borderless  = 1u << 1,
    WF_HighDPI     = 1u << 2,
    WF_Hidden      = 1u << 3,   // created hidden; Show() later
    WF_AlwaysOnTop = 1u << 4
};

struct WindowDesc
{
    const char* Title  = "NimbleEngine";
    int         Width  = 1280;
    int         Height = 720;
    int         X      = -1;          // -1 => centered
    int         Y      = -1;
    uint32_t    Flags  = WF_Resizable | WF_HighDPI;
    EWindowMode Mode   = WM_Windowed;

    // Which GDI backend will present here. SDL must be told at CREATE time --
    // SDL_WINDOW_OPENGL and SDL_WINDOW_VULKAN cannot be added to an existing
    // window, and getting this wrong produces a window that silently refuses a
    // surface later, nowhere near the call that was actually wrong.
    ERenderPlatformHint Backend = RPH_None;   // RPH_None | RPH_OpenGL | RPH_Vulkan | RPH_Metal
};
```

`ERenderPlatformHint` is a **deliberate duplicate** of GDI's `ERenderPlatform`,
not an include of `GDIDefinitions.h`. Including GDI's header here would make
GameWindow depend on the whole GDI target for one enum, and drag `vulkan.h`
into every host translation unit. The host maps between the two in one line;
the smoke test asserts the mapping stays correct.

### `GameWindowInput.h`

Engine-owned enums, **not** SDL's -- so no host translation unit includes
`SDL.h`, and an SDL3 -> SDL4 bump is one translation table rather than a
sweep of the codebase.

```cpp
enum EKeyCode     : uint16_t { KC_Unknown = 0, KC_A, /* ... */ KC_F1, KC_Escape, KC_MAX };
enum EMouseButton : uint8_t  { MB_Left = 0, MB_Middle, MB_Right, MB_X1, MB_X2, MB_MAX };
enum EModifierKeys: uint16_t { MOD_None = 0, MOD_Shift = 1<<0, MOD_Ctrl = 1<<1,
                               MOD_Alt = 1<<2, MOD_Super = 1<<3 };

struct KeyEvent         { EKeyCode Key; uint16_t Modifiers; bool bPressed; bool bRepeat; };
struct MouseButtonEvent { EMouseButton Button; EngineCore::Point Pos; uint8_t ClickCount;
                          bool bPressed; uint16_t Modifiers; };
struct MouseMoveEvent   { EngineCore::Point Pos; int DeltaX; int DeltaY; uint16_t Modifiers; };
struct MouseWheelEvent  { float DeltaX; float DeltaY; EngineCore::Point Pos; uint16_t Modifiers; };
```

All trivially copyable POD -- the same constraint `ModuleReloadState` imposes
on its blob -- so a hot-reloadable module can queue one without a cross-image
lifetime question.

`Pos` is in **physical pixels**, matching `GDIPlatform::appGetWindowPixelSize`'s
contract. A mouse position in logical pixels tested against a swapchain sized
in physical pixels is the picking bug that costs an afternoon on a scaled
display and reproduces on nobody else's machine.

### `GameWindowDelegates.h` -- the callback table

```cpp
struct GameWindowDelegates
{
    void* UserData = nullptr;

    // --- lifecycle / exit -------------------------------------------------
    // OnCloseRequested is the X button and Alt-F4. It is a REQUEST: return
    // false to veto (an unsaved-work prompt), true to proceed. On true -- or
    // with the slot left null -- the window is marked and destroyed by the reap
    // at the end of that PumpEvents, never inside the call itself.
    bool (*OnCloseRequested)(GameWindow*, void*) = nullptr;

    // Process-level quit (SDL_EVENT_QUIT: last window gone, OS logoff, SIGINT).
    // Not per-window, so it carries no GameWindow*, and it fires ONCE, on the
    // primary window's table -- firing it per window would call a host's single
    // quit handler N times.
    void (*OnQuit)(void*) = nullptr;

    // --- window state -----------------------------------------------------
    void (*OnResized)(GameWindow*, const EngineCore::Size& InPixelSize, void*) = nullptr;
    void (*OnMoved)(GameWindow*, const EngineCore::Point&, void*)              = nullptr;
    void (*OnMinimized)(GameWindow*, void*)                                    = nullptr;
    void (*OnMaximized)(GameWindow*, void*)                                    = nullptr;
    void (*OnRestored)(GameWindow*, void*)                                     = nullptr;
    void (*OnFocusChanged)(GameWindow*, bool bInHasFocus, void*)               = nullptr;
    void (*OnMouseFocusChanged)(GameWindow*, bool bInHasMouse, void*)          = nullptr;
    void (*OnWindowModeChanged)(GameWindow*, EWindowMode, void*)               = nullptr;
    void (*OnDisplayScaleChanged)(GameWindow*, float InScale, void*)           = nullptr;

    // --- input ------------------------------------------------------------
    void (*OnKey)(GameWindow*, const KeyEvent&, void*)                 = nullptr;
    void (*OnText)(GameWindow*, const char* InUtf8, void*)             = nullptr;  // valid for the call only
    void (*OnMouseButton)(GameWindow*, const MouseButtonEvent&, void*) = nullptr;
    void (*OnMouseMove)(GameWindow*, const MouseMoveEvent&, void*)     = nullptr;
    void (*OnMouseWheel)(GameWindow*, const MouseWheelEvent&, void*)   = nullptr;
};
```

**Why raw function pointers and not `std::function`.** A `std::function`
constructed in one image and destroyed in another is the cross-heap free that
`CoreConfig.h` and `ModuleReloadState` are both explicitly designed around: it
looks fine under `/MD` and becomes silent corruption the moment somebody builds
with `/MT`. A `void (*)(...)` plus a `void*` has no owner and no allocation, so
the question does not arise. It is also the shape `CoreLog.h`'s `LogHandler`
already uses, so this seam reads like the one next to it.

**Why not a virtual `IWindowListener`.** A listener implemented inside a hot
module leaves a dangling vtable in the window after unload -- the first hazard
`CoreModule.h` documents, and the crash lands in the *host's* pump, nowhere
near the module.

Every slot is nullable and null-checked at the dispatch site; a host binds
three of them and ignores the rest. There is one `UserData` for the whole table
rather than one per slot: a host needing different contexts per callback wants
a dispatcher object, and per-slot `void*` doubles the struct for a case nobody
has.

`OnCloseRequested` returning `bool` is the one asymmetry in the table, and that
is the reason for it -- close is the only event where the host's answer changes
what the window does next.

### `GameWindow.h`

```cpp
class GAMEWINDOW_API GameWindow
{
public:
    ~GameWindow();
    GameWindow(const GameWindow&)            = delete;   // owns an SDL_Window
    GameWindow& operator=(const GameWindow&) = delete;

    // --- identity ---------------------------------------------------------
    uint32_t GetID() const;                  // SDL window id; the dispatch key

    // The NATIVE OS handle -- HWND on Windows, NSView* on macOS -- and NOT the
    // SDL_Window*. GDIPlatformWindows.cpp reinterpret_casts WindowHandle.Native
    // straight to HWND (see ToHWND there), so handing it an SDL_Window* is a
    // crash inside appGLCreateContext with nothing pointing at the cause.
    // CoreTypes.h's WindowHandle comment lists "an SDL_Window* under SDL" as an
    // option; that option is deliberately NOT taken, and this is the note that
    // says so.
    EngineCore::WindowHandle GetNativeHandle() const;

    // The SDL handle, for SDL_Vulkan_CreateSurface / SDL_GL_CreateContext
    // inside GameWindow's own platform code. Returned as void* so that no
    // public header in this module includes SDL.h.
    void* GetSDLWindow() const;

    // --- geometry ---------------------------------------------------------
    EngineCore::Size  GetPixelSize()    const;   // physical -- what a swapchain wants
    EngineCore::Size  GetLogicalSize()  const;   // what SDL calls the window size
    float             GetDisplayScale() const;
    EngineCore::Point GetPosition()     const;

    void SetTitle(const char* InTitle);
    void SetSize(int InWidth, int InHeight);     // logical
    void SetPosition(int InX, int InY);
    void Center();

    // --- state ------------------------------------------------------------
    bool IsMinimized() const;
    bool IsMaximized() const;
    bool HasFocus()    const;
    bool IsVisible()   const;

    void Show();
    void Hide();
    void Minimize();
    void Maximize();
    void Restore();
    void Raise();

    EWindowMode GetWindowMode() const;
    bool SetWindowMode(EWindowMode InMode);      // false if the mode switch was refused
    bool ToggleFullscreen();                     // Windowed <-> the last fullscreen mode

    void SetRelativeMouseMode(bool bInEnabled);  // FPS-style capture
    void SetCursorVisible(bool bInVisible);
    void SetTextInputActive(bool bInActive);     // gates OnText and the IME

    // --- callbacks --------------------------------------------------------
    void SetDelegates(const GameWindowDelegates& InDelegates);
    const GameWindowDelegates& GetDelegates() const;
    void ClearDelegates();                       // see RELOAD HAZARDS below

    // --- exit -------------------------------------------------------------
    // Marks the window for destruction. It is NOT destroyed here: destroying
    // it inside a callback would free the object the dispatch loop is standing
    // on. GameWindowSystem::PumpEvents reaps marked windows after the event
    // queue drains.
    void RequestClose();
    bool IsCloseRequested() const;

private:
    friend class GameWindowSystem_Impl;          // only the system constructs one
    GameWindow();
    struct Impl;                                 // SDL_Window*, cached state, delegates
    Impl* P;
};
```

**Cached state, not live SDL queries.** `IsMinimized()`, `HasFocus()` and
`GetPixelSize()` read members updated by the event dispatch, not
`SDL_GetWindowFlags`. A renderer asks for the size every frame, and a syscall
per frame per query is the thing that shows up in a profile six months later.
The cached value is authoritative anyway: the event is what the host reacted
to.

### `GameWindowSystem.h`

```cpp
namespace GameWindowSystem
{
    // SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS). Idempotent and refcounted.
    // Fails loudly through CORE_LOG_ERROR with SDL_GetError() attached.
    bool Initialize();
    void Shutdown();                 // destroys every live window first
    bool IsInitialized();

    GameWindow* CreateGameWindow(const WindowDesc& InDesc);   // null on failure
    void        DestroyGameWindow(GameWindow* InWindow);

    // Drains SDL's queue, routes each event to its window's delegates, then
    // reaps the windows marked by RequestClose(). Call once per frame, on the
    // thread that called Initialize().
    //
    // Returns false when the application should exit: OnQuit fired, or the
    // last window closed. That return value IS the game loop's condition --
    // there is no separate ShouldQuit() to forget to check.
    bool PumpEvents();

    // Escape hatch for a host that wants to block rather than spin (a tool
    // window, a paused editor). A zero timeout is exactly PumpEvents.
    bool WaitEvents(int InTimeoutMilliseconds);

    GameWindow* FindWindow(uint32_t InWindowID);
    GameWindow* GetPrimaryWindow();      // the first one created; null if none
    size_t      GetWindowCount();
}
```

Multi-window support falls out of routing by SDL window ID rather than being
designed for. The registry is a `std::vector<GameWindow*>` scanned linearly,
which is the same call `CoreConfig.h` makes about its intrusive list: linear is
correct at N ~ 1, and the scan happens per event, never per pixel.

### Hosted windows (an editor viewport)

A host with its own UI toolkit and event loop -- the Qt editor -- can put a
window it owns under GameWindow, so a game running inside it receives input
through exactly the `GameWindowDelegates` it gets standalone:

```cpp
GameWindowSystem::SetHostOwnsMessageLoop(true);   // before Initialize()
GameWindowSystem::Initialize();
GameWindow* View = GameWindowSystem::WrapNativeWindow(ViewportHandle);
// ... every frame: GameWindowSystem::PumpEvents();
// ... characters the toolkit swallowed: GameWindowSystem::InjectTextInput(View, Utf8);
GameWindowSystem::DestroyGameWindow(View);         // before the host destroys the window
```

* **`SetHostOwnsMessageLoop`** sets `SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP=0`:
  the host's loop dispatches the platform messages, and `PumpEvents` only
  drains what SDL has queued. Two loops on one thread would steal each other's
  messages.
* **`WrapNativeWindow`** adopts the host's window
  (`SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER`). SDL chains its window
  procedure in front of the host's, so both see every message; destroying the
  GameWindow restores the host's procedure and leaves the window alive.
  Windows only for now.
* **Keyboard focus** follows the Win32 focus for a wrapped child window, via
  the SDL patch below. The host must put the Win32 focus on the window -- Qt,
  for one, keeps it on the top-level window unless told otherwise.
* **`InjectTextInput`** exists because Qt removes each `WM_CHAR` from the queue
  while translating the `WM_KEYDOWN` before it and never dispatches it
  (measured: every `WM_CHAR` fetched, none delivered), so SDL makes no text.
  The host forwards the characters it saw; they arrive through `OnText` like
  any other. GameWindow keeps each string alive until the pump that delivers
  it, since SDL owns only the text of events it created.

Measured end to end with the editor's SDL input probe (View > SDL Input
Probe, `--sdl-input-probe`): mouse enter/leave, motion, buttons, wheel and drag
capture, keys with modifiers, text, and focus gain/loss all arrive, while Qt
still handles the same input for the editor.

---

## Gamepads and mouse capture

**Gamepads** use SDL's gamepad API, which maps any supported controller onto
the standard layout (`GameWindowGamepad.h`). A pad belongs to a player, not
a window, so its events go to one process-level table,
`GameWindowSystem::SetGamepadDelegates`, which the engine's `InputRouter`
takes.

- **Player slots.** A connecting pad gets the lowest free player slot (up to
  four) and keeps it until it disconnects.
- **Values.** Sticks report -1..1 and triggers 0..1.
- **Startup.** The subsystem starts with SDL. A failure is a warning, and
  windows still work.
- **Focus.** SDL drops a pad's button presses while no window of the app has
  focus (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` changes that).

**Mouse capture**, `GameWindow::SetRelativeMouseMode`, is SDL's relative
mouse mode. The cursor is hidden and held in the window, and motion keeps
arriving as deltas, for mouse-look.

## Threading contract

SDL requires window creation and event pumping on the **same thread**, and on
macOS that thread must be the main thread.

`Initialize()` records `EngineCore::appThreadID()`; every public entry point
asserts against it with `CORE_CHECK`. The consequence worth stating up front:
**delegates are invoked on the main thread, inside `PumpEvents`.** A callback
that blocks stalls the window. Anything expensive goes onto an
`EngineCore::ThreadSafeQueue` -- already in Core, already used by GDI's
`GRenderCommands` -- and is drained by whoever owns the work.

---

## RELOAD HAZARDS

`CoreModule.h`'s **DANGLING CALLBACKS** hazard applies to this module verbatim,
and more sharply than it does to the log seam: a window outlives a hot module
far more naturally than a log handler does.

> A module that calls `Window->SetDelegates(...)` in `OnStartup` with function
> pointers into its own image leaves the window holding pointers into unmapped
> memory after unload. The next mouse move jumps into nothing, and the crash
> lands in the **host's** `PumpEvents`, nowhere near the module.
>
> **Rule: whatever `OnStartup` binds, `OnShutdown` clears with
> `ClearDelegates()`.**

`GameWindowSystem::Shutdown()` walks the live windows and emits a
`CORE_LOG_WARN` for any that still hold a non-null delegate table at teardown
-- the same "the manager cannot enforce this, but it does detect and warn"
posture `CoreModuleManager` takes for `GLogHandler` and `GFileProvider`.

**Second hazard, specific to this module: do not destroy a window from inside
its own callback.** `RequestClose()` plus the deferred reap in `PumpEvents` is
the supported path, and the reason is written beside the declaration.

---

## Config vars

Four `NIMBLE_CONFIG_*` declarations at file scope in `GameWindowSystem.cpp`,
self-registering the way `CoreConfig.h` intends:

```cpp
NIMBLE_CONFIG_INT   (GWindowDefaultWidth,    "Window", "Width",       1280, nullptr);
NIMBLE_CONFIG_INT   (GWindowDefaultHeight,   "Window", "Height",       720, nullptr);
NIMBLE_CONFIG_BOOL  (GWindowStartFullscreen, "Window", "Fullscreen",  false, nullptr);
NIMBLE_CONFIG_STRING(GWindowDefaultTitle,    "Window", "Title", "NimbleEngine", nullptr);
```

These supply the values `WindowDesc` leaves at its own defaults. They do **not**
override an explicit `WindowDesc` -- a host that asked for 800x600 meant it.

---

## SDL

SDL3 3.4.16 is vendored in source form, with the engine's other third-party
libraries, at:

```
Engine/ThirdParty/Source/SDL-release-3.4.16/
```

There is no `find_package(SDL3)`, no `FetchContent` and no fallback to a system
SDL: the engine builds SDL **from this source tree**, so a fresh clone
configures with no per-machine setup and every platform gets the same SDL. That
is the portability property, and it is why the tree is committed rather than
downloaded.

**Who builds it.** `Engine/ThirdParty/CMakeLists.txt`, which adds SDL's own
CMakeLists with its options forced into the cache first (never by editing SDL's
files): no test library, tests, examples or install rules, and `SDL_RENDER` off
-- GDI is the renderer, and nothing calls `SDL_Render*`. By default that project
is built once per toolchain into `Engine/Intermediate/ThirdParty/<toolchain>/`
and every build tree links the result through an IMPORTED `SDL3::SDL3`
(`Engine/cmake/NimbleThirdParty.cmake`); `-DNIMBLE_PREBUILT_THIRDPARTY=OFF`
builds it inside the tree instead, for stepping through SDL.

**SDL3.dll placement.** The runtime directory (`Binaries/<Platform>_<Config>/`)
needs SDL beside `EngineCore.dll`. Built in the tree, SDL inherits the root's
runtime output directories and lands there itself. Prebuilt, it lives in
`Engine/Intermediate`, so `nimble_stage_thirdparty_runtime(GameWindow)` copies
it in after GameWindow links (on Linux, as its soname, `libSDL3.so.0`).

**What is committed.** All of it except the examples and the test suite -- 475
files and ~17 MB that this engine never builds. Both are gated in SDL's own
CMakeLists and both gates are forced OFF: `add_subdirectory(examples)` runs
only under `if(SDL_EXAMPLES)`, `add_subdirectory(test)` only under
`if(SDL_TESTS)`. The excluded paths are `examples/`, `test/`, and the
`VisualC/` directories holding the `.vcxproj` files that build them
(`Engine/ThirdParty/.gitignore`).

Two things that are **not** excluded despite the names, because they are not
what they look like:

* `src/test/` is where the `SDL3_test` library's sources actually live -- SDL
  globs `src/test/*.c` for it, nothing from the top-level `test/`. It stays.
* `Xcode/SDLTest/` is a mixed project covering both that library and the test
  programs, so dropping it would take a project for code we still vendor.
  `VisualC/SDL_test/`, the library-only equivalent, stays for the same reason.

Everything else is committed **faithfully**, including the 14 files SDL's own
`.gitignore` would have dropped -- the `pkg-support` `*Config.cmake` files and
the `.hlsl` shader sources under `src/render/gpu/shaders/`. A vendored
dependency should be a byte-faithful copy of the release: filtering it through
the upstream project's development-time ignore rules produces a tree that
silently differs from what was extracted, and makes the next version bump diff
against phantom changes. (The build itself does not need those 14 -- the
shaders it compiles are the checked-in `.dxil.h` / `.spv.h` / `.msl.h` outputs
-- but `build-shaders.sh` does.)

**Bumping SDL**: drop the new tree in beside the old one under
`Engine/ThirdParty/Source/`, change `NIMBLE_THIRDPARTY_SDL_DIR` in
`Engine/ThirdParty/cmake/NimbleThirdPartyUsage.cmake` and the paths in
`Engine/ThirdParty/.gitignore`, and re-apply the local patches below. The
version is spelled out rather than globbed so that which SDL a given commit
builds against is answerable from the source, not from whatever is on disk.

### Local patches to SDL

The tree is a faithful copy of the release **except** for the patches listed
here. Each is marked in the source with `NimbleEngine local patch` and must be
re-applied (or confirmed fixed upstream) when SDL is bumped:

| File | Function | What and why |
|---|---|---|
| `src/video/windows/SDL_windowsevents.c` | `WIN_UpdateFocus` | For an adopted **child** window (`SDL_WINDOW_EXTERNAL` + `WS_CHILD`), keyboard focus is the Win32 focus (`GetFocus() == hwnd`), not "is the foreground window". A child is never the foreground window, so without this a wrapped editor viewport never gets SDL keyboard focus: its key events carry window 0 and its text is dropped. `SetupWindowData` already applies the same `GetFocus()` test when the window is adopted; this makes later focus changes agree with it. A candidate to upstream. |

`grep -rn "NimbleEngine local patch" Engine/ThirdParty/Source/SDL-release-3.4.16` lists them.

---

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(GameWindow VERSION 0.1 LANGUAGES CXX)

# Deliberately NOT standalone-configurable, unlike Core and GDI. Those are
# subtrees of standalone repositories and must keep `cmake -S Modules/Core`
# working. GameWindow lives in this repository, so it is free to use the
# engine-level nimble_add_module() -- which is exactly what that helper was
# written for and has had no consumer for until now.

option(GAMEWINDOW_BUILD_SMOKETEST "Build GameWindowSmokeTest"            OFF)

# === SDL3 ==================================================================
# Vendored at Engine/ThirdParty/Source and built by Engine/ThirdParty --
# prebuilt once per toolchain, or in this tree (NIMBLE_PREBUILT_THIRDPARTY).
# SDL3::SDL3 is the shared library either way.

# === Sources ===============================================================
set(GAMEWINDOW_SOURCES
        Source/Private/GameWindow/GameWindow.cpp
        Source/Private/GameWindow/GameWindowSystem.cpp
        Source/Private/GameWindow/GameWindowDispatch.cpp
        Source/Private/GameWindow/GameWindowInputSDL.cpp
        Source/Public/GameWindow/GameWindowAPI.h
        Source/Public/GameWindow/GameWindowLog.h
        Source/Public/GameWindow/GameWindowTypes.h
        Source/Public/GameWindow/GameWindowInput.h
        Source/Public/GameWindow/GameWindowDelegates.h
        Source/Public/GameWindow/GameWindow.h
        Source/Public/GameWindow/GameWindowSystem.h
)

# Exactly one native-handle implementation is compiled, so a port adds one file
# here and changes nothing else -- the arrangement GDI's platform layer uses.
if(WIN32)
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeWindows.cpp)
elseif(APPLE)
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeMac.mm)
else()
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeLinux.cpp)
endif()

# === The target ============================================================
# SDL3::SDL3 is PRIVATE, and that is load-bearing: no public GameWindow header
# includes SDL.h, which is why GetSDLWindow() returns void* and the input enums
# are engine-owned rather than SDL's. A host links GameWindow and never sees
# SDL at all. If this ever has to become PUBLIC, something leaked.
#
# Always SHARED: GameWindowSystem is process-global (the SDL video subsystem,
# the window registry) and both NimbleEngine.dll and the editor reach it, so it
# has to live in exactly one image. See the CMakeLists for the full argument.
nimble_add_module(GameWindow SHARED
        PREFIX       GAMEWINDOW
        SOURCES      ${GAMEWINDOW_SOURCES}
        PRIVATE_DEPS SDL3::SDL3)

# A prebuilt SDL3 is copied in beside GameWindow.dll after it links.
nimble_stage_thirdparty_runtime(GameWindow)

if(GAMEWINDOW_BUILD_SMOKETEST)
    add_executable(GameWindowSmokeTest Tests/SmokeMain.cpp)
    target_link_libraries(GameWindowSmokeTest PRIVATE GameWindow::GameWindow)
    set_target_properties(GameWindowSmokeTest PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME GameWindow.Smoke COMMAND GameWindowSmokeTest)
endif()

source_group("GameWindow" FILES ${GAMEWINDOW_SOURCES})
```

Then one line in `Engine/CMakeLists.txt`, guarded the way GDI already is:

```cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GameWindow/CMakeLists.txt")
    add_subdirectory(Modules/GameWindow)
endif()
```

---

## Smoke test

`Tests/SmokeMain.cpp`, written in `Modules/Core/Tests/SmokeMain.cpp`'s style --
an `Expect(cond, "what")` counter and a nonzero exit status, which is exactly
what `add_test()` checks.

Its **structural** job, the one `EngineCoreSmokeTest` and `GDISmokeTest` each
have: if this target ever fails to link, SDL has leaked into a public header.

Its behavioural cases, all runnable headless in CI under
`SDL_VIDEODRIVER=dummy`:

* `Initialize` / `Shutdown` is idempotent and correctly refcounted.
* `CreateGameWindow` with `WF_Hidden` succeeds; `GetID()` is nonzero;
  `FindWindow(GetID())` round-trips to the same pointer.
* A synthesized `SDL_EVENT_WINDOW_MINIMIZED` pushed with `SDL_PushEvent` fires
  `OnMinimized` exactly once and flips `IsMinimized()`.
* `ClearDelegates()`, then the same synthesized event: nothing fires and
  nothing crashes. This is the reload hazard above, as a test.
* `RequestClose()` called from inside a callback does not destroy during
  dispatch; the window is gone once `PumpEvents` returns.
* The scancode table has no duplicate `EKeyCode` mappings and no `KC_MAX`
  overrun.
* `ERenderPlatformHint` still maps onto GDI's `ERenderPlatform` values.

---

## Build order

1. **Skeleton and CMake.** `GameWindowAPI.h`, `GameWindowLog.h`, the empty
   target, the vendored SDL `add_subdirectory`, the root `add_subdirectory`.
   Acceptance: it configures, SDL builds, the module links.
2. **Window lifetime.** `WindowDesc`, `CreateGameWindow` / `DestroyGameWindow`,
   `GetNativeHandle` on Windows. Acceptance: a window appears and closes
   cleanly.
3. **The pump and the window-state callbacks.** `PumpEvents`, dispatch by
   window ID, the deferred reap. Close and quit, minimize, maximize, restore,
   focus, resize, move. **This is the milestone the module was asked for;
   everything after it is additive.**
4. **Input callbacks.** Keyboard including modifiers and repeat, text input,
   mouse buttons, motion and wheel, and the SDL translation tables.
5. **Fullscreen and display.** `SetWindowMode`, `ToggleFullscreen`,
   `OnDisplayScaleChanged`, relative-mouse mode.
6. **GDI handoff.** `WF_*` and `Backend` mapped onto `SDL_WINDOW_VULKAN` /
   `SDL_WINDOW_OPENGL` at create time, and a host snippet:
   `InitializeGDI(ERP_Vulkan, Window->GetNativeHandle())`.
7. **Config vars, the smoke test, and the macOS / Linux native-handle files.**

Steps 1-3 are the useful stopping point if you want a window on screen before
committing to the rest.

---

## Open question, deferred to step 6

`SDL_Vulkan_CreateSurface` and `SDL_GL_CreateContext` overlap with GDI's
existing `GDIPlatform` seam. There are two defensible answers:

* **GameWindow supplies only the HWND** (what this plan assumes). GDI's
  `GDIPlatformWindows.cpp` stays authoritative, and nothing inside the GDI
  subtree changes -- which matters, because every local edit to a subtree is a
  merge conflict on the next pull.
* **GameWindow exposes surface and context creation**, SDL does the work, and
  GDI's Win32 platform file becomes dead code on this path. Cleaner in the long
  run and it is what `GDIVulkanPlatform.h` anticipates, but it is a change
  inside a vendored tree and should be made upstream in the GDI repository
  rather than here.

Decide at step 6, not before.

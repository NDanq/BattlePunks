cmake_minimum_required(VERSION 3.16)

project(GameWindow VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# GameWindow -- window creation, event pumping and input delivery, on SDL3.
#
# Deliberately NOT standalone-configurable, unlike Core and GDI. Those are
# subtrees of standalone repositories and must keep `cmake -S Modules/Core`
# working on their own. GameWindow lives in this repository, so it is free to
# use the engine-level nimble_add_module() -- which is what that helper was
# written for and has had no consumer until now.
#
# See README.md for the design: the callback table, the threading contract and
# the reload hazards.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "GameWindow is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure -- see the note at "
            "the top of this file and the README.")
endif()

# GAMEWINDOW_SHARED is obsolete (always shared, below); drop it from old caches.
unset(GAMEWINDOW_SHARED CACHE)

# === SDL3 ===================================================================
# SDL 3.4.16 is vendored at Engine/ThirdParty/Source/SDL-release-3.4.16 and
# built by Engine/ThirdParty/CMakeLists.txt, which also says which of its
# options are forced and why (SDL_RENDER off: GDI is the renderer). SDL3::SDL3
# is that shared library -- prebuilt once per toolchain, or built in this tree
# with NIMBLE_PREBUILT_THIRDPARTY=OFF (Engine/cmake/NimbleThirdParty.cmake).
# No find_package and no fallback to a system SDL: every platform gets this one.

# ===========================================================================
# Sources
# ===========================================================================

set(GAMEWINDOW_SOURCES
        Source/Private/GameWindow/GameWindow.cpp
        Source/Private/GameWindow/GameWindowSystem.cpp
        Source/Private/GameWindow/GameWindowDispatch.cpp
        Source/Private/GameWindow/GameWindowInputSDL.cpp
        Source/Private/GameWindow/GameWindowKeyNames.cpp
        Source/Private/GameWindow/GameWindowGamepad.cpp
        Source/Private/GameWindow/GameWindowInternal.h
        Source/Public/GameWindow/GameWindowAPI.h
        Source/Public/GameWindow/GameWindowLog.h
        Source/Public/GameWindow/GameWindowTypes.h
        Source/Public/GameWindow/GameWindowInput.h
        Source/Public/GameWindow/GameWindowGamepad.h
        Source/Public/GameWindow/GameWindowDelegates.h
        Source/Public/GameWindow/GameWindow.h
        Source/Public/GameWindow/GameWindowSystem.h
)

# === Platform layer ========================================================
# Exactly one implementation compiles, so a port adds one file here and changes
# nothing else -- the arrangement GDI's Source/Private/GDI/Platform/ uses.
#
# Linux is X11-only (XWayland on a Wayland desktop); GameWindowNativeLinux.cpp
# says why; GameWindowNativeMac.cpp hands GDI a Metal-backed NSView. Failing the configure is the honest
# response until it exists: the alternative is a module that builds and hands
# GDI a null window handle.
if(WIN32)
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeWindows.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeLinux.cpp)
elseif(APPLE)
    # An NSView* backed by a CAMetalLayer, made by SDL_Metal_CreateView.
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeMac.cpp)
else()
    message(FATAL_ERROR
            "GameWindow has no native-handle implementation for this platform yet. "
            "Only the Windows and Linux files exist under Source/Private/GameWindow/Platform/.")
endif()

# ===========================================================================
# The GameWindow target
# ===========================================================================
#
# SDL3::SDL3 is PRIVATE, and that is load-bearing rather than tidy: no public
# header in this module includes SDL.h -- which is why GetSDLWindow() returns
# void* and the input enums in GameWindowInput.h are engine-owned rather than
# SDL's. A host links GameWindow and never sees SDL. If this ever has to become
# PUBLIC, something leaked into Source/Public.
#
# GameWindow does NOT link GDI, and GDI does not link GameWindow. GDI owns its
# own window-system seam and takes an EngineCore::WindowHandle; GameWindow's job
# ends at producing one. That is what keeps SDL out of GDI's backends, exactly
# as GDIVulkanPlatform.h anticipates.
#
# ALWAYS SHARED: GameWindowSystem is process-global (SDL video, the window
# registry, the injected-text queue). The editor and NimbleEngine both use it,
# and two static copies would be two silently disjoint registries.
nimble_add_module(GameWindow SHARED
        PREFIX       GAMEWINDOW
        SOURCES      ${GAMEWINDOW_SOURCES}
        PRIVATE_DEPS SDL3::SDL3)

# A prebuilt SDL3 lives outside the runtime directory: copied in beside this DLL.
nimble_stage_thirdparty_runtime(GameWindow)

# PRIVATE, not PUBLIC: Private/GameWindow/GameWindowInternal.h is included as
# "Private/GameWindow/..." from this module's own .cpp files and must not be
# reachable from a consumer. nimble_add_module already added Source/ PUBLIC for
# the "Public/GameWindow/..." includes.
target_include_directories(GameWindow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# GameWindowSmokeTest arrives with Tests/SmokeMain.cpp at build step 7.

source_group("GameWindow" FILES ${GAMEWINDOW_SOURCES})

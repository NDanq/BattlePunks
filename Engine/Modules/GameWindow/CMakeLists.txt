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

option(GAMEWINDOW_SHARED "Build GameWindow as a shared library" OFF)

# === Vendored SDL3 =========================================================
# SDL3 lives in source form under External/, the way Modules/GDI carries
# External/wglext: the dependency sits with the one module that needs it, and
# nothing engine-wide has to know SDL exists. No find_package, no FetchContent
# -- a fresh clone configures with no per-machine setup.
#
# The options are forced into the cache BEFORE add_subdirectory so that SDL's
# tree is never edited. Same discipline the root CMakeLists applies to the Core
# and GDI subtrees, and the same FORCE reasoning it gives for CORE_SHARED: a
# stale cache from an earlier configure would otherwise silently produce a
# build nobody asked for.
#
# SDL_TEST_LIBRARY in particular defaults to ON and would build an SDL3_test
# static library this engine never links.
#
# SDL_EXAMPLES and SDL_TESTS are not merely off, they are unbuildable:
# External/.../examples and External/.../test are deliberately not vendored
# (see the module's .gitignore). SDL only descends into those directories when
# the matching option is ON, so forcing both OFF is what keeps that true.
# Turning either on against this checkout fails the configure -- correctly,
# because the directory really is absent.
#
# SDL_TEST_LIBRARY is a THIRD thing despite the name: the SDL3_test library is
# built from src/test/, which IS vendored. It is off because nothing here links
# it, not because its sources are missing.
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

set(GAMEWINDOW_SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/SDL-release-3.4.16")

if(NOT EXISTS "${GAMEWINDOW_SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "Vendored SDL3 not found at ${GAMEWINDOW_SDL_DIR}. GameWindow does not fall "
            "back to a system SDL -- see the SDL section of Modules/GameWindow/README.md.")
endif()

# SDL declares project(SDL3 LANGUAGES C), which enables C in its own scope even
# though this project is CXX-only. Nothing to do about it, but it is the reason
# a toolchain with no C compiler fails here rather than in our sources.
if(NOT TARGET SDL3::SDL3)
    add_subdirectory("${GAMEWINDOW_SDL_DIR}" SDLBuild)
endif()

# ===========================================================================
# Sources
# ===========================================================================

set(GAMEWINDOW_SOURCES
        Source/Private/GameWindow/GameWindow.cpp
        Source/Private/GameWindow/GameWindowSystem.cpp
        Source/Private/GameWindow/GameWindowDispatch.cpp
        Source/Private/GameWindow/GameWindowInternal.h
        Source/Public/GameWindow/GameWindowAPI.h
        Source/Public/GameWindow/GameWindowLog.h
        Source/Public/GameWindow/GameWindowTypes.h
        Source/Public/GameWindow/GameWindowInput.h
        Source/Public/GameWindow/GameWindowDelegates.h
        Source/Public/GameWindow/GameWindow.h
        Source/Public/GameWindow/GameWindowSystem.h
)

# === Platform layer ========================================================
# Exactly one implementation compiles, so a port adds one file here and changes
# nothing else -- the arrangement GDI's Source/Private/GDI/Platform/ uses.
#
# The Mac and Linux files arrive at build step 7. Failing the configure is the
# honest response until then: the alternative is a module that builds and hands
# GDI a null window handle.
if(WIN32)
    list(APPEND GAMEWINDOW_SOURCES Source/Private/GameWindow/Platform/GameWindowNativeWindows.cpp)
else()
    message(FATAL_ERROR
            "GameWindow has no native-handle implementation for this platform yet. "
            "Only Source/Private/GameWindow/Platform/GameWindowNativeWindows.cpp exists; "
            "the Mac and Linux files are build step 7 in the README.")
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
if(GAMEWINDOW_SHARED)
    nimble_add_module(GameWindow SHARED
            PREFIX       GAMEWINDOW
            SOURCES      ${GAMEWINDOW_SOURCES}
            PRIVATE_DEPS SDL3::SDL3)
else()
    nimble_add_module(GameWindow
            PREFIX       GAMEWINDOW
            SOURCES      ${GAMEWINDOW_SOURCES}
            PRIVATE_DEPS SDL3::SDL3)
endif()

# PRIVATE, not PUBLIC: Private/GameWindow/GameWindowInternal.h is included as
# "Private/GameWindow/..." from this module's own .cpp files and must not be
# reachable from a consumer. nimble_add_module already added Source/ PUBLIC for
# the "Public/GameWindow/..." includes.
target_include_directories(GameWindow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# GameWindowSmokeTest arrives with Tests/SmokeMain.cpp at build step 7.

source_group("GameWindow" FILES ${GAMEWINDOW_SOURCES})

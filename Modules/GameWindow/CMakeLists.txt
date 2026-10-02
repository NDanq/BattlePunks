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

# SDL_RENDER is SDL's own 2D renderer -- SDL_CreateRenderer, SDL_Texture,
# SDL_RenderPresent and the whole backend set behind them: Direct3D 9, 11 and
# 12, Vulkan, OpenGL, OpenGL ES 2, Metal, software, and several console
# backends. GDI is this engine's renderer and talks to the GPU through the
# native window handle GameWindow::GetNativeHandle() hands it, so none of that
# is reachable: there is not one SDL_Render* call anywhere outside
# External/.
#
# What SDL is used for here is windowing, events and input. Those live in
# SDL_VIDEO, which stays on -- Render merely depends on it
# (define_sdl_subsystem(Render DEPS SDL_VIDEO)), so turning Render off takes
# nothing the window needs with it.
#
# WHAT THIS ACTUALLY DOES, because it is not what the name suggests. SDL turns
# a subsystem off by defining SDL_RENDER_DISABLED in its generated build config,
# NOT by dropping sources from the target: every file under src/render is still
# handed to the compiler and still produces an object. What changes is that ~29
# of the 31 back-end translation units (Direct3D 9/11/12, Vulkan, OpenGL,
# OpenGL ES 2, Metal, software, PS2, PSP, Vita, N-Gage) compile away to about
# 1.7 KB of nothing, and SDL_render.c keeps only its public entry points.
#
# Measured on this tree: SDL object output drops from 21.9 MB to 19.4 MB, about
# 11%, and SDL3.dll shrinks with it. So it is a real saving in code built and
# shipped, and a modest one in wall-clock build time -- the file count, and
# therefore the number of compiler invocations, is unchanged.
#
# The FORCE is effective for the same reason it is on the options above:
# define_sdl_subsystem declares this through cmake_dependent_option, whose
# option() call leaves an existing cache entry alone.
set(SDL_RENDER       OFF CACHE BOOL "" FORCE)

set(GAMEWINDOW_SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/SDL-release-3.4.16")

if(NOT EXISTS "${GAMEWINDOW_SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "Vendored SDL3 not found at ${GAMEWINDOW_SDL_DIR}. GameWindow does not fall "
            "back to a system SDL -- see the SDL section of Modules/GameWindow/README.md.")
endif()

# SDL declares project(SDL3 LANGUAGES C), which enables C in its own scope even
# though this project is CXX-only. Nothing to do about it, but it is the reason
# a toolchain with no C compiler fails here rather than in our sources.
# SDL compiles with a precompiled header, and MSVC's PCH flags make a
# compilation uncacheable: sccache reports /Fp and /Yc as non-cacheable reasons
# and skips those calls entirely. Measured on this tree, that was 242 of 325
# compilations -- and those 242 ARE SDL, the one thing here big enough to be
# worth caching in the first place. Leaving the PCH on means the compiler cache
# does nothing for the target it was installed for.
#
# So when a cache is in play the PCH is turned off and the objects become
# cacheable instead. It is a straight trade of one speedup for another: a PCH
# makes a cold compile cheaper, a cache hit skips the compile altogether. The
# cache wins as soon as the same objects are built a second time anywhere on the
# machine -- another build tree, a wiped tree, a standalone Games/Sandbox
# configure -- which is the situation this repository is actually in.
#
# Left ON with no cache, where the PCH is a straight win and nothing is lost.
# Set at THIS directory scope so it reaches SDL through add_subdirectory and
# touches nothing else; no other module here uses a PCH anyway.
if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "sccache"
   OR CMAKE_C_COMPILER_LAUNCHER MATCHES "sccache")
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    message(STATUS "GameWindow: compiler cache detected -- building SDL without its PCH so its objects cache")
endif()

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
#
# ALWAYS SHARED: GameWindowSystem is process-global (SDL video, the window
# registry, the injected-text queue). The editor and NimbleEngine both use it,
# and two static copies would be two silently disjoint registries.
nimble_add_module(GameWindow SHARED
        PREFIX       GAMEWINDOW
        SOURCES      ${GAMEWINDOW_SOURCES}
        PRIVATE_DEPS SDL3::SDL3)

# PRIVATE, not PUBLIC: Private/GameWindow/GameWindowInternal.h is included as
# "Private/GameWindow/..." from this module's own .cpp files and must not be
# reachable from a consumer. nimble_add_module already added Source/ PUBLIC for
# the "Public/GameWindow/..." includes.
target_include_directories(GameWindow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# GameWindowSmokeTest arrives with Tests/SmokeMain.cpp at build step 7.

source_group("GameWindow" FILES ${GAMEWINDOW_SOURCES})

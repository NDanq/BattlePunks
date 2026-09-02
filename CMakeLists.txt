cmake_minimum_required(VERSION 3.16)

project(EngineCore VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# EngineCore -- the Qt-free, graphics-API-free foundation layer.
#
# Holds the SIMD math, the scalar/geometry types, logging, the OS seam, the
# image container and the file-provider interface. Anything that needs to know
# what a VkInstance or an HGLRC is belongs in GDI, not here: a game engine
# links EngineCore to get the math without dragging in a renderer.
#
# The directory (and its future standalone git repo) is named "Core"; the
# CMake target and the C++ namespace are "EngineCore", because a library
# literally called Core sitting next to an application called CoreClient reads
# badly on a link line.
# ===========================================================================

option(CORE_SHARED          "Build EngineCore as a shared library"      OFF)
option(CORE_WITH_SIMD       "AVX2 + SSE4.1 paths in Vector.h"            ON)
option(CORE_COMPAT_GLOBAL_ALIASES "Re-export uint/uchar/Vector*/Matrix4x4 at global scope" ON)
option(CORE_WITH_QT_BRIDGE  "Build the optional Qt interop bridge"        ON)
option(CORE_BUILD_SMOKETEST "Build EngineCoreSmokeTest"                 OFF)

if(CORE_SHARED)
    set(CORE_LIB_TYPE SHARED)
else()
    set(CORE_LIB_TYPE STATIC)
endif()

set( CORE_SOURCES
        Source/Private/Core/CoreLog.cpp
        Source/Private/Core/CoreImage.cpp
        Source/Private/Core/CoreFileProvider.cpp
        Source/Private/Core/CoreString.cpp
        Source/Public/Core/CoreAPI.h
        Source/Public/Core/CoreTypes.h
        Source/Public/Core/CoreLog.h
        Source/Public/Core/CoreImage.h
        Source/Public/Core/CoreFileProvider.h
        Source/Public/Core/CoreString.h
        Source/Public/Core/CoreQueue.h
        Source/Public/Core/CorePlatform.h
)

# === Platform layer ===
# Exactly one implementation of the app* seam is compiled. A new platform adds
# one file here and changes nothing else -- that is the whole point of the
# seam, and the reason no other file in EngineCore or GDI may call an OS API
# directly.
if(WIN32)
    set( CORE_PLATFORM_SOURCES Source/Private/Core/Platform/CorePlatformWindows.cpp )
elseif(UNIX)
    # Covers both macOS and Linux: the only divergence is where per-user data
    # lives, which is a path convention handled inside the file.
    set( CORE_PLATFORM_SOURCES Source/Private/Core/Platform/CorePlatformPosix.cpp )
else()
    message(FATAL_ERROR "EngineCore has no CorePlatform implementation for this platform. Add one under Source/Private/Core/Platform/ -- it is the only file a new platform needs.")
endif()

set( CORE_MATH_SOURCES
        Source/Public/Math/Vector.h
        Source/Public/Math/Matrix.h
)

add_library(EngineCore ${CORE_LIB_TYPE}
        ${CORE_SOURCES}
        ${CORE_PLATFORM_SOURCES}
        ${CORE_MATH_SOURCES}
)
add_library(EngineCore::EngineCore ALIAS EngineCore)

# PUBLIC so that every existing `#include "Public/Math/Vector.h"` in the
# codebase keeps resolving verbatim through the physical move. This one
# property is what makes the whole split a no-op for source files -- protect
# it. It only holds while the Public/ subdirectory names of EngineCore, GDI
# and CoreClient stay disjoint (Math/ vs GDI/ vs Canvas/), so do not introduce
# a second Public/<same-name>/ anywhere.
target_include_directories(EngineCore
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Source>
)

target_compile_features(EngineCore PUBLIC cxx_std_17)

# EngineCore is Qt-free by definition. CoreClient sets CMAKE_AUTOMOC/UIC/RCC ON
# as directory properties, and add_subdirectory hands those down to this scope,
# which would quietly make the Qt-free library depend on moc being available.
# Force them off on the target so the in-tree build and a standalone Qt-less
# configure behave identically.
set_target_properties(EngineCore PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

if(CORE_SHARED)
    target_compile_definitions(EngineCore PUBLIC  CORE_SHARED_BUILD)
    target_compile_definitions(EngineCore PRIVATE CORE_BUILDING)
    if(MSVC)
        # std:: members in exported classes; unavoidable and harmless as long as
        # a shared build uses one toolchain and one CRT across all modules.
        target_compile_options(EngineCore PRIVATE /wd4251)
    endif()
endif()

# === Global compatibility aliases ===
# ON re-exports uint/uchar and Vector2/3/4/Matrix4x4 at global scope, which is
# what keeps the ~185 existing unqualified uses across GDI and CoreClient
# compiling unchanged. A game engine embedding EngineCore turns this OFF and
# qualifies with EngineCore::.
#
# PUBLIC for the same reason WITH_SIMD is: it changes which names exist at
# global scope, so every translation unit in the build must agree.
if(NOT CORE_COMPAT_GLOBAL_ALIASES)
    target_compile_definitions(EngineCore PUBLIC CORE_NO_GLOBAL_ALIASES)
    message("EngineCore: global compatibility aliases DISABLED (EngineCore:: qualification required)")
endif()

# === SIMD ===
# Enables AVX2 + SSE4.1 intrinsics in Vector.h. This block MOVED here from
# CoreClient/CMakeLists.txt and must exist in exactly ONE place in the whole
# build: WITH_SIMD changes sizeof/alignof of Vector2/3/4 and Matrix4x4, and a
# mismatch between EngineCore, GDI and CoreClient corrupts memory with no
# compiler diagnostic at all. Both the definition and /arch:AVX2 are therefore
# PUBLIC, so every consumer inherits exactly what EngineCore was built with.
# The static_asserts at the bottom of Vector.h/Matrix.h are the backstop.
#
# The intrinsics are x86-only. On arm64 (Apple silicon) the option is ignored
# and Vector.h's scalar fallbacks are used; a NEON path is a later
# optimization, not a prerequisite.
if(CORE_WITH_SIMD AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    target_compile_definitions(EngineCore PUBLIC WITH_SIMD)
    if(MSVC)
        target_compile_options(EngineCore PUBLIC /arch:AVX2)
    else()
        target_compile_options(EngineCore PUBLIC -mavx2 -mfma)
    endif()
    message("EngineCore: building with SIMD (AVX2 + SSE4.1)")
elseif(CORE_WITH_SIMD)
    message("EngineCore: building without SIMD (${CMAKE_SYSTEM_PROCESSOR} is not an AVX2 target; using scalar fallbacks)")
endif()

# === EngineCoreQtBridge ===
# The Qt interop target. Kept separate so that EngineCore itself never links
# Qt, and so a non-Qt consumer simply does not build this directory.
if(CORE_WITH_QT_BRIDGE)
    add_subdirectory(Source/Bridge/Qt)
endif()

# === Smoke test ===
# Links EngineCore and nothing else. Its first job is structural: if this ever
# fails to link, something has dragged Qt or a graphics API into EngineCore --
# which is the regression the whole split exists to prevent. Its second job is
# to exercise the app* seam and the Image container, which were written before
# anything called them.
if(CORE_BUILD_SMOKETEST)
    add_executable(EngineCoreSmokeTest Tests/SmokeMain.cpp)
    target_link_libraries(EngineCoreSmokeTest PRIVATE EngineCore::EngineCore)
    set_target_properties(EngineCoreSmokeTest PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

    # Register with CTest so CI has one command to run rather than having to know
    # the binary's name and location. SmokeMain returns the failure count as its
    # exit status, which is exactly what add_test() checks.
    #
    # enable_testing() only takes effect from the top-level scope, so `ctest`
    # works when EngineCore is configured directly. When it is pulled in with
    # add_subdirectory, the parent project must call enable_testing() itself for
    # this test to appear in its ctest run.
    enable_testing()
    add_test(NAME EngineCore.Smoke COMMAND EngineCoreSmokeTest)
    set_tests_properties(EngineCore.Smoke PROPERTIES
        TIMEOUT 120
        LABELS  "smoke"
    )
endif()

source_group("Core"     FILES ${CORE_SOURCES})
source_group("Platform" FILES ${CORE_PLATFORM_SOURCES})
source_group("Math"     FILES ${CORE_MATH_SOURCES})

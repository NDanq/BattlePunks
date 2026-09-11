cmake_minimum_required(VERSION 3.16)

project(NimbleEngine VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# NimbleEngine -- the engine-level aggregate.
#
# Deliberately thin. Modules/Core and Modules/GDI are vendored git subtrees of
# standalone repositories and MUST keep configuring on their own; nothing here
# may require an edit inside either tree, because every local edit to a subtree
# is a merge conflict on the next pull. This file therefore only sets cache
# variables those trees already read, plus the runtime layout that no
# self-contained module can decide for itself.
#
# WHY THIS FILE EXISTS AT ALL (it did not, before hot reload)
#
#   * Hot reload requires the host executable, EngineCore and every module DLL
#     to be siblings in one directory. CMAKE_RUNTIME_OUTPUT_DIRECTORY is only
#     inherited by targets created at or below the scope that sets it -- with N
#     independent top-level projects there is no such scope.
#   * CORE_SHARED=ON has to hold for Core AND every module in ONE configure.
#     Two separate configures cannot be made to agree, and the failure mode is
#     a second GLogHandler at runtime rather than a build error.
#   * Nothing can express "the smoke test depends on the module DLLs having
#     been built" across separate CMake projects.
#   * Modules/Core/CMakeLists.txt already notes that enable_testing() only
#     takes effect from the top-level scope.
#
# Both subtrees already guard their GitVersion blocks with
# `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)`, so being pulled in
# with add_subdirectory is a case they anticipate. Nothing inside either tree
# needs editing for this file to work.
# ===========================================================================

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NimbleModule)

enable_testing()

# === Binaries/ =============================================================
# One flat runtime directory per platform+config: Binaries/<Platform>_<Config>,
# e.g. Binaries/Windows_Debug, Binaries/Mac_Release, Binaries/Linux_RelWithDebInfo.
#
# The <Config> half is the CMake configuration name verbatim. The <Platform>
# half keeps a Windows build and a future Mac/Linux build from landing their
# EngineCore.dll / libEngineCore.so in the same folder.
#
# Every module DLL ends up here for free: these variables are inherited by any
# target created at or below this scope, and add_subdirectory(Modules/*) runs
# below -- so EngineCore, GDI (when GDI_SHARED), the hot-reload modules and any
# future nimble_add_module target all write here without a per-module edit.
#
# Hot reload REQUIRES this stay ONE flat directory: a shadow-copied module DLL
# resolves EngineCore.dll through the loader's application-directory search,
# which is the directory of the EXE -- so the exe, EngineCore and every module
# have to be siblings. On Linux the same layout is what makes the modules'
# $ORIGIN/.. RPATH resolve libEngineCore.so from one level above their shadow
# directory. The <Platform>_<Config> split does not break that: it is still a
# single directory, just a differently named one.
#
# Import libs / .exp go to <dir>/Lib so .lib clutter stays out of the flat
# runtime directory.
if(WIN32)
    set(NM_PLATFORM Windows)
elseif(APPLE)
    set(NM_PLATFORM Mac)
else()
    set(NM_PLATFORM Linux)
endif()

get_property(NM_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NM_MULTI_CONFIG)
    # Visual Studio / Xcode / Ninja Multi-Config: only the per-config variables
    # matter, and CMake appends nothing to them. Setting the plain variable here
    # would just get a /Debug tacked on by the generator and scatter output.
    foreach(NM_CFG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${NM_CFG}" NM_CFG_U)
        set(NM_CFG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${NM_PLATFORM}_${NM_CFG}")
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${NM_CFG_U} "${NM_CFG_DIR}")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${NM_CFG_U} "${NM_CFG_DIR}")
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${NM_CFG_U} "${NM_CFG_DIR}/Lib")
    endforeach()
else()
    # Ninja / Makefiles: a single CMAKE_BUILD_TYPE, known now. Default it so the
    # folder is never Binaries/<Platform>_ with an empty config.
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build configuration" FORCE)
    endif()
    set(NM_CFG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${NM_PLATFORM}_${CMAKE_BUILD_TYPE}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${NM_CFG_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${NM_CFG_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${NM_CFG_DIR}/Lib")
endif()

# === EngineCore linkage ====================================================
# Hot reload is only sound with a single shared EngineCore -- one GLogHandler,
# one GFileProvider, one ModuleManager, one ModuleReloadState::Write. FORCE
# rather than a plain default because a stale cache from an earlier configure
# would otherwise silently produce a build where modules cannot work and
# nothing says so. A consumer who genuinely wants a static Core configures
# Modules/Core directly, which still works and is covered by its own tests.
set(CORE_SHARED ON CACHE BOOL "Build EngineCore as a shared library" FORCE)

add_subdirectory(Modules/Core)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GDI/CMakeLists.txt")
    add_subdirectory(Modules/GDI)
endif()

# GameWindow is not a subtree -- it is written for this repository -- but it is
# guarded the same way so a checkout without it still configures.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GameWindow/CMakeLists.txt")
    add_subdirectory(Modules/GameWindow)
endif()

# === NimbleEngine =========================================================
# The engine layer itself: the thing that owns the module set, drives the frame
# and is what a game under Games/ links. Its sources live at Engine/Source, so
# it is declared HERE rather than in a subdirectory -- nimble_add_module()
# derives the PUBLIC include directory from CMAKE_CURRENT_SOURCE_DIR/Source, and
# from an Engine/Source/CMakeLists.txt scope that would resolve to
# Engine/Source/Source.
#
# This is not a violation of the "deliberately thin" rule at the top of this
# file. That rule is about not reaching INTO the subtrees. The engine layer is
# engine-level by definition and has no other home.
#
# ORDER IS LOAD-BEARING: this must stay below the add_subdirectory(Modules/*)
# calls. nimble_add_module() links EngineCore::EngineCore eagerly, and a
# namespaced ALIAS has to already exist when target_link_libraries names it.
#
# SHARED, for the same reason CORE_SHARED is FORCEd above: a hot-reloaded plugin
# that calls back into the engine layer needs exactly ONE copy of it in the
# process. A second copy is not a link error, it is a second module registry at
# runtime with nothing to say so. The target is created at this scope, so it
# inherits CMAKE_RUNTIME_OUTPUT_DIRECTORY and lands in
# Binaries/<Platform>_<Config> beside EngineCore -- which is what the hot-reload
# sibling rule requires.
set(NIMBLEENGINE_SOURCES
        Source/Private/NimbleEngine/NimbleEngine.cpp
        Source/Public/NimbleEngine/NimbleEngine.h
        Source/Public/NimbleEngine/NimbleEngineAPI.h
)

nimble_add_module(NimbleEngine SHARED
        PREFIX  NIMBLEENGINE
        SOURCES ${NIMBLEENGINE_SOURCES})

# PRIVATE, not PUBLIC: the engine layer's headers must not leak GameWindow or
# GDI types to a game. If either of these ever has to become PUBLIC_DEPS,
# something in Source/Public/NimbleEngine/ grew an #include it should not have.
#
# if(TARGET) rather than if(EXISTS) because the module directories are already
# guarded above -- this mirrors the same "a checkout without GDI still
# configures" property without re-testing paths.
#
# Hot-reload plugins contribute NOTHING here. nimble_add_hot_module() builds
# MODULE targets that are never linked against; NimbleEngine reaches them
# through CoreModuleManager at runtime. What they will want once they exist is
# add_dependencies(NimbleEngine <plugin targets>), so that building the engine
# also rebuilds the plugins it is about to load.
if(TARGET GameWindow)
    target_link_libraries(NimbleEngine PRIVATE GameWindow::GameWindow)
endif()

if(TARGET GDI)
    target_link_libraries(NimbleEngine PRIVATE GDI::GDI)
endif()

# PRIVATE so this target's own .cpp files can say "Private/NimbleEngine/...".
# nimble_add_module already added Source/ PUBLIC for the "Public/NimbleEngine/..."
# includes a consumer uses.
target_include_directories(NimbleEngine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

source_group("NimbleEngine" FILES ${NIMBLEENGINE_SOURCES})

# === Games/ ================================================================
# Hosts that link the engine layer. Added from THIS scope, below the
# NimbleEngine target, for two reasons that are both load-bearing:
#
#   * CMAKE_RUNTIME_OUTPUT_DIRECTORY is inherited by targets created at or
#     below this scope (see the Binaries/ block above). A game declared at repo
#     root would be a SIBLING of Engine and would not inherit it -- and on
#     Windows, landing beside NimbleEngine.dll / EngineCore.dll / SDL3.dll is
#     the only way an exe resolves them.
#   * NimbleEngine::NimbleEngine is an ALIAS, and target_link_libraries can only
#     name one that already exists.
#
# The explicit binary directory is required because the source directory is
# outside this one. Guarded by EXISTS so a checkout without Games/ configures,
# matching how the module subdirectories above are handled.
option(NIMBLE_BUILD_SANDBOX "Build the Games/Sandbox host" ON)
if(NIMBLE_BUILD_SANDBOX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Games/Sandbox/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Games/Sandbox"
                     "${CMAKE_CURRENT_BINARY_DIR}/Games/Sandbox")
endif()

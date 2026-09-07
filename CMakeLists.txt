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
# One flat runtime directory, per config.
#
# Hot reload REQUIRES this layout: a shadow-copied module DLL resolves
# EngineCore.dll through the loader's application-directory search, which is
# the directory of the EXE -- so the exe, EngineCore and every module have to
# be siblings. On Linux the same layout is what makes the modules' $ORIGIN/..
# RPATH resolve libEngineCore.so from one level above their shadow directory.
#
# The per-config variables are NOT redundant: a multi-config generator (Visual
# Studio) appends /Debug to the plain variable and ignores your intent, which
# would scatter the four configurations' binaries into places that do not match
# what the plain variable says.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/Lib")
foreach(NM_CFG Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${NM_CFG}" NM_CFG_U)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${NM_CFG_U} "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${NM_CFG}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${NM_CFG_U} "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${NM_CFG}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${NM_CFG_U} "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${NM_CFG}/Lib")
endforeach()

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

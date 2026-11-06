cmake_minimum_required(VERSION 3.16)

project(NimbleThirdParty LANGUAGES C CXX)

# ===========================================================================
# NimbleThirdParty -- every vendored library the engine compiles, in one
# project: SDL3, Jolt, Lua, pugixml, ENet, miniaudio, bc7enc_rdo, xatlas,
# Recast Navigation, zstd and meshoptimizer.
# Their sources are under Source/, each a faithful copy of its upstream release
# (SDL carries one local patch; Modules/GameWindow/README.md lists it).
#
# BUILT ONCE PER TOOLCHAIN, NOT ONCE PER BUILD TREE. Third-party code is about
# two-thirds of a fresh tree's compiles, and the engine is configured into
# many trees (one per game, target and configuration). So by default
# Engine/cmake/NimbleThirdParty.cmake configures THIS directory as its own
# project, builds it into Engine/Intermediate/ThirdParty/<toolchain>/, and
# every tree on that toolchain links the result through IMPORTED targets. The
# compiler cache cannot do this job: each tree has its own cache key
# ("ONE CACHE KEY PER BUILD TREE" in Engine/CMakeLists.txt).
#
# TWO WAYS IN, ONE DESCRIPTION:
#   standalone      the prebuild above. Debug and Release only -- the engine
#                   maps RelWithDebInfo and Shipping onto Release. Writes a
#                   manifest naming each library's files for the importer.
#   add_subdirectory  NIMBLE_PREBUILT_THIRDPARTY=OFF: the libraries become
#                   ordinary targets of the engine's tree again, built with its
#                   flags -- for stepping through SDL or Jolt, or bumping one.
# What a library is to its users (includes, PUBLIC defines, link deps) is in
# cmake/NimbleThirdPartyUsage.cmake, applied to the real targets here and to
# the IMPORTED ones by the engine.
#
# Every library is STATIC and position-independent (it ends up inside a
# module's shared library), except SDL3, which is its own shared library.
# ===========================================================================

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NimbleThirdPartyUsage.cmake")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NM_TP_STANDALONE ON)
else()
    set(NM_TP_STANDALONE OFF)
endif()

# === Standalone (the prebuild) ==============================================
if(NM_TP_STANDALONE)
    if(NOT NIMBLE_THIRDPARTY_OUTPUT_DIR)
        message(FATAL_ERROR "NimbleThirdParty is configured by Engine/cmake/NimbleThirdParty.cmake, "
                            "which sets NIMBLE_THIRDPARTY_OUTPUT_DIR.")
    endif()

    # C++17, as the engine.
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # The engine's instruction-set flags, '|'-separated on the command line (a
    # ';' would split the -D argument).
    string(REPLACE "|" ";" NIMBLE_THIRDPARTY_SIMD_FLAGS "${NIMBLE_THIRDPARTY_SIMD_FLAGS}")

    get_property(NM_TP_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NM_TP_MULTI_CONFIG)
        set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)
    endif()

    # Debug info in both configurations, so a crash inside SDL or Jolt still
    # has names. /Z7 rather than /Zi puts it in the objects: no shared .pdb for
    # parallel compiles to contend over, and a static library carries its own.
    # The SDL3 DLL gets a .pdb from the linker, found through the path the DLL
    # records -- in this output directory, which stays put.
    if(MSVC)
        foreach(NM_LANG C CXX)
            string(REPLACE "/Zi" "/Z7" CMAKE_${NM_LANG}_FLAGS_DEBUG "${CMAKE_${NM_LANG}_FLAGS_DEBUG}")
            string(APPEND CMAKE_${NM_LANG}_FLAGS_RELEASE " /Z7")
        endforeach()
        string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
    else()
        foreach(NM_LANG C CXX)
            string(APPEND CMAKE_${NM_LANG}_FLAGS_RELEASE " -g")
        endforeach()
    endif()

    # <output>/<Config>/{bin,lib}: one directory per configuration, so Debug
    # and Release libraries keep their plain names side by side.
    foreach(NM_CFG Debug Release)
        string(TOUPPER "${NM_CFG}" NM_CFG_U)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${NM_CFG_U} "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/bin")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${NM_CFG_U} "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/lib")
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${NM_CFG_U} "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/lib")
    endforeach()
elseif(NOT DEFINED NIMBLE_THIRDPARTY_SIMD_FLAGS)
    message(FATAL_ERROR "NimbleThirdParty: NIMBLE_THIRDPARTY_SIMD_FLAGS is unset (Engine/cmake/NimbleThirdParty.cmake sets it).")
endif()

# nm_tp_library_properties(<target>)
#   What every compiled library here shares: position-independent (each ends up
#   inside a shared library), upstream's warnings silenced, no Qt automation.
function(nm_tp_library_properties NM_TARGET)
    if(MSVC)
        target_compile_options(${NM_TARGET} PRIVATE /W0)
    else()
        target_compile_options(${NM_TARGET} PRIVATE -w)
    endif()
    set_target_properties(${NM_TARGET} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
            FOLDER "ThirdParty")
endfunction()

# === SDL3 ===================================================================
# Windowing, events and input for Modules/GameWindow. A SHARED library (SDL3.dll
# / libSDL3.so), built by SDL's own CMakeLists.
#
# The options are forced into the cache BEFORE add_subdirectory so that SDL's
# tree is never edited. FORCE because a stale cache from an earlier configure
# would otherwise silently produce a build nobody asked for.
#
# SDL_TEST_LIBRARY defaults to ON and would build an SDL3_test static library
# this engine never links. It is built from src/test/, which IS vendored; it is
# off because nothing links it, not because its sources are missing.
#
# SDL_EXAMPLES and SDL_TESTS are not merely off, they are unbuildable:
# Source/SDL-.../examples and .../test are deliberately not vendored (see
# .gitignore). SDL only descends into them when the matching option is ON, so
# forcing both OFF is what keeps that true.
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

# SDL_RENDER is SDL's own 2D renderer -- SDL_CreateRenderer, SDL_Texture and
# the whole backend set behind them (Direct3D 9/11/12, Vulkan, OpenGL, OpenGL
# ES 2, Metal, software, consoles). GDI is this engine's renderer and talks to
# the GPU through the native window handle GameWindow hands it, so none of that
# is reachable: not one SDL_Render* call exists outside Source/.
#
# What SDL is used for is windowing, events and input, in SDL_VIDEO, which stays
# on -- Render merely depends on it.
#
# WHAT THIS ACTUALLY DOES, because it is not what the name suggests: SDL turns a
# subsystem off by defining SDL_RENDER_DISABLED in its generated build config,
# NOT by dropping sources. Every file under src/render still compiles, but ~29
# of the 31 back-end translation units compile away to almost nothing. Measured:
# SDL object output drops from 21.9 MB to 19.4 MB, and SDL3.dll shrinks with it.
#
# The FORCE is effective because define_sdl_subsystem declares this through
# cmake_dependent_option, whose option() call leaves an existing entry alone.
set(SDL_RENDER       OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${NIMBLE_THIRDPARTY_SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Vendored SDL3 not found at ${NIMBLE_THIRDPARTY_SDL_DIR}. GameWindow does not "
                        "fall back to a system SDL -- see the SDL section of Modules/GameWindow/README.md.")
endif()

# SDL compiles with a precompiled header, and MSVC's PCH flags make a compile
# uncacheable: sccache skips /Fp and /Yc calls -- 242 of SDL's ~325. Where a
# compiler cache is in play (this directory added to an engine tree that uses
# sccache), the PCH goes so the objects cache. The prebuild has no cache and
# keeps the PCH, a straight win for a cold build.
if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "sccache" OR CMAKE_C_COMPILER_LAUNCHER MATCHES "sccache")
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    message(STATUS "NimbleThirdParty: compiler cache detected -- building SDL without its PCH so its objects cache")
endif()

add_subdirectory("${NIMBLE_THIRDPARTY_SDL_DIR}" SDLBuild)
set_target_properties(SDL3-shared PROPERTIES FOLDER "ThirdParty")

# === Jolt ===================================================================
# Jolt Physics 5.6.0 (MIT), for Modules/JoltPhysics. Only the library's own
# sources are vendored: the files Jolt.cmake compiles with every option off --
# no samples, tests, viewer, GPU compute backends, shaders or object streams.
# The vendored tree IS the source list, so it is globbed.
#
# STATIC, inside JoltPhysics.dll alone -- exactly one copy of Jolt's factory and
# type registry in the process.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_JOLT_DIR}/Jolt/Jolt.h")
    message(FATAL_ERROR "NimbleThirdParty: no vendored Jolt at '${NIMBLE_THIRDPARTY_JOLT_DIR}'")
endif()
file(GLOB_RECURSE NM_TP_JOLT_SOURCES CONFIGURE_DEPENDS "${NIMBLE_THIRDPARTY_JOLT_DIR}/Jolt/*.cpp")
add_library(Jolt STATIC ${NM_TP_JOLT_SOURCES})
add_library(Jolt::Jolt ALIAS Jolt)
nimble_thirdparty_usage(Jolt Jolt BUILD)
# THE SAME SIMD AS EVERYTHING ELSE. Jolt picks its code paths from the
# compiler's predefined macros (__AVX2__ ...), and EngineCore hands /arch:AVX2
# or -mavx2 -mfma to everything that links it -- which this library does not.
target_compile_options(Jolt PRIVATE ${NIMBLE_THIRDPARTY_SIMD_FLAGS})
nm_tp_library_properties(Jolt)
source_group(TREE "${NIMBLE_THIRDPARTY_JOLT_DIR}" PREFIX "Jolt" FILES ${NM_TP_JOLT_SOURCES})

# === Lua ====================================================================
# Lua 5.4 (MIT), for GameCore's scripting. Lua ships no CMakeLists.txt -- its
# Makefile is make-specific and platform-switched by hand -- so it is built
# here from the library sources only: lua.c and luac.c are the standalone
# interpreter's and compiler's own main()s and are left out. Listed rather
# than globbed, so a source added upstream does not silently join the library
# without a review of what it is.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_LUA_DIR}/src/lua.h")
    message(FATAL_ERROR "Vendored Lua not found at ${NIMBLE_THIRDPARTY_LUA_DIR}. GameCore does not fall "
                        "back to a system Lua -- see the Lua section of Modules/GameCore/README.md.")
endif()
set(NM_TP_LUA_SOURCES)
foreach(NM_FILE lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit
                liolib llex lmathlib lmem loadlib lobject lopcodes loslib lparser lstate lstring
                lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
    list(APPEND NM_TP_LUA_SOURCES "${NIMBLE_THIRDPARTY_LUA_DIR}/src/${NM_FILE}.c")
endforeach()
add_library(Lua STATIC ${NM_TP_LUA_SOURCES})
add_library(Lua::Lua ALIAS Lua)
nimble_thirdparty_usage(Lua Lua BUILD)
# luaconf.h detects _WIN32 into LUA_USE_WINDOWS itself. LUA_USE_LINUX /
# LUA_USE_MACOSX are NOT auto-detected and must be asked for, to get
# dlopen-based require("lib") and the POSIX extras luaconf.h gates behind them.
if(UNIX AND NOT APPLE)
    target_compile_definitions(Lua PRIVATE LUA_USE_LINUX)
elseif(APPLE)
    target_compile_definitions(Lua PRIVATE LUA_USE_MACOSX)
endif()
# lprefix.h defines it for every Lua unit already; belt and braces, so nothing
# relies on a lucky include order.
if(MSVC)
    target_compile_definitions(Lua PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
nm_tp_library_properties(Lua)
source_group("Lua" FILES ${NM_TP_LUA_SOURCES})

# === pugixml ================================================================
# pugixml 1.16 (MIT). Text worlds and prefabs are XML (WorldPackage.h): every
# engine build but Shipping reads them, and Shipping links none of it.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_PUGIXML_DIR}/src/pugixml.hpp")
    message(FATAL_ERROR "Vendored pugixml not found at ${NIMBLE_THIRDPARTY_PUGIXML_DIR}.")
endif()
add_library(PugiXml STATIC "${NIMBLE_THIRDPARTY_PUGIXML_DIR}/src/pugixml.cpp")
add_library(PugiXml::PugiXml ALIAS PugiXml)
nimble_thirdparty_usage(PugiXml PugiXml BUILD)
nm_tp_library_properties(PugiXml)

# === ENet ===================================================================
# ENet 1.3.18 (MIT), for NetCore and Collab: reliable and unreliable channels
# over UDP, connection handling and fragmentation in nine C files.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_ENET_DIR}/include/enet/enet.h")
    message(FATAL_ERROR "Vendored ENet not found at ${NIMBLE_THIRDPARTY_ENET_DIR}.")
endif()
set(NM_TP_ENET_SOURCES)
# unix.c and win32.c guard themselves on _WIN32, so both are listed.
foreach(NM_FILE callbacks compress host list packet peer protocol unix win32)
    list(APPEND NM_TP_ENET_SOURCES "${NIMBLE_THIRDPARTY_ENET_DIR}/${NM_FILE}.c")
endforeach()
add_library(ENet STATIC ${NM_TP_ENET_SOURCES})
add_library(ENet::ENet ALIAS ENet)
nimble_thirdparty_usage(ENet ENet BUILD)
if(WIN32)
    target_compile_definitions(ENet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    # What ENet's own CMakeLists probes for; every supported POSIX target has them.
    target_compile_definitions(ENet PRIVATE
            HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
            HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()
nm_tp_library_properties(ENet)
source_group("ENet" FILES ${NM_TP_ENET_SOURCES})

# === miniaudio ==============================================================
# miniaudio 0.11.25 (MIT-0 or public domain), for Modules/Audio: device I/O on
# every desktop backend, decoders (WAV, FLAC, MP3; Vorbis through the
# stb_vorbis it ships in extras/), a resource manager, a node-graph mixer and a
# 3D spatializer. One header; Impl/MiniaudioImpl.c is its implementation unit.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_MINIAUDIO_DIR}/miniaudio.h")
    message(FATAL_ERROR "Vendored miniaudio not found at ${NIMBLE_THIRDPARTY_MINIAUDIO_DIR}.")
endif()
add_library(miniaudio STATIC "${CMAKE_CURRENT_SOURCE_DIR}/Impl/MiniaudioImpl.c")
add_library(miniaudio::miniaudio ALIAS miniaudio)
nimble_thirdparty_usage(miniaudio miniaudio BUILD)
nm_tp_library_properties(miniaudio)

# === bc7enc_rdo =============================================================
# bc7enc_rdo (MIT or public domain) at richgel999/bc7enc_rdo b943862, for
# RenderCore's TextureCompression.cpp: BC1-BC5 (rgbcx) and BC7 (bc7enc) blocks.
# The decoders serve a backend without block formats, so every build links
# them; only an editor build references the encoder, and a static library
# hands a link only the objects it uses. Off MSVC (where /O2 clashes with
# Debug's /RTC1) optimised even in Debug -- unoptimised, BC7 is many times
# slower.
add_library(NimbleBc7enc STATIC
        "${NIMBLE_THIRDPARTY_BC7ENC_DIR}/rgbcx.cpp"
        "${NIMBLE_THIRDPARTY_BC7ENC_DIR}/bc7decomp.cpp"
        "${NIMBLE_THIRDPARTY_BC7ENC_DIR}/bc7enc.cpp")
nimble_thirdparty_usage(Bc7enc NimbleBc7enc BUILD)
nm_tp_library_properties(NimbleBc7enc)
if(NOT MSVC)
    target_compile_options(NimbleBc7enc PRIVATE -O2)
endif()

# === xatlas =================================================================
# xatlas (MIT) at jpcy/xatlas f700c77, for the editor's lightmap UV charts
# (LightmapUVs.cpp).
add_library(NimbleXatlas STATIC "${NIMBLE_THIRDPARTY_XATLAS_DIR}/xatlas.cpp")
nimble_thirdparty_usage(Xatlas NimbleXatlas BUILD)
nm_tp_library_properties(NimbleXatlas)

# === Recast Navigation ======================================================
# Recast Navigation 1.6.0 (zlib), for Modules/AICore: Recast builds navmeshes
# from triangles, Detour queries them, DetourCrowd steers agents over them,
# DetourTileCache adds obstacles at runtime, DebugUtils draws them. Only the
# five libraries are vendored -- no RecastDemo, tests or docs -- and the tree
# is the source list, so it is globbed.
#
# One STATIC library, inside AICore.dll alone; nothing else includes it
# (the editor draws navmeshes through AICore's NavDebugDraw.h).
if(NOT EXISTS "${NIMBLE_THIRDPARTY_RECAST_DIR}/Detour/Include/DetourNavMesh.h")
    message(FATAL_ERROR "Vendored Recast Navigation not found at ${NIMBLE_THIRDPARTY_RECAST_DIR}.")
endif()
file(GLOB NM_TP_RECAST_SOURCES CONFIGURE_DEPENDS
        "${NIMBLE_THIRDPARTY_RECAST_DIR}/Recast/Source/*.cpp"
        "${NIMBLE_THIRDPARTY_RECAST_DIR}/Detour/Source/*.cpp"
        "${NIMBLE_THIRDPARTY_RECAST_DIR}/DetourCrowd/Source/*.cpp"
        "${NIMBLE_THIRDPARTY_RECAST_DIR}/DetourTileCache/Source/*.cpp"
        "${NIMBLE_THIRDPARTY_RECAST_DIR}/DebugUtils/Source/*.cpp")
add_library(RecastNav STATIC ${NM_TP_RECAST_SOURCES})
add_library(RecastNav::RecastNav ALIAS RecastNav)
nimble_thirdparty_usage(RecastNav RecastNav BUILD)
# Asserts in Debug only. Shipping links the Release build, so it has none.
target_compile_definitions(RecastNav PRIVATE $<$<NOT:$<CONFIG:Debug>>:RC_DISABLE_ASSERTS>)
nm_tp_library_properties(RecastNav)
source_group(TREE "${NIMBLE_THIRDPARTY_RECAST_DIR}" PREFIX "RecastNav" FILES ${NM_TP_RECAST_SOURCES})

# === zstd ===================================================================
# Zstandard 1.5.7 (BSD), for EngineCore: a content pack's entries are
# compressed with it (CorePack.h). Only lib/common, lib/compress and
# lib/decompress are vendored -- no CLI, dictionary builder or legacy formats
# -- and those folders are the source list, so they are globbed.
#
# Single-threaded (no ZSTD_MULTITHREAD): the pack writer runs a compressor per
# thread itself. ZSTD_DISABLE_ASM keeps the build C-only; the assembly is only
# a faster Huffman decoder for GCC and Clang on x86-64.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_ZSTD_DIR}/lib/zstd.h")
    message(FATAL_ERROR "Vendored zstd not found at ${NIMBLE_THIRDPARTY_ZSTD_DIR}.")
endif()
file(GLOB NM_TP_ZSTD_SOURCES CONFIGURE_DEPENDS
        "${NIMBLE_THIRDPARTY_ZSTD_DIR}/lib/common/*.c"
        "${NIMBLE_THIRDPARTY_ZSTD_DIR}/lib/compress/*.c"
        "${NIMBLE_THIRDPARTY_ZSTD_DIR}/lib/decompress/*.c")
add_library(Zstd STATIC ${NM_TP_ZSTD_SOURCES})
add_library(Zstd::Zstd ALIAS Zstd)
nimble_thirdparty_usage(Zstd Zstd BUILD)
target_compile_definitions(Zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0 ZSTD_TRACE=0)
nm_tp_library_properties(Zstd)
source_group(TREE "${NIMBLE_THIRDPARTY_ZSTD_DIR}/lib" PREFIX "Zstd" FILES ${NM_TP_ZSTD_SOURCES})

# === meshoptimizer ==========================================================
# meshoptimizer 0.25 (MIT) at zeux/meshoptimizer v0.25, for RenderCore's
# MeshletBuild.cpp: meshlets and their culling bounds. Only src/ is vendored,
# and it is the source list, so it is globbed.
if(NOT EXISTS "${NIMBLE_THIRDPARTY_MESHOPT_DIR}/src/meshoptimizer.h")
    message(FATAL_ERROR "Vendored meshoptimizer not found at ${NIMBLE_THIRDPARTY_MESHOPT_DIR}.")
endif()
file(GLOB NM_TP_MESHOPT_SOURCES CONFIGURE_DEPENDS "${NIMBLE_THIRDPARTY_MESHOPT_DIR}/src/*.cpp")
add_library(NimbleMeshopt STATIC ${NM_TP_MESHOPT_SOURCES})
nimble_thirdparty_usage(Meshopt NimbleMeshopt BUILD)
nm_tp_library_properties(NimbleMeshopt)

# === Manifest (standalone) ==================================================
# Per configuration, the files the engine's IMPORTED targets point at:
# <output>/<Config>/NimbleThirdPartyManifest.cmake. Written at generate time
# from the targets themselves, so the importer never guesses a name or a
# directory (SDL's in particular).
if(NM_TP_STANDALONE)
    set(NM_TP_MANIFEST "# Written by Engine/ThirdParty/CMakeLists.txt. Read by Engine/cmake/NimbleThirdParty.cmake.\n")
    # Keyed by the names in NIMBLE_THIRDPARTY_LIBRARIES; SDL3's real target is SDL3-shared.
    foreach(NM_PAIR SDL3:SDL3-shared Jolt:Jolt Lua:Lua PugiXml:PugiXml ENet:ENet miniaudio:miniaudio
                    Bc7enc:NimbleBc7enc Xatlas:NimbleXatlas
                    RecastNav:RecastNav Zstd:Zstd Meshopt:NimbleMeshopt)
        string(REPLACE ":" ";" NM_PAIR "${NM_PAIR}")
        list(GET NM_PAIR 0 NM_NAME)
        list(GET NM_PAIR 1 NM_TARGET)
        string(APPEND NM_TP_MANIFEST "set(NM_TP_FILE_${NM_NAME} \"$<TARGET_FILE:${NM_TARGET}>\")\n")
    endforeach()
    if(WIN32)
        string(APPEND NM_TP_MANIFEST "set(NM_TP_IMPLIB_SDL3 \"$<TARGET_LINKER_FILE:SDL3-shared>\")\n")
    else()
        string(APPEND NM_TP_MANIFEST "set(NM_TP_SONAME_SDL3 \"$<TARGET_SONAME_FILE_NAME:SDL3-shared>\")\n")
    endif()
    file(GENERATE OUTPUT "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/$<CONFIG>/NimbleThirdPartyManifest.cmake"
                  CONTENT "${NM_TP_MANIFEST}")
endif()

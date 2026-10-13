cmake_minimum_required(VERSION 3.16)

# Configured on its own: Clang on Linux, as a game's CMakeLists picks (see "Compiler" below).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_C_COMPILER   clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

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
include(NimbleMaterials)
include(NimbleProject)
include(NimblePack)

enable_testing()

# === C++ standard ===========================================================
# C++17, exactly. Every target already asks for cxx_std_17, but that is a
# minimum: a compiler whose default is newer (GCC 16 defaults to gnu++20)
# satisfies it with no -std flag at all, and C++20 code then compiles on that
# machine and fails on MSVC, which defaults to C++14 and is given /std:c++17.
# Setting the standard makes CMake pass the flag on every compiler. Extensions
# off, so it is -std=c++17 rather than -std=gnu++17. Directory-scoped, so it
# reaches the vendored subtrees without editing them.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# === Compiler ===============================================================
# Linux builds with Clang, and only Clang: one compiler to keep working beside
# MSVC rather than two, each with its own idea of what is valid. A game's
# CMakeLists picks clang/clang++ before project() (Engine/Templates/Game), so
# a new tree gets it; a tree configured with another compiler has to be
# reconfigured from scratch (cmake --fresh), since CMake fixes the compiler
# when a tree is first made.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NimbleEngine: Linux builds with Clang only; this tree uses ${CMAKE_CXX_COMPILER_ID} "
                        "(${CMAKE_CXX_COMPILER}). Reconfigure it from scratch with "
                        "-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ --fresh.")
endif()

# === Compiler cache =========================================================
# sccache, when it is installed. Objects are cached by content hash, per build
# tree (NIMBLE_BUILD_TREE_ID below): a tree recreated after a wipe, or rebuilt
# after a branch switch, takes its objects from the cache instead of compiling
# them. Trees do not share entries; see "ONE CACHE KEY PER BUILD TREE". What
# trees DO share is the third-party libraries, built once per toolchain rather
# than cached ("Third-party libraries" below).
#
# MUST be set before the first target is created: CMAKE_<LANG>_COMPILER_LAUNCHER
# is captured into each target as it is declared, so a value set after
# add_subdirectory(Modules/*) would miss everything that matters.
#
# AUTO-DETECTED, not required. A machine without sccache builds exactly as it
# did before; nothing here is a new prerequisite for a fresh clone, which is the
# same stance Modules/GameWindow takes about never needing per-machine setup.
option(NIMBLE_USE_SCCACHE "Use sccache to cache compiled objects, when available" ON)

if(NIMBLE_USE_SCCACHE)
    find_program(NM_SCCACHE_PROGRAM sccache)

    if(NM_SCCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   "${NM_SCCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${NM_SCCACHE_PROGRAM}")

        # /Zi MUST BECOME /Z7, and the reason is not the one usually given.
        #
        # The common claim is that sccache cannot cache /Zi. Measured against the
        # version installed here (0.17.0), that is FALSE: a /Zi compile reports
        # zero non-cacheable calls and a cache hit reproduces the .pdb next to
        # the .obj. Cacheability is not the problem.
        #
        # The problem is parallelism. /Zi does not write debug info into the
        # object; it hands it to mspdbsrv.exe, which serialises many cl.exe
        # processes onto one shared .pdb. Launch those cl.exe processes as
        # CHILDREN OF SCCACHE and that coordination breaks: a parallel build dies
        # with "fatal error C1041: cannot open program database ... please use
        # /FS" -- while /FS is already on the command line, which is what makes
        # the message so misleading. A one-file-at-a-time test never shows it.
        #
        # /Z7 puts the debug info in the .obj instead, so there is no shared file
        # to contend over. The linker still produces a .pdb for each DLL and exe,
        # so debugging is unchanged; the objects are larger, which is the trade.
        #
        # CMAKE_MSVC_DEBUG_INFORMATION_FORMAT would say this more directly, but
        # that is CMake 3.25 and this project targets 3.16.
        if(MSVC)
            foreach(NM_FLAG_VAR
                    CMAKE_C_FLAGS_DEBUG            CMAKE_CXX_FLAGS_DEBUG
                    CMAKE_C_FLAGS_RELWITHDEBINFO   CMAKE_CXX_FLAGS_RELWITHDEBINFO)
                string(REPLACE "/Zi" "/Z7" ${NM_FLAG_VAR} "${${NM_FLAG_VAR}}")
            endforeach()
        endif()

        # ONE CACHE KEY PER BUILD TREE. A hit from another tree is unsafe with
        # MSVC and Ninja: sccache replays the /showIncludes of the compile that
        # filled the entry, so this tree's .ninja_deps record THAT tree's
        # headers. Change a header here and nothing here rebuilds -- objects go
        # stale against it (an engine object calling a game module's virtuals
        # through the old vtable; SDL relinked on every build once the other
        # tree was deleted). Trees whose commands match (relative paths: the
        # vendored SDL, every git worktree of the project) are exactly the ones
        # that hit.
        #
        # sccache keys a compile by its PREPROCESSED text, not its -D flags, and
        # MSVC's (/EP) carries no file paths -- so a define does not separate
        # trees. A declaration does: every compile force-includes this tree's
        # NimbleBuildTree.h, one typedef named for the tree. A tree still hits
        # its own entries after a wipe or a branch switch.
        string(SHA1 NM_SCCACHE_TREE "${CMAKE_BINARY_DIR}")
        string(SUBSTRING "${NM_SCCACHE_TREE}" 0 12 NM_SCCACHE_TREE)
        set(NM_SCCACHE_TREE_HEADER "${CMAKE_BINARY_DIR}/NimbleBuildTree.h")
        set(NM_SCCACHE_TREE_TEXT "/* This build tree's compiler-cache key (Engine/CMakeLists.txt). */\ntypedef int NimbleBuildTree_${NM_SCCACHE_TREE};\n")
        if(EXISTS "${NM_SCCACHE_TREE_HEADER}")
            file(READ "${NM_SCCACHE_TREE_HEADER}" NM_SCCACHE_TREE_OLD)
        endif()
        if(NOT NM_SCCACHE_TREE_OLD STREQUAL NM_SCCACHE_TREE_TEXT)
            file(WRITE "${NM_SCCACHE_TREE_HEADER}" "${NM_SCCACHE_TREE_TEXT}")   # only when it changes: every object depends on it
        endif()
        if(MSVC)
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/FI${NM_SCCACHE_TREE_HEADER}>")
        else()
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-include;${NM_SCCACHE_TREE_HEADER}>")
        endif()

        message(STATUS "NimbleEngine: compiler cache enabled -- ${NM_SCCACHE_PROGRAM} (keys for this tree: ${NM_SCCACHE_TREE})")
    else()
        message(STATUS "NimbleEngine: sccache not found; compiling without a compiler cache")
    endif()
endif()

# === Target type ===========================================================
#   Editor   the default: every module, the Qt editor and editor-only code.
#   Client   a game build: WITH_EDITOR=0, no editor.
#   Server   same as Client for now; a switch for future server-only gating.
#
# ONE PER CONFIGURE: WITH_EDITOR changes class layouts and exports, so every
# image must agree. It is a PUBLIC definition on EngineCore (below), so even
# a standalone game configure inherits it. Always 1 or 0, never undefined.
set(NIMBLE_TARGET "Editor" CACHE STRING "What this configure builds: Editor, Client or Server")
set_property(CACHE NIMBLE_TARGET PROPERTY STRINGS Editor Client Server)

if(NOT NIMBLE_TARGET MATCHES "^(Editor|Client|Server)$")
    message(FATAL_ERROR "NIMBLE_TARGET must be Editor, Client or Server (got '${NIMBLE_TARGET}')")
endif()

if(NIMBLE_TARGET STREQUAL "Editor")
    set(NIMBLE_WITH_EDITOR 1)
else()
    set(NIMBLE_WITH_EDITOR 0)
endif()

# === Networking ============================================================
# NetCore (Modules/NetCore): ENet transport, replication and RPC routing, and
# the dedicated server a game builds with nimble_add_server_launcher. A game
# that never networks turns it off -- in its config file (the project block
# below; Project Settings > Project > Modules):
#
#     [Modules]
#     NetCore=false
#
# or in its CMakeLists before adding the engine:
#
#     set(NIMBLE_WITH_NETCORE OFF CACHE BOOL "" FORCE)
#
# and nothing networked is compiled or linked -- no NetCore, no ENet, no
# ws2_32. Game code still builds unchanged: the replication tables
# (GameCore/ComponentReplication.h) compile to nothing, RPCs run locally
# (GameCore/ComponentRpc.h) and NimbleEngine/NetFacade.h answers "standalone".
#
# ONE PER CONFIGURE, like WITH_EDITOR, and for the same reason: it is a PUBLIC
# definition on EngineCore (below), so every image agrees. Always 1 or 0.
# === The project's module choices ==========================================
# The game's Config/DefaultEngine.ini says which optional modules it wants, in
# [Modules] (cmake/NimbleProject.cmake) -- what the editor's Project Settings
# edits. A game names the file before adding the engine:
#
#     set(NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
#
# Configured from here with Sandbox added below, Sandbox's is the project.
if(NOT NIMBLE_PROJECT_CONFIG AND (NOT DEFINED NIMBLE_BUILD_SANDBOX OR NIMBLE_BUILD_SANDBOX)
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Games/Sandbox/Config/DefaultEngine.ini")
    get_filename_component(NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/../Games/Sandbox/Config/DefaultEngine.ini" ABSOLUTE)
endif()

option(NIMBLE_WITH_NETCORE "Build the NetCore networking module (off: networking is compiled out)" ON)
nimble_project_module(NetCore NIMBLE_WITH_NETCORE "Build the NetCore networking module (off: networking is compiled out)")

if(NIMBLE_WITH_NETCORE)
    set(NIMBLE_NETCORE_DEFINE 1)
else()
    set(NIMBLE_NETCORE_DEFINE 0)
    if(NIMBLE_TARGET STREQUAL "Server")
        message(FATAL_ERROR "NIMBLE_TARGET=Server needs NetCore -- configure with NIMBLE_WITH_NETCORE=ON")
    endif()
endif()

# === Physics ===============================================================
# JoltPhysics (Modules/JoltPhysics): rigid bodies and joints on the vendored
# Jolt Physics, simulating GameCore's Rigid Body and Physics Constraint
# components. OFF by default -- a game that wants physics opts in, in its
# config file (the project block above; Project Settings > Project > Modules):
#
#     [Modules]
#     JoltPhysics=true
#
# or in its CMakeLists before adding the engine:
#
#     set(NIMBLE_WITH_JOLTPHYSICS ON CACHE BOOL "" FORCE)
#
# Off, nothing of Jolt is compiled. The components still exist (GameCore), so
# levels and prefabs carrying them load, edit and save; they do not simulate,
# and loading one says so (GameSession, WarnInactiveModuleComponents).
# NimbleEngine/PhysicsFacade.h answers "unavailable" and raycasts against
# Bounds instead.
#
# ONE PER CONFIGURE, like WITH_NETCORE: a PUBLIC definition on EngineCore
# (below). Always 1 or 0.
option(NIMBLE_WITH_JOLTPHYSICS "Build the JoltPhysics module (off: physics components are inert)" OFF)
nimble_project_module(JoltPhysics NIMBLE_WITH_JOLTPHYSICS "Build the JoltPhysics module (off: physics components are inert)")

if(NIMBLE_WITH_JOLTPHYSICS)
    set(NIMBLE_JOLTPHYSICS_DEFINE 1)
else()
    set(NIMBLE_JOLTPHYSICS_DEFINE 0)
endif()

# === Audio =================================================================
# Audio (Modules/Audio): sound on the vendored miniaudio -- an output device
# and mixer with buses, .nmsound assets, and GameCore's Audio Source and Audio
# Listener components heard in 3D. ON by default; a game that wants no sound
# turns it off, in its config file (Project Settings > Project > Modules):
#
#     [Modules]
#     Audio=false
#
# or in its CMakeLists before adding the engine:
#
#     set(NIMBLE_WITH_AUDIO OFF CACHE BOOL "" FORCE)
#
# Off, nothing of miniaudio is compiled. The components still exist
# (GameCore), so levels and prefabs carrying them load, edit and save; they
# are silent, and loading one says so (WarnInactiveModuleComponents).
# NimbleEngine/AudioFacade.h answers "unavailable" and the Audio script
# library's calls do nothing.
#
# ONE PER CONFIGURE, like WITH_NETCORE: a PUBLIC definition on EngineCore
# (below). Always 1 or 0.
option(NIMBLE_WITH_AUDIO "Build the Audio module (off: the game is silent)" ON)
nimble_project_module(Audio NIMBLE_WITH_AUDIO "Build the Audio module (off: the game is silent)")

if(NIMBLE_WITH_AUDIO)
    set(NIMBLE_AUDIO_DEFINE 1)
else()
    set(NIMBLE_AUDIO_DEFINE 0)
endif()

# === AI ====================================================================
# AICore (Modules/AICore): navmeshes (Recast), pathfinding and crowds (Detour),
# behavior trees with utility scoring, and perception. ON by default; a game
# with no AI turns it off, in its config file (Project Settings > Project >
# Modules):
#
#     [Modules]
#     AICore=false
#
# or in its CMakeLists before adding the engine:
#
#     set(NIMBLE_WITH_AICORE OFF CACHE BOOL "" FORCE)
#
# Off, nothing of Recast is compiled and NimbleEngine/AIFacade.h answers
# "unavailable". A game that defines its own behavior tree tasks links AICore
# itself, under if(TARGET AICore).
#
# ONE PER CONFIGURE, like WITH_NETCORE: a PUBLIC definition on EngineCore
# (below). Always 1 or 0.
option(NIMBLE_WITH_AICORE "Build the AICore module (off: no navigation or behavior trees)" ON)
nimble_project_module(AICore NIMBLE_WITH_AICORE "Build the AICore module (off: no navigation or behavior trees)")

if(NIMBLE_WITH_AICORE)
    set(NIMBLE_AICORE_DEFINE 1)
else()
    set(NIMBLE_AICORE_DEFINE 0)
endif()

message(STATUS "NimbleEngine: target ${NIMBLE_TARGET} (WITH_EDITOR=${NIMBLE_WITH_EDITOR}, WITH_NETCORE=${NIMBLE_NETCORE_DEFINE}, "
               "WITH_JOLTPHYSICS=${NIMBLE_JOLTPHYSICS_DEFINE}, WITH_AUDIO=${NIMBLE_AUDIO_DEFINE}, WITH_AICORE=${NIMBLE_AICORE_DEFINE})")

# === Configurations ========================================================
#   Debug            unoptimised, full debug info.
#   RelWithDebInfo   optimised with debug info (CMake's flags).
#   Shipping         optimised, no debug info, NDEBUG; NIMBLE_SHIPPING=1.
#
# Shipping's flags are CACHE variables so a standalone game configure (the
# top-level project) sees them. Seeded from Release while EMPTY and FORCEd:
# project() pre-creates them empty for a custom build type, so an "if not
# defined" guard would leave Shipping unoptimised.
foreach(NM_LANG C CXX)
    if("${CMAKE_${NM_LANG}_FLAGS_SHIPPING}" STREQUAL "")
        set(CMAKE_${NM_LANG}_FLAGS_SHIPPING "${CMAKE_${NM_LANG}_FLAGS_RELEASE}"
            CACHE STRING "Flags used by the ${NM_LANG} compiler during Shipping builds" FORCE)
    endif()
endforeach()
foreach(NM_KIND EXE SHARED MODULE STATIC)
    if("${CMAKE_${NM_KIND}_LINKER_FLAGS_SHIPPING}" STREQUAL "" AND NOT "${CMAKE_${NM_KIND}_LINKER_FLAGS_RELEASE}" STREQUAL "")
        set(CMAKE_${NM_KIND}_LINKER_FLAGS_SHIPPING "${CMAKE_${NM_KIND}_LINKER_FLAGS_RELEASE}"
            CACHE STRING "Flags used by the linker for ${NM_KIND} targets during Shipping builds" FORCE)
    endif()
endforeach()
mark_as_advanced(CMAKE_C_FLAGS_SHIPPING CMAKE_CXX_FLAGS_SHIPPING
                 CMAKE_EXE_LINKER_FLAGS_SHIPPING CMAKE_SHARED_LINKER_FLAGS_SHIPPING
                 CMAKE_MODULE_LINKER_FLAGS_SHIPPING CMAKE_STATIC_LINKER_FLAGS_SHIPPING)

get_property(NM_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NM_MULTI_CONFIG)
    # Offer it in multi-config generators, once.
    if(NOT "Shipping" IN_LIST CMAKE_CONFIGURATION_TYPES)
        list(APPEND CMAKE_CONFIGURATION_TYPES Shipping)
        set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Shipping" AND NIMBLE_WITH_EDITOR)
    # Nothing ships the editor; fail at configure.
    message(FATAL_ERROR "Shipping is a Client or Server configuration -- configure with "
                        "-DNIMBLE_TARGET=Client (or Server), or pick Debug / RelWithDebInfo for the Editor")
endif()

# === Uncooked content =======================================================
# Every configuration but Shipping runs UNCOOKED: it reads the project's text
# worlds and prefabs, as the editor does, and a Client or Server cooks each
# one itself the first time it loads it (and again when the source changes),
# into Intermediate/Cooked/ beside the executable -- so a development build
# never waits on a manual cook. Shipping reads only what was cooked ahead,
# into <project>/Saved/Cooked/ (the editor's Build > Cook Content). See GameCore's
# WorldPackage.h (LoadContentWorld).
#
# WITH_UNCOOKED is that switch in code, 1 or 0, PUBLIC on EngineCore below.
# NIMBLE_UNCOOKED_CONTENT is it for a game's CMakeLists: what to stage beside
# the executable -- the text sources, or the cooked files. A multi-config
# generator stages the sources (its Shipping configuration needs a
# single-config tree to stage the cooked ones).
if(NM_MULTI_CONFIG OR NOT CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(NIMBLE_UNCOOKED_CONTENT ON  CACHE INTERNAL "Stage text worlds and prefabs (every build but Shipping)")
else()
    set(NIMBLE_UNCOOKED_CONTENT OFF CACHE INTERNAL "Stage text worlds and prefabs (every build but Shipping)")
endif()

# === Tests ==================================================================
# NIMBLE_BUILD_TESTS is the one switch for every test executable in the
# engine; each module's own option (UI_BUILD_TESTS, GAMECORE_BUILD_SMOKETEST,
# ...) still picks among them when it is on. Debug and RelWithDebInfo build
# them by default; the editor's Build > Build Project passes it from its
# "Build test executables" option.
#
# SHIPPING NEVER BUILDS TESTS. A single-config Shipping tree turns every
# module's option off whatever NIMBLE_BUILD_TESTS says (a normal variable
# shadows the option: CMP0077, NEW at 3.16), so the targets never exist; a
# multi-config tree leaves them out of its Shipping build (the pass at the
# end of this file).
option(NIMBLE_BUILD_TESTS "Build the engine's test executables (Debug and RelWithDebInfo; never Shipping)" ON)
set(NM_TEST_OPTIONS
        NIMBLEENGINE_BUILD_TESTS NIMBLEEDITOR_BUILD_TESTS
        CORE_BUILD_SMOKETEST GDI_BUILD_SMOKETEST GAMECORE_BUILD_SMOKETEST
        RENDERCORE_BUILD_SMOKETEST RENDERCORE_BUILD_ANIMATION_TESTS
        NETCORE_BUILD_TESTS JOLTPHYSICS_BUILD_TESTS AUDIO_BUILD_TESTS
        SEQUENCER_BUILD_TESTS UI_BUILD_TESTS NODEGRAPH_BUILD_TESTS COLLAB_BUILD_TESTS)
if(NOT NM_MULTI_CONFIG AND CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(NM_TESTS_ENABLED OFF)
    if(NIMBLE_BUILD_TESTS)
        message(STATUS "NimbleEngine: Shipping builds no tests (NIMBLE_BUILD_TESTS is ignored)")
    endif()
else()
    set(NM_TESTS_ENABLED ${NIMBLE_BUILD_TESTS})
endif()
if(NOT NM_TESTS_ENABLED)
    foreach(NM_TEST_OPTION IN LISTS NM_TEST_OPTIONS)
        set(${NM_TEST_OPTION} OFF)
    endforeach()
endif()
message(STATUS "NimbleEngine: tests ${NM_TESTS_ENABLED}")

# === Packaged content =======================================================
# NIMBLE_PACKAGE_CONTENT: a game's content in Paks/Engine.nmpak and
# Paks/Game.nmpak beside the executable instead of loose files
# (cmake/NimblePack.cmake). On by default for Shipping; the editor's Build >
# Build Project turns it on for whatever configuration it builds. A packaged
# build always takes the cooked worlds, prefabs and UI, whatever the
# configuration: a pack holds what the game reads through the file provider,
# and the text loaders read the disk. Never in an Editor target, which edits
# the loose files, nor with a multi-config generator (no single directory).
if(NIMBLE_WITH_EDITOR OR NM_MULTI_CONFIG)
    set(NIMBLE_PACKAGE_CONTENT OFF CACHE BOOL "Pack a game's content into Paks/ instead of loose files" FORCE)
else()
    if(CMAKE_BUILD_TYPE STREQUAL "Shipping")
        set(NM_PACKAGE_DEFAULT ON)
    else()
        set(NM_PACKAGE_DEFAULT OFF)
    endif()
    set(NIMBLE_PACKAGE_CONTENT ${NM_PACKAGE_DEFAULT} CACHE BOOL "Pack a game's content into Paks/ instead of loose files")
endif()

# NIMBLE_RUN_UNCOOKED: a Debug or RelWithDebInfo game runs uncooked by default
# -- reading the project's raw files (config, scripts, Content, text worlds,
# prefabs and UI) from its source folder instead of anything staged or packed
# (-uncooked / -cooked on its command line choose either way; ParseLaunchArgs).
# The editor's Build Project sets it from its "Run uncooked" option. No effect
# on Shipping, which reads only cooked content.
option(NIMBLE_RUN_UNCOOKED "Debug/RelWithDebInfo games read the project's raw files by default (-uncooked)" OFF)

# What a game stages: the cooked worlds and prefabs unless it runs on the
# text sources (an uncooked build that is not packaged).
if(NIMBLE_UNCOOKED_CONTENT AND NOT NIMBLE_PACKAGE_CONTENT)
    set(NIMBLE_STAGE_TEXT_CONTENT ON  CACHE INTERNAL "Stage text worlds and prefabs rather than cooked ones")
else()
    set(NIMBLE_STAGE_TEXT_CONTENT OFF CACHE INTERNAL "Stage text worlds and prefabs rather than cooked ones")
endif()

# === Binaries/ =============================================================
# One flat runtime directory per platform+config:
# <NIMBLE_BINARIES_DIR>/<Platform>_<Config>, e.g. Windows_Debug, Mac_Release,
# Linux_RelWithDebInfo -- with a _<Target> suffix for a Client or Server
# target (Windows_Debug_Client); see the Target type block above.
#
# WHERE THAT ROOT POINTS, and why it is not Engine/Binaries. Hot reload requires
# the host executable, EngineCore and every module DLL to be siblings in one
# directory, so there is exactly ONE such directory per build -- which means it
# belongs to whoever is being run, not to the engine that happens to declare it.
# The default is therefore the Sandbox host's own Binaries, and a second game
# overrides this cache variable to claim its own instead of inheriting a folder
# named after another title.
#
# Overridable rather than hardcoded, because the alternative is every future
# host editing this line and conflicting with every other host that did.
# Normalised before it reaches the cache, so the value shown in cmake-gui and
# baked into every target property is a real path rather than one with an
# "Engine/../Games" hinge in the middle of it.
get_filename_component(NM_DEFAULT_BINARIES_DIR
                       "${CMAKE_CURRENT_SOURCE_DIR}/../Games/Sandbox/Binaries" ABSOLUTE)
set(NIMBLE_BINARIES_DIR "${NM_DEFAULT_BINARIES_DIR}"
    CACHE PATH "Root of the flat runtime layout; <Platform>_<Config> is created below it")
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
# Client/Server builds get their own folder: their DLLs lack editor exports
# and would break NimbleEditor.exe if they overwrote its DLLs.
if(NIMBLE_TARGET STREQUAL "Editor")
    set(NM_TARGET_SUFFIX "")
else()
    set(NM_TARGET_SUFFIX "_${NIMBLE_TARGET}")
endif()

if(WIN32)
    set(NM_PLATFORM Windows)
elseif(APPLE)
    set(NM_PLATFORM Mac)
else()
    set(NM_PLATFORM Linux)
endif()

# NM_MULTI_CONFIG was read in the Configurations block above.
if(NM_MULTI_CONFIG)
    # Visual Studio / Xcode / Ninja Multi-Config: only the per-config variables
    # matter, and CMake appends nothing to them. Setting the plain variable here
    # would just get a /Debug tacked on by the generator and scatter output.
    foreach(NM_CFG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${NM_CFG}" NM_CFG_U)
        set(NM_CFG_DIR "${NIMBLE_BINARIES_DIR}/${NM_PLATFORM}_${NM_CFG}${NM_TARGET_SUFFIX}")
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
    set(NM_CFG_DIR "${NIMBLE_BINARIES_DIR}/${NM_PLATFORM}_${CMAKE_BUILD_TYPE}${NM_TARGET_SUFFIX}")
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

# The build-wide switches, as EngineCore usage requirements so everything that
# links it agrees. Always 1 or 0.
target_compile_definitions(EngineCore PUBLIC
        WITH_EDITOR=${NIMBLE_WITH_EDITOR}
        WITH_NETCORE=${NIMBLE_NETCORE_DEFINE}
        WITH_JOLTPHYSICS=${NIMBLE_JOLTPHYSICS_DEFINE}
        WITH_AUDIO=${NIMBLE_AUDIO_DEFINE}
        WITH_AICORE=${NIMBLE_AICORE_DEFINE}
        NIMBLE_SHIPPING=$<IF:$<CONFIG:Shipping>,1,0>
        WITH_UNCOOKED=$<IF:$<CONFIG:Shipping>,0,1>)

# === Third-party libraries ==================================================
# SDL3, Jolt, Lua, pugixml, ENet, miniaudio, bc7enc_rdo and xatlas, vendored in
# ThirdParty/Source. Prebuilt once per toolchain into Intermediate/ThirdParty
# and IMPORTED here, so no build tree compiles them itself
# (NIMBLE_PREBUILT_THIRDPARTY; see cmake/NimbleThirdParty.cmake).
#
# ORDER IS LOAD-BEARING: after Modules/Core, whose SIMD flags Jolt is built
# with, and before every module that links one of them.
include(cmake/NimbleThirdParty.cmake)

# NimbleCook cooks the engine's own shader set from Content/Shaders (see
# "Cooked shaders" below), so GDI's cook -- its full Odessa set, into its own source
# tree, plus .qrc bundles nothing here compiles -- is off. FORCE, as with
# CORE_SHARED: a stale cache would cook both and stage neither consistently.
set(GDI_COOK_SHADERS OFF CACHE BOOL "Cook Shaders/ into Shaders/Cooked as part of the GDI build" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GDI/CMakeLists.txt")
    add_subdirectory(Modules/GDI)
endif()

# GameWindow is not a subtree -- it is written for this repository -- but it is
# guarded the same way so a checkout without it still configures.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GameWindow/CMakeLists.txt")
    add_subdirectory(Modules/GameWindow)
endif()

# GameCore likewise: written for this repository, guarded the same way. Added
# AFTER GameWindow and BEFORE the NimbleEngine target below for the same
# ordering reason GameWindow itself is -- nimble_add_module() links
# EngineCore::EngineCore eagerly and NimbleEngine's own nimble_add_module()
# call further down needs GameCore::GameCore to already exist as a namespaced
# ALIAS when it is named.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GameCore/CMakeLists.txt")
    add_subdirectory(Modules/GameCore)
endif()

# RenderCore: the render stack, on its own render thread. Written for this
# repository, guarded the same way as GameCore, and for the same ordering
# reason -- placed AFTER GameCore (it links GameCore::GameCore PUBLIC) and
# BEFORE the NimbleEngine target below (whose own nimble_add_module() call
# needs RenderCore::RenderCore to already exist as a namespaced ALIAS).
#
# ALSO guarded on TARGET GDI: RenderCore links GDI PRIVATE and is the ONLY
# image in the process allowed to -- see RenderCoreInternal.h's comment -- so
# it cannot configure at all without GDI already present. A checkout without
# GDI (or without RenderCore itself) still configures, matching the existing
# "a checkout without GDI still configures" property GameWindow's own guard
# states.
if(TARGET GDI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/RenderCore/CMakeLists.txt")
    add_subdirectory(Modules/RenderCore)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/RenderCore/CMakeLists.txt")
    message(STATUS "RenderCore present but GDI is not configured; skipping RenderCore")
endif()

# NetCore: networking, when NIMBLE_WITH_NETCORE is on (the Networking block
# above). After GameCore, whose World and registries it drives, and before the
# NimbleEngine target, which links it.
if(NIMBLE_WITH_NETCORE AND TARGET GameCore
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/NetCore/CMakeLists.txt")
    add_subdirectory(Modules/NetCore)
endif()

# Collab: editor collaboration -- the protocol, server sessions and client
# (Modules/Editor/COLLABORATION.md). Core and ENet only, so after NetCore,
# whose vendored ENet it shares, but not needing it on. Editor and Server
# targets: what NimbleEditorServer and the editor link.
if(NOT NIMBLE_TARGET STREQUAL "Client" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Collab/CMakeLists.txt")
    add_subdirectory(Modules/Collab)
endif()

# JoltPhysics: rigid-body physics, when NIMBLE_WITH_JOLTPHYSICS is on (the
# Physics block above). After GameCore, whose components it simulates, and
# before NimbleEngine, which attaches it to each level's World.
if(NIMBLE_WITH_JOLTPHYSICS AND TARGET GameCore
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/JoltPhysics/CMakeLists.txt")
    add_subdirectory(Modules/JoltPhysics)
endif()

# Audio: sound, when NIMBLE_WITH_AUDIO is on (the Audio block above). After
# GameCore, whose components it plays, and before NimbleEngine, which attaches
# it to each level's World.
if(NIMBLE_WITH_AUDIO AND TARGET GameCore
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Audio/CMakeLists.txt")
    add_subdirectory(Modules/Audio)
endif()

# Sequencer: cutscenes -- .nmseq timelines and the Sequence Player component
# (Modules/Sequencer/README.md). After GameCore and RenderCore, which it
# drives, and before NimbleEngine, which attaches it to each level's World.
if(TARGET GameCore AND TARGET RenderCore AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Sequencer/CMakeLists.txt")
    add_subdirectory(Modules/Sequencer)
endif()

# AICore: navigation, behavior trees and perception, when NIMBLE_WITH_AICORE is
# on (the AI block above). After GameCore, whose World and components it
# drives, and before NimbleEngine, which attaches it to each level's World.
if(NIMBLE_WITH_AICORE AND TARGET GameCore
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/AICore/CMakeLists.txt")
    add_subdirectory(Modules/AICore)
endif()

# UI: in-game interfaces from XML documents (Modules/UI/README.md). After
# RenderCore, whose UI draw list it paints into, and before NimbleEngine,
# which owns a game session's UISystem.
if(TARGET RenderCore AND TARGET GameCore AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/UI/CMakeLists.txt")
    add_subdirectory(Modules/UI)
endif()

# NodeGraph: material/script graphs generating GLSL and Lua. Editor only;
# after GameCore, which it uses.
if(NIMBLE_WITH_EDITOR AND TARGET GameCore
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/NodeGraph/CMakeLists.txt")
    add_subdirectory(Modules/NodeGraph)
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
# <NIMBLE_BINARIES_DIR>/<Platform>_<Config> beside EngineCore -- which is what
# the hot-reload sibling rule requires.
set(NIMBLEENGINE_SOURCES
        Source/Private/NimbleEngine/NimbleEngine.cpp
        Source/Private/NimbleEngine/ContentReport.cpp
        Source/Private/NimbleEngine/GameSession.cpp
        Source/Private/NimbleEngine/GameFramework.cpp
        Source/Private/NimbleEngine/GameMode.cpp
        Source/Private/NimbleEngine/GameUI.cpp
        Source/Private/NimbleEngine/GameConsole.cpp
        Source/Private/NimbleEngine/PlayerState.cpp
        Source/Private/NimbleEngine/Avatar.cpp
        Source/Private/NimbleEngine/PlayerStart.cpp
        Source/Private/NimbleEngine/GameModule.cpp
        Source/Private/NimbleEngine/Level.cpp
        Source/Private/NimbleEngine/SaveGame.cpp
        Source/Private/NimbleEngine/WorldList.cpp
        Source/Private/NimbleEngine/SaveGameFormat.h
        Source/Private/NimbleEngine/Input.cpp
        Source/Private/NimbleEngine/InputInternal.h
        Source/Private/NimbleEngine/InputRouter.cpp
        Source/Private/NimbleEngine/CollisionMeshes.cpp
        Source/Private/NimbleEngine/AudioFacade.cpp
        Source/Private/NimbleEngine/NetFacade.cpp
        Source/Private/NimbleEngine/LagCompensation.cpp
        Source/Private/NimbleEngine/FrameRecorder.cpp
        Source/Private/NimbleEngine/FrameRecorder.h
        Source/Private/NimbleEngine/NetHost.cpp
        Source/Private/NimbleEngine/NetScript.cpp
        Source/Private/NimbleEngine/PhysicsFacade.cpp
        Source/Private/NimbleEngine/Ragdoll.cpp
        Source/Private/NimbleEngine/RagdollProfile.cpp
        Source/Private/NimbleEngine/TerrainFacade.cpp
        Source/Private/NimbleEngine/AIFacade.cpp
        Source/Private/NimbleEngine/AIInternal.h
        Source/Private/NimbleEngine/SequenceFacade.cpp
        Source/Private/NimbleEngine/InactiveModules.cpp
        Source/Private/NimbleEngine/ShaderCook.cpp
        Source/Public/NimbleEngine/NimbleEngine.h
        Source/Public/NimbleEngine/GameMode.h
        Source/Public/NimbleEngine/Avatar.h
        Source/Public/NimbleEngine/PlayerStart.h
        Source/Public/NimbleEngine/LagCompensation.h
        Source/Public/NimbleEngine/GameSession.h
        Source/Public/NimbleEngine/GameConsole.h
        Source/Public/NimbleEngine/GameModule.h
        Source/Public/NimbleEngine/NimbleEngineAPI.h
        Source/Public/NimbleEngine/Level.h
        Source/Public/NimbleEngine/GameState.h
        Source/Public/NimbleEngine/PlayerState.h
        Source/Public/NimbleEngine/SaveGame.h
        Source/Public/NimbleEngine/WorldList.h
        Source/Public/NimbleEngine/NimbleEngineTypes.h
        Source/Public/NimbleEngine/Input.h
        Source/Public/NimbleEngine/InputRouter.h
        Source/Public/NimbleEngine/AudioFacade.h
        Source/Public/NimbleEngine/NetFacade.h
        Source/Public/NimbleEngine/NetHost.h
        Source/Public/NimbleEngine/PhysicsFacade.h
        Source/Public/NimbleEngine/Ragdoll.h
        Source/Public/NimbleEngine/TerrainFacade.h
        Source/Public/NimbleEngine/AIFacade.h
        Source/Public/NimbleEngine/SequenceFacade.h
        Source/Public/NimbleEngine/ShaderCook.h
)

nimble_add_module(NimbleEngine SHARED
        PREFIX  NIMBLEENGINE
        SOURCES ${NIMBLEENGINE_SOURCES})

# Where a development build finds NimbleCook's output (ShaderCook.h): the
# Engine folder it was built from. Not in Shipping, which names no source path.
target_compile_definitions(NimbleEngine PRIVATE
        "$<$<NOT:$<CONFIG:Shipping>>:NIMBLE_ENGINE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\">")

# PRIVATE, not PUBLIC: the engine layer's headers must not leak GameWindow
# types to a game. If this ever has to become a PUBLIC_DEP, something in
# Source/Public/NimbleEngine/ grew an #include it should not have.
#
# if(TARGET) rather than if(EXISTS) because the module directory is already
# guarded above -- this mirrors the same "a checkout without GameWindow still
# configures" property without re-testing paths.
#
# GDI is NOT linked here at all, and that is new: GDI is a STATIC library, so
# it may live in exactly ONE image in the whole process (see
# RenderCoreInternal.h's comment for what a second copy would corrupt), and
# RenderCore.dll is that image. NimbleEngine reaches drawing exclusively
# through RenderCore::Renderer and ENQUEUE_RENDER_COMMAND -- see the PUBLIC
# link just below -- and never through GDI directly.
#
# Hot-reload plugins contribute NOTHING here. nimble_add_hot_module() builds
# MODULE targets that are never linked against; NimbleEngine reaches them
# through CoreModuleManager at runtime. What they will want once they exist is
# add_dependencies(NimbleEngine <plugin targets>), so that building the engine
# also rebuilds the plugins it is about to load.
if(TARGET GameWindow)
    target_link_libraries(NimbleEngine PRIVATE GameWindow::GameWindow)
endif()

# PUBLIC, not PRIVATE -- the two deliberate exceptions to the rule stated
# just above. World/Entity (GameCore) and Renderer/RenderCommand/RenderProxy
# (RenderCore) ARE the game-facing surface: GameSession.h forward-declares
# GameCore::World, GameCore::ScriptEngine and RenderCore::Renderer, and
# exposes GameSession::GetWorld()/GetScriptEngine()/GetRenderer() returning
# them by reference, so a game that includes NimbleEngine.h needs both
# modules' headers and needs to LINK both to use what it forward-declares.
# Nothing from GameWindow becomes reachable through this -- PUBLIC applies to
# GameCore and RenderCore alone, and the PRIVATE link above is unaffected.
if(TARGET GameCore)
    target_link_libraries(NimbleEngine PUBLIC GameCore::GameCore)
endif()

if(TARGET RenderCore)
    target_link_libraries(NimbleEngine PUBLIC RenderCore::RenderCore)
endif()

# PRIVATE: a game networks through GameCore's replication and RPC macros and
# NetFacade.h, never NetCore's headers, so turning NetCore off cannot break a
# game's include or link. A game that wants NetSession itself links NetCore,
# under if(TARGET NetCore).
if(TARGET NetCore)
    target_link_libraries(NimbleEngine PRIVATE NetCore::NetCore)
endif()

# PRIVATE, as NetCore: a game reaches physics through GameCore's components
# and NimbleEngine/PhysicsFacade.h, so turning it off breaks no game.
if(TARGET JoltPhysics)
    target_link_libraries(NimbleEngine PRIVATE JoltPhysics::JoltPhysics)
endif()

# PRIVATE, as JoltPhysics: a game reaches sound through GameCore's components,
# the Audio script library and NimbleEngine/AudioFacade.h, so turning it off
# breaks no game.
if(TARGET Audio)
    target_link_libraries(NimbleEngine PRIVATE Audio::Audio)
endif()

# PRIVATE: a game plays cutscenes through the Sequence Player component, the
# Sequence script library and NimbleEngine/SequenceFacade.h.
if(TARGET Sequencer)
    target_link_libraries(NimbleEngine PRIVATE Sequencer::Sequencer)
endif()

# PRIVATE: a game reaches AI through its components, the AI script library
# and NimbleEngine/AIFacade.h. A game that registers behavior tree tasks of
# its own links AICore itself, under if(TARGET AICore).
if(TARGET AICore)
    target_link_libraries(NimbleEngine PRIVATE AICore::AICore)
endif()

# A session's UISystem (GameSession::GetUI) is game-facing: PUBLIC.
if(TARGET UI)
    target_link_libraries(NimbleEngine PUBLIC UI::UI)
endif()

# winmm, for timeBeginPeriod/timeEndPeriod in the frame limiter. Without raising
# the timer resolution every sleep rounds up to Windows' ~15.6ms scheduler tick,
# which cannot express a 60fps frame at all -- NimbleEngine.cpp's
# TimerResolutionScope has the measurements. PRIVATE, like the module deps
# above: this is an implementation detail of the loop, not something a game
# links against.
if(WIN32)
    target_link_libraries(NimbleEngine PRIVATE winmm)
endif()

# PRIVATE so this target's own .cpp files can say "Private/NimbleEngine/...".
# nimble_add_module already added Source/ PUBLIC for the "Public/NimbleEngine/..."
# includes a consumer uses.
target_include_directories(NimbleEngine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

source_group("NimbleEngine" FILES ${NIMBLEENGINE_SOURCES})

# === Input tests ===========================================================
# The input path without a window: names, edges, maps and their config
# overrides, and the InputComponent's resolve ahead of TP_PrePhysics scripts.
option(NIMBLEENGINE_BUILD_TESTS "Build NimbleEngineInputTests" ON)
if(NIMBLEENGINE_BUILD_TESTS)
    add_executable(NimbleEngineInputTests Tests/InputTests.cpp)
    target_link_libraries(NimbleEngineInputTests PRIVATE NimbleEngine::NimbleEngine)
    # GameWindow and SDL for the gamepad test only: a virtual pad driven
    # through the real pump. A game never links either.
    if(TARGET GameWindow AND TARGET SDL3::SDL3)
        target_link_libraries(NimbleEngineInputTests PRIVATE GameWindow::GameWindow SDL3::SDL3)
        target_compile_definitions(NimbleEngineInputTests PRIVATE NIMBLEENGINE_TESTS_HAVE_SDL=1)
    endif()
    set_target_properties(NimbleEngineInputTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    # From Binaries, so the engine DLLs resolve and staged content
    # (Engine/Content/Meshes) is found by its relative path.
    add_test(NAME NimbleEngine.InputTests COMMAND NimbleEngineInputTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineInputTests>")
    set_tests_properties(NimbleEngine.InputTests PROPERTIES TIMEOUT 120 LABELS "smoke")

    # Collision: overlaps, events, layers, queries, and shapes from meshes.
    add_executable(NimbleEngineTerrainTests Tests/TerrainTests.cpp)
    target_link_libraries(NimbleEngineTerrainTests PRIVATE NimbleEngine::NimbleEngine)
    set_target_properties(NimbleEngineTerrainTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME NimbleEngine.TerrainTests COMMAND NimbleEngineTerrainTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineTerrainTests>")
    set_tests_properties(NimbleEngine.TerrainTests PROPERTIES TIMEOUT 120 LABELS "smoke")

    add_executable(NimbleEngineCollisionTests Tests/CollisionTests.cpp)
    target_link_libraries(NimbleEngineCollisionTests PRIVATE NimbleEngine::NimbleEngine)
    set_target_properties(NimbleEngineCollisionTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME NimbleEngine.CollisionTests COMMAND NimbleEngineCollisionTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineCollisionTests>")
    set_tests_properties(NimbleEngine.CollisionTests PROPERTIES TIMEOUT 120 LABELS "smoke")

    # The game framework: phases, rules, PlayerState, the Game library, saves.
    add_executable(NimbleEngineGameFrameworkTests Tests/GameFrameworkTests.cpp)
    target_link_libraries(NimbleEngineGameFrameworkTests PRIVATE NimbleEngine::NimbleEngine)
    set_target_properties(NimbleEngineGameFrameworkTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME NimbleEngine.GameFrameworkTests COMMAND NimbleEngineGameFrameworkTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineGameFrameworkTests>")
    set_tests_properties(NimbleEngine.GameFrameworkTests PROPERTIES TIMEOUT 120 LABELS "smoke")

    # Avatars (Avatar.h): possession, the harness's input, the server's half of
    # the input stream, simulations, AutoPossess and NetGameMode.
    add_executable(NimbleEngineAvatarTests Tests/AvatarTests.cpp)
    target_link_libraries(NimbleEngineAvatarTests PRIVATE NimbleEngine::NimbleEngine)
    set_target_properties(NimbleEngineAvatarTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME NimbleEngine.AvatarTests COMMAND NimbleEngineAvatarTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineAvatarTests>")
    set_tests_properties(NimbleEngine.AvatarTests PROPERTIES TIMEOUT 120 LABELS "smoke")

    # Physics: bodies, shapes from meshes, forces, joints, events, raycasts.
    # Only with the module (NIMBLE_WITH_JOLTPHYSICS).
    if(TARGET JoltPhysics AND JOLTPHYSICS_BUILD_TESTS)
        add_executable(NimbleEnginePhysicsTests Tests/PhysicsTests.cpp)
        target_link_libraries(NimbleEnginePhysicsTests PRIVATE NimbleEngine::NimbleEngine JoltPhysics::JoltPhysics)
        set_target_properties(NimbleEnginePhysicsTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
        add_test(NAME NimbleEngine.PhysicsTests COMMAND NimbleEnginePhysicsTests
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEnginePhysicsTests>")
        set_tests_properties(NimbleEngine.PhysicsTests PROPERTIES TIMEOUT 120 LABELS "smoke")

        # Ragdolls (Ragdoll.h): CrashDummy's profile, joint limits, a death.
        add_executable(NimbleEngineRagdollTests Tests/RagdollTests.cpp)
        target_link_libraries(NimbleEngineRagdollTests PRIVATE NimbleEngine::NimbleEngine JoltPhysics::JoltPhysics)
        set_target_properties(NimbleEngineRagdollTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
        add_test(NAME NimbleEngine.RagdollTests COMMAND NimbleEngineRagdollTests
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineRagdollTests>")
        set_tests_properties(NimbleEngine.RagdollTests PROPERTIES TIMEOUT 120 LABELS "smoke")
    endif()

    # Sound from scripts: the Audio library, Audio Source from Lua, and the
    # PhysicsLab engine-sound graph. Only with the module (NIMBLE_WITH_AUDIO).
    if(TARGET Audio AND TARGET NodeGraph AND AUDIO_BUILD_TESTS)
        add_executable(NimbleEngineAudioScriptTests Tests/AudioScriptTests.cpp)
        target_link_libraries(NimbleEngineAudioScriptTests PRIVATE NimbleEngine::NimbleEngine Audio::Audio NodeGraph::NodeGraph)
        set_target_properties(NimbleEngineAudioScriptTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
        add_test(NAME NimbleEngine.AudioScriptTests COMMAND NimbleEngineAudioScriptTests
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineAudioScriptTests>")
        set_tests_properties(NimbleEngine.AudioScriptTests PROPERTIES TIMEOUT 120 LABELS "smoke")
    endif()

    # AVATARS_GUIDE.md's examples -- C++, Lua and a script graph -- built and
    # run. The graph needs NodeGraph (an Editor build).
    if(TARGET NodeGraph)
        add_executable(NimbleEngineAvatarGuideTests Tests/AvatarGuideTests.cpp)
        target_link_libraries(NimbleEngineAvatarGuideTests PRIVATE NimbleEngine::NimbleEngine NodeGraph::NodeGraph)
        set_target_properties(NimbleEngineAvatarGuideTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
        add_test(NAME NimbleEngine.AvatarGuideTests COMMAND NimbleEngineAvatarGuideTests
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineAvatarGuideTests>")
        set_tests_properties(NimbleEngine.AvatarGuideTests PROPERTIES TIMEOUT 120 LABELS "smoke")
        add_executable(NimbleEngineGameModeGuideTests Tests/GameModeGuideTests.cpp)
        target_link_libraries(NimbleEngineGameModeGuideTests PRIVATE NimbleEngine::NimbleEngine NodeGraph::NodeGraph)
        set_target_properties(NimbleEngineGameModeGuideTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
        add_test(NAME NimbleEngine.GameModeGuideTests COMMAND NimbleEngineGameModeGuideTests
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEngineGameModeGuideTests>")
        set_tests_properties(NimbleEngine.GameModeGuideTests PROPERTIES TIMEOUT 120 LABELS "smoke")
    endif()
endif()

# === Cooked shaders ========================================================
# NimbleCook (Tools/NimbleCook, built below) cooks the engine's shaders --
# the build never runs a shader compiler -- into
# Engine/Saved/Cooked/Shaders/<Variant>/, laid out as the runtime reads it.
# This stages them:
#
#   Runtime/   Content/Shaders at the paths GDI and RenderCore ask
#              EngineCore::FilesystemProvider for
#              ("Shaders/Cooked/Vulkan/Basic.vsh.spv"), and the Engine shader
#              cache, Shaders/Cache/Engine.<Platform>.nmsc, which
#              Renderer::Initialize mounts (RenderCore/MATERIALS.md): the
#              engine's materials (DefaultLit, named
#              "Engine/Content/Materials/DefaultLit.material.nmgraph" like the
#              meshes) and the shared MaterialDefault vertex shaders
#   Editor/    Editor targets: the engine's materials loose too
#   Shipping/  a packaged Shipping build: the Engine cache naming nothing,
#              over Runtime/'s
#
# Content/Shaders holds only what the engine draws with -- Basic, Mesh,
# Screen, the material templates and the shared includes -- not GDI's Odessa
# set (brush, cursor, layer blit, checkerboard), which GDI registers but only
# loads on request.
#
# A variant not cooked yet is a warning and a configure dependency, so the
# cook that makes it re-configures (nimble_cooked_shader_commands,
# cmake/NimbleMaterials.cmake); after that, each cook restages. Cook with
#
#     NimbleCook shaders [-project <game folder>] [-editor] [-shipping]
#
# or the editor's Build > Cook Content.
#
# Single-config only (needs CMAKE_RUNTIME_OUTPUT_DIRECTORY), as the copy it
# replaces was.
if(TARGET GDI AND CMAKE_RUNTIME_OUTPUT_DIRECTORY
   AND (GDI_WITH_OPENGL OR GDI_WITH_VULKAN OR GDI_WITH_D3D12))
    # Packaged: staged into the Engine pack's folder, not beside the executable.
    set(NM_SHADER_ROOT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    if(NIMBLE_PACKAGE_CONTENT)
        nimble_pack_stage_dir(Engine NM_SHADER_ROOT)
    endif()
    set(NM_SHADER_VARIANTS Runtime "${NM_SHADER_ROOT}")
    if(NIMBLE_PACKAGE_CONTENT AND CMAKE_BUILD_TYPE STREQUAL "Shipping")
        list(APPEND NM_SHADER_VARIANTS Shipping "${NM_SHADER_ROOT}")
    endif()
    if(NIMBLE_WITH_EDITOR)
        list(APPEND NM_SHADER_VARIANTS Editor "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    endif()
    nimble_cooked_shader_commands(NM_SHADER_STAGE_COMMANDS NM_SHADER_COOKED_STAMPS "NimbleEngine"
            "${CMAKE_CURRENT_SOURCE_DIR}/Saved/Cooked/Shaders" ${NM_SHADER_VARIANTS})

    if(NM_SHADER_STAGE_COMMANDS)
        set(NM_SHADER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/NimbleEngine_Shaders.stamp")
        add_custom_command(
                OUTPUT  "${NM_SHADER_STAMP}"
                ${NM_SHADER_STAGE_COMMANDS}
                COMMAND ${CMAKE_COMMAND} -E touch "${NM_SHADER_STAMP}"
                DEPENDS ${NM_SHADER_COOKED_STAMPS}
                COMMENT "NimbleEngine: staging the shaders NimbleCook cooked"
                VERBATIM)

        add_custom_target(NimbleStageShaders ALL DEPENDS "${NM_SHADER_STAMP}")
        add_dependencies(NimbleEngine NimbleStageShaders)
        set_target_properties(NimbleStageShaders PROPERTIES FOLDER "Engine")
        if(NIMBLE_PACKAGE_CONTENT)
            nimble_pack_add_dir(Engine "Shaders" "${NM_SHADER_ROOT}/Shaders" "${NM_SHADER_STAMP}" NimbleStageShaders)
        endif()
    endif()
endif()

# === Engine/Content/Meshes and Characters ==================================
# The engine's primitive meshes (Box, Sphere, Plane, Cylinder, Capsule; made by
# Scripts/generate_primitive_meshes.py) and its characters (Characters/<Name>/:
# a skinned mesh, its skeleton, clips, anim states and props, and prefabs with
# their Lua -- ANIMATION.md, "Engine characters"), staged beside the executable
# at the same path, so a RenderComponent names one "Engine/Content/Meshes/Box.nmmesh"
# in every target.
# Single-config only, as the shader staging above.
file(GLOB NM_ENGINE_MESHES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Meshes/*.nmmesh")
file(GLOB_RECURSE NM_ENGINE_CHARACTERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/Content/Characters/*.nmmesh"
     "${CMAKE_CURRENT_SOURCE_DIR}/Content/Characters/*.nmskel"
     "${CMAKE_CURRENT_SOURCE_DIR}/Content/Characters/*.nmanim"
     "${CMAKE_CURRENT_SOURCE_DIR}/Content/Characters/*.nmanimstate"
     "${CMAKE_CURRENT_SOURCE_DIR}/Content/Characters/*.nmprefab"
     "${CMAKE_CURRENT_SOURCE_DIR}/Content/Characters/*.lua")
list(APPEND NM_ENGINE_MESHES ${NM_ENGINE_CHARACTERS})
get_filename_component(NM_ENGINE_PARENT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)   # "Engine/Content/..." below it
if(NIMBLE_PACKAGE_CONTENT)
    nimble_pack_add_files(Engine "${NM_ENGINE_PARENT}" ${NM_ENGINE_MESHES})
elseif(TARGET NimbleEngine AND CMAKE_RUNTIME_OUTPUT_DIRECTORY AND NM_ENGINE_MESHES)
    set(NM_MESH_OUTPUTS)
    foreach(NM_MESH ${NM_ENGINE_MESHES})
        file(RELATIVE_PATH NM_MESH_REL "${CMAKE_CURRENT_SOURCE_DIR}/Content" "${NM_MESH}")
        get_filename_component(NM_MESH_DIR "${NM_MESH_REL}" DIRECTORY)
        set(NM_MESH_DST "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine/Content/${NM_MESH_REL}")
        add_custom_command(
                OUTPUT  "${NM_MESH_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine/Content/${NM_MESH_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NM_MESH}" "${NM_MESH_DST}"
                DEPENDS "${NM_MESH}"
                COMMENT "NimbleEngine: staging Engine/Content/${NM_MESH_REL}"
                VERBATIM)
        list(APPEND NM_MESH_OUTPUTS "${NM_MESH_DST}")
    endforeach()
    add_custom_target(NimbleStageMeshes ALL DEPENDS ${NM_MESH_OUTPUTS})
    add_dependencies(NimbleEngine NimbleStageMeshes)
    set_target_properties(NimbleStageMeshes PROPERTIES FOLDER "Engine")
endif()

# === Engine/Content/Fonts ==================================================
# The UI's default font (UIFontCache::DEFAULT_FONT, Noto Sans under the OFL --
# OFL.txt beside it ships with it), staged as the meshes are, at the same path.
file(GLOB NM_ENGINE_FONTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Fonts/*.ttf"
                                            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Fonts/*.txt")
if(NIMBLE_PACKAGE_CONTENT)
    nimble_pack_add_files(Engine "${NM_ENGINE_PARENT}" ${NM_ENGINE_FONTS})
elseif(TARGET NimbleEngine AND CMAKE_RUNTIME_OUTPUT_DIRECTORY AND NM_ENGINE_FONTS)
    set(NM_FONT_OUTPUTS)
    foreach(NM_FONT ${NM_ENGINE_FONTS})
        get_filename_component(NM_FONT_NAME "${NM_FONT}" NAME)
        set(NM_FONT_DST "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine/Content/Fonts/${NM_FONT_NAME}")
        add_custom_command(
                OUTPUT  "${NM_FONT_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine/Content/Fonts"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NM_FONT}" "${NM_FONT_DST}"
                DEPENDS "${NM_FONT}"
                COMMENT "NimbleEngine: staging Engine/Content/Fonts/${NM_FONT_NAME}"
                VERBATIM)
        list(APPEND NM_FONT_OUTPUTS "${NM_FONT_DST}")
    endforeach()
    add_custom_target(NimbleStageFonts ALL DEPENDS ${NM_FONT_OUTPUTS})
    add_dependencies(NimbleEngine NimbleStageFonts)
    set_target_properties(NimbleStageFonts PROPERTIES FOLDER "Engine")
endif()

# === NimblePak ==============================================================
# Makes and lists content packs (Tools/NimblePak.cpp; cmake/NimblePack.cmake
# runs it for a packaged build). Beside EngineCore, whose library it uses.
add_executable(NimblePak Tools/NimblePak.cpp)
target_link_libraries(NimblePak PRIVATE EngineCore::EngineCore GameCore Lua::Lua)
set_target_properties(NimblePak PROPERTIES FOLDER "Engine")

# === NimbleCook =============================================================
# Cooks the engine's shaders and every material's permutations into
# Saved/Cooked/Shaders/, which the build only stages ("Cooked shaders" above;
# cmake/NimbleMaterials.cmake). It runs NodeGraph's cook_material_graphs.py,
# so it is the one tool that needs Python and the shader toolchain; the build
# needs neither. It defaults to this tree's GDI backends, Engine folder and
# Python (-target, -engine and -python override them).
find_package(Python3 COMPONENTS Interpreter QUIET)
set(NM_COOK_TARGETS)
foreach(NM_COOK_PLATFORM OpenGL Vulkan D3D12)
    string(TOUPPER "${NM_COOK_PLATFORM}" NM_COOK_PLATFORM_UPPER)
    if(GDI_WITH_${NM_COOK_PLATFORM_UPPER})
        list(APPEND NM_COOK_TARGETS ${NM_COOK_PLATFORM})
    endif()
endforeach()
string(REPLACE ";" "," NM_COOK_TARGETS "${NM_COOK_TARGETS}")
add_executable(NimbleCook Tools/NimbleCook/NimbleCookMain.cpp)
target_compile_definitions(NimbleCook PRIVATE
        "NIMBLECOOK_ENGINE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        "NIMBLECOOK_PYTHON=\"${Python3_EXECUTABLE}\""
        "NIMBLECOOK_TARGETS=\"${NM_COOK_TARGETS}\"")
target_compile_features(NimbleCook PRIVATE cxx_std_17)
set_target_properties(NimbleCook PROPERTIES FOLDER "Engine")

# === NimbleEditorServer =====================================================
# The editor collaboration server (Tools/EditorServer; Modules/Collab): Core,
# Collab and ENet, no Qt and no game code, so a host needs only a compiler.
if(TARGET Collab)
    add_executable(NimbleEditorServer Tools/EditorServer/EditorServerMain.cpp)
    target_link_libraries(NimbleEditorServer PRIVATE EngineCore::EngineCore Collab::Collab)
    set_target_properties(NimbleEditorServer PROPERTIES FOLDER "Engine")

    # Its content worker, one per session when it has --repo: started beside it.
    add_executable(NimbleContentWorker Tools/ContentWorker/ContentWorkerMain.cpp)
    target_link_libraries(NimbleContentWorker PRIVATE EngineCore::EngineCore Collab::Collab)
    set_target_properties(NimbleContentWorker PROPERTIES FOLDER "Engine")
    add_dependencies(NimbleEditorServer NimbleContentWorker)
endif()

# === Editor ================================================================
# The Qt editor: after NimbleStageShaders, and needs RenderCore. Skips itself
# when Qt6 Widgets is missing. Editor targets only.
option(NIMBLE_BUILD_EDITOR "Build the Qt editor (Modules/Editor) in an Editor target" ON)
if(NIMBLE_BUILD_EDITOR AND NIMBLE_WITH_EDITOR AND TARGET RenderCore
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Editor/CMakeLists.txt")
    add_subdirectory(Modules/Editor)
elseif(NOT NIMBLE_WITH_EDITOR)
    message(STATUS "NimbleEditor: not built for a ${NIMBLE_TARGET} target")
endif()

# === Games/ ================================================================
# Hosts that link the engine layer. Added from THIS scope, below the
# NimbleEngine target, for two reasons that are both load-bearing:
#
#   * CMAKE_RUNTIME_OUTPUT_DIRECTORY is inherited by targets created at or
#     below this scope (see the Binaries/ block above). A game declared at repo
#     root would be a SIBLING of Engine and would not inherit it -- and on
#     Windows, landing beside NimbleEngine.dll / EngineCore.dll / SDL3.dll is
#     the only way an exe resolves them. NIMBLE_BINARIES_DIR defaults to this
#     host's own Binaries, so the whole build collects there.
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

# === Tests: never in Shipping (multi-config) ================================
# A multi-config generator configures every configuration at once, so the
# test executables exist in its Shipping configuration too: leave them out of
# that configuration's build (see "Tests" above). The engine's test targets
# are named *Tests or *SmokeTest; vendored trees (ThirdParty/) are not looked at.
if(NM_MULTI_CONFIG AND NM_TESTS_ENABLED)
    function(nm_exclude_tests_from_shipping InDir)
        get_property(NM_TARGETS DIRECTORY "${InDir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(NM_T IN LISTS NM_TARGETS)
            get_target_property(NM_TYPE ${NM_T} TYPE)
            if(NM_TYPE STREQUAL "EXECUTABLE" AND NM_T MATCHES "(Tests|SmokeTest)$")
                set_target_properties(${NM_T} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD_SHIPPING ON)
            endif()
        endforeach()
        get_property(NM_SUBDIRS DIRECTORY "${InDir}" PROPERTY SUBDIRECTORIES)
        foreach(NM_SUB IN LISTS NM_SUBDIRS)
            if(NOT NM_SUB MATCHES "/ThirdParty/")
                nm_exclude_tests_from_shipping("${NM_SUB}")
            endif()
        endforeach()
    endfunction()
    nm_exclude_tests_from_shipping("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

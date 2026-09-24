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

# === Compiler cache =========================================================
# sccache, when it is installed. Objects are cached by content hash, so the
# expensive thing this repository builds -- the vendored SDL, ~260 translation
# units of it -- is compiled once per MACHINE rather than once per build tree.
# A second tree (a standalone Games/Sandbox configure, a CLion tree beside a
# command-line one, a tree recreated after a wipe) then links SDL instead of
# rebuilding it.
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

        message(STATUS "NimbleEngine: compiler cache enabled -- ${NM_SCCACHE_PROGRAM}")
    else()
        message(STATUS "NimbleEngine: sccache not found; compiling without a compiler cache")
    endif()
endif()

# === Target type ===========================================================
# What this configure builds, the way a shipped engine splits it:
#
#   Editor   the default: every module, the Qt editor, and editor-only engine
#            code (the property registry, the Renderer's debug lines and view
#            override) compiled in.
#   Client   a game build: WITH_EDITOR=0, no editor executable, and none of
#            that editor-only code in any module.
#   Server   the same as Client today. The distinct name exists so server-only
#            gating has a switch to hang on when it is needed.
#
# ONE PER CONFIGURE, not per target: WITH_EDITOR changes class layouts and
# exported symbols (Renderer grows members under it), and every image in the
# process must agree. It travels as a PUBLIC compile definition on EngineCore
# (set just after Modules/Core is added, below), so every target that links the
# engine inherits it -- including a game configured STANDALONE, whose own
# directory sits ABOVE this one and so would never see a directory-scoped
# add_compile_definitions() made here. The subtrees need no edit for it.
#
# Always defined, to 1 or 0 -- never left undefined. GameCoreAPI.h and
# RenderCoreAPI.h #error if it is missing, because `#if WITH_EDITOR` on an
# undefined name is a silent 0: a build that forgot the flag would drop the
# editor code without a word.
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

message(STATUS "NimbleEngine: target ${NIMBLE_TARGET} (WITH_EDITOR=${NIMBLE_WITH_EDITOR})")

# === Configurations ========================================================
# Debug, RelWithDebInfo and Shipping are the three this project builds:
#
#   Debug            unoptimised, full debug info -- day-to-day work.
#   RelWithDebInfo   optimised, WITH debug info -- profiling, and debugging an
#                    optimised build. CMake's own flags.
#   Shipping         optimised, NO debug info, NDEBUG -- what a player runs.
#                    NIMBLE_SHIPPING is 1 here and 0 everywhere else, for code
#                    that must not ship (dev cheats, verbose logging).
#
# Shipping is a custom configuration, so its flags are declared here. CACHE
# variables rather than plain ones, for the same reason WITH_EDITOR rides on
# EngineCore: a game configured standalone is the TOP-LEVEL project and adds
# this directory beneath it, so a normal variable set here would never reach
# the game's own targets -- the game exe would build Shipping unoptimised.
#
# Seeded from Release while EMPTY, not while undefined, and FORCEd: when
# CMAKE_BUILD_TYPE names a custom configuration, project() itself creates these
# cache entries -- empty -- so an "if not defined" guard never fires and
# Shipping silently compiles with no optimisation at all (observed). A value a
# user has set is non-empty and is left alone.
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
    # Offer it in Visual Studio / Xcode / Ninja Multi-Config. Checked, so a
    # re-configure does not append it twice.
    if(NOT "Shipping" IN_LIST CMAKE_CONFIGURATION_TYPES)
        list(APPEND CMAKE_CONFIGURATION_TYPES Shipping)
        set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Shipping" AND NIMBLE_WITH_EDITOR)
    # An editor is a development tool; nothing ships it. Refused outright
    # rather than built half-way, so a mis-picked preset fails at configure.
    message(FATAL_ERROR "Shipping is a Client or Server configuration -- configure with "
                        "-DNIMBLE_TARGET=Client (or Server), or pick Debug / RelWithDebInfo for the Editor")
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
# A Client or Server build gets its own folder, <Platform>_<Config>_<Target>:
# its DLLs lack the editor-only exports, and landing them over the editor
# build's would leave NimbleEditor.exe unable to load. The Editor keeps the
# unsuffixed name every existing path (VS_DEBUGGER_WORKING_DIRECTORY, docs)
# already uses.
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

# The build-wide switches, as EngineCore usage requirements -- every module,
# the editor and every game link EngineCore, so each one compiles with the same
# values whichever directory it was declared in. See the Target type and
# Configurations blocks above. Both are always defined, to 1 or 0.
target_compile_definitions(EngineCore PUBLIC
        WITH_EDITOR=${NIMBLE_WITH_EDITOR}
        NIMBLE_SHIPPING=$<IF:$<CONFIG:Shipping>,1,0>)

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
        Source/Private/NimbleEngine/GameSession.cpp
        Source/Private/NimbleEngine/Level.cpp
        Source/Private/NimbleEngine/SaveGame.cpp
        Source/Private/NimbleEngine/SaveGameFormat.h
        Source/Public/NimbleEngine/NimbleEngine.h
        Source/Public/NimbleEngine/GameMode.h
        Source/Public/NimbleEngine/GameSession.h
        Source/Public/NimbleEngine/NimbleEngineAPI.h
        Source/Public/NimbleEngine/Level.h
        Source/Public/NimbleEngine/GameState.h
        Source/Public/NimbleEngine/SaveGame.h
        Source/Public/NimbleEngine/NimbleEngineTypes.h
)

nimble_add_module(NimbleEngine SHARED
        PREFIX  NIMBLEENGINE
        SOURCES ${NIMBLEENGINE_SOURCES})

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

# === Cooked shaders ========================================================
# GDI's cooked shaders staged beside the executable, so that
# EngineCore::FilesystemProvider -- which Application::Initialize() installs,
# resolving bundle-relative paths against the working directory -- can find
# them at the path GDI asks for ("Shaders/Cooked/Vulkan/Basic.vsh.spv").
#
# AT ENGINE SCOPE, not in a game's own CMakeLists, and that is the whole
# point: Games/Sandbox stages its OWN Config/ and Scripts/ because those
# belong to that game, but the cooked shaders belong to GDI and every game
# that ever links this engine needs exactly the same set. Staging them per
# game would duplicate this block into every future host, each free to get
# it subtly wrong.
#
# A DIRECTORY COPY rather than one rule per file: the cooked set is produced
# by cook_shaders.py (whose outputs the Cook*Shaders targets already depend
# on), so the file list is not knowable here without duplicating that
# script's own naming rules. The dependency on the cook targets is what
# orders this after the shaders actually exist; without it a clean build
# races an empty source directory.
#
# copy_directory, NOT copy_directory_if_different: the latter is CMake 3.26+
# and this project pins 3.16 (see cmake_minimum_required at the top). The
# cost is that this target is considered on every build -- acceptable for a
# handful of small files, and the same trade Games/Sandbox's own multi-config
# staging fallback already makes for the same reason.
if(TARGET GDI AND GDI_COOKED_SHADER_DIR AND CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    add_custom_target(NimbleStageShaders ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${GDI_COOKED_SHADER_DIR}"
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/Cooked"
            COMMENT "NimbleEngine: staging GDI's cooked shaders beside the executable"
            VERBATIM)

    # Ordered after whichever cook targets this configure actually created --
    # each is guarded by its own GDI_WITH_* option in Modules/GDI.
    foreach(NM_COOK_TARGET CookVulkanShaders CookOpenGLShaders CookD3D12Shaders CookMetalShaders)
        if(TARGET ${NM_COOK_TARGET})
            add_dependencies(NimbleStageShaders ${NM_COOK_TARGET})
        endif()
    endforeach()

    add_dependencies(NimbleEngine NimbleStageShaders)
    set_target_properties(NimbleStageShaders PROPERTIES FOLDER "Engine")
endif()

# === Editor ================================================================
# The Qt editor. Below the NimbleStageShaders block, which it depends on, and
# guarded on RenderCore because the viewport draws through it. Skipped (with a
# status line, from inside its own CMakeLists) when Qt6 Widgets is not found,
# so a machine without Qt still configures the engine and the games.
#
# Editor targets only: a Client or Server configure never adds the directory,
# and the editor's sources are additionally wrapped in `#if WITH_EDITOR`.
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

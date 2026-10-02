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
include(NimbleMaterials)

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

message(STATUS "NimbleEngine: target ${NIMBLE_TARGET} (WITH_EDITOR=${NIMBLE_WITH_EDITOR})")

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
        NIMBLE_SHIPPING=$<IF:$<CONFIG:Shipping>,1,0>)

# The engine cooks its own shader set from Content/Shaders (see "Cooked
# shaders" below), so GDI's cook -- its full Odessa set, into its own source
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
        Source/Private/NimbleEngine/GameSession.cpp
        Source/Private/NimbleEngine/GameModule.cpp
        Source/Private/NimbleEngine/Level.cpp
        Source/Private/NimbleEngine/SaveGame.cpp
        Source/Private/NimbleEngine/SaveGameFormat.h
        Source/Public/NimbleEngine/NimbleEngine.h
        Source/Public/NimbleEngine/GameMode.h
        Source/Public/NimbleEngine/GameSession.h
        Source/Public/NimbleEngine/GameModule.h
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
# Content/Shaders cooked straight into the runtime directory, at the paths GDI
# and RenderCore ask EngineCore::FilesystemProvider for
# ("Shaders/Cooked/Vulkan/Basic.vsh.spv"). Content/Shaders holds only what the
# engine draws with -- Basic, Mesh, Screen, the material templates and the
# shared includes -- not GDI's Odessa set (brush, cursor, layer blit,
# checkerboard), which GDI registers but only loads on request.
#
# Content/Materials (DefaultLit, a material graph cooked from its <gen_code>,
# named "Engine/Content/Materials/DefaultLit.material.nmgraph" like the
# meshes) and the shared MaterialDefault vertex shaders go into the Engine
# shader cache, Shaders/Cache/Engine.<Platform>.nmsc, which
# Renderer::Initialize mounts (RenderCore/MATERIALS.md); Editor targets stage
# them loose too.
#
# Nothing is written into the source tree and no .qrc is generated: the
# runtime reads plain files, so the cook is a staging step for the host, not
# a prerequisite of compiling GDI. Stamp-file incremental, like
# nimble_stage_materials.
#
# Single-config only (needs CMAKE_RUNTIME_OUTPUT_DIRECTORY), as the copy it
# replaces was.
if(TARGET GDI AND CMAKE_RUNTIME_OUTPUT_DIRECTORY
   AND (GDI_WITH_OPENGL OR GDI_WITH_VULKAN OR GDI_WITH_D3D12))
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    file(GLOB NM_ENGINE_SHADERS CONFIGURE_DEPENDS
            "${NIMBLE_ENGINE_SHADER_DIR}/*.vsh"
            "${NIMBLE_ENGINE_SHADER_DIR}/*.fsh")
    file(GLOB NM_ENGINE_MATERIALS CONFIGURE_DEPENDS "${NIMBLE_ENGINE_MATERIAL_DIR}/*.material.nmgraph")
    get_filename_component(NM_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)   # "Engine/Content/..." below it
    nimble_shader_cook_commands(NM_SHADER_COOK_COMMANDS
            SRC "${NIMBLE_ENGINE_SHADER_DIR}"
            OUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/Cooked")
    nimble_shader_cook_commands(NM_ENGINE_CACHE_COMMANDS DEFAULTS
            SRC    "${NIMBLE_ENGINE_MATERIAL_DIR}"
            ROOT   "${NM_ENGINE_ROOT}"
            CACHE  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/Cache"
            MODULE Engine
            SUBDIR Materials)
    list(APPEND NM_SHADER_COOK_COMMANDS ${NM_ENGINE_CACHE_COMMANDS})
    if(NIMBLE_WITH_EDITOR)
        nimble_shader_cook_commands(NM_ENGINE_LOOSE_COMMANDS DEFAULTS
                SRC      "${NIMBLE_ENGINE_MATERIAL_DIR}"
                ROOT     "${NM_ENGINE_ROOT}"
                OUT      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/Cooked"
                DATA_OUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                SUBDIR   Materials)
        list(APPEND NM_SHADER_COOK_COMMANDS ${NM_ENGINE_LOOSE_COMMANDS})
    endif()

    set(NM_SHADER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/NimbleEngine_Shaders.stamp")
    add_custom_command(
            OUTPUT  "${NM_SHADER_STAMP}"
            ${NM_SHADER_COOK_COMMANDS}
            COMMAND ${CMAKE_COMMAND} -E touch "${NM_SHADER_STAMP}"
            DEPENDS ${NM_ENGINE_SHADERS} ${NM_ENGINE_MATERIALS} ${NIMBLE_SHADER_INCLUDES}
                    "${NIMBLE_MATERIAL_COOK_SCRIPT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/GDI/Scripts/cook_shaders.py"
            COMMENT "NimbleEngine: cooking Content/Shaders and the Engine shader cache"
            VERBATIM)

    add_custom_target(NimbleStageShaders ALL DEPENDS "${NM_SHADER_STAMP}")
    add_dependencies(NimbleEngine NimbleStageShaders)
    set_target_properties(NimbleStageShaders PROPERTIES FOLDER "Engine")
endif()

# === Engine/Content/Meshes =================================================
# The engine's primitive meshes (Box, Sphere, Plane; made by
# Scripts/generate_primitive_meshes.py), staged beside the executable at the
# same path, so a RenderComponent names one "Engine/Content/Meshes/Box.nmmesh"
# in every target. Single-config only, as the shader staging above.
file(GLOB NM_ENGINE_MESHES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Meshes/*.nmmesh")
if(TARGET NimbleEngine AND CMAKE_RUNTIME_OUTPUT_DIRECTORY AND NM_ENGINE_MESHES)
    set(NM_MESH_OUTPUTS)
    foreach(NM_MESH ${NM_ENGINE_MESHES})
        get_filename_component(NM_MESH_NAME "${NM_MESH}" NAME)
        set(NM_MESH_DST "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine/Content/Meshes/${NM_MESH_NAME}")
        add_custom_command(
                OUTPUT  "${NM_MESH_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Engine/Content/Meshes"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NM_MESH}" "${NM_MESH_DST}"
                DEPENDS "${NM_MESH}"
                COMMENT "NimbleEngine: staging Engine/Content/Meshes/${NM_MESH_NAME}"
                VERBATIM)
        list(APPEND NM_MESH_OUTPUTS "${NM_MESH_DST}")
    endforeach()
    add_custom_target(NimbleStageMeshes ALL DEPENDS ${NM_MESH_OUTPUTS})
    add_dependencies(NimbleEngine NimbleStageMeshes)
    set_target_properties(NimbleStageMeshes PROPERTIES FOLDER "Engine")
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

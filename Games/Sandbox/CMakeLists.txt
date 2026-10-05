cmake_minimum_required(VERSION 3.16)

project(Sandbox VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# Sandbox -- the example game.
#
# The working reference for how a game drives NimbleEngine, and the host each
# new engine feature gets exercised in. It is a target under Games/ rather than
# a test under Engine/ for that reason: it consumes the engine the way a real
# title would, through the public API and nothing else, so anything it cannot
# reach is a gap in that API rather than a gap here.
#
# CONFIGURES TWO WAYS, and both are supported:
#
#   1. FROM ENGINE. Engine/CMakeLists.txt add_subdirectory()s this at its own
#      scope, which is how a full build gets it. This is the normal path.
#
#   2. STANDALONE. Open Games/Sandbox directly in an IDE, or point cmake at it.
#      This directory is then the top-level project and pulls the Engine tree
#      in itself. That is what the SANDBOX_STANDALONE block below is for.
#
# The same test the vendored subtrees use -- CMAKE_SOURCE_DIR STREQUAL
# CMAKE_CURRENT_SOURCE_DIR -- separates the two, so the pattern here is the one
# already established in Modules/Core and Modules/GDI rather than a new one.
#
# Nothing here reaches into a vendored subtree, so the "deliberately thin" rule
# at the top of Engine/CMakeLists.txt does not apply: Games/ is neither
# Modules/Core nor Modules/GDI.
# ===========================================================================

# C++17, exactly -- see Engine/CMakeLists.txt. Repeated here because a
# standalone configure creates this directory's targets in a scope the Engine
# tree's setting does not reach.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SANDBOX_STANDALONE ON)
else()
    set(SANDBOX_STANDALONE OFF)
endif()

# === Standalone: bring the engine in ourselves =============================
if(SANDBOX_STANDALONE)
    message(STATUS "Sandbox: standalone configure -- adding the Engine tree")

    # A cache entry rather than a hardcoded path, so a checkout with a different
    # layout can be pointed at its engine without editing this file.
    set(SANDBOX_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine"
        CACHE PATH "Path to the Engine tree Sandbox links against")

    if(NOT EXISTS "${SANDBOX_ENGINE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Sandbox: no Engine/CMakeLists.txt at '${SANDBOX_ENGINE_DIR}'. "
            "Set SANDBOX_ENGINE_DIR to the Engine tree, or configure the Engine "
            "directory instead and let it add this one.")
    endif()

    # MUST come before add_subdirectory. Engine/CMakeLists.txt add_subdirectory()s
    # THIS directory when NIMBLE_BUILD_SANDBOX is on, which standalone would turn
    # into Sandbox adding Engine adding Sandbox -- a duplicate target, and a
    # confusing error a long way from its cause. option() leaves an existing
    # cache entry alone, which is what makes setting it here effective.
    set(NIMBLE_BUILD_SANDBOX OFF CACHE BOOL
        "Build the Games/Sandbox host (off here: Sandbox IS the top-level project)" FORCE)

    # Explicit binary dir because the source dir is outside this one.
    add_subdirectory("${SANDBOX_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/Engine")
endif()

if(NOT TARGET NimbleEngine)
    message(FATAL_ERROR
        "Sandbox: the NimbleEngine target does not exist. When added from "
        "Engine/CMakeLists.txt this directory must come AFTER the "
        "nimble_add_module(NimbleEngine ...) call that creates it.")
endif()

# === Sources ===============================================================
# Source/, laid out like an engine module: Public/Sandbox/ for the headers,
# Private/Sandbox/ for the .cpp files, included Source-relative
# ("Public/Sandbox/SandboxScripting.h"). nimble_add_hot_module adds Source/ to
# the include path.
set(SANDBOX_GAME_SOURCES
        Source/Private/Sandbox/SandboxGame.cpp
        Source/Private/Sandbox/SandboxScripting.cpp
        Source/Private/Sandbox/SandboxInput.cpp
        Source/Private/Sandbox/BrickBreaker.cpp
        Source/Public/Sandbox/SandboxScripting.h
        Source/Public/Sandbox/BrickBreaker.h
)

# === The game module =======================================================
# SandboxGame.cpp as a module DLL, loaded by name (NimbleEngine/GameModule.h)
# by the launcher and the editor's Play-in-Editor, never linked. A game module
# is a Core module, hence nimble_add_hot_module(); its .dll is a LIBRARY
# artifact, so it lands via LIBRARY_OUTPUT_DIRECTORY.
nimble_add_hot_module(SandboxGame
        SOURCES      ${SANDBOX_GAME_SOURCES}
        PRIVATE_DEPS NimbleEngine::NimbleEngine)
set_target_properties(SandboxGame PROPERTIES FOLDER "Games")

# === The launcher ==========================================================
# The engine's own launcher (Engine/Source/Launcher/GameLauncherMain.cpp),
# built as Sandbox: it runs [Game] Module, SandboxGame if unset. Also puts the
# executable and the module beside the engine libraries, however this
# directory was configured (Engine/cmake/NimbleModule.cmake).
nimble_add_game_launcher(Sandbox MODULE SandboxGame)

# The editor loads this game at Play, so building the editor builds it too.
if(TARGET NimbleEditor)
    add_dependencies(NimbleEditor SandboxGame)

    # Edit > Project Settings edits this game's master config, not the staged
    # copy (see the Editor's CMakeLists.txt).
    set_property(TARGET NimbleEditor PROPERTY
            NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
endif()

# === Runtime layout ========================================================
# Everything lands in Binaries/<Platform>_<Config> BELOW THIS DIRECTORY: the
# engine DLLs, the module DLLs and this executable all together, because hot
# reload requires them to be siblings and because the one directory that holds
# them belongs to the host being run. Engine/CMakeLists.txt's NIMBLE_BINARIES_DIR
# defaults here for that reason; a second game overrides it to claim its own.
#
# nimble_add_game_launcher above already put the executable and the module
# there, on either configure path. The staging below needs the same directory
# as a plain path: asked of the engine's target, which knows it either way
# (inherited from Engine/CMakeLists.txt's scope, or not, when standalone).
get_property(SANDBOX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
get_target_property(SANDBOX_RT_DIR NimbleEngine RUNTIME_OUTPUT_DIRECTORY)

# === Config staging ========================================================
# Application::Initialize() reads "Config/DefaultEngine.ini" relative to the
# working directory. Staging it beside the executable means the same file is
# found whether the exe is launched from an IDE (see the debugger working
# directory below) or by hand from Binaries/.
#
# copy_if_different, so an untouched file does not re-copy on every build. The
# copy in THIS directory is the master -- edits made to the staged one are
# overwritten by the next build that changes the source.
#
# NOT add_custom_command(TARGET ... POST_BUILD). A POST_BUILD command only runs
# when its target actually relinks, and editing an .ini does not relink an
# executable -- so the staged copy silently went stale and the app kept reading
# the previous values through a rebuild that reported success. That was
# observed, not theorised: a run configured for 90 frames ran forever on a stale
# MaxFrames=0.
#
# A REAL OUTPUT WITH A REAL DEPENDENCY, so the build graph knows when this is up
# to date and a no-change build genuinely does nothing. The obvious alternative,
# a custom target marked ALL, is considered every build and therefore prints a
# step every build -- which costs nothing but makes an incremental build look
# like it is working when it is not, and that is the exact signal you want
# trustworthy when you are asking "why did that rebuild?".
set(SANDBOX_CONFIG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")

if(EXISTS "${SANDBOX_CONFIG_SRC}" AND SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
    set(SANDBOX_CONFIG_DST "${SANDBOX_RT_DIR}/Config/DefaultEngine.ini")

    add_custom_command(
            OUTPUT  "${SANDBOX_CONFIG_DST}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SANDBOX_RT_DIR}/Config"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SANDBOX_CONFIG_SRC}" "${SANDBOX_CONFIG_DST}"
            DEPENDS "${SANDBOX_CONFIG_SRC}"
            COMMENT "Sandbox: staging Config/DefaultEngine.ini beside the executable"
            VERBATIM)

    add_custom_target(SandboxStageConfig DEPENDS "${SANDBOX_CONFIG_DST}")
    add_dependencies(Sandbox SandboxStageConfig)
    set_target_properties(SandboxStageConfig PROPERTIES FOLDER "Games")

elseif(EXISTS "${SANDBOX_CONFIG_SRC}")
    # Multi-config fallback. The destination differs per configuration, so a
    # single OUTPUT path cannot describe it, and a generator expression is not
    # allowed in OUTPUT until CMake 3.20 -- above the 3.16 this project targets.
    # An always-considered target is the honest option here; copy_if_different
    # keeps it to a timestamp compare.
    add_custom_target(SandboxStageConfig ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:Sandbox>/Config"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SANDBOX_CONFIG_SRC}"
                    "$<TARGET_FILE_DIR:Sandbox>/Config/DefaultEngine.ini"
            DEPENDS "${SANDBOX_CONFIG_SRC}"
            COMMENT "Sandbox: staging Config/DefaultEngine.ini beside the executable"
            VERBATIM)
    add_dependencies(SandboxStageConfig Sandbox)
    set_target_properties(SandboxStageConfig PROPERTIES FOLDER "Games")
endif()

# === Scripts/ ===============================================================
# ScriptEngine::RunFile resolves paths the same way Application::Initialize()
# resolves "Config/DefaultEngine.ini" -- relative to the working directory --
# so Scripts/ is staged beside the executable by the identical mechanism as
# Config/ above, for the identical reason (an IDE run and a shell run must
# both find it, and a POST_BUILD step would silently go stale exactly as the
# comment on SANDBOX_CONFIG_DST explains).
#
# A script graph (*.script.nmgraph) is listed the same way: the runtime runs
# the Lua stored in it (<gen_code>).
#
# LISTED, NOT GLOBBED: a new script is a CMakeLists.txt edit, so the build
# graph is honest about what it stages rather than silently picking up
# whatever a directory scan happens to find -- the same call ComponentPool.h's
# design notes make for enumerating things explicitly over discovering them.
set(SANDBOX_SCRIPT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Main.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Mover.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Pickup.script.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/ScoreBoard.script.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/InputDemo.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/JumpCounter.script.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Zone.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Ball.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/Brick.script.nmgraph"
)

if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
    set(SANDBOX_SCRIPT_OUTPUTS "")
    foreach(SANDBOX_SCRIPT_SRC ${SANDBOX_SCRIPT_FILES})
        get_filename_component(SANDBOX_SCRIPT_NAME "${SANDBOX_SCRIPT_SRC}" NAME)
        set(SANDBOX_SCRIPT_DST "${SANDBOX_RT_DIR}/Scripts/${SANDBOX_SCRIPT_NAME}")

        add_custom_command(
                OUTPUT  "${SANDBOX_SCRIPT_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SANDBOX_RT_DIR}/Scripts"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SANDBOX_SCRIPT_SRC}" "${SANDBOX_SCRIPT_DST}"
                DEPENDS "${SANDBOX_SCRIPT_SRC}"
                COMMENT "Sandbox: staging Scripts/${SANDBOX_SCRIPT_NAME} beside the executable"
                VERBATIM)
        list(APPEND SANDBOX_SCRIPT_OUTPUTS "${SANDBOX_SCRIPT_DST}")
    endforeach()

    add_custom_target(SandboxStageScripts DEPENDS ${SANDBOX_SCRIPT_OUTPUTS})
    add_dependencies(Sandbox SandboxStageScripts)
    set_target_properties(SandboxStageScripts PROPERTIES FOLDER "Games")

else()
    # Multi-config fallback, same reasoning as SandboxStageConfig's own.
    set(SANDBOX_SCRIPT_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Sandbox>/Scripts")
    foreach(SANDBOX_SCRIPT_SRC ${SANDBOX_SCRIPT_FILES})
        get_filename_component(SANDBOX_SCRIPT_NAME "${SANDBOX_SCRIPT_SRC}" NAME)
        list(APPEND SANDBOX_SCRIPT_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SANDBOX_SCRIPT_SRC}"
                        "$<TARGET_FILE_DIR:Sandbox>/Scripts/${SANDBOX_SCRIPT_NAME}")
    endforeach()

    add_custom_target(SandboxStageScripts ALL
            ${SANDBOX_SCRIPT_COMMANDS}
            DEPENDS ${SANDBOX_SCRIPT_FILES}
            COMMENT "Sandbox: staging Scripts/ beside the executable"
            VERBATIM)
    add_dependencies(SandboxStageScripts Sandbox)
    set_target_properties(SandboxStageScripts PROPERTIES FOLDER "Games")
endif()

# === Content/ ===============================================================
# Imported .nmmesh / .nmtexture files, render targets (.nmrt2d / .nmrt3d), post process stacks (.pp.nmgraph)
# and baked lighting (.nmlighting),
# staged at the same relative path beside
# the executable (how a RenderComponent names them). Globbed, unlike Scripts/:
# the editor creates these, so listing them would mean editing this file on
# every import.
file(GLOB_RECURSE SANDBOX_CONTENT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmmesh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmtexture"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.pp.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt2d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmlighting"
)

if(SANDBOX_CONTENT_FILES)
    if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
        set(SANDBOX_CONTENT_OUTPUTS "")
        foreach(SANDBOX_CONTENT_SRC ${SANDBOX_CONTENT_FILES})
            file(RELATIVE_PATH SANDBOX_CONTENT_REL "${CMAKE_CURRENT_SOURCE_DIR}" "${SANDBOX_CONTENT_SRC}")
            set(SANDBOX_CONTENT_DST "${SANDBOX_RT_DIR}/${SANDBOX_CONTENT_REL}")
            get_filename_component(SANDBOX_CONTENT_DIR "${SANDBOX_CONTENT_DST}" DIRECTORY)

            add_custom_command(
                    OUTPUT  "${SANDBOX_CONTENT_DST}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${SANDBOX_CONTENT_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${SANDBOX_CONTENT_SRC}" "${SANDBOX_CONTENT_DST}"
                    DEPENDS "${SANDBOX_CONTENT_SRC}"
                    COMMENT "Sandbox: staging ${SANDBOX_CONTENT_REL} beside the executable"
                    VERBATIM)
            list(APPEND SANDBOX_CONTENT_OUTPUTS "${SANDBOX_CONTENT_DST}")
        endforeach()

        add_custom_target(SandboxStageContent DEPENDS ${SANDBOX_CONTENT_OUTPUTS})
        add_dependencies(Sandbox SandboxStageContent)
        set_target_properties(SandboxStageContent PROPERTIES FOLDER "Games")
    else()
        # Multi-config fallback, as SandboxStageScripts': the whole folder.
        add_custom_target(SandboxStageContent ALL
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${CMAKE_CURRENT_SOURCE_DIR}/Content" "$<TARGET_FILE_DIR:Sandbox>/Content"
                DEPENDS ${SANDBOX_CONTENT_FILES}
                COMMENT "Sandbox: staging Content/ beside the executable"
                VERBATIM)
        add_dependencies(SandboxStageContent Sandbox)
        set_target_properties(SandboxStageContent PROPERTIES FOLDER "Games")
    endif()
endif()

# === Content/Prefabs/ ======================================================
# Prefabs and the scripts beside them, staged at the same relative path beside
# the executable ("Content/Prefabs/Door.nmprefab", as SpawnPrefab and a level
# name them). Globbed, like Content/: the Prefab Editor makes these. An Editor
# target stages the text prefabs it edits; a Client or Server target cannot
# read text, so it stages the cooked ones from Cooked/Content/Prefabs/ -- made
# in the editor with the console command `cook prefabs`.
file(GLOB_RECURSE SANDBOX_PREFAB_SCRIPTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.script.nmgraph"
)
if(NIMBLE_TARGET STREQUAL "Editor")
    set(SANDBOX_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(SANDBOX_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Cooked")
endif()
file(GLOB_RECURSE SANDBOX_PREFAB_ASSETS CONFIGURE_DEPENDS "${SANDBOX_PREFAB_ROOT}/Content/Prefabs/*.nmprefab")
file(GLOB_RECURSE SANDBOX_PREFAB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.nmprefab")
if(NOT NIMBLE_TARGET STREQUAL "Editor" AND SANDBOX_PREFAB_SOURCES AND NOT SANDBOX_PREFAB_ASSETS)
    message(WARNING "Sandbox: Content/Prefabs/ has prefabs but Cooked/Content/Prefabs/ has none; a ${NIMBLE_TARGET} build cannot read "
                    "text prefabs. Cook them in the editor (console: cook prefabs).")
endif()

set(SANDBOX_PREFAB_OUTPUTS "")
foreach(SANDBOX_PREFAB_SRC ${SANDBOX_PREFAB_SCRIPTS} ${SANDBOX_PREFAB_ASSETS})
    if(SANDBOX_PREFAB_SRC IN_LIST SANDBOX_PREFAB_SCRIPTS)
        file(RELATIVE_PATH SANDBOX_PREFAB_REL "${CMAKE_CURRENT_SOURCE_DIR}" "${SANDBOX_PREFAB_SRC}")
    else()
        file(RELATIVE_PATH SANDBOX_PREFAB_REL "${SANDBOX_PREFAB_ROOT}" "${SANDBOX_PREFAB_SRC}")
    endif()
    if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
        set(SANDBOX_PREFAB_DST "${SANDBOX_RT_DIR}/${SANDBOX_PREFAB_REL}")
    else()
        set(SANDBOX_PREFAB_DST "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_PREFAB_REL}")
    endif()
    get_filename_component(SANDBOX_PREFAB_REL_DIR "${SANDBOX_PREFAB_REL}" DIRECTORY)
    list(APPEND SANDBOX_PREFAB_COPIES
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_PREFAB_REL_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SANDBOX_PREFAB_SRC}" "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_PREFAB_REL}")
    if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
        get_filename_component(SANDBOX_PREFAB_DIR "${SANDBOX_PREFAB_DST}" DIRECTORY)
        add_custom_command(
                OUTPUT  "${SANDBOX_PREFAB_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SANDBOX_PREFAB_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SANDBOX_PREFAB_SRC}" "${SANDBOX_PREFAB_DST}"
                DEPENDS "${SANDBOX_PREFAB_SRC}"
                COMMENT "Sandbox: staging ${SANDBOX_PREFAB_REL} beside the executable"
                VERBATIM)
        list(APPEND SANDBOX_PREFAB_OUTPUTS "${SANDBOX_PREFAB_DST}")
    endif()
endforeach()

if(SANDBOX_PREFAB_OUTPUTS)
    add_custom_target(SandboxStagePrefabs DEPENDS ${SANDBOX_PREFAB_OUTPUTS})
    add_dependencies(Sandbox SandboxStagePrefabs)
    set_target_properties(SandboxStagePrefabs PROPERTIES FOLDER "Games")
elseif(SANDBOX_PREFAB_COPIES)
    # Multi-config fallback, as SandboxStageScripts': every file, every build.
    add_custom_target(SandboxStagePrefabs ALL ${SANDBOX_PREFAB_COPIES}
            COMMENT "Sandbox: staging Content/Prefabs/ beside the executable"
            VERBATIM)
    add_dependencies(SandboxStagePrefabs Sandbox)
    set_target_properties(SandboxStagePrefabs PROPERTIES FOLDER "Games")
endif()

# === Content/UI/ ===========================================================
# UI documents (Engine/Modules/UI/README.md) and the Lua beside them, staged at
# the same relative path ("Content/UI/HUD.nmui", as GameSession::GetUI().Open
# names one). As prefabs: an Editor target stages the XML it edits; a Client
# or Server target cannot read XML, so it stages the cooked ones from
# Cooked/Content/UI/ -- made in the editor with the console command `cook ui`.
if(NIMBLE_TARGET STREQUAL "Editor")
    set(SANDBOX_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(SANDBOX_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Cooked")
endif()
file(GLOB_RECURSE SANDBOX_UI_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.ui.nmgraph")
file(GLOB_RECURSE SANDBOX_UI_DOCUMENTS CONFIGURE_DEPENDS "${SANDBOX_UI_ROOT}/Content/UI/*.nmui")
file(GLOB_RECURSE SANDBOX_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.nmui")
if(NOT NIMBLE_TARGET STREQUAL "Editor" AND SANDBOX_UI_SOURCES AND NOT SANDBOX_UI_DOCUMENTS)
    message(WARNING "Sandbox: Content/UI/ has documents but Cooked/Content/UI/ has none; a ${NIMBLE_TARGET} build cannot read "
                    "XML. Cook them in the editor (console: cook ui).")
endif()

set(SANDBOX_UI_OUTPUTS "")
set(SANDBOX_UI_COPIES "")
foreach(SANDBOX_UI_SRC ${SANDBOX_UI_SCRIPTS} ${SANDBOX_UI_DOCUMENTS})
    if(SANDBOX_UI_SRC IN_LIST SANDBOX_UI_SCRIPTS)
        file(RELATIVE_PATH SANDBOX_UI_REL "${CMAKE_CURRENT_SOURCE_DIR}" "${SANDBOX_UI_SRC}")
    else()
        file(RELATIVE_PATH SANDBOX_UI_REL "${SANDBOX_UI_ROOT}" "${SANDBOX_UI_SRC}")
    endif()
    get_filename_component(SANDBOX_UI_REL_DIR "${SANDBOX_UI_REL}" DIRECTORY)
    if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
        add_custom_command(
                OUTPUT  "${SANDBOX_RT_DIR}/${SANDBOX_UI_REL}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SANDBOX_RT_DIR}/${SANDBOX_UI_REL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SANDBOX_UI_SRC}" "${SANDBOX_RT_DIR}/${SANDBOX_UI_REL}"
                DEPENDS "${SANDBOX_UI_SRC}"
                COMMENT "Sandbox: staging ${SANDBOX_UI_REL} beside the executable"
                VERBATIM)
        list(APPEND SANDBOX_UI_OUTPUTS "${SANDBOX_RT_DIR}/${SANDBOX_UI_REL}")
    else()
        list(APPEND SANDBOX_UI_COPIES
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_UI_REL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SANDBOX_UI_SRC}" "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_UI_REL}")
    endif()
endforeach()

if(SANDBOX_UI_OUTPUTS)
    add_custom_target(SandboxStageUI DEPENDS ${SANDBOX_UI_OUTPUTS})
    add_dependencies(Sandbox SandboxStageUI)
    set_target_properties(SandboxStageUI PROPERTIES FOLDER "Games")
elseif(SANDBOX_UI_COPIES)
    # Multi-config fallback, as SandboxStagePrefabs': every file, every build.
    add_custom_target(SandboxStageUI ALL ${SANDBOX_UI_COPIES} COMMENT "Sandbox: staging Content/UI/ beside the executable" VERBATIM)
    add_dependencies(SandboxStageUI Sandbox)
    set_target_properties(SandboxStageUI PROPERTIES FOLDER "Games")
endif()

# === Content/Levels/ =======================================================
# Level files, staged at the same relative path beside the executable
# ("Content/Levels/BrickBreaker.nmworld", as a level loads it). An Editor target
# stages the text worlds -- the .nmworld manifest and the per-entity and
# per-instance files in the folder beside it; a Client or Server target cannot
# read text, so it stages the cooked ones from Content/Levels/Cooked/ (the
# editor's `cook` console command).
if(NIMBLE_TARGET STREQUAL "Editor")
    file(GLOB_RECURSE SANDBOX_LEVEL_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/*.nmworld"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/*.nmentity"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/*.nmprefabinst")
    list(FILTER SANDBOX_LEVEL_FILES EXCLUDE REGEX "/Content/Levels/Cooked/")
else()
    file(GLOB_RECURSE SANDBOX_LEVEL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/Cooked/*.nmworld")
endif()

set(SANDBOX_LEVEL_OUTPUTS "")
set(SANDBOX_LEVEL_COPIES "")
foreach(SANDBOX_LEVEL_SRC ${SANDBOX_LEVEL_FILES})
    file(RELATIVE_PATH SANDBOX_LEVEL_REL "${CMAKE_CURRENT_SOURCE_DIR}" "${SANDBOX_LEVEL_SRC}")
    get_filename_component(SANDBOX_LEVEL_REL_DIR "${SANDBOX_LEVEL_REL}" DIRECTORY)
    list(APPEND SANDBOX_LEVEL_COPIES
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_LEVEL_REL_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SANDBOX_LEVEL_SRC}" "$<TARGET_FILE_DIR:Sandbox>/${SANDBOX_LEVEL_REL}")
    if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
        set(SANDBOX_LEVEL_DST "${SANDBOX_RT_DIR}/${SANDBOX_LEVEL_REL}")
        add_custom_command(
                OUTPUT  "${SANDBOX_LEVEL_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SANDBOX_RT_DIR}/${SANDBOX_LEVEL_REL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SANDBOX_LEVEL_SRC}" "${SANDBOX_LEVEL_DST}"
                DEPENDS "${SANDBOX_LEVEL_SRC}"
                COMMENT "Sandbox: staging ${SANDBOX_LEVEL_REL} beside the executable"
                VERBATIM)
        list(APPEND SANDBOX_LEVEL_OUTPUTS "${SANDBOX_LEVEL_DST}")
    endif()
endforeach()

if(SANDBOX_LEVEL_OUTPUTS)
    add_custom_target(SandboxStageLevels DEPENDS ${SANDBOX_LEVEL_OUTPUTS})
    add_dependencies(Sandbox SandboxStageLevels)
    set_target_properties(SandboxStageLevels PROPERTIES FOLDER "Games")
elseif(SANDBOX_LEVEL_COPIES)
    # Multi-config fallback, as SandboxStagePrefabs': every file, every build.
    add_custom_target(SandboxStageLevels ALL ${SANDBOX_LEVEL_COPIES}
            COMMENT "Sandbox: staging Content/Levels/ beside the executable"
            VERBATIM)
    add_dependencies(SandboxStageLevels Sandbox)
    set_target_properties(SandboxStageLevels PROPERTIES FOLDER "Games")
endif()

# === Content/Materials/ =====================================================
# Material graphs, cooked from the code generated into each (<gen_code>) for
# every built backend into SandboxGame's shader cache, which GameModuleHost
# mounts with the module -- and, in Editor targets, staged loose as well
# (Engine/cmake/NimbleMaterials.cmake). Named by path below this folder:
# "Content/Materials/Glow.material.nmgraph".
if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
    nimble_stage_materials(TARGET Sandbox DIR "${CMAKE_CURRENT_SOURCE_DIR}/Content/Materials"
                           ROOT "${CMAKE_CURRENT_SOURCE_DIR}" MODULE SandboxGame RUNTIME_DIR "${SANDBOX_RT_DIR}")
else()
    nimble_stage_materials(TARGET Sandbox DIR "${CMAKE_CURRENT_SOURCE_DIR}/Content/Materials"
                           ROOT "${CMAKE_CURRENT_SOURCE_DIR}" MODULE SandboxGame)
endif()

# === IDE ===================================================================
# The debugger's working directory is the executable's, where Config/ is
# staged (nimble_add_game_launcher): without it a Visual Studio or CLion run
# starts in the build directory and silently runs on defaults.

# Only standalone: in a full Engine configure the startup project belongs to
# whoever configured it, and quietly repointing it at a game would be rude.
if(SANDBOX_STANDALONE)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Sandbox)
endif()

source_group("Sandbox" FILES ${SANDBOX_GAME_SOURCES})

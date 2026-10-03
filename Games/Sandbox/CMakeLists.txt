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
set(SANDBOX_LAUNCHER_SOURCES
        Source/Private/Sandbox/SandboxMain.cpp
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
# Thin executable (Application::ExecGame). add_dependencies only ensures the
# game module is built; it is not linked.
add_executable(Sandbox ${SANDBOX_LAUNCHER_SOURCES})
add_dependencies(Sandbox SandboxGame)

# The editor loads this game at Play, so building the editor builds it too.
if(TARGET NimbleEditor)
    add_dependencies(NimbleEditor SandboxGame)

    # Edit > Project Settings edits this game's master config, not the staged
    # copy (see the Editor's CMakeLists.txt).
    set_property(TARGET NimbleEditor PROPERTY
            NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
endif()

# Same Qt-freedom the modules get from nimble_add_module(). Stated explicitly
# because add_executable() does not go through that function, and a parent scope
# that enabled AUTOMOC as a directory property would hand it down.
set_target_properties(Sandbox PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# One link is enough: nimble_add_module() gives NimbleEngine a PUBLIC include
# directory, PUBLIC cxx_std_17 and a PUBLIC EngineCore, so all three arrive
# transitively. GameWindow and GDI stay PRIVATE to the engine layer and are
# deliberately NOT visible here -- a game links the engine, not the renderer.
target_link_libraries(Sandbox PRIVATE NimbleEngine::NimbleEngine)

# === Runtime layout ========================================================
# Everything lands in Binaries/<Platform>_<Config> BELOW THIS DIRECTORY: the
# engine DLLs, the module DLLs and this executable all together, because hot
# reload requires them to be siblings and because the one directory that holds
# them belongs to the host being run. Engine/CMakeLists.txt's NIMBLE_BINARIES_DIR
# defaults here for that reason; a second game overrides it to claim its own.
#
# Engine/CMakeLists.txt sets CMAKE_RUNTIME_OUTPUT_DIRECTORY from that root at ITS
# scope, and that is only inherited by targets created at or below it. Added from
# Engine (path 1) we are below it and inherit for free.
#
# Standalone (path 2) we are ABOVE it -- the engine is our subdirectory -- so we
# inherit nothing and would land in the build tree, away from NimbleEngine.dll,
# EngineCore.dll and SDL3.dll. On Windows the loader resolves those from the
# directory of the EXE, so that build would configure, compile, link, and then
# fail to start.
#
# The engine's own target already knows the answer, so ask it rather than
# recomputing the <Platform>_<Config> string here -- a second copy of that logic
# is a second thing to keep in step.

# Needed on both paths below, so it is read once here rather than inside either.
get_property(SANDBOX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(SANDBOX_STANDALONE)
    get_target_property(SANDBOX_RT_DIR NimbleEngine RUNTIME_OUTPUT_DIRECTORY)
    if(SANDBOX_RT_DIR)
        set_target_properties(Sandbox PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SANDBOX_RT_DIR}")
        # The launcher looks for the module beside itself.
        set_target_properties(SandboxGame PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${SANDBOX_RT_DIR}")
    endif()

    # Multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) ignore
    # the plain property and read the per-config one, which the engine also has.
    if(SANDBOX_MULTI_CONFIG)
        foreach(SANDBOX_CFG IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${SANDBOX_CFG}" SANDBOX_CFG_U)
            get_target_property(SANDBOX_RT_CFG NimbleEngine
                                RUNTIME_OUTPUT_DIRECTORY_${SANDBOX_CFG_U})
            if(SANDBOX_RT_CFG)
                set_target_properties(Sandbox PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY_${SANDBOX_CFG_U} "${SANDBOX_RT_CFG}")
                set_target_properties(SandboxGame PROPERTIES
                        LIBRARY_OUTPUT_DIRECTORY_${SANDBOX_CFG_U} "${SANDBOX_RT_CFG}")
            endif()
        endforeach()
    endif()
else()
    # Added from Engine, we inherited the variable itself. Captured under the
    # same name so the config staging below has one plain path to work with
    # whichever way we were configured.
    set(SANDBOX_RT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

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
# Imported .nmmesh / .nmtexture files, staged at the same relative path beside
# the executable (how a RenderComponent names them). Globbed, unlike Scripts/:
# the editor creates these, so listing them would mean editing this file on
# every import.
file(GLOB_RECURSE SANDBOX_CONTENT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmmesh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmtexture"
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

# === Prefabs/ ==============================================================
# Prefabs and the scripts beside them, staged at the same relative path beside
# the executable ("Prefabs/Door.nmprefab", as SpawnPrefab and a level name
# them). Globbed, like Content/: the Prefab Editor makes these. An Editor
# target stages the text prefabs it edits; a Client or Server target cannot
# read text, so it stages the cooked ones from Cooked/Prefabs/ -- made in the
# editor with the console command `cook prefabs`.
file(GLOB_RECURSE SANDBOX_PREFAB_SCRIPTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Prefabs/*.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Prefabs/*.script.nmgraph"
)
if(NIMBLE_TARGET STREQUAL "Editor")
    set(SANDBOX_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(SANDBOX_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Cooked")
endif()
file(GLOB_RECURSE SANDBOX_PREFAB_ASSETS CONFIGURE_DEPENDS "${SANDBOX_PREFAB_ROOT}/Prefabs/*.nmprefab")
file(GLOB_RECURSE SANDBOX_PREFAB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Prefabs/*.nmprefab")
if(NOT NIMBLE_TARGET STREQUAL "Editor" AND SANDBOX_PREFAB_SOURCES AND NOT SANDBOX_PREFAB_ASSETS)
    message(WARNING "Sandbox: Prefabs/ has prefabs but Cooked/Prefabs/ has none; a ${NIMBLE_TARGET} build cannot read "
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
            COMMENT "Sandbox: staging Prefabs/ beside the executable"
            VERBATIM)
    add_dependencies(SandboxStagePrefabs Sandbox)
    set_target_properties(SandboxStagePrefabs PROPERTIES FOLDER "Games")
endif()

# === Materials/ =============================================================
# Material graphs, cooked from the code generated into each (<gen_code>) for
# every built backend into SandboxGame's shader cache, which GameModuleHost
# mounts with the module -- and, in Editor targets, staged loose as well
# (Engine/cmake/NimbleMaterials.cmake). Named by path below this folder:
# "Materials/Glow.material.nmgraph".
if(SANDBOX_RT_DIR AND NOT SANDBOX_MULTI_CONFIG)
    nimble_stage_materials(TARGET Sandbox DIR "${CMAKE_CURRENT_SOURCE_DIR}/Materials" MODULE SandboxGame
                           RUNTIME_DIR "${SANDBOX_RT_DIR}")
else()
    nimble_stage_materials(TARGET Sandbox DIR "${CMAKE_CURRENT_SOURCE_DIR}/Materials" MODULE SandboxGame)
endif()

# === IDE ===================================================================
# Without this, a Visual Studio or CLion run starts in the build directory and
# the config lookup above silently misses -- the app still runs, on defaults,
# which is the kind of difference between "ran from the IDE" and "ran from the
# shell" that costs an afternoon.
set_target_properties(Sandbox PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Sandbox>"
        FOLDER "Games")

# Only standalone: in a full Engine configure the startup project belongs to
# whoever configured it, and quietly repointing it at a game would be rude.
if(SANDBOX_STANDALONE)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Sandbox)
endif()

source_group("Sandbox" FILES ${SANDBOX_GAME_SOURCES} ${SANDBOX_LAUNCHER_SOURCES})

cmake_minimum_required(VERSION 3.16)

project(LightLab VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# LightLab -- the lights and shadows demo.
#
# A walled yard of boxes and spheres lit by every light the engine has -- a
# cascaded sun, a sky light, two shadowed spots, a modulated point light and an
# unshadowed coloured one -- and a free-flying camera to look at them with.
# Like Sandbox it consumes the engine through the public API only.
#
# CONFIGURES TWO WAYS, exactly as Games/Sandbox does (see the top of its
# CMakeLists.txt for the full reasoning):
#
#   1. FROM ENGINE. Engine/CMakeLists.txt add_subdirectory()s this when
#      NIMBLE_BUILD_LIGHTLAB is on.
#
#   2. STANDALONE. Open Games/LightLab directly in an IDE, or point cmake at it
#      (CMakePresets.json). This directory is then the top-level project and
#      pulls the Engine tree in itself.
# ===========================================================================

# C++17, exactly -- see Engine/CMakeLists.txt. Repeated here because a
# standalone configure creates this directory's targets in a scope the Engine
# tree's setting does not reach.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LIGHTLAB_STANDALONE ON)
else()
    set(LIGHTLAB_STANDALONE OFF)
endif()

# === Standalone: bring the engine in ourselves =============================
if(LIGHTLAB_STANDALONE)
    message(STATUS "LightLab: standalone configure -- adding the Engine tree")

    # A cache entry rather than a hardcoded path, so a checkout with a different
    # layout can be pointed at its engine without editing this file.
    set(LIGHTLAB_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine"
        CACHE PATH "Path to the Engine tree LightLab links against")

    if(NOT EXISTS "${LIGHTLAB_ENGINE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "LightLab: no Engine/CMakeLists.txt at '${LIGHTLAB_ENGINE_DIR}'. "
            "Set LIGHTLAB_ENGINE_DIR to the Engine tree, or configure the Engine "
            "directory instead and let it add this one.")
    endif()

    # MUST come before add_subdirectory. Engine/CMakeLists.txt adds the Games/
    # hosts itself when their options are on: Sandbox would be a second game in
    # this build, and LightLab would add THIS directory a second time -- a
    # duplicate target. option() leaves an existing cache entry alone, which is
    # what makes setting these here effective.
    set(NIMBLE_BUILD_SANDBOX OFF CACHE BOOL
        "Build the Games/Sandbox host (off here: LightLab is the top-level project)" FORCE)
    set(NIMBLE_BUILD_LIGHTLAB OFF CACHE BOOL
        "Build the Games/LightLab host (off here: LightLab IS the top-level project)" FORCE)

    # Claims the runtime layout for this host. Engine/CMakeLists.txt defaults
    # NIMBLE_BINARIES_DIR to Sandbox's Binaries; its set(... CACHE) leaves an
    # existing entry alone, so this one wins. Not FORCE, so -D still overrides.
    set(NIMBLE_BINARIES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
        CACHE PATH "Root of the flat runtime layout; <Platform>_<Config> is created below it")

    # Explicit binary dir because the source dir is outside this one.
    add_subdirectory("${LIGHTLAB_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/Engine")
endif()

if(NOT TARGET NimbleEngine)
    message(FATAL_ERROR
        "LightLab: the NimbleEngine target does not exist. When added from "
        "Engine/CMakeLists.txt this directory must come AFTER the "
        "nimble_add_module(NimbleEngine ...) call that creates it.")
endif()

# === The game module =======================================================
# LightLabGame.cpp as a module DLL, loaded by name (NimbleEngine/GameModule.h)
# by the launcher and the editor's Play-in-Editor, never linked.
nimble_add_hot_module(LightLabGame
        SOURCES      LightLabGame.cpp LightLabInput.cpp
        PRIVATE_DEPS NimbleEngine::NimbleEngine)
set_target_properties(LightLabGame PROPERTIES FOLDER "Games")

# === The launcher ==========================================================
# Thin executable (Application::ExecGame). add_dependencies only ensures the
# game module is built; it is not linked.
add_executable(LightLab LightLabMain.cpp)
add_dependencies(LightLab LightLabGame)

# The editor loads this game at Play, so building the editor builds it too.
if(TARGET NimbleEditor)
    add_dependencies(NimbleEditor LightLabGame)

    # Edit > Project Settings edits this game's master config, not the staged
    # copy (see the Editor's CMakeLists.txt).
    set_property(TARGET NimbleEditor PROPERTY
            NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
endif()

# Same Qt-freedom the modules get from nimble_add_module().
set_target_properties(LightLab PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

target_link_libraries(LightLab PRIVATE NimbleEngine::NimbleEngine)

# === Runtime layout ========================================================
# Everything lands in Binaries/<Platform>_<Config> beside the engine's own
# DLLs; see Games/Sandbox/CMakeLists.txt for why. Standalone we are ABOVE the
# engine's scope and inherit nothing, so ask its target where it went.
get_property(LIGHTLAB_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(LIGHTLAB_STANDALONE)
    get_target_property(LIGHTLAB_RT_DIR NimbleEngine RUNTIME_OUTPUT_DIRECTORY)
    if(LIGHTLAB_RT_DIR)
        set_target_properties(LightLab PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${LIGHTLAB_RT_DIR}")
        # The launcher looks for the module beside itself.
        set_target_properties(LightLabGame PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${LIGHTLAB_RT_DIR}")
    endif()

    # Multi-config generators ignore the plain property and read the per-config one.
    if(LIGHTLAB_MULTI_CONFIG)
        foreach(LIGHTLAB_CFG IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${LIGHTLAB_CFG}" LIGHTLAB_CFG_U)
            get_target_property(LIGHTLAB_RT_CFG NimbleEngine
                                RUNTIME_OUTPUT_DIRECTORY_${LIGHTLAB_CFG_U})
            if(LIGHTLAB_RT_CFG)
                set_target_properties(LightLab PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY_${LIGHTLAB_CFG_U} "${LIGHTLAB_RT_CFG}")
                set_target_properties(LightLabGame PROPERTIES
                        LIBRARY_OUTPUT_DIRECTORY_${LIGHTLAB_CFG_U} "${LIGHTLAB_RT_CFG}")
            endif()
        endforeach()
    endif()
else()
    # Added from Engine, we inherited the variable itself.
    set(LIGHTLAB_RT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# === Config staging ========================================================
# Application::Initialize() reads "Config/DefaultEngine.ini" relative to the
# working directory, so it is staged beside the executable. A real OUTPUT with
# a real DEPENDS rather than POST_BUILD, which goes stale when only the .ini
# changes -- Games/Sandbox/CMakeLists.txt has the history.
set(LIGHTLAB_CONFIG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")

if(EXISTS "${LIGHTLAB_CONFIG_SRC}" AND LIGHTLAB_RT_DIR AND NOT LIGHTLAB_MULTI_CONFIG)
    set(LIGHTLAB_CONFIG_DST "${LIGHTLAB_RT_DIR}/Config/DefaultEngine.ini")

    add_custom_command(
            OUTPUT  "${LIGHTLAB_CONFIG_DST}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${LIGHTLAB_RT_DIR}/Config"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LIGHTLAB_CONFIG_SRC}" "${LIGHTLAB_CONFIG_DST}"
            DEPENDS "${LIGHTLAB_CONFIG_SRC}"
            COMMENT "LightLab: staging Config/DefaultEngine.ini beside the executable"
            VERBATIM)

    add_custom_target(LightLabStageConfig DEPENDS "${LIGHTLAB_CONFIG_DST}")
    add_dependencies(LightLab LightLabStageConfig)
    set_target_properties(LightLabStageConfig PROPERTIES FOLDER "Games")

elseif(EXISTS "${LIGHTLAB_CONFIG_SRC}")
    # Multi-config fallback: one OUTPUT cannot name a per-config destination.
    add_custom_target(LightLabStageConfig ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:LightLab>/Config"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LIGHTLAB_CONFIG_SRC}"
                    "$<TARGET_FILE_DIR:LightLab>/Config/DefaultEngine.ini"
            DEPENDS "${LIGHTLAB_CONFIG_SRC}"
            COMMENT "LightLab: staging Config/DefaultEngine.ini beside the executable"
            VERBATIM)
    add_dependencies(LightLabStageConfig LightLab)
    set_target_properties(LightLabStageConfig PROPERTIES FOLDER "Games")
endif()

# === Levels/ ===============================================================
# Levels/LightLab.nmworld, as the editor saves it: the manifest and its folder
# of .nmentity files, staged at the same path beside the executable, where
# LightLabLevel::OnLoad loads it (the editor also re-stages it on every Save).
# A Client/Server build reads only cooked worlds, so it stages
# Levels/Cooked/<Name>.nmworld (the editor's 'cook' console command) as
# Levels/<Name>.nmworld when there is one; without one the level is built in
# code, the same scene.
if(NIMBLE_TARGET STREQUAL "Editor")
    file(GLOB_RECURSE LIGHTLAB_LEVEL_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Levels/*.nmworld"
            "${CMAKE_CURRENT_SOURCE_DIR}/Levels/*.nmentity")
    list(FILTER LIGHTLAB_LEVEL_FILES EXCLUDE REGEX "/Levels/Cooked/")
    set(LIGHTLAB_LEVEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    file(GLOB LIGHTLAB_LEVEL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Levels/Cooked/*.nmworld")
    set(LIGHTLAB_LEVEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Levels/Cooked")
endif()

set(LIGHTLAB_LEVEL_OUTPUTS "")
set(LIGHTLAB_LEVEL_COPIES "")
foreach(LIGHTLAB_LEVEL_SRC ${LIGHTLAB_LEVEL_FILES})
    file(RELATIVE_PATH LIGHTLAB_LEVEL_REL "${LIGHTLAB_LEVEL_ROOT}" "${LIGHTLAB_LEVEL_SRC}")
    if(NOT NIMBLE_TARGET STREQUAL "Editor")
        set(LIGHTLAB_LEVEL_REL "Levels/${LIGHTLAB_LEVEL_REL}")
    endif()
    get_filename_component(LIGHTLAB_LEVEL_REL_DIR "${LIGHTLAB_LEVEL_REL}" DIRECTORY)
    if(LIGHTLAB_RT_DIR AND NOT LIGHTLAB_MULTI_CONFIG)
        set(LIGHTLAB_LEVEL_DST "${LIGHTLAB_RT_DIR}/${LIGHTLAB_LEVEL_REL}")
        add_custom_command(
                OUTPUT  "${LIGHTLAB_LEVEL_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${LIGHTLAB_RT_DIR}/${LIGHTLAB_LEVEL_REL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIGHTLAB_LEVEL_SRC}" "${LIGHTLAB_LEVEL_DST}"
                DEPENDS "${LIGHTLAB_LEVEL_SRC}"
                COMMENT "LightLab: staging ${LIGHTLAB_LEVEL_REL} beside the executable"
                VERBATIM)
        list(APPEND LIGHTLAB_LEVEL_OUTPUTS "${LIGHTLAB_LEVEL_DST}")
    else()
        list(APPEND LIGHTLAB_LEVEL_COPIES
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LightLab>/${LIGHTLAB_LEVEL_REL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIGHTLAB_LEVEL_SRC}" "$<TARGET_FILE_DIR:LightLab>/${LIGHTLAB_LEVEL_REL}")
    endif()
endforeach()
if(LIGHTLAB_LEVEL_OUTPUTS)
    add_custom_target(LightLabStageLevels DEPENDS ${LIGHTLAB_LEVEL_OUTPUTS})
    add_dependencies(LightLab LightLabStageLevels)
    if(TARGET NimbleEditor)
        add_dependencies(NimbleEditor LightLabStageLevels)   # Play-in-Editor loads the staged copy
    endif()
    set_target_properties(LightLabStageLevels PROPERTIES FOLDER "Games")
elseif(LIGHTLAB_LEVEL_COPIES)
    add_custom_target(LightLabStageLevels ALL ${LIGHTLAB_LEVEL_COPIES}
            COMMENT "LightLab: staging Levels/ beside the executable"
            VERBATIM)
    add_dependencies(LightLabStageLevels LightLab)
    set_target_properties(LightLabStageLevels PROPERTIES FOLDER "Games")
endif()

# No Scripts/, Prefabs/, Content/ or Materials/ to stage: its prefabs are
# native, and it draws the engine's own meshes with DefaultLit -- all staged
# by Engine/CMakeLists.txt.

# === IDE ===================================================================
# Without this an IDE run starts in the build directory and misses the config.
set_target_properties(LightLab PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:LightLab>"
        FOLDER "Games")

# Only standalone: in a full Engine configure the startup project belongs to
# whoever configured it.
if(LIGHTLAB_STANDALONE)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT LightLab)
endif()

source_group("LightLab" FILES LightLabMain.cpp LightLabGame.cpp LightLabInput.cpp)

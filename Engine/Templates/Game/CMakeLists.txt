cmake_minimum_required(VERSION 3.16)

project(@PROJECT_NAME@ VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# @PROJECT_NAME@ -- a NimbleEngine game, made by the editor's File > New
# Project from Engine/Templates/Game. Games/Sandbox is the full example of
# everything a game can do; this is the least that runs.
#
# CONFIGURES STANDALONE: open this directory in an IDE, or
#
#     cmake --preset editor-debug
#     cmake --build --preset editor-debug
#
# It adds the Engine tree itself, so nothing under Engine/ names this game and
# the engine is never edited to add one.
#
#   @PROJECT_NAME@Game   the game module (@PROJECT_NAME@Game.cpp): level, mode, state
#   @PROJECT_NAME@       the launcher: the engine's, built under this name
#   NimbleEditor         the editor, editing this project (Editor targets)
#
# Everything runs from Binaries/<Platform>_<Config>[_<Target>] below this
# directory: the engine and game libraries, and staged copies of Config/,
# Scripts/ and Content/ at the same relative paths.
# ===========================================================================

# C++17, exactly -- see Engine/CMakeLists.txt.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GAME_NAME   "@PROJECT_NAME@")
set(GAME_MODULE "@PROJECT_NAME@Game")

# === The engine =============================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(GAME_ENGINE_DIR "@ENGINE_DIR@" CACHE PATH "The Engine tree ${GAME_NAME} builds against")
    if(NOT EXISTS "${GAME_ENGINE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "${GAME_NAME}: no Engine/CMakeLists.txt at '${GAME_ENGINE_DIR}'. Set GAME_ENGINE_DIR to the Engine tree.")
    endif()

    # This game's own runtime directory. The engine's default is Sandbox's
    # Binaries; one directory belongs to one host (Engine/CMakeLists.txt).
    set(NIMBLE_BINARIES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
        CACHE PATH "Root of the flat runtime layout; <Platform>_<Config> is created below it")

    # Off before the engine is added: it would add Games/Sandbox to this build.
    set(NIMBLE_BUILD_SANDBOX OFF CACHE BOOL "Build the Games/Sandbox host (off: ${GAME_NAME} is the top-level project)" FORCE)

    add_subdirectory("${GAME_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/Engine")
endif()

if(NOT TARGET NimbleEngine)
    message(FATAL_ERROR "${GAME_NAME}: the NimbleEngine target does not exist; configure this directory as the top-level project.")
endif()

# === The game module ========================================================
# Loaded by name (NimbleEngine/GameModule.h) by the launcher and the editor's
# Play-in-Editor, never linked. Add new sources here.
nimble_add_hot_module(${GAME_MODULE}
        SOURCES      @PROJECT_NAME@Game.cpp
        PRIVATE_DEPS NimbleEngine::NimbleEngine)
set_target_properties(${GAME_MODULE} PROPERTIES FOLDER "Games")

# === The launcher ===========================================================
# The engine's own (Engine/Source/Launcher/GameLauncherMain.cpp), built as
# ${GAME_NAME}: it runs [Game] Module, ${GAME_MODULE} if unset. It also puts the
# executable and the module beside the engine libraries, with the debugger
# starting there. A game that needs its own main() passes SOURCES <file>
# (Engine/cmake/NimbleModule.cmake).
nimble_add_game_launcher(${GAME_NAME} MODULE ${GAME_MODULE})

# === The dedicated server ===================================================
# ${GAME_NAME}Server: the same module, headless, as a network session's
# authority (Engine/Source/Launcher/ServerLauncherMain.cpp). Skipped when the
# engine is configured without networking. A game that never networks can
# drop this line and, before the engine is added above, turn NetCore off:
#
#     set(NIMBLE_WITH_NETCORE OFF CACHE BOOL "" FORCE)
nimble_add_server_launcher(${GAME_NAME}Server MODULE ${GAME_MODULE})

# The editor loads this game at Play, and edits this project's master config.
if(TARGET NimbleEditor)
    add_dependencies(NimbleEditor ${GAME_MODULE})
    set_property(TARGET NimbleEditor PROPERTY
            NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
endif()

# === Runtime layout =========================================================
# Where the launcher put the executable, beside the engine libraries; the
# staging below copies Config/, Scripts/ and Content/ there.
get_property(GAME_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
get_target_property(GAME_RT_DIR NimbleEngine RUNTIME_OUTPUT_DIRECTORY)

# === Staging ================================================================
# game_stage(<Name> <root> <files...>): each file copied beside the executable
# at its path below <root> ("Scripts/Main.lua"), which is how the runtime names
# it. A real output per file, so an edit restages without a relink; staged
# before the game module, so building the editor alone stages too. Multi-config
# generators have no single destination, so they copy after the launcher.
function(game_stage STAGE_NAME STAGE_ROOT)
    set(STAGE_OUTPUTS)
    set(STAGE_COPIES)
    foreach(STAGE_SRC ${ARGN})
        file(RELATIVE_PATH STAGE_REL "${STAGE_ROOT}" "${STAGE_SRC}")
        get_filename_component(STAGE_REL_DIR "${STAGE_REL}" DIRECTORY)
        if(GAME_RT_DIR AND NOT GAME_MULTI_CONFIG)
            add_custom_command(
                    OUTPUT  "${GAME_RT_DIR}/${STAGE_REL}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAME_RT_DIR}/${STAGE_REL_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STAGE_SRC}" "${GAME_RT_DIR}/${STAGE_REL}"
                    DEPENDS "${STAGE_SRC}"
                    COMMENT "${GAME_NAME}: staging ${STAGE_REL} beside the executable"
                    VERBATIM)
            list(APPEND STAGE_OUTPUTS "${GAME_RT_DIR}/${STAGE_REL}")
        else()
            list(APPEND STAGE_COPIES
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${GAME_NAME}>/${STAGE_REL_DIR}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STAGE_SRC}" "$<TARGET_FILE_DIR:${GAME_NAME}>/${STAGE_REL}")
        endif()
    endforeach()

    set(STAGE_TARGET ${GAME_NAME}Stage${STAGE_NAME})
    if(STAGE_OUTPUTS)
        add_custom_target(${STAGE_TARGET} DEPENDS ${STAGE_OUTPUTS})
        add_dependencies(${GAME_MODULE} ${STAGE_TARGET})
    elseif(STAGE_COPIES)
        add_custom_target(${STAGE_TARGET} ALL ${STAGE_COPIES} COMMENT "${GAME_NAME}: staging ${STAGE_NAME}" VERBATIM)
        add_dependencies(${STAGE_TARGET} ${GAME_NAME})
    else()
        return()
    endif()
    set_target_properties(${STAGE_TARGET} PROPERTIES FOLDER "Games")
endfunction()

# Config/: Application::Initialize() reads Config/DefaultEngine.ini. The copy
# here is the master; the staged one is overwritten by the next build.
file(GLOB GAME_CONFIG_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Config/*.ini")
game_stage(Config "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_CONFIG_FILES})

# Scripts/: Lua and script graphs (the runtime runs a graph's stored Lua).
file(GLOB_RECURSE GAME_SCRIPT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/*.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/*.script.nmgraph")
game_stage(Scripts "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_SCRIPT_FILES})

# Content/: imported meshes and textures ("Content/Meshes/Crate.nmmesh"), and
# post process stacks (a Post Process component's "Content/PostProcess/X.pp.nmgraph", render targets (.nmrt2d / .nmrt3d), particle systems (.nmparticle),
# baked lighting (a world's Lighting/ folder: .nmlighting and its lightmaps), read as they are, text in every build).
file(GLOB_RECURSE GAME_CONTENT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmmesh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmtexture"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.pp.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt2d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmparticle"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmlighting")
game_stage(Content "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_CONTENT_FILES})

# Content/Prefabs/: every build but Shipping stages the text prefabs -- an
# Editor edits them, a Client or Server cooks each the first time it loads it
# (NIMBLE_UNCOOKED_CONTENT); Shipping stages the cooked ones from
# Cooked/Content/Prefabs/ -- made in the editor with the console's `cook prefabs`.
file(GLOB_RECURSE GAME_PREFAB_SCRIPTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.script.nmgraph")
game_stage(PrefabScripts "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_PREFAB_SCRIPTS})

if(NIMBLE_UNCOOKED_CONTENT)
    set(GAME_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(GAME_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Cooked")
endif()
file(GLOB_RECURSE GAME_PREFAB_FILES CONFIGURE_DEPENDS "${GAME_PREFAB_ROOT}/Content/Prefabs/*.nmprefab")
file(GLOB_RECURSE GAME_PREFAB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.nmprefab")
if(NOT NIMBLE_UNCOOKED_CONTENT AND GAME_PREFAB_SOURCES AND NOT GAME_PREFAB_FILES)
    message(WARNING "${GAME_NAME}: Content/Prefabs/ has prefabs but Cooked/Content/Prefabs/ has none; a Shipping "
                    "build cannot read text prefabs. Cook them in the editor (console: cook prefabs).")
endif()
game_stage(Prefabs "${GAME_PREFAB_ROOT}" ${GAME_PREFAB_FILES})

# Content/UI/: UI documents (Engine/Modules/UI/README.md) and the Lua beside
# them. As prefabs: an Editor target stages the XML it edits; a Client or
# Server target cannot read XML, so it stages the cooked ones from
# Cooked/Content/UI/ -- made in the editor with the console's `cook ui`.
file(GLOB_RECURSE GAME_UI_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.lua")
game_stage(UIScripts "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_UI_SCRIPTS})

if(NIMBLE_TARGET STREQUAL "Editor")
    set(GAME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(GAME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Cooked")
endif()
file(GLOB_RECURSE GAME_UI_FILES CONFIGURE_DEPENDS "${GAME_UI_ROOT}/Content/UI/*.nmui")
file(GLOB_RECURSE GAME_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.nmui")
if(NOT NIMBLE_TARGET STREQUAL "Editor" AND GAME_UI_SOURCES AND NOT GAME_UI_FILES)
    message(WARNING "${GAME_NAME}: Content/UI/ has documents but Cooked/Content/UI/ has none; a ${NIMBLE_TARGET} "
                    "build cannot read XML. Cook them in the editor (console: cook ui).")
endif()
game_stage(UI "${GAME_UI_ROOT}" ${GAME_UI_FILES})

# Content/Materials/: material graphs, cooked into the module's shader cache
# (Engine/cmake/NimbleMaterials.cmake), named "Content/Materials/<Name>.material.nmgraph".
if(GAME_RT_DIR AND NOT GAME_MULTI_CONFIG)
    nimble_stage_materials(TARGET ${GAME_NAME} DIR "${CMAKE_CURRENT_SOURCE_DIR}/Content/Materials"
                           ROOT "${CMAKE_CURRENT_SOURCE_DIR}" MODULE ${GAME_MODULE} RUNTIME_DIR "${GAME_RT_DIR}")
    if(TARGET ${GAME_NAME}StageMaterials)
        add_dependencies(${GAME_MODULE} ${GAME_NAME}StageMaterials)   # the editor's build cooks them too
    endif()
else()
    nimble_stage_materials(TARGET ${GAME_NAME} DIR "${CMAKE_CURRENT_SOURCE_DIR}/Content/Materials"
                           ROOT "${CMAKE_CURRENT_SOURCE_DIR}" MODULE ${GAME_MODULE})
endif()

# Content/Levels/: worlds saved from the editor, which a level loads by path
# (GameCore::LoadContentWorld("Content/Levels/Arena.nmworld")). Every build but
# Shipping stages the text worlds -- the .nmworld and the folder of entity and
# instance files beside it -- which an Editor edits and a Client or Server
# cooks the first time it loads one; Shipping, the cooked ones from
# Content/Levels/Cooked/ (the editor's console: cook <world>).
if(NIMBLE_UNCOOKED_CONTENT)
    file(GLOB_RECURSE GAME_LEVEL_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/*.nmworld"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/*.nmentity"
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/*.nmprefabinst")
    list(FILTER GAME_LEVEL_FILES EXCLUDE REGEX "/Content/Levels/Cooked/")
else()
    file(GLOB_RECURSE GAME_LEVEL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Levels/Cooked/*.nmworld")
endif()
game_stage(Levels "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_LEVEL_FILES})

# === IDE ====================================================================
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT ${GAME_NAME})

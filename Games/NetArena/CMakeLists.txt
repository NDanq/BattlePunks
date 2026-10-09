cmake_minimum_required(VERSION 3.16)

project(NetArena VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# NetArena -- a NimbleEngine game, made by the editor's File > New
# Project from Engine/Templates/Game. Games/Sandbox is the full example of
# everything a game can do; this is the least that runs.
#
# CONFIGURES STANDALONE: open this directory in an IDE, or
#
#     cmake --preset editor-debug
#     cmake --build --preset editor-debug
#
# (On Linux the presets build with clang: editor-debug-clang, ...)
#
# It adds the Engine tree itself, so nothing under Engine/ names this game and
# the engine is never edited to add one.
#
#   NetArenaGame   the game module (NetArenaGame.cpp): level, mode, state
#   NetArena       the launcher: the engine's, built under this name
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

set(GAME_NAME   "NetArena")
set(GAME_MODULE "NetArenaGame")

# === The engine =============================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(GAME_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine" CACHE PATH "The Engine tree ${GAME_NAME} builds against")
    if(NOT EXISTS "${GAME_ENGINE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "${GAME_NAME}: no Engine/CMakeLists.txt at '${GAME_ENGINE_DIR}'. Set GAME_ENGINE_DIR to the Engine tree.")
    endif()

    # This game's own runtime directory. The engine's default is Sandbox's
    # Binaries; one directory belongs to one host (Engine/CMakeLists.txt).
    set(NIMBLE_BINARIES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
        CACHE PATH "Root of the flat runtime layout; <Platform>_<Config> is created below it")

    # Off before the engine is added: it would add Games/Sandbox to this build.
    set(NIMBLE_BUILD_SANDBOX OFF CACHE BOOL "Build the Games/Sandbox host (off: ${GAME_NAME} is the top-level project)" FORCE)

    # Which optional engine modules this game wants: [Modules] in its config
    # (Engine/cmake/NimbleProject.cmake; the editor's Project Settings).
    set(NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")

    add_subdirectory("${GAME_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/Engine")
endif()

if(NOT TARGET NimbleEngine)
    message(FATAL_ERROR "${GAME_NAME}: the NimbleEngine target does not exist; configure this directory as the top-level project.")
endif()

# === The game module ========================================================
# Loaded by name (NimbleEngine/GameModule.h) by the launcher and the editor's
# Play-in-Editor, never linked. Add new sources here.
nimble_add_hot_module(${GAME_MODULE}
        SOURCES      NetArenaGame.cpp
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
    if(NIMBLE_PACKAGE_CONTENT)
        nimble_pack_add_files(Game "${STAGE_ROOT}" ${ARGN})   # into Paks/Game.nmpak, at the end
        return()
    endif()
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
# post process stacks (a Post Process component's "Content/PostProcess/X.pp.nmgraph", render targets (.nmrt2d / .nmrt3d),
# baked lighting (a world's Lighting/ folder: .nmlighting and its lightmaps), and terrain heightfields and their foliage
# (a world's Terrain/ folder: .nmterrain, .nmfoliage), read as they are, text in every build).
file(GLOB_RECURSE GAME_CONTENT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmmesh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmtexture"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmsound"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.pp.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt2d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmlighting"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmterrain"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmfoliage")
game_stage(Content "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_CONTENT_FILES})

# Content/Prefabs/: every build but Shipping stages the text prefabs -- an
# Editor edits them, a Client or Server cooks each the first time it loads it
# (NIMBLE_STAGE_TEXT_CONTENT); Shipping and packaged builds stage the cooked ones from
# Saved/Cooked/Content/Prefabs/ -- made in the editor by Build > Cook Content.
file(GLOB_RECURSE GAME_PREFAB_SCRIPTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.script.nmgraph")
game_stage(PrefabScripts "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_PREFAB_SCRIPTS})

if(NIMBLE_STAGE_TEXT_CONTENT)
    set(GAME_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(GAME_PREFAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Saved/Cooked")
endif()
file(GLOB_RECURSE GAME_PREFAB_FILES CONFIGURE_DEPENDS "${GAME_PREFAB_ROOT}/Content/Prefabs/*.nmprefab")
file(GLOB_RECURSE GAME_PREFAB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Prefabs/*.nmprefab")
if(NOT NIMBLE_STAGE_TEXT_CONTENT AND GAME_PREFAB_SOURCES AND NOT GAME_PREFAB_FILES)
    message(WARNING "${GAME_NAME}: Content/Prefabs/ has prefabs but Saved/Cooked/Content/Prefabs/ has none; a Shipping "
                    "build cannot read text prefabs. Cook them in the editor (console: cook prefabs).")
endif()
game_stage(Prefabs "${GAME_PREFAB_ROOT}" ${GAME_PREFAB_FILES})

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

# Content/Levels/: the worlds Project Settings > Worlds lists with Cook on
# ([World_*] in Config/DefaultEngine.ini), staged at the path a level loads
# them by. Every build but Shipping stages the text worlds -- the .nmworld
# manifest and the per-entity and per-instance files in the folder beside it;
# Shipping, the cooked ones from Saved/Cooked/ (the editor's Build > Cook Content).
# A world not listed, or not cooked, is not staged (nimble_project_worlds).
nimble_project_worlds("${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini" "${CMAKE_CURRENT_SOURCE_DIR}"
        GAME_LEVEL_FILES GAME_LEVEL_ROOT)
game_stage(Levels "${GAME_LEVEL_ROOT}" ${GAME_LEVEL_FILES})

# === Packaged content =======================================================
# With NIMBLE_PACKAGE_CONTENT (Shipping, and the editor's Build Project), what
# game_stage recorded and the engine's content go into Paks/Engine.nmpak and
# Paks/Game.nmpak beside the executable (Engine/cmake/NimblePack.cmake).
if(GAME_RT_DIR AND NOT GAME_MULTI_CONFIG)
    nimble_package_game(TARGET ${GAME_NAME} RUNTIME_DIR "${GAME_RT_DIR}")
    if(TARGET ${GAME_NAME}Paks AND TARGET ${GAME_NAME}Server)
        add_dependencies(${GAME_NAME}Server ${GAME_NAME}Paks)
    endif()
endif()

# === IDE ====================================================================
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT ${GAME_NAME})

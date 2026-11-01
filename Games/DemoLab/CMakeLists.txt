cmake_minimum_required(VERSION 3.16)

# Linux builds with Clang only (Engine/CMakeLists.txt refuses anything else).
# Before project(), so a new build tree picks it up; one already made with
# another compiler needs cmake --fresh.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_C_COMPILER   clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(DemoLab VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# DemoLab -- a NimbleEngine game, made by the editor's File > New
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
#   DemoLabGame   the game module (DemoLabGame.cpp): level, mode, state
#   DemoLab       the launcher: the engine's, built under this name
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

set(GAME_NAME   "DemoLab")
set(GAME_MODULE "DemoLabGame")

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
        SOURCES      DemoLabGame.cpp
                    Source/LabVersion.h
                    Source/Core/LabTypes.h
                    Source/Core/LabHealth.h
                    Source/Core/LabHealth.cpp
                    Source/Core/LabPlayer.h
                    Source/Core/LabPlayer.cpp
                    Source/Core/LabInput.cpp
                    Source/Core/LabMove.cpp
                    Source/Core/LabFootsteps.h
                    Source/Core/LabFootsteps.cpp
                    Source/Weapons/LabWeapon.h
                    Source/Weapons/LabWeapon.cpp
                    Source/Weapons/LabCombat.h
                    Source/Weapons/LabCombat.cpp
                    Source/Weapons/LabProjectile.h
                    Source/Weapons/LabProjectile.cpp
                    Source/AI/LabBots.h
                    Source/AI/LabBots.cpp
                    Source/AI/LabArmour.h
                    Source/AI/LabArmour.cpp
                    Source/World/LabSignals.h
                    Source/World/LabSignals.cpp
                    Source/World/LabInteractable.h
                    Source/World/LabInteractable.cpp
                    Source/World/LabEncounter.h
                    Source/World/LabEncounter.cpp
                    Source/World/LabLogic.h
                    Source/World/LabLogic.cpp
                    Source/World/LabMotion.h
                    Source/World/LabMotion.cpp
                    Source/Flow/LabTravel.h
                    Source/Flow/LabTravel.cpp
                    Source/Flow/LabFlow.h
                    Source/Flow/LabFlow.cpp
                    Source/UI/LabHUD.h
                    Source/UI/LabHUD.cpp
                    Source/UI/LabUI.h
                    Source/UI/LabUI.cpp
                    Source/GameModes/LabGameModeBase.h
                    Source/GameModes/LabGameModeBase.cpp
                    Source/GameModes/LabStoryMode.cpp
                    Source/GameModes/LabMenuMode.cpp
                    Source/GameModes/LabAutoPlay.h
                    Source/GameModes/LabAutoPlay.cpp
                    Source/GameModes/LabMenuTest.h
                    Source/GameModes/LabMenuTest.cpp
                    Source/GameModes/LabInputDriver.h
                    Source/GameModes/LabInputDriver.cpp
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
# drop this line and turn NetCore off in Config/DefaultEngine.ini:
#
#     [Modules]
#     NetCore=false
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
# post process stacks (a Post Process component's "Content/PostProcess/X.pp.nmgraph", render targets (.nmrt2d / .nmrt3d), particle systems (.nmparticle), animation (.nmskel, .nmanim, .nmanimstate, .nmretarget), sounds (.nmsound),
# baked lighting (a world's Lighting/ folder: .nmlighting and its lightmaps), baked navmeshes (.nmnav), and terrain heightfields and their foliage
# (a world's Terrain/ folder: .nmterrain, .nmfoliage), read as they are, text in every build).
file(GLOB_RECURSE GAME_CONTENT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmmesh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmtexture"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.pp.nmgraph"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt2d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmrt3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmparticle"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmskel"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmanim"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmanimstate"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmretarget"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmsound"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmlighting"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmterrain"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmfoliage"
        "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmnav")
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

# Content/UI/: UI documents (Engine/Modules/UI/README.md) and the Lua beside
# them. As prefabs: the Editor and every build but Shipping stage the XML
# (NIMBLE_STAGE_TEXT_CONTENT; WITH_UNCOOKED reads it); Shipping and packaged
# builds stage the cooked ones from Saved/Cooked/Content/UI/ -- made in the
# editor by Build > Cook Content. A fresh checkout (a content worker's) has none.
file(GLOB_RECURSE GAME_UI_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.lua")
game_stage(UIScripts "${CMAKE_CURRENT_SOURCE_DIR}" ${GAME_UI_SCRIPTS})

if(NIMBLE_TARGET STREQUAL "Editor" OR NIMBLE_STAGE_TEXT_CONTENT)
    set(GAME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(GAME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Saved/Cooked")
endif()
file(GLOB_RECURSE GAME_UI_FILES CONFIGURE_DEPENDS "${GAME_UI_ROOT}/Content/UI/*.nmui")
file(GLOB_RECURSE GAME_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/UI/*.nmui")
if(NOT NIMBLE_TARGET STREQUAL "Editor" AND NOT NIMBLE_STAGE_TEXT_CONTENT AND GAME_UI_SOURCES AND NOT GAME_UI_FILES)
    message(WARNING "${GAME_NAME}: Content/UI/ has documents but Saved/Cooked/Content/UI/ has none; a ${NIMBLE_TARGET} "
                    "build cannot read XML. Cook them in the editor (console: cook ui).")
endif()
game_stage(UI "${GAME_UI_ROOT}" ${GAME_UI_FILES})

# Sequences (.nmseq, anywhere under Content/): cutscenes
# (Engine/Modules/Sequencer/README.md). As prefabs: every build but Shipping
# stages the XML (NIMBLE_STAGE_TEXT_CONTENT); Shipping and packaged builds
# stage the cooked ones from Saved/Cooked/ -- made in the editor by Build >
# Cook Content.
if(NIMBLE_STAGE_TEXT_CONTENT)
    set(GAME_SEQUENCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(GAME_SEQUENCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Saved/Cooked")
endif()
file(GLOB_RECURSE GAME_SEQUENCE_FILES CONFIGURE_DEPENDS "${GAME_SEQUENCE_ROOT}/Content/*.nmseq")
file(GLOB_RECURSE GAME_SEQUENCE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmseq")
if(NOT NIMBLE_STAGE_TEXT_CONTENT AND GAME_SEQUENCE_SOURCES AND NOT GAME_SEQUENCE_FILES)
    message(WARNING "${GAME_NAME}: Content/ has sequences but Saved/Cooked/Content/ has none; a Shipping "
                    "build cannot read XML sequences. Cook them in the editor (Build > Cook Content).")
endif()
game_stage(Sequences "${GAME_SEQUENCE_ROOT}" ${GAME_SEQUENCE_FILES})

# Behavior trees (.nmbt, anywhere under Content/): AI Brains' trees
# (Engine/Modules/AICore/README.md). As sequences: every build but Shipping
# stages the XML; Shipping and packaged builds stage the cooked ones from
# Saved/Cooked/ -- made in the editor by Build > Cook Content.
if(NIMBLE_STAGE_TEXT_CONTENT)
    set(GAME_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    set(GAME_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Saved/Cooked")
endif()
file(GLOB_RECURSE GAME_TREE_FILES CONFIGURE_DEPENDS "${GAME_TREE_ROOT}/Content/*.nmbt")
file(GLOB_RECURSE GAME_TREE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Content/*.nmbt")
if(NOT NIMBLE_STAGE_TEXT_CONTENT AND GAME_TREE_SOURCES AND NOT GAME_TREE_FILES)
    message(WARNING "${GAME_NAME}: Content/ has behavior trees but Saved/Cooked/Content/ has none; a Shipping "
                    "build cannot read XML trees. Cook them in the editor (Build > Cook Content).")
endif()
game_stage(BehaviorTrees "${GAME_TREE_ROOT}" ${GAME_TREE_FILES})

# Content/Materials/: material graphs, cooked by NimbleCook into the module's shader cache and staged here
# (Engine/cmake/NimbleMaterials.cmake), named "Content/Materials/<Name>.material.nmgraph".
if(GAME_RT_DIR AND NOT GAME_MULTI_CONFIG)
    nimble_stage_materials(TARGET ${GAME_NAME} DIR "${CMAKE_CURRENT_SOURCE_DIR}/Content/Materials"
                           ROOT "${CMAKE_CURRENT_SOURCE_DIR}" MODULE ${GAME_MODULE} RUNTIME_DIR "${GAME_RT_DIR}")
    if(TARGET ${GAME_NAME}StageMaterials)
        add_dependencies(${GAME_MODULE} ${GAME_NAME}StageMaterials)   # the editor's build stages them too
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

# === Tests ==================================================================
# DemoLab.AutoPlay: the whole game played by itself, headless
# (Source/GameModes/LabAutoPlay.h) -- the main menu's New Game, L1 (the crank),
# L2 (breaker, keycard, valves, elevator), L3 (the theft, W1, a deliberate fall
# and the checkpoint reload, W2 with the beacon, the freight lift), L2 on alarm
# (W3, stragglers, the W4 hold, the roof hatch), the credits and back to the
# menu. Passes on its log line; a step it cannot finish fails it.
if(NIMBLE_BUILD_TESTS AND NOT CMAKE_BUILD_TYPE STREQUAL "Shipping")
    enable_testing()
    add_test(NAME DemoLab.AutoPlay
             COMMAND ${GAME_NAME} -headless -DemoLab.AutoPlay=1 -DemoLab.StoryDamageScale=0.15 -Game.StartLevel=MainMenu
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:${GAME_NAME}>")
    set_tests_properties(DemoLab.AutoPlay PROPERTIES TIMEOUT 1500 LABELS "smoke"
                         ENVIRONMENT "PULSE_SERVER=unix:/nonexistent"
                         PASS_REGULAR_EXPRESSION "\\[DemoLab\\] AutoPlay PASS"
                         FAIL_REGULAR_EXPRESSION "\\[DemoLab\\] AutoPlay FAIL")

    # DemoLab.MenuTest: the menus driven through their documents
    # (Source/GameModes/LabMenuTest.h) -- Settings (values apply, and closing
    # saves them), each Chapter Select row (level, state, entry, sidearm,
    # checkpoint), F1's overlay (toggles, live stat unit), Pause (Resume,
    # Restart checkpoint, Main menu). DemoLab.SettingsPersist: a second process
    # reads back the settings the first saved (Saved/DemoLabTest/User.ini, never
    # the player's own).
    add_test(NAME DemoLab.MenuTest
             COMMAND ${GAME_NAME} -headless -DemoLab.MenuTest=1 -Game.StartLevel=MainMenu
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:${GAME_NAME}>")
    set_tests_properties(DemoLab.MenuTest PROPERTIES TIMEOUT 900 LABELS "smoke"
                         ENVIRONMENT "PULSE_SERVER=unix:/nonexistent"
                         FIXTURES_SETUP DemoLabUserSettings
                         PASS_REGULAR_EXPRESSION "\\[DemoLab\\] MenuTest PASS"
                         FAIL_REGULAR_EXPRESSION "\\[DemoLab\\] MenuTest FAIL")
    add_test(NAME DemoLab.SettingsPersist
             COMMAND ${GAME_NAME} -headless -DemoLab.MenuTest=2 -Game.StartLevel=MainMenu
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:${GAME_NAME}>")
    set_tests_properties(DemoLab.SettingsPersist PROPERTIES TIMEOUT 300 LABELS "smoke"
                         ENVIRONMENT "PULSE_SERVER=unix:/nonexistent"
                         FIXTURES_REQUIRED DemoLabUserSettings
                         PASS_REGULAR_EXPRESSION "\\[DemoLab\\] MenuTest PASS"
                         FAIL_REGULAR_EXPRESSION "\\[DemoLab\\] MenuTest FAIL")

    # DemoLab.C5Handover: C5 alone (-DemoLab.Cinematic), Interact mashed through
    # it and the hand-over at the sidearm locker; nothing may be picked up.
    add_test(NAME DemoLab.C5Handover
             COMMAND ${GAME_NAME} -headless -DemoLab.Cinematic=C5 -DemoLab.CineMashInteract=1 -Game.StartLevel=L3_Core
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:${GAME_NAME}>")
    set_tests_properties(DemoLab.C5Handover PROPERTIES TIMEOUT 400 LABELS "smoke"
                         ENVIRONMENT "PULSE_SERVER=unix:/nonexistent"
                         PASS_REGULAR_EXPRESSION "\\[DemoLab\\] cinematic test C5: hand-over PASS"
                         FAIL_REGULAR_EXPRESSION "hand-over FAIL")
endif()

cmake_minimum_required(VERSION 3.16)

project(AICore VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# AICore -- navigation and decisions: navmeshes baked with Recast (tiled,
# through DetourTileCache so tiles rebuild and obstacles come and go at
# runtime), paths and crowds with Detour, behavior trees with utility scoring,
# and perception. See README.md, and GUIDE.md for using it from a game.
#
# OPTIONAL: Engine/CMakeLists.txt adds this directory only when
# NIMBLE_WITH_AICORE is on. NimbleEngine links it PRIVATE and attaches an
# AIWorld to each level's World (GameSession); a game reaches it through its
# components, the "AI" script library and NimbleEngine/AIFacade.h, or links
# it itself to register behavior tree tasks.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "AICore is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(AICORE_BUILD_TESTS "Build the AICore tests" ON)

# === Recast =================================================================
# Recast Navigation 1.6.0 (https://github.com/recastnavigation/recastnavigation,
# zlib). Vendored at Engine/ThirdParty/Source and built by
# Engine/ThirdParty/CMakeLists.txt as RecastNav::RecastNav, whose usage
# requirements (DT_VIRTUAL_QUERYFILTER) are in NimbleThirdPartyUsage.cmake.

# ===========================================================================
# Sources
# ===========================================================================

set(AICORE_SOURCES
        Source/Private/AICore/AIComponents.cpp
        Source/Private/AICore/AIScriptLibrary.cpp
        Source/Private/AICore/AIWorld.cpp
        Source/Private/AICore/Blackboard.cpp
        Source/Private/AICore/BTBinary.cpp
        Source/Private/AICore/BTNodes.cpp
        Source/Private/AICore/BTRegistry.cpp
        Source/Private/AICore/BTRuntime.cpp
        Source/Private/AICore/BTXml.cpp
        Source/Private/AICore/NavBuild.cpp
        Source/Private/AICore/NavDebugDraw.cpp
        Source/Private/AICore/NavGeometry.cpp
        Source/Private/AICore/NavInternal.h
        Source/Private/AICore/NavQuery.cpp
        Source/Private/AICore/NavWorld.cpp
        Source/Private/AICore/Perception.cpp
        Source/Private/AICore/UtilityNodes.cpp
        Source/Public/AICore/AICoreAPI.h
        Source/Public/AICore/AIWorld.h
        Source/Public/AICore/BehaviorTree.h
        Source/Public/AICore/BehaviorTreeFile.h
        Source/Public/AICore/Blackboard.h
        Source/Public/AICore/NavDebugDraw.h
        Source/Public/AICore/NavGeometry.h
        Source/Public/AICore/NavMesh.h
        Source/Public/AICore/NavQuery.h
        Source/Public/AICore/NavTypes.h
        Source/Public/AICore/NavWorld.h
        Source/Public/AICore/Perception.h
        Source/Public/AICore/Components/AIBrainComponent.h
        Source/Public/AICore/Components/NavAgentComponent.h
        Source/Public/AICore/Components/NavMeshVolumeComponent.h
        Source/Public/AICore/Components/NavModifierComponent.h
        Source/Public/AICore/Components/NavObstacleComponent.h
        Source/Public/AICore/Components/PerceptionComponent.h
)

# SHARED, like GameCore: one registry of behavior tree nodes, one asset cache
# and one AIWorld per World, and the components register once. Recast is
# PRIVATE -- no public header includes it.
nimble_add_module(AICore SHARED
        PREFIX       AICORE
        SOURCES      ${AICORE_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS RecastNav::RecastNav)

target_include_directories(AICore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(AICore PROPERTIES FOLDER "Engine")

# .nmbt XML is read by every build but Shipping (WITH_UNCOOKED), as .nmseq's
# is. PRIVATE: no public header includes pugixml.hpp.
if(TARGET PugiXml::PugiXml)
    target_link_libraries(AICore PRIVATE $<$<NOT:$<CONFIG:Shipping>>:PugiXml::PugiXml>)
endif()

if(AICORE_BUILD_TESTS)
    add_executable(AICoreTests Tests/AICoreTests.cpp)
    target_link_libraries(AICoreTests PRIVATE AICore::AICore RecastNav::RecastNav)
    target_include_directories(AICoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    # The Toon Arena fixture (Games/WorldsLab/Tools/Toony/make_toon_arena.py writes it).
    target_compile_definitions(AICoreTests PRIVATE "AICORE_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data\"")
    set_target_properties(AICoreTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME AICore.Tests COMMAND AICoreTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:AICoreTests>")
    set_tests_properties(AICore.Tests PROPERTIES TIMEOUT 120 LABELS "smoke")
endif()

source_group("AICore" FILES ${AICORE_SOURCES})

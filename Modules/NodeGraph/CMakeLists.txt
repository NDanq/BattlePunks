cmake_minimum_required(VERSION 3.16)

project(NodeGraph VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# NodeGraph -- node graphs that GENERATE SOURCE: material graphs to the
# engine's macro GLSL (cooked by GDI's cook_shaders.py like any hand-written
# shader), script graphs to a Lua class table (attached with SetScript like any
# hand-written script). The runtime never sees a graph.
#
# EDITOR TARGETS ONLY, and Qt-free: the editor draws graphs, but loading,
# saving and compiling them live here so Build > Cook Content and the tests can
# regenerate every graph without a window. Engine/CMakeLists.txt adds this
# directory only when NIMBLE_WITH_EDITOR is 1.
#
# STATIC, unlike GameCore. Nothing here is a process-wide registry that two
# images would each get a copy of in a harmful way: the schemas are rebuilt
# from GameCore's registries (which DO live in one place, GameCore.dll) by
# whichever executable links this -- the editor, or the test.
#
# Links GameCore PUBLIC: node identity is GameCore::EntityGuid, and the script
# schema reads the component script bindings. pugixml PRIVATE, as GameCore
# links it -- no public header here includes pugixml.hpp. A second static
# copy is harmless: pugixml keeps no global state (Lua's reason for living in
# exactly one image does not apply).
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "NodeGraph is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

if(NOT TARGET PugiXml::PugiXml)
    message(FATAL_ERROR "NodeGraph needs PugiXml::PugiXml, which GameCore declares for editor targets.")
endif()

option(NODEGRAPH_BUILD_TESTS "Build NodeGraphTests (golden-file codegen tests)" ON)

set(NODEGRAPH_SOURCES
        Source/Private/NodeGraph/Graph.cpp
        Source/Private/NodeGraph/Schema.cpp
        Source/Private/NodeGraph/GraphFile.cpp
        Source/Private/NodeGraph/GraphCompiler.cpp
        Source/Private/NodeGraph/GraphIndex.cpp
        Source/Private/NodeGraph/GraphIndex.h
        Source/Private/NodeGraph/CodeWriter.h
        Source/Private/NodeGraph/MaterialSchema.cpp
        Source/Private/NodeGraph/ScriptSchema.cpp
        Source/Private/NodeGraph/NodeGraphLog.h
        Source/Public/NodeGraph/Graph.h
        Source/Public/NodeGraph/Schema.h
        Source/Public/NodeGraph/GraphFile.h
        Source/Public/NodeGraph/GraphCompiler.h
)

nimble_add_module(NodeGraph
        PREFIX       NODEGRAPH
        SOURCES      ${NODEGRAPH_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS PugiXml::PugiXml)

target_include_directories(NodeGraph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(NodeGraph PROPERTIES FOLDER "Editor")

if(NODEGRAPH_BUILD_TESTS)
    add_executable(NodeGraphTests Tests/NodeGraphTests.cpp)
    target_link_libraries(NodeGraphTests PRIVATE NodeGraph::NodeGraph)
    target_compile_definitions(NodeGraphTests PRIVATE
            NODEGRAPH_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data")
    set_target_properties(NodeGraphTests PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            FOLDER  "Editor")

    # Run from the flat Binaries directory so GameCore.dll and EngineCore.dll
    # resolve, as every other test and host does.
    add_test(NAME NodeGraph.Tests COMMAND NodeGraphTests)
    set_tests_properties(NodeGraph.Tests PROPERTIES
            TIMEOUT 120
            LABELS  "smoke")

endif()

# === Material shader checks ===================================================
# Scripts/cook_material_graphs.py puts generated shaders through GDI's own
# cook_shaders.py and glslc. Python is looked up HERE, not taken from GDI's
# find_package: FindPython's Python3_EXECUTABLE is a directory-scope variable,
# invisible outside Modules/GDI. Optional -- without Python or glslc the
# editor skips the check and the test below is not added.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(NODEGRAPH_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")

# For the Editor's CMakeLists, which passes both to the graph windows.
set(NODEGRAPH_PYTHON "${Python3_EXECUTABLE}" CACHE INTERNAL "Python for cook_material_graphs.py; empty if none")
set(NODEGRAPH_COOK_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_material_graphs.py" CACHE INTERNAL
    "cook_material_graphs.py, run by the editor to check generated shaders")

# The golden material output through the real shader toolchain.
if(NODEGRAPH_BUILD_TESTS AND Python3_EXECUTABLE AND NODEGRAPH_GLSLC)
    add_test(NAME NodeGraph.MaterialGLSL
             COMMAND "${Python3_EXECUTABLE}" "${NODEGRAPH_COOK_SCRIPT}"
                     -check "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Glow.fsh"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Masks.fsh"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Logic.fsh")
    set_tests_properties(NodeGraph.MaterialGLSL PROPERTIES TIMEOUT 120 LABELS "smoke")
endif()

source_group("NodeGraph" FILES ${NODEGRAPH_SOURCES})

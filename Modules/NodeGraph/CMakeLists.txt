cmake_minimum_required(VERSION 3.16)

project(NodeGraph VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# NodeGraph -- graphs that GENERATE SOURCE: material graphs to GLSL, script
# graphs to Lua class tables. The runtime never sees a graph.
#
# Editor targets only, and Qt-free, so cooking and tests need no window.
# STATIC: it holds no process-wide state (schemas are rebuilt from GameCore's
# registries). pugixml PRIVATE; a second static copy is harmless.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "NodeGraph is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

if(NOT TARGET PugiXml::PugiXml)
    message(FATAL_ERROR "NodeGraph needs PugiXml::PugiXml (Engine/ThirdParty).")
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
        Source/Private/NodeGraph/PostProcessSchema.cpp
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

    # From Binaries, so the engine DLLs resolve.
    add_test(NAME NodeGraph.Tests COMMAND NodeGraphTests)
    set_tests_properties(NodeGraph.Tests PROPERTIES
            TIMEOUT 120
            LABELS  "smoke")

endif()

# === NodeGraphGenerate ========================================================
# The editor's on-save generation from the command line: compiles graphs and
# writes their outputs into each graph's <gen_code>. -module <game dll> loads a
# game module first, for graphs that use its script declarations.
add_executable(NodeGraphGenerate Tools/NodeGraphGenerate.cpp)
target_link_libraries(NodeGraphGenerate PRIVATE NodeGraph::NodeGraph)
set_target_properties(NodeGraphGenerate PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Editor")

# === Material shader checks ===================================================
# cook_material_graphs.py runs generated shaders through GDI's cook_shaders.py
# and glslc. Python is found here (GDI's result is directory-scoped).
# Optional: without Python or glslc the check and test are skipped.
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
                     -check "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Glow.mtl.glsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Masks.mtl.glsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Logic.mtl.glsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Cutout.mtl.glsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Wave.mtl.glsl"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Textured.mtl.glsl")
    set_tests_properties(NodeGraph.MaterialGLSL PROPERTIES TIMEOUT 120 LABELS "smoke")
endif()

source_group("NodeGraph" FILES ${NODEGRAPH_SOURCES})

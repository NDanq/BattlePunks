cmake_minimum_required(VERSION 3.16)

project(Sequencer VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# Sequencer -- cutscenes: a timeline asset (.nmseq) of bindings and tracks,
# the Sequence Player component that plays one, and the per-World system
# (SequenceWorld) that drives players and writes their tracks. See README.md.
#
# After GameCore and RenderCore (animation and cameras, linked PRIVATE: no
# public header needs them), and before NimbleEngine, which attaches a
# SequenceWorld to each level's World.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "Sequencer is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(SEQUENCER_BUILD_TESTS "Build the Sequencer tests" ON)

set(SEQUENCER_SOURCES
        Source/Private/Sequencer/SequenceAsset.cpp
        Source/Private/Sequencer/SequenceBinary.cpp
        Source/Private/Sequencer/SequenceXml.cpp
        Source/Private/Sequencer/SequencePlayerComponent.cpp
        Source/Private/Sequencer/SequenceInstance.cpp
        Source/Private/Sequencer/SequenceWorld.cpp
        Source/Public/Sequencer/SequencerAPI.h
        Source/Public/Sequencer/SequenceAsset.h
        Source/Public/Sequencer/SequenceFile.h
        Source/Public/Sequencer/SequenceInstance.h
        Source/Public/Sequencer/SequencePlayerComponent.h
        Source/Public/Sequencer/SequenceWorld.h
)

# SHARED, like Audio: one asset cache and one SequenceWorld registry per
# process, and the component's registration happens once.
nimble_add_module(Sequencer SHARED
        PREFIX       SEQUENCER
        SOURCES      ${SEQUENCER_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS RenderCore::RenderCore)

target_include_directories(Sequencer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(Sequencer PROPERTIES FOLDER "Engine")

# XML is read by every build but Shipping (WITH_UNCOOKED), as UI's is.
# PRIVATE: no public header includes pugixml.hpp.
if(TARGET PugiXml::PugiXml)
    target_link_libraries(Sequencer PRIVATE $<$<NOT:$<CONFIG:Shipping>>:PugiXml::PugiXml>)
endif()

# The fixtures are XML, which a Shipping build cannot read; the cooked path
# they also test is the same code in every build.
if(SEQUENCER_BUILD_TESTS)
    add_executable(SequencerTests Tests/SequencerTests.cpp)
    target_link_libraries(SequencerTests PRIVATE Sequencer::Sequencer RenderCore::RenderCore)
    set_target_properties(SequencerTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME Sequencer.Tests COMMAND SequencerTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:SequencerTests>")
    set_tests_properties(Sequencer.Tests PROPERTIES TIMEOUT 60 LABELS "smoke")
endif()

source_group("Sequencer" FILES ${SEQUENCER_SOURCES})

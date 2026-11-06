cmake_minimum_required(VERSION 3.16)

project(Collab VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# Collab -- editor collaboration: the protocol, CollabServer's sessions and
# CollabClient, over ENet. What NimbleEditorServer runs and the editor joins
# (Modules/Editor/COLLABORATION.md).
#
# NO QT, NO GAMECORE: Core and ENet only, so the server builds on a host with
# a compiler and CMake and nothing else, and a GameModule can never take it
# down. Its own small transport (CollabTransport), since NetCore's is private
# to NetCore and NetCore needs GameCore.
#
# STATIC: linked into NimbleEditorServer and NimbleEditor, one copy each.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "Collab is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(COLLAB_BUILD_TESTS "Build the Collab loopback test" ON)

# === ENet ===================================================================
# ENet::ENet, NetCore's transport too: one vendored copy, built by
# Engine/ThirdParty, so Collab does not depend on NetCore being on.

# ===========================================================================
# Sources
# ===========================================================================

set(COLLAB_SOURCES
        Source/Public/Collab/CollabAPI.h
        Source/Public/Collab/CollabProtocol.h
        Source/Public/Collab/CollabClient.h
        Source/Public/Collab/CollabServer.h
        Source/Private/Collab/CollabTransport.h
        Source/Private/Collab/CollabTransport.cpp
        Source/Private/Collab/CollabProtocol.cpp
        Source/Private/Collab/CollabClient.cpp
        Source/Private/Collab/CollabServer.cpp
        Source/Public/Collab/ContentWorker.h
        Source/Private/Collab/ContentWorker.cpp
        Source/Private/Collab/CollabProcess.h
        Source/Private/Collab/CollabProcess.cpp
)

# ENet is PRIVATE -- no public header includes enet.h.
nimble_add_module(Collab
        PREFIX       COLLAB
        SOURCES      ${COLLAB_SOURCES}
        PRIVATE_DEPS ENet::ENet)

target_include_directories(Collab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(Collab PROPERTIES FOLDER "Engine")

if(COLLAB_BUILD_TESTS)
    add_executable(CollabLoopbackTests Tests/CollabLoopbackTests.cpp)
    target_link_libraries(CollabLoopbackTests PRIVATE Collab::Collab)
    set_target_properties(CollabLoopbackTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME Collab.Loopback COMMAND CollabLoopbackTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:CollabLoopbackTests>")
    set_tests_properties(Collab.Loopback PROPERTIES TIMEOUT 120 LABELS "smoke")
endif()

source_group("Collab" FILES ${COLLAB_SOURCES})

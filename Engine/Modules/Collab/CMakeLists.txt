cmake_minimum_required(VERSION 3.16)

project(Collab VERSION 0.1 LANGUAGES C CXX)

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
# NetCore's vendored ENet 1.3.18 (MIT), one copy in the tree. Whichever of
# NetCore and Collab configures first defines the target; Collab must not
# depend on NetCore being on.
set(COLLAB_ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Source/enet-1.3.18")
if(NOT TARGET ENet::ENet)
    if(NOT EXISTS "${COLLAB_ENET_DIR}/include/enet/enet.h")
        message(FATAL_ERROR "Vendored ENet not found at ${COLLAB_ENET_DIR}.")
    endif()
    set(COLLAB_ENET_SOURCES
            ${COLLAB_ENET_DIR}/callbacks.c
            ${COLLAB_ENET_DIR}/compress.c
            ${COLLAB_ENET_DIR}/host.c
            ${COLLAB_ENET_DIR}/list.c
            ${COLLAB_ENET_DIR}/packet.c
            ${COLLAB_ENET_DIR}/peer.c
            ${COLLAB_ENET_DIR}/protocol.c
            ${COLLAB_ENET_DIR}/unix.c
            ${COLLAB_ENET_DIR}/win32.c)
    add_library(ENet STATIC ${COLLAB_ENET_SOURCES})
    add_library(ENet::ENet ALIAS ENet)
    target_include_directories(ENet PUBLIC "${COLLAB_ENET_DIR}/include")
    if(WIN32)
        target_link_libraries(ENet PUBLIC ws2_32 winmm)
        target_compile_definitions(ENet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
    else()
        target_compile_definitions(ENet PRIVATE
                HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
                HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
    endif()
    set_target_properties(ENet PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            FOLDER "Engine/External")
endif()

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

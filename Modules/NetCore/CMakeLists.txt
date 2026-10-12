cmake_minimum_required(VERSION 3.16)

project(NetCore VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# NetCore -- networking: an ENet transport, a server-authoritative session
# that replicates networked entities (GameCore's NetComponent and replication
# tables) and routes RPCs (GameCore/ComponentRpc.h), and the editor link stub.
#
# OPTIONAL: Engine/CMakeLists.txt adds this directory only when
# NIMBLE_WITH_NETCORE is on. Nothing outside the engine layer, the server
# launcher and the editor includes a NetCore header, so with it off there is
# nothing to compile out by hand -- see that file's Networking block.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "NetCore is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(NETCORE_BUILD_TESTS "Build the NetCore loopback test" ON)

# === ENet ===================================================================
# ENet 1.3.18 (http://enet.bespin.org, MIT): reliable and unreliable channels
# over UDP, connection handling and fragmentation. Vendored at
# Engine/ThirdParty/Source and built by Engine/ThirdParty/CMakeLists.txt as
# ENet::ENet, which Collab links too.

# ===========================================================================
# Sources
# ===========================================================================

set(NETCORE_SOURCES
        Source/Private/NetCore/NetTransport.h
        Source/Private/NetCore/NetTransport.cpp
        Source/Private/NetCore/NetProtocol.h
        Source/Private/NetCore/NetSchema.h
        Source/Private/NetCore/NetSchema.cpp
        Source/Private/NetCore/NetReplication.h
        Source/Private/NetCore/NetReplication.cpp
        Source/Private/NetCore/NetSession.cpp
        Source/Public/NetCore/NetCoreAPI.h
        Source/Public/NetCore/NetTypes.h
        Source/Public/NetCore/NetSession.h
)

# SHARED, like GameCore: one session and one installed RPC sink per process.
# ENet is PRIVATE -- no public header includes enet.h.
nimble_add_module(NetCore SHARED
        PREFIX       NETCORE
        SOURCES      ${NETCORE_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS ENet::ENet)

target_include_directories(NetCore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(NetCore PROPERTIES FOLDER "Engine")

if(NETCORE_BUILD_TESTS)
    add_executable(NetCoreLoopbackTests Tests/LoopbackTests.cpp)
    target_link_libraries(NetCoreLoopbackTests PRIVATE NetCore::NetCore)
    set_target_properties(NetCoreLoopbackTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME NetCore.Loopback COMMAND NetCoreLoopbackTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NetCoreLoopbackTests>")
    set_tests_properties(NetCore.Loopback PROPERTIES TIMEOUT 120 LABELS "smoke")
endif()

source_group("NetCore" FILES ${NETCORE_SOURCES})

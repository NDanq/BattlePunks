cmake_minimum_required(VERSION 3.16)

project(NetCore VERSION 0.1 LANGUAGES C CXX)

# ===========================================================================
# NetCore -- networking: an ENet transport, a server-authoritative session
# that replicates networked entities (GameCore's NetComponent and replication
# tables) and routes RPCs (GameCore/ComponentRpc.h), and the editor link stub.
#
# OPTIONAL: Engine/CMakeLists.txt adds this directory only when
# NIMBLE_WITH_NETCORE is on. Nothing outside the engine layer, the server
# launcher and the editor includes a NetCore header, so with it off there is
# nothing to compile out by hand -- see that file's Networking block.
#
# LANGUAGES C CXX: ENet is a C library, built by hand below like GameCore's Lua.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "NetCore is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(NETCORE_BUILD_TESTS "Build the NetCore loopback test" ON)

# === Vendored ENet ==========================================================
# ENet 1.3.18 (http://enet.bespin.org, MIT): reliable and unreliable channels
# over UDP, connection handling and fragmentation in nine C files. Source form
# under External/, as GameCore vendors Lua: no find_package, no FetchContent,
# a fresh clone configures with no per-machine setup.
set(NETCORE_ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/enet-1.3.18")

if(NOT EXISTS "${NETCORE_ENET_DIR}/include/enet/enet.h")
    message(FATAL_ERROR "Vendored ENet not found at ${NETCORE_ENET_DIR}.")
endif()

set(NETCORE_ENET_SOURCES
        External/enet-1.3.18/callbacks.c
        External/enet-1.3.18/compress.c
        External/enet-1.3.18/host.c
        External/enet-1.3.18/list.c
        External/enet-1.3.18/packet.c
        External/enet-1.3.18/peer.c
        External/enet-1.3.18/protocol.c
        External/enet-1.3.18/unix.c
        External/enet-1.3.18/win32.c
)

if(NOT TARGET ENet::ENet)
    add_library(ENet STATIC ${NETCORE_ENET_SOURCES})
    add_library(ENet::ENet ALIAS ENet)

    target_include_directories(ENet PUBLIC "${NETCORE_ENET_DIR}/include")

    if(WIN32)
        # unix.c and win32.c guard themselves on _WIN32, so both are listed.
        target_link_libraries(ENet PUBLIC ws2_32 winmm)
        target_compile_definitions(ENet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
    else()
        # What ENet's own CMakeLists probes for; every supported POSIX target has them.
        target_compile_definitions(ENet PRIVATE
                HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
                HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
    endif()

    # Linked into NetCore.dll, so position-independent (GameCore's Lua reason).
    set_target_properties(ENet PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
            FOLDER "Engine/External")

    source_group("External/ENet" FILES ${NETCORE_ENET_SOURCES})
endif()

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

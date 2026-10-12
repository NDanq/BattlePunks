cmake_minimum_required(VERSION 3.16)

project(JoltPhysics VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# JoltPhysics -- rigid-body physics for a GameCore World, on Jolt Physics:
# GameCore's Rigid Body and Physics Constraint components become Jolt bodies
# and joints, stepped in TP_Physics, their poses written back to the
# Transforms. See README.md.
#
# OPT IN: Engine/CMakeLists.txt adds this directory only when
# NIMBLE_WITH_JOLTPHYSICS is on (off by default). The components live in
# GameCore, so a level that carries them loads without this module; the
# engine layer warns that they will not simulate.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "JoltPhysics is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(JOLTPHYSICS_BUILD_TESTS "Build the JoltPhysics tests (Engine/Tests/PhysicsTests.cpp)" ON)

# === Vendored Jolt ==========================================================
# Jolt Physics 5.6.0 (https://github.com/jrouwe/JoltPhysics, MIT -- LICENSE
# beside it). Only the library's own sources are vendored: the files Jolt.cmake
# compiles with every option off -- no samples, tests, viewer, GPU compute
# backends, shaders or object streams. The vendored tree IS the source list,
# so it is globbed; bumping Jolt is the same extraction from the new release
# plus JOLTPHYSICS_JOLT_DIR below.
set(JOLTPHYSICS_JOLT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Source/JoltPhysics-5.6.0")
if(NOT EXISTS "${JOLTPHYSICS_JOLT_DIR}/Jolt/Jolt.h")
    message(FATAL_ERROR "JoltPhysics: no vendored Jolt at '${JOLTPHYSICS_JOLT_DIR}'")
endif()

if(NOT TARGET Jolt::Jolt)
    file(GLOB_RECURSE JOLTPHYSICS_JOLT_SOURCES CONFIGURE_DEPENDS "${JOLTPHYSICS_JOLT_DIR}/Jolt/*.cpp")

    # STATIC, inside JoltPhysics.dll alone -- exactly one copy of Jolt's
    # factory and type registry in the process, as GDI lives in RenderCore.
    add_library(Jolt STATIC ${JOLTPHYSICS_JOLT_SOURCES})
    add_library(Jolt::Jolt ALIAS Jolt)
    target_include_directories(Jolt PUBLIC "${JOLTPHYSICS_JOLT_DIR}")
    target_compile_features(Jolt PUBLIC cxx_std_17)

    # PUBLIC, so every translation unit that includes a Jolt header agrees with
    # the library: 32-bit object layers (JoltLayers.h packs layer and mask in
    # them) and asserts in Debug. Jolt's version check (JoltRuntime.cpp) is
    # the backstop if they ever drift.
    target_compile_definitions(Jolt PUBLIC
            JPH_OBJECT_LAYER_BITS=32
            $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>)

    # THE SAME SIMD AS EVERYTHING ELSE. Jolt picks its code paths from the
    # compiler's predefined macros (__AVX2__ ...), and EngineCore hands
    # /arch:AVX2 or -mavx2 -mfma to everything that links it -- which
    # JoltPhysics does, and this library does not. So it takes EngineCore's
    # flags explicitly. GCC and Clang need three more: Jolt treats AVX2 as
    # implying LZCNT, TZCNT (BMI) and F16C, whose intrinsics are always_inline
    # and refuse to inline without them -- PUBLIC, because Jolt's headers use
    # them too.
    target_compile_options(Jolt PRIVATE $<TARGET_PROPERTY:EngineCore::EngineCore,INTERFACE_COMPILE_OPTIONS>)
    get_target_property(JOLTPHYSICS_CORE_OPTIONS EngineCore::EngineCore INTERFACE_COMPILE_OPTIONS)
    if(NOT MSVC AND JOLTPHYSICS_CORE_OPTIONS MATCHES "-mavx2")
        target_compile_options(Jolt PUBLIC -mbmi -mlzcnt -mf16c)
    endif()

    # Vendored: its warnings are upstream's.
    if(MSVC)
        target_compile_options(Jolt PRIVATE /W0)
    else()
        target_compile_options(Jolt PRIVATE -w)
    endif()

    set_target_properties(Jolt PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            FOLDER "Engine/External")
    source_group(TREE "${JOLTPHYSICS_JOLT_DIR}" PREFIX "External" FILES ${JOLTPHYSICS_JOLT_SOURCES})
endif()

# ===========================================================================
# Sources
# ===========================================================================

set(JOLTPHYSICS_SOURCES
        Source/Private/JoltPhysics/JoltRuntime.h
        Source/Private/JoltPhysics/JoltRuntime.cpp
        Source/Private/JoltPhysics/JoltLayers.h
        Source/Private/JoltPhysics/JoltConvert.h
        Source/Private/JoltPhysics/JoltShapes.h
        Source/Private/JoltPhysics/JoltShapes.cpp
        Source/Private/JoltPhysics/PhysicsWorld.cpp
        Source/Private/JoltPhysics/PhysicsSettings.cpp
        Source/Public/JoltPhysics/JoltPhysicsAPI.h
        Source/Public/JoltPhysics/PhysicsWorld.h
)

# SHARED, like GameCore and NetCore: one Jolt runtime per process. Jolt is
# PRIVATE -- no public header includes a Jolt header.
nimble_add_module(JoltPhysics SHARED
        PREFIX       JOLTPHYSICS
        SOURCES      ${JOLTPHYSICS_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS Jolt::Jolt)

target_include_directories(JoltPhysics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(JoltPhysics PROPERTIES FOLDER "Engine")

source_group("JoltPhysics" FILES ${JOLTPHYSICS_SOURCES})

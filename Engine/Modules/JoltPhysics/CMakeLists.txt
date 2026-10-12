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

# === Jolt ===================================================================
# Jolt Physics 5.6.0 (MIT) is vendored at Engine/ThirdParty/Source and built by
# Engine/ThirdParty/CMakeLists.txt as Jolt::Jolt: STATIC, so it lives inside
# JoltPhysics.dll alone -- exactly one copy of Jolt's factory and type registry
# in the process, as GDI lives in RenderCore. Built with EngineCore's SIMD
# flags, and its layout defines (JPH_OBJECT_LAYER_BITS, Debug asserts) reach
# this module through Jolt::Jolt (ThirdParty/cmake/NimbleThirdPartyUsage.cmake).

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

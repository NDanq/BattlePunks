cmake_minimum_required(VERSION 3.16)

project(RenderCore VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# RenderCore -- the render stack: the render thread, the game-thread/render-
# thread command queue (ENQUEUE_RENDER_COMMAND), and RenderProxy, the
# render-thread mirror of a GameCore entity that draws.
#
# Deliberately NOT standalone-configurable, matching GameCore and GameWindow:
# RenderCore lives in this repository and is free to use nimble_add_module().
#
# See DESIGN.md for the full design; this file only wires the module up.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "RenderCore is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure -- see the note at "
            "the top of this file and Modules/GameCore's CMakeLists.txt for the "
            "same discipline applied there.")
endif()

if(NOT TARGET GDI)
    message(FATAL_ERROR
            "RenderCore requires the GDI target to exist. Engine/CMakeLists.txt guards "
            "add_subdirectory(Modules/RenderCore) on TARGET GDI for exactly this reason; "
            "reaching this message means that guard was bypassed.")
endif()

option(RENDERCORE_BUILD_SMOKETEST "Build RenderCoreSmokeTest" OFF)

# ===========================================================================
# Sources
# ===========================================================================
#
# The full module -- see DESIGN.md for the design and README.md for usage.
# Grown one build-order step at a time (skeleton; RenderThread + commands;
# proxies/scene/the quad primitive; WorldBridge + the Renderer facade;
# Engine/Sandbox integration), each step appending its own files here rather
# than pre-declaring empty ones, so an incomplete file never silently joined
# the build ahead of the step that actually implemented it.
set(RENDERCORE_SOURCES
        Source/Private/RenderCore/RenderCoreInternal.h
        Source/Private/RenderCore/RenderCore.cpp
        Source/Private/RenderCore/RenderThreadInternal.h
        Source/Private/RenderCore/RenderThread.cpp
        Source/Private/RenderCore/RenderCommand.cpp
        Source/Private/RenderCore/RenderContext.cpp
        Source/Private/RenderCore/RenderProxy.cpp
        Source/Private/RenderCore/RenderScene.cpp
        Source/Private/RenderCore/PrimitiveProxy.h
        Source/Private/RenderCore/MaterialPreview.h
        Source/Private/RenderCore/PrimitiveProxy.cpp
        Source/Private/RenderCore/RenderComponents.cpp
        Source/Private/RenderCore/WorldBridge.h
        Source/Private/RenderCore/WorldBridge.cpp
        Source/Private/RenderCore/Renderer.cpp
        Source/Public/RenderCore/RenderCoreAPI.h
        Source/Public/RenderCore/RenderCoreLog.h
        Source/Public/RenderCore/RenderTypes.h
        Source/Public/RenderCore/RenderCoreVersion.h
        Source/Public/RenderCore/RenderCommand.h
        Source/Public/RenderCore/RenderThread.h
        Source/Public/RenderCore/RenderContext.h
        Source/Public/RenderCore/RenderView.h
        Source/Public/RenderCore/RenderProxy.h
        Source/Public/RenderCore/RenderScene.h
        Source/Public/RenderCore/RenderComponents.h
        Source/Public/RenderCore/Renderer.h
)

# ===========================================================================
# The RenderCore target
# ===========================================================================
#
# ALWAYS SHARED -- there is no RENDERCORE_SHARED option, matching GameCore's
# own ALWAYS SHARED stance. RenderCore owns process-global state (the render
# command queue, the render-thread identity behind IsInRenderThread(), the
# proxy-type registry) that must exist exactly once; see RenderCoreAPI.h's
# comment for the full argument, which is GameCoreAPI.h's registry argument
# applied here.
#
# GDI::GDI is PRIVATE, and load-bearing rather than tidy: no public RenderCore
# header includes a GDI header -- RenderCoreInternal.h is the only one that
# does, and it lives under Private/ (see its own comment for why that matters,
# matching GameCoreInternal.h's lua.h discipline and GameWindowInternal.h's
# SDL.h discipline). GDI is a STATIC library, so RenderCore.dll is the ONLY
# image in the process allowed to link it -- see RenderCoreInternal.h's
# comment for what goes wrong if a second image did. RenderCoreSmokeTest links
# RenderCore and nothing else -- not GDI::GDI -- which is the structural check
# that this stays true, the same role GameCoreSmokeTest plays for Lua.
nimble_add_module(RenderCore SHARED
        PREFIX       RENDERCORE
        SOURCES      ${RENDERCORE_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS GDI::GDI)

# PRIVATE, not PUBLIC: RenderCoreInternal.h is included as
# "Private/RenderCore/RenderCoreInternal.h" from this module's own .cpp files
# and must not be reachable from a consumer. nimble_add_module already added
# Source/ PUBLIC for the "Public/RenderCore/..." includes.
target_include_directories(RenderCore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

if(RENDERCORE_BUILD_SMOKETEST)
    add_executable(RenderCoreSmokeTest Tests/SmokeMain.cpp)
    target_link_libraries(RenderCoreSmokeTest PRIVATE RenderCore::RenderCore)
    set_target_properties(RenderCoreSmokeTest PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

    enable_testing()
    add_test(NAME RenderCore.Smoke COMMAND RenderCoreSmokeTest)
    set_tests_properties(RenderCore.Smoke PROPERTIES
        TIMEOUT 120
        LABELS  "smoke"
    )
endif()

source_group("RenderCore" FILES ${RENDERCORE_SOURCES})

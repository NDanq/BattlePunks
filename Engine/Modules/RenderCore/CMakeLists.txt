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
option(RENDERCORE_BUILD_ANIMATION_TESTS "Build RenderCoreAnimationTests (headless; no window or GPU)" ON)

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
        Source/Private/RenderCore/ParallelRecording.cpp
        Source/Private/RenderCore/RenderCommand.cpp
        Source/Private/RenderCore/RenderContext.cpp
        Source/Private/RenderCore/DrawPolicy.h
        Source/Private/RenderCore/RenderPipeline.cpp
        Source/Private/RenderCore/SceneTargets.h
        Source/Private/RenderCore/RenderProxy.cpp
        Source/Private/RenderCore/RenderScene.cpp
        Source/Private/RenderCore/RenderComponentProxy.h
        Source/Private/RenderCore/MaterialPreview.h
        Source/Private/RenderCore/RenderComponentProxy.cpp
        Source/Private/RenderCore/Components/RenderComponent.cpp
        Source/Private/RenderCore/Components/ParticleSystemComponent.cpp
        Source/Private/RenderCore/Components/TerrainComponent.cpp
        Source/Private/RenderCore/ParticleSimulation.h
        Source/Private/RenderCore/ParticleSimulation.cpp
        Source/Private/RenderCore/ParticleSystemAsset.cpp
        Source/Private/RenderCore/Components/CameraComponent.cpp
        Source/Private/RenderCore/Components/DirectionalLightComponent.cpp
        Source/Private/RenderCore/Components/SkyLightComponent.cpp
        Source/Private/RenderCore/Components/PointLightComponent.cpp
        Source/Private/RenderCore/Components/SpotLightComponent.cpp
        Source/Private/RenderCore/ShadowSetup.h
        Source/Private/RenderCore/ShadowSetup.cpp
        Source/Private/RenderCore/ShadowSettings.cpp
        Source/Private/RenderCore/PostProcessStack.cpp
        Source/Private/RenderCore/Components/PostProcessComponent.cpp
        Source/Private/RenderCore/Components/SceneCaptureProbeComponent.cpp
        Source/Private/RenderCore/Components/ReflectionProbeComponent.cpp
        Source/Private/RenderCore/AssetFileCommon.h
        Source/Private/RenderCore/MeshAsset.cpp
        Source/Private/RenderCore/AssetCacheCommon.h
        Source/Private/RenderCore/SkeletonAsset.cpp
        Source/Private/RenderCore/AnimClipAsset.cpp
        Source/Private/RenderCore/AnimPose.cpp
        Source/Private/RenderCore/AnimStateAsset.cpp
        Source/Private/RenderCore/AnimInstance.cpp
        Source/Private/RenderCore/AnimationSystem.cpp
        Source/Private/RenderCore/Components/AnimationComponent.cpp
        Source/Private/RenderCore/Components/AnimSocketComponent.cpp
        Source/Private/RenderCore/BakedLightingAsset.cpp
        Source/Private/RenderCore/TextureAsset.cpp
        Source/Private/RenderCore/TextureStreaming.cpp
        Source/Private/RenderCore/TextureStreaming.h
        Source/Private/RenderCore/TextureCompression.cpp
        Source/Private/RenderCore/RenderTargetAsset.cpp
        Source/Private/RenderCore/SceneCapture.cpp
        Source/Private/RenderCore/SceneCapture.h
        Source/Private/RenderCore/WorldBridge.h
        Source/Private/RenderCore/WorldBridge.cpp
        Source/Private/RenderCore/Renderer.cpp
        Source/Private/RenderCore/MaterialDescription.cpp
        Source/Private/RenderCore/ShaderCache.cpp
        Source/Private/RenderCore/UIDrawList.cpp
        Source/Private/RenderCore/UIRenderer.h
        Source/Private/RenderCore/UIRenderer.cpp
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
        Source/Public/RenderCore/AssetImportInfo.h
        Source/Public/RenderCore/MeshAsset.h
        Source/Public/RenderCore/AnimMath.h
        Source/Public/RenderCore/SkeletonAsset.h
        Source/Public/RenderCore/AnimClipAsset.h
        Source/Public/RenderCore/AnimPose.h
        Source/Public/RenderCore/AnimStateAsset.h
        Source/Public/RenderCore/AnimInstance.h
        Source/Public/RenderCore/AnimationSystem.h
        Source/Public/RenderCore/Components/AnimationComponent.h
        Source/Public/RenderCore/Components/AnimSocketComponent.h
        Source/Public/RenderCore/BakedLightingAsset.h
        Source/Public/RenderCore/TextureAsset.h
        Source/Public/RenderCore/TextureCompression.h
        Source/Public/RenderCore/RenderTargetAsset.h
        Source/Public/RenderCore/RenderComponents.h
        Source/Public/RenderCore/Components/RenderComponent.h
        Source/Public/RenderCore/Components/ParticleSystemComponent.h
        Source/Public/RenderCore/Components/TerrainComponent.h
        Source/Public/RenderCore/ParticleSystemAsset.h
        Source/Public/RenderCore/ParticleTypes.h
        Source/Public/RenderCore/Components/CameraComponent.h
        Source/Public/RenderCore/Components/DirectionalLightComponent.h
        Source/Public/RenderCore/Components/SkyLightComponent.h
        Source/Public/RenderCore/Components/PointLightComponent.h
        Source/Public/RenderCore/Components/SpotLightComponent.h
        Source/Public/RenderCore/Components/LightShadow.h
        Source/Public/RenderCore/PostProcessStack.h
        Source/Public/RenderCore/Components/PostProcessComponent.h
        Source/Public/RenderCore/Components/SceneCaptureProbeComponent.h
        Source/Public/RenderCore/Components/ReflectionProbeComponent.h
        Source/Public/RenderCore/Renderer.h
        Source/Public/RenderCore/MaterialDescription.h
        Source/Public/RenderCore/RenderLighting.h
        Source/Public/RenderCore/RenderPipeline.h
        Source/Public/RenderCore/ShaderCache.h
        Source/Public/RenderCore/UIDrawList.h
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

# bc7enc_rdo (MIT or public domain), vendored at richgel999/bc7enc_rdo
# b943862: BC1-BC5 (rgbcx) and BC7 (bc7enc) blocks; TextureCompression.cpp.
# The decoders are for a backend without block formats, so every build has
# them; the encoder only an editor build. Its own library, built without the
# engine's warnings and, off MSVC (where /O2 clashes with Debug's /RTC1),
# optimised even in Debug -- unoptimised, BC7 is many times slower.
set(RENDERCORE_BC7ENC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Source/bc7enc_rdo-b943862")
add_library(NimbleBc7enc STATIC
        "${RENDERCORE_BC7ENC_DIR}/rgbcx.cpp"
        "${RENDERCORE_BC7ENC_DIR}/bc7decomp.cpp")
if(NIMBLE_WITH_EDITOR)
    target_sources(NimbleBc7enc PRIVATE "${RENDERCORE_BC7ENC_DIR}/bc7enc.cpp")
endif()
target_include_directories(NimbleBc7enc PUBLIC "${RENDERCORE_BC7ENC_DIR}")
if(MSVC)
    target_compile_options(NimbleBc7enc PRIVATE /w)
else()
    target_compile_options(NimbleBc7enc PRIVATE -w -O2)
endif()
set_target_properties(NimbleBc7enc PROPERTIES FOLDER "RenderCore/External" POSITION_INDEPENDENT_CODE ON)
target_link_libraries(RenderCore PRIVATE NimbleBc7enc)

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
    add_test(NAME RenderCore.Smoke COMMAND RenderCoreSmokeTest
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderCoreSmokeTest>")   # staged meshes load by relative path
    set_tests_properties(RenderCore.Smoke PROPERTIES
        TIMEOUT 120
        LABELS  "smoke"
    )
endif()

# The animation runtime, headless (ANIMATION.md): pose math, clip playback, a
# World's AnimationSystem. Writes its assets beside the executable, with the
# Save*File writers, so editor builds only.
if(RENDERCORE_BUILD_ANIMATION_TESTS AND NIMBLE_WITH_EDITOR)
    add_executable(RenderCoreAnimationTests Tests/AnimationTests.cpp)
    target_link_libraries(RenderCoreAnimationTests PRIVATE RenderCore::RenderCore)
    set_target_properties(RenderCoreAnimationTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "RenderCore")

    enable_testing()
    add_test(NAME RenderCore.Animation COMMAND RenderCoreAnimationTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderCoreAnimationTests>")
endif()

source_group("RenderCore" FILES ${RENDERCORE_SOURCES})

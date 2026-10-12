cmake_minimum_required(VERSION 3.16)

project(Audio VERSION 0.1 LANGUAGES C CXX)

# ===========================================================================
# Audio -- sound for a GameCore World, on miniaudio: one engine-wide output
# device and mixer (AudioDevice), sound assets (.nmsound) and, per World, the
# voices behind GameCore's Audio Source and Audio Listener components. See
# README.md.
#
# OPTIONAL: Engine/CMakeLists.txt adds this directory only when
# NIMBLE_WITH_AUDIO is on (the default). The components live in GameCore, so
# a level that carries them loads without this module; the engine layer warns
# that they will be silent.
#
# LANGUAGES C CXX: miniaudio and stb_vorbis are C, built by hand below like
# NetCore's ENet.
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "Audio is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure.")
endif()

option(AUDIO_BUILD_TESTS "Build the Audio tests" ON)

# === Vendored miniaudio =====================================================
# miniaudio 0.11.25 (https://miniaud.io, MIT-0 or public domain -- LICENSE
# beside it): device I/O on every desktop backend, decoders (WAV, FLAC, MP3;
# Vorbis through the stb_vorbis it ships in extras/), a resource manager with
# its own job threads, a node-graph mixer and a 3D spatializer. One header;
# MiniaudioImpl.c is its single implementation unit.
set(AUDIO_MINIAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/Source/miniaudio-0.11.25")

if(NOT EXISTS "${AUDIO_MINIAUDIO_DIR}/miniaudio.h")
    message(FATAL_ERROR "Vendored miniaudio not found at ${AUDIO_MINIAUDIO_DIR}.")
endif()

if(NOT TARGET miniaudio::miniaudio)
    add_library(miniaudio STATIC Source/Private/Audio/MiniaudioImpl.c)
    add_library(miniaudio::miniaudio ALIAS miniaudio)

    target_include_directories(miniaudio PUBLIC "${AUDIO_MINIAUDIO_DIR}")

    # PUBLIC, so every unit that includes miniaudio.h sees the same build of
    # it. No encoders: the engine only ever reads sound.
    target_compile_definitions(miniaudio PUBLIC MA_NO_ENCODING)

    find_package(Threads REQUIRED)
    target_link_libraries(miniaudio PUBLIC Threads::Threads)
    if(APPLE)
        # Linked, not dlopen'd: a hardened runtime refuses the dlopen.
        target_compile_definitions(miniaudio PUBLIC MA_NO_RUNTIME_LINKING)
        target_link_libraries(miniaudio PUBLIC
                "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    elseif(UNIX)
        # Backends (PipeWire, PulseAudio, ALSA, JACK) are dlopen'd at runtime,
        # so a machine without one of them still starts.
        target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} m)
    endif()

    # Vendored: its warnings are upstream's.
    if(MSVC)
        target_compile_options(miniaudio PRIVATE /W0)
    else()
        target_compile_options(miniaudio PRIVATE -w)
    endif()

    # Linked into Audio.dll, so position-independent (GameCore's Lua reason).
    set_target_properties(miniaudio PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
            FOLDER "Engine/External")
endif()

# ===========================================================================
# Sources
# ===========================================================================

set(AUDIO_SOURCES
        Source/Private/Audio/AudioInternal.h
        Source/Private/Audio/AudioDevice.cpp
        Source/Private/Audio/SoundAsset.cpp
        Source/Private/Audio/AudioWorld.cpp
        Source/Public/Audio/AudioAPI.h
        Source/Public/Audio/AudioDevice.h
        Source/Public/Audio/SoundAsset.h
        Source/Public/Audio/AudioWorld.h
)

# SHARED, like NetCore and JoltPhysics: one device and mixer per process.
# miniaudio is PRIVATE -- no public header includes miniaudio.h.
nimble_add_module(Audio SHARED
        PREFIX       AUDIO
        SOURCES      ${AUDIO_SOURCES}
        PUBLIC_DEPS  GameCore::GameCore
        PRIVATE_DEPS miniaudio::miniaudio)

target_include_directories(Audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(Audio PROPERTIES FOLDER "Engine")

if(AUDIO_BUILD_TESTS)
    add_executable(AudioTests Tests/AudioTests.cpp)
    target_link_libraries(AudioTests PRIVATE Audio::Audio)
    set_target_properties(AudioTests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Engine")
    add_test(NAME Audio.Tests COMMAND AudioTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:AudioTests>")
    set_tests_properties(Audio.Tests PROPERTIES TIMEOUT 120 LABELS "smoke")
endif()

source_group("Audio" FILES ${AUDIO_SOURCES})

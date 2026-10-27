# ===========================================================================
# NimbleThirdPartyUsage.cmake -- what each vendored library IS to the code that
# uses it: where its sources live, and its usage requirements (include
# directories, PUBLIC defines, link dependencies).
#
# ONE DESCRIPTION, TWO USERS. ThirdParty/CMakeLists.txt applies it to the real
# targets it builds; Engine/cmake/NimbleThirdParty.cmake applies it to the
# IMPORTED targets that stand for the prebuilt libraries. A define that
# changes a library's layout (JPH_OBJECT_LAYER_BITS, MA_NO_ENCODING) is spelled
# here once, so the library and its users cannot drift apart.
#
# Header-only libraries (cgltf, stb) and MikkTSpace (one C file, compiled by
# the module that uses it) have no target: their directories are here for the
# modules that include them.
#
# Versions are spelled out rather than globbed: which release a commit builds
# against is answerable from the source, not from whatever is on disk.
# Bumping one is the new directory under Source/ plus its line here.
# ===========================================================================

include_guard(GLOBAL)

get_filename_component(NIMBLE_THIRDPARTY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../Source" ABSOLUTE)

set(NIMBLE_THIRDPARTY_SDL_DIR      "${NIMBLE_THIRDPARTY_SOURCE_DIR}/SDL-release-3.4.16")
set(NIMBLE_THIRDPARTY_JOLT_DIR     "${NIMBLE_THIRDPARTY_SOURCE_DIR}/JoltPhysics-5.6.0")
set(NIMBLE_THIRDPARTY_LUA_DIR      "${NIMBLE_THIRDPARTY_SOURCE_DIR}/lua-5.4.8")
set(NIMBLE_THIRDPARTY_PUGIXML_DIR  "${NIMBLE_THIRDPARTY_SOURCE_DIR}/pugixml-1.16")
set(NIMBLE_THIRDPARTY_ENET_DIR     "${NIMBLE_THIRDPARTY_SOURCE_DIR}/enet-1.3.18")
set(NIMBLE_THIRDPARTY_MINIAUDIO_DIR "${NIMBLE_THIRDPARTY_SOURCE_DIR}/miniaudio-0.11.25")
set(NIMBLE_THIRDPARTY_BC7ENC_DIR   "${NIMBLE_THIRDPARTY_SOURCE_DIR}/bc7enc_rdo-b943862")
set(NIMBLE_THIRDPARTY_XATLAS_DIR   "${NIMBLE_THIRDPARTY_SOURCE_DIR}/xatlas-f700c77")
set(NIMBLE_THIRDPARTY_CGLTF_DIR    "${NIMBLE_THIRDPARTY_SOURCE_DIR}/cgltf-1.15")
set(NIMBLE_THIRDPARTY_MIKKTSPACE_DIR "${NIMBLE_THIRDPARTY_SOURCE_DIR}/mikktspace-3e895b4")
set(NIMBLE_THIRDPARTY_STB_DIR      "${NIMBLE_THIRDPARTY_SOURCE_DIR}/stb")
set(NIMBLE_THIRDPARTY_RECAST_DIR   "${NIMBLE_THIRDPARTY_SOURCE_DIR}/recastnavigation-1.6.0")
set(NIMBLE_THIRDPARTY_ZSTD_DIR     "${NIMBLE_THIRDPARTY_SOURCE_DIR}/zstd-1.5.7")

# The compiled libraries: <Name> <target> pairs. <Name> is the key both sides
# use (manifest variables, nimble_thirdparty_usage); <target> is the name the
# engine links, an alias or IMPORTED target.
set(NIMBLE_THIRDPARTY_LIBRARIES
        SDL3      SDL3::SDL3
        Jolt      Jolt::Jolt
        Lua       Lua::Lua
        PugiXml   PugiXml::PugiXml
        ENet      ENet::ENet
        miniaudio miniaudio::miniaudio
        Bc7enc    NimbleBc7enc
        Xatlas    NimbleXatlas
        RecastNav RecastNav::RecastNav
        Zstd      Zstd::Zstd)

# nimble_thirdparty_usage(<Name> <target> BUILD|IMPORTED)
#   Applies <Name>'s usage requirements to <target>. BUILD: the real library
#   (PUBLIC, and link-only dependencies PRIVATE, which a static library hands
#   on as $<LINK_ONLY:>). IMPORTED: the prebuilt stand-in (all INTERFACE).
#
#   NIMBLE_THIRDPARTY_SIMD_FLAGS must be set: EngineCore's instruction-set
#   flags (/arch:AVX2, -mavx2 -mfma), which Jolt is built with.
function(nimble_thirdparty_usage NM_NAME NM_TARGET NM_MODE)
    if(NM_MODE STREQUAL "BUILD")
        set(NM_PUBLIC PUBLIC)
        set(NM_LINK_ONLY PRIVATE)
    elseif(NM_MODE STREQUAL "IMPORTED")
        set(NM_PUBLIC INTERFACE)
        set(NM_LINK_ONLY INTERFACE)
    else()
        message(FATAL_ERROR "nimble_thirdparty_usage: mode is BUILD or IMPORTED, not '${NM_MODE}'")
    endif()

    if(NM_NAME STREQUAL "SDL3")
        # BUILD: SDL's own CMakeLists already describes SDL3::SDL3.
        if(NM_MODE STREQUAL "IMPORTED")
            target_include_directories(${NM_TARGET} INTERFACE "${NIMBLE_THIRDPARTY_SDL_DIR}/include")
        endif()

    elseif(NM_NAME STREQUAL "Jolt")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_JOLT_DIR}")
        target_compile_features(${NM_TARGET} ${NM_PUBLIC} cxx_std_17)
        # Every translation unit that includes a Jolt header agrees with the
        # library: 32-bit object layers (JoltLayers.h packs layer and mask in
        # them) and asserts in Debug. Jolt's version check (JoltRuntime.cpp) is
        # the backstop if they ever drift.
        target_compile_definitions(${NM_TARGET} ${NM_PUBLIC}
                JPH_OBJECT_LAYER_BITS=32
                $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>)
        # GCC and Clang: Jolt treats AVX2 as implying LZCNT, TZCNT (BMI) and
        # F16C, whose intrinsics are always_inline and refuse to inline without
        # them -- and Jolt's headers use them too.
        if(NOT MSVC AND NIMBLE_THIRDPARTY_SIMD_FLAGS MATCHES "-mavx2")
            target_compile_options(${NM_TARGET} ${NM_PUBLIC} -mbmi -mlzcnt -mf16c)
        endif()

    elseif(NM_NAME STREQUAL "Lua")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_LUA_DIR}/src")
        if(UNIX AND NOT APPLE)
            target_link_libraries(${NM_TARGET} ${NM_LINK_ONLY} m ${CMAKE_DL_LIBS})
        endif()

    elseif(NM_NAME STREQUAL "PugiXml")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_PUGIXML_DIR}/src")
        # Unused, and most of the library.
        target_compile_definitions(${NM_TARGET} ${NM_PUBLIC} PUGIXML_NO_XPATH)
        target_compile_features(${NM_TARGET} ${NM_PUBLIC} cxx_std_17)

    elseif(NM_NAME STREQUAL "ENet")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_ENET_DIR}/include")
        if(WIN32)
            target_link_libraries(${NM_TARGET} ${NM_PUBLIC} ws2_32 winmm)
        endif()

    elseif(NM_NAME STREQUAL "miniaudio")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_MINIAUDIO_DIR}")
        # Every unit that includes miniaudio.h sees the same build of it. No
        # encoders: the engine only ever reads sound.
        target_compile_definitions(${NM_TARGET} ${NM_PUBLIC} MA_NO_ENCODING)
        find_package(Threads REQUIRED)
        target_link_libraries(${NM_TARGET} ${NM_PUBLIC} Threads::Threads)
        if(APPLE)
            # Linked, not dlopen'd: a hardened runtime refuses the dlopen.
            target_compile_definitions(${NM_TARGET} ${NM_PUBLIC} MA_NO_RUNTIME_LINKING)
            target_link_libraries(${NM_TARGET} ${NM_PUBLIC}
                    "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
        elseif(UNIX)
            # Backends (PipeWire, PulseAudio, ALSA, JACK) are dlopen'd at
            # runtime, so a machine without one of them still starts.
            target_link_libraries(${NM_TARGET} ${NM_PUBLIC} ${CMAKE_DL_LIBS} m)
        endif()

    elseif(NM_NAME STREQUAL "Bc7enc")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_BC7ENC_DIR}")

    elseif(NM_NAME STREQUAL "Xatlas")
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_XATLAS_DIR}")
        find_package(Threads REQUIRED)
        target_link_libraries(${NM_TARGET} ${NM_LINK_ONLY} Threads::Threads)

    elseif(NM_NAME STREQUAL "RecastNav")
        foreach(NM_PART Recast Detour DetourCrowd DetourTileCache DebugUtils)
            target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_RECAST_DIR}/${NM_PART}/Include")
        endforeach()
        # dtQueryFilter's passFilter / getCost become virtual, so AICore can
        # subclass it for team and area costs. It changes the class's layout,
        # so the library and every unit that includes DetourNavMeshQuery.h
        # must agree. DT_POLYREF64 stays off: 32-bit poly refs are plenty
        # for one level's tiles.
        target_compile_definitions(${NM_TARGET} ${NM_PUBLIC} DT_VIRTUAL_QUERYFILTER)

    elseif(NM_NAME STREQUAL "Zstd")
        # zstd.h and zstd_errors.h only; nothing outside lib/ is vendored.
        target_include_directories(${NM_TARGET} ${NM_PUBLIC} "${NIMBLE_THIRDPARTY_ZSTD_DIR}/lib")

    else()
        message(FATAL_ERROR "nimble_thirdparty_usage: no library named '${NM_NAME}'")
    endif()
endfunction()

# ===========================================================================
# NimbleThirdParty.cmake -- the vendored libraries (Engine/ThirdParty), built
# once per toolchain and linked by every build tree that uses it.
#
# Included by Engine/CMakeLists.txt after Modules/Core (Jolt needs EngineCore's
# instruction-set flags) and before every module that links a library.
#
# NIMBLE_PREBUILT_THIRDPARTY=ON (the default)
#   Engine/ThirdParty is configured as a project of its own and built into
#   Engine/Intermediate/ThirdParty/<key>/, and SDL3::SDL3, Jolt::Jolt, Lua::Lua
#   and the rest are IMPORTED targets over the result. The first tree on a
#   toolchain pays for the build, once; every later configure only asks the
#   prebuild's own build system whether anything changed (a no-op in a second).
#   That check runs at CONFIGURE time: after editing a vendored source,
#   reconfigure (or turn this off) to have it rebuilt.
#
# NIMBLE_PREBUILT_THIRDPARTY=OFF
#   Engine/ThirdParty is added to this tree as a subdirectory, so the libraries
#   are built here with this tree's flags -- for stepping through SDL or Jolt,
#   or bumping one.
#
# <key> names what changes the compiled libraries and nothing else: platform,
# compiler, generator, base flags and EngineCore's SIMD flags. Editor, Client
# and Server trees share one; so do all of a game's configurations, mapped onto
# the two the prebuild makes (Debug; Release for RelWithDebInfo and Shipping).
# A single-config tree builds only the one it needs. Each prebuild records its
# toolchain in <key>/Toolchain.txt.
#
# Concurrent configures (an IDE loading several profiles) serialise on a lock
# per key, so one builds and the rest wait, then find it built.
# ===========================================================================

option(NIMBLE_PREBUILT_THIRDPARTY
       "Build the vendored libraries once per toolchain into Engine/Intermediate/ThirdParty (off: build them in this tree)" ON)

get_filename_component(NIMBLE_THIRDPARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty" ABSOLUTE)
include("${NIMBLE_THIRDPARTY_DIR}/cmake/NimbleThirdPartyUsage.cmake")

if(NOT TARGET EngineCore)
    message(FATAL_ERROR "NimbleThirdParty.cmake: include it after Modules/Core (Jolt takes EngineCore's SIMD flags).")
endif()

# THE SAME SIMD AS EVERYTHING ELSE: EngineCore hands its instruction-set flags
# to whatever links it, and Jolt -- which picks code paths from __AVX2__ and
# friends -- has to be built with them too.
get_target_property(NIMBLE_THIRDPARTY_SIMD_FLAGS EngineCore INTERFACE_COMPILE_OPTIONS)
if(NOT NIMBLE_THIRDPARTY_SIMD_FLAGS)
    set(NIMBLE_THIRDPARTY_SIMD_FLAGS "")
endif()

# nimble_stage_thirdparty_runtime(<target>)
#   Copies the prebuilt SDL3 shared library next to <target>'s output after it
#   links: into the flat runtime directory, where the exe's loader finds it.
#   Built in this tree (NIMBLE_PREBUILT_THIRDPARTY=OFF), SDL lands there itself.
function(nimble_stage_thirdparty_runtime NM_TARGET)
    get_target_property(NM_IMPORTED SDL3::SDL3 IMPORTED)
    if(NOT NM_IMPORTED)
        return()
    endif()
    if(WIN32)
        set(NM_NAME "$<TARGET_FILE_NAME:SDL3::SDL3>")
    else()
        set(NM_NAME "$<TARGET_SONAME_FILE_NAME:SDL3::SDL3>")
    endif()
    add_custom_command(TARGET ${NM_TARGET} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${NM_TARGET}>/${NM_NAME}"
            VERBATIM)
endfunction()

if(NOT NIMBLE_PREBUILT_THIRDPARTY)
    message(STATUS "NimbleEngine: third-party libraries build in this tree (NIMBLE_PREBUILT_THIRDPARTY=OFF)")
    # EXCLUDE_FROM_ALL: only what a module links is built.
    add_subdirectory("${NIMBLE_THIRDPARTY_DIR}" "${CMAKE_BINARY_DIR}/ThirdParty" EXCLUDE_FROM_ALL)
    return()
endif()

# === Toolchain key ==========================================================
# The C compiler, when this tree has none yet: the engine project is CXX-only,
# and C is first enabled by the modules below. cl compiles both; a clang++ has
# its clang beside it.
if(CMAKE_C_COMPILER)
    set(NM_TP_C_COMPILER "${CMAKE_C_COMPILER}")
elseif(MSVC)
    set(NM_TP_C_COMPILER "${CMAKE_CXX_COMPILER}")
elseif(CMAKE_CXX_COMPILER MATCHES "clang\\+\\+")
    string(REGEX REPLACE "clang\\+\\+" "clang" NM_TP_C_COMPILER "${CMAKE_CXX_COMPILER}")
else()
    set(NM_TP_C_COMPILER "")
endif()

string(REPLACE ";" "|" NM_TP_SIMD_ARG "${NIMBLE_THIRDPARTY_SIMD_FLAGS}")

set(NM_TP_TOOLCHAIN
        "System:     ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR} (${CMAKE_SIZEOF_VOID_P}-byte pointers)"
        "Generator:  ${CMAKE_GENERATOR} ${CMAKE_GENERATOR_PLATFORM} ${CMAKE_GENERATOR_TOOLSET}"
        "Toolchain:  ${CMAKE_TOOLCHAIN_FILE}"
        "C:          ${NM_TP_C_COMPILER}"
        "C++:        ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})"
        "C++ flags:  ${CMAKE_CXX_FLAGS}"
        "SIMD:       ${NM_TP_SIMD_ARG}"
        "macOS:      ${CMAKE_OSX_ARCHITECTURES} ${CMAKE_OSX_DEPLOYMENT_TARGET}")
string(REPLACE ";" "\n" NM_TP_TOOLCHAIN "${NM_TP_TOOLCHAIN}")
string(SHA1 NM_TP_HASH "${NM_TP_TOOLCHAIN}")
string(SUBSTRING "${NM_TP_HASH}" 0 8 NM_TP_HASH)
string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" NM_TP_COMPILER)
string(REGEX MATCH "^[0-9]+\\.[0-9]+" NM_TP_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(NM_TP_KEY "${NM_TP_COMPILER}${NM_TP_COMPILER_VERSION}-${NM_TP_HASH}")

get_filename_component(NIMBLE_THIRDPARTY_INTERMEDIATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../Intermediate/ThirdParty" ABSOLUTE)
set(NIMBLE_THIRDPARTY_OUTPUT_DIR "${NIMBLE_THIRDPARTY_INTERMEDIATE_DIR}/${NM_TP_KEY}")

# The configurations this tree links.
get_property(NM_TP_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NM_TP_MULTI_CONFIG)
    set(NM_TP_CONFIGS Debug Release)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(NM_TP_CONFIGS Debug)
else()
    set(NM_TP_CONFIGS Release)
endif()

# === Prebuild ===============================================================
# nm_tp_prebuild(): configure (first time only) and build each configuration,
# under the key's lock. A function so the lock is released on return.
function(nm_tp_prebuild)
    file(MAKE_DIRECTORY "${NIMBLE_THIRDPARTY_OUTPUT_DIR}")
    file(LOCK "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/.lock" GUARD FUNCTION TIMEOUT 7200
         RESULT_VARIABLE NM_LOCK_RESULT)
    if(NOT NM_LOCK_RESULT EQUAL 0)
        message(FATAL_ERROR "NimbleEngine: could not lock ${NIMBLE_THIRDPARTY_OUTPUT_DIR} (${NM_LOCK_RESULT})")
    endif()
    file(WRITE "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/Toolchain.txt" "${NM_TP_TOOLCHAIN}\n")

    set(NM_ARGS
            -G "${CMAKE_GENERATOR}"
            "-DNIMBLE_THIRDPARTY_OUTPUT_DIR=${NIMBLE_THIRDPARTY_OUTPUT_DIR}"
            "-DNIMBLE_THIRDPARTY_SIMD_FLAGS=${NM_TP_SIMD_ARG}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}")
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND NM_ARGS -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND NM_ARGS -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    if(CMAKE_MAKE_PROGRAM AND NOT NM_TP_MULTI_CONFIG)
        list(APPEND NM_ARGS "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    if(NM_TP_C_COMPILER)
        list(APPEND NM_ARGS "-DCMAKE_C_COMPILER=${NM_TP_C_COMPILER}")
    endif()
    foreach(NM_VAR CMAKE_TOOLCHAIN_FILE CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET
                   CMAKE_SHARED_LINKER_FLAGS)
        if(${NM_VAR})
            list(APPEND NM_ARGS "-D${NM_VAR}=${${NM_VAR}}")
        endif()
    endforeach()

    foreach(NM_CFG IN LISTS NM_TP_CONFIGS)
        if(NM_TP_MULTI_CONFIG)
            set(NM_BUILD_DIR "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/Build")
            set(NM_CFG_ARGS)
        else()
            set(NM_BUILD_DIR "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/Build/${NM_CFG}")
            set(NM_CFG_ARGS "-DCMAKE_BUILD_TYPE=${NM_CFG}")
        endif()

        if(NOT EXISTS "${NM_BUILD_DIR}/CMakeCache.txt")
            message(STATUS "NimbleEngine: configuring the third-party libraries (${NM_CFG}) in ${NM_BUILD_DIR}")
            execute_process(
                    COMMAND "${CMAKE_COMMAND}" -S "${NIMBLE_THIRDPARTY_DIR}" -B "${NM_BUILD_DIR}" ${NM_ARGS} ${NM_CFG_ARGS}
                    RESULT_VARIABLE NM_RESULT)
            if(NOT NM_RESULT EQUAL 0)
                file(REMOVE "${NM_BUILD_DIR}/CMakeCache.txt")   # so the next configure starts over
                message(FATAL_ERROR "NimbleEngine: configuring the third-party libraries failed (${NM_RESULT}); see the output above")
            endif()
        endif()

        if(NOT EXISTS "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/.built")
            message(STATUS "NimbleEngine: building the third-party libraries (${NM_CFG}) -- once for this toolchain, "
                           "shared by every build tree on it")
        endif()
        execute_process(
                COMMAND "${CMAKE_COMMAND}" --build "${NM_BUILD_DIR}" --config ${NM_CFG}
                RESULT_VARIABLE NM_RESULT)
        if(NOT NM_RESULT EQUAL 0)
            file(REMOVE "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/.built")
            message(FATAL_ERROR "NimbleEngine: building the third-party libraries (${NM_CFG}) failed (${NM_RESULT}); "
                                "see the output above, or configure with -DNIMBLE_PREBUILT_THIRDPARTY=OFF to build them in this tree")
        endif()
        file(TOUCH "${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/.built")
    endforeach()
endfunction()

nm_tp_prebuild()

# The prebuild's own sources: an edit to its CMake reconfigures this tree, which
# rebuilds it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${NIMBLE_THIRDPARTY_DIR}/CMakeLists.txt"
        "${NIMBLE_THIRDPARTY_DIR}/cmake/NimbleThirdPartyUsage.cmake")

# === IMPORTED targets =======================================================
# GLOBAL, so a game's own CMakeLists above the engine sees them too.
# Debug links the Debug build; every other configuration (RelWithDebInfo,
# Shipping, Release, MinSizeRel) the Release build -- optimised, NDEBUG and the
# release C runtime, as each of them is.
set(NM_TP_LIBRARIES ${NIMBLE_THIRDPARTY_LIBRARIES})
while(NM_TP_LIBRARIES)
    list(POP_FRONT NM_TP_LIBRARIES NM_NAME NM_TARGET)

    if(NM_NAME STREQUAL "SDL3")
        add_library(${NM_TARGET} SHARED IMPORTED GLOBAL)
    else()
        add_library(${NM_TARGET} STATIC IMPORTED GLOBAL)
    endif()

    foreach(NM_CFG IN LISTS NM_TP_CONFIGS)
        string(TOUPPER "${NM_CFG}" NM_CFG_U)
        unset(NM_TP_FILE_${NM_NAME})
        unset(NM_TP_IMPLIB_${NM_NAME})
        unset(NM_TP_SONAME_${NM_NAME})
        include("${NIMBLE_THIRDPARTY_OUTPUT_DIR}/${NM_CFG}/NimbleThirdPartyManifest.cmake")
        if(NOT EXISTS "${NM_TP_FILE_${NM_NAME}}")
            message(FATAL_ERROR "NimbleEngine: the third-party build has no ${NM_NAME} at '${NM_TP_FILE_${NM_NAME}}'")
        endif()
        set_property(TARGET ${NM_TARGET} APPEND PROPERTY IMPORTED_CONFIGURATIONS ${NM_CFG_U})
        set_target_properties(${NM_TARGET} PROPERTIES IMPORTED_LOCATION_${NM_CFG_U} "${NM_TP_FILE_${NM_NAME}}")
        if(NM_TP_IMPLIB_${NM_NAME})
            set_target_properties(${NM_TARGET} PROPERTIES IMPORTED_IMPLIB_${NM_CFG_U} "${NM_TP_IMPLIB_${NM_NAME}}")
        endif()
        if(NM_TP_SONAME_${NM_NAME})
            set_target_properties(${NM_TARGET} PROPERTIES IMPORTED_SONAME_${NM_CFG_U} "${NM_TP_SONAME_${NM_NAME}}")
        endif()
    endforeach()

    set_target_properties(${NM_TARGET} PROPERTIES
            MAP_IMPORTED_CONFIG_DEBUG          Debug
            MAP_IMPORTED_CONFIG_RELEASE        Release
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
            MAP_IMPORTED_CONFIG_MINSIZEREL     Release
            MAP_IMPORTED_CONFIG_SHIPPING       Release)
    nimble_thirdparty_usage(${NM_NAME} ${NM_TARGET} IMPORTED)
endwhile()

message(STATUS "NimbleEngine: third-party libraries prebuilt in ${NIMBLE_THIRDPARTY_OUTPUT_DIR} (${NM_TP_CONFIGS})")


# ===========================================================================
# GitVersion.cmake -- stamp a generated header with git-derived build identity.
#
# Replaces the retired Perforce "Build CL" stamp. One number synced from a
# server is gone; git's equivalent is the commit, so the canonical build id is
# `git describe` (a release tag + commits-since + short SHA) plus a CI-supplied
# monotonic build number for OS-facing version fields.
#
# ---------------------------------------------------------------------------
# Usage (from a CMakeLists.txt, after the consuming target exists):
#
#   list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#   include(GitVersion)
#   git_version_stamp(
#       PREFIX   CORECLIENT                       # macro prefix in the template
#       VERSION  "${PROJECT_VERSION}"
#       TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Public/BuildInfo.h.in"
#       OUTPUT   "${CMAKE_CURRENT_BINARY_DIR}/Generated/BuildInfo.h"
#       REPOS    "ENGINECORE=${CMAKE_CURRENT_SOURCE_DIR}/../Core"   # optional
#                "GDI=${CMAKE_CURRENT_SOURCE_DIR}/../GDI"
#       TARGET   CoreClient                       # optional dependency edge
#   )
#   target_include_directories(CoreClient PRIVATE
#       "${CMAKE_CURRENT_BINARY_DIR}/Generated")
#
# The header is written once at configure time and re-stamped on EVERY build
# by an ALL custom target. configure_file() only rewrites OUTPUT when a value
# actually changed, so a plain rebuild does not cascade into the world.
#
# Offline / no-.git builds (a source tarball) reuse the last good values from
# GitVersion.cache next to OUTPUT. An "unknown" stamp only ever means "no git
# and no cache".
#
# This file is also its own worker: the build-time target re-invokes it with
# `cmake -D GV_RUN=1 ... -P GitVersion.cmake`.
# ===========================================================================

find_package(Git QUIET)

set(_GIT_VERSION_CMAKE "${CMAKE_CURRENT_LIST_FILE}" CACHE INTERNAL "" FORCE)

# --- git <args> in <dir>, trimmed; empty output means "failed / no git" ------
function(_gv_git _dir _outvar)
    set(${_outvar} "" PARENT_SCOPE)
    if(NOT GIT_EXECUTABLE)
        return()
    endif()
    execute_process(
        COMMAND           "${GIT_EXECUTABLE}" -C "${_dir}" ${ARGN}
        OUTPUT_VARIABLE   _o
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE   _rc)
    if(_rc EQUAL 0)
        set(${_outvar} "${_o}" PARENT_SCOPE)
    endif()
endfunction()

# --- describe one working tree; sets <p>_GIT_{DESCRIBE,SHA,SHA_SHORT,BRANCH,
#     COMMIT_DATE,DIRTY} in the caller's scope ------------------------------
function(_gv_describe_dir _dir _p)
    _gv_git("${_dir}" _d  describe --tags --always --dirty --long)
    _gv_git("${_dir}" _s  rev-parse HEAD)
    _gv_git("${_dir}" _ss rev-parse --short=12 HEAD)
    _gv_git("${_dir}" _b  rev-parse --abbrev-ref HEAD)
    _gv_git("${_dir}" _cd show -s --format=%cd --date=format:%Y-%m-%dT%H:%M:%SZ HEAD)
    _gv_git("${_dir}" _porcelain status --porcelain --untracked-files=no)

    if(_d MATCHES "-dirty$" OR NOT "${_porcelain}" STREQUAL "")
        set(_dirty 1)
    else()
        set(_dirty 0)
    endif()

    set(${_p}_GIT_DESCRIBE    "${_d}"     PARENT_SCOPE)
    set(${_p}_GIT_SHA         "${_s}"     PARENT_SCOPE)
    set(${_p}_GIT_SHA_SHORT   "${_ss}"    PARENT_SCOPE)
    set(${_p}_GIT_BRANCH      "${_b}"     PARENT_SCOPE)
    set(${_p}_GIT_COMMIT_DATE "${_cd}"    PARENT_SCOPE)
    set(${_p}_GIT_DIRTY       "${_dirty}" PARENT_SCOPE)
endfunction()

# --- collect everything and write OUTPUT from TEMPLATE ----------------------
function(_gv_run)
    cmake_parse_arguments(A "" "PREFIX;TEMPLATE;OUTPUT;VERSION;SOURCE_DIR" "REPOS" ${ARGN})
    set(P "${A_PREFIX}")

    _gv_describe_dir("${A_SOURCE_DIR}" _self)

    get_filename_component(_outdir "${A_OUTPUT}" DIRECTORY)
    set(_cache "${_outdir}/GitVersion.cache")

    if("${_self_GIT_SHA}" STREQUAL "")
        # No git in this tree -- fall back to the last successful stamp.
        if(EXISTS "${_cache}")
            include("${_cache}")
            message(STATUS "GitVersion: git unavailable, reusing cached stamp (${_self_GIT_DESCRIBE})")
        else()
            message(WARNING "GitVersion: git unavailable and no cache -- stamping 'unknown'.")
        endif()
    else()
        file(WRITE "${_cache}"
            "# Last good git stamp for ${P}. Regenerated on every successful configure/build.\n"
            "set(_self_GIT_DESCRIBE    \"${_self_GIT_DESCRIBE}\")\n"
            "set(_self_GIT_SHA         \"${_self_GIT_SHA}\")\n"
            "set(_self_GIT_SHA_SHORT   \"${_self_GIT_SHA_SHORT}\")\n"
            "set(_self_GIT_BRANCH      \"${_self_GIT_BRANCH}\")\n"
            "set(_self_GIT_COMMIT_DATE \"${_self_GIT_COMMIT_DATE}\")\n"
            "set(_self_GIT_DIRTY       \"${_self_GIT_DIRTY}\")\n")
    endif()

    foreach(_k DESCRIBE SHA SHA_SHORT BRANCH COMMIT_DATE)
        if(NOT _self_GIT_${_k})
            set(_self_GIT_${_k} "unknown")
        endif()
    endforeach()
    if(NOT DEFINED _self_GIT_DIRTY OR "${_self_GIT_DIRTY}" STREQUAL "")
        set(_self_GIT_DIRTY 0)
    endif()

    set(${P}_VERSION         "${A_VERSION}")
    set(${P}_GIT_DESCRIBE    "${_self_GIT_DESCRIBE}")
    set(${P}_GIT_SHA         "${_self_GIT_SHA}")
    set(${P}_GIT_SHA_SHORT   "${_self_GIT_SHA_SHORT}")
    set(${P}_GIT_BRANCH      "${_self_GIT_BRANCH}")
    set(${P}_GIT_COMMIT_DATE "${_self_GIT_COMMIT_DATE}")
    set(${P}_GIT_DIRTY       "${_self_GIT_DIRTY}")

    # Monotonic build counter: CI supplies it, local developer builds get 0.
    set(_bn "$ENV{BUILD_NUMBER}")
    if("${_bn}" STREQUAL "")
        set(_bn "$ENV{GITHUB_RUN_NUMBER}")
    endif()
    if("${_bn}" STREQUAL "")
        set(_bn 0)
    endif()
    set(${P}_BUILD_NUMBER "${_bn}")

    string(TIMESTAMP ${P}_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

    set(_vs "${_self_GIT_DESCRIBE}")
    if(NOT "${_self_GIT_BRANCH}" STREQUAL "unknown")
        set(_vs "${_vs} (${_self_GIT_BRANCH})")
    endif()
    if(NOT "${_bn}" STREQUAL "0")
        set(_vs "${_vs} #${_bn}")
    endif()
    set(${P}_VERSION_STRING "${_vs}")

    # Dependency trees compiled into this binary ("NAME=dir" entries).
    foreach(_entry IN LISTS A_REPOS)
        if("${_entry}" STREQUAL "")
            continue()
        endif()
        string(FIND "${_entry}" "=" _eq)
        if(_eq LESS 0)
            message(WARNING "GitVersion: ignoring malformed REPOS entry '${_entry}' (expected NAME=dir)")
            continue()
        endif()
        string(SUBSTRING "${_entry}" 0 ${_eq} _name)
        math(EXPR _vstart "${_eq} + 1")
        string(SUBSTRING "${_entry}" ${_vstart} -1 _rdir)
        _gv_describe_dir("${_rdir}" _dep)
        if("${_dep_GIT_SHA_SHORT}" STREQUAL "")
            set(_dep_GIT_SHA_SHORT "unknown")
            set(_dep_GIT_DESCRIBE  "unknown")
        endif()
        set(${P}_${_name}_GIT_SHA_SHORT "${_dep_GIT_SHA_SHORT}")
        set(${P}_${_name}_GIT_DESCRIBE  "${_dep_GIT_DESCRIBE}")
    endforeach()

    get_filename_component(_od "${A_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_od}")
    configure_file("${A_TEMPLATE}" "${A_OUTPUT}" @ONLY)
endfunction()

# --- public entry point ----------------------------------------------------
function(git_version_stamp)
    cmake_parse_arguments(G "" "PREFIX;TEMPLATE;OUTPUT;VERSION;TARGET" "REPOS" ${ARGN})

    if(NOT G_PREFIX OR NOT G_TEMPLATE OR NOT G_OUTPUT)
        message(FATAL_ERROR "git_version_stamp: PREFIX, TEMPLATE and OUTPUT are required.")
    endif()

    # 1. Stamp now so the header exists for the first configure.
    _gv_run(PREFIX "${G_PREFIX}" TEMPLATE "${G_TEMPLATE}" OUTPUT "${G_OUTPUT}"
            VERSION "${G_VERSION}" SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
            REPOS "${G_REPOS}")

    foreach(_v VERSION GIT_DESCRIBE GIT_SHA GIT_SHA_SHORT GIT_BRANCH GIT_DIRTY
               GIT_COMMIT_DATE BUILD_NUMBER BUILD_TIMESTAMP VERSION_STRING)
        set(${G_PREFIX}_${_v} "${${G_PREFIX}_${_v}}" PARENT_SCOPE)
    endforeach()
    message(STATUS "Build version [${G_PREFIX}]: ${${G_PREFIX}_VERSION_STRING} "
                   "(sha ${${G_PREFIX}_GIT_SHA_SHORT}, build #${${G_PREFIX}_BUILD_NUMBER})")

    # 2. Re-stamp on every build.
    string(TOLOWER "${G_PREFIX}_git_version_stamp" _stamp_target)
    string(REPLACE ";" "|" _repo_joined "${G_REPOS}")

    add_custom_target(${_stamp_target} ALL
        BYPRODUCTS "${G_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}"
                -D "GV_RUN=1"
                -D "GV_PREFIX=${G_PREFIX}"
                -D "GV_TEMPLATE=${G_TEMPLATE}"
                -D "GV_OUTPUT=${G_OUTPUT}"
                -D "GV_VERSION=${G_VERSION}"
                -D "GV_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                -D "GV_REPOS=${_repo_joined}"
                -P "${_GIT_VERSION_CMAKE}"
        COMMENT "Stamping git build info -> ${G_OUTPUT}"
        VERBATIM)

    if(G_TARGET AND TARGET "${G_TARGET}")
        add_dependencies("${G_TARGET}" ${_stamp_target})
    endif()
endfunction()

# --- worker mode: `cmake -D GV_RUN=1 ... -P GitVersion.cmake` --------------
if(GV_RUN)
    string(REPLACE "|" ";" _repos "${GV_REPOS}")
    _gv_run(PREFIX "${GV_PREFIX}" TEMPLATE "${GV_TEMPLATE}" OUTPUT "${GV_OUTPUT}"
            VERSION "${GV_VERSION}" SOURCE_DIR "${GV_SOURCE_DIR}" REPOS "${_repos}")
endif()

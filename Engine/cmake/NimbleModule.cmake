# ===========================================================================
# NimbleModule.cmake -- the boilerplate every Nimble module target repeats.
#
# Centralizes the STATIC/SHARED choice, the <PREFIX>_SHARED_BUILD /
# <PREFIX>_BUILDING pair, /wd4251, the AUTOMOC/UIC/RCC opt-out, the PUBLIC
# Source/ include directory and the EngineCore link -- the block that
# Modules/Core/CMakeLists.txt and Modules/GDI/CMakeLists.txt currently each
# spell out by hand.
#
# WHY THIS LIVES AT ENGINE LEVEL AND NOT IN Core/cmake/
#
# Core and GDI are vendored git subtrees of standalone repositories. A helper
# inside Core/cmake/ would be invisible to GDI's own standalone configure, and
# putting engine-wide policy inside a library that is supposed to stand alone
# gets it backwards. Core and GDI are deliberately NOT retrofitted onto these
# functions yet: rewriting their CMakeLists to include() an engine-level path
# would break `cmake -S Engine/Modules/Core`, and every such edit becomes a
# merge conflict on the next subtree pull. The tree already has the precedent
# for the right answer -- GitVersion.cmake exists twice, once per subtree --
# so revisit the retrofit by vendoring this file the same way once it is
# stable. Until then these serve new modules only.
#
# WHAT THIS DELIBERATELY DOES NOT DO
#
# It never sets WITH_SIMD, /arch:AVX2 or CORE_NO_GLOBAL_ALIASES. Those arrive
# PUBLIC from EngineCore::EngineCore and must have exactly ONE origin in the
# whole build -- Modules/Core/CMakeLists.txt says why at length, and a module
# that sets them itself reintroduces precisely the silent sizeof() mismatch
# that comment is about. A module that needs to know whether SIMD is on TESTS
# WITH_SIMD; it does not define it.
# ===========================================================================

# ---------------------------------------------------------------------------
# nimble_add_module(<target>
#     PREFIX       <SYMBOL_PREFIX>       # e.g. GDI -> GDI_SHARED_BUILD / GDI_BUILDING
#     SOURCES      <files...>
#     [SHARED]                           # otherwise STATIC
#     [PUBLIC_DEPS  <targets...>]
#     [PRIVATE_DEPS <targets...>])
#
# A normal engine library: linked against at build time, STATIC by default.
# ---------------------------------------------------------------------------
function(nimble_add_module NM_TARGET)
    cmake_parse_arguments(NM "SHARED" "PREFIX" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    if(NOT NM_SOURCES)
        message(FATAL_ERROR "nimble_add_module(${NM_TARGET}) requires SOURCES")
    endif()
    if(NOT NM_PREFIX)
        message(FATAL_ERROR "nimble_add_module(${NM_TARGET}) requires PREFIX -- it names the "
                            "<PREFIX>_SHARED_BUILD / <PREFIX>_BUILDING pair that drives the "
                            "module's export macro")
    endif()

    if(NM_SHARED)
        set(NM_LIB_TYPE SHARED)
    else()
        set(NM_LIB_TYPE STATIC)
    endif()

    add_library(${NM_TARGET} ${NM_LIB_TYPE} ${NM_SOURCES})
    add_library(${NM_TARGET}::${NM_TARGET} ALIAS ${NM_TARGET})

    target_include_directories(${NM_TARGET}
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Source>)
    target_compile_features(${NM_TARGET} PUBLIC cxx_std_17)

    # Qt-freedom, enforced by the build rather than by convention: a parent
    # scope that turns AUTOMOC on as a directory property would otherwise hand
    # it down silently.
    set_target_properties(${NM_TARGET} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

    target_link_libraries(${NM_TARGET} PUBLIC  EngineCore::EngineCore ${NM_PUBLIC_DEPS})
    if(NM_PRIVATE_DEPS)
        target_link_libraries(${NM_TARGET} PRIVATE ${NM_PRIVATE_DEPS})
    endif()

    if(NM_SHARED)
        # The three-state export macro falls out of the link graph: PUBLIC so
        # consumers see dllimport, PRIVATE so this target sees dllexport.
        target_compile_definitions(${NM_TARGET} PUBLIC  ${NM_PREFIX}_SHARED_BUILD)
        target_compile_definitions(${NM_TARGET} PRIVATE ${NM_PREFIX}_BUILDING)
        if(MSVC)
            # std:: members in exported classes. Unavoidable, and harmless as
            # long as one toolchain and one CRT are used across the build --
            # which the hot-reload ABI check enforces at runtime anyway.
            target_compile_options(${NM_TARGET} PUBLIC /wd4251)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# nimble_add_hot_module(<target>
#     SOURCES      <files...>
#     [PUBLIC_DEPS  <targets...>]
#     [PRIVATE_DEPS <targets...>])
#
# A hot-reloadable plugin: loaded by CoreModuleManager at runtime, never linked
# against. Implements EngineCore::IModule and uses NIMBLE_IMPLEMENT_MODULE.
# ---------------------------------------------------------------------------
function(nimble_add_hot_module NM_TARGET)
    cmake_parse_arguments(NM "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    if(NOT NM_SOURCES)
        message(FATAL_ERROR "nimble_add_hot_module(${NM_TARGET}) requires SOURCES")
    endif()

    # A hot module against a STATIC EngineCore gives each DLL its own
    # GLogHandler, its own GFileProvider, its own ModuleManager and -- the one
    # that actually corrupts -- its own copy of ModuleReloadState::Write, so the
    # reload blob is allocated in the module's heap and freed in the host's.
    # None of that produces a diagnostic at build time. Fail here instead.
    if(NOT CORE_SHARED)
        message(FATAL_ERROR
                "nimble_add_hot_module(${NM_TARGET}) requires CORE_SHARED=ON. "
                "Hot reload needs exactly one copy of EngineCore in the process.")
    endif()

    # MODULE, not SHARED: a hot-reload plugin is never linked against, which is
    # precisely what CMake's MODULE library type means, and it keeps the target
    # out of any accidental target_link_libraries.
    add_library(${NM_TARGET} MODULE ${NM_SOURCES})

    # No "lib" prefix, so the filename is identical on every platform and the
    # host's watch path needs no per-platform spelling.
    set_target_properties(${NM_TARGET} PROPERTIES
            PREFIX ""
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF)

    target_include_directories(${NM_TARGET}
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Source>)
    target_compile_features(${NM_TARGET} PUBLIC cxx_std_17)

    target_link_libraries(${NM_TARGET} PUBLIC  EngineCore::EngineCore ${NM_PUBLIC_DEPS})
    if(NM_PRIVATE_DEPS)
        target_link_libraries(${NM_TARGET} PRIVATE ${NM_PRIVATE_DEPS})
    endif()

    if(MSVC)
        # Record only the PDB's FILENAME in the DLL rather than an absolute
        # path, so a shadow copy resolves its symbols from the PDB copied
        # alongside it and the debugger never opens -- and so never locks -- the
        # original. Without this, hot reload merely trades a locked DLL for a
        # locked PDB and the next build fails with LNK1201 instead of LNK1104.
        # See the shadow-copy notes in CoreModuleManager.cpp.
        target_link_options(${NM_TARGET} PRIVATE /PDBALTPATH:%_PDB%)
        target_compile_options(${NM_TARGET} PRIVATE /wd4251)
    elseif(UNIX AND NOT APPLE)
        # A shadow copy sits one directory below the real libraries, so
        # $ORIGIN/.. is where libEngineCore.so is under the flat
        # Binaries/<Platform>_<Config> layout Engine/CMakeLists.txt sets up.
        set_target_properties(${NM_TARGET} PROPERTIES BUILD_RPATH "$ORIGIN/..")
    endif()
endfunction()

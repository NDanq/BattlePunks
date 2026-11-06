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

# ---------------------------------------------------------------------------
# nimble_add_game_launcher(<target>
#     MODULE    <game module target>
#     [SOURCES  <files...>])
#
# A game's executable, named <target> ("Sandbox", "Asteroids"). By default it
# is the engine's own launcher, Engine/Source/Launcher/GameLauncherMain.cpp,
# which runs the game module named by [Game] Module, MODULE if unset -- so a
# game has no main() of its own. SOURCES replaces it, for a game that needs
# one (command line, crash reporting); NIMBLE_GAME_MODULE and NIMBLE_GAME_NAME
# are defined either way.
#
# Also: the module built first (it is loaded, never linked), the executable
# and the module in the engine's runtime directory beside the engine
# libraries -- what hot reload and the Windows loader both need -- and a
# debugger working directory there, where Config/ is staged.
# ---------------------------------------------------------------------------
set(NIMBLE_GAME_LAUNCHER_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../Source/Launcher/GameLauncherMain.cpp"
    CACHE INTERNAL "The engine's game launcher, built by nimble_add_game_launcher")
set(NIMBLE_SERVER_LAUNCHER_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../Source/Launcher/ServerLauncherMain.cpp"
    CACHE INTERNAL "The engine's dedicated server launcher, built by nimble_add_server_launcher")

function(nimble_add_game_launcher NM_TARGET)
    cmake_parse_arguments(NM "" "MODULE" "SOURCES" ${ARGN})

    if(NOT NM_SOURCES)
        get_filename_component(NM_SOURCES "${NIMBLE_GAME_LAUNCHER_SOURCE}" ABSOLUTE)
    endif()

    _nimble_add_launcher(${NM_TARGET} "${NM_MODULE}" "${NM_TARGET}" "${NM_SOURCES}")
    _nimble_record_for_editor(client ${NM_MODULE} ${NM_TARGET})
endfunction()

# ---------------------------------------------------------------------------
# nimble_add_server_launcher(<target>
#     MODULE    <game module target>
#     [SOURCES  <files...>])
#
# A game's dedicated server, named <target> ("LightLabServer"): a CONSOLE
# executable that runs the same game module headless as the session's
# authority (Engine/Source/Launcher/ServerLauncherMain.cpp; NetCore's
# NetSession.h). SOURCES replaces that main(), as for nimble_add_game_launcher.
#
# Needs NetCore. When NIMBLE_WITH_NETCORE is off it SKIPS, with a status
# message, rather than failing -- so a game's CMakeLists declares its server
# unconditionally and a configure without networking still works.
# ---------------------------------------------------------------------------
function(nimble_add_server_launcher NM_TARGET)
    cmake_parse_arguments(NM "" "MODULE" "SOURCES" ${ARGN})

    if(NOT TARGET NetCore)
        message(STATUS "nimble_add_server_launcher(${NM_TARGET}): skipped -- NetCore is not built (NIMBLE_WITH_NETCORE=OFF)")
        return()
    endif()
    if(NOT NM_SOURCES)
        get_filename_component(NM_SOURCES "${NIMBLE_SERVER_LAUNCHER_SOURCE}" ABSOLUTE)
    endif()

    # NIMBLE_GAME_NAME is the game's, not the server's: it prefixes the
    # server's console output and leak report.
    string(REGEX REPLACE "Server$" "" NM_GAME_NAME "${NM_TARGET}")
    _nimble_add_launcher(${NM_TARGET} "${NM_MODULE}" "${NM_GAME_NAME}" "${NM_SOURCES}")
    _nimble_record_for_editor(server ${NM_MODULE} ${NM_TARGET})
endfunction()

# The body both launchers share: an executable from NM_SOURCES that loads
# NM_MODULE, linked to the engine layer and placed beside it.
function(_nimble_add_launcher NM_TARGET NM_MODULE NM_GAME_NAME NM_SOURCES)
    if(NOT NM_MODULE OR NOT TARGET ${NM_MODULE})
        message(FATAL_ERROR "${NM_TARGET}: a launcher requires MODULE <game module target>, declared first")
    endif()
    if(NOT TARGET NimbleEngine)
        message(FATAL_ERROR "${NM_TARGET}: the NimbleEngine target does not exist yet")
    endif()

    add_executable(${NM_TARGET} ${NM_SOURCES})
    target_compile_definitions(${NM_TARGET} PRIVATE
            "NIMBLE_GAME_MODULE=\"${NM_MODULE}\""
            "NIMBLE_GAME_NAME=\"${NM_GAME_NAME}\"")
    # Where -uncooked reads the project's raw files: the game's folder (this
    # is called from its CMakeLists). Never in Shipping, which cannot run
    # uncooked and should not carry a source path. NIMBLE_RUN_UNCOOKED makes
    # running uncooked the build's default (-cooked turns it off).
    target_compile_definitions(${NM_TARGET} PRIVATE
            "$<$<NOT:$<CONFIG:Shipping>>:NIMBLE_PROJECT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\">")
    if(NIMBLE_RUN_UNCOOKED)
        target_compile_definitions(${NM_TARGET} PRIVATE "$<$<NOT:$<CONFIG:Shipping>>:NIMBLE_RUN_UNCOOKED=1>")
    endif()
    add_dependencies(${NM_TARGET} ${NM_MODULE})
    # The enabled plugins' libraries, which the launcher loads by the
    # manifest (NimblePlugins.cmake) whether or not the game links them.
    get_property(NM_PLUGIN_MODULES GLOBAL PROPERTY NIMBLE_PLUGIN_MODULE_TARGETS)
    if(NM_PLUGIN_MODULES)
        add_dependencies(${NM_TARGET} ${NM_PLUGIN_MODULES})
    endif()

    # One link: NimbleEngine brings its include directory, cxx_std_17 and
    # EngineCore. GameWindow and GDI stay private to the engine layer.
    target_link_libraries(${NM_TARGET} PRIVATE NimbleEngine::NimbleEngine)

    # add_executable does not go through nimble_add_module, so the Qt opt-out
    # is restated: a parent scope with AUTOMOC on would hand it down.
    set_target_properties(${NM_TARGET} PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${NM_TARGET}>"
            FOLDER "Games")

    # Beside the engine libraries. Inherited when the game is added below the
    # engine's scope; not when the game is the top-level project and the
    # engine its subdirectory -- so asked of the engine's target either way.
    get_target_property(NM_RT_DIR NimbleEngine RUNTIME_OUTPUT_DIRECTORY)
    if(NM_RT_DIR)
        set_target_properties(${NM_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NM_RT_DIR}")
        set_target_properties(${NM_MODULE} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${NM_RT_DIR}")
    endif()
    get_property(NM_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NM_MULTI_CONFIG)
        foreach(NM_CFG IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${NM_CFG}" NM_CFG_U)
            get_target_property(NM_RT_CFG NimbleEngine RUNTIME_OUTPUT_DIRECTORY_${NM_CFG_U})
            if(NM_RT_CFG)
                set_target_properties(${NM_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${NM_CFG_U} "${NM_RT_CFG}")
                set_target_properties(${NM_MODULE} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${NM_CFG_U} "${NM_RT_CFG}")
            endif()
        endforeach()
    endif()
endfunction()

# The editor builds and runs a game's executables -- Build > Compile Source
# builds the client and server beside the module; Play over the network starts
# them as extra clients and as the dedicated server. It learns their names
# here, as "<kind>:<module>:<target>" (kind: client or server), read by the
# Editor's generated EditorProject.h (Modules/Editor/CMakeLists.txt).
function(_nimble_record_for_editor NM_KIND NM_MODULE NM_TARGET)
    if(TARGET NimbleEditor)
        set_property(TARGET NimbleEditor APPEND PROPERTY NIMBLE_GAME_EXECUTABLES "${NM_KIND}:${NM_MODULE}:${NM_TARGET}")
    endif()
endfunction()

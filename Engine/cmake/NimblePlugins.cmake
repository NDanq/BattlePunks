# ===========================================================================
# NimblePlugins.cmake -- plugins: modules a project drops in, not ones the
# engine names.
#
# A plugin is a folder that can be copied from one project to another:
#
#     <Name>/
#       <Name>.nmplugin        the descriptor (below)
#       CMakeLists.txt         nimble_add_plugin_module() per module
#       Source/<Module>/       Public/<Module>/... and Private/<Module>/...
#       Content/               read as "<Name>:/..." and only so (strict)
#       Config/Default<Name>.ini   defaults, under the project's config
#       Tests/
#
# Plugins are SOURCE ONLY: each project builds them with its own toolchain,
# so the C++ ABI always matches (CoreModule.h, "TOOLCHAIN AND CRT MUST
# MATCH"). There are no prebuilt plugin binaries.
#
# WHERE THEY ARE FOUND, first match wins:
#
#     <Project>/Plugins/*      the project's own; overrides one of the same name below
#     NIMBLE_PLUGIN_DIRS       a shared plugin library, a submodule checkout
#     Engine/Plugins/*         the engine's optional features
#
# WHICH ARE BUILT: [Plugins] in the project's config file, the one the
# editor's Project Settings writes, as [Modules] is (NimbleProject.cmake):
#
#     [Plugins]
#     TimeOfDay=true
#
# A plugin the file does not name is built if its descriptor says
# EnabledByDefault=true. A plugin that is not enabled is not configured at
# all: nothing of it is compiled, staged or loaded.
#
# THE DESCRIPTOR, read here with file(STRINGS) like every other .ini:
#
#     [Plugin]
#     Name=TimeOfDay               must match the file and folder name
#     Version=0.1.0                major.minor.patch
#     EngineVersion=0.1            the oldest engine it builds against
#     Category=Rendering
#     Description=...
#     EnabledByDefault=false
#
#     [Module_TimeOfDay]           one per nimble_add_plugin_module target
#     Type=Runtime                 Runtime | Editor | ClientOnly | ServerOnly
#
#     [Dependencies]
#     Plugins=Other>=1.2.0, Third  each must be found AND enabled
#     OptionalPlugins=Sequencer    ordered before this one when enabled
#
# Module types decide which targets build a module:
#
#     Runtime     every target
#     Editor      Editor only (WITH_EDITOR=1)
#     ClientOnly  Editor and Client -- never a dedicated server
#     ServerOnly  Editor and Server -- never a shipped client
#
# WHAT THE BUILD LEAVES BESIDE THE EXECUTABLE (NimbleEngine/Plugin.h reads it):
#
#     Plugins/Plugins.manifest     the enabled plugins in load order, and
#                                  each one's module libraries
#     Plugins/<Name>/              its descriptor, Config/ and Content/, and
#                                  its shader cache (Shaders/Cache/<Name>.*) --
#                                  or, packaged, Paks/Plugins/<Name>.nmpak
#
# SHADERS: a plugin's Content/Shaders (custom render passes, by file name;
# name them for the plugin, "TimeOfDaySky.fsh" -- the namespace is shared)
# and Content/Materials are cooked by NimbleCook, as a project's are -- the
# build never runs a shader compiler. `NimbleCook shaders -project <game>`
# cooks every plugin the game enables, below <plugin>/Saved/Cooked/Shaders/,
# into a cache named for it; the build stages that, and the host mounts it.
#
# The resolution half (descriptor parsing, enabling, dependency order) is
# kept free of targets so Engine/Tests/Plugins/PluginResolveTest.cmake can
# run it in script mode.
# ===========================================================================

include("${CMAKE_CURRENT_LIST_DIR}/NimblePluginTemplate.cmake")

# Every starter in Engine/Templates/Plugin made into a plugin in the build
# tree, built and its test run, so a starter that no longer compiles is
# caught. Check-only: never in the manifest, never loaded by a game.
option(NIMBLE_CHECK_PLUGIN_TEMPLATES "Build and test every plugin starter (Engine/Templates/Plugin)" OFF)

# ---------------------------------------------------------------------------
# _nimble_ini_read(<File> <OutPrefix>)
#   Every [Section] Key=Value of <File> as <OutPrefix>_<section>_<key> (both
#   lowercased), and <OutPrefix>_SECTIONS: the section names as written.
#   ';' and '#' start a comment line; the last of a repeated key wins.
# ---------------------------------------------------------------------------
macro(_nimble_ini_read NM_INI_FILE NM_INI_OUT)
    set(${NM_INI_OUT}_SECTIONS "")
    set(_NM_INI_SECTION "")
    file(STRINGS "${NM_INI_FILE}" _NM_INI_LINES)
    foreach(_NM_INI_LINE IN LISTS _NM_INI_LINES)
        string(STRIP "${_NM_INI_LINE}" _NM_INI_LINE)
        if(_NM_INI_LINE STREQUAL "" OR _NM_INI_LINE MATCHES "^[;#]")
            continue()
        endif()
        if(_NM_INI_LINE MATCHES "^\\[(.*)\\]$")
            string(STRIP "${CMAKE_MATCH_1}" _NM_INI_SECTION_NAME)
            list(APPEND ${NM_INI_OUT}_SECTIONS "${_NM_INI_SECTION_NAME}")
            string(TOLOWER "${_NM_INI_SECTION_NAME}" _NM_INI_SECTION)
            continue()
        endif()
        if(_NM_INI_LINE MATCHES "^([^=]+)=(.*)$")
            string(STRIP "${CMAKE_MATCH_1}" _NM_INI_KEY)
            string(STRIP "${CMAKE_MATCH_2}" _NM_INI_VALUE)
            string(TOLOWER "${_NM_INI_KEY}" _NM_INI_KEY)
            set(${NM_INI_OUT}_${_NM_INI_SECTION}_${_NM_INI_KEY} "${_NM_INI_VALUE}")
        endif()
    endforeach()
endmacro()

# "a, b>=1.0 ,c" -> "a;b>=1.0;c"
function(_nimble_plugin_split_list NM_TEXT NM_OUT)
    string(REPLACE "," ";" NM_ITEMS "${NM_TEXT}")
    set(NM_RESULT "")
    foreach(NM_ITEM IN LISTS NM_ITEMS)
        string(STRIP "${NM_ITEM}" NM_ITEM)
        if(NOT NM_ITEM STREQUAL "")
            list(APPEND NM_RESULT "${NM_ITEM}")
        endif()
    endforeach()
    set(${NM_OUT} "${NM_RESULT}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# _nimble_plugin_read_descriptor(<File>)
#   Validates <File> and records it as GLOBAL properties
#   NIMBLE_PLUGIN_<Name>_{DIR,VERSION,ENGINE_VERSION,DEFAULT,DEPS,OPTIONAL_DEPS,
#   MODULES,MODULE_<Module>_TYPE}. FATAL_ERROR on anything malformed: a
#   descriptor is code, and a typo in one should stop the configure.
# ---------------------------------------------------------------------------
function(_nimble_plugin_read_descriptor NM_FILE)
    get_filename_component(NM_DIR "${NM_FILE}" DIRECTORY)
    get_filename_component(NM_FOLDER "${NM_DIR}" NAME)
    get_filename_component(NM_STEM "${NM_FILE}" NAME_WE)
    _nimble_ini_read("${NM_FILE}" NM_D)

    set(NM_NAME "${NM_D_plugin_name}")
    if(NM_NAME STREQUAL "")
        message(FATAL_ERROR "${NM_FILE}: [Plugin] Name is required")
    endif()
    # Two characters at least: "C:/..." is a drive, never a plugin's content.
    if(NOT NM_NAME MATCHES "^[A-Za-z][A-Za-z0-9]+$")
        message(FATAL_ERROR "${NM_FILE}: plugin name '${NM_NAME}' must be a letter then letters or digits, "
                            "two characters at least -- it is the '${NM_NAME}:/' prefix of the plugin's content")
    endif()
    if(NOT NM_NAME STREQUAL NM_STEM OR NOT NM_NAME STREQUAL NM_FOLDER)
        message(FATAL_ERROR "${NM_FILE}: plugin '${NM_NAME}' must live in ${NM_NAME}/${NM_NAME}.nmplugin")
    endif()

    set(NM_VERSION "${NM_D_plugin_version}")
    if(NOT NM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "${NM_FILE}: [Plugin] Version must be major.minor.patch (got '${NM_VERSION}')")
    endif()
    set(NM_ENGINE_VERSION "${NM_D_plugin_engineversion}")
    if(NOT NM_ENGINE_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
        message(FATAL_ERROR "${NM_FILE}: [Plugin] EngineVersion is required, e.g. 0.1 (got '${NM_ENGINE_VERSION}')")
    endif()

    string(TOLOWER "${NM_D_plugin_enabledbydefault}" NM_DEFAULT)
    if(NM_DEFAULT MATCHES "^(true|1|yes|on)$")
        set(NM_DEFAULT ON)
    elseif(NM_DEFAULT MATCHES "^(false|0|no|off|)$")
        set(NM_DEFAULT OFF)
    else()
        message(FATAL_ERROR "${NM_FILE}: [Plugin] EnabledByDefault must be true or false")
    endif()

    set(NM_MODULES "")
    foreach(NM_SECTION IN LISTS NM_D_SECTIONS)
        if(NM_SECTION MATCHES "^Module_(.+)$")
            set(NM_MODULE "${CMAKE_MATCH_1}")
            string(TOLOWER "${NM_SECTION}" NM_SECTION_LOWER)
            set(NM_TYPE "${NM_D_${NM_SECTION_LOWER}_type}")
            if(NOT NM_TYPE MATCHES "^(Runtime|Editor|ClientOnly|ServerOnly)$")
                message(FATAL_ERROR "${NM_FILE}: [${NM_SECTION}] Type must be Runtime, Editor, ClientOnly or ServerOnly "
                                    "(got '${NM_TYPE}')")
            endif()
            list(APPEND NM_MODULES "${NM_MODULE}")
            set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_MODULE_${NM_MODULE}_TYPE "${NM_TYPE}")
        endif()
    endforeach()

    _nimble_plugin_split_list("${NM_D_dependencies_plugins}" NM_DEPS)
    _nimble_plugin_split_list("${NM_D_dependencies_optionalplugins}" NM_OPTIONAL)
    foreach(NM_DEP IN LISTS NM_DEPS NM_OPTIONAL)
        if(NOT NM_DEP MATCHES "^[A-Za-z][A-Za-z0-9]+(>=[0-9]+\\.[0-9]+\\.[0-9]+)?$")
            message(FATAL_ERROR "${NM_FILE}: dependency '${NM_DEP}' must be Name or Name>=major.minor.patch")
        endif()
    endforeach()

    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DIR            "${NM_DIR}")
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_VERSION        "${NM_VERSION}")
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_ENGINE_VERSION "${NM_ENGINE_VERSION}")
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DEFAULT        "${NM_DEFAULT}")
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DEPS           "${NM_DEPS}")
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_OPTIONAL_DEPS  "${NM_OPTIONAL}")
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_MODULES        "${NM_MODULES}")
endfunction()

# ---------------------------------------------------------------------------
# nimble_plugins_resolve(
#     DIRS           <search roots, first wins...>
#     CONFIG         <project .ini, may be empty>
#     ENGINE_VERSION <x.y>
#     [ENABLE        <names...>]      enabled whatever the config says
#     OUT_ORDER      <var>)
#
#   Finds every <root>/<Name>/<Name>.nmplugin, decides which are enabled,
#   checks their dependencies and returns the enabled ones in load order --
#   each after everything it depends on. Also sets the GLOBAL property
#   NIMBLE_PLUGINS_FOUND (every plugin found, enabled or not).
# ---------------------------------------------------------------------------
function(nimble_plugins_resolve)
    cmake_parse_arguments(NM "" "CONFIG;ENGINE_VERSION;OUT_ORDER" "DIRS;ENABLE" ${ARGN})

    # Discovery: the first root that has a name keeps it.
    set(NM_FOUND "")
    foreach(NM_ROOT IN LISTS NM_DIRS)
        if(NOT IS_DIRECTORY "${NM_ROOT}")
            continue()
        endif()
        file(GLOB NM_DESCRIPTORS LIST_DIRECTORIES false "${NM_ROOT}/*/*.nmplugin")
        list(SORT NM_DESCRIPTORS)
        foreach(NM_FILE IN LISTS NM_DESCRIPTORS)
            get_filename_component(NM_NAME "${NM_FILE}" NAME_WE)
            if(NM_NAME IN_LIST NM_FOUND)
                get_property(NM_KEPT GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DIR)
                message(STATUS "NimbleEngine: plugin ${NM_NAME} at ${NM_KEPT} overrides the one in ${NM_ROOT}")
                continue()
            endif()
            _nimble_plugin_read_descriptor("${NM_FILE}")
            list(APPEND NM_FOUND "${NM_NAME}")
        endforeach()
    endforeach()
    set_property(GLOBAL PROPERTY NIMBLE_PLUGINS_FOUND "${NM_FOUND}")

    # Enabling: the project's [Plugins] line, else the descriptor's default.
    if(NM_CONFIG AND EXISTS "${NM_CONFIG}")
        _nimble_ini_read("${NM_CONFIG}" NM_P)
    endif()
    set(NM_ENABLED "")
    foreach(NM_NAME IN LISTS NM_FOUND)
        string(TOLOWER "${NM_NAME}" NM_KEY)
        get_property(NM_ON GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DEFAULT)
        if(DEFINED NM_P_plugins_${NM_KEY})
            string(TOLOWER "${NM_P_plugins_${NM_KEY}}" NM_VALUE)
            if(NM_VALUE MATCHES "^(true|1|yes|on)$")
                set(NM_ON ON)
            elseif(NM_VALUE MATCHES "^(false|0|no|off)$")
                set(NM_ON OFF)
            else()
                message(WARNING "${NM_CONFIG}: [Plugins] ${NM_NAME}=${NM_VALUE} is not true or false; ignored")
            endif()
        endif()
        if(NM_ON OR NM_NAME IN_LIST NM_ENABLE)
            list(APPEND NM_ENABLED "${NM_NAME}")
        endif()
    endforeach()

    # A [Plugins] line naming nothing found is a typo or a missing checkout.
    if(NM_CONFIG AND EXISTS "${NM_CONFIG}")
        file(STRINGS "${NM_CONFIG}" NM_LINES)
        set(NM_IN_PLUGINS OFF)
        foreach(NM_LINE IN LISTS NM_LINES)
            string(STRIP "${NM_LINE}" NM_LINE)
            if(NM_LINE MATCHES "^\\[(.*)\\]$")
                string(TOLOWER "${CMAKE_MATCH_1}" NM_SECTION)
                if(NM_SECTION STREQUAL "plugins")
                    set(NM_IN_PLUGINS ON)
                else()
                    set(NM_IN_PLUGINS OFF)
                endif()
            elseif(NM_IN_PLUGINS AND NM_LINE MATCHES "^([^=;#]+)=")
                string(STRIP "${CMAKE_MATCH_1}" NM_WANT)
                set(NM_KNOWN OFF)
                foreach(NM_NAME IN LISTS NM_FOUND)
                    string(TOLOWER "${NM_NAME}" NM_A)
                    string(TOLOWER "${NM_WANT}" NM_B)
                    if(NM_A STREQUAL NM_B)
                        set(NM_KNOWN ON)
                    endif()
                endforeach()
                if(NOT NM_KNOWN)
                    message(WARNING "${NM_CONFIG}: [Plugins] names '${NM_WANT}', which is in none of: ${NM_DIRS}")
                endif()
            endif()
        endforeach()
    endif()

    # Checks: engine version, then each required dependency found, enabled
    # and new enough.
    foreach(NM_NAME IN LISTS NM_ENABLED)
        get_property(NM_NEEDS GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_ENGINE_VERSION)
        if(NM_ENGINE_VERSION AND NM_ENGINE_VERSION VERSION_LESS NM_NEEDS)
            message(FATAL_ERROR "Plugin ${NM_NAME} needs engine ${NM_NEEDS} or newer; this engine is ${NM_ENGINE_VERSION}")
        endif()
        get_property(NM_DEPS GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DEPS)
        foreach(NM_DEP IN LISTS NM_DEPS)
            string(REGEX REPLACE ">=.*$" "" NM_DEP_NAME "${NM_DEP}")
            if(NOT NM_DEP_NAME IN_LIST NM_FOUND)
                message(FATAL_ERROR "Plugin ${NM_NAME} depends on ${NM_DEP_NAME}, which is in none of: ${NM_DIRS}")
            endif()
            if(NOT NM_DEP_NAME IN_LIST NM_ENABLED)
                message(FATAL_ERROR "Plugin ${NM_NAME} depends on ${NM_DEP_NAME}, which is not enabled: "
                                    "add ${NM_DEP_NAME}=true to [Plugins] in ${NM_CONFIG}")
            endif()
            if(NM_DEP MATCHES ">=(.+)$")
                set(NM_MIN "${CMAKE_MATCH_1}")
                get_property(NM_HAVE GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_DEP_NAME}_VERSION)
                if(NM_HAVE VERSION_LESS NM_MIN)
                    message(FATAL_ERROR "Plugin ${NM_NAME} needs ${NM_DEP_NAME} ${NM_MIN} or newer; found ${NM_HAVE}")
                endif()
            endif()
        endforeach()
    endforeach()

    # Order: depth-first, each plugin after what it needs. A cycle is fatal.
    set(NM_ORDER "")
    foreach(NM_NAME IN LISTS NM_ENABLED)
        _nimble_plugin_visit("${NM_NAME}")
    endforeach()
    set(${NM_OUT_ORDER} "${NM_ORDER}" PARENT_SCOPE)
endfunction()

# The depth-first step of nimble_plugins_resolve: a macro, so it appends to
# the caller's NM_ORDER. An explicit stack (NM_STACK, the current path)
# rather than recursion, which a macro cannot do with its own variables.
macro(_nimble_plugin_visit NM_START)
    set(NM_STACK "${NM_START}")
    while(NM_STACK)
        list(GET NM_STACK -1 NM_TOP)
        if(NM_TOP IN_LIST NM_ORDER)
            list(REMOVE_AT NM_STACK -1)
            continue()
        endif()
        get_property(NM_V_DEPS GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_TOP}_DEPS)
        get_property(NM_V_OPT  GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_TOP}_OPTIONAL_DEPS)
        set(NM_PENDING "")
        foreach(NM_V_DEP IN LISTS NM_V_DEPS NM_V_OPT)
            string(REGEX REPLACE ">=.*$" "" NM_V_DEP "${NM_V_DEP}")
            if(NM_V_DEP IN_LIST NM_ENABLED AND NOT NM_V_DEP IN_LIST NM_ORDER)
                if(NM_V_DEP IN_LIST NM_STACK)
                    string(REPLACE ";" " -> " NM_CYCLE "${NM_STACK};${NM_V_DEP}")
                    message(FATAL_ERROR "Plugin dependency cycle: ${NM_CYCLE}")
                endif()
                list(APPEND NM_PENDING "${NM_V_DEP}")
            endif()
        endforeach()
        if(NM_PENDING)
            list(GET NM_PENDING 0 NM_NEXT)
            list(APPEND NM_STACK "${NM_NEXT}")
        else()
            list(APPEND NM_ORDER "${NM_TOP}")
            list(REMOVE_AT NM_STACK -1)
        endif()
    endwhile()
endmacro()

# ---------------------------------------------------------------------------
# nimble_add_plugin_module(<Module>
#     SOURCES      <files...>
#     [PUBLIC_DEPS  <targets...>]
#     [PRIVATE_DEPS <targets...>])
#
#   From a plugin's CMakeLists.txt: the module its descriptor declares as
#   [Module_<Module>], built from Source/<Module>/ as a SHARED library named
#   <Module> on every platform (no "lib"), beside the engine's. Linked to
#   NimbleEngine; includes read "Public/<Module>/...". The <MODULE>_API
#   export header is generated (Public/<Module>/<Module>API.h), so a plugin
#   cannot forget it -- the MSVC LNK2019 the msvc-compat rules warn of.
#
#   Not created when the descriptor's Type leaves this target out (an Editor
#   module in a Client build); test for it with if(TARGET <Module>).
#
#   A game module that uses a plugin links it as any library, under
#   if(TARGET <Module>).
# ---------------------------------------------------------------------------
function(nimble_add_plugin_module NM_TARGET)
    cmake_parse_arguments(NM "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    if(NOT NIMBLE_PLUGIN_NAME)
        message(FATAL_ERROR "nimble_add_plugin_module(${NM_TARGET}) belongs in a plugin's CMakeLists.txt")
    endif()
    get_property(NM_TYPE GLOBAL PROPERTY NIMBLE_PLUGIN_${NIMBLE_PLUGIN_NAME}_MODULE_${NM_TARGET}_TYPE)
    if(NOT NM_TYPE)
        message(FATAL_ERROR "${NIMBLE_PLUGIN_NAME}: module ${NM_TARGET} is not declared in ${NIMBLE_PLUGIN_NAME}.nmplugin "
                            "-- add a [Module_${NM_TARGET}] section with its Type")
    endif()

    set(NM_SKIP "")
    if(NM_TYPE STREQUAL "Editor" AND NOT NIMBLE_WITH_EDITOR)
        set(NM_SKIP "an Editor module")
    elseif(NM_TYPE STREQUAL "ClientOnly" AND NIMBLE_TARGET STREQUAL "Server")
        set(NM_SKIP "a ClientOnly module")
    elseif(NM_TYPE STREQUAL "ServerOnly" AND NIMBLE_TARGET STREQUAL "Client")
        set(NM_SKIP "a ServerOnly module")
    endif()
    if(NM_SKIP)
        message(STATUS "${NIMBLE_PLUGIN_NAME}: ${NM_TARGET} skipped -- ${NM_SKIP} in a ${NIMBLE_TARGET} build")
        return()
    endif()

    if(TARGET ${NM_TARGET})
        message(FATAL_ERROR "${NIMBLE_PLUGIN_NAME}: a target named ${NM_TARGET} already exists (an engine module, a game "
                            "or another plugin's) -- rename the module, its [Module_] section and its Source/ folder")
    endif()

    string(TOUPPER "${NM_TARGET}" NM_PREFIX)
    nimble_add_module(${NM_TARGET} SHARED
            PREFIX       ${NM_PREFIX}
            SOURCES      ${NM_SOURCES}
            PUBLIC_DEPS  NimbleEngine::NimbleEngine ${NM_PUBLIC_DEPS}
            PRIVATE_DEPS ${NM_PRIVATE_DEPS})

    # The export header, the three states every *API.h spells by hand.
    set(NM_GEN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/Generated/${NM_TARGET}")
    set(NM_API_TEXT "#ifndef ${NM_PREFIX}_${NM_PREFIX}API_H
#define ${NM_PREFIX}_${NM_PREFIX}API_H

/* Generated by nimble_add_plugin_module (Engine/cmake/NimblePlugins.cmake).
 * The three states CoreAPI.h documents:
 *   ${NM_PREFIX}_SHARED_BUILD + _BUILDING -> dllexport (building the module)
 *   ${NM_PREFIX}_SHARED_BUILD alone       -> dllimport (linking it)
 *   neither                          -> empty */
#if defined(_WIN32)
    #define ${NM_PREFIX}_API_EXPORT __declspec(dllexport)
    #define ${NM_PREFIX}_API_IMPORT __declspec(dllimport)
#else
    #define ${NM_PREFIX}_API_EXPORT __attribute__((visibility(\"default\")))
    #define ${NM_PREFIX}_API_IMPORT __attribute__((visibility(\"default\")))
#endif

#ifdef ${NM_PREFIX}_SHARED_BUILD
    #ifdef ${NM_PREFIX}_BUILDING
        #define ${NM_PREFIX}_API ${NM_PREFIX}_API_EXPORT
    #else
        #define ${NM_PREFIX}_API ${NM_PREFIX}_API_IMPORT
    #endif
#else
    #define ${NM_PREFIX}_API
#endif

#endif
")
    set(NM_API_FILE "${NM_GEN_ROOT}/Public/${NM_TARGET}/${NM_TARGET}API.h")
    set(NM_API_OLD "")
    if(EXISTS "${NM_API_FILE}")
        file(READ "${NM_API_FILE}" NM_API_OLD)
    endif()
    if(NOT NM_API_OLD STREQUAL NM_API_TEXT)
        file(WRITE "${NM_API_FILE}" "${NM_API_TEXT}")   # only when it changes: everything includes it
    endif()

    target_include_directories(${NM_TARGET} PUBLIC
            $<BUILD_INTERFACE:${NIMBLE_PLUGIN_DIR}/Source/${NM_TARGET}>
            $<BUILD_INTERFACE:${NM_GEN_ROOT}>)
    target_compile_definitions(${NM_TARGET} PRIVATE "NIMBLE_PLUGIN_NAME=\"${NIMBLE_PLUGIN_NAME}\"")

    # One file name everywhere, so the manifest needs no per-platform spelling.
    set_target_properties(${NM_TARGET} PROPERTIES
            PREFIX ""
            FOLDER "Plugins/${NIMBLE_PLUGIN_NAME}")
    if(MSVC)
        # The editor loads a shadow copy (Plugin.h, SetShadowCopyDir) so a
        # build can replace the library while it runs; the PDB's file name
        # only, so the copy's symbols come from the PDB copied beside it and
        # the original is never locked either (as nimble_add_hot_module).
        target_link_options(${NM_TARGET} PRIVATE /PDBALTPATH:%_PDB%)
    endif()

    set_property(GLOBAL APPEND PROPERTY NIMBLE_PLUGIN_${NIMBLE_PLUGIN_NAME}_BUILT_MODULES ${NM_TARGET})
    # Loaded, not necessarily linked: every launcher (_nimble_add_launcher)
    # and the editor build it first. A template check's never is.
    get_property(NM_CHECK_ONLY GLOBAL PROPERTY NIMBLE_PLUGIN_${NIMBLE_PLUGIN_NAME}_CHECK_ONLY)
    if(NOT NM_CHECK_ONLY)
        set_property(GLOBAL APPEND PROPERTY NIMBLE_PLUGIN_MODULE_TARGETS ${NM_TARGET})
        if(TARGET NimbleEditor)
            add_dependencies(NimbleEditor ${NM_TARGET})
        endif()
    endif()
    if(TARGET NimblePlugin${NIMBLE_PLUGIN_NAME}Stage)
        add_dependencies(${NM_TARGET} NimblePlugin${NIMBLE_PLUGIN_NAME}Stage)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# nimble_add_plugin_test(<Target> SOURCES <files...> [LINK <targets...>])
#
#   A plugin's test executable, registered with ctest under the "plugin"
#   label and run from the runtime directory, where its modules and staged
#   files are. Nothing when the engine's tests are off (Shipping).
# ---------------------------------------------------------------------------
function(nimble_add_plugin_test NM_TARGET)
    cmake_parse_arguments(NM "" "" "SOURCES;LINK" ${ARGN})
    if(NOT NIMBLE_PLUGIN_TESTS)
        return()
    endif()
    add_executable(${NM_TARGET} ${NM_SOURCES})
    target_link_libraries(${NM_TARGET} PRIVATE ${NM_LINK})
    set_target_properties(${NM_TARGET} PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            FOLDER "Plugins/${NIMBLE_PLUGIN_NAME}")
    add_test(NAME Plugin.${NIMBLE_PLUGIN_NAME}.${NM_TARGET} COMMAND ${NM_TARGET}
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:${NM_TARGET}>")
    get_property(NM_CHECK_ONLY GLOBAL PROPERTY NIMBLE_PLUGIN_${NIMBLE_PLUGIN_NAME}_CHECK_ONLY)
    if(NM_CHECK_ONLY)
        set_tests_properties(Plugin.${NIMBLE_PLUGIN_NAME}.${NM_TARGET} PROPERTIES TIMEOUT 120 LABELS "plugin;template")
    else()
        set_tests_properties(Plugin.${NIMBLE_PLUGIN_NAME}.${NM_TARGET} PROPERTIES TIMEOUT 120 LABELS "plugin;smoke")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# _nimble_stage_plugin(<Name> <Dir> <RuntimeDir>)
#   The descriptor, Config/ and Content/ beside the executable under
#   Plugins/<Name>/ -- or, packaging, into Paks/Plugins/<Name>.nmpak
#   (nimble_package_game). Source/ and Tests/ are never staged.
# ---------------------------------------------------------------------------
function(_nimble_stage_plugin NM_NAME NM_DIR NM_RT_DIR)
    file(GLOB_RECURSE NM_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
         "${NM_DIR}/Config/*" "${NM_DIR}/Content/*")
    # Shader sources are cooked into its cache (below), not read as content.
    list(FILTER NM_FILES EXCLUDE REGEX "^${NM_DIR}/Content/(Shaders|Materials)/")
    list(APPEND NM_FILES "${NM_DIR}/${NM_NAME}.nmplugin")
    _nimble_stage_plugin_shaders(${NM_NAME} "${NM_DIR}" "${NM_RT_DIR}")

    if(NIMBLE_PACKAGE_CONTENT)
        nimble_pack_add_files(Plugin_${NM_NAME} "${NM_DIR}" ${NM_FILES})
        set_property(GLOBAL APPEND PROPERTY NIMBLE_PACK_PLUGINS ${NM_NAME})
        return()
    endif()
    if(NOT NM_RT_DIR)
        return()   # multi-config: staged by nobody, read from the source folder (uncooked)
    endif()

    set(NM_OUTPUTS "")
    foreach(NM_FILE IN LISTS NM_FILES)
        file(RELATIVE_PATH NM_REL "${NM_DIR}" "${NM_FILE}")
        get_filename_component(NM_REL_DIR "${NM_REL}" DIRECTORY)
        set(NM_DST "${NM_RT_DIR}/Plugins/${NM_NAME}/${NM_REL}")
        add_custom_command(
                OUTPUT  "${NM_DST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${NM_RT_DIR}/Plugins/${NM_NAME}/${NM_REL_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NM_FILE}" "${NM_DST}"
                DEPENDS "${NM_FILE}"
                COMMENT "${NM_NAME}: staging Plugins/${NM_NAME}/${NM_REL}"
                VERBATIM)
        list(APPEND NM_OUTPUTS "${NM_DST}")
    endforeach()
    add_custom_target(NimblePlugin${NM_NAME}Stage ALL DEPENDS ${NM_OUTPUTS})
    set_target_properties(NimblePlugin${NM_NAME}Stage PROPERTIES FOLDER "Plugins/${NM_NAME}")
    if(TARGET NimblePlugin${NM_NAME}StageShaders)
        add_dependencies(NimblePlugin${NM_NAME}Stage NimblePlugin${NM_NAME}StageShaders)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# _nimble_stage_plugin_shaders(<Name> <Dir> <RuntimeDir>)
#   What NimbleCook cooked from <Dir>/Content/Shaders and Content/Materials
#   (<Dir>/Saved/Cooked/Shaders/Runtime/, and Shipping/ over it for a packaged
#   Shipping build) into Plugins/<Name>/ beside the executable, or its pack.
#   Not cooked yet: a warning, as for a game's materials (NimbleMaterials.cmake).
# ---------------------------------------------------------------------------
function(_nimble_stage_plugin_shaders NM_NAME NM_DIR NM_RT_DIR)
    if(NOT IS_DIRECTORY "${NM_DIR}/Content/Shaders" AND NOT IS_DIRECTORY "${NM_DIR}/Content/Materials")
        return()
    endif()
    set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_SHADERS ON)

    if(NIMBLE_PACKAGE_CONTENT)
        nimble_pack_stage_dir(Plugin_${NM_NAME} NM_DEST)
    elseif(NM_RT_DIR)
        set(NM_DEST "${NM_RT_DIR}/Plugins/${NM_NAME}")
    else()
        return()
    endif()
    set(NM_VARIANTS Runtime "${NM_DEST}")
    if(NIMBLE_PACKAGE_CONTENT AND CMAKE_BUILD_TYPE STREQUAL "Shipping")
        list(APPEND NM_VARIANTS Shipping "${NM_DEST}")
    endif()
    nimble_cooked_shader_commands(NM_COMMANDS NM_DEPENDS "${NM_NAME}" "${NM_DIR}/Saved/Cooked/Shaders" ${NM_VARIANTS})
    if(NOT NM_COMMANDS)
        return()
    endif()

    set(NM_STAMP "${CMAKE_BINARY_DIR}/Plugins/${NM_NAME}_Shaders.stamp")
    add_custom_command(
            OUTPUT  "${NM_STAMP}"
            ${NM_COMMANDS}
            COMMAND ${CMAKE_COMMAND} -E touch "${NM_STAMP}"
            DEPENDS ${NM_DEPENDS}
            COMMENT "${NM_NAME}: staging the shaders NimbleCook cooked"
            VERBATIM)
    add_custom_target(NimblePlugin${NM_NAME}StageShaders ALL DEPENDS "${NM_STAMP}")
    set_target_properties(NimblePlugin${NM_NAME}StageShaders PROPERTIES FOLDER "Plugins/${NM_NAME}")
    if(NIMBLE_PACKAGE_CONTENT)
        nimble_pack_add_dir(Plugin_${NM_NAME} "Shaders" "${NM_DEST}/Shaders" "${NM_STAMP}" NimblePlugin${NM_NAME}StageShaders)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# nimble_add_plugins()
#
#   Engine/CMakeLists.txt, once, below NimbleEngine and the editor: resolves
#   the plugins, adds each enabled one's CMakeLists.txt in load order and
#   writes Plugins/Plugins.manifest beside the executable.
# ---------------------------------------------------------------------------
function(nimble_add_plugins)
    set(NM_DIRS "")
    if(NIMBLE_PROJECT_CONFIG)
        get_filename_component(NM_CONFIG_DIR "${NIMBLE_PROJECT_CONFIG}" DIRECTORY)
        get_filename_component(NM_PROJECT_DIR "${NM_CONFIG_DIR}" DIRECTORY)
        list(APPEND NM_DIRS "${NM_PROJECT_DIR}/Plugins")
    endif()
    list(APPEND NM_DIRS ${NIMBLE_PLUGIN_DIRS} "${PROJECT_SOURCE_DIR}/Plugins")
    if(NIMBLE_PROJECT_CONFIG AND EXISTS "${NIMBLE_PROJECT_CONFIG}")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NIMBLE_PROJECT_CONFIG}")
    endif()
    # A plugin dropped into a search root re-configures by itself.
    foreach(NM_ROOT IN LISTS NM_DIRS)
        if(IS_DIRECTORY "${NM_ROOT}")
            file(GLOB NM_SEEN CONFIGURE_DEPENDS "${NM_ROOT}/*/*.nmplugin")
        endif()
    endforeach()

    set(NM_SEARCH_DIRS ${NM_DIRS})   # what the manifest records: never the checks' folder
    set(NM_CHECKS "")
    if(NIMBLE_CHECK_PLUGIN_TEMPLATES)
        set(NM_CHECK_ROOT "${CMAKE_BINARY_DIR}/PluginTemplateCheck")
        set(NM_TEMPLATE_DIR "${PROJECT_SOURCE_DIR}/Templates/Plugin")
        file(GLOB_RECURSE NM_TEMPLATE_FILES CONFIGURE_DEPENDS "${NM_TEMPLATE_DIR}/*")
        file(GLOB NM_STARTERS LIST_DIRECTORIES true "${NM_TEMPLATE_DIR}/*")
        foreach(NM_STARTER IN LISTS NM_STARTERS)
            get_filename_component(NM_STARTER "${NM_STARTER}" NAME)
            if(NOT IS_DIRECTORY "${NM_TEMPLATE_DIR}/${NM_STARTER}" OR NM_STARTER STREQUAL "Common")
                continue()
            endif()
            # Every option on: the most of each starter compiled.
            nimble_create_plugin(TEMPLATE_DIR "${NM_TEMPLATE_DIR}" STARTER ${NM_STARTER} DEST "${NM_CHECK_ROOT}"
                                 NAME Check${NM_STARTER} ENGINE_VERSION "${PROJECT_VERSION}"
                                 DESCRIPTION "The ${NM_STARTER} starter, checked" TEST EDITOR CONTENT UPDATE)
            list(APPEND NM_CHECKS Check${NM_STARTER})
            set_property(GLOBAL PROPERTY NIMBLE_PLUGIN_Check${NM_STARTER}_CHECK_ONLY ON)
        endforeach()
        list(APPEND NM_DIRS "${NM_CHECK_ROOT}")
        message(STATUS "NimbleEngine: checking the plugin starters: ${NM_CHECKS} (NIMBLE_CHECK_PLUGIN_TEMPLATES)")
    endif()

    nimble_plugins_resolve(
            DIRS           ${NM_DIRS}
            CONFIG         "${NIMBLE_PROJECT_CONFIG}"
            ENGINE_VERSION "${PROJECT_VERSION}"
            ENABLE         ${NM_CHECKS}
            OUT_ORDER      NM_ORDER)
    set_property(GLOBAL PROPERTY NIMBLE_PLUGINS_ENABLED "${NM_ORDER}")

    get_target_property(NM_RT_DIR NimbleEngine RUNTIME_OUTPUT_DIRECTORY)
    get_property(NM_MULTI GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NM_MULTI)
        set(NM_RT_DIR "")
    endif()

    set(NIMBLE_PLUGIN_TESTS ${NM_TESTS_ENABLED})
    set(NM_MANIFEST "; Generated by Engine/cmake/NimblePlugins.cmake -- the enabled plugins, in load order.\n")
    # Where they were looked for, for the tools that list every plugin (the
    # editor's Project Settings > Plugins). Source paths: never in Shipping.
    string(REPLACE ";" "|" NM_ROOTS "${NM_SEARCH_DIRS}")
    string(APPEND NM_MANIFEST "$<$<NOT:$<CONFIG:Shipping>>:\n[Search]\nRoots=${NM_ROOTS}\nEngineVersion=${PROJECT_VERSION}\n>")
    foreach(NM_NAME IN LISTS NM_ORDER)
        get_property(NM_DIR GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DIR)
        get_property(NM_VERSION GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_VERSION)
        get_property(NM_CHECK_ONLY GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_CHECK_ONLY)
        if(NOT NM_CHECK_ONLY)
            message(STATUS "NimbleEngine: plugin ${NM_NAME} ${NM_VERSION} (${NM_DIR})")
            _nimble_stage_plugin(${NM_NAME} "${NM_DIR}" "${NM_RT_DIR}")
        endif()
        set(NIMBLE_PLUGIN_NAME "${NM_NAME}")
        set(NIMBLE_PLUGIN_DIR  "${NM_DIR}")
        if(EXISTS "${NM_DIR}/CMakeLists.txt")
            add_subdirectory("${NM_DIR}" "${CMAKE_BINARY_DIR}/Plugins/${NM_NAME}")
        endif()

        if(NM_CHECK_ONLY)
            continue()   # built and tested, never loaded
        endif()
        get_property(NM_BUILT GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_BUILT_MODULES)
        set(NM_FILES "")
        foreach(NM_MODULE IN LISTS NM_BUILT)
            list(APPEND NM_FILES "$<TARGET_FILE_NAME:${NM_MODULE}>")
        endforeach()
        string(REPLACE ";" "," NM_FILES "${NM_FILES}")
        get_property(NM_SHADERS GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_SHADERS)
        if(NM_SHADERS)
            set(NM_SHADERS "true")
        else()
            set(NM_SHADERS "false")
        endif()
        string(APPEND NM_MANIFEST "\n[Plugin_${NM_NAME}]\nVersion=${NM_VERSION}\nModules=${NM_FILES}\nShaders=${NM_SHADERS}\n")
        # Where -uncooked and the editor read its raw files. Never in Shipping.
        string(APPEND NM_MANIFEST "$<$<NOT:$<CONFIG:Shipping>>:Source=${NM_DIR}\n>")
    endforeach()

    # Always written, empty or not: an absent manifest and "no plugins" read the same.
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:NimbleEngine>/Plugins/Plugins.manifest" CONTENT "${NM_MANIFEST}")
endfunction()

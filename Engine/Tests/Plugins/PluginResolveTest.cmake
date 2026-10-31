# ===========================================================================
# PluginResolveTest.cmake -- nimble_plugins_resolve (cmake/NimblePlugins.cmake)
# in script mode: discovery, overrides, enabling, dependency order, and every
# refusal. ctest runs it as Plugins.Resolve:
#
#     cmake -DNIMBLE_CMAKE_DIR=<Engine/cmake> -DWORK_DIR=<scratch> -P PluginResolveTest.cmake
#
# Each case writes its plugin folders under WORK_DIR/<case> and runs itself
# again with -DCASE=<case>, so a FATAL_ERROR can be checked for rather than
# ending the test.
# ===========================================================================
cmake_minimum_required(VERSION 3.16)

if(NOT NIMBLE_CMAKE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "PluginResolveTest needs -DNIMBLE_CMAKE_DIR and -DWORK_DIR")
endif()
include("${NIMBLE_CMAKE_DIR}/NimblePlugins.cmake")

# A child run: resolve one case's folders and print the order.
if(CASE)
    set(NM_ROOT "${WORK_DIR}/${CASE}")
    nimble_plugins_resolve(
            DIRS           "${NM_ROOT}/Project" "${NM_ROOT}/Engine"
            CONFIG         "${NM_ROOT}/Config.ini"
            ENGINE_VERSION "0.1"
            OUT_ORDER      NM_ORDER)
    set(NM_DIRS "")
    foreach(NM_NAME IN LISTS NM_ORDER)
        get_property(NM_DIR GLOBAL PROPERTY NIMBLE_PLUGIN_${NM_NAME}_DIR)
        file(RELATIVE_PATH NM_DIR "${NM_ROOT}" "${NM_DIR}")
        list(APPEND NM_DIRS "${NM_DIR}")
    endforeach()
    message(STATUS "ORDER=${NM_ORDER}")
    message(STATUS "DIRS=${NM_DIRS}")
    return()
endif()

set(GFailures 0)

# plugin(<case> <root: Project|Engine> <Name> <Version> <descriptor lines...>)
function(plugin NM_CASE NM_ROOT NM_NAME NM_VERSION)
    string(REPLACE ";" "\n" NM_EXTRA "${ARGN}")
    file(WRITE "${WORK_DIR}/${NM_CASE}/${NM_ROOT}/${NM_NAME}/${NM_NAME}.nmplugin"
         "[Plugin]\nName=${NM_NAME}\nVersion=${NM_VERSION}\nEngineVersion=0.1\n${NM_EXTRA}\n")
endfunction()

function(config NM_CASE)
    string(REPLACE ";" "\n" NM_TEXT "${ARGN}")
    file(WRITE "${WORK_DIR}/${NM_CASE}/Config.ini" "[Plugins]\n${NM_TEXT}\n")
endfunction()

# expect(<case> ORDER <a;b> | DIRS <...> | ERROR <text>)
function(expect NM_CASE NM_WHAT NM_WANT)
    execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DNIMBLE_CMAKE_DIR=${NIMBLE_CMAKE_DIR}" "-DWORK_DIR=${WORK_DIR}" "-DCASE=${NM_CASE}"
                    -P "${CMAKE_CURRENT_LIST_FILE}"
            RESULT_VARIABLE NM_RESULT
            OUTPUT_VARIABLE NM_OUT
            ERROR_VARIABLE  NM_ERR)
    set(NM_ALL "${NM_OUT}${NM_ERR}")
    set(NM_OK OFF)
    if(NM_WHAT STREQUAL "ERROR")
        string(FIND "${NM_ALL}" "${NM_WANT}" NM_AT)
        if(NOT NM_RESULT EQUAL 0 AND NM_AT GREATER -1)
            set(NM_OK ON)
        endif()
    elseif(NM_RESULT EQUAL 0 AND NM_ALL MATCHES "${NM_WHAT}=([^\n]*)")
        if(CMAKE_MATCH_1 STREQUAL "${NM_WANT}")
            set(NM_OK ON)
        endif()
    endif()
    if(NM_OK)
        message(STATUS "  ok    ${NM_CASE}: ${NM_WHAT} ${NM_WANT}")
    else()
        message(STATUS "  FAIL  ${NM_CASE}: wanted ${NM_WHAT} '${NM_WANT}', got (exit ${NM_RESULT}):\n${NM_ALL}")
        math(EXPR GFailures "${GFailures} + 1")
        set(GFailures ${GFailures} PARENT_SCOPE)
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")

# Dependencies first, optional ones included when enabled; the rest by name.
plugin(order Project Alpha 1.0.0 "[Dependencies]" "Plugins=Beta>=1.2.0")
plugin(order Engine  Beta  1.3.0 "[Dependencies]" "OptionalPlugins=Gamma, Delta")
plugin(order Engine  Gamma 0.1.0 "EnabledByDefault=true")
plugin(order Engine  Delta 0.1.0)
config(order "Alpha=true" "Beta=true")
expect(order ORDER "Gamma;Beta;Alpha")

# A project's plugin overrides the engine's of the same name.
plugin(override Project Beta 2.0.0)
plugin(override Engine  Beta 1.0.0)
config(override "Beta=true")
expect(override DIRS "Project/Beta")

# Enabled by default unless the project says false; off by default unless true.
plugin(enable Engine OnByDefault 1.0.0 "EnabledByDefault=true")
plugin(enable Engine OffByDefault 1.0.0)
plugin(enable Engine TurnedOff 1.0.0 "EnabledByDefault=true")
config(enable "TurnedOff=false" "offbydefault=yes")
expect(enable ORDER "OffByDefault;OnByDefault")

# Module sections are read; a bad Type is refused.
plugin(moduletype Engine Moduled 1.0.0 "[Module_Moduled]" "Type=Sometimes")
config(moduletype "Moduled=true")
expect(moduletype ERROR "Type must be Runtime, Editor, ClientOnly or ServerOnly")

# Refusals.
plugin(missing Engine Alpha 1.0.0 "[Dependencies]" "Plugins=Nowhere")
config(missing "Alpha=true")
expect(missing ERROR "depends on Nowhere, which is in none of")

plugin(disabled Engine Alpha 1.0.0 "[Dependencies]" "Plugins=Beta")
plugin(disabled Engine Beta 1.0.0)
config(disabled "Alpha=true")
expect(disabled ERROR "depends on Beta, which is not enabled")

plugin(tooold Engine Alpha 1.0.0 "[Dependencies]" "Plugins=Beta>=2.0.0")
plugin(tooold Engine Beta 1.9.9)
config(tooold "Alpha=true" "Beta=true")
expect(tooold ERROR "needs Beta 2.0.0 or newer; found 1.9.9")

plugin(cycle Engine Alpha 1.0.0 "[Dependencies]" "Plugins=Beta")
plugin(cycle Engine Beta 1.0.0 "[Dependencies]" "Plugins=Alpha")
config(cycle "Alpha=true" "Beta=true")
expect(cycle ERROR "Plugin dependency cycle")

plugin(shortname Engine A 1.0.0)
expect(shortname ERROR "must be a letter then letters or digits")

plugin(badversion Engine Alpha 1.0)
expect(badversion ERROR "Version must be major.minor.patch")

file(WRITE "${WORK_DIR}/newengine/Engine/Alpha/Alpha.nmplugin"
     "[Plugin]\nName=Alpha\nVersion=1.0.0\nEngineVersion=0.9\nEnabledByDefault=true\n")
expect(newengine ERROR "needs engine 0.9 or newer; this engine is 0.1")

file(WRITE "${WORK_DIR}/misnamed/Engine/Alpha/Alpha.nmplugin" "[Plugin]\nName=Beta\nVersion=1.0.0\nEngineVersion=0.1\n")
expect(misnamed ERROR "must live in Beta/Beta.nmplugin")

if(GFailures GREATER 0)
    message(FATAL_ERROR "PluginResolveTest: ${GFailures} failure(s)")
endif()
message(STATUS "PluginResolveTest: all passed")

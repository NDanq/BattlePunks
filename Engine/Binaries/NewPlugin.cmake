# ===========================================================================
# NewPlugin.cmake -- a new plugin from Engine/Templates/Plugin, without the editor.
#
#     cmake -DNAME=<Name> -DPROJECT=<game folder> [-DSTARTER=Component] -P Engine/Binaries/NewPlugin.cmake
#     cmake -DNAME=<Name> -DLOCATION=<plugins folder> [...] -P Engine/Binaries/NewPlugin.cmake
#
#   PROJECT       makes <game>/Plugins/<Name> and enables it in the game's
#                 Config/DefaultEngine.ini ([Plugins] <Name>=true)
#   LOCATION      makes <folder>/<Name> and enables nothing: Engine/Plugins,
#                 or a NIMBLE_PLUGIN_DIRS folder
#   STARTER       Empty, Component (default) or RenderPass
#   CATEGORY      the Add Component group and descriptor Category (Gameplay)
#   DESCRIPTION   one line
#   TEST          ON (default) or OFF: Tests/ and its ctest
#   EDITOR        OFF (default) or ON: an Editor-only module beside it
#   CONTENT       OFF (default) or ON: a Content/ folder, read as <Name>:/...
#
# The editor's File > New Plugin makes the same plugin, byte for byte
# (NimbleEditorProjectTests compares them). Then build the game, and for a
# RenderPass starter cook its shader: NimbleCook shaders -project <game>.
# ===========================================================================

cmake_minimum_required(VERSION 3.16)

get_filename_component(NP_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${NP_ENGINE_DIR}/cmake/NimblePluginTemplate.cmake")

if(NOT DEFINED NAME OR NAME STREQUAL "")
    message(FATAL_ERROR "NewPlugin: no name. Usage: cmake -DNAME=<Name> -DPROJECT=<game folder> [-DSTARTER=Component] -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
if(NOT DEFINED STARTER OR STARTER STREQUAL "")
    set(STARTER Component)
endif()
if(NOT DEFINED TEST)
    set(TEST ON)
endif()

if(DEFINED PROJECT AND NOT PROJECT STREQUAL "")
    get_filename_component(PROJECT "${PROJECT}" ABSOLUTE)
    set(NP_CONFIG "${PROJECT}/Config/DefaultEngine.ini")
    if(NOT EXISTS "${NP_CONFIG}")
        message(FATAL_ERROR "NewPlugin: ${PROJECT} is not a game folder (no Config/DefaultEngine.ini)")
    endif()
    set(NP_DEST "${PROJECT}/Plugins")
elseif(DEFINED LOCATION AND NOT LOCATION STREQUAL "")
    get_filename_component(NP_DEST "${LOCATION}" ABSOLUTE)
else()
    message(FATAL_ERROR "NewPlugin: say where: -DPROJECT=<game folder> or -DLOCATION=<plugins folder>")
endif()

# Taken: the engine's modules and targets, and every plugin already beside it.
nimble_plugin_reserved_names(NP_RESERVED "${NP_ENGINE_DIR}")
foreach(NP_ROOT "${NP_DEST}" "${NP_ENGINE_DIR}/Plugins")
    file(GLOB NP_SEEN "${NP_ROOT}/*/*.nmplugin")
    foreach(NP_FILE IN LISTS NP_SEEN)
        get_filename_component(NP_FILE "${NP_FILE}" NAME_WE)
        list(APPEND NP_RESERVED "${NP_FILE}")
    endforeach()
endforeach()

nimble_engine_version(NP_ENGINE_VERSION "${NP_ENGINE_DIR}")
set(NP_FLAGS)
foreach(NP_OPTION TEST EDITOR CONTENT)
    if(${NP_OPTION})
        list(APPEND NP_FLAGS ${NP_OPTION})
    endif()
endforeach()

nimble_create_plugin(
        TEMPLATE_DIR   "${NP_ENGINE_DIR}/Templates/Plugin"
        STARTER        "${STARTER}"
        DEST           "${NP_DEST}"
        NAME           "${NAME}"
        ENGINE_VERSION "${NP_ENGINE_VERSION}"
        CATEGORY       "${CATEGORY}"
        DESCRIPTION    "${DESCRIPTION}"
        RESERVED       ${NP_RESERVED}
        ${NP_FLAGS})

if(NP_CONFIG)
    # [Plugins] <Name>=true: under the section's header, or a new section.
    file(READ "${NP_CONFIG}" NP_TEXT)
    if(NP_TEXT MATCHES "(^|\n)\\[Plugins\\][^\n]*\n")
        string(REGEX REPLACE "(^|\n)(\\[Plugins\\][^\n]*\n)" "\\1\\2${NAME}=true\n" NP_TEXT "${NP_TEXT}")
    else()
        if(NOT NP_TEXT MATCHES "\n$")
            string(APPEND NP_TEXT "\n")
        endif()
        string(APPEND NP_TEXT "\n[Plugins]\n${NAME}=true\n")
    endif()
    file(WRITE "${NP_CONFIG}" "${NP_TEXT}")
    message(STATUS "NewPlugin: ${NP_DEST}/${NAME}, enabled in ${NP_CONFIG}. Build the game to add it.")
else()
    message(STATUS "NewPlugin: ${NP_DEST}/${NAME}. Enable it in a game's [Plugins] (${NAME}=true) and build.")
endif()

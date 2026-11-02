# ===========================================================================
# NimblePluginTemplate.cmake -- a new plugin from Engine/Templates/Plugin.
#
# nimble_create_plugin() is what Engine/Binaries/NewPlugin.cmake runs, and
# what a configure with NIMBLE_CHECK_PLUGIN_TEMPLATES runs on every starter
# (NimblePlugins.cmake). The editor's File > New Plugin
# (Editor/Source/Private/Editor/Project/PluginTemplate.cpp) does the same in
# C++; NimbleEditorProjectTests checks the two write the same files, byte for
# byte. Change both. Engine/Templates/Plugin/README.md describes the tokens.
#
# Script mode or configure: no targets made here.
# ===========================================================================

# nimble_plugin_reserved_names(<OutVar> <EngineDir>)
#   Names a new plugin cannot take: the engine's modules and targets.
function(nimble_plugin_reserved_names NM_OUT NM_ENGINE_DIR)
    set(NM_NAMES Engine EngineCore NimbleEngine NimbleEditor NimbleCook NimblePak Games Templates Plugins)
    file(GLOB NM_MODULES LIST_DIRECTORIES true "${NM_ENGINE_DIR}/Modules/*")
    foreach(NM_MODULE IN LISTS NM_MODULES)
        if(IS_DIRECTORY "${NM_MODULE}")
            get_filename_component(NM_MODULE "${NM_MODULE}" NAME)
            list(APPEND NM_NAMES "${NM_MODULE}")
        endif()
    endforeach()
    set(${NM_OUT} "${NM_NAMES}" PARENT_SCOPE)
endfunction()

# nimble_engine_version(<OutVar> <EngineDir>)
#   project(NimbleEngine VERSION x.y) from <EngineDir>/CMakeLists.txt.
function(nimble_engine_version NM_OUT NM_ENGINE_DIR)
    file(STRINGS "${NM_ENGINE_DIR}/CMakeLists.txt" NM_LINE REGEX "^project\\(NimbleEngine VERSION [0-9.]+")
    string(REGEX REPLACE "^project\\(NimbleEngine VERSION ([0-9.]+).*$" "\\1" NM_VERSION "${NM_LINE}")
    set(${NM_OUT} "${NM_VERSION}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# nimble_create_plugin(
#     TEMPLATE_DIR   <Engine/Templates/Plugin>
#     STARTER        <Empty | Component | RenderPass>
#     DEST           <the folder to make <NAME>/ in>
#     NAME           <Name>
#     ENGINE_VERSION <x.y>
#     [CATEGORY      <text>]          default Gameplay
#     [DESCRIPTION   <text>]
#     [RESERVED      <names...>]      refused, without case
#     [TEST] [EDITOR] [CONTENT]
#     [UPDATE])                       the folder may exist: write only what differs
#
#   FATAL_ERROR, writing nothing, on a bad name, category or starter, or a
#   folder that already exists (without UPDATE).
# ---------------------------------------------------------------------------
function(nimble_create_plugin)
    cmake_parse_arguments(NP "TEST;EDITOR;CONTENT;UPDATE" "TEMPLATE_DIR;STARTER;DEST;NAME;ENGINE_VERSION;CATEGORY;DESCRIPTION" "RESERVED" ${ARGN})

    if(NOT NP_NAME MATCHES "^[A-Za-z][A-Za-z0-9]+$")
        message(FATAL_ERROR "NewPlugin: '${NP_NAME}': a letter, then letters or digits only, two characters at least")
    endif()
    string(TOLOWER "${NP_NAME}" NP_LOWER)
    foreach(NP_TAKEN IN LISTS NP_RESERVED)
        string(TOLOWER "${NP_TAKEN}" NP_TAKEN_LOWER)
        if(NP_LOWER STREQUAL NP_TAKEN_LOWER)
            message(FATAL_ERROR "NewPlugin: '${NP_NAME}' is taken: a plugin, an engine module or a build target already has that name")
        endif()
    endforeach()
    if("${NP_CATEGORY}" STREQUAL "")
        set(NP_CATEGORY "Gameplay")
    endif()
    if(NOT NP_CATEGORY MATCHES "^[A-Za-z0-9 ]+$")
        message(FATAL_ERROR "NewPlugin: category '${NP_CATEGORY}': letters, digits and spaces only")
    endif()
    string(REGEX REPLACE "[\r\n]+" " " NP_DESCRIPTION "${NP_DESCRIPTION}")
    string(STRIP "${NP_DESCRIPTION}" NP_DESCRIPTION)
    if("${NP_DESCRIPTION}" STREQUAL "")
        set(NP_DESCRIPTION "A NimbleEngine plugin")
    endif()
    if(NOT IS_DIRECTORY "${NP_TEMPLATE_DIR}/${NP_STARTER}" OR NOT IS_DIRECTORY "${NP_TEMPLATE_DIR}/Common"
       OR NP_STARTER STREQUAL "Common")
        message(FATAL_ERROR "NewPlugin: no starter '${NP_STARTER}' in ${NP_TEMPLATE_DIR}")
    endif()
    set(NP_OUT "${NP_DEST}/${NP_NAME}")
    if(EXISTS "${NP_OUT}" AND NOT NP_UPDATE)
        message(FATAL_ERROR "NewPlugin: ${NP_OUT} already exists")
    endif()
    string(TOUPPER "${NP_NAME}" NP_UPPER)

    # Common's files, then the starter's over them.
    file(GLOB_RECURSE NP_COMMON  RELATIVE "${NP_TEMPLATE_DIR}/Common"        "${NP_TEMPLATE_DIR}/Common/*")
    file(GLOB_RECURSE NP_OWN     RELATIVE "${NP_TEMPLATE_DIR}/${NP_STARTER}" "${NP_TEMPLATE_DIR}/${NP_STARTER}/*")
    set(NP_ALL ${NP_COMMON} ${NP_OWN})
    list(REMOVE_DUPLICATES NP_ALL)
    list(SORT NP_ALL)

    foreach(NP_REL IN LISTS NP_ALL)
        if(NP_REL IN_LIST NP_OWN)
            set(NP_SRC "${NP_TEMPLATE_DIR}/${NP_STARTER}/${NP_REL}")
            set(NP_FROM_COMMON OFF)
        else()
            set(NP_SRC "${NP_TEMPLATE_DIR}/Common/${NP_REL}")
            set(NP_FROM_COMMON ON)
        endif()
        if(NOT NP_TEST AND NP_REL MATCHES "^Tests/")
            continue()
        endif()
        if(NOT NP_EDITOR AND NP_REL MATCHES "^Source/__PLUGIN__Editor/")
            continue()
        endif()
        if(NOT NP_CONTENT AND NP_FROM_COMMON AND NP_REL MATCHES "^Content/")
            continue()
        endif()

        file(READ "${NP_SRC}" NP_TEXT)
        string(REPLACE "\r\n" "\n" NP_TEXT "${NP_TEXT}")

        # @IF_<OPTION>@ lines: kept less the marker, or dropped whole.
        foreach(NP_OPTION TEST EDITOR CONTENT)
            if(NP_${NP_OPTION})
                string(REGEX REPLACE "(^|\n)([ \t]*)@IF_${NP_OPTION}@" "\\1\\2" NP_TEXT "${NP_TEXT}")
            else()
                while(NP_TEXT MATCHES "(^|\n)[ \t]*@IF_${NP_OPTION}@[^\n]*\n")
                    string(REGEX REPLACE "(^|\n)[ \t]*@IF_${NP_OPTION}@[^\n]*\n" "\\1" NP_TEXT "${NP_TEXT}")
                endwhile()
            endif()
        endforeach()

        string(REPLACE "@PLUGIN_NAME@"        "${NP_NAME}"           NP_TEXT "${NP_TEXT}")
        string(REPLACE "@PLUGIN_UPPER@"       "${NP_UPPER}"          NP_TEXT "${NP_TEXT}")
        string(REPLACE "@PLUGIN_CATEGORY@"    "${NP_CATEGORY}"       NP_TEXT "${NP_TEXT}")
        string(REPLACE "@PLUGIN_DESCRIPTION@" "${NP_DESCRIPTION}"    NP_TEXT "${NP_TEXT}")
        string(REPLACE "@PLUGIN_STARTER@"     "${NP_STARTER}"        NP_TEXT "${NP_TEXT}")
        string(REPLACE "@ENGINE_VERSION@"     "${NP_ENGINE_VERSION}" NP_TEXT "${NP_TEXT}")

        string(REPLACE "__PLUGIN__" "${NP_NAME}" NP_DST_REL "${NP_REL}")
        if(NP_UPDATE AND EXISTS "${NP_OUT}/${NP_DST_REL}")
            file(READ "${NP_OUT}/${NP_DST_REL}" NP_OLD)
            if(NP_OLD STREQUAL NP_TEXT)
                continue()   # unchanged: its timestamp kept, so nothing rebuilds
            endif()
        endif()
        # LF on every platform, as the editor writes it: file(WRITE) is text mode
        # and gives CRLF on Windows. (file(CONFIGURE) is CMake 3.18+.)
        string(FIND "${NP_TEXT}" "\n" NP_NEWLINE)
        if(NP_NEWLINE EQUAL -1 OR CMAKE_VERSION VERSION_LESS 3.18)
            file(WRITE "${NP_OUT}/${NP_DST_REL}" "${NP_TEXT}")
        else()
            get_filename_component(NP_DST_DIR "${NP_OUT}/${NP_DST_REL}" DIRECTORY)
            file(MAKE_DIRECTORY "${NP_DST_DIR}")
            string(REGEX REPLACE "\n$" "" NP_TEXT "${NP_TEXT}")
            file(CONFIGURE OUTPUT "${NP_OUT}/${NP_DST_REL}" CONTENT "@NP_TEXT@" @ONLY NEWLINE_STYLE UNIX)
        endif()
    endforeach()
endfunction()

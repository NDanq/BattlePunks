# ===========================================================================
# NimbleProject.cmake -- the project's module choices, from its config file.
#
# A game opts in or out of the optional engine modules in its own
# Config/DefaultEngine.ini, the file the editor's Project Settings edits:
#
#     [Modules]
#     NetCore=true
#     JoltPhysics=false
#
# The game's CMakeLists names that file before adding the engine:
#
#     set(NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
#     add_subdirectory(<engine> ...)
#
# and Engine/CMakeLists.txt turns each key it finds into the module's
# NIMBLE_WITH_<MODULE> cache option. A key the file does not have leaves the
# option alone -- its default, or a -D on the configure line. A key it does
# have wins over -D: the file is the project's, and the editor writes it.
#
# The file is a configure dependency, so the next build after an edit
# re-configures by itself and builds (or drops) the module.
#
# Read here with plain file(STRINGS), so no Python or other tool is needed to
# configure: sections in [brackets], Key=Value lines, ';' and '#' comments.
# ===========================================================================

# nimble_project_module(<Key> <OptionVariable> <Doc>)
#   [Modules] <Key> in NIMBLE_PROJECT_CONFIG, if present, FORCEs <OptionVariable>.
function(nimble_project_module NM_KEY NM_OPTION NM_DOC)
    if(NOT NIMBLE_PROJECT_CONFIG OR NOT EXISTS "${NIMBLE_PROJECT_CONFIG}")
        return()
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NIMBLE_PROJECT_CONFIG}")

    file(STRINGS "${NIMBLE_PROJECT_CONFIG}" NM_LINES)
    set(NM_SECTION "")
    set(NM_VALUE "")
    foreach(NM_LINE IN LISTS NM_LINES)
        string(STRIP "${NM_LINE}" NM_LINE)
        if(NM_LINE MATCHES "^[;#]" OR NM_LINE STREQUAL "")
            continue()
        endif()
        if(NM_LINE MATCHES "^\\[(.*)\\]")
            string(STRIP "${CMAKE_MATCH_1}" NM_SECTION)
            string(TOLOWER "${NM_SECTION}" NM_SECTION)
            continue()
        endif()
        if(NM_SECTION STREQUAL "modules" AND NM_LINE MATCHES "^([^=]+)=(.*)$")
            string(STRIP "${CMAKE_MATCH_1}" NM_LINE_KEY)
            string(STRIP "${CMAKE_MATCH_2}" NM_LINE_VALUE)
            string(TOLOWER "${NM_LINE_KEY}" NM_LINE_KEY)
            string(TOLOWER "${NM_KEY}" NM_WANT)
            if(NM_LINE_KEY STREQUAL NM_WANT)
                set(NM_VALUE "${NM_LINE_VALUE}")   # the last one wins, as at runtime
            endif()
        endif()
    endforeach()

    if(NM_VALUE STREQUAL "")
        return()
    endif()
    string(TOLOWER "${NM_VALUE}" NM_VALUE)
    if(NM_VALUE MATCHES "^(true|1|yes|on)$")
        set(NM_ON ON)
    elseif(NM_VALUE MATCHES "^(false|0|no|off)$")
        set(NM_ON OFF)
    else()
        message(WARNING "${NIMBLE_PROJECT_CONFIG}: [Modules] ${NM_KEY}=${NM_VALUE} is not true or false; ignored")
        return()
    endif()

    if((NM_ON AND NOT ${NM_OPTION}) OR (NOT NM_ON AND ${NM_OPTION}))
        message(STATUS "NimbleEngine: ${NM_OPTION}=${NM_ON}, from [Modules] ${NM_KEY} in ${NIMBLE_PROJECT_CONFIG}")
    endif()
    set(${NM_OPTION} ${NM_ON} CACHE BOOL "${NM_DOC}" FORCE)
endfunction()

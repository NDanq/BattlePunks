# ===========================================================================
# NimbleProject.cmake -- the project's module choices and world list, from its
# config file.
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

# nimble_project_worlds(<ConfigFile> <ProjectDir> <OutFiles> <OutRoot>)
#   The level files a build stages: the [World_*] sections of <ConfigFile>
#   (Project Settings > Worlds; NimbleEngine/WorldList.h) whose Cook is not
#   false. Each is staged at <its path relative to <OutRoot>> beside the
#   executable -- the path the game loads it by.
#
#     NIMBLE_UNCOOKED_CONTENT   the text world: its .nmworld manifest and the
#                               .nmentity / .nmprefabinst files in the folder
#                               of the same name beside it. <OutRoot> is
#                               <ProjectDir>.
#     Shipping                  the cooked world, <ProjectDir>/Saved/Cooked/<File>
#                               (the editor's Build > Cook Content). <OutRoot>
#                               is <ProjectDir>/Saved/Cooked; a listed world not
#                               cooked yet is a warning.
#
#   A world left out of the list, or with Cook=false, is not staged at all.
function(nimble_project_worlds NM_CONFIG NM_PROJECT_DIR NM_OUT_FILES NM_OUT_ROOT)
    set(NM_FILES "")
    if(NIMBLE_UNCOOKED_CONTENT)
        set(NM_ROOT "${NM_PROJECT_DIR}")
    else()
        set(NM_ROOT "${NM_PROJECT_DIR}/Saved/Cooked")
    endif()
    if(NOT EXISTS "${NM_CONFIG}")
        set(${NM_OUT_FILES} "" PARENT_SCOPE)
        set(${NM_OUT_ROOT} "${NM_ROOT}" PARENT_SCOPE)
        return()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NM_CONFIG}")

    # Section by section: name -> File, Cook.
    file(STRINGS "${NM_CONFIG}" NM_LINES)
    set(NM_WORLDS "")
    set(NM_SECTION "")
    foreach(NM_LINE IN LISTS NM_LINES)
        string(STRIP "${NM_LINE}" NM_LINE)
        if(NM_LINE MATCHES "^[;#]" OR NM_LINE STREQUAL "")
            continue()
        endif()
        if(NM_LINE MATCHES "^\\[(.*)\\]")
            string(STRIP "${CMAKE_MATCH_1}" NM_SECTION)
            string(TOLOWER "${NM_SECTION}" NM_SECTION_LOWER)
            if(NM_SECTION_LOWER MATCHES "^world_(.+)$")
                set(NM_WORLD "${CMAKE_MATCH_1}")
                if(NOT NM_WORLD IN_LIST NM_WORLDS)
                    list(APPEND NM_WORLDS "${NM_WORLD}")
                    set(NM_COOK_${NM_WORLD} "true")
                endif()
            else()
                set(NM_WORLD "")
            endif()
            continue()
        endif()
        if(NM_WORLD AND NM_LINE MATCHES "^([^=]+)=(.*)$")
            string(STRIP "${CMAKE_MATCH_1}" NM_KEY)
            string(STRIP "${CMAKE_MATCH_2}" NM_VALUE)
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" NM_VALUE "${NM_VALUE}")
            string(TOLOWER "${NM_KEY}" NM_KEY)
            if(NM_KEY STREQUAL "file")
                set(NM_FILE_${NM_WORLD} "${NM_VALUE}")
            elseif(NM_KEY STREQUAL "cook")
                string(TOLOWER "${NM_VALUE}" NM_COOK_${NM_WORLD})
            endif()
        endif()
    endforeach()

    foreach(NM_WORLD IN LISTS NM_WORLDS)
        set(NM_FILE "${NM_FILE_${NM_WORLD}}")
        if(NM_FILE STREQUAL "" OR NM_COOK_${NM_WORLD} MATCHES "^(false|0|no|off)$")
            continue()
        endif()
        if(NIMBLE_UNCOOKED_CONTENT)
            if(NOT EXISTS "${NM_PROJECT_DIR}/${NM_FILE}")
                message(WARNING "${NM_CONFIG}: [World_${NM_WORLD}] File=${NM_FILE} does not exist; it is not staged")
                continue()
            endif()
            list(APPEND NM_FILES "${NM_PROJECT_DIR}/${NM_FILE}")
            get_filename_component(NM_DIR "${NM_PROJECT_DIR}/${NM_FILE}" DIRECTORY)
            get_filename_component(NM_STEM "${NM_FILE}" NAME_WLE)
            file(GLOB_RECURSE NM_ENTITIES CONFIGURE_DEPENDS
                    "${NM_DIR}/${NM_STEM}/*.nmentity" "${NM_DIR}/${NM_STEM}/*.nmprefabinst")
            list(APPEND NM_FILES ${NM_ENTITIES})
        else()
            # A configure dependency even while missing: a cook that makes it re-configures.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NM_ROOT}/${NM_FILE}")
            if(EXISTS "${NM_ROOT}/${NM_FILE}")
                list(APPEND NM_FILES "${NM_ROOT}/${NM_FILE}")
            else()
                message(WARNING "${NM_CONFIG}: [World_${NM_WORLD}] is not cooked (no ${NM_ROOT}/${NM_FILE}); a "
                        "Shipping build cannot load it. Cook it in the editor: Build > Cook Content.")
            endif()
        endif()
    endforeach()

    set(${NM_OUT_FILES} "${NM_FILES}" PARENT_SCOPE)
    set(${NM_OUT_ROOT} "${NM_ROOT}" PARENT_SCOPE)
endfunction()

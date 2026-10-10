# ===========================================================================
# NewProject.cmake -- a new game from Engine/Templates/Game, without the editor.
#
#     cmake -DNAME=<Name> [-DLOCATION=<folder>] -P Engine/Binaries/NewProject.cmake
#
# or the wrappers beside this file: NewProject.sh / NewProject.bat <Name> [<folder>].
#
# Makes <folder>/<Name> (default folder: Games/, beside Engine/) exactly as the
# editor's File > New Project does (Editor/Source/Private/Editor/Project/
# ProjectTemplate.cpp): every template file copied, with
#
#   @PROJECT_NAME@   replaced by <Name>              (contents)
#   @ENGINE_DIR@     replaced by the Engine tree, as the new CMakeLists reads it
#   __PROJECT__      replaced by <Name>              (file names)
#
# and every line ending LF, whatever the checkout has (core.autocrlf).
#
# The two are kept identical by NimbleEditorProjectTests, which runs this
# script and compares its output with the editor's file by file. Change both.
#
# The engine is never edited: the game configures standalone and adds the
# Engine tree itself.
# ===========================================================================

cmake_minimum_required(VERSION 3.18)   # file(CONFIGURE)

get_filename_component(NP_ENGINE_DIR   "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(NP_TEMPLATE_DIR "${NP_ENGINE_DIR}/Templates/Game" ABSOLUTE)

function(np_fail NP_MESSAGE)
    message(FATAL_ERROR "NewProject: ${NP_MESSAGE}")
endfunction()

# --- Arguments ---------------------------------------------------------------
if(NOT DEFINED NAME OR NAME STREQUAL "")
    np_fail("no name. Usage: cmake -DNAME=<Name> [-DLOCATION=<folder>] -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

# Same rules as EditorProject::ValidateName: it names classes and targets.
if(NOT NAME MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
    np_fail("'${NAME}': letters, digits and _ only, starting with a letter")
endif()
string(LENGTH "${NAME}" NP_LENGTH)
if(NP_LENGTH GREATER 48)
    np_fail("'${NAME}': at most 48 characters")
endif()
string(TOLOWER "${NAME}" NP_LOWER)
foreach(NP_RESERVED engine nimbleengine nimbleeditor games templates)
    if(NP_LOWER STREQUAL NP_RESERVED)
        np_fail("\"${NAME}\" is used by the engine")
    endif()
endforeach()

if(NOT EXISTS "${NP_TEMPLATE_DIR}/CMakeLists.txt")
    np_fail("the project template is missing: ${NP_TEMPLATE_DIR}")
endif()

if(NOT DEFINED LOCATION OR LOCATION STREQUAL "")
    set(LOCATION "${NP_ENGINE_DIR}/../Games")
endif()
get_filename_component(NP_LOCATION "${LOCATION}" ABSOLUTE)   # relative to the caller's directory
set(NP_PROJECT_DIR "${NP_LOCATION}/${NAME}")
if(EXISTS "${NP_PROJECT_DIR}")
    np_fail("${NP_PROJECT_DIR} already exists")
endif()

# --- @ENGINE_DIR@ ------------------------------------------------------------
# Relative where there is a relative path (Sandbox's "../../Engine"); absolute
# otherwise (another drive).
file(RELATIVE_PATH NP_ENGINE_REL "${NP_PROJECT_DIR}" "${NP_ENGINE_DIR}")
if(IS_ABSOLUTE "${NP_ENGINE_REL}")
    set(NP_ENGINE_FOR_CMAKE "${NP_ENGINE_DIR}")
else()
    set(NP_ENGINE_FOR_CMAKE [=[${CMAKE_CURRENT_SOURCE_DIR}/]=])
    string(APPEND NP_ENGINE_FOR_CMAKE "${NP_ENGINE_REL}")
endif()

# --- Copy --------------------------------------------------------------------
# Into a hidden sibling first, renamed into place at the end, so a failure
# never leaves a half-made <Name> behind.
set(NP_STAGING "${NP_LOCATION}/.${NAME}.creating")
file(REMOVE_RECURSE "${NP_STAGING}")

file(GLOB_RECURSE NP_FILES LIST_DIRECTORIES false RELATIVE "${NP_TEMPLATE_DIR}" "${NP_TEMPLATE_DIR}/*")
list(SORT NP_FILES)
foreach(NP_FILE IN LISTS NP_FILES)
    string(REPLACE "__PROJECT__" "${NAME}" NP_TARGET "${NP_FILE}")

    file(READ "${NP_TEMPLATE_DIR}/${NP_FILE}" NP_TEXT)
    string(REPLACE "@PROJECT_NAME@" "${NAME}" NP_TEXT "${NP_TEXT}")
    string(REPLACE "@ENGINE_DIR@" "${NP_ENGINE_FOR_CMAKE}" NP_TEXT "${NP_TEXT}")

    # LF on every platform. file(READ) has already dropped any CR, but
    # file(WRITE) on Windows turns each LF into CRLF; CONFIGURE writes LF and
    # substitutes only @NP_TEXT@ itself (its value is not scanned again). It
    # ends the text with a newline, so the file's own last one is dropped
    # first. Text with no newline at all (an empty .gitkeep) has nothing to
    # turn into CRLF: written as it is. (A file with lines but no final
    # newline would gain one; the template has none.)
    string(FIND "${NP_TEXT}" "\n" NP_NEWLINE)
    if(NP_NEWLINE EQUAL -1)
        file(WRITE "${NP_STAGING}/${NP_TARGET}" "${NP_TEXT}")
    else()
        string(REGEX REPLACE "\n$" "" NP_TEXT "${NP_TEXT}")
        file(CONFIGURE OUTPUT "${NP_STAGING}/${NP_TARGET}" CONTENT "@NP_TEXT@" @ONLY NEWLINE_STYLE UNIX)
    endif()
    message(STATUS "  ${NP_TARGET}")
endforeach()

file(RENAME "${NP_STAGING}" "${NP_PROJECT_DIR}")

file(TO_NATIVE_PATH "${NP_PROJECT_DIR}" NP_NATIVE)
message(STATUS "")
message(STATUS "Created ${NAME} in ${NP_NATIVE}")
message(STATUS "Build and open it:")
message(STATUS "    cd \"${NP_NATIVE}\"")
message(STATUS "    cmake --preset editor-debug")
message(STATUS "    cmake --build --preset editor-debug")
message(STATUS "Then run Binaries/<Platform>_Debug/NimbleEditor. Docs/GettingStarted.md walks through the rest.")

# ===========================================================================
# UnpackExample.cmake -- an example project (Engine/Examples) into <folder>/<Name>.
# Included by Engine/Binaries/NewProject.cmake when -DEXAMPLE=<Example> is given:
#
#     cmake -DEXAMPLE=DemoLab [-DNAME=<Name>] [-DLOCATION=<folder>] [-DBUILD=ON] [-DJOBS=<N>]
#           -P Engine/Binaries/NewProject.cmake
#
# NAME defaults to the example's own; LOCATION to Games/ beside Engine/.
# File > New Project runs this same script (NewProjectWizard.cpp), so the
# editor and the command line unpack identically.
#
# Steps, each logged as "NewProject: [i/n] <step>" (the wizard's progress):
#
#   unpack     check every part against Examples.json's SHA-256, extract them
#              into a hidden staging folder, rename, move it into place --
#              a failure never leaves a half-made <Name> behind
#   configure  cmake --preset <PRESET>             in the new project
#   build      cmake --build --preset <PRESET>
#   cook       Binaries/<Platform>_<Config>/NimbleCook shaders -project <dir> -editor
#
# STEPS picks them (a ;-list); by default "unpack", and all four with BUILD=ON.
# Without unpack the project must already exist: a retry of a failed build.
# PRESET defaults to editor-relwithdebinfo; JOBS, if given, caps the build's
# parallel jobs (--parallel), else the generator picks. EXAMPLES_DIR (default
# Engine/Examples) is where Examples.json and the archives are; tests point
# it at a fixture.
#
# Rename (NAME other than the example's): the example's name, and the same
# in capitals (include guards), replaced by NAME in the text files listed in
# NP_TEXT_EXTENSIONS, and in file names outside Content/ (assets keep theirs:
# other assets find them by path). Binary assets are never touched.
#
# The project's CMakeLists finds the engine at "../../Engine", as every game
# in Games/ does; unpacked anywhere else, that path is replaced with the one
# NewProject.cmake worked out (@ENGINE_DIR@ for a template).
#
# Expects from NewProject.cmake: NAME, NP_ENGINE_DIR, NP_LOCATION,
# NP_PROJECT_DIR, NP_ENGINE_FOR_CMAKE and np_fail().
# ===========================================================================

set(NP_TEXT_EXTENSIONS
    .txt .cmake .json .cpp .h .hpp .inl .ini .lua .py .md .sh .bat .gitignore
    .nmentity .nmworld .nmprefab .nmprefabinst .nmgraph .nmui .nmseq .nmbt .nmparticle)

if(NOT DEFINED EXAMPLES_DIR OR EXAMPLES_DIR STREQUAL "")
    set(EXAMPLES_DIR "${NP_ENGINE_DIR}/Examples")
endif()
get_filename_component(NP_EXAMPLES_DIR "${EXAMPLES_DIR}" ABSOLUTE)
if(NOT DEFINED PRESET OR PRESET STREQUAL "")
    set(PRESET "editor-relwithdebinfo")
endif()
if(NOT DEFINED STEPS OR STEPS STREQUAL "")
    if(BUILD)
        set(STEPS unpack configure build cook)
    else()
        set(STEPS unpack)
    endif()
endif()
foreach(NP_STEP IN LISTS STEPS)
    if(NOT NP_STEP MATCHES "^(unpack|configure|build|cook)$")
        np_fail("unknown step '${NP_STEP}': unpack, configure, build or cook")
    endif()
endforeach()

# --- The example's entry in Examples.json -------------------------------------
set(NP_INDEX_FILE "${NP_EXAMPLES_DIR}/Examples.json")
if(NOT EXISTS "${NP_INDEX_FILE}")
    np_fail("no example index at ${NP_INDEX_FILE}")
endif()
file(READ "${NP_INDEX_FILE}" NP_INDEX)
string(JSON NP_SCHEMA ERROR_VARIABLE NP_JSON_ERROR GET "${NP_INDEX}" Schema)
if(NOT NP_SCHEMA EQUAL 1)
    np_fail("${NP_INDEX_FILE} is not an example index this engine reads (Schema ${NP_SCHEMA})")
endif()

set(NP_ENTRY "")
string(JSON NP_COUNT LENGTH "${NP_INDEX}" Examples)
if(NP_COUNT GREATER 0)
    math(EXPR NP_LAST "${NP_COUNT} - 1")
    foreach(NP_I RANGE ${NP_LAST})
        string(JSON NP_EACH GET "${NP_INDEX}" Examples ${NP_I} Name)
        if(NP_EACH STREQUAL EXAMPLE)
            string(JSON NP_ENTRY GET "${NP_INDEX}" Examples ${NP_I})
        endif()
    endforeach()
endif()
if(NP_ENTRY STREQUAL "")
    np_fail("no example named '${EXAMPLE}' in ${NP_INDEX_FILE}")
endif()

string(TOUPPER "${EXAMPLE}" NP_EXAMPLE_UPPER)
string(TOUPPER "${NAME}" NP_NAME_UPPER)

list(LENGTH STEPS NP_STEP_COUNT)
set(NP_STEP_INDEX 0)
macro(np_step NP_WHAT)
    math(EXPR NP_STEP_INDEX "${NP_STEP_INDEX} + 1")
    message(STATUS "NewProject: [${NP_STEP_INDEX}/${NP_STEP_COUNT}] ${NP_WHAT}")
endmacro()

# --- unpack ----------------------------------------------------------------------
if("unpack" IN_LIST STEPS)
    np_step("Unpacking ${EXAMPLE} into ${NP_PROJECT_DIR}")
    if(EXISTS "${NP_PROJECT_DIR}")
        np_fail("${NP_PROJECT_DIR} already exists")
    endif()

    string(JSON NP_PART_COUNT LENGTH "${NP_ENTRY}" Parts)
    if(NP_PART_COUNT EQUAL 0)
        np_fail("${EXAMPLE} lists no archives in ${NP_INDEX_FILE}")
    endif()
    math(EXPR NP_LAST "${NP_PART_COUNT} - 1")
    set(NP_PARTS "")
    foreach(NP_I RANGE ${NP_LAST})
        string(JSON NP_FILE GET "${NP_ENTRY}" Parts ${NP_I} File)
        string(JSON NP_WANT GET "${NP_ENTRY}" Parts ${NP_I} Sha256)
        set(NP_PART "${NP_EXAMPLES_DIR}/${NP_FILE}")
        if(NOT EXISTS "${NP_PART}")
            np_fail("${NP_PART} is missing")
        endif()
        file(SHA256 "${NP_PART}" NP_HAVE)
        if(NOT NP_HAVE STREQUAL NP_WANT)
            np_fail("${NP_PART} is not the archive Examples.json lists (damaged, or packed without updating it)")
        endif()
        list(APPEND NP_PARTS "${NP_PART}")
    endforeach()

    set(NP_STAGING "${NP_LOCATION}/.${NAME}.creating")
    file(REMOVE_RECURSE "${NP_STAGING}")
    file(MAKE_DIRECTORY "${NP_STAGING}")
    foreach(NP_PART IN LISTS NP_PARTS)
        message(STATUS "  ${NP_PART}")
        file(ARCHIVE_EXTRACT INPUT "${NP_PART}" DESTINATION "${NP_STAGING}")
    endforeach()
    if(NOT EXISTS "${NP_STAGING}/CMakeLists.txt")
        file(REMOVE_RECURSE "${NP_STAGING}")
        np_fail("the archives of ${EXAMPLE} did not unpack to a project")
    endif()

    if(NOT NAME STREQUAL EXAMPLE)
        message(STATUS "  renaming ${EXAMPLE} to ${NAME}")
        file(GLOB_RECURSE NP_FILES LIST_DIRECTORIES false RELATIVE "${NP_STAGING}" "${NP_STAGING}/*")
        list(SORT NP_FILES)
        foreach(NP_FILE IN LISTS NP_FILES)
            get_filename_component(NP_NAME_PART "${NP_FILE}" NAME)
            string(REGEX MATCH "\\.[^.]*$" NP_EXT "${NP_NAME_PART}")
            if(NP_EXT IN_LIST NP_TEXT_EXTENSIONS)
                file(READ "${NP_STAGING}/${NP_FILE}" NP_TEXT)
                string(FIND "${NP_TEXT}" "${EXAMPLE}" NP_AT)
                string(FIND "${NP_TEXT}" "${NP_EXAMPLE_UPPER}" NP_AT_UPPER)
                if(NP_AT GREATER -1 OR NP_AT_UPPER GREATER -1)
                    string(REPLACE "${EXAMPLE}" "${NAME}" NP_TEXT "${NP_TEXT}")
                    string(REPLACE "${NP_EXAMPLE_UPPER}" "${NP_NAME_UPPER}" NP_TEXT "${NP_TEXT}")
                    # LF on every platform, as NewProject.cmake writes a template;
                    # a script keeps its executable bit.
                    set(NP_EXECUTABLE OFF)
                    if(IS_EXECUTABLE "${NP_STAGING}/${NP_FILE}")
                        set(NP_EXECUTABLE ON)
                    endif()
                    string(FIND "${NP_TEXT}" "\n" NP_NEWLINE)
                    if(NP_NEWLINE EQUAL -1)
                        file(WRITE "${NP_STAGING}/${NP_FILE}" "${NP_TEXT}")
                    else()
                        string(REGEX REPLACE "\n$" "" NP_TEXT "${NP_TEXT}")
                        file(CONFIGURE OUTPUT "${NP_STAGING}/${NP_FILE}" CONTENT "@NP_TEXT@" @ONLY NEWLINE_STYLE UNIX)
                    endif()
                    if(NP_EXECUTABLE)
                        file(CHMOD "${NP_STAGING}/${NP_FILE}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                             GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
                    endif()
                endif()
            endif()
            # Assets keep their names: other assets, binary ones too, find them by path.
            string(REPLACE "${EXAMPLE}" "${NAME}" NP_TARGET "${NP_FILE}")
            if(NOT NP_TARGET STREQUAL NP_FILE AND NOT NP_FILE MATCHES "^Content/")
                get_filename_component(NP_TARGET_DIR "${NP_STAGING}/${NP_TARGET}" DIRECTORY)
                file(MAKE_DIRECTORY "${NP_TARGET_DIR}")
                file(RENAME "${NP_STAGING}/${NP_FILE}" "${NP_STAGING}/${NP_TARGET}")
            endif()
        endforeach()
        # Folders named after the example, emptied by the renames above.
        file(GLOB_RECURSE NP_DIRS LIST_DIRECTORIES true RELATIVE "${NP_STAGING}" "${NP_STAGING}/*${EXAMPLE}*")
        foreach(NP_DIR IN LISTS NP_DIRS)
            if(IS_DIRECTORY "${NP_STAGING}/${NP_DIR}" AND NOT NP_DIR MATCHES "^Content/")
                file(GLOB_RECURSE NP_LEFT "${NP_STAGING}/${NP_DIR}/*")
                if(NP_LEFT STREQUAL "")
                    file(REMOVE_RECURSE "${NP_STAGING}/${NP_DIR}")
                endif()
            endif()
        endforeach()
    endif()

    set(NP_GAMES_ENGINE [=[${CMAKE_CURRENT_SOURCE_DIR}/../../Engine]=])
    if(NOT NP_ENGINE_FOR_CMAKE STREQUAL NP_GAMES_ENGINE)
        file(READ "${NP_STAGING}/CMakeLists.txt" NP_TEXT)
        string(REPLACE "${NP_GAMES_ENGINE}" "${NP_ENGINE_FOR_CMAKE}" NP_TEXT "${NP_TEXT}")
        string(REGEX REPLACE "\n$" "" NP_TEXT "${NP_TEXT}")
        file(CONFIGURE OUTPUT "${NP_STAGING}/CMakeLists.txt" CONTENT "@NP_TEXT@" @ONLY NEWLINE_STYLE UNIX)
    endif()

    file(RENAME "${NP_STAGING}" "${NP_PROJECT_DIR}")
elseif(NOT EXISTS "${NP_PROJECT_DIR}/CMakeLists.txt")
    np_fail("${NP_PROJECT_DIR} is not a project; unpack it first (STEPS without unpack only resume one)")
endif()

# --- configure, build -----------------------------------------------------------
foreach(NP_STEP configure build)
    if(NP_STEP IN_LIST STEPS)
        if(NP_STEP STREQUAL "configure")
            np_step("Configuring (cmake --preset ${PRESET})")
            set(NP_ARGS --preset "${PRESET}")
        else()
            np_step("Building (cmake --build --preset ${PRESET})")
            set(NP_ARGS --build --preset "${PRESET}")
            if(DEFINED JOBS AND JOBS MATCHES "^[1-9][0-9]*$")
                list(APPEND NP_ARGS --parallel ${JOBS})
            endif()
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" ${NP_ARGS} WORKING_DIRECTORY "${NP_PROJECT_DIR}" RESULT_VARIABLE NP_RESULT)
        if(NOT NP_RESULT EQUAL 0)
            np_fail("${NP_STEP} failed (${NP_RESULT}); the project is kept. Fix it and run again with -DSTEPS=\"${NP_STEP}...\"")
        endif()
    endif()
endforeach()

# --- cook ------------------------------------------------------------------------
if("cook" IN_LIST STEPS)
    np_step("Cooking shaders (NimbleCook)")
    if(PRESET MATCHES "relwithdebinfo")
        set(NP_CONFIG RelWithDebInfo)
    elseif(PRESET MATCHES "debug")
        set(NP_CONFIG Debug)
    else()
        np_fail("cook: no editor binaries for preset '${PRESET}'")
    endif()
    if(CMAKE_HOST_WIN32)
        set(NP_COOK "${NP_PROJECT_DIR}/Binaries/Windows_${NP_CONFIG}/NimbleCook.exe")
    elseif(CMAKE_HOST_APPLE)
        set(NP_COOK "${NP_PROJECT_DIR}/Binaries/Mac_${NP_CONFIG}/NimbleCook")
    else()
        set(NP_COOK "${NP_PROJECT_DIR}/Binaries/Linux_${NP_CONFIG}/NimbleCook")
    endif()
    if(NOT EXISTS "${NP_COOK}")
        np_fail("no ${NP_COOK}: build first")
    endif()
    execute_process(COMMAND "${NP_COOK}" shaders -project "${NP_PROJECT_DIR}" -editor
                    WORKING_DIRECTORY "${NP_PROJECT_DIR}" RESULT_VARIABLE NP_RESULT)
    if(NOT NP_RESULT EQUAL 0)
        np_fail("cook failed (${NP_RESULT}); the project is kept. Run again with -DSTEPS=cook")
    endif()
endif()

file(TO_NATIVE_PATH "${NP_PROJECT_DIR}" NP_NATIVE)
message(STATUS "")
message(STATUS "Created ${NAME} from the ${EXAMPLE} example in ${NP_NATIVE}")
if(NOT "build" IN_LIST STEPS)
    message(STATUS "Build and open it:")
    message(STATUS "    cd \"${NP_NATIVE}\"")
    message(STATUS "    cmake --preset ${PRESET}")
    message(STATUS "    cmake --build --preset ${PRESET}")
endif()

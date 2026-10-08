# ===========================================================================
# NimblePack.cmake -- a packaged game's content in packs, not loose files.
#
# With NIMBLE_PACKAGE_CONTENT on (Shipping, and every Build Project build), a
# Client or Server build stages nothing beside the executable but its
# binaries and two content packs (Core's CorePack.h), which the launcher
# mounts before it reads anything:
#
#     Paks/Engine.nmpak   the engine's: cooked shaders, its shader cache,
#                         Engine/Content meshes and fonts
#     Paks/Game.nmpak     the game's: Config/, Scripts/, Content/, the cooked
#                         worlds, prefabs and UI documents, its shader caches
#
# Every place that would copy a file beside the executable records it here
# instead (nimble_pack_add_files); a step that generates files writes them to
# a folder under the build tree and records that (nimble_pack_add_dir). At
# the end of a game's CMakeLists, nimble_package_game builds both packs with
# Engine/Tools/NimblePak, beside that game's executable.
#
# A pack stores paths only as hashes. A debug-info build's (anything but
# Shipping) also keeps the path behind each, for `NimblePak list` and logs; a
# Shipping build's instead strips its scripts to bytecode and its config of
# comments, and scrambles every entry's bytes (NimblePak --strip --scramble).
#
# Single-config only, as the staging it replaces: a pack needs one runtime
# directory to land in.
# ===========================================================================

# nimble_pack_stage_dir(<Pack> <OutVar>)
#   Where a step that generates a pack's files writes them.
function(nimble_pack_stage_dir NP_PACK NP_OUT)
    set(${NP_OUT} "${CMAKE_BINARY_DIR}/PackStage/${NP_PACK}" PARENT_SCOPE)
endfunction()

# nimble_pack_add_files(<Pack> <Root> <files...>)
#   Each file into <Pack>, as its path relative to <Root>: what it would have
#   been staged as beside the executable.
function(nimble_pack_add_files NP_PACK NP_ROOT)
    foreach(NP_FILE ${ARGN})
        file(RELATIVE_PATH NP_REL "${NP_ROOT}" "${NP_FILE}")
        set_property(GLOBAL APPEND PROPERTY NIMBLE_PACK_${NP_PACK}_LINES "F\t${NP_REL}\t${NP_FILE}")
        set_property(GLOBAL APPEND PROPERTY NIMBLE_PACK_${NP_PACK}_DEPENDS "${NP_FILE}")
    endforeach()
endfunction()

# nimble_pack_add_dir(<Pack> <RuntimePrefix> <Folder> <Stamp> <Target>)
#   Every file <Target> generates under <Folder>, as <RuntimePrefix>/<path>;
#   <Stamp> is the file its command touches, so the pack follows it.
function(nimble_pack_add_dir NP_PACK NP_PREFIX NP_DIR NP_STAMP NP_TARGET)
    set_property(GLOBAL APPEND PROPERTY NIMBLE_PACK_${NP_PACK}_LINES "D\t${NP_PREFIX}\t${NP_DIR}")
    set_property(GLOBAL APPEND PROPERTY NIMBLE_PACK_${NP_PACK}_DEPENDS "${NP_STAMP}")
    set_property(GLOBAL APPEND PROPERTY NIMBLE_PACK_${NP_PACK}_TARGETS ${NP_TARGET})
endfunction()

# nimble_package_game(TARGET <game executable> RUNTIME_DIR <dir> [DEPENDS <targets...>])
#   Paks/Engine.nmpak and Paks/Game.nmpak in <dir>, built before <game>.
#   Nothing without NIMBLE_PACKAGE_CONTENT.
function(nimble_package_game)
    cmake_parse_arguments(NPG "" "TARGET;RUNTIME_DIR" "DEPENDS" ${ARGN})
    if(NOT NIMBLE_PACKAGE_CONTENT)
        return()
    endif()
    set(NP_TARGET "${NPG_TARGET}")
    set(NP_RUNTIME_DIR "${NPG_RUNTIME_DIR}")
    if(NOT NP_TARGET OR NOT NP_RUNTIME_DIR)
        message(FATAL_ERROR "nimble_package_game(TARGET <game> RUNTIME_DIR <dir>): packaging needs a single-config build")
    endif()

    # Debug-info builds keep each path; Shipping strips what a person reads
    # (Lua to stripped bytecode, .ini comments) and scrambles the rest.
    if(CMAKE_BUILD_TYPE STREQUAL "Shipping")
        set(NP_NAMES --strip --scramble)
    else()
        set(NP_NAMES "--names")
    endif()

    set(NP_OUTPUTS)
    set(NP_TARGETS)
    foreach(NP_PACK Engine Game)
        get_property(NP_LINES GLOBAL PROPERTY NIMBLE_PACK_${NP_PACK}_LINES)
        get_property(NP_DEPENDS GLOBAL PROPERTY NIMBLE_PACK_${NP_PACK}_DEPENDS)
        get_property(NP_PACK_TARGETS GLOBAL PROPERTY NIMBLE_PACK_${NP_PACK}_TARGETS)
        list(APPEND NP_TARGETS ${NP_PACK_TARGETS})

        set(NP_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/${NP_TARGET}.${NP_PACK}.paklist")
        string(REPLACE ";" "\n" NP_TEXT "${NP_LINES}")
        file(WRITE "${NP_MANIFEST}.new" "${NP_TEXT}\n")
        configure_file("${NP_MANIFEST}.new" "${NP_MANIFEST}" COPYONLY)   # touched only when it changes

        set(NP_OUT "${NP_RUNTIME_DIR}/Paks/${NP_PACK}.nmpak")
        add_custom_command(
                OUTPUT  "${NP_OUT}"
                COMMAND NimblePak pack "${NP_MANIFEST}" "${NP_OUT}" ${NP_PACK} ${NP_NAMES}
                DEPENDS NimblePak "${NP_MANIFEST}" ${NP_DEPENDS}
                COMMENT "${NP_TARGET}: packing Paks/${NP_PACK}.nmpak"
                VERBATIM)
        list(APPEND NP_OUTPUTS "${NP_OUT}")
    endforeach()

    add_custom_target(${NP_TARGET}Paks DEPENDS ${NP_OUTPUTS})
    list(APPEND NP_TARGETS ${NPG_DEPENDS})
    if(NP_TARGETS)
        list(REMOVE_DUPLICATES NP_TARGETS)
    endif()
    foreach(NP_EACH ${NP_TARGETS})
        if(TARGET ${NP_EACH})
            add_dependencies(${NP_TARGET}Paks ${NP_EACH})
        endif()
    endforeach()
    add_dependencies(${NP_TARGET} ${NP_TARGET}Paks)
    set_target_properties(${NP_TARGET}Paks PROPERTIES FOLDER "Games")
    message(STATUS "${NP_TARGET}: content packaged into ${NP_RUNTIME_DIR}/Paks/ (NIMBLE_PACKAGE_CONTENT)")
endfunction()

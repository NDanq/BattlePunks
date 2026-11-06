# ===========================================================================
# nimble_stage_materials(TARGET <game> DIR <folder> [ROOT <dir>] [MODULE <name>] [RUNTIME_DIR <dir>])
#
# Stages the material graphs in <folder> (*.material.nmgraph, recursively) as
# NimbleCook cooked them -- the build never runs a shader compiler. ROOT
# (default: <folder>'s parent, the game's folder) is the project NimbleCook
# was given, which cooks below <ROOT>/Saved/Cooked/Shaders/:
#
#     Runtime/Shaders/Cache/<MODULE>.<Platform>.nmsc    every build: the
#         module's shader cache, what GameModuleHost mounts when MODULE loads
#         (RenderCore/MATERIALS.md). A graph is named by its path below ROOT:
#             Materials/Glow.material
#             Shaders/Cooked/<Platform>/Materials/Glow@<key>.<stage>[.spv|.cso]
#     Editor/...                                        Editor targets: the
#         same loose, which the editor's own on-save cook overwrites and its
#         file provider prefers over any cache
#     Shipping/Shaders/Cache/<MODULE>.<Platform>.nmsc   a packaged Shipping
#         build, over Runtime/'s: the cache naming nothing (ShaderCache.h)
#
# Cook them with
#
#     NimbleCook shaders -project <ROOT> [-editor] [-shipping]
#
# or the editor's Build > Cook Content. Without MODULE nothing is cached, and
# a Client build stages nothing at all. A variant not cooked yet is a warning
# (its materials draw as plain tints) and a configure dependency, so the cook
# that makes it re-configures; after that, each cook restages.
#
# nimble_cooked_shader_commands(<commands-var> <depends-var> <what> <cooked-dir>
#                               [<Variant> <dest-dir>]...)
#
# The COMMAND list that copies each <cooked-dir>/<Variant>/ into <dest-dir>,
# in order (a later variant overwrites), and the stamps it depends on. A
# variant NimbleCook has not made is left out, with a warning. Shared by the
# above and the engine's own staging (Engine/CMakeLists.txt).
# ===========================================================================

# CACHE: the functions run in the caller's scope. (CMAKE_CURRENT_FUNCTION_LIST_DIR
# needs CMake 3.17.) For the editor's material checks and shader PDBs, which
# still run cook_material_graphs.py themselves.
get_filename_component(NM_COOK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Modules/NodeGraph/Scripts/cook_material_graphs.py" ABSOLUTE)
set(NIMBLE_MATERIAL_COOK_SCRIPT "${NM_COOK_SCRIPT}" CACHE INTERNAL "NodeGraph's material cook, run by NimbleCook")
get_filename_component(NM_ENGINE_SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/../Content/Shaders" ABSOLUTE)
set(NIMBLE_ENGINE_SHADER_DIR "${NM_ENGINE_SHADER_DIR}" CACHE INTERNAL "Engine/Content/Shaders: the engine's shaders, and the include root of every cook")
get_filename_component(NM_ENGINE_MATERIAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../Content/Materials" ABSOLUTE)
set(NIMBLE_ENGINE_MATERIAL_DIR "${NM_ENGINE_MATERIAL_DIR}" CACHE INTERNAL "Engine/Content/Materials: the engine's own materials (DefaultLit)")

function(nimble_cooked_shader_commands NC_COMMANDS_VAR NC_DEPENDS_VAR NC_WHAT NC_COOKED)
    set(NC_COMMANDS)
    set(NC_DEPENDS)
    set(NC_MISSING)
    set(NC_ARGS ${ARGN})
    list(LENGTH NC_ARGS NC_COUNT)
    set(NC_INDEX 0)
    while(NC_INDEX LESS NC_COUNT)
        list(GET NC_ARGS ${NC_INDEX} NC_VARIANT)
        math(EXPR NC_INDEX "${NC_INDEX} + 1")
        list(GET NC_ARGS ${NC_INDEX} NC_DEST)
        math(EXPR NC_INDEX "${NC_INDEX} + 1")

        set(NC_STAMP "${NC_COOKED}/${NC_VARIANT}.stamp")
        # A configure dependency even while missing: the cook that makes it re-configures.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NC_STAMP}")
        if(NOT EXISTS "${NC_STAMP}")
            list(APPEND NC_MISSING ${NC_VARIANT})
            continue()
        endif()
        list(APPEND NC_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_COOKED}/${NC_VARIANT}" "${NC_DEST}")
        list(APPEND NC_DEPENDS "${NC_STAMP}")
    endwhile()
    if(NC_MISSING)
        string(REPLACE ";" ", " NC_MISSING "${NC_MISSING}")
        message(WARNING "${NC_WHAT}: shaders not cooked (${NC_MISSING}; no stamp in ${NC_COOKED}/), so not staged. "
                        "Cook them with NimbleCook (Engine/Tools/NimbleCook): "
                        "NimbleCook shaders [-project <dir>] [-editor] [-shipping] -- or the editor's Build > Cook Content.")
    endif()
    set(${NC_COMMANDS_VAR} ${NC_COMMANDS} PARENT_SCOPE)
    set(${NC_DEPENDS_VAR} ${NC_DEPENDS} PARENT_SCOPE)
endfunction()

function(nimble_stage_materials)
    cmake_parse_arguments(NM "" "TARGET;DIR;ROOT;MODULE;RUNTIME_DIR" "" ${ARGN})

    if(NOT NM_TARGET OR NOT NM_DIR)
        message(FATAL_ERROR "nimble_stage_materials(TARGET <game> DIR <folder> [ROOT <dir>] [MODULE <name>])")
    endif()
    if(NOT NM_ROOT)
        get_filename_component(NM_ROOT "${NM_DIR}" DIRECTORY)
    endif()

    file(GLOB_RECURSE NM_GRAPHS CONFIGURE_DEPENDS "${NM_DIR}/*.material.nmgraph")
    if(NOT NM_GRAPHS OR (NOT NM_MODULE AND NOT NIMBLE_TARGET STREQUAL "Editor"))
        return()
    endif()

    # With a known runtime directory, stage before the game builds; otherwise
    # (multi-config) after it, since $<TARGET_FILE_DIR> adds a dependency.
    # Packaged: into the game pack's folder (NimblePack.cmake).
    if(NIMBLE_PACKAGE_CONTENT)
        nimble_pack_stage_dir(Game NM_RUNTIME)
        set(NM_RUNTIME_DIR "${NM_RUNTIME}")
    elseif(NM_RUNTIME_DIR)
        set(NM_RUNTIME "${NM_RUNTIME_DIR}")
    else()
        set(NM_RUNTIME "$<TARGET_FILE_DIR:${NM_TARGET}>")
    endif()

    set(NM_VARIANTS)
    if(NM_MODULE)
        list(APPEND NM_VARIANTS Runtime "${NM_RUNTIME}")
        if(NIMBLE_PACKAGE_CONTENT AND CMAKE_BUILD_TYPE STREQUAL "Shipping")
            list(APPEND NM_VARIANTS Shipping "${NM_RUNTIME}")
        endif()
    endif()
    if(NIMBLE_TARGET STREQUAL "Editor")   # a cache variable: visible from a standalone game configure too
        list(APPEND NM_VARIANTS Editor "${NM_RUNTIME}")
    endif()
    nimble_cooked_shader_commands(NM_COMMANDS NM_DEPENDS "${NM_TARGET}" "${NM_ROOT}/Saved/Cooked/Shaders" ${NM_VARIANTS})
    if(NOT NM_COMMANDS)
        return()
    endif()

    set(NM_STAMP "${CMAKE_CURRENT_BINARY_DIR}/${NM_TARGET}_Materials.stamp")
    add_custom_command(
            OUTPUT  "${NM_STAMP}"
            ${NM_COMMANDS}
            COMMAND ${CMAKE_COMMAND} -E touch "${NM_STAMP}"
            DEPENDS ${NM_DEPENDS}
            COMMENT "${NM_TARGET}: staging the materials NimbleCook cooked from ${NM_DIR}"
            VERBATIM)

    if(NIMBLE_PACKAGE_CONTENT)
        add_custom_target(${NM_TARGET}StageMaterials DEPENDS "${NM_STAMP}")
        nimble_pack_add_dir(Game "Shaders" "${NM_RUNTIME}/Shaders" "${NM_STAMP}" ${NM_TARGET}StageMaterials)
    elseif(NM_RUNTIME_DIR)
        add_custom_target(${NM_TARGET}StageMaterials DEPENDS "${NM_STAMP}")
        add_dependencies(${NM_TARGET} ${NM_TARGET}StageMaterials)
    else()
        add_custom_target(${NM_TARGET}StageMaterials ALL DEPENDS "${NM_STAMP}")
        add_dependencies(${NM_TARGET}StageMaterials ${NM_TARGET})
    endif()
    set_target_properties(${NM_TARGET}StageMaterials PROPERTIES FOLDER "Games")
endfunction()

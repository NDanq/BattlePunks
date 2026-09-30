# ===========================================================================
# nimble_stage_materials(TARGET <game> DIR <folder> [RUNTIME_DIR <dir>])
#
# A game's MATERIALS, ready for its runtime directory: every <Name>.fsh and
# <Name>.material a material graph generated into <folder> (the editor writes
# them beside Glow.material.nmgraph and they are committed), laid out where
# RenderCore's material loader reads them --
#
#     <runtime>/Materials/<Name>.material
#     <runtime>/Shaders/Cooked/<Platform>/Materials/<Name>.fsh[.spv|.cso]
#
# -- for every backend GDI is built with (GDI_WITH_OPENGL / VULKAN / D3D12).
# The shaders go through NodeGraph's cook_material_graphs.py, which reuses
# GDI's own cook_shaders.py, so the toolchain is the one GDI's shaders already
# need (Python, the Vulkan SDK's glslc and spirv-cross, fxc).
#
# Works in every target type: nothing here needs the editor or the NodeGraph
# module, only the committed generated files and the script beside it.
#
# GLOBBED, unlike Sandbox's scripts, and on purpose: the files are generated
# output, one pair per graph, and the editor adds new ones -- listing them by
# hand would only be a second place to forget. CONFIGURE_DEPENDS re-globs when
# one appears. Rebuilt only when a source changes (a stamp file).
# ===========================================================================

# CACHE, not a plain variable: the function runs in the GAME's directory scope,
# which never sees a variable set in Engine's. (CMAKE_CURRENT_FUNCTION_LIST_DIR
# would say it directly, but that is CMake 3.17 and this project targets 3.16.)
get_filename_component(NM_COOK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Modules/NodeGraph/Scripts/cook_material_graphs.py" ABSOLUTE)
set(NIMBLE_MATERIAL_COOK_SCRIPT "${NM_COOK_SCRIPT}" CACHE INTERNAL "NodeGraph's material cook, for nimble_stage_materials")
get_filename_component(NM_PLATFORM_HEADER "${CMAKE_CURRENT_LIST_DIR}/../Modules/NodeGraph/Shaders/MaterialPlatform.glsl" ABSOLUTE)
set(NIMBLE_MATERIAL_PLATFORM_HEADER "${NM_PLATFORM_HEADER}" CACHE INTERNAL "NodeGraph's MaterialPlatform.glsl, which cooked materials may include")

function(nimble_stage_materials)
    cmake_parse_arguments(NM "" "TARGET;DIR;RUNTIME_DIR" "" ${ARGN})

    if(NOT NM_TARGET OR NOT NM_DIR)
        message(FATAL_ERROR "nimble_stage_materials(TARGET <game> DIR <folder>)")
    endif()

    file(GLOB NM_SHADERS   CONFIGURE_DEPENDS "${NM_DIR}/*.fsh")
    file(GLOB NM_MATERIALS CONFIGURE_DEPENDS "${NM_DIR}/*.material")
    if(NOT NM_SHADERS)
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_EXECUTABLE)
        message(STATUS "${NM_TARGET}: Python not found -- its materials are not cooked, and draw as plain tints")
        return()
    endif()

    # A known runtime directory stages BEFORE the game builds (a Play right
    # after a build finds them); without one -- a multi-config generator,
    # where it is only a generator expression -- the staging runs AFTER the
    # game, since naming $<TARGET_FILE_DIR> of a target makes a dependency on
    # it. The same split Games/Sandbox's own Scripts/ staging makes.
    if(NM_RUNTIME_DIR)
        set(NM_RUNTIME "${NM_RUNTIME_DIR}")
    else()
        set(NM_RUNTIME "$<TARGET_FILE_DIR:${NM_TARGET}>")
    endif()
    set(NM_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "${NM_RUNTIME}/Materials")
    foreach(NM_MATERIAL ${NM_MATERIALS})
        list(APPEND NM_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NM_MATERIAL}" "${NM_RUNTIME}/Materials/")
    endforeach()

    foreach(NM_PLATFORM OpenGL Vulkan D3D12)
        string(TOUPPER "${NM_PLATFORM}" NM_PLATFORM_UPPER)
        if(GDI_WITH_${NM_PLATFORM_UPPER})
            list(APPEND NM_COMMANDS
                    COMMAND "${Python3_EXECUTABLE}" "${NIMBLE_MATERIAL_COOK_SCRIPT}"
                            -target ${NM_PLATFORM} -src "${NM_DIR}"
                            -out "${NM_RUNTIME}/Shaders/Cooked" -subdir Materials)
        endif()
    endforeach()

    set(NM_STAMP "${CMAKE_CURRENT_BINARY_DIR}/${NM_TARGET}_Materials.stamp")
    add_custom_command(
            OUTPUT  "${NM_STAMP}"
            ${NM_COMMANDS}
            COMMAND ${CMAKE_COMMAND} -E touch "${NM_STAMP}"
            DEPENDS ${NM_SHADERS} ${NM_MATERIALS} "${NIMBLE_MATERIAL_COOK_SCRIPT}" "${NIMBLE_MATERIAL_PLATFORM_HEADER}"
            COMMENT "${NM_TARGET}: cooking and staging materials from ${NM_DIR}"
            VERBATIM)

    if(NM_RUNTIME_DIR)
        add_custom_target(${NM_TARGET}StageMaterials DEPENDS "${NM_STAMP}")
        add_dependencies(${NM_TARGET} ${NM_TARGET}StageMaterials)
    else()
        add_custom_target(${NM_TARGET}StageMaterials ALL DEPENDS "${NM_STAMP}")
        add_dependencies(${NM_TARGET}StageMaterials ${NM_TARGET})
    endif()
    set_target_properties(${NM_TARGET}StageMaterials PROPERTIES FOLDER "Games")
endfunction()

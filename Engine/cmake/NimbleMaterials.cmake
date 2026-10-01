# ===========================================================================
# nimble_stage_materials(TARGET <game> DIR <folder> [RUNTIME_DIR <dir>])
#
# Cooks and stages the committed <Name>.fsh / <Name>.material pairs in
# <folder> for every built GDI backend:
#
#     <runtime>/Materials/<Name>.material
#     <runtime>/Shaders/Cooked/<Platform>/Materials/<Name>.fsh[.spv|.cso]
#
# Uses cook_material_graphs.py (GDI's toolchain). Works in every target type.
# Globbed (generated files) with CONFIGURE_DEPENDS; stamp-file incremental.
# ===========================================================================

# CACHE: the function runs in the game's scope. (CMAKE_CURRENT_FUNCTION_LIST_DIR
# needs CMake 3.17.)
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

    # With a known runtime directory, stage before the game builds; otherwise
    # (multi-config) after it, since $<TARGET_FILE_DIR> adds a dependency.
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

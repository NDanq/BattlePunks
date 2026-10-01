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
#
# nimble_shader_cook_commands(<out-var> SRC <file-or-dir> OUT <dir> [SUBDIR <name>])
#
# The COMMAND list that cooks SRC into OUT/<Platform>/[SUBDIR/] for every
# built GDI backend. Shared by the above and the engine's own shader staging
# (Engine/CMakeLists.txt). Includes resolve against Engine/Content/Shaders.
# ===========================================================================

# CACHE: the functions run in the caller's scope. (CMAKE_CURRENT_FUNCTION_LIST_DIR
# needs CMake 3.17.)
get_filename_component(NM_COOK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Modules/NodeGraph/Scripts/cook_material_graphs.py" ABSOLUTE)
set(NIMBLE_MATERIAL_COOK_SCRIPT "${NM_COOK_SCRIPT}" CACHE INTERNAL "NodeGraph's material cook, for nimble_stage_materials")
get_filename_component(NM_ENGINE_SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/../Content/Shaders" ABSOLUTE)
set(NIMBLE_ENGINE_SHADER_DIR "${NM_ENGINE_SHADER_DIR}" CACHE INTERNAL "Engine/Content/Shaders: the engine's shaders, and the include root of every cook")
file(GLOB NM_SHADER_INCLUDES CONFIGURE_DEPENDS "${NM_ENGINE_SHADER_DIR}/*.glsl")
set(NIMBLE_SHADER_INCLUDES "${NM_SHADER_INCLUDES}" CACHE INTERNAL "Engine/Content/Shaders/*.glsl; a change recooks everything that includes them")

function(nimble_shader_cook_commands NM_OUT_VAR)
    cmake_parse_arguments(NC "" "SRC;OUT;SUBDIR" "" ${ARGN})

    set(NC_SUBDIR_ARGS)
    if(NC_SUBDIR)
        set(NC_SUBDIR_ARGS -subdir ${NC_SUBDIR})
    endif()

    # Metal is not cooked: cook_material_graphs.py has no Metal target, and on
    # Apple GDI_WITH_VULKAN is forced off, which GDI's own Metal cook needed.
    set(NC_COMMANDS)
    foreach(NC_PLATFORM OpenGL Vulkan D3D12)
        string(TOUPPER "${NC_PLATFORM}" NC_PLATFORM_UPPER)
        if(GDI_WITH_${NC_PLATFORM_UPPER})
            list(APPEND NC_COMMANDS
                    COMMAND "${Python3_EXECUTABLE}" "${NIMBLE_MATERIAL_COOK_SCRIPT}"
                            -target ${NC_PLATFORM} -src "${NC_SRC}"
                            -out "${NC_OUT}" ${NC_SUBDIR_ARGS})
        endif()
    endforeach()
    set(${NM_OUT_VAR} ${NC_COMMANDS} PARENT_SCOPE)
endfunction()

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

    nimble_shader_cook_commands(NM_COOK_COMMANDS SRC "${NM_DIR}" OUT "${NM_RUNTIME}/Shaders/Cooked" SUBDIR Materials)
    list(APPEND NM_COMMANDS ${NM_COOK_COMMANDS})

    set(NM_STAMP "${CMAKE_CURRENT_BINARY_DIR}/${NM_TARGET}_Materials.stamp")
    add_custom_command(
            OUTPUT  "${NM_STAMP}"
            ${NM_COMMANDS}
            COMMAND ${CMAKE_COMMAND} -E touch "${NM_STAMP}"
            DEPENDS ${NM_SHADERS} ${NM_MATERIALS} "${NIMBLE_MATERIAL_COOK_SCRIPT}" ${NIMBLE_SHADER_INCLUDES}
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

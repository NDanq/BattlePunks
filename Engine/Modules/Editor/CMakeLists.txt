# ===========================================================================
# NimbleEditor -- the Qt Widgets editor. An executable in the flat Binaries
# directory, beside the engine DLLs.
#
#   GameCore, RenderCore   the World and the Renderer.
#   EngineCore::QtBridge   value conversions. Qt-only, no GDI.
#   NodeGraph              material and script graphs.
#   NimbleEngine           GameSession / GameModuleHost for Play-in-Editor
#                          (not Application: Qt owns the main loop).
#
#   NOT GDI or GDI::QtBridge: GDI is static and must live only in
#   RenderCore.dll. The viewport passes QWidget::winId() instead.
#   NOT the game: loaded by name at Play.
#
# Editor targets only; sources are also wrapped in `#if WITH_EDITOR`.
# ===========================================================================

find_package(Qt6 6.8 COMPONENTS Core Gui Widgets QUIET)

if(NOT Qt6Widgets_FOUND)
    message(STATUS "NimbleEditor: Qt6 Widgets not found; skipping the editor")
    return()
endif()

set(NIMBLE_EDITOR_SOURCES
        Source/Private/Editor/EditorMain.cpp
        Source/Private/Editor/AboutDialog.cpp
        Source/Private/Editor/AboutDialog.h
        Source/Private/Editor/EditorContext.cpp
        Source/Private/Editor/EditorHistory.cpp
        Source/Private/Editor/EditorHistory.h
        Source/Private/Editor/EditorCamera.cpp
        Source/Private/Editor/EditorLog.cpp
        Source/Private/Editor/EditorConsole.cpp
        Source/Private/Editor/ConsolePanel.cpp
        Source/Private/Editor/EditorMainWindow.cpp
        Source/Private/Editor/ViewportPanel.cpp
        Source/Private/Editor/ViewportWidget.cpp
        Source/Private/Editor/TransformGizmo.cpp
        Source/Private/Editor/OutlinerPanel.cpp
        Source/Private/Editor/DetailsPanel.cpp
        Source/Private/Editor/PropertyWidgets.cpp
        Source/Private/Editor/PropertyWidgets.h
        Source/Private/Editor/SDLInputProbe.cpp
        Source/Private/Editor/ViewportSDLInput.cpp
        Source/Private/Editor/ViewportSDLInput.h
        Source/Private/Editor/PlaySession.cpp
        Source/Private/Editor/PlaySession.h
        Source/Private/Editor/SDLInputProbe.h
        Source/Private/Editor/EditorTheme.cpp
        Source/Private/Editor/EditorTheme.h
        Source/Private/Editor/ConfigIniFile.cpp
        Source/Private/Editor/ConfigIniFile.h
        Source/Private/Editor/ProjectSettingsDialog.cpp
        Source/Private/Editor/ProjectSettingsDialog.h
        Source/Private/Editor/SettingsDialog.cpp
        Source/Private/Editor/SettingsDialog.h
        Source/Private/Editor/SourceBuild.cpp
        Source/Private/Editor/SourceBuild.h
        Source/Private/Editor/Graph/GraphAssets.cpp
        Source/Private/Editor/Graph/GraphAssets.h
        Source/Private/Editor/Graph/GraphDetailsPanel.cpp
        Source/Private/Editor/Graph/GraphDetailsPanel.h
        Source/Private/Editor/Graph/GraphDocument.cpp
        Source/Private/Editor/Graph/GraphDocument.h
        Source/Private/Editor/Graph/GraphEditorTab.cpp
        Source/Private/Editor/Graph/GraphEditorTab.h
        Source/Private/Editor/Graph/GraphEditorWindow.cpp
        Source/Private/Editor/Graph/GraphEditorWindow.h
        Source/Private/Editor/Graph/GraphItems.cpp
        Source/Private/Editor/Graph/GraphItems.h
        Source/Private/Editor/Graph/GraphScene.cpp
        Source/Private/Editor/Graph/GraphScene.h
        Source/Private/Editor/Graph/GraphView.cpp
        Source/Private/Editor/Graph/GraphView.h
        Source/Private/Editor/Graph/NodePalette.cpp
        Source/Private/Editor/Graph/NodePalette.h
        Source/Public/Editor/EditorContext.h
        Source/Public/Editor/EditorCamera.h
        Source/Public/Editor/EditorLog.h
        Source/Public/Editor/EditorConsole.h
        Source/Public/Editor/ConsolePanel.h
        Source/Public/Editor/EditorMainWindow.h
        Source/Public/Editor/ViewportPanel.h
        Source/Public/Editor/ViewportWidget.h
        Source/Public/Editor/TransformGizmo.h
        Source/Public/Editor/OutlinerPanel.h
        Source/Public/Editor/DetailsPanel.h
        Source/Private/Editor/ContentBrowserPanel.cpp
        Source/Public/Editor/ContentBrowserPanel.h
        Source/Private/Editor/Content/ContentPaths.cpp
        Source/Private/Editor/Content/ContentPaths.h
        Source/Private/Editor/Content/MeshPreviewView.cpp
        Source/Private/Editor/Content/MeshPreviewView.h
        Source/Private/Editor/Content/MeshViewerWindow.cpp
        Source/Private/Editor/Content/MeshViewerWindow.h
        Source/Private/Editor/Content/ImportWizard.cpp
        Source/Private/Editor/Content/ImportWizard.h
)

# glTF / image importers. Widget-free, so the import tests build them alone.
set(NIMBLE_EDITOR_IMPORT_SOURCES
        Source/Private/Editor/Content/AssetImport.cpp
        Source/Private/Editor/Content/AssetImport.h
        Source/Private/Editor/Content/GltfImporter.cpp
        Source/Private/Editor/Content/GltfImporter.h
)
list(APPEND NIMBLE_EDITOR_SOURCES ${NIMBLE_EDITOR_IMPORT_SOURCES})

# cgltf (MIT), vendored; implemented in GltfImporter.cpp.
set(NIMBLE_EDITOR_CGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/cgltf-1.15")

add_executable(NimbleEditor WIN32 ${NIMBLE_EDITOR_SOURCES})

# The only engine target with AUTOMOC on.
set_target_properties(NimbleEditor PROPERTIES
        AUTOMOC ON
        AUTOUIC OFF
        AUTORCC OFF
        FOLDER  "Editor"
)

target_include_directories(NimbleEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")

target_link_libraries(NimbleEditor PRIVATE
        GameCore::GameCore
        RenderCore::RenderCore
        GameWindow::GameWindow
        NimbleEngine::NimbleEngine
        EngineCore::QtBridge
        NodeGraph::NodeGraph
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

# === Import tests =================================================================
# Runs from Binaries, where NimbleEditor's windeployqt put Qt (hence the dependency).
option(NIMBLEEDITOR_BUILD_TESTS "Build NimbleEditorImportTests" ON)
if(NIMBLEEDITOR_BUILD_TESTS)
    add_executable(NimbleEditorImportTests Tests/ImportTests.cpp ${NIMBLE_EDITOR_IMPORT_SOURCES})
    target_include_directories(NimbleEditorImportTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorImportTests PRIVATE
            RenderCore::RenderCore
            EngineCore::QtBridge
            Qt6::Core
            Qt6::Gui
    )
    set_target_properties(NimbleEditorImportTests PROPERTIES FOLDER "Editor")
    add_dependencies(NimbleEditorImportTests NimbleEditor)
    add_test(NAME Editor.Import COMMAND NimbleEditorImportTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorImportTests>")
endif()

# === Engine content ===========================================================
# The Content Browser shows the engine's own content (Engine/Content) beside
# the project's, from the source tree this editor was built from; without it,
# from the copy staged beside the executable (ContentPaths.cpp).
get_filename_component(NM_EDITOR_ENGINE_CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/../../Content" ABSOLUTE)
set_source_files_properties(Source/Private/Editor/Content/ContentPaths.cpp PROPERTIES
        COMPILE_DEFINITIONS "NIMBLEEDITOR_ENGINE_CONTENT_DIR=\"${NM_EDITOR_ENGINE_CONTENT}\"")

# === Node graphs ================================================================
# Shader check on save via NodeGraph's cook_material_graphs.py; skipped if
# either path is empty.
set_source_files_properties(Source/Private/Editor/Graph/GraphAssets.cpp PROPERTIES
        COMPILE_DEFINITIONS "NIMBLEEDITOR_PYTHON=\"${NODEGRAPH_PYTHON}\";NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT=\"${NODEGRAPH_COOK_SCRIPT}\"")

# === Build version stamp ======================================================
# For Help > About, via Core's GitVersion.cmake (included by path). The header
# is rewritten only when a value changes; only AboutDialog.cpp includes it.
include("${CMAKE_CURRENT_SOURCE_DIR}/../Core/cmake/GitVersion.cmake")
git_version_stamp(
        PREFIX   NIMBLEEDITOR
        VERSION  "${NimbleEngine_VERSION}"
        TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Private/Editor/EditorBuildInfo.h.in"
        OUTPUT   "${CMAKE_CURRENT_BINARY_DIR}/Generated/EditorBuildInfo.h"
        TARGET   NimbleEditor
)
target_include_directories(NimbleEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")

# Bare tokens (stringized), on one file so a config switch recompiles only it.
set_source_files_properties(Source/Private/Editor/AboutDialog.cpp PROPERTIES
        COMPILE_DEFINITIONS "NIMBLE_BUILD_CONFIG=$<CONFIG>;NIMBLE_BUILD_TARGET=${NIMBLE_TARGET}")

# === Build tree =================================================================
# For Build > Compile Source (`cmake --build` on this tree). With MSVC under
# Ninja, SourceBuild runs vcvarsall.bat when INCLUDE is unset; its arguments
# come from cl.exe's path (.../bin/Host<host>/<target>/cl.exe).
set(NM_EDITOR_VCVARSALL "")
set(NM_EDITOR_VCVARS_ARGS "")
if(MSVC AND CMAKE_CXX_COMPILER MATCHES "^(.*)/VC/Tools/MSVC/([^/]+)/bin/Host([^/]+)/([^/]+)/cl\\.exe$")
    set(NM_EDITOR_VS_ROOT       "${CMAKE_MATCH_1}")
    set(NM_EDITOR_MSVC_VERSION  "${CMAKE_MATCH_2}")
    string(TOLOWER "${CMAKE_MATCH_3}" NM_EDITOR_VC_HOST)
    string(TOLOWER "${CMAKE_MATCH_4}" NM_EDITOR_VC_TARGET)
    # vcvarsall spells x64 "amd64".
    string(REPLACE "x64" "amd64" NM_EDITOR_VC_HOST   "${NM_EDITOR_VC_HOST}")
    string(REPLACE "x64" "amd64" NM_EDITOR_VC_TARGET "${NM_EDITOR_VC_TARGET}")
    if(NM_EDITOR_VC_HOST STREQUAL NM_EDITOR_VC_TARGET)
        set(NM_EDITOR_VC_ARCH "${NM_EDITOR_VC_TARGET}")
    else()
        set(NM_EDITOR_VC_ARCH "${NM_EDITOR_VC_HOST}_${NM_EDITOR_VC_TARGET}")
    endif()
    if(EXISTS "${NM_EDITOR_VS_ROOT}/VC/Auxiliary/Build/vcvarsall.bat")
        set(NM_EDITOR_VCVARSALL   "${NM_EDITOR_VS_ROOT}/VC/Auxiliary/Build/vcvarsall.bat")
        set(NM_EDITOR_VCVARS_ARGS "${NM_EDITOR_VC_ARCH} -vcvars_ver=${NM_EDITOR_MSVC_VERSION}")
    endif()
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Source/Private/Editor/EditorBuildTree.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/Generated/EditorBuildTree.h" @ONLY)

# === The project ================================================================
# Edit > Project Settings edits the game's master Config/DefaultEngine.ini,
# not the staged copy. The game's CMakeLists sets:
#
#   set_property(TARGET NimbleEditor PROPERTY NIMBLE_PROJECT_CONFIG "<path>")
#
# Read by file(GENERATE), since the game's directory is configured later.
# Empty: the dialog edits the staged copy.
file(GENERATE
        OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/Generated/EditorProject.h"
        CONTENT "#pragma once
// Generated by the Editor's CMakeLists.txt (file(GENERATE)). DO NOT EDIT.
#define NIMBLEEDITOR_PROJECT_CONFIG \"$<TARGET_PROPERTY:NimbleEditor,NIMBLE_PROJECT_CONFIG>\"
")

set_source_files_properties(Source/Private/Editor/SourceBuild.cpp PROPERTIES
        COMPILE_DEFINITIONS "NIMBLE_BUILD_CONFIG=$<CONFIG>")

# The cooked shaders the Renderer loads at startup, staged by the engine.
if(TARGET NimbleStageShaders)
    add_dependencies(NimbleEditor NimbleStageShaders)
endif()

# Qt's DLLs beside the exe, so it runs without Qt on PATH.
if(WIN32)
    get_target_property(NM_QT_QMAKE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(NM_QT_BIN_DIR "${NM_QT_QMAKE}" DIRECTORY)
    find_program(NM_WINDEPLOYQT windeployqt HINTS "${NM_QT_BIN_DIR}" NO_DEFAULT_PATH)

    if(NM_WINDEPLOYQT)
        add_custom_command(TARGET NimbleEditor POST_BUILD
                COMMAND "${NM_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                        --no-opengl-sw --no-system-d3d-compiler
                        "$<TARGET_FILE:NimbleEditor>"
                COMMENT "NimbleEditor: deploying Qt beside the executable"
                VERBATIM)
    else()
        message(STATUS "NimbleEditor: windeployqt not found; add Qt's bin directory to PATH to run the editor")
    endif()

    set_target_properties(NimbleEditor PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditor>")
endif()

source_group("Editor" FILES ${NIMBLE_EDITOR_SOURCES})

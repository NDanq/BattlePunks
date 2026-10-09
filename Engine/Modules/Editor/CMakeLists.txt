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
        Source/Private/Editor/DocumentationWindow.cpp
        Source/Private/Editor/DocumentationWindow.h
        Source/Private/Editor/EditorContext.cpp
        Source/Private/Editor/EditorDocument.cpp
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
        Source/Private/Editor/EditorLights.cpp
        Source/Private/Editor/EditorLights.h
        Source/Private/Editor/EditorEditActions.cpp
        Source/Private/Editor/EditorEditActions.h
        Source/Private/Editor/EditorPrimitives.cpp
        Source/Private/Editor/EditorPrimitives.h
        Source/Private/Editor/DetailsPanel.cpp
        Source/Private/Editor/PropertyWidgets.cpp
        Source/Private/Editor/PropertyWidgets.h
        Source/Private/Editor/SDLInputProbe.cpp
        Source/Private/Editor/ViewportSDLInput.cpp
        Source/Private/Editor/ViewportSDLInput.h
        Source/Private/Editor/PlaySession.cpp
        Source/Private/Editor/PlayProcesses.cpp
        Source/Private/Editor/PlayProcesses.h
        Source/Private/Editor/PlaySession.h
        Source/Private/Editor/SDLInputProbe.h
        Source/Private/Editor/EditorTheme.cpp
        Source/Private/Editor/EditorTheme.h
        Source/Private/Editor/ConfigIniFile.cpp
        Source/Private/Editor/ConfigIniFile.h
        Source/Private/Editor/ProjectSettingsDialog.cpp
        Source/Private/Editor/InputMapsPage.cpp
        Source/Private/Editor/QtInputNames.cpp
        Source/Private/Editor/ProjectSettingsDialog.h
        Source/Private/Editor/InputMapsPage.h
        Source/Private/Editor/QtInputNames.h
        Source/Private/Editor/WorldSettingsDialog.cpp
        Source/Private/Editor/WorldSettingsDialog.h
        Source/Private/Editor/SettingsDialog.cpp
        Source/Private/Editor/SettingsDialog.h
        Source/Private/Editor/SourceBuild.cpp
        Source/Private/Editor/SourceBuild.h
        Source/Private/Editor/Build/ContentCook.cpp
        Source/Private/Editor/Build/ContentCook.h
        Source/Private/Editor/Build/ProjectBuild.cpp
        Source/Private/Editor/Build/ProjectBuild.h
        Source/Private/Editor/Build/ProjectBuildDialog.cpp
        Source/Private/Editor/Build/ProjectBuildDialog.h
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
        Source/Private/Editor/Particles/ParticleAssets.cpp
        Source/Private/Editor/Particles/ParticleAssets.h
        Source/Private/Editor/Particles/ParticleCurveWidgets.cpp
        Source/Private/Editor/Particles/ParticleCurveWidgets.h
        Source/Private/Editor/Particles/ParticleDocument.cpp
        Source/Private/Editor/Particles/ParticleDocument.h
        Source/Private/Editor/Particles/ParticleEditorTab.cpp
        Source/Private/Editor/Particles/ParticleEditorTab.h
        Source/Private/Editor/Particles/ParticleEditorWindow.cpp
        Source/Private/Editor/Particles/ParticleEditorWindow.h
        Source/Private/Editor/Particles/ParticleEmitterPanels.cpp
        Source/Private/Editor/Particles/ParticleEmitterPanels.h
        Source/Private/Editor/Prefab/PrefabAssets.cpp
        Source/Private/Editor/Prefab/PrefabAssets.h
        Source/Private/Editor/Prefab/PrefabEditorTab.cpp
        Source/Private/Editor/Prefab/PrefabEditorTab.h
        Source/Private/Editor/Prefab/PrefabEditorWindow.cpp
        Source/Private/Editor/Prefab/PrefabEditorWindow.h
        Source/Private/Editor/Prefab/PrefabScriptPanel.cpp
        Source/Private/Editor/Prefab/PrefabScriptPanel.h
        Source/Private/Editor/UI/UIAssets.cpp
        Source/Private/Editor/UI/UIAssets.h
        Source/Private/Editor/UI/UIDesignDocument.cpp
        Source/Private/Editor/UI/UIDesignDocument.h
        Source/Private/Editor/UI/UIDesignerViewport.cpp
        Source/Private/Editor/UI/UIDesignerViewport.h
        Source/Private/Editor/UI/UIDetailsPanel.cpp
        Source/Private/Editor/UI/UIGraphLink.cpp
        Source/Private/Editor/UI/UIPreviewRuntime.cpp
        Source/Private/Editor/UI/UIPreviewRuntime.h
        Source/Private/Editor/UI/UIGraphLink.h
        Source/Private/Editor/UI/UIDetailsPanel.h
        Source/Private/Editor/UI/UIEditorTab.cpp
        Source/Private/Editor/UI/UIEditorTab.h
        Source/Private/Editor/UI/UIEditorWindow.cpp
        Source/Private/Editor/UI/UIEditorWindow.h
        Source/Private/Editor/UI/UIHierarchyPanel.cpp
        Source/Private/Editor/UI/UIHierarchyPanel.h
        Source/Private/Editor/UI/UIPalettePanel.cpp
        Source/Private/Editor/UI/UIPalettePanel.h
        Source/Private/Editor/Graph/GraphItems.cpp
        Source/Private/Editor/Graph/GraphItems.h
        Source/Private/Editor/Graph/GraphScene.cpp
        Source/Private/Editor/Graph/GraphScene.h
        Source/Private/Editor/Graph/GraphView.cpp
        Source/Private/Editor/Graph/GraphView.h
        Source/Private/Editor/Graph/NodePalette.cpp
        Source/Private/Editor/Graph/NodePalette.h
        Source/Public/Editor/EditorContext.h
        Source/Public/Editor/EditorDocument.h
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
        Source/Private/Editor/Content/RenderTargetAssets.cpp
        Source/Private/Editor/Content/RenderTargetAssets.h
        Source/Private/Editor/Content/ContentPaths.h
        Source/Private/Editor/Content/AnimationAssets.cpp
        Source/Private/Editor/Content/AnimationAssets.h
        Source/Private/Editor/Content/AnimPreviewWidget.cpp
        Source/Private/Editor/Content/AnimPreviewWidget.h
        Source/Private/Editor/Content/AnimStateEditorWindow.cpp
        Source/Private/Editor/Content/AnimStateEditorWindow.h
        Source/Private/Editor/Content/AnimStateGraph.cpp
        Source/Private/Editor/Content/AnimStateGraph.h
        Source/Private/Editor/Content/ClipTimeline.cpp
        Source/Private/Editor/Content/ClipTimeline.h
        Source/Private/Editor/Content/MeshPreviewView.cpp
        Source/Private/Editor/Content/MeshPreviewView.h
        Source/Private/Editor/Content/MeshViewerWindow.cpp
        Source/Private/Editor/Content/MeshViewerWindow.h
        Source/Private/Editor/Content/TextureViewerWindow.cpp
        Source/Private/Editor/Content/TextureViewerWindow.h
        Source/Private/Editor/Content/ImportWizard.cpp
        Source/Private/Editor/Content/ImportWizard.h
        Source/Private/Editor/Project/NewProjectWizard.cpp
        Source/Private/Editor/Project/NewProjectWizard.h
)

# File > New Project's generator. Widget-free, so the project tests build it alone.
set(NIMBLE_EDITOR_PROJECT_SOURCES
        Source/Private/Editor/Project/ProjectTemplate.cpp
        Source/Private/Editor/Project/ProjectTemplate.h
)
list(APPEND NIMBLE_EDITOR_SOURCES ${NIMBLE_EDITOR_PROJECT_SOURCES})

# glTF / image importers. Widget-free, so the import tests build them alone.
set(NIMBLE_EDITOR_IMPORT_SOURCES
        Source/Private/Editor/Content/AssetImport.cpp
        Source/Private/Editor/Content/AssetImport.h
        Source/Private/Editor/Content/GltfImporter.cpp
        Source/Private/Editor/Content/GltfImporter.h
        Source/Private/Editor/Content/LightmapUVs.cpp
        Source/Private/Editor/Content/LightmapUVs.h
)
list(APPEND NIMBLE_EDITOR_SOURCES ${NIMBLE_EDITOR_IMPORT_SOURCES})

# The light baker (Build > Bake Lightmaps). Widget-free but BakeLightmaps.cpp,
# so the lighting tests build them alone. Optimised even in Debug: a bake
# traces hundreds of millions of rays.
set(NIMBLE_EDITOR_LIGHTING_SOURCES
        Source/Private/Editor/Lighting/LightBakeBVH.cpp
        Source/Private/Editor/Lighting/LightBakeBVH.h
        Source/Private/Editor/Lighting/LightBaker.cpp
        Source/Private/Editor/Lighting/LightBaker.h
        Source/Private/Editor/Lighting/LightBakeScene.cpp
        Source/Private/Editor/Lighting/LightBakeScene.h
)
list(APPEND NIMBLE_EDITOR_SOURCES ${NIMBLE_EDITOR_LIGHTING_SOURCES}
        Source/Private/Editor/Lighting/LightBakeWorld.cpp
        Source/Private/Editor/Lighting/LightBakeWorld.h
        Source/Private/Editor/Lighting/BakeLightmaps.cpp
        Source/Private/Editor/Lighting/BakeLightmaps.h
)
# STUB(NetCore-Editor): the editor link (EditorNetSession.h), when NetCore is built.
if(TARGET NetCore)
    list(APPEND NIMBLE_EDITOR_SOURCES
            Source/Private/Editor/EditorNetSession.cpp
            Source/Private/Editor/EditorNetSession.h)
endif()

if(NOT MSVC)
    set_source_files_properties(
            Source/Private/Editor/Lighting/LightBakeBVH.cpp
            Source/Private/Editor/Lighting/LightBaker.cpp
            Source/Private/Editor/Lighting/LightBakeScene.cpp
            PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# cgltf (MIT), vendored; implemented in GltfImporter.cpp.
set(NIMBLE_EDITOR_CGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/cgltf-1.15")

# xatlas (MIT), vendored at jpcy/xatlas f700c77: lightmap UV charts (LightmapUVs.cpp).
# Its own library, built without the engine's warnings.
set(NIMBLE_EDITOR_XATLAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/xatlas-f700c77")
add_library(NimbleXatlas STATIC "${NIMBLE_EDITOR_XATLAS_DIR}/xatlas.cpp")
target_include_directories(NimbleXatlas PUBLIC "${NIMBLE_EDITOR_XATLAS_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(NimbleXatlas PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(NimbleXatlas PRIVATE /w)
else()
    target_compile_options(NimbleXatlas PRIVATE -w)
endif()
set_target_properties(NimbleXatlas PROPERTIES FOLDER "Editor/External" POSITION_INDEPENDENT_CODE ON)

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
        NimbleXatlas
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
if(TARGET NetCore)
    target_link_libraries(NimbleEditor PRIVATE NetCore::NetCore)
endif()
# Sound import and preview (Content/AssetImport.cpp), when the module is built.
if(TARGET Audio)
    target_link_libraries(NimbleEditor PRIVATE Audio::Audio)
endif()

# === Import tests =================================================================
# Runs from Binaries, where NimbleEditor's windeployqt put Qt (hence the dependency).
option(NIMBLEEDITOR_BUILD_TESTS "Build NimbleEditorImportTests, NimbleEditorDocumentTests, NimbleEditorDetailsTests, NimbleEditorUIDesignerTests and NimbleEditorParticleTests" ON)
if(NIMBLEEDITOR_BUILD_TESTS)
    add_executable(NimbleEditorImportTests Tests/ImportTests.cpp ${NIMBLE_EDITOR_IMPORT_SOURCES})
    target_include_directories(NimbleEditorImportTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorImportTests PRIVATE
            RenderCore::RenderCore
            EngineCore::QtBridge
            NimbleXatlas
            Qt6::Core
            Qt6::Gui
    )
    set_target_properties(NimbleEditorImportTests PROPERTIES FOLDER "Editor")
    add_dependencies(NimbleEditorImportTests NimbleEditor)
    add_test(NAME Editor.Import COMMAND NimbleEditorImportTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorImportTests>")

    # The light baker, headless: no World, no Qt.
    add_executable(NimbleEditorLightingTests Tests/LightingTests.cpp ${NIMBLE_EDITOR_LIGHTING_SOURCES})
    target_include_directories(NimbleEditorLightingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(NimbleEditorLightingTests PRIVATE RenderCore::RenderCore)
    if(NOT MSVC)
        set_source_files_properties(Tests/LightingTests.cpp PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
    set_target_properties(NimbleEditorLightingTests PROPERTIES FOLDER "Editor")
    add_dependencies(NimbleEditorLightingTests NimbleEditor)
    add_test(NAME Editor.Lighting COMMAND NimbleEditorLightingTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorLightingTests>")

    # EditorDocument and its history, headless: no Renderer, no window.
    add_executable(NimbleEditorDocumentTests Tests/DocumentTests.cpp
            Source/Public/Editor/EditorDocument.h
            Source/Private/Editor/EditorDocument.cpp
            Source/Private/Editor/EditorHistory.h
            Source/Private/Editor/EditorHistory.cpp)
    target_include_directories(NimbleEditorDocumentTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(NimbleEditorDocumentTests PRIVATE
            RenderCore::RenderCore
            NimbleEngine::NimbleEngine   # WarnInactiveModuleComponents on world open
            Qt6::Core
            Qt6::Gui
    )
    set_target_properties(NimbleEditorDocumentTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorDocumentTests NimbleEditor)
    add_test(NAME Editor.Document COMMAND NimbleEditorDocumentTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorDocumentTests>")

    # File > New Project from the real template, into a temporary folder.
    # And Project Settings' world list, as ConfigIniFile writes it.
    add_executable(NimbleEditorProjectTests Tests/ProjectTests.cpp ${NIMBLE_EDITOR_PROJECT_SOURCES}
            Source/Private/Editor/ConfigIniFile.cpp Source/Private/Editor/ConfigIniFile.h)
    target_include_directories(NimbleEditorProjectTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(NimbleEditorProjectTests PRIVATE EngineCore::EngineCore Qt6::Core)
    set_target_properties(NimbleEditorProjectTests PROPERTIES FOLDER "Editor")
    # And Engine/Binaries/NewProject.cmake, the same without the editor: its
    # output must match file for file.
    get_filename_component(NM_EDITOR_NEW_PROJECT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../Binaries/NewProject.cmake" ABSOLUTE)
    target_compile_definitions(NimbleEditorProjectTests PRIVATE
            "NIMBLEEDITOR_CMAKE_COMMAND=\"${CMAKE_COMMAND}\""
            "NIMBLEEDITOR_NEW_PROJECT_SCRIPT=\"${NM_EDITOR_NEW_PROJECT_SCRIPT}\"")
    add_dependencies(NimbleEditorProjectTests NimbleEditor)
    add_test(NAME Editor.Project COMMAND NimbleEditorProjectTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorProjectTests>")

    # The Details panel, offscreen: the editor's sources without its main().
    set(NIMBLE_EDITOR_DETAILS_SOURCES ${NIMBLE_EDITOR_SOURCES})
    list(REMOVE_ITEM NIMBLE_EDITOR_DETAILS_SOURCES Source/Private/Editor/EditorMain.cpp)
    add_executable(NimbleEditorDetailsTests Tests/DetailsTests.cpp ${NIMBLE_EDITOR_DETAILS_SOURCES})
    target_include_directories(NimbleEditorDetailsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorDetailsTests PRIVATE
            GameCore::GameCore
            RenderCore::RenderCore
            GameWindow::GameWindow
            NimbleEngine::NimbleEngine
            EngineCore::QtBridge
            NodeGraph::NodeGraph
            NimbleXatlas
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorDetailsTests PRIVATE NetCore::NetCore)
    endif()
    set_target_properties(NimbleEditorDetailsTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorDetailsTests NimbleEditor)
    add_test(NAME Editor.Details COMMAND NimbleEditorDetailsTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorDetailsTests>")

    # The UI Designer's document and panels, offscreen: the editor's sources without its main().
    add_executable(NimbleEditorUIDesignerTests Tests/UIDesignerTests.cpp ${NIMBLE_EDITOR_DETAILS_SOURCES})
    target_include_directories(NimbleEditorUIDesignerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorUIDesignerTests PRIVATE
            GameCore::GameCore
            RenderCore::RenderCore
            GameWindow::GameWindow
            NimbleEngine::NimbleEngine
            EngineCore::QtBridge
            NodeGraph::NodeGraph
            NimbleXatlas
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorUIDesignerTests PRIVATE NetCore::NetCore)
    endif()
    set_target_properties(NimbleEditorUIDesignerTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorUIDesignerTests NimbleEditor)
    add_test(NAME Editor.UIDesigner COMMAND NimbleEditorUIDesignerTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorUIDesignerTests>")

    # The Particle Editor's document and panels, offscreen: the editor's sources without its main().
    add_executable(NimbleEditorParticleTests Tests/ParticleEditorTests.cpp ${NIMBLE_EDITOR_DETAILS_SOURCES})
    target_include_directories(NimbleEditorParticleTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorParticleTests PRIVATE
            GameCore::GameCore
            RenderCore::RenderCore
            GameWindow::GameWindow
            NimbleEngine::NimbleEngine
            EngineCore::QtBridge
            NodeGraph::NodeGraph
            NimbleXatlas
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorParticleTests PRIVATE NetCore::NetCore)
    endif()
    set_target_properties(NimbleEditorParticleTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorParticleTests NimbleEditor)
    add_test(NAME Editor.Particles COMMAND NimbleEditorParticleTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorParticleTests>")

    # The tests compile the editor's own sources, sound import among them.
    if(TARGET Audio)
        foreach(NM_EDITOR_TEST IN ITEMS NimbleEditorImportTests NimbleEditorLightingTests NimbleEditorDocumentTests
                NimbleEditorProjectTests NimbleEditorDetailsTests NimbleEditorUIDesignerTests NimbleEditorParticleTests)
            if(TARGET ${NM_EDITOR_TEST})
                target_link_libraries(${NM_EDITOR_TEST} PRIVATE Audio::Audio)
            endif()
        endforeach()
    endif()
endif()

# === Engine content ===========================================================
# The Content Browser shows the engine's own content (Engine/Content) beside
# the project's, from the source tree this editor was built from; without it,
# from the copy staged beside the executable (ContentPaths.cpp).
get_filename_component(NM_EDITOR_ENGINE_CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/../../Content" ABSOLUTE)
set_source_files_properties(Source/Private/Editor/Content/ContentPaths.cpp PROPERTIES
        COMPILE_DEFINITIONS "NIMBLEEDITOR_ENGINE_CONTENT_DIR=\"${NM_EDITOR_ENGINE_CONTENT}\"")

# === Project templates ========================================================
# File > New Project copies Engine/Templates/Game from the source tree this
# editor was built from; without it, from a copy beside the executable.
get_filename_component(NM_EDITOR_TEMPLATES "${CMAKE_CURRENT_SOURCE_DIR}/../../Templates" ABSOLUTE)
set_property(SOURCE Source/Private/Editor/Project/ProjectTemplate.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS "NIMBLEEDITOR_TEMPLATES_DIR=\"${NM_EDITOR_TEMPLATES}\"")

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
// \"client:<module>:<target>;server:<module>:<target>;...\" -- nimble_add_game_launcher
// and nimble_add_server_launcher record them (Engine/cmake/NimbleModule.cmake).
#define NIMBLEEDITOR_GAME_EXECUTABLES \"$<JOIN:$<TARGET_PROPERTY:NimbleEditor,NIMBLE_GAME_EXECUTABLES>,;>\"
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

# The Details, UI Designer and Particle tests compile the editor's sources, so they need
# its generated headers too (build info, project config); made by
# NimbleEditor's build, which they depend on.
if(TARGET NimbleEditorDetailsTests)
    target_include_directories(NimbleEditorDetailsTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
endif()
if(TARGET NimbleEditorUIDesignerTests)
    target_include_directories(NimbleEditorUIDesignerTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
endif()
if(TARGET NimbleEditorParticleTests)
    target_include_directories(NimbleEditorParticleTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
endif()

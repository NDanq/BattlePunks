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

# Qt Svg draws the Lucide icons (EditorIcons.cpp). Optional: without it, the
# style's standard icons stand in.
find_package(Qt6 6.8 OPTIONAL_COMPONENTS Svg QUIET)
if(NOT TARGET Qt6::Svg)
    message(STATUS "NimbleEditor: Qt6 Svg not found; the editor uses the style's standard icons")
endif()

set(NIMBLE_EDITOR_SOURCES
        Source/Private/Editor/EditorMain.cpp
        Source/Private/Editor/AboutDialog.cpp
        Source/Private/Editor/AboutDialog.h
        Source/Private/Editor/ComponentPicker.cpp
        Source/Private/Editor/ComponentPicker.h
        Source/Private/Editor/ComponentDocs.cpp
        Source/Private/Editor/ComponentDocs.h
        Source/Private/Editor/EditorLayoutSettings.cpp
        Source/Private/Editor/EditorLayoutSettings.h
        Source/Private/Editor/DocumentationWindow.cpp
        Source/Private/Editor/DocumentationWindow.h
        Source/Private/Editor/EditorContext.cpp
        Source/Private/Editor/EditorDocument.cpp
        Source/Private/Editor/EditorHistory.cpp
        Source/Private/Editor/EditorHistory.h
        Source/Private/Editor/EditorCamera.cpp
        Source/Private/Editor/EditorLog.cpp
        Source/Public/Editor/EditorContentEvents.h
        Source/Private/Editor/EditorContentEvents.cpp
        Source/Public/Editor/EditorNotifications.h
        Source/Private/Editor/EditorNotifications.cpp
        Source/Private/Editor/NotificationWidgets.cpp
        Source/Private/Editor/NotificationWidgets.h
        Source/Private/Editor/SourceControl/GitClient.cpp
        Source/Private/Editor/SourceControl/GitClient.h
        Source/Private/Editor/SourceControl/SourceControlWindow.cpp
        Source/Private/Editor/SourceControl/SourceControlWindow.h
        Source/Private/Editor/Collab/CollabSessionWindow.cpp
        Source/Private/Editor/Collab/CollabSessionWindow.h
        Source/Private/Editor/Collab/EditorCollabSession.cpp
        Source/Private/Editor/Collab/EditorCollabSession.h
        Source/Private/Editor/EditorConsole.cpp
        Source/Private/Editor/ConsolePanel.cpp
        Source/Private/Editor/EditorMainWindow.cpp
        Source/Private/Editor/ViewportPanel.cpp
        Source/Private/Editor/ViewportWidget.cpp
        Source/Private/Editor/TransformGizmo.cpp
        Source/Private/Editor/OutlinerPanel.cpp
        Source/Private/Editor/EditorLights.cpp
        Source/Private/Editor/EditorLights.h
        Source/Private/Editor/EditorPlayerStarts.cpp
        Source/Private/Editor/EditorPlayerStarts.h
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
        Source/Private/Editor/EditorIcons.cpp
        Source/Private/Editor/EditorIcons.h
        Source/Private/Editor/AI/AIDebuggerPanel.cpp
        Source/Private/Editor/AI/AIDebuggerPanel.h
        Source/Private/Editor/AI/BehaviorTreeEditorWindow.cpp
        Source/Private/Editor/AI/BehaviorTreeEditorWindow.h
        Source/Private/Editor/AI/EditorAI.cpp
        Source/Private/Editor/AI/EditorAI.h
        Source/Private/Editor/AI/NavigationPanel.cpp
        Source/Private/Editor/AI/NavigationPanel.h
        Source/Private/Editor/AI/PathTesterTool.cpp
        Source/Private/Editor/AI/PathTesterTool.h
        Source/Private/Editor/Sequencer/SequencerAssets.cpp
        Source/Private/Editor/Sequencer/SequencerAssets.h
        Source/Private/Editor/Sequencer/SequencerCurveEditor.cpp
        Source/Private/Editor/Sequencer/SequencerCurveEditor.h
        Source/Private/Editor/Sequencer/SequencerDetails.cpp
        Source/Private/Editor/Sequencer/SequencerDetails.h
        Source/Private/Editor/Sequencer/SequencerDocument.cpp
        Source/Private/Editor/Sequencer/SequencerDocument.h
        Source/Private/Editor/Sequencer/SequencerTab.cpp
        Source/Private/Editor/Sequencer/SequencerTab.h
        Source/Private/Editor/Sequencer/SequencerTimeline.cpp
        Source/Private/Editor/Sequencer/SequencerTimeline.h
        Source/Private/Editor/Sequencer/SequencerWindow.cpp
        Source/Private/Editor/Sequencer/SequencerWindow.h
        Source/Private/Editor/Terrain/FoliageEditing.cpp
        Source/Private/Editor/Procedural/ProceduralEditing.cpp
        Source/Private/Editor/Procedural/ProceduralEditing.h
        Source/Private/Editor/Terrain/FoliageEditing.h
        Source/Private/Editor/Terrain/TerrainEditing.cpp
        Source/Private/Editor/Terrain/TerrainEditing.h
        Source/Private/Editor/Terrain/TerrainTool.cpp
        Source/Private/Editor/Terrain/TerrainTool.h
        Source/Private/Editor/Terrain/TerrainToolsWindow.cpp
        Source/Private/Editor/Terrain/TerrainToolsWindow.h
        Source/Public/Editor/ViewportTool.h
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
        Source/Private/Editor/Build/StartupCook.cpp
        Source/Private/Editor/Build/StartupCook.h
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
        Source/Private/Editor/Content/RetargetEditorWindow.cpp
        Source/Private/Editor/Content/RetargetEditorWindow.h
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
        Source/Private/Editor/Project/NewPluginDialog.cpp
        Source/Private/Editor/Project/NewPluginDialog.h
        Source/Private/Editor/Project/PluginsPage.cpp
        Source/Private/Editor/Project/PluginsPage.h
        Source/Private/Editor/Project/NewProjectWizard.cpp
        Source/Private/Editor/Project/NewProjectWizard.h
)

# git status parsing and the Source Control dock's path groups. No Qt, so the
# source control tests build it alone.
set(NIMBLE_EDITOR_GIT_SOURCES
        Source/Private/Editor/SourceControl/GitStatus.cpp
        Source/Private/Editor/SourceControl/GitStatus.h
)
list(APPEND NIMBLE_EDITOR_SOURCES ${NIMBLE_EDITOR_GIT_SOURCES})

# File > New Project's generator. Widget-free, so the project tests build it alone.
set(NIMBLE_EDITOR_PROJECT_SOURCES
        Source/Private/Editor/Project/PluginTemplate.cpp
        Source/Private/Editor/Project/PluginTemplate.h
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
        Source/Private/Editor/Content/MeshTangents.cpp
        Source/Private/Editor/Content/MeshTangents.h
        Source/Private/Editor/Content/MikkTSpaceImpl.cpp
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

if(NOT MSVC)
    set_source_files_properties(
            Source/Private/Editor/Lighting/LightBakeBVH.cpp
            Source/Private/Editor/Lighting/LightBaker.cpp
            Source/Private/Editor/Lighting/LightBakeScene.cpp
            PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# cgltf (MIT, header-only; implemented in GltfImporter.cpp) and xatlas (MIT,
# NimbleXatlas: lightmap UV charts, LightmapUVs.cpp) are vendored at
# Engine/ThirdParty/Source; xatlas is built by Engine/ThirdParty/CMakeLists.txt.
set(NIMBLE_EDITOR_CGLTF_DIR "${NIMBLE_THIRDPARTY_CGLTF_DIR}")
# MikkTSpace (zlib; mesh tangents, MeshTangents.cpp) is vendored there too and
# compiled as C++ by MikkTSpaceImpl.cpp, the engine project having no C.
set_source_files_properties(
        Source/Private/Editor/Content/MeshTangents.cpp
        Source/Private/Editor/Content/MikkTSpaceImpl.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${NIMBLE_THIRDPARTY_MIKKTSPACE_DIR}")

add_executable(NimbleEditor WIN32 ${NIMBLE_EDITOR_SOURCES})
# The application icon: the .exe's own icon on Windows (Explorer, the taskbar
# before Qt is up); every platform gets the window icon from :/app (EditorMain).
if(WIN32)
    target_sources(NimbleEditor PRIVATE Resources/AppIcon/NimbleEditor.rc)
endif()

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
        Collab::Collab
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
if(TARGET NetCore)
    target_link_libraries(NimbleEditor PRIVATE NetCore::NetCore)
endif()
# Sequences: the cook (Build/ContentCook.cpp), and the Sequencer to come.
if(TARGET Sequencer)
    target_link_libraries(NimbleEditor PRIVATE Sequencer::Sequencer)
endif()
# Sound import and preview (Content/AssetImport.cpp), when the module is built.
if(TARGET Audio)
    target_link_libraries(NimbleEditor PRIVATE Audio::Audio)
endif()
# Navigation, the AI debugger and the behavior tree editor (Source/Private/Editor/AI).
if(TARGET AICore)
    target_link_libraries(NimbleEditor PRIVATE AICore::AICore)
endif()

# === Icons ======================================================================
# Lucide (ISC), vendored at lucide-static 1.53.0: External/lucide-1.53.0, its
# LICENSE beside the SVGs. Compiled in as :/lucide/<name>.svg (EditorIcons.h).
set(NIMBLE_EDITOR_LUCIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/lucide-1.53.0")
qt_add_resources(NimbleEditor "LucideIcons"
        PREFIX "/lucide"
        BASE   "${NIMBLE_EDITOR_LUCIDE_DIR}/icons"
        FILES
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/play.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/pause.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/square.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/undo-2.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/redo-2.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/save.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/skip-back.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/skip-forward.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/step-back.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/step-forward.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/repeat.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/circle-dot.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/key-round.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/clapperboard.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/plane.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/magnet.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/plus.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/zoom-in.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/zoom-out.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/scan.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/volume-2.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/audio-waveform.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/captions.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/eye.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/circle-question-mark.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/bell.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/bell-off.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/circle-x.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/circle-check.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/triangle-alert.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/info.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/x.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/git-branch.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/refresh-cw.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/arrow-up.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/arrow-down.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/cloud-download.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/upload.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/git-commit-horizontal.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/lock.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/git-merge.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/arrow-left.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/arrow-right.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/folder-up.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/folder-plus.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/folder-open.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/folder.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/file-text.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/list.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/import.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/external-link.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/rotate-ccw.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/minus.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/chevron-up.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/chevron-down.svg"
            "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/brush-cleaning.svg"
)
# The editor's own icons in Lucide's style (the Terrain tools), beside them as :/lucide/<name>.svg.
set(NIMBLE_EDITOR_OWN_ICONS
        "Resources/Icons/terrain.svg"
        "Resources/Icons/terrain-raise.svg"
        "Resources/Icons/terrain-lower.svg"
        "Resources/Icons/terrain-smooth.svg"
        "Resources/Icons/terrain-flatten.svg"
        "Resources/Icons/terrain-noise.svg"
        "Resources/Icons/terrain-set-height.svg"
        "Resources/Icons/terrain-paint.svg"
        "Resources/Icons/terrain-smooth-paint.svg"
        "Resources/Icons/foliage.svg"
        "Resources/Icons/foliage-erase.svg")
qt_add_resources(NimbleEditor "EditorOwnIcons"
        PREFIX "/lucide"
        BASE   "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icons"
        FILES  ${NIMBLE_EDITOR_OWN_ICONS})
qt_add_resources(NimbleEditor "AppIcon"
        PREFIX "/app"
        BASE   "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon"
        FILES
            "Resources/AppIcon/NimbleEditor-16.png"
            "Resources/AppIcon/NimbleEditor-32.png"
            "Resources/AppIcon/NimbleEditor-48.png"
            "Resources/AppIcon/NimbleEditor-64.png"
            "Resources/AppIcon/NimbleEditor-128.png"
            "Resources/AppIcon/NimbleEditor-256.png"
            "Resources/AppIcon/NimbleEditor-512.png"
)
if(TARGET Qt6::Svg)
    target_link_libraries(NimbleEditor PRIVATE Qt6::Svg)
    set_source_files_properties(Source/Private/Editor/EditorIcons.cpp PROPERTIES
            COMPILE_DEFINITIONS "NIMBLEEDITOR_HAS_QTSVG=1")
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

    # Notifications, offscreen: the model, toasts, the dock and the status bar's bell.
    add_executable(NimbleEditorNotificationTests Tests/NotificationTests.cpp
            Source/Public/Editor/EditorNotifications.h
            Source/Private/Editor/EditorNotifications.cpp
            Source/Private/Editor/NotificationWidgets.h
            Source/Private/Editor/NotificationWidgets.cpp
            Source/Private/Editor/EditorIcons.h
            Source/Private/Editor/EditorIcons.cpp)
    target_include_directories(NimbleEditorNotificationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(NimbleEditorNotificationTests PRIVATE EngineCore::EngineCore Qt6::Core Qt6::Gui Qt6::Widgets)
    set_target_properties(NimbleEditorNotificationTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorNotificationTests NimbleEditor)
    add_test(NAME Editor.Notifications COMMAND NimbleEditorNotificationTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorNotificationTests>")

    # Source control: porcelain v2 fixtures, path groups, and git itself
    # against a local bare repository (skipped without git), then the dock.
    add_executable(NimbleEditorSourceControlTests Tests/SourceControlTests.cpp
            ${NIMBLE_EDITOR_GIT_SOURCES}
            Source/Private/Editor/SourceControl/GitClient.cpp
            Source/Private/Editor/SourceControl/GitClient.h
            Source/Private/Editor/SourceControl/SourceControlWindow.cpp
            Source/Private/Editor/SourceControl/SourceControlWindow.h
            Source/Private/Editor/ConfigIniFile.cpp
            Source/Private/Editor/ConfigIniFile.h
            Source/Public/Editor/EditorNotifications.h
            Source/Private/Editor/EditorNotifications.cpp
            Source/Private/Editor/EditorIcons.h
            Source/Private/Editor/EditorIcons.cpp)
    target_include_directories(NimbleEditorSourceControlTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(NimbleEditorSourceControlTests PRIVATE EngineCore::EngineCore Qt6::Core Qt6::Gui Qt6::Widgets)
    set_target_properties(NimbleEditorSourceControlTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorSourceControlTests NimbleEditor)
    add_test(NAME Editor.SourceControl COMMAND NimbleEditorSourceControlTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorSourceControlTests>")

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
    # And NewPlugin.cmake, File > New Plugin's twin, the same way.
    get_filename_component(NM_EDITOR_NEW_PLUGIN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../Binaries/NewPlugin.cmake" ABSOLUTE)
    target_compile_definitions(NimbleEditorProjectTests PRIVATE
            "NIMBLEEDITOR_CMAKE_COMMAND=\"${CMAKE_COMMAND}\""
            "NIMBLEEDITOR_NEW_PROJECT_SCRIPT=\"${NM_EDITOR_NEW_PROJECT_SCRIPT}\""
            "NIMBLEEDITOR_NEW_PLUGIN_SCRIPT=\"${NM_EDITOR_NEW_PLUGIN_SCRIPT}\"")
    # And example projects: the Tiny fixture, unpacked by NewProject.cmake
    # -DEXAMPLE=, and packed again by Engine/Examples/pack_example.py (with a
    # Python that has compression.zstd; skipped otherwise).
    get_filename_component(NM_EDITOR_PACK_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../Examples/pack_example.py" ABSOLUTE)
    target_compile_definitions(NimbleEditorProjectTests PRIVATE
            "NIMBLEEDITOR_EXAMPLES_FIXTURE=\"${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/Examples\""
            "NIMBLEEDITOR_EXAMPLE_PROJECT_FIXTURE=\"${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data/ExampleProject/Tiny\""
            "NIMBLEEDITOR_PACK_SCRIPT=\"${NM_EDITOR_PACK_SCRIPT}\"")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        target_compile_definitions(NimbleEditorProjectTests PRIVATE "NIMBLEEDITOR_PYTHON=\"${Python3_EXECUTABLE}\"")
    endif()
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
            Collab::Collab
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorDetailsTests PRIVATE NetCore::NetCore)
    endif()
    if(TARGET AICore)
        target_link_libraries(NimbleEditorDetailsTests PRIVATE AICore::AICore)
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
            Collab::Collab
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorUIDesignerTests PRIVATE NetCore::NetCore)
    endif()
    if(TARGET AICore)
        target_link_libraries(NimbleEditorUIDesignerTests PRIVATE AICore::AICore)
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
            Collab::Collab
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorParticleTests PRIVATE NetCore::NetCore)
    endif()
    if(TARGET AICore)
        target_link_libraries(NimbleEditorParticleTests PRIVATE AICore::AICore)
    endif()
    set_target_properties(NimbleEditorParticleTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorParticleTests NimbleEditor)
    add_test(NAME Editor.Particles COMMAND NimbleEditorParticleTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorParticleTests>")

    # The Sequencer's document, timeline, curves, details and preview, offscreen.
    if(TARGET Sequencer)
        add_executable(NimbleEditorSequencerTests Tests/SequencerEditorTests.cpp ${NIMBLE_EDITOR_DETAILS_SOURCES})
        target_include_directories(NimbleEditorSequencerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
        target_link_libraries(NimbleEditorSequencerTests PRIVATE
                GameCore::GameCore
                RenderCore::RenderCore
                GameWindow::GameWindow
                NimbleEngine::NimbleEngine
                EngineCore::QtBridge
                NodeGraph::NodeGraph
                NimbleXatlas
                Collab::Collab
                Qt6::Core
                Qt6::Gui
                Qt6::Widgets
        )
        if(TARGET NetCore)
            target_link_libraries(NimbleEditorSequencerTests PRIVATE NetCore::NetCore)
        endif()
        if(TARGET AICore)
            target_link_libraries(NimbleEditorSequencerTests PRIVATE AICore::AICore)
        endif()
        set_target_properties(NimbleEditorSequencerTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
        add_dependencies(NimbleEditorSequencerTests NimbleEditor)
        add_test(NAME Editor.Sequencer COMMAND NimbleEditorSequencerTests
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorSequencerTests>")
    endif()

    # Terrain mode: making a terrain, strokes as undo steps, saving, the dock, offscreen.
    add_executable(NimbleEditorTerrainTests Tests/TerrainEditorTests.cpp ${NIMBLE_EDITOR_DETAILS_SOURCES})
    target_include_directories(NimbleEditorTerrainTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorTerrainTests PRIVATE
            GameCore::GameCore
            RenderCore::RenderCore
            GameWindow::GameWindow
            NimbleEngine::NimbleEngine
            EngineCore::QtBridge
            NodeGraph::NodeGraph
            NimbleXatlas
            Collab::Collab
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorTerrainTests PRIVATE NetCore::NetCore)
    endif()
    if(TARGET AICore)
        target_link_libraries(NimbleEditorTerrainTests PRIVATE AICore::AICore)
    endif()
    set_target_properties(NimbleEditorTerrainTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorTerrainTests NimbleEditor)
    add_test(NAME Editor.Terrain COMMAND NimbleEditorTerrainTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorTerrainTests>")

    # Collaboration: two editors' sessions against an in-process server, offscreen.
    add_executable(NimbleEditorCollabTests Tests/CollabEditorTests.cpp ${NIMBLE_EDITOR_DETAILS_SOURCES})
    target_include_directories(NimbleEditorCollabTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source "${NIMBLE_EDITOR_CGLTF_DIR}")
    target_link_libraries(NimbleEditorCollabTests PRIVATE
            GameCore::GameCore
            RenderCore::RenderCore
            GameWindow::GameWindow
            NimbleEngine::NimbleEngine
            EngineCore::QtBridge
            NodeGraph::NodeGraph
            NimbleXatlas
            Collab::Collab
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    if(TARGET NetCore)
        target_link_libraries(NimbleEditorCollabTests PRIVATE NetCore::NetCore)
    endif()
    if(TARGET AICore)
        target_link_libraries(NimbleEditorCollabTests PRIVATE AICore::AICore)
    endif()
    set_target_properties(NimbleEditorCollabTests PROPERTIES FOLDER "Editor" AUTOMOC ON)
    add_dependencies(NimbleEditorCollabTests NimbleEditor)
    add_test(NAME Editor.Collab COMMAND NimbleEditorCollabTests
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:NimbleEditorCollabTests>")

    # The offscreen tests compile EditorIcons.cpp too (no resources: fallback icons).
    if(TARGET Qt6::Svg)
        foreach(NM_EDITOR_TEST IN ITEMS NimbleEditorDetailsTests NimbleEditorUIDesignerTests NimbleEditorParticleTests NimbleEditorSequencerTests
                NimbleEditorTerrainTests NimbleEditorNotificationTests NimbleEditorSourceControlTests NimbleEditorCollabTests)
            if(TARGET ${NM_EDITOR_TEST})
                target_link_libraries(${NM_EDITOR_TEST} PRIVATE Qt6::Svg)
            endif()
        endforeach()
    endif()

    # The tests compile the editor's own sources, sound import among them.
    if(TARGET Audio)
        foreach(NM_EDITOR_TEST IN ITEMS NimbleEditorImportTests NimbleEditorLightingTests NimbleEditorDocumentTests
                NimbleEditorProjectTests NimbleEditorDetailsTests NimbleEditorUIDesignerTests NimbleEditorParticleTests
                NimbleEditorSequencerTests NimbleEditorTerrainTests NimbleEditorCollabTests)
            if(TARGET ${NM_EDITOR_TEST})
                target_link_libraries(${NM_EDITOR_TEST} PRIVATE Audio::Audio)
            endif()
        endforeach()
    endif()
    # And the content cook, which cooks sequences.
    if(TARGET Sequencer)
        foreach(NM_EDITOR_TEST IN ITEMS NimbleEditorDetailsTests NimbleEditorUIDesignerTests NimbleEditorParticleTests NimbleEditorSequencerTests
                NimbleEditorTerrainTests NimbleEditorCollabTests)
            if(TARGET ${NM_EDITOR_TEST})
                target_link_libraries(${NM_EDITOR_TEST} PRIVATE Sequencer::Sequencer)
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

# The cooked shaders the Renderer loads at startup, staged by the engine; and
# NimbleCook beside the editor, which Build > Cook Content runs to cook them.
if(TARGET NimbleStageShaders)
    add_dependencies(NimbleEditor NimbleStageShaders)
endif()
if(TARGET NimbleCook)
    add_dependencies(NimbleEditor NimbleCook)
endif()

# Qt's DLLs beside the exe, so it runs without Qt on PATH. qoffscreen too:
# the editor tests run headless (QT_QPA_PLATFORM=offscreen), and windeployqt
# deploys only qwindows unless asked.
if(WIN32)
    get_target_property(NM_QT_QMAKE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(NM_QT_BIN_DIR "${NM_QT_QMAKE}" DIRECTORY)
    find_program(NM_WINDEPLOYQT windeployqt HINTS "${NM_QT_BIN_DIR}" NO_DEFAULT_PATH)

    if(NM_WINDEPLOYQT)
        add_custom_command(TARGET NimbleEditor POST_BUILD
                COMMAND "${NM_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                        --no-opengl-sw --no-system-d3d-compiler
                        --include-plugins qoffscreen
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

# The Details, UI Designer, Particle and Sequencer tests compile the editor's sources, so they need
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
if(TARGET NimbleEditorCollabTests)
    target_include_directories(NimbleEditorCollabTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
endif()
if(TARGET NimbleEditorTerrainTests)
    target_include_directories(NimbleEditorTerrainTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    qt_add_resources(NimbleEditorTerrainTests "EditorOwnIconsTests"
            PREFIX "/lucide"
            BASE   "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Icons"
            FILES  ${NIMBLE_EDITOR_OWN_ICONS})
endif()
if(TARGET NimbleEditorSequencerTests)
    target_include_directories(NimbleEditorSequencerTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    # The Lucide icons too, so its snapshot shows the toolbar as the editor draws it.
    qt_add_resources(NimbleEditorSequencerTests "LucideIconsTests"
            PREFIX "/lucide"
            BASE   "${NIMBLE_EDITOR_LUCIDE_DIR}/icons"
            FILES
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/play.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/pause.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/square.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/undo-2.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/redo-2.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/save.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/skip-back.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/skip-forward.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/step-back.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/step-forward.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/repeat.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/circle-dot.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/key-round.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/clapperboard.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/plane.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/magnet.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/plus.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/zoom-in.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/zoom-out.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/scan.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/volume-2.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/audio-waveform.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/captions.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/eye.svg"
                "${NIMBLE_EDITOR_LUCIDE_DIR}/icons/circle-question-mark.svg"
    )
endif()

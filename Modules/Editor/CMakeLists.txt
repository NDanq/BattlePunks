# ===========================================================================
# NimbleEditor -- the Qt Widgets editor.
#
# An EXECUTABLE, not a nimble_add_module library: nothing links against the
# editor. Added from Engine/CMakeLists.txt, so it inherits the flat
# <NIMBLE_BINARIES_DIR>/<Platform>_<Config> runtime directory and lands beside
# EngineCore.dll, GameCore.dll and RenderCore.dll -- the same sibling rule a
# game host follows.
#
# WHAT IT LINKS, AND WHAT IT MUST NOT
#
#   GameCore, RenderCore   the World it edits and the Renderer that draws it.
#   EngineCore::QtBridge   value conversions (QColor <-> Color). Qt-only, no GDI.
#
#   NOT GDI, and NOT GDI::QtBridge. GDI is a static library that may live in
#   exactly one image in the process -- RenderCore.dll (RenderCoreInternal.h).
#   GDIQtBridge links GDI PUBLIC, so linking it here would put a second copy in
#   the editor's exe. The viewport hands the Renderer a native handle taken from
#   QWidget::winId() instead, which is all GDIQt::ToWindowHandle would do.
#
#   NimbleEngine       GameSession and GameModuleHost, for Play-in-Editor: the
#                      editor hosts a game session the way Application does.
#                      NOT Application itself -- it owns an SDL window and its
#                      own event loop, and the editor's main thread belongs to
#                      Qt. EditorContext drives the same frame sequence (tick,
#                      fence, collect, present, EndFrame) from a Qt timer.
#
#   NOT the game. The game module ([Game] Module) is loaded at Play by name;
#   the game's CMakeLists makes NimbleEditor depend on it so it is built.
#
# EDITOR TARGETS ONLY. Engine/CMakeLists.txt adds this directory only when
# NIMBLE_TARGET is Editor (WITH_EDITOR=1), and every source below is wrapped in
# `#if WITH_EDITOR` as well; EditorMain.cpp #errors if built any other way.
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
        Source/Private/Editor/PlaySession.cpp
        Source/Private/Editor/PlaySession.h
        Source/Private/Editor/SDLInputProbe.h
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
)

add_executable(NimbleEditor WIN32 ${NIMBLE_EDITOR_SOURCES})

# The only engine target with moc on: the editor is a Qt application, and its
# panels talk through signals. Every other target keeps AUTOMOC OFF.
set_target_properties(NimbleEditor PROPERTIES
        AUTOMOC ON
        AUTOUIC OFF
        AUTORCC OFF
        FOLDER  "Editor"
)

target_include_directories(NimbleEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

target_link_libraries(NimbleEditor PRIVATE
        GameCore::GameCore
        RenderCore::RenderCore
        GameWindow::GameWindow
        NimbleEngine::NimbleEngine
        EngineCore::QtBridge
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

# === Build version stamp ======================================================
# Help > About shows the commit this editor was built from. Core's
# GitVersion.cmake does the stamping (Core only stamps itself when configured
# standalone, so an engine build has no stamp otherwise); included by path, not
# via CMAKE_MODULE_PATH, so nothing about Core's own configure changes. It
# re-stamps on every build but only rewrites the header when a value changed,
# and only AboutDialog.cpp includes it.
include("${CMAKE_CURRENT_SOURCE_DIR}/../Core/cmake/GitVersion.cmake")
git_version_stamp(
        PREFIX   NIMBLEEDITOR
        VERSION  "${NimbleEngine_VERSION}"
        TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Private/Editor/EditorBuildInfo.h.in"
        OUTPUT   "${CMAKE_CURRENT_BINARY_DIR}/Generated/EditorBuildInfo.h"
        TARGET   NimbleEditor
)
target_include_directories(NimbleEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")

# What git cannot know. Bare tokens (AboutDialog.cpp stringizes them), and on
# that one file so a configuration switch does not recompile the editor.
set_source_files_properties(Source/Private/Editor/AboutDialog.cpp PROPERTIES
        COMPILE_DEFINITIONS "NIMBLE_BUILD_CONFIG=$<CONFIG>;NIMBLE_BUILD_TARGET=${NIMBLE_TARGET}")

# The cooked shaders the Renderer loads at startup, staged by the engine.
if(TARGET NimbleStageShaders)
    add_dependencies(NimbleEditor NimbleStageShaders)
endif()

# Qt's own DLLs and platform plugin beside the exe, so it runs from the
# Binaries directory without Qt on PATH. windeployqt picks debug or release Qt
# from the exe it is given.
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

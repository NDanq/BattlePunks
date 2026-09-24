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
#   NOT NimbleEngine. Application owns an SDL window and its own event loop on
#   the main thread; the editor's main thread belongs to Qt. EditorContext drives
#   the same frame sequence (fence, collect, present, EndFrame) from a Qt timer.
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
        Source/Private/Editor/EditorContext.cpp
        Source/Private/Editor/EditorCamera.cpp
        Source/Private/Editor/EditorMainWindow.cpp
        Source/Private/Editor/ViewportPanel.cpp
        Source/Private/Editor/ViewportWidget.cpp
        Source/Private/Editor/OutlinerPanel.cpp
        Source/Private/Editor/DetailsPanel.cpp
        Source/Private/Editor/PropertyWidgets.cpp
        Source/Private/Editor/PropertyWidgets.h
        Source/Private/Editor/SDLInputProbe.cpp
        Source/Private/Editor/SDLInputProbe.h
        Source/Public/Editor/EditorContext.h
        Source/Public/Editor/EditorCamera.h
        Source/Public/Editor/EditorMainWindow.h
        Source/Public/Editor/ViewportPanel.h
        Source/Public/Editor/ViewportWidget.h
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
        EngineCore::QtBridge
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

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

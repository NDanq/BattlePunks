# ===========================================================================
# UI -- in-game interfaces from XML documents (README.md)
# ===========================================================================
#
# Widgets, layout, styles, documents (.nmui), actions and bindings, fonts
# (stb_truetype), and painting into RenderCore's UI draw list. Knows no
# input system and no window: a host feeds it a UIInputFrame and a
# viewport size (NimbleEngine's GameSession; the editor's designer).
#
# Added by Engine/CMakeLists.txt after RenderCore and before NimbleEngine.

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR "Modules/UI is configured through Engine/CMakeLists.txt (nimble_add_module is not defined)")
endif()

option(UI_BUILD_TESTS "Build UITests" ON)

set(UI_SOURCES
        Source/Public/UI/UIAPI.h
        Source/Public/UI/UILog.h
        Source/Public/UI/UITypes.h
        Source/Public/UI/UIAction.h
        Source/Public/UI/UIProperty.h
        Source/Public/UI/UIWidget.h
        Source/Public/UI/UIWidgets.h
        Source/Public/UI/UIFont.h
        Source/Public/UI/UIBinding.h
        Source/Public/UI/UIDocument.h
        Source/Public/UI/UIInput.h
        Source/Public/UI/UISystem.h
        Source/Public/UI/UIFile.h
        Source/Public/UI/UIAnimation.h
        Source/Public/UI/UIVisualState.h
        Source/Private/UI/UIInternal.h
        Source/Private/UI/UIStb.h
        Source/Private/UI/UITypes.cpp
        Source/Private/UI/UIAction.cpp
        Source/Private/UI/UIProperty.cpp
        Source/Private/UI/UIWidget.cpp
        Source/Private/UI/UIWidgets.cpp
        Source/Private/UI/UIFont.cpp
        Source/Private/UI/UIBinding.cpp
        Source/Private/UI/UIDocument.cpp
        Source/Private/UI/UISystem.cpp
        Source/Private/UI/UIBinary.cpp
        Source/Private/UI/UIXml.cpp
        Source/Private/UI/UIAnimation.cpp
        Source/Private/UI/UIScriptLibrary.cpp
        Source/Private/UI/UIBuiltinActions.cpp
)

# ALWAYS SHARED: the widget type and native action registries exist once (UIAPI.h).
nimble_add_module(UI SHARED
        PREFIX      UI
        SOURCES     ${UI_SOURCES}
        PUBLIC_DEPS GameCore::GameCore RenderCore::RenderCore)

# Private/ includes, and stb (External/stb, compiled into UIFont.cpp only).
target_include_directories(UI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source ${CMAKE_CURRENT_SOURCE_DIR}/External/stb)

# XML is read by every build but Shipping (WITH_UNCOOKED): the editor, and a
# Debug or RelWithDebInfo game run uncooked. PRIVATE: no public header
# includes pugixml.hpp.
if(TARGET PugiXml::PugiXml)
    target_link_libraries(UI PRIVATE $<$<NOT:$<CONFIG:Shipping>>:PugiXml::PugiXml>)
endif()

source_group("UI" FILES ${UI_SOURCES})

# Editor builds only: the fixtures are XML, which a Client build cannot read.
# The cooked path they also test is the same code in every build.
if(UI_BUILD_TESTS AND NIMBLE_WITH_EDITOR)
    add_executable(UITests Tests/UITests.cpp)
    target_link_libraries(UITests PRIVATE UI::UI)
    target_include_directories(UITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    set_target_properties(UITests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_compile_definitions(UITests PRIVATE UI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data"
                                               UI_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../..")

    enable_testing()
    add_test(NAME UI.Core COMMAND UITests)
    set_tests_properties(UI.Core PROPERTIES TIMEOUT 120 LABELS "ui")
endif()

# ===========================================================================
# EngineCoreQtBridge -- Qt interop for EngineCore's value types.
#
# One of exactly two targets in the EngineCore/GDI projects allowed to include
# Qt (the other is GDIQtBridge). A Qt host links this; an SDL host does not,
# and EngineCore itself never references it. If Qt ever appears anywhere else
# under Core/Source, that is the regression the static checks look for.
#
# Note there is deliberately no Qt log sink here. The obvious one would forward
# to Odessa's OLogger -- but OLogger lives in CoreClient, so putting it here
# would invert the dependency and make EngineCore's bridge depend on the
# application. The host installs its own EngineCore::SetLogHandler in main()
# instead, which is both simpler and correctly directed.
# ===========================================================================

find_package(Qt6 6.8 COMPONENTS Core Gui REQUIRED)

add_library(EngineCoreQtBridge STATIC
        CoreQtConversions.cpp
        CoreQtConversions.h
        CoreQtFileProvider.cpp
        CoreQtFileProvider.h
)
add_library(EngineCore::QtBridge ALIAS EngineCoreQtBridge)

target_include_directories(EngineCoreQtBridge
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(EngineCoreQtBridge
        PUBLIC EngineCore::EngineCore
               Qt6::Core
               Qt6::Gui
)

# No Q_OBJECT anywhere in the bridge -- these are free functions over value
# types. Keeping moc off makes that a build-enforced property rather than a
# convention.
set_target_properties(EngineCoreQtBridge PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

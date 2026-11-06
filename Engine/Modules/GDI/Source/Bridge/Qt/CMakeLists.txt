# ===========================================================================
# GDIQtBridge -- Qt interop for GDI's types.
#
# The only target in the GDI project allowed to include Qt. CoreClient links
# it; an SDL host links GDI alone. Once the migration completes, the static
# check `grep -rn '#include *<Q' GDI/Source | grep -v /Bridge/` must come back
# empty -- this directory is the sanctioned exception it excludes.
#
# Layers on EngineCoreQtBridge: the value-type conversions (QImage <-> Image,
# QColor <-> Color, QRect <-> Rect) live there because they are not
# graphics-specific. What lives here is anything that touches a GDI type.
# ===========================================================================

find_package(Qt6 6.8 COMPONENTS Core Gui REQUIRED)

set( GDI_QT_BRIDGE_SOURCES
        GDIQtWindow.cpp
        GDIQtWindow.h
        GDIQtTexture.cpp
        GDIQtTexture.h
        GDIQtVulkanInstance.cpp
        GDIQtVulkanInstance.h
)

# The GL context wrapper includes GDIOpenGL.h, which is WGL-only, so it exists
# only alongside the GL backend.
if(GDI_WITH_OPENGL)
    list(APPEND GDI_QT_BRIDGE_SOURCES GDIQtGLContext.cpp GDIQtGLContext.h)
endif()

add_library(GDIQtBridge STATIC ${GDI_QT_BRIDGE_SOURCES})
add_library(GDI::QtBridge ALIAS GDIQtBridge)

target_include_directories(GDIQtBridge
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# GDIQtVulkanInstance includes vulkan.h; GDI already links Vulkan PUBLIC, but
# being explicit keeps this target buildable on its own terms.
target_link_libraries(GDIQtBridge
        PUBLIC GDI::GDI
               EngineCore::QtBridge
               Qt6::Core
               Qt6::Gui
)

# No Q_OBJECT here, and there should never be one: the bridge converts values
# and hands over handles, it does not participate in signals and slots.
set_target_properties(GDIQtBridge PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

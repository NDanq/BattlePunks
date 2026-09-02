cmake_minimum_required(VERSION 3.16)

project(GDI VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# GDI -- Odessa's Graphics Device Interface: an Unreal-style RHI with OpenGL
# 4.5, Vulkan and Metal backends.
#
# Depends on EngineCore and, once the de-Qt work completes, on nothing else.
# Qt interop lives exclusively in the GDIQtBridge target below; an SDL host
# links GDI alone.
#
# Only code that issues explicit GPU work through the GDI abstraction belongs
# here. The CPU-side canvas renderer, layer stack and image kernels live in
# CoreClient/Source/{Public,Private}/Canvas/ and depend on GDI, never the
# reverse.
# ===========================================================================

option(GDI_SHARED          "Build GDI as a shared library"           OFF)
option(GDI_WITH_QT_BRIDGE  "Build the optional Qt interop bridge"     ON)
option(GDI_BUILD_SMOKETEST "Build GDISmokeTest"                      OFF)

# (Both transitional options are gone. GDI no longer includes an application
#  header -- logging goes through GDILog.h and the render platform is passed
#  into InitializeGDI -- and it no longer includes or links Qt at all.)


# === Backend selection ===
# Which backends are buildable is a property of the platform, not a taste:
#
#   OpenGL  Windows only. QOpenGLGDIContext creates its contexts through WGL --
#           HWND / HDC / HGLRC / wglCreateContextAttribsARB appear unguarded in
#           QOpenGLDynamicGDI.h, so the backend does not compile anywhere else.
#           A Linux port would need a GLX or EGL context path.
#   Vulkan  Windows and Linux. Not available on Apple (no loader; MoltenVK
#           would need Qt itself built against it).
#   Metal   Apple only -- and mandatory there, since macOS caps OpenGL at 4.1
#           while the renderer hard-requires 4.4, leaving no fallback.
if(APPLE)
    set(GDI_WITH_OPENGL_DEFAULT OFF)
    set(GDI_WITH_VULKAN_DEFAULT OFF)
    set(GDI_WITH_METAL_DEFAULT  ON)
elseif(WIN32)
    set(GDI_WITH_OPENGL_DEFAULT ON)
    set(GDI_WITH_VULKAN_DEFAULT ON)
    set(GDI_WITH_METAL_DEFAULT  OFF)
else()  # Linux / other Unix
    set(GDI_WITH_OPENGL_DEFAULT OFF)
    set(GDI_WITH_VULKAN_DEFAULT ON)
    set(GDI_WITH_METAL_DEFAULT  OFF)
endif()

option(GDI_WITH_OPENGL "Build the OpenGL GDI backend (Windows only)" ${GDI_WITH_OPENGL_DEFAULT})
option(GDI_WITH_VULKAN "Build the Vulkan GDI backend"                ${GDI_WITH_VULKAN_DEFAULT})
option(GDI_WITH_METAL  "Build the Metal GDI backend (Apple only)"    ${GDI_WITH_METAL_DEFAULT})

# Fail loudly at configure time rather than emitting a wall of unknown-type
# errors from the backend headers.
if(GDI_WITH_OPENGL AND NOT WIN32)
    message(FATAL_ERROR "GDI_WITH_OPENGL is Windows-only: QOpenGLGDIContext builds its contexts with WGL. Porting it needs a GLX/EGL context path.")
endif()
if(GDI_WITH_METAL AND NOT APPLE)
    message(FATAL_ERROR "GDI_WITH_METAL requires an Apple target.")
endif()
if(APPLE)
    if(GDI_WITH_VULKAN)
        message(WARNING "Vulkan is not available on Apple targets; forcing GDI_WITH_VULKAN OFF.")
        set(GDI_WITH_VULKAN OFF CACHE BOOL "Build the Vulkan GDI backend" FORCE)
    endif()
    if(NOT GDI_WITH_METAL)
        message(FATAL_ERROR "GDI_WITH_METAL is OFF on an Apple target, which would leave no usable render backend.")
    endif()
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

if(NOT GDI_WITH_OPENGL AND NOT GDI_WITH_VULKAN AND NOT GDI_WITH_METAL)
    message(FATAL_ERROR "No render backend enabled -- GDI would provide no driver.")
endif()

# === EngineCore ===
# When GDI is configured standalone it pulls EngineCore in itself. When it is
# built as part of Odessa, CoreClient has already added it, so the target
# exists and adding it twice would be an error.
if(NOT TARGET EngineCore)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../Core CoreBuild)
endif()

# ===========================================================================
# Sources
# ===========================================================================

set( GDI_CORE_SOURCES
        Source/Private/GDI/GDI.cpp
        Source/Private/GDI/GDIShader.cpp
        Source/Private/GDI/GDITexture.cpp
        Source/Private/GDI/GDIView.cpp
        Source/Private/GDI/GDIRenderTarget.cpp
        Source/Public/GDI/GDIAPI.h
        Source/Public/GDI/GDIPlatform.h
        Source/Public/GDI/GDILog.h
        Source/Public/GDI/GDI.h
        Source/Public/GDI/GDIDefinitions.h
        Source/Public/GDI/GDIShader.h
        Source/Public/GDI/GDITexture.h
        Source/Public/GDI/GDIMesh.h
        Source/Public/GDI/GDIRenderTarget.h
        Source/Public/GDI/GDIView.h
        Source/Public/GDI/GDISync.h
)

# Shader classes. These were misfiled in CoreClient's PROJECT_SOURCES; they are
# GDI's, and they belong with it.
set( GDI_SHADERCLASS_SOURCES
        Source/Public/GDI/Shaders/Classes/TestbedShaders.h
        Source/Public/GDI/Shaders/Classes/UIShaders.h
        Source/Public/GDI/Shaders/Classes/BrushShaders.h
)

if(GDI_WITH_OPENGL)
    set( GDI_OPENGL_SOURCES
            Source/Private/GDI/OpenGL/GDIOpenGL.cpp
            Source/Public/GDI/OpenGL/GDIOpenGL.h
            Source/Private/GDI/OpenGL/QOGLCommands.cpp
            Source/Private/GDI/OpenGL/QOGLTexture.cpp
            Source/Private/GDI/OpenGL/QOGLShader.cpp
            Source/Private/GDI/OpenGL/QOGLRenderTarget.cpp
            Source/Private/GDI/OpenGL/QOGLMesh.cpp
            Source/Private/GDI/OpenGL/QOGLSync.cpp
            Source/Public/GDI/OpenGL/QOpenGLDynamicGDI.h
            Source/Public/GDI/OpenGL/QOGLTexture.h
            Source/Public/GDI/OpenGL/QOGLShader.h
            Source/Public/GDI/OpenGL/QOGLMesh.h
            Source/Public/GDI/OpenGL/QOGLRenderTarget.h
            Source/Public/GDI/OpenGL/QOGLSync.h
    )
endif()

if(GDI_WITH_VULKAN)
    find_package(Vulkan REQUIRED)

    set( GDI_VULKAN_SOURCES
            Source/Private/GDI/Vulkan/QVulkanDynamicGDI.cpp
            Source/Private/GDI/Vulkan/QVKCommands.cpp
            Source/Private/GDI/Vulkan/QVKTexture.cpp
            Source/Private/GDI/Vulkan/QVKShader.cpp
            Source/Private/GDI/Vulkan/QVKRenderTarget.cpp
            Source/Private/GDI/Vulkan/QVKMesh.cpp
            Source/Private/GDI/Vulkan/QVKSync.cpp
            Source/Private/GDI/Vulkan/QVKDevice.cpp
            Source/Private/GDI/Vulkan/QVKSwapchain.cpp
            Source/Private/GDI/Vulkan/QVKCommandManager.cpp
            Source/Private/GDI/Vulkan/QVKPipelineCache.cpp
            Source/Private/GDI/Vulkan/QVKDescriptorAllocator.cpp
            # A real private header that was absent from CoreClient's list: it
            # compiled via inclusion but was invisible to IDE trees and to any
            # audit of the source lists.
            Source/Private/GDI/Vulkan/QVKTransientOps.h
            Source/Public/GDI/Vulkan/GDIVulkanPlatform.h
            Source/Public/GDI/Vulkan/QVulkanDynamicGDI.h
            Source/Public/GDI/Vulkan/QVKTexture.h
            Source/Public/GDI/Vulkan/QVKShader.h
            Source/Public/GDI/Vulkan/QVKMesh.h
            Source/Public/GDI/Vulkan/QVKRenderTarget.h
            Source/Public/GDI/Vulkan/QVKSync.h
            Source/Public/GDI/Vulkan/QVKDevice.h
            Source/Public/GDI/Vulkan/QVKSwapchain.h
            Source/Public/GDI/Vulkan/QVKCommandManager.h
            Source/Public/GDI/Vulkan/QVKPipelineCache.h
            Source/Public/GDI/Vulkan/QVKDescriptorAllocator.h
    )
endif()

if(GDI_WITH_METAL)
    # Objective-C++ sources. Everything that sends a Metal message lives in a
    # .mm; the public headers use only the MTLForward.h typedefs so plain C++
    # translation units can still include them.
    set( GDI_METAL_SOURCES
            Source/Private/GDI/Metal/QMetalDynamicGDI.mm
            Source/Private/GDI/Metal/QMTLCommands.mm
            Source/Private/GDI/Metal/QMTLDevice.mm
            Source/Private/GDI/Metal/QMTLCommandManager.mm
            Source/Private/GDI/Metal/QMTLSwapchain.mm
            Source/Private/GDI/Metal/QMTLPipelineCache.mm
            Source/Private/GDI/Metal/QMTLRenderTarget.mm
            Source/Private/GDI/Metal/QMTLTexture.mm
            Source/Private/GDI/Metal/QMTLMesh.mm
            Source/Private/GDI/Metal/QMTLShader.mm
            Source/Private/GDI/Metal/QMTLSync.mm
            Source/Private/GDI/Metal/MTLUtil.mm
            Source/Private/GDI/Metal/MTLUtil.h
            Source/Private/GDI/Metal/QMTLPipelineCache.h
            Source/Public/GDI/Metal/MTLForward.h
            Source/Public/GDI/Metal/QMetalDynamicGDI.h
            Source/Public/GDI/Metal/QMTLDevice.h
            Source/Public/GDI/Metal/QMTLCommandManager.h
            Source/Public/GDI/Metal/QMTLSwapchain.h
            Source/Public/GDI/Metal/QMTLRenderTarget.h
            Source/Public/GDI/Metal/QMTLTexture.h
            Source/Public/GDI/Metal/QMTLMesh.h
            Source/Public/GDI/Metal/QMTLShader.h
            Source/Public/GDI/Metal/QMTLSync.h
    )
endif()

# === Platform layer ===
# GDI's graphics-window seam (appGLGetProcAddress today; Vulkan surface
# creation joins it at Step 12). Exactly one implementation is compiled, so a
# port adds one file here and changes nothing else.
if(WIN32)
    set( GDI_PLATFORM_SOURCES Source/Private/GDI/Platform/GDIPlatformWindows.cpp )
else()
    set( GDI_PLATFORM_SOURCES )
endif()

# ===========================================================================
# The GDI target
# ===========================================================================

if(GDI_SHARED)
    set(GDI_LIB_TYPE SHARED)
else()
    set(GDI_LIB_TYPE STATIC)
endif()

add_library(GDI ${GDI_LIB_TYPE}
        ${GDI_CORE_SOURCES}
        ${GDI_PLATFORM_SOURCES}
        ${GDI_SHADERCLASS_SOURCES}
        ${GDI_OPENGL_SOURCES}
        ${GDI_VULKAN_SOURCES}
        ${GDI_METAL_SOURCES}
)
add_library(GDI::GDI ALIAS GDI)

# PUBLIC Source  -- so every existing `#include "Public/GDI/GDI.h"` keeps
#                   resolving verbatim through the physical move.
# PRIVATE root   -- QOpenGLDynamicGDI.h includes
#                   "External/wglext/include/wglext.h", which is
#                   PROJECT-ROOT-relative rather than Source-relative. It is
#                   the one include in the codebase that is not Source-based,
#                   which is why External/wglext moved here with GDI.
target_include_directories(GDI
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Source>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(GDI PUBLIC cxx_std_17)

# PUBLIC: GDI's public headers expose EngineCore types, and CoreClient must
# inherit WITH_SIMD and the compatibility aliases from the same single source.
target_link_libraries(GDI PUBLIC EngineCore::EngineCore)

if(GDI_SHARED)
    target_compile_definitions(GDI PUBLIC  GDI_SHARED_BUILD)
    target_compile_definitions(GDI PRIVATE GDI_BUILDING)
    if(MSVC)
        target_compile_options(GDI PRIVATE /wd4251)
    endif()
endif()

# Backend selection macros are PUBLIC because CoreClient branches on them too
# (CanvasWidget.cpp, QCanvasRenderer.cpp).
#
# CRITICAL: GDI.cpp uses `#if WITH_VULKAN_GDI`, NOT `#ifdef`. An undefined
# macro evaluates to 0 with no warning, which would silently delete the
# backend rather than fail the build. These must reach every consumer.
if(GDI_WITH_OPENGL)
    target_compile_definitions(GDI PUBLIC WITH_OPENGL_GDI)
endif()
if(GDI_WITH_VULKAN)
    target_compile_definitions(GDI PUBLIC WITH_VULKAN_GDI)
    target_link_libraries(GDI PUBLIC Vulkan::Vulkan)
endif()
if(GDI_WITH_METAL)
    target_compile_definitions(GDI PUBLIC WITH_METAL_GDI)
endif()

if(GDI_WITH_OPENGL)
    find_package(OpenGL REQUIRED)
    target_link_libraries(GDI PUBLIC ${OPENGL_LIBRARIES})
    target_include_directories(GDI PUBLIC ${OPENGL_INCLUDE_DIRS})
    if(WIN32)
        target_link_libraries(GDI PUBLIC opengl32 gdi32 user32)
        # PUBLIC, not PRIVATE: QOpenGLDynamicGDI.h is a PUBLIC header and it
        # includes <wglext.h>, so every consumer that pulls it in -- Canvas
        # code in CoreClient does -- has to be able to resolve it too.
        target_include_directories(GDI PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/External/wglext/include>)
    endif()
endif()

if(GDI_WITH_METAL)
    # Metal      -- device, queues, pipelines, encoders.
    # QuartzCore -- CAMetalLayer, the present surface.
    # Foundation -- NSString/NSError throughout the .mm sources.
    # AppKit/UIKit -- NSView/UIView, to reach the window's backing layer.
    target_link_libraries(GDI PUBLIC
            "-framework Metal"
            "-framework QuartzCore"
            "-framework Foundation"
    )
    if(IOS)
        target_link_libraries(GDI PUBLIC "-framework UIKit")
    else()
        target_link_libraries(GDI PUBLIC "-framework AppKit")
    endif()

    # The backend manages Objective-C lifetimes explicitly (MTLRetained /
    # retain-release), matching Vulkan's explicit handle management. ARC must
    # stay off or every one of those calls becomes a compile error.
    foreach(MM_SOURCE IN LISTS GDI_METAL_SOURCES)
        if(MM_SOURCE MATCHES "\\.mm$")
            set_source_files_properties("${MM_SOURCE}" PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
        endif()
    endforeach()
endif()

# --- TRANSITIONAL: Qt ------------------------------------------------------
# GDI no longer contains a single Q_OBJECT -- GDIContext was the last one --
# so AUTOMOC is off in both branches below. What remains here is the Qt LINK,
# which the backends still need for QWindow/QVulkanInstance until Steps 12-13.
# GDI is Qt-free. AUTOMOC/UIC/RCC are off because there is no Q_OBJECT left in
# the project, and Qt is neither included nor linked -- the only Qt in this
# tree is the optional GDIQtBridge target below.
set_target_properties(GDI PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# ===========================================================================
# Shader cooking
# ===========================================================================
# Moved here verbatim from CoreClient. Only the meaning of
# CMAKE_CURRENT_SOURCE_DIR changes, which is exactly right:
# cook_shaders.py derives its paths from Path(__file__).parent and
# SCRIPT_DIR.parent / "Shaders", so moving the script and the shaders together
# is self-consistent and the script itself needs no edit.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(GDI_COOKED_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked" CACHE INTERNAL "GDI cooked shader output root")

if(GDI_WITH_VULKAN)
    file(GLOB VULKAN_SHADER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.vsh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.fsh"
    )

    set(VULKAN_SPV_OUTPUTS "")
    foreach(SRC IN LISTS VULKAN_SHADER_SOURCES)
        get_filename_component(FNAME "${SRC}" NAME)
        list(APPEND VULKAN_SPV_OUTPUTS
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked/Vulkan/${FNAME}.spv"
        )
    endforeach()

    add_custom_command(
        OUTPUT  ${VULKAN_SPV_OUTPUTS}
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_shaders.py"
                -target Vulkan
        DEPENDS ${VULKAN_SHADER_SOURCES}
                "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_shaders.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Cooking Vulkan shaders (SPIR-V)"
        VERBATIM
    )

    add_custom_target(CookVulkanShaders DEPENDS ${VULKAN_SPV_OUTPUTS})
    add_dependencies(GDI CookVulkanShaders)
endif()

if(WIN32 AND GDI_WITH_OPENGL)
    file(GLOB OPENGL_SHADER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.vsh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.fsh"
    )

    set(OPENGL_COOKED_OUTPUTS "")
    foreach(SRC IN LISTS OPENGL_SHADER_SOURCES)
        get_filename_component(FNAME "${SRC}" NAME)
        list(APPEND OPENGL_COOKED_OUTPUTS
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked/OpenGL/${FNAME}"
        )
    endforeach()

    add_custom_command(
        OUTPUT  ${OPENGL_COOKED_OUTPUTS}
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_shaders.py"
                -target OpenGL
        DEPENDS ${OPENGL_SHADER_SOURCES}
                "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_shaders.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Cooking OpenGL shaders"
        VERBATIM
    )

    add_custom_target(CookOpenGLShaders DEPENDS ${OPENGL_COOKED_OUTPUTS})
    add_dependencies(GDI CookOpenGLShaders)
endif()

# The Metal cook TRANSPILES the Vulkan SPIR-V to MSL with spirv-cross, so it
# needs both the .spv outputs and the spirv-cross binary. Neither is available
# on a Mac without the Vulkan SDK, and the cooked .metal files are checked in,
# so the target is only wired up when the toolchain is actually present.
if(GDI_WITH_METAL)
    find_program(SPIRV_CROSS_EXECUTABLE spirv-cross
        HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")

    if(SPIRV_CROSS_EXECUTABLE AND GDI_WITH_VULKAN)
        file(GLOB METAL_SHADER_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.vsh"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.fsh"
        )

        set(METAL_COOKED_OUTPUTS "")
        foreach(SRC IN LISTS METAL_SHADER_SOURCES)
            get_filename_component(FNAME "${SRC}" NAME)
            list(APPEND METAL_COOKED_OUTPUTS
                "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked/Metal/${FNAME}.metal"
            )
        endforeach()

        add_custom_command(
            OUTPUT  ${METAL_COOKED_OUTPUTS}
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_shaders.py"
                    -target Metal
            DEPENDS ${VULKAN_SPV_OUTPUTS}
                    "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/cook_shaders.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Cooking Metal shaders (SPIR-V -> MSL)"
            VERBATIM
        )

        add_custom_target(CookMetalShaders DEPENDS ${METAL_COOKED_OUTPUTS})
        add_dependencies(GDI CookMetalShaders)
        message("Metal shaders will be cooked with ${SPIRV_CROSS_EXECUTABLE}")
    else()
        message("spirv-cross not found (or Vulkan cook disabled) -- using the committed Shaders/Cooked/Metal output.")
    endif()
endif()

# --- TRANSITIONAL: cooked-shader .qrc bundles ------------------------------
# GDI's shaders are still delivered to the running app as Qt resources, so the
# .qrc files have to be compiled by something with AUTORCC on -- which is
# CoreClient. Exported here so CoreClient does not hardcode paths into GDI's
# tree. Deleted once shader loading moves to IFileProvider and the cooked
# directory is simply copied next to the binary.
set(GDI_SHADER_QRC_FILES "")
if(GDI_WITH_OPENGL)
    list(APPEND GDI_SHADER_QRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked/OpenGL/OpenGLShaders_Cooked.qrc")
endif()
if(GDI_WITH_VULKAN)
    list(APPEND GDI_SHADER_QRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked/Vulkan/VulkanShaders_Cooked.qrc")
endif()
if(GDI_WITH_METAL)
    list(APPEND GDI_SHADER_QRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Cooked/Metal/MetalShaders_Cooked.qrc")
endif()
set(GDI_SHADER_QRC_FILES "${GDI_SHADER_QRC_FILES}" CACHE INTERNAL "Cooked shader .qrc bundles, compiled by the host application")

# ===========================================================================
# GDIQtBridge -- the only target in this project allowed to include Qt
# ===========================================================================
if(GDI_WITH_QT_BRIDGE)
    add_subdirectory(Source/Bridge/Qt)
endif()

# ===========================================================================
# GDISmokeTest -- links GDI and EngineCore only, deliberately NOT the bridge.
# It is the acceptance test that cannot pass while any Qt include remains
# outside Bridge/.
# ===========================================================================
if(GDI_BUILD_SMOKETEST)
    add_executable(GDISmokeTest Tests/SmokeMain.cpp)
    target_link_libraries(GDISmokeTest PRIVATE GDI::GDI)
    set_target_properties(GDISmokeTest PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

source_group("GDI"          FILES ${GDI_CORE_SOURCES})
source_group("GDI\\Shaders" FILES ${GDI_SHADERCLASS_SOURCES})
source_group("GDI\\OpenGL"  FILES ${GDI_OPENGL_SOURCES})
source_group("GDI\\Vulkan"  FILES ${GDI_VULKAN_SOURCES})
source_group("GDI\\Metal"   FILES ${GDI_METAL_SOURCES})

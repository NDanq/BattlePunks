# ===========================================================================
# Sandbox -- the host executable for the engine layer.
#
# Added from Engine/CMakeLists.txt rather than from a repo-root CMakeLists, and
# deliberately so. CMAKE_RUNTIME_OUTPUT_DIRECTORY is set at Engine scope and is
# only inherited by targets created at or below it, so a Sandbox declared as a
# SIBLING of Engine would not land in Binaries/<Platform>_<Config> -- and on
# Windows that directory is the only thing that lets this exe resolve
# NimbleEngine.dll, EngineCore.dll and SDL3.dll, which must be its siblings.
#
# This does not reach into a vendored subtree, so the "deliberately thin" rule
# at the top of Engine/CMakeLists.txt does not apply: Games/ is neither
# Modules/Core nor Modules/GDI.
# ===========================================================================

add_executable(Sandbox SandboxMain.cpp)

# Same Qt-freedom the modules get from nimble_add_module(). Stated explicitly
# because add_executable() does not go through that function, and a parent
# scope that enabled AUTOMOC as a directory property would hand it down.
set_target_properties(Sandbox PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# One link is enough: nimble_add_module() gives NimbleEngine a PUBLIC include
# directory, PUBLIC cxx_std_17 and a PUBLIC EngineCore, so all three arrive
# transitively. GameWindow and GDI stay PRIVATE to the engine layer and are
# deliberately NOT visible here -- a game links the engine, not the renderer.
target_link_libraries(Sandbox PRIVATE NimbleEngine::NimbleEngine)

source_group("Sandbox" FILES SandboxMain.cpp)

cmake_minimum_required(VERSION 3.16)

project(GameCore VERSION 0.1 LANGUAGES C CXX)

# ===========================================================================
# GameCore -- gameplay: entities, components (an ECS built for cache-friendly
# iteration), a phased tick, delegates/events, and Lua scripting, on Lua 5.4.
#
# LANGUAGES C CXX, unlike GameWindow's CXX-only: Lua is a C library, and its
# sources are added to THIS project's own target list (Lua has no CMakeLists
# of its own -- unlike SDL, it is built by hand below), so C has to be enabled
# here rather than falling out of add_subdirectory() the way it does for SDL.
#
# Deliberately NOT standalone-configurable, matching GameWindow: GameCore lives
# in this repository and is free to use nimble_add_module().
# ===========================================================================

if(NOT COMMAND nimble_add_module)
    message(FATAL_ERROR
            "GameCore is configured through Engine/CMakeLists.txt, which provides "
            "nimble_add_module(). It has no standalone configure -- see the note at "
            "the top of this file and Modules/GameWindow's CMakeLists.txt for the "
            "same discipline applied there.")
endif()

option(GAMECORE_BUILD_SMOKETEST "Build GameCoreSmokeTest" OFF)

# === Vendored Lua ===========================================================
# Lua 5.4 lives in source form under External/, exactly as GameWindow vendors
# SDL3 under its own External/: the dependency sits with the one module that
# needs it, nothing engine-wide has to know Lua exists, there is no
# find_package and no FetchContent, and a fresh clone configures with no
# per-machine setup.
#
# Version spelled out rather than globbed, for the same reason
# GAMEWINDOW_SDL_DIR is: which Lua a given commit builds against is answerable
# from the source, not from whatever happens to be on disk. Bumping it is one
# edit here plus the matching path in this module's .gitignore.
set(GAMECORE_LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/lua-5.4.8")

if(NOT EXISTS "${GAMECORE_LUA_DIR}/src/lua.h")
    message(FATAL_ERROR
            "Vendored Lua not found at ${GAMECORE_LUA_DIR}. GameCore does not fall "
            "back to a system Lua -- see the Lua section of Modules/GameCore/README.md.")
endif()

# Lua ships no CMakeLists.txt of its own (unlike SDL) -- its own Makefile is
# make-specific and platform-switched by hand. Built directly here instead,
# from the library sources only: lua.c and luac.c are the STANDALONE
# INTERPRETER AND COMPILER'S OWN main()s and are excluded, listed explicitly
# rather than globbed so that a future Lua source added upstream does not
# silently join this target without a review of what it is.
set(GAMECORE_LUA_SOURCES
        External/lua-5.4.8/src/lapi.c
        External/lua-5.4.8/src/lauxlib.c
        External/lua-5.4.8/src/lbaselib.c
        External/lua-5.4.8/src/lcode.c
        External/lua-5.4.8/src/lcorolib.c
        External/lua-5.4.8/src/lctype.c
        External/lua-5.4.8/src/ldblib.c
        External/lua-5.4.8/src/ldebug.c
        External/lua-5.4.8/src/ldo.c
        External/lua-5.4.8/src/ldump.c
        External/lua-5.4.8/src/lfunc.c
        External/lua-5.4.8/src/lgc.c
        External/lua-5.4.8/src/linit.c
        External/lua-5.4.8/src/liolib.c
        External/lua-5.4.8/src/llex.c
        External/lua-5.4.8/src/lmathlib.c
        External/lua-5.4.8/src/lmem.c
        External/lua-5.4.8/src/loadlib.c
        External/lua-5.4.8/src/lobject.c
        External/lua-5.4.8/src/lopcodes.c
        External/lua-5.4.8/src/loslib.c
        External/lua-5.4.8/src/lparser.c
        External/lua-5.4.8/src/lstate.c
        External/lua-5.4.8/src/lstring.c
        External/lua-5.4.8/src/lstrlib.c
        External/lua-5.4.8/src/ltable.c
        External/lua-5.4.8/src/ltablib.c
        External/lua-5.4.8/src/ltm.c
        External/lua-5.4.8/src/lundump.c
        External/lua-5.4.8/src/lutf8lib.c
        External/lua-5.4.8/src/lvm.c
        External/lua-5.4.8/src/lzio.c
)

if(NOT TARGET Lua::Lua)
    add_library(Lua STATIC ${GAMECORE_LUA_SOURCES})
    add_library(Lua::Lua ALIAS Lua)

    target_include_directories(Lua PUBLIC "${GAMECORE_LUA_DIR}/src")

    # luaconf.h auto-detects _WIN32 into LUA_USE_WINDOWS on its own (see the
    # #if defined(_WIN32) block near its top), so nothing is defined for
    # Windows here. LUA_USE_LINUX / LUA_USE_MACOSX are NOT auto-detected the
    # same way and must be requested explicitly to get dlopen-based
    # require("lib") support, readline-backed line editing in a REPL (this
    # engine never builds lua.c, so that half does not apply) and the POSIX
    # extras luaconf.h gates behind them.
    if(UNIX AND NOT APPLE)
        target_compile_definitions(Lua PRIVATE LUA_USE_LINUX)
        target_link_libraries(Lua PRIVATE m ${CMAKE_DL_LIBS})
    elseif(APPLE)
        target_compile_definitions(Lua PRIVATE LUA_USE_MACOSX)
    endif()

    # lprefix.h already #defines _CRT_SECURE_NO_WARNINGS itself for every
    # translation unit that includes it (which is all of them -- every Lua .c
    # file starts with #define LUA_CORE/LUA_LIB then includes lprefix.h
    # indirectly through lua.h), so this is a defensive belt-and-braces rather
    # than a fix for an observed warning: nothing here should be silently
    # relying on a lucky include order.
    if(MSVC)
        target_compile_definitions(Lua PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    # Lua links into GameCore.dll (a SHARED module -- see the note on
    # nimble_add_module() below), so its objects must be position-independent
    # on a platform that distinguishes PIC from non-PIC code. A no-op on
    # Windows and harmless to set unconditionally elsewhere.
    set_target_properties(Lua PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    source_group("External/Lua" FILES ${GAMECORE_LUA_SOURCES})
endif()

# === Vendored pugixml (uncooked builds) =====================================
# Text worlds and prefabs are XML (WorldPackage.h). Every build but Shipping
# reads them (WITH_UNCOOKED); Shipping links none of it. Built by hand, like Lua.
# PUGIXML_NO_XPATH: unused, and most of the library.
set(GAMECORE_PUGIXML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/pugixml-1.16")

if(NOT TARGET PugiXml::PugiXml)
    if(NOT EXISTS "${GAMECORE_PUGIXML_DIR}/src/pugixml.hpp")
        message(FATAL_ERROR "Vendored pugixml not found at ${GAMECORE_PUGIXML_DIR}.")
    endif()

    # EXCLUDE_FROM_ALL: built only where linked, so never for Shipping.
    add_library(PugiXml STATIC EXCLUDE_FROM_ALL "${GAMECORE_PUGIXML_DIR}/src/pugixml.cpp")
    add_library(PugiXml::PugiXml ALIAS PugiXml)
    target_include_directories(PugiXml PUBLIC "${GAMECORE_PUGIXML_DIR}/src")
    target_compile_definitions(PugiXml PUBLIC PUGIXML_NO_XPATH)
    target_compile_features(PugiXml PUBLIC cxx_std_17)

    # Linked into GameCore.dll, so position-independent for Lua's reason.
    set_target_properties(PugiXml PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )
    source_group("External/pugixml" FILES "${GAMECORE_PUGIXML_DIR}/src/pugixml.cpp")
endif()

# ===========================================================================
# Sources
# ===========================================================================

set(GAMECORE_SOURCES
        Source/Private/GameCore/GameCoreInternal.h
        Source/Private/GameCore/ComponentType.cpp
        Source/Private/GameCore/ComponentPool.cpp
        Source/Private/GameCore/Components/TransformComponent.cpp
        Source/Private/GameCore/Components/BoundsComponent.cpp
        Source/Private/GameCore/Components/CollisionComponent.cpp
        Source/Private/GameCore/Components/NetComponent.cpp
        Source/Private/GameCore/Components/RigidBodyComponent.cpp
        Source/Private/GameCore/Components/PhysicsConstraintComponent.cpp
        Source/Private/GameCore/Components/VehicleComponent.cpp
        Source/Private/GameCore/Delegate.cpp
        Source/Private/GameCore/EventBus.cpp
        Source/Private/GameCore/TickPhase.cpp
        Source/Private/GameCore/SpatialIndex.cpp
        Source/Private/GameCore/Collision.cpp
        Source/Private/GameCore/World.cpp
        Source/Private/GameCore/WorldSettings.cpp
        Source/Private/GameCore/ScriptEngine.cpp
        Source/Private/GameCore/GraphText.cpp
        Source/Private/GameCore/ScriptBindings.cpp
        Source/Private/GameCore/ScriptSystem.cpp
        Source/Private/GameCore/ComponentSerializer.cpp
        Source/Private/GameCore/WorldSerializer.cpp
        Source/Private/GameCore/WorldPackage.cpp
        Source/Private/GameCore/Prefab.cpp
        Source/Private/GameCore/NativePrefab.cpp
        Source/Private/GameCore/ArchiveTextDump.cpp
        Source/Private/GameCore/ComponentScript.cpp
        Source/Private/GameCore/ScriptLibrary.cpp
        Source/Private/GameCore/ConfigScriptLibrary.cpp
        Source/Private/GameCore/ScriptStruct.cpp
        Source/Private/GameCore/ComponentProperties.cpp
        Source/Private/GameCore/ComponentReplication.cpp
        Source/Private/GameCore/ComponentRpc.cpp
        Source/Public/GameCore/GameCoreAPI.h
        Source/Public/GameCore/GameCoreLog.h
        Source/Public/GameCore/EntityID.h
        Source/Public/GameCore/EntityGuid.h
        Source/Public/GameCore/AABB.h
        Source/Public/GameCore/SpatialIndex.h
        Source/Public/GameCore/TickPhase.h
        Source/Public/GameCore/Mobility.h
        Source/Public/GameCore/Delegate.h
        Source/Public/GameCore/EventBus.h
        Source/Public/GameCore/ComponentType.h
        Source/Public/GameCore/ComponentPool.h
        Source/Public/GameCore/World.h
        Source/Public/GameCore/WorldSettings.h
        Source/Public/GameCore/Entity.h
        Source/Public/GameCore/Components.h
        Source/Public/GameCore/Components/TransformComponent.h
        Source/Public/GameCore/Components/BoundsComponent.h
        Source/Public/GameCore/Components/CollisionComponent.h
        Source/Public/GameCore/Components/NetComponent.h
        Source/Public/GameCore/Components/RigidBodyComponent.h
        Source/Public/GameCore/Components/PhysicsConstraintComponent.h
        Source/Public/GameCore/Components/VehicleComponent.h
        Source/Public/GameCore/TransformMath.h
        Source/Public/GameCore/ScriptEngine.h
        Source/Public/GameCore/ComponentSerializer.h
        Source/Public/GameCore/WorldSerializer.h
        Source/Public/GameCore/WorldPackage.h
        Source/Public/GameCore/GraphText.h
        Source/Public/GameCore/ArchiveTextDump.h
        Source/Public/GameCore/ComponentScript.h
        Source/Public/GameCore/ScriptLibrary.h
        Source/Public/GameCore/NativePrefab.h
        Source/Public/GameCore/ScriptStruct.h
        Source/Public/GameCore/ComponentProperties.h
        Source/Public/GameCore/ComponentReplication.h
        Source/Public/GameCore/ComponentRpc.h
)

# ===========================================================================
# The GameCore target
# ===========================================================================
#
# ALWAYS SHARED -- there is no GAMECORE_SHARED option, unlike GameWindow's.
# ComponentType.h and EventBus.h hold their registries in fixed, lazily-filled
# tables behind function-local statics and out-of-line GAMECORE_API functions;
# a STATIC GameCore pulled PUBLIC through NimbleEngine.dll would give
# NimbleEngine.dll and Sandbox.exe each their OWN copy of every registry, so
# TransformComponent would register once per image and get a DIFFERENT id in
# each -- with no diagnostic anywhere, just a component read at the wrong
# offset the first time an id crossed the boundary. See GameCoreAPI.h's
# comment for the same argument in more detail; this is the CORE_SHARED /
# NimbleEngine-SHARED reasoning from Engine/CMakeLists.txt, applied here.
#
# Lua::Lua is PRIVATE: no public GameCore header includes lua.h --
# GameCoreInternal.h is the only one that does, and it lives under Private/
# (see its own comment for why that is load-bearing, matching
# GameWindowInternal.h's SDL.h discipline) -- lua_State is forward-declared in
# ScriptEngine.h and returned as an opaque pointer. GameCoreSmokeTest links
# GameCore and nothing else; if it ever fails to link because something needs
# a Lua symbol, that is this property breaking.
nimble_add_module(GameCore SHARED
        PREFIX       GAMECORE
        SOURCES      ${GAMECORE_SOURCES}
        PRIVATE_DEPS Lua::Lua)

# PRIVATE, like Lua: no public header includes pugixml.hpp.
if(TARGET PugiXml::PugiXml)
    target_link_libraries(GameCore PRIVATE $<$<NOT:$<CONFIG:Shipping>>:PugiXml::PugiXml>)
endif()

# PRIVATE, not PUBLIC: GameCoreInternal.h is included as
# "Private/GameCore/GameCoreInternal.h" from this module's own .cpp files and
# must not be reachable from a consumer. nimble_add_module already added
# Source/ PUBLIC for the "Public/GameCore/..." includes.
target_include_directories(GameCore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)

if(GAMECORE_BUILD_SMOKETEST)
    add_executable(GameCoreSmokeTest Tests/SmokeMain.cpp)
    target_link_libraries(GameCoreSmokeTest PRIVATE GameCore::GameCore)
    set_target_properties(GameCoreSmokeTest PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

    enable_testing()
    add_test(NAME GameCore.Smoke COMMAND GameCoreSmokeTest)
    set_tests_properties(GameCore.Smoke PROPERTIES
        TIMEOUT 120
        LABELS  "smoke"
    )
endif()

source_group("GameCore" FILES ${GAMECORE_SOURCES})

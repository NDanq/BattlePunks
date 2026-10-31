cmake_minimum_required(VERSION 3.16)

project(GameCore VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# GameCore -- gameplay: entities, components (an ECS built for cache-friendly
# iteration), a phased tick, delegates/events, and Lua scripting, on Lua 5.4.
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

# === Lua and pugixml ========================================================
# Lua 5.4.8 (Lua::Lua) and pugixml 1.16 (PugiXml::PugiXml, for the text worlds
# and prefabs every build but Shipping reads) are vendored at
# Engine/ThirdParty/Source and built by Engine/ThirdParty/CMakeLists.txt.
# No find_package and no fallback to a system Lua.

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
        Source/Private/GameCore/Components/AudioSourceComponent.cpp
        Source/Private/GameCore/Components/AudioListenerComponent.cpp
        Source/Private/GameCore/Components/SplineComponent.cpp
        Source/Private/GameCore/Delegate.cpp
        Source/Private/GameCore/EventBus.cpp
        Source/Private/GameCore/TickPhase.cpp
        Source/Private/GameCore/SpatialIndex.cpp
        Source/Private/GameCore/Collision.cpp
        Source/Private/GameCore/Terrain.cpp
        Source/Private/GameCore/Foliage.cpp
        Source/Private/GameCore/Spline.cpp
        Source/Private/GameCore/TerrainBrush.cpp
        Source/Private/GameCore/World.cpp
        Source/Private/GameCore/WorldSettings.cpp
        Source/Private/GameCore/Scalability.cpp
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
        Source/Public/GameCore/Terrain.h
        Source/Public/GameCore/Foliage.h
        Source/Public/GameCore/Spline.h
        Source/Public/GameCore/TerrainBrush.h
        Source/Public/GameCore/SpatialIndex.h
        Source/Public/GameCore/TickPhase.h
        Source/Public/GameCore/DetailMode.h
        Source/Public/GameCore/Mobility.h
        Source/Public/GameCore/Delegate.h
        Source/Public/GameCore/EventBus.h
        Source/Public/GameCore/ComponentType.h
        Source/Public/GameCore/ComponentPool.h
        Source/Public/GameCore/World.h
        Source/Public/GameCore/WorldSettings.h
        Source/Public/GameCore/Scalability.h
        Source/Public/GameCore/Entity.h
        Source/Public/GameCore/Components.h
        Source/Public/GameCore/Components/TransformComponent.h
        Source/Public/GameCore/Components/BoundsComponent.h
        Source/Public/GameCore/Components/CollisionComponent.h
        Source/Public/GameCore/Components/NetComponent.h
        Source/Public/GameCore/Components/RigidBodyComponent.h
        Source/Public/GameCore/Components/PhysicsConstraintComponent.h
        Source/Public/GameCore/Components/VehicleComponent.h
        Source/Public/GameCore/Components/AudioSourceComponent.h
        Source/Public/GameCore/Components/AudioListenerComponent.h
        Source/Public/GameCore/Components/SplineComponent.h
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

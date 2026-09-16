#ifndef GAMECORE_GAMECOREAPI_H
#define GAMECORE_GAMECOREAPI_H

/*
 * Symbol visibility for GameCore.
 *
 * The same three states CoreAPI.h documents, for the same reasons:
 *
 *   GAMECORE_SHARED_BUILD undefined         -> GAMECORE_API is empty (static)
 *   GAMECORE_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   GAMECORE_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * UNLIKE GameWindow, this is not a choice a game gets to make: CMakeLists.txt
 * calls nimble_add_module(GameCore SHARED ...) unconditionally, with no
 * GAMECORE_SHARED option. Component and event names are held in fixed-size
 * registries (ComponentType.h, EventBus.h) built from lazily-run, function-local
 * statics. A STATIC GameCore pulled PUBLIC through NimbleEngine.dll would give
 * NimbleEngine.dll and Sandbox.exe each their own copy of every registry, so
 * TransformComponent would register once per image and get two different ids --
 * with no diagnostic anywhere, just component data read at the wrong offset the
 * first time an id crossed the boundary. Exactly the reasoning
 * Engine/CMakeLists.txt gives for forcing CORE_SHARED and building NimbleEngine
 * itself SHARED.
 *
 * nimble_add_module() sets GAMECORE_SHARED_BUILD PUBLIC and GAMECORE_BUILDING
 * PRIVATE on the target, so the two states fall out of the link graph and nobody
 * has to remember which one they are in.
 *
 * Annotate types with out-of-line virtuals and free functions. Do NOT annotate
 * the header-only POD structs (EntityID.h, AABB.h, TickPhase.h, Entity.h) --
 * they have no out-of-line members, they instantiate in the consumer, and
 * dllimport on them buys C4251 noise at best.
 */

#if defined(_WIN32)
    #define GAMECORE_API_EXPORT __declspec(dllexport)
    #define GAMECORE_API_IMPORT __declspec(dllimport)
    #define GAMECORE_API_LOCAL
#else
    #define GAMECORE_API_EXPORT __attribute__((visibility("default")))
    #define GAMECORE_API_IMPORT __attribute__((visibility("default")))
    #define GAMECORE_API_LOCAL  __attribute__((visibility("hidden")))
#endif

#ifdef GAMECORE_SHARED_BUILD
    #ifdef GAMECORE_BUILDING
        #define GAMECORE_API GAMECORE_API_EXPORT
    #else
        #define GAMECORE_API GAMECORE_API_IMPORT
    #endif
#else
    #define GAMECORE_API
#endif

#endif //GAMECORE_GAMECOREAPI_H

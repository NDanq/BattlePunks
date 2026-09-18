#ifndef GAMECORE_GAMECOREAPI_H
#define GAMECORE_GAMECOREAPI_H

/*
 * Symbol visibility for GameCore. The same three states CoreAPI.h documents,
 * for the same reasons:
 *
 *   GAMECORE_SHARED_BUILD undefined         -> GAMECORE_API is empty (static)
 *   GAMECORE_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   GAMECORE_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * ALWAYS SHARED, unlike GameWindow: component and event names live in
 * fixed-size registries (ComponentType.h, EventBus.h) built from function-local
 * statics. Built STATIC and pulled PUBLIC through NimbleEngine.dll, each image
 * would get its own copy, so TransformComponent would register once per image
 * and get two different ids -- no diagnostic, just component data read at the
 * wrong offset the first time an id crossed the boundary. CMakeLists.txt
 * therefore calls nimble_add_module(GameCore SHARED ...) with no opt-out,
 * setting GAMECORE_SHARED_BUILD PUBLIC and GAMECORE_BUILDING PRIVATE so the
 * state falls out of the link graph.
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

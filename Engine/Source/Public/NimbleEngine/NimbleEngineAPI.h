#ifndef NIMBLEENGINE_NIMBLEENGINEAPI_H
#define NIMBLEENGINE_NIMBLEENGINEAPI_H

/*
 * Symbol visibility for NimbleEngine.
 *
 * The same three states CoreAPI.h and GameWindowAPI.h document, for the same
 * reasons:
 *
 *   NIMBLEENGINE_SHARED_BUILD undefined       -> NE_API is empty (static)
 *   NIMBLEENGINE_SHARED_BUILD + _BUILDING     -> dllexport (building the DLL)
 *   NIMBLEENGINE_SHARED_BUILD alone           -> dllimport (consuming the DLL)
 *
 * nimble_add_module() sets NIMBLEENGINE_SHARED_BUILD PUBLIC and
 * NIMBLEENGINE_BUILDING PRIVATE on the target, so the two states fall out of the
 * link graph and nobody has to remember which one they are in.
 *
 * This matters more here than in a leaf module: Engine/CMakeLists.txt builds
 * NimbleEngine SHARED so that a hot-reloaded plugin calling back into the engine
 * layer sees ONE copy of it, for the same reason CORE_SHARED is forced ON. An
 * engine symbol a plugin needs and that is not annotated is a load-time failure
 * in the plugin, not a build error here.
 *
 * Annotate types with out-of-line virtuals and free functions. Do NOT annotate
 * header-only POD structs -- they have no out-of-line members, they instantiate
 * in the consumer, and dllimport on them buys C4251 noise at best.
 */

#if defined(_WIN32)
    #define NIMBLEENGINE_API_EXPORT __declspec(dllexport)
    #define NIMBLEENGINE_API_IMPORT __declspec(dllimport)
    #define NIMBLEENGINE_API_LOCAL
#else
    #define NIMBLEENGINE_API_EXPORT __attribute__((visibility("default")))
    #define NIMBLEENGINE_API_IMPORT __attribute__((visibility("default")))
    #define NIMBLEENGINE_API_LOCAL  __attribute__((visibility("hidden")))
#endif

#ifdef NIMBLEENGINE_SHARED_BUILD
    #ifdef NIMBLEENGINE_BUILDING
        #define NE_API NIMBLEENGINE_API_EXPORT
    #else
        #define NE_API NIMBLEENGINE_API_IMPORT
    #endif
#else
    #define NE_API
#endif

#endif //NIMBLEENGINE_NIMBLEENGINEAPI_H

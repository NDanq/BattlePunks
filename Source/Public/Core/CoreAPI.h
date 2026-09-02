#ifndef ENGINECORE_COREAPI_H
#define ENGINECORE_COREAPI_H

/*
 * Symbol visibility for EngineCore.
 *
 * Three states:
 *   CORE_SHARED_BUILD undefined      -> CORE_API is empty (static library)
 *   CORE_SHARED_BUILD + CORE_BUILDING -> dllexport   (building the DLL)
 *   CORE_SHARED_BUILD alone           -> dllimport   (consuming the DLL)
 *
 * CORE_SHARED_BUILD is PUBLIC on the target, CORE_BUILDING is PRIVATE, so the
 * two states fall out of the link graph without anyone having to remember.
 *
 * Annotate types with out-of-line virtuals and free functions. Do NOT annotate
 * header-only inline structs or templates -- they instantiate in the consumer,
 * and dllimport on them produces C4251 noise at best and link errors at worst.
 */

#if defined(_WIN32)
    #define CORE_API_EXPORT __declspec(dllexport)
    #define CORE_API_IMPORT __declspec(dllimport)
    #define CORE_API_LOCAL
#else
    #define CORE_API_EXPORT __attribute__((visibility("default")))
    #define CORE_API_IMPORT __attribute__((visibility("default")))
    #define CORE_API_LOCAL  __attribute__((visibility("hidden")))
#endif

#ifdef CORE_SHARED_BUILD
    #ifdef CORE_BUILDING
        #define CORE_API CORE_API_EXPORT
    #else
        #define CORE_API CORE_API_IMPORT
    #endif
#else
    #define CORE_API
#endif

#endif //ENGINECORE_COREAPI_H

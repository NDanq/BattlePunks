#ifndef JOLTPHYSICS_JOLTPHYSICSAPI_H
#define JOLTPHYSICS_JOLTPHYSICSAPI_H

/*
 * Symbol visibility for JoltPhysics -- the three states CoreAPI.h documents:
 *
 *   JOLTPHYSICS_SHARED_BUILD undefined         -> JOLTPHYSICS_API is empty (static)
 *   JOLTPHYSICS_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   JOLTPHYSICS_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * Always SHARED (CMakeLists.txt): one Jolt runtime -- its factory, type
 * registry and job system -- per process, as GameCore's registries are.
 */

#if defined(_WIN32)
    #define JOLTPHYSICS_API_EXPORT __declspec(dllexport)
    #define JOLTPHYSICS_API_IMPORT __declspec(dllimport)
#else
    #define JOLTPHYSICS_API_EXPORT __attribute__((visibility("default")))
    #define JOLTPHYSICS_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef JOLTPHYSICS_SHARED_BUILD
    #ifdef JOLTPHYSICS_BUILDING
        #define JOLTPHYSICS_API JOLTPHYSICS_API_EXPORT
    #else
        #define JOLTPHYSICS_API JOLTPHYSICS_API_IMPORT
    #endif
#else
    #define JOLTPHYSICS_API
#endif

/* Only configured when it is on; a JoltPhysics header in a WITH_JOLTPHYSICS=0
 * build is an include that should have been guarded. */
#if !defined(WITH_JOLTPHYSICS) || !WITH_JOLTPHYSICS
    #error "JoltPhysics headers need WITH_JOLTPHYSICS=1 -- guard the include with #if WITH_JOLTPHYSICS"
#endif

#endif //JOLTPHYSICS_JOLTPHYSICSAPI_H

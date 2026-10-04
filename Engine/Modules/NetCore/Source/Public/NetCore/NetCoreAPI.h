#ifndef NETCORE_NETCOREAPI_H
#define NETCORE_NETCOREAPI_H

/*
 * Symbol visibility for NetCore -- the three states CoreAPI.h documents:
 *
 *   NETCORE_SHARED_BUILD undefined         -> NETCORE_API is empty (static)
 *   NETCORE_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   NETCORE_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * Always SHARED (CMakeLists.txt): one session and one installed RPC sink per
 * process, as GameCore's registries are one per process.
 */

#if defined(_WIN32)
    #define NETCORE_API_EXPORT __declspec(dllexport)
    #define NETCORE_API_IMPORT __declspec(dllimport)
#else
    #define NETCORE_API_EXPORT __attribute__((visibility("default")))
    #define NETCORE_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef NETCORE_SHARED_BUILD
    #ifdef NETCORE_BUILDING
        #define NETCORE_API NETCORE_API_EXPORT
    #else
        #define NETCORE_API NETCORE_API_IMPORT
    #endif
#else
    #define NETCORE_API
#endif

/* Only configured when it is on; a NetCore header in a WITH_NETCORE=0 build
 * is an include that should have been guarded. */
#if !defined(WITH_NETCORE) || !WITH_NETCORE
    #error "NetCore headers need WITH_NETCORE=1 -- guard the include with #if WITH_NETCORE"
#endif

#endif //NETCORE_NETCOREAPI_H

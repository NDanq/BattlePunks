#ifndef AICORE_AICOREAPI_H
#define AICORE_AICOREAPI_H

/*
 * Symbol visibility for AICore -- the three states CoreAPI.h documents:
 *
 *   AICORE_SHARED_BUILD undefined         -> AICORE_API is empty (static)
 *   AICORE_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   AICORE_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * Always SHARED (CMakeLists.txt): one behavior tree node registry, one asset
 * cache and one AIWorld per World in the process.
 */

#if defined(_WIN32)
    #define AICORE_API_EXPORT __declspec(dllexport)
    #define AICORE_API_IMPORT __declspec(dllimport)
#else
    #define AICORE_API_EXPORT __attribute__((visibility("default")))
    #define AICORE_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef AICORE_SHARED_BUILD
    #ifdef AICORE_BUILDING
        #define AICORE_API AICORE_API_EXPORT
    #else
        #define AICORE_API AICORE_API_IMPORT
    #endif
#else
    #define AICORE_API
#endif

#endif //AICORE_AICOREAPI_H

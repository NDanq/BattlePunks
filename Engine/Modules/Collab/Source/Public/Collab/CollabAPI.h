#ifndef COLLAB_COLLABAPI_H
#define COLLAB_COLLABAPI_H

/*
 * Symbol visibility for Collab -- the three states CoreAPI.h documents:
 *
 *   COLLAB_SHARED_BUILD undefined         -> COLLAB_API is empty (static)
 *   COLLAB_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   COLLAB_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * Built STATIC today (CMakeLists.txt), so COLLAB_API is empty; the macro is
 * on every cross-module symbol so a switch to SHARED needs no source change.
 */

#if defined(_WIN32)
    #define COLLAB_API_EXPORT __declspec(dllexport)
    #define COLLAB_API_IMPORT __declspec(dllimport)
#else
    #define COLLAB_API_EXPORT __attribute__((visibility("default")))
    #define COLLAB_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef COLLAB_SHARED_BUILD
    #ifdef COLLAB_BUILDING
        #define COLLAB_API COLLAB_API_EXPORT
    #else
        #define COLLAB_API COLLAB_API_IMPORT
    #endif
#else
    #define COLLAB_API
#endif

#endif //COLLAB_COLLABAPI_H

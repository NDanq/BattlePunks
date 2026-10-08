#ifndef AUDIO_AUDIOAPI_H
#define AUDIO_AUDIOAPI_H

/*
 * Symbol visibility for Audio -- the three states CoreAPI.h documents:
 *
 *   AUDIO_SHARED_BUILD undefined         -> AUDIO_API is empty (static)
 *   AUDIO_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   AUDIO_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * Always SHARED (CMakeLists.txt): one output device and mixer per process.
 */

#if defined(_WIN32)
    #define AUDIO_API_EXPORT __declspec(dllexport)
    #define AUDIO_API_IMPORT __declspec(dllimport)
#else
    #define AUDIO_API_EXPORT __attribute__((visibility("default")))
    #define AUDIO_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef AUDIO_SHARED_BUILD
    #ifdef AUDIO_BUILDING
        #define AUDIO_API AUDIO_API_EXPORT
    #else
        #define AUDIO_API AUDIO_API_IMPORT
    #endif
#else
    #define AUDIO_API
#endif

/* Only configured when it is on; an Audio header in a WITH_AUDIO=0 build is
 * an include that should have been guarded. */
#if !defined(WITH_AUDIO) || !WITH_AUDIO
    #error "Audio headers need WITH_AUDIO=1 -- guard the include with #if WITH_AUDIO"
#endif

#endif //AUDIO_AUDIOAPI_H

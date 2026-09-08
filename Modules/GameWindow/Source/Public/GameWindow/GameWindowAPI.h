#ifndef GAMEWINDOW_GAMEWINDOWAPI_H
#define GAMEWINDOW_GAMEWINDOWAPI_H

/*
 * Symbol visibility for GameWindow.
 *
 * The same three states CoreAPI.h documents, for the same reasons:
 *
 *   GAMEWINDOW_SHARED_BUILD undefined         -> GAMEWINDOW_API is empty (static)
 *   GAMEWINDOW_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   GAMEWINDOW_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * nimble_add_module() sets GAMEWINDOW_SHARED_BUILD PUBLIC and GAMEWINDOW_BUILDING
 * PRIVATE on the target, so the two states fall out of the link graph and nobody
 * has to remember which one they are in.
 *
 * Annotate types with out-of-line virtuals and free functions. Do NOT annotate
 * the header-only POD structs in GameWindowTypes.h, GameWindowInput.h or
 * GameWindowDelegates.h -- they have no out-of-line members, they instantiate
 * in the consumer, and dllimport on them buys C4251 noise at best.
 */

#if defined(_WIN32)
    #define GAMEWINDOW_API_EXPORT __declspec(dllexport)
    #define GAMEWINDOW_API_IMPORT __declspec(dllimport)
    #define GAMEWINDOW_API_LOCAL
#else
    #define GAMEWINDOW_API_EXPORT __attribute__((visibility("default")))
    #define GAMEWINDOW_API_IMPORT __attribute__((visibility("default")))
    #define GAMEWINDOW_API_LOCAL  __attribute__((visibility("hidden")))
#endif

#ifdef GAMEWINDOW_SHARED_BUILD
    #ifdef GAMEWINDOW_BUILDING
        #define GAMEWINDOW_API GAMEWINDOW_API_EXPORT
    #else
        #define GAMEWINDOW_API GAMEWINDOW_API_IMPORT
    #endif
#else
    #define GAMEWINDOW_API
#endif

#endif //GAMEWINDOW_GAMEWINDOWAPI_H

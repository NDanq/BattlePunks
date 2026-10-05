#ifndef UI_UIAPI_H
#define UI_UIAPI_H

/*
 * Symbol visibility for the UI module: the three states GameCoreAPI.h
 * documents, for the same reasons.
 *
 *   UI_SHARED_BUILD undefined      -> UI_API is empty (static)
 *   UI_SHARED_BUILD + UI_BUILDING  -> dllexport (building the DLL)
 *   UI_SHARED_BUILD alone          -> dllimport (consuming the DLL)
 *
 * ALWAYS SHARED: the widget type and native action registries must exist
 * exactly once, or a game module's widget would register into a copy the
 * engine never reads -- RenderCoreAPI.h's argument, for this module's tables.
 */

#if defined(_WIN32)
    #define UI_API_EXPORT __declspec(dllexport)
    #define UI_API_IMPORT __declspec(dllimport)
#else
    #define UI_API_EXPORT __attribute__((visibility("default")))
    #define UI_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef UI_SHARED_BUILD
    #ifdef UI_BUILDING
        #define UI_API UI_API_EXPORT
    #else
        #define UI_API UI_API_IMPORT
    #endif
#else
    #define UI_API
#endif

#ifndef WITH_EDITOR
    #error "WITH_EDITOR is not defined -- configure through Engine/CMakeLists.txt, or define it to 0 or 1"
#endif

#endif //UI_UIAPI_H

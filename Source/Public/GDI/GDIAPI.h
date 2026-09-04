#ifndef CORECLIENT_GDIAPI_H
#define CORECLIENT_GDIAPI_H

/*
 * Symbol visibility for GDI. Mirrors EngineCore's CoreAPI.h exactly -- see
 * that file for the three-state rationale.
 *
 *   GDI_SHARED_BUILD undefined     -> GDI_API is empty (static library)
 *   GDI_SHARED_BUILD + GDI_BUILDING -> dllexport
 *   GDI_SHARED_BUILD alone          -> dllimport
 *
 * WHAT TO ANNOTATE
 *   Yes: classes with out-of-line virtuals -- GDIDynamicDriver, GDIContext,
 *        GDIRenderTarget, GDITexture2D, GDIShader, GDIMesh, GDIView, GDISync
 *        -- and free functions. Globals (GDynamicGDI, GSync, GRenderCommands,
 *        GDebugGroupID) need `GDI_API extern`; those are the most likely
 *        unresolved externals the first time someone sets GDI_SHARED=ON.
 *
 *   No:  header-only inline structs (GDIDefinitions.h's state descs) and
 *        templates (RegisterShader/GetShader). Those instantiate in the
 *        consumer, where dllimport produces C4251 noise at best and link
 *        errors at worst.
 *
 * A shared build requires every module to use one toolchain and one CRT.
 */

#if defined(_WIN32)
    #define GDI_API_EXPORT __declspec(dllexport)
    #define GDI_API_IMPORT __declspec(dllimport)
    #define GDI_API_LOCAL
#else
    #define GDI_API_EXPORT __attribute__((visibility("default")))
    #define GDI_API_IMPORT __attribute__((visibility("default")))
    #define GDI_API_LOCAL  __attribute__((visibility("hidden")))
#endif

#ifdef GDI_SHARED_BUILD
    #ifdef GDI_BUILDING
        #define GDI_API GDI_API_EXPORT
    #else
        #define GDI_API GDI_API_IMPORT
    #endif
#else
    #define GDI_API
#endif

#endif //CORECLIENT_GDIAPI_H

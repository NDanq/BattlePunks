#ifndef RENDERCORE_RENDERCOREAPI_H
#define RENDERCORE_RENDERCOREAPI_H

/*
 * Symbol visibility for RenderCore. The same three states GameCoreAPI.h
 * documents, for the same reasons:
 *
 *   RENDERCORE_SHARED_BUILD undefined         -> RENDERCORE_API is empty (static)
 *   RENDERCORE_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   RENDERCORE_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * ALWAYS SHARED, unlike GameWindow: RenderCore owns process-global state that
 * must exist exactly once -- the render-command queue, the render-thread
 * identity behind IsInRenderThread(), and RegisterProxyType's proxy-type table.
 * Built STATIC and pulled PUBLIC through NimbleEngine.dll, each image would get
 * its own copy of all three, and a command enqueued from one would sit in a
 * queue the render thread never drains. CMakeLists.txt therefore calls
 * nimble_add_module(RenderCore SHARED ...) with no opt-out, setting
 * RENDERCORE_SHARED_BUILD PUBLIC and RENDERCORE_BUILDING PRIVATE so the state
 * falls out of the link graph.
 *
 * Annotate types with out-of-line virtuals and free functions. Do NOT annotate
 * a header-only POD struct with no out-of-line members (RenderTypes.h,
 * RenderView.h, RenderCommand.h's header) -- it instantiates in the consumer,
 * and dllimport on it buys C4251 noise at best.
 */

#if defined(_WIN32)
    #define RENDERCORE_API_EXPORT __declspec(dllexport)
    #define RENDERCORE_API_IMPORT __declspec(dllimport)
    #define RENDERCORE_API_LOCAL
#else
    #define RENDERCORE_API_EXPORT __attribute__((visibility("default")))
    #define RENDERCORE_API_IMPORT __attribute__((visibility("default")))
    #define RENDERCORE_API_LOCAL  __attribute__((visibility("hidden")))
#endif

#ifdef RENDERCORE_SHARED_BUILD
    #ifdef RENDERCORE_BUILDING
        #define RENDERCORE_API RENDERCORE_API_EXPORT
    #else
        #define RENDERCORE_API RENDERCORE_API_IMPORT
    #endif
#else
    #define RENDERCORE_API
#endif

/* Engine/CMakeLists.txt defines WITH_EDITOR as 1 or 0 for every target; an
 * undefined name would silently read as 0. */
#ifndef WITH_EDITOR
    #error "WITH_EDITOR is not defined -- configure through Engine/CMakeLists.txt, or define it to 0 or 1"
#endif

#endif //RENDERCORE_RENDERCOREAPI_H

#ifndef RENDERCORE_RENDERCOREAPI_H
#define RENDERCORE_RENDERCOREAPI_H

/*
 * Symbol visibility for RenderCore.
 *
 * The same three states GameCoreAPI.h documents, for the same reasons:
 *
 *   RENDERCORE_SHARED_BUILD undefined         -> RENDERCORE_API is empty (static)
 *   RENDERCORE_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   RENDERCORE_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * UNLIKE GameWindow, this is not a choice a game gets to make: CMakeLists.txt
 * calls nimble_add_module(RenderCore SHARED ...) unconditionally, with no
 * RENDERCORE_SHARED option. RenderCore owns process-global state that must
 * exist exactly once per process -- the render-command queue, the
 * render-thread identity behind IsInRenderThread(), and the proxy-type table
 * RegisterProxyType fills. A STATIC RenderCore pulled PUBLIC through
 * NimbleEngine.dll would give NimbleEngine.dll and Sandbox.exe each their own
 * copy of all three, so a command enqueued from one image would sit in a
 * queue the render thread (latched against the OTHER copy) never drains --
 * exactly the GameCoreAPI.h argument for GameCore's own registries, applied
 * here.
 *
 * nimble_add_module() sets RENDERCORE_SHARED_BUILD PUBLIC and RENDERCORE_BUILDING
 * PRIVATE on the target, so the two states fall out of the link graph and nobody
 * has to remember which one they are in.
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

#endif //RENDERCORE_RENDERCOREAPI_H

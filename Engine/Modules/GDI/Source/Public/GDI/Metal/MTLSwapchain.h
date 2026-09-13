#ifndef CORECLIENT_MTLSWAPCHAIN_H
#define CORECLIENT_MTLSWAPCHAIN_H


#include "Public/GDI/Metal/MTLForward.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>

class MTLDevice;
class MTLRenderTarget;

/**
 * UI-thread-owned present path -- the Metal counterpart of VKSwapchain.
 *
 * There is no swapchain object to create or recreate: a MetalSurface QWindow is
 * already backed by a CAMetalLayer, and the layer manages its own drawable pool.
 * All this class does is
 *   1. adopt the window's CAMetalLayer (lazily, on the first present, exactly as
 *      the Vulkan backend defers surface creation),
 *   2. own a self-contained Screen.vsh/.fsh blit pipeline targeting the layer's
 *      pixel format (the render-thread MTLPipelineCache targets the RT format and
 *      is not safe to touch from the UI thread),
 *   3. per present: acquire a drawable, draw the full-screen quad sampling the
 *      offscreen Main RT, and present.
 *
 * A window resize is just a new drawableSize -- none of the swapchain/semaphore
 * recreation the Vulkan path needs.
 *
 * Render/present ordering does NOT live here: both threads submit to the single
 * MTLCommandQueue owned by MTLDevice, and Metal orders and hazard-tracks command
 * buffers on a queue by commit order (see MetalDynamicGDI's class comment).
 */
class MTLSwapchain
{
public:
    MTLSwapchain() = default;
    ~MTLSwapchain();

    /**
     * Adopt InWindow's CAMetalLayer and build the blit pipeline. Must be called
     * from the UI thread, after the window has been created and exposed.
     */
    bool Initialize(MTLDevice* InDevice, EngineCore::WindowHandle InWindow);

    /** Release the blit pipeline and drop the layer reference. */
    void Shutdown();

    /** Blit InMainRT into the next drawable and present it. UI thread only. */
    void PresentFrame(MTLRenderTarget* InMainRT);

    bool IsValid() const { return Layer != nullptr && BlitPipeline != nullptr; }

    /**
     * True once Initialize() has failed. A failure here is a window-setup problem
     * (wrong surface type, no backing layer, missing cooked Screen shaders) that
     * will not fix itself, and PresentToWindow runs every frame -- without this
     * latch the same failure would be logged forever.
     */
    bool HasFailed() const { return bInitFailed; }

private:
    bool CreateBlitPipeline();
    void DestroyBlitPipeline();

    /** Load a cooked .metal resource and return its `main0` function (+1). */
    MTLFunctionRef LoadFunction(const std::string& MslResourcePath, MTLLibraryRef& OutLibrary) const;

    /** Track the window's backing-store size into the layer. */
    void UpdateDrawableSize();

    MTLDevice*         Device       = nullptr;
    EngineCore::WindowHandle Window;
    CAMetalLayerRef    Layer        = nullptr;   // borrowed from the NSView

    // Screen-quad blit pipeline (Screen.vsh/.fsh) targeting the layer format.
    MTLLibraryRef      BlitVSLibrary = nullptr;
    MTLLibraryRef      BlitFSLibrary = nullptr;
    MTLFunctionRef     BlitVS        = nullptr;
    MTLFunctionRef     BlitFS        = nullptr;
    MTLRenderPipelineStateRef BlitPipeline = nullptr;
    MTLSamplerStateRef BlitSampler   = nullptr;
    MTLBufferRef       QuadBuffer    = nullptr;

    bool               bInitFailed   = false;
};

#endif //CORECLIENT_MTLSWAPCHAIN_H

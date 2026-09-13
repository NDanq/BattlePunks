#ifndef CORECLIENT_MTLFORWARD_H
#define CORECLIENT_MTLFORWARD_H

// ---------------------------------------------------------------------------
// MTLForward.h -- Objective-C <-> C++ bridge for the Metal GDI backend.
//
// Public GDI headers (MetalDynamicGDI.h, MTLDevice.h, ...) are included from
// BOTH plain C++ translation units (e.g. GDI.cpp, QCanvasRenderer.cpp) and the
// Objective-C++ backend .mm files. The Metal object types are Objective-C ids, so
// they cannot appear directly in a header that a .cpp compiles.
//
// This header resolves that: under __OBJC__ the aliases are the real
// `id<MTLProtocol>` / Objective-C class pointer types; otherwise they degrade to
// `void*`. Because the backend is built with ARC DISABLED (-fno-objc-arc), an
// `id` is a plain non-owning pointer and is layout-identical to `void*`, so a
// class laid out in a .cpp and in a .mm agree on member size/offset.
//
// RULES:
//   * Public headers may store these aliases as members and pass them by value.
//   * Public headers must NEVER send Objective-C messages ([obj doThing]) or
//     dereference the objects -- all messaging lives in Private/GDI/Metal/*.mm.
//   * Ownership (retain/release) is manual; use MTLRetained<T> (below) inside the
//     .mm files. Do not put MTLRetained members in public headers.
// ---------------------------------------------------------------------------

#ifdef __OBJC__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

using MTLDeviceRef              = id<MTLDevice>;
using MTLCommandQueueRef        = id<MTLCommandQueue>;
using MTLCommandBufferRef       = id<MTLCommandBuffer>;
using MTLRenderCommandEncoderRef= id<MTLRenderCommandEncoder>;
using MTLBlitCommandEncoderRef  = id<MTLBlitCommandEncoder>;
using MTLLibraryRef             = id<MTLLibrary>;
using MTLFunctionRef            = id<MTLFunction>;
using MTLRenderPipelineStateRef = id<MTLRenderPipelineState>;
using MTLDepthStencilStateRef   = id<MTLDepthStencilState>;
using MTLBufferRef              = id<MTLBuffer>;
using MTLTextureRef             = id<MTLTexture>;
using MTLSamplerStateRef        = id<MTLSamplerState>;
using MTLSharedEventRef         = id<MTLSharedEvent>;
using CAMetalLayerRef           = CAMetalLayer*;
using CAMetalDrawableRef        = id<CAMetalDrawable>;

#else // !__OBJC__  (plain C++ translation unit)

using MTLDeviceRef              = void*;
using MTLCommandQueueRef        = void*;
using MTLCommandBufferRef       = void*;
using MTLRenderCommandEncoderRef= void*;
using MTLBlitCommandEncoderRef  = void*;
using MTLLibraryRef             = void*;
using MTLFunctionRef            = void*;
using MTLRenderPipelineStateRef = void*;
using MTLDepthStencilStateRef   = void*;
using MTLBufferRef              = void*;
using MTLTextureRef             = void*;
using MTLSamplerStateRef        = void*;
using MTLSharedEventRef         = void*;
using CAMetalLayerRef           = void*;
using CAMetalDrawableRef        = void*;

#endif // __OBJC__

#ifdef __OBJC__
// ---------------------------------------------------------------------------
// MTLRetained<T> -- minimal RAII owner for a retained Objective-C object, the
// non-ARC analog of the Vulkan backend's explicit vkCreate/vkDestroy handle
// management. Only usable from .mm files (needs the Objective-C runtime); never
// place it in a public header.
//
//   MTLRetained<id<MTLBuffer>> Buf([device newBufferWithLength:n options:0]);
//     // newBuffer... already returns a +1 object, so adopt it (no extra retain):
//   MTLRetained<id<MTLBuffer>> Buf = MTLRetained<id<MTLBuffer>>::Adopt([...]);
//
// Adopt()  : take ownership of an already-+1 object (from new*/copy* factories).
// Retain() : take a +1 on a borrowed/autoreleased object and own that.
// ---------------------------------------------------------------------------
template <typename T>
class MTLRetained
{
public:
    MTLRetained() = default;

    static MTLRetained Adopt(T InObj)   { MTLRetained R; R.Obj = InObj; return R; }
    static MTLRetained Retain(T InObj)  { MTLRetained R; R.Obj = [InObj retain]; return R; }

    MTLRetained(const MTLRetained& Other) { Obj = [Other.Obj retain]; }
    MTLRetained& operator=(const MTLRetained& Other)
    {
        if(this != &Other)
        {
            T Old = Obj;
            Obj = [Other.Obj retain];
            [Old release];
        }
        return *this;
    }

    MTLRetained(MTLRetained&& Other) noexcept { Obj = Other.Obj; Other.Obj = nil; }
    MTLRetained& operator=(MTLRetained&& Other) noexcept
    {
        if(this != &Other)
        {
            [Obj release];
            Obj = Other.Obj;
            Other.Obj = nil;
        }
        return *this;
    }

    ~MTLRetained() { [Obj release]; }

    T    Get() const     { return Obj; }
    operator T() const   { return Obj; }
    explicit operator bool() const { return Obj != nil; }

    /** Relinquish ownership without releasing (hand the +1 off to a raw member). */
    T Release() { T R = Obj; Obj = nil; return R; }

    /** Drop the current object and adopt a new already-+1 one. */
    void Reset(T InObj = nil)
    {
        if(Obj != InObj)
        {
            [Obj release];
            Obj = InObj;
        }
    }

private:
    T Obj = nil;
};
#endif // __OBJC__

#endif //CORECLIENT_MTLFORWARD_H

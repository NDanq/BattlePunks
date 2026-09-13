#include "Public/GDI/Metal/MTLTexture.h"
#include <algorithm>
#include "Public/GDI/Metal/MTLDevice.h"
#include "Private/GDI/Metal/MTLUtil.h"

#include <cstring>
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// ---------------------------------------------------------------------------
//  Construction / destruction
// ---------------------------------------------------------------------------

MTLTextureResource::MTLTextureResource(MTLDevice*        InDevice,
                                         const EngineCore::Image&     InImage,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InImage, 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    // Width/Height come from the GDITextureResource(EngineCore::Image) constructor.
    AllocateTexture();
    BuildSampler();
    if(!InImage.IsNull())
    {
        UploadImageData(InImage);
        bInitialized = true;
    }
}

MTLTextureResource::MTLTextureResource(MTLDevice*        InDevice,
                                         GDITexture2D*     InParent,
                                         const EngineCore::Image&     InImage,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InImage, 0, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    AllocateTexture();
    BuildSampler();
    if(!InImage.IsNull())
    {
        UploadImageData(InImage);
        bInitialized = true;
    }
}

MTLTextureResource::MTLTextureResource(MTLDevice*        InDevice,
                                         GDITexture2D*     InParent,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    // The base (parent, border, filter) constructor stores nothing -- pull the
    // dimensions across ourselves, exactly as the Vulkan path does.
    if(InParent)
    {
        Width  = static_cast<int>(InParent->Width);
        Height = static_cast<int>(InParent->Height);
    }
    // Uninitialized content; caller must Update() before sampling.
    AllocateTexture();
    BuildSampler();
}

MTLTextureResource::MTLTextureResource(MTLDevice*        InDevice,
                                         MTLTextureRef     InExternalTexture,
                                         GDITexture2D*     InParent,
                                         EGDITextureBorder InBorder,
                                         EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , Device(InDevice)
    , StoredBorder(InBorder)
    , StoredFilter(InFilter)
{
    if(InParent)
    {
        Width  = static_cast<int>(InParent->Width);
        Height = static_cast<int>(InParent->Height);
    }
    // Borrow the render target's texture; build only a sampler.
    Texture      = InExternalTexture;
    bOwnsTexture = false;
    BuildSampler();
    bInitialized = (Texture != nullptr);
}

MTLTextureResource::~MTLTextureResource()
{
    Release();
}

// ---------------------------------------------------------------------------
//  GDITextureResource overrides
// ---------------------------------------------------------------------------

void MTLTextureResource::Update(const EngineCore::Image& InImage)
{
    if(!Device || !Device->IsValid() || InImage.IsNull()) return;

    if(InImage.GetWidth() != Width || InImage.GetHeight() != Height)
    {
        Width  = InImage.GetWidth();
        Height = InImage.GetHeight();
        DestroyTexture();
        AllocateTexture();
    }

    UploadImageData(InImage);
    bInitialized = true;
}

void MTLTextureResource::Release()
{
    // A committed command buffer retains everything it references, so an
    // immediate release here can never invalidate an in-flight frame -- the
    // Vulkan backend's DeferDestroy* machinery has no Metal counterpart.
    if(Sampler != nullptr)
    {
        [Sampler release];
        Sampler = nullptr;
    }
    if(bOwnsTexture)
    {
        DestroyTexture();
    }
    else
    {
        Texture = nullptr;
    }
    bInitialized = false;
}

EngineCore::Image MTLTextureResource::ToImage() const
{
    if(!bInitialized || Texture == nullptr) return EngineCore::Image();
    if(Width <= 0 || Height <= 0)           return EngineCore::Image();

    // Borrowed render-target textures are PRIVATE; getBytes: is illegal on them.
    // MTLRenderTarget::ToImage covers that case with a blit-encoder readback.
    if(!bOwnsTexture)
    {
        GDI_LOG("[MTLTexture] ToImage on a borrowed render-target texture -- use MTLRenderTarget::ToImage.")
        return EngineCore::Image();
    }

    EngineCore::Image Result(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    [Texture getBytes:Result.GetData()
          bytesPerRow:(NSUInteger)(Width * 4)
           fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)Width, (NSUInteger)Height)
          mipmapLevel:0];
    return Result;
}

// ---------------------------------------------------------------------------
//  Private helpers
// ---------------------------------------------------------------------------

void MTLTextureResource::AllocateTexture()
{
    if(!Device || !Device->IsValid() || Width <= 0 || Height <= 0) return;

    MTLTextureDescriptor* Desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:(NSUInteger)Width
                                                           height:(NSUInteger)Height
                                                        mipmapped:NO];
    Desc.usage       = MTLTextureUsageShaderRead;
    // Shared storage keeps replaceRegion:/getBytes: legal from the CPU, which is
    // what lets this class skip the Vulkan staging-buffer path entirely.
    Desc.storageMode = MTLStorageModeShared;

    Texture = [Device->GetDevice() newTextureWithDescriptor:Desc];   // +1
    if(Texture == nullptr)
    {
        GDI_LOG("[MTLTexture] newTextureWithDescriptor failed.")
    }
}

void MTLTextureResource::DestroyTexture()
{
    if(Texture != nullptr)
    {
        [Texture release];
        Texture = nullptr;
    }
}

void MTLTextureResource::BuildSampler()
{
    if(!Device || !Device->IsValid()) return;

    if(Sampler != nullptr)
    {
        [Sampler release];
        Sampler = nullptr;
    }

    MTLSamplerDescriptor* Desc = [[[MTLSamplerDescriptor alloc] init] autorelease];
    Desc.minFilter    = MTLUtil::DecodeSamplerFilter(StoredFilter);
    Desc.magFilter    = MTLUtil::DecodeSamplerFilter(StoredFilter);
    Desc.mipFilter    = MTLSamplerMipFilterNotMipmapped;   // no mipmaps, matching Vulkan
    Desc.sAddressMode = MTLUtil::DecodeSamplerAddress(StoredBorder);
    Desc.tAddressMode = MTLUtil::DecodeSamplerAddress(StoredBorder);
    Desc.rAddressMode = MTLUtil::DecodeSamplerAddress(StoredBorder);
#if TARGET_OS_OSX
    // Matches VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK. Only meaningful with
    // ClampToBorderColor, which is macOS-only (see MTLUtil::DecodeSamplerAddress).
    Desc.borderColor  = MTLSamplerBorderColorTransparentBlack;
#endif

    Sampler = [Device->GetDevice() newSamplerStateWithDescriptor:Desc];   // +1
    if(Sampler == nullptr)
    {
        GDI_LOG("[MTLTexture] newSamplerStateWithDescriptor failed.")
    }
}

void MTLTextureResource::UploadImageData(const EngineCore::Image& InImage)
{
    if(Texture == nullptr) return;

    // RGBA8888 maps byte-for-byte onto MTLPixelFormatRGBA8Unorm.
    const EngineCore::Image Formatted = InImage.ConvertedTo(EngineCore::EImageFormat::RGBA8);
    if(Formatted.IsNull()) return;

    const NSUInteger CopyWidth  = (NSUInteger)std::min(Formatted.GetWidth(),  Width);
    const NSUInteger CopyHeight = (NSUInteger)std::min(Formatted.GetHeight(), Height);
    if(CopyWidth == 0 || CopyHeight == 0) return;

    [Texture replaceRegion:MTLRegionMake2D(0, 0, CopyWidth, CopyHeight)
               mipmapLevel:0
                 withBytes:Formatted.GetData()
               bytesPerRow:(NSUInteger)Formatted.GetStride()];
}

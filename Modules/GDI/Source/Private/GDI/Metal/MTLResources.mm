#include "Private/GDI/Metal/MTLInternal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <regex>

#include "Public/Core/CoreFileProvider.h"
#include "Public/GDI/GDIHalf.h"
#include "Public/GDI/GDILog.h"

extern GDIDynamicDriver* GDynamicGDI;

MetalDynamicGDI* GetMetalDriver()
{
    return dynamic_cast<MetalDynamicGDI*>(GDynamicGDI);
}

// ===========================================================================
// Shaders
// ===========================================================================

namespace
{
    std::atomic<uint64_t> GNextShaderID{1};

    /* spirv-cross writes a compute shader's local_size as
     * `constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(64u, 1u, 1u);`. */
    MTLSize ParseWorkGroupSize(const std::string& InSource)
    {
        static const std::regex Pattern(R"(gl_WorkGroupSize[^=]*=\s*uint3\(\s*(\d+)u?\s*,\s*(\d+)u?\s*,\s*(\d+)u?\s*\))");
        std::smatch Match;
        if(std::regex_search(InSource, Match, Pattern))
        {
            return MTLSizeMake(NSUInteger(std::stoul(Match[1])), NSUInteger(std::stoul(Match[2])), NSUInteger(std::stoul(Match[3])));
        }
        return MTLSizeMake(1, 1, 1);
    }
}

MTLShaderResourceImpl::MTLShaderResourceImpl(id<MTLDevice> InDevice, const EShaderType InType, const std::string& InMslPath)
    : GDIShaderResource(InType, InMslPath)
    , ID(GNextShaderID.fetch_add(1))
{
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    std::vector<uint8_t> Bytes;
    if(!InDevice || !Provider || !Provider->ReadAll(InMslPath, Bytes) || Bytes.empty())
    {
        GDI_LOG_ERROR("[MTLShaderResource] Failed to read MSL: " + InMslPath)
        return;
    }
    const std::string Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
    NSString* Source = MTLUtil::ToNS(Text);
    if(!Source)
    {
        GDI_LOG_ERROR("[MTLShaderResource] MSL is not UTF-8: " + InMslPath)
        return;
    }

    MTLCompileOptions* Options = [[MTLCompileOptions alloc] init];
    Options.languageVersion = MTLLanguageVersion2_3;   // cook_shaders.py's MSL_VERSION
    Options.fastMathEnabled = YES;
    NSError* Error = nil;
    Library = [InDevice newLibraryWithSource:Source options:Options error:&Error];
    if(!Library)
    {
        MTLUtil::LogError("compiling " + InMslPath, Error);
        return;
    }
    // spirv-cross names every entry point main0.
    Function = [Library newFunctionWithName:@"main0"];
    if(!Function)
    {
        GDI_LOG_ERROR("[MTLShaderResource] No main0 in " + InMslPath)
        Library = nil;
        return;
    }
    Function.label = MTLUtil::ToNS(InMslPath);
    if(InType == ST_Compute)
    {
        WorkGroupSize = ParseWorkGroupSize(Text);
    }
    bInitialized = true;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, const int InValue)
{
    MTLUniformValue V;
    V.Tag    = MTLUniformValue::INT;
    V.Data.i = InValue;
    Scalars[InName] = V;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, const float InValue)
{
    MTLUniformValue V;
    V.Tag    = MTLUniformValue::FLOAT;
    V.Data.f = InValue;
    Scalars[InName] = V;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, const Vector2 InVector)
{
    MTLUniformValue V;
    V.Tag       = MTLUniformValue::VEC2;
    V.Data.v[0] = float(InVector.GetX());
    V.Data.v[1] = float(InVector.GetY());
    Scalars[InName] = V;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, const Vector3 InVector)
{
    MTLUniformValue V;
    V.Tag       = MTLUniformValue::VEC3;
    V.Data.v[0] = float(InVector.GetX());
    V.Data.v[1] = float(InVector.GetY());
    V.Data.v[2] = float(InVector.GetZ());
    Scalars[InName] = V;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, const Vector4 InVector)
{
    MTLUniformValue V;
    V.Tag       = MTLUniformValue::VEC4;
    V.Data.v[0] = float(InVector.GetX());
    V.Data.v[1] = float(InVector.GetY());
    V.Data.v[2] = float(InVector.GetZ());
    V.Data.v[3] = float(InVector.GetW());
    Scalars[InName] = V;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, const Matrix4x4 InMatrix)
{
    // Row-major doubles to std140's column-major floats, as every backend does.
    MTLUniformValue V;
    V.Tag = MTLUniformValue::MAT;
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            V.Data.v[Col * 4 + Row] = static_cast<float>(InMatrix.Data[Row][Col]);
    Scalars[InName] = V;
}

void MTLShaderResourceImpl::SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex)
{
    if(!InTexture2D) return;
    SetUniform(InName, InTexture2D->Resource, InIndex);
}

void MTLShaderResourceImpl::SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex)
{
    if(!InTexResource) return;
    MTLTextureBinding Binding;
    Binding.Resource = InTexResource;
    Binding.Index    = InIndex;
    Textures[InName] = Binding;
    Texture0 = nullptr;
    for(const auto& Entry : Textures)
    {
        if(Entry.second.Index == 0)
        {
            Texture0 = Entry.second.Resource;
            break;
        }
    }
}

void MTLShaderResourceImpl::Release()
{
    Scalars.clear();
    Textures.clear();
    Texture0 = nullptr;
    Core     = CoreBlocks();
    Function = nil;
    Library  = nil;
    bInitialized = false;
}

// ===========================================================================
// Textures
// ===========================================================================

namespace
{
    /* A command buffer of its own for an upload, committed at once. The queue
     * runs it before any frame committed after -- every frame that can sample
     * the texture, for a new one. */
    id<MTLCommandBuffer> UploadCommandBuffer(MetalState* InState)
    {
        id<MTLCommandBuffer> Cmd = [InState->Queue commandBuffer];
        Cmd.label = @"Nimble.Upload";
        return Cmd;
    }

    std::vector<EngineCore::Image> AsRGBA8(const EngineCore::Image* InLevels, const uint32_t InCount)
    {
        std::vector<EngineCore::Image> Out;
        Out.reserve(InCount);
        for(uint32_t Level = 0; Level < InCount; ++Level)
        {
            Out.push_back(InLevels[Level].GetFormat() == EngineCore::EImageFormat::RGBA8
                          ? InLevels[Level] : InLevels[Level].ConvertedTo(EngineCore::EImageFormat::RGBA8));
        }
        return Out;
    }

    /* InBytes of InData into InTexture's level, through a staging buffer and a blit. */
    void BlitIntoTexture(id<MTLBlitCommandEncoder> InBlit, id<MTLDevice> InDevice, id<MTLTexture> InTexture,
                         const NSUInteger InLevel, const NSUInteger InWidth, const NSUInteger InHeight,
                         const void* InData, const NSUInteger InBytesPerRow, const NSUInteger InBytes)
    {
        id<MTLBuffer> Staging = [InDevice newBufferWithBytes:InData length:InBytes options:MTLResourceStorageModeShared];
        if(!Staging) return;
        [InBlit copyFromBuffer:Staging sourceOffset:0 sourceBytesPerRow:InBytesPerRow sourceBytesPerImage:InBytes
                    sourceSize:MTLSizeMake(InWidth, InHeight, 1) toTexture:InTexture destinationSlice:0
              destinationLevel:InLevel destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
}

MTLTextureResourceImpl::MTLTextureResourceImpl(MetalState* InState, GDITexture2D* InParent, const EngineCore::Image* InLevels,
                                               const uint32_t InCount, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , State(InState)
    , Border(InBorder)
    , Filter(InFilter)
{
    Parent = InParent;
    if(InLevels && InCount > 0)
    {
        Upload(InLevels, InCount);
    }
}

MTLTextureResourceImpl::MTLTextureResourceImpl(MetalState* InState, const MTLPixelFormat InFormat, const GDICompressedLevel* InLevels,
                                               const uint32_t InCount, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
    : GDITextureResource(nullptr, InBorder, InFilter)
    , State(InState)
    , Border(InBorder)
    , Filter(InFilter)
{
    Parent = nullptr;
    if(!State || !InLevels || InCount == 0) return;
    Width  = int(InLevels[0].Width);
    Height = int(InLevels[0].Height);

    MTLTextureDescriptor* Desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:InFormat width:NSUInteger(Width)
                                                                                    height:NSUInteger(Height) mipmapped:InCount > 1];
    Desc.mipmapLevelCount = InCount;
    Desc.usage            = MTLTextureUsageShaderRead;
    Desc.storageMode      = MTLStorageModePrivate;
    Texture = [State->Device newTextureWithDescriptor:Desc];
    if(!Texture) return;

    const bool       bSmallBlocks = InFormat == MTLPixelFormatBC1_RGBA || InFormat == MTLPixelFormatBC4_RUnorm;
    const NSUInteger BlockBytes   = bSmallBlocks ? 8 : 16;
    id<MTLCommandBuffer>      Cmd  = UploadCommandBuffer(State);
    id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        const GDICompressedLevel& Each = InLevels[Level];
        const NSUInteger BlocksWide = std::max<NSUInteger>(1, (Each.Width + 3) / 4);
        BlitIntoTexture(Blit, State->Device, Texture, Level, Each.Width, Each.Height, Each.Data, BlocksWide * BlockBytes, Each.Bytes);
    }
    [Blit endEncoding];
    [Cmd commit];
    bInitialized = true;
}

MTLTextureResourceImpl::MTLTextureResourceImpl(MetalState* InState, GDITexture2D* InParent, const EGDITextureBorder InBorder,
                                               const EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , State(InState)
    , Border(InBorder)
    , Filter(InFilter)
{
    Parent = InParent;
}

MTLTextureResourceImpl::MTLTextureResourceImpl(MetalState* InState, id<MTLTexture> InExternal, GDITexture2D* InParent,
                                               const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
    : GDITextureResource(InParent, InBorder, InFilter)
    , State(InState)
    , Texture(InExternal)
    , Border(InBorder)
    , Filter(InFilter)
{
    Parent = InParent;
    if(Texture)
    {
        Width  = int(Texture.width);
        Height = int(Texture.height);
        bInitialized = true;
    }
}

MTLTextureResourceImpl::~MTLTextureResourceImpl()
{
    Texture = nil;   // the command buffers still using it keep it alive (retained references)
}

void MTLTextureResourceImpl::Upload(const EngineCore::Image* InLevels, const uint32_t InCount)
{
    if(!State || !State->Device) return;
    const std::vector<EngineCore::Image> Levels = AsRGBA8(InLevels, InCount);
    if(Levels.empty() || Levels[0].GetWidth() <= 0 || Levels[0].GetHeight() <= 0) return;

    const NSUInteger W = NSUInteger(Levels[0].GetWidth());
    const NSUInteger H = NSUInteger(Levels[0].GetHeight());
    if(!Texture || Texture.width != W || Texture.height != H || Texture.mipmapLevelCount != Levels.size()
       || Texture.pixelFormat != MTLPixelFormatRGBA8Unorm)
    {
        MTLTextureDescriptor* Desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:W height:H mipmapped:Levels.size() > 1];
        Desc.mipmapLevelCount = Levels.size();
        Desc.usage            = MTLTextureUsageShaderRead;
        Desc.storageMode      = MTLStorageModePrivate;
        Texture = [State->Device newTextureWithDescriptor:Desc];
        if(!Texture) return;
        if(!DebugName.empty()) Texture.label = MTLUtil::ToNS(DebugName);
    }
    Width  = int(W);
    Height = int(H);

    id<MTLCommandBuffer>      Cmd  = UploadCommandBuffer(State);
    id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
    for(size_t Level = 0; Level < Levels.size(); ++Level)
    {
        const EngineCore::Image& Each = Levels[Level];
        const NSUInteger Row = NSUInteger(Each.GetWidth()) * 4;
        BlitIntoTexture(Blit, State->Device, Texture, Level, NSUInteger(Each.GetWidth()), NSUInteger(Each.GetHeight()),
                        Each.GetData(), Row, Row * NSUInteger(Each.GetHeight()));
    }
    [Blit endEncoding];
    [Cmd commit];
    bInitialized = true;
}

void MTLTextureResourceImpl::Update(const EngineCore::Image& InImage)
{
    Upload(&InImage, 1);
}

void MTLTextureResourceImpl::Release()
{
    Texture      = nil;
    bInitialized = false;
}

EngineCore::Image MTLTextureResourceImpl::ToImage() const
{
    if(!Texture || !State || Texture.pixelFormat != MTLPixelFormatRGBA8Unorm) return EngineCore::Image();
    if(MetalDynamicGDI* Driver = GetMetalDriver()) Driver->FlushAndWait(true);

    const NSUInteger W = Texture.width, H = Texture.height, Row = W * 4;
    id<MTLBuffer> Readback = [State->Device newBufferWithLength:Row * H options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>      Cmd  = [State->Queue commandBuffer];
    id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
    [Blit copyFromTexture:Texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                 toBuffer:Readback destinationOffset:0 destinationBytesPerRow:Row destinationBytesPerImage:Row * H];
    [Blit endEncoding];
    [Cmd commit];
    [Cmd waitUntilCompleted];

    EngineCore::Image Out(int(W), int(H), EngineCore::EImageFormat::RGBA8);
    std::memcpy(Out.GetData(), Readback.contents, Row * H);
    return Out;
}

void MTLTextureResourceImpl::SetDebugName(const std::string& InName)
{
    GDITextureResource::SetDebugName(InName);
    if(Texture && GDIShaderDebugEnabled()) Texture.label = MTLUtil::ToNS(InName);
}

// ===========================================================================
// Render targets
// ===========================================================================

MTLRenderTargetImpl::MTLRenderTargetImpl(MetalState* InState, const std::string& InName, const uint InWidth, const uint InHeight,
                                         const MTLPixelFormat InFormat)
    : GDIRenderTarget(InWidth, InHeight)
    , State(InState)
    , Name(InName)
    , Format(InFormat)
{
    Allocate();
}

MTLRenderTargetImpl::~MTLRenderTargetImpl()
{
    if(MetalDynamicGDI* Driver = GetMetalDriver()) Driver->ForgetRenderTarget(this);
    Destroy();
    for(GDITexture2D** Wrapper : { &ColorWrapper, &DepthWrapper })
    {
        if(*Wrapper)
        {
            delete (*Wrapper)->Resource;
            (*Wrapper)->Resource = nullptr;
            delete *Wrapper;
            *Wrapper = nullptr;
        }
    }
}

void MTLRenderTargetImpl::Allocate()
{
    if(!State || !State->Device || Width == 0 || Height == 0) return;

    MTLTextureDescriptor* Desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:Format width:Width height:Height mipmapped:NO];
    Desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    Desc.storageMode = MTLStorageModePrivate;
    Color = [State->Device newTextureWithDescriptor:Desc];
    if(!Color)
    {
        GDI_LOG_ERROR("[MTLRenderTarget] No colour texture for " + Name)
        return;
    }
    Color.label = MTLUtil::ToNS("RT:" + Name);

    // D32, sampleable (ToDepthTexture2D), as every backend's.
    MTLTextureDescriptor* DepthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:Width height:Height mipmapped:NO];
    DepthDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    DepthDesc.storageMode = MTLStorageModePrivate;
    Depth = [State->Device newTextureWithDescriptor:DepthDesc];
    if(Depth)
    {
        Depth.label      = MTLUtil::ToNS("RT:" + Name + " Depth");
        bDepthNeedsClear = true;
    }
    else
    {
        GDI_LOG_ERROR("[MTLRenderTarget] No depth buffer for " + Name + "; it draws without a depth test")
    }

    /* Zeroed, as a fresh D3D12 or Vulkan target reads in practice: a private
     * Metal texture starts as whatever the memory held. RenderCore leans on it --
     * with dynamic resolution a view draws only part of its target, and a blur
     * (bloom) still reads past the drawn edge, which then has to be black. */
    {
        id<MTLCommandBuffer> Cmd = [State->Queue commandBuffer];
        Cmd.label = @"Nimble.ClearNewTarget";
        MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
        Pass.colorAttachments[0].texture     = Color;
        Pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
        Pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);
        Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if(Depth)
        {
            Pass.depthAttachment.texture     = Depth;
            Pass.depthAttachment.loadAction  = MTLLoadActionClear;
            Pass.depthAttachment.clearDepth  = 1.0;
            Pass.depthAttachment.storeAction = MTLStoreActionStore;
            bDepthNeedsClear = false;
        }
        [[Cmd renderCommandEncoderWithDescriptor:Pass] endEncoding];
        [Cmd commit];
    }

    // Wrappers handed out before keep pointing at the live textures.
    if(ColorWrapper && ColorWrapper->Resource)
    {
        static_cast<MTLTextureResourceImpl*>(ColorWrapper->Resource)->Adopt(Color);
        ColorWrapper->Width  = Width;
        ColorWrapper->Height = Height;
    }
    if(DepthWrapper && DepthWrapper->Resource)
    {
        static_cast<MTLTextureResourceImpl*>(DepthWrapper->Resource)->Adopt(Depth);
        DepthWrapper->Width  = Width;
        DepthWrapper->Height = Height;
    }
}

void MTLRenderTargetImpl::Destroy()
{
    Color = nil;
    Depth = nil;
    bDepthNeedsClear = false;
    bHasContent.store(false);
}

void MTLRenderTargetImpl::Release()
{
    if(MetalDynamicGDI* Driver = GetMetalDriver()) Driver->ForgetRenderTarget(this);
    Destroy();
}

void MTLRenderTargetImpl::Resize(const uint InWidth, const uint InHeight)
{
    if(MetalDynamicGDI* Driver = GetMetalDriver())
    {
        Driver->SetResizing(true);
        Driver->FlushAndWait(false);
    }
    GDIRenderTarget::Resize(InWidth, InHeight);
    Destroy();
    Allocate();
}

EngineCore::Image MTLRenderTargetImpl::ToImage()
{
    if(!Color || !State || Width == 0 || Height == 0) return EngineCore::Image();
    if(MetalDynamicGDI* Driver = GetMetalDriver()) Driver->FlushAndWait(true);

    NSUInteger Texel = 4;
    switch(Format)
    {
        case MTLPixelFormatRGBA16Float: Texel = 8;  break;
        case MTLPixelFormatRGBA32Float: Texel = 16; break;
        default:                        Texel = 4;  break;
    }
    const NSUInteger Row = NSUInteger(Width) * Texel;
    id<MTLBuffer> Readback = [State->Device newBufferWithLength:Row * Height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>      Cmd  = [State->Queue commandBuffer];
    id<MTLBlitCommandEncoder> Blit = [Cmd blitCommandEncoder];
    [Blit copyFromTexture:Color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(Width, Height, 1)
                 toBuffer:Readback destinationOffset:0 destinationBytesPerRow:Row destinationBytesPerImage:Row * Height];
    [Blit endEncoding];
    [Cmd commit];
    [Cmd waitUntilCompleted];

    EngineCore::Image Out(int(Width), int(Height), EngineCore::EImageFormat::RGBA8);
    const size_t Pixels = size_t(Width) * size_t(Height);
    const void*  Data   = Readback.contents;
    switch(Format)
    {
        case MTLPixelFormatRGBA16Float: GDIHalvesToBytes(static_cast<const uint16_t*>(Data), Out.GetData(), Pixels * 4); break;
        case MTLPixelFormatRGB10A2Unorm: GDIRgb10A2ToBytes(static_cast<const uint32_t*>(Data), Out.GetData(), Pixels); break;
        case MTLPixelFormatRGBA32Float: GDIFloatsToBytes(static_cast<const float*>(Data), Out.GetData(), Pixels * 4); break;
        case MTLPixelFormatR32Float:    GDIR32FToBytes(static_cast<const float*>(Data), Out.GetData(), Pixels); break;
        default:                        std::memcpy(Out.GetData(), Data, Pixels * 4); break;
    }
    return Out;
}

GDITexture2D* MTLRenderTargetImpl::ToGDITexture2D()
{
    if(!Color) return nullptr;
    if(!ColorWrapper)
    {
        ColorWrapper           = new GDITexture2D();
        ColorWrapper->Width    = Width;
        ColorWrapper->Height   = Height;
        ColorWrapper->Resource = new MTLTextureResourceImpl(State, Color, ColorWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Linear);
    }
    return ColorWrapper;
}

GDITexture2D* MTLRenderTargetImpl::ToDepthTexture2D()
{
    if(!Depth) return nullptr;
    if(!DepthWrapper)
    {
        DepthWrapper           = new GDITexture2D();
        DepthWrapper->Width    = Width;
        DepthWrapper->Height   = Height;
        DepthWrapper->Resource = new MTLTextureResourceImpl(State, Depth, DepthWrapper, GDI_TB_Clamp_To_Edge, GDI_TF_Nearest);
    }
    return DepthWrapper;
}

void MTLRenderTargetImpl::ResolveRenderTarget(GDIRenderTarget* Dest)
{
    auto* Target = dynamic_cast<MTLRenderTargetImpl*>(Dest);
    if(!Target || Target == this || !Color || !Target->GetColor() || !State) return;

    bool bTransient = false;
    id<MTLCommandBuffer> Cmd = State->CommandBufferForWork(bTransient);
    if(!Cmd) return;
    // The copy ends the pass: what was bound is unbound, as D3D12's resolve does.
    State->bPassBound = false;
    State->ColorCount = 0;
    State->DepthRT    = nullptr;

    int32_t EndSample = -1;
    id<MTLBlitCommandEncoder> Blit = State->BlitEncoder(Cmd, bTransient, EndSample);
    const NSUInteger W = std::min<NSUInteger>(Width, Target->GetWidth());
    const NSUInteger H = std::min<NSUInteger>(Height, Target->GetHeight());
    if(Color.pixelFormat == Target->GetColor().pixelFormat)
    {
        [Blit copyFromTexture:Color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                    toTexture:Target->GetColor() destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [Blit endEncoding];
    State->EncoderEnded(EndSample);
    Target->MarkContent();
    if(bTransient)
    {
        [Cmd commit];
        [Cmd waitUntilCompleted];
    }
}

// ===========================================================================
// Buffers
// ===========================================================================

MTLBufferImpl::MTLBufferImpl(id<MTLDevice> InDevice, const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
    : GDIBuffer(InBytes, InUsage)
{
    if(!InDevice || InBytes == 0) return;
    // Shared: unified memory on Apple GPUs, and ReadBuffer maps it directly.
    Buffer = InInitialData ? [InDevice newBufferWithBytes:InInitialData length:InBytes options:MTLResourceStorageModeShared]
                           : [InDevice newBufferWithLength:InBytes options:MTLResourceStorageModeShared];
    if(Buffer && !InInitialData)
    {
        std::memset(Buffer.contents, 0, InBytes);
    }
}

void MTLBufferImpl::Release()
{
    Buffer = nil;   // in-flight command buffers keep it alive
    delete this;
}

void MTLBufferImpl::SetDebugName(const std::string& InName)
{
    GDIBuffer::SetDebugName(InName);
    if(Buffer && GDIShaderDebugEnabled()) Buffer.label = MTLUtil::ToNS(InName);
}

// ===========================================================================
// Meshes
// ===========================================================================

MTLMeshImpl::MTLMeshImpl(MetalState* InState) : State(InState)
{
}

MTLMeshImpl::~MTLMeshImpl()
{
    Release();
}

void MTLMeshImpl::InitResource()
{
    if(!State || !State->Device) return;
    id<MTLDevice> Device = State->Device;
    const NSUInteger Vertices = RenderData.NumVertices / 3;
    if(Vertices == 0 || !RenderData.Position || !RenderData.TexCoord) return;

    const auto Make = [Device](const void* InData, const NSUInteger InBytes) -> id<MTLBuffer>
    {
        return InData && InBytes ? [Device newBufferWithBytes:InData length:InBytes options:MTLResourceStorageModeShared] : nil;
    };
    using namespace MTLBindings;
    Streams[Position] = Make(RenderData.Position, Vertices * 3 * sizeof(float));
    Streams[TexCoord] = Make(RenderData.TexCoord, Vertices * 2 * sizeof(float));
    if(!Streams[Position] || !Streams[TexCoord]) return;

    if(IsIndexed())
    {
        Streams[Normal]  = Make(RenderData.Normal,  Vertices * 3 * sizeof(float));
        Streams[Color]   = Make(RenderData.Color,   Vertices * 4 * sizeof(float));
        Streams[UV1]     = Make(RenderData.UV1,     Vertices * 2 * sizeof(float));
        Streams[Tangent] = Make(RenderData.Tangent, Vertices * 4 * sizeof(float));
        Indices          = Make(RenderData.Indices, NSUInteger(RenderData.NumIndices) * sizeof(uint32_t));
        if(!Streams[Normal] || !Streams[Color] || !Streams[UV1] || !Streams[Tangent] || !Indices) return;
        if(IsSkinned())
        {
            Streams[Joints]  = Make(RenderData.Joints,  Vertices * 4 * sizeof(float));
            Streams[Weights] = Make(RenderData.Weights, Vertices * 4 * sizeof(float));
            if(!Streams[Joints] || !Streams[Weights]) return;
        }
    }
    bValid = true;
    ApplyDebugName();
}

bool MTLMeshImpl::InitDynamic(const uint32_t InMaxVertices, const uint32_t InMaxIndices, const uint32_t InRegions)
{
    if(!State || !State->Device || bValid) return false;
    MakeDynamic(InMaxVertices, InMaxIndices);
    const NSUInteger Regions  = InRegions < 1 ? 1 : InRegions;
    const NSUInteger Vertices = NSUInteger(InMaxVertices) * Regions;
    using namespace MTLBindings;
    const auto Make = [this](const NSUInteger InBytes) -> id<MTLBuffer>
    {
        id<MTLBuffer> Buffer = [State->Device newBufferWithLength:std::max<NSUInteger>(InBytes, 16) options:MTLResourceStorageModeShared];
        if(Buffer) std::memset(Buffer.contents, 0, Buffer.length);
        return Buffer;
    };
    Streams[Position] = Make(Vertices * 3 * sizeof(float));
    Streams[TexCoord] = Make(Vertices * 2 * sizeof(float));
    Streams[Normal]   = Make(Vertices * 3 * sizeof(float));
    Streams[Color]    = Make(Vertices * 4 * sizeof(float));
    Streams[UV1]      = Make(Vertices * 2 * sizeof(float));
    Streams[Tangent]  = Make(Vertices * 4 * sizeof(float));
    Indices           = Make(NSUInteger(InMaxIndices) * Regions * sizeof(uint32_t));
    bValid = Streams[Position] && Streams[TexCoord] && Streams[Normal] && Streams[Color] && Streams[UV1] && Streams[Tangent] && Indices;
    ApplyDebugName();
    return bValid;
}

bool MTLMeshImpl::WriteDynamic(const GDIMeshDesc& InDesc, const uint32_t InBaseVertex, const uint32_t InBaseIndex)
{
    if(!bValid) return false;
    using namespace MTLBindings;
    const size_t Base = InBaseVertex, N = InDesc.NumVertices;
    const auto At = [this](const EStream InStream) { return static_cast<float*>(Streams[InStream].contents); };
    std::memcpy(At(Position) + Base * 3, InDesc.Positions, N * 3 * sizeof(float));
    std::memcpy(At(TexCoord) + Base * 2, InDesc.UV0, N * 2 * sizeof(float));
    if(InDesc.Normals)  std::memcpy(At(Normal) + Base * 3, InDesc.Normals, N * 3 * sizeof(float));
    if(InDesc.UV1)      std::memcpy(At(UV1) + Base * 2, InDesc.UV1, N * 2 * sizeof(float));
    if(InDesc.Tangents) std::memcpy(At(Tangent) + Base * 4, InDesc.Tangents, N * 4 * sizeof(float));
    float* Colors = At(Color) + Base * 4;
    if(InDesc.Colors) std::memcpy(Colors, InDesc.Colors, N * 4 * sizeof(float));
    else              std::fill(Colors, Colors + N * 4, 1.0f);
    std::memcpy(static_cast<uint32_t*>(Indices.contents) + InBaseIndex, InDesc.Indices, size_t(InDesc.NumIndices) * sizeof(uint32_t));
    return true;
}

void MTLMeshImpl::Release()
{
    for(__strong id<MTLBuffer>& Each : Streams) { Each = nil; }
    Indices = nil;
    if(!bDynamic) FreeRenderData();
    bValid = false;
}

void MTLMeshImpl::SetDebugName(const std::string& InName)
{
    GDIMesh::SetDebugName(InName);
    ApplyDebugName();
}

void MTLMeshImpl::ApplyDebugName()
{
    if(DebugName.empty() || !GDIShaderDebugEnabled()) return;
    static const char* const Names[9] = { "Positions", "TexCoords", "Normals", "Colors", "UV1", "Instances", "Joints", "Weights", "Tangents" };
    for(int Stream = 0; Stream < 9; ++Stream)
    {
        if(Streams[Stream]) Streams[Stream].label = MTLUtil::ToNS(DebugName + " " + Names[Stream]);
    }
    if(Indices) Indices.label = MTLUtil::ToNS(DebugName + " Indices");
}

void MTLMeshImpl::Draw()
{
    if(MetalDynamicGDI* Driver = GetMetalDriver()) Driver->DrawMesh(this);
}

EMTLVertexLayout MTLMeshImpl::GetLayout(const bool bInInstanceBuffer) const
{
    if(IsIndexed())
    {
        return bInInstanceBuffer ? MVL_MeshInstanced : (IsSkinned() ? MVL_MeshSkinned : MVL_Mesh);
    }
    return IsInstanced() ? MVL_QuadInstanced : MVL_Quad;
}

void MTLMeshImpl::Encode(id<MTLRenderCommandEncoder> InEncoder, MetalState& InState)
{
    using namespace MTLBindings;
    MetalDynamicGDI* Driver = InState.Driver;
    const NSUInteger VertexCount = GetVertexCount();
    if(!bValid || VertexCount == 0) return;

    [InEncoder setVertexBuffer:Streams[Position] offset:0 atIndex:Stream(Position)];
    [InEncoder setVertexBuffer:Streams[TexCoord] offset:0 atIndex:Stream(TexCoord)];

    if(IsIndexed())
    {
        [InEncoder setVertexBuffer:Streams[Normal]  offset:0 atIndex:Stream(Normal)];
        [InEncoder setVertexBuffer:Streams[Color]   offset:0 atIndex:Stream(Color)];
        [InEncoder setVertexBuffer:Streams[UV1]     offset:0 atIndex:Stream(UV1)];
        [InEncoder setVertexBuffer:Streams[Tangent] offset:0 atIndex:Stream(Tangent)];
        if(Streams[Joints] && Streams[Weights])
        {
            [InEncoder setVertexBuffer:Streams[Joints]  offset:0 atIndex:Stream(Joints)];
            [InEncoder setVertexBuffer:Streams[Weights] offset:0 atIndex:Stream(Weights)];
        }

        // GDISetIndexBuffer's buffer, else the mesh's own.
        id<MTLBuffer> IndexBuffer = Indices;
        NSUInteger    IndexCount  = RenderData.NumIndices;
        if(auto* Override = static_cast<MTLBufferImpl*>(Driver->GetIndexBuffer()))
        {
            if(!Override->IsValid()) return;
            IndexBuffer = Override->GetBuffer();
            IndexCount  = Override->GetSize() / sizeof(uint32_t);
        }

        NSUInteger Instances = 1;
        if(auto* Instance = static_cast<MTLBufferImpl*>(Driver->GetInstanceBuffer()))
        {
            if(!Instance->IsValid()) return;
            [InEncoder setVertexBuffer:Instance->GetBuffer() offset:0 atIndex:Stream(MTLBindings::Instance)];
            Instances = Driver->GetInstanceCount();
        }

        // GDISetIndirectArgs: the GPU's counts (MTLDrawIndexedPrimitivesIndirectArguments is GDI's 20-byte layout).
        if(auto* Args = static_cast<MTLBufferImpl*>(Driver->GetIndirectArgs()))
        {
            if(!Args->IsValid()) return;
            [InEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexType:MTLIndexTypeUInt32 indexBuffer:IndexBuffer
                           indexBufferOffset:0 indirectBuffer:Args->GetBuffer() indirectBufferOffset:Driver->GetIndirectArgsOffset()];
            return;
        }

        uint32_t First = 0, Count = 0;
        if(Driver->GetIndexBuffer())
        {
            // The draw range is into the override buffer (GDI.h).
            GetClampedDrawRange(First, Count);
            First = std::min<uint32_t>(DrawFirstIndex, uint32_t(IndexCount));
            Count = DrawIndexCount == 0 ? uint32_t(IndexCount) - First : std::min<uint32_t>(DrawIndexCount, uint32_t(IndexCount) - First);
        }
        else
        {
            GetClampedDrawRange(First, Count);
        }
        if(Count == 0 || Instances == 0) return;
        const NSUInteger IndexStart = NSUInteger(Driver->GetIndexBuffer() ? 0 : GetDynamicBaseIndex()) + First;
        [InEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:Count indexType:MTLIndexTypeUInt32
                             indexBuffer:IndexBuffer indexBufferOffset:IndexStart * sizeof(uint32_t)
                           instanceCount:Instances baseVertex:NSInteger(GetDynamicBaseVertex()) baseInstance:0];
        return;
    }

    if(IsInstanced())
    {
        // The old instanced quads: this batch's transforms, opacities and colours, uploaded per draw.
        const size_t Count = InstancedTransforms.size();
        if(Count == 0) return;
        std::vector<float> Transforms(Count * 16), Colors(Count * 3);
        for(size_t Each = 0; Each < Count; ++Each)
        {
            const Matrix4x4& M = InstancedTransforms[Each];
            for(int Row = 0; Row < 4; ++Row)
                for(int Col = 0; Col < 4; ++Col)
                    Transforms[Each * 16 + Col * 4 + Row] = static_cast<float>(M.Data[Row][Col]);
            const Vector3& C = InstancedColors[Each];
            Colors[Each * 3 + 0] = float(C.GetX());
            Colors[Each * 3 + 1] = float(C.GetY());
            Colors[Each * 3 + 2] = float(C.GetZ());
        }
        const MTLUploadRing::Allocation T = InState.Upload(Transforms.data(), Transforms.size() * sizeof(float));
        const MTLUploadRing::Allocation O = InState.Upload(InstancedOpacities.data(), InstancedOpacities.size() * sizeof(float));
        const MTLUploadRing::Allocation C = InState.Upload(Colors.data(), Colors.size() * sizeof(float));
        if(!T.Buffer || !O.Buffer || !C.Buffer) return;
        [InEncoder setVertexBuffer:T.Buffer offset:T.Offset atIndex:Stream(QuadTransform)];
        [InEncoder setVertexBuffer:O.Buffer offset:O.Offset atIndex:Stream(QuadOpacity)];
        [InEncoder setVertexBuffer:C.Buffer offset:C.Offset atIndex:Stream(QuadColor)];
        [InEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:VertexCount instanceCount:Count];
        return;
    }

    [InEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:VertexCount];
}

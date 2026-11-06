#include "Private/GDI/Metal/MTLInternal.h"

#include <algorithm>
#include <cstring>

#include "Public/GDI/GDILog.h"

// ===========================================================================
// Decoders
// ===========================================================================

MTLBlendFactor MTLUtil::BlendFactor(const EBlendMode InMode)
{
    switch(InMode)
    {
        case BM_Zero:                     return MTLBlendFactorZero;
        case BM_One:                      return MTLBlendFactorOne;
        case BM_Src_Color:                return MTLBlendFactorSourceColor;
        case BM_One_Minus_Src_Color:      return MTLBlendFactorOneMinusSourceColor;
        case BM_Dest_Color:               return MTLBlendFactorDestinationColor;
        case BM_One_Minus_Dest_Color:     return MTLBlendFactorOneMinusDestinationColor;
        case BM_Src_Alpha:                return MTLBlendFactorSourceAlpha;
        case BM_One_Minus_Src_Alpha:      return MTLBlendFactorOneMinusSourceAlpha;
        case BM_Dest_Alpha:               return MTLBlendFactorDestinationAlpha;
        case BM_One_Minus_Dest_Alpha:     return MTLBlendFactorOneMinusDestinationAlpha;
        case BM_Constant_Color:           return MTLBlendFactorBlendColor;
        case BM_One_Minus_Constant_Color: return MTLBlendFactorOneMinusBlendColor;
        case BM_Constant_Alpha:           return MTLBlendFactorBlendAlpha;
        case BM_One_Minus_Constant_Alpha: return MTLBlendFactorOneMinusBlendAlpha;
        case BM_Src_Saturate:             return MTLBlendFactorSourceAlphaSaturated;
        default:                          return MTLBlendFactorZero;
    }
}

MTLBlendOperation MTLUtil::BlendOp(const EBlendOp InOp)
{
    switch(InOp)
    {
        case BO_Max: return MTLBlendOperationMax;
        case BO_Min: return MTLBlendOperationMin;
        case BO_Add:
        default:     return MTLBlendOperationAdd;
    }
}

MTLColorWriteMask MTLUtil::ColorMask(const EColorMask InMask)
{
    switch(InMask)
    {
        case EColM_R:    return MTLColorWriteMaskRed;
        case EColM_G:    return MTLColorWriteMaskGreen;
        case EColM_B:    return MTLColorWriteMaskBlue;
        case EColM_A:    return MTLColorWriteMaskAlpha;
        case EColM_RGB:  return MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        case EColM_RGBA: return MTLColorWriteMaskAll;
        default:         return MTLColorWriteMaskNone;
    }
}

MTLCompareFunction MTLUtil::Compare(const ECompareTestMode InMode)
{
    switch(InMode)
    {
        case CTM_Never:    return MTLCompareFunctionNever;
        case CTM_ALWAYS:   return MTLCompareFunctionAlways;
        case CTM_LESS:     return MTLCompareFunctionLess;
        case CTM_LEQUAL:   return MTLCompareFunctionLessEqual;
        case CTM_GREATER:  return MTLCompareFunctionGreater;
        case CTM_GEQUAL:   return MTLCompareFunctionGreaterEqual;
        case CTM_EQUAL:    return MTLCompareFunctionEqual;
        case CTM_NOTEQUAL: return MTLCompareFunctionNotEqual;
        default:           return MTLCompareFunctionLess;
    }
}

MTLPixelFormat MTLUtil::RenderTargetFormat(const EGDIRenderTargetFormat InFormat)
{
    switch(InFormat)
    {
        case GDI_RTF_RGBA16F: return MTLPixelFormatRGBA16Float;
        case GDI_RTF_RGB10A2: return MTLPixelFormatRGB10A2Unorm;
        case GDI_RTF_RGBA32F: return MTLPixelFormatRGBA32Float;
        case GDI_RTF_R32F:    return MTLPixelFormatR32Float;
        case GDI_RTF_RGBA8:
        default:              return MTLPixelFormatRGBA8Unorm;
    }
}

bool MTLUtil::IsBlendable(const MTLPixelFormat /*InFormat*/)
{
    // Every render-target format GDI makes blends on the Apple GPUs (M-series:
    // RGBA32Float and R32Float included).
    return true;
}

NSString* MTLUtil::ToNS(const std::string& InString)
{
    return [[NSString alloc] initWithBytes:InString.data() length:InString.size() encoding:NSUTF8StringEncoding];
}

void MTLUtil::LogError(const std::string& InWhat, NSError* InError)
{
    const char* Reason = InError ? [[InError localizedDescription] UTF8String] : "unknown error";
    GDI_LOG_ERROR("[Metal] " + InWhat + ": " + std::string(Reason ? Reason : ""))
}

// ===========================================================================
// Vertex layouts
// ===========================================================================

namespace
{
    void AddAttribute(MTLVertexDescriptor* InDesc, const NSUInteger InLocation, const MTLVertexFormat InFormat,
                      const NSUInteger InStream, const NSUInteger InOffset = 0)
    {
        InDesc.attributes[InLocation].format      = InFormat;
        InDesc.attributes[InLocation].offset      = InOffset;
        InDesc.attributes[InLocation].bufferIndex = MTLBindings::Stream(InStream);
    }

    void AddStream(MTLVertexDescriptor* InDesc, const NSUInteger InStream, const NSUInteger InStride, const bool bInPerInstance = false)
    {
        MTLVertexBufferLayoutDescriptor* Layout = InDesc.layouts[MTLBindings::Stream(InStream)];
        Layout.stride       = InStride;
        Layout.stepFunction = bInPerInstance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        Layout.stepRate     = 1;
    }
}

MTLVertexDescriptor* MTLPipelineCache::VertexDescriptor(const uint32_t InLayout)
{
    using namespace MTLBindings;
    MTLVertexDescriptor* Desc = [MTLVertexDescriptor vertexDescriptor];

    // Locations as the shaders declare them (Common.glsl, the material templates).
    AddAttribute(Desc, 0, MTLVertexFormatFloat3, Position);
    AddAttribute(Desc, 1, MTLVertexFormatFloat2, TexCoord);
    AddStream(Desc, Position, sizeof(float) * 3);
    AddStream(Desc, TexCoord, sizeof(float) * 2);

    if(InLayout == MVL_QuadInstanced)
    {
        for(NSUInteger Column = 0; Column < 4; ++Column)
        {
            AddAttribute(Desc, 2 + Column, MTLVertexFormatFloat4, QuadTransform, Column * 16);
        }
        AddAttribute(Desc, 6, MTLVertexFormatFloat,  QuadOpacity);
        AddAttribute(Desc, 7, MTLVertexFormatFloat3, QuadColor);
        AddStream(Desc, QuadTransform, sizeof(float) * 16, true);
        AddStream(Desc, QuadOpacity,   sizeof(float),      true);
        AddStream(Desc, QuadColor,     sizeof(float) * 3,  true);
        return Desc;
    }
    if(InLayout == MVL_Quad)
    {
        return Desc;
    }

    AddAttribute(Desc, 8,  MTLVertexFormatFloat3, Normal);
    AddAttribute(Desc, 9,  MTLVertexFormatFloat4, Color);
    AddAttribute(Desc, 10, MTLVertexFormatFloat2, UV1);
    AddAttribute(Desc, 13, MTLVertexFormatFloat4, Tangent);
    AddStream(Desc, Normal,  sizeof(float) * 3);
    AddStream(Desc, Color,   sizeof(float) * 4);
    AddStream(Desc, UV1,     sizeof(float) * 2);
    AddStream(Desc, Tangent, sizeof(float) * 4);

    if(InLayout == MVL_MeshInstanced)
    {
        // GDIBuffer.h's instance: a float4x4 transform, then a float4 colour, 80 bytes.
        for(NSUInteger Column = 0; Column < 4; ++Column)
        {
            AddAttribute(Desc, 2 + Column, MTLVertexFormatFloat4, Instance, Column * 16);
        }
        AddAttribute(Desc, 7, MTLVertexFormatFloat4, Instance, 64);
        AddStream(Desc, Instance, GDI_INSTANCE_STRIDE, true);
    }
    else if(InLayout == MVL_MeshSkinned)
    {
        AddAttribute(Desc, 11, MTLVertexFormatFloat4, Joints);
        AddAttribute(Desc, 12, MTLVertexFormatFloat4, Weights);
        AddStream(Desc, Joints,  sizeof(float) * 4);
        AddStream(Desc, Weights, sizeof(float) * 4);
    }
    return Desc;
}

// ===========================================================================
// Pipelines
// ===========================================================================

bool MTLPipelineCache::Key::operator==(const Key& InOther) const
{
    if(ColorCount != InOther.ColorCount) return false;
    for(uint32_t Index = 0; Index < GDI_MAX_COLOR_TARGETS; ++Index)
    {
        if(Colors[Index] != InOther.Colors[Index]) return false;
    }
    return VertexID == InOther.VertexID && FragmentID == InOther.FragmentID && Layout == InOther.Layout
        && bBlend == InOther.bBlend && Src == InOther.Src && Dst == InOther.Dst && ColorOp == InOther.ColorOp
        && AlphaOp == InOther.AlphaOp && WriteMask == InOther.WriteMask && DepthFormat == InOther.DepthFormat;
}

uint64_t MTLPipelineCache::Hash(const Key& InKey)
{
    uint64_t H = 0xcbf29ce484222325ULL;
    const auto Mix = [&H](const void* InData, const size_t InBytes)
    {
        const auto* Bytes = static_cast<const uint8_t*>(InData);
        for(size_t Index = 0; Index < InBytes; ++Index)
        {
            H ^= Bytes[Index];
            H *= 0x00000100000001B3ULL;
        }
    };
    Mix(&InKey.VertexID, sizeof(InKey.VertexID));
    Mix(&InKey.FragmentID, sizeof(InKey.FragmentID));
    Mix(&InKey.Layout, sizeof(InKey.Layout));
    Mix(&InKey.bBlend, sizeof(InKey.bBlend));
    Mix(&InKey.Src, sizeof(InKey.Src));
    Mix(&InKey.Dst, sizeof(InKey.Dst));
    Mix(&InKey.ColorOp, sizeof(InKey.ColorOp));
    Mix(&InKey.AlphaOp, sizeof(InKey.AlphaOp));
    Mix(&InKey.WriteMask, sizeof(InKey.WriteMask));
    Mix(&InKey.ColorCount, sizeof(InKey.ColorCount));
    Mix(InKey.Colors, sizeof(InKey.Colors));
    Mix(&InKey.DepthFormat, sizeof(InKey.DepthFormat));
    return H;
}

namespace
{
    /* The alpha factor for a colour one, as D3D12's ToAlphaBlend: alpha has no colour to read. */
    MTLBlendFactor ToAlphaFactor(const MTLBlendFactor InFactor)
    {
        switch(InFactor)
        {
            case MTLBlendFactorSourceColor:              return MTLBlendFactorSourceAlpha;
            case MTLBlendFactorOneMinusSourceColor:      return MTLBlendFactorOneMinusSourceAlpha;
            case MTLBlendFactorDestinationColor:         return MTLBlendFactorDestinationAlpha;
            case MTLBlendFactorOneMinusDestinationColor: return MTLBlendFactorOneMinusDestinationAlpha;
            case MTLBlendFactorBlendColor:               return MTLBlendFactorBlendAlpha;
            case MTLBlendFactorOneMinusBlendColor:       return MTLBlendFactorOneMinusBlendAlpha;
            default:                                     return InFactor;
        }
    }
}

id<MTLRenderPipelineState> MTLPipelineCache::GetRender(const Key& InKey, const MTLShaderResourceImpl* InVS,
                                                       const MTLShaderResourceImpl* InPS)
{
    if(!InVS || !InPS || !InVS->IsValid() || !InPS->IsValid()) return nil;

    const uint64_t H = Hash(InKey);
    {
        std::shared_lock<std::shared_mutex> Read(Lock);
        const auto Range = Pipelines.equal_range(H);
        for(auto It = Range.first; It != Range.second; ++It)
        {
            if(It->second.K == InKey) return It->second.Pipeline;
        }
    }

    MTLRenderPipelineDescriptor* Desc = [[MTLRenderPipelineDescriptor alloc] init];
    Desc.vertexFunction   = InVS->GetFunction();
    Desc.fragmentFunction = InPS->GetFunction();
    Desc.vertexDescriptor = VertexDescriptor(InKey.Layout);
    Desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    const uint32_t Count = std::max<uint32_t>(1, std::min<uint32_t>(InKey.ColorCount, GDI_MAX_COLOR_TARGETS));
    for(uint32_t Index = 0; Index < Count; ++Index)
    {
        MTLRenderPipelineColorAttachmentDescriptor* Color = Desc.colorAttachments[Index];
        Color.pixelFormat     = InKey.Colors[Index] != MTLPixelFormatInvalid ? InKey.Colors[Index] : MTLPixelFormatRGBA8Unorm;
        Color.writeMask       = InKey.WriteMask;
        Color.blendingEnabled = InKey.bBlend && MTLUtil::IsBlendable(Color.pixelFormat);
        Color.sourceRGBBlendFactor        = InKey.Src;
        Color.destinationRGBBlendFactor   = InKey.Dst;
        Color.rgbBlendOperation           = InKey.ColorOp;
        Color.sourceAlphaBlendFactor      = ToAlphaFactor(InKey.Src);
        Color.destinationAlphaBlendFactor = ToAlphaFactor(InKey.Dst);
        Color.alphaBlendOperation         = InKey.AlphaOp;
    }
    Desc.depthAttachmentPixelFormat = InKey.DepthFormat;

    NSError* Error = nil;
    id<MTLRenderPipelineState> Pipeline = [Device newRenderPipelineStateWithDescriptor:Desc error:&Error];
    if(!Pipeline)
    {
        MTLUtil::LogError("render pipeline (layout " + std::to_string(InKey.Layout) + ")", Error);
    }

    std::unique_lock<std::shared_mutex> Write(Lock);
    Pipelines.emplace(H, Entry{ InKey, Pipeline });
    return Pipeline;
}

id<MTLComputePipelineState> MTLPipelineCache::GetCompute(const MTLShaderResourceImpl* InCS)
{
    if(!InCS || !InCS->IsValid()) return nil;
    {
        std::shared_lock<std::shared_mutex> Read(Lock);
        const auto It = ComputePipelines.find(InCS->GetID());
        if(It != ComputePipelines.end()) return It->second;
    }
    NSError* Error = nil;
    id<MTLComputePipelineState> Pipeline = [Device newComputePipelineStateWithFunction:InCS->GetFunction() error:&Error];
    if(!Pipeline)
    {
        MTLUtil::LogError("compute pipeline", Error);
    }
    std::unique_lock<std::shared_mutex> Write(Lock);
    ComputePipelines[InCS->GetID()] = Pipeline;
    return Pipeline;
}

id<MTLDepthStencilState> MTLPipelineCache::GetDepthStencil(const bool bInTest, const bool bInWrite, const MTLCompareFunction InCompare)
{
    const uint32_t K = (bInTest ? 1u : 0u) | (bInWrite ? 2u : 0u) | (uint32_t(InCompare) << 2);
    {
        std::shared_lock<std::shared_mutex> Read(Lock);
        const auto It = DepthStencils.find(K);
        if(It != DepthStencils.end()) return It->second;
    }
    MTLDepthStencilDescriptor* Desc = [[MTLDepthStencilDescriptor alloc] init];
    Desc.depthCompareFunction = bInTest ? InCompare : MTLCompareFunctionAlways;
    Desc.depthWriteEnabled    = bInTest && bInWrite;
    id<MTLDepthStencilState> State = [Device newDepthStencilStateWithDescriptor:Desc];
    std::unique_lock<std::shared_mutex> Write(Lock);
    DepthStencils[K] = State;
    return State;
}

id<MTLSamplerState> MTLPipelineCache::GetSampler(const EGDITextureBorder InBorder, const EGDITextureFilter InFilter, const bool bInMips)
{
    const float    Anisotropy = GDIGetMaxAnisotropy();
    const uint32_t K = uint32_t(InBorder) | (uint32_t(InFilter) << 4) | (bInMips ? 0x100u : 0u);
    {
        std::shared_lock<std::shared_mutex> Read(Lock);
        const auto It = Samplers.find(K);
        if(It != Samplers.end()) return It->second;
    }
    MTLSamplerAddressMode Address = MTLSamplerAddressModeRepeat;
    switch(InBorder)
    {
        case GDI_TB_Mirrored_Repeat: Address = MTLSamplerAddressModeMirrorRepeat; break;
        case GDI_TB_Clamp_To_Edge:   Address = MTLSamplerAddressModeClampToEdge; break;
        case GDI_TB_Clamp_To_Border: Address = MTLSamplerAddressModeClampToBorderColor; break;
        case GDI_TB_Repeat:
        default:                     Address = MTLSamplerAddressModeRepeat; break;
    }
    const bool bLinear = InFilter != GDI_TF_Nearest;
    MTLSamplerDescriptor* Desc = [[MTLSamplerDescriptor alloc] init];
    Desc.sAddressMode = Address;
    Desc.tAddressMode = Address;
    Desc.rAddressMode = Address;
    Desc.borderColor  = MTLSamplerBorderColorTransparentBlack;
    Desc.minFilter    = bLinear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    Desc.magFilter    = Desc.minFilter;
    Desc.mipFilter    = bInMips ? (bLinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest) : MTLSamplerMipFilterNotMipmapped;
    Desc.maxAnisotropy = (bInMips && bLinear) ? NSUInteger(std::clamp(Anisotropy, 1.0f, 16.0f)) : 1;
    id<MTLSamplerState> Sampler = [Device newSamplerStateWithDescriptor:Desc];
    std::unique_lock<std::shared_mutex> Write(Lock);
    Samplers[K] = Sampler;
    return Sampler;
}

// ===========================================================================
// Upload ring
// ===========================================================================

void MTLUploadRing::Initialize(id<MTLDevice> InDevice, const uint32_t InSlots)
{
    Device = InDevice;
    Slots.assign(InSlots < 1 ? 1 : InSlots, Slot());
}

void MTLUploadRing::Rewind(const uint32_t InSlot)
{
    if(InSlot >= Slots.size()) return;
    Slot& Each = Slots[InSlot];
    // Keep the biggest chunk, drop the rest: a frame that needed more chains again.
    if(Each.Chunks.size() > 1)
    {
        auto Biggest = std::max_element(Each.Chunks.begin(), Each.Chunks.end(),
                                        [](const Chunk& A, const Chunk& B) { return A.Buffer.length < B.Buffer.length; });
        Chunk Keep = *Biggest;
        Each.Chunks.clear();
        Each.Chunks.push_back(Keep);
    }
    for(Chunk& C : Each.Chunks) { C.Used = 0; }
}

MTLUploadRing::Allocation MTLUploadRing::Allocate(const uint32_t InSlot, const NSUInteger InBytes)
{
    Allocation Out;
    if(InSlot >= Slots.size() || !Device) return Out;
    Slot& Each = Slots[InSlot];
    const NSUInteger Bytes = (std::max<NSUInteger>(InBytes, 16) + 255) & ~NSUInteger(255);
    if(Each.Chunks.empty() || Each.Chunks.back().Used + Bytes > Each.Chunks.back().Buffer.length)
    {
        const NSUInteger Previous = Each.Chunks.empty() ? 0 : Each.Chunks.back().Buffer.length;
        const NSUInteger Size     = std::max<NSUInteger>({ kChunkBytes, Previous * 2, Bytes });
        Chunk New;
        New.Buffer = [Device newBufferWithLength:Size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        if(!New.Buffer) return Out;
        New.Buffer.label = @"Nimble.UploadRing";
        Each.Chunks.push_back(New);
    }
    Chunk& C   = Each.Chunks.back();
    Out.Buffer = C.Buffer;
    Out.Offset = C.Used;
    Out.CPU    = static_cast<uint8_t*>(C.Buffer.contents) + C.Used;
    C.Used    += Bytes;
    return Out;
}

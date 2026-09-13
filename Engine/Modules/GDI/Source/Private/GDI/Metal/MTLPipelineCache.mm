#include "Private/GDI/Metal/MTLPipelineCache.h"
#include <cstdint>
#include "Public/GDI/Metal/MTLDevice.h"
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// ---------------------------------------------------------------------------
//  FNV-1a over the named key fields (never over the struct, which has padding)
// ---------------------------------------------------------------------------

static inline void MixBytes(uint64_t& H, const void* Data, size_t N)
{
    const auto* B = static_cast<const uint8_t*>(Data);
    for(size_t i = 0; i < N; ++i)
    {
        H ^= static_cast<uint64_t>(B[i]);
        H *= 0x00000100000001B3ULL;
    }
}

uint64_t MTLPipelineCache::HashPipelineKey(const PipelineKey& K)
{
    uint64_t H = 0xcbf29ce484222325ULL;
    MixBytes(H, &K.VertexFunction,   sizeof(K.VertexFunction));
    MixBytes(H, &K.FragmentFunction, sizeof(K.FragmentFunction));
    MixBytes(H, &K.VertexLayoutID,   sizeof(K.VertexLayoutID));
    MixBytes(H, &K.BlendEnable,      sizeof(K.BlendEnable));
    MixBytes(H, &K.SrcColorBlend,    sizeof(K.SrcColorBlend));
    MixBytes(H, &K.DstColorBlend,    sizeof(K.DstColorBlend));
    MixBytes(H, &K.SrcAlphaBlend,    sizeof(K.SrcAlphaBlend));
    MixBytes(H, &K.DstAlphaBlend,    sizeof(K.DstAlphaBlend));
    MixBytes(H, &K.ColorBlendOp,     sizeof(K.ColorBlendOp));
    MixBytes(H, &K.AlphaBlendOp,     sizeof(K.AlphaBlendOp));
    MixBytes(H, &K.ColorWriteMask,   sizeof(K.ColorWriteMask));
    MixBytes(H, &K.RTFormat,         sizeof(K.RTFormat));
    return H;
}

uint64_t MTLPipelineCache::HashDepthStencilKey(const DepthStencilKey& K)
{
    uint64_t H = 0xcbf29ce484222325ULL;
    MixBytes(H, &K.DepthTest,        sizeof(K.DepthTest));
    MixBytes(H, &K.DepthWrite,       sizeof(K.DepthWrite));
    MixBytes(H, &K.DepthCompare,     sizeof(K.DepthCompare));
    MixBytes(H, &K.StencilTest,      sizeof(K.StencilTest));
    MixBytes(H, &K.StencilCompare,   sizeof(K.StencilCompare));
    MixBytes(H, &K.StencilFail,      sizeof(K.StencilFail));
    MixBytes(H, &K.DepthFail,        sizeof(K.DepthFail));
    MixBytes(H, &K.DepthStencilPass, sizeof(K.DepthStencilPass));
    MixBytes(H, &K.StencilMask,      sizeof(K.StencilMask));
    return H;
}

// ---------------------------------------------------------------------------
//  Vertex input layouts -- the Metal mirror of VKPipelineCache::GetVertexInputState
// ---------------------------------------------------------------------------

MTLVertexDescriptor* MTLPipelineCache::BuildVertexDescriptor(uint32_t LayoutID)
{
    MTLVertexDescriptor* Desc = [MTLVertexDescriptor vertexDescriptor];   // autoreleased

    // attribute 0 -- vec3 position, its own buffer, per-vertex.
    Desc.attributes[0].format      = MTLVertexFormatFloat3;
    Desc.attributes[0].offset      = 0;
    Desc.attributes[0].bufferIndex = MTLBindings::PositionBuffer;
    Desc.layouts[MTLBindings::PositionBuffer].stride       = sizeof(float) * 3;
    Desc.layouts[MTLBindings::PositionBuffer].stepFunction = MTLVertexStepFunctionPerVertex;

    // attribute 1 -- vec2 texcoord. Basic_Instanced.vsh declares this input as a
    // float3; Metal pads the missing component with (0,0,0,1), exactly as Vulkan
    // does for a narrower attribute format, so one float2 buffer serves both.
    Desc.attributes[1].format      = MTLVertexFormatFloat2;
    Desc.attributes[1].offset      = 0;
    Desc.attributes[1].bufferIndex = MTLBindings::TexCoordBuffer;
    Desc.layouts[MTLBindings::TexCoordBuffer].stride       = sizeof(float) * 2;
    Desc.layouts[MTLBindings::TexCoordBuffer].stepFunction = MTLVertexStepFunctionPerVertex;

    if(LayoutID == MVL_Instanced)
    {
        // attributes 2-5 -- mat4 per-instance transform as four column vec4s.
        for(uint32_t Row = 0; Row < 4; ++Row)
        {
            Desc.attributes[2 + Row].format      = MTLVertexFormatFloat4;
            Desc.attributes[2 + Row].offset      = Row * sizeof(float) * 4;
            Desc.attributes[2 + Row].bufferIndex = MTLBindings::InstanceBuffer;
        }
        Desc.layouts[MTLBindings::InstanceBuffer].stride       = sizeof(float) * 16;
        Desc.layouts[MTLBindings::InstanceBuffer].stepFunction = MTLVertexStepFunctionPerInstance;
        Desc.layouts[MTLBindings::InstanceBuffer].stepRate     = 1;

        // attribute 6 -- per-instance opacity (drives per-stamp alpha for pressure).
        Desc.attributes[6].format      = MTLVertexFormatFloat;
        Desc.attributes[6].offset      = 0;
        Desc.attributes[6].bufferIndex = MTLBindings::OpacityBuffer;
        Desc.layouts[MTLBindings::OpacityBuffer].stride       = sizeof(float);
        Desc.layouts[MTLBindings::OpacityBuffer].stepFunction = MTLVertexStepFunctionPerInstance;
        Desc.layouts[MTLBindings::OpacityBuffer].stepRate     = 1;

        // attribute 7 -- per-instance color (pressure-driven color transfer).
        Desc.attributes[7].format      = MTLVertexFormatFloat3;
        Desc.attributes[7].offset      = 0;
        Desc.attributes[7].bufferIndex = MTLBindings::ColorBuffer;
        Desc.layouts[MTLBindings::ColorBuffer].stride       = sizeof(float) * 3;
        Desc.layouts[MTLBindings::ColorBuffer].stepFunction = MTLVertexStepFunctionPerInstance;
        Desc.layouts[MTLBindings::ColorBuffer].stepRate     = 1;
    }

    return Desc;
}

// ---------------------------------------------------------------------------
//  Life-cycle
// ---------------------------------------------------------------------------

bool MTLPipelineCache::Initialize(MTLDevice* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid())
    {
        Device = nullptr;
        return false;
    }
    GDI_LOG("[MTLPipelineCache] Initialized.")
    return true;
}

void MTLPipelineCache::Shutdown()
{
    for(void* P : Pipelines)
    {
        [(id<MTLRenderPipelineState>)P release];
    }
    Pipelines.clear();

    for(void* D : DepthStencilStates)
    {
        [(id<MTLDepthStencilState>)D release];
    }
    DepthStencilStates.clear();

    Device = nullptr;
}

// ---------------------------------------------------------------------------
//  Retrieval
// ---------------------------------------------------------------------------

id<MTLRenderPipelineState> MTLPipelineCache::GetOrCreatePipeline(const PipelineKey& Key)
{
    if(!Device || Key.VertexFunction == nullptr || Key.FragmentFunction == nullptr)
        return nil;

    const uint64_t H = HashPipelineKey(Key);

    auto It = Pipelines.find(H);
    if(It != Pipelines.end())
    {
        return (id<MTLRenderPipelineState>)It->second;
    }

    id<MTLRenderPipelineState> New = CreatePipeline(Key);   // +1 (or nil)
    // Cache nil too: a shader that fails to build will fail identically every
    // frame, and re-attempting the compile per draw would stall the render thread.
    Pipelines[H] = (void*)New;

#ifdef QT_DEBUG
    if(New != nil)
    {
        GDI_LOG(std::string("[MTLPipelineCache] New pipeline: VS=0x%1 FS=0x%2 vtxLayout=%3")
             .arg(reinterpret_cast<uintptr_t>(Key.VertexFunction),   0, 16)
             .arg(reinterpret_cast<uintptr_t>(Key.FragmentFunction), 0, 16)
             .arg(Key.VertexLayoutID))
    }
#endif

    return New;
}

id<MTLRenderPipelineState> MTLPipelineCache::CreatePipeline(const PipelineKey& Key) const
{
    MTLRenderPipelineDescriptor* Desc = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    Desc.vertexFunction   = (id<MTLFunction>)Key.VertexFunction;
    Desc.fragmentFunction = (id<MTLFunction>)Key.FragmentFunction;
    Desc.vertexDescriptor = BuildVertexDescriptor(Key.VertexLayoutID);

    MTLRenderPipelineColorAttachmentDescriptor* Color = Desc.colorAttachments[0];
    Color.pixelFormat                 = Key.RTFormat;
    Color.blendingEnabled             = Key.BlendEnable ? YES : NO;
    Color.sourceRGBBlendFactor        = Key.SrcColorBlend;
    Color.destinationRGBBlendFactor   = Key.DstColorBlend;
    Color.sourceAlphaBlendFactor      = Key.SrcAlphaBlend;
    Color.destinationAlphaBlendFactor = Key.DstAlphaBlend;
    Color.rgbBlendOperation           = Key.ColorBlendOp;
    Color.alphaBlendOperation         = Key.AlphaBlendOp;
    Color.writeMask                   = Key.ColorWriteMask;

    NSError* Error = nil;
    id<MTLRenderPipelineState> State =
        [Device->GetDevice() newRenderPipelineStateWithDescriptor:Desc error:&Error];   // +1
    if(State == nil)
    {
        const char* Reason = Error ? [[Error localizedDescription] UTF8String] : "unknown error";
        GDI_LOG(std::string("[MTLPipelineCache] newRenderPipelineState failed: ") + ::GDILogDetail::ToLogString(Reason))
    }
    return State;
}

id<MTLDepthStencilState> MTLPipelineCache::GetOrCreateDepthStencil(const DepthStencilKey& Key)
{
    if(!Device) return nil;

    const uint64_t H = HashDepthStencilKey(Key);

    auto It = DepthStencilStates.find(H);
    if(It != DepthStencilStates.end())
    {
        return (id<MTLDepthStencilState>)It->second;
    }

    MTLDepthStencilDescriptor* Desc = [[[MTLDepthStencilDescriptor alloc] init] autorelease];
    // Depth testing without a depth attachment is a no-op in Metal (as in Vulkan),
    // so a disabled state is expressed as Always/no-write rather than being skipped.
    Desc.depthCompareFunction = Key.DepthTest ? Key.DepthCompare : MTLCompareFunctionAlways;
    Desc.depthWriteEnabled    = Key.DepthWrite ? YES : NO;

    if(Key.StencilTest)
    {
        MTLStencilDescriptor* Stencil = [[[MTLStencilDescriptor alloc] init] autorelease];
        Stencil.stencilCompareFunction    = Key.StencilCompare;
        Stencil.stencilFailureOperation   = Key.StencilFail;
        Stencil.depthFailureOperation     = Key.DepthFail;
        Stencil.depthStencilPassOperation = Key.DepthStencilPass;
        Stencil.readMask                  = Key.StencilMask;
        Stencil.writeMask                 = Key.StencilMask;
        Desc.frontFaceStencil = Stencil;
        Desc.backFaceStencil  = Stencil;
    }

    id<MTLDepthStencilState> State = [Device->GetDevice() newDepthStencilStateWithDescriptor:Desc];  // +1
    DepthStencilStates[H] = (void*)State;
    return State;
}

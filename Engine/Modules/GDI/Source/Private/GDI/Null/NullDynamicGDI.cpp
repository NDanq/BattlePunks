#include "Public/GDI/Null/NullDynamicGDI.h"
#include "Public/GDI/Null/NullRenderTarget.h"
#include "Public/GDI/Null/NullShader.h"
#include "Public/GDI/Null/NullMesh.h"
#include "Public/GDI/Null/NullTexture.h"

void NullDynamicGDI::AssignContext(GDIContext* InContext)
{
    Context = InContext;
}

/*
 * The one line that matters most in this file. OpenGLDynamicGDI::Allocate()
 * bails out when Context is null (there is no drawable to make current), which
 * is exactly the state a headless host is in. Skipping SetThreadID() would
 * leave InRenderThread() false forever, and GDIRenderTarget::DrawTexture's
 * GDI_CHECK(GDIInRenderThread(), ...) is FATAL -- so a server calling that path
 * would abort at startup for lack of a GPU it was never going to use.
 */
void NullDynamicGDI::Allocate()
{
    SetThreadID(EngineCore::appThreadID());
    bInitialized = true;
    if(GSync)
    {
        GSync->Initialize();
    }
}

void NullDynamicGDI::Destroy()
{
    for(NullRecordingContext* Context : Contexts) { delete Context; }
    Contexts.clear();
    OpenChunks = 0;
    /* Nothing else to tear down -- this backend owns no device -- but the base
     * class still expects its caches empty by destruction time, and a caller
     * that loaded a shader against the Null backend has one to free. */
    ReleaseCachedResources();
}

void NullDynamicGDI::SwapBuffers()
{
    Stats.SwapBufferCalls++;
}

void NullDynamicGDI::Flush()
{
    Stats.FlushCalls++;
}

void NullDynamicGDI::Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth, uint InStencilMask)
{
    Stats.ClearCalls++;
    Stats.LastClearColor = InClearColor;
}

void NullDynamicGDI::SetViewport(uint InX, uint InY, uint InWidth, uint InHeight)
{
    Stats.LastViewportX = InX;
    Stats.LastViewportY = InY;
    Stats.LastViewportWidth = InWidth;
    Stats.LastViewportHeight = InHeight;

    /* As every backend: a viewport resets the scissor to itself. */
    Stats.LastScissorX      = InX;
    Stats.LastScissorY      = InY;
    Stats.LastScissorWidth  = InWidth;
    Stats.LastScissorHeight = InHeight;
}

void NullDynamicGDI::SetScissorRect(const uint InX, const uint InY, const uint InWidth, const uint InHeight)
{
    Stats.ScissorCalls++;
    Stats.LastScissorX      = InX;
    Stats.LastScissorY      = InY;
    Stats.LastScissorWidth  = InWidth;
    Stats.LastScissorHeight = InHeight;
}

void NullDynamicGDI::SetDepthState(IDepthStateDesc)
{
}

void NullDynamicGDI::SetBlendState(IBlendStateDesc)
{
}

void NullDynamicGDI::SetStencilState(IStencilStateDesc)
{
}

void NullDynamicGDI::SetColorMask(EColorMask)
{
}

void NullDynamicGDI::BeginRendering()
{
    Stats.BeginRenderingCalls++;
}

void NullDynamicGDI::EndRendering()
{
    Stats.EndRenderingCalls++;
}

GDIRenderTarget* NullDynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    Stats.RenderTargetsCreated++;
    return new NullRenderTarget(InWidth, InHeight);
}

void NullDynamicGDI::SetRenderTarget(GDIRenderTarget* InRenderTarget)
{
}

/* Any number of targets draws nothing as well as one does. */
void NullDynamicGDI::SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom)
{
}

void NullDynamicGDI::ReleaseRenderTarget(GDIRenderTarget* InRenderTarget)
{
    if(InRenderTarget)
    {
        InRenderTarget->Release();
    }
}

/*
 * SrcCode is deliberately never opened or parsed -- that is what lets
 * GDIGetShader<>() succeed in a process with no EngineCore::IFileProvider
 * installed. See the note on NullShaderResource.
 */
GDIShaderResource* NullDynamicGDI::CreateShaderResource(EShaderType InType, const std::string& SrcCode)
{
    Stats.ShaderResourcesCreated++;
    return new NullShaderResource(InType, SrcCode);
}

void NullDynamicGDI::DrawScreenQuadPrimitive()
{
    Stats.DrawScreenQuadCalls++;
}

/*
 * Overridden rather than inherited. GDIDynamicDriver::DrawObject (GDI.cpp)
 * GDI_CHECKs BoundVertexShader/BoundPixelShader -- never initialized by the
 * base constructor -- and then dereferences InObject->View and Mesh->Draw().
 * A server passing a null or partially-built GDIViewObject through the base
 * path would hit a fatal check or a crash; here it is just a counted no-op.
 */
void NullDynamicGDI::DrawObject(class GDIViewObject*)
{
    Stats.DrawObjectCalls++;
    Stats.PredicatedDrawCalls += bPredicated ? 1 : 0;
    if(InstanceBuffer)
    {
        Stats.InstancedDrawCalls++;
        Stats.LastInstanceCount = InstanceCount;
    }
}

GDIBuffer* NullDynamicGDI::CreateBuffer(const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
{
    Stats.BuffersCreated++;
    return new NullBuffer(InBytes, InUsage, InInitialData);
}

bool NullDynamicGDI::DispatchCompute(GDIShader*, const void*, const uint32_t InParamBytes, GDIBuffer* const*,
                                     const uint32_t InBufferCount, const uint32_t InX, const uint32_t InY, const uint32_t InZ)
{
    Stats.DispatchCalls++;
    Stats.LastDispatchGroups[0] = InX;
    Stats.LastDispatchGroups[1] = InY;
    Stats.LastDispatchGroups[2] = InZ;
    Stats.LastDispatchBuffers   = InBufferCount;
    Stats.LastDispatchParamBytes = InParamBytes;
    return true;
}

GDIOcclusionQueries* NullDynamicGDI::CreateOcclusionQueries(const uint32_t InCapacity)
{
    return new NullOcclusionQueries(InCapacity);
}

void NullDynamicGDI::BeginOcclusionRound(GDIOcclusionQueries* InSet, const uint32_t InCount)
{
    auto* Set = static_cast<NullOcclusionQueries*>(InSet);
    Set->Ended.assign(InCount, 0);
    Set->RoundBlock = Stats.BeginRenderingCalls;
    Set->bPending   = true;
    Stats.OcclusionRounds++;
}

void NullDynamicGDI::EndOcclusionQuery(GDIOcclusionQueries* InSet, const uint32_t InIndex)
{
    auto* Set = static_cast<NullOcclusionQueries*>(InSet);
    if(InIndex < Set->Ended.size())
    {
        Set->Ended[InIndex] = 1;
        Stats.OcclusionQueries++;
    }
}

void NullDynamicGDI::SetOcclusionPredicate(GDIOcclusionQueries* InSet, const int32_t InIndex)
{
    bPredicated = InSet && InIndex >= 0;
}

bool NullDynamicGDI::ReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible)
{
    auto* Set = static_cast<NullOcclusionQueries*>(InSet);
    if(!Set->bPending || Stats.BeginRenderingCalls <= Set->RoundBlock)
    {
        return false;
    }
    /* One never ended answers "visible": it has nothing to say. */
    OutVisible.resize(Set->Ended.size());
    for(size_t Index = 0; Index < Set->Ended.size(); ++Index)
    {
        OutVisible[Index] = Set->Ended[Index] && !bOcclusionAnswer ? 0 : 1;
    }
    Set->bPending = false;
    return true;
}

bool NullDynamicGDI::ReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes)
{
    OutBytes = static_cast<NullBuffer*>(InBuffer)->Data;
    return true;
}

void NullDynamicGDI::DrawObject(class GDIViewObject*, class GDIView*)
{
    Stats.DrawObjectCalls++;
    Stats.PredicatedDrawCalls += bPredicated ? 1 : 0;
}

void NullDynamicGDI::BeginDrawEvent(const std::string& EventName)
{
}

void NullDynamicGDI::EndDrawEvent()
{
}

void NullDynamicGDI::SetBoundShader(GDIShader* VS, GDIShader* PS)
{
    Stats.LastBoundVertexShader = VS;
    Stats.LastBoundPixelShader = PS;
    BoundVertexShader = VS;
    BoundPixelShader = PS;
}

GDIMesh* NullDynamicGDI::CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced)
{
    Stats.MeshesCreated++;
    auto* Mesh = new NullMesh();
    Mesh->SetSupportsInstanced(bIsInstanced);
    return Mesh;
}

GDIMesh* NullDynamicGDI::CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced)
{
    Stats.MeshesCreated++;
    auto* Mesh = new NullMesh();
    Mesh->SetSupportsInstanced(bIsInstanced);
    return Mesh;
}

GDIMesh* NullDynamicGDI::CreateMesh(const GDIMeshDesc& InDesc)
{
    GDIMesh::ORenderData Data;
    if(!GDIMakeIndexedRenderData(InDesc, Data)) return nullptr;
    Stats.MeshesCreated++;
    return new NullMesh(Data);
}

GDIMesh* NullDynamicGDI::CreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    Stats.MeshesCreated++;
    Stats.DynamicMeshesCreated++;
    return new NullMesh(InMaxVertices, InMaxIndices);
}

GDITextureResource* NullDynamicGDI::CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    Stats.TextureResourcesCreated++;
    return new NullTextureResource(InImage, 0, InBorder, InFilter);
}

GDITextureResource* NullDynamicGDI::CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    Stats.TextureResourcesCreated++;
    return new NullTextureResource(InParent, InImage, 0, InBorder, InFilter);
}

GDITextureResource* NullDynamicGDI::CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
{
    Stats.TextureResourcesCreated++;
    return new NullTextureResource(InParent, InBorder, InFilter);
}

bool NullDynamicGDI::CheckError(const std::string& FailStr)
{
    // false means "no error occurred" -- matches VulkanDynamicGDI::CheckError
    // and MetalDynamicGDI::CheckError, both of which return false here too.
    return false;
}

EngineCore::Color NullDynamicGDI::GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size&)
{
    return EngineCore::Color();
}

bool NullDynamicGDI::BeginParallelPass(const uint32_t InChunks, GDIRecordingContext** OutContexts)
{
    if(OpenChunks > 0 || InChunks == 0) return false;
    while(Contexts.size() < InChunks) { Contexts.push_back(new NullRecordingContext()); }
    for(uint32_t Chunk = 0; Chunk < InChunks; ++Chunk)
    {
        NullRecordingContext& Context = *Contexts[Chunk];
        Context = NullRecordingContext();
        Context.BoundVertexShader = BoundVertexShader;
        Context.BoundPixelShader  = BoundPixelShader;
        Context.InstanceBuffer    = InstanceBuffer;
        Context.InstanceCount     = InstanceCount;
        Context.Index             = Chunk;
        OutContexts[Chunk]        = &Context;
    }
    OpenChunks = InChunks;
    return true;
}

void NullDynamicGDI::EndParallelPass()
{
    for(uint32_t Chunk = 0; Chunk < OpenChunks; ++Chunk)
    {
        const NullRecordingContext& Context = *Contexts[Chunk];
        Stats.DrawObjectCalls     += Context.Draws;
        Stats.PredicatedDrawCalls += Context.Predicated;
        Stats.InstancedDrawCalls  += Context.Instanced;
        if(Context.Instanced > 0) { Stats.LastInstanceCount = Context.LastInstanceCount; }
    }
    Stats.ParallelPasses += OpenChunks > 0 ? 1 : 0;
    Stats.ParallelChunks += OpenChunks;
    OpenChunks = 0;
}

void NullDynamicGDI::RecordDrawObject(GDIRecordingContext* InContext, class GDIViewObject*, class GDIView*)
{
    auto& Context = *static_cast<NullRecordingContext*>(InContext);
    ++Context.Draws;
    Context.Predicated += Context.PredicateSet && Context.PredicateIndex >= 0 ? 1 : 0;
    if(Context.InstanceBuffer)
    {
        ++Context.Instanced;
        Context.LastInstanceCount = Context.InstanceCount;
    }
}

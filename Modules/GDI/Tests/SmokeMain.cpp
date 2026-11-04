/*
 * GDISmokeTest -- links GDI and EngineCore only, deliberately not the Qt
 * bridge (see CMakeLists.txt). Its main job here is to exercise NullGDI, the
 * headless/server render backend: InitializeGDI(ERP_Null, ...) with no
 * window, no GPU context, and -- importantly -- no EngineCore::IFileProvider
 * installed at all. If NullGDI ever reaches for a file or a real GPU handle,
 * this test is where that shows up.
 *
 * A log handler is installed before anything else so a GDI_CHECK Fatal
 * (CoreLog.h) returns instead of aborting -- same reasoning as
 * EngineCoreSmokeTest: we want to report every failure, not die on the
 * first one.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <string>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreImage.h"

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIRenderTarget.h"
#include "Public/GDI/GDITexture.h"
#include "Public/GDI/GDIView.h"
#include "Public/GDI/GDIQuery.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/Null/NullDynamicGDI.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"

using namespace EngineCore;

static int GFailures = 0;
static int GChecks   = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static int GFatalCalls = 0;

static void TestLogHandler(const LogVerbosity Verbosity, const char* /*Message*/)
{
    if(Verbosity == LogVerbosity::Fatal)
    {
        ++GFatalCalls;
    }
}

// ---------------------------------------------------------------------------

static void TestInitAndPlatform()
{
    std::printf("[init]\n");

    InitializeGDI(ERP_Null, WindowHandle());
    Expect(GDynamicGDI != nullptr, "GDynamicGDI is set after InitializeGDI(ERP_Null)");
    Expect(GSync != nullptr, "GSync is set after InitializeGDI(ERP_Null)");
    Expect(GDIGetRenderPlatform() == ERP_Null, "GDIGetRenderPlatform reports ERP_Null");

    // Behaviour #1: Allocate() must latch the render thread without a
    // context. AssignContextGDI(nullptr) is legal input -- NullGDI never
    // dereferences its context.
    AssignContextGDI(nullptr);
    AllocateResourcesGDI();
    Expect(GDIInRenderThread(), "the calling thread is the render thread after AllocateResourcesGDI()");
}

static void TestFreeFunctionSweep()
{
    std::printf("[free functions]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    GDIBeginRendering();
    GDIClear(CM_Color | CM_Depth, EngineCore::Color(1, 2, 3), 1.0f, 0);
    GDISetViewport(0, 0, 640, 480);
    GDISetDepthState(IDepthStateDesc());
    GDISetBlendState(IBlendStateDesc());
    GDISetStencilState(IStencilStateDesc());
    GDISetColorMask(EColM_RGBA);
    GDIBeginDrawEvent("SmokeTestEvent");
    GDIEndDrawEvent();
    GDIEndRendering();
    GDISwapBuffers();
    GDIFlush();

    const NullGDIStats& Stats = Null->GetStats();
    Expect(Stats.ClearCalls == 1, "one Clear call counted");
    Expect(Stats.LastClearColor.R == 1 && Stats.LastClearColor.G == 2 && Stats.LastClearColor.B == 3,
           "Clear's color is recorded");
    Expect(Stats.LastViewportWidth == 640 && Stats.LastViewportHeight == 480, "SetViewport is recorded");
    Expect(Stats.BeginRenderingCalls == 1, "one BeginRendering call counted");
    Expect(Stats.EndRenderingCalls == 1, "one EndRendering call counted");
    Expect(Stats.SwapBufferCalls == 1, "one SwapBuffers call counted");
    Expect(Stats.FlushCalls == 1, "one Flush call counted");
    Expect(!GDICheckError("smoke"), "CheckError reports no error");
    Expect(GDIGetColorFromSamplePoint(Point(), Size()) == EngineCore::Color(), "GetColorFromSamplePoint returns a default-constructed Color");
}

static void TestResourceCreation()
{
    std::printf("[resource creation]\n");

    // GDIRenderTargetManager::Resize()/Release() (GDIRenderTarget.h) and
    // GDIShader::UpdateUserDefinedUniforms() (GDIShader.cpp) both dereference
    // whatever GDICreateXxx hands back with no null check -- every one of
    // these must be non-null.
    GDIRenderTarget* RT = GDICreateRenderTarget("SmokeRT", 64, 64);
    Expect(RT != nullptr, "GDICreateRenderTarget returns non-null");
    Expect(RT->IsValid(), "a Null render target reports itself valid");
    Expect(RT->ToGDITexture2D() != nullptr, "a Null render target's ToGDITexture2D is non-null");

    GDIMesh* Mesh = GDICreateQuadMesh(64.0f, 64.0f);
    Expect(Mesh != nullptr, "GDICreateQuadMesh(w,h) returns non-null");

    const EngineCore::Image ScratchImage(4, 4, EngineCore::EImageFormat::RGBA8);
    GDITextureResource* Tex = GDICreateTextureResource(ScratchImage);
    Expect(Tex != nullptr, "GDICreateTextureResource(Image) returns non-null");

    GDIShaderResource* Shader = GDICreateShaderResource(ST_Vertex, "Basic.vsh");
    Expect(Shader != nullptr, "GDICreateShaderResource returns non-null");

    // These three were created directly (not through GDIShader/GDITexture2D
    // ownership), so this test owns them and cleans them up itself.
    Tex->Release();
    delete Tex;
    Shader->Release();
    delete Shader;
    Mesh->Release();
    delete Mesh;
    GDIReleaseRenderTarget(RT);
    delete RT;

    // An indexed mesh: two triangles over four vertices.
    const float    Positions[] = { 0, 0, 0,   1, 0, 0,   1, 1, 0,   0, 1, 0 };
    const float    Normals[]   = { 0, 0, -1,  0, 0, -1,  0, 0, -1,  0, 0, -1 };
    const float    UVs[]       = { 0, 0,      1, 0,      1, 1,      0, 1 };
    const uint32_t Indices[]   = { 0, 1, 2,   0, 2, 3 };

    GDIMeshDesc Desc;
    Desc.Positions   = Positions;
    Desc.Normals     = Normals;
    Desc.UV0         = UVs;
    Desc.NumVertices = 4;
    Desc.Indices     = Indices;
    Desc.NumIndices  = 6;

    GDIMesh* Indexed = GDICreateMesh(Desc);
    Expect(Indexed != nullptr && Indexed->IsIndexed() && Indexed->GetNumIndices() == 6,
           "GDICreateMesh builds an indexed mesh");
    if(Indexed)
    {
        Indexed->SetDrawRange(3, 3);
        Indexed->Draw();
        Indexed->Release();
        delete Indexed;
    }

    const uint32_t BadIndices[] = { 0, 1, 4 };
    Desc.Indices    = BadIndices;
    Desc.NumIndices = 3;
    Expect(GDICreateMesh(Desc) == nullptr, "GDICreateMesh refuses an index past the last vertex");

    Desc.Indices    = Indices;
    Desc.NumIndices = 5;
    Expect(GDICreateMesh(Desc) == nullptr, "GDICreateMesh refuses a partial triangle");
}

static void TestScissorAndDynamicMesh()
{
    std::printf("[scissor and dynamic mesh]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    const NullGDIStats& Stats = Null->GetStats();

    // From the top-left of the viewport: the target's rows, top first.
    GDISetViewport(0, 0, 200, 100);
    Expect(Stats.LastScissorWidth == 200 && Stats.LastScissorHeight == 100, "SetViewport resets the scissor to the viewport");
    GDISetScissorRect(10, 20, 30, 40);
    Expect(Stats.LastScissorX == 10 && Stats.LastScissorY == 20 &&
           Stats.LastScissorWidth == 30 && Stats.LastScissorHeight == 40,
           "GDISetScissorRect's top-left rectangle is the target's rows, unflipped");
    GDISetScissorRect(190, 90, 50, 50);
    Expect(Stats.LastScissorX == 190 && Stats.LastScissorY == 90 &&
           Stats.LastScissorWidth == 10 && Stats.LastScissorHeight == 10,
           "GDISetScissorRect clips to the viewport");
    GDISetScissorRect(500, 500, 5, 5);
    Expect(Stats.LastScissorWidth == 0 && Stats.LastScissorHeight == 0, "a scissor outside the viewport is empty");

    GDISetViewport(50, 10, 100, 80);
    GDISetScissorRect(0, 0, 100, 20);
    Expect(Stats.LastScissorX == 50 && Stats.LastScissorY == 10 && Stats.LastScissorHeight == 20,
           "GDISetScissorRect is relative to an offset viewport");
    GDIClearScissorRect();
    Expect(Stats.LastScissorX == 50 && Stats.LastScissorY == 10 && Stats.LastScissorWidth == 100 && Stats.LastScissorHeight == 80,
           "GDIClearScissorRect restores the viewport");

    const float    Positions[] = { 0, 0, 0,   1, 0, 0,   1, 1, 0,   0, 1, 0 };
    const float    UVs[]       = { 0, 0,      1, 0,      1, 1,      0, 1 };
    const uint32_t Indices[]   = { 0, 1, 2,   0, 2, 3 };
    GDIMeshDesc Desc;
    Desc.Positions   = Positions;
    Desc.UV0         = UVs;   // no normals: a dynamic mesh does not need them
    Desc.NumVertices = 4;
    Desc.Indices     = Indices;
    Desc.NumIndices  = 6;

    Expect(GDICreateDynamicMesh(0, 6) == nullptr, "GDICreateDynamicMesh refuses no capacity");
    GDIMesh* Dynamic = GDICreateDynamicMesh(10, 15);
    Expect(Dynamic != nullptr && Dynamic->IsDynamic() && Dynamic->GetDynamicVertexCapacity() == 10 &&
           Dynamic->GetDynamicIndexCapacity() == 15, "GDICreateDynamicMesh makes a dynamic mesh of its capacity");
    if(!Dynamic) return;
    Expect(!Dynamic->IsIndexed(), "a dynamic mesh draws nothing before its first update");

    GDIBeginRendering();
    Expect(GDIUpdateDynamicMesh(Dynamic, Desc) && Dynamic->IsIndexed() && Dynamic->GetNumIndices() == 6,
           "the first update in a block is drawn");
    Expect(GDIUpdateDynamicMesh(Dynamic, Desc), "a second update in the same block appends");
    Expect(!GDIUpdateDynamicMesh(Dynamic, Desc), "an update past the capacity in one block is refused");
    Expect(Dynamic->GetNumIndices() == 6, "a refused update leaves the mesh as it was");
    GDIEndRendering();

    GDIBeginRendering();
    Expect(GDIUpdateDynamicMesh(Dynamic, Desc), "the next block starts the mesh over");
    const uint32_t BadIndices[] = { 0, 1, 4 };
    Desc.Indices    = BadIndices;
    Desc.NumIndices = 3;
    Expect(!GDIUpdateDynamicMesh(Dynamic, Desc), "an update with an index past its vertices is refused");
    GDIEndRendering();

    delete Dynamic;
}

/* A shader object for the checks below: Null resources read no file. */
class TestComputeShader : public GDIShader
{
public:
    void InitUniformBindings() override {}
};

/*
 * Compute and instancing (GDIBuffer.h; GDIDispatchCompute,
 * GDISetInstanceBuffer): what the front end accepts and refuses, on Null,
 * which "supports" compute so a caller's whole path runs without a GPU.
 */
static void TestComputeAndInstancing()
{
    std::printf("[compute and instancing]\n");
    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    Expect(GDISupportsCompute(), "Null reports compute, so callers' paths run headless");
    Expect(GDICreateBuffer(0, GDI_BU_Storage) == nullptr && GDICreateBuffer(64, 0) == nullptr,
           "a buffer needs bytes and a usage");
    const uint32_t Initial[4] = { 1, 2, 3, 4 };
    GDIBuffer* Storage = GDICreateBuffer(sizeof(Initial), GDI_BU_Storage, Initial);
    GDIBuffer* Zeroed  = GDICreateBuffer(64, GDI_BU_Storage | GDI_BU_Instance);
    std::vector<uint8_t> Bytes;
    Expect(Storage && Zeroed && Null->GetStats().BuffersCreated == 2 && GDIReadBuffer(Storage, Bytes)
           && Bytes.size() == sizeof(Initial) && std::memcmp(Bytes.data(), Initial, sizeof(Initial)) == 0,
           "a buffer starts with its data, and reads back");
    Expect(GDIReadBuffer(Zeroed, Bytes) && Bytes.size() == 64 && Bytes[0] == 0 && Bytes[63] == 0, "or with zeros");
    if(!Storage || !Zeroed) return;

    TestComputeShader Compute;
    Compute.InitResource(ST_Compute, "Test.csh");
    TestComputeShader Pixel;
    Pixel.InitResource(ST_Pixel, "Test.fsh");
    const float Params[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    GDIBuffer* Both[2] = { Storage, Zeroed };
    GDIBuffer* Five[5] = { Storage, Storage, Storage, Storage, Storage };
    GDIBuffer* None[1] = { nullptr };

    Expect(GDIDispatchCompute(&Compute, Params, sizeof(Params), Both, 2, 4, 2, 1) && Null->GetStats().DispatchCalls == 1
           && Null->GetStats().LastDispatchGroups[0] == 4 && Null->GetStats().LastDispatchGroups[1] == 2
           && Null->GetStats().LastDispatchBuffers == 2 && Null->GetStats().LastDispatchParamBytes == sizeof(Params),
           "a dispatch goes through with its groups, buffers and parameters");
    Expect(!GDIDispatchCompute(&Pixel, Params, sizeof(Params), Both, 2, 1)
           && !GDIDispatchCompute(nullptr, Params, sizeof(Params), Both, 2, 1),
           "only a compute shader dispatches");
    Expect(!GDIDispatchCompute(&Compute, Params, GDI_COMPUTE_PARAMS_MAX_BYTES + 4, Both, 2, 1)
           && !GDIDispatchCompute(&Compute, Five, 0, Five, 5, 1)
           && !GDIDispatchCompute(&Compute, nullptr, 0, None, 1, 1)
           && !GDIDispatchCompute(&Compute, nullptr, 0, Both, 2, 0),
           "too many parameter bytes or buffers, a missing buffer, or no groups: refused");
    GDIBuffer* InstanceOnly = GDICreateBuffer(GDI_INSTANCE_STRIDE, GDI_BU_Instance);
    GDIBuffer* WithInstance[1] = { InstanceOnly };
    Expect(InstanceOnly && !GDIDispatchCompute(&Compute, nullptr, 0, WithInstance, 1, 1), "a buffer a shader writes must be a storage buffer");
    Expect(Null->GetStats().DispatchCalls == 1, "a refused dispatch reaches no backend");

    /* Zeroed holds 64 bytes: no whole instance (80). */
    GDIBuffer* Instances = GDICreateBuffer(2 * GDI_INSTANCE_STRIDE, GDI_BU_Storage | GDI_BU_Instance);
    GDISetInstanceBuffer(Instances, 2);
    GDIDrawObject(nullptr);
    Expect(Null->GetStats().InstancedDrawCalls == 1 && Null->GetStats().LastInstanceCount == 2,
           "after GDISetInstanceBuffer a draw is instanced, that many times");
    GDISetInstanceBuffer(Instances, 3);
    GDIDrawObject(nullptr);
    GDISetInstanceBuffer(Zeroed, 1);
    GDIDrawObject(nullptr);
    GDISetInstanceBuffer(Storage, 1);
    GDIDrawObject(nullptr);
    Expect(Null->GetStats().InstancedDrawCalls == 1, "more instances than the buffer holds, or a storage-only buffer: one instance");
    GDISetInstanceBuffer(nullptr, 0);
    GDIDrawObject(nullptr);
    Expect(Null->GetStats().InstancedDrawCalls == 1 && Null->GetStats().DrawObjectCalls == 5, "and null goes back to one");

    /* Updates in frame order (GDIUpdateBuffer) and draws from a buffer's indices (GDISetIndexBuffer). */
    Expect(GDISupportsIndexBuffers() && GDISupportsMeshletCulling(), "Null reports index buffers and meshlet culling");
    const uint32_t Later[2] = { 7, 8 };
    Expect(GDIUpdateBuffer(Storage, 8, Later, sizeof(Later)) && GDIReadBuffer(Storage, Bytes) && Bytes.size() == sizeof(Initial)
           && std::memcmp(Bytes.data() + 8, Later, sizeof(Later)) == 0 && std::memcmp(Bytes.data(), Initial, 8) == 0
           && Null->GetStats().BufferUpdates == 1 && Null->GetStats().BufferUpdateBytes == sizeof(Later),
           "an update writes its bytes at its offset, and leaves the rest");
    Expect(!GDIUpdateBuffer(Storage, 12, Later, sizeof(Later)) && !GDIUpdateBuffer(Storage, 2, Later, 4)
           && !GDIUpdateBuffer(Storage, 0, Later, 6) && !GDIUpdateBuffer(Storage, 0, nullptr, 4)
           && !GDIUpdateBuffer(nullptr, 0, Later, 4) && !GDIUpdateBuffer(Storage, 0, Later, 0)
           && Null->GetStats().BufferUpdates == 1,
           "past the end, unaligned, empty or missing: refused, and nothing reaches the backend");

    GDIBuffer* Indices = GDICreateBuffer(64, GDI_BU_Storage | GDI_BU_Index);
    Expect(Indices && (Indices->GetUsage() & GDI_BU_Index) != 0, "an index buffer is a buffer like any other");
    GDISetIndexBuffer(Indices);
    GDIDrawObject(nullptr);
    Expect(Null->GetStats().IndexBufferDrawCalls == 1, "after GDISetIndexBuffer a draw reads its indices");
    GDISetIndexBuffer(Storage);
    GDIDrawObject(nullptr);
    Expect(Null->GetStats().IndexBufferDrawCalls == 1, "a buffer without GDI_BU_Index is not taken");
    GDISetIndexBuffer(nullptr);
    GDIDrawObject(nullptr);
    Expect(Null->GetStats().IndexBufferDrawCalls == 1, "and null goes back to the mesh's own");

    for(GDIBuffer* Each : { Storage, Zeroed, InstanceOnly, Instances, Indices }) { if(Each) Each->Release(); }
}

/*
 * Parallel passes (GDIBeginParallelPass; RenderCore's MULTITHREADED_RENDERING.md,
 * phase 2), on Null: four threads record at once, each through its own
 * context. Each sees only its own state, the render thread's is untouched,
 * nothing reaches the driver until the pass ends, and then every draw is
 * counted as DrawObject counts it. (Expect is not thread-safe: the threads
 * note what they saw, the checks run after they join.)
 */
static void TestParallelPasses()
{
    std::printf("[parallel passes]\n");
    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    const NullGDIStats Before = Null->GetStats();

    const GDIParallelRecordingSupport Support = GDIGetParallelRecordingSupport();
    Expect(Support.bSupported && Support.bSerialOnly, "Null supports parallel recording, its chunks serial");

    GDIRecordingContext* Contexts[GDI_PARALLEL_CHUNKS_MAX] = {};
    Expect(!GDIBeginParallelPass(0, Contexts) && !GDIBeginParallelPass(GDI_PARALLEL_CHUNKS_MAX + 1, Contexts),
           "a pass of no chunks, or too many, is refused");

    constexpr uint32_t Chunks = 4, DrawsPerChunk = 250;
    GDIBuffer*           Instances = GDICreateBuffer(4 * GDI_INSTANCE_STRIDE, GDI_BU_Storage | GDI_BU_Instance);
    GDIOcclusionQueries* Queries   = GDICreateOcclusionQueries(8);
    Expect(GDIBeginParallelPass(Chunks, Contexts), "a parallel pass of four chunks begins");
    Expect(!GDIBeginParallelPass(1, Contexts), "and no second one while it is open");

    struct Seen
    {
        bool       bInRenderThread = false;
        bool       bCurrent        = false;
        bool       bNestedRefused  = false;
        uint8_t    Block0          = 0;
        GDIShader* VS              = nullptr;
        uint       ViewportWidth   = 0;
        uint32_t   DrawFirst       = 0;
        uint32_t   Recorded        = 0;
    } Results[Chunks];
    std::vector<std::thread> Threads;
    for(uint32_t Chunk = 0; Chunk < Chunks; ++Chunk)
    {
        Threads.emplace_back([&, Chunk]()
        {
            GDISetCurrentRecording(Contexts[Chunk]);
            GDIRecordingContext* Dummy[1] = {};
            Results[Chunk].bNestedRefused = !GDIBeginParallelPass(1, Dummy);
            GDIViewObject Object{};
            for(uint32_t Draw = 0; Draw < DrawsPerChunk; ++Draw)
            {
                uint8_t Bytes[16];
                std::memset(Bytes, int(Chunk * 10 + Draw % 7), sizeof(Bytes));
                GDISetBoundShader(reinterpret_cast<GDIShader*>(uintptr_t(0x1000) * (Chunk + 1)), nullptr);
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Bytes, sizeof(Bytes));
                GDISetViewport(0, 0, 100 + Chunk, 50);
                GDISetDrawRange(nullptr, Draw, 3);
                GDISetOcclusionPredicate(Draw % 2 ? Queries : nullptr, Draw % 2 ? int32_t(Chunk) : -1);
                const bool bLast = Chunk == 3 && Draw == DrawsPerChunk - 1;
                GDISetInstanceBuffer(bLast ? Instances : nullptr, bLast ? 3 : 0);
                GDIDrawObject(&Object);
            }
            Seen& Mine           = Results[Chunk];
            Mine.bInRenderThread = GDIInRenderThread();
            Mine.bCurrent        = GDIGetCurrentRecording() == Contexts[Chunk];
            Mine.Block0          = Contexts[Chunk]->MaterialBlocks[0].empty() ? 0 : Contexts[Chunk]->MaterialBlocks[0][0];
            Mine.VS              = Contexts[Chunk]->BoundVertexShader;
            Mine.ViewportWidth   = Contexts[Chunk]->ViewportWidth;
            Mine.DrawFirst       = Contexts[Chunk]->DrawFirst;
            Mine.Recorded        = Contexts[Chunk]->DrawsRecorded;
            GDISetCurrentRecording(nullptr);
        });
    }
    for(std::thread& Each : Threads) { Each.join(); }

    bool bAllOwn = true, bAllCounted = true, bAllRender = true, bAllNested = true;
    for(uint32_t Chunk = 0; Chunk < Chunks; ++Chunk)
    {
        const Seen& Each = Results[Chunk];
        bAllOwn = bAllOwn && Each.bCurrent && Each.Block0 == uint8_t(Chunk * 10 + (DrawsPerChunk - 1) % 7)
               && Each.VS == reinterpret_cast<GDIShader*>(uintptr_t(0x1000) * (Chunk + 1))
               && Each.ViewportWidth == 100 + Chunk && Each.DrawFirst == DrawsPerChunk - 1;
        bAllCounted = bAllCounted && Each.Recorded == DrawsPerChunk;
        bAllRender  = bAllRender && Each.bInRenderThread;
        bAllNested  = bAllNested && Each.bNestedRefused;
    }
    Expect(bAllOwn, "each thread's setters wrote its own context: shaders, blocks, viewport, draw range");
    Expect(bAllCounted, "each context counted its own draws");
    Expect(bAllRender, "a thread recording a chunk counts as the render thread");
    Expect(bAllNested, "a recording thread cannot begin a pass of its own");
    Expect(GDIGetCurrentRecording() == nullptr, "the render thread has no context of its own");
    Expect(Null->GetStats().DrawObjectCalls == Before.DrawObjectCalls && Null->GetStats().LastViewportWidth == Before.LastViewportWidth,
           "nothing reached the driver's own state while the pass was open");

    GDIEndParallelPass();
    const NullGDIStats After = Null->GetStats();
    Expect(After.DrawObjectCalls == Before.DrawObjectCalls + Chunks * DrawsPerChunk, "at the pass's end every chunk's draws are counted");
    Expect(After.PredicatedDrawCalls == Before.PredicatedDrawCalls + Chunks * DrawsPerChunk / 2, "the predicated ones as predicated");
    Expect(After.InstancedDrawCalls == Before.InstancedDrawCalls + 1 && After.LastInstanceCount == 3, "and the instanced one as instanced");
    Expect(After.ParallelPasses == Before.ParallelPasses + 1 && After.ParallelChunks == Before.ParallelChunks + Chunks,
           "one pass, four chunks");
    Expect(GDIBeginParallelPass(2, Contexts), "a pass ended, the next can begin");
    GDIEndParallelPass();

    if(Instances) { Instances->Release(); }
    if(Queries)   { Queries->Release(); }
}

static void TestRenderTargetManagerCycle()
{
    std::printf("[render target manager]\n");

    // The unguarded-dereference path: every method here calls straight
    // through GDIRenderTarget* with no null check.
    GDIRenderTargetManager Manager;
    Manager.Allocate(64, 64);
    Expect(Manager.GetMain() != nullptr, "manager's Main target is non-null after Allocate");
    Expect(Manager.GetBuffer1() != nullptr, "manager's Buffer1 target is non-null after Allocate");

    Manager.Resize(128, 128);
    Expect(Manager.GetMain()->GetWidth() == 128, "Resize updates the Main target's width");

    Manager.ResolveCanvasBuffer();
    Manager.RestoreCanvasBuffer();
    Manager.Release();
}

static void TestDrawObjectOverrides()
{
    std::printf("[DrawObject]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    // GDIDynamicDriver::DrawObject (GDI.cpp) GDI_CHECKs BoundVertexShader /
    // BoundPixelShader -- never initialized by the base constructor -- and
    // then dereferences InObject->View and Mesh->Draw(). NullDynamicGDI
    // overrides both overloads instead of inheriting them, so a null object
    // here must be a harmless counted no-op, not a fatal or a crash.
    GDIDrawObject(nullptr);
    GDIDrawObject(nullptr, nullptr);

    Expect(Null->GetStats().DrawObjectCalls == 2, "both DrawObject overloads route to NullDynamicGDI and are counted");
    Expect(GFatalCalls == 0, "no GDI_CHECK fired while exercising DrawObject with null input");
}

/*
 * Exercises GDIRenderTarget::DrawTexture, which is the one call site that
 * combines GDI_CHECK(GDIInRenderThread(), ...) (fatal if Allocate() failed to
 * latch the thread) with GDIGetShader<Screen_VS/Screen_PS>() (fatal if no
 * file provider is reachable).
 *
 * The texture handed to DrawTexture is deliberately never freed here:
 * Screen_PS::SetScreenBlitTexture stores it in the shader's Uniforms map, and
 * GDIShader::CleanupAllocatedUniformData (GDIShader.cpp) deletes whatever is
 * stored there -- unconditionally, with no null check -- when the shader
 * itself is destroyed at ReleaseGDI(). Freeing it here as well would be a
 * double free; leaving Screen_PS's texture uniform at its default nullptr
 * would instead crash on that same unconditional delete. A single
 * test-owned, never-touched-again texture is the safe shape for this call.
 */
static void TestRenderTargetDrawTexture()
{
    std::printf("[DrawTexture]\n");

    auto* Null = static_cast<NullDynamicGDI*>(GDynamicGDI);
    Null->ResetStats();

    GDIRenderTarget* RT = GDICreateRenderTarget("DrawTextureRT", 32, 32);
    const EngineCore::Image ScratchImage(32, 32, EngineCore::EImageFormat::RGBA8);
    auto* ScratchTexture = new GDITexture2D(ScratchImage);

    RT->DrawTexture(ScratchTexture);

    Expect(GFatalCalls == 0, "DrawTexture does not hit a fatal GDI_CHECK on the render thread");
    Expect(Null->GetStats().DrawScreenQuadCalls == 1, "DrawTexture issues one DrawScreenQuadPrimitive call");
    Expect(Null->GetStats().LastBoundVertexShader != nullptr, "DrawTexture binds a vertex shader");
    Expect(Null->GetStats().LastBoundPixelShader != nullptr, "DrawTexture binds a pixel shader");

    GDIReleaseRenderTarget(RT);
    delete RT;
    // ScratchTexture is intentionally not deleted -- see the function comment.
}

static void TestRelease()
{
    std::printf("[release]\n");

    ReleaseGDI();
    Expect(GDynamicGDI == nullptr, "GDynamicGDI is null after ReleaseGDI");
    Expect(GSync == nullptr, "GSync is null after ReleaseGDI");
}

int main()
{
    SetLogHandler(&TestLogHandler);

    TestInitAndPlatform();
    TestFreeFunctionSweep();
    TestResourceCreation();
    TestScissorAndDynamicMesh();
    TestComputeAndInstancing();
    TestRenderTargetManagerCycle();
    TestDrawObjectOverrides();
    TestParallelPasses();
    TestRenderTargetDrawTexture();
    TestRelease();

    std::printf("\n%d/%d checks passed\n", GChecks - GFailures, GChecks);
    return GFailures == 0 ? 0 : 1;
}

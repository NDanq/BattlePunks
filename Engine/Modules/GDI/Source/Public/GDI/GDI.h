#ifndef CORECLIENT_GDI_H
#define CORECLIENT_GDI_H


#include "Public/Core/CoreQueue.h"
#include "Public/Core/CorePlatform.h"

#include "GDIDefinitions.h"
#include "GDIBuffer.h"
#include "GDIQuery.h"
#include "GDIShader.h"
#include "GDIMesh.h"
#include "GDISync.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
#include <atomic>
#include <unordered_map>
#include <vector>

class GDIContext;
class GDIRenderTarget;

#ifndef IMPLEMENT_GDI_FUNCTION_DECLARATION
#define IMPLEMENT_GDI_FUNCTION_DECLARATION( Return, Name, Args ) \
    Return GDI##Name Args;
#endif //IMPLEMENT_GDI_FUNCTION_DECLARATION()

#ifndef IMPLEMENT_GDI_FUNCTION_BODY
#if __clang__
#define IMPLEMENT_GDI_FUNCTION_BODY( Return, Name, ArgsDecl, ReturnStatement, ArgsLit ) \
    Return GDI##Name ArgsDecl                                                           \
    {                                                                                   \
       if( GDynamicGDI )                                                                \
       {                                                                                \
         ReturnStatement GDynamicGDI->Name ArgsLit;                                     \
       }\
    }
#else
#define IMPLEMENT_GDI_FUNCTION_BODY( Return, Name, ArgsDecl, ReturnStatement, ArgsLit ) \
    Return GDI##Name ArgsDecl                                                           \
    {                                                                                   \
        if(GDynamicGDI)                                                                 \
        {                                                                               \
            ReturnStatement GDynamicGDI->Name ArgsLit;                                  \
        }                                                                               \
    }
#endif

#endif //IMPLEMENT_GDI_FUNCTION_BODY


#ifndef IMPLEMENT_GDI_FUNCTION_BODY_RET
#if __clang__
#define IMPLEMENT_GDI_FUNCTION_BODY_RET( Return, Name, ArgsDecl, ReturnStatement, ArgsLit ) \
    Return GDI##Name ArgsDecl                                                           \
    {                                                                                   \
        if(GDynamicGDI)                                                                 \
        {                                                                               \
            ReturnStatement GDynamicGDI->Name ArgsLit;                                \
        }                                                                               \
        else                                                                            \
        {                                                                               \
            return NULL;                                                                 \
        }                                                                               \
    }
#else
#define IMPLEMENT_GDI_FUNCTION_BODY_RET( Return, Name, ArgsDecl, ReturnStatement, ArgsLit ) \
    Return GDI##Name ArgsDecl                                                           \
    {                                                                                   \
        if(GDynamicGDI)                                                                 \
        {                                                                               \
            ReturnStatement GDynamicGDI->Name ArgsLit;                                  \
        }                                                                               \
        else                                                                            \
        {                                                                               \
            return NULL;                                                                 \
        }                                                                               \
    }
#endif
#endif

class GDIRenderCommandBase
{
public:
    GDIRenderCommandBase() = default;

    /*
     * The drain loop owns these and deletes them through this base pointer,
     * while ENQUEUE_RENDERCOMMAND_* news a DERIVED type whose members are
     * whatever the call site captured -- std::string among them. Deleting a
     * derived object through a base without a virtual destructor is undefined
     * behaviour and leaks every non-trivial member, so this is load-bearing
     * rather than defensive.
     */
    virtual ~GDIRenderCommandBase() = default;

    virtual void Execute() = 0;
};

extern EngineCore::ThreadSafeQueue<GDIRenderCommandBase*> GRenderCommands;

/*
 * TOPOLOGY DIAGNOSTIC -- migration risk F4.
 *
 * These started as a transitional check from when GRenderCommands was a
 * QQueue offering no synchronization, written from the UI thread and drained
 * by QCanvasRenderer::Tick. That container swap has since happened -- the
 * declaration above is an EngineCore::ThreadSafeQueue -- so the data race they
 * were watching for is gone, and the note that used to promise their removal
 * at that point went with it.
 *
 * They are kept because they answer a question the container cannot: a
 * thread-safe queue is not the same claim as single-producer/single-consumer
 * discipline on top of it. These record the first thread seen in each role and
 * complain once if a second appears, which is what turns "the game and render
 * threads are split" from an assumption into an observation.
 *
 * NoteDrain/NoteDrainEnd bracket the drain loop as a whole, not each command:
 * the thread_local flag they raise between them is what lets an enqueue issued
 * from inside a command's Execute() be told apart from an ordinary game-thread
 * enqueue.
 */
void GDIRenderQueue_NoteEnqueue();
void GDIRenderQueue_NoteDrain();
void GDIRenderQueue_NoteDrainEnd();

/*
 * There is deliberately no zero-parameter ENQUEUE_RENDERCOMMAND.
 *
 * The macro that used to sit here was syntactically broken -- missing a
 * semicolon after `= default` and after the class body -- so it could never
 * have compiled, and therefore never had a caller. It was removed rather than
 * ported. Add one back only if something actually needs it, and compile it.
 */

#ifndef ENQUEUE_RENDERCOMMAND_ONEPARAM
#define ENQUEUE_RENDERCOMMAND_ONEPARAM(Name, Param1Type, Param1Name, Param1Data, Code) \
    class Name##RenderCommand : public GDIRenderCommandBase                            \
    {                                                                                  \
    public:                                                                            \
        Name##RenderCommand() = default;                                               \
        Name##RenderCommand(Param1Type InData)                                         \
        {                                                                              \
            Param1Name = InData;                                                       \
        }                                                                              \
        void Execute() override Code                                                   \
    private:                                                                           \
        Param1Type Param1Name;                                                         \
    };                                                                                 \
    GDIRenderQueue_NoteEnqueue(),                                  \
    GRenderCommands.Enqueue(new Name##RenderCommand(Param1Data))
#endif

#ifndef ENQUEUE_RENDERCOMMAND_TWOPARAM
#define ENQUEUE_RENDERCOMMAND_TWOPARAM(Name,    Param1Type, Param1Name, Param1Data,     \
                                                Param2Type, Param2Name, Param2Data, Code) \
    class Name##RenderCommand : public GDIRenderCommandBase                            \
    {                                                                                  \
    public:                                                                            \
        Name##RenderCommand() = default;                                               \
        Name##RenderCommand(Param1Type InData, Param2Type InData2)                     \
        {                                                                              \
            Param1Name = InData;                                                       \
            Param2Name = InData2;                                                      \
        }                                                                              \
        void Execute() override   Code                                                 \
    private:                                                                           \
        Param1Type Param1Name;                                                         \
        Param2Type Param2Name;                                                         \
    };                                                                                 \
    GDIRenderQueue_NoteEnqueue(),                                  \
    GRenderCommands.Enqueue(new Name##RenderCommand(Param1Data, Param2Data))
#endif


/* The render platform is chosen by the host and passed in; GDI does not
 * reach into application settings to discover it. */
void InitializeGDI( ERenderPlatform Platform, EngineCore::WindowHandle InWindow );
void AssignContextGDI( GDIContext* InContext );
void AllocateResourcesGDI();
void ReleaseGDI();

/*
 * A plain class: no QObject, no Q_OBJECT, no QOffscreenSurface.
 *
 * It derived from QObject for exactly one reason -- so the host could call
 * moveToThread() on it -- and owned a QOffscreenSurface only the OpenGL
 * backend ever touched. Neither belonged in a renderer:
 *
 *   - The thread affinity was Qt bookkeeping. GDI's own render-thread check is
 *     SetThreadID/InRenderThread over appThreadID(), which needs no QObject.
 *   - The drawable is the host's business now (see IGDIGLContext). Vulkan and
 *     Metal never used the surface at all; the app had to guard every access
 *     with `if(Context->Surface)`.
 *
 * Dropping QObject is what lets AUTOMOC go: this Q_OBJECT was the only one in
 * the entire GDI project.
 */
class GDIContext
{
public:
    enum GDIContextType : uint
    {
        OpenGLContext = 0,
        VulkanContext,
        MetalContext,
        NullContext,
        D3D12Context,
        MAX_GDIContextType
    };

    GDIContext()
    {
        ContextType = MAX_GDIContextType;
    }

    virtual ~GDIContext() = default;

    GDIContextType          ContextType;
};


/**
 * What a backend offers for recording draws on several threads (parallel
 * command recording: Vulkan secondary command buffers, D3D12 command lists,
 * Metal's parallel render encoder), for RenderCore's gate. Nothing records in
 * parallel yet; this is the capability, not the switch ([Rendering]
 * ParallelRecording decides that).
 *
 *   bSupported            chunks of draws may be recorded on other threads
 *                         and executed in order on the frame's primary
 *   bSerialOnly           the chunked path runs, but every chunk on the
 *                         calling thread (Null: for headless tests)
 *   bInheritedQueries     an occlusion query begun on the primary may stay
 *                         open across executed chunks (Vulkan inheritedQueries)
 *   bInheritedPredication a predicate begun on the primary applies inside
 *                         chunks (Vulkan inheritedConditionalRendering)
 *   Mechanism             how, for the log
 *   Reason                why not, when !bSupported
 */
struct GDIParallelRecordingSupport
{
    bool        bSupported            = false;
    bool        bSerialOnly           = false;
    bool        bInheritedQueries     = false;
    bool        bInheritedPredication = false;
    const char* Mechanism             = "";
    const char* Reason                = "";
};

class GDIOcclusionQueries;
class GDIBuffer;

/**
 * One thread's draw state while it records a chunk of a parallel pass
 * (GDIBeginParallelPass; RenderCore's MULTITHREADED_RENDERING.md, phase 2).
 *
 * While a context is current on a thread (GDISetCurrentRecording), the draw
 * path's GDI* setters -- GDISetBoundShader, GDISetUniformBlock,
 * GDISetMaterialTexture, the depth, blend, stencil and colour-mask states,
 * GDISetViewport and the scissor, GDISetInstanceBuffer, GDISetIndirectArgs,
 * GDISetOcclusionPredicate, GDISetDrawRange -- write into it instead of the
 * driver, and GDIDrawObject records into its own command buffer. So several
 * threads record at once, each into its own context, with no shared state.
 * Anything else (render targets, clears, queries, dispatches, resources) is
 * the render thread's alone, outside the pass.
 *
 * The backend's subclass adds its command buffer and caches. A context starts
 * each pass with the driver's state as it was at GDIBeginParallelPass.
 */
class GDIRecordingContext
{
public:
    virtual ~GDIRecordingContext() = default;

    GDIShader*            BoundVertexShader = nullptr;
    GDIShader*            BoundPixelShader  = nullptr;
    /* [0] material, [1] scene, [2] skin, [3] previous skin, [4] light, as the driver's. */
    std::vector<uint8_t>  MaterialBlocks[GDI_UNIFORM_BLOCK_COUNT];
    uint64_t              MaterialBlockSerials[GDI_UNIFORM_BLOCK_COUNT] = {};
    GDITextureResource*   MaterialTextures[GDI_MATERIAL_TEXTURE_SLOTS] = {};
    GDIBuffer*            InstanceBuffer     = nullptr;
    uint32_t              InstanceCount      = 0;
    GDIBuffer*            IndirectArgs       = nullptr;
    uint32_t              IndirectArgsOffset = 0;
    GDIOcclusionQueries*  PredicateSet       = nullptr;
    int32_t               PredicateIndex     = -1;
    uint                  ViewportX = 0, ViewportY = 0, ViewportWidth = 0, ViewportHeight = 0;
    /* GDISetDrawRange's: in a context the range belongs to the draw, not to
     * the mesh, which other threads may be drawing (0 and 0: the mesh's own). */
    uint32_t              DrawFirst = 0;
    uint32_t              DrawCount = 0;
    /* Which of the pass's contexts: its chunk, executed in this order. */
    uint32_t              Index = 0;
    /* Draws recorded into it this pass. */
    uint32_t              DrawsRecorded = 0;
};

/* GDISetUniformBlock's storage, shared by the driver and recording contexts:
 * InData into the block InBinding names, its serial bumped if the bytes
 * changed. False for a binding that is not a block. */
bool GDIStoreUniformBlock(std::vector<uint8_t> (&InOutBlocks)[GDI_UNIFORM_BLOCK_COUNT], uint64_t (&InOutSerials)[GDI_UNIFORM_BLOCK_COUNT], uint InBinding,
                          const void* InData, uint InSize);

struct GDITextureMemoryStats;   // below, with GDIGetTextureMemoryStats

class GDIDynamicDriver
{
public:
    /*
     * The QObject* parent this used to take was stored and never read -- Qt
     * parenting bought nothing here, since GDI owns its objects explicitly.
     * Removed rather than carried through the migration.
     */
    explicit GDIDynamicDriver(EngineCore::WindowHandle InWindow)
    : Window(InWindow),
    Context(nullptr),
    ThreadID(0),
    bInitialized(false){ }

    virtual ~GDIDynamicDriver()
    {
        /* A BACKEND MUST HAVE CALLED ReleaseCachedResources() BY NOW, from its
         * own Destroy(), or this is a use-after-free rather than a cleanup:
         * every cached shader and texture holds a raw pointer to the DEVICE,
         * and ~VulkanDynamicGDI (like every other backend) runs Destroy() --
         * which deletes that device -- BEFORE this base destructor runs.
         *
         * The crash that proved it, with the debug heap's freed-memory fill
         * still in the register:
         *
         *   vulkan_1!vkResetEvent            ds:feeefeee`feeefeee
         *   RenderCore!VKShaderResource::Release
         *   RenderCore!GDIShader::~GDIShader
         *   RenderCore!GDIDynamicDriver::~GDIDynamicDriver   <- here
         *   RenderCore!VulkanDynamicGDI::~VulkanDynamicGDI
         *
         * Kept as a safety net rather than removed, because a backend that
         * never allocated anything still has to be destructible: both maps are
         * cleared by ReleaseCachedResources(), so this loop does nothing on the
         * correct path and only catches a backend that forgot to call it --
         * where leaking is the better failure than dereferencing freed memory.
         */
        GDI_CHECK(LoadedShaderMap.empty() && LoadedTextures.empty(),
                  "[GDI] ~GDIDynamicDriver: cached resources still live -- the backend's "
                  "Destroy() must call ReleaseCachedResources() before deleting its device");
    }

    /*
     * Releases every cached shader and texture, while the device they point at
     * is STILL ALIVE.
     *
     * EVERY BACKEND'S Destroy() MUST CALL THIS, first, before tearing down its
     * device -- see ~GDIDynamicDriver for what happens otherwise. Idempotent:
     * a second call finds both maps empty, which is what makes it safe for a
     * Destroy() that early-outs on !bInitialized and for one that runs twice.
     */
    void ReleaseCachedResources()
    {
        // std::unordered_map iterates key/value PAIRS, where QHash iterated
        // values -- hence .second rather than the bare element.
        for(auto& Entry : LoadedShaderMap)
        {
            delete Entry.second;
        }
        LoadedShaderMap.clear();

        for(auto& Entry : LoadedTextures)
        {
            delete Entry.second;
        }
        LoadedTextures.clear();
    }

    /* Assign Context */
    virtual void AssignContext( GDIContext* InContext ) = 0;

    /* Allocate Render Device */
    virtual void Allocate() = 0;

    /* Destroy Render device */
    virtual void Destroy() = 0;

    /* Swap buffers and present back buffer to screen */
    virtual void SwapBuffers() = 0;

    /* Flush all render commands */
    virtual void Flush() = 0;

    /* Clear current render target. */
    virtual void Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth = 1.0f, uint InStencilMask = 0x00) = 0;

    /*
     * ORIENTATION, on every backend: a render target is stored top-down. Its
     * row 0 is the top of the image and is NDC y = +1, texture v = 0 when the
     * target is sampled, gl_FragCoord.y = 0.5 when it is drawn, and the first
     * row ToImage returns -- the order RenderDoc and EngineCore::Image use.
     * Vulkan gets there with a negative viewport height, OpenGL with
     * glClipControl(GL_UPPER_LEFT); D3D12 and Metal are top-down natively.
     * Viewport and scissor rectangles count rows from that top.
     */
    virtual void SetViewport(uint InX, uint InY, uint InWidth, uint InHeight) = 0;

    /*
     * Draws after this touch only InWidth x InHeight pixels at InX, InY -- in
     * the render target's own rows, row 0 its top (as GDISetScissorRect takes
     * them, from the viewport's top-left). Clear ignores it, and the next SetViewport
     * resets it to the viewport. The default ignores it: a backend without
     * scissoring draws unclipped.
     */
    virtual void SetScissorRect(uint /*InX*/, uint /*InY*/, uint /*InWidth*/, uint /*InHeight*/) {}

    virtual void SetDepthState(IDepthStateDesc) = 0;

    virtual void SetBlendState(IBlendStateDesc) = 0;

    virtual void SetStencilState(IStencilStateDesc) = 0;

    virtual void SetColorMask(EColorMask) = 0;

    virtual void BeginRendering() = 0;

    virtual void EndRendering() = 0;

    virtual GDIRenderTarget* CreateRenderTarget(std::string Name, uint InWidth, uint InHeight) = 0;

    /* A render target of InFormat. This default makes RGBA8 whatever is
     * asked: a backend that can do more overrides both of these. */
    virtual GDIRenderTarget* CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat /*InFormat*/)
    {
        return CreateRenderTarget(std::move(Name), InWidth, InHeight);
    }
    virtual bool SupportsRenderTargetFormat(EGDIRenderTargetFormat InFormat) const { return InFormat == GDI_RTF_RGBA8; }

    virtual void SetRenderTarget( GDIRenderTarget* InRenderTarget ) = 0;

    /*
     * One pass writing InCount colour targets (at most GDI_MAX_COLOR_TARGETS,
     * all the same size) and the depth attachment of InDepthFrom, or none when
     * it is null. Fragment output N goes to InColors[N]; blend and colour mask
     * apply to every target; Clear clears them all. SetRenderTarget(RT) is
     * SetRenderTargets(&RT, 1, RT). A target whose colour or depth is not bound
     * is kept sampleable (ToGDITexture2D, ToDepthTexture2D).
     *
     * The default binds InColors[0] alone, for a backend without it (Metal).
     */
    virtual void SetRenderTargets( GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom );

    virtual void ReleaseRenderTarget( GDIRenderTarget* InRenderTarget ) = 0;

    virtual GDIShaderResource* CreateShaderResource(EShaderType InType, const std::string& SourceCode) = 0;

    virtual void DrawScreenQuadPrimitive() = 0;

    virtual void DrawObject(class GDIViewObject*);

    virtual void DrawObject(class GDIViewObject*, class GDIView*);

    virtual void BeginDrawEvent(const std::string& EventName) = 0;

    virtual void EndDrawEvent() = 0;

    /* Whether BeginDrawEvent records anything a capture tool sees: what a
     * caller asks before building a label it would otherwise throw away. */
    virtual bool HasDrawEvents() const { return true; }

    /**
     * GPU timestamps, for timing GPU work ("stat frame"). Within one
     * BeginRendering/EndRendering block, WriteGPUTimestamp(i) records the time
     * the GPU reaches that point, for i below GDI_GPU_TIMESTAMP_MAX. A block is
     * complete by the BeginRendering GDIGetFramesInFlight() blocks after it,
     * so from then until the next one GetPreviousGPUTimestamps gives that
     * block's: OutMilliseconds[i] for each bit i of the returned mask, on one
     * GPU clock (only differences mean anything). A backend that cannot
     * time -- the default -- writes nothing and returns 0.
     */
    virtual bool     SupportsGPUTimestamps() const { return false; }
    virtual void     WriteGPUTimestamp(uint32_t /*InIndex*/) {}
    virtual uint64_t GetPreviousGPUTimestamps(double* /*OutMilliseconds*/) { return 0; }

    /**
     * Occlusion queries (GDIQuery.h; validated by the GDI* functions). A
     * backend without them keeps these defaults: none made, nothing done.
     * SupportsOcclusionPredication: whether GDISetOcclusionPredicate skips
     * draws, or is ignored and they all draw.
     */
    /**
     * Parallel draw recording (GDIGetParallelRecordingSupport): whether this
     * backend could record draws on several threads and execute them in
     * order. A backend that cannot keeps this default.
     */
    virtual GDIParallelRecordingSupport GetParallelRecordingSupport() const
    {
        GDIParallelRecordingSupport Out;
        Out.Reason = "this backend records every draw on one thread";
        return Out;
    }

    /**
     * Parallel passes (GDIBeginParallelPass). A backend that records in
     * parallel makes InChunks contexts recording inside the bound targets'
     * rendering and returns them; EndParallelPass executes them in order and
     * reopens the rendering as before. The Record* hooks are the GDI* setters
     * while a context is current. Defaults: no parallel passes.
     */
    virtual bool BeginParallelPass(uint32_t /*InChunks*/, GDIRecordingContext** /*OutContexts*/) { return false; }
    virtual void EndParallelPass() {}
    virtual void RecordDepthState(GDIRecordingContext* /*InContext*/, IDepthStateDesc /*InDesc*/) {}
    virtual void RecordBlendState(GDIRecordingContext* /*InContext*/, IBlendStateDesc /*InDesc*/) {}
    virtual void RecordStencilState(GDIRecordingContext* /*InContext*/, IStencilStateDesc /*InDesc*/) {}
    virtual void RecordColorMask(GDIRecordingContext* /*InContext*/, EColorMask /*InMask*/) {}
    virtual void RecordViewport(GDIRecordingContext* /*InContext*/, uint, uint, uint, uint) {}
    virtual void RecordScissorRect(GDIRecordingContext* /*InContext*/, uint, uint, uint, uint) {}
    virtual void RecordDrawObject(GDIRecordingContext* /*InContext*/, class GDIViewObject* /*InObject*/, class GDIView* /*InView*/) {}
    virtual void RecordBeginDrawEvent(GDIRecordingContext* /*InContext*/, const std::string& /*InName*/) {}
    virtual void RecordEndDrawEvent(GDIRecordingContext* /*InContext*/) {}

    virtual bool                 SupportsOcclusionQueries() const { return false; }
    virtual bool                 SupportsOcclusionPredication() const { return false; }
    virtual GDIOcclusionQueries* CreateOcclusionQueries(uint32_t /*InCapacity*/) { return nullptr; }
    virtual void                 BeginOcclusionRound(GDIOcclusionQueries* /*InSet*/, uint32_t /*InCount*/) {}
    virtual void                 BeginOcclusionQuery(GDIOcclusionQueries* /*InSet*/, uint32_t /*InIndex*/) {}
    virtual void                 EndOcclusionQuery(GDIOcclusionQueries* /*InSet*/, uint32_t /*InIndex*/) {}
    virtual void                 ResolveOcclusionQueries(GDIOcclusionQueries* /*InSet*/) {}
    virtual void                 SetOcclusionPredicate(GDIOcclusionQueries* /*InSet*/, int32_t /*InIndex*/) {}
    virtual bool                 ReadOcclusionResults(GDIOcclusionQueries* /*InSet*/, std::vector<uint8_t>& /*OutVisible*/) { return false; }

    /**
     * Present the most recent rendered frame to the supplied window.
     * Called from the UI thread after RendererFinished(). OpenGL keeps its existing
     * QOpenGLContext-based present path and overrides this as a no-op; Vulkan
     * uses this to acquire a swapchain image, blit the offscreen RT, and present.
     */
    virtual void PresentToWindow(EngineCore::WindowHandle /*InWindow*/) {}

    /**
     * Present InSource -- one finished render target, not necessarily the last
     * one bound -- to InWindow. UI thread, like PresentToWindow. Each window gets
     * its own swapchain on first use, so several windows can each show their
     * own target. Skipped while InSource is mid-render, as PresentToWindow is.
     * Backends that present differently (OpenGL, Null) ignore it.
     */
    virtual void PresentRenderTargetToWindow(EngineCore::WindowHandle /*InWindow*/, GDIRenderTarget* /*InSource*/) {}

    /**
     * Destroys InWindow's swapchain, if it has one. UI thread. Call before the
     * native window is destroyed; presenting to it again creates a new one.
     */
    virtual void ReleaseWindow(EngineCore::WindowHandle /*InWindow*/) {}

    template<typename GDIShaderRef = GDIShader()> void RegisterShader(std::string SrcPath, EShaderType InType)
    {
        const std::string ShaderKey = TYPEID_TO_QSTR(GDIShaderRef);
        RegisteredShaderMap[ShaderKey] = new GDIShaderData( SrcPath, InType);
    }

    template<typename GDIShaderRef = GDIShader()> GDIShaderRef* GetShader()
    {
        const std::string ShaderKey = TYPEID_TO_QSTR(GDIShaderRef);
        GDIShader* Shader = LoadedShaderMap[ShaderKey];
        if(!Shader)
        {
            GDIShaderData* ShaderData = RegisteredShaderMap[ShaderKey];
            if(!ShaderData)
            {
                GDI_CHECK(0, "[GDI] Failed to find " + ShaderKey + "! Forgot to register?");
                return nullptr;
            }

            Shader = new GDIShaderRef(ShaderKey);
            if(Shader)
            {
                Shader->InitResource(ShaderData->ShaderType,
                                     ShaderData->ShaderPath );
                Shader->InitUniformBindings();
                LoadedShaderMap[ShaderKey] = Shader;
            }
        }

        if(GDIShaderRef* LoadedShader = (GDIShaderRef*)Shader)
        {
            return LoadedShader;
        }
        return nullptr;
    }

    void RegisterTexture(const std::string& TextureKey, GDITexture2D* InTexture)
    {
        GDI_LOG("[DynamicGDI] Registered Tex: " + TextureKey)
        LoadedTextures[TextureKey] = InTexture;
    }

    GDITexture2D* GetTexture(const std::string& TextureKey)
    {
        GDITexture2D* Texture = nullptr;
        Texture = LoadedTextures[TextureKey];
//        if(Texture == nullptr)
//
//            GDI_LOG("[DynamicGDI] Failed to find texture: " + TextureKey)
//        }
        return Texture;
    }

    virtual void SetBoundShader(GDIShader*, GDIShader*) = 0;

    /*
     * Material draw state (GDIDefinitions.h): raw std140 bytes for binding 2
     * or 3, copied, and a texture per slot. Read by a draw only when a bound
     * shader UsesMaterialLayout(); persists until changed. A null texture
     * draws the backend's fallback.
     */
    void SetUniformBlock(uint InBinding, const void* InData, uint InSize);
    void SetMaterialTexture(uint InSlot, GDITextureResource* InTexture);
    /* InTexture is being destroyed: clear every material slot that still
     * holds it. A slot keeps its texture until changed, and a draw reads all
     * of them, so a texture freed while still in one (a streamed texture
     * retired after frames with no material draws) would otherwise reach the
     * next draw as a dangling pointer. */
    void ForgetTextureResource(const GDITextureResource* InTexture);

    /* Whether the bound pair asks for the material layout. */
    bool IsMaterialDraw() const;

    virtual GDIMesh* CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) = 0;

    virtual GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) = 0;

    /* An indexed mesh with normals (GDIMeshDesc), drawn by range
     * (SetDrawRange). Null if unsupported or InDesc is invalid. */
    virtual GDIMesh* CreateMesh(const GDIMeshDesc& InDesc) = 0;

    /* A dynamic mesh (GDIMesh::UpdateDynamic) with room for InMaxVertices and
     * InMaxIndices per BeginRendering block. Null if unsupported -- the default. */
    virtual GDIMesh* CreateDynamicMesh(uint32_t /*InMaxVertices*/, uint32_t /*InMaxIndices*/) { return nullptr; }

    virtual GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) = 0;

    virtual GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) = 0;

    virtual GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) = 0;

    /* A texture with its mip chain: InLevels[0] full size, each next half
     * the last (at least 1), InCount in all; a Linear filter samples them
     * trilinear. This default makes level 0 alone, for a backend without
     * mips (Metal and Null so far): it samples the full size. */
    virtual GDITextureResource* CreateTextureResourceMips(const EngineCore::Image* InLevels, uint32_t InCount,
                                                          EGDITextureBorder InBorder, EGDITextureFilter InFilter)
    {
        return InLevels && InCount > 0 ? CreateTextureResource(InLevels[0], InBorder, InFilter) : nullptr;
    }

    /*
     * Compute and GPU buffers (GDIBuffer.h). A backend without them keeps
     * these defaults: no compute, no buffers, every dispatch refused.
     */
    virtual bool       SupportsCompute() const { return false; }
    /* InBytes, InUsage's EGDIBufferUsage bits; InInitialData (InBytes of it)
     * or zeros. Null where unsupported. */
    virtual GDIBuffer* CreateBuffer(uint32_t /*InBytes*/, uint8_t /*InUsage*/, const void* /*InInitialData*/) { return nullptr; }
    /* InShader (ST_Compute) over InX x InY x InZ workgroups, recorded in
     * frame order: what earlier draws and dispatches wrote it sees, and what
     * it writes the next ones see. Validated by GDIDispatchCompute. */
    virtual bool       DispatchCompute(GDIShader* /*InShader*/, const void* /*InParams*/, uint32_t /*InParamBytes*/,
                                       GDIBuffer* const* /*InBuffers*/, uint32_t /*InBufferCount*/,
                                       uint32_t /*InX*/, uint32_t /*InY*/, uint32_t /*InZ*/) { return false; }
    /* InBuffer's bytes as the GPU left them, waiting for it: for tests and tools. */
    virtual bool       ReadBuffer(GDIBuffer* /*InBuffer*/, std::vector<uint8_t>& /*OutBytes*/) { return false; }

    /* GDISetInstanceBuffer's buffer and count, which indexed draws read. */
    GDIBuffer* GetInstanceBuffer() const { return InstanceBuffer; }
    uint32_t   GetInstanceCount()  const { return InstanceCount; }

    /* Whether an instanced indexed draw can take its counts from a buffer
     * (GDISetIndirectArgs). No by default. */
    virtual bool SupportsIndirectDraw() const { return false; }
    /* GDISetIndirectArgs's buffer and byte offset; null: the draws' own counts. */
    GDIBuffer* GetIndirectArgs()       const { return IndirectArgs; }
    uint32_t   GetIndirectArgsOffset() const { return IndirectArgsOffset; }

    /* Where the backend keeps texture memory, for stats (GDIGetTextureMemoryStats);
     * false from a backend that does not say. */
    virtual bool GetTextureMemoryStats(GDITextureMemoryStats& /*OutStats*/) const { return false; }

    /* Whether InFormat can be sampled (CreateCompressedTextureResource).
     * This default, for a backend without block formats (Metal and Null so
     * far), says no: the caller decodes to RGBA8 instead. */
    virtual bool SupportsCompressedFormat(EGDICompressedFormat /*InFormat*/) const { return false; }

    /* A block-compressed texture with its mip chain: InLevels[0] full size,
     * each next half the last (at least 1), InCount in all, sampled like
     * CreateTextureResourceMips. Null where SupportsCompressedFormat says no. */
    virtual GDITextureResource* CreateCompressedTextureResource(EGDICompressedFormat /*InFormat*/,
                                                                const GDICompressedLevel* /*InLevels*/, uint32_t /*InCount*/,
                                                                EGDITextureBorder /*InBorder*/, EGDITextureFilter /*InFilter*/)
    {
        return nullptr;
    }

    virtual bool CheckError(const std::string& FailStr) = 0;

    virtual ERenderPlatform GetRenderPlatform() const = 0;

    virtual EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) = 0;

    bool InRenderThread()
    {
        return ThreadID == EngineCore::appThreadID();
    }

    /* Allocate() finished. A backend records its thread before it can fail,
     * so InRenderThread() alone does not mean it has a device. */
    bool IsInitialized() const { return bInitialized; }
protected:
    inline void SetThreadID(const EngineCore::ThreadID InThreadID)
    {
        ThreadID = InThreadID;
    }

    EngineCore::ThreadID  ThreadID;

    EngineCore::WindowHandle Window;

    GDIContext*     Context;

    std::unordered_map<std::string, GDIShaderData*>  RegisteredShaderMap;
    std::unordered_map<std::string, GDIShader*>      LoadedShaderMap;
    std::unordered_map<std::string, GDITexture2D*>   LoadedTextures;

    GDIShader* BoundVertexShader;
    GDIShader* BoundPixelShader;
    bool bInitialized = false;

    /* SetUniformBlock's bytes for bindings 2, 3, 20 and 21 (the skin blocks) and 22 (the light block), and SetMaterialTexture's. */
    /* GDISetViewport's rectangle, which GDISetScissorRect clips to. */
    uint                  ViewportX = 0, ViewportY = 0, ViewportWidth = 0, ViewportHeight = 0;

    /* Bumped by each GDIBeginRendering: a dynamic mesh's appends restart
     * when it changes. */
    uint64_t              RenderingSerial = 0;

    /* Frames the backend lets the GPU run behind the CPU, and which of them
     * the open block records (Vulkan's and D3D12's command slots; 1 and 0 elsewhere): a
     * dynamic mesh keeps a region of its buffers per frame in flight. Set by
     * the backend at Allocate and BeginRendering. */
    uint32_t              FramesInFlight = 1;
    uint32_t              FrameSlot      = 0;
    friend uint32_t GDIGetFramesInFlight();

    friend void GDISetViewport(uint, uint, uint, uint);
    friend void GDIBeginRendering();
    friend void GDISetScissorRect(uint, uint, uint, uint);
    friend void GDIClearScissorRect();
    friend bool GDIUpdateDynamicMesh(GDIMesh*, const GDIMeshDesc&);
    friend uint64_t GDIGetRenderingSerial();
    friend void GDISetInstanceBuffer(GDIBuffer*, uint32_t);
    friend void GDISetIndirectArgs(GDIBuffer*, uint32_t);

    /* GDISetInstanceBuffer's: null and 0 draw one instance, as before. */
    GDIBuffer*            InstanceBuffer = nullptr;
    uint32_t              InstanceCount  = 0;

    /* GDISetIndirectArgs's: null, the draws use their own counts. */
    GDIBuffer*            IndirectArgs       = nullptr;
    uint32_t              IndirectArgsOffset = 0;

    /* [0] material, [1] scene, [2] skin, [3] previous skin (empty: no skinned
     * draw since the last clear), [4] light (empty: no light list set). */
    std::vector<uint8_t>  MaterialBlocks[GDI_UNIFORM_BLOCK_COUNT];
    /* Bumped by each SetUniformBlock, so a backend can reuse an upload of a
     * block that has not changed since (the scene block: once per frame). */
    uint64_t              MaterialBlockSerials[GDI_UNIFORM_BLOCK_COUNT] = {};
    GDITextureResource*   MaterialTextures[GDI_MATERIAL_TEXTURE_SLOTS] = {};
};

//Expose to global context

extern GDIDynamicDriver*    GDynamicGDI;
extern GDISync*             GSync;

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SwapBuffers, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, Flush, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, Clear, (uint InColorMask, EngineCore::Color InColor, float InDepth = 1.0f, uint InStencil = 0x00))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetViewport, (uint, uint, uint, uint));

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetDepthState, (IDepthStateDesc))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetBlendState, (IBlendStateDesc))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetStencilState, (IStencilStateDesc))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetColorMask, (EColorMask))

IMPLEMENT_GDI_FUNCTION_DECLARATION(ERenderPlatform, GetRenderPlatform, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetRenderTarget, (GDIRenderTarget*))
IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetRenderTargets, (GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, ReleaseRenderTarget, (GDIRenderTarget*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDIRenderTarget*, CreateRenderTarget, (std::string, uint, uint))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDIShaderResource* , CreateShaderResource, (EShaderType, const std::string& ))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, BeginRendering, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, EndRendering, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, DrawScreenQuadPrimitive, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDIMesh*, CreateQuadMesh, (const float* VertData, const float* TexCoordData, const int InNumVerts, const int InNumTexCoords, bool bIsInstanced = false))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDIMesh*, CreateQuadMesh, (const float& InWidth, const float& InHeight, bool bIsInstanced = false))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDIMesh*, CreateMesh, (const GDIMeshDesc& InDesc))

/* Copies InDesc into malloc'd RenderData. False (OutData untouched) if empty,
 * out of range, or not whole triangles. */
bool GDIMakeIndexedRenderData(const GDIMeshDesc& InDesc, GDIMesh::ORenderData& OutData);

/* Whether InDesc is drawable as CreateMesh takes it: positions, UVs and
 * indices (normals too unless bInNormalsOptional), whole triangles, in range. */
bool GDIValidateMeshDesc(const GDIMeshDesc& InDesc, bool bInNormalsOptional);

/* GDIDynamicDriver::CreateDynamicMesh; null without a driver or support. */
GDIMesh* GDICreateDynamicMesh(uint32_t InMaxVertices, uint32_t InMaxIndices);

/* InMesh->UpdateDynamic(InDesc) in the current BeginRendering block. */
bool GDIUpdateDynamicMesh(GDIMesh* InMesh, const GDIMeshDesc& InDesc);

/* How many BeginRendering blocks have begun: a dynamic mesh's appends are
 * per block. */
uint64_t GDIGetRenderingSerial();

/* How many BeginRendering blocks the GPU may still be running when the next
 * begins (1 without a driver): what the CPU writes for a block -- a query
 * set's answers, a dynamic mesh's region -- it may reuse only that many
 * blocks later. Vulkan's and D3D12's are [Rendering] FramesInFlight. */
uint32_t GDIGetFramesInFlight();

/* [Rendering] FramesInFlight, clamped to 1..GDI_FRAMES_IN_FLIGHT_MAX: what a
 * backend that can run ahead reads when it makes its device. */
uint32_t GDIGetConfiguredFramesInFlight();

/* [Rendering] SwapchainExtraImages, clamped to 0..2: images a swapchain asks
 * for beyond the surface's minimum. */
uint32_t GDIGetConfiguredSwapchainExtraImages();

/*
 * Scissor, in pixels of the current viewport from its TOP-LEFT (the way a
 * UI lays out), which are the target's own rows: draws after this touch only
 * that rectangle, clipped to the viewport. GDISetViewport and
 * GDIClearScissorRect reset it to the whole viewport; Clear ignores it.
 */
void GDISetScissorRect(uint InX, uint InY, uint InWidth, uint InHeight);
void GDIClearScissorRect();

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, DrawObject, (class GDIViewObject*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, DrawObject, (class GDIViewObject*, class GDIView*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetBoundShader, (GDIShader*, GDIShader*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetUniformBlock, (uint InBinding, const void* InData, uint InSize))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetMaterialTexture, (uint InSlot, GDITextureResource* InTexture))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDITextureResource*, CreateTextureResource, (const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDITextureResource*, CreateTextureResource, (GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDITextureResource*, CreateTextureResource, (GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest))

IMPLEMENT_GDI_FUNCTION_DECLARATION(bool, InRenderThread, ())

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, RegisterTexture, (const std::string&, GDITexture2D*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(GDITexture2D*, GetTexture, (const std::string& TextureKey))

IMPLEMENT_GDI_FUNCTION_DECLARATION(bool, CheckError, (const std::string& InFailStr))

IMPLEMENT_GDI_FUNCTION_DECLARATION(EngineCore::Color, GetColorFromSamplePoint, (const EngineCore::Point& InPoint, const EngineCore::Size& InSize))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, BeginDrawEvent, (const std::string& InEventName))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, EndDrawEvent, ())

/* GDIDynamicDriver::HasDrawEvents: false with no driver, or with [Shaders]
 * ShaderDebug off. */
bool GDIHasDrawEvents();

/*
 * [Shaders] ShaderDebug (Edit > Editor Settings > Shaders; default on): draw
 * events (GDIBeginDrawEvent) and object names (GDISetDebugName, shader
 * modules, render targets) reach capture tools. Off, regions are dropped at
 * once; names are given as objects are made, so ones made while it was on
 * keep theirs.
 */
bool GDIShaderDebugEnabled();

/*
 * [Shaders] ShaderPDB with [Shaders] PDBDir (the editor sets it to
 * Engine/Shaders/PDBs): the debug-info file the shader cook wrote for a
 * cooked shader -- "Shaders/Cooked/Vulkan/Basic.vsh.spv" is
 * "<PDBDir>/Vulkan/Basic.vsh.spv", SPIR-V with its source; a D3D12 .cso's is
 * "<PDBDir>/D3D12/<name>.pdb". Empty when the setting is off or the file is
 * not there. Vulkan hands it to RenderDoc on the shader module (VKShader).
 */
std::string GDIGetShaderPDBPath(const std::string& InCookedPath);

/* GDIDynamicDriver::CreateRenderTargetFormat / SupportsRenderTargetFormat. */
/* Anisotropic filtering for textures with a mip chain and Linear filtering:
 * InMax 1 (off) to 16, clamped to what the device supports. Read when a
 * sampler is made, so set it before textures are created -- RenderCore does,
 * from [Rendering] Anisotropy, before the render thread starts the device.
 * Render targets (one level) are never filtered anisotropically. */
void  GDISetMaxAnisotropy(float InMax);
float GDIGetMaxAnisotropy();

/*
 * VSync: whether a present waits for the display's vertical blank.
 *
 * On, every window shows whole frames at its refresh rate, and the present
 * itself holds the UI thread -- and so the game loop -- to that rate: Vulkan
 * FIFO, D3D12 sync interval 1, Metal's displaySyncEnabled, a GL swap interval
 * of 1. Off (the default), a present never waits for the display, so the loop
 * runs as fast as [Display] FrameRateLimit lets it (0: unlimited): Vulkan
 * MAILBOX where the surface has it, else IMMEDIATE; D3D12 sync interval 0 with
 * frames DXGI is not ready for dropped; Metal and GL with sync off. MAILBOX and
 * DXGI's flip model show the newest whole frame at each refresh and do not
 * tear; IMMEDIATE and an unsynced GL or Metal window may.
 *
 * Any thread, any time, before or after the device exists: each backend reads
 * it at a window's next present and rebuilds what it must (a Vulkan swapchain).
 * RenderCore sets it from [Display] VSync.
 */
void GDISetVSync(bool bInEnabled);
bool GDIGetVSync();

GDIRenderTarget* GDICreateRenderTargetFormat(std::string InName, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat);
bool             GDISupportsRenderTargetFormat(EGDIRenderTargetFormat InFormat);

/* GDIDynamicDriver::CreateTextureResourceMips; null without a driver. */
GDITextureResource* GDICreateTextureResourceMips(const EngineCore::Image* InLevels, uint32_t InCount,
                                                 EGDITextureBorder InBorder, EGDITextureFilter InFilter);

/* Texture memory as the backend holds it (Vulkan: VKMemoryHeap.h). Bytes
 * the images use, the blocks they are carved from, images on their own
 * allocation, and the upload ring. False without a driver or from a backend
 * that does not track it. */
struct GDITextureMemoryStats
{
    uint64_t BlockBytes     = 0;
    uint64_t BlockUsedBytes = 0;
    uint64_t DedicatedBytes = 0;
    uint64_t StagingBytes   = 0;   // the ring's size, once made
    uint64_t StagingInUse   = 0;
    uint32_t Blocks         = 0;
    uint32_t BlockImages    = 0;
    uint32_t DedicatedImages = 0;
};
bool GDIGetTextureMemoryStats(GDITextureMemoryStats& OutStats);

/* GDIDynamicDriver::SupportsCompressedFormat / CreateCompressedTextureResource;
 * false and null without a driver. */
bool                GDISupportsCompressedFormat(EGDICompressedFormat InFormat);
GDITextureResource* GDICreateCompressedTextureResource(EGDICompressedFormat InFormat, const GDICompressedLevel* InLevels,
                                                       uint32_t InCount, EGDITextureBorder InBorder, EGDITextureFilter InFilter);

/*
 * Compute shaders and GPU buffers, where the backend has them (Vulkan, D3D12
 * and Null so far; GDIDynamicDriver's defaults elsewhere): false and null
 * without a driver or support.
 *
 * GDIDispatchCompute: InShader is an ST_Compute shader; InParams (at most
 * GDI_COMPUTE_PARAMS_MAX_BYTES) its block at binding 0, InBuffers (at most
 * GDI_COMPUTE_MAX_STORAGE_BUFFERS, each GDI_BU_Storage) at bindings 1.. in
 * order. Between GDIBeginRendering and GDIEndRendering it is recorded in
 * order with the draws (closing the open render pass, which it reopens);
 * outside, it runs at once and is waited for. False, with nothing done, if
 * anything is missing or invalid.
 */
bool       GDISupportsCompute();
GDIBuffer* GDICreateBuffer(uint32_t InBytes, uint8_t InUsage, const void* InInitialData = nullptr);
bool       GDIDispatchCompute(GDIShader* InShader, const void* InParams, uint32_t InParamBytes,
                              GDIBuffer* const* InBuffers, uint32_t InBufferCount,
                              uint32_t InX, uint32_t InY = 1, uint32_t InZ = 1);
bool       GDIReadBuffer(GDIBuffer* InBuffer, std::vector<uint8_t>& OutBytes);

/*
 * Instancing: the indexed draws after this (GDIDrawObject of a GDICreateMesh
 * mesh) each draw InCount instances, reading InBuffer (GDI_BU_Instance) as
 * GDIBuffer.h lays an instance out -- until it is called again; null or 0
 * goes back to one instance. Only where GDISupportsCompute(); elsewhere it
 * is ignored and the draws draw once.
 */
void       GDISetInstanceBuffer(GDIBuffer* InBuffer, uint32_t InCount);

/*
 * Indirect instancing: while set, an instanced indexed draw (one with
 * GDISetInstanceBuffer) takes its index count, instance count, first index,
 * vertex offset and first instance from InArgs (GDI_BU_Indirect) at
 * InOffsetBytes, laid out as GDI_INDIRECT_ARGS_STRIDE says -- the GPU's own
 * count, written by a compute shader, with no readback. Its mesh's draw
 * range and the instance buffer's count are then ignored. Null goes back to
 * them. Only where GDISupportsIndirectDraw(); elsewhere it is ignored.
 */
bool       GDISupportsIndirectDraw();
void       GDISetIndirectArgs(GDIBuffer* InArgs, uint32_t InOffsetBytes = 0);

/* GPU timestamps: GDIDynamicDriver::WriteGPUTimestamp's contract. */
bool     GDISupportsGPUTimestamps();
void     GDIWriteGPUTimestamp(uint32_t InIndex);
uint64_t GDIGetPreviousGPUTimestamps(double* OutMilliseconds);   // GDI_GPU_TIMESTAMP_MAX of them

/*
 * Occlusion queries (GDIQuery.h), where the backend has them (Vulkan, D3D12
 * and Null so far): false and null without a driver or support. Out of range
 * indices, and a round past the set's capacity, are ignored.
 *
 * GDISetOcclusionPredicate: the draws after it are skipped if InSet's query
 * InIndex (resolved this round) found nothing, until it is called with null
 * or -1. Where !GDISupportsOcclusionPredication() it is ignored: they draw.
 *
 * GDIReadOcclusionResults: OutVisible[i] for each query i of InSet's last
 * round, nonzero if it passed, once that round's block has finished on the
 * GPU -- by the next GDIBeginRendering. False, OutVisible untouched, before
 * then or with no round; it never waits. Each round is read at most once.
 */
bool                 GDISupportsOcclusionQueries();
bool                 GDISupportsOcclusionPredication();

/**
 * Parallel draw recording (RenderCore's MULTITHREADED_RENDERING.md, "The
 * gate"): what the backend and its device offer, decided at device creation.
 * GDISupportsParallelRecording: bSupported. With no driver, nothing.
 */
GDIParallelRecordingSupport GDIGetParallelRecordingSupport();
bool                 GDISupportsParallelRecording();

/**
 * A parallel pass (render thread), inside the bound render targets: false
 * (nothing begun) where the backend cannot -- issue serially. Otherwise
 * OutContexts[0..InChunks) are recording; each thread recording a chunk makes
 * its context current (GDISetCurrentRecording), issues its draws through the
 * usual GDI* setters and GDIDrawObject, and clears it. GDIEndParallelPass,
 * once every chunk is recorded, executes them in chunk order where the pass
 * began; the render thread's own state is as before the pass, but what is
 * bound in the command buffer is not (backends forget it). No clears,
 * render-target changes, queries or dispatches between begin and end.
 * InChunks at most GDI_PARALLEL_CHUNKS_MAX.
 */
bool                 GDIBeginParallelPass(uint32_t InChunks, GDIRecordingContext** OutContexts);
void                 GDIEndParallelPass();
/* The calling thread's recording context, null for none (the render thread's
 * own state). Thread-local. */
void                 GDISetCurrentRecording(GDIRecordingContext* InContext);
GDIRecordingContext* GDIGetCurrentRecording();
/* The index range InMesh's next draw uses: in a recording context, the
 * draw's own (the mesh may be shared); else GDIMesh::SetDrawRange. */
void                 GDISetDrawRange(GDIMesh* InMesh, uint32_t InFirst, uint32_t InCount);
GDIOcclusionQueries* GDICreateOcclusionQueries(uint32_t InCapacity);
void                 GDIBeginOcclusionRound(GDIOcclusionQueries* InSet, uint32_t InCount);
void                 GDIBeginOcclusionQuery(GDIOcclusionQueries* InSet, uint32_t InIndex);
void                 GDIEndOcclusionQuery(GDIOcclusionQueries* InSet, uint32_t InIndex);
void                 GDIResolveOcclusionQueries(GDIOcclusionQueries* InSet);
void                 GDISetOcclusionPredicate(GDIOcclusionQueries* InSet, int32_t InIndex);
bool                 GDIReadOcclusionResults(GDIOcclusionQueries* InSet, std::vector<uint8_t>& OutVisible);

IMPLEMENT_GDI_FUNCTION_BODY(
        template<typename ShaderRef> void,
        RegisterShader,
        (std::string SrcPath, EShaderType InType),
        ,
        <ShaderRef>(SrcPath, InType)
        )

IMPLEMENT_GDI_FUNCTION_BODY_RET(
        template<typename ShaderRef> ShaderRef*,
        GetShader,
        (),
        return,
        <ShaderRef>()
        )

/*
 * A name for a GPU object in capture tools -- RenderDoc's Resource Inspector,
 * PIX, Xcode -- and validation messages: an asset's path, so a draw's mesh,
 * texture or buffers read as what they are. Nothing where the backend cannot
 * name objects (Vulkan names them through VK_EXT_debug_utils when the
 * instance has it; RenderDoc's layer always provides it. D3D12 through
 * ID3D12Object::SetName). Null objects are ignored. Render thread.
 */
void GDISetDebugName(GDIMesh* InMesh, const std::string& InName);
void GDISetDebugName(GDIBuffer* InBuffer, const std::string& InName);
void GDISetDebugName(GDITextureResource* InTexture, const std::string& InName);

extern std::atomic<int> GDebugGroupID;   // bumped by every thread's draw events

/* A named region of GPU work for capture tools (RenderDoc, PIX, Xcode): begun
 * here, ended when it goes out of scope; regions nest. Render thread only. */
class GDIDrawEvent
{
public:
    explicit GDIDrawEvent(const std::string& InString)
    {
        GDebugGroupID++;
        GDIBeginDrawEvent(InString);
    }
    ~GDIDrawEvent()
    {
        GDebugGroupID--;
        GDIEndDrawEvent();
    }
    GDIDrawEvent(const GDIDrawEvent&)            = delete;
    GDIDrawEvent& operator=(const GDIDrawEvent&) = delete;
};

/* SCOPED_DRAW_EVENT("BasePass"): a GDIDrawEvent to the end of the enclosing
 * scope. Compiled out of shipping builds. */
#define GDI_DRAW_EVENT_CONCAT_INNER(A, B) A##B
#define GDI_DRAW_EVENT_CONCAT(A, B)       GDI_DRAW_EVENT_CONCAT_INNER(A, B)
#if defined(NIMBLE_SHIPPING) && NIMBLE_SHIPPING
    #define SCOPED_DRAW_EVENT(Name) do {} while(0)
#else
    #define SCOPED_DRAW_EVENT(Name) const GDIDrawEvent GDI_DRAW_EVENT_CONCAT(ScopedDrawEvent_, __LINE__)(Name)
#endif

#ifndef DECLARE_SCOPED_DRAWEVENT
#define DECLARE_SCOPED_DRAWEVENT(Name, StringLiteral) \
class Name##DrawEvent : public GDIDrawEvent           \
{                                                     \
    public:                                           \
        explicit Name##DrawEvent(const std::string& InStr) : GDIDrawEvent(InStr) {}\
} Name##_DrawEvent(StringLiteral);
#endif

#endif //CORECLIENT_GDI_H

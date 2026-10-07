#ifndef CORECLIENT_GDI_H
#define CORECLIENT_GDI_H


#include "Public/Core/CoreQueue.h"
#include "Public/Core/CorePlatform.h"

#include "GDIDefinitions.h"
#include "GDIBuffer.h"
#include "GDIShader.h"
#include "GDIMesh.h"
#include "GDISync.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
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
     * the GPU reaches that point, for i below GDI_GPU_TIMESTAMP_MAX. The block
     * is complete by the next BeginRendering (one frame in flight), so from
     * then until the next one GetPreviousGPUTimestamps gives the previous
     * block's: OutMilliseconds[i] for each bit i of the returned mask, on one
     * GPU clock (only differences mean anything). A backend that cannot
     * time -- the default -- writes nothing and returns 0.
     */
    virtual bool     SupportsGPUTimestamps() const { return false; }
    virtual void     WriteGPUTimestamp(uint32_t /*InIndex*/) {}
    virtual uint64_t GetPreviousGPUTimestamps(double* /*OutMilliseconds*/) { return 0; }

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

    /* SetUniformBlock's bytes for bindings 2, 3 and 12 (GDI_SKIN_BLOCK_BINDING), and SetMaterialTexture's. */
    /* GDISetViewport's rectangle, which GDISetScissorRect clips to. */
    uint                  ViewportX = 0, ViewportY = 0, ViewportWidth = 0, ViewportHeight = 0;

    /* Bumped by each GDIBeginRendering: a dynamic mesh's appends restart
     * when it changes. */
    uint64_t              RenderingSerial = 0;

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

    /* [0] material, [1] scene, [2] skin (empty: no skinned draw since the last clear). */
    std::vector<uint8_t>  MaterialBlocks[3];
    /* Bumped by each SetUniformBlock, so a backend can reuse an upload of a
     * block that has not changed since (the scene block: once per frame). */
    uint64_t              MaterialBlockSerials[3] = {};
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
GDIRenderTarget* GDICreateRenderTargetFormat(std::string InName, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat);
bool             GDISupportsRenderTargetFormat(EGDIRenderTargetFormat InFormat);

/* GDIDynamicDriver::CreateTextureResourceMips; null without a driver. */
GDITextureResource* GDICreateTextureResourceMips(const EngineCore::Image* InLevels, uint32_t InCount,
                                                 EGDITextureBorder InBorder, EGDITextureFilter InFilter);

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

extern int GDebugGroupID;

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

#ifndef CORECLIENT_GDI_H
#define CORECLIENT_GDI_H


#include "Public/Core/CoreQueue.h"
#include "Public/Core/CorePlatform.h"

#include "GDIDefinitions.h"
#include "GDIShader.h"
#include "GDIMesh.h"
#include "GDISync.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>
#include <unordered_map>

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
            ReturnStatement GDynamicGDI->##Name ArgsLit;                                \
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
            ReturnStatement GDynamicGDI->##Name ArgsLit;                                \
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

    virtual void SetViewport(uint InX, uint InY, uint InWidth, uint InHeight) = 0;

    virtual void SetDepthState(IDepthStateDesc) = 0;

    virtual void SetBlendState(IBlendStateDesc) = 0;

    virtual void SetStencilState(IStencilStateDesc) = 0;

    virtual void SetColorMask(EColorMask) = 0;

    virtual void BeginRendering() = 0;

    virtual void EndRendering() = 0;

    virtual GDIRenderTarget* CreateRenderTarget(std::string Name, uint InWidth, uint InHeight) = 0;

    virtual void SetRenderTarget( GDIRenderTarget* InRenderTarget ) = 0;

    virtual void ReleaseRenderTarget( GDIRenderTarget* InRenderTarget ) = 0;

    virtual GDIShaderResource* CreateShaderResource(EShaderType InType, const std::string& SourceCode) = 0;

    virtual void DrawScreenQuadPrimitive() = 0;

    virtual void DrawObject(class GDIViewObject*);

    virtual void DrawObject(class GDIViewObject*, class GDIView*);

    virtual void BeginDrawEvent(const std::string& EventName) = 0;

    virtual void EndDrawEvent() = 0;

    /**
     * Present the most recent rendered frame to the supplied window.
     * Called from the UI thread after RendererFinished(). OpenGL keeps its existing
     * QOpenGLContext-based present path and overrides this as a no-op; Vulkan
     * uses this to acquire a swapchain image, blit the offscreen RT, and present.
     */
    virtual void PresentToWindow(EngineCore::WindowHandle /*InWindow*/) {}

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

    virtual GDIMesh* CreateQuadMesh(const float* VertData, const float* TexData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) = 0;

    virtual GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) = 0;

    /* An indexed mesh with normals (GDIMeshDesc), drawn by range
     * (SetDrawRange). Null if unsupported or InDesc is invalid. */
    virtual GDIMesh* CreateMesh(const GDIMeshDesc& InDesc) = 0;

    virtual GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) = 0;

    virtual GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) = 0;

    virtual GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) = 0;

    virtual bool CheckError(const std::string& FailStr) = 0;

    virtual ERenderPlatform GetRenderPlatform() const = 0;

    virtual EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point&, const EngineCore::Size& InSize) = 0;

    bool InRenderThread()
    {
        return ThreadID == EngineCore::appThreadID();
    }
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

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, DrawObject, (class GDIViewObject*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, DrawObject, (class GDIViewObject*, class GDIView*))

IMPLEMENT_GDI_FUNCTION_DECLARATION(void, SetBoundShader, (GDIShader*, GDIShader*))

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

extern int GDebugGroupID;
class GDIDrawEvent
{
public:
    GDIDrawEvent(std::string InString)
    {
        GDebugGroupID++;
        GDIBeginDrawEvent(InString);
    }
    ~GDIDrawEvent()
    {
        GDebugGroupID--;
        GDIEndDrawEvent();
    }
    static int DebugGroupID;
};

#ifndef DECLARE_SCOPED_DRAWEVENT
#define DECLARE_SCOPED_DRAWEVENT(Name, StringLiteral) \
class Name##DrawEvent : public GDIDrawEvent           \
{                                                     \
    public:                                           \
        explicit Name##DrawEvent(std::string InStr)   : GDIDrawEvent(std::move(InStr)) {}\
        ~Name##DrawEvent()                            \
        {                                             \
            GDIDrawEvent::~GDIDrawEvent();            \
        }                                             \
                                                        \
} Name##_DrawEvent(StringLiteral);
#endif

#endif //CORECLIENT_GDI_H

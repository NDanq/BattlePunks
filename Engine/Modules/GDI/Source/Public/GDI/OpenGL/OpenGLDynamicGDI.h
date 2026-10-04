#ifndef CORECLIENT_OPENGLDYNAMICGDI_H
#define CORECLIENT_OPENGLDYNAMICGDI_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#if _WINDOWS
#include <Windows.h>
#include <gl/GL.h>
// Vendored Khronos WGL extension header. Resolved via the wglext include
// directory GDI puts on its PUBLIC path -- this used to be a
// project-root-relative path, which only worked because
// CMAKE_INCLUDE_CURRENT_DIR happened to put CoreClient root on the path.
// It is a public header, so consumers have to resolve it too.
#include <wglext.h>
#pragma comment (lib, "opengl32.lib")
#endif


#include "Public/GDI/OpenGL/GDIGLContextInterface.h"
#include "Public/GDI/GDIPlatform.h"
#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <string>

/*
 * Holds the native GL contexts and a host-supplied drawable.
 *
 * Context CREATION is still raw WGL here (it always was -- InitContext below
 * is unchanged). What moved out is the DRAWABLE: the host installs an
 * IGDIGLContext that knows how to make the render-thread context current,
 * because under Qt that means a QOffscreenSurface and GDI must not know what
 * one is. See GDIGLContextInterface.h for why the host owns this.
 */
class OpenGLGDIContext : public GDIContext
{
public:
    OpenGLGDIContext()
    {
        ContextType = GDIContext::OpenGLContext;
    }

    ~OpenGLGDIContext() override
    {
#if _WINDOWS
        ::wglMakeCurrent(NULL, NULL);
        if(m_MainContext)     ::wglDeleteContext(m_MainContext);
        if(m_ThreadedContext) ::wglDeleteContext(m_ThreadedContext);
        m_MainContext = nullptr;
        m_ThreadedContext = nullptr;
#endif
    }

    /*
     * Creates the two native contexts: one for the host's thread and one,
     * sharing with it, for the render thread. Sharing is mandatory -- the
     * renderer hands the host a framebuffer texture handle to sample.
     *
     * Returns the native HGLRCs; the host wraps them in whatever its toolkit
     * needs (Odessa's bridge wraps them as QOpenGLContext).
     */
    void* InitMainContext(void* InNativeWindow, int InMajorVersion, int InMinorVersion)
    {
        HWND hwnd = static_cast<HWND>(InNativeWindow);
        HDC hdc = GetDC(hwnd);
        m_MainContext = InitContext(hwnd, hdc, InMajorVersion, InMinorVersion, nullptr);
        ReleaseDC(hwnd, hdc);
        return m_MainContext;
    }

    void* InitSharedContext(void* InNativeWindow, int InMajorVersion, int InMinorVersion)
    {
        HWND hwnd = static_cast<HWND>(InNativeWindow);
        HDC hdc = GetDC(hwnd);
        wglMakeCurrent(hdc, m_MainContext);
        m_ThreadedContext = InitContext(hwnd, hdc, InMajorVersion, InMinorVersion, m_MainContext);
        wglMakeCurrent(NULL, NULL);
        ReleaseDC(hwnd, hdc);
        return m_ThreadedContext;
    }

    /* The host's drawable. Not owned. */
    void SetGLContextInterface(IGDIGLContext* InInterface) { GLContextInterface = InInterface; }
    IGDIGLContext* GetGLContextInterface() const { return GLContextInterface; }

    void* GetMainNativeContext()     const { return m_MainContext; }
    void* GetThreadedNativeContext() const { return m_ThreadedContext; }

private:
    HGLRC InitContext( HWND hwnd, HDC hdc, int InMajorVersion, int InMinorVersion, HGLRC ShareContext )
    {
        PIXELFORMATDESCRIPTOR pfd;
        memset(&pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
        pfd.nSize = sizeof( PIXELFORMATDESCRIPTOR );
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL | PFD_DRAW_TO_BITMAP;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 32;
        pfd.iLayerType = PFD_MAIN_PLANE;

        int PixelFormat = ChoosePixelFormat(hdc, &pfd);
        GDI_CHECK(PixelFormat != 0, "No Pixel format found");
        SetPixelFormat(hdc, PixelFormat, &pfd);

        HGLRC temp = wglCreateContext(hdc);
        wglMakeCurrent(hdc, temp);

        PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

        int WGLAttribs[] =
        {
            WGL_CONTEXT_MAJOR_VERSION_ARB, InMajorVersion,
            WGL_CONTEXT_MINOR_VERSION_ARB, InMinorVersion,
            WGL_CONTEXT_FLAGS_ARB,0,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        HGLRC wglContext = wglCreateContextAttribsARB(hdc, ShareContext, WGLAttribs);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(temp);
        wglMakeCurrent(hdc, wglContext);

        return wglContext;
    }

    IGDIGLContext* GLContextInterface = nullptr;

#if _WINDOWS
    HGLRC   m_MainContext = nullptr;
    HGLRC   m_ThreadedContext = nullptr;
#endif
};

class OpenGLDynamicGDI : public GDIDynamicDriver,
        protected GDIGLFunctions
{
public:
    explicit OpenGLDynamicGDI( EngineCore::WindowHandle InWindow) : GDIDynamicDriver(InWindow),
        Context(nullptr)
    {

    }
    /* Assign Context */
    void AssignContext( GDIContext* InContext ) override
    {
        Context = ((OpenGLGDIContext*)InContext);
    }

    /* Allocate Render Device */
    void Allocate() override
    {
        if(!bInitialized && Context)
        {
            SetThreadID(EngineCore::appThreadID());
            if(Context->GetGLContextInterface())
            {
                MakeCurrentSafe();
                GDI_CHECK( GDIGLFunctions::initializeOpenGLFunctions(), "[GDIThread] Failed to initialize OpenGL Functions");
                GDI_LOG("[DynamicGDI] Initialized OpenGL Functions in Render Thread");

                bInitialized = true;
            }
            if(GSync)
            {
                GSync->Initialize();
            }
        }
    }

    /* Destroy Render device */
    void Destroy() override;

    /* Swap buffers and present back buffer to screen */
    void SwapBuffers() override;

    /* Flush all render commands */
    void Flush() override;

    void BeginRendering() override;

    void EndRendering() override;

    void Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth = 1.0f, uint InStencilMask = 0x00) override;

    void SetViewport(uint InX, uint InY, uint InWidth, uint InHeight) override;

    void SetDepthState(IDepthStateDesc) override;

    void SetBlendState(IBlendStateDesc) override;

    void SetStencilState(IStencilStateDesc) override;

    void SetColorMask(EColorMask) override;

    GDIRenderTarget* CreateRenderTarget(std::string Name, uint InWidth, uint InHeight) override;
    GDIRenderTarget* CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat) override;
    bool             SupportsRenderTargetFormat(EGDIRenderTargetFormat InFormat) const override;

    void SetRenderTarget(GDIRenderTarget* InRenderTarget) override;
    void SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom) override;

    void ReleaseRenderTarget(GDIRenderTarget* InRenderTarget) override;

    GDIShaderResource* CreateShaderResource(EShaderType InType, const std::string& SrcCode) override;

    void DrawScreenQuadPrimitive() override;

    void SetBoundShader(GDIShader*, GDIShader*) override;

    using GDIDynamicDriver::DrawObject;

    /* The base draw, after binding the material layout's UBOs and texture
     * units when the bound pair asks for it. */
    void DrawObject(class GDIViewObject* InObject, class GDIView* InView) override;

    GDIMesh* CreateQuadMesh(const float* VertData, const float* TexCoordData, const int InNumVerts, const int NumTexCoords, bool bIsInstanced = false) override;

    GDIMesh* CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced = false) override;

    GDIMesh* CreateMesh(const GDIMeshDesc& InDesc) override;

    GDITextureResource* CreateTextureResource(const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    GDITextureResource* CreateTextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest) override;

    GDITextureResource* CreateTextureResourceMips(const EngineCore::Image* InLevels, uint32_t InCount, EGDITextureBorder InBorder, EGDITextureFilter InFilter) override;

    bool CheckError(const std::string& InFailStr) override;

    ERenderPlatform GetRenderPlatform() const override { return ERP_OpenGL; }

    EngineCore::Color GetColorFromSamplePoint(const EngineCore::Point& InPoint, const EngineCore::Size& InSize) override;

    GLenum DecodeCTM(ECompareTestMode);

    GLenum DecodeBM(EBlendMode);

    GLenum DecodeStencilOp(EStencilOp);

    void BeginDrawEvent(const std::string& InStr) override;

    void EndDrawEvent() override;

    bool     SupportsGPUTimestamps() const override { return true; }
    void     WriteGPUTimestamp(uint32_t InIndex) override;
    uint64_t GetPreviousGPUTimestamps(double* OutMilliseconds) override;

private:
    inline void MakeCurrentSafe()
    {
        if(InRenderThread())
        {
            IGDIGLContext* GLInterface = Context ? Context->GetGLContextInterface() : nullptr;
            GDI_CHECK(GLInterface, "[GDIThread] No GL context interface installed by the host!");
            if(GLInterface)
            {
                const bool bSucceeded = GLInterface->MakeCurrent();
                GDI_CHECK( bSucceeded, "[GDIThread] Error: Failed to set context!" );
            }
            return;
        }
        GDI_CHECK(0, "[GDIThread]Attempted to set context while outside render thread!");
    }

    /* Uploads the material blocks (bindings 2, 3) and binds texture units 4-11. */
    void ApplyMaterialBindings();

    OpenGLGDIContext*   Context;
    uint                BoundShaderProgram;
    uint                MaterialUBOs[2] = {};
    uint                ScratchFramebuffer = 0;   // SetRenderTargets' attachments
    class OpenGLMesh*   ScreenMesh = nullptr;
    GLsync              ThreadSync;

    /* GPU timestamps (GDIDynamicDriver::WriteGPUTimestamp), render thread only:
     * GL_TIMESTAMP queries made on first use, read at the next BeginRendering
     * -- waiting for them, as Vulkan and D3D12 wait the previous frame there. */
    GLuint   TimestampQueries[GDI_GPU_TIMESTAMP_MAX] = {};
    uint64_t TimestampsWritten  = 0;
    uint64_t PreviousTimestamps = 0;
    double   PreviousTimestampMs[GDI_GPU_TIMESTAMP_MAX] = {};
};

#endif //CORECLIENT_OPENGLDYNAMICGDI_H


#include "Public/GDI/OpenGL/GDIOpenGL.h"
#include "Public/GDI/OpenGL/OGLRenderTarget.h"
#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"
#include "Public/GDI/OpenGL/OGLTexture.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <vector>
#include <string>

GDIRenderTarget* OpenGLDynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    return new OpenGLRenderTarget(InWidth, InHeight);
}

GDIRenderTarget* OpenGLDynamicGDI::CreateRenderTargetFormat(std::string Name, uint InWidth, uint InHeight, EGDIRenderTargetFormat InFormat)
{
    return new OpenGLRenderTarget(InWidth, InHeight, InFormat);
}

bool OpenGLDynamicGDI::SupportsRenderTargetFormat(const EGDIRenderTargetFormat InFormat) const
{
    /* GL_RGBA16F is colour-renderable and blendable from OpenGL 3.0, which the backend requires. */
    return InFormat == GDI_RTF_RGBA8 || InFormat == GDI_RTF_RGBA16F;
}

void OpenGLDynamicGDI::SetRenderTarget(GDIRenderTarget *InRenderTarget)
{
    if(InRenderTarget)
    {
        InRenderTarget->Bind();
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/*
 * One target with its own depth is its own framebuffer. Anything else -- a
 * G-buffer, or a target drawn without its depth -- attaches the textures to
 * one shared framebuffer. A texture may be attached to both at once.
 */
void OpenGLDynamicGDI::SetRenderTargets(GDIRenderTarget* const* InColors, uint InCount, GDIRenderTarget* InDepthFrom)
{
    if(!InColors || InCount == 0 || !InColors[0]) return;
    if(InCount == 1 && InDepthFrom == InColors[0])
    {
        SetRenderTarget(InColors[0]);
        return;
    }
    if(InCount > GDI_MAX_COLOR_TARGETS) InCount = GDI_MAX_COLOR_TARGETS;

    if(ScratchFramebuffer == 0)
    {
        glGenFramebuffers(1, &ScratchFramebuffer);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, ScratchFramebuffer);

    GLenum Buffers[GDI_MAX_COLOR_TARGETS] = {};
    for(uint Index = 0; Index < GDI_MAX_COLOR_TARGETS; ++Index)
    {
        auto* RT = Index < InCount ? dynamic_cast<OpenGLRenderTarget*>(InColors[Index]) : nullptr;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + Index, GL_TEXTURE_2D,
                               RT ? RT->GetFramebufferTexture() : 0, 0);
        Buffers[Index] = GL_COLOR_ATTACHMENT0 + Index;
    }
    auto* DepthRT = dynamic_cast<OpenGLRenderTarget*>(InDepthFrom);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, DepthRT ? DepthRT->GetDepthTexture() : 0, 0);
    glDrawBuffers(GLsizei(InCount), Buffers);
}

void OpenGLDynamicGDI::ReleaseRenderTarget(GDIRenderTarget *InRenderTarget)
{
    GDI_CHECK(InRenderTarget, "[OpenGLDynamicGDI] RenderTarget Needs to be Valid!");
    InRenderTarget->Release();
}

OpenGLRenderTarget::OpenGLRenderTarget(const uint InWidth, const uint InHeight, const EGDIRenderTargetFormat InFormat)
: GDIRenderTarget(InWidth, InHeight)
, RTFormat(InFormat)
{
    TargetGDIType = RTGDI_OpenGL;
    initializeOpenGLFunctions();

    Allocate();
}

OpenGLRenderTarget::~OpenGLRenderTarget()
{
    Release();
}

void OpenGLRenderTarget::Bind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, FrameBuffer.FrameBufferID);
}

void OpenGLRenderTarget::Release()
{
    FrameBuffer.FrameTexture->Resource->Release();
    delete FrameBuffer.FrameTexture;
    FrameBuffer.FrameTexture = nullptr;
    glDeleteFramebuffers(1, &FrameBuffer.FrameBufferID);
    FrameBuffer.FrameBufferID = 0;
    if(DepthWrapper)
    {
        delete DepthWrapper->Resource;   // borrowed the texture: no Release()
        delete DepthWrapper;
        DepthWrapper = nullptr;
    }
    if(FrameBuffer.DepthTextureID)
    {
        glDeleteTextures(1, &FrameBuffer.DepthTextureID);
        FrameBuffer.DepthTextureID = 0;
    }
}

bool OpenGLRenderTarget::IsValid()
{
    return FrameBuffer.FrameBufferID != 0 &&
            FrameBuffer.FrameTexture != nullptr;
}

void OpenGLRenderTarget::Allocate()
{
    FrameBuffer.FrameTexture = new GDITexture2D(Width, Height, nullptr);

    if(auto* TextureResource = dynamic_cast<OpenGLTextureResource*>( FrameBuffer.FrameTexture->Resource ))
    {
        /* An HDR target: the colour texture re-specified as half float. glReadPixels (ToImage) asks for
         * GL_UNSIGNED_BYTE, which clamps it into 0-255 as the other backends' readbacks do. */
        if(RTFormat == GDI_RTF_RGBA16F)
        {
            glBindTexture(GL_TEXTURE_2D, TextureResource->TextureHandle);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, GLsizei(Width), GLsizei(Height), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glGenFramebuffers(1, &FrameBuffer.FrameBufferID);
        GDI_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "[QOpenGLFrameBuffer] Error, framebuffer generation incomplete!");
        glBindFramebuffer(GL_FRAMEBUFFER, FrameBuffer.FrameBufferID);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, TextureResource->TextureHandle, 0);

        // Every target has depth, so ResolveRenderTarget's depth blit matches.
        // A texture, not a renderbuffer, so a later pass can sample it.
        glGenTextures(1, &FrameBuffer.DepthTextureID);
        glBindTexture(GL_TEXTURE_2D, FrameBuffer.DepthTextureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, GLsizei(Width), GLsizei(Height), 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, FrameBuffer.DepthTextureID, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glFlush();
        return;
    }
    GDI_CHECK(0,"Error");
}

void OpenGLRenderTarget::Resize(const uint InWidth, const uint InHeight)
{
    GDI_CHECK(GDIInRenderThread(), "Must be called from render thread!!")

    GDIRenderTarget::Resize(InWidth, InHeight);
    Release();
    Allocate();
}

void OpenGLRenderTarget::ResolveRenderTarget(GDIRenderTarget *Dest)
{
    GDI_CHECK(GDIGLFunctions::hasOpenGLFramebufferBlit(), "Framebuffer blitting not supported");
    const int iWidth = int(Width);
    const int iHeight = int(Height);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, FrameBuffer.FrameBufferID);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ((OpenGLRenderTarget*)Dest)->GetFramebufferID());
    glBlitFramebuffer(
                      0, 0, iWidth, iHeight,
                      0, 0, iWidth, iHeight,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                      GL_NEAREST
                      );
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

uint OpenGLRenderTarget::GetFramebufferID() const
{
    return FrameBuffer.FrameBufferID;
}

uint OpenGLRenderTarget::GetFramebufferTexture() const
{
    if(FrameBuffer.FrameTexture)
    {
        if(auto* TextureResource = dynamic_cast<OpenGLTextureResource*>(FrameBuffer.FrameTexture->Resource))
        {
            return TextureResource->TextureHandle;
        }
    }
    return 0;
}

GDITexture2D* OpenGLRenderTarget::ToGDITexture2D()
{
    return FrameBuffer.FrameTexture;
}

GDITexture2D* OpenGLRenderTarget::ToDepthTexture2D()
{
    if(FrameBuffer.DepthTextureID == 0) return nullptr;
    if(!DepthWrapper)
    {
        auto* Resource          = new OpenGLTextureResource();
        Resource->TextureHandle = FrameBuffer.DepthTextureID;
        DepthWrapper            = new GDITexture2D();
        DepthWrapper->Width     = Width;
        DepthWrapper->Height    = Height;
        DepthWrapper->Resource  = Resource;
    }
    return DepthWrapper;
}

EngineCore::Image OpenGLRenderTarget::ToImage()
{
    const int iWidth  = int(Width);
    const int iHeight = int(Height);

    /*
     * FORMAT: glReadPixels is asked for GL_BGRA, so the bytes land B,G,R,A --
     * which is exactly EImageFormat::BGRA8. The old code wrapped this in a
     * QImage tagged Format_ARGB32, and that was CORRECT despite the name:
     * Format_ARGB32 describes the packed 32-bit integer, and on little-endian
     * that is BGRA in memory. The Qt bridge maps BGRA8 back to Format_ARGB32,
     * so callers receive byte-identical pixels to before.
     *
     * Do not "simplify" this to RGBA8 to match the Vulkan backend. The two
     * backends genuinely differ here, and unifying them swaps red and blue on
     * OpenGL only -- visible as a blue brush where a red one was asked for.
     */
    std::vector<unsigned char> PixelData(size_t(iWidth) * size_t(iHeight) * 4u);

    Bind();
    glReadPixels(0, 0, iWidth, iHeight, GL_BGRA, GL_UNSIGNED_BYTE, PixelData.data());

    // Tightly packed straight out of glReadPixels (4-byte pixels, default
    // GL_PACK_ALIGNMENT of 4), so a stride of 0 is accurate here.
    EngineCore::Image Image = EngineCore::Image::CopyFrom(
            PixelData.data(), iWidth, iHeight, EngineCore::EImageFormat::BGRA8, 0);

    // Row 0 is the top (glClipControl, OpenGLDynamicGDI::BeginRendering), as
    // EngineCore::Image's: the rows come back as they are.
    return Image;
}


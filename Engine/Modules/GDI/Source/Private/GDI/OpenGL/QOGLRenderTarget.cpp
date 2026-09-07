
#include "Public/GDI/OpenGL/GDIOpenGL.h"
#include "Public/GDI/OpenGL/QOGLRenderTarget.h"
#include "Public/GDI/OpenGL/QOpenGLDynamicGDI.h"
#include "Public/GDI/OpenGL/QOGLTexture.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <vector>
#include <string>

GDIRenderTarget* QOpenGLDynamicGDI::CreateRenderTarget(std::string Name, uint InWidth, uint InHeight)
{
    return new QOpenGLRenderTarget(InWidth, InHeight);
}

void QOpenGLDynamicGDI::SetRenderTarget(GDIRenderTarget *InRenderTarget)
{
    if(InRenderTarget)
    {
        InRenderTarget->Bind();
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void QOpenGLDynamicGDI::ReleaseRenderTarget(GDIRenderTarget *InRenderTarget)
{
    GDI_CHECK(InRenderTarget, "[QOpenGLDynamicGDI] RenderTarget Needs to be Valid!");
    InRenderTarget->Release();
}

QOpenGLRenderTarget::QOpenGLRenderTarget(const uint InWidth, const uint InHeight)
: GDIRenderTarget(InWidth, InHeight)
{
    TargetGDIType = RTGDI_OpenGL;
    initializeOpenGLFunctions();

    Allocate();
}

QOpenGLRenderTarget::~QOpenGLRenderTarget()
{
    Release();
}

void QOpenGLRenderTarget::Bind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, FrameBuffer.FrameBufferID);
}

void QOpenGLRenderTarget::Release()
{
    FrameBuffer.FrameTexture->Resource->Release();
    delete FrameBuffer.FrameTexture;
    FrameBuffer.FrameTexture = nullptr;
    glDeleteFramebuffers(1, &FrameBuffer.FrameBufferID);
    FrameBuffer.FrameBufferID = 0;
}

bool QOpenGLRenderTarget::IsValid()
{
    return FrameBuffer.FrameBufferID != 0 &&
            FrameBuffer.FrameTexture != nullptr;
}

void QOpenGLRenderTarget::Allocate()
{
    FrameBuffer.FrameTexture = new GDITexture2D(Width, Height, nullptr);

    if(auto* TextureResource = dynamic_cast<QOpenGLTextureResource*>( FrameBuffer.FrameTexture->Resource ))
    {
        glGenFramebuffers(1, &FrameBuffer.FrameBufferID);
        GDI_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "[QOpenGLFrameBuffer] Error, framebuffer generation incomplete!");
        glBindFramebuffer(GL_FRAMEBUFFER, FrameBuffer.FrameBufferID);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, TextureResource->TextureHandle, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glFlush();
        return;
    }
    GDI_CHECK(0,"Error");
}

void QOpenGLRenderTarget::Resize(const uint InWidth, const uint InHeight)
{
    GDI_CHECK(GDIInRenderThread(), "Must be called from render thread!!")

    GDIRenderTarget::Resize(InWidth, InHeight);
    Release();
    Allocate();
}

void QOpenGLRenderTarget::ResolveRenderTarget(GDIRenderTarget *Dest)
{
    GDI_CHECK(GDIGLFunctions::hasOpenGLFramebufferBlit(), "Framebuffer blitting not supported");
    const int iWidth = int(Width);
    const int iHeight = int(Height);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, FrameBuffer.FrameBufferID);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ((QOpenGLRenderTarget*)Dest)->GetFramebufferID());
    glBlitFramebuffer(
                      0, 0, iWidth, iHeight,
                      0, 0, iWidth, iHeight,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                      GL_NEAREST
                      );
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

uint QOpenGLRenderTarget::GetFramebufferID() const
{
    return FrameBuffer.FrameBufferID;
}

uint QOpenGLRenderTarget::GetFramebufferTexture() const
{
    if(FrameBuffer.FrameTexture)
    {
        if(auto* TextureResource = dynamic_cast<QOpenGLTextureResource*>(FrameBuffer.FrameTexture->Resource))
        {
            return TextureResource->TextureHandle;
        }
    }
    return 0;
}

GDITexture2D* QOpenGLRenderTarget::ToGDITexture2D()
{
    return FrameBuffer.FrameTexture;
}

EngineCore::Image QOpenGLRenderTarget::ToImage()
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

    // GL rows run bottom-up; this is the same single vertical flip the QImage
    // version applied. One flip, in the same place. Nothing else changes.
    Image.FlipVertical();
    return Image;
}


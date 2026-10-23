#ifndef CORECLIENT_OGLRENDERTARGET_H
#define CORECLIENT_OGLRENDERTARGET_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIRenderTarget.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class OpenGLRenderTarget : public GDIRenderTarget, protected GDIGLFunctions
{
public:
    /* InFormat: the colour texture's -- GL_RGBA16F, GL_RGB10_A2, GL_RGBA32F or GL_R32F; otherwise RGBA8. */
    OpenGLRenderTarget(const uint InWidth, const uint InHeight, EGDIRenderTargetFormat InFormat = GDI_RTF_RGBA8);
    ~OpenGLRenderTarget();

    void Bind() override;

    void Release() override;

     void Resize(const uint InWidth, const uint InHeight) override;

    bool IsValid() override;

    EngineCore::Image ToImage() override;

    GDITexture2D* ToGDITexture2D() override;
    GDITexture2D* ToDepthTexture2D() override;

    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    uint GetFramebufferID() const;

    uint GetFramebufferTexture() const;
    uint GetDepthTexture() const { return FrameBuffer.DepthTextureID; }
protected:
    void Allocate() override;
    struct OGLFrameBuffer{
        uint FrameBufferID = 0;
        GDITexture2D* FrameTexture;
        uint DepthTextureID = 0;  // a 24-bit depth texture: 3D draws' depth test, and sampleable
    } FrameBuffer;

    /* Borrows DepthTextureID (ToDepthTexture2D); never deletes it. */
    GDITexture2D* DepthWrapper = nullptr;

    EGDIRenderTargetFormat RTFormat = GDI_RTF_RGBA8;
};

#endif //CORECLIENT_OGLRENDERTARGET_H

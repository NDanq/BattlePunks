#ifndef CORECLIENT_QOGLRENDERTARGET_H
#define CORECLIENT_QOGLRENDERTARGET_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIRenderTarget.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class QOpenGLRenderTarget : public GDIRenderTarget, protected GDIGLFunctions
{
public:
    QOpenGLRenderTarget(const uint InWidth, const uint InHeight );
    ~QOpenGLRenderTarget();

    void Bind() override;

    void Release() override;

     void Resize(const uint InWidth, const uint InHeight) override;

    bool IsValid() override;

    EngineCore::Image ToImage() override;

    GDITexture2D* ToGDITexture2D() override;

    void ResolveRenderTarget(GDIRenderTarget* Dest) override;

    uint GetFramebufferID() const;

    uint GetFramebufferTexture() const;
protected:
    void Allocate() override;
    struct OGLFrameBuffer{
        uint FrameBufferID = 0;
        GDITexture2D* FrameTexture;
    } FrameBuffer;
};

#endif //CORECLIENT_QOGLRENDERTARGET_H

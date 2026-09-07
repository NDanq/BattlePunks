#ifndef CORECLIENT_NULLRENDERTARGET_H
#define CORECLIENT_NULLRENDERTARGET_H

#include "Public/GDI/GDIRenderTarget.h"

/*
 * GDIRenderTargetManager (GDIRenderTarget.h) dereferences whatever
 * GDICreateRenderTarget hands back with no null check -- Resize() and
 * Release() both call straight through the pointer. A Null render target must
 * therefore be a real, valid object, never nullptr, and IsValid() must say so
 * (the base default is false, which would read as "the renderer is broken").
 *
 * Follows QOpenGLRenderTarget's shape: TargetGDIType stays RTGDI_None (the
 * base default -- there is no RTGDI_Null and the plan does not add one), the
 * constructor calls Allocate(), the destructor calls Release().
 */
class NullRenderTarget : public GDIRenderTarget
{
public:
    NullRenderTarget(const uint InWidth, const uint InHeight) :
        GDIRenderTarget(InWidth, InHeight)
    {
        Allocate();
    }

    ~NullRenderTarget()
    {
        Release();
    }

    void Bind() override {}

    void Release() override
    {
        if(OwnedTexture)
        {
            OwnedTexture->Release();
            delete OwnedTexture;
            OwnedTexture = nullptr;
        }
    }

    bool IsValid() override { return true; }

    GDITexture2D* ToGDITexture2D() override { return OwnedTexture; }

    void ResolveRenderTarget(GDIRenderTarget* Dest) override {}

protected:
    void Allocate() override
    {
        // Matches QOpenGLRenderTarget::Allocate(): a real GDITexture2D backed
        // by a NullTextureResource (see NullDynamicGDI::CreateTextureResource),
        // so ToGDITexture2D() never hands back nullptr.
        OwnedTexture = new GDITexture2D(Width, Height, nullptr);
    }

private:
    GDITexture2D* OwnedTexture = nullptr;
};

#endif //CORECLIENT_NULLRENDERTARGET_H

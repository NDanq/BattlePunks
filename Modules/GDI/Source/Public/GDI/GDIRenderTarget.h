#ifndef CORECLIENT_GDIRENDERTARGET_H
#define CORECLIENT_GDIRENDERTARGET_H

#include "GDI.h"
#include "GDITexture.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

enum ERenderTargetGDIType : char
{
    RTGDI_None = 0,
    RTGDI_OpenGL,
    RTGDI_MAX
};

enum ERenderTargetPass : char
{
    Main = 0,
    Buffer1,
    PostBuffer1,
    /* Insert new RenderTarget Pass before this line */
    MAX_RenderTargetPass
};

/* An image on its way back from a render target (GDIRenderTarget::BeginReadback).
 * Render thread only. */
class GDIReadback
{
public:
    virtual ~GDIReadback() = default;

    /* True once Take will not wait on the GPU. */
    virtual bool IsReady() = 0;

    /* The pixels, once (empty: the backend reads nothing back). */
    virtual EngineCore::Image Take() = 0;
};

/* A readback done at once: what a backend without an asynchronous one gives. */
class GDIImmediateReadback : public GDIReadback
{
public:
    explicit GDIImmediateReadback(EngineCore::Image InImage) : Pixels(std::move(InImage)) {}

    bool              IsReady() override { return true; }
    EngineCore::Image Take() override    { return std::move(Pixels); }

private:
    EngineCore::Image Pixels;
};

class GDIRenderTarget
{
public:
    GDIRenderTarget() = default;

    GDIRenderTarget(const uint InWidth, const uint InHeight) :
    Width(InWidth),
    Height(InHeight)
    {
        TargetGDIType = RTGDI_None;
    }

    /* Virtual: render targets are deleted through this base, and a backend's
     * destructor must run -- it unregisters the target from its driver's live
     * list, which otherwise keeps a freed target the next bind walks over. */
    virtual ~GDIRenderTarget() = default;

    virtual void Resize(const uint InWidth, const uint InHeight)
    {
        Width = InWidth;
        Height = InHeight;
    }

    virtual bool IsValid() { return false; }

    virtual void Bind() = 0;

    virtual void Release() = 0;

    const uint GetWidth() const { return Width; }
    const uint GetHeight() const { return Height; }

    virtual EngineCore::Image ToImage() { return EngineCore::Image(); }

    /* ToImage without waiting for the GPU: the copy joins the commands being
     * recorded, and is ready a frame or so later -- a preview drawn every
     * frame does not stall each one. Backends without one read back at once.
     * The caller owns the result, and polls it on the render thread. */
    virtual GDIReadback* BeginReadback() { return new GDIImmediateReadback(ToImage()); }

    virtual GDITexture2D* ToGDITexture2D() = 0;

    /* The depth attachment, for sampling (point-filtered; its value is the
     * window depth, 0 near to 1 far). Null if the backend cannot, or the
     * target has no depth. Valid until the target is resized or released. */
    virtual GDITexture2D* ToDepthTexture2D() { return nullptr; }

    virtual void ResolveRenderTarget(GDIRenderTarget* Dest) = 0;

     virtual void DrawTexture(GDITexture2D* InTexture2D);

protected:
    virtual void Allocate() = 0;
    ERenderTargetGDIType TargetGDIType;
    uint Width;
    uint Height;
};

class GDIRenderTargetManager
{
public:
    GDIRenderTargetManager() :
    bHasRestorableData(false)
    {
        for(auto & RenderTarget : RenderTargets)
        {
            RenderTarget = nullptr;
        }
    }

    inline void Allocate(const uint InWidth, const uint InHeight)
    {
        for(auto& RenderTarget : RenderTargets)
        {
            RenderTarget = GDICreateRenderTarget("", InWidth, InHeight);
        }
    }

    inline void Resize(const uint InWidth, const uint InHeight)
    {
        bHasRestorableData = false;
        for(auto& RenderTarget : RenderTargets)
        {
            RenderTarget->Resize(InWidth, InHeight);
        }
    }

    inline void Release()
    {
        for(auto & RenderTarget : RenderTargets)
        {
            RenderTarget->Release();
        }
    }

    GDIRenderTarget* GetRenderTarget(ERenderTargetPass InPass)
    {
        GDI_CHECK((InPass >= 0 && InPass < ERenderTargetPass::MAX_RenderTargetPass), "Requested Render Target is not in range!")
        return RenderTargets[InPass];
    }

    void ResolveCanvasBuffer()
    {
        bHasRestorableData = true;
        GetMain()->ResolveRenderTarget(GetBuffer1());
    }

    void RestoreCanvasBuffer()
    {
        if(bHasRestorableData)
        {
            GetBuffer1()->ResolveRenderTarget(GetMain());
        }
    }

    GDIRenderTarget* GetMain(){ return RenderTargets[Main]; }
    GDIRenderTarget* GetBuffer1(){ return RenderTargets[Buffer1]; }

private:
    GDIRenderTarget* RenderTargets[MAX_RenderTargetPass];
    bool            bHasRestorableData;
};

#endif //CORECLIENT_GDIRENDERTARGET_H

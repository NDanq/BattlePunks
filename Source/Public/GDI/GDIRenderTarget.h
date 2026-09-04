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

    ~GDIRenderTarget()
    {

    }

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

    virtual GDITexture2D* ToGDITexture2D() = 0;

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

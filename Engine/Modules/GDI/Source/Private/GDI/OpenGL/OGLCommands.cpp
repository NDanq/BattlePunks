#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"
#include "Public/GDI/GDIPlatform.h"
#include "Public/GDI/GDIView.h"
#include "Public/GDI/GDIMesh.h"
#include "Public/GDI/OpenGL/OGLMesh.h"
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

#ifndef ENUM_NOT_FOUND
#define ENUM_NOT_FOUND (-1)
#endif

GLenum OpenGLDynamicGDI::DecodeCTM(ECompareTestMode InCompareMode)
{
    switch (InCompareMode)
    {
        case CTM_Never:
            return GL_NEVER;
        case CTM_ALWAYS:
            return GL_ALWAYS;
        case CTM_LESS:
            return GL_LESS;
        case CTM_LEQUAL:
            return GL_LEQUAL;
        case CTM_GREATER:
            return GL_GREATER;
        case CTM_GEQUAL:
            return GL_GEQUAL;
        case CTM_EQUAL:
            return GL_EQUAL;
        case CTM_NOTEQUAL:
            return GL_NOTEQUAL;
    }
    return ENUM_NOT_FOUND;
}

GLenum OpenGLDynamicGDI::DecodeBM( EBlendMode InBlendMode )
{
    switch( InBlendMode )
    {
        case BM_Zero:
            return GL_ZERO;
        case BM_One:
            return GL_ONE;
        case BM_Src_Color:
            return GL_SRC_COLOR;
        case BM_One_Minus_Src_Color:
            return GL_ONE_MINUS_SRC_COLOR;
        case BM_Dest_Color:
            return GL_DST_COLOR;
        case BM_One_Minus_Dest_Color:
            return GL_ONE_MINUS_DST_COLOR;
        case BM_Src_Alpha:
            return GL_SRC_ALPHA;
        case BM_One_Minus_Src_Alpha:
            return GL_ONE_MINUS_SRC_ALPHA;
        case BM_Dest_Alpha:
            return GL_DST_ALPHA;
        case BM_One_Minus_Dest_Alpha:
            return GL_ONE_MINUS_DST_ALPHA;
        case BM_Constant_Color:
            return GL_CONSTANT_COLOR;
        case BM_One_Minus_Constant_Color:
            return GL_ONE_MINUS_CONSTANT_COLOR;
        case BM_Constant_Alpha:
            return GL_CONSTANT_ALPHA;
        case BM_One_Minus_Constant_Alpha:
            return GL_ONE_MINUS_CONSTANT_ALPHA;
    }
    return ENUM_NOT_FOUND;
}

GLenum OpenGLDynamicGDI::DecodeStencilOp(EStencilOp InStencilOP)
{
    switch(InStencilOP)
    {
        case SO_Keep:
            return GL_KEEP;
        case SO_Zero:
            return GL_ZERO;
        case SO_Replace:
            return GL_REPLACE;
        case SO_Incr:
            return GL_INCR;
        case SO_Incr_Wrap:
            return GL_INCR_WRAP;
        case SO_Decr:
            return GL_DECR;
        case SO_Decr_Wrap:
            return GL_DECR;
        case SO_Invert:
            return GL_INVERT;
    }
    return ENUM_NOT_FOUND;
}

void OpenGLDynamicGDI::Destroy()
{
    /* BEFORE the guard below, and before Context is deleted: cached shaders
     * and textures hold GL objects owned by that context -- see
     * ~GDIDynamicDriver. */
    ReleaseCachedResources();

    if( bInitialized && Context )
    {
        bInitialized = false;
        if(MaterialUBOs[0]) { glDeleteBuffers(2, MaterialUBOs); MaterialUBOs[0] = MaterialUBOs[1] = 0; }
        if(ScratchFramebuffer) { glDeleteFramebuffers(1, &ScratchFramebuffer); ScratchFramebuffer = 0; }
        delete ScreenMesh;
        delete Context;
    }
}

void OpenGLDynamicGDI::Clear(uint InClearMode, EngineCore::Color InClearColor, float InDepth, uint InStencilMask)
{
    const bool bClearColor = (InClearMode & CM_Color);
    const bool bClearDepth = (InClearMode & CM_Depth);
    const bool bClearStencil = (InClearMode & CM_Stencil);
    GLenum ClearFlags = 0;

    if( bClearColor )
    {
        const float CR = InClearColor.Rf();
        const float CG = InClearColor.Gf();
        const float CB = InClearColor.Bf();
        const float CA = InClearColor.Af();
        glClearColor(CR, CG, CB, CA);
        ClearFlags |= GL_COLOR_BUFFER_BIT;
    }

    if( bClearDepth )
    {
        glClearDepth(InDepth);
        ClearFlags |= GL_DEPTH_BUFFER_BIT;
    }

    if( bClearStencil )
    {
        glClearStencil(InStencilMask);
        ClearFlags |= GL_STENCIL_BUFFER_BIT;
    }

    glClear( ClearFlags );
}

void OpenGLDynamicGDI::SwapBuffers()
{
    if(IGDIGLContext* GLInterface = Context ? Context->GetGLContextInterface() : nullptr)
    {
        Flush();
        // The host's offscreen drawable, not the visible window -- the host
        // presents the finished frame itself.
        GLInterface->SwapBuffers();
    }
}

void OpenGLDynamicGDI::BeginRendering()
{
    MakeCurrentSafe();
    if( ThreadSync != NULL )
    {
        glWaitSync(ThreadSync, 0, 1000000);
    }
}

void OpenGLDynamicGDI::EndRendering()
{
    Flush();
    ThreadSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if(IGDIGLContext* GLInterface = Context ? Context->GetGLContextInterface() : nullptr)
    {
        GLInterface->DoneCurrent();
    }
}

void OpenGLDynamicGDI::SetViewport( uint InX, uint InY, uint InWidth, uint InHeight )
{
    glViewport(InX, InY, InWidth, InHeight);
}

void OpenGLDynamicGDI::Flush()
{
    glFlush();
}

void OpenGLDynamicGDI::SetDepthState(IDepthStateDesc InDepthState)
{
    InDepthState.bDepthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    GLenum EnableDepthMask = InDepthState.bDepthMask ? GL_TRUE : GL_FALSE;
    glDepthFunc( DecodeCTM(InDepthState.DepthMode) );
    glDepthMask( EnableDepthMask );
}

void OpenGLDynamicGDI::SetStencilState( IStencilStateDesc InStencilState )
{
    InStencilState.bStencilTest ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    glStencilFunc( DecodeCTM(InStencilState.StencilMode),
                    InStencilState.Reference,
                    InStencilState.Mask );
    const GLenum  SFail = DecodeStencilOp(InStencilState.SrcFail);
    const GLenum  DFail = DecodeStencilOp(InStencilState.DestFail);
    const GLenum SDPass = DecodeStencilOp(InStencilState.SDPass);
    glStencilOp(SFail, DFail, SDPass);
}

static GLenum DecodeBO(EBlendOp InOp)
{
    switch(InOp)
    {
        case BO_Max: return GL_MAX;
        case BO_Min: return GL_MIN;
        case BO_Add:
        default:     return GL_FUNC_ADD;
    }
}

void OpenGLDynamicGDI::SetBlendState(IBlendStateDesc InBlendState)
{
    InBlendState.bBlend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    glBlendFunc(DecodeBM(InBlendState.Source), DecodeBM(InBlendState.Dest));
    // Color and alpha equations can differ (the brush stamp pass uses Max on alpha
    // to avoid intra-stroke opacity compounding). GL_MAX/GL_MIN ignore the factors.
    glBlendEquationSeparate(DecodeBO(InBlendState.Op), DecodeBO(InBlendState.AlphaOp));
}

void OpenGLDynamicGDI::SetColorMask(EColorMask InColorMask)
{
    bool bRed = ( InColorMask & EColorMask::EColM_R ) == 0;
    bool bGreen = (InColorMask & EColorMask::EColM_G) == 0;
    bool bBlue = (InColorMask & EColorMask::EColM_B) == 0;
    bool bAlpha = (InColorMask & EColorMask::EColM_A) == 0;

    if(InColorMask == EColM_None)
    {
        bRed = bGreen = bBlue = bAlpha = false;
    }

    if(InColorMask == EColM_RGB)
    {
        bRed = true;
        bGreen = true;
        bBlue = true;
        bAlpha = false;
    }
    else if(InColorMask == EColM_RGBA)
    {
        bRed = true;
        bGreen = true;
        bBlue = true;
        bAlpha = true;
    }

    glColorMask(bRed, bGreen, bBlue, bAlpha);
}

bool OpenGLDynamicGDI::CheckError(const std::string& InFailStr)
{
    GLenum ErrCode;
    if((ErrCode = glGetError()) != GL_NO_ERROR)
    {
        std::string ErrReason;
        switch(ErrCode)
        {
            case GL_INVALID_ENUM:                       ErrReason = "INVALID_ENUM"; break;
            case GL_INVALID_VALUE:                      ErrReason = "INVALID_VALUE"; break;
            case GL_INVALID_OPERATION:                  ErrReason = "INVALID_OPERATION"; break;
            case GL_STACK_OVERFLOW:                     ErrReason = "STACK_OVERFLOW"; break;
            case GL_STACK_UNDERFLOW:                    ErrReason = "STACK_UNDERFLOW"; break;
            case GL_OUT_OF_MEMORY:                      ErrReason = "OUT_OF_MEMORY"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:      ErrReason = "INVALID_FRAMEBUFFER_OPERATION";
        }
        GDI_LOG(InFailStr + " : " + ErrReason);
    }
    return ErrCode == GL_NO_ERROR;
}

EngineCore::Color OpenGLDynamicGDI::GetColorFromSamplePoint(const EngineCore::Point &InPoint, const EngineCore::Size& InSize)
{
    auto Data = new uchar[4];
    glReadPixels(InPoint.X, InSize.Height - InPoint.Y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, Data);

    // Alpha stays at the default 255, exactly as the QColor version did:
    // only RGB was ever assigned here.
    EngineCore::Color NewColor;
    NewColor.R = Data[0];
    NewColor.G = Data[1];
    NewColor.B = Data[2];
#if 0
    GDI_LOG(   "Red: " + ::GDILogDetail::GDINum(NewColor.R) +
            " | Green: " + ::GDILogDetail::GDINum(NewColor.G) +
            " | Blue: " + ::GDILogDetail::GDINum(NewColor.B));
#endif

    delete[] Data;
    Data = nullptr;

    return NewColor;
}

void OpenGLDynamicGDI::BeginDrawEvent(const std::string &InStr)
{
    const int StrLength = InStr.length();
    const int DebugGroupID = GDebugGroupID;
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, DebugGroupID, StrLength, QSTR_TO_CONSTCHAR(InStr));
}

void OpenGLDynamicGDI::EndDrawEvent()
{
    glPopDebugGroup();
}

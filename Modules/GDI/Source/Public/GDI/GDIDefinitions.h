#ifndef CORECLIENT_GDIDEFINITIONS_H
#define CORECLIENT_GDIDEFINITIONS_H

/*
 * uint / uchar. This header has always used them (IStencilStateDesc::Mask and
 * friends) without ever declaring them -- they leaked in from <QtGlobal> via
 * whichever Qt header happened to be included first. That works only for as
 * long as Qt is in the build, and there are ~185 such uses across GDI. Taking
 * the typedefs from EngineCore instead makes them independent of Qt now,
 * rather than at the moment the last Qt include is removed.
 */
#include "Public/Core/CoreTypes.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

#ifndef QSTR_TO_CONSTCHAR
#define QSTR_TO_CONSTCHAR( String ) String.c_str()
#endif //QSTR_TO_CONSTCHAR

/*
 * QSTR_TO_CONSTCHAR_UTF8 was removed: it had no callers and still expanded to
 * QByteArray/toUtf8, so it would not have compiled after the string change.
 * std::string::c_str() is what the one remaining macro above does.
 */

#ifndef TYPEID_TO_QSTR
#define TYPEID_TO_QSTR( Input ) std::string( typeid(Input).name() )
#endif

enum EClearMode : char
{
    CM_Color = 1,
    CM_Depth = 2,
    CM_Stencil = 4,
    CM_Max
};

enum ECompareTestMode : char
{
    CTM_Never = 0,
    CTM_ALWAYS,
    CTM_LESS,
    CTM_LEQUAL,
    CTM_GREATER,
    CTM_GEQUAL,
    CTM_EQUAL,
    CTM_NOTEQUAL,
    CTM_MAX
};

enum EBlendMode : char
{
    BM_Zero = 0,
    BM_One,
    BM_Src_Color,
    BM_One_Minus_Src_Color,
    BM_Dest_Color,
    BM_One_Minus_Dest_Color,
    BM_Src_Alpha,
    BM_One_Minus_Src_Alpha,
    BM_Dest_Alpha,
    BM_One_Minus_Dest_Alpha,
    BM_Constant_Color,
    BM_One_Minus_Constant_Color,
    BM_Constant_Alpha,
    BM_One_Minus_Constant_Alpha,
    BM_Src_Saturate,
    BM_MAX
};

// Blend equation applied to (Src*SrcFactor) and (Dst*DstFactor). Defaults to Add
// everywhere; Max is used by the brush stamp pass so overlapping stamps take the
// highest per-stamp alpha (pressure) instead of compounding into visible ribbing.
enum EBlendOp : char
{
    BO_Add = 0,
    BO_Max,
    BO_Min,
    BO_MAX
};

enum EStencilOp
{
    SO_Keep = 0,
    SO_Zero,
    SO_Replace,
    SO_Incr,
    SO_Incr_Wrap,
    SO_Decr,
    SO_Decr_Wrap,
    SO_Invert
};

enum EGDITextureType : char
{
    GDI_TT_QtNative = 0,
    GDI_TT_OpenGL,

    /*Insert new types above this line.*/
    MAX_EGDITextureType
};

enum EGDITextureBorder : char
{
    GDI_TB_Repeat = 0,
    GDI_TB_Mirrored_Repeat,
    GDI_TB_Clamp_To_Edge,
    GDI_TB_Clamp_To_Border,
    GDI_TB_MAX
};

enum EGDITextureFilter : char
{
    GDI_TF_Nearest = 0,
    GDI_TF_Linear,
    GDI_TF_MAX
};

enum EShaderType : char
{
    ST_Vertex = 0,
    ST_Pixel,
    /*Insert new shader types above this.*/
    ST_MAX
};

enum EColorMask : char
{
    EColM_None=     0,
    EColM_R =		1,
    EColM_G =		2,
    EColM_B =		4,
    EColM_A =		8,
    EColM_RGB =		9,
    EColM_RGBA =	10,
    EColM_MAX
};

/*
 * ERP_Null IS ZERO, deliberately -- the one edit RenderCore makes inside this
 * vendored subtree (see Engine/Modules/RenderCore/DESIGN.md). A
 * zero-initialized or default-constructed platform value should be the one
 * that needs no window and no GPU, not ERP_OpenGL, which needs a live WGL
 * context. RenderCore::ERenderBackend (RenderTypes.h) mirrors these values
 * and RenderCoreInternal.h static_asserts every pair, so a future reorder --
 * including a subtree pull that reverts this one -- fails the build instead
 * of silently selecting the wrong backend.
 */
enum ERenderPlatform : char
{
    ERP_Null = 0,
    ERP_OpenGL,
    ERP_Vulkan,
    ERP_Metal,
    ERP_D3D12,
    ERP_MAX
};

struct IDepthStateDesc
{
    bool				bDepthTest;
    bool                bDepthMask;
    ECompareTestMode	DepthMode;

    IDepthStateDesc() :
            bDepthTest(false),
            bDepthMask(false),
            DepthMode(CTM_LESS) {  }
};

struct IStencilStateDesc
{
    bool				bStencilTest;
    ECompareTestMode	StencilMode;
    int					Reference;
    uint				Mask;
    EStencilOp          SrcFail;
    EStencilOp          DestFail;
    EStencilOp          SDPass;

    IStencilStateDesc() :
            bStencilTest(false),
            StencilMode(CTM_LESS),
            Reference(0), Mask(1),
            SrcFail(SO_Keep),
            DestFail(SO_Keep),
            SDPass(SO_Keep){  }

    IStencilStateDesc(const bool bTest,
                      const ECompareTestMode InStencilMode,
                      int InReference, int InMask,
                      EStencilOp InSrcFail, EStencilOp InDestFail,
                      EStencilOp InSDPass):
                      bStencilTest(bTest),
                      StencilMode(InStencilMode),
                      Reference(InReference),
                      Mask(InMask),
                      SrcFail(InSrcFail),
                      DestFail(InDestFail),
                      SDPass(InSDPass) {}
};

struct IBlendStateDesc
{
    bool				bBlend;
    EBlendMode			Source;
    EBlendMode			Dest;
    EBlendOp			Op;         // color blend equation
    EBlendOp			AlphaOp;    // alpha blend equation (defaults to Op)

    IBlendStateDesc() :
            bBlend(false),
            Source(BM_One),
            Dest(BM_Zero),
            Op(BO_Add),
            AlphaOp(BO_Add) {  }

    IBlendStateDesc(const bool bInBlend, const EBlendMode InSource, const EBlendMode InDest,
                    const EBlendOp InOp = BO_Add) :
    bBlend(bInBlend),
    Source(InSource),
    Dest(InDest),
    Op(InOp),
    AlphaOp(InOp)
    {

    }

    IBlendStateDesc(const bool bInBlend, const EBlendMode InSource, const EBlendMode InDest,
                    const EBlendOp InColorOp, const EBlendOp InAlphaOp) :
    bBlend(bInBlend),
    Source(InSource),
    Dest(InDest),
    Op(InColorOp),
    AlphaOp(InAlphaOp)
    {

    }
};

struct ITextureState
{
    EGDITextureBorder	WrapS;
    EGDITextureBorder	WrapT;
    EGDITextureFilter	MinFilter;
    EGDITextureFilter	MaxFilter;

    ITextureState() :
            WrapS(GDI_TB_Repeat),
            WrapT(GDI_TB_Repeat),
            MinFilter(GDI_TF_Linear),
            MaxFilter(GDI_TF_Linear) {  }
};

/*
 * The material binding layout (Engine/Modules/RenderCore/MATERIALS.md). A draw
 * whose vertex or pixel shader resource has SetMaterialLayout(true) binds:
 *
 *   0, 1   View, Transform        as every shader
 *   2      material block         GDISetUniformBlock, raw std140 bytes
 *   3      scene block            GDISetUniformBlock, raw std140 bytes
 *   4-11   material textures 0-7  GDISetMaterialTexture
 *
 * Both blocks are visible to both stages. Shaders declare them with
 * UBO_BLOCK_BEGIN/END and the textures with MATERIAL_SAMPLER2D.
 */
constexpr unsigned GDI_MATERIAL_BLOCK_BINDING   = 2;
constexpr unsigned GDI_SCENE_BLOCK_BINDING      = 3;
constexpr unsigned GDI_MATERIAL_TEXTURE_BINDING = 4;
constexpr unsigned GDI_MATERIAL_TEXTURE_SLOTS   = 8;
constexpr unsigned GDI_MATERIAL_BLOCK_MAX_BYTES = 1024;

/* Colour targets one pass can write (GDISetRenderTargets): a G-buffer's. */
constexpr unsigned GDI_MAX_COLOR_TARGETS        = 4;

#endif //CORECLIENT_GDIDEFINITIONS_H

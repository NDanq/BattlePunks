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
#include <cstddef>
#include <cstdint>
#include <string>
#include <typeinfo>

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

/* A render target's colour format. RGBA8 is every backend's; RGBA16F (half
 * float, HDR) only where GDISupportsRenderTargetFormat says so. */
enum EGDIRenderTargetFormat : char
{
    GDI_RTF_RGBA8   = 0,
    GDI_RTF_RGBA16F = 1,
};

/* A texture's 4x4 block-compressed format (GDICreateCompressedTextureResource),
 * UNORM -- each reads as its RGBA8 source would: BC1 RGB with alpha 1, BC3
 * RGBA, BC4 (r, 0, 0, 1), BC5 (r, g, 0, 1), BC7 RGBA. BC1 and BC4 are 8
 * bytes a block, the rest 16. */
enum EGDICompressedFormat : char
{
    GDI_CF_BC1 = 0,
    GDI_CF_BC3,
    GDI_CF_BC4,
    GDI_CF_BC5,
    GDI_CF_BC7,
    GDI_CF_MAX
};

/* One level of a block-compressed texture: Width x Height texels, Bytes of
 * blocks row by row ((Width + 3) / 4 of them a row). */
struct GDICompressedLevel
{
    uint32_t       Width  = 0;
    uint32_t       Height = 0;
    const uint8_t* Data   = nullptr;
    size_t         Bytes  = 0;
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
    ST_Compute,   // a .csh: GDIDispatchCompute, where GDISupportsCompute()
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
 *   12     skin block             GDISetUniformBlock: a skinned mesh's bone
 *                                 palette, three vec4 rows per bone (Skinning.glsl)
 *   13     previous skin block    GDISetUniformBlock: the palette it had the
 *                                 frame before, for its velocity (as 12)
 *
 * The blocks are visible to both stages. Shaders declare them with
 * UBO_BLOCK_BEGIN/END and the textures with MATERIAL_SAMPLER2D. Only a
 * skinned vertex shader declares the skin block; every material draw binds
 * whatever was last set (16 zero bytes if nothing was), so set it before
 * each skinned draw. Vulkan, D3D12 and OpenGL; Metal draws no indexed meshes.
 */
constexpr unsigned GDI_MATERIAL_BLOCK_BINDING   = 2;
constexpr unsigned GDI_SCENE_BLOCK_BINDING      = 3;
constexpr unsigned GDI_MATERIAL_TEXTURE_BINDING = 4;
constexpr unsigned GDI_MATERIAL_TEXTURE_SLOTS   = 8;
constexpr unsigned GDI_SKIN_BLOCK_BINDING       = 12;
constexpr unsigned GDI_PREV_SKIN_BLOCK_BINDING  = 13;
/* 256 bones of three vec4 rows: within every backend's guaranteed 16 KB. */
constexpr unsigned GDI_SKIN_MAX_BONES           = 256;
constexpr unsigned GDI_SKIN_BLOCK_MAX_BYTES     = GDI_SKIN_MAX_BONES * 48;
constexpr unsigned GDI_GPU_TIMESTAMP_MAX        = 64;   // GPU timestamps one BeginRendering/EndRendering block may write
constexpr unsigned GDI_MATERIAL_BLOCK_MAX_BYTES = 1024;
/* The scene block carries the frame's lights and shadow matrices: larger. */
constexpr unsigned GDI_SCENE_BLOCK_MAX_BYTES    = 4096;

/* The OpenGL backend's minimum: a 4.5 core context. Every shader declares
 * "#version 450" (Vulkan's SPIR-V is compiled from the same source). */
constexpr int      GDI_OPENGL_MIN_MAJOR         = 4;
constexpr int      GDI_OPENGL_MIN_MINOR         = 5;

/* Colour targets one pass can write (GDISetRenderTargets): a G-buffer's and
 * its velocity. Each may have its own format. */
constexpr unsigned GDI_MAX_COLOR_TARGETS        = 5;

/* Chunks one parallel pass records at most (GDIBeginParallelPass). */
constexpr unsigned GDI_PARALLEL_CHUNKS_MAX      = 32;

/*
 * A compute dispatch's bindings (GDIDispatchCompute): its parameters as a
 * uniform block at binding 0, then its storage buffers at bindings 1, 2, ...
 * Shaders declare them with COMPUTE_PARAMS_BEGIN/END and STORAGE_BUFFER.
 */
constexpr unsigned GDI_COMPUTE_PARAMS_MAX_BYTES     = 1024;
constexpr unsigned GDI_COMPUTE_MAX_STORAGE_BUFFERS  = 4;

#endif //CORECLIENT_GDIDEFINITIONS_H

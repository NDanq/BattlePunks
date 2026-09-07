#ifndef CORECLIENT_MTLUTIL_H
#define CORECLIENT_MTLUTIL_H

// Backend-internal helpers. Included only from Objective-C++ (.mm) sources -- it
// returns Metal enum types, which are only defined by <Metal/Metal.h>.
#ifndef __OBJC__
#error "MTLUtil.h is Objective-C++ only; include it from a .mm translation unit."
#endif

#import <Metal/Metal.h>

#include "Public/GDI/GDIDefinitions.h"

// ---------------------------------------------------------------------------
// State decoders -- the Metal analog of VKUtil::Decode* (QVKCommands.cpp) and the
// sampler/compare tables in QVKTexture.cpp / QVKPipelineCache.cpp. These translate
// the GDI's platform-neutral render-state enums into Metal enum values, so the
// pipeline/sampler/depth-stencil descriptors can be built directly from GDI state.
// ---------------------------------------------------------------------------
namespace MTLUtil
{
    /** EBlendMode -> MTLBlendFactor (1:1; BM_Src_Saturate -> SourceAlphaSaturated). */
    MTLBlendFactor        DecodeBlendFactor(EBlendMode InMode);

    /** EBlendOp -> MTLBlendOperation (the blend equation; BO_Max backs the brush
     *  stamp pass, where overlapping stamps must take the highest alpha). */
    MTLBlendOperation     DecodeBlendOp(EBlendOp InOp);

    /** EColorMask -> MTLColorWriteMask. Mirrors VKUtil::DecodeColorMask exactly:
     *  the RGB/RGBA enumerators are symbolic tags, not bitfields. */
    MTLColorWriteMask     DecodeColorMask(EColorMask InMask);

    /** ECompareTestMode -> MTLCompareFunction (depth + stencil compare). */
    MTLCompareFunction    DecodeCompareFunction(ECompareTestMode InMode);

    /** EStencilOp -> MTLStencilOperation. */
    MTLStencilOperation   DecodeStencilOp(EStencilOp InOp);

    /** EGDITextureBorder -> MTLSamplerAddressMode. Clamp-to-border is macOS-only;
     *  on iOS it degrades to ClampToEdge (see the plan's iOS note). */
    MTLSamplerAddressMode DecodeSamplerAddress(EGDITextureBorder InBorder);

    /** EGDITextureFilter -> MTLSamplerMinMagFilter. */
    MTLSamplerMinMagFilter DecodeSamplerFilter(EGDITextureFilter InFilter);
}

#endif //CORECLIENT_MTLUTIL_H

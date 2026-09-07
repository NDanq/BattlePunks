#include "Private/GDI/Metal/MTLUtil.h"

#include <TargetConditionals.h>

namespace MTLUtil
{

MTLBlendFactor DecodeBlendFactor(EBlendMode InMode)
{
    switch(InMode)
    {
        case BM_Zero:                       return MTLBlendFactorZero;
        case BM_One:                        return MTLBlendFactorOne;
        case BM_Src_Color:                  return MTLBlendFactorSourceColor;
        case BM_One_Minus_Src_Color:        return MTLBlendFactorOneMinusSourceColor;
        case BM_Dest_Color:                 return MTLBlendFactorDestinationColor;
        case BM_One_Minus_Dest_Color:       return MTLBlendFactorOneMinusDestinationColor;
        case BM_Src_Alpha:                  return MTLBlendFactorSourceAlpha;
        case BM_One_Minus_Src_Alpha:        return MTLBlendFactorOneMinusSourceAlpha;
        case BM_Dest_Alpha:                 return MTLBlendFactorDestinationAlpha;
        case BM_One_Minus_Dest_Alpha:       return MTLBlendFactorOneMinusDestinationAlpha;
        case BM_Constant_Color:             return MTLBlendFactorBlendColor;
        case BM_One_Minus_Constant_Color:   return MTLBlendFactorOneMinusBlendColor;
        case BM_Constant_Alpha:             return MTLBlendFactorBlendAlpha;
        case BM_One_Minus_Constant_Alpha:   return MTLBlendFactorOneMinusBlendAlpha;
        case BM_Src_Saturate:               return MTLBlendFactorSourceAlphaSaturated;
        default:                            return MTLBlendFactorZero;
    }
}

MTLBlendOperation DecodeBlendOp(EBlendOp InOp)
{
    switch(InOp)
    {
        case BO_Max: return MTLBlendOperationMax;
        case BO_Min: return MTLBlendOperationMin;
        case BO_Add:
        default:     return MTLBlendOperationAdd;
    }
}

MTLColorWriteMask DecodeColorMask(EColorMask InMask)
{
    // NOTE: EColM_RGB (9) / EColM_RGBA (10) are symbolic tags, not OR'd bitfields
    // (R=1,G=2,B=4,A=8). Switch on the exact values, mirroring VKUtil::DecodeColorMask.
    switch(InMask)
    {
        case EColM_None: return MTLColorWriteMaskNone;
        case EColM_R:    return MTLColorWriteMaskRed;
        case EColM_G:    return MTLColorWriteMaskGreen;
        case EColM_B:    return MTLColorWriteMaskBlue;
        case EColM_A:    return MTLColorWriteMaskAlpha;
        case EColM_RGB:  return MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        case EColM_RGBA: return MTLColorWriteMaskAll;
        default:         return MTLColorWriteMaskAll;
    }
}

MTLCompareFunction DecodeCompareFunction(ECompareTestMode InMode)
{
    switch(InMode)
    {
        case CTM_Never:    return MTLCompareFunctionNever;
        case CTM_ALWAYS:   return MTLCompareFunctionAlways;
        case CTM_LESS:     return MTLCompareFunctionLess;
        case CTM_LEQUAL:   return MTLCompareFunctionLessEqual;
        case CTM_GREATER:  return MTLCompareFunctionGreater;
        case CTM_GEQUAL:   return MTLCompareFunctionGreaterEqual;
        case CTM_EQUAL:    return MTLCompareFunctionEqual;
        case CTM_NOTEQUAL: return MTLCompareFunctionNotEqual;
        default:           return MTLCompareFunctionLess;
    }
}

MTLStencilOperation DecodeStencilOp(EStencilOp InOp)
{
    switch(InOp)
    {
        case SO_Keep:      return MTLStencilOperationKeep;
        case SO_Zero:      return MTLStencilOperationZero;
        case SO_Replace:   return MTLStencilOperationReplace;
        case SO_Incr:      return MTLStencilOperationIncrementClamp;
        case SO_Incr_Wrap: return MTLStencilOperationIncrementWrap;
        case SO_Decr:      return MTLStencilOperationDecrementClamp;
        case SO_Decr_Wrap: return MTLStencilOperationDecrementWrap;
        case SO_Invert:    return MTLStencilOperationInvert;
        default:           return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode DecodeSamplerAddress(EGDITextureBorder InBorder)
{
    switch(InBorder)
    {
        case GDI_TB_Repeat:          return MTLSamplerAddressModeRepeat;
        case GDI_TB_Mirrored_Repeat: return MTLSamplerAddressModeMirrorRepeat;
        case GDI_TB_Clamp_To_Edge:   return MTLSamplerAddressModeClampToEdge;
        case GDI_TB_Clamp_To_Border:
#if TARGET_OS_OSX
            return MTLSamplerAddressModeClampToBorderColor;
#else
            // ClampToBorderColor is unavailable on iOS -- degrade to ClampToEdge.
            return MTLSamplerAddressModeClampToEdge;
#endif
        default:                     return MTLSamplerAddressModeRepeat;
    }
}

MTLSamplerMinMagFilter DecodeSamplerFilter(EGDITextureFilter InFilter)
{
    switch(InFilter)
    {
        case GDI_TF_Nearest: return MTLSamplerMinMagFilterNearest;
        case GDI_TF_Linear:  return MTLSamplerMinMagFilterLinear;
        default:             return MTLSamplerMinMagFilterNearest;
    }
}

} // namespace MTLUtil

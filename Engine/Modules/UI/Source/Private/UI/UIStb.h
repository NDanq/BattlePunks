#ifndef UI_UISTB_H
#define UI_UISTB_H

/*
 * stb_truetype and stb_rect_pack (ThirdParty/Source/stb), compiled into the one file
 * that bakes fonts (UIFont.cpp) with internal linkage, so they can never
 * clash with another stb copy in the process. Include nowhere else.
 */

#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable : 4244 4267 4505)
#endif

#include "stb_rect_pack.h"
#include "stb_truetype.h"

#if defined(__clang__)
    #pragma clang diagnostic pop
#elif defined(__GNUC__)
    #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
    #pragma warning(pop)
#endif

#endif //UI_UISTB_H

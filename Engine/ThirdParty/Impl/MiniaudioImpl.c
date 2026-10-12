/*
 * miniaudio's single implementation unit, with stb_vorbis for .ogg: the
 * header first so miniaudio sees stb_vorbis' declarations and enables its
 * Vorbis decoder, then the implementation (miniaudio's documented order).
 */

#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

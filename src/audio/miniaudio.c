/* miniaudio's implementation for the built-in audio backend (JSUI_AUDIO,
 * cmake/Deps.cmake), with Ogg Vorbis decoding through stb_vorbis: its
 * declarations first, so miniaudio enables its Vorbis decoder, then its
 * implementation. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

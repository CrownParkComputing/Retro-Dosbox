/* Retro-Dosbox sdl3: SDL2-style bare include -> SDL3 prefixed header. */
#ifndef RETRODOSBOX_SDL3COMPAT_SDL_audio_H
#define RETRODOSBOX_SDL3COMPAT_SDL_audio_H
#include <SDL3/SDL_audio.h>
#endif

/* SDL3 REMOVED the unsigned 16-bit audio formats; only U8 and the signed
 * formats survive. The vendored SDL_sound decoders still name them in
 * conversion tables. Restoring SDL2's numeric values (now unused by SDL3)
 * keeps those tables compiling; the branches are dead because SDL3 can never
 * report a U16 format. */
#ifndef AUDIO_U16LSB
#define AUDIO_U16LSB ((SDL_AudioFormat)0x0010)
#define AUDIO_U16MSB ((SDL_AudioFormat)0x1010)
#define AUDIO_U16    AUDIO_U16LSB
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define AUDIO_U16SYS AUDIO_U16LSB
#else
#define AUDIO_U16SYS AUDIO_U16MSB
#endif
#endif

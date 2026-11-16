/* Retro-Dosbox sdl3 branch: SDL2-style bare include -> SDL3's prefixed header.
 * SDL3 installs to /usr/include/SDL3 and 'pkg-config sdl3 --cflags' is EMPTY,
 * so "SDL_endian.h" cannot resolve without this forwarder. */
#ifndef RETRODOSBOX_SDL3COMPAT_SDL_endian_H
#define RETRODOSBOX_SDL3COMPAT_SDL_endian_H
#include <SDL3/SDL_endian.h>
#endif

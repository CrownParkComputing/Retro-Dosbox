/* Retro-Dosbox sdl3: SDL3 ships no SDL_config.h -- it was an SDL1/SDL2 build
 * artifact, and in an SDL2 build this include resolved to
 * /usr/include/SDL2/SDL_config.h.
 *
 * The vendored SDL1 CD-ROM code (vs/sdl/src/cdrom, which DOSBox-X still uses
 * because SDL2 and SDL3 both dropped CD-ROM support) needs the platform
 * defines that file supplied. In particular SDL_syscdrom.c reads:
 *
 *     #ifdef SDL_CDROM_LINUX        <- gates the whole implementation
 *     ...
 *     #ifdef __LINUX__              <- gates #include <linux/cdrom.h>
 *
 * Without __LINUX__ the kernel header never arrives and every ioctl constant
 * (CDROM_MSF, CDROMSUBCHNL, struct cdrom_tochdr ...) is undeclared -- 28
 * errors that look like missing CD-ROM support but are one missing define.
 */
#ifndef RETRODOSBOX_SDL3COMPAT_SDL_CONFIG_H
#define RETRODOSBOX_SDL3COMPAT_SDL_CONFIG_H

#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_stdinc.h>

#if defined(__linux__) && !defined(__LINUX__)
#define __LINUX__ 1
#endif

#if defined(__linux__) && !defined(HAVE_LINUX_VERSION_H)
#define HAVE_LINUX_VERSION_H 1
#endif

#endif

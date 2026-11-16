/*
 * Retro-Dosbox (sdl3 branch): SDL2's SDL_syswm.h over SDL3.
 *
 * SDL3 DELETED this header outright. Native window handles now come from the
 * window-properties API:
 *
 *     SDL_PropertiesID p = SDL_GetWindowProperties(win);
 *     Display *d = SDL_GetPointerProperty(p, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
 *     Window   w = SDL_GetNumberProperty (p, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
 *
 * DOSBox-X reads exactly four X11 fields (display, window, fswindow, wmwindow)
 * plus .subsystem, across sdlmain_linux.cpp (X11 IME / WM hints), sdlmain.cpp,
 * clipboard.cpp and glide.cpp -- about 45 sites. Rewriting all of them is
 * pointless churn when the shape maps cleanly, so this reconstructs the old
 * struct and fills it from the new API.
 *
 * fswindow/wmwindow no longer exist as distinct handles in SDL3 (they were
 * SDL2 internals for its own fullscreen/WM wrapper windows); both are reported
 * as the real window, which is what the call sites want -- they use them to
 * set WM properties.
 *
 * This shim is X11-only by design. Under Wayland the subsystem is reported as
 * such and the X11 fields stay zero, so callers take their "unknown windowing
 * system" path rather than acting on a null Display.
 */
#ifndef RETRODOSBOX_SDL3COMPAT_SDL_SYSWM_H
#define RETRODOSBOX_SDL3COMPAT_SDL_SYSWM_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDL_SYSWM_UNKNOWN = 0,
    SDL_SYSWM_WINDOWS,
    SDL_SYSWM_X11,
    SDL_SYSWM_COCOA,
    SDL_SYSWM_WAYLAND,
    SDL_SYSWM_ANDROID
} SDL_SYSWM_TYPE;

typedef struct SDL_SysWMinfo {
    Uint32          version;      /* SDL2 had an SDL_version struct here; SDL3
                                   * removed the type. Nothing in this tree
                                   * reads it, and SDL_VERSION() below keeps
                                   * the old call sites compiling. */
    SDL_SYSWM_TYPE  subsystem;
    struct {
        struct {
            void        *display;   /* ::Display*  */
            unsigned long window;   /* ::Window    */
            unsigned long fswindow; /* SDL2-ism; == window here */
            unsigned long wmwindow; /* SDL2-ism; == window here */
        } x11;
    } info;
} SDL_SysWMinfo;

/* SDL2 delivered native window-system messages through SDL_SYSWMEVENT with an
 * SDL_SysWMmsg payload. SDL3 removed the whole mechanism (Win32 hooks go
 * through SDL_SetWindowsMessageHook instead). menu.cpp still declares a
 * handler taking one, so the type must exist; the Win32 body behind it is
 * compiled only on WIN32, and nothing constructs one of these on Linux. */
typedef struct SDL_SysWMmsg {
    Uint32         version;
    SDL_SYSWM_TYPE subsystem;
    union {
        struct { void *hwnd; unsigned int msg; Uint64 wParam; Sint64 lParam; } win;
        int dummy;
    } msg;
} SDL_SysWMmsg;

/* SDL2 spelled this SDL_VERSION(&info.version) before SDL_GetWindowWMInfo(). */
#ifndef SDL_VERSION
#define SDL_VERSION(x) do { *(Uint32*)(x) = (Uint32)SDL_VERSIONNUM(SDL_MAJOR_VERSION,SDL_MINOR_VERSION,SDL_MICRO_VERSION); } while (0)
#endif

static inline bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info)
{
    const char *driver;

    if (window == NULL || info == NULL) return false;

    SDL_memset(info, 0, sizeof(*info));

    driver = SDL_GetCurrentVideoDriver();
    if (driver == NULL) return false;

    if (SDL_strcmp(driver, "x11") == 0) {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        unsigned long w;

        info->subsystem      = SDL_SYSWM_X11;
        info->info.x11.display =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        w = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        info->info.x11.window   = w;
        info->info.x11.fswindow = w;
        info->info.x11.wmwindow = w;
        return true;
    }

    if (SDL_strcmp(driver, "wayland") == 0) info->subsystem = SDL_SYSWM_WAYLAND;
    else if (SDL_strcmp(driver, "cocoa") == 0) info->subsystem = SDL_SYSWM_COCOA;
    else if (SDL_strcmp(driver, "windows") == 0) info->subsystem = SDL_SYSWM_WINDOWS;
    else info->subsystem = SDL_SYSWM_UNKNOWN;

    /* Reported, but with no X11 handles -- callers must not use them. */
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* RETRODOSBOX_SDL3COMPAT_SDL_SYSWM_H */

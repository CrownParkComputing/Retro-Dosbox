/*
 * Retro-Dosbox (sdl3 branch): SDL2-source-compatibility header over SDL3.
 *
 * WHY THIS EXISTS
 * ---------------
 * DOSBox-X's own plan for SDL3 is in include/dosbox.h:
 *
 *   // HACK: To make SDL3 porting easier, define SDL2 to prevent SDL1 code
 *   //       from compiling
 *   #if defined(C_SDL3) && !defined(C_SDL2)
 *   # define C_SDL2 1
 *   #endif
 *
 * i.e. keep the SDL2-era code paths compiling and put SDL3 underneath them,
 * rather than adding a third #if branch to ~3000 call sites (sdlmain.cpp alone
 * has 532). This header is the "underneath" half.
 *
 * THE RENAMES ARE NOT OUR PROBLEM
 * -------------------------------
 * SDL3 ships <SDL3/SDL_oldnames.h> with ~1032 SDL2 -> SDL3 name mappings,
 * gated on SDL_ENABLE_OLD_NAMES (which we set in CPPFLAGS). That covers the
 * entire mechanical-rename pile -- SDL_FreeSurface, SDL_CondWait,
 * SDL_AllocPalette, SDL_RenderCopy, SDL_UpperBlit and the rest -- officially
 * and maintained by SDL upstream. Do NOT hand-roll those here; a local #define
 * would only shadow a better one.
 *
 * Note what happens WITHOUT the flag: old names map to e.g.
 * SDL_FreeSurface_renamed_SDL_DestroySurface, so you get a compile error that
 * names the replacement. That is a feature -- it is how you find genuinely
 * changed APIs rather than silently mistranslating them.
 *
 * SO WHAT IS LEFT FOR THIS FILE
 * -----------------------------
 * Only what SDL_oldnames.h deliberately does NOT cover: APIs whose SEMANTICS
 * changed, not merely their spelling. Those cannot be a rename, because a
 * rename would compile and then behave differently. Every entry below earns
 * its place with a comment saying what actually changed.
 *
 * The dangerous class to watch for while porting: SDL3 flipped many functions
 * from "0 == success" to "true == success". An inverted check still compiles
 * AND still runs. SDL_oldnames.h does not and cannot save you from that.
 */
#ifndef RETRODOSBOX_SDL3COMPAT_SDL_H
#define RETRODOSBOX_SDL3COMPAT_SDL_H

#ifndef SDL_ENABLE_OLD_NAMES
/* Also set in CPPFLAGS by configure; belt and braces for any TU that reaches
 * this header first. Must precede <SDL3/SDL.h>. */
#define SDL_ENABLE_OLD_NAMES 1
#endif

#include <SDL3/SDL.h>

/* ---------------------------------------------------------------------- */
/* Spellings SDL_oldnames.h does not cover                                 */
/* ---------------------------------------------------------------------- */

/* SDL2 exported DECLSPEC; SDL3 namespaced it to SDL_DECLSPEC. This is not in
 * SDL_oldnames.h because it is a build macro rather than an API name, yet it
 * accounts for ~85% of the errors in this tree: the vendored SDL_ttf.h and
 * SDL_sound.h headers use it in every prototype (SNDDECLSPEC is defined in
 * terms of DECLSPEC, so this fixes both). */
#ifndef DECLSPEC
#define DECLSPEC SDL_DECLSPEC
#endif

/* ---------------------------------------------------------------------- */
/* Semantic changes SDL_oldnames.h does not cover                          */
/* ---------------------------------------------------------------------- */

/* SDL2: SDL_SetRelativeMouseMode(SDL_bool) was global.
 * SDL3: relative mode is per-window. There is no global form to rename to. */
#ifndef SDL_SetRelativeMouseMode
#define SDL_SetRelativeMouseMode(b) SDL_SetWindowRelativeMouseMode(NULL,(b))
#endif

/* SDL2: SDL_FreeFormat() released a per-surface SDL_PixelFormat object.
 * SDL3: SDL_Surface::format is a plain enum; there is nothing to free. */
#ifndef SDL_FreeFormat
#define SDL_FreeFormat(x) ((void)(x))
#endif

/* SDL2: SDL_ShowCursor(SDL_ENABLE|SDL_DISABLE) both set and queried.
 * SDL3: split into SDL_ShowCursor(void) / SDL_HideCursor(void), and the
 * SDL_ENABLE/SDL_DISABLE constants are gone.
 *
 * Every call site in this tree passes an argument (SDL2 style), so a
 * function-like macro is safe here -- but note it would break a genuine
 * zero-argument SDL3 call, so do not introduce one. */
#ifndef SDL_ENABLE
#define SDL_ENABLE  1
#endif
#ifndef SDL_DISABLE
#define SDL_DISABLE 0
#endif
static inline bool retrodosbox_sdl3_show_cursor(int on) {
    return on ? SDL_ShowCursor() : SDL_HideCursor();
}
#define SDL_ShowCursor(x) retrodosbox_sdl3_show_cursor(x)

/* SDL2: text input was GLOBAL -- SDL_StartTextInput(void),
 *       SDL_StopTextInput(void), SDL_SetTextInputRect(const SDL_Rect*).
 * SDL3: all three are per-window, and SDL_SetTextInputRect is gone entirely,
 *       replaced by SDL_SetTextInputArea(window, rect, cursor).
 *
 * There is no global form to rename to, which is why SDL_oldnames.h does not
 * cover these. SDL_GetKeyboardFocus() is the honest equivalent of "the window
 * SDL2 would have meant": the one currently taking keyboard input.
 *
 * These only surface on a build that defines MACOSX, because the call sites
 * sit behind `(defined(WIN32) || defined(MACOSX)) && defined(C_SDL2)` -- so an
 * iOS build hits them and Android never does.
 *
 * Each helper is defined BEFORE its macro, so the body calls the real SDL3
 * function rather than expanding into itself. */
static inline bool retrodosbox_sdl3_start_text_input(void) {
    return SDL_StartTextInput(SDL_GetKeyboardFocus());
}
#define SDL_StartTextInput() retrodosbox_sdl3_start_text_input()

static inline bool retrodosbox_sdl3_stop_text_input(void) {
    return SDL_StopTextInput(SDL_GetKeyboardFocus());
}
#define SDL_StopTextInput() retrodosbox_sdl3_stop_text_input()

/* cursor 0: the tree sets only the rectangle, never a cursor offset within it. */
static inline bool retrodosbox_sdl3_set_text_input_rect(const SDL_Rect *r) {
    return SDL_SetTextInputArea(SDL_GetKeyboardFocus(), r, 0);
}
#define SDL_SetTextInputRect(r) retrodosbox_sdl3_set_text_input_rect(r)

/* SDL2: one SDL_WINDOWEVENT type, with the specific event in
 *       event.window.event.
 * SDL3: each window event is its OWN top-level type (SDL_EVENT_WINDOW_*), and
 *       event.window has no .event member.
 *
 * SDL_oldnames.h already maps every SDL_WINDOWEVENT_xxx constant to its
 * SDL_EVENT_WINDOW_xxx equivalent, so the inner switches keep working once
 * `.window.event` is rewritten to `.type` (done in the sources).
 *
 * That leaves the outer `case SDL_WINDOWEVENT:` wrapper, which must now match
 * ANY window event. SDL3 guarantees they are a contiguous range, so this
 * expands to a GCC/Clang case range and the wrapper needs no source edit:
 *
 *     case SDL_WINDOWEVENT:  ->  case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST:
 *
 * Consequence: SDL_WINDOWEVENT is now USABLE ONLY AS A CASE LABEL. Any
 * `event.type == SDL_WINDOWEVENT` comparison is a syntax error -- deliberately,
 * because such a test needs rethinking rather than translating. The one site in
 * sdlmain.cpp was rewritten to test the specific event instead. */
#ifndef SDL_WINDOWEVENT
#define SDL_WINDOWEVENT SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST
#endif

/* SDL2 had two fullscreen flags; SDL3 has one (fullscreen is always
 * "desktop"-style, with the video mode chosen via SDL_SetWindowFullscreenMode). */
#ifndef SDL_WINDOW_FULLSCREEN_DESKTOP
#define SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
#endif

/* SDL2 button/key state was Uint8 SDL_PRESSED/SDL_RELEASED; SDL3 uses a bool
 * `down` field. These cover standalone uses of the constants -- but a
 * comparison against event.button.state still needs a source edit, because the
 * STRUCT MEMBER was renamed too. That is the point: the constant can be
 * shimmed, the member cannot, and pretending otherwise would hide the change. */
#ifndef SDL_PRESSED
#define SDL_PRESSED  true
#endif
#ifndef SDL_RELEASED
#define SDL_RELEASED false
#endif

/* ---------------------------------------------------------------------- */
/* SDL_Keysym                                                              */
/* ---------------------------------------------------------------------- */
/* SDL3 deleted SDL_Keysym and folded its fields straight into
 * SDL_KeyboardEvent. Most call sites just became event.key.scancode etc., but
 * a few pass "the keysym" around as a value -- gui_tk's SDL_to_GUI() and
 * SDLSymToChar() take one by const reference, and sdl_mapper binds one.
 * Reconstructing the small struct is far less invasive than rewriting those
 * signatures to take a whole event. */
typedef struct SDL_Keysym {
    SDL_Scancode scancode;
    SDL_Keycode  sym;
    Uint16       mod;
} SDL_Keysym;

static inline SDL_Keysym retrodosbox_keysym(const SDL_KeyboardEvent *e)
{
    SDL_Keysym k;
    k.scancode = e->scancode;
    k.sym      = e->key;
    k.mod      = (Uint16)e->mod;
    return k;
}

/* ---------------------------------------------------------------------- */
/* Removed flags                                                           */
/* ---------------------------------------------------------------------- */
/* SDL3 windows are shown unless SDL_WINDOW_HIDDEN; SDL_SWSURFACE was an
 * SDL1 surface flag with no SDL3 meaning. Both are now no-op bits. */
#ifndef SDL_WINDOW_SHOWN
#define SDL_WINDOW_SHOWN 0
#endif
#ifndef SDL_SWSURFACE
#define SDL_SWSURFACE 0
#endif
/* SDL2's SDL_ShowCursor(SDL_QUERY) asked rather than set. */
#ifndef SDL_QUERY
#define SDL_QUERY (-1)
#endif

/* ---------------------------------------------------------------------- */
/* Calls whose SHAPE changed (arity or return), not just their name        */
/* ---------------------------------------------------------------------- */
/* Each wrapper calls the real SDL3 function through a parenthesised name,
 * (SDL_Foo)(...), so the function-like macro below does not recurse. */

/* SDL2: SDL_CreateRGBSurface(flags,w,h,depth,Rmask,Gmask,Bmask,Amask)
 * SDL3: SDL_CreateSurface(w,h,SDL_PixelFormat) -- the format is looked up from
 *       the bit depth and masks instead of being described by them. */
static inline SDL_Surface *retrodosbox_create_rgb_surface(
        Uint32 flags, int w, int h, int depth,
        Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
    (void)flags;
    return SDL_CreateSurface(w, h,
        SDL_GetPixelFormatForMasks(depth, rmask, gmask, bmask, amask));
}
#define SDL_CreateRGBSurface(f,w,h,d,r,g,b,a) \
    retrodosbox_create_rgb_surface((f),(w),(h),(d),(r),(g),(b),(a))

/* SDL2 enumerated displays and joysticks by dense index; SDL3 hands out an
 * array of stable IDs. These keep the index-based call sites working. */
static inline int retrodosbox_num_video_displays(void)
{
    int n = 0; SDL_DisplayID *d = SDL_GetDisplays(&n); SDL_free(d); return n;
}
#define SDL_GetNumVideoDisplays() retrodosbox_num_video_displays()

static inline SDL_DisplayID retrodosbox_display_id(int index)
{
    int n = 0; SDL_DisplayID id = 0;
    SDL_DisplayID *d = SDL_GetDisplays(&n);
    if (d) { if (index >= 0 && index < n) id = d[index]; SDL_free(d); }
    if (id == 0) id = SDL_GetPrimaryDisplay();
    return id;
}

/* SDL2: int SDL_GetDesktopDisplayMode(int index, SDL_DisplayMode *out)  (0 == ok)
 * SDL3: const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID)
 * Note the return convention flips too: callers testing "!= 0" for failure
 * keep working because we return 0 on success. */
static inline int retrodosbox_desktop_mode(int index, SDL_DisplayMode *out)
{
    const SDL_DisplayMode *m = (SDL_GetDesktopDisplayMode)(retrodosbox_display_id(index));
    if (!m || !out) return -1;
    *out = *m; return 0;
}
#define SDL_GetDesktopDisplayMode(i,o) retrodosbox_desktop_mode((i),(o))

static inline int retrodosbox_current_mode(int index, SDL_DisplayMode *out)
{
    const SDL_DisplayMode *m = (SDL_GetCurrentDisplayMode)(retrodosbox_display_id(index));
    if (!m || !out) return -1;
    *out = *m; return 0;
}
#define SDL_GetCurrentDisplayMode(i,o) retrodosbox_current_mode((i),(o))

static inline int retrodosbox_window_fs_mode(SDL_Window *w, SDL_DisplayMode *out)
{
    const SDL_DisplayMode *m = (SDL_GetWindowFullscreenMode)(w);
    if (!m || !out) return -1;
    *out = *m; return 0;
}
#define SDL_GetWindowFullscreenMode(w,o) retrodosbox_window_fs_mode((w),(o))

static inline int retrodosbox_num_joysticks(void)
{
    int n = 0; SDL_JoystickID *j = SDL_GetJoysticks(&n); SDL_free(j); return n;
}
#define SDL_NumJoysticks() retrodosbox_num_joysticks()

static inline const char *retrodosbox_joystick_name(int index)
{
    int n = 0; const char *name = NULL;
    SDL_JoystickID *j = SDL_GetJoysticks(&n);
    if (j) { if (index >= 0 && index < n) name = SDL_GetJoystickNameForID(j[index]); SDL_free(j); }
    return name;
}
#define SDL_JoystickNameForIndex(i) retrodosbox_joystick_name(i)

/* SDL2's SDL_JoystickEventState(SDL_ENABLE|SDL_DISABLE|SDL_QUERY) split into
 * a setter and a getter in SDL3. */
static inline int retrodosbox_joystick_event_state(int state)
{
    if (state == SDL_QUERY) return SDL_JoystickEventsEnabled() ? 1 : 0;
    SDL_SetJoystickEventsEnabled(state ? true : false);
    return state;
}
#define SDL_JoystickEventState(s) retrodosbox_joystick_event_state(s)

/* ---------------------------------------------------------------------- */
/* Vendored SDL_sound decoders (src/libs/decoders)                         */
/* ---------------------------------------------------------------------- */
/* These live here, not in our SDL_audio.h forwarder: SDL3's own SDL.h
 * includes <SDL3/SDL_audio.h> directly, so the forwarder is never reached by
 * anything that includes SDL.h. */

/* SDL3 dropped the UNSIGNED 16-bit audio formats; only U8 and the signed
 * formats remain. The decoders still name them in conversion tables. SDL2's
 * numeric values are free now, so the branches compile and stay dead --
 * SDL3 can never report a U16 format. */
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

/* SDL2: SDL_RWread(ctx, ptr, size, maxnum) -> number of OBJECTS read.
 * SDL3: SDL_ReadIO(ctx, ptr, size_in_bytes) -> number of BYTES read.
 * SDL_oldnames.h maps the NAME but cannot fix the arity or the unit, so the
 * mapping must be replaced rather than relied upon. */
static inline size_t retrodosbox_rwread(SDL_IOStream *ctx, void *ptr,
                                        size_t size, size_t maxnum)
{
    size_t got;
    if (size == 0 || maxnum == 0) return 0;
    got = SDL_ReadIO(ctx, ptr, size * maxnum);
    return got / size;
}
#undef SDL_RWread
#define SDL_RWread(c,p,s,n) retrodosbox_rwread((c),(p),(size_t)(s),(size_t)(n))

static inline size_t retrodosbox_rwwrite(SDL_IOStream *ctx, const void *ptr,
                                         size_t size, size_t num)
{
    size_t put;
    if (size == 0 || num == 0) return 0;
    put = SDL_WriteIO(ctx, ptr, size * num);
    return put / size;
}
#undef SDL_RWwrite
#define SDL_RWwrite(c,p,s,n) retrodosbox_rwwrite((c),(p),(size_t)(s),(size_t)(n))

/* whence became a typed enum; the decoders pass a plain int. */
#undef SDL_RWseek
#define SDL_RWseek(c,o,w) SDL_SeekIO((c),(o),(SDL_IOWhence)(w))

/* ---------------------------------------------------------------------- */
/* Remaining shape changes                                                 */
/* ---------------------------------------------------------------------- */

/* SDL2: SDL_CreateRGBSurfaceFrom(pixels,w,h,depth,pitch,Rmask,...,Amask)
 * SDL3: SDL_CreateSurfaceFrom(w,h,format,pixels,pitch) -- note the order. */
static inline SDL_Surface *retrodosbox_create_rgb_surface_from(
        void *pixels, int w, int h, int depth, int pitch,
        Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
    return SDL_CreateSurfaceFrom(w, h,
        SDL_GetPixelFormatForMasks(depth, rmask, gmask, bmask, amask),
        pixels, pitch);
}
#define SDL_CreateRGBSurfaceFrom(px,w,h,d,p,r,g,b,a) \
    retrodosbox_create_rgb_surface_from((px),(w),(h),(d),(p),(r),(g),(b),(a))

/* SDL3 added a scale mode argument. SDL_oldnames.h maps the name only. */
#undef SDL_UpperBlitScaled
#define SDL_UpperBlitScaled(s,sr,d,dr) \
    SDL_BlitSurfaceScaled((s),(sr),(d),(dr),SDL_SCALEMODE_NEAREST)

/* SDL2's pause flag became explicit Pause/Resume calls. */
static inline bool retrodosbox_pause_audio_device(SDL_AudioDeviceID dev, int pause_on)
{
    return pause_on ? SDL_PauseAudioDevice(dev) : SDL_ResumeAudioDevice(dev);
}
#define SDL_PauseAudioDevice(d,p) retrodosbox_pause_audio_device((d),(p))

/* SDL3 added an out-param for the modifier state. */
#define SDL_GetScancodeFromKey(k) (SDL_GetScancodeFromKey)((k), NULL)

/* Removed init flags: SDL3 always has the timer subsystem, and the crash
 * "parachute" concept is gone entirely. */
#ifndef SDL_INIT_TIMER
#define SDL_INIT_TIMER 0
#endif
#ifndef SDL_INIT_NOPARACHUTE
#define SDL_INIT_NOPARACHUTE 0
#endif

/* SDL2: SDL_MapRGB(const SDL_PixelFormat *fmt, r, g, b)
 * SDL3: SDL_MapRGB(const SDL_PixelFormatDetails*, const SDL_Palette*, r, g, b)
 * Call sites pass a surface's format, which is now an enum. NULL palette is
 * correct for the non-indexed surfaces these are used on. */
#define SDL_MapRGB(fmt,r,g,b) \
    (SDL_MapRGB)(SDL_GetPixelFormatDetails(fmt), NULL, (r),(g),(b))
#define SDL_MapRGBA(fmt,r,g,b,a) \
    (SDL_MapRGBA)(SDL_GetPixelFormatDetails(fmt), NULL, (r),(g),(b),(a))
#define SDL_GetRGB(pix,fmt,r,g,b) \
    (SDL_GetRGB)((pix), SDL_GetPixelFormatDetails(fmt), NULL, (r),(g),(b))
#define SDL_GetRGBA(pix,fmt,r,g,b,a) \
    (SDL_GetRGBA)((pix), SDL_GetPixelFormatDetails(fmt), NULL, (r),(g),(b),(a))
/* NOTE: these four take the surface's FORMAT ENUM, which is what almost every
 * call site passes. Code that already holds a const SDL_PixelFormatDetails*
 * must bypass the macro with the parenthesised form, e.g.
 *     (SDL_MapRGBA)(details, NULL, r,g,b,a)
 * -- see gui_tk.cpp, which caches the details pointer across a blit loop. */

/* Same scale-mode argument as SDL_UpperBlitScaled; oldnames maps the name only. */
#undef SDL_BlitScaled
#define SDL_BlitScaled(s,sr,d,dr) \
    SDL_BlitSurfaceScaled((s),(sr),(d),(dr),SDL_SCALEMODE_NEAREST)

/* SDL2: SDL_GetVersion(SDL_version *out), a struct of major/minor/patch.
 * SDL3: int SDL_GetVersion(void), a packed number. */
typedef struct SDL_version { Uint8 major, minor, patch; } SDL_version;
static inline void retrodosbox_get_version(SDL_version *v)
{
    const int n = (SDL_GetVersion)();
    v->major = (Uint8)SDL_VERSIONNUM_MAJOR(n);
    v->minor = (Uint8)SDL_VERSIONNUM_MINOR(n);
    v->patch = (Uint8)SDL_VERSIONNUM_MICRO(n);
}
#define SDL_GetVersion(v) retrodosbox_get_version(v)

/* SDL3 dropped warp-based relative mouse mode; centering is the survivor. */
#ifndef SDL_HINT_MOUSE_RELATIVE_MODE_WARP
#define SDL_HINT_MOUSE_RELATIVE_MODE_WARP SDL_HINT_MOUSE_RELATIVE_MODE_CENTER
#endif

#endif /* RETRODOSBOX_SDL3COMPAT_SDL_H */

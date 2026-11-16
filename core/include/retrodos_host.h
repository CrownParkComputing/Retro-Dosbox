/*
 * retro-dosbox — plain-C host API over the DOSBox-X core.
 *
 * This is the only door between the core and any host UI (an SDL3+ImGui
 * frontend, an Android app, a test harness). New capability = new export
 * here, never a platform-specific side channel.
 *
 * Deliberately shaped like retro-x86's retro86_host.h so the two cores in the
 * family present the same surface. Where it DIFFERS, it differs because DOS
 * differs from a PC-in-a-box:
 *
 *   - 86Box inserts media into numbered drive bays. DOS mounts a HOST FOLDER
 *     as a drive letter, which is how nearly every DOS game is actually
 *     launched, so mount_dir() has no 86Box equivalent.
 *   - DOSBox-X's launch channel is a generated .conf file, not a list of
 *     arguments: mounts, machine type, cycles and the [autoexec] that runs the
 *     program all live there. run() therefore takes an argv carrying -conf.
 *   - DOS video modes are frequently non-square-pixel (320x200 is a 4:3
 *     picture, not 16:10), so the host must be told the aspect ratio. 86Box
 *     has no such need.
 *   - send_command() exists because after boot the DOS shell is a real
 *     interface; 86Box has no shell to talk to.
 *
 * THREADING
 * ---------
 * The DOSBox-X mainloop owns essentially all emulator state and is not
 * thread-safe. run() occupies whichever thread calls it. Every other function
 * here is safe to call from another thread: each is either an atomic flag the
 * mainloop reads, or a request the mainloop drains at a safe point. None of
 * them calls into the emulator directly.
 */
#ifndef RETRODOS_HOST_H
#define RETRODOS_HOST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#    define RETRODOS_HOST_API __declspec(dllexport)
#else
#    define RETRODOS_HOST_API __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Run the emulator with the given command line (the arguments DOSBox-X
 * accepts: -conf, -defaultdir, -c ...). Blocks until retrodos_host_quit()
 * ends the session. Returns the session's exit code.
 *
 * The .conf handed via -conf is the ONLY channel by which a title is
 * launched. There is deliberately no "start this game" argument here.
 *
 * NOTE: a conf generated for a non-interactive host MUST contain
 *   [dosbox]
 *   working directory option=noprompt
 * Without it, DOSBox-X sees a non-TTY stdin, decides to prompt for a working
 * directory, and blocks forever in a folder-picker dialog that no embedded
 * host can answer. */
RETRODOS_HOST_API int retrodos_host_run(int argc, char **argv);

/* Ask the running session to end. Safe from any thread; the shutdown happens
 * on the emulator thread, which then returns from run(). */
RETRODOS_HOST_API void retrodos_host_quit(void);

/* Pause/resume emulation. Paused means no CPU emulation and no new frames,
 * but the framebuffer keeps its last contents so the host can still draw. */
RETRODOS_HOST_API void retrodos_host_set_pause(bool paused);

/* Reboot the machine. hard=true is a power-cycle, false a soft reset. */
RETRODOS_HOST_API void retrodos_host_reset(bool hard);

/* 1 while the mainloop is running, 0 otherwise. */
RETRODOS_HOST_API bool retrodos_host_is_running(void);

/* Hand the engine the host's SDL_Window (as void* so this header stays free of
 * SDL types). Call BEFORE run().
 *
 * Game Link renders offscreen, but the engine was never designed to run with
 * NO window at all -- on desktop it creates one regardless and later code
 * assumes sdl.window is valid. Suppressing creation therefore just moves the
 * failure to the first dereference. On a platform that allows exactly one
 * window (Android), and where the host already owns it, the honest answer is
 * to let the engine adopt the host's rather than ask for a second. */
RETRODOS_HOST_API void retrodos_host_set_window(void *sdl_window);

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* Keyboard, by SDL scancode (SDL_SCANCODE_*). Scancodes rather than
 * characters on purpose: DOS games read the keyboard at the scancode level,
 * and a character API cannot express a held arrow key or tell the two Alt
 * keys apart. Injected through the same mapper path a real key takes, so
 * DOSBox-X's key mapper and every DOS program see a normal key.
 *
 * (86Box takes XT set-1 codes here; DOSBox-X's mapper is SDL-scancode based,
 * so translating in the host would only add a lossy hop.) */
RETRODOS_HOST_API void retrodos_host_send_key(int sdl_scancode, bool pressed);

/* Relative mouse motion, in emulated-screen pixels. */
RETRODOS_HOST_API void retrodos_host_mouse_move(int dx, int dy);

/* Absolute position as a fraction of the frame, x1000 (0..1000). For
 * touch-to-point, where there is no relative motion to report. The emulated
 * mouse is relative, so the core converts. */
RETRODOS_HOST_API void retrodos_host_mouse_position(int x_per_mille, int y_per_mille);

/* button: 0 left, 1 right, 2 middle. */
RETRODOS_HOST_API void retrodos_host_mouse_button(int button, bool pressed);

/* Vertical wheel delta (+1 per notch up, -1 per notch down). */
RETRODOS_HOST_API void retrodos_host_mouse_wheel(int dz);

/* Emulated PC joystick. port 0 or 1; mask is RETRODOS_JOY_*; axes -1000..1000
 * for games that read a proportional stick rather than digital directions. */
#define RETRODOS_JOY_UP      0x01
#define RETRODOS_JOY_DOWN    0x02
#define RETRODOS_JOY_LEFT    0x04
#define RETRODOS_JOY_RIGHT   0x08
#define RETRODOS_JOY_BUTTON1 0x10
#define RETRODOS_JOY_BUTTON2 0x20
#define RETRODOS_JOY_BUTTON3 0x40
#define RETRODOS_JOY_BUTTON4 0x80
RETRODOS_HOST_API void retrodos_host_joystick(int port, int mask, int axis_x, int axis_y);

/* ------------------------------------------------------------------ */
/* Framebuffer tap                                                     */
/* ------------------------------------------------------------------ */

/* Enable the framebuffer tap. Disabled by default (zero overhead). */
RETRODOS_HOST_API void retrodos_host_set_framebuffer_output(bool enabled);

/* Copy the current frame as packed 0xXXRRGGBB pixels into dst. capacity is in
 * uint32_t elements. Returns 1 when a frame was copied, 0 when none is
 * available or dst is too small. out_* may be NULL.
 *
 * A copy rather than a borrowed pointer because the emulator thread rewrites
 * the buffer in place; anything handed to a rasterizer must be owned memory. */
RETRODOS_HOST_API int retrodos_host_copy_framebuffer(uint32_t *dst, int capacity,
                                                     int *out_w, int *out_h,
                                                     uint64_t *out_serial);

/* Monotonic counter, incremented once per completed frame. Poll it to skip
 * re-uploading an unchanged frame -- DOS text mode can sit on an identical
 * picture for seconds. */
RETRODOS_HOST_API uint64_t retrodos_host_framebuffer_serial(void);

/* Current framebuffer dimensions (0x0 before the first frame). */
RETRODOS_HOST_API void retrodos_host_framebuffer_size(int *out_w, int *out_h);

/* The aspect ratio DOSBox-X says this frame should be displayed at, x1000
 * (1200 means 1.2). 0 if unknown. Needed because DOS modes are frequently
 * non-square-pixel: 320x200 is a 4:3 display, not 16:10. */
RETRODOS_HOST_API int retrodos_host_pixel_aspect_x1000(void);

/* ------------------------------------------------------------------ */
/* Media and the shell                                                 */
/* ------------------------------------------------------------------ */

/* Mount a host folder as a DOS drive ("mount C <path>"). This is how the vast
 * majority of DOS games are launched and has no 86Box equivalent.
 * drive is 'A'..'Z'. Returns 0 on success. */
RETRODOS_HOST_API int retrodos_host_mount_dir(char drive, const char *host_path);

/* Mount a CD image (ISO/CUE/BIN) as a DOS drive ("imgmount D <iso> -t iso"). */
RETRODOS_HOST_API int retrodos_host_mount_cd(char drive, const char *image_path);

/* Unmount a drive letter. */
RETRODOS_HOST_API int retrodos_host_unmount(char drive);

/* Type a line into the DOS shell as if the user had typed it, then Enter.
 * Goes through the same keyboard buffer as real typing. */
RETRODOS_HOST_API int retrodos_host_send_command(const char *line);

/* ---- Changing a disc in a machine that is already running ----
 *
 * The three functions above type at the DOS shell, which means they work only
 * while a DOS shell is what is running. Once a guest OS has been booted --
 * Windows 98 from a hard disk image, say -- the shell is gone and typing
 * "imgmount" simply sends those letters to Windows.
 *
 * These two do the same thing DOSBox-X's own "change CD image" and "change
 * floppy image" menu items do, which is to change the media behind the
 * emulated drive and tell the guest that it changed. They work whether or not
 * a guest OS is booted, which is the entire point of them.
 *
 * Both take a HOST path, and both return 0 on success. Failure means the
 * image could not be opened, or that there is no drive of that kind to put it
 * in -- a machine booted with no CD-ROM attached cannot grow one, because the
 * guest's own drivers were loaded for the hardware it found at boot. */

/* Put a CD image (ISO/CUE/BIN/CHD...) into the drive at [drive]. Passing NULL
 * or an empty path opens the drive and leaves it empty. */
RETRODOS_HOST_API int retrodos_host_insert_cd(char drive, const char *image_path);

/* Put a floppy image into drive 'A' or 'B'. As above, an empty path ejects. */
RETRODOS_HOST_API int retrodos_host_insert_floppy(char drive, const char *image_path);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

/* Name of the DOS program currently executing (DOSBox-X's RunningProgram),
 * e.g. "DOOM". Writes at most buf_len bytes, always NUL-terminated, and
 * returns the length the full answer would have needed. */
RETRODOS_HOST_API int retrodos_host_running_program(char *buf, int buf_len);

/* Measured frames per second of the render callback. */
RETRODOS_HOST_API int retrodos_host_fps(void);

/* Emulated CPU speed in cycles, as the status line reports it. */
RETRODOS_HOST_API int retrodos_host_cycles(void);

#ifdef __cplusplus
}
#endif

#endif /* RETRODOS_HOST_H */

/*
 * DOSDeck — the plain-C door between the Swift app and the C++ underneath.
 *
 * Swift sees THIS and nothing else. retrodos_host.h is plain C already and
 * could in principle be imported directly, but the app also needs the conf
 * builder and the Windows machine logic, and both of those are C++ with
 * std::string in their signatures. Rather than teach Swift about two headers
 * and a C++ interop mode, everything the app needs arrives here flattened:
 * fixed-size structs, char buffers, int returns.
 *
 * WHY THERE IS A SWIFT APP AT ALL
 * -------------------------------
 * The interface used to be frontend/retrodos_frontend.cpp -- SDL3 and Dear
 * ImGui -- and that same file is the interface of the Android app and of
 * every other Retro-* title. Apple rejected the family under guideline 4.3:
 * several apps from one developer, too alike. A rename does not answer that
 * and neither does new artwork over the same screens. What answers it is a
 * different app: DOSDeck's interface is written here, in Swift, for iOS, and
 * shares not one screen with anything else we ship. The GPL engine underneath
 * is shared, credited, and unmodified.
 *
 * THREADING
 * ---------
 * dd_start() puts the DOSBox-X mainloop on a thread of its own and returns
 * immediately. Everything else here is safe to call from the main thread:
 * each call is either an atomic read or a request the mainloop drains at a
 * safe point. Nothing here blocks on the emulator.
 */
#ifndef DOSDECK_BRIDGE_H
#define DOSDECK_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/*
 * Start the engine on its own thread with the conf at conf_path.
 *
 * Returns false if an engine has already been started in this process, which
 * is a hard limit rather than a policy: DOSBox-X initialises its globals once
 * and tears them down asymmetrically, so a second dosbox_x_main() boots
 * straight into a triple fault. Android works around it by restarting its own
 * process; iOS has no such move -- an app that calls exit() is a crash report
 * -- so the app is built so that it never needs to. One session runs for the
 * life of the launch, and everything that looks like "start something else"
 * is done inside it: dd_mount_dir() plus dd_type() to run another title,
 * dd_insert_cd() to change a disc, and dd_write_phase_batch() plus
 * dd_reset() to move a Windows installation to its next step.
 */
bool dd_start(const char *conf_path, const char *workdir);

/* True once dd_start() has succeeded, for the life of the process. The front
 * door asks, because "Windows or DOS" can only be answered once per launch. */
bool dd_has_started(void);

/* True while the mainloop is actually running. False before the first frame
 * and after the session ends. */
bool dd_is_running(void);

/* End the session. The engine thread returns from its mainloop; the process
 * stays alive, and no further session can be started in it. */
void dd_quit(void);

/* No CPU emulation and no new frames, but the last picture is kept. Used when
 * the app leaves the foreground -- iOS will kill a backgrounded app that
 * keeps burning CPU, and a paused emulator is also a correctly paused game. */
void dd_set_pause(bool paused);

/* Reset the machine. hard=true is a power cycle, and a power cycle re-runs
 * [autoexec] -- which is what moves a Windows installation from one phase to
 * the next without a second dd_start(). Measured, not assumed. */
void dd_reset(bool hard);

/* ================================================================== */
/* The picture                                                         */
/* ================================================================== */

/* Incremented once per completed frame. DOS text mode can sit on the same
 * picture for seconds, so the renderer polls this and re-uploads nothing when
 * it has not moved. */
uint64_t dd_frame_serial(void);

/* Current size, 0x0 before the first frame. */
void dd_frame_size(int *out_w, int *out_h);

/* Copy the current frame as packed 0xXXRRGGBB into dst (capacity in uint32_t
 * elements). 1 on success, 0 when there is no frame or dst is too small. */
int dd_copy_frame(uint32_t *dst, int capacity, int *out_w, int *out_h,
                  uint64_t *out_serial);

/*
 * The PIXEL aspect ratio of this frame, x1000. 0 when the engine has not said.
 *
 * This is how far a row must be stretched VERTICALLY, not the shape of the
 * finished picture: 720x400 reports 1350 because 400 x 1.35 is 540 and 720x540
 * is the 4:3 the mode should be seen at, and 320x200 reports 1200 for the same
 * reason. The display ratio is therefore width / (height x par).
 *
 * Worth being exact about, because DOS modes are routinely non-square-pixel
 * and getting this wrong is the classic emulator tell.
 */
int dd_pixel_aspect_x1000(void);

/* ================================================================== */
/* Input                                                               */
/* ================================================================== */

/* By SDL scancode: DOS games read the keyboard at the scancode level, and a
 * character API cannot express a held arrow key. The DD_SCAN_* values below
 * are those scancodes, repeated here so Swift needs no SDL header. */
void dd_key(int scancode, bool pressed);
void dd_mouse_move(int dx, int dy);
void dd_mouse_button(int button, bool pressed);   /* 0 left 1 right 2 middle */
void dd_mouse_wheel(int dz);
void dd_joystick(int port, int mask, int axis_x, int axis_y);

#define DD_JOY_UP      0x01
#define DD_JOY_DOWN    0x02
#define DD_JOY_LEFT    0x04
#define DD_JOY_RIGHT   0x08
#define DD_JOY_BUTTON1 0x10
#define DD_JOY_BUTTON2 0x20
#define DD_JOY_BUTTON3 0x40
#define DD_JOY_BUTTON4 0x80

/* SDL scancodes, the ones a DOS keyboard has. */
enum {
    DD_SCAN_A = 4,  DD_SCAN_B = 5,  DD_SCAN_C = 6,  DD_SCAN_D = 7,
    DD_SCAN_E = 8,  DD_SCAN_F = 9,  DD_SCAN_G = 10, DD_SCAN_H = 11,
    DD_SCAN_I = 12, DD_SCAN_J = 13, DD_SCAN_K = 14, DD_SCAN_L = 15,
    DD_SCAN_M = 16, DD_SCAN_N = 17, DD_SCAN_O = 18, DD_SCAN_P = 19,
    DD_SCAN_Q = 20, DD_SCAN_R = 21, DD_SCAN_S = 22, DD_SCAN_T = 23,
    DD_SCAN_U = 24, DD_SCAN_V = 25, DD_SCAN_W = 26, DD_SCAN_X = 27,
    DD_SCAN_Y = 28, DD_SCAN_Z = 29,
    DD_SCAN_1 = 30, DD_SCAN_2 = 31, DD_SCAN_3 = 32, DD_SCAN_4 = 33,
    DD_SCAN_5 = 34, DD_SCAN_6 = 35, DD_SCAN_7 = 36, DD_SCAN_8 = 37,
    DD_SCAN_9 = 38, DD_SCAN_0 = 39,
    DD_SCAN_RETURN = 40, DD_SCAN_ESCAPE = 41, DD_SCAN_BACKSPACE = 42,
    DD_SCAN_TAB = 43, DD_SCAN_SPACE = 44, DD_SCAN_MINUS = 45,
    DD_SCAN_EQUALS = 46, DD_SCAN_LEFTBRACKET = 47, DD_SCAN_RIGHTBRACKET = 48,
    DD_SCAN_BACKSLASH = 49, DD_SCAN_SEMICOLON = 51, DD_SCAN_APOSTROPHE = 52,
    DD_SCAN_GRAVE = 53, DD_SCAN_COMMA = 54, DD_SCAN_PERIOD = 55,
    DD_SCAN_SLASH = 56, DD_SCAN_CAPSLOCK = 57,
    DD_SCAN_F1 = 58, DD_SCAN_F2 = 59, DD_SCAN_F3 = 60, DD_SCAN_F4 = 61,
    DD_SCAN_F5 = 62, DD_SCAN_F6 = 63, DD_SCAN_F7 = 64, DD_SCAN_F8 = 65,
    DD_SCAN_F9 = 66, DD_SCAN_F10 = 67, DD_SCAN_F11 = 68, DD_SCAN_F12 = 69,
    DD_SCAN_PAUSE = 72, DD_SCAN_INSERT = 73, DD_SCAN_HOME = 74,
    DD_SCAN_PAGEUP = 75, DD_SCAN_DELETE = 76, DD_SCAN_END = 77,
    DD_SCAN_PAGEDOWN = 78, DD_SCAN_RIGHT = 79, DD_SCAN_LEFT = 80,
    DD_SCAN_DOWN = 81, DD_SCAN_UP = 82,
    DD_SCAN_KP_DIVIDE = 84, DD_SCAN_KP_MULTIPLY = 85, DD_SCAN_KP_MINUS = 86,
    DD_SCAN_KP_PLUS = 87, DD_SCAN_KP_ENTER = 88,
    DD_SCAN_LCTRL = 224, DD_SCAN_LSHIFT = 225, DD_SCAN_LALT = 226,
    DD_SCAN_RCTRL = 228, DD_SCAN_RSHIFT = 229, DD_SCAN_RALT = 230,
};

/* ================================================================== */
/* Media and the shell, inside a running session                       */
/* ================================================================== */

/* Mount a host folder as a DOS drive. This is how a second title is started
 * without a second session: mount its folder, then dd_type() the program. */
int dd_mount_dir(char drive, const char *host_path);
int dd_unmount(char drive);

/*
 * Type a line at the DOS prompt, as if the user had typed it, then Enter.
 *
 * Only meaningful while a DOS shell is what is running. Once a guest OS has
 * been booted the shell is gone and this types into Windows instead.
 */
int dd_type(const char *line);

/*
 * Change the disc in a drive that already exists, which is what the Windows
 * session's CD-ROM list does. Works whether or not a guest OS is booted --
 * unlike dd_type("IMGMOUNT ..."), which needs a DOS prompt to type at.
 *
 * An empty or NULL path opens the drive and leaves it empty. Fails when there
 * is no drive of that kind to put it in: a machine booted without a CD-ROM
 * cannot grow one, because the guest loaded drivers for the hardware it found
 * at boot.
 */
int dd_insert_cd(char drive, const char *image_path);
int dd_insert_floppy(char drive, const char *image_path);

/* ================================================================== */
/* Status                                                              */
/* ================================================================== */

/* The DOS program executing right now ("DOOM"), or "COMMAND" at a prompt. */
int dd_running_program(char *buf, int buf_len);
int dd_fps(void);
int dd_cycles(void);

/* ================================================================== */
/* Confs                                                               */
/* ================================================================== */

/*
 * The DOS machine, flattened from retrodos::Settings.
 *
 * Every field is here because getting it wrong stops real titles running, and
 * the DOS wizard exposes all of them -- that is what "DOSBox-X mode" means.
 * Strings are fixed buffers so Swift can hold the struct by value.
 */
typedef struct {
    int  cycles_max;        /* fixed cycles are what save the early titles   */
    int  cycles_fixed;
    int  core_fast;         /* writes core=auto; iOS has no JIT, so auto is
                             * the interpreter there and this changes nothing
                             * -- kept so one struct serves both platforms    */
    int  memsize;           /* MB; 32 suits DOS/4GW 1.97 and its contemporaries */
    int  vmemsize;          /* MB of video memory; 0 = let DOSBox-X decide    */
    int  fpu;
    int  ems;
    int  umb;
    int  pcspeaker;
    int  aspect_mode;       /* 0 auto, 1 4:3, 2 16:9, 3 fill                  */
    int  integer_scale;
    int  joystick;          /* emulate a game port at all                     */
    char machine[32];       /* svga_s3, vgaonly, ega, cga, tandy, hercules... */
    char cputype[32];       /* auto, 386, 486, pentium...                     */
    char sbtype[32];        /* sbpro2, sb16, sb1, gb, none                    */
    char dos_ver[16];       /* "" = DOSBox-X's own 5.0                        */
} DDMachine;

/* The defaults, filled in so Swift never has to repeat them. */
void dd_machine_defaults(DDMachine *out);

/*
 * Write the conf for a DOS session to out_path. Returns 0 on success.
 *
 *   mount_dir    mounted as C:. Empty mounts nothing.
 *   run_cmd      run from [autoexec]. Empty leaves a DOS prompt, which is
 *                what the wizard does when nothing is chosen -- "dosbox-x
 *                mode" in the plainest sense.
 *   cd_image     an ISO/CUE put in E: at boot. Empty for none.
 *   boot_floppy  a floppy image put in A: and BOOTED, for a disk that carries
 *                its own operating system -- the bundled FreeDOS floppy, say.
 *                When this is set, run_cmd is not used: the machine is handed
 *                to whatever is on the disk and DOSBox-X's own DOS is gone.
 *
 * WHY THE CD HAS TO BE CHOSEN NOW
 * A drive that is not there at boot cannot be added later: the guest loaded
 * drivers for the hardware it found. dd_insert_cd() can CHANGE the disc in E:
 * for the rest of the session, but only if there was a disc in it here.
 */
int dd_write_dos_conf(const DDMachine *m, const char *title,
                      const char *mount_dir, const char *run_cmd,
                      const char *cd_image, const char *boot_floppy,
                      const char *out_path);

/* ================================================================== */
/* The Windows machine                                                 */
/* ================================================================== */

enum { DD_OS_WIN98 = 0, DD_OS_FREEDOS = 1 };

/* Where an installation has got to. The phases exist because each needs a
 * different boot, not to drive a progress bar. */
enum {
    DD_PHASE_CREATE = 0,   /* no disk image yet                             */
    DD_PHASE_INSTALL,      /* image exists and is empty: boot the CD        */
    DD_PHASE_CONTINUE,     /* Setup has rebooted: boot the disk, CD still in */
    DD_PHASE_RUN,          /* installed: boot the disk                      */
};

typedef struct {
    char dir[1024];        /* the machine's own folder                      */
    char iso[1024];        /* the user's CD image. Never ours: no operating
                            * system is bundled, downloaded or linked to    */
    char hdd[64];          /* "hdd.img"                                     */
    int  os;               /* DD_OS_*                                       */
    int  size_mb;          /* 0 = the 8GB template from the DOSBox-X guide  */
    int  memsize;          /* 128 is the guide's default; 9x tops out at 512 */
    int  phase;            /* DD_PHASE_*                                    */
    int  voodoo;           /* a 3dfx Voodoo on the PCI bus. Expensive, and
                            * without it no 3D game of the era runs at all  */
} DDWinMachine;

void dd_win_defaults(DDWinMachine *out, const char *dir, int os);

/* Read the machine's state file, reconciled against what is actually on disk
 * -- so .phase is the truth about this machine, not what was written down. */
bool dd_win_load(const char *dir, DDWinMachine *out);
bool dd_win_save(const DDWinMachine *m);

/* The phase the FILES support, whatever the state file claims. */
int dd_win_true_phase(const DDWinMachine *m);

/* True when \WINDOWS\WIN.COM is in the disk image -- read from the image, so
 * it is a fact and not a recorded claim. This is what says the install has
 * got far enough to boot the disk instead of the CD. */
bool dd_win_installed(const DDWinMachine *m);

/* What the user still has to supply before this phase can run, or "" when it
 * is ready. Writes at most buf_len bytes. */
int dd_win_blocker(const DDWinMachine *m, char *buf, int buf_len);

/* Back to the beginning. erase_disk also removes the disk image, which is
 * what starting the installation again means. The chosen CD is kept. */
bool dd_win_reset(DDWinMachine *m, bool erase_disk);

/* "Windows 98" / "FreeDOS", and the phase in words, for the walkthrough. */
const char *dd_os_name(int os);
const char *dd_os_folder(int os);
const char *dd_phase_name(int phase);

/*
 * The conf for a Windows machine whose phases the HOST drives.
 *
 * Different from the desktop conf in exactly one respect, and it is the whole
 * reason this exists: [autoexec] does not contain the phase's commands. It
 * mounts the machine folder as Y: and calls GO.BAT from it, and the app
 * rewrites GO.BAT. A power cycle re-runs [autoexec], so the next phase starts
 * in the SAME session -- which is the only way iOS can run an installation
 * that reboots, since the process cannot restart and the engine cannot run
 * twice.
 *
 * Y:, never Z:. Z: is DOSBox-X's own virtual drive, and mounting over it
 * takes the built-in commands away and leaves the machine rebooting itself.
 */
int dd_write_win_conf(const DDWinMachine *m, const char *out_path);

/* The batch file the current phase needs -- the commands that used to be
 * [autoexec]. Written into the machine folder as GO.BAT, where the conf above
 * calls it. Rewrite it and dd_reset(true) to move to the next phase. */
int dd_write_phase_batch(const DDWinMachine *m, const char *out_path);

/* True when a disk or floppy image holds something bootable (IO.SYS,
 * KERNEL.SYS, or PC/DR DOS boot files). Decides whether a chosen image is
 * booted or simply mounted. */
bool dd_image_has_os(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DOSDECK_BRIDGE_H */

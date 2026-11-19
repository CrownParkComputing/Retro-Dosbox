/*
 * retro-dosbox — plain-C host API over the DOSBox-X core.
 *
 * Implementation notes:
 *
 *  - retrodos_host_run() enters DOSBox-X's main(). It returns after the
 *    engine's own teardown, so a host may run several sessions in one
 *    process. (Upstream has historically been treated as a one-shot core;
 *    the asymmetric-teardown bugs that made it so are fixed on this branch.)
 *
 *  - Nothing here calls into the emulator from the caller's thread. Requests
 *    are queued and drained by retrodos_host_pump(), which sdlmain.cpp calls
 *    from GFX_Events().
 *
 *    THE DRAIN POINT MATTERS. The obvious place to drain is the frame
 *    boundary, but DOSBox-X only publishes a frame when the picture CHANGES.
 *    A DOS program effectively always draws something, so that looks fine --
 *    until a guest sits still, at which point the queue stops being drained
 *    entirely: no keys, no mouse, no pause, no quit. And since input is
 *    precisely what would have changed the picture, the machine can never be
 *    woken again. GFX_Events() runs every tick whether or not anything was
 *    drawn, which is why the pump lives there.
 *
 *  - Input uses the same core entry points the Game Link output uses
 *    (MAPPER_CheckEvent with a synthesised SDL_Event, Mouse_CursorMoved,
 *    Mouse_Button*), so DOSBox-X's key mapper -- and therefore every DOS
 *    program -- sees an ordinary key.
 *
 *  - The framebuffer tap reads the Game Link framebuffer, which is a plain
 *    malloc'd 32-bit buffer (0xAARRGGBB little-endian). We are not writing a
 *    renderer, we are re-pointing an existing, upstream-maintained one.
 */
#include "config.h"

#include "retrodos_host.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdio>

#include "SDL.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "dosbox.h"
#include "video.h"
#include "mouse.h"
#include "mapper.h"
#include "joystick.h"
#include "render.h"
#include "dos_inc.h"
#include "bios_disk.h"
#include "../dos/drives.h"

/* DOSBox-X entry point, exported by sdlmain.cpp for embedders. */
extern "C" int dosbox_x_main(int argc, char *argv[]);

/* Owned by the engine. */
extern std::string RunningProgram;

void MAPPER_AutoType(std::vector<std::string> &sequence, const uint32_t wait_ms,
                     const uint32_t pace_ms, bool choice);
void MAPPER_CheckEvent(SDL_Event *event);

/*
 * Typing a line into the guest, one character at a time.
 *
 * This replaced MAPPER_AutoType, which this file used to call with the whole
 * line as a single element -- and that vector is a list of MAPPER BUTTON
 * NAMES ("a", "space", "enter"), not text. Every call therefore ended in
 *
 *   MAPPER: Couldn't find a button named 'ECHO hi >> C:\RAN.TXT', stopping.
 *
 * and typed nothing at all, which silently broke send_command() and the three
 * mount helpers built on it.
 *
 * Characters go into the BIOS keyboard buffer, which is where a real key
 * lands and what COMMAND.COM reads. DOSBox-X's own clipboard paste does the
 * same thing -- but it is NOT reused here, for the reason the drain explains:
 * it consumes a character whether or not the buffer had room for it.
 */
bool BIOS_AddKeyToBuffer(uint16_t code);

/* What is still to be typed, and how long to wait before typing more of it.
 * Written and drained on the emulator thread only: Request::Command arrives
 * through the pump, which is already there. */
std::string g_typing;
int         g_type_cooldown = 0;

/* Defined in sdlmain.cpp. See the note in the MouseMove case below. */
extern "C++" { extern bool user_cursor_locked; }

/* True while a guest OS owns the machine, i.e. there is no DOS shell.
 *
 * These three, and the swap list below, are declared here rather than pulled
 * from a header because that is how the rest of the engine reaches them --
 * and they must be at file scope: inside the anonymous namespace below they
 * would quietly become separate, undefined internal symbols. */
extern bool    dos_kernel_disabled;
extern int32_t swapPosition;
extern int     swapInDisksSpecificDrive;

namespace {

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

std::atomic<bool>     g_running{false};
std::atomic<bool>     g_paused{false};
std::atomic<bool>     g_fb_enabled{false};
std::atomic<uint64_t> g_fb_serial{0};
std::atomic<int>      g_fb_w{0};
std::atomic<int>      g_fb_h{0};
std::atomic<int>      g_aspect_x1000{0};

/* The published frame, copied out of the engine's buffer while the engine
 * thread is inside the publish hook and therefore not rewriting it. */
std::mutex            g_fb_mutex;
std::vector<uint32_t> g_fb;
int                   g_fb_pitch_px = 0;

struct Request {
    enum Kind {
        Quit, Reset, Key, MouseMove, MousePos, MouseButton, MouseWheel,
        Joystick, Command, InsertCd, InsertFloppy
    } kind;
    int         a = 0, b = 0, c = 0, d = 0;
    bool        flag = false;
    std::string text;
};

std::mutex          g_req_mutex;
std::deque<Request> g_requests;

void queue(const Request &r)
{
    std::lock_guard<std::mutex> lock(g_req_mutex);
    /* A host that spams input while the engine is wedged must not grow this
     * without bound; dropping the oldest is better than dying. */
    if (g_requests.size() > 4096) g_requests.pop_front();
    g_requests.push_back(r);
}

/* ------------------------------------------------------------------ */
/* Changing a disc under a running machine                             */
/* ------------------------------------------------------------------ */
/*
 * This is DOSBox-X's own "change CD image" / "change floppy image", with the
 * file dialog taken out.
 *
 * MenuBrowseCDImage() and MenuBrowseFDImage() in src/dos/dos_programs.cpp are
 * the originals and are the reference for every decision below -- they are
 * also the only paths in the engine that change media while a guest OS is
 * booted, which is the case this exists for. They cannot simply be called:
 * each opens tinyfd_openFileDialog() to ask for the filename, and an embedded
 * host has nothing to answer that with. The host supplies the path instead.
 *
 * Both run on the emulator thread, from the pump, because they touch Drives[],
 * imageDiskList[] and the IDE controller.
 *
 * The comment upstream leaves at the top of the IMGMOUNT menu path is worth
 * repeating here: "it would be better to have an internal API for mounting ISO
 * images and replacement. Running a command line to run IMGMOUNT is THE
 * primary reason we cannot provide this menu command while running a guest
 * OS." This is that internal API, for the two cases a host actually needs.
 */

/*
 * Is this file something an emulated CD-ROM could read?
 *
 * Needed because the booted-guest path below swaps the filename behind a live
 * isoDrive rather than constructing a new one, and setFileName() does not
 * check. Without this, inserting a path that does not exist reported success,
 * fired a media change, and left the guest staring at a drive with nothing
 * readable in it -- which looks like a broken emulator, not a bad path.
 *
 * A CD001 descriptor at sector 16 is the ISO 9660 signature. Container
 * formats that are not raw ISO (cue sheets, CHD) are accepted on the strength
 * of opening, because their contents are the image loader's business.
 */
bool looks_like_cd_image(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    const size_t dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? std::string() : path.substr(dot + 1);
    for (char &c : ext) c = (char)tolower((unsigned char)c);

    bool ok = true;
    if (ext == "iso" || ext == "bin" || ext == "img") {
        char sig[5] = {0};
        ok = (fseek(f, 16 * 2048 + 1, SEEK_SET) == 0 &&
              fread(sig, 1, 5, f) == 5 &&
              std::memcmp(sig, "CD001", 5) == 0);
    }
    fclose(f);
    return ok;
}

bool change_cd(int drive_letter, const std::string &path)
{
    const int d = drive_letter - 'A';
    if (d < 0 || d >= DOS_DRIVES) return false;

    isoDrive *cdrom = dynamic_cast<isoDrive *>(Drives[d]);
    if (cdrom == NULL) {
        /* No CD-ROM there to put a disc in. Refused rather than created: the
         * guest's drivers bound to the hardware it found when it booted, and a
         * drive that appears afterwards is one Windows will not see anyway. */
        LOG_MSG("retrodos: drive %c is not a CD-ROM", (char)drive_letter);
        return false;
    }

    const bool empty = std::string(Drives[d]->GetInfo() + 9) == "empty";

    if (path.empty()) {
        /* Ejecting is inserting nothing. isoDrive treats a path it cannot open
         * as an empty drive, which is exactly the open-tray state. */
        cdrom->setFileName("");
        DriveManager::ChangeDisk(d, cdrom);
        return true;
    }

    if (!looks_like_cd_image(path)) {
        LOG_MSG("retrodos: %s did not open as a CD image", path.c_str());
        return false;
    }

    std::vector<std::string> options;
    int error = -1;
    const uint8_t mediaid = 0xF8;

    if (dos_kernel_disabled && !empty) {
        /* The guest is running and the drive already holds a disc: swap the
         * file behind the existing drive object rather than replacing it.
         * Replacing it would invalidate the pointer the IDE controller holds. */
        cdrom->setFileName(path.c_str());
    } else {
        DOS_Drive *made = new isoDrive((char)drive_letter, path.c_str(),
                                       mediaid, error, options);
        if (error) {
            delete made;
            LOG_MSG("retrodos: %s did not open as a CD image", path.c_str());
            return false;
        }
        cdrom = dynamic_cast<isoDrive *>(made);
        Drives[d] = cdrom;
    }

    /* The part that tells the guest. Without it Windows keeps reading the
     * directory it cached from the previous disc. */
    if (cdrom) DriveManager::ChangeDisk(d, cdrom);
    return true;
}

bool change_floppy(int drive_letter, const std::string &path)
{
    const int d = drive_letter - 'A';
    if (d < 0 || d > 1) return false;   /* A: and B:, as the BIOS has */

    if (path.empty()) {
        /* There is no "no disk" image to load, and an emulated drive with
         * nothing in it is what imageDiskList == NULL already means. */
        if (imageDiskList[d]) {
            imageDiskList[d]->Release();
            imageDiskList[d] = NULL;
            imageDiskChange[d] = true;
        }
        return true;
    }

    std::vector<std::string> options;
    fatDrive *made = new fatDrive(path.c_str(), 0, 0, 0, 0, options);
    if (!made->created_successfully) {
        delete made;
        LOG_MSG("retrodos: %s did not open as a floppy image", path.c_str());
        return false;
    }

    if (dos_kernel_disabled) {
        /* A booted guest reads the floppy through the BIOS disk list, not
         * through a DOS drive -- so that is what has to change, with the
         * change flag set so the BIOS reports a media change to the guest.
         * This is the case this function exists for: Windows enumerated its
         * floppy drive at boot and is reading the hardware, not DOS. */
        if (made->loadedDisk == NULL) { delete made; return false; }
        if (imageDiskList[d]) imageDiskList[d]->Release();
        imageDiskList[d] = made->loadedDisk;
        imageDiskChange[d] = true;

        /* Keep the swap list pointing at what is actually in the drive, or the
         * next Ctrl+F4 swaps back to a disk that is no longer there. */
        if (swapInDisksSpecificDrive == d && diskSwap[swapPosition]) {
            diskSwap[swapPosition]->Release();
            diskSwap[swapPosition] = made->loadedDisk;
            diskSwap[swapPosition]->Addref();
        }
    } else if (Drives[d] != NULL) {
        DriveManager::ChangeDisk(d, made);
    } else {
        /* DOS is running and there is no drive there yet, so this is a mount
         * rather than a disk change. The three calls are what IMGMOUNT's own
         * AddToDriveManager does, in its order: without InitializeDrive the
         * drive letter exists in the table and answers "Invalid drive
         * specification", which is what this did before. */
        DriveManager::AppendDisk(d, made);
        DriveManager::InitializeDrive(d);
        mem_writeb(Real2Phys(dos.tables.mediaid) + (unsigned int)d * dos.tables.dpb_size,
                   0xF0 /* 1.44MB floppy */);
        if (made->loadedDisk) {
            if (imageDiskList[d]) imageDiskList[d]->Release();
            imageDiskList[d] = made->loadedDisk;
            imageDiskList[d]->Addref();
            imageDiskChange[d] = true;
        }
    }
    return true;
}

void inject_key(int scancode, bool pressed)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "retrodos", "inject_key sc=%d down=%d", scancode, (int)pressed);
#endif
    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));
    ev.key.type     = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.scancode = (SDL_Scancode)scancode;
    ev.key.key      = SDLK_UNKNOWN;
    ev.key.mod      = KMOD_NONE;
    ev.key.down     = pressed;
    MAPPER_CheckEvent(&ev);
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Engine-side hooks                                                   */
/* ------------------------------------------------------------------ */

/* Called from OUTPUT_GAMELINK_Transfer() each time a frame is completed. */
extern "C" void retrodos_host_frame_posted(const uint32_t *pixels, int32_t width,
                                           int32_t height, int32_t pitch_bytes,
                                           double ratio)
{
    g_aspect_x1000.store((int)(ratio * 1000.0));

    if (!g_fb_enabled.load() || pixels == nullptr || width <= 0 || height <= 0)
        return;

    const int pitch_px = pitch_bytes / (int)sizeof(uint32_t);
    if (pitch_px < width) return;

    {
        std::lock_guard<std::mutex> lock(g_fb_mutex);
        g_fb.resize((size_t)pitch_px * (size_t)height);
        std::memcpy(g_fb.data(), pixels,
                    (size_t)pitch_px * (size_t)height * sizeof(uint32_t));
        g_fb_pitch_px = pitch_px;
    }

    g_fb_w.store(width);
    g_fb_h.store(height);
    g_fb_serial.fetch_add(1);
}

/* Called from GFX_Events() every tick -- see the note at the top of the file
 * about why this is not the frame boundary. */
extern "C" void retrodos_host_pump(void)
{
    std::deque<Request> batch;
    {
        std::lock_guard<std::mutex> lock(g_req_mutex);
        batch.swap(g_requests);
    }

    for (const Request &r : batch) {
        switch (r.kind) {
        case Request::Quit:
            /* The same kill switch DOSBox-X's own Ctrl+F9 uses: an exception
             * thrown on this thread out of DOSBOX_RunMachine, which makes
             * main() run its full teardown and return. */
            throw 1;

        case Request::Reset:
            throw int(r.flag ? 3 : 2); /* engine's reset codes */

        case Request::Key:
            inject_key(r.a, r.flag);
            break;

        case Request::MouseMove:
            /*
             * Tell the engine the pointer is captured, because for a relative
             * delta it always is.
             *
             * user_cursor_locked is DOSBox-X's "I have grabbed the mouse"
             * flag, and it gates every relative-motion device there is:
             * KEYBOARD_AUX_Event throws the movement away without it, and so
             * do the serial mouse and the INT 33h mickey accumulators. It is
             * set in exactly one place upstream -- sdlmain's own SDL motion
             * handler, from its own capture state -- and that handler never
             * runs here, because the host owns the window. So it was false for
             * ever.
             *
             * The symptom is oddly specific and was hard to read: buttons work
             * and motion does not, because the button fields are assigned
             * outside that guard. A guest OS draws a cursor that clicks where
             * it sits and will not move.
             *
             * A relative delta only exists when the host has the pointer, so
             * asserting it here is simply true rather than a workaround.
             */
            user_cursor_locked = true;
            Mouse_CursorMoved((float)r.a, (float)r.b, 0, 0, true);
            break;

        case Request::MousePos: {
            /* Absolute placement is the opposite case: the pointer is NOT
             * captured, and the engine positions its cursor directly. */
            user_cursor_locked = false;
            /* A PS/2 mouse is relative, so "put it here" is steering, not
             * teleporting. Convert against the current frame size. */
            const int w = g_fb_w.load(), h = g_fb_h.load();
            if (w > 0 && h > 0)
                Mouse_CursorMoved((float)(r.a * w / 1000), (float)(r.b * h / 1000),
                                  0, 0, true);
            break;
        }

        case Request::MouseButton:
            if (r.flag) Mouse_ButtonPressed((uint8_t)r.a);
            else        Mouse_ButtonReleased((uint8_t)r.a);
            break;

        case Request::MouseWheel:
            Mouse_WheelMoved(r.a);
            break;

        case Request::Joystick: {
            /* Drive the emulated game port. There is no physical stick behind
             * this: the frontend synthesises the state from a gamepad or from
             * the on-screen pad, and the guest cannot tell the difference.
             *
             * Enable on first use rather than at startup. A DOS game that
             * probes the port and finds a stick centred at rest behaves
             * differently from one that finds no stick at all, and a title
             * being played with touch controls or the keyboard should see the
             * latter until the player actually uses a stick. */
            const Bitu port = (Bitu)(r.a & 1);
            JOYSTICK_Enable(port, true);

            /* Axes arrive as -1000..1000 so the ABI stays integer-only; the
             * joystick module wants -1.0..1.0. */
            JOYSTICK_Move_X(port, (float)r.c / 1000.0f);
            JOYSTICK_Move_Y(port, (float)r.d / 1000.0f);

            /* Two buttons per port is what the standard game port exposes. */
            for (Bitu b = 0; b < 2; ++b)
                JOYSTICK_Button(port, b, (r.b & (1 << b)) != 0);
            break;
        }

        case Request::Command:
            /* Queued, not typed: the BIOS keyboard buffer holds 16 keys, and
             * a mount command is longer than that. The drain below feeds it
             * at the guest's reading pace, and a second command queued behind
             * the first simply types after it. */
            g_typing += r.text;
            g_typing += '\r';
            break;

        case Request::InsertCd:     change_cd(r.a, r.text);     break;
        case Request::InsertFloppy: change_floppy(r.a, r.text); break;
        }
    }

    /*
     * Type a queued command.
     *
     * Within a line, as fast as the guest will take it: the guest's own
     * keyboard buffer is the pacing, because BIOS_AddKeyToBuffer refuses when
     * its 16 keys are full and the character is then kept for the next tick
     * rather than thrown away.
     *
     * BETWEEN lines, a pause -- and that is the part that was not obvious.
     * DOS discards pending keystrokes when a command finishes, which is
     * type-ahead protection and exactly right for a person at a keyboard: you
     * should not have half a sentence execute because you kept typing while
     * something scrolled. It is fatal to typing ahead, though, and it is
     * fatal SILENTLY. Queuing
     *
     *   C:   /   mount D "<path>"   /   D:   /   GO.BAT
     *
     * put this on the screen:
     *
     *   C:\>C:
     *   C:\>p/claude-1000/.../GAME"      <- "mount D "/tm" eaten
     *   C:\>T                            <- "GO.BA" eaten
     *
     * Every line after the first lost its beginning, which reads as corrupted
     * input and is really a flush. So each Enter starts a cooldown, and
     * nothing is typed until it expires: by then the shell has finished the
     * command, drawn its prompt and is asking for a line again.
     *
     * A tick is an emulated millisecond, so this is about a second. Generous
     * on purpose -- the cost of being wrong is a command silently losing its
     * first few characters, and the cost of waiting is that mounting a drive
     * takes a second longer than it might.
     */
    if (!g_typing.empty()) {
        if (g_type_cooldown > 0) {
            --g_type_cooldown;
        } else {
            const unsigned char ch = (unsigned char)g_typing[0];
            /* Enter as the BIOS reports it: scancode 0x1C, ASCII 0x0D. */
            const uint16_t code = (ch == '\r') ? 0x1C0D : (uint16_t)ch;
            if (BIOS_AddKeyToBuffer(code)) {
                g_typing.erase(0, 1);
                if (ch == '\r') g_type_cooldown = 1000;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* True once a host has started the engine through this API. The engine asks
 * so it can leave SDL alone on teardown -- SDL belongs to whoever created it,
 * and here that is the frontend, not the engine. */
std::atomic<bool> g_embedded{false};

extern "C" bool retrodos_host_embedded(void)
{
    return g_embedded.load();
}

/* The host's window, if it gave us one. Read by sdlmain.cpp at startup. */
static std::atomic<void *> g_host_window{nullptr};

extern "C" void retrodos_host_set_window(void *sdl_window)
{
    g_host_window.store(sdl_window);
}

extern "C" void *retrodos_host_window(void)
{
    return g_host_window.load();
}

extern "C" int retrodos_host_run(int argc, char **argv)
{
    g_embedded.store(true);
    g_running.store(true);
    const int rc = dosbox_x_main(argc, argv);
    g_running.store(false);
    return rc;
}

extern "C" void retrodos_host_quit(void)
{
    Request r; r.kind = Request::Quit; queue(r);
}

extern "C" void retrodos_host_set_pause(bool paused)
{
    g_paused.store(paused);
}

extern "C" void retrodos_host_reset(bool hard)
{
    Request r; r.kind = Request::Reset; r.flag = hard; queue(r);
}

extern "C" bool retrodos_host_is_running(void)
{
    return g_running.load();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

extern "C" void retrodos_host_send_key(int sdl_scancode, bool pressed)
{
    Request r; r.kind = Request::Key; r.a = sdl_scancode; r.flag = pressed; queue(r);
}

extern "C" void retrodos_host_mouse_move(int dx, int dy)
{
    Request r; r.kind = Request::MouseMove; r.a = dx; r.b = dy; queue(r);
}

extern "C" void retrodos_host_mouse_position(int x_per_mille, int y_per_mille)
{
    Request r; r.kind = Request::MousePos; r.a = x_per_mille; r.b = y_per_mille; queue(r);
}

extern "C" void retrodos_host_mouse_button(int button, bool pressed)
{
    Request r; r.kind = Request::MouseButton; r.a = button; r.flag = pressed; queue(r);
}

extern "C" void retrodos_host_mouse_wheel(int dz)
{
    Request r; r.kind = Request::MouseWheel; r.a = dz; queue(r);
}

extern "C" void retrodos_host_joystick(int port, int mask, int axis_x, int axis_y)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "retrodos",
        "host_joystick port=%d mask=%d x=%d y=%d", port, mask, axis_x, axis_y);
#endif
    Request r; r.kind = Request::Joystick;
    r.a = port; r.b = mask; r.c = axis_x; r.d = axis_y; queue(r);
}

/* ------------------------------------------------------------------ */
/* Framebuffer tap                                                     */
/* ------------------------------------------------------------------ */

extern "C" void retrodos_host_set_framebuffer_output(bool enabled)
{
    g_fb_enabled.store(enabled);
}

extern "C" uint64_t retrodos_host_framebuffer_serial(void)
{
    return g_fb_serial.load();
}

extern "C" void retrodos_host_framebuffer_size(int *out_w, int *out_h)
{
    if (out_w) *out_w = g_fb_w.load();
    if (out_h) *out_h = g_fb_h.load();
}

extern "C" int retrodos_host_pixel_aspect_x1000(void)
{
    return g_aspect_x1000.load();
}

extern "C" int retrodos_host_copy_framebuffer(uint32_t *dst, int capacity,
                                              int *out_w, int *out_h,
                                              uint64_t *out_serial)
{
    if (dst == nullptr || capacity <= 0) return 0;

    std::lock_guard<std::mutex> lock(g_fb_mutex);
    const int w = g_fb_w.load(), h = g_fb_h.load();
    if (w <= 0 || h <= 0 || g_fb.empty()) return 0;
    if (capacity < w * h) return 0;

    /* Hand back the VISIBLE rectangle, not the padded allocation: the
     * engine's render pitch is not always width*4, and a host that assumed it
     * was would get a sheared picture. */
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + (size_t)y * (size_t)w,
                    g_fb.data() + (size_t)y * (size_t)g_fb_pitch_px,
                    (size_t)w * sizeof(uint32_t));

    if (out_w)      *out_w = w;
    if (out_h)      *out_h = h;
    if (out_serial) *out_serial = g_fb_serial.load();
    return 1;
}

/* ------------------------------------------------------------------ */
/* Media and the shell                                                 */
/* ------------------------------------------------------------------ */

extern "C" int retrodos_host_mount_dir(char drive, const char *host_path)
{
    if (host_path == nullptr || *host_path == '\0') return -1;
    Request r; r.kind = Request::Command;
    r.text = std::string("mount ") + drive + " \"" + host_path + "\"";
    queue(r);
    return 0;
}

extern "C" int retrodos_host_mount_cd(char drive, const char *image_path)
{
    if (image_path == nullptr || *image_path == '\0') return -1;
    Request r; r.kind = Request::Command;
    r.text = std::string("imgmount ") + drive + " \"" + image_path + "\" -t iso";
    queue(r);
    return 0;
}

extern "C" int retrodos_host_unmount(char drive)
{
    Request r; r.kind = Request::Command;
    r.text = std::string("mount -u ") + drive;
    queue(r);
    return 0;
}

extern "C" int retrodos_host_send_command(const char *line)
{
    if (line == nullptr) return -1;
    Request r; r.kind = Request::Command; r.text = line; queue(r);
    return 0;
}

extern "C" int retrodos_host_insert_cd(char drive, const char *image_path)
{
    if (drive < 'A' || drive > 'Z') return -1;
    Request r; r.kind = Request::InsertCd; r.a = drive;
    r.text = image_path ? image_path : "";
    queue(r);
    return 0;
}

extern "C" int retrodos_host_insert_floppy(char drive, const char *image_path)
{
    if (drive != 'A' && drive != 'B') return -1;
    Request r; r.kind = Request::InsertFloppy; r.a = drive;
    r.text = image_path ? image_path : "";
    queue(r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

extern "C" int retrodos_host_running_program(char *buf, int buf_len)
{
    const std::string prog = RunningProgram;
    const int need = (int)prog.size();
    if (buf && buf_len > 0) {
        const int n = (need < buf_len - 1) ? need : buf_len - 1;
        std::memcpy(buf, prog.data(), (size_t)n);
        buf[n] = '\0';
    }
    return need;
}

extern "C" int retrodos_host_fps(void)
{
    return (int)render.frameskip.max;
}

extern "C" int retrodos_host_cycles(void)
{
    extern int32_t CPU_CycleMax;
    return (int)CPU_CycleMax;
}

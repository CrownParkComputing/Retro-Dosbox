/*
 * DOSDeck — implementation of the bridge. See dosdeck_bridge.h for the shape
 * of it and why it exists.
 *
 * Almost everything here is a pass-through to retrodos_host.h or a flattening
 * of a C++ type into a C struct. The two places that are more than that are
 * dd_start(), which decides how SDL behaves on iOS, and the Windows conf,
 * which is the desktop conf with its [autoexec] replaced.
 */
#include "dosdeck_bridge.h"

#include "retrodos_host.h"
#include "../../frontend/retrodos_config.h"
#include "../../frontend/retrodos_win98.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_started{false};

/* Copy a std::string into a fixed C buffer, always NUL-terminated. */
void put(char *dst, size_t cap, const std::string &s)
{
    if (cap == 0) return;
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

bool write_file(const char *path, const std::string &text)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

retrodos::Settings from_c(const DDMachine *m)
{
    retrodos::Settings s;
    s.cycles_max     = m->cycles_max != 0;
    s.cycles_fixed   = m->cycles_fixed;
    s.core_dynamic   = m->core_fast != 0;
    s.memsize        = m->memsize;
    s.vmemsize       = m->vmemsize;
    s.fpu            = m->fpu != 0;
    s.ems            = m->ems != 0;
    s.umb            = m->umb != 0;
    s.pcspeaker      = m->pcspeaker != 0;
    s.aspect_mode    = m->aspect_mode;
    s.integer_scale  = m->integer_scale != 0;
    s.machine        = m->machine;
    s.cputype        = m->cputype;
    s.sbtype         = m->sbtype;
    s.dos_ver        = m->dos_ver;
    /* The game port exists only when asked for: a DOS game that probes it and
     * finds a stick behaves differently from one that finds nothing, and
     * several then steer on their own from an axis nobody is touching. */
    s.pad_sends_joystick = m->joystick != 0;
    return s;
}

retrodos::Win98Install from_c(const DDWinMachine *m)
{
    retrodos::Win98Install w;
    w.dir     = m->dir;
    w.iso     = m->iso;
    w.hdd     = m->hdd[0] ? m->hdd : "hdd.img";
    w.os      = m->os == DD_OS_FREEDOS ? retrodos::OsKind::FreeDos
                                       : retrodos::OsKind::Win98;
    w.size_mb = m->size_mb;
    w.memsize = m->memsize;
    w.voodoo  = m->voodoo != 0;
    switch (m->phase) {
    case DD_PHASE_INSTALL:  w.phase = retrodos::Win98Phase::Install;  break;
    case DD_PHASE_CONTINUE: w.phase = retrodos::Win98Phase::Continue; break;
    case DD_PHASE_RUN:      w.phase = retrodos::Win98Phase::Run;      break;
    default:                w.phase = retrodos::Win98Phase::Create;   break;
    }
    return w;
}

void to_c(const retrodos::Win98Install &w, DDWinMachine *out)
{
    put(out->dir, sizeof(out->dir), w.dir);
    put(out->iso, sizeof(out->iso), w.iso);
    put(out->hdd, sizeof(out->hdd), w.hdd);
    out->os      = w.os == retrodos::OsKind::FreeDos ? DD_OS_FREEDOS : DD_OS_WIN98;
    out->size_mb = w.size_mb;
    out->memsize = w.memsize;
    out->voodoo  = w.voodoo ? 1 : 0;
    switch (w.phase) {
    case retrodos::Win98Phase::Install:  out->phase = DD_PHASE_INSTALL;  break;
    case retrodos::Win98Phase::Continue: out->phase = DD_PHASE_CONTINUE; break;
    case retrodos::Win98Phase::Run:      out->phase = DD_PHASE_RUN;      break;
    default:                             out->phase = DD_PHASE_CREATE;   break;
    }
}

} /* namespace */

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

extern "C" bool dd_start(const char *conf_path, const char *workdir)
{
    if (conf_path == nullptr) return false;
    if (g_started.exchange(true)) return false;

    /*
     * The engine must not touch UIKit, so it gets SDL's dummy video driver.
     *
     * DOSBox-X calls SDL_Init(SDL_INIT_AUDIO|SDL_INIT_VIDEO|...) from whatever
     * thread runs its mainloop, and here that is not the main thread. On iOS
     * the real video driver is UIKit: initialising it off the main thread
     * means UIScreen and UIApplication calls off the main thread, which is
     * undefined at best. It also assumes SDL owns the application, and here
     * SwiftUI does.
     *
     * Nothing is lost. The engine renders OFFSCREEN through Game Link and the
     * app reads finished frames from the framebuffer tap; it creates no window
     * (set_window(nullptr) below) and presents nothing through SDL. Measured
     * on a desktop build before it was relied on here: with the dummy driver
     * the session starts, runs and delivers frames exactly as with a real one.
     *
     * AUDIO IS UNAFFECTED -- it is a separate subsystem and still CoreAudio.
     */
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);

    const std::string conf = conf_path;
    const std::string dir  = workdir ? workdir : "";

    std::thread([conf, dir]() {
        /* Deliberately NOT lending the engine a window: a borrowed window is
         * one it would reconfigure, present to and destroy. */
        retrodos_host_set_window(nullptr);
        retrodos_host_set_framebuffer_output(true);

        std::vector<char> c(conf.begin(), conf.end()); c.push_back('\0');
        std::vector<char> d(dir.begin(), dir.end());   d.push_back('\0');
        char a0[] = "dosbox-x", a1[] = "-conf", a3[] = "-defaultdir";
        char *argv[] = { a0, a1, c.data(), a3, d.data(), nullptr };
        retrodos_host_run(5, argv);
    }).detach();

    return true;
}

extern "C" bool dd_has_started(void)       { return g_started.load(); }
extern "C" bool dd_is_running(void)        { return retrodos_host_is_running(); }
extern "C" void dd_quit(void)              { retrodos_host_quit(); }
extern "C" void dd_set_pause(bool paused)  { retrodos_host_set_pause(paused); }
extern "C" void dd_reset(bool hard)        { retrodos_host_reset(hard); }

/* ================================================================== */
/* The picture                                                         */
/* ================================================================== */

extern "C" uint64_t dd_frame_serial(void) { return retrodos_host_framebuffer_serial(); }
extern "C" void dd_frame_size(int *w, int *h) { retrodos_host_framebuffer_size(w, h); }
extern "C" int dd_pixel_aspect_x1000(void) { return retrodos_host_pixel_aspect_x1000(); }

extern "C" int dd_copy_frame(uint32_t *dst, int capacity, int *w, int *h,
                             uint64_t *serial)
{
    return retrodos_host_copy_framebuffer(dst, capacity, w, h, serial);
}

/* ================================================================== */
/* Input                                                               */
/* ================================================================== */

extern "C" void dd_key(int scancode, bool pressed) { retrodos_host_send_key(scancode, pressed); }
extern "C" void dd_mouse_move(int dx, int dy)      { retrodos_host_mouse_move(dx, dy); }
extern "C" void dd_mouse_button(int b, bool down)  { retrodos_host_mouse_button(b, down); }
extern "C" void dd_mouse_wheel(int dz)             { retrodos_host_mouse_wheel(dz); }
extern "C" void dd_joystick(int port, int mask, int x, int y)
{
    retrodos_host_joystick(port, mask, x, y);
}

/* ================================================================== */
/* Media and the shell                                                 */
/* ================================================================== */

extern "C" int dd_mount_dir(char drive, const char *path) { return retrodos_host_mount_dir(drive, path); }
extern "C" int dd_unmount(char drive)                     { return retrodos_host_unmount(drive); }
extern "C" int dd_type(const char *line)                  { return retrodos_host_send_command(line); }
extern "C" int dd_insert_cd(char drive, const char *p)    { return retrodos_host_insert_cd(drive, p); }
extern "C" int dd_insert_floppy(char drive, const char *p){ return retrodos_host_insert_floppy(drive, p); }

/* ================================================================== */
/* Status                                                              */
/* ================================================================== */

extern "C" int dd_running_program(char *buf, int len) { return retrodos_host_running_program(buf, len); }
extern "C" int dd_fps(void)                           { return retrodos_host_fps(); }
extern "C" int dd_cycles(void)                        { return retrodos_host_cycles(); }

/* ================================================================== */
/* Confs                                                               */
/* ================================================================== */

extern "C" void dd_machine_defaults(DDMachine *out)
{
    if (!out) return;
    const retrodos::Settings s;          /* the C++ defaults, not a second copy */
    std::memset(out, 0, sizeof(*out));
    out->cycles_max    = s.cycles_max ? 1 : 0;
    out->cycles_fixed  = s.cycles_fixed;
    out->core_fast     = s.core_dynamic ? 1 : 0;
    out->memsize       = s.memsize;
    out->vmemsize      = s.vmemsize;
    out->fpu           = s.fpu ? 1 : 0;
    out->ems           = s.ems ? 1 : 0;
    out->umb           = s.umb ? 1 : 0;
    out->pcspeaker     = s.pcspeaker ? 1 : 0;
    out->aspect_mode   = s.aspect_mode;
    out->integer_scale = s.integer_scale ? 1 : 0;
    out->joystick      = s.pad_sends_joystick ? 1 : 0;
    put(out->machine, sizeof(out->machine), s.machine);
    put(out->cputype, sizeof(out->cputype), s.cputype);
    put(out->sbtype,  sizeof(out->sbtype),  s.sbtype);
    put(out->dos_ver, sizeof(out->dos_ver), s.dos_ver);
}

extern "C" int dd_write_dos_conf(const DDMachine *m, const char *title,
                                 const char *mount_dir, const char *run_cmd,
                                 const char *cd_image, const char *boot_floppy,
                                 const char *out_path)
{
    if (!m || !out_path) return -1;
    const retrodos::Settings s = from_c(m);

    std::vector<retrodos::DriveImage> drives;
    if (cd_image && *cd_image) {
        retrodos::DriveImage d;
        d.letter = 'E';
        d.path = cd_image;
        d.kind = retrodos::DriveImage::Cd;
        drives.push_back(d);
    }

    std::string run = run_cmd ? run_cmd : "";
    bool raw = false;
    if (boot_floppy && *boot_floppy) {
        retrodos::DriveImage d;
        d.letter = 'A';
        d.path = boot_floppy;
        d.kind = retrodos::DriveImage::Floppy;
        drives.push_back(d);
        /* Raw, because "BOOT A:" is a built-in command with an argument. The
         * quoting that protects a program name with a space in it would turn
         * this into a search for a program called "BOOT A:". */
        run = "BOOT A:";
        raw = true;
    }

    const std::string conf = retrodos::build_conf(
        s, title ? title : "DOS", mount_dir ? mount_dir : "", run, raw,
        std::string(), drives);
    return write_file(out_path, conf) ? 0 : -1;
}

/* ================================================================== */
/* The Windows machine                                                 */
/* ================================================================== */

extern "C" void dd_win_defaults(DDWinMachine *out, const char *dir, int os)
{
    if (!out) return;
    retrodos::Win98Install w;
    w.dir = dir ? dir : "";
    w.os  = os == DD_OS_FREEDOS ? retrodos::OsKind::FreeDos : retrodos::OsKind::Win98;
    std::memset(out, 0, sizeof(*out));
    to_c(w, out);
}

extern "C" bool dd_win_load(const char *dir, DDWinMachine *out)
{
    if (!dir || !out) return false;
    retrodos::Win98Install w;
    const bool ok = retrodos::win98_load(dir, w);
    if (w.dir.empty()) w.dir = dir;
    to_c(w, out);
    return ok;
}

extern "C" bool dd_win_save(const DDWinMachine *m)
{
    if (!m) return false;
    retrodos::Win98Install w = from_c(m);
    return retrodos::win98_save(w);
}

extern "C" int dd_win_true_phase(const DDWinMachine *m)
{
    if (!m) return DD_PHASE_CREATE;
    const retrodos::Win98Install w = from_c(m);
    switch (retrodos::win98_true_phase(w)) {
    case retrodos::Win98Phase::Install:  return DD_PHASE_INSTALL;
    case retrodos::Win98Phase::Continue: return DD_PHASE_CONTINUE;
    case retrodos::Win98Phase::Run:      return DD_PHASE_RUN;
    default:                             return DD_PHASE_CREATE;
    }
}

extern "C" bool dd_win_installed(const DDWinMachine *m)
{
    if (!m) return false;
    return retrodos::win98_installed(from_c(m));
}

extern "C" int dd_win_blocker(const DDWinMachine *m, char *buf, int len)
{
    if (!m || !buf || len <= 0) return -1;
    const std::string s = retrodos::win98_blocker(from_c(m));
    put(buf, (size_t)len, s);
    return (int)s.size();
}

extern "C" bool dd_win_reset(DDWinMachine *m, bool erase_disk)
{
    if (!m) return false;
    retrodos::Win98Install w = from_c(m);
    const bool ok = retrodos::win98_reset(w, erase_disk);
    to_c(w, m);
    return ok;
}

extern "C" const char *dd_os_name(int os)
{
    return retrodos::os_name(os == DD_OS_FREEDOS ? retrodos::OsKind::FreeDos
                                                 : retrodos::OsKind::Win98);
}

extern "C" const char *dd_os_folder(int os)
{
    return retrodos::os_folder(os == DD_OS_FREEDOS ? retrodos::OsKind::FreeDos
                                                   : retrodos::OsKind::Win98);
}

extern "C" const char *dd_phase_name(int phase)
{
    switch (phase) {
    case DD_PHASE_INSTALL:  return retrodos::win98_phase_name(retrodos::Win98Phase::Install);
    case DD_PHASE_CONTINUE: return retrodos::win98_phase_name(retrodos::Win98Phase::Continue);
    case DD_PHASE_RUN:      return retrodos::win98_phase_name(retrodos::Win98Phase::Run);
    default:                return retrodos::win98_phase_name(retrodos::Win98Phase::Create);
    }
}

extern "C" int dd_write_win_conf(const DDWinMachine *m, const char *out_path)
{
    if (!m || !out_path) return -1;
    const retrodos::Win98Install w = from_c(m);

    /*
     * The desktop conf, with its [autoexec] taken off and ours put on.
     *
     * Everything above [autoexec] -- the int13 fakery a 9x guest needs, the
     * reported DOS version of 7.1, the Sound Blaster model, the Voodoo on the
     * PCI bus -- is win98_conf()'s and stays its business. Only the phase
     * commands move, because on iOS they cannot live in the conf: changing
     * them would mean a new session, and there is only ever one.
     */
    std::string conf = retrodos::win98_conf(w);
    const std::string marker = "\n[autoexec]\n";
    const size_t at = conf.rfind(marker);
    if (at == std::string::npos) return -1;
    conf.erase(at + marker.size());

    /*
     * Y:, and never Z:.
     *
     * Z: is DOSBox-X's own virtual drive, where its built-in commands live.
     * Mounting over it takes IMGMOUNT and the rest away and leaves the machine
     * rebooting itself with no diagnostic -- which is exactly what the first
     * version of this did.
     */
    conf += "@echo off\n";
    conf += "MOUNT -q Y \"" + w.dir + "\"\n";
    conf += "@CALL Y:\\GO.BAT\n";

    return write_file(out_path, conf) ? 0 : -1;
}

extern "C" int dd_write_phase_batch(const DDWinMachine *m, const char *out_path)
{
    if (!m || !out_path) return -1;
    const retrodos::Win98Install w = from_c(m);

    /*
     * GO.BAT: the installation's whole sequence, deciding for itself at every
     * boot which step it is on.
     *
     * On the desktop each phase is a different [autoexec] and the frontend
     * launches a NEW session to change it. That cannot work here -- there is
     * only ever one session -- so the phase has to be decided somewhere that
     * is consulted again on every boot. [autoexec] IS consulted again on every
     * boot: a machine reset re-runs it, and so does a reboot the guest asks
     * for itself. Windows Setup restarts the machine several times before it
     * is finished, and each of those has to boot the right thing.
     *
     * So the decision moves into the batch file, where it is made from the
     * same two facts the host would have used:
     *
     *   no disk image yet          -> IMGMAKE one
     *   image, but no Windows on it -> boot the CD and run Setup
     *   Windows on the image        -> boot the disk
     *
     * The second test is the one that matters: it is the guest reading its own
     * hard disk, which is the same question win98_installed() answers from
     * outside, and it is re-asked at every single boot. Setup partitions and
     * restarts, and the CD boots again because Windows is not there yet; Setup
     * copies its files and restarts, and now the disk boots because it is.
     * Nothing has to notice, and nothing has to be timed.
     *
     * The host still rewrites this file when the machine's SETTINGS change --
     * a different CD, a different disk size -- and resets to apply them.
     */
    const std::string hdd  = w.hdd.empty() ? std::string("hdd.img") : w.hdd;
    const std::string hddq = hdd.find(' ') == std::string::npos ? hdd : "\"" + hdd + "\"";
    const std::string isoq = w.iso.find(' ') == std::string::npos ? w.iso : "\"" + w.iso + "\"";

    /* What proves the operating system is on the disk. The same file
     * image_has_windows() looks for from the host side, so the guest and the
     * app cannot disagree about whether an install has happened. */
    const char *proof = w.os == retrodos::OsKind::FreeDos
                      ? "C:\\FREEDOS\\BIN\\COMMAND.COM"
                      : "C:\\WINDOWS\\WIN.COM";

    std::string b;
    b += "@ECHO OFF\n";
    /* Y: is the machine folder, mounted by the conf. Testing the image through
     * it tests the HOST file -- IMGMAKE has not run yet, so there is no drive
     * to test it on. */
    b += "IF NOT EXIST Y:\\" + hdd + " GOTO MAKE\n";
    b += "IMGMOUNT C " + hddq + "\n";
    /*
     * "-t iso" is not optional, and leaving it off fails in a way that blames
     * the disc.
     *
     * Without it DOSBox-X still recognises the file and attaches it as an IDE
     * CD-ROM -- the log even says "CD-ROM image mounted to drive D" -- but
     * there is no MSCDEX drive for "-bootcd D" to read, so the El Torito
     * lookup finds the boot record, mounts the extracted image as a HARD DISK
     * on the IDE slave, and the guest prints
     *
     *   This is not a bootable disk. Please insert a bootable floppy ...
     *
     * against a Windows 98 SE disc that is unambiguously bootable. With it:
     *
     *   MSCDEX installed.
     *   Drive A is mounted as El Torito floppy drive
     *
     * Measured against a real disc, by hand at the prompt. The desktop
     * frontend's win98_autoexec() omits it too and presumably has the same
     * fault; this file is iOS's and is fixed here.
     */
    if (!w.iso.empty()) b += "IMGMOUNT D " + isoq + " -t iso\n";
    b += std::string("IF EXIST ") + proof + " GOTO HDD\n";
    if (!w.iso.empty()) {
        /* "-t floppy" is not optional, whatever the guide says: without it
         * DOSBox-X finds the El Torito boot record, loads the boot sector, and
         * the guest prints "This is not a bootable disk" because the floppy
         * emulation was never set up for it to read the rest from. */
        b += "IMGMOUNT A -t floppy -bootcd D\n";
        b += "BOOT A:\n";
    }
    b += "GOTO END\n";
    b += ":HDD\n";
    b += "BOOT C:\n";
    b += "GOTO END\n";
    b += ":MAKE\n";
    /* The guide's own example. 8GB because with a reported DOS version of 7.1
     * anything over 512MB is FAT32, and Windows 98's generic IDE driver cannot
     * handle a volume over 128GB whatever the filesystem allows. */
    if (w.size_mb > 0)
        b += "IMGMAKE " + hddq + " -t hd -size " + std::to_string(w.size_mb) + "\n";
    else
        b += "IMGMAKE " + hddq + " -t hd_8gig\n";
    b += ":END\n";

    /* CRLF: this is read by DOS, and a batch file with bare LF line endings
     * leaves a stray character on the end of every command. */
    std::string crlf;
    for (const char ch : b) {
        if (ch == '\n') crlf += '\r';
        crlf += ch;
    }
    return write_file(out_path, crlf) ? 0 : -1;
}

extern "C" bool dd_image_has_os(const char *path)
{
    return path ? retrodos::image_has_os(path) : false;
}

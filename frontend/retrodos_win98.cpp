#include "retrodos_win98.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace retrodos {

namespace {

std::string join(const std::string &dir, const std::string &name)
{
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

bool file_exists(const std::string &path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

std::string state_path(const std::string &dir)
{
    return join(dir, "win98.cfg");
}

/*
 * The ISO as the guest must see it.
 *
 * IMGMOUNT takes a host path, but the [autoexec] line is parsed by the DOS
 * shell, which treats a space as an argument separator. A path with a space in
 * it therefore mounts nothing and the install boots to a prompt with no CD --
 * which looks like a bad image rather than a quoting bug. Quoting is what
 * DOSBox-X's own shell expects.
 */
std::string dos_arg(const std::string &path)
{
    if (path.find(' ') == std::string::npos) return path;
    return "\"" + path + "\"";
}

/* ------------------------------------------------------------------ */
/* Reading the guest's own disk                                        */
/* ------------------------------------------------------------------ */
/*
 * Just enough FAT to answer one question: is Windows on this image yet?
 *
 * It was worth writing because the alternative is asking the user. The wizard
 * used to advance from "installing" to "installed" only when somebody pressed
 * a button saying so -- which is a question the app should not have to ask,
 * since the answer is sitting in the disk image it just made.
 *
 * Deliberately minimal: the MBR's first partition, FAT16 or FAT32, short names
 * only, one file in one top-level directory. Anything it does not understand
 * reads as "cannot tell", never as a guess -- a false yes would tell the user
 * a half-finished install is ready.
 */

Uint32 le32(const unsigned char *p) {
    return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}
Uint16 le16(const unsigned char *p) { return (Uint16)((Uint16)p[0] | ((Uint16)p[1] << 8)); }

bool read_at(SDL_IOStream *io, Uint64 off, void *buf, size_t len)
{
    if (SDL_SeekIO(io, (Sint64)off, SDL_IO_SEEK_SET) < 0) return false;
    return SDL_ReadIO(io, buf, len) == len;
}

/* A directory entry's name, as FAT stores it: eight characters and three,
 * space padded, with no dot. "WIN     COM" is WIN.COM. */
bool dirent_named(const unsigned char *e, const char *name83)
{
    for (int i = 0; i < 11; ++i)
        if (SDL_toupper(e[i]) != (unsigned char)name83[i]) return false;
    return true;
}

struct Fat {
    SDL_IOStream *io = nullptr;
    Uint64 part_off = 0;        /* bytes from the start of the image         */
    Uint32 bytes_per_sector = 0;
    Uint32 sectors_per_cluster = 0;
    Uint64 fat_off = 0;         /* bytes; first FAT                          */
    Uint64 data_off = 0;        /* bytes; cluster 2                          */
    Uint64 root_off = 0;        /* bytes; FAT16 fixed root, 0 on FAT32       */
    Uint32 root_entries = 0;    /* FAT16 only                                */
    Uint32 root_cluster = 0;    /* FAT32 only                                */
    bool   fat32 = false;
    Uint32 cluster_count = 0;
};

/* The next cluster in a chain, or 0 when the chain ends or the image is not
 * making sense. 0 is safe: it is never a valid data cluster. */
Uint32 fat_next(const Fat &f, Uint32 cluster)
{
    if (cluster < 2 || cluster >= f.cluster_count + 2) return 0;
    unsigned char b[4] = {0};
    if (f.fat32) {
        if (!read_at(f.io, f.fat_off + (Uint64)cluster * 4, b, 4)) return 0;
        const Uint32 n = le32(b) & 0x0FFFFFFFu;
        return (n >= 0x0FFFFFF8u) ? 0 : n;
    }
    if (!read_at(f.io, f.fat_off + (Uint64)cluster * 2, b, 2)) return 0;
    const Uint32 n = le16(b);
    return (n >= 0xFFF8u) ? 0 : n;
}

/*
 * Find one 8.3 name in a directory. Returns its first cluster, or 0 when it is
 * not there. [want_dir] decides which of a file and a directory of the same
 * name counts, because "WINDOWS" as a file is not the folder we are after.
 *
 * start_cluster 0 means the FAT16 fixed root region.
 */
Uint32 dir_find(const Fat &f, Uint32 start_cluster, const char *name83, bool want_dir)
{
    const Uint32 cluster_bytes = f.bytes_per_sector * f.sectors_per_cluster;
    if (cluster_bytes == 0 || cluster_bytes > (1u << 20)) return 0;

    std::vector<unsigned char> buf(cluster_bytes);
    Uint32 cluster = start_cluster;
    Uint64 fixed_left = 0;
    Uint64 fixed_off  = 0;
    if (start_cluster == 0) {
        fixed_off  = f.root_off;
        fixed_left = (Uint64)f.root_entries * 32;
    }

    /* A chain that loops would otherwise spin here forever, and a corrupt
     * image is exactly the case this has to survive. */
    for (Uint32 guard = 0; guard < 65536; ++guard) {
        Uint64 off, len;
        if (start_cluster == 0) {
            if (fixed_left == 0) return 0;
            len = (fixed_left < cluster_bytes) ? fixed_left : cluster_bytes;
            off = fixed_off;
            fixed_off  += len;
            fixed_left -= len;
        } else {
            if (cluster < 2) return 0;
            off = f.data_off + (Uint64)(cluster - 2) * cluster_bytes;
            len = cluster_bytes;
        }
        if (!read_at(f.io, off, buf.data(), (size_t)len)) return 0;

        for (Uint64 i = 0; i + 32 <= len; i += 32) {
            const unsigned char *e = &buf[(size_t)i];
            if (e[0] == 0x00) return 0;          /* nothing beyond this point */
            if (e[0] == 0xE5) continue;          /* deleted                   */
            if ((e[11] & 0x0F) == 0x0F) continue; /* long-name fragment       */
            if (e[11] & 0x08) continue;          /* volume label              */
            if (!dirent_named(e, name83)) continue;
            const bool is_dir = (e[11] & 0x10) != 0;
            if (is_dir != want_dir) continue;
            Uint32 first = le16(e + 26);
            if (f.fat32) first |= (Uint32)le16(e + 20) << 16;
            /* A zero-length file has no cluster; report 1, which is not a
             * valid cluster but is a truthful "yes, it is there". */
            return first ? first : 1u;
        }

        if (start_cluster != 0) {
            cluster = fat_next(f, cluster);
            if (cluster == 0) return 0;
        }
    }
    return 0;
}

/* Open the image's first FAT partition. False when the image is not one, which
 * includes the perfectly normal case of a disk IMGMAKE made and nothing has
 * formatted yet. */
bool fat_open(const std::string &image, Fat &f)
{
    f.io = SDL_IOFromFile(image.c_str(), "rb");
    if (!f.io) return false;

    unsigned char sec[512];
    if (!read_at(f.io, 0, sec, sizeof sec)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    Uint64 start_lba = 0;
    for (int i = 0; i < 4; ++i) {
        const unsigned char *e = sec + 0x1BE + i * 16;
        const unsigned char type = e[4];
        switch (type) {
        case 0x01: case 0x04: case 0x06:      /* FAT12/16                    */
        case 0x0B: case 0x0C:                 /* FAT32, CHS and LBA          */
        case 0x0E:                            /* FAT16 LBA                   */
            start_lba = le32(e + 8);
            break;
        default:
            continue;
        }
        if (start_lba) break;
    }
    if (!start_lba) return false;

    f.part_off = start_lba * 512ull;
    if (!read_at(f.io, f.part_off, sec, sizeof sec)) return false;

    f.bytes_per_sector    = le16(sec + 0x0B);
    f.sectors_per_cluster = sec[0x0D];
    const Uint32 reserved = le16(sec + 0x0E);
    const Uint32 num_fats = sec[0x10];
    f.root_entries        = le16(sec + 0x11);
    Uint32 total_sectors  = le16(sec + 0x13);
    Uint32 fat_sectors    = le16(sec + 0x16);
    if (total_sectors == 0) total_sectors = le32(sec + 0x20);
    if (fat_sectors == 0) {
        fat_sectors   = le32(sec + 0x24);
        f.fat32       = true;
        f.root_cluster = le32(sec + 0x2C);
    }

    if (f.bytes_per_sector != 512 || f.sectors_per_cluster == 0 ||
        num_fats == 0 || reserved == 0 || fat_sectors == 0 || total_sectors == 0)
        return false;

    f.fat_off = f.part_off + (Uint64)reserved * f.bytes_per_sector;
    const Uint64 root_sectors =
        ((Uint64)f.root_entries * 32 + f.bytes_per_sector - 1) / f.bytes_per_sector;
    const Uint64 first_data =
        (Uint64)reserved + (Uint64)num_fats * fat_sectors + root_sectors;
    f.root_off  = f.part_off + ((Uint64)reserved + (Uint64)num_fats * fat_sectors)
                             * f.bytes_per_sector;
    f.data_off  = f.part_off + first_data * f.bytes_per_sector;
    f.cluster_count = (Uint32)((total_sectors - first_data) / f.sectors_per_cluster);
    if (f.cluster_count == 0) return false;
    return true;
}

/*
 * Is there an installed Windows on this image?
 *
 * WIN.COM inside \WINDOWS is the marker. Setup writes it in its file-copy
 * stage, before the hardware-detection reboots, so this is honestly "Setup has
 * got as far as copying Windows" -- which is exactly what the Continue phase
 * means, and it is what lets that phase be reached without asking.
 */
bool image_has_windows(const std::string &image)
{
    /*
     * Cached against the image's own timestamp.
     *
     * The wizard asks this every frame -- it is part of deciding which step to
     * show -- and the answer costs a directory walk across an 8GB file. The
     * image only changes while the guest is running, which is precisely when
     * nothing is asking, so keying on the modification time gives one scan per
     * visit to the page. UI thread only, like everything else in this file.
     */
    static std::string cached_path;
    static Sint64      cached_time = 0;
    static Uint64      cached_size = 0;
    static bool        cached_result = false;

    SDL_PathInfo info;
    if (!SDL_GetPathInfo(image.c_str(), &info)) return false;
    if (image == cached_path && info.modify_time == cached_time &&
        info.size == cached_size)
        return cached_result;

    Fat f;
    bool found = false;
    if (fat_open(image, f)) {
        const Uint32 root = f.fat32 ? f.root_cluster : 0;
        const Uint32 windir = dir_find(f, root, "WINDOWS    ", true);
        if (windir >= 2) found = dir_find(f, windir, "WIN     COM", false) != 0;
    }
    if (f.io) SDL_CloseIO(f.io);

    cached_path   = image;
    cached_time   = info.modify_time;
    cached_size   = info.size;
    cached_result = found;
    return found;
}

} /* namespace */

const char *win98_phase_name(Win98Phase p)
{
    switch (p) {
    case Win98Phase::Create:   return "Create the disk";
    case Win98Phase::Install:  return "Install from CD";
    case Win98Phase::Continue: return "Finish installing";
    case Win98Phase::Run:      return "Windows 98";
    }
    return "Windows 98";
}

std::string win98_blocker(const Win98Install &w)
{
    if (w.dir.empty()) return "No folder chosen for this machine.";

    /* The CD is needed for both install phases, and Windows keeps asking for
     * it afterwards until its files are copied to C:, so it is only optional
     * once the machine is installed. */
    if (w.phase != Win98Phase::Run) {
        if (w.iso.empty())
            return "Choose your Windows 98 CD-ROM image (.iso or .cue).";
        if (!file_exists(w.iso))
            return "That CD-ROM image is no longer there: " + w.iso;
    }

    if (w.phase != Win98Phase::Create && !file_exists(join(w.dir, w.hdd)))
        return "The hard disk image is missing. Create it again.";

    return std::string();
}

std::string win98_autoexec(const Win98Install &w)
{
    const std::string hdd = dos_arg(w.hdd);
    const std::string iso = dos_arg(w.iso);
    std::string a;

    switch (w.phase) {
    case Win98Phase::Create:
        /*
         * Guide: "IMGMAKE hdd.img -t hd_8gig", or a custom size via
         * "-t hd -size 16384".
         *
         * 8GB is the guide's own example and a sensible default: with a
         * reported DOS version of 7.1 anything over 512MB is formatted FAT32,
         * and Windows 98's generic IDE driver cannot handle a volume over
         * 128GB whatever the filesystem allows.
         */
        if (w.size_mb > 0)
            a += "IMGMAKE " + hdd + " -t hd -size " + std::to_string(w.size_mb) + "\n";
        else
            a += "IMGMAKE " + hdd + " -t hd_8gig\n";
        /*
         * EXIT, not PAUSE.
         *
         * PAUSE was the obvious way to let the result stay on screen, and it
         * hangs forever: this phase has no keyboard -- the frontend owns the
         * window, and on a handheld there is nothing to press. Tested, and it
         * sat there until it was killed, with a finished 8GB image beside it.
         *
         * Exiting hands control back to the frontend, which can then check
         * whether the image appeared and say so in the UI. That is a better
         * report than a DOS screen the user has to read anyway.
         */
        a += "EXIT\n";
        break;

    case Win98Phase::Install:
        /*
         * Guide: mount the disk, then boot the CD.
         *
         *   IMGMOUNT C hdd.img
         *   IMGMOUNT D Win98.iso
         *   IMGMOUNT A -bootcd D
         *   BOOT A:
         *
         * "IMGMOUNT A -bootcd D" extracts the El Torito boot floppy from the
         * CD, which is why no separate boot disk is needed -- and why this
         * only works with an OEM Full edition disc. A CD without a boot record
         * fails here with "El Torito CD-ROM boot record not found", which the
         * wizard surfaces verbatim rather than translating into something
         * vaguer.
         */
        a += "IMGMOUNT C " + hdd + "\n";
        a += "IMGMOUNT D " + iso + "\n";
        /*
         * "-t floppy" is not optional, whatever the guide says.
         *
         * The guide gives "IMGMOUNT A -bootcd D". Run against a real Windows
         * 98 SE disc that is unambiguously El Torito bootable, DOSBox-X finds
         * the boot record, reports the right entry and loads the boot sector
         * -- and the guest then prints "This is not a bootable disk", because
         * the floppy emulation was never set up for it to read the rest of
         * the image from. IMGMOUNT's own help spells the type out:
         *
         *   IMGMOUNT drive [-t floppy] -bootcd cdDrive (or -el-torito cdDrive)
         *
         * With the type it reaches the Windows 98 CD-ROM Startup Menu, which
         * is where the guide says you should be.
         */
        a += "IMGMOUNT A -t floppy -bootcd D\n";
        a += "BOOT A:\n";
        break;

    case Win98Phase::Continue:
        /*
         * Guide: after SETUP's first reboot, boot the hard disk instead, with
         * the CD still mounted because Windows asks for it repeatedly through
         * the rest of the install.
         */
        a += "IMGMOUNT C " + hdd + "\n";
        a += "IMGMOUNT D " + iso + "\n";
        a += "BOOT C:\n";
        break;

    case Win98Phase::Run:
        a += "IMGMOUNT C " + hdd + "\n";
        /* The CD stays optional once installed: if the user still has it, mount
         * it, because Windows will ask for it until \WIN98 is copied to C:. */
        if (!w.iso.empty() && file_exists(w.iso))
            a += "IMGMOUNT D " + iso + "\n";
        a += "BOOT C:\n";
        break;
    }

    return a;
}

std::string win98_conf(const Win98Install &w)
{
    std::string c;

    /*
     * [sdl] is ours, not the guide's.
     *
     * The guide assumes DOSBox-X owns its window and says autolock=true. Here
     * the frontend owns the window and the engine renders offscreen through
     * Game Link, exactly as build_conf() does for a game -- and both lines are
     * required, because output=gamelink without "gamelink master=true" falls
     * back to output=surface and renders into a window nobody reads.
     */
    c += "[sdl]\n";
    c += "output=gamelink\n";
    c += "gamelink master=true\n";
    c += "autolock=false\n";
    c += "waitonerror=false\n";
    c += "showmenu=false\n";

    c += "\n[dosbox]\n";
    c += "title=Windows 98\n";
    c += "memsize=" + std::to_string(w.memsize) + "\n";
    /* Ours, for the same reasons build_conf gives: upstream's banner points at
     * upstream's issue tracker, and a non-TTY stdin makes DOSBox-X block in a
     * working-directory prompt that has nowhere to appear on a phone. */
    c += "startbanner=false\n";
    c += "working directory option=noprompt\n";

    c += "\n[video]\n";
    c += "vmemsize=8\n";
    /* The guide lifts the VESA mode limits so Windows sees its higher modes. */
    c += "vesa modelist width limit=0\n";
    c += "vesa modelist height limit=0\n";

    c += "\n[dos]\n";
    /*
     * 7.1 is not cosmetic. It is the only value in DOSBox-X's entire config
     * that names Windows 98, it is what enables long filenames, and FAT32
     * images will not mount without a reported version of 7.1 or higher --
     * IMGMOUNT otherwise stops to ask whether to change it, and an [autoexec]
     * has nobody to answer.
     */
    c += "ver=7.1\n";
    /*
     * For the same reason: left at its default of "ask", mounting a FAT32
     * image stops to ask whether to change the reported DOS version, and an
     * [autoexec] has nobody to answer.
     *
     * "auto", not "set". The valid values are ask/auto/manual -- "set" was a
     * guess, and DOSBox-X answered it with "It might now be reset to the
     * default value: ask", which is to say silently back to the prompt this
     * line exists to avoid.
     */
    c += "fat32setversion=auto\n";
    /* The guide removes both rate limits; they exist to slow disk access to
     * period-accurate speeds, which during a multi-hundred-megabyte install is
     * simply a long wait. */
    c += "hard drive data rate limit=0\n";
    c += "floppy drive data rate limit=0\n";

    c += "\n[cpu]\n";
    c += "cputype=pentium_mmx\n";
    /*
     * core=normal during installation is the guide's explicit instruction,
     * pending upstream issue #2215, and the repo's own NOTES file for Windows
     * 98 SE says the same in more detail. The interpreter is slow but it
     * finishes; the dynamic core produces crashes during SETUP that look like
     * a bad CD.
     *
     * After installation the guide says dynamic_x86 should work -- but never
     * "dynamic" by name here, for the reason build_conf documents at length:
     * iOS forbids memory that is both writable and executable, so its core is
     * built without any dynamic recompiler and naming one asks for something
     * that is not in the binary. "auto" asks for the best core that was
     * actually built.
     */
    c += std::string("core=") +
         ((w.phase == Win98Phase::Run && w.fast_core_after_install) ? "auto"
                                                                   : "normal") + "\n";

    c += "\n[keyboard]\n";
    /*
     * The PS/2 mouse, stated rather than left to the default.
     *
     * A booted guest has no other way to be given a mouse: DOSBox-X's INT 33h
     * driver is a DOS service and Windows never calls it, so the pointer comes
     * through the 8042 auxiliary port or not at all. Both of these already
     * default this way, but a Windows guest is precisely the case where a
     * changed default would be silent and baffling -- Windows finds no mouse
     * during hardware detection, installs no driver, and afterwards draws a
     * cursor that does not move.
     */
    c += "aux=true\n";
    c += "auxdevice=intellimouse\n";

    c += "\n[sblaster]\n";
    /* The guide's choice. The ViBRA is the PnP part Windows 98 has a driver
     * for, so sound works after install without hunting for one. */
    c += "sbtype=sb16vibra\n";

    /*
     * The int13 fakery is what makes Windows' own 32-bit disk access work.
     * Without it Windows falls back to real-mode compatibility-mode disk
     * access -- it boots, it runs, and it is extremely slow, with a "drives
     * are using MS-DOS compatibility mode" warning buried in System
     * Properties that nobody connects to the install having been wrong.
     */
    c += "\n[fdc, primary]\n";
    c += "int13fakev86io=true\n";

    c += "\n[ide, primary]\n";
    c += "int13fakeio=true\n";
    c += "int13fakev86io=true\n";

    c += "\n[ide, secondary]\n";
    c += "int13fakeio=true\n";
    c += "int13fakev86io=true\n";
    /* Guide: 4000ms, so Windows' auto-insert notification triggers properly. */
    c += "cd-rom insertion delay=4000\n";

    c += "\n[render]\n";
    c += "scaler=none\n";

    c += "\n[autoexec]\n";
    c += win98_autoexec(w);

    return c;
}

bool win98_is_install_dir(const std::string &dir)
{
    return file_exists(state_path(dir));
}

Win98Phase win98_true_phase(const Win98Install &w)
{
    /*
     * The phase the machine is actually in, as opposed to the one recorded.
     *
     * They come apart easily and the result is baffling: a saved phase of
     * Continue with no disk image runs "IMGMOUNT C hdd.img" against a file
     * that is not there, and the guest drops to a prompt that cannot boot
     * anything. Nothing in that failure mentions a missing disk.
     *
     * So a recorded phase is never trusted to be further along than the files
     * support -- it can only ever be revised DOWN here. Going forward is a
     * decision the wizard makes after a step succeeds, because "Setup has
     * finished" is not observable from out here.
     */
    if (w.dir.empty()) return Win98Phase::Create;
    if (!file_exists(join(w.dir, w.hdd))) return Win98Phase::Create;
    if (w.phase == Win98Phase::Create)    return Win98Phase::Install;

    /*
     * Forward, but only on evidence.
     *
     * Windows on the disk means the CD has already done its job, and booting
     * it again restarts Setup on top of a machine that was part way through
     * installing -- which is what happened the first time this was used, and
     * it is not a state the user can recover from by reading the screen.
     *
     * "Setup has finished every pass" is still not observable from out here,
     * so this stops at Continue and the last step remains the user's to
     * confirm. Reaching Continue is the part that matters: from there the
     * autoexec boots the hard disk, which is what an installed Windows and a
     * half-installed one both want.
     */
    if (w.phase == Win98Phase::Install && win98_installed(w))
        return Win98Phase::Continue;

    return w.phase;
}

bool win98_installed(const Win98Install &w)
{
    if (w.dir.empty()) return false;
    const std::string img = join(w.dir, w.hdd);
    if (!file_exists(img)) return false;
    return image_has_windows(img);
}

bool win98_load(const std::string &dir, Win98Install &out)
{
    /* Copied before [out] is cleared: a caller that passes its own member --
     * win98_load(w.dir, w) -- would otherwise have the path wiped out from
     * under it by the line below and load from the working directory. */
    const std::string path = state_path(dir);
    const std::string folder = dir;

    out = Win98Install{};
    out.dir = folder;

    SDL_IOStream *in = SDL_IOFromFile(path.c_str(), "rb");
    if (!in) return false;

    const Sint64 size = SDL_GetIOSize(in);
    if (size <= 0 || size > (1 << 20)) { SDL_CloseIO(in); return false; }

    std::string text((size_t)size, '\0');
    const size_t got = SDL_ReadIO(in, text.data(), text.size());
    SDL_CloseIO(in);
    if (got != text.size()) return false;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;

        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        const std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (!v.empty() && v.back() == '\r') v.pop_back();

        if      (k == "iso")     out.iso = v;
        else if (k == "hdd")     out.hdd = v;
        else if (k == "size_mb") out.size_mb = std::atoi(v.c_str());
        else if (k == "memsize") out.memsize = std::atoi(v.c_str());
        else if (k == "fast_core_after_install") out.fast_core_after_install = (v == "1");
        else if (k == "phase") {
            const int p = std::atoi(v.c_str());
            /* Clamped rather than trusted: a hand-edited or truncated file
             * must not index off the end of the switch. */
            out.phase = (p >= 0 && p <= (int)Win98Phase::Run) ? (Win98Phase)p
                                                             : Win98Phase::Create;
        }
    }

    if (out.hdd.empty()) out.hdd = "hdd.img";
    if (out.memsize < 16 || out.memsize > 512) out.memsize = 128;
    /* Never hand back a phase the files do not support. */
    out.phase = win98_true_phase(out);
    return true;
}

bool win98_reset(Win98Install &w, bool erase_disk)
{
    if (w.dir.empty()) return false;
    if (erase_disk) {
        const std::string img = join(w.dir, w.hdd);
        if (file_exists(img)) SDL_RemovePath(img.c_str());
    }
    w.phase = Win98Phase::Create;
    w.fast_core_after_install = false;
    /* The chosen disc is kept. Starting the installation again almost never
     * means "and I have a different Windows CD now", and making the user find
     * it a second time would be a small punishment for a reasonable act. */
    return win98_save(w);
}

bool win98_save(const Win98Install &w)
{
    if (w.dir.empty()) return false;
    SDL_CreateDirectory(w.dir.c_str());

    std::string s = "# Retro-DOS: Windows 98 machine\n";
    s += "phase=" + std::to_string((int)w.phase) + "\n";
    s += "iso=" + w.iso + "\n";
    s += "hdd=" + w.hdd + "\n";
    s += "size_mb=" + std::to_string(w.size_mb) + "\n";
    s += "memsize=" + std::to_string(w.memsize) + "\n";
    s += std::string("fast_core_after_install=") +
         (w.fast_core_after_install ? "1" : "0") + "\n";

    SDL_IOStream *out = SDL_IOFromFile(state_path(w.dir).c_str(), "wb");
    if (!out) return false;
    const size_t put = SDL_WriteIO(out, s.data(), s.size());
    SDL_CloseIO(out);
    return put == s.size();
}

} /* namespace retrodos */

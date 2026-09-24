/*
 * retro-dos — installing and running Windows 98.
 *
 * This is a config generator and a four-state machine, and deliberately
 * nothing else. Installing Windows 98 under DOSBox-X is a documented
 * procedure with a specific config file and a specific sequence of DOS
 * commands, and all of it is expressible as a dosbox.conf plus an [autoexec]
 * block — which the app can already write and run. So no new emulator
 * plumbing is needed, and no new host ABI: this file produces text, and
 * launch() runs it exactly as it runs a game.
 *
 * Everything here follows DOSBox-X's own guide, "Installing Windows 98 in
 * DOSBox-X", on the project wiki. Where a value looks arbitrary it is quoted
 * from that guide, and the reason is in the comment beside it. The guide is
 * the authority: if it changes, this file changes with it rather than
 * acquiring a local opinion.
 *
 * What this file will NOT do is find, bundle or download a Windows 98
 * CD-ROM. The user supplies their own image, which is the same position the
 * guide itself takes -- "Getting this image file is outside the scope of this
 * guide" -- and the same position Retro-DOS takes on every other piece of
 * software it does not own.
 */
#ifndef RETRODOS_WIN98_H
#define RETRODOS_WIN98_H

#include <string>

namespace retrodos {

/*
 * Where an installation has got to.
 *
 * The phases exist because the guide's [autoexec] is different in each one --
 * this is not a progress bar. Installing boots the CD; continuing boots the
 * hard disk with the CD still mounted, because Windows asks for it; running
 * needs neither the boot floppy nor, eventually, the CD at all.
 */
/*
 * Which operating system the machine installs.
 *
 * FreeDOS came second and rides on the Windows 98 machinery because the shape
 * is identical: a disk image made by IMGMAKE, a CD booted through El Torito,
 * an installer that reboots part way, then the hard disk booted with the CD
 * still in. What differs is text, the file that proves the install got
 * somewhere, and a couple of config lines -- so it is a field, not a copy of
 * this file.
 */
enum class OsKind { Win98 = 0, FreeDos };

enum class Win98Phase {
    Create = 0,   /* no disk image yet: IMGMAKE it */
    Install,      /* image exists, empty: boot the CD and run SETUP */
    Continue,     /* SETUP has rebooted at least once: boot C:, CD still needed */
    Run,          /* installed: boot C: */
};

/*
 * One Windows 98 machine.
 *
 * Paths are host paths. `dir` is the folder the whole machine lives in, and is
 * what gets mounted; it is a normal library folder, so an installed Windows
 * appears in the Library beside the games with no special case.
 */
struct Win98Install {
    std::string dir;            /* host folder holding hdd.img and the conf  */
    OsKind      os = OsKind::Win98;
    std::string iso;            /* the user's Windows 98 / FreeDOS CD image  */
    std::string hdd = "hdd.img";
    int         size_mb = 0;    /* 0 = use the hd_8gig template              */
    int         memsize = 128;  /* guide default; Windows 98 tops out at 512 */
    Win98Phase  phase = Win98Phase::Create;

    /* The guide is explicit that installation must run on the interpreter --
     * see the note on core= in the .cpp. This flips to true only once the
     * install has finished, and only if the user asks for it. */
    bool fast_core_after_install = false;

    /*
     * A 3dfx Voodoo 1 on the PCI bus.
     *
     * On by default, because a Windows 98 machine without one cannot run the
     * 3D games of its own era at all -- they look for Glide or for a Direct3D
     * device and find neither, and the S3 Trio that DOSBox-X gives the guest
     * is a 2D card. The card is emulated whole, so Windows finds new hardware
     * on the next boot and wants the 3dfx driver for it.
     *
     * A setting rather than a fact because the emulated Voodoo is expensive:
     * it is a software rasteriser, and on a handheld it is the difference
     * between a game that is slow and one that does not move. Turning it off
     * gives back exactly the machine this app shipped before it existed.
     */
    bool voodoo = true;
};

/* The [autoexec] the current phase needs, without the section header. */
std::string win98_autoexec(const Win98Install &w);

/*
 * The complete dosbox.conf for this machine, [autoexec] included.
 *
 * Self-contained rather than routed through build_conf(): a Windows 98 guest
 * needs settings a DOS game never wants (int13 v86 fakery, a reported DOS
 * version of 7.1, a specific Sound Blaster model), and threading all of them
 * through the game path as special cases would put Windows-only behaviour in
 * everything's way.
 */
std::string win98_conf(const Win98Install &w);

/*
 * The phase the files actually support, which is never further along than
 * what is on disk. win98_load() applies it, so a caller normally gets this
 * for free; it is exposed because the wizard also wants to ask mid-session.
 */
Win98Phase win98_true_phase(const Win98Install &w);

/*
 * True when there is an installed Windows on the machine's hard disk image --
 * read from the image itself, so it is a fact rather than a recorded claim.
 *
 * Specifically \WINDOWS\WIN.COM, which Setup writes during its file-copy
 * stage. That makes this "Windows is on the disk", which is what decides
 * whether the CD still needs booting; it is NOT "Setup has finished", which
 * cannot be seen from outside the guest and stays the user's to confirm.
 */
bool win98_installed(const Win98Install &w);

/*
 * True when a disk or floppy image has an operating system to boot: an
 * MS-DOS/Windows IO.SYS, a FreeDOS KERNEL.SYS, or PC DOS / DR DOS boot files
 * in its root directory. Handles a partitioned hard disk image and a plain
 * floppy image alike. This is what decides whether Launch boots the image
 * or drops to the built-in DOS with it mounted.
 */
bool image_has_os(const std::string &path);

/*
 * Back to the beginning. [erase_disk] also removes the hard disk image, which
 * is what "start the installation again" means -- a half-installed Windows is
 * not something Setup can be pointed at a second time.
 *
 * The chosen CD is kept either way.
 */
bool win98_reset(Win98Install &w, bool erase_disk);

/* Phase state, one flat key=value file in the machine's own folder, the same
 * shape as the app's other config files. Missing or unreadable reads as a
 * fresh Create. */
bool win98_load(const std::string &dir, Win98Install &out);
bool win98_save(const Win98Install &w);

/* True when `dir` holds a Windows 98 machine, i.e. the library should show it
 * as one rather than scanning it for DOS executables. */
bool win98_is_install_dir(const std::string &dir);

/* Human text for the phase, for the wizard and the library row. */
const char *win98_phase_name(Win98Phase p);
const char *win98_phase_name(const Win98Install &w);

/* "Windows 98" / "FreeDOS": the library folder the machine lives in under the
 * games root, the display name, and the name of its Setup page. */
const char *os_folder(OsKind os);
const char *os_name(OsKind os);

/*
 * The FreeDOS disc the wizard asks for. The Legacy CD, not the LiveCD: it
 * boots by El Torito floppy emulation, which is the only kind "IMGMOUNT A -t
 * floppy -bootcd" knows how to start. The file goes in the games folder beside
 * any other disc image and the wizard lists it. Nothing is linked to or
 * downloaded: the user puts it there.
 */
extern const char *const kFreeDosIsoName;

/*
 * What the user still has to supply before [phase] can run, or "" when it is
 * ready. Checked rather than assumed: a missing ISO produces a guest that
 * boots to a DOS prompt and looks like a broken emulator.
 */
std::string win98_blocker(const Win98Install &w);

} /* namespace retrodos */

#endif /* RETRODOS_WIN98_H */

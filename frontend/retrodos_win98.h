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
    std::string iso;            /* the user's Windows 98 CD image            */
    std::string hdd = "hdd.img";
    int         size_mb = 0;    /* 0 = use the hd_8gig template              */
    int         memsize = 128;  /* guide default; Windows 98 tops out at 512 */
    Win98Phase  phase = Win98Phase::Create;

    /* The guide is explicit that installation must run on the interpreter --
     * see the note on core= in the .cpp. This flips to true only once the
     * install has finished, and only if the user asks for it. */
    bool fast_core_after_install = false;
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

/*
 * What the user still has to supply before [phase] can run, or "" when it is
 * ready. Checked rather than assumed: a missing ISO produces a guest that
 * boots to a DOS prompt and looks like a broken emulator.
 */
std::string win98_blocker(const Win98Install &w);

} /* namespace retrodos */

#endif /* RETRODOS_WIN98_H */

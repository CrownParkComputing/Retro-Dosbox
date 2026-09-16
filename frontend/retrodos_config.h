/*
 * retro-dosbox — persisted settings, and the DOSBox-X conf they generate.
 *
 * Two layers, because DOS titles disagree with each other constantly:
 *   global defaults  — what a game gets unless it says otherwise
 *   per-game override — one file per title, only the keys that differ
 *
 * Kept as plain key=value text rather than anything structured: it is small,
 * a human can fix it with a text editor when a game will not start, and it
 * survives an app update with no migration code.
 */
#ifndef RETRODOS_CONFIG_H
#define RETRODOS_CONFIG_H

#include <string>
#include <vector>

namespace retrodos {

/* The knobs that actually decide whether a DOS game runs. Every one of these
 * is here because getting it wrong breaks real titles, not for completeness. */
struct Settings {
    /* cycles: "max" suits most later titles; a FIXED count is what saves the
     * early ones, which busy-wait for timing and run absurdly fast otherwise. */
    bool        cycles_max   = true;
    int         cycles_fixed = 20000;

    /* dynamic is much faster; normal is the compatibility fallback. */
    bool        core_dynamic = true;

    /* 32 MB, not more: DOS/4GW 1.97 miscalculates with large memory and a pile
     * of early-90s titles refuse to start above it. */
    int         memsize      = 32;

    /* sbpro2 is the safest broad default for the DOS era. */
    std::string sbtype       = "sbpro2";

    /* ---- the machine ----
     *
     * Every field below is a DOSBox-X config key, and every default is
     * DOSBox-X's own, so a config written before these existed behaves
     * identically after the upgrade.
     */

    /* [dosbox] machine -- which graphics hardware the guest finds.
     *
     * The single biggest compatibility lever there is, and the one worth
     * reaching for first when a game starts and draws nothing: an early title
     * written for CGA or Tandy can be confused by an S3, and a few refuse
     * outright. svga_s3 is DOSBox-X's default and right for most things. */
    std::string machine      = "svga_s3";

    /* [cpu] cputype -- what CPUID and the feature checks report.
     *
     * "auto" suits DOS. It matters when a game refuses to start on a CPU it
     * thinks is too slow, and for a Windows guest, which looks. */
    std::string cputype      = "auto";

    /* [cpu] fpu -- the 387. On by default; a handful of titles detect one and
     * then use it badly, so it is worth being able to remove. */
    bool        fpu          = true;

    /* [video] vmemsize -- video memory in MB. 0 means "do not write the key",
     * i.e. let DOSBox-X choose, which is what almost everything wants. VESA
     * modes on later titles are the reason to raise it. */
    int         vmemsize     = 0;

    /* [dos] ver -- the DOS version the guest is told it is running on.
     *
     * Empty means DOSBox-X's own default (5.0). A few titles check and refuse;
     * "7.1" additionally enables long filenames and is what makes FAT32 disk
     * images mountable, which is why a Windows 98 guest needs it. */
    std::string dos_ver;

    /* [dos] ems / umb -- expanded memory and upper memory blocks.
     *
     * Both on by default and both are classic reasons a game will not start:
     * some titles find EMS and then misuse it, and a few will not load with
     * UMBs present. Turning one off is a standard first move. */
    bool        ems          = true;
    bool        umb          = true;

    /* [speaker] pcspeaker -- the internal beeper.
     *
     * On by default. It is the ONLY sound many pre-1990 titles have, so it is
     * worth surfacing rather than leaving people to conclude a game is
     * silent. */
    bool        pcspeaker    = true;

    /* DOS modes are frequently non-square-pixel: 320x200 is a 4:3 picture, not
     * 16:10. Kept for configs written before aspect_mode existed. */
    bool        aspect_correct = true;

    /* How the picture is fitted to the screen:
     *   0 Auto  -- the ratio the engine reports for the current video mode
     *   1 4:3   -- force it, for a mode the engine describes badly
     *   2 16:9  -- fill a widescreen handheld, geometry be damned
     *   3 Fill  -- stretch to the window exactly
     * Auto is right almost always; the rest exist because "almost" is not
     * "always" and the player is the one looking at it. */
    int         aspect_mode = 0;
    /* Integer scaling is sharper but leaves bigger borders on a handheld. */
    bool        integer_scale  = false;

    /* ---- controls ----
     *
     * What each virtual button (on-screen or gamepad -- they are the same
     * buttons) sends, as an SDL scancode. Per game, because DOS never agreed
     * on a control scheme: Descent wants the arrows and Ctrl, Keen wants Ctrl
     * and Alt for jump and pogo, and a flight sim wants the stick.
     *
     * 0 means unbound. Indexed by PadButton; sized to PAD_COUNT, which is 16.
     */
    int         pad_keys[16] = {0};

    /* Drive the emulated game port as well as (or instead of) sending keys.
     * Most DOS games are keyboard games, so keys are the default; a game that
     * genuinely wants a stick can be switched over per title. */
    bool        pad_sends_keys      = true;
    bool        pad_sends_joystick  = false;

    /* Whether the on-screen controls are wanted. Independent of whether a
     * gamepad is plugged in: that is a fact about the hardware right now, this
     * is what the user asked for. */
    bool        onscreen_pad        = true;

    bool operator==(const Settings &o) const {
        for (int i = 0; i < 16; ++i)
            if (pad_keys[i] != o.pad_keys[i]) return false;
        return cycles_max == o.cycles_max && cycles_fixed == o.cycles_fixed &&
               core_dynamic == o.core_dynamic && memsize == o.memsize &&
               sbtype == o.sbtype && aspect_correct == o.aspect_correct &&
               aspect_mode == o.aspect_mode &&
               integer_scale == o.integer_scale &&
               pad_sends_keys == o.pad_sends_keys &&
               pad_sends_joystick == o.pad_sends_joystick &&
               onscreen_pad == o.onscreen_pad &&
               machine == o.machine && cputype == o.cputype &&
               fpu == o.fpu && vmemsize == o.vmemsize &&
               dos_ver == o.dos_ver && ems == o.ems && umb == o.umb &&
               pcspeaker == o.pcspeaker;
    }
};

/** The keys a DOS game most often wants, used when nothing has been bound.
 *  Fills all PAD_COUNT entries of Settings::pad_keys. */
void default_pad_keys(int *keys);

/** A mapping for the 6-degrees-of-freedom shooters -- Descent and its kin --
 *  laid out for an Xbox-style pad. Fills all PAD_COUNT entries. */
void descent_pad_keys(int *keys);

/* App-level state that is not per-game. */
struct AppConfig {
    std::string library_root;      /* where games live                    */
    bool        wizard_done = false;
    Settings    defaults;

    /* On-screen control positions, as "button,x,y,r;..." with x/y/r as
     * fractions. Held as an opaque string so this layer keeps knowing nothing
     * about the pad -- it is one layout for the device, not per game, because
     * where a thumb comfortably rests does not change with the title. */
    std::string pad_layout;

    /* Name of a game to start immediately on boot, then clear. Written just
     * before the Android process restart that a second engine run requires
     * (the engine cannot run twice in one process); the fresh process reads
     * it, clears it FIRST so a bad game cannot cause a restart loop, and
     * launches. Empty in every steady state. */
    std::string pending_launch;
};

bool load_app_config(const std::string &path, AppConfig &out);
bool save_app_config(const std::string &path, const AppConfig &cfg);

/* Per-game overrides live beside the app config, one file per title. */
bool load_game_settings(const std::string &dir, const std::string &game, Settings &out);
bool save_game_settings(const std::string &dir, const std::string &game, const Settings &s);

/* Build the DOSBox-X conf for one title. mount_dir is a REAL filesystem path:
 * DOSBox-X mounts a directory by path and cannot be handed a content:// URI. */
/* run_raw emits run_cmd into autoexec unquoted. Program names are quoted
 * because DOS titles routinely contain spaces, but a built-in DOSBox-X command
 * that takes an argument -- "boot FREEDOS.IMG" -- would then be read as the
 * name of a program to find, and fail. */
/* extra_sections is conf text appended AFTER our own settings and before
 * [autoexec]. It carries the sound configuration a game ships with, which is
 * what that title was actually packaged against; a later key wins, so those
 * values override our defaults for that game only. */
std::string build_conf(const Settings &s, const std::string &title,
                       const std::string &mount_dir, const std::string &run_cmd,
                       bool run_raw = false,
                       const std::string &extra_sections = std::string());

} /* namespace retrodos */

#endif /* RETRODOS_CONFIG_H */

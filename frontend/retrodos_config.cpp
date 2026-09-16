#include "retrodos_config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <map>

namespace retrodos {
namespace {

std::string read_file(const std::string &path)
{
    size_t len = 0;
    void *data = SDL_LoadFile(path.c_str(), &len);
    if (!data) return std::string();
    std::string s((const char *)data, len);
    SDL_free(data);
    return s;
}

bool write_file(const std::string &path, const std::string &body)
{
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "w");
    if (!io) return false;
    const bool ok = SDL_WriteIO(io, body.data(), body.size()) == body.size();
    SDL_CloseIO(io);
    return ok;
}

std::map<std::string, std::string> parse_kv(const std::string &text)
{
    std::map<std::string, std::string> kv;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(i, e - i);
        i = e + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

bool as_bool(const std::map<std::string, std::string> &kv, const char *k, bool dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : (it->second == "1" || it->second == "true");
}

int as_int(const std::map<std::string, std::string> &kv, const char *k, int dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : SDL_atoi(it->second.c_str());
}

std::string as_str(const std::map<std::string, std::string> &kv, const char *k,
                   const std::string &dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : it->second;
}

void append_settings(std::string &s, const Settings &v)
{
    s += "cycles_max=";     s += v.cycles_max ? "1" : "0";     s += "\n";
    s += "cycles_fixed=";   s += std::to_string(v.cycles_fixed); s += "\n";
    s += "core_dynamic=";   s += v.core_dynamic ? "1" : "0";   s += "\n";
    s += "memsize=";        s += std::to_string(v.memsize);    s += "\n";
    s += "sbtype=";         s += v.sbtype;                     s += "\n";
    s += "aspect_correct="; s += v.aspect_correct ? "1" : "0"; s += "\n";
    s += "integer_scale=";  s += v.integer_scale ? "1" : "0";  s += "\n";
    s += "aspect_mode=";    s += std::to_string(v.aspect_mode);   s += "\n";
    s += "pad_sends_keys=";     s += v.pad_sends_keys ? "1" : "0";     s += "\n";
    s += "pad_sends_joystick="; s += v.pad_sends_joystick ? "1" : "0"; s += "\n";
    s += "onscreen_pad=";       s += v.onscreen_pad ? "1" : "0";       s += "\n";
    /* One line, so a per-game override file stays readable and a hand edit is
     * a single change rather than twelve. */
    s += "pad_keys=";
    for (int i = 0; i < 16; ++i) {
        if (i) s += ",";
        s += std::to_string(v.pad_keys[i]);
    }
    s += "\n";
}

void read_settings(const std::map<std::string, std::string> &kv, Settings &v)
{
    v.cycles_max     = as_bool(kv, "cycles_max", v.cycles_max);
    v.cycles_fixed   = as_int (kv, "cycles_fixed", v.cycles_fixed);
    v.core_dynamic   = as_bool(kv, "core_dynamic", v.core_dynamic);
    v.memsize        = as_int (kv, "memsize", v.memsize);
    v.sbtype         = as_str (kv, "sbtype", v.sbtype);
    v.aspect_correct = as_bool(kv, "aspect_correct", v.aspect_correct);
    v.integer_scale  = as_bool(kv, "integer_scale", v.integer_scale);
    /* Migration: a config written before aspect_mode existed expressed "do not
     * correct" as aspect_correct=0, which is what Fill now means. */
    v.aspect_mode    = as_int (kv, "aspect_mode",
                               v.aspect_correct ? v.aspect_mode : 3);
    v.pad_sends_keys     = as_bool(kv, "pad_sends_keys", v.pad_sends_keys);
    v.pad_sends_joystick = as_bool(kv, "pad_sends_joystick", v.pad_sends_joystick);
    v.onscreen_pad       = as_bool(kv, "onscreen_pad", v.onscreen_pad);

    const std::string keys = as_str(kv, "pad_keys", std::string());
    if (!keys.empty()) {
        int n = 0;
        size_t i = 0;
        /* Up to 16; a file written before L3/R3 existed has 14 entries and the
         * two stick clicks simply stay unbound. */
        while (i <= keys.size() && n < 16) {
            size_t e = keys.find(',', i);
            if (e == std::string::npos) e = keys.size();
            v.pad_keys[n++] = atoi(keys.substr(i, e - i).c_str());
            i = e + 1;
        }
    }
}

/* A game name becomes a filename, so it must not carry separators. */
std::string sanitise(const std::string &name)
{
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out += ok ? c : '_';
    }
    if (out.size() > 100) out.resize(100);
    return out;
}

} /* namespace */

void default_pad_keys(int *k)
{
    /* The DOS convention, not a modern console one. Arrows move; Ctrl fires
     * and Alt is the second action in a great many titles (Keen jumps on Ctrl
     * and pogos on Alt, Doom fires on Ctrl and uses on Space, Descent fires on
     * Ctrl). Enter and Escape are what menus expect. */
    k[0]  = SDL_SCANCODE_UP;
    k[1]  = SDL_SCANCODE_DOWN;
    k[2]  = SDL_SCANCODE_LEFT;
    k[3]  = SDL_SCANCODE_RIGHT;
    k[4]  = SDL_SCANCODE_LCTRL;    /* A */
    k[5]  = SDL_SCANCODE_LALT;     /* B */
    k[6]  = SDL_SCANCODE_SPACE;    /* X */
    k[7]  = SDL_SCANCODE_LSHIFT;   /* Y */
    k[8]  = SDL_SCANCODE_PAGEUP;   /* L */
    k[9]  = SDL_SCANCODE_PAGEDOWN; /* R */
    k[10] = 0;                     /* LT -- unused by a generic DOS game */
    k[11] = 0;                     /* RT */
    k[12] = SDL_SCANCODE_RETURN;   /* Start  */
    k[13] = SDL_SCANCODE_ESCAPE;   /* Select */
    k[14] = 0;                     /* L3 -- no DOS convention to point at */
    k[15] = 0;                     /* R3 */
}

void descent_pad_keys(int *k)
{
    /* Descent flies in six degrees of freedom, which is exactly why a d-pad
     * plus two buttons cannot hold it: turning, pitching, banking, sliding,
     * throttle and two weapons all want to be live at once. Laid out the way a
     * modern pad would do it -- aim on the stick, weapons on the triggers,
     * throttle on the face buttons, roll on the shoulders.
     *
     * These are Descent's DEFAULT keyboard bindings. If a game has been
     * reconfigured in its own setup, change these to match rather than the
     * other way round. */
    k[0]  = SDL_SCANCODE_UP;       /* pitch down  (nose up on the stick) */
    k[1]  = SDL_SCANCODE_DOWN;     /* pitch up    */
    k[2]  = SDL_SCANCODE_LEFT;     /* turn left   */
    k[3]  = SDL_SCANCODE_RIGHT;    /* turn right  */
    k[4]  = SDL_SCANCODE_A;        /* A  -- accelerate */
    k[5]  = SDL_SCANCODE_Z;        /* B  -- reverse    */
    k[6]  = SDL_SCANCODE_TAB;      /* X  -- automap    */
    k[7]  = SDL_SCANCODE_B;        /* Y  -- drop bomb  */
    k[8]  = SDL_SCANCODE_Q;        /* LB -- bank left  */
    k[9]  = SDL_SCANCODE_E;        /* RB -- bank right */
    k[10] = SDL_SCANCODE_SPACE;    /* LT -- secondary fire */
    k[11] = SDL_SCANCODE_LCTRL;    /* RT -- primary fire   */
    k[12] = SDL_SCANCODE_RETURN;   /* Start  */
    k[13] = SDL_SCANCODE_ESCAPE;   /* Select */
    k[14] = SDL_SCANCODE_R;        /* L3 -- rear view */
    k[15] = SDL_SCANCODE_F1;       /* R3 -- help/automap alt */
}

bool load_app_config(const std::string &path, AppConfig &out)
{
    const std::string text = read_file(path);
    if (text.empty()) return false;
    const auto kv = parse_kv(text);
    out.library_root = as_str(kv, "library_root", out.library_root);
    out.wizard_done  = as_bool(kv, "wizard_done", false);
    out.pad_layout   = as_str(kv, "pad_layout", out.pad_layout);
    out.pending_launch = as_str(kv, "pending_launch", out.pending_launch);
    read_settings(kv, out.defaults);
    return true;
}

bool save_app_config(const std::string &path, const AppConfig &cfg)
{
    std::string s = "# Retro-DOS settings\n";
    s += "library_root=" + cfg.library_root + "\n";
    s += "wizard_done=";  s += cfg.wizard_done ? "1" : "0"; s += "\n";
    if (!cfg.pad_layout.empty()) s += "pad_layout=" + cfg.pad_layout + "\n";
    if (!cfg.pending_launch.empty()) s += "pending_launch=" + cfg.pending_launch + "\n";
    append_settings(s, cfg.defaults);
    return write_file(path, s);
}

bool load_game_settings(const std::string &dir, const std::string &game, Settings &out)
{
    const std::string text = read_file(dir + "/" + sanitise(game) + ".cfg");
    if (text.empty()) return false;
    read_settings(parse_kv(text), out);
    return true;
}

bool save_game_settings(const std::string &dir, const std::string &game, const Settings &s)
{
    SDL_CreateDirectory(dir.c_str());
    std::string body = "# per-game overrides for: " + game + "\n";
    append_settings(body, s);
    return write_file(dir + "/" + sanitise(game) + ".cfg", body);
}

std::string build_conf(const Settings &s, const std::string &title,
                       const std::string &mount_dir, const std::string &run_cmd,
                       bool run_raw, const std::string &extra_sections)
{
    std::string c;

    c += "[sdl]\n";
    /* The engine renders OFFSCREEN and the frontend owns the window. Both
     * lines are required: output=gamelink without 'gamelink master=true'
     * silently falls back to output=surface, and then the engine renders into
     * a window nobody reads -- a black screen with no error anywhere. */
    c += "output=gamelink\n";
    c += "gamelink master=true\n";
    c += "autolock=false\n";
    c += "waitonerror=false\n";
    c += "showmenu=false\n";

    c += "[dosbox]\n";
    /* DOSBox-X's welcome banner points at UPSTREAM's issue tracker
     * (github.com/joncampbell123/dosbox-x/issues). Sending our users there is
     * wrong twice over: they cannot report a Retro-DOS bug to that project,
     * and it is the first thing on screen in every store screenshot. The
     * credit belongs in About, which names DOSBox-X and its licence. */
    c += "startbanner=false\n";
    /* Not optional. With a non-TTY stdin DOSBox-X decides to prompt for a
     * working directory and blocks in a folder picker; on Android there is
     * neither stdin nor a dialog, so it hangs with no diagnostic at all. */
    c += "working directory option=noprompt\n";
    c += "memsize=" + std::to_string(s.memsize) + "\n";
    c += "title=" + title + "\n";

    c += "[cpu]\n";
    /* "auto", never "dynamic", when the user asks for the fast core.
     *
     * The dynamic core recompiles x86 blocks into native code at run time, and
     * iOS does not permit memory that is both writable and executable -- the
     * wall that keeps every App Store emulator interpreter-only. ios/
     * build-core.sh therefore passes --disable-dynamic-core, --disable-dynamic-x86
     * and --disable-dynrec, so naming "dynamic" there asks for a core that is
     * not in the binary at all.
     *
     * "auto" asks DOSBox-X for the best core it actually has: the dynamic one
     * where a dynamic core was built, the interpreter where none was. So this
     * is unchanged on Android, which builds them, and correct on iOS, which
     * cannot.
     *
     * This is not a new lesson. The Flutter build hit it and fixed it in
     * fc3456b, kept as tag archive/flutter-app; the rewrite reintroduced it. */
    c += std::string("core=") + (s.core_dynamic ? "auto" : "normal") + "\n";
    if (s.cycles_max) c += "cycles=max\n";
    else              c += "cycles=fixed " + std::to_string(s.cycles_fixed) + "\n";

    c += "[sblaster]\n";
    c += "sbtype=" + s.sbtype + "\n";

    c += "[joystick]\n";
    /* "none" unless the pad is actually driving the game port. A DOS game that
     * probes the port and finds a stick behaves differently from one that finds
     * nothing -- several auto-select joystick control and then steer on their
     * own from an axis nobody is touching. So the port only exists when the
     * player has asked for it. */
    c += std::string("joysticktype=") + (s.pad_sends_joystick ? "2axis" : "none") + "\n";
    c += "timed=false\n";   /* untimed axes are what a synthesised stick wants */
    c += "autofire=false\n";

    /* Whatever the game shipped, last, so it wins over the defaults above. */
    if (!extra_sections.empty()) c += extra_sections;

    c += "[autoexec]\n";
    /* @echo off, and MOUNT -q.
     *
     * Without them the first thing a player sees is our own plumbing: the
     * shell echoes `mount C "..."` with the full container path, which on iOS
     * is sixty characters of UUID under /var/mobile/Containers, and MOUNT then
     * prints the same path back as "Drive C is mounted as local directory".
     * Neither is anything the user chose or can act on. The game's own output
     * is unaffected -- echo off only silences the commands WE issue. */
    c += "@echo off\n";
    c += "mount -q C \"" + mount_dir + "\"\n";
    c += "C:\n";
    if (!run_cmd.empty()) {
        /* A program name is quoted because DOS titles are full of spaces; a
         * built-in command with arguments must not be, or it is looked up as
         * one long filename. */
        if (run_raw) c += run_cmd + "\n";
        else         c += "\"" + run_cmd + "\"\n";
    }

    return c;
}

} /* namespace retrodos */

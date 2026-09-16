/*
 * retro-dosbox frontend — SDL3 + Dear ImGui.
 *
 * This owns the window. The engine runs HEADLESS behind it through the Game
 * Link output and hands us finished frames via the retrodos_host framebuffer
 * tap. That arrangement buys three things at once:
 *
 *   - DOSBox-X's own menu bar and config GUI never appear, because there is no
 *     DOSBox-X window for them to be drawn into.
 *   - The DOS picture is SCALED. With --disable-opengl the engine's only
 *     on-screen backend is `surface`, which blits 1:1 and leaves a 720x400
 *     picture in the corner of a 1920x1080 handheld.
 *   - There is somewhere to put a wizard, a library and a keyboard, which an
 *     emulator that boots straight into DOS has nowhere to host.
 *
 * Two threads: the engine thread runs the blocking DOSBox-X mainloop; this one
 * does SDL, ImGui and the frame blit. Everything sent to the engine goes
 * through retrodos_host_*, which queues rather than touching emulator state.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_internal.h"     /* ClearActiveID, for drag-scrolling lists */
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "retrodos_host.h"
#include "retrodos_brand.h"
#include "retrodos_win98.h"
#include "retrodos_config.h"
#include "retrodos_osk.h"
#include "retrodos_saf.h"
#include "retrodos_demo.h"
#include "retrodos_media.h"
#include "retrodos_pad.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "retrodos", __VA_ARGS__)
#else
#define LOGI(...) do { SDL_Log(__VA_ARGS__); } while (0)
#endif

using retrodos::AppConfig;
using retrodos::Settings;

namespace {

/* ------------------------------------------------------------------ */
/* Library                                                             */
/* ------------------------------------------------------------------ */

struct Game {
    std::string name;
    std::string dir;          /* real path; empty for a SAF game until staged */
    std::string run;          /* DOS command, may be empty */
    char        initial = '#';
    bool        from_saf = false;
    bool        run_raw  = false;  /* emit `run` unquoted (a DOSBox-X command) */
    std::string autoexec;          /* [autoexec] from the game's own dosbox.conf */
    std::string audio_profile;     /* its sound sections, verbatim */
    bool        is_demo  = false;  /* bundled content: never staged, never scanned */
    std::string slug;              /* RetroMedia catalogue slug, when matched */
    /* A complete dosbox.conf to use verbatim instead of building one from the
     * Settings. A Windows guest needs sections a DOS game never wants -- a
     * reported DOS version of 7.1, int13 v86 fakery, a particular Sound
     * Blaster -- and threading those through build_conf as special cases would
     * put Windows-only behaviour in every game's way. */
    std::string conf_override;
};

bool ends_with_ci(const std::string &s, const char *suffix)
{
    const size_t n = SDL_strlen(suffix);
    if (s.size() < n) return false;
    return SDL_strncasecmp(s.c_str() + s.size() - n, suffix, n) == 0;
}

/* Reduce a title to something two sources can agree on.
 *
 * A folder on the card is called "DOOM2 (1994) [v1.9]" and the catalogue calls
 * it "Doom II (1994)". Nothing matches on raw strings, so both sides are
 * lowercased, stripped of bracketed qualifiers, and reduced to letters and
 * digits before they are compared. */
std::string canon(const std::string &s)
{
    std::string out;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth) continue;
        /* Drop an archive extension: the folder and the .zip of the same game
         * must reduce to the same key. */
        if (c == '.' && (ends_with_ci(s, ".zip") || ends_with_ci(s, ".exe")) &&
            i + 4 == s.size())
            break;
        if (SDL_isalnum((unsigned char)c))
            out += (char)SDL_tolower((unsigned char)c);
    }
    /* "The Secret of Monkey Island" vs "Secret of Monkey Island, The". */
    if (out.compare(0, 3, "the") == 0 && out.size() > 3) out = out.substr(3);
    return out;
}

/**
 * The [autoexec] block of a dosbox.conf sitting in the game's own folder.
 *
 * Collections in this format ship a per-game conf that states exactly how the
 * title starts -- mounting its CD image, picking a sound driver, calling the
 * right batch file. That is authoritative, and far better than inferring a
 * launch from whatever executables happen to be lying around: Descent's folder
 * alone holds ASKECHO.COM, JCHOICE.EXE, network.bat and run.bat, and only one
 * of those starts the game.
 *
 * The block is returned verbatim. These confs do not mount C: themselves --
 * they assume the launcher has already mounted the game folder there, which is
 * exactly what build_conf does.
 */
std::string read_conf_section(const std::string &text, const char *section)
{
    const size_t start = text.find(section);
    if (start == std::string::npos) return std::string();

    std::string out;
    size_t i = text.find('\n', start);
    while (i != std::string::npos && i + 1 <= text.size()) {
        size_t e = text.find('\n', ++i);
        std::string line = text.substr(i, (e == std::string::npos ? text.size() : e) - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') break;
        if (!line.empty() && line[0] != '#') out += line + "\n";
        if (e == std::string::npos) break;
        i = e;
    }
    return out;
}

/**
 * The sound settings a game ships with.
 *
 * These packs tune the sound card per title -- Descent asks for sb16 at
 * 220/7/1 with OPL3 and a 49716Hz mixer, which is what the game was configured
 * against when it was packaged. Overriding all of that with one global default
 * is what leaves a game silent: the guest is driving hardware at an address the
 * emulator did not put a card on.
 *
 * Only the audio sections are taken. Video and CPU stay under our control,
 * because those interact with the framebuffer tap and with settings the user
 * can see and change.
 */
std::string bundled_audio_profile(const std::string &dir)
{
    const std::string path = dir + "/dosbox.conf";
    size_t len = 0;
    void *data = SDL_LoadFile(path.c_str(), &len);
    if (!data) return std::string();
    const std::string text((const char *)data, len);
    SDL_free(data);

    static const char *kSections[] = { "[sblaster]", "[mixer]", "[midi]",
                                       "[speaker]", "[gus]" };
    std::string out;
    for (const char *s : kSections) {
        const std::string body = read_conf_section(text, s);
        if (!body.empty()) { out += s; out += "\n"; out += body; }
    }
    return out;
}

std::string bundled_autoexec(const std::string &dir)
{
    const std::string path = dir + "/dosbox.conf";
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "r");
    if (!io) return std::string();

    size_t len = 0;
    void *data = SDL_LoadFile_IO(io, &len, true);   /* closes io */
    if (!data) return std::string();
    const std::string text((const char *)data, len);
    SDL_free(data);

    const size_t start = text.find("[autoexec]");
    if (start == std::string::npos) return std::string();

    std::string out;
    size_t i = text.find('\n', start);
    while (i != std::string::npos && i + 1 <= text.size()) {
        size_t e = text.find('\n', ++i);
        std::string line = text.substr(i, (e == std::string::npos ? text.size() : e) - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        /* Another section header ends the block. */
        if (!line.empty() && line[0] == '[') break;

        /* These confs are written for Windows, so a HOST path in them uses
         * backslashes -- "imgmount d .\cd\descent.cue". On Android that is one
         * literal filename and the mount silently fails, taking the game's CD
         * with it. Only mount/imgmount arguments are host paths; a backslash
         * anywhere else is a DOS path and must be left alone. */
        if (SDL_strncasecmp(line.c_str(), "imgmount", 8) == 0 ||
            SDL_strncasecmp(line.c_str(), "mount", 5) == 0) {
            for (char &c : line) if (c == '\\') c = '/';
        }

        if (!line.empty()) out += line + "\n";
        if (e == std::string::npos) break;
        i = e;
    }
    return out;
}

/* Batch files that are plainly not the game. Collections leave installers and
 * multiplayer helpers beside the real launcher, and picking one of those looks
 * to the user like the emulator failing to start the title. */
bool is_support_script(const std::string &f)
{
    static const char *kSkip[] = { "install", "setup", "uninstall", "network",
                                   "config", "readme", "modem", "serial" };
    for (const char *s : kSkip)
        if (SDL_strcasestr(f.c_str(), s)) return true;
    return false;
}

/* .BAT first: installers habitually leave a one-line batch file that sets up
 * the environment the .EXE expects, and running the .EXE directly then fails
 * in ways that look like emulation bugs. */
void find_runnable(const std::string &dir, Game &g)
{
    /* A conf shipped with the game wins over anything guessed from the
     * directory listing. */
    g.autoexec      = bundled_autoexec(dir);
    g.audio_profile = bundled_audio_profile(dir);
    if (!g.autoexec.empty()) {
        g.run = "game profile";     /* label only; the conf drives the launch */
        return;
    }

    int n = 0;
    char **files = SDL_GlobDirectory(dir.c_str(), NULL, SDL_GLOB_CASEINSENSITIVE, &n);
    if (!files) return;

    std::string bat, com, exe;
    std::string fallback_bat, fallback_com, fallback_exe;
    for (int i = 0; i < n; ++i) {
        const std::string f = files[i];
        /* This glob recurses too. A nested hit is useless here: the name goes
         * straight into autoexec, where DOS needs a backslash path relative to
         * the mounted drive, so "DESCENT/DESCENT.BAT" would simply not run. */
        if (f.find('/') != std::string::npos) continue;
        if (ends_with_ci(f, ".bat")) {
            /* "run.bat"/"start.bat" beat an alphabetically earlier helper. */
            const bool preferred = SDL_strcasestr(f.c_str(), "run") ||
                                   SDL_strcasestr(f.c_str(), "start") ||
                                   SDL_strcasestr(f.c_str(), "play");
            if (is_support_script(f)) { if (fallback_bat.empty()) fallback_bat = f; }
            else if (bat.empty() || preferred) { if (preferred || bat.empty()) bat = f; }
        }
        else if (ends_with_ci(f, ".com")) {
            if (is_support_script(f)) { if (fallback_com.empty()) fallback_com = f; }
            else if (com.empty()) com = f;
        }
        else if (ends_with_ci(f, ".exe")) {
            if (is_support_script(f)) { if (fallback_exe.empty()) fallback_exe = f; }
            else if (exe.empty()) exe = f;
        }
    }
    SDL_free(files);

    /* A setup or install program is a last resort, not a first choice -- but it
     * IS a resort. Some titles ship nothing else, and refusing to offer the one
     * executable present leaves the game unstartable. */
    if (bat.empty()) bat = fallback_bat;
    if (com.empty()) com = fallback_com;
    if (exe.empty()) exe = fallback_exe;
    g.run = !bat.empty() ? bat : (!com.empty() ? com : exe);
}

/* A game is a directory directly inside the library root -- one level, never
 * deeper.
 *
 * readdir, NOT SDL_GlobDirectory: the latter RECURSES. With flat test folders
 * that went unnoticed, but the first real game exposed it -- Descent's own
 * cd/, DESCENT/, DESCENT/SB16/ and the ~/.config tree DOSBox-X leaves behind
 * each turned into a separate library entry, so one download produced eight
 * rows and only the first of them could start. */
std::vector<Game> scan_library(const std::string &root)
{
    std::vector<Game> games;
    if (root.empty()) return games;

#if !defined(_WIN32)
    DIR *d = opendir(root.c_str());
    if (!d) return games;

    std::vector<std::string> entries;
    while (struct dirent *e = readdir(d)) entries.push_back(e->d_name);
    closedir(d);

    for (const std::string &name : entries) {
        if (name == "." || name == "..") continue;
#else
    int n = 0;
    char **glob = SDL_GlobDirectory(root.c_str(), "*", 0, &n);
    if (!glob) return games;

    for (int i = 0; i < n; ++i) {
        const std::string name = glob[i];
        if (name == "." || name == ".." ||
            name.find('/') != std::string::npos) continue;
#endif
        /* Skip dotfiles and the stray '~' trees DOSBox-X leaves behind when
         * handed a HOME it cannot resolve -- they are not games. */
        if (name[0] == '.' || name[0] == '~') continue;

        const std::string full = root + "/" + name;
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(full.c_str(), &info)) continue;
        if (info.type != SDL_PATHTYPE_DIRECTORY) continue;

        Game g;
        g.name = name;
        g.dir  = full;
        find_runnable(full, g);
        const char c = (char)SDL_toupper((unsigned char)name[0]);
        g.initial = (c >= 'A' && c <= 'Z') ? c : '#';
        games.push_back(g);
    }
#if defined(_WIN32)
    SDL_free(glob);
#endif

    std::sort(games.begin(), games.end(), [](const Game &a, const Game &b) {
        return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return games;
}

/* Games that live in a SAF-granted tree. Only names are known here: the
 * contents are not reachable by path until the title is staged, so the
 * runnable is discovered after staging rather than now. */
std::vector<Game> scan_saf_library()
{
    std::vector<Game> games;
    for (const std::string &name : retrodos::saf_list_games()) {
        if (name.empty() || name[0] == '.') continue;
        Game g;
        g.name     = name;
        g.from_saf = true;
        const char c = (char)SDL_toupper((unsigned char)name[0]);
        g.initial = (c >= 'A' && c <= 'Z') ? c : '#';
        games.push_back(g);
    }
    return games;
}

/* ------------------------------------------------------------------ */
/* Candidate library roots                                             */
/* ------------------------------------------------------------------ */

/* App-specific directories on EVERY volume need no permission and are real
 * filesystem paths -- which is what matters, because DOSBox-X mounts a
 * directory BY PATH and cannot be handed a SAF content:// URI. Anything
 * outside these needs a granted document tree, and then the game has to be
 * staged into one of these before it can be mounted at all. */
std::vector<std::string> candidate_roots()
{
    std::vector<std::string> out;
#if defined(__ANDROID__)
    if (const char *ext = SDL_GetAndroidExternalStoragePath())
        out.push_back(std::string(ext) + "/dos");

    /* The same app-private directory on removable storage: where a large
     * collection realistically lives on a handheld. */
    /* readdir, NOT SDL_GlobDirectory: the latter stats every entry, and
     * /storage/self is a self-referential symlink that makes it hang outright
     * -- the app starts, SDL comes up, and the first frame never arrives.
     * readdir only needs the names, which is all this wants. */
    if (DIR *d = opendir("/storage")) {
        while (struct dirent *e = readdir(d)) {
            const std::string v = e->d_name;
            if (v == "emulated" || v == "self" || v == "." || v == "..") continue;
            out.push_back("/storage/" + v +
                          "/Android/data/com.crownparkcomputing.retrodos/files/dos");
        }
        closedir(d);
    }
#elif defined(__APPLE__)
    /* Documents, NOT the pref path.
     *
     * UIFileSharingEnabled and LSSupportsOpeningDocumentsInPlace expose the
     * app's Documents directory in the Files app, and ONLY that one. The pref
     * path is Library/Application Support, which the user cannot reach from
     * Files at all -- so a library there could never have a game added to it.
     *
     * SDL_FOLDER_DOCUMENTS is that directory on iOS. The pref path stays as a
     * fallback in case it cannot be resolved, because a library in an
     * unreachable folder still beats no library at all. */
    if (const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS))
        out.push_back(std::string(docs) + "dos");
    if (out.empty()) {
        if (char *pref = SDL_GetPrefPath("CrownParkComputing", "Retro-DOS")) {
            out.push_back(std::string(pref) + "dos");
            SDL_free(pref);
        }
    }
#else
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", "Retro-DOS")) {
        out.push_back(std::string(pref) + "dos");
        SDL_free(pref);
    }
#endif
    return out;
}

/* TextDisabled that WRAPS.
 *
 * ImGui::TextDisabled does not wrap, so the dimmed explanatory blocks here
 * were written with hard newlines sized for a landscape handheld -- and ran
 * straight off the right edge on a phone held upright. Wrapping lets the same
 * sentence fit any width. */
void TextDimWrapped(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/* Dragging the list scrolls the list.
 *
 * ImGui's answer to overflow is a scrollbar, and on a touch screen that is the
 * wrong answer: the bar is a few pixels wide, and the natural gesture -- put a
 * finger on the content and pull -- either does nothing or presses whatever
 * row it landed on. Called inside a scrolling region each frame, this turns a
 * mostly-vertical drag into scrolling and cancels the press it started on, so
 * a tap still selects and a pull never does.
 *
 * Horizontal drags are left alone deliberately: that is how a slider is set,
 * and stealing it would break every slider that shares a page with this. */
void scroll_by_drag()
{
    ImGuiIO &io = ImGui::GetIO();
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        return;
    /* Below the threshold a touch is a tap; only past it does it become a
     * scroll. Scaled from the font so it tracks the UI scale. */
    const float thr = ImGui::GetFontSize() * 0.35f;
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, thr)) return;
    const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, thr);
    if (SDL_fabsf(d.y) <= SDL_fabsf(d.x)) return;
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    /* The row the finger started on is mid-press by now; without this the
     * scroll would end by activating it. */
    ImGui::ClearActiveID();
}

/* What the user should see as "the library". Once a folder is granted, the
 * app-private path is only a staging area and showing it is actively
 * misleading -- it is not where their games are. */
std::string library_label(const std::string &root)
{
    const std::string uri = retrodos::saf_tree_uri();
    if (uri.empty()) return root;

    /* content://...tree/FEDD-B1FF%3ADOS%20Games...%2FGames -- decode enough of
     * the tail to be recognisable rather than showing a raw URI. */
    std::string t = uri.substr(uri.find_last_of('/') + 1);
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '%' && i + 2 < t.size()) {
            const int hi = SDL_isdigit(t[i+1]) ? t[i+1]-'0' : (SDL_toupper(t[i+1])-'A'+10);
            const int lo = SDL_isdigit(t[i+2]) ? t[i+2]-'0' : (SDL_toupper(t[i+2])-'A'+10);
            const char c = (char)(hi * 16 + lo);
            out += (c == ':') ? '/' : c;
            i += 2;
        } else out += t[i];
    }
    return out;
}

/* A path the user can ACT on.
 *
 * The container path is true and useless: on iOS it is sixty characters of
 * UUID under /var/mobile/Containers, and no part of it can be typed into the
 * Files app. What the user needs is the name they will actually see there.
 * Everywhere else the real path is the useful answer, so it is left alone. */
std::string root_label(const std::string &root)
{
#if defined(__APPLE__)
    const std::string tail = root.substr(root.find_last_of('/') + 1);
    return "Retro-DOS  >  " + (tail.empty() ? std::string("dos") : tail)
         + "   (in the Files app)";
#else
    /* Android: defer to library_label, which decodes a granted SAF tree into
     * something readable and returns the plain path when there is no grant. */
    return library_label(root);
#endif
}


/* ------------------------------------------------------------------ */
/* Pad layout persistence                                              */
/* ------------------------------------------------------------------ */

/* "button,x,y,r;..." -- fractions, so a layout moved on one screen is still
 * where the thumb expects it on another. */
std::string pad_layout_to_string(const std::vector<retrodos::PadControl> &v)
{
    std::string s;
    char buf[96];
    for (const retrodos::PadControl &c : v) {
        SDL_snprintf(buf, sizeof(buf), "%d,%.4f,%.4f,%.4f;",
                     c.button, c.x, c.y, c.radius);
        s += buf;
    }
    return s;
}

bool pad_layout_from_string(const std::string &s,
                            std::vector<retrodos::PadControl> &out)
{
    std::vector<retrodos::PadControl> v;
    size_t i = 0;
    while (i < s.size()) {
        const size_t e = s.find(';', i);
        if (e == std::string::npos) break;
        const std::string rec = s.substr(i, e - i);
        i = e + 1;

        int b = 0; float x = 0, y = 0, r = 0;
        if (SDL_sscanf(rec.c_str(), "%d,%f,%f,%f", &b, &x, &y, &r) != 4) continue;
        if (b < 0 || b >= retrodos::PAD_COUNT) continue;

        retrodos::PadControl c;
        c.button = b;
        c.label  = retrodos::pad_button_name(b);
        /* The default layout's labels are terser than the button names. */
        switch (b) {
        case retrodos::PAD_UP:     c.label = "^";   break;
        case retrodos::PAD_DOWN:   c.label = "v";   break;
        case retrodos::PAD_LEFT:   c.label = "<";   break;
        case retrodos::PAD_RIGHT:  c.label = ">";   break;
        case retrodos::PAD_START:  c.label = "Ent"; break;
        case retrodos::PAD_SELECT: c.label = "Esc"; break;
        default: break;
        }
        c.x = x; c.y = y; c.radius = r;
        v.push_back(c);
    }
    if (v.empty()) return false;
    out.swap(v);
    return true;
}

bool path_is_dir(const std::string &p)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

/* ------------------------------------------------------------------ */
/* Engine thread                                                       */
/* ------------------------------------------------------------------ */

std::atomic<bool> g_engine_done{false};

void engine_thread(std::string conf, std::string workdir)
{
    /* Deliberately NOT lending the engine our window: a borrowed window is one
     * it will reconfigure, present to and destroy, and any of those silently
     * kills the host's renderer. */
    retrodos_host_set_window(nullptr);

    char a0[] = "dosbox-x";
    char a1[] = "-conf";
    char a3[] = "-defaultdir";
    std::vector<char> c(conf.begin(), conf.end());       c.push_back('\0');
    std::vector<char> d(workdir.begin(), workdir.end()); d.push_back('\0');
    char *argv[] = { a0, a1, c.data(), a3, d.data(), nullptr };

    retrodos_host_set_framebuffer_output(true);
    retrodos_host_run(5, argv);
    g_engine_done.store(true);
}

/* ------------------------------------------------------------------ */
/* Presentation                                                        */
/* ------------------------------------------------------------------ */

SDL_FRect fit(int fb_w, int fb_h, int aspect_x1000, int win_w, int win_h,
              const Settings &s)
{
    /* DOS modes are frequently non-square-pixel: 320x200 is a 4:3 picture, not
     * 16:10. Ignoring the ratio the engine reports is the classic DOS-emulator
     * tell -- but "Auto" is not always what the player wants on a widescreen
     * handheld, which is why the other modes exist. */
    double ratio;
    switch (s.aspect_mode) {
    case 1:  ratio = 4.0 / 3.0;  break;
    case 2:  ratio = 16.0 / 9.0; break;
    case 3:                       /* Fill: no letterboxing at all */
        {
            SDL_FRect r;
            r.x = 0.0f; r.y = 0.0f;
            r.w = (float)win_w; r.h = (float)win_h;
            return r;
        }
    default:
        ratio = (aspect_x1000 > 0) ? (aspect_x1000 / 1000.0)
              : (fb_h > 0 ? ((double)fb_w / (double)fb_h) : (4.0 / 3.0));
        break;
    }
    if (ratio <= 0.0) ratio = 4.0 / 3.0;

    double w = (double)win_w, h = w / ratio;
    if (h > (double)win_h) { h = (double)win_h; w = h * ratio; }

    if (s.integer_scale && fb_w > 0 && fb_h > 0) {
        const int k = std::max(1, std::min(win_w / fb_w, win_h / fb_h));
        w = (double)(fb_w * k);
        h = (double)(fb_h * k);
    }

    SDL_FRect r;
    r.w = (float)w; r.h = (float)h;
    r.x = (float)((win_w - w) * 0.5);
    r.y = (float)((win_h - h) * 0.5);
    return r;
}

/* ------------------------------------------------------------------ */
/* Settings UI (shared by global defaults and per-game overrides)      */
/* ------------------------------------------------------------------ */

/* The keys a DOS game is actually bound to, offered as a list.
 *
 * A "press the key you want" binder is the obvious design and the wrong one
 * here: the device this runs on has no keyboard, so there would be no way to
 * answer the prompt. */
struct KeyChoice { int scancode; const char *name; };

const KeyChoice kKeyChoices[] = {
    { 0,                       "(none)"    },
    { SDL_SCANCODE_UP,         "Up"        },
    { SDL_SCANCODE_DOWN,       "Down"      },
    { SDL_SCANCODE_LEFT,       "Left"      },
    { SDL_SCANCODE_RIGHT,      "Right"     },
    { SDL_SCANCODE_LCTRL,      "Ctrl"      },
    { SDL_SCANCODE_LALT,       "Alt"       },
    { SDL_SCANCODE_LSHIFT,     "Shift"     },
    { SDL_SCANCODE_SPACE,      "Space"     },
    { SDL_SCANCODE_RETURN,     "Enter"     },
    { SDL_SCANCODE_ESCAPE,     "Esc"       },
    { SDL_SCANCODE_TAB,        "Tab"       },
    { SDL_SCANCODE_BACKSPACE,  "Backspace" },
    { SDL_SCANCODE_PAGEUP,     "PgUp"      },
    { SDL_SCANCODE_PAGEDOWN,   "PgDn"      },
    { SDL_SCANCODE_HOME,       "Home"      },
    { SDL_SCANCODE_END,        "End"       },
    { SDL_SCANCODE_INSERT,     "Insert"    },
    { SDL_SCANCODE_DELETE,     "Delete"    },
    { SDL_SCANCODE_A, "A" }, { SDL_SCANCODE_B, "B" }, { SDL_SCANCODE_C, "C" },
    { SDL_SCANCODE_D, "D" }, { SDL_SCANCODE_E, "E" }, { SDL_SCANCODE_F, "F" },
    { SDL_SCANCODE_G, "G" }, { SDL_SCANCODE_H, "H" }, { SDL_SCANCODE_I, "I" },
    { SDL_SCANCODE_J, "J" }, { SDL_SCANCODE_K, "K" }, { SDL_SCANCODE_L, "L" },
    { SDL_SCANCODE_M, "M" }, { SDL_SCANCODE_N, "N" }, { SDL_SCANCODE_O, "O" },
    { SDL_SCANCODE_P, "P" }, { SDL_SCANCODE_Q, "Q" }, { SDL_SCANCODE_R, "R" },
    { SDL_SCANCODE_S, "S" }, { SDL_SCANCODE_T, "T" }, { SDL_SCANCODE_U, "U" },
    { SDL_SCANCODE_V, "V" }, { SDL_SCANCODE_W, "W" }, { SDL_SCANCODE_X, "X" },
    { SDL_SCANCODE_Y, "Y" }, { SDL_SCANCODE_Z, "Z" },
    { SDL_SCANCODE_1, "1" }, { SDL_SCANCODE_2, "2" }, { SDL_SCANCODE_3, "3" },
    { SDL_SCANCODE_4, "4" }, { SDL_SCANCODE_5, "5" }, { SDL_SCANCODE_6, "6" },
    { SDL_SCANCODE_7, "7" }, { SDL_SCANCODE_8, "8" }, { SDL_SCANCODE_9, "9" },
    { SDL_SCANCODE_0, "0" },
    { SDL_SCANCODE_F1, "F1" }, { SDL_SCANCODE_F2, "F2" }, { SDL_SCANCODE_F3, "F3" },
    { SDL_SCANCODE_F4, "F4" }, { SDL_SCANCODE_F5, "F5" }, { SDL_SCANCODE_F6, "F6" },
    { SDL_SCANCODE_F7, "F7" }, { SDL_SCANCODE_F8, "F8" }, { SDL_SCANCODE_F9, "F9" },
    { SDL_SCANCODE_F10, "F10" }, { SDL_SCANCODE_F11, "F11" }, { SDL_SCANCODE_F12, "F12" },
    /* The rest of the keyboard. A game's setup can demand any key at all --
     * flight sims bind the keypad, Build games bind punctuation -- and a
     * binder that cannot offer the key the game insists on is a dead end. */
    { SDL_SCANCODE_RCTRL,        "RCtrl"    },
    { SDL_SCANCODE_RALT,         "RAlt"     },
    { SDL_SCANCODE_RSHIFT,       "RShift"   },
    { SDL_SCANCODE_COMMA,        ","        },
    { SDL_SCANCODE_PERIOD,       "."        },
    { SDL_SCANCODE_SLASH,        "/"        },
    { SDL_SCANCODE_SEMICOLON,    ";"        },
    { SDL_SCANCODE_APOSTROPHE,   "'"        },
    { SDL_SCANCODE_LEFTBRACKET,  "["        },
    { SDL_SCANCODE_RIGHTBRACKET, "]"        },
    { SDL_SCANCODE_MINUS,        "-"        },
    { SDL_SCANCODE_EQUALS,       "="        },
    { SDL_SCANCODE_GRAVE,        "`"        },
    { SDL_SCANCODE_BACKSLASH,    "\\"       },
    { SDL_SCANCODE_KP_0, "KP 0" }, { SDL_SCANCODE_KP_1, "KP 1" },
    { SDL_SCANCODE_KP_2, "KP 2" }, { SDL_SCANCODE_KP_3, "KP 3" },
    { SDL_SCANCODE_KP_4, "KP 4" }, { SDL_SCANCODE_KP_5, "KP 5" },
    { SDL_SCANCODE_KP_6, "KP 6" }, { SDL_SCANCODE_KP_7, "KP 7" },
    { SDL_SCANCODE_KP_8, "KP 8" }, { SDL_SCANCODE_KP_9, "KP 9" },
    { SDL_SCANCODE_KP_ENTER,    "KP Enter" },
    { SDL_SCANCODE_KP_PLUS,     "KP +"     },
    { SDL_SCANCODE_KP_MINUS,    "KP -"     },
    { SDL_SCANCODE_KP_MULTIPLY, "KP *"     },
    { SDL_SCANCODE_KP_DIVIDE,   "KP /"     },
    { SDL_SCANCODE_KP_PERIOD,   "KP ."     },
};

const char *key_name(int scancode)
{
    for (const KeyChoice &k : kKeyChoices)
        if (k.scancode == scancode) return k.name;
    return "(other)";
}

void controls_widgets(Settings &s)
{
    ImGui::TextUnformatted("Controls");
    ImGui::Checkbox("On-screen controls when no gamepad is connected",
                    &s.onscreen_pad);
    ImGui::TextDisabled("A connected gamepad hides them automatically.");

    ImGui::Spacing();
    ImGui::Checkbox("Buttons send keys", &s.pad_sends_keys);
    ImGui::Checkbox("Buttons drive the joystick port", &s.pad_sends_joystick);
    TextDimWrapped("Most DOS games are keyboard games, so keys are the default. "
                   "Turn the joystick on for titles that support one: the game "
                   "then finds a stick on the game port from the moment it "
                   "starts, with A and B as its two buttons.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Key for each button");

    /* Two columns. Fourteen stacked combos do not fit a handheld screen, and a
     * settings page that scrolls hides half its own controls. */
    {
        const float col = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
        const int   half = (retrodos::PAD_COUNT + 1) / 2;
        for (int row = 0; row < half; ++row) {
            for (int c = 0; c < 2; ++c) {
                const int b = row + c * half;
                if (b >= retrodos::PAD_COUNT) break;
                if (c) ImGui::SameLine(0.0f, 12.0f);
                ImGui::PushID(b);
                ImGui::SetNextItemWidth(col - ImGui::GetFontSize() * 4.5f);
                if (ImGui::BeginCombo(retrodos::pad_button_name(b),
                                      key_name(s.pad_keys[b]))) {
                    /* The popup is its own window, and the key list is long:
                     * a finger must be able to pull it, not hunt the bar. */
                    scroll_by_drag();
                    for (const KeyChoice &k : kKeyChoices) {
                        const bool sel = (s.pad_keys[b] == k.scancode);
                        if (ImGui::Selectable(k.name, sel)) s.pad_keys[b] = k.scancode;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        }
    }

    if (ImGui::Button("DOS defaults", ImVec2(0, 0)))
        retrodos::default_pad_keys(s.pad_keys);
    ImGui::SameLine();
    if (ImGui::Button("Descent / 6DOF", ImVec2(0, 0)))
        retrodos::descent_pad_keys(s.pad_keys);
    ImGui::TextDisabled("Descent flies in six degrees of freedom, so it needs more\n"
                        "than a d-pad and two buttons: aim on the stick, weapons on\n"
                        "the triggers, throttle on A/B, roll on the shoulders.");
}

/* A combo over a fixed list of DOSBox-X config values.
 *
 * The value written to the conf is the string itself, not an index: an index
 * would silently change meaning the day a value is inserted into the list, and
 * the config file is meant to stay hand-editable. */
void conf_combo(const char *label, std::string &value,
                const char *const *values, const char *const *labels, int count)
{
    int cur = 0;
    for (int i = 0; i < count; ++i)
        if (value == values[i]) { cur = i; break; }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::Combo(label, &cur, labels, count)) value = values[cur];
}

void settings_widgets(Settings &s)
{
    /* ---------------------------------------------------------------- */
    ImGui::TextUnformatted("Machine");
    {
        static const char *vals[] = { "svga_s3", "svga_s3trio64", "vesa_vbe3",
                                      "vgaonly", "ega", "cga", "tandy", "pcjr" };
        static const char *names[] = { "SVGA (S3 Virge) - default", "SVGA (S3 Trio64)",
                                       "VESA VBE 3", "VGA only", "EGA", "CGA",
                                       "Tandy 1000", "PCjr" };
        conf_combo("Graphics", s.machine, vals, names, (int)SDL_arraysize(vals));
    }
    TextDimWrapped("The first thing to change when a game starts and draws "
                   "nothing. Titles written for CGA or Tandy can be confused "
                   "by an S3, and a few refuse it outright.");

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("CPU");
#if defined(__APPLE__)
    /* There is no dynamic core on iOS to offer. It recompiles x86 into native
     * code at run time, which needs memory that is both writable and
     * executable, and iOS does not permit that -- so the core is built without
     * it and the interpreter is the only one there is. Offering a "faster"
     * switch that cannot make anything faster is worse than offering nothing:
     * a player toggling it and seeing no change will reasonably conclude the
     * setting is broken. */
    TextDimWrapped("Interpreter core -- iOS does not permit the just-in-time "
                   "recompilation a faster core needs.");
#else
    ImGui::Checkbox("Dynamic core (faster; turn off if a game misbehaves)", &s.core_dynamic);
#endif
    ImGui::Checkbox("Cycles: max", &s.cycles_max);
    if (!s.cycles_max) {
        ImGui::SliderInt("Fixed cycles", &s.cycles_fixed, 300, 100000);
        TextDimWrapped("Early titles busy-wait for timing and run absurdly fast "
                       "on 'max'. A fixed count is what fixes them.");
    }
    {
        static const char *vals[] = { "auto", "386", "486old", "pentium",
                                      "pentium_mmx" };
        static const char *names[] = { "Auto - default", "386", "486",
                                       "Pentium", "Pentium MMX" };
        conf_combo("Reported CPU", s.cputype, vals, names, (int)SDL_arraysize(vals));
    }
    ImGui::Checkbox("Maths coprocessor (FPU)", &s.fpu);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("Memory");
    ImGui::SliderInt("Memory (MB)", &s.memsize, 1, 64);
    TextDimWrapped("32 MB is the safe default: DOS/4GW 1.97 miscalculates with "
                   "more, and several early-90s titles then refuse to start.");
    ImGui::Checkbox("Expanded memory (EMS)", &s.ems);
    ImGui::SameLine();
    ImGui::Checkbox("Upper memory (UMB)", &s.umb);
    TextDimWrapped("Both on suits most games. Turning one off is the standard "
                   "next move when a title will not load at all.");
    {
        static const char *vals[] = { "", "3.3", "5.0", "6.22", "7.0", "7.1" };
        static const char *names[] = { "Default (5.0)", "3.3", "5.0", "6.22",
                                       "7.0", "7.1 (long filenames, FAT32)" };
        conf_combo("Reported DOS", s.dos_ver, vals, names, (int)SDL_arraysize(vals));
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("Sound");
    static const char *sb[] = { "sbpro2", "sb16", "sb16vibra", "sbpro1", "sb2",
                                "sb1", "none" };
    for (int i = 0; i < (int)SDL_arraysize(sb); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(sb[i], s.sbtype == sb[i])) s.sbtype = sb[i];
    }
    ImGui::Checkbox("PC speaker", &s.pcspeaker);
    TextDimWrapped("The beeper is the only sound many pre-1990 titles have.");

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("Display");
    {
        static const char *kAspect[] = { "Auto (as the mode intends)", "4:3",
                                         "16:9", "Fill the screen" };
        int mode = (s.aspect_mode >= 0 && s.aspect_mode < 4) ? s.aspect_mode : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::Combo("Aspect", &mode, kAspect, 4)) s.aspect_mode = mode;
    }
    ImGui::Checkbox("Integer scaling (sharper, bigger borders)", &s.integer_scale);
    {
        static const char *vals[] = { "0", "1", "2", "4", "8" };
        static const char *names[] = { "Automatic - default", "1 MB", "2 MB",
                                       "4 MB", "8 MB" };
        std::string v = std::to_string(s.vmemsize);
        conf_combo("Video memory", v, vals, names, (int)SDL_arraysize(vals));
        s.vmemsize = atoi(v.c_str());
    }
    TextDimWrapped("Raise it only for a later title that wants a high VESA "
                   "mode. A number here caps what the guest can use as "
                   "readily as it raises it.");
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

#if defined(__ANDROID__)
    /* The engine narrates its startup on stdout, which on Android goes
     * nowhere -- a boot that fails early is otherwise completely silent.
     * Unbuffered, because a buffered log is lost exactly when it is the only
     * evidence left. */
    if (const char *ext = SDL_GetAndroidExternalStoragePath()) {
        char p[1024];
        SDL_snprintf(p, sizeof(p), "%s/retrodos-stdout.log", ext);
        if (freopen(p, "w", stdout)) setvbuf(stdout, NULL, _IONBF, 0);
        if (freopen(p, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGI("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window   *win = nullptr;
    SDL_Renderer *ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Retro-DOS", 1280, 720,
                                     SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE,
                                     &win, &ren)) {
        LOGI("window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   /* our own config owns durable state */

    /* Navigable by keyboard and by pad, not only by touch.
     *
     * A DOS launcher on a handheld is used with a controller as often as with
     * a finger, and an iPad with a keyboard case has no touchpad to fall back
     * on. Both flags are additive: touch keeps working exactly as it did.
     *
     * It also makes the UI drivable without a human, which is what an
     * automated store-screenshot run needs -- simctl cannot inject taps. */
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    /* Input trickling is left ON deliberately.
     *
     * Turning it off looks like the fix for a touchscreen -- apply the tap's
     * position and its press in one frame -- but it is the opposite: a tap
     * delivers move, down and up in a single batch, and without trickling all
     * three are applied to the same frame, so the button is never observed
     * down and the press is lost entirely. Trickling is what spreads them over
     * consecutive frames and makes a quick tap register at all. */
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    {   /* A handheld is held at arm's length, so the whole UI is scaled rather
         * than shipping one designed for a mouse on a desk.
         *
         * The font is RASTERISED at the target size instead of being stretched
         * with FontGlobalScale: that only magnifies the built-in 13px bitmap,
         * which stays soft and reads small however far it is pushed. Asking
         * for the size up front gives crisp glyphs. */
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        /* The SHORT side, not the height.
         *
         * Both of these describe "how big is a comfortable glyph on a screen
         * held at arm's length", and that is the short axis whichever way the
         * device is turned. Keying off height alone assumes landscape: in
         * portrait on a 1320x2868 phone it read 2868 and asked for a 110px
         * font at scale 5.3, so about twenty characters fitted a line and
         * every panel ran off the right edge.
         *
         * No effect on Android, which is locked to sensorLandscape in the
         * manifest, so the short side IS the height there. */
        const float shorter = (float)std::min(ww, wh);
        const float scale = std::max(1.0f, shorter / 540.0f);
        ImGui::GetStyle().ScaleAllSizes(scale);

        const float font_px = std::max(20.0f, shorter / 26.0f);  /* ~41px at 1080p */
        ImGui::GetIO().Fonts->Clear();
        /* The bundled face first. ImGui's built-in is ProggyClean, a 13px
         * bitmap from the 1990s -- fine for a debug overlay and wrong for an
         * application someone is meant to read at arm's length.
         *
         * The fallback is not optional: Clear() above leaves the atlas empty,
         * and building nothing at all means every later draw call asserts on a
         * null font. A missing asset must make this plainer, not fatal. */
        if (!retrodos::load_ui_font(font_px)) {
            ImFontConfig fc;
            fc.SizePixels = font_px;
            ImGui::GetIO().Fonts->AddFontDefault(&fc);
        }
        ImGui::GetIO().Fonts->Build();
        /* The backend rebuilds its font texture lazily on the next frame; in
         * this ImGui version the explicit texture calls are not public. */
        ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
    }

    /* Config lives in internal storage: it is ours, small, and must survive
     * the SD card being absent. */
    std::string cfg_dir;
#if defined(__ANDROID__)
    if (const char *in = SDL_GetAndroidInternalStoragePath()) cfg_dir = in;
#else
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", "Retro-DOS")) {
        cfg_dir = pref; SDL_free(pref);
    }
#endif
    const std::string cfg_path  = cfg_dir + "/retrodos.cfg";
    const std::string games_dir = cfg_dir + "/games";
    const std::string conf_path = cfg_dir + "/dosbox-x.conf";

    AppConfig cfg;
    retrodos::load_app_config(cfg_path, cfg);

    /* A config written before controls existed has no bindings at all, and an
     * all-zero table means every button is unbound -- the pad would draw and do
     * nothing. Seed it once; after that the user's choices persist. */
    {
        bool any = false;
        for (int i = 0; i < retrodos::PAD_COUNT; ++i)
            if (cfg.defaults.pad_keys[i]) { any = true; break; }
        if (!any) retrodos::default_pad_keys(cfg.defaults.pad_keys);
    }

    std::vector<std::string> roots = candidate_roots();
    for (const auto &r : roots) SDL_CreateDirectory(r.c_str());
    if (cfg.library_root.empty() && !roots.empty()) cfg.library_root = roots.front();

    /* Staged SAF games land here: inside the library root, hidden from the
     * scanner by the leading dot so a staged copy never shows up as a second
     * entry in the list. */
    const std::string stage_dir = cfg.library_root + "/.staged";

    /* Bundled content, unpacked out of the package to a real path because
     * DOSBox-X mounts a directory and cannot read from an APK or app bundle. */
    const std::string demo_dir = cfg_dir + "/demo";

    std::vector<Game> games;
    auto refresh = [&]() {
        /* BOTH sources, always.
         *
         * A granted SAF tree used to replace the folder scan entirely, on the
         * grounds that it is what the user picked. That was wrong the moment
         * games could be downloaded: a download lands in library_root, which is
         * a real path we can mount, and the SAF-only scan meant it could never
         * appear in the list -- the title downloaded successfully and simply
         * vanished. */
        games = scan_library(cfg.library_root);

        if (retrodos::saf_has_grant()) {
            /* A local copy wins over the SAF entry for the same title: it is
             * already a real path, so it starts without being staged first. */
            std::vector<std::string> keys;
            keys.reserve(games.size());
            for (const auto &g : games) keys.push_back(canon(g.name));

            for (auto &g : scan_saf_library()) {
                if (std::find(keys.begin(), keys.end(), canon(g.name)) != keys.end())
                    continue;
                games.push_back(g);
            }
            std::sort(games.begin(), games.end(), [](const Game &a, const Game &b) {
                return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
            });
        }

        /* Nothing found -- a fresh install, a revoked grant, or an absent SD
         * card. Offer the bundled content rather than an empty list, which
         * reads as a broken app and, for a store reviewer, IS one: there would
         * otherwise be nothing to press. Only extract when it is needed, so a
         * device with a real library never pays for it. */
        if (games.empty() && retrodos::demo_prepare(demo_dir)) {
            const retrodos::DemoKind kinds[] = { retrodos::DemoKind::Demo,
                                                 retrodos::DemoKind::FreeDos };
            for (retrodos::DemoKind k : kinds) {
                Game g;
                g.name    = retrodos::demo_title(k);
                g.dir     = demo_dir;
                g.run     = retrodos::demo_command(k, g.run_raw);
                g.is_demo = true;
                g.initial = (char)SDL_toupper((unsigned char)g.name[0]);
                games.push_back(g);
            }
        }
    };
    refresh();

    enum class View { Wizard, Shell, Emulator };
    enum class Page { Library, Artwork, Downloads, Settings, Input, Windows, Demo, About };
    View view = cfg.wizard_done ? View::Shell : View::Wizard;
    Page page = Page::Library;

    std::thread engine;
    SDL_Texture *fb_tex = nullptr;
    int fb_tex_w = 0, fb_tex_h = 0;
    uint64_t last_serial = 0;
    std::vector<uint32_t> fb_copy;

    char  search[128] = {0};
    char  filter_letter = 0;      /* 0 = no A-Z filter */

    /* ---- Controls ----
     *
     * The glass pad and a physical gamepad produce the SAME button mask, and
     * only that mask is translated into keys or stick movement. A DOS game
     * cannot tell the two apart and neither does anything below here. */
    retrodos::VirtualPad pad;
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        pad.reset_layout(ww, wh);
        std::vector<retrodos::PadControl> saved;
        if (pad_layout_from_string(cfg.pad_layout, saved)) pad.set_controls(saved);
    }
    std::vector<SDL_Gamepad *> gamepads;
    unsigned gp_buttons = 0;      /* PAD_* bits from a physical gamepad */
    int      gp_axis_x = 0, gp_axis_y = 0;   /* -1000..1000, analog stick */
    unsigned pad_prev = 0;        /* last mask applied to the guest */
    bool     joy_primed = false;  /* game-port stick announced to the guest */

    auto refresh_gamepads = [&]() {
        for (SDL_Gamepad *g : gamepads) SDL_CloseGamepad(g);
        gamepads.clear();
        int n = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&n)) {
            for (int i = 0; i < n; ++i)
                if (SDL_Gamepad *g = SDL_OpenGamepad(ids[i])) gamepads.push_back(g);
            SDL_free(ids);
        }
        if (gamepads.empty()) { gp_buttons = 0; gp_axis_x = gp_axis_y = 0; }
        pad.set_gamepad_present(!gamepads.empty());
    };
    refresh_gamepads();

    /* Turn one SDL gamepad button into a virtual one. */
    auto gp_bit = [](int b) -> int {
        switch (b) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return retrodos::PAD_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return retrodos::PAD_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return retrodos::PAD_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return retrodos::PAD_RIGHT;
        case SDL_GAMEPAD_BUTTON_SOUTH:          return retrodos::PAD_A;
        case SDL_GAMEPAD_BUTTON_EAST:           return retrodos::PAD_B;
        case SDL_GAMEPAD_BUTTON_WEST:           return retrodos::PAD_X;
        case SDL_GAMEPAD_BUTTON_NORTH:          return retrodos::PAD_Y;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return retrodos::PAD_L;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return retrodos::PAD_R;
        case SDL_GAMEPAD_BUTTON_START:          return retrodos::PAD_START;
        case SDL_GAMEPAD_BUTTON_BACK:           return retrodos::PAD_SELECT;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return retrodos::PAD_L3;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return retrodos::PAD_R3;
        default:                                return -1;
        }
    };

    /* ---- RetroMedia ---- */
    retrodos::MediaAccount account;
    std::string  media_msg;
    bool         media_busy = false;
    std::vector<retrodos::MediaGame> catalogue;   /* admin download browser */
    std::map<std::string, SDL_Texture *> art;     /* game name -> box art */
    std::vector<std::string> art_queue;           /* slugs still to fetch */
    std::map<std::string, std::string> art_owner; /* slug -> local game name */
    int  art_done = 0, art_total = 0;
    bool art_mode = false;        /* this catalogue fetch is for artwork */
    char m_email[128] = {0}, m_pass[128] = {0}, m_key[160] = {0};
    char cat_search[128] = {0};
    char cat_letter = 0;

    SDL_strlcpy(m_email, retrodos::media_last_email().c_str(), sizeof(m_email));
    if (retrodos::media_available()) retrodos::media_begin_status();

    /* Turn a cached RGBA file into a texture. Uploading happens here, on the
     * render thread, because that is the only thread allowed to touch the
     * renderer -- the bridge only ever hands back a path. */
    auto load_art = [&](const std::string &name, const std::string &path) {
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        if (!retrodos::media_read_art(path, w, h, rgba)) return;
        SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t) return;
        SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        auto it = art.find(name);
        if (it != art.end() && it->second) SDL_DestroyTexture(it->second);
        art[name] = t;
    };
    int   selected = -1;          /* game index for per-game settings */
    Settings active = cfg.defaults;
    bool  show_overlay = false, show_osk = false;
    bool  show_controls = false;   /* in-game mapping panel */
    std::string playing;           /* title of the running game */
    bool  running = true;

    /* Whether an engine has already run in this process. The engine cannot
     * run twice: DOSBox-X's globals are initialised once and torn down
     * asymmetrically, and the second machine boots straight into a triple
     * fault. On Android the fix is a process restart with the chosen game
     * remembered; elsewhere the in-process attempt remains, as the least-bad
     * option available. */
    bool engine_has_run = false;

    auto launch = [&](const Game &in) {
        Game g = in;

        if (engine_has_run) {
            cfg.pending_launch = g.name;
            retrodos::save_app_config(cfg_path, cfg);
            if (retrodos::android_restart_app())
                return;             /* the process is about to die */
            cfg.pending_launch.clear();   /* no restart here; press on */
        }

        /* A SAF game has no path yet. Copy it into our own directory, which IS
         * a real path -- DOSBox-X mounts a directory by path and cannot be
         * given a content:// URI. Only the title being launched is copied, and
         * only the first time. */
        if (g.from_saf && !g.is_demo) {
            SDL_CreateDirectory(stage_dir.c_str());
            const std::string dest = stage_dir + "/" + g.name;
            if (!retrodos::saf_stage_game(g.name, dest)) {
                LOGI("stage failed for %s", g.name.c_str());
                return;
            }
            g.dir = dest;
            find_runnable(dest, g);   /* only now can we look inside */
        }

        Settings s = cfg.defaults;
        retrodos::load_game_settings(games_dir, g.name, s);  /* overrides win */
        active = s;
        /* A conf shipped with the game states how it starts; use it verbatim
         * rather than the guessed program name. */
        const bool use_profile = !g.autoexec.empty();
        const std::string conf = !g.conf_override.empty()
            ? g.conf_override
            : retrodos::build_conf(
                s, g.name, g.dir,
                use_profile ? g.autoexec : g.run,
                use_profile ? true : g.run_raw,
                g.audio_profile);
        if (SDL_IOStream *io = SDL_IOFromFile(conf_path.c_str(), "w")) {
            SDL_WriteIO(io, conf.data(), conf.size());
            SDL_CloseIO(io);
        }
        last_serial  = 0;
        show_overlay = show_osk = false;
        pad_prev     = 0;
        joy_primed   = false;
        playing      = g.name;
        engine_has_run = true;
        engine = std::thread(engine_thread, conf_path, g.dir);
        view = View::Emulator;
    };

    /* Auto-launch after a phoenix restart. Cleared and SAVED before the
     * launch, so a game that kills the process cannot restart-loop the app --
     * the second time round the note is simply gone. */
    if (!cfg.pending_launch.empty()) {
        const std::string want = cfg.pending_launch;
        cfg.pending_launch.clear();
        retrodos::save_app_config(cfg_path, cfg);
        view = View::Shell;   /* never the wizard: a restart means we ran */
        for (const Game &g : games) {
            if (g.name == want) { launch(g); break; }
        }
        if (view != View::Emulator && retrodos::demo_prepare(demo_dir)) {
            /* Demo titles are built on the fly and never live in `games`
             * while a real library exists. */
            const retrodos::DemoKind kinds[] = { retrodos::DemoKind::Demo,
                                                 retrodos::DemoKind::FreeDos };
            for (retrodos::DemoKind k : kinds) {
                if (retrodos::demo_title(k) != want) continue;
                Game g;
                g.name    = retrodos::demo_title(k);
                g.dir     = demo_dir;
                g.run     = retrodos::demo_command(k, g.run_raw);
                g.is_demo = true;
                launch(g);
                break;
            }
        }
    }

    int win_w = 0, win_h = 0;

    while (running) {
        /* Fetched BEFORE the event loop: touch hit-testing needs the size,
         * and querying it afterwards would test this frame's fingers against
         * last frame's dimensions -- wrong for exactly one frame after every
         * rotation, which is when a control has just moved. */
        SDL_GetWindowSizeInPixels(win, &win_w, &win_h);
        /* True when the UI is covering the game and DOS should see nothing. */
        const bool ui_modal = (view != View::Emulator) || show_overlay ||
                              show_osk || show_controls;

        /* ImGui is fed events even while the game is in front, because the
         * emulator view carries a small always-visible control strip. Without
         * this it could be drawn but never clicked, which is how the on-screen
         * keyboard ended up reachable only by a key no handheld has.
         *
         * What stops the game from losing its input is WantCaptureMouse: it is
         * only true while the pointer is actually over one of those controls. */
        const ImGuiIO &io = ImGui::GetIO();
        const bool ui_wants_mouse = ui_modal || io.WantCaptureMouse;
        const bool ui_wants_keys  = ui_modal || io.WantCaptureKeyboard;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);

            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                if (view != View::Emulator) break;
                const bool down = (ev.type == SDL_EVENT_KEY_DOWN);
                /* Back/Escape always reaches the overlay, never DOS: on a
                 * handheld with no keyboard it is the only way out. */
                if (ev.key.scancode == SDL_SCANCODE_AC_BACK ||
                    ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (down) show_overlay = !show_overlay;
                    break;
                }
                if (!ui_wants_keys) retrodos_host_send_key((int)ev.key.scancode, down);
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                if (!ui_wants_mouse && view == View::Emulator)
                    retrodos_host_mouse_move((int)ev.motion.xrel, (int)ev.motion.yrel);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (!ui_wants_mouse && view == View::Emulator) {
                    const int b = (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                                  (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : 0;
                    retrodos_host_mouse_button(b, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                }
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                /* Re-enumerate rather than track deltas: a handheld's built-in
                 * controls can appear and vanish around sleep, and a miscounted
                 * delta would leave the glass pad hidden with nothing to play
                 * with. */
                refresh_gamepads();
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const int b = gp_bit(ev.gbutton.button);
                if (b >= 0) {
                    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) gp_buttons |=  (1u << b);
                    else                                          gp_buttons &= ~(1u << b);
                }
                break;
            }

            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                LOGI("gpaxis axis=%d value=%d", (int)ev.gaxis.axis, (int)ev.gaxis.value);
                /* The left stick drives the emulated stick directly, and also
                 * synthesises direction presses so a keyboard game is playable
                 * with it. The dead zone is generous: a worn thumbstick that
                 * rests off-centre would otherwise hold a direction forever. */
                const int v = ev.gaxis.value;                 /* -32768..32767 */
                const int scaled = (int)((long)v * 1000 / 32767);
                const int kDead = 380;                        /* ~38% */

                if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                    gp_axis_x = scaled;
                    gp_buttons &= ~((1u << retrodos::PAD_LEFT) | (1u << retrodos::PAD_RIGHT));
                    if (scaled < -kDead) gp_buttons |= 1u << retrodos::PAD_LEFT;
                    if (scaled >  kDead) gp_buttons |= 1u << retrodos::PAD_RIGHT;
                } else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                           ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                    /* Triggers rest at 0 and run to full, so they are treated
                     * as buttons with a threshold rather than an axis. Half
                     * travel is where a finger means it. */
                    const int bit = (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                                        ? retrodos::PAD_LT : retrodos::PAD_RT;
                    if (scaled > 500) gp_buttons |=  (1u << bit);
                    else              gp_buttons &= ~(1u << bit);
                } else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                    gp_axis_y = scaled;
                    gp_buttons &= ~((1u << retrodos::PAD_UP) | (1u << retrodos::PAD_DOWN));
                    if (scaled < -kDead) gp_buttons |= 1u << retrodos::PAD_UP;
                    if (scaled >  kDead) gp_buttons |= 1u << retrodos::PAD_DOWN;
                }
                break;
            }

            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
                /* Only while the game is in front: elsewhere a finger is a
                 * mouse click on the UI, which ImGui has already had. */
                if (view == View::Emulator && !show_overlay && !show_osk &&
                    !show_controls)
                    pad.handle_event(ev, win_w, win_h);
                break;

            default: break;
            }
        }

        /* Release any on-screen key whose hold time has elapsed. Every frame,
         * not only while the keyboard is drawn: hiding it between the press and
         * the release would leave the key held down in the guest. */
        retrodos::osk_update();

        /* ---- Deliver the pad to the guest ----
         *
         * One mask from both sources, applied on CHANGE only. Re-sending a held
         * button every frame would look to DOS like the key repeating at 60Hz,
         * which turns a menu selection into a blur. */
        if (view == View::Emulator) {
            pad.set_enabled(active.onscreen_pad);

            /* Announce the stick BEFORE the game asks.
             *
             * The bridge enables the emulated stick on the first input it
             * receives -- which used to mean the game port read as empty until
             * the player actually moved something. A DOS game probes the port
             * once, at startup or in its setup screen, decides there is no
             * joystick, and never looks again. Sending one centred, buttons-up
             * report at boot is what makes detection succeed. Re-armed when the
             * option is toggled on mid-game from the Controls panel. */
            if (active.pad_sends_joystick) {
                if (!joy_primed) { retrodos_host_joystick(0, 0, 0, 0); joy_primed = true; }
            } else joy_primed = false;

            const unsigned mask = pad.held() | gp_buttons;
            if (mask != pad_prev) {
                if (active.pad_sends_keys) {
                    const unsigned changed = mask ^ pad_prev;
                    for (int b = 0; b < retrodos::PAD_COUNT; ++b) {
                        if (!(changed & (1u << b))) continue;
                        const int sc = active.pad_keys[b];
                        if (sc) retrodos_host_send_key(sc, (mask & (1u << b)) != 0);
                    }
                }

                if (active.pad_sends_joystick) {
                    /* An analog stick is passed through as-is; the glass pad
                     * and a d-pad can only be full deflection. */
                    int ax = gp_axis_x, ay = gp_axis_y;
                    if (!pad.gamepad_present()) {
                        ax = (mask & (1u << retrodos::PAD_RIGHT)) ?  1000 :
                             (mask & (1u << retrodos::PAD_LEFT))  ? -1000 : 0;
                        ay = (mask & (1u << retrodos::PAD_DOWN))  ?  1000 :
                             (mask & (1u << retrodos::PAD_UP))    ? -1000 : 0;
                    }
                    /* The standard game port has two buttons, so only A and B
                     * reach it; everything else is a key. */
                    const int jmask = ((mask & (1u << retrodos::PAD_A)) ? 1 : 0) |
                                      ((mask & (1u << retrodos::PAD_B)) ? 2 : 0);
                    retrodos_host_joystick(0, jmask, ax, ay);
                }
                pad_prev = mask;
            } else if (active.pad_sends_joystick && pad.gamepad_present() &&
                       (gp_axis_x || gp_axis_y)) {
                /* Analog movement inside the dead zone changes no button bit,
                 * so without this the stick would only update when it crossed
                 * a threshold -- fine for a d-pad game, useless for a flight
                 * sim. */
                const int jmask = ((mask & (1u << retrodos::PAD_A)) ? 1 : 0) |
                                  ((mask & (1u << retrodos::PAD_B)) ? 2 : 0);
                retrodos_host_joystick(0, jmask, gp_axis_x, gp_axis_y);
            }
        }

        /* Drain finished network work. Everything the bridge does is async, so
         * this is where results become UI state -- never on the caller's side
         * of a begin_*(), which returns long before the server answers. */
        {
            retrodos::MediaResult mr;
            while (retrodos::media_poll(mr)) {
                if (mr.op != retrodos::MediaOp::Artwork) media_busy = false;
                if (!mr.message.empty()) media_msg = mr.message;

                switch (mr.op) {
                case retrodos::MediaOp::Status:
                case retrodos::MediaOp::Login:
                    account = mr.account;
                    if (mr.ok && account.signed_in) {
                        /* The password has served its purpose; the session is
                         * what persists. Do not leave it sitting in a buffer. */
                        SDL_memset(m_pass, 0, sizeof(m_pass));
                        SDL_memset(m_key,  0, sizeof(m_key));
                        /* No "signed in" banner: the Artwork page states the
                         * account directly below, and saying it twice reads as
                         * a bug. Only a FAILURE needs a message here. */
                        if (mr.ok) media_msg.clear();
                    }
                    break;

                case retrodos::MediaOp::Logout:
                    account = retrodos::MediaAccount();
                    catalogue.clear();
                    break;

                case retrodos::MediaOp::Catalogue:
                    if (!mr.ok) break;
                    catalogue = mr.games;
                    if (art_mode) {
                        art_mode = false;
                        /* This catalogue was fetched to find artwork, not to
                         * browse: match it against the library and queue only
                         * the titles we actually have. */
                        std::map<std::string, const retrodos::MediaGame *> by_key;
                        for (const auto &cg : mr.games) by_key[canon(cg.title)] = &cg;
                        art_queue.clear();
                        art_owner.clear();
                        for (auto &g : games) {
                            if (g.is_demo) continue;
                            auto it = by_key.find(canon(g.name));
                            if (it == by_key.end() || it->second->preview.empty()) continue;
                            g.slug = it->second->slug;
                            art_owner[g.slug] = g.name;
                            art_queue.push_back(g.slug);
                        }
                        art_total = (int)art_queue.size();
                        art_done  = 0;
                        media_msg = art_total ? ("Matched " + std::to_string(art_total) +
                                                 " of " + std::to_string(games.size()))
                                              : "No titles matched the catalogue";
                        /* Kick the first fetch; the rest follow one at a time as
                         * each result lands, so a big library does not open
                         * hundreds of sockets at once. */
                        if (!art_queue.empty()) {
                            const std::string slug = art_queue.back();
                            art_queue.pop_back();
                            for (const auto &cg : mr.games)
                                if (cg.slug == slug) {
                                    retrodos::media_begin_artwork(slug, cg.preview);
                                    break;
                                }
                        }
                    }
                    break;

                case retrodos::MediaOp::Artwork:
                    if (mr.ok && !mr.art.path.empty()) {
                        auto own = art_owner.find(mr.art.slug);
                        if (own != art_owner.end()) load_art(own->second, mr.art.path);
                    }
                    ++art_done;
                    if (!art_queue.empty()) {
                        const std::string slug = art_queue.back();
                        art_queue.pop_back();
                        for (const auto &cg : catalogue)
                            if (cg.slug == slug) {
                                retrodos::media_begin_artwork(slug, cg.preview);
                                break;
                            }
                    } else {
                        media_busy = false;
                        if (art_total) media_msg = "Artwork: " + std::to_string(art_done) +
                                                   " of " + std::to_string(art_total);
                        art_total = 0;
                    }
                    break;

                case retrodos::MediaOp::Download:
                    /* The game is on disk now, so the library is stale. */
                    if (mr.ok) refresh();
                    break;

                default: break;
                }
            }
        }


        /* ImGui's SDL_Renderer backend sets a clip rect per draw command;
         * reset the render state so each frame starts from a known one. */
        SDL_SetRenderClipRect(ren, NULL);
        SDL_SetRenderViewport(ren, NULL);
        SDL_SetRenderDrawColor(ren, 12, 12, 16, 255);
        SDL_RenderClear(ren);

        if (view == View::Emulator) {
            /* Poll the serial, not the pixels: DOS text mode can sit on an
             * identical picture for seconds, and re-uploading it every frame
             * would burn a handheld's battery for nothing. */
            const uint64_t serial = retrodos_host_framebuffer_serial();
            if (serial != last_serial) {
                int w = 0, h = 0;
                retrodos_host_framebuffer_size(&w, &h);
                if (w > 0 && h > 0) {
                    fb_copy.resize((size_t)w * (size_t)h);
                    uint64_t got = 0;
                    if (retrodos_host_copy_framebuffer(fb_copy.data(),
                                                       (int)fb_copy.size(),
                                                       &w, &h, &got)) {
                        if (!fb_tex || fb_tex_w != w || fb_tex_h != h) {
                            if (fb_tex) SDL_DestroyTexture(fb_tex);
                            /* XRGB, not ARGB: the engine's framebuffer carries
                             * no meaningful alpha, so treating the top byte as
                             * one draws a fully transparent picture -- the draw
                             * still succeeds and the screen stays black. */
                            fb_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
                                                       SDL_TEXTUREACCESS_STREAMING, w, h);
                            SDL_SetTextureScaleMode(fb_tex, SDL_SCALEMODE_NEAREST);
                            SDL_SetTextureBlendMode(fb_tex, SDL_BLENDMODE_NONE);
                            fb_tex_w = w; fb_tex_h = h;
                        }
                        SDL_UpdateTexture(fb_tex, nullptr, fb_copy.data(), w * 4);
                        last_serial = serial;
                    }
                }
            }

            if (fb_tex) {
                const SDL_FRect dst = fit(fb_tex_w, fb_tex_h,
                                          retrodos_host_pixel_aspect_x1000(),
                                          win_w, win_h, active);
                SDL_RenderTexture(ren, fb_tex, nullptr, &dst);
            }

            if (g_engine_done.load()) {
                if (engine.joinable()) engine.join();
                g_engine_done.store(false);
                view = View::Shell;
                show_overlay = show_osk = false;
                refresh();
            }
        }

        /* The frame is built every time, not only when the UI is in front: the
         * emulator view carries a small control strip, and a widget that is not
         * submitted cannot be hovered, clicked, or reported by
         * WantCaptureMouse. */
        {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            /* The usable rectangle, not the whole screen.
             *
             * On a device with a Dynamic Island or a home indicator, drawing
             * from (0,0) puts the title under the cutout -- which is exactly
             * what the first iOS build did.
             *
             * SDL reports the safe area in WINDOW coordinates while everything
             * here is in PIXELS (SDL_GetWindowSizeInPixels), so it has to be
             * multiplied by the pixel density or the inset is out by 3x on a
             * Retina screen.
             *
             * Apple-only by instruction, not because the problem is: a notched
             * Android phone draws under its cutout too, and this is one #if
             * away from fixing that as well. */
            ImVec2 origin(0.0f, 0.0f);
            ImVec2 full((float)win_w, (float)win_h);
#if defined(__APPLE__)
            {
                SDL_Rect safe;
                if (SDL_GetWindowSafeArea(win, &safe) && safe.w > 0 && safe.h > 0) {
                    const float d = SDL_GetWindowPixelDensity(win);
                    origin = ImVec2((float)safe.x * d, (float)safe.y * d);
                    full   = ImVec2((float)safe.w * d, (float)safe.h * d);
                }
            }
#endif

            /* ---------------- Wizard ---------------- */
            if (view == View::Wizard) {
                ImGui::SetNextWindowPos(origin);
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##wizard", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

                ImGui::TextUnformatted("Retro-DOS - first run");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("Choose where your DOS games live. Each game should "
                                   "be its own folder.");
                ImGui::Spacing();
#if defined(__APPLE__)
                TextDimWrapped("This folder is the app's own, and it is the one the "
                               "Files app shows. Put each game in its own folder "
                               "inside it and it appears in the library next time "
                               "you look.");
#else
                TextDimWrapped("These folders belong to the app, so they need no "
                               "permission and work on removable storage. Android "
                               "only grants access elsewhere as a document tree, "
                               "which the emulator cannot mount directly -- games "
                               "kept outside have to be copied in first.");
#endif
                ImGui::Spacing();

                for (size_t i = 0; i < roots.size(); ++i) {
                    ImGui::PushID((int)i);
                    if (ImGui::RadioButton("##root", cfg.library_root == roots[i]))
                        cfg.library_root = roots[i];
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s%s", root_label(roots[i]).c_str(),
                                       path_is_dir(roots[i]) ? "" : "   (will be created)");
                    ImGui::PopID();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
#if defined(__APPLE__)
                /* No folder picker here. iOS has no document-tree grant that the
                 * emulator could mount, saf_pick_folder is a stub, and offering a
                 * button that does nothing is worse than not offering one. Files
                 * is how a game arrives, so say that instead. */
                TextDimWrapped("To add a game: open the Files app, find Retro-DOS "
                               "under On My iPhone or On My iPad, and put the "
                               "game's folder inside it.");
#else
                ImGui::TextWrapped("Or choose any folder on the device:");
                if (retrodos::saf_has_grant()) {
                    ImGui::TextWrapped("Using: %s", library_label(cfg.library_root).c_str());
                    TextDimWrapped("Each game is copied to the folder above the first "
                                   "time you play it, then mounted from there.");
                } else {
                    TextDimWrapped("Android grants only the exact folder you pick, never "
                                   "all files. The emulator mounts real paths, so the "
                                   "game you launch is copied in first.");
                }
                if (ImGui::Button(retrodos::saf_has_grant() ? "Choose a different folder"
                                                            : "Choose folder...",
                                  ImVec2(0, 0)))
                    retrodos::saf_pick_folder();
#endif

                ImGui::Spacing();
#if !defined(__APPLE__)
                if (ImGui::Button("Rescan volumes", ImVec2(0, 0))) {
                    roots = candidate_roots();
                    for (const auto &r : roots) SDL_CreateDirectory(r.c_str());
                }
                ImGui::SameLine();
#endif
                if (ImGui::Button("Continue", ImVec2(0, 0))) {
                    SDL_CreateDirectory(cfg.library_root.c_str());
                    cfg.wizard_done = true;
                    retrodos::save_app_config(cfg_path, cfg);
                    refresh();
                    view = View::Shell;
                }
                ImGui::End();
            }

            /* ---------------- Shell: nav rail + content ---------------- */
            /* The same shape as the rest of the Retro-* family: a fixed rail
             * on the left naming the pages, and one content pane. A handheld
             * has no window chrome and no room for a menu bar, so the rail is
             * the only navigation -- and being always visible, it also shows
             * where you are. */
            else if (view == View::Shell) {
                ImGui::SetNextWindowPos(origin);
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##shell", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus);

                const float sc      = ImGui::GetFontSize() / 20.0f;
                const float rail_w  = 200.0f * sc;
                const float margin  = 10.0f * sc;

                ImGui::SetCursorPos(ImVec2(margin, margin));
                ImGui::BeginChild("rail", ImVec2(rail_w, full.y - margin * 2.0f),
                                  ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

                {
                    /* The wordmark, fitted to the rail, or the app's name if
                     * it did not package. Width-limited rather than a fixed
                     * size, so it stays right from a phone to a desktop
                     * window. */
                    int lw = 0, lh = 0;
                    if (SDL_Texture *mark = retrodos::wordmark(ren, &lw, &lh)) {
                        const float avail = ImGui::GetContentRegionAvail().x;
                        const float w = std::min(avail, (float)lw);
                        const float h = w * (float)lh / (float)lw;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             (avail - w) * 0.5f);
                        ImGui::Image((ImTextureID)(intptr_t)mark, ImVec2(w, h));
                    } else {
                        ImGui::TextUnformatted("RETRO-DOS");
                    }
                }
                ImGui::Separator();

                const float bw = ImGui::GetContentRegionAvail().x;
                const float bh = ImGui::GetFrameHeight() * 1.5f;
                auto nav = [&](const char *label, Page p) {
                    const bool on = (page == p);
                    if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button(label, ImVec2(bw, bh))) { page = p; selected = -1; }
                    if (on) ImGui::PopStyleColor();
                };

                nav("Library", Page::Library);
                if (retrodos::media_available()) {
                    nav("Artwork", Page::Artwork);
                    /* Two independent gates. The platform one comes first and
                     * is absolute: an App Store build must not offer game
                     * downloads at all, however the account is privileged. The
                     * admin one then reflects what the server would allow, so
                     * the rail never advertises something that would fail. */
                    if (retrodos::media_downloads_available() &&
                        account.signed_in && account.is_admin)
                        nav("Downloads", Page::Downloads);
                }
                nav("Machine",  Page::Settings);
                nav("Input",    Page::Input);
                ImGui::Spacing();
                ImGui::Separator();
                nav("Windows", Page::Windows);
                nav("Demo",  Page::Demo);
                nav("About", Page::About);

                ImGui::EndChild();

                ImGui::SetCursorPos(ImVec2(rail_w + margin * 2.0f, margin));
                /* Only the Library scrolls -- it is the one page whose length
                 * is the user's data rather than our layout. Everything else is
                 * built to fit, and a scrollbar there would be an admission
                 * that it does not. */
                ImGui::BeginChild("content",
                                  ImVec2(full.x - rail_w - margin * 3.0f,
                                         full.y - margin * 2.0f),
                                  ImGuiChildFlags_Borders |
                                  ImGuiChildFlags_AlwaysUseWindowPadding,
                                  /* Every page scrolls.
                                   *
                                   * The rule used to be that only the Library
                                   * did, because everything else was "built to
                                   * fit" and a scrollbar would admit it did
                                   * not. It does not: Machine's Display section
                                   * was cut off by the Save row with no way to
                                   * reach it, and About's source offer sat
                                   * below the fold -- settings and a legal
                                   * notice that exist but cannot be read.
                                   *
                                   * ImGui only draws a scrollbar when content
                                   * actually overflows, so a page that does fit
                                   * looks exactly as it did. This costs those
                                   * pages nothing and stops the layout silently
                                   * eating things on smaller screens. */
                                  0);
                scroll_by_drag();

                const float cw = ImGui::GetContentRegionAvail().x;

                /* ---- Library ---- */
                if (page == Page::Library) {
                    ImGui::TextUnformatted("Library");
                    {
                        const float bw2 = ImGui::CalcTextSize("Add game").x +
                                          ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float bw3 = ImGui::CalcTextSize("Rescan").x +
                                          ImGui::GetStyle().FramePadding.x * 2.0f;
                        ImGui::SameLine(cw - bw2 - bw3 - ImGui::GetStyle().ItemSpacing.x);
                        if (ImGui::Button("Add game")) retrodos::saf_pick_game(cfg.library_root);
                        ImGui::SameLine();
                        if (ImGui::Button("Rescan")) refresh();
                    }

                    /* The install runs on its own thread, so the result appears
                     * here rather than being returned by the button. */
                    {
                        const std::string st = retrodos::saf_install_status();
                        if (!st.empty()) {
                            ImGui::TextDisabled("%s", st.c_str());
                            static std::string last;
                            /* Rescan once, when the message changes to a
                             * finished one -- not every frame. */
                            if (st != last) {
                                last = st;
                                if (st.compare(0, 6, "Added ") == 0) refresh();
                            }
                        }
                    }
                    ImGui::Separator();

                    ImGui::SetNextItemWidth(cw * 0.6f);
                    ImGui::InputTextWithHint("##search", "Search local games...",
                                             search, sizeof(search));

                    /* A-Z strip: with thousands of titles, scrolling is not a
                     * navigation method. */
                    /* Only letters that actually have something behind them.
                     *
                     * A DOS collection is never evenly spread, and a full A-Z
                     * strip is mostly dead targets: pressing one to be shown an
                     * empty list teaches nothing except that the control lies.
                     * Showing only the letters present makes every chip a
                     * promise, and gives the remaining ones more width to be
                     * tapped with. */
                    {
                        bool has[27] = { false };   /* 0-25 = A-Z, 26 = '#' */
                        for (const Game &g : games) {
                            if (g.initial >= 'A' && g.initial <= 'Z') has[g.initial - 'A'] = true;
                            else has[26] = true;
                        }

                        int count = 1;              /* "All" is always offered */
                        for (bool b : has) if (b) ++count;

                        /* A filter can outlive the games behind it -- a rescan,
                         * a card removed. Drop it rather than leave the list
                         * showing nothing with no visible way back. */
                        if (filter_letter) {
                            const bool still = (filter_letter == '#')
                                ? has[26]
                                : (filter_letter >= 'A' && filter_letter <= 'Z' &&
                                   has[filter_letter - 'A']);
                            if (!still) filter_letter = 0;
                        }

                        const float gapx = 3.0f;
                        const float cell = (ImGui::GetContentRegionAvail().x -
                                            gapx * (float)(count - 1)) / (float)count;
                        bool first = true;
                        auto chip = [&](const char *label, char value) {
                            if (!first) ImGui::SameLine(0.0f, gapx);
                            first = false;
                            const bool on = (filter_letter == value);
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(label, ImVec2(cell, 0)))
                                filter_letter = on ? 0 : value;
                            if (on) ImGui::PopStyleColor();
                        };

                        chip("All", 0);
                        for (char c = 'A'; c <= 'Z'; ++c) {
                            if (!has[c - 'A']) continue;
                            ImGui::PushID((int)c);
                            const char lbl[2] = { c, 0 };
                            chip(lbl, c);
                            ImGui::PopID();
                        }
                        if (has[26]) chip("#", '#');
                    }

                    std::vector<int> shown;
                    shown.reserve(games.size());
                    for (int i = 0; i < (int)games.size(); ++i) {
                        if (filter_letter && games[i].initial != filter_letter) continue;
                        if (search[0] && !SDL_strcasestr(games[i].name.c_str(), search)) continue;
                        shown.push_back(i);
                    }
                    ImGui::Text("%zu of %zu", shown.size(), games.size());
                    ImGui::SameLine();
                    ImGui::TextDisabled("  %s", root_label(cfg.library_root).c_str());

                    if (games.empty()) {
                        ImGui::Spacing();
                        ImGui::TextWrapped("No games found in:");
                        ImGui::TextWrapped("%s", root_label(cfg.library_root).c_str());
                        ImGui::Spacing();
                        ImGui::TextWrapped("Put each game in its own folder there and press "
                                           "Rescan, or try the Demo page.");
                    } else if (shown.empty()) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("No games match this filter.");
                    } else {
                        ImGui::BeginChild("##list");
                        scroll_by_drag();
                        const float row_h = ImGui::GetFontSize() * 2.2f;
                        /* Clip: a widget per title would cost thousands of draw
                         * calls a frame on a real collection. */
                        ImGuiListClipper clipper;
                        clipper.Begin((int)shown.size(), row_h);
                        while (clipper.Step()) {
                            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                                const int gi = shown[r];
                                ImGui::PushID(gi);

                                /* Box art where we have it, sized from the row so
                                 * the list keeps one rhythm whether or not a title
                                 * matched the catalogue. */
                                auto ai = art.find(games[gi].name);
                                if (ai != art.end() && ai->second) {
                                    SDL_Texture *t = ai->second;
                                    const float tw = (t->h > 0)
                                        ? row_h * ((float)t->w / (float)t->h) : row_h;
                                    ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(tw, row_h));
                                    ImGui::SameLine();
                                }

                                if (ImGui::Selectable(games[gi].name.c_str(), false, 0,
                                                      ImVec2(0, row_h)))
                                    launch(games[gi]);
                                ImGui::SameLine(cw * 0.58f);
                                {
                                    /* Clipped to its column. The command is
                                     * whatever the game needs -- "boot
                                     * FREEDOS.IMG -l A" is longer than the
                                     * column is wide -- and without this it
                                     * drew straight through the Setup button
                                     * beside it. */
                                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                                    const float colw = cw * 0.86f - cw * 0.58f
                                                     - ImGui::GetStyle().ItemSpacing.x * 2.0f;
                                    ImGui::PushClipRect(p0,
                                        ImVec2(p0.x + colw, p0.y + row_h), true);
                                    ImGui::TextDisabled("%s", games[gi].run.empty()
                                                        ? "(no runnable found)"
                                                        : games[gi].run.c_str());
                                    ImGui::PopClipRect();
                                }
                                ImGui::SameLine(cw * 0.86f);
                                if (ImGui::SmallButton("Setup")) {
                                    selected = gi; page = Page::Settings;
                                }
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndChild();
                    }
                }

                /* ---- Artwork (and the RetroMedia account) ---- */
                else if (page == Page::Artwork) {
                    ImGui::TextUnformatted("Artwork");
                    ImGui::Separator();

                    if (!media_msg.empty()) {
                        ImGui::TextWrapped("%s", media_msg.c_str());
                        ImGui::Separator();
                    }

                    if (account.signed_in) {
                        ImGui::Text("Signed in as %s", account.email.c_str());
                        if (account.is_admin) ImGui::TextUnformatted("Administrator");
                        else ImGui::TextDisabled("Standard account - artwork only");
                        ImGui::Text("Credits: %d    Free today: %d",
                                    account.credits, account.free_remaining);
                        if (ImGui::Button("Sign out")) {
                            media_busy = true; retrodos::media_begin_logout();
                        }
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextWrapped("Match your library against the DOS catalogue and "
                                           "fetch box art for every title found.");
                        ImGui::BeginDisabled(media_busy || games.empty());
                        if (ImGui::Button("Get artwork for my library")) {
                            media_busy = true; art_mode = true;
                            art_total = 1; art_done = 0;
                            media_msg = "Fetching catalogue...";
                            retrodos::media_begin_catalogue("", "", false);
                        }
                        ImGui::EndDisabled();
                        if (art_total > 0 && art_done <= art_total)
                            ImGui::Text("%d / %d", art_done, art_total);
                        ImGui::Text("Cached: %zu", art.size());
                    } else {
                        ImGui::TextWrapped("Sign in to media.crownparkcomputing.com for box "
                                           "art. Administrators can also download games.");
                        ImGui::Spacing();
                        ImGui::SetNextItemWidth(cw * 0.55f);
                        ImGui::InputText("Email", m_email, sizeof(m_email));
                        ImGui::SetNextItemWidth(cw * 0.55f);
                        ImGui::InputText("Password", m_pass, sizeof(m_pass),
                                         ImGuiInputTextFlags_Password);
                        ImGui::BeginDisabled(media_busy || !m_email[0] || !m_pass[0]);
                        if (ImGui::Button("Sign in")) {
                            media_busy = true; media_msg = "Signing in...";
                            retrodos::media_begin_login(m_email, m_pass);
                        }
                        ImGui::EndDisabled();

                        ImGui::Spacing();
                        ImGui::Separator();
                        /* An API key suits a shared handheld better: revocable
                         * from the website, and the only route for an account
                         * that signs in with Google and has no password. */
                        ImGui::TextWrapped("Or paste an API key from your account page "
                                           "(starts with rmk_):");
                        ImGui::SetNextItemWidth(cw * 0.55f);
                        ImGui::InputText("API key", m_key, sizeof(m_key),
                                         ImGuiInputTextFlags_Password);
                        ImGui::BeginDisabled(media_busy || !m_key[0]);
                        if (ImGui::Button("Use API key")) {
                            media_busy = true; media_msg = "Checking key...";
                            retrodos::media_begin_login_key(m_key);
                        }
                        ImGui::EndDisabled();
                    }
                }

                /* ---- Downloads (admin) ---- */
                else if (page == Page::Downloads) {
                    /* The page can outlive its own preconditions: signing out
                     * while it is open would otherwise leave a download button
                     * on screen that the server will refuse. */
                    if (!retrodos::media_downloads_available() ||
                        !account.signed_in || !account.is_admin) {
                        page = Page::Library;
                    }
                    ImGui::TextUnformatted("Downloads");
                    ImGui::Separator();

                    if (!media_msg.empty()) {
                        ImGui::TextWrapped("%s", media_msg.c_str());
                    }
                    {   /* A 1 GB title is minutes of otherwise silent work, and a
                         * screen that looks frozen invites a second press. */
                        const std::string prog = retrodos::media_progress();
                        if (!prog.empty()) {
                            ImGui::TextWrapped("%s", prog.c_str());
                            float frac = -1.0f;
                            const size_t pc = prog.rfind('%');
                            if (pc != std::string::npos && pc > 0) {
                                const size_t b = prog.rfind('(', pc);
                                if (b != std::string::npos)
                                    frac = (float)atof(prog.c_str() + b + 1) / 100.0f;
                            }
                            if (frac >= 0.0f && frac <= 1.0f)
                                ImGui::ProgressBar(frac, ImVec2(cw * 0.6f, 0.0f));
                            else
                                ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                                                   ImVec2(cw * 0.6f, 0.0f), "working");
                        }
                    }
                    ImGui::Separator();

                    ImGui::SetNextItemWidth(cw * 0.45f);
                    ImGui::InputTextWithHint("##csearch", "Search catalogue...",
                                             cat_search, sizeof(cat_search));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(media_busy);
                    if (ImGui::Button("Browse")) {
                        media_busy = true; art_mode = false;
                        media_msg = "Loading catalogue...";
                        const char l[2] = { cat_letter, 0 };
                        retrodos::media_begin_catalogue(cat_search,
                                                        cat_letter ? l : "", true);
                    }
                    ImGui::EndDisabled();

                    if (ImGui::Button("All")) cat_letter = 0;
                    for (char c = 'A'; c <= 'Z'; ++c) {
                        ImGui::SameLine(0.0f, 2.0f);
                        ImGui::PushID(1000 + c);
                        const bool on = (cat_letter == c);
                        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        const char lbl[2] = { c, 0 };
                        if (ImGui::Button(lbl)) cat_letter = on ? 0 : c;
                        if (on) ImGui::PopStyleColor();
                        ImGui::PopID();
                    }

                    ImGui::Text("%zu titles", catalogue.size());
                    ImGui::BeginChild("##cat");
                    scroll_by_drag();
                    /* Rows are given room: at arm's length a list packed at text
                     * height is easy to mis-tap, and a mis-tap here starts a
                     * download that can run to a gigabyte. */
                    const float row_h = ImGui::GetFrameHeight() * 1.7f;
                    ImGuiListClipper clip;
                    clip.Begin((int)catalogue.size(), row_h);
                    while (clip.Step()) {
                        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                            const retrodos::MediaGame &cg = catalogue[r];
                            ImGui::PushID(r);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(cg.title.c_str());
                            ImGui::SameLine(cw * 0.58f);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("%.1f MB",
                                                (double)cg.bytes / (1024.0 * 1024.0));
                            ImGui::SameLine(cw * 0.78f);
                            ImGui::BeginDisabled(media_busy || cg.rom_files == 0);
                            if (ImGui::Button(cg.rom_files ? "Download" : "No files")) {
                                media_busy = true;
                                media_msg = "Downloading " + cg.title + "...";
                                retrodos::media_begin_download(cg.slug, cfg.library_root);
                            }
                            ImGui::EndDisabled();
                            const float used = ImGui::GetFrameHeight();
                            if (row_h > used) ImGui::Dummy(ImVec2(0.0f, row_h - used));
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();
                }

                /* ---- Machine settings ---- */
                else if (page == Page::Settings || page == Page::Input) {
                    static Settings edit;
                    static int edit_for = -3;

                    const bool per_game = (selected >= 0 && selected < (int)games.size());
                    if (edit_for != selected) {
                        edit = cfg.defaults;
                        if (per_game)
                            retrodos::load_game_settings(games_dir, games[selected].name, edit);
                        edit_for = selected;
                    }

                    if (per_game) ImGui::Text("%s - %s",
                                              page == Page::Input ? "Input" : "Machine",
                                              games[selected].name.c_str());
                    else ImGui::Text("%s - defaults for all games",
                                     page == Page::Input ? "Input" : "Machine");

                    /* Both halves of a game's setup, reachable from each other
                     * WITHOUT losing which game is being edited. The rail's own
                     * Machine/Input entries clear the selection -- they mean
                     * "the defaults" -- so without this there was no route at
                     * all to a single game's input bindings from its Setup
                     * button. */
                    {
                        const Page other = (page == Page::Input) ? Page::Settings
                                                                 : Page::Input;
                        ImGui::SameLine(cw - ImGui::GetFontSize() * 9.0f);
                        if (ImGui::SmallButton(page == Page::Input
                                                   ? "Machine settings"
                                                   : "Input bindings"))
                            page = other;
                    }
                    ImGui::Separator();

                    ImGui::BeginChild("##sset",
                                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f),
                                      0, ImGuiWindowFlags_NoScrollbar);
                    scroll_by_drag();
                    if (page == Page::Input) controls_widgets(edit);
                    else                     settings_widgets(edit);
                    if (per_game) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Saved for this game only; others keep the "
                                            "defaults.");
                    }
                    ImGui::EndChild();

                    if (ImGui::Button("Save")) {
                        if (per_game)
                            retrodos::save_game_settings(games_dir, games[selected].name, edit);
                        else { cfg.defaults = edit; retrodos::save_app_config(cfg_path, cfg); }
                        edit_for = -3; selected = -1; page = Page::Library;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        edit_for = -3; selected = -1; page = Page::Library;
                    }
                    if (page == Page::Settings) {
                        ImGui::SameLine();
                        if (ImGui::Button("Change games folder")) {
                            edit_for = -3; selected = -1;
                            roots = candidate_roots();
                            view = View::Wizard;
                        }
                    }
                }

                /* ---- Demo ---- */
                else if (page == Page::Windows) {
                    ImGui::TextUnformatted("Windows 98");
                    ImGui::Separator();

                    /* The machine lives in the library folder like a game, so
                     * once installed it appears in the Library and starts the
                     * same way everything else does. */
                    const std::string wdir = cfg.library_root + "/Windows 98";

                    static retrodos::Win98Install w;
                    static bool w_loaded = false;
                    if (!w_loaded) {
                        if (!retrodos::win98_load(wdir, w)) w.dir = wdir;
                        w_loaded = true;
                    }

                    /* Observable facts beat remembered state: if the disk
                     * image is there, the first step is done whatever the file
                     * says. This is what makes the page correct after a
                     * crash, a manual delete, or a copy from another device. */
                    const bool have_disk =
                        retrodos::win98_blocker(w).find("hard disk") == std::string::npos &&
                        w.phase != retrodos::Win98Phase::Create;
                    if (w.phase == retrodos::Win98Phase::Create) {
                        retrodos::Win98Install probe = w;
                        probe.phase = retrodos::Win98Phase::Install;
                        if (retrodos::win98_blocker(probe).find("hard disk") == std::string::npos) {
                            w.phase = retrodos::Win98Phase::Install;
                            retrodos::win98_save(w);
                        }
                    }
                    (void)have_disk;

                    TextDimWrapped("Runs a real copy of Windows 98 in the "
                                   "emulator, following DOSBox-X's own guide. "
                                   "You supply the CD; nothing is downloaded.");
                    ImGui::Spacing();

                    /* ---- step 1: the CD ---- */
                    ImGui::TextUnformatted("1.  Your Windows 98 CD");
                    static char iso_buf[1024] = {0};
                    static bool iso_primed = false;
                    if (!iso_primed) {
                        SDL_strlcpy(iso_buf, w.iso.c_str(), sizeof(iso_buf));
                        iso_primed = true;
                    }
                    ImGui::SetNextItemWidth(cw * 0.72f);
                    if (ImGui::InputText("##iso", iso_buf, sizeof(iso_buf)))
                        w.iso = iso_buf;
                    ImGui::SameLine();
                    if (ImGui::Button("Use this disc")) {
                        w.iso = iso_buf;
                        retrodos::win98_save(w);
                    }

                    /* Anything that looks like a disc image in the library
                     * folder, offered as a button -- typing an absolute path
                     * on a handheld with an on-screen keyboard is miserable. */
                    {
                        int offered = 0;
                        if (SDL_Storage *st = SDL_OpenFileStorage(cfg.library_root.c_str())) {
                            if (char **found = SDL_GlobStorageDirectory(
                                    st, nullptr, "*.iso", SDL_GLOB_CASEINSENSITIVE, nullptr)) {
                                for (int i = 0; found[i] && offered < 6; ++i) {
                                    /* One level only: the glob recurses, and a
                                     * nested hit is a path DOS cannot mount. */
                                    if (SDL_strchr(found[i], '/')) continue;
                                    ImGui::PushID(i);
                                    if (ImGui::Button(found[i])) {
                                        w.iso = cfg.library_root + "/" + found[i];
                                        SDL_strlcpy(iso_buf, w.iso.c_str(), sizeof(iso_buf));
                                        retrodos::win98_save(w);
                                    }
                                    ImGui::PopID();
                                    ++offered;
                                }
                                SDL_free(found);
                            }
                            SDL_CloseStorage(st);
                        }
                        if (!offered)
                            TextDimWrapped("Put the .iso in your games folder and it "
                                           "will be offered here.");
                    }

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    /* ---- step 2: the disk ---- */
                    ImGui::TextUnformatted("2.  The hard disk");
                    static int size_choice = 0;
                    static const char *kSizes[] = { "8 GB (recommended)", "2 GB",
                                                    "16 GB", "32 GB" };
                    static const int   kSizeMb[] = { 0, 2048, 16384, 32768 };
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
                    ImGui::Combo("Size", &size_choice, kSizes, (int)SDL_arraysize(kSizes));
                    w.size_mb = kSizeMb[size_choice];
                    TextDimWrapped("Over 512 MB is formatted FAT32. Windows 98's own "
                                   "IDE driver cannot handle a volume above 128 GB.");

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    /* ---- step 3: run it ---- */
                    ImGui::TextUnformatted("3.  Install");
                    ImGui::Text("Next step: %s", retrodos::win98_phase_name(w.phase));

                    const std::string blocked = retrodos::win98_blocker(w);
                    if (!blocked.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                        ImGui::TextWrapped("%s", blocked.c_str());
                        ImGui::PopStyleColor();
                    }

                    ImGui::BeginDisabled(!blocked.empty());
                    if (ImGui::Button(w.phase == retrodos::Win98Phase::Create
                                          ? "Create the disk"
                                          : (w.phase == retrodos::Win98Phase::Run
                                                 ? "Start Windows 98"
                                                 : "Run this step"),
                                      ImVec2(cw * 0.55f, 0))) {
                        SDL_CreateDirectory(w.dir.c_str());
                        retrodos::win98_save(w);
                        Game g;
                        g.name = "Windows 98";
                        g.dir  = w.dir;
                        g.conf_override = retrodos::win98_conf(w);
                        launch(g);
                    }
                    ImGui::EndDisabled();

                    /* The two transitions the app cannot observe. SETUP
                     * rebooting and Windows finally being installed both look
                     * identical from out here -- the engine simply exits -- so
                     * the person who just watched it is asked rather than
                     * guessed at. */
                    if (w.phase == retrodos::Win98Phase::Install) {
                        ImGui::Spacing();
                        if (ImGui::Button("Setup has restarted - carry on")) {
                            w.phase = retrodos::Win98Phase::Continue;
                            retrodos::win98_save(w);
                        }
                        TextDimWrapped("Press this when the installer reboots and you "
                                       "are back at a DOS prompt.");
                    } else if (w.phase == retrodos::Win98Phase::Continue) {
                        ImGui::Spacing();
                        if (ImGui::Button("Windows is installed")) {
                            w.phase = retrodos::Win98Phase::Run;
                            retrodos::win98_save(w);
                        }
                    } else if (w.phase == retrodos::Win98Phase::Run) {
                        ImGui::Spacing();
                        ImGui::Checkbox("Faster CPU core now that setup is done",
                                        &w.fast_core_after_install);
                        if (ImGui::IsItemDeactivatedAfterEdit()) retrodos::win98_save(w);
                        TextDimWrapped("Installation must run on the slow interpreter; "
                                       "afterwards the faster core is usually fine.");
                    }

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                    TextDimWrapped("Your CD must be an OEM Full edition - those are the "
                                   "ones that boot. No Windows files are bundled with "
                                   "this app or downloaded by it.");
                }
                else if (page == Page::Demo) {
                    ImGui::TextUnformatted("Demo");
                    ImGui::Separator();
                    ImGui::TextWrapped("Bundled with the app, so there is always something "
                                       "to run even with no games installed.");
                    ImGui::Spacing();

                    if (!retrodos::demo_prepare(demo_dir)) {
                        ImGui::TextDisabled("The bundled content could not be unpacked.");
                    } else {
                        const retrodos::DemoKind kinds[] = { retrodos::DemoKind::Demo,
                                                             retrodos::DemoKind::FreeDos };
                        for (retrodos::DemoKind k : kinds) {
                            Game g;
                            g.name    = retrodos::demo_title(k);
                            g.dir     = demo_dir;
                            g.run     = retrodos::demo_command(k, g.run_raw);
                            g.is_demo = true;
                            ImGui::PushID(g.name.c_str());
                            if (ImGui::Button(g.name.c_str(), ImVec2(cw * 0.9f, 0)))
                                launch(g);
                            ImGui::PopID();
                        }
                        ImGui::Spacing();
                        ImGui::TextDisabled("FreeDOS 1.3 is included verbatim under the GPL;\n"
                                            "the demonstration program was written for this\n"
                                            "project. See demo/NOTICE.md.");
                    }
                }

                /* ---- About ---- */
                else if (page == Page::About) {
                    ImGui::TextUnformatted("About");
                    ImGui::Separator();
                    ImGui::TextWrapped("Retro-DOS");
                    ImGui::TextDisabled("DOSBox-X core on SDL3, with a Dear ImGui frontend.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Games are mounted as folders, so each title runs from "
                                       "its own directory. Where a game ships its own "
                                       "dosbox.conf, its [autoexec] and sound settings are "
                                       "used as-is rather than guessed at.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Library: %s", root_label(cfg.library_root).c_str());
                    ImGui::TextWrapped("Games installed: %zu", games.size());
                    if (account.signed_in)
                        ImGui::TextWrapped("RetroMedia: %s%s", account.email.c_str(),
                                           account.is_admin ? " (administrator)" : "");

                    /* Licences and where the source is.
                     *
                     * Not decoration. This app ships GPL binaries -- DOSBox-X
                     * and the FreeDOS image -- and the GPL obliges whoever
                     * distributes them to convey the licence and an offer of
                     * the corresponding source to every recipient. A LICENSE
                     * file in the repository does not reach someone who
                     * installed from the App Store; this screen does.
                     *
                     * It is also what the review notes point at when they say
                     * the credit lives in About. */
                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                    ImGui::TextUnformatted("Licences and source");
                    TextDimWrapped("DOSBox-X and the bundled FreeDOS 1.3 image: GNU GPL v2. "
                                   "SDL3: zlib. Dear ImGui: MIT. The demo program is ours, "
                                   "under the same GPL.");
                    TextDimWrapped("Complete source, including this app and its build recipe:");
                    TextDimWrapped("github.com/CrownParkComputing/Retro-Dosbox");
                    TextDimWrapped("github.com/joncampbell123/dosbox-x  -  freedos.org/download");
                }

                ImGui::EndChild();
                ImGui::End();
            }

            /* ---------------- In-game overlay ---------------- */
            if (view == View::Emulator && show_overlay) {
                ImGui::SetNextWindowPos(ImVec2(origin.x + full.x * 0.5f, origin.y + full.y * 0.35f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::Begin("Paused", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize);
                char prog[64] = {0};
                retrodos_host_running_program(prog, sizeof(prog));
                ImGui::Text("Running: %s", prog[0] ? prog : "DOS");
                ImGui::Separator();
                const ImVec2 bw(ImGui::GetFontSize() * 11.0f, 0);
                if (ImGui::Button("Resume", bw)) show_overlay = false;
                /* One panel at a time. The keyboard and the controls panel
                 * both cover the game, both block keys from reaching DOS, and
                 * stacked they buried each other's close buttons -- a player
                 * ended up wedged with "1" apparently doing nothing. */
                if (ImGui::Button(show_osk ? "Hide keyboard" : "Show keyboard", bw)) {
                    show_osk = !show_osk; show_overlay = false;
                    if (show_osk) show_controls = false;
                }
                if (ImGui::Button("Controls", bw)) {
                    show_controls = true; show_overlay = false; show_osk = false;
                }
                if (ImGui::Button("Ctrl+Alt+Del", bw)) {
                    retrodos::osk_send_ctrl_alt_del(); show_overlay = false;
                }

                /* Laying the controls out belongs here rather than on the
                 * settings page: where a button should sit depends on what the
                 * game is showing underneath it, so it has to be done with the
                 * game on screen. */
                if (pad.gamepad_present()) {
                    ImGui::TextDisabled("Gamepad connected");
                } else if (pad.enabled()) {
                    if (ImGui::Button(pad.editing() ? "Done moving controls"
                                                    : "Move controls", bw)) {
                        const bool now = !pad.editing();
                        pad.set_editing(now);
                        show_overlay = false;
                        if (!now) {
                            /* Persist on leaving edit mode: a layout that is
                             * lost on exit is worse than none. */
                            cfg.pad_layout = pad_layout_to_string(pad.controls());
                            retrodos::save_app_config(cfg_path, cfg);
                        }
                    }
                    if (pad.editing() && ImGui::Button("Reset control layout", bw)) {
                        pad.reset_layout(win_w, win_h);
                        cfg.pad_layout = pad_layout_to_string(pad.controls());
                        retrodos::save_app_config(cfg_path, cfg);
                    }
                }
                if (ImGui::Button("Reset machine", bw)) {
                    retrodos_host_reset(true); show_overlay = false;
                }
                if (ImGui::Button("Quit to library", bw)) {
                    retrodos_host_quit(); show_overlay = false;
                }
                ImGui::End();
            }

            /* ---------------- In-game control mapping ---------------- */
            /* Remapping belongs here as much as on the settings page: which
             * key a button should send is a question you only answer while
             * looking at the game, and edits apply to the LIVE settings, so a
             * change can be tried without leaving. */
            if (view == View::Emulator && show_controls) {
                ImGui::SetNextWindowPos(ImVec2(origin.x + full.x * 0.5f, origin.y + full.y * 0.5f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(full.x * 0.92f, full.y * 0.86f));
                ImGui::SetNextWindowBgAlpha(0.94f);
                ImGui::Begin("Controls", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

                ImGui::Text("Controls - %s", playing.empty() ? "this game"
                                                             : playing.c_str());
                ImGui::TextDisabled("Changes take effect immediately.");
                ImGui::Separator();

                ImGui::BeginChild("##ctl",
                                  ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f),
                                  0, ImGuiWindowFlags_NoScrollbar);
                scroll_by_drag();
                controls_widgets(active);
                ImGui::EndChild();

                if (ImGui::Button("Save for this game")) {
                    if (!playing.empty())
                        retrodos::save_game_settings(games_dir, playing, active);
                    show_controls = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Close")) show_controls = false;
                ImGui::SameLine();
                ImGui::TextDisabled("Unsaved changes last until the game exits.");
                ImGui::End();
            }

            /* ---------------- Always-on emulator controls ---------------- */
            /* A handheld has no Escape key and often no usable Back button, so
             * without this the menu and the on-screen keyboard are unreachable
             * once a game is running -- the emulator becomes a one-way trip.
             * Kept small and translucent, and parked in the top-right where DOS
             * games put the least. */
            if (view == View::Emulator && !show_overlay && !show_controls) {
                const float pad = ImGui::GetStyle().WindowPadding.x;
                ImGui::SetNextWindowPos(ImVec2(origin.x + full.x - pad, origin.y + pad),
                                        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                ImGui::SetNextWindowBgAlpha(0.35f);
                ImGui::Begin("##emubar", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing);
                if (ImGui::Button("Menu")) show_overlay = true;
                ImGui::SameLine();

                /* Joystick and keys, one tap each. These are the two switches
                 * a player actually throws mid-game -- a title's setup screen
                 * wants the stick dead while it reads the keyboard, then wants
                 * it back -- and a panel is too much ceremony for that. Lit
                 * when active, like the nav rail. Saved to the game
                 * immediately: a toggle that silently reverts on the next
                 * launch reads as broken, and the joystick one MUST persist to
                 * matter, because detection happens as the game boots. */
                {
                    auto toggle_chip = [&](const char *label, bool &flag) {
                        const bool on = flag;
                        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        if (ImGui::Button(label)) {
                            flag = !flag;
                            if (!playing.empty())
                                retrodos::save_game_settings(games_dir, playing, active);
                        }
                        if (on) ImGui::PopStyleColor();
                    };
                    toggle_chip("Joy",  active.pad_sends_joystick);
                    ImGui::SameLine();
                    toggle_chip("Keys", active.pad_sends_keys);
                }
                ImGui::SameLine();

                /* One tap to the mapping panel. Burying it behind Menu ->
                 * Controls made the most-adjusted thing in a DOS session --
                 * which keys the pad sends, whether the stick is live -- two
                 * presses away and invisible; a player who never opens the
                 * menu never learns it exists. */
                if (ImGui::Button("Pad")) { show_controls = true; show_osk = false; }
                ImGui::SameLine();
                if (ImGui::Button(show_osk ? "Hide keys" : "Keyboard")) show_osk = !show_osk;
                ImGui::End();
            }

            /* Under the OSK and the overlay: when either is up the player is
             * not driving the game, and a pad drawn on top of a keyboard is
             * just clutter. */
            if (view == View::Emulator && !show_osk && !show_overlay && !show_controls)
                pad.draw(win_w, win_h);

            if (view == View::Emulator && show_osk)
                retrodos::osk_draw(full.x, full.y);

            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        }

        SDL_RenderPresent(ren);
    }

    if (engine.joinable()) { retrodos_host_quit(); engine.join(); }
    retrodos::save_app_config(cfg_path, cfg);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (fb_tex) SDL_DestroyTexture(fb_tex);
    retrodos::brand_shutdown();      /* before the renderer that owns its texture */
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

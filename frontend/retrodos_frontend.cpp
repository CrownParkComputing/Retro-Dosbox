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
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/mman.h>
#include <semaphore.h>
#endif
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
#include "retrodos_touchpad.h"
#include "retrodos_touchmouse.h"

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
    bool        is_app   = false;  /* from apps/, shown under Applications */
    std::string slug;              /* RetroMedia catalogue slug, when matched */
    /* A complete dosbox.conf to use verbatim instead of building one from the
     * Settings. A Windows guest needs sections a DOS game never wants -- a
     * reported DOS version of 7.1, int13 v86 fakery, a particular Sound
     * Blaster -- and threading those through build_conf as special cases would
     * put Windows-only behaviour in every game's way. */
    std::string conf_override;

    /* A Windows machine rather than a DOS game. Its folder holds a disk image
     * and a recorded install phase, never an executable, so the Library must
     * not offer it a DOS "Setup" program or describe it as having no runnable
     * -- both of which it did, and both of which ended at a C:\> prompt. */
    bool        is_machine = false;
    retrodos::Win98Phase machine_phase = retrodos::Win98Phase::Create;
    retrodos::OsKind     machine_os    = retrodos::OsKind::Win98;

    /* Open the on-screen keyboard the moment this starts. Set for anything
     * that stops and waits for typing before it has shown the user any way
     * to type: the FreeDOS floppy (its installer asks Y/N at once) and a
     * Windows machine mid-install (BIOS DEL, Setup's own prompts). Without
     * it the first thing a new starter sees is a question they cannot
     * answer, and the only way past is a Bluetooth keyboard. */
    bool        keyboard_on_start = false;
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

    /* Everything the game's own dosbox.conf says about the machine, so a
     * title that shipped a working config gets it honoured -- the CPU and
     * core it wants, its memory, video card, sound, DOS version. Not [sdl]
     * (we own the window and the gamelink output) and not [autoexec] (run
     * separately). Appended after our defaults, so the game's keys win. */
    static const char *kSections[] = { "[dosbox]", "[cpu]", "[dos]", "[render]",
                                       "[video]", "[sblaster]", "[mixer]", "[midi]",
                                       "[speaker]", "[gus]", "[joystick]", "[pci]",
                                       "[voodoo]" };
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
            /*
             * Heal a flattened repack. Some game archives nest the disc in a
             * cd/ folder and the files in a game/ folder, with a conf that
             * mounts them from there -- and an unzip that flattened the
             * archive leaves the conf pointing at folders that no longer
             * exist. A path whose folder is gone but whose file is now at the
             * top is rewritten to the top, so the mount finds it anyway.
             */
            size_t sp = line.find_last_of(' ');
            /* the last space-separated token that is not a -flag/-t value */
            std::vector<std::string> toks;
            { std::string t; for (char c : line) { if (c==' '){ if(!t.empty()){toks.push_back(t);t.clear();} } else t.push_back(c);} if(!t.empty()) toks.push_back(t); }
            for (size_t ti = 0; ti < toks.size(); ++ti) {
                std::string &tk = toks[ti];
                if (tk.empty() || tk[0]=='-') continue;
                if (tk.find('/') == std::string::npos) continue;
                std::string rel = tk;
                if (rel.rfind("./",0)==0) rel = rel.substr(2);
                const std::string full = dir + "/" + rel;
                SDL_PathInfo pi;
                if (!SDL_GetPathInfo(full.c_str(), &pi)) {
                    const std::string leaf = rel.substr(rel.find_last_of("/\\") + 1);
                    if (SDL_GetPathInfo((dir + "/" + leaf).c_str(), &pi)) tk = leaf;
                }
            }
            line.clear();
            for (size_t ti=0; ti<toks.size(); ++ti){ if(ti) line+=" "; line+=toks[ti]; }
            (void)sp;
        } else if (SDL_strncasecmp(line.c_str(), "cd ", 3) == 0 ||
                   SDL_strncasecmp(line.c_str(), "cd\t", 4) == 0) {
            /* cd into a folder the flattening removed: the files are at the
             * top now, so staying put is correct. Drop the line. */
            std::string arg = line.substr(3);
            while (!arg.empty() && (arg.front()==' '||arg.front()=='\\'||arg.front()=='/')) arg.erase(arg.begin());
            for (char &c : arg) if (c=='\\') c='/';
            if (!arg.empty() && arg != "\\" && arg != "/") {
                SDL_PathInfo pi;
                if (!SDL_GetPathInfo((dir + "/" + arg).c_str(), &pi) || pi.type != SDL_PATHTYPE_DIRECTORY)
                    continue;   /* skip the broken cd */
            }
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
    /* A Windows machine is recognised before anything else is looked at.
     *
     * Its folder does contain executables -- an installed Windows is full of
     * them -- but none of them is what starting it means, and picking one at
     * random is how the Library came to run COMMAND.COM inside a mounted
     * Windows directory and call it a game. */
    if (retrodos::win98_is_install_dir(dir)) {
        retrodos::Win98Install w;
        g.is_machine = true;
        if (retrodos::win98_load(dir, w)) {
            g.machine_phase = w.phase;
            g.machine_os    = w.os;
            g.run = retrodos::win98_phase_name(w);
        } else {
            g.run = "Windows 98";
        }
        return;
    }

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
        if (char *pref = SDL_GetPrefPath("CrownParkComputing", RETRODOS_APP_NAME)) {
            out.push_back(std::string(pref) + "dos");
            SDL_free(pref);
        }
    }
#else
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", RETRODOS_APP_NAME)) {
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
/*
 * Disc and disk images lying about in a folder, for the "change the disc"
 * panel.
 *
 * Top level only. The glob recurses, and an image three folders down is
 * usually part of a game that already mounts it for itself -- offering it as
 * something to shove into the running machine is noise, not a feature.
 */
std::vector<std::string> scan_images(const std::string &dir, bool cd)
{
    static const char *const kCd[]     = { "*.iso", "*.cue", "*.bin", "*.chd",
                                           "*.mdf", "*.gog", "*.ins", "*.inst",
                                           nullptr };
    static const char *const kFloppy[] = { "*.ima", "*.img", "*.xdf", "*.fdi",
                                           "*.hdm", "*.nfd", "*.d88", "*.td0",
                                           nullptr };

    std::vector<std::string> out;
    if (dir.empty()) return out;

    SDL_Storage *st = SDL_OpenFileStorage(dir.c_str());
    if (!st) return out;
    for (const char *const *pat = cd ? kCd : kFloppy; *pat; ++pat) {
        int n = 0;
        char **found = SDL_GlobStorageDirectory(st, nullptr, *pat,
                                                SDL_GLOB_CASEINSENSITIVE, &n);
        if (!found) continue;
        for (int i = 0; i < n && found[i]; ++i) {
            if (SDL_strchr(found[i], '/')) continue;
            const std::string full = dir + "/" + found[i];
            if (!cd) {
                /* .img is also what hard disk images are called; a floppy
                 * is at most 2.88 MB, so anything bigger is not one. */
                SDL_PathInfo info;
                if (SDL_GetPathInfo(full.c_str(), &info) && info.size > 4u * 1024 * 1024)
                    continue;
            }
            out.push_back(full);
        }
        SDL_free(found);
    }
    SDL_CloseStorage(st);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/* The part of a path a person recognises. */
std::string base_name(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/* Where disc images are kept. Empty in the config means "with the games",
 * which is where they are until somebody says otherwise. */
std::string disc_root(const retrodos::AppConfig &cfg)
{
    return cfg.iso_root.empty() ? cfg.library_root : cfg.iso_root;
}

std::string size_label(const std::string &path)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info) || info.size <= 0) return std::string();
    const double mb = (double)info.size / (1024.0 * 1024.0);
    char buf[32];
    if (mb >= 1024.0) snprintf(buf, sizeof buf, "%.1f GB", mb / 1024.0);
    else              snprintf(buf, sizeof buf, "%.0f MB", mb);
    return buf;
}

/*
 * Small square buttons with a drawn glyph: + and - for adding and removing a
 * drive, and the insert / eject symbols from the front of a real drive for
 * its media. Drawn rather than typed because the UI font has no eject glyph,
 * and a button that says "Eject" in eight letters is wider than the thing it
 * ejects.
 */
enum class Icon { Plus, Minus, Insert, Eject };

bool icon_button(const char *id, Icon icon, const char *tip = nullptr)
{
    const float sz = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive
                                      : hover ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), bg, ImGui::GetStyle().FrameRounding);
    const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    const float r = sz * 0.28f, t = std::max(1.5f, sz * 0.09f);
    switch (icon) {
    case Icon::Plus:
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, t);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), fg, t);
        break;
    case Icon::Minus:
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, t);
        break;
    case Icon::Eject:
        /* A triangle over a bar: the symbol on the drive button itself. */
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r, c.y + r * 0.35f),
                              ImVec2(c.x + r, c.y + r * 0.35f), fg);
        dl->AddRectFilled(ImVec2(c.x - r, c.y + r * 0.6f), ImVec2(c.x + r, c.y + r * 0.6f + t), fg);
        break;
    case Icon::Insert:
        /* The same, the other way up: media going in. */
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.6f - t), ImVec2(c.x + r, c.y - r * 0.6f), fg);
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.35f), ImVec2(c.x + r, c.y - r * 0.35f),
                              ImVec2(c.x, c.y + r), fg);
        break;
    }
    if (tip && hover) ImGui::SetTooltip("%s", tip);
    return pressed;
}

/*
 * Take a folder the user picked as the parent folder.
 *
 * If they picked one of the sub-folders the app makes -- games/, discs/ and
 * so on, which is an easy thing to do when that is the folder with the files
 * in it -- the parent is what they meant: otherwise games would be read from
 * games/games and the library would say it found nothing.
 */
void choose_storage_root(retrodos::AppConfig &cfg, std::string root)
{
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    static const char *const subs[] = { "games", "apps", "discs", "machines",
                                        "dos", "windows", "freedos" };
    for (int pass = 0; pass < 2; ++pass) {
        const std::string leaf = base_name(root);
        bool is_sub = false;
        for (const char *sname : subs) if (SDL_strcasecmp(leaf.c_str(), sname) == 0) is_sub = true;
        if (!is_sub) break;
        const size_t cut = root.find_last_of('/');
        if (cut == std::string::npos || cut == 0) break;
        root = root.substr(0, cut);
    }
    cfg.storage_root = root;
    retrodos::apply_storage_root(cfg);
}

/* The drive at one letter, or nullptr. A drive may be empty (no path). */
retrodos::DriveImage *drive_at(std::vector<retrodos::DriveImage> &drives, char letter)
{
    for (auto &d : drives) if (d.letter == letter) return &d;
    return nullptr;
}

/*
 * Add a drive of one kind and give it a letter the way a PC would: floppies
 * take A: then B:; a hard disk takes C: if nothing else has it, otherwise the
 * next free letter from D:; a CD-ROM the next free letter from D:. Returns
 * the letter, or 0 when there is no room. [c_taken] says C: is the loaded
 * title's folder and not available.
 */
char drive_add(std::vector<retrodos::DriveImage> &drives, retrodos::DriveImage::Kind kind,
               bool c_taken)
{
    auto free_letter = [&](char from, char to) -> char {
        for (char L = from; L <= to; ++L)
            if (!drive_at(drives, L) && !(L == 'C' && c_taken)) return L;
        return 0;
    };
    char L = 0;
    if (kind == retrodos::DriveImage::Floppy) L = free_letter('A', 'B');
    else if (kind == retrodos::DriveImage::Hdd) L = free_letter('C', 'Z');
    else L = free_letter('D', 'Z');
    if (!L) return 0;
    retrodos::DriveImage d; d.letter = L; d.kind = kind;
    drives.push_back(d);
    std::sort(drives.begin(), drives.end(),
              [](const retrodos::DriveImage &x, const retrodos::DriveImage &y) {
                  return x.letter < y.letter; });
    return L;
}

void drive_remove(std::vector<retrodos::DriveImage> &drives, char letter)
{
    drives.erase(std::remove_if(drives.begin(), drives.end(),
        [&](const retrodos::DriveImage &d) { return d.letter == letter; }), drives.end());
}

/* Put an image in an existing drive. The same image cannot be in two drives,
 * so it leaves the other one. */
void drive_insert(std::vector<retrodos::DriveImage> &drives, char letter, const std::string &path)
{
    for (auto &d : drives) if (d.path == path) d.path.clear();
    if (retrodos::DriveImage *d = drive_at(drives, letter)) d->path = path;
}

void drive_eject(std::vector<retrodos::DriveImage> &drives, char letter)
{
    if (retrodos::DriveImage *d = drive_at(drives, letter)) d->path.clear();
}

const char *drive_kind_name(retrodos::DriveImage::Kind k)
{
    return k == retrodos::DriveImage::Floppy ? "floppy drive"
         : k == retrodos::DriveImage::Hdd    ? "hard disk" : "CD-ROM drive";
}

/*
 * A shelf of images -- CDs, floppies or hard disks: a row of initials, a
 * search button that opens a modal (an always-present field threw the
 * on-screen keyboard over half the shelf), and the images filling the rest
 * of the page. Each row has a button per drive letter it may go in; the
 * Launch page mounts whatever is in the drives.
 *
 * [letters] are the drives this kind of image may be put in. [blocked] is a
 * letter that is currently spoken for by something else -- C: while a title
 * is loaded -- and is shown but not offered.
 */
/* Returns true when an image was put in a drive, so the caller can go back
 * to Launch: choosing is done, and Launch is where the result shows. */
/* [delete_pick], when given, puts a delete icon on every row that is not a
 * machine's own disk (those are erased from the machine's page, on purpose);
 * pressing it stores the path there for the caller to confirm and act on. */
bool media_shelf(const char *id, const char *noun, const std::vector<std::string> &paths,
                 std::vector<retrodos::DriveImage> &drives, retrodos::DriveImage::Kind kind,
                 char &insert_target, float cw, const std::string &folder,
                 std::string *delete_pick = nullptr)
{
    bool inserted = false;
    /* The drives this kind of image can go in: the ones of its kind that
     * have been added to the machine. None means the shelf can only look. */
    std::string letters;
    for (const auto &d : drives) if (d.kind == kind) letters.push_back(d.letter);
    /* Drives are added on Launch, not here: this page only fills them. */
    if (letters.empty())
        ImGui::TextDisabled("No %s on the machine. Add one on Launch.", drive_kind_name(kind));
    else if (insert_target && letters.find(insert_target) != std::string::npos)
        ImGui::TextDisabled("Tap an image to put it in %c:", insert_target);
    else
        ImGui::TextDisabled("Press a drive letter to put an image in it.");

    ImGui::PushID(id);
    static std::map<std::string, std::string> searches;
    static std::map<std::string, char> initials;
    std::string &search = searches[id];
    char &letter = initials[id];
    const float fs = ImGui::GetFontSize();

    auto initial_of = [](const std::string &path) {
        const std::string n = base_name(path);
        const char c = n.empty() ? '#' : (char)SDL_toupper((unsigned char)n[0]);
        return (c >= 'A' && c <= 'Z') ? c : '#';
    };

    {
        char label[128];
        if (!search.empty()) SDL_snprintf(label, sizeof label, "Search: %s  X", search.c_str());
        else                 SDL_snprintf(label, sizeof label, "Search...");
        if (ImGui::Button(label, ImVec2(0, fs * 1.9f))) {
            if (!search.empty()) search.clear();
            else ImGui::OpenPopup("Find");
        }
        if (ImGui::BeginPopupModal("Find", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char box[96];
            ImGui::SetNextItemWidth(fs * 18.0f);
            const bool go = ImGui::InputTextWithHint("##term", "part of a name", box,
                                                     sizeof box,
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
            ImGui::Spacing();
            if (go || ImGui::Button("Search", ImVec2(fs * 8.0f, 0))) {
                search = box; box[0] = 0; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(fs * 8.0f, 0))) {
                box[0] = 0; ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    {
        bool has[27] = { false };
        for (const std::string &p : paths) {
            const char c = initial_of(p);
            has[c == '#' ? 26 : c - 'A'] = true;
        }
        int count = 1;
        for (bool b : has) if (b) ++count;
        if (letter) {
            const bool still = letter == '#' ? has[26] : has[letter - 'A'];
            if (!still) letter = 0;
        }
        const float gapx = 3.0f;
        const float cell = (ImGui::GetContentRegionAvail().x - gapx * (float)(count - 1))
                         / (float)count;
        bool first = true;
        auto chip = [&](const char *label, char value) {
            if (!first) ImGui::SameLine(0.0f, gapx);
            first = false;
            const bool on = (letter == value);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label, ImVec2(cell, 0))) letter = on ? 0 : value;
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
        if (has[26]) chip("0-9", '#');
    }

    ImGui::BeginChild("##shelf", ImVec2(0, 0));
    scroll_by_drag();
    int shown = 0;
    const float row_h = fs * 2.2f;
    const size_t nletters = letters.size();
    const float btn_w = fs * 2.2f;
    const float names_w = cw - (btn_w + ImGui::GetStyle().ItemSpacing.x) * (float)nletters
                        - fs * 6.0f;
    for (const std::string &path : paths) {
        const std::string name = base_name(path);
        if (letter && initial_of(path) != letter) continue;
        if (!search.empty() && !SDL_strcasestr(name.c_str(), search.c_str())) continue;
        ++shown;
        ImGui::PushID(path.c_str());
        char in_letter = 0;
        for (const auto &d : drives) if (d.path == path) in_letter = d.letter;
        const bool targeting = insert_target && letters.find(insert_target) != std::string::npos;
        if (ImGui::Selectable(name.c_str(), in_letter != 0,
                              targeting ? 0 : ImGuiSelectableFlags_Disabled,
                              ImVec2(names_w, row_h)) && targeting) {
            drive_insert(drives, insert_target, path);
            insert_target = 0;
            inserted = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", size_label(path).c_str());
        {
            /* Where it lives, relative to the folder, so two discs with one
             * name in two places can be told apart. */
            const size_t cut = path.find_last_of('/');
            std::string where = cut == std::string::npos ? "" : path.substr(0, cut);
            if (where.compare(0, folder.size(), folder) == 0) where = where.substr(folder.size());
            if (!where.empty() && where[0] == '/') where = where.substr(1);
            if (!where.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s/", where.c_str()); }
        }
        for (size_t i = 0; i < nletters; ++i) {
            const char L = letters[i];
            ImGui::SameLine();
            const bool here = (in_letter == L);
            const char lbl[3] = { L, ':', 0 };
            if (here) {
                ImGui::TextDisabled("%s", lbl);
                ImGui::SameLine(0.0f, 2.0f);
                char eid[8]; SDL_snprintf(eid, sizeof eid, "##ej%c", L);
                if (icon_button(eid, Icon::Eject, "Eject")) { drive_eject(drives, L); insert_target = 0; }
            } else if (ImGui::Button(lbl, ImVec2(btn_w, 0))) {
                drive_insert(drives, L, path); inserted = true;
                insert_target = 0;
            }
        }
        if (delete_pick) {
            const std::string dir = path.substr(0, path.find_last_of('/'));
            if (!retrodos::win98_is_install_dir(dir)) {
                ImGui::SameLine();
                if (icon_button("##del", Icon::Minus, "Delete this image")) *delete_pick = path;
            }
        }
        ImGui::PopID();
    }
    if (paths.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("No %s images yet. Put them in:", noun);
        ImGui::TextDisabled("%s", folder.c_str());
    } else if (!shown) {
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing matches.");
    }
    ImGui::EndChild();
    ImGui::PopID();
    return inserted;
}

/* Hard disk images: the loose ones in the DOS discs folder. */
/*
 * Every image of one kind anywhere under the chosen folder.
 *
 * Recursive, because the folder is the user's and its layout is theirs: a
 * Windows CD under discs/windows, a game's own CD beside the game, a
 * collection of floppies in a folder of their own. A shelf that only looked
 * in one sub-folder showed a fraction of what was there and said "no media".
 *
 * The machines' own disks are on the hard disk shelf like any other: put one
 * in C: with nothing loaded and Launch boots it as that machine. Skipped are
 * the app's hidden staging folders and anything under a dot-folder.
 */
enum class MediaKind { Cd, Floppy, Hdd };

std::vector<std::string> scan_media(const retrodos::AppConfig &cfg, MediaKind kind)
{
    static const char *const kCd[]     = { ".iso", ".cue", ".chd", ".mdf", ".gog",
                                           ".ins", ".inst", nullptr };
    static const char *const kFloppy[] = { ".ima", ".img", ".xdf", ".fdi", ".hdm",
                                           ".nfd", ".d88", ".td0", nullptr };
    static const char *const kHdd[]    = { ".img", ".vhd", ".hdd", nullptr };
    const char *const *exts = kind == MediaKind::Cd ? kCd
                            : kind == MediaKind::Floppy ? kFloppy : kHdd;

    std::vector<std::string> out;
    const std::string root = cfg.storage_root.empty() ? cfg.library_root : cfg.storage_root;
    if (root.empty()) return out;

    /*
     * Walked by hand, one directory at a time.
     *
     * SDL_GlobStorageDirectory looked like a recursive search and is not one
     * in the way that matters: its '*' does not cross a '/', so "*.iso" only
     * ever matched files in the top folder. Every shelf under a parent folder
     * with sub-folders was empty, and said so.
     */
    std::vector<std::string> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const std::string dir = stack.back();
        stack.pop_back();
        int n = 0;
        char **found = SDL_GlobDirectory(dir.c_str(), "*", 0, &n);
        if (!found) continue;
        for (int i = 0; i < n && found[i]; ++i) {
            const std::string name = found[i];
            if (name.empty() || name[0] == '.') continue;      /* .staged, .downloading */
            const std::string full = dir + "/" + name;
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(full.c_str(), &info)) continue;
            if (info.type == SDL_PATHTYPE_DIRECTORY) {
                /* Deep enough for any sensible layout, shallow enough that a
                 * folder with a symlink loop in it does not hang the app. */
                if (std::count(full.begin(), full.end(), '/') -
                    std::count(root.begin(), root.end(), '/') < 6)
                    stack.push_back(full);
                continue;
            }
            if (info.type != SDL_PATHTYPE_FILE) continue;
            bool match = false;
            for (const char *const *e = exts; *e && !match; ++e) {
                const size_t el = SDL_strlen(*e);
                match = name.size() > el &&
                        SDL_strcasecmp(name.c_str() + name.size() - el, *e) == 0;
            }
            if (!match) continue;
            /* .img is both a floppy and a hard disk; size tells them apart. */
            const bool big = info.size > 4u * 1024 * 1024;
            if (kind == MediaKind::Floppy && big) continue;
            if (kind == MediaKind::Hdd && !big) continue;
            out.push_back(full);
        }
        SDL_free(found);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
        const int c = SDL_strcasecmp(base_name(a).c_str(), base_name(b).c_str());
        return c ? c < 0 : a < b; });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/*
 * The names in the root directory of an ISO 9660 image -- enough to see
 * whether a DOS CD has a program to run straight off the disc, which most of
 * them do. Primary volume descriptor at sector 16, root record inside it,
 * one pass over the root extent. Nothing deeper is looked at, for the same
 * reason the folder scan stops at the top: what goes into the autoexec has
 * to be a name DOS can run from the drive's root.
 */
std::vector<std::string> iso_root_names(const std::string &path)
{
    std::vector<std::string> out;
    if (!ends_with_ci(path, ".iso")) return out;
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return out;
    unsigned char pvd[2048];
    if (SDL_SeekIO(io, 16 * 2048, SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(io, pvd, sizeof pvd) == sizeof pvd
        && SDL_memcmp(pvd + 1, "CD001", 5) == 0) {
        const unsigned char *root = pvd + 156;
        const Uint32 extent = (Uint32)root[2] | ((Uint32)root[3] << 8) | ((Uint32)root[4] << 16) | ((Uint32)root[5] << 24);
        const Uint32 size   = (Uint32)root[10] | ((Uint32)root[11] << 8) | ((Uint32)root[12] << 16) | ((Uint32)root[13] << 24);
        if (size > 0 && size < (1u << 22) && SDL_SeekIO(io, (Sint64)extent * 2048, SDL_IO_SEEK_SET) >= 0) {
            std::vector<unsigned char> dir(size);
            if (SDL_ReadIO(io, dir.data(), size) == size) {
                size_t off = 0;
                while (off + 33 <= size) {
                    const unsigned len = dir[off];
                    if (len == 0) { off = (off / 2048 + 1) * 2048; continue; }   /* sector padding */
                    const unsigned nlen = dir[off + 32];
                    const bool is_dir = (dir[off + 25] & 2) != 0;
                    if (!is_dir && nlen > 1 && off + 33 + nlen <= size) {
                        std::string name((const char *)&dir[off + 33], nlen);
                        const size_t semi = name.find(';');
                        if (semi != std::string::npos) name = name.substr(0, semi);
                        if (!name.empty() && name.back() == '.') name.pop_back();
                        out.push_back(name);
                    }
                    off += len;
                }
            }
        }
    }
    SDL_CloseIO(io);
    return out;
}

/* The program to run from a list of root names, by the folder scan's rules:
 * a batch file first (run/start/play preferred), then .com, then .exe, with
 * setup/install programs only as a last resort. */
std::string pick_runnable(const std::vector<std::string> &names, const std::string &hint)
{
    /* A program named like the disc itself wins outright: settlers.iso has
     * QUICK.BAT, QUICKN.BAT, SETTLERN.BAT and SETTLERS.BAT in its root, and
     * alphabetical order picks QUICK, which skips the intro and hangs. The
     * one the disc is named after is the one its own instructions run. */
    if (hint.size() >= 4) {
        for (const std::string &f : names) {
            const size_t dot = f.find_last_of('.');
            if (dot == std::string::npos) continue;
            const std::string stem = f.substr(0, dot);
            const bool runnable = ends_with_ci(f, ".bat") || ends_with_ci(f, ".exe") ||
                                  ends_with_ci(f, ".com");
            if (runnable && SDL_strcasecmp(stem.c_str(), hint.c_str()) == 0) return f;
        }
    }
    std::string bat, com, exe, fb_bat, fb_com, fb_exe;
    for (const std::string &f : names) {
        if (ends_with_ci(f, ".bat")) {
            const bool preferred = SDL_strcasestr(f.c_str(), "run") ||
                                   SDL_strcasestr(f.c_str(), "start") ||
                                   SDL_strcasestr(f.c_str(), "play");
            if (is_support_script(f)) { if (fb_bat.empty()) fb_bat = f; }
            else if (bat.empty() || preferred) bat = f;
        } else if (ends_with_ci(f, ".com")) {
            if (is_support_script(f)) { if (fb_com.empty()) fb_com = f; }
            else if (com.empty()) com = f;
        } else if (ends_with_ci(f, ".exe")) {
            if (is_support_script(f)) { if (fb_exe.empty()) fb_exe = f; }
            else if (exe.empty()) exe = f;
        }
    }
    if (bat.empty()) bat = fb_bat;
    if (com.empty()) com = fb_com;
    if (exe.empty()) exe = fb_exe;
    return !bat.empty() ? bat : (!com.empty() ? com : exe);
}

/*
 * Predefined PCs. Each is a whole machine -- video card, CPU and speed,
 * memory, sound -- so choosing one on Launch is choosing the era the title
 * came from rather than seven settings. Values are the ones the Machine page
 * offers, so a preset is only ever a shortcut to what could be set by hand.
 */
struct PcPreset {
    const char *name;
    const char *machine, *cputype, *sbtype;
    bool cycles_max; int cycles; int memsize; bool dynamic; bool speaker;
};
static const PcPreset kPresets[] = {
    { "286 / EGA / PC speaker",          "ega",     "386",         "none",   false, 3000,  4,  false, true },
    { "386 / VGA / Sound Blaster 2",     "vgaonly", "386",         "sb2",    false, 12000, 8,  false, true },
    { "486 / SVGA / Sound Blaster Pro",  "svga_s3", "486old",      "sbpro2", false, 40000, 16, true,  true },
    { "Pentium / SVGA / Sound Blaster 16","svga_s3", "pentium",    "sb16",   true,  0,     32, true,  true },
    { "Pentium MMX / SVGA / SB16 (fast)","svga_s3", "pentium_mmx", "sb16",   true,  0,     64, true,  true },
};

void apply_preset(retrodos::Settings &s, const PcPreset &p)
{
    s.machine      = p.machine;
    s.cputype      = p.cputype;
    s.sbtype       = p.sbtype;
    s.cycles_max   = p.cycles_max;
    if (!p.cycles_max) s.cycles_fixed = p.cycles;
    s.memsize      = p.memsize;
    s.core_dynamic = p.dynamic;
    s.pcspeaker    = p.speaker;
}

/* Which preset the settings currently match, or -1 for a hand-made machine. */
int matching_preset(const retrodos::Settings &s)
{
    for (int i = 0; i < (int)SDL_arraysize(kPresets); ++i) {
        const PcPreset &p = kPresets[i];
        if (s.machine == p.machine && s.cputype == p.cputype && s.sbtype == p.sbtype &&
            s.cycles_max == p.cycles_max && (p.cycles_max || s.cycles_fixed == p.cycles) &&
            s.memsize == p.memsize && s.core_dynamic == p.dynamic)
            return i;
    }
    return -1;
}

/*
 * The PC on the Launch page, drawn rather than photographed: a tower with
 * two 5.25" bays and two 3.5" bays whose lights come on when something is in
 * them, and a monitor showing the loaded title. Sized from the space it is
 * given, so it fills a phone in landscape and a desktop window alike.
 */
[[maybe_unused]] void draw_pc(ImDrawList *dl, ImVec2 p, ImVec2 size, const std::vector<retrodos::DriveImage> &drives,
             const char *title)
{
    const float fs = ImGui::GetFontSize();
    const ImU32 beige = IM_COL32(214, 205, 182, 255), beige_d = IM_COL32(160, 150, 128, 255);
    const ImU32 dark  = IM_COL32(40, 42, 48, 255),   screen  = IM_COL32(12, 24, 40, 255);
    const ImU32 led_on = IM_COL32(90, 220, 90, 255), led_off = IM_COL32(60, 70, 60, 255);
    const ImU32 text = IM_COL32(200, 220, 240, 255);

    /* Monitor on the left, tower on the right, both standing on the floor. */
    const float tower_w = size.x * 0.34f, tower_h = size.y * 0.92f;
    const float mon_w = size.x * 0.58f, mon_h = mon_w * 0.78f;
    const ImVec2 floor(p.x, p.y + size.y);
    const ImVec2 t0(floor.x + size.x - tower_w, floor.y - tower_h);
    const ImVec2 t1(floor.x + size.x, floor.y);
    dl->AddRectFilled(t0, t1, beige, fs * 0.3f);
    dl->AddRect(t0, t1, beige_d, fs * 0.3f, 0, 2.0f);

    /* One bay per drive, in letter order: 5.25" for CD-ROMs and hard disks,
     * 3.5" for floppies; the light is on when there is media in it. */
    const float bay_x0 = t0.x + tower_w * 0.12f, bay_x1 = t1.x - tower_w * 0.12f;
    float y = t0.y + tower_h * 0.08f;
    bool has_c = false;
    for (const auto &d : drives) {
        if (d.kind == retrodos::DriveImage::Hdd) { if (d.letter == 'C') has_c = !d.path.empty(); continue; }
        const bool big = d.kind == retrodos::DriveImage::Cd;
        const float h = tower_h * (big ? 0.09f : 0.06f);
        if (y + h > t1.y - tower_h * 0.18f) break;
        dl->AddRectFilled(ImVec2(bay_x0, y), ImVec2(bay_x1, y + h), beige_d, fs * 0.15f);
        if (big)
            dl->AddRectFilled(ImVec2(bay_x0 + 4, y + h * 0.35f), ImVec2(bay_x1 - 4, y + h * 0.5f), dark);
        else
            dl->AddRectFilled(ImVec2(bay_x0 + 6, y + h * 0.4f), ImVec2(bay_x1 - h * 1.2f, y + h * 0.55f), dark);
        dl->AddCircleFilled(ImVec2(bay_x1 - h * (big ? 0.35f : 0.5f), y + h * (big ? 0.78f : 0.5f)),
                            h * (big ? 0.11f : 0.16f), d.path.empty() ? led_off : led_on);
        char lbl[3] = { d.letter, ':', 0 };
        dl->AddText(ImVec2(bay_x0 + 4, y + (big ? 2.0f : -1.0f)), dark, lbl);
        y += h + tower_h * 0.03f;
    }
    /* Power button and its light, C: (the hard disk) has the amber one. */
    dl->AddCircleFilled(ImVec2(t0.x + tower_w * 0.5f, t1.y - tower_h * 0.08f), fs * 0.45f, beige_d);
    dl->AddCircleFilled(ImVec2(t0.x + tower_w * 0.2f, t1.y - tower_h * 0.08f), fs * 0.16f,
                        (has_c || (title && title[0])) ? IM_COL32(240, 180, 60, 255) : led_off);
    dl->AddText(ImVec2(t0.x + tower_w * 0.28f, t1.y - tower_h * 0.08f - fs * 0.5f), dark, "C:");

    /* The monitor. */
    const ImVec2 m0(floor.x, floor.y - mon_h - fs * 0.8f);
    const ImVec2 m1(floor.x + mon_w, floor.y - fs * 0.8f);
    dl->AddRectFilled(m0, m1, beige, fs * 0.4f);
    dl->AddRect(m0, m1, beige_d, fs * 0.4f, 0, 2.0f);
    const ImVec2 s0(m0.x + mon_w * 0.07f, m0.y + mon_h * 0.08f);
    const ImVec2 s1(m1.x - mon_w * 0.07f, m1.y - mon_h * 0.14f);
    dl->AddRectFilled(s0, s1, screen, fs * 0.2f);
    /* stand */
    dl->AddRectFilled(ImVec2(m0.x + mon_w * 0.35f, m1.y), ImVec2(m1.x - mon_w * 0.35f, floor.y), beige_d);
    if (title && title[0]) {
        dl->PushClipRect(s0, s1, true);
        dl->AddText(ImVec2(s0.x + fs * 0.5f, s0.y + fs * 0.4f), text, "C:\\>");
        dl->AddText(ImVec2(s0.x + fs * 0.5f, s0.y + fs * 1.6f), text, title);
        dl->PopClipRect();
    } else {
        dl->AddText(ImVec2(s0.x + fs * 0.5f, s0.y + fs * 0.4f), IM_COL32(90, 110, 130, 255),
                    "No title loaded");
    }
}

/*
 * The Windows half of the Library.
 *
 * One machine and the discs that belong to it. It exists because a Windows
 * guest is not a DOS game and was never comfortable in that list: it has an
 * install to get through before it is worth anything, a disc it needs while it
 * does, and a state -- half installed, installed, running -- that no game has.
 */
void windows_widgets(retrodos::AppConfig &cfg, float cw, retrodos::OsKind os,
                     int &jump_to_os, int &start_os)
{
    const std::string wdir = retrodos::machines_dir(cfg) + "/" + retrodos::os_folder(os);
    retrodos::Win98Install w;
    const bool have = retrodos::win98_is_install_dir(wdir) &&
                      retrodos::win98_load(wdir, w);
    if (!have) {
        if (os == retrodos::OsKind::FreeDos)
            TextDimWrapped("No FreeDOS machine yet. The FreeDOS Setup page makes one.");
        else
            TextDimWrapped("No Windows machine yet. The Windows Setup page makes one.");
        return;
    }

    ImGui::TextUnformatted(retrodos::os_name(os));
    ImGui::SameLine();
    ImGui::TextDisabled("- %s", retrodos::win98_phase_name(w));

    const std::string blocked = retrodos::win98_blocker(w);
    if (!blocked.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", blocked.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const bool ready = (w.phase == retrodos::Win98Phase::Run) && blocked.empty();
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button(os == retrodos::OsKind::FreeDos ? "Start FreeDOS" : "Start Windows",
                      ImVec2(cw * 0.32f, 0)))
        start_os = (int)os;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ready ? "Walkthrough" : "Continue setup", ImVec2(cw * 0.32f, 0)))
        jump_to_os = (int)os;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* The discs, CDs only: a Windows machine takes a CD, and the floppies in
     * the same folder belong to the DOS side of the house. */
    const std::vector<std::string> cds = scan_media(cfg, MediaKind::Cd);
    if (cds.empty()) {
        ImGui::TextWrapped("No disc images anywhere in:");
        ImGui::TextDisabled("%s", cfg.storage_root.empty() ? cfg.library_root.c_str()
                                                           : cfg.storage_root.c_str());
        return;
    }

    ImGui::TextDisabled("CDs - the one in use is the machine's CD-ROM");
    ImGui::Spacing();
    ImGui::BeginChild("##winiso");
    scroll_by_drag();
    for (const std::string &path : cds) {
        ImGui::PushID(path.c_str());
        ImGui::TextUnformatted(base_name(path).c_str());
        ImGui::SameLine(cw * 0.62f);
        ImGui::TextDisabled("%s", size_label(path).c_str());
        ImGui::SameLine(cw * 0.76f);
        if (w.iso == path) {
            ImGui::TextDisabled("in the drive");
        } else if (ImGui::SmallButton("Use this disc")) {
            w.iso = path;
            retrodos::win98_save(w);
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::Separator();
    TextDimWrapped("While the machine is running, Esc then Discs and disks swaps "
                   "the disc without stopping it.");
    ImGui::EndChild();
}

std::string root_label(const std::string &root)
{
#if defined(__APPLE__)
    const std::string tail = root.substr(root.find_last_of('/') + 1);
    return RETRODOS_APP_NAME "  >  " + (tail.empty() ? std::string("dos") : tail)
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

bool path_is_file(const std::string &p)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
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
    ImGui::Checkbox("Show the on-screen controller", &s.onscreen_pad);
    ImGui::TextDisabled("A connected gamepad hides it automatically. Arrange the "
                        "buttons -- drag, resize, add -- from the pause menu's "
                        "Controls panel while a game is running.");
    {
        static const char *const ids[]   = { "dos", "generic", "xbox360", "saturn" };
        static const char *const names[] = { "DOS (stick + fire)", "Joystick",
                                             "360 pad", "Saturn pad" };
        int cur = 0;
        for (int i = 0; i < 4; ++i) if (s.touch_pad == ids[i]) cur = i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::BeginCombo("Layout", names[cur])) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(names[i], i == cur)) s.touch_pad = ids[i];
            ImGui::EndCombo();
        }
    }

    ImGui::Spacing();
    ImGui::Checkbox("Buttons send keys", &s.pad_sends_keys);
    ImGui::Checkbox("Buttons drive the joystick port", &s.pad_sends_joystick);
    TextDimWrapped("Most DOS games are keyboard games, so keys are the default. "
                   "Turn the joystick on for titles that support one: the game "
                   "then finds a stick on the game port from the moment it "
                   "starts, with A and B as its two buttons.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Pointer");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::SliderInt("Speed", &s.mouse_speed, 25, 400, "%d%%");
    TextDimWrapped("100% moves the pointer as far as your finger does. Windows "
                   "adds its own pointer speed on top, so turn this down if the "
                   "pointer runs away from you -- and remember the guest has a "
                   "mouse setting of its own.");
    TextDimWrapped("On glass: drag to move, tap to click, press and hold for a "
                   "right click, double-tap and drag to drag.");

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

/*
 * Which tab to open on. Kept as names rather than an int because the caller is
 * choosing a page, not an index, and the two rail entries that lead here --
 * Machine and Input -- differ by nothing else.
 */
enum class MachineTab { None, Cpu, Video, Sound, Input, Dos };

void settings_widgets(Settings &s, MachineTab open)
{
    /*
     * Tabbed the way DOSBox-X's own menu bar is: CPU, Video, Sound, DOS.
     *
     * Not an arbitrary grouping -- those are literally its top-level menus
     * (def_menu__toplevel in src/gui/menu.cpp), so anyone who has used
     * DOSBox-X already knows which tab a setting is under, and anyone reading
     * its documentation finds our tab named the same as the menu the docs
     * mention. One long scrolling column was also simply hard to search.
     *
     * [open] is honoured ONLY on the frame a page is entered, which is the
     * whole reason it is not a bool any more. The previous version asked for
     * the Input tab on every frame the Input page was up, so that page could
     * never show another tab -- and, because one tab bar serves both entries
     * and ImGui remembers the selection, Machine afterwards opened on Input
     * too. The two rail entries showed the same screen.
     */
    auto want = [&](MachineTab t) -> ImGuiTabItemFlags {
        return (open == t) ? ImGuiTabItemFlags_SetSelected : 0;
    };

    if (!ImGui::BeginTabBar("##machine", ImGuiTabBarFlags_None)) return;

    if (ImGui::BeginTabItem("CPU", nullptr, want(MachineTab::Cpu))) {
        ImGui::Spacing();
#if defined(__APPLE__)
        /* There is no dynamic core on iOS to offer. It recompiles x86 into
         * native code at run time, which needs memory that is both writable
         * and executable, and iOS does not permit that -- so the core is built
         * without it and the interpreter is the only one there is. Offering a
         * "faster" switch that cannot make anything faster is worse than
         * offering nothing: a player toggling it and seeing no change will
         * reasonably conclude the setting is broken. */
        TextDimWrapped("Interpreter core -- iOS does not permit the just-in-time "
                       "recompilation a faster core needs.");
#else
        ImGui::Checkbox("Dynamic core (faster; turn off if a game misbehaves)",
                        &s.core_dynamic);
#endif
        ImGui::Checkbox("Cycles: max", &s.cycles_max);
        if (!s.cycles_max) {
            ImGui::SliderInt("Fixed cycles", &s.cycles_fixed, 300, 100000);
            TextDimWrapped("Early titles busy-wait for timing and run absurdly "
                           "fast on 'max'. A fixed count is what fixes them.");
        }
        {
            static const char *vals[] = { "auto", "386", "486old", "pentium",
                                          "pentium_mmx" };
            static const char *names[] = { "Auto - default", "386", "486",
                                           "Pentium", "Pentium MMX" };
            conf_combo("Reported CPU", s.cputype, vals, names,
                       (int)SDL_arraysize(vals));
        }
        ImGui::Checkbox("Maths coprocessor (FPU)", &s.fpu);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Video", nullptr, want(MachineTab::Video))) {
        ImGui::Spacing();
        {
            static const char *vals[] = { "svga_s3", "svga_s3trio64", "vesa_vbe3",
                                          "vgaonly", "ega", "cga", "tandy", "pcjr" };
            static const char *names[] = { "SVGA (S3 Virge) - default",
                                           "SVGA (S3 Trio64)", "VESA VBE 3",
                                           "VGA only", "EGA", "CGA",
                                           "Tandy 1000", "PCjr" };
            conf_combo("Graphics card", s.machine, vals, names,
                       (int)SDL_arraysize(vals));
        }
        TextDimWrapped("The first thing to change when a game starts and draws "
                       "nothing. Titles written for CGA or Tandy can be confused "
                       "by an S3, and a few refuse it outright.");
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

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::TextUnformatted("On this screen");
        {
            static const char *kAspect[] = { "Auto (as the mode intends)", "4:3",
                                             "16:9", "Fill the screen" };
            int mode = (s.aspect_mode >= 0 && s.aspect_mode < 4) ? s.aspect_mode : 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::Combo("Aspect", &mode, kAspect, 4)) s.aspect_mode = mode;
        }
        ImGui::Checkbox("Integer scaling (sharper, bigger borders)",
                        &s.integer_scale);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Sound", nullptr, want(MachineTab::Sound))) {
        ImGui::Spacing();
        static const char *sb[] = { "sbpro2", "sb16", "sb16vibra", "sbpro1",
                                    "sb2", "sb1", "none" };
        for (int i = 0; i < (int)SDL_arraysize(sb); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(sb[i], s.sbtype == sb[i])) s.sbtype = sb[i];
        }
        TextDimWrapped("Sound Blaster Pro 2 is the safest broad choice for the "
                       "DOS era. sb16vibra is the card Windows 9x has a driver "
                       "for.");
        ImGui::Spacing();
        ImGui::Checkbox("PC speaker", &s.pcspeaker);
        TextDimWrapped("The beeper is the only sound many pre-1990 titles have.");
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Input", nullptr, want(MachineTab::Input))) {
        ImGui::Spacing();
        controls_widgets(s);
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("DOS", nullptr, want(MachineTab::Dos))) {
        ImGui::Spacing();
        ImGui::SliderInt("Memory (MB)", &s.memsize, 1, 64);
        TextDimWrapped("32 MB is the safe default: DOS/4GW 1.97 miscalculates "
                       "with more, and several early-90s titles then refuse to "
                       "start.");
        ImGui::Spacing();
        ImGui::Checkbox("Expanded memory (EMS)", &s.ems);
        ImGui::SameLine();
        ImGui::Checkbox("Upper memory (UMB)", &s.umb);
        TextDimWrapped("Both on suits most games. Turning one off is the "
                       "standard next move when a title will not load at all.");
        {
            static const char *vals[] = { "", "3.3", "5.0", "6.22", "7.0", "7.1" };
            static const char *names[] = { "Default (5.0)", "3.3", "5.0", "6.22",
                                           "7.0", "7.1 (long filenames, FAT32)" };
            conf_combo("Reported DOS", s.dos_ver, vals, names,
                       (int)SDL_arraysize(vals));
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

#if defined(__linux__) && !defined(__ANDROID__)
    /*
     * Prefer X11 (XWayland) over native Wayland.
     *
     * A DOS game steers its pointer with relative mouse motion, which needs
     * SDL's relative mouse mode -- and on several Wayland compositors that
     * mode is unreliable or silently delivers nothing, so the pointer does
     * not move in the guest even though everything reports success. Under
     * X11 relative mode is rock solid. The list is a preference, not a
     * demand: a machine with no X server still falls back to wayland.
     */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");

    /* Clear a GameLink shared-memory segment left behind by a previous run
     * that did not exit cleanly (a crash, or a force-quit). The core warns
     * "MUTEX already exists ... left over from a crash" and then renders into
     * the stale segment, so the guest runs but the window freezes on whatever
     * frame was there. Removing it here means a hard kill never wedges the
     * next launch. Desktop Linux only; the names are the core's own. */
    shm_unlink("/DWD_GAMELINK_MMAP_R4");
    sem_unlink("/DWD_GAMELINK_MUTEX_R4");
#endif

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
    if (!SDL_CreateWindowAndRenderer(RETRODOS_APP_NAME, 1280, 720,
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
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", RETRODOS_APP_NAME)) {
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
    if (cfg.library_root.empty() && !roots.empty()) {
        /* Nothing chosen yet: the first app-owned root is the parent folder
         * until the wizard says otherwise, with games/, discs/ and machines/
         * under it. An older config that already names a games folder is
         * left exactly as it is. */
        cfg.storage_root = roots.front();
        retrodos::apply_storage_root(cfg);
    }
    /* The three derived folders must exist before anything scans them. */
    auto make_storage_dirs = [&]() {
        if (!cfg.storage_root.empty()) SDL_CreateDirectory(cfg.storage_root.c_str());
        SDL_CreateDirectory(cfg.library_root.c_str());
        if (!cfg.apps_root.empty())     SDL_CreateDirectory(cfg.apps_root.c_str());
        if (!cfg.iso_root.empty()) {
            SDL_CreateDirectory(cfg.iso_root.c_str());
            if (!cfg.storage_root.empty()) {
                SDL_CreateDirectory((cfg.iso_root + "/dos").c_str());
                SDL_CreateDirectory((cfg.iso_root + "/windows").c_str());
                SDL_CreateDirectory((cfg.iso_root + "/freedos").c_str());
            }
        }
        if (!cfg.machines_root.empty()) SDL_CreateDirectory(cfg.machines_root.c_str());
    };
    make_storage_dirs();

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
        if (!cfg.apps_root.empty()) {
            for (Game &g : scan_library(cfg.apps_root)) {
                g.is_app = true;
                games.push_back(g);
            }
        }

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
                g.keyboard_on_start = retrodos::demo_wants_keyboard(k);
                g.initial = (char)SDL_toupper((unsigned char)g.name[0]);
                games.push_back(g);
            }
        }
    };
    refresh();

    enum class View { Wizard, Shell, Emulator };
    enum class Page { Launch, Library, Media, Artwork, Downloads,
                      Settings, Account, Windows, FreeDos, Demo, About };
    View view = cfg.wizard_done ? View::Shell : View::Wizard;
    Page page = Page::Launch;
    /* The page drawn on the previous frame, so a page can tell that it has
     * just been opened. */
    Page page_last_frame = Page::Launch;
    /* Which half of the Library is showing. Not Pages of their own -- they are
     * different lists of the same thing, "what you have". Windows appears only
     * once there is a machine to show. */
    /* Set by the Discs list when a disc is chosen for Windows, so the
     * walkthrough opens on the step that was waiting for it. */
    int jump_to_os = -1;          /* -1, or an OsKind */
    /* What the Launch page will start: the title chosen on Games or
     * Applications, and whatever was put in the drives on CDs and Floppies.
     * A copy of the Game rather than an index, because a rescan reorders
     * the list under it. */
    Game        loaded_game;
    bool        loaded = false;
    std::vector<retrodos::DriveImage> drives;   /* the machine's drives, and what is in them */
    char insert_target = 0;                     /* a drive letter Media should fill next */
    int  media_tab = 0;
    /* The guided-launch walkthrough on the Launch page: wiz_kind is what the
     * user said they are doing, wiz_step which drive prompt they are on. */
    int  wiz_kind = 0;   /* 1 = play a CD, 2 = install to a disk, 3 = boot a disk, 4 = DOS prompt */
    int  wiz_step = 0;   /* 1..N a drive prompt; N+1 the ready-to-launch step */
    /* A PC starts with a floppy drive, a hard disk and a CD-ROM; more can be
     * added and any can be removed. */
    drive_add(drives, retrodos::DriveImage::Floppy, false);
    drive_add(drives, retrodos::DriveImage::Hdd, false);
    drive_add(drives, retrodos::DriveImage::Cd, false);
    /* Set by the Windows list. Acted on after it has finished drawing, because
     * launch() tears down this frame's UI state. */
    int start_os = -1;            /* -1, or an OsKind */
    /* Something the Library needs to tell the user about the last thing they
     * pressed -- a game that turned out to have nothing in it, usually. */
    std::string library_note;

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

    /* The customizable on-screen controller (retrodos_touchpad). Its Sink
     * produces the same PAD_* bits a physical gamepad does, so the whole
     * downstream pipeline -- pad_keys binding, keys-vs-joystick -- is shared.
     * The old VirtualPad is kept only to track whether a gamepad is present. */
    touchpad::Overlay tpad;
    unsigned touch_buttons = 0;
    auto tpad_reload = [&]() {
        const touchpad::Profile *pr = touchpad::profile_by_id(cfg.defaults.touch_pad);
        if (!pr) pr = &touchpad::profile_dos();
        tpad.set(pr, touchpad::Layout::load(cfg_dir, *pr));
    };
    tpad_reload();
    touchpad::Sink tpad_sink;
    tpad_sink.directions = [&](bool u, bool d, bool l, bool r) {
        auto set = [&](int bit, bool on) {
            if (on) touch_buttons |= (1u << bit); else touch_buttons &= ~(1u << bit); };
        set(retrodos::PAD_UP, u); set(retrodos::PAD_DOWN, d);
        set(retrodos::PAD_LEFT, l); set(retrodos::PAD_RIGHT, r);
    };
    tpad_sink.action = [&](const std::string &id, bool down) {
        int bit = -1;
        if      (id == "fire1" || id == "a") bit = retrodos::PAD_A;
        else if (id == "fire2" || id == "b") bit = retrodos::PAD_B;
        else if (id == "x")     bit = retrodos::PAD_X;
        else if (id == "y")     bit = retrodos::PAD_Y;
        else if (id == "start") bit = retrodos::PAD_START;
        else if (id == "back")  bit = retrodos::PAD_SELECT;
        if (bit >= 0) { if (down) touch_buttons |= (1u << bit);
                        else      touch_buttons &= ~(1u << bit); }
    };
#if defined(__ANDROID__) || defined(__APPLE__)
    const bool tpad_platform = true;
#else
    const bool tpad_platform = false;
#endif
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
    /* Default to the owner's email so RetroMedia login is one field, not two,
     * on a fresh install. Only the identity is filled in -- never a password. */
    if (m_email[0] == '\0')
        SDL_strlcpy(m_email, "jonathanmarkwhittingham@gmail.com", sizeof(m_email));
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
    /* What the Windows wizard was telling the user to do when it launched.
     *
     * The instructions live on the wizard page, which is exactly where you
     * cannot see them once the emulator is full-screen -- so the pause panel
     * carries a copy. Empty for anything that is not a Windows install. */
    std::string win_hint;
    /* Which Windows machine this launch belongs to, and at which phase, so the
     * wizard can move on when the engine exits. Empty for anything else. */
    std::string win_running_dir;
    retrodos::Win98Phase win_running_phase = retrodos::Win98Phase::Create;
    retrodos::OsKind     win_running_os    = retrodos::OsKind::Win98;
    /* Set when the phase is changed from outside the wizard page, so the page
     * re-reads instead of showing the copy it loaded on first open. */
    bool win_stale = false;
    /* The Windows/FreeDOS page shows the machine once it is installed and
     * the walkthrough otherwise; this is "show the walkthrough anyway",
     * set by the machine view's Walkthrough button and cleared on leaving
     * the page. */
    bool machine_wizard = false;
    /* Set to open the setup wizard on a particular step next frame; -1 is
     * "leave it where it was". The rail's Setup entry uses it to land on
     * the folder step directly. */
    int wizard_open_step = -1;
    /* True once the parent folder has been picked on purpose -- in this
     * session, or by a config saved by an earlier one. The startup default
     * does not count. */
    bool storage_chosen = cfg.wizard_done && !cfg.storage_root.empty();
    auto machine_ready = [&](retrodos::OsKind os) {
        const std::string dir = retrodos::machines_dir(cfg) + "/" + retrodos::os_folder(os);
        retrodos::Win98Install m;
        return retrodos::win98_is_install_dir(dir) && retrodos::win98_load(dir, m) &&
               m.phase == retrodos::Win98Phase::Run && retrodos::win98_blocker(m).empty();
    };
    /* True while the pointer is held for the guest. Read a frame later than it
     * is set, which is fine: it only has to be right, not instantaneous. */
    bool mouse_grabbed = false;
    unsigned long mouse_seen = 0, mouse_sent = 0;
    /* The user has asked for the pointer back and means to keep it until they
     * say otherwise. Distinct from the UI being in front, which releases it
     * only for as long as the panel is up. */
    bool mouse_free = false;
    /* The touchscreen, read as a trackpad rather than as a mouse that
     * teleports. Real mouse motion goes through it too, so both share one
     * accumulator -- see retrodos_touchmouse.h. */
    retrodos::TouchMouse touch;
    /* Where the guest's picture is on screen, from the last frame that drew
     * one. The pointer gain is derived from it: a finger should move the
     * pointer the distance it travelled ON THE GLASS, which means knowing how
     * much the picture was scaled to get there. */
    SDL_FRect fb_dst = { 0, 0, 0, 0 };
    /* Where the always-visible Esc chip is, from the frame that drew it. A
     * finger landing on it belongs to the chip, and must not also arrive in
     * the guest as a click -- which is what made a tap on Esc select an icon
     * on the Windows desktop behind it. Empty when the chip is not up. */
    SDL_FRect emubar = { 0, 0, 0, 0 };
    /* True while a booted guest owns the machine -- Windows, or anything else
     * past BOOT -- which is what decides how mouse movement has to be scaled.
     * Polled rather than assumed: a DOS game can boot a guest too. */
    bool guest_os = false;
    Uint64 guest_os_checked = 0;
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
        /* Only a Windows install carries a hint; anything else clears it, so a
         * stale crib cannot follow a game into its pause menu. */
        if (g.conf_override.empty()) { win_hint.clear(); win_running_dir.clear(); }

        if (engine_has_run) {
            cfg.pending_launch = g.name;
            retrodos::save_app_config(cfg_path, cfg);
            if (retrodos::android_restart_app())
                return;             /* the process is about to die */
            /* Cleared on DISK, not just in memory.
             *
             * The line above wrote pending_launch to the config before trying
             * to restart. Where no restart happens -- desktop, iOS, or Android
             * refusing -- clearing only the in-memory copy left the file
             * saying "launch this on startup" for ever, so every subsequent
             * start auto-ran the last thing played. */
            cfg.pending_launch.clear();
            retrodos::save_app_config(cfg_path, cfg);
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

        /*
         * A Windows machine is not a DOS game, whichever screen started it.
         *
         * Its folder holds a disk image and a phase, not an executable, so
         * build_conf mounts the folder as C: and drops the user at a DOS
         * prompt -- which is what the Library's own launch and Setup buttons
         * did. Checked here rather than in each caller because this is the one
         * place every launch passes through: the Library, the wizard, a
         * pending launch after a restart.
         */
        if (g.conf_override.empty() && retrodos::win98_is_install_dir(g.dir)) {
            retrodos::Win98Install lw;
            /* win98_load already reconciles the recorded phase against the
             * files -- and now against the contents of the disk image -- so
             * lw.phase is the truth about this machine, not what was written
             * down last. */
            const bool lw_ok = retrodos::win98_load(g.dir, lw);
            if (!lw_ok) lw.dir = g.dir;
            if (!retrodos::win98_blocker(lw).empty()) {
                /* Something it needs is missing -- usually the CD. Booting
                 * anyway produces a black machine that explains nothing, so
                 * hand the user back to the page that can say what is wrong. */
                win_stale = true;
                page = lw.os == retrodos::OsKind::FreeDos ? Page::FreeDos : Page::Windows;
                view = View::Shell;
                return;
            }
            /* Write it back, so the wizard and the Library row stop
             * describing a machine that has already moved on. Only when the
             * state file actually read: saving over one that would not load
             * would throw the CD path away with it. */
            if (lw_ok) {
                retrodos::win98_save(lw);
                win_stale = true;
            }

            g.conf_override   = retrodos::win98_conf(lw);
            win_running_dir   = lw.dir;
            win_running_phase = lw.phase;
            win_running_os    = lw.os;
            /* Install and Continue both begin with typing: DEL for the
             * BIOS, then Setup's questions. A finished base boots to a
             * desktop and wants the pointer instead. */
            g.keyboard_on_start = lw.phase == retrodos::Win98Phase::Install ||
                                  lw.phase == retrodos::Win98Phase::Continue;
        }

        /*
         * Nothing to run is not a reason to start a machine.
         *
         * build_conf mounts the folder and leaves the user at a C:\> prompt,
         * which is indistinguishable from a broken emulator -- and it is
         * exactly what an interrupted download produced: a folder in the
         * library with nothing in it yet. Say so instead.
         */
        if (!g.is_machine && g.conf_override.empty() && !g.dir.empty() &&
            g.run.empty() && g.autoexec.empty()) {
            library_note = g.name + ": nothing here to run. If it was still "
                           "downloading, it did not finish.";
            view = View::Shell;
            return;
        }

        Settings s = cfg.defaults;
        retrodos::load_game_settings(games_dir, g.name, s);  /* overrides win */
        active = s;
        /* Empty drives mount nothing, and a hard disk lettered C: gives way
         * to the title's folder, which IS C: for a game. */
        std::vector<retrodos::DriveImage> launch_drives;
        for (const auto &d : drives) {
            if (d.path.empty()) continue;
            if (d.letter == 'C' && !g.dir.empty()) continue;
            launch_drives.push_back(d);
        }
        /* A conf shipped with the game states how it starts; use it verbatim
         * rather than the guessed program name. */
        const bool use_profile = !g.autoexec.empty();
        std::string conf = !g.conf_override.empty()
            ? g.conf_override
            : retrodos::build_conf(
                s, g.name, g.dir,
                use_profile ? g.autoexec : g.run,
                use_profile ? true : g.run_raw,
                g.audio_profile,
                g.is_demo ? std::vector<retrodos::DriveImage>() : launch_drives);
        if (!g.conf_override.empty()) {
            /* A machine's conf is its own, but the floppy drives on the
             * Launch page still count: the driver disk in A: has to be there
             * when Windows boots and finds its new card, and a floppy cannot
             * be added to a machine after BOOT. Mounted just before it. */
            std::string mounts;
            bool have_a = false;
            for (const auto &d : launch_drives)
                if (d.kind == retrodos::DriveImage::Floppy) {
                    mounts += std::string("IMGMOUNT ") + d.letter + " \"" + d.path + "\" -t floppy\n";
                    if (d.letter == 'A') have_a = true;
                }
            /* Always an A:. A booted guest binds its drivers to the drives
             * it found at boot, so a floppy drive that was not there then
             * cannot be added from the pause menu -- only the disk in an
             * existing one can change. A blank image stands in for "drive
             * present, no disk", and the menu swaps a real one in later. */
            if (!have_a) {
                const std::string blank = cfg_dir + "/blank-floppy.img";
                if (!path_is_file(blank)) {
                    if (SDL_IOStream *io = SDL_IOFromFile(blank.c_str(), "wb")) {
                        std::vector<char> z(1474560, 0);
                        SDL_WriteIO(io, z.data(), z.size());
                        SDL_CloseIO(io);
                    }
                }
                if (path_is_file(blank))
                    mounts += "IMGMOUNT A \"" + blank + "\" -t floppy\n";
            }
            const size_t boot = conf.find("\nBOOT ");
            if (!mounts.empty() && boot != std::string::npos) conf.insert(boot + 1, mounts);
        }
        if (SDL_IOStream *io = SDL_IOFromFile(conf_path.c_str(), "w")) {
            SDL_WriteIO(io, conf.data(), conf.size());
            SDL_CloseIO(io);
        }
        last_serial  = 0;
        show_overlay = false;
        show_osk     = g.keyboard_on_start;
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

        /*
         * A Windows machine may not be in `games` at all -- the list is built
         * from the library root, and a machine created by the wizard in this
         * session is only there after a rescan. Hand launch() the folder and
         * let it recognise what is in it, which is the same path the Library
         * takes.
         */
        {
            const std::string wdir = retrodos::machines_dir(cfg) + "/" + want;
            if (retrodos::win98_is_install_dir(wdir)) {
                Game g;
                g.name = want;
                g.dir  = wdir;
                launch(g);
            }
        }

        if (view != View::Emulator) {
            for (const Game &g : games) {
                if (g.name == want) { launch(g); break; }
            }
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
                g.keyboard_on_start = retrodos::demo_wants_keyboard(k);
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
        /*
         * While the pointer is captured it belongs to the guest, full stop.
         *
         * WantCaptureMouse cannot be consulted then. Relative mode stops SDL
         * reporting an absolute position, so ImGui's cursor stays wherever it
         * was when the capture began -- and if that was over the emulator
         * view's little control strip, WantCaptureMouse is true for ever and
         * not one mouse event reaches the guest. The mouse appears completely
         * dead, which is exactly what it did.
         *
         * The strip is unclickable while captured, and that is the right
         * trade: Escape releases the pointer and everything is clickable
         * again.
         */
        const bool ui_wants_mouse = ui_modal ||
                                    (!mouse_grabbed && io.WantCaptureMouse);
        /* The on-screen keyboard alone does not take a real keyboard away
         * from the guest: it is opened FOR people without one, and someone
         * who plugs one in while it is up should not find every key dead
         * until they work out to hide it. Any other panel still does. */
        const bool ui_modal_sans_osk = (view != View::Emulator) || show_overlay ||
                                       show_controls;
        const bool ui_wants_keys  = ui_modal_sans_osk || io.WantCaptureKeyboard;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);

            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;

            /* Do not keep the emulator running out of sight. A minimised or
             * hidden window means the machine should pause -- it is burning a
             * core otherwise, and a game that ran on while it could not be seen
             * is a game that lost its place. Resumed when the window comes
             * back. Only the engine is paused; the frontend keeps ticking so
             * it can notice the window return. */
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
                if (engine_has_run) retrodos_host_set_pause(true);
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                if (engine_has_run) retrodos_host_set_pause(false);
                break;

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
                /*
                 * Ctrl+F10 -- release the pointer, and take it back.
                 *
                 * DOSBox's own shortcut for this, so anyone who has used it
                 * already knows it. Handled here and deliberately NOT passed
                 * on: a guest that also saw Ctrl+F10 would act on a key the
                 * user meant for the emulator.
                 *
                 * Escape already frees the pointer by opening the overlay, but
                 * only while that panel is up, and it is a key DOS and Windows
                 * both want. This is the one that stays.
                 */
#if !defined(__ANDROID__) && !defined(__APPLE__)
                if (down && ev.key.scancode == SDL_SCANCODE_F10 &&
                    (SDL_GetModState() & SDL_KMOD_CTRL)) {
                    mouse_free = !mouse_free;
                    break;
                }
#endif
                if (!ui_wants_keys) retrodos_host_send_key((int)ev.key.scancode, down);
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                /* Counted on both sides of the gate, because "the mouse does
                 * nothing" has two completely different causes -- the events
                 * are not arriving, or they are arriving and being withheld --
                 * and from outside the app they look identical. The overlay
                 * shows both numbers. */
                ++mouse_seen;
                /*
                 * A finger is not a mouse, and SDL's kindness in pretending
                 * otherwise is the whole problem.
                 *
                 * SDL synthesises mouse events from touches so that ordinary
                 * UI works on glass, which it must keep doing -- ImGui has
                 * already had this event. But the guest was being given them
                 * too, and the numbers are nonsense there: the "relative"
                 * motion of a tap is the distance from wherever the LAST touch
                 * landed, so every tap threw the pointer across the screen,
                 * and a drag arrived as a held button, which is why a Windows
                 * desktop answered a scrolled finger by drawing a selection
                 * box or carrying an icon off with it.
                 *
                 * Touches reach the guest through TouchMouse instead, from the
                 * finger events themselves.
                 */
                if (ev.motion.which != SDL_TOUCH_MOUSEID &&
                    !ui_wants_mouse && view == View::Emulator) {
                    ++mouse_sent;
                    touch.add_motion(ev.motion.xrel, ev.motion.yrel);
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which == SDL_TOUCH_MOUSEID) break;  /* see above */
                if (!ui_wants_mouse && view == View::Emulator) {
#if !defined(__ANDROID__) && !defined(__APPLE__)
                    /* Released, and the user has clicked back into the
                     * picture: take the pointer again rather than sending a
                     * click the guest cannot place. Same gesture DOSBox uses,
                     * and the click itself is swallowed. */
                    if (mouse_free && ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                        mouse_free = false;
                        break;
                    }
#endif
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
                 * mouse click on the UI, which ImGui has already had.
                 *
                 * The pad gets first refusal, and anything that misses one of
                 * its buttons -- which is everything when the pad is off, or
                 * hidden behind a gamepad -- is glass to point with. */
                if (view == View::Emulator && !show_overlay && !show_osk &&
                    !show_controls) {
                    /* Only where the finger STARTED, and only the press.
                     *
                     * A drag that happens to end over the chip still has to
                     * reach the handler, or the gesture is never finished and
                     * the button it is holding is held for good. Events for a
                     * finger the handler is not tracking are ignored there
                     * anyway, so passing them on costs nothing. */
                    const float fx = ev.tfinger.x * (float)win_w;
                    const float fy = ev.tfinger.y * (float)win_h;
                    const bool on_bar = ev.type == SDL_EVENT_FINGER_DOWN &&
                                        emubar.w > 0.0f &&
                                        fx >= emubar.x && fx <= emubar.x + emubar.w &&
                                        fy >= emubar.y && fy <= emubar.y + emubar.h;
                    const bool tpad_on = tpad_platform && active.onscreen_pad &&
                                         gamepads.empty();
                    const bool tpad_took = (tpad_on || tpad.editing()) &&
                        tpad.handle(ev, ImVec2((float)win_w, (float)win_h),
                                    ImVec2(0, 0), ImVec2((float)win_w, (float)win_h),
                                    tpad_sink);
                    if (!on_bar && !tpad_took)
                        touch.handle_event(ev, win_w, win_h);
                }
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
            /* Desktop never has the on-screen pad, so its state should not
             * claim otherwise -- the overlay reads enabled() to decide whether
             * to offer "Move controls", and offering a layout editor for
             * something that is not on screen is worse than not offering it. */
#if defined(__ANDROID__) || defined(__APPLE__)
            pad.set_enabled(active.onscreen_pad);
#else
            pad.set_enabled(false);
#endif

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

            const unsigned mask = touch_buttons | gp_buttons;
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
                fb_dst = dst;
            }

            /* ---- pointer speed ----
             *
             * The pointer should travel as far as the finger did, measured on
             * the glass. That is one number times another: how many guest
             * pixels a screen pixel is worth (the picture is scaled to fit,
             * and by how much changes with the video mode), and how many units
             * of movement a guest pixel costs.
             *
             * The second one depends on who is reading the mouse. A booted
             * guest gets PS/2 packets, and DOSBox-X's 8042 halves what it is
             * given -- (1<<resolution)/16, with the resolution of 3 Windows
             * programs -- so two units buy one count. DOS reads INT 33h, where
             * the engine does its own mickey arithmetic and a unit is a pixel.
             * Telling them apart is what "Guest OS" is: the name DOSBox-X
             * gives the running program once BOOT has taken the machine.
             *
             * The quantum goes with it, because that halving is integer
             * division and it discards the remainder. See set_quantum().
             */
            if (ui_modal) {
                /* A panel is in front: the glass belongs to it, and a finger
                 * that was down when it opened must not still be pressing a
                 * button behind it. */
                touch.reset();
            } else {
                const Uint64 now_ms = SDL_GetTicks();
                if (now_ms - guest_os_checked > 500) {
                    guest_os_checked = now_ms;
                    char prog[64] = {0};
                    retrodos_host_running_program(prog, sizeof(prog));
                    guest_os = (SDL_strcmp(prog, "Guest OS") == 0);
                }

                float guest_px_per_screen_px = 1.0f;
                if (fb_dst.w > 1.0f && fb_tex_w > 0)
                    guest_px_per_screen_px = (float)fb_tex_w / fb_dst.w;

                /*
                 * And then Windows moves it further than you asked.
                 *
                 * A guest OS applies its own pointer speed and acceleration to
                 * what the mouse reports, so movement that is 1:1 at the wire
                 * arrives on screen at something like one and a half to twice
                 * that -- fast enough that the pointer overshoots everything
                 * and a drag feels like a flick. Neither number can be read
                 * from out here, so this is a measured compromise rather than
                 * a derivation. Measured on a Windows 98 desktop: at 0.5 the
                 * pointer tracks a finger exactly 1:1, which says the guest is
                 * doubling what it is sent. 0.3 is deliberately below that --
                 * a pointer that lands where you meant is worth more than one
                 * that keeps up with your thumb, and the slider is right there
                 * for anyone who disagrees. DOS has no such layer and gets
                 * none of this.
                 */
                const float kGuestPointerAccel = 0.3f;

                touch.set_quantum(guest_os ? 2 : 1);
                touch.set_gain(guest_px_per_screen_px *
                               (guest_os ? 2.0f * kGuestPointerAccel : 1.0f) *
                               (float)active.mouse_speed / 100.0f);
                touch.update();
            }

            /*
             * Hold the pointer while the guest owns the screen.
             *
             * A booted guest -- Windows, or anything else past BOOT -- reads
             * the mouse as PS/2 relative packets. Without capture the host
             * pointer walks to the edge of the window and simply stops
             * producing motion, so the guest's pointer drifts a little and
             * then sticks: the mouse looks broken rather than uncaptured. The
             * DOS mouse driver hides this because DOSBox-X can place its
             * cursor absolutely; a guest OS has no such route.
             *
             * Released whenever any of our own UI is in front, so the pointer
             * is always available for the overlay, the keyboard and the pad
             * editor -- and Escape, which opens the overlay, is therefore also
             * how you get the pointer back.
             *
             * Desktop only. A touch screen has no pointer to capture, and
             * asking for relative mode there turns taps into deltas from
             * wherever the last one happened to land.
             */
#if !defined(__ANDROID__) && !defined(__APPLE__)
            {
                const bool want_grab = (view == View::Emulator) && !mouse_free &&
                                       !show_overlay && !show_osk &&
                                       !show_controls && !pad.editing();
                if (want_grab != mouse_grabbed) {
                    if (SDL_SetWindowRelativeMouseMode(win, want_grab))
                        mouse_grabbed = want_grab;
                    else
                        LOGI("relative mouse mode refused: %s", SDL_GetError());
                }
            }
#endif

            if (g_engine_done.load()) {
                if (engine.joinable()) engine.join();
                g_engine_done.store(false);
                view = View::Shell;
                show_overlay = show_osk = false;

                /*
                 * Take the pointer back, by force.
                 *
                 * The engine shares this process's SDL, and it hides the
                 * cursor for itself -- SDL_ShowCursor(SDL_DISABLE) in half a
                 * dozen places in sdlmain.cpp -- without restoring it on the
                 * way out. So closing a guest left the launcher with no mouse
                 * pointer at all and nothing on screen to explain it. Letting
                 * the grab flag fall out on its own is not enough: the flag
                 * governs relative mode, and this is the cursor itself.
                 *
                 * Asserted rather than tracked. Whatever the engine did to the
                 * window, the frontend owns it again now.
                 */
                SDL_SetWindowRelativeMouseMode(win, false);
                SDL_ShowCursor();
                mouse_grabbed = false;
                mouse_free    = false;
                /* A button still held when the picture goes is held for ever,
                 * and there is no longer anything to release it into. */
                touch.reset();
                guest_os = false;

                /*
                 * Setup rebooting IS the step boundary.
                 *
                 * The guide says so plainly: when the installer reboots you are
                 * back at the prompt, and from then on you boot the hard disk
                 * rather than the CD. Leaving the phase alone meant the next
                 * launch ran the install step again -- IMGMOUNT A -bootcd,
                 * BOOT A: -- which starts Setup over from the beginning on a
                 * disk that has just had Windows written to it.
                 *
                 * So the phase advances the moment the engine exits, without
                 * asking. It is the one transition here that is genuinely
                 * observable, and it is the one the user is least able to
                 * guess at.
                 */
                if (!win_running_dir.empty() &&
                    win_running_phase == retrodos::Win98Phase::Install) {
                    retrodos::Win98Install fin;
                    if (retrodos::win98_load(win_running_dir, fin)) {
                        /* Not for FreeDOS on evidence alone: its installer
                         * restarts the machine once to partition the disk,
                         * before it has written a single file. Advancing
                         * then would boot an empty C:. win98_load has
                         * already moved it on if the shell is on the disk. */
                        if (fin.os == retrodos::OsKind::Win98)
                            fin.phase = retrodos::Win98Phase::Continue;
                        retrodos::win98_save(fin);
                        win_stale = true;
                    }
                }
                win_running_dir.clear();

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

            /* Did the user arrive somewhere new this frame? Captured here, at
             * the top, because a button pressed further down changes `page`
             * for the NEXT frame -- comparing after the fact would see the new
             * page already recorded and report no change at all. */
            const bool page_changed = (page_last_frame != page);
            page_last_frame = page;
            if (page_changed) machine_wizard = false;

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
                /*
                 * The first-run walkthrough, one step at a time.
                 *
                 * It used to be a single screen: a list of folders and a
                 * Continue button. That is enough for somebody who already
                 * knows what a DOS game is and where this app can reach, and
                 * no help at all to anyone else -- and on iOS it asked a
                 * question with exactly one possible answer, which reads as a
                 * broken control rather than a sandbox.
                 *
                 * The steps are the same everywhere; what changes inside them
                 * is what each platform can actually do. That difference is
                 * stated rather than hidden, because "you cannot put games
                 * anywhere else" is a fact about the device, and a user who is
                 * not told it will go looking for the setting that is missing.
                 */
                static int wstep = 0;
                if (wizard_open_step >= 0) { wstep = wizard_open_step; wizard_open_step = -1; }

                ImGui::SetNextWindowPos(origin);
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##wizard", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

                static const char *kWizName[] = {
                    "Welcome", "Where games live", "Add a game", "Try it", "Ready"
                };
                const int kWizSteps = (int)SDL_arraysize(kWizName);
                if (wstep < 0) wstep = 0;
                if (wstep >= kWizSteps) wstep = kWizSteps - 1;

                ImGui::Text("%s  -  step %d of %d: %s", RETRODOS_APP_NAME,
                            wstep + 1, kWizSteps, kWizName[wstep]);
                ImGui::Separator();

                ImGui::BeginChild("##wizbody",
                                  ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
                scroll_by_drag();
                ImGui::PushTextWrapPos(0.0f);

                bool wiz_next = true;      /* may this step be left forwards? */

                if (wstep == 0) {
                    ImGui::TextWrapped("This runs DOS software -- the games and programs "
                                       "that came on floppies and CD-ROMs for the PC.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("A game here is a FOLDER. Whatever was on the disc "
                                       "or in the download -- the .EXE, its data files, "
                                       "everything -- goes in one folder, and that folder "
                                       "is the game. There is nothing to install.");
                    ImGui::Spacing();
                    TextDimWrapped("The emulator is " RETRODOS_APP_CORE ", which is free "
                                   "software under the GPL. This app does not supply DOS "
                                   "games, and cannot fetch them for you.");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    TextDimWrapped("Three short steps: where games live, how to put one "
                                   "there, and something to run right now.");
                }

                else if (wstep == 1) {
#if defined(__APPLE__)
                    /*
                     * No choice to offer, so none is offered.
                     *
                     * iOS gives an app one directory it can both read and show
                     * to the user -- its own Documents -- and a document-tree
                     * grant elsewhere is not something the emulator can mount.
                     * A folder list here would be one radio button, and a
                     * "choose another" button would do nothing. Saying why is
                     * more use than either.
                     */
                    ImGui::TextWrapped("Games live in this app's own folder:");
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", root_label(cfg.library_root).c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("There is nowhere else to choose. iPhone and iPad "
                                       "only let an app read its own folder, so this one "
                                       "is it -- and it is the one the Files app shows "
                                       "you.");
                    ImGui::Spacing();
                    TextDimWrapped("That restriction is the platform's, not ours: an app "
                                   "cannot reach your Downloads, another app's files, or "
                                   "an external drive without you copying things in.");
#else
                    ImGui::TextWrapped("Choose ONE folder for everything. Inside it "
                                       RETRODOS_APP_NAME " keeps these folders and "
                                       "nothing else:");
                    ImGui::Indent();
                    ImGui::TextWrapped("games/           one folder or .zip per DOS game");
                    ImGui::TextWrapped("apps/            DOS applications, the same way");
                    ImGui::TextWrapped("discs/dos/       CDs and floppies for DOS");
                    ImGui::TextWrapped("discs/windows/   the Windows CD and Windows game CDs");
                    ImGui::TextWrapped("discs/freedos/   the FreeDOS CD");
                    ImGui::TextWrapped("machines/        Windows 98 and FreeDOS, each with its hard disk");
                    ImGui::Unindent();
                    ImGui::Spacing();
#if defined(__ANDROID__)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.4f, 1.0f));
                    ImGui::TextWrapped("Android needs your permission to use that folder.");
                    ImGui::PopStyleColor();
                    TextDimWrapped("Press \"Choose a folder on this device\" below and the "
                                   "system asks you to pick a folder and allow this app to "
                                   "read and write it. Nothing outside that folder is "
                                   "touched, and the permission lasts until you remove the "
                                   "app. Pick a folder you made yourself (under Documents "
                                   "or Download) rather than the root of a drive, which "
                                   "Android often refuses to hand over. If you would "
                                   "rather grant nothing, the app's own storage below "
                                   "needs no permission -- but a PC only sees it under "
                                   "Android/data and it is deleted with the app.");
                    ImGui::Spacing();
#endif

                    /* A folder the user picked, as the first choice. The path
                     * comes back "" when Android will not let the app write
                     * there directly, and that is said out loud rather than
                     * discovered when a disk image fails to be made. */
                    static std::string saf_root, saf_label, saf_seen;
                    static int saf_poll = 0;
                    if (++saf_poll % 30 == 0 || saf_root.empty()) {
                        saf_root  = retrodos::saf_root_path();
                        saf_label = retrodos::saf_root_label();
                        /* A folder just picked is the folder wanted: choosing
                         * it in the system picker and then having to press a
                         * radio button beside it was a second step nobody
                         * expected, and skipping it left the app on its own
                         * storage with "dos" on the end. */
                        if (!saf_root.empty() && saf_root != saf_seen) {
                            saf_seen = saf_root;
                            choose_storage_root(cfg, saf_root);
                            storage_chosen = true;
                        }
                    }
#if !defined(__ANDROID__)
                    /* A desktop has no document-tree grant; the system's own
                     * folder dialog stands in for it. The callback arrives on
                     * another thread, so it only records the choice and the
                     * frame loop applies it. */
                    static std::string  dlg_picked;
                    static std::atomic<bool> dlg_ready{false};
                    if (dlg_ready.load()) {
                        dlg_ready.store(false);
                        if (!dlg_picked.empty()) {
                            choose_storage_root(cfg, dlg_picked);
                            storage_chosen = true;
                        }
                    }
                    auto pick_folder = [&]() {
                        SDL_ShowOpenFolderDialog(
                            [](void *, const char *const *files, int) {
                                if (files && files[0]) dlg_picked = files[0];
                                else dlg_picked.clear();
                                dlg_ready.store(true);
                            },
                            nullptr, win, cfg.storage_root.empty() ? nullptr
                                                                   : cfg.storage_root.c_str(),
                            false);
                    };
                    /* The grant-based rows below never apply here. */
                    saf_root.clear(); saf_label.clear();
#else
                    auto pick_folder = [&]() { retrodos::saf_pick_root(); };
#endif
                    const bool on_saf = !saf_root.empty() && cfg.storage_root == saf_root;
                    if (ImGui::RadioButton("##saf", on_saf) && !saf_root.empty()) {
                        choose_storage_root(cfg, saf_root);
                        storage_chosen = true;
                    }
                    ImGui::SameLine();
                    if (!saf_root.empty()) {
                        ImGui::TextWrapped("%s", saf_label.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Change")) pick_folder();
                    } else if (!saf_label.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                        ImGui::TextWrapped("%s -- Android will not let the app write there "
                                           "directly. Pick a folder you made yourself under "
                                           "Documents or Download, or use app storage below.",
                                           saf_label.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Change")) pick_folder();
                    } else {
                        if (ImGui::Button("Choose a folder on this device...")) pick_folder();
                    }
                    TextDimWrapped("Your own folder: easy to reach from a PC over USB or "
                                   "from a file manager, and it stays if the app is "
                                   "removed.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Or use the app's own storage:");
                    for (size_t i = 0; i < roots.size(); ++i) {
                        ImGui::PushID((int)i);
                        if (ImGui::RadioButton("##root", cfg.storage_root == roots[i])) {
                            cfg.storage_root = roots[i];
                            retrodos::apply_storage_root(cfg);
                            storage_chosen = true;
                        }
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s%s", root_label(roots[i]).c_str(),
                                           path_is_dir(roots[i]) ? ""
                                                                 : "   (will be created)");
                        ImGui::PopID();
                    }
                    TextDimWrapped("Needs no permission and works on removable storage, "
                                   "but a PC only sees it under Android/data, and it goes "
                                   "when the app does.");
                    if (ImGui::SmallButton("Rescan volumes")) {
                        roots = candidate_roots();
                        for (const auto &r : roots) SDL_CreateDirectory(r.c_str());
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (storage_chosen) {
                        ImGui::TextWrapped("Chosen: %s", cfg.storage_root.c_str());
                        ImGui::TextDisabled("games in %s/games, discs under %s/discs",
                                            cfg.storage_root.c_str(), cfg.storage_root.c_str());
                    } else {
                        ImGui::TextWrapped("Chosen: nothing yet");
                    }
                    ImGui::Spacing();
                    TextDimWrapped("Have a games collection somewhere else too? It can be "
                                   "browsed where it is; each game is copied in the first "
                                   "time it is played.");
                    if (ImGui::SmallButton(retrodos::saf_has_grant() ? "Change collection folder"
                                                                     : "Browse a collection..."))
                        retrodos::saf_pick_folder();
#endif
                    /* Not skippable. The default the app starts with is only
                     * so it can run; where everything lives is the one choice
                     * setup exists to make. */
                    if (!storage_chosen) {
                        wiz_next = false;
                        ImGui::Spacing();
                        ImGui::TextDisabled("Choose a folder to continue.");
                    }
                }

                else if (wstep == 2) {
                    SDL_CreateDirectory(cfg.library_root.c_str());
#if defined(__APPLE__)
                    ImGui::TextWrapped("To add a game:");
                    ImGui::Bullet();
                    ImGui::TextWrapped("Open the Files app.");
                    ImGui::Bullet();
                    ImGui::TextWrapped("Go to On My iPhone (or On My iPad) and find "
                                       RETRODOS_APP_NAME ".");
                    ImGui::Bullet();
                    ImGui::TextWrapped("Put the game's folder inside the 'dos' folder "
                                       "there.");
                    ImGui::Spacing();
                    TextDimWrapped("A zip works too: unzip it in Files first, then move "
                                   "the folder across. AirDrop, iCloud Drive and a USB "
                                   "drive all land in Files, so all three work.");
#elif defined(__ANDROID__)
                    ImGui::TextWrapped("To add a game, copy its folder into:");
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", root_label(cfg.library_root).c_str());
                    ImGui::Spacing();
                    TextDimWrapped("Any file manager can reach it, and so can a USB cable. "
                                   "Or use Add game on the library screen, which copies a "
                                   "folder in for you.");
#else
                    ImGui::TextWrapped("To add a game, copy its folder into:");
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", cfg.library_root.c_str());
#endif
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    /* The step confirms itself. Instructions followed in
                     * another app are instructions you cannot tell you have
                     * got right, and "did that work?" is the question this
                     * screen exists to answer. */
                    if (ImGui::Button("Look again")) refresh();
                    ImGui::SameLine();
                    {
                        int found = 0;
                        for (const Game &g : games) if (!g.is_machine) ++found;
                        if (found > 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
                            ImGui::Text("%d found", found);
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("nothing there yet");
                        }
                    }
                    ImGui::Spacing();
                    TextDimWrapped("You can skip this and come back to it -- the next step "
                                   "has something to run either way.");
                }

                else if (wstep == 3) {
                    ImGui::TextWrapped("Something to run right now, with no files of your "
                                       "own:");
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
                            g.keyboard_on_start = retrodos::demo_wants_keyboard(k);
                            ImGui::PushID(g.name.c_str());
                            if (ImGui::Button(g.name.c_str(),
                                              ImVec2(ImGui::GetFontSize() * 18.0f, 0))) {
                                /* Finish first. Starting the emulator from
                                 * inside the wizard and leaving wizard_done
                                 * false would drop the user back here when the
                                 * game exits, with no sign of what happened. */
                                cfg.wizard_done = true;
                                retrodos::save_app_config(cfg_path, cfg);
                                view = View::Shell;
                                launch(g);
                            }
                            ImGui::PopID();
                        }
                        ImGui::Spacing();
                        TextDimWrapped("FreeDOS 1.3 is included verbatim under the GPL; "
                                       "the demonstration program was written for this "
                                       "project.");
                    }
                }

                else {
                    ImGui::TextWrapped("That is everything.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Your games folder:");
                    ImGui::TextDisabled("%s", root_label(cfg.library_root).c_str());
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextWrapped("Worth knowing:");
                    ImGui::Bullet();
                    ImGui::TextWrapped("A game that will not start is usually a Machine "
                                       "setting -- the graphics card, or the speed.");
                    ImGui::Bullet();
                    ImGui::TextWrapped("Windows Setup installs Windows 98 from a CD image "
                                       "you own, step by step.");
                    ImGui::Bullet();
                    ImGui::TextWrapped("This screen is under Machine, as Change games "
                                       "folder, whenever you want it again.");
                }

                ImGui::PopTextWrapPos();
                ImGui::EndChild();

                /* ---- footer ---- */
                ImGui::Separator();
                ImGui::BeginDisabled(wstep == 0);
                if (ImGui::Button("Back")) --wstep;
                ImGui::EndDisabled();
                ImGui::SameLine();

                if (wstep < kWizSteps - 1) {
                    ImGui::BeginDisabled(!wiz_next);
                    if (ImGui::Button("Next")) {
                        if (wstep == 1) { make_storage_dirs(); refresh(); }
                        ++wstep;
                    }
                    ImGui::EndDisabled();
                } else {
                    if (ImGui::Button("Finish")) {
                        make_storage_dirs();
                        cfg.wizard_done = true;
                        retrodos::save_app_config(cfg_path, cfg);
                        refresh();
                        wstep = 0;
                        view = View::Shell;
                    }
                }

                ImGui::SameLine();
                ImGui::TextDisabled("   step %d of %d", wstep + 1, kWizSteps);
                /* No Skip. Setup runs start to finish, on a new install and
                 * on every rerun: choosing where everything lives -- and, on
                 * Android, granting the app access to it -- is the one thing
                 * that has to happen before anything else works. */
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
                        ImGui::TextUnformatted(RETRODOS_APP_NAME);
                    }
                    /* The core, under the name, on the screen you see first.
                     *
                     * Whatever this app is called, the emulator inside it is
                     * somebody else's work under the GPL. Burying that in an
                     * About page reachable in three taps is the letter of the
                     * licence at best; saying it here is the point of it. */
                    {
                        const char *credit = "powered by " RETRODOS_APP_CORE;
                        const float avail = ImGui::GetContentRegionAvail().x;
                        const float tw = ImGui::CalcTextSize(credit).x;
                        if (tw < avail)
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                                 (avail - tw) * 0.5f);
                        ImGui::TextDisabled("%s", credit);
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

                /* Launch first: it is what the app was opened to do. Then
                 * one screen per kind of thing you own. */
                nav("Launch",   Page::Launch);
                nav("Media",    Page::Media);
                /* One "Wizards" entry; Windows and FreeDOS are tabs across
                 * the top of the page it opens. */
                nav("Wizards",  page == Page::FreeDos ? Page::FreeDos : Page::Windows);
                nav("Machine",  Page::Settings);

                ImGui::Spacing();
                ImGui::Separator();

                if (retrodos::media_available()) {
                    /* One "RetroMedia" entry; Login (the account), Artwork and
                     * -- for an admin -- Downloads are tabs across the top of
                     * it. There is no separate Account entry. */
                    const bool on_media = page == Page::Account ||
                                          page == Page::Artwork || page == Page::Downloads;
                    if (on_media) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button("RetroMedia", ImVec2(bw, bh)) && !on_media) {
                        page = Page::Account; selected = -1;
                    }
                    if (on_media) ImGui::PopStyleColor();
                }

                ImGui::Spacing();
                ImGui::Separator();

                nav("Demo",  Page::Demo);
                {
                    /* Straight to the folder step: "where is everything" is
                     * the one thing people come back to setup for. */
                    if (ImGui::Button("Setup", ImVec2(bw, bh))) {
                        selected = -1;
                        roots = candidate_roots();
                        wizard_open_step = 0;
                        view = View::Wizard;
                    }
                }
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
                if (page == Page::Launch) {
                    /*
                     * Launch is a set of questions, not a picture: what are
                     * you doing, then which disc goes in which drive, then
                     * Launch. What it will actually do is still derived from
                     * the drives, so a title loaded from Library or a disk
                     * set up by hand launches without the questions.
                     */
                    const float fs = ImGui::GetFontSize();
                    if (page_changed && !loaded) { wiz_kind = 0; wiz_step = 0; }

                    const retrodos::DriveImage *c_hdd = drive_at(drives, 'C');
                    const retrodos::DriveImage *a_fd  = drive_at(drives, 'A');

                    /* ---- what Launch will do, from the drives ---- */
                    enum class Plan { Nothing, Title, Machine, BootFloppy, BootDisk, RunDisc, Prompt };
                    Plan plan = Plan::Nothing;
                    bool any_media = false;
                    for (const auto &d : drives) if (!d.path.empty()) any_media = true;
                    std::string c_folder;
                    if (c_hdd && !c_hdd->path.empty())
                        c_folder = c_hdd->path.substr(0, c_hdd->path.find_last_of('/'));
                    if (loaded && (!loaded_game.run.empty() || !loaded_game.autoexec.empty()))
                        plan = Plan::Title;
                    else if (!loaded && !c_folder.empty() && retrodos::win98_is_install_dir(c_folder))
                        plan = Plan::Machine;
                    else if (!loaded && a_fd && !a_fd->path.empty() &&
                             retrodos::image_has_os(a_fd->path))
                        plan = Plan::BootFloppy;
                    else if (!loaded && c_hdd && !c_hdd->path.empty() &&
                             retrodos::image_has_os(c_hdd->path))
                        plan = Plan::BootDisk;
                    static std::string disc_seen, disc_run;
                    static char disc_letter = 0;
                    if (plan == Plan::Nothing && !loaded) {
                        const retrodos::DriveImage *cd = nullptr;
                        for (const auto &d : drives)
                            if (d.kind == retrodos::DriveImage::Cd && !d.path.empty()) { cd = &d; break; }
                        if (cd) {
                            if (cd->path != disc_seen) {
                                disc_seen = cd->path;
                                std::string stem = base_name(cd->path);
                                const size_t dot = stem.find_last_of('.');
                                if (dot != std::string::npos) stem = stem.substr(0, dot);
                                disc_run  = pick_runnable(iso_root_names(cd->path), stem);
                            }
                            disc_letter = cd->letter;
                            if (!disc_run.empty()) plan = Plan::RunDisc;
                        } else { disc_seen.clear(); disc_run.clear(); }
                    }
                    if (plan == Plan::Nothing && !loaded && any_media) plan = Plan::Prompt;

                    auto do_launch = [&]() {
                        if (plan == Plan::Title) launch(loaded_game);
                        else if (plan == Plan::Machine) {
                            Game g; g.name = base_name(c_folder); g.dir = c_folder; launch(g);
                        } else {
                            Game g; g.dir = ""; g.run_raw = true;
                            if (plan == Plan::BootFloppy)    { g.name = base_name(a_fd->path);  g.run = "BOOT A:"; }
                            else if (plan == Plan::BootDisk) { g.name = base_name(c_hdd->path); g.run = "BOOT C:"; }
                            else if (plan == Plan::RunDisc)  { g.name = base_name(disc_seen);
                                                               g.run  = std::string(1, disc_letter) + ":\n" + disc_run; }
                            else { g.name = "DOS prompt"; g.run = (c_hdd && !c_hdd->path.empty()) ? "C:" : ""; }
                            launch(g);
                        }
                    };

                    ImGui::TextUnformatted("Launch");
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (loaded) {
                        /* A title from Library is already set up: name it, say
                         * what is in its drives, and launch. No questions. */
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.71f, 0.97f, 1.0f));
                        ImGui::TextWrapped("%s", loaded_game.name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::TextDisabled("%s", loaded_game.run.empty() ? "(no runnable found)"
                                                                          : loaded_game.run.c_str());
                        if (!loaded_game.autoexec.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
                            ImGui::TextWrapped("Uses this title's own dosbox.conf.");
                            ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                        ImGui::Text("C:  %s  (%s folder)", base_name(loaded_game.dir).c_str(),
                                    loaded_game.is_app ? "application" : "game");
                        for (const auto &d : drives)
                            if (!d.path.empty() && d.letter != 'C')
                                ImGui::Text("%c:  %s", d.letter, base_name(d.path).c_str());
                        ImGui::Spacing();
                        ImGui::BeginDisabled(plan == Plan::Nothing);
                        if (ImGui::Button("Launch", ImVec2(fs * 10.0f, fs * 2.0f))) do_launch();
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Button("Settings")) {
                            for (int i = 0; i < (int)games.size(); ++i)
                                if (games[i].name == loaded_game.name &&
                                    games[i].is_app == loaded_game.is_app) { selected = i; page = Page::Settings; break; }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Set up drives instead")) { loaded = false; wiz_kind = 0; wiz_step = 0; }
                    } else {
                        /* The questions. One picks what you are doing, then one
                         * prompt per drive, then Launch. */
                        const std::string ddir = cfg.storage_root.empty()
                            ? cfg.library_root : retrodos::discs_dir(cfg, "dos");
                        auto pick = [&](MediaKind mk, retrodos::DriveImage::Kind dk, char letter) -> bool {
                            const std::vector<std::string> imgs = scan_media(cfg, mk);
                            const retrodos::DriveImage *in = drive_at(drives, letter);
                            ImGui::BeginChild((std::string("##wl") + letter).c_str(),
                                              ImVec2(0, fs * 12.0f), ImGuiChildFlags_Borders);
                            for (const std::string &p2 : imgs) {
                                const bool on = in && in->path == p2;
                                if (ImGui::Selectable(base_name(p2).c_str(), on)) {
                                    if (!drive_at(drives, letter)) drive_add(drives, dk, loaded);
                                    drive_insert(drives, letter, p2);
                                    in = drive_at(drives, letter);
                                }
                            }
                            if (imgs.empty()) ImGui::TextDisabled("None found in\n%s", ddir.c_str());
                            ImGui::EndChild();
                            return in && !in->path.empty();
                        };

                        if (wiz_kind == 0) {
                            ImGui::TextWrapped("What do you want to do?");
                            ImGui::Spacing();
                            const float bw2 = fs * 24.0f, bh2 = fs * 2.2f;
                            /* Play a game: pick the disc image and it is
                             * mounted and run straight off it. */
                            if (ImGui::Button("Play a game (disc image)", ImVec2(bw2, bh2)))
                                { wiz_kind = 1; wiz_step = 1; }
                            /* Boot a machine: start it if it is installed,
                             * otherwise open its wizard to make it. */
                            auto boot_machine = [&](retrodos::OsKind os, Page pg) {
                                const std::string dir = retrodos::machines_dir(cfg) + "/" +
                                                        retrodos::os_folder(os);
                                retrodos::Win98Install m;
                                wiz_kind = 0; wiz_step = 0;
                                if (retrodos::win98_is_install_dir(dir) &&
                                    retrodos::win98_load(dir, m) &&
                                    m.phase == retrodos::Win98Phase::Run) {
                                    Game g; g.name = retrodos::os_folder(os); g.dir = dir;
                                    launch(g);
                                } else {
                                    page = pg; win_stale = true;
                                }
                            };
                            if (ImGui::Button("Boot Windows 98", ImVec2(bw2, bh2)))
                                boot_machine(retrodos::OsKind::Win98, Page::Windows);
                            if (ImGui::Button("Boot FreeDOS", ImVec2(bw2, bh2)))
                                boot_machine(retrodos::OsKind::FreeDos, Page::FreeDos);
                            if (ImGui::Button("Just a DOS prompt", ImVec2(bw2, bh2)))
                                { wiz_kind = 4; wiz_step = 1; }
                            ImGui::Spacing();
                            if (ImGui::Button("Quit " RETRODOS_APP_NAME, ImVec2(bw2, bh2)))
                                running = false;
                        } else {
                            struct WStep { char letter; MediaKind mk; retrodos::DriveImage::Kind dk;
                                           bool required; const char *prompt; };
                            std::vector<WStep> steps;
                            if (wiz_kind == 1)
                                steps = { { 'D', MediaKind::Cd, retrodos::DriveImage::Cd, true,
                                            "Pick the game's disc image (.iso/.cue). It is "
                                            "mounted as D: and run straight from the disc." } };
                            else
                                steps = {
                                    { 'C', MediaKind::Hdd, retrodos::DriveImage::Hdd, false, "Optional: a hard disk into C:." },
                                    { 'D', MediaKind::Cd, retrodos::DriveImage::Cd, false, "Optional: a CD into D:." },
                                };
                            const int total = (int)steps.size();
                            if (wiz_step < 1) wiz_step = 1;
                            if (wiz_step > total + 1) wiz_step = total + 1;

                            if (wiz_step <= total) {
                                const WStep &w = steps[wiz_step - 1];
                                ImGui::TextDisabled("Step %d of %d", wiz_step, total + 1);
                                ImGui::TextWrapped("%s", w.prompt);
                                ImGui::Spacing();
                                const bool got = pick(w.mk, w.dk, w.letter);
                                ImGui::Spacing();
                                if (ImGui::Button("Back")) { if (wiz_step > 1) --wiz_step; else { wiz_kind = 0; wiz_step = 0; } }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(w.required && !got);
                                if (ImGui::Button(w.required ? "Next" : (got ? "Next" : "Skip"))) ++wiz_step;
                                ImGui::EndDisabled();
                                if (w.required && !got) { ImGui::Spacing(); ImGui::TextDisabled("Pick a disc above to continue."); }
                            } else {
                                const char *summary =
                                    wiz_kind == 1 ? "Ready. Launch runs the game from the disc."
                                                  : "Ready. Launch opens the built-in DOS with whatever is mounted.";
                                ImGui::TextWrapped("%s", summary);
                                ImGui::Spacing();
                                if (ImGui::Button("Back")) --wiz_step;
                                ImGui::SameLine();
                                ImGui::BeginDisabled(plan == Plan::Nothing);
                                if (ImGui::Button("Launch", ImVec2(fs * 10.0f, fs * 2.0f))) do_launch();
                                ImGui::EndDisabled();
                                ImGui::SameLine();
                                if (ImGui::Button("Start over")) { wiz_kind = 0; wiz_step = 0;
                                    for (auto &d : drives) d.path.clear(); }
                            }
                        }
                    }
                }
                else if (page == Page::Media) {
                    const char *tabs[] = { "CDs", "Floppies", "Hard disks" };
                    /* Asked for one kind from Launch: show only that kind,
                     * with the drive it is for and a way out. Otherwise the
                     * three tabs. */
                    const retrodos::DriveImage *target = insert_target
                        ? drive_at(drives, insert_target) : nullptr;
                    if (target) {
                        media_tab = target->kind == retrodos::DriveImage::Cd ? 0
                                  : target->kind == retrodos::DriveImage::Floppy ? 1 : 2;
                        ImGui::Text("Insert into %c:  (%s)", target->letter,
                                    drive_kind_name(target->kind));
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Cancel")) { insert_target = 0; page = Page::Launch; }
                    } else {
                        /* CDs, Floppies, Hard disks -- and Configs, the games,
                         * which is its own page but reached from the same
                         * strip so the side rail is not one entry longer. */
                        const char *ntabs[] = { "CDs", "Floppies", "Hard disks", "Configs" };
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
                        for (int t = 0; t < 4; ++t) {
                            if (t) ImGui::SameLine();
                            const bool on = (t < 3) && media_tab == t;
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(ntabs[t], ImVec2(tw, 0))) {
                                if (t == 3) { page = Page::Library; selected = -1; }
                                else media_tab = t;
                            }
                            if (on) ImGui::PopStyleColor();
                        }
                    }
                    ImGui::Spacing();
                    /* Which drive letters each kind may go in. C: is the
                     * title's folder while one is loaded, so a hard disk can
                     * only take it when nothing is. */
                    bool done = false;
                    const std::string ddir = cfg.storage_root.empty() ? cfg.library_root
                                                                      : cfg.storage_root;
                    if (media_tab == 0)
                        done = media_shelf("cds", "CD", scan_media(cfg, MediaKind::Cd),
                                           drives, retrodos::DriveImage::Cd, insert_target, cw, ddir);
                    else if (media_tab == 1) {
                        std::vector<std::string> fds = scan_media(cfg, MediaKind::Floppy);
                        /* The bundled FreeDOS boot floppy is always on the
                         * shelf: with it in A: any machine boots to a DOS
                         * prompt, which is the floppy most people want. */
                        const std::string fd_boot = demo_dir + "/FREEDOS.IMG";
                        if (retrodos::demo_prepare(demo_dir) && path_is_file(fd_boot))
                            fds.insert(fds.begin(), fd_boot);
                        done = media_shelf("fds", "floppy", fds,
                                           drives, retrodos::DriveImage::Floppy, insert_target, cw, ddir);
                    } else {
                        /*
                         * A blank hard disk, made by the engine's own IMGMAKE
                         * -- partitioned and formatted, which is what a DOS
                         * installer booted from a CD wants to find. The same
                         * mechanism the Windows walkthrough uses: a one-shot
                         * machine that makes the file and exits.
                         */
                        static char new_name[64] = "dos";
                        static int  new_size = 1;
                        static const char *kSizes[]  = { "256 MB", "512 MB", "1 GB", "2 GB", "8 GB" };
                        static const int   kSizeMb[] = { 256, 512, 1024, 2048, 8192 };
                        static std::string delete_pick;
                        const std::string hdd_dir = cfg.storage_root.empty()
                            ? cfg.library_root : retrodos::discs_dir(cfg, "dos");
                        if (icon_button("##newhdd", Icon::Plus, "Create a blank hard disk"))
                            ImGui::OpenPopup("New hard disk");
                        ImGui::SameLine(); ImGui::TextUnformatted("New disk");
                        if (ImGui::BeginPopupModal("New hard disk", nullptr,
                                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::TextWrapped("A blank, formatted disk to install DOS onto, "
                                               "or to give a game room of its own.");
                            ImGui::Spacing();
                            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
                            ImGui::InputText("Name", new_name, sizeof new_name);
                            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
                            ImGui::Combo("Size", &new_size, kSizes, (int)SDL_arraysize(kSizes));
                            TextDimWrapped("Takes only the space it uses. Over 512 MB is "
                                           "FAT32, which MS-DOS 6 cannot read; FreeDOS and "
                                           "Windows 98 can.");
                            ImGui::Spacing();
                            const std::string target = hdd_dir + "/" + new_name + ".img";
                            const bool exists = path_is_file(target);
                            if (exists) ImGui::TextDisabled("That name is taken.");
                            ImGui::BeginDisabled(!new_name[0] || exists);
                            if (ImGui::Button("Create", ImVec2(ImGui::GetFontSize() * 8.0f, 0))) {
                                SDL_CreateDirectory(hdd_dir.c_str());
                                Game g;
                                g.name    = std::string("New disk ") + new_name;
                                g.dir     = hdd_dir;
                                g.run     = "IMGMAKE \"" + target + "\" -t hd -size " +
                                            std::to_string(kSizeMb[new_size]) + "\nEXIT";
                                g.run_raw = true;
                                g.is_demo = true;   /* no drives, no per-title settings */
                                ImGui::CloseCurrentPopup();
                                launch(g);
                            }
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 8.0f, 0)))
                                ImGui::CloseCurrentPopup();
                            ImGui::EndPopup();
                        }
                        ImGui::Spacing();
                        done = media_shelf("hdds", "hard disk", scan_media(cfg, MediaKind::Hdd),
                                           drives, retrodos::DriveImage::Hdd, insert_target, cw, ddir,
                                           &delete_pick);
                        if (!delete_pick.empty()) ImGui::OpenPopup("Delete hard disk?");
                        if (ImGui::BeginPopupModal("Delete hard disk?", nullptr,
                                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::TextWrapped("Delete %s?", base_name(delete_pick).c_str());
                            TextDimWrapped("Everything on it is lost. This cannot be undone.");
                            ImGui::Spacing();
                            if (ImGui::Button("Delete", ImVec2(ImGui::GetFontSize() * 8.0f, 0))) {
                                for (auto &d : drives) if (d.path == delete_pick) d.path.clear();
                                SDL_RemovePath(delete_pick.c_str());
                                delete_pick.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Keep", ImVec2(ImGui::GetFontSize() * 8.0f, 0))) {
                                delete_pick.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                    }
                    /* Chosen: back to Launch, where the drive now shows it. */
                    if (done) page = Page::Launch;
                }

                if (page == Page::Library) {
                    /* The Media strip, with Configs (this page) lit. */
                    {
                        const char *ntabs[] = { "CDs", "Floppies", "Hard disks", "Configs" };
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
                        for (int t = 0; t < 4; ++t) {
                            if (t) ImGui::SameLine();
                            const bool on = (t == 3);
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(ntabs[t], ImVec2(tw, 0)) && t < 3) {
                                media_tab = t; page = Page::Media; selected = -1;
                            }
                            if (on) ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                    }
                    const std::string &list_root = cfg.library_root;
                    ImGui::TextUnformatted("Game configs");
                    {
                        const float bw2 = ImGui::CalcTextSize("Add").x +
                                          ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float bw3 = ImGui::CalcTextSize("Rescan").x +
                                          ImGui::GetStyle().FramePadding.x * 2.0f;
                        ImGui::SameLine(cw - bw2 - bw3 - ImGui::GetStyle().ItemSpacing.x);
                        if (ImGui::Button("Add")) retrodos::saf_pick_game(list_root);
                        ImGui::SameLine();
                        if (ImGui::Button("Rescan")) refresh();
                    }

                    if (!library_note.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                        ImGui::TextWrapped("%s", library_note.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("x")) library_note.clear();
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

                    {
                        {
                            const float fs = ImGui::GetFontSize();
                            char label[160];
                            if (search[0]) SDL_snprintf(label, sizeof label, "Search: %s  X", search);
                            else           SDL_snprintf(label, sizeof label, "Search...");
                            if (ImGui::Button(label, ImVec2(0, fs * 1.9f))) {
                                if (search[0]) search[0] = 0;
                                else ImGui::OpenPopup("Find a title");
                            }
                            if (ImGui::BeginPopupModal("Find a title", nullptr,
                                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                                static char box[96];
                                ImGui::SetNextItemWidth(fs * 18.0f);
                                const bool go = ImGui::InputTextWithHint(
                                    "##term", "part of a name", box, sizeof box,
                                    ImGuiInputTextFlags_EnterReturnsTrue);
                                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
                                ImGui::Spacing();
                                if (go || ImGui::Button("Search", ImVec2(fs * 8.0f, 0))) {
                                    SDL_strlcpy(search, box, sizeof search);
                                    box[0] = 0; ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel", ImVec2(fs * 8.0f, 0))) {
                                    box[0] = 0; ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }

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
                                if (g.is_machine) continue;
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
                            if (has[26]) chip("0-9", '#');
                        }

                        /*
                         * A Windows machine is not a DOS game and does not
                         * belong in the list of them. It has its own page --
                         * Windows Setup -- which knows how far through the
                         * install it is and can start it; here it was one more
                         * row to scroll past, under a letter that promised a
                         * game and delivered an operating system.
                         *
                         * It stays in `games` because launching still goes
                         * through that list; it is only hidden from this view.
                         */
                        std::vector<int> shown;
                        int dos_games = 0;
                        shown.reserve(games.size());
                        for (int i = 0; i < (int)games.size(); ++i) {
                            if (games[i].is_machine) continue;
                            ++dos_games;
                            if (filter_letter && games[i].initial != filter_letter) continue;
                            if (search[0] && !SDL_strcasestr(games[i].name.c_str(), search)) continue;
                            shown.push_back(i);
                        }
                        ImGui::Text("%zu of %d", shown.size(), dos_games);
                        ImGui::SameLine();
                        ImGui::TextDisabled("  %s", root_label(list_root).c_str());

                        if (dos_games == 0) {
                            ImGui::Spacing();
                            ImGui::TextWrapped("No games found in:");
                            ImGui::TextWrapped("%s", root_label(list_root).c_str());
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

                                    /* A title with nothing runnable in it is
                                     * drawn, but not offered: pressing it
                                     * could only mount an empty folder. */
                                    const bool startable =
                                        games[gi].is_machine ||
                                        !games[gi].run.empty() ||
                                        !games[gi].autoexec.empty();
                                    /* A tap loads it onto the Launch page rather
                                     * than starting it: the drives get chosen
                                     * there, and starting is one deliberate
                                     * press on one deliberate button. */
                                    const bool is_loaded = loaded &&
                                        loaded_game.name == games[gi].name &&
                                        loaded_game.is_app == games[gi].is_app;
                                    if (ImGui::Selectable(games[gi].name.c_str(), is_loaded,
                                            startable ? 0 : ImGuiSelectableFlags_Disabled,
                                            ImVec2(0, row_h))) {
                                        loaded_game = games[gi];
                                        loaded = true;
                                        page = Page::Launch;
                                    }
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
                                    /* A machine has no per-game DOS settings to
                                     * open, so the button beside it says what it
                                     * will actually do: an installed machine
                                     * starts, an unfinished one goes to the
                                     * walkthrough that knows what is left. */
                                    if (games[gi].is_machine) {
                                        const bool ready = games[gi].machine_phase ==
                                                           retrodos::Win98Phase::Run;
                                        if (ImGui::SmallButton(ready ? "Start" : "Setup")) {
                                            if (ready) launch(games[gi]);
                                            else {
                                                win_stale = true;   /* re-read the phase */
                                                page = games[gi].machine_os ==
                                                       retrodos::OsKind::FreeDos
                                                     ? Page::FreeDos : Page::Windows;
                                            }
                                        }
                                    } else if (ImGui::SmallButton("Setup")) {
                                        selected = gi; page = Page::Settings;
                                    }
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndChild();
                        }
                    }
                }

                /* ---- Artwork ---- */
                else if (page == Page::Artwork) {
                    {
                        /* Login, Artwork, Downloads(admin) -- one screen. */
                        const bool admin = retrodos::media_downloads_available() &&
                                           account.signed_in && account.is_admin;
                        struct MT { const char *label; Page pg; bool show; };
                        const MT mts[] = {
                            { "Login",     Page::Account,   true },
                            { "Artwork",   Page::Artwork,   true },
                            { "Downloads", Page::Downloads, admin },
                        };
                        int n = 0; for (const MT &m : mts) if (m.show) ++n;
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x * (float)(n - 1)) / (float)n;
                        bool first = true;
                        for (const MT &m : mts) {
                            if (!m.show) continue;
                            if (!first) ImGui::SameLine();
                            first = false;
                            const bool on = page == m.pg;
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(m.label, ImVec2(tw, 0)) && !on) { page = m.pg; }
                            if (on) ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                    }
                    ImGui::TextUnformatted("Artwork");
                    ImGui::Separator();

                    if (!media_msg.empty()) {
                        ImGui::TextWrapped("%s", media_msg.c_str());
                        ImGui::Separator();
                    }

                    if (!account.signed_in) {
                        /* The sign-in form itself lives on Account. Two copies
                         * of it would be two places to fix, and this page has
                         * nothing to say until there is an account. */
                        ImGui::TextWrapped("Box art comes from your RetroMedia account.");
                        ImGui::Spacing();
                        if (ImGui::Button("Sign in", ImVec2(cw * 0.4f, 0)))
                            page = Page::Account;
                    } else {
                        ImGui::TextDisabled("Signed in as %s", account.email.c_str());
                        ImGui::Spacing();
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
                    }
                }

                /* ---- Account ---- */
                else if (page == Page::Account) {
                    {
                        /* Login, Artwork, Downloads(admin) -- one screen. */
                        const bool admin = retrodos::media_downloads_available() &&
                                           account.signed_in && account.is_admin;
                        struct MT { const char *label; Page pg; bool show; };
                        const MT mts[] = {
                            { "Login",     Page::Account,   true },
                            { "Artwork",   Page::Artwork,   true },
                            { "Downloads", Page::Downloads, admin },
                        };
                        int n = 0; for (const MT &m : mts) if (m.show) ++n;
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x * (float)(n - 1)) / (float)n;
                        bool first = true;
                        for (const MT &m : mts) {
                            if (!m.show) continue;
                            if (!first) ImGui::SameLine();
                            first = false;
                            const bool on = page == m.pg;
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(m.label, ImVec2(tw, 0)) && !on) { page = m.pg; }
                            if (on) ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                    }
                    ImGui::TextUnformatted("RetroMedia account");
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
                        ImGui::Spacing();
                        if (ImGui::Button("Sign out")) {
                            media_busy = true; retrodos::media_begin_logout();
                        }
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        if (ImGui::Button("Get artwork for my library", ImVec2(cw * 0.5f, 0)))
                            page = Page::Artwork;
                        if (retrodos::media_downloads_available() && account.is_admin) {
                            ImGui::SameLine();
                            if (ImGui::Button("Downloads")) page = Page::Downloads;
                        }
                    } else {
                        ImGui::TextWrapped("Sign in to media.crownparkcomputing.com for box "
                                           "art. Administrators can also download games.");
                        ImGui::Spacing();

                        /* Email and password first, because that is how
                         * people sign in. */
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
                        ImGui::Spacing();
                        /* An API key stays as the alternative: it is revocable
                         * from the website and is the better thing to leave
                         * behind on a shared handheld. */
                        ImGui::TextWrapped("Or paste an API key from your account page "
                                           "(it starts with rmk_):");
                        ImGui::SetNextItemWidth(cw * 0.55f);
                        ImGui::InputText("API key", m_key, sizeof(m_key),
                                         ImGuiInputTextFlags_Password);
                        ImGui::BeginDisabled(media_busy || !m_key[0]);
                        if (ImGui::Button("Use API key")) {
                            media_busy = true; media_msg = "Checking key...";
                            retrodos::media_begin_login_key(m_key);
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Paste")) {
                            if (char *clip = SDL_GetClipboardText()) {
                                SDL_strlcpy(m_key, clip, sizeof(m_key));
                                SDL_free(clip);
                            }
                        }
                    }
                }

                /* ---- Downloads (admin) ---- */
                else if (page == Page::Downloads) {
                    {
                        /* Login, Artwork, Downloads(admin) -- one screen. */
                        const bool admin = retrodos::media_downloads_available() &&
                                           account.signed_in && account.is_admin;
                        struct MT { const char *label; Page pg; bool show; };
                        const MT mts[] = {
                            { "Login",     Page::Account,   true },
                            { "Artwork",   Page::Artwork,   true },
                            { "Downloads", Page::Downloads, admin },
                        };
                        int n = 0; for (const MT &m : mts) if (m.show) ++n;
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x * (float)(n - 1)) / (float)n;
                        bool first = true;
                        for (const MT &m : mts) {
                            if (!m.show) continue;
                            if (!first) ImGui::SameLine();
                            first = false;
                            const bool on = page == m.pg;
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(m.label, ImVec2(tw, 0)) && !on) { page = m.pg; }
                            if (on) ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                    }
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

                    /*
                     * The whole catalogue is fetched once and filtered here.
                     *
                     * It used to send the search and the letter to the server
                     * and re-fetch -- which meant nothing happened at all until
                     * you noticed the Browse button and pressed it again. A
                     * filter that needs a second button is a filter that looks
                     * broken. There are a few hundred titles and they are
                     * already in memory, so filtering is instant and, unlike
                     * the round trip, cannot half-work.
                     */
                    ImGui::SetNextItemWidth(cw * 0.45f);
                    ImGui::InputTextWithHint("##csearch", "Search catalogue...",
                                             cat_search, sizeof(cat_search));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(media_busy);
                    if (ImGui::Button(catalogue.empty() ? "Load catalogue" : "Refresh")) {
                        media_busy = true; art_mode = false;
                        media_msg = "Loading catalogue...";
                        cat_letter = 0;
                        cat_search[0] = 0;
                        retrodos::media_begin_catalogue("", "", true);
                    }
                    ImGui::EndDisabled();

                    /* Only the letters with something behind them, the same
                     * rule the DOS Library follows: every chip a promise. */
                    if (!catalogue.empty()) {
                        bool has[27] = { false };
                        auto initial_of = [](const std::string &t) -> char {
                            const char c = t.empty() ? '#'
                                         : (char)SDL_toupper((unsigned char)t[0]);
                            return (c >= 'A' && c <= 'Z') ? c : '#';
                        };
                        for (const retrodos::MediaGame &g : catalogue) {
                            const char c = initial_of(g.title);
                            if (c == '#') has[26] = true; else has[c - 'A'] = true;
                        }
                        if (cat_letter) {
                            const bool still = (cat_letter == '#')
                                ? has[26]
                                : (cat_letter >= 'A' && cat_letter <= 'Z' &&
                                   has[cat_letter - 'A']);
                            if (!still) cat_letter = 0;
                        }

                        int count = 1;
                        for (bool b : has) if (b) ++count;
                        const float gapx = 3.0f;
                        const float cell = (ImGui::GetContentRegionAvail().x -
                                            gapx * (float)(count - 1)) / (float)count;
                        bool first = true;
                        auto chip = [&](const char *label, char value) {
                            if (!first) ImGui::SameLine(0.0f, gapx);
                            first = false;
                            const bool on = (cat_letter == value);
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(label, ImVec2(cell, 0)))
                                cat_letter = on ? 0 : value;
                            if (on) ImGui::PopStyleColor();
                        };
                        chip("All", 0);
                        for (char c = 'A'; c <= 'Z'; ++c) {
                            if (!has[c - 'A']) continue;
                            ImGui::PushID(1000 + c);
                            const char lbl[2] = { c, 0 };
                            chip(lbl, c);
                            ImGui::PopID();
                        }
                        if (has[26]) chip("#", '#');
                    }

                    std::vector<int> cshown;
                    cshown.reserve(catalogue.size());
                    for (int i = 0; i < (int)catalogue.size(); ++i) {
                        const std::string &t = catalogue[i].title;
                        if (cat_letter) {
                            const char c0 = t.empty() ? '#'
                                          : (char)SDL_toupper((unsigned char)t[0]);
                            const char ini = (c0 >= 'A' && c0 <= 'Z') ? c0 : '#';
                            if (ini != cat_letter) continue;
                        }
                        if (cat_search[0] && !SDL_strcasestr(t.c_str(), cat_search)) continue;
                        cshown.push_back(i);
                    }

                    if (catalogue.empty())
                        ImGui::TextDisabled("Press Load catalogue.");
                    else
                        ImGui::Text("%zu of %zu titles", cshown.size(), catalogue.size());

                    ImGui::BeginChild("##cat");
                    scroll_by_drag();
                    /* Rows are given room: at arm's length a list packed at text
                     * height is easy to mis-tap, and a mis-tap here starts a
                     * download that can run to a gigabyte. */
                    const float row_h = ImGui::GetFrameHeight() * 1.7f;
                    ImGuiListClipper clip;
                    clip.Begin((int)cshown.size(), row_h);
                    while (clip.Step()) {
                        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                            const retrodos::MediaGame &cg = catalogue[cshown[r]];
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
                else if (page == Page::Settings) {
                    static Settings edit;
                    static int edit_for = -3;

                    /* Choose the tab only on arrival, so the page is still a
                     * tabbed page once you are on it: Machine opens on CPU,
                     * Input opens on Input, and after that whichever tab you
                     * pick stays picked until you leave. */
                    const MachineTab open_tab =
                        page_changed ? MachineTab::Cpu : MachineTab::None;

                    const bool per_game = (selected >= 0 && selected < (int)games.size());
                    if (edit_for != selected) {
                        edit = cfg.defaults;
                        if (per_game)
                            retrodos::load_game_settings(games_dir, games[selected].name, edit);
                        edit_for = selected;
                    }

                    if (per_game) ImGui::Text("Machine - %s",
                                              games[selected].name.c_str());
                    else ImGui::TextUnformatted("Machine - defaults for all games");

                    /* No cross-link button any more. Input is a tab beside
                     * CPU, Video, Sound and DOS, so both halves of a game's
                     * setup are one click apart and neither loses which game is
                     * being edited -- which is the whole job the button was
                     * doing. */
                    ImGui::Separator();

                    ImGui::BeginChild("##sset",
                                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f),
                                      0, ImGuiWindowFlags_NoScrollbar);
                    scroll_by_drag();
                    settings_widgets(edit, open_tab);
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
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("Run setup again")) {
                            /* Back through the whole wizard, storage step
                             * included. Nothing is moved or deleted: choosing
                             * a different folder simply points the app at it. */
                            edit_for = -3; selected = -1;
                            roots = candidate_roots();
                            view = View::Wizard;
                        }
                    }
                }

                /* ---- Demo ---- */
                else if ((page == Page::Windows || page == Page::FreeDos) && !machine_wizard &&
                         machine_ready(page == Page::FreeDos ? retrodos::OsKind::FreeDos
                                                             : retrodos::OsKind::Win98)) {
                    {
                        /* Windows and FreeDOS, combined: a tab switches which
                         * one this page is about, so the rail carries one
                         * "Wizards" entry rather than two. */
                        const char *wt[] = { "Windows", "FreeDOS" };
                        const Page   wp[] = { Page::Windows, Page::FreeDos };
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
                        for (int t = 0; t < 2; ++t) {
                            if (t) ImGui::SameLine();
                            const bool on = page == wp[t];
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(wt[t], ImVec2(tw, 0)) && !on) {
                                page = wp[t]; win_stale = true; machine_wizard = false;
                            }
                            if (on) ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                    }
                    /* An installed machine: start it, swap its disc, or go
                     * back into the walkthrough on purpose. The walkthrough is
                     * what this page shows until the machine exists. */
                    const retrodos::OsKind os = page == Page::FreeDos
                        ? retrodos::OsKind::FreeDos : retrodos::OsKind::Win98;
                    windows_widgets(cfg, cw, os, jump_to_os, start_os);
                    if (jump_to_os >= 0) {
                        machine_wizard = true;     /* asked for it: show it */
                        win_stale = true;
                        jump_to_os = -1;
                    }
                    if (start_os >= 0) {
                        start_os = -1;
                        Game g;
                        g.name = retrodos::os_folder(os);
                        g.dir  = retrodos::machines_dir(cfg) + "/" + g.name;
                        launch(g);
                    }
                }
                else if (page == Page::Windows || page == Page::FreeDos) {
                    {
                        /* Windows and FreeDOS, combined: a tab switches which
                         * one this page is about, so the rail carries one
                         * "Wizards" entry rather than two. */
                        const char *wt[] = { "Windows", "FreeDOS" };
                        const Page   wp[] = { Page::Windows, Page::FreeDos };
                        const float tw = (cw - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
                        for (int t = 0; t < 2; ++t) {
                            if (t) ImGui::SameLine();
                            const bool on = page == wp[t];
                            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(wt[t], ImVec2(tw, 0)) && !on) {
                                page = wp[t]; win_stale = true; machine_wizard = false;
                            }
                            if (on) ImGui::PopStyleColor();
                        }
                        ImGui::Spacing();
                    }
                    /*
                     * A walkthrough, not a settings page.
                     *
                     * Installing Windows 98 is a five-stage procedure with two
                     * reboots in the middle, and the first version of this was
                     * three labelled sections and a button -- everything was on
                     * screen and nothing told you what to do next. One step at
                     * a time, each saying what is about to happen and what you
                     * will see when it works, is what this actually needs.
                     */
                    /* One page, two operating systems. FreeDOS shares every
                     * mechanism -- disk, CD boot, phases -- and differs in
                     * what to tell the user at each step, so the text
                     * branches on `fd` and nothing else does. */
                    const retrodos::OsKind os = page == Page::FreeDos
                        ? retrodos::OsKind::FreeDos : retrodos::OsKind::Win98;
                    const bool fd = os == retrodos::OsKind::FreeDos;
                    const std::string osname = retrodos::os_name(os);
                    const std::string wdir = retrodos::machines_dir(cfg) + "/" + retrodos::os_folder(os);

                    static retrodos::Win98Install w;
                    static bool w_loaded = false;
                    static int  step = 0;
                    static bool iso_primed = false;
                    static retrodos::OsKind w_os = retrodos::OsKind::Win98;
                    if (win_stale) { w_loaded = false; win_stale = false; }
                    /* The statics belong to whichever machine was shown last;
                     * switching pages must not carry one machine's disc and
                     * step over to the other. */
                    if (w_os != os) { w_os = os; w_loaded = false; }
                    if (!w_loaded) {
                        if (!retrodos::win98_load(wdir, w)) w.dir = wdir;
                        w.os = os;
                        iso_primed = false;
                        w_loaded = true;
                        /* Resume where the machine actually is, so coming back
                         * to this page does not start the explanation again. */
                        step = (w.phase == retrodos::Win98Phase::Create)   ? 0
                             : (w.phase == retrodos::Win98Phase::Install)  ? 2
                             : (w.phase == retrodos::Win98Phase::Continue) ? 3
                                                                           : 4;
                    }

                    /*
                     * Re-check against the files every frame, not once on load.
                     *
                     * A recorded phase that has run ahead of reality is the
                     * worst failure this page has: a saved Continue with no
                     * disk image mounts nothing and boots nothing, and the
                     * guest just sits at a prompt saying nothing about a
                     * missing disk. So the phase is clamped DOWN to what the
                     * files support, and the step follows it back.
                     */
                    {
                        const retrodos::Win98Phase truth = retrodos::win98_true_phase(w);
                        if (truth != w.phase) {
                            w.phase = truth;
                            retrodos::win98_save(w);
                        }
                        if (w.phase == retrodos::Win98Phase::Create && step > 2)
                            step = 2;         /* no disk: cannot be past making it */
                        else if (w.phase == retrodos::Win98Phase::Install && step < 2)
                            step = 2;
                        else if (w.phase == retrodos::Win98Phase::Continue && step < 4)
                            step = 4;         /* Setup has rebooted at least once */
                        else if (w.phase == retrodos::Win98Phase::Run && step < 5)
                            step = 5;
                    }

                    static const char *kStepName[] = {
                        "Before you start", "Choose your CD", "Make the disk",
                        "Run Setup", "Finish Setup", "Done"
                    };
                    const int kSteps = (int)SDL_arraysize(kStepName);
                    if (step < 0) step = 0;
                    if (step >= kSteps) step = kSteps - 1;

                    ImGui::Text("%s  -  step %d of %d: %s",
                                osname.c_str(), step + 1, kSteps, kStepName[step]);
                    ImGui::Separator();
                    ImGui::Spacing();

                    bool can_advance = true;

                    ImGui::BeginChild("##wizbody",
                                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.4f));
                    scroll_by_drag();
                    /* Prose wraps to a column, not to the window. On a wide
                     * desktop the content pane is thousands of pixels across,
                     * and a paragraph set that wide is genuinely hard to read:
                     * the eye loses the line coming back. About 70 characters
                     * is the usual advice and it is what this works out at. */
                    const float col = std::min(cw, ImGui::GetFontSize() * 34.0f);
                    ImGui::PushTextWrapPos(col);

                    if (step == 0 && fd) {
                        ImGui::TextWrapped("This installs FreeDOS 1.3 inside the emulator "
                                           "from its own CD image, the same way the "
                                           "Windows 98 walkthrough does.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("You need two things:");
                        ImGui::Bullet();
                        ImGui::TextWrapped("The FreeDOS 1.3 Legacy CD image, %s, in your "
                                           "discs folder. The next step says where.",
                                           retrodos::kFreeDosIsoName);
                        ImGui::Bullet();
                        ImGui::TextWrapped("About ten minutes. The installer restarts "
                                           "the machine once part way through.");
                        ImGui::Spacing();
                        TextDimWrapped("The FreeDOS floppy on the Demo page only boots "
                                       "to a prompt. Installing onto a hard disk needs "
                                       "the CD, which is where the packages are.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("Everything is kept in:");
                        ImGui::TextDisabled("%s", wdir.c_str());
                    }
                    else if (step == 0) {
                        ImGui::TextWrapped("This installs a real copy of Windows 98 "
                                           "inside the emulator, following DOSBox-X's "
                                           "own guide.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("You need two things:");
                        ImGui::Bullet();
                        ImGui::TextWrapped("A Windows 98 CD image (.iso). It must be an "
                                           "OEM Full edition -- those are the ones that "
                                           "boot on their own. An upgrade disc will not.");
                        ImGui::Bullet();
                        ImGui::TextWrapped("About 20 minutes, and patience: Setup runs "
                                           "on the slow, accurate processor because the "
                                           "fast one crashes it.");
                        ImGui::Spacing();
                        TextDimWrapped("Nothing is downloaded and no Windows files are "
                                       "bundled with this app. The disc is yours.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("Everything is kept in:");
                        ImGui::TextDisabled("%s", wdir.c_str());
                    }
                    else if (step == 1) {
                        if (fd) {
                            ImGui::TextWrapped("Put your FreeDOS 1.3 CD image, %s, in "
                                               "the FreeDOS discs folder:",
                                               retrodos::kFreeDosIsoName);
                            ImGui::TextDisabled("%s", retrodos::discs_dir(cfg, "freedos").c_str());
                            ImGui::Spacing();
                            TextDimWrapped("Use the Legacy CD, not the LiveCD: only the "
                                           "Legacy CD boots here. FreeDOS floppy images "
                                           "can go in the same folder and show under "
                                           "Discs, but this walkthrough installs from "
                                           "the CD.");
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                            ImGui::TextWrapped("Then pick it here.");
                        } else {
                            ImGui::TextWrapped("Pick your Windows 98 disc image.");
                        }
                        ImGui::Spacing();

                        static char iso_buf[1024] = {0};
                        if (!iso_primed) {
                            SDL_strlcpy(iso_buf, w.iso.c_str(), sizeof(iso_buf));
                            iso_primed = true;
                        }

                        /* Discs in the games folder, as buttons. Typing an
                         * absolute path on a handheld with an on-screen
                         * keyboard is miserable. */
                        int offered = 0;
                        const std::string droot = cfg.storage_root.empty() ? cfg.library_root
                                                                           : cfg.storage_root;
                        for (const std::string &full : scan_media(cfg, MediaKind::Cd)) {
                            if (offered >= 12) break;
                            ImGui::PushID(full.c_str());
                            const bool chosen = (w.iso == full);
                            if (chosen)
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::Button(base_name(full).c_str(), ImVec2(cw * 0.86f, ImGui::GetFontSize() * 2.1f))) {
                                w.iso = full;
                                SDL_strlcpy(iso_buf, w.iso.c_str(), sizeof(iso_buf));
                                retrodos::win98_save(w);
                            }
                            if (chosen) ImGui::PopStyleColor();
                            ImGui::PopID();
                            ++offered;
                        }
                        if (!offered) {
                            TextDimWrapped("No CD image found anywhere in your folder. "
                                           "Put one there and it will be listed here, or "
                                           "type the full path:");
                            ImGui::TextDisabled("%s", droot.c_str());
                        } else {
                            ImGui::Spacing();
                            TextDimWrapped("Or type a full path:");
                        }
                        ImGui::SetNextItemWidth(cw * 0.86f);
                        if (ImGui::InputText("##isopath", iso_buf, sizeof(iso_buf))) {
                            w.iso = iso_buf;
                            retrodos::win98_save(w);
                        }

                        ImGui::Spacing();
                        if (w.iso.empty()) {
                            can_advance = false;
                            ImGui::TextDisabled("Choose a disc to continue.");
                        } else {
                            ImGui::Text("Using: %s", w.iso.c_str());
                        }
                    }
                    else if (step == 2) {
                        ImGui::TextWrapped("%s needs a hard disk to install onto. "
                                           "This makes an empty one.", osname.c_str());
                        ImGui::Spacing();
                        static int size_choice = 0;
                        static const char *kSizes[] = { "8 GB (recommended)", "2 GB",
                                                        "16 GB", "32 GB" };
                        static const int   kSizeMb[] = { 0, 2048, 16384, 32768 };
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
                        ImGui::Combo("Size", &size_choice, kSizes,
                                     (int)SDL_arraysize(kSizes));
                        w.size_mb = kSizeMb[size_choice];
                        if (fd)
                            TextDimWrapped("It only takes up what it actually uses, so a "
                                           "bigger disk costs nothing until you fill it. "
                                           "Over 512 MB is formatted FAT32, which FreeDOS "
                                           "handles.");
                        else
                            TextDimWrapped("It only takes up what it actually uses, so a "
                                           "bigger disk costs nothing until you fill it. "
                                           "Over 512 MB is formatted FAT32; Windows 98's own "
                                           "driver cannot handle more than 128 GB.");
                        ImGui::Spacing();

                        const bool made = (w.phase != retrodos::Win98Phase::Create);
                        if (made) {
                            ImGui::TextWrapped("The disk is ready.");
                        } else {
                            if (ImGui::Button("Make the disk", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                                SDL_CreateDirectory(w.dir.c_str());
                                retrodos::win98_save(w);
                                Game g;
                                g.name = retrodos::os_folder(os);
                                g.dir  = w.dir;
                                g.conf_override = retrodos::win98_conf(w);
                                launch(g);
                            }
                            TextDimWrapped("A DOS screen appears for a moment and closes "
                                           "again by itself. That is all this step does.");
                            can_advance = false;
                        }
                    }
                    else if (step == 3 && fd) {
                        ImGui::TextWrapped("Runs the FreeDOS installer from the CD. "
                                           "The keyboard opens itself; the on-screen "
                                           "hint lists the keys.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("Y to proceed and to partition. It restarts to "
                                           "a prompt -- that is normal: come back and "
                                           "press Start again. Second run: Y to format "
                                           "C:, pick a layout, Full installation, Y.");
                        ImGui::Spacing();

                        const std::string blocked = retrodos::win98_blocker(w);
                        if (!blocked.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                            ImGui::TextWrapped("%s", blocked.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::BeginDisabled(!blocked.empty());
                        if (ImGui::Button("Start the installer", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            win_hint =
                                "Language: 1.  Proceed: Y.\n"
                                "1st run: partition Y, restart Y, then start this again.\n"
                                "2nd run: format Y, layout, Full installation, install Y, restart Y.";
                            w.phase = retrodos::Win98Phase::Install;
                            retrodos::win98_save(w);
                            win_running_dir   = w.dir;
                            win_running_phase = w.phase;
                            win_running_os    = w.os;
                            Game g;
                            g.name = retrodos::os_folder(os);
                            g.dir  = w.dir;
                            g.conf_override = retrodos::win98_conf(w);
                            launch(g);
                        }
                        ImGui::EndDisabled();
                        ImGui::Spacing();
                        TextDimWrapped("If it says 'El Torito boot record not found' or "
                                       "'This is not a bootable disk', the image is the "
                                       "LiveCD. Only the Legacy CD boots here.");
                    }
                    else if (step == 3) {
                        ImGui::TextWrapped("Starts the machine from the CD to run Setup.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("First menu: choose 2, Boot from CD-ROM.");
                        ImGui::TextWrapped("Second menu: choose 1, or wait 30 seconds.");
                        ImGui::Spacing();
                        TextDimWrapped("Setup copies files and restarts to a DOS prompt "
                                       "-- that is normal: come back and press Next. At a "
                                       "prompt instead? Type D:  then  cd \\WIN98  then  setup");
                        ImGui::Spacing();

                        const std::string blocked = retrodos::win98_blocker(w);
                        if (!blocked.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                            ImGui::TextWrapped("%s", blocked.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::BeginDisabled(!blocked.empty());
                        if (ImGui::Button("Start Setup", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            win_hint =
                                "1st menu: choose 2, Boot from CD-ROM.\n"
                                "2nd menu: choose 1, or wait 30s.\n"
                                "At a prompt instead?  D:  then  cd \\WIN98  then  setup";
                            w.phase = retrodos::Win98Phase::Install;
                            retrodos::win98_save(w);
                            win_running_dir   = w.dir;
                            win_running_phase = w.phase;
                            win_running_os    = w.os;
                            Game g;
                            g.name = "Windows 98";
                            g.dir  = w.dir;
                            g.conf_override = retrodos::win98_conf(w);
                            launch(g);
                        }
                        ImGui::EndDisabled();
                        ImGui::Spacing();
                        TextDimWrapped("If it says 'This is not a bootable disk', the "
                                       "disc is not an OEM Full edition and cannot start "
                                       "Setup on its own.");
                    }
                    else if (step == 4 && fd) {
                        ImGui::TextWrapped("FreeDOS is on the hard disk. From here the "
                                           "machine boots from it, with the CD still in "
                                           "the drive as D: so more packages can be added "
                                           "later.");
                        ImGui::Spacing();
                        const std::string blocked4 = retrodos::win98_blocker(w);
                        if (!blocked4.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                            ImGui::TextWrapped("%s", blocked4.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::BeginDisabled(!blocked4.empty());
                        if (ImGui::Button("Boot FreeDOS", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            win_hint = "It should boot to a C:\\> prompt. If it stops at "
                                       "'bad or missing command interpreter', the "
                                       "packages did not finish: Start again.";
                            w.phase = retrodos::Win98Phase::Continue;
                            retrodos::win98_save(w);
                            win_running_dir   = w.dir;
                            win_running_phase = w.phase;
                            win_running_os    = w.os;
                            Game g;
                            g.name = retrodos::os_folder(os);
                            g.dir  = w.dir;
                            g.conf_override = retrodos::win98_conf(w);
                            launch(g);
                        }
                        ImGui::EndDisabled();
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        if (retrodos::win98_installed(w)) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
                            ImGui::TextWrapped("FreeDOS is on the hard disk.");
                            ImGui::PopStyleColor();
                        }
                        ImGui::TextWrapped("Once it boots to a C:\\> prompt on its own, "
                                           "it is installed:");
                        if (ImGui::Button("FreeDOS is installed", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            w.phase = retrodos::Win98Phase::Run;
                            retrodos::win98_save(w);
                            step = 5;
                        }
                        can_advance = false;   /* the button above is the way on */
                    }
                    else if (step == 4) {
                        ImGui::TextWrapped("Setup has restarted the machine at least "
                                           "once. From here it boots from the hard disk "
                                           "and finishes, with the CD still in the "
                                           "drive because it keeps asking for it.");
                        ImGui::Spacing();
                        ImGui::TextWrapped("Expect to run this a few times: Windows "
                                           "restarts itself several times before it is "
                                           "finished, and each restart brings you back "
                                           "here.");
                        ImGui::Spacing();
                        const std::string blocked4 = retrodos::win98_blocker(w);
                        if (!blocked4.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                            ImGui::TextWrapped("%s", blocked4.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::BeginDisabled(!blocked4.empty());
                        if (ImGui::Button("Continue Setup", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            win_hint = "Let Setup finish. It restarts the machine "
                                       "several times; each time you land back in "
                                       "Retro-DOS, press Continue Setup again.";
                            w.phase = retrodos::Win98Phase::Continue;
                            retrodos::win98_save(w);
                            win_running_dir   = w.dir;
                            win_running_phase = w.phase;
                            win_running_os    = w.os;
                            Game g;
                            g.name = "Windows 98";
                            g.dir  = w.dir;
                            g.conf_override = retrodos::win98_conf(w);
                            launch(g);
                        }
                        ImGui::EndDisabled();
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        /* Read from the disk image rather than assumed: the app
                         * can see that Windows is on there, and saying so is
                         * the difference between asking the user to confirm
                         * something and asking them to guess. */
                        if (retrodos::win98_installed(w)) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
                            ImGui::TextWrapped("Windows is on the hard disk.");
                            ImGui::PopStyleColor();
                        }
                        ImGui::TextWrapped("Once Windows reaches its desktop and asks "
                                           "you nothing more, it is installed:");
                        if (ImGui::Button("Windows is installed", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            w.phase = retrodos::Win98Phase::Run;
                            retrodos::win98_save(w);
                            step = 5;
                        }
                        can_advance = false;   /* the button above is the way on */
                    }
                    else if (fd) {
                        ImGui::TextWrapped("FreeDOS 1.3 is installed.");
                        ImGui::Spacing();
                        if (ImGui::Button("Start FreeDOS", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            w.phase = retrodos::Win98Phase::Run;
                            retrodos::win98_save(w);
                            win_running_dir   = w.dir;
                            win_running_phase = w.phase;
                            win_running_os    = w.os;
                            Game g;
                            g.name = retrodos::os_folder(os);
                            g.dir  = w.dir;
                            g.conf_override = retrodos::win98_conf(w);
                            launch(g);
                        }
                        ImGui::Spacing();
                        ImGui::Checkbox("Faster processor now that it is installed",
                                        &w.fast_core_after_install);
                        if (ImGui::IsItemDeactivatedAfterEdit()) retrodos::win98_save(w);
                        TextDimWrapped("The installer runs on the slow, accurate "
                                       "processor. Afterwards the fast one is fine.");
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        TextDimWrapped("The CD stays in as D: while the image is still "
                                       "in your games folder; FDIMPLES installs more "
                                       "packages from it. This machine is its own hard "
                                       "disk: your DOS Library games are not inside it, "
                                       "and still run from the Library as before.");
                    }
                    else {
                        ImGui::TextWrapped("Windows 98 is installed.");
                        ImGui::Spacing();
                        if (ImGui::Button("Start Windows 98", ImVec2(cw * 0.62f, ImGui::GetFontSize() * 2.2f))) {
                            w.phase = retrodos::Win98Phase::Run;
                            retrodos::win98_save(w);
                            win_running_dir   = w.dir;
                            win_running_phase = w.phase;
                            win_running_os    = w.os;
                            Game g;
                            g.name = "Windows 98";
                            g.dir  = w.dir;
                            g.conf_override = retrodos::win98_conf(w);
                            launch(g);
                        }
                        ImGui::Spacing();
                        ImGui::Checkbox("Faster processor now that Setup is done",
                                        &w.fast_core_after_install);
                        if (ImGui::IsItemDeactivatedAfterEdit()) retrodos::win98_save(w);
                        TextDimWrapped("Setup has to run on the slow, accurate "
                                       "processor. Afterwards the fast one is usually "
                                       "fine, and much faster.");
                        ImGui::Spacing();
                        ImGui::Checkbox("3dfx Voodoo graphics card", &w.voodoo);
                        if (ImGui::IsItemDeactivatedAfterEdit()) retrodos::win98_save(w);
                        TextDimWrapped("A 3dfx Voodoo Graphics (Voodoo 1): the 3D card "
                                       "the Glide and Direct3D games of 1996-1999 are "
                                       "looking for. It is a 3D-only add-in card, so "
                                       "Windows lists it under 'Sound, video and game "
                                       "controllers', never under Display adapters -- "
                                       "until it has a driver it is 'PCI Multimedia "
                                       "Video Device' under Other devices.");
                        ImGui::Spacing();
                        TextDimWrapped("It needs the 3dfx Voodoo Graphics Windows 9x "
                                       "reference driver (3.01.00). Put it in A: from the "
                                       "Floppies shelf, or in a drive from CDs, before "
                                       "starting Windows; when Windows finds the PCI "
                                       "Multimedia Video Device let it search that drive. "
                                       "Found earlier and skipped? Device Manager > Other "
                                       "devices > that device > Update Driver. Emulated "
                                       "in software, so it is slow on a handheld.");
                        ImGui::Spacing();
                        TextDimWrapped("An add-in card, not a display adapter: Glide games "
                                       "and Direct3D games that let you pick a 3D device "
                                       "use it. A game that only looks at the primary "
                                       "display for Direct3D (Sega Rally 2, and most from "
                                       "late 1999 on) will never list it -- those need a "
                                       "Banshee or Voodoo3-class card as the display, "
                                       "which is what Retro-X86 provides.");
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        TextDimWrapped("To stop Windows asking for the CD, copy the "
                                       "\\WIN98 folder from the disc to C: once you are "
                                       "inside Windows.");
                    }

                    ImGui::PopTextWrapPos();
                    ImGui::EndChild();

                    /* ---- the wizard's own footer ---- */
                    ImGui::Separator();
                    const ImVec2 navsz(ImGui::GetFontSize() * 7.0f, ImGui::GetFontSize() * 2.2f);
                    ImGui::BeginDisabled(step == 0);
                    if (ImGui::Button("Back", navsz)) --step;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!can_advance || step >= kSteps - 1);
                    if (ImGui::Button("Next", navsz)) ++step;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("   step %d of %d", step + 1, kSteps);

                    /* Start again.
                     *
                     * A half-finished Windows cannot be resumed by pointing
                     * Setup at it a second time -- it has to go back to an
                     * empty disk -- so this is the only way out of an install
                     * that went wrong, and it needs to be reachable from every
                     * step rather than only the one where things broke.
                     */
                    if (machine_wizard) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Back to the machine")) machine_wizard = false;
                    }
                    if (step > 0) {
                        ImGui::SameLine(cw - ImGui::GetFontSize() * 7.0f);
                        if (ImGui::SmallButton("Start again"))
                            ImGui::OpenPopup("Start again?");
                    }
                    if (ImGui::BeginPopupModal("Start again?", nullptr,
                                               ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextWrapped("Erase the hard disk and begin the "
                                           "installation from the start?");
                        ImGui::Spacing();
                        TextDimWrapped("Anything installed inside the machine is lost. "
                                       "Your CD image is kept.");
                        ImGui::Spacing();
                        if (ImGui::Button("Erase and start again")) {
                            retrodos::win98_reset(w, true);
                            step = 0;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
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
                            g.keyboard_on_start = retrodos::demo_wants_keyboard(k);
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
            /*
             * Resume is pinned, and everything else scrolls.
             *
             * This used to auto-size and grow downwards, one full-width button
             * per row. Open the disc list on a machine with a few images and
             * the panel became taller than the window -- and because it is
             * centred, it grew off BOTH ends, taking Resume with it. The way
             * out of the menu is the one control that must never be the thing
             * that scrolled away.
             */
            if (view == View::Emulator && show_overlay) {
                const float fs = ImGui::GetFontSize();
                const float panel_w = fs * 26.0f;
                const float panel_h = (full.y * 0.8f < fs * 32.0f)
                                    ? full.y * 0.8f : fs * 32.0f;
                ImGui::SetNextWindowPos(ImVec2(origin.x + full.x * 0.5f,
                                               origin.y + full.y * 0.5f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
                ImGui::Begin("Paused", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse);

                char prog[64] = {0};
                retrodos_host_running_program(prog, sizeof(prog));

                /* ---- pinned ---- */
                const float halfw = (ImGui::GetContentRegionAvail().x -
                                     ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (ImGui::Button("Resume", ImVec2(halfw, fs * 1.8f)))
                    show_overlay = false;
                ImGui::SameLine();
                if (ImGui::Button("Quit to library", ImVec2(halfw, fs * 1.8f))) {
                    retrodos_host_quit(); show_overlay = false;
                }
                ImGui::TextDisabled("Running: %s", prog[0] ? prog : "DOS");
                ImGui::Separator();

                ImGui::BeginChild("##pausebody", ImVec2(0, 0), 0,
                                  ImGuiWindowFlags_NoScrollbar);
                scroll_by_drag();

                if (!win_hint.empty()) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextWrapped("%s", win_hint.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Separator();
                }

                /* Two to a row from here down. Every one of these is a short
                 * label, and a column of full-width buttons was most of why
                 * the panel did not fit. */
                const ImVec2 bw(halfw, 0);
                /* One panel at a time. The keyboard and the controls panel
                 * both cover the game, both block keys from reaching DOS, and
                 * stacked they buried each other's close buttons -- a player
                 * ended up wedged with "1" apparently doing nothing. */
                /*
                 * "##kb", and not because the label needs hiding.
                 *
                 * ImGui identifies a widget by its label, so a button whose
                 * label changes with the thing it toggles is a DIFFERENT
                 * widget from one frame to the next. A press is delivered over
                 * two frames -- down, then release -- and if anything flips
                 * the state in between, the release lands on an id that no
                 * longer exists and the press is silently dropped. The visible
                 * text still changes; the identity no longer does.
                 */
                /* No keyboard button here: it is on the always-visible
                 * strip beside Esc, so it can be reached without opening
                 * this menu. */
                if (ImGui::Button("Controls", bw)) {
                    show_controls = true; show_overlay = false; show_osk = false;
                }
                /* If the guest's pointer is not moving, this says which half
                 * is at fault without anyone having to guess. */
                if (ImGui::Button(mouse_free ? "Give mouse to guest##mousefree"
                                             : "Release mouse##mousefree", bw))
                    mouse_free = !mouse_free;
                ImGui::SameLine();
                if (ImGui::Button("Ctrl+Alt+Del", bw)) {
                    retrodos::osk_send_ctrl_alt_del(); show_overlay = false;
                }
                ImGui::TextDisabled("mouse: %lu seen, %lu sent%s - or Ctrl+F10",
                                    mouse_seen, mouse_sent,
                                    mouse_grabbed ? ", held" : "");
                /* Fingers do not go through the mouse path at all, so without
                 * this line a touchscreen reads "0 sent" and looks broken. */
                ImGui::TextDisabled("touch: %lu to the guest", touch.sent());
                /*
                 * Paste, through SDL's clipboard rather than DOSBox-X's.
                 *
                 * The engine has a PasteClipboard, but its Linux path is X11
                 * only -- useless under Wayland -- and it is a stub on Android
                 * and iOS. SDL3's clipboard works on all four, and this build
                 * already links SDL3.
                 *
                 * The text is typed as keystrokes rather than injected, because
                 * that is the only route a guest OS understands: Windows is
                 * reading a keyboard controller, not a host API.
                 */
                if (retrodos::osk_type_pending() > 0) {
                    ImGui::TextDisabled("Typing... %d left",
                                        retrodos::osk_type_pending());
                } else if (SDL_HasClipboardText()) {
                    if (ImGui::Button("Paste clipboard", bw)) {
                        if (char *text = SDL_GetClipboardText()) {
                            const int n = retrodos::osk_type_text(text);
                            SDL_free(text);
                            /* Closing the overlay is the point: the keys have
                             * to land in the guest, and nothing reaches it
                             * while our own UI is in front. */
                            if (n > 0) show_overlay = false;
                        }
                    }
                }

                /* Laying the controls out belongs here rather than on the
                 * settings page: where a button should sit depends on what the
                 * game is showing underneath it, so it has to be done with the
                 * game on screen. */
                if (!gamepads.empty()) {
                    ImGui::TextDisabled("Gamepad connected -- on-screen controls hidden");
                } else if (active.onscreen_pad) {
                    /* The controller layout: which profile, and arranging it.
                     * Dragging, resizing and adding buttons happen on the
                     * game with the overlay live, so it is done from here. */
                    static const char *const kProfIds[]  = { "dos", "generic", "xbox360", "saturn" };
                    static const char *const kProfNames[] = { "DOS (stick + fire)", "Joystick",
                                                              "360 pad", "Saturn pad" };
                    int curp = 0;
                    for (int i = 0; i < 4; ++i) if (cfg.defaults.touch_pad == kProfIds[i]) curp = i;
                    ImGui::SetNextItemWidth(bw.x);
                    if (ImGui::BeginCombo("##tpadprofile", kProfNames[curp])) {
                        for (int i = 0; i < 4; ++i)
                            if (ImGui::Selectable(kProfNames[i], i == curp)) {
                                cfg.defaults.touch_pad = kProfIds[i];
                                retrodos::save_app_config(cfg_path, cfg);
                                tpad_reload();
                            }
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button(tpad.editing() ? "Done arranging"
                                                     : "Arrange controls", bw)) {
                        tpad.set_editing(!tpad.editing());
                        show_overlay = false;
                        if (!tpad.editing()) tpad.layout().save(cfg_dir);
                    }
                    if (tpad.editing()) {
                        if (tpad.designer_controls(&tpad_sink)) tpad.layout().save(cfg_dir);
                        if (tpad.has_selection() && tpad.cluster_panel())
                            tpad.layout().save(cfg_dir);
                    }
                    if (tpad.take_dirty()) tpad.layout().save(cfg_dir);
                }
                /*
                 * Changing the disc without stopping the machine.
                 *
                 * This is not an IMGMOUNT typed at a prompt -- there is no
                 * prompt once Windows has booted, and those letters would go
                 * straight into whatever has focus in the guest. It goes
                 * through the host API, which changes the media behind the
                 * emulated drive and tells the guest it changed, the same way
                 * DOSBox-X's own "change CD image" menu item does.
                 */
                if (ImGui::CollapsingHeader("Discs and disks")) {
                    static char cd_drive = 'D', fd_drive = 'A';
                    static std::string media_note;
                    static std::vector<std::string> cds, fds;
                    static bool media_scanned = false;
                    if (!media_scanned) {
                        const char *kind = win_running_dir.empty() ? "dos"
                            : win_running_os == retrodos::OsKind::FreeDos ? "freedos"
                                                                          : "windows";
                        (void)kind;
                        cds = scan_media(cfg, MediaKind::Cd);
                        fds = scan_media(cfg, MediaKind::Floppy);
                        /* The bundled FreeDOS boot floppy, as on the shelf. */
                        if (path_is_file(demo_dir + "/FREEDOS.IMG"))
                            fds.insert(fds.begin(), demo_dir + "/FREEDOS.IMG");
                        if (!win_running_dir.empty()) {
                            for (const std::string &p : scan_images(win_running_dir, true))
                                cds.push_back(p);
                            for (const std::string &p : scan_images(win_running_dir, false))
                                fds.push_back(p);
                        }
                        media_scanned = true;
                    }

                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                    auto drive_picker = [&](const char *label, char &letter,
                                            char lo, char hi) {
                        ImGui::TextDisabled("%s", label);
                        ImGui::SameLine();
                        ImGui::PushID(label);
                        if (ImGui::SmallButton("<") && letter > lo) --letter;
                        ImGui::SameLine();
                        ImGui::Text("%c:", letter);
                        ImGui::SameLine();
                        if (ImGui::SmallButton(">") && letter < hi) ++letter;
                        ImGui::PopID();
                    };

                    drive_picker("CD-ROM", cd_drive, 'C', 'Z');
                    for (size_t i = 0; i < cds.size() && i < 12; ++i) {
                        ImGui::PushID((int)(100 + i));
                        if (ImGui::Button(base_name(cds[i]).c_str(),
                                          ImVec2(ImGui::GetFontSize() * 24.0f, 0))) {
                            retrodos_host_insert_cd(cd_drive, cds[i].c_str());
                            media_note = std::string("Inserted ") +
                                         base_name(cds[i]) + " in " + cd_drive + ":";
                        }
                        ImGui::PopID();
                    }
                    if (cds.empty()) ImGui::TextDisabled("No disc images found.");
                    if (ImGui::SmallButton("Eject CD")) {
                        retrodos_host_insert_cd(cd_drive, "");
                        media_note = std::string("Ejected ") + cd_drive + ":";
                    }

                    ImGui::Spacing();
                    drive_picker("Floppy", fd_drive, 'A', 'B');
                    for (size_t i = 0; i < fds.size() && i < 12; ++i) {
                        ImGui::PushID((int)(200 + i));
                        if (ImGui::Button(base_name(fds[i]).c_str(),
                                          ImVec2(ImGui::GetFontSize() * 24.0f, 0))) {
                            retrodos_host_insert_floppy(fd_drive, fds[i].c_str());
                            media_note = std::string("Inserted ") +
                                         base_name(fds[i]) + " in " + fd_drive + ":";
                        }
                        ImGui::PopID();
                    }
                    if (fds.empty()) ImGui::TextDisabled("No floppy images found.");
                    if (ImGui::SmallButton("Eject floppy")) {
                        retrodos_host_insert_floppy(fd_drive, "");
                        media_note = std::string("Ejected ") + fd_drive + ":";
                    }

                    ImGui::SameLine();
                    if (ImGui::SmallButton("Rescan")) media_scanned = false;

                    if (!media_note.empty()) {
                        ImGui::Spacing();
                        ImGui::TextWrapped("%s", media_note.c_str());
                    }
                    /* Said plainly, because the failure is otherwise silent
                     * and looks like a broken button: the guest bound its
                     * drivers to the hardware it found when it booted. */
                    TextDimWrapped("Images are taken from your games folder. A drive that "
                                   "was not there when the machine booted cannot be added "
                                   "now -- only the disc in an existing one can change.");
                    ImGui::PopTextWrapPos();
                }

                ImGui::Spacing();
                if (ImGui::Button("Reset machine", bw)) {
                    retrodos_host_reset(true); show_overlay = false;
                }

                ImGui::EndChild();
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
            /*
             * One button, and only because there has to be one.
             *
             * A handheld has no Escape key and often no usable Back button, so
             * without this the menu is unreachable once a game is running and
             * the emulator becomes a one-way trip. Everything that used to sit
             * beside it -- Joy, Keys, Pad, Keyboard -- is in that menu already,
             * and four chips laid over somebody's Windows desktop is four more
             * things in the way than the screen can spare.
             *
             * Labelled for the key it stands in for, so the button and the
             * keyboard teach each other.
             */
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
                if (ImGui::Button("Esc")) show_overlay = true;
                ImGui::SameLine();
                /* The keyboard has its own toggle here, out on the always-
                 * visible strip, so it does not need the pause menu opened
                 * first. Highlighted while it is up. */
                if (show_osk) ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("Kbd")) {
                    show_osk = !show_osk;
                    if (show_osk) show_controls = false;
                }
                if (show_osk) ImGui::PopStyleColor();
                const ImVec2 bp = ImGui::GetWindowPos(), bs = ImGui::GetWindowSize();
                emubar = SDL_FRect{ bp.x, bp.y, bs.x, bs.y };
                ImGui::End();
            } else {
                emubar = SDL_FRect{ 0, 0, 0, 0 };
            }

            /*
             * The on-screen pad is for a machine with no keyboard.
             *
             * On a desktop there is a real one, and a translucent D-pad over a
             * Windows desktop is not a control scheme, it is something in the
             * way. On a phone it is the only way to play at all, so there it
             * still follows the player's own setting.
             *
             * Also under the keyboard and the menu: when either is up the
             * player is not driving the game.
             */
#if defined(__ANDROID__) || defined(__APPLE__)
            if (view == View::Emulator && !show_osk && !show_overlay &&
                ((active.onscreen_pad && gamepads.empty() && !show_controls) ||
                 tpad.editing()))
                tpad.draw(ImGui::GetForegroundDrawList(), ImVec2(0, 0),
                          ImVec2((float)win_w, (float)win_h));
#endif

            /* The press-and-hold ring. Not behind the same #if: a touchscreen
             * laptop or a Linux handheld has fingers too, and the ring only
             * ever appears while one is actually held. */
            if (view == View::Emulator && !show_osk && !show_overlay &&
                !show_controls)
                touch.draw();

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
#if !defined(__ANDROID__) && !defined(__APPLE__)
    /* Never leave the desktop without its pointer. */
    SDL_SetWindowRelativeMouseMode(win, false);
#endif
    retrodos::brand_shutdown();      /* before the renderer that owns its texture */
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

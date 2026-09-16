/*
 * retro-dosbox — bundled demo content.
 *
 * Extraction, rather than reading in place, is forced by the engine: DOSBox-X
 * mounts a host DIRECTORY, so the content has to exist as real files. On
 * Android the assets live compressed inside the APK; on iOS they are bundle
 * resources. SDL_IOFromFile reaches both -- on Android a relative path falls
 * through to the asset manager, and on iOS to the bundle -- which is why the
 * copy below is written against SDL's I/O rather than fopen.
 */
#include "retrodos_demo.h"
#include "retrodos_brand.h"

#include <SDL3/SDL.h>

#include <vector>

namespace retrodos {
namespace {

struct Asset {
    const char *name;      /* relative path in the package, and on disk */
};

/* DEMO.COM is ours; FREEDOS.IMG is FreeDOS 1.3's boot floppy, verbatim.
 * demo/NOTICE.md records where the image came from and under what terms. */
const Asset kAssets[] = {
    { "DEMO.COM"    },
    { "FREEDOS.IMG" },
};

/** Copy one packaged asset to [dest], unless an identical-sized copy is
 *  already there. Size is a sufficient check here: the content is immutable
 *  and only ever replaced by an app update, which replaces both sides. */
bool extract(const char *name, const std::string &dest)
{
    /* An ABSOLUTE path off Android.
     *
     * A relative path resolves against the process working directory, and the
     * emulator moves it: DOSBox-X's MOUNT chdir()s while validating the
     * directory it is given (dos_programs.cpp). So "demo/DEMO.COM" resolved to
     * the bundle on a fresh launch and to nothing at all once a game had been
     * run -- the Demo page worked until you used it, then reported that the
     * content could not be unpacked, with the files sitting in the bundle the
     * whole time.
     *
     * Android keeps the relative form on purpose: there is no filesystem path
     * for an APK asset, and a bare relative name is what SDL routes to the
     * asset manager. SDL_GetBasePath() returns NULL there anyway. */
    std::string src = std::string("demo/") + name;
#if !defined(__ANDROID__)
    if (const char *base = SDL_GetBasePath())
        src = std::string(base) + src;
#endif

    SDL_IOStream *in = SDL_IOFromFile(src.c_str(), "rb");
    if (!in) {
        SDL_Log("retrodos: bundled asset missing: %s (%s)", src.c_str(), SDL_GetError());
        return false;
    }
    const Sint64 size = SDL_GetIOSize(in);
    if (size <= 0) { SDL_CloseIO(in); return false; }

    SDL_PathInfo info;
    if (SDL_GetPathInfo(dest.c_str(), &info) &&
        info.type == SDL_PATHTYPE_FILE && info.size == (Uint64)size) {
        SDL_CloseIO(in);
        return true;                      /* already unpacked */
    }

    std::vector<unsigned char> buf((size_t)size);
    const size_t got = SDL_ReadIO(in, buf.data(), buf.size());
    SDL_CloseIO(in);
    if (got != buf.size()) {
        SDL_Log("retrodos: short read on %s", src.c_str());
        return false;
    }

    SDL_IOStream *out = SDL_IOFromFile(dest.c_str(), "wb");
    if (!out) {
        SDL_Log("retrodos: cannot write %s (%s)", dest.c_str(), SDL_GetError());
        return false;
    }
    const size_t put = SDL_WriteIO(out, buf.data(), buf.size());
    SDL_CloseIO(out);

    if (put != buf.size()) {
        /* A half-written image would boot to garbage, which is far more
         * confusing than no demo at all. Remove it. */
        SDL_RemovePath(dest.c_str());
        SDL_Log("retrodos: short write on %s", dest.c_str());
        return false;
    }
    return true;
}

} /* namespace */

bool demo_prepare(const std::string &dest_dir)
{
    if (dest_dir.empty()) return false;
    SDL_CreateDirectory(dest_dir.c_str());

    for (const Asset &a : kAssets)
        if (!extract(a.name, dest_dir + "/" + a.name)) return false;

    return true;
}

std::string demo_command(DemoKind kind, bool &run_raw)
{
    if (kind == DemoKind::FreeDos) {
        /* BOOT is a DOSBox-X command taking an image argument, so it must not
         * be quoted the way a program name is -- quoting it would have the
         * shell look for a file literally called "boot FREEDOS.IMG".
         *
         * -l A boots it as drive A:, which is what the image expects; booting
         * it as C: lands in a FreeDOS installer rather than a prompt. */
        run_raw = true;
        return "boot FREEDOS.IMG -l A";
    }
    run_raw = false;
    return "DEMO.COM";
}

std::string demo_title(DemoKind kind)
{
    return kind == DemoKind::FreeDos ? "FreeDOS 1.3 (DOS prompt)"
                                     : RETRODOS_APP_NAME " demo";
}

} /* namespace retrodos */

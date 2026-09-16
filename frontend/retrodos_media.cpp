/*
 * retro-dosbox — native side of the RetroMedia client.
 *
 * The bridge speaks a tab/newline delimited protocol rather than JSON:
 *
 *     OP <TAB> OK|ERR <TAB> message <LF>
 *     ...zero or more payload records, one per line, tab separated...
 *
 * That is not laziness about formats -- it keeps a JSON parser out of the
 * native build for a handful of flat record types, and every field that could
 * contain a tab or newline is scrubbed on the Kotlin side before it is sent.
 */
#include "retrodos_media.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace retrodos {
namespace {

/* ---------------------------------------------------------------------- */
/* Protocol                                                                */
/* ---------------------------------------------------------------------- */

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

MediaOp op_from(const std::string &s)
{
    if (s == "STATUS")    return MediaOp::Status;
    if (s == "LOGIN")     return MediaOp::Login;
    if (s == "LOGOUT")    return MediaOp::Logout;
    if (s == "CATALOGUE") return MediaOp::Catalogue;
    if (s == "ARTWORK")   return MediaOp::Artwork;
    if (s == "DOWNLOAD")  return MediaOp::Download;
    return MediaOp::None;
}

long long to_ll(const std::string &s) { return s.empty() ? 0 : atoll(s.c_str()); }
int       to_i (const std::string &s) { return s.empty() ? 0 : atoi(s.c_str()); }

/** Turn one wire message into a MediaResult. */
bool parse(const std::string &raw, MediaResult &out)
{
    if (raw.empty()) return false;

    const std::vector<std::string> lines = split(raw, '\n');
    const std::vector<std::string> head  = split(lines[0], '\t');
    if (head.size() < 2) return false;

    out = MediaResult();
    out.op      = op_from(head[0]);
    out.ok      = (head[1] == "OK");
    out.message = head.size() > 2 ? head[2] : std::string();

    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        const std::vector<std::string> f = split(lines[i], '\t');

        switch (out.op) {
        case MediaOp::Status:
        case MediaOp::Login:
            /* email, isAdmin, credits, freeRemaining. A STATUS with no record
             * is the signed-out case, which is a success, not an error. */
            if (f.size() >= 4) {
                out.account.signed_in      = true;
                out.account.email          = f[0];
                out.account.is_admin       = (f[1] == "1");
                out.account.credits        = to_i(f[2]);
                out.account.free_remaining = to_i(f[3]);
            }
            break;

        case MediaOp::Catalogue:
            if (f.size() >= 5) {
                MediaGame g;
                g.slug      = f[0];
                g.title     = f[1];
                g.rom_files = to_i(f[2]);
                g.bytes     = to_ll(f[3]);
                g.preview   = f[4];
                out.games.push_back(g);
            }
            break;

        case MediaOp::Artwork:
            if (f.size() >= 4) {
                out.art.slug = f[0];
                out.art.path = f[1];
                out.art.w    = to_i(f[2]);
                out.art.h    = to_i(f[3]);
            }
            break;

        case MediaOp::Download:
            out.path = f[0];
            break;

        default:
            break;
        }
    }
    return out.op != MediaOp::None;
}

} /* namespace */

/* ---------------------------------------------------------------------- */
/* Cached artwork                                                          */
/* ---------------------------------------------------------------------- */

bool media_read_art(const std::string &path, int &w, int &h,
                    std::vector<unsigned char> &rgba)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    unsigned char hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "RDA1", 4) != 0) {
        fclose(f);
        return false;
    }
    /* Big-endian, written by ByteBuffer on the other side. */
    w = (hdr[4]  << 24) | (hdr[5]  << 16) | (hdr[6]  << 8) | hdr[7];
    h = (hdr[8]  << 24) | (hdr[9]  << 16) | (hdr[10] << 8) | hdr[11];

    /* Bound the allocation: a corrupt or truncated cache file must not turn
     * into a multi-gigabyte malloc on a handheld. */
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) { fclose(f); return false; }

    rgba.resize((size_t)w * (size_t)h * 4);
    const size_t got = fread(rgba.data(), 1, rgba.size(), f);
    fclose(f);
    if (got != rgba.size()) { rgba.clear(); return false; }
    return true;
}

} /* namespace retrodos */

/* ====================================================================== */

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <jni.h>

namespace retrodos {
namespace {

jclass find_bridge(JNIEnv *env)
{
    jclass cls = env->FindClass("com/crownparkcomputing/retrodos/MediaBridge");
    if (!cls) { env->ExceptionClear(); SDL_Log("retrodos: MediaBridge not found"); }
    return cls;
}

std::string jstring_to_std(JNIEnv *env, jstring s)
{
    if (!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

/* Call a static method whose parameters are all strings. The signature is built
 * from the count, which covers every entry point the bridge exposes. */
void call_strings(const char *name, const char *const *args, int n,
                  const bool *trailing_bool = nullptr, bool bv = false)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return;
    jclass cls = find_bridge(env);
    if (!cls) return;

    std::string sig = "(";
    for (int i = 0; i < n; ++i) sig += "Ljava/lang/String;";
    if (trailing_bool) sig += "Z";
    sig += ")V";

    jmethodID m = env->GetStaticMethodID(cls, name, sig.c_str());
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return; }

    jvalue v[4];
    for (int i = 0; i < n; ++i) v[i].l = env->NewStringUTF(args[i]);
    if (trailing_bool) v[n].z = bv ? JNI_TRUE : JNI_FALSE;

    env->CallStaticVoidMethodA(cls, m, v);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }

    for (int i = 0; i < n; ++i) env->DeleteLocalRef(v[i].l);
    env->DeleteLocalRef(cls);
}

std::string call_string(const char *name)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return std::string();
    jclass cls = find_bridge(env);
    if (!cls) return std::string();

    jmethodID m = env->GetStaticMethodID(cls, name, "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return std::string(); }

    jstring js = (jstring)env->CallStaticObjectMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    std::string out = jstring_to_std(env, js);
    if (js) env->DeleteLocalRef(js);
    env->DeleteLocalRef(cls);
    return out;
}

} /* namespace */

bool media_available(void) { return true; }

/* Android only. An iOS build compiles the stubs below, where this is false. */
bool media_downloads_available(void) { return true; }

void media_begin_status(void) { call_strings("beginStatus", nullptr, 0); }
void media_begin_logout(void) { call_strings("beginLogout", nullptr, 0); }

void media_begin_login(const std::string &email, const std::string &password)
{
    const char *a[2] = { email.c_str(), password.c_str() };
    call_strings("beginLogin", a, 2);
}

void media_begin_login_key(const std::string &api_key)
{
    const char *a[1] = { api_key.c_str() };
    call_strings("beginLoginKey", a, 1);
}

void media_begin_catalogue(const std::string &search, const std::string &letter,
                           bool roms_only)
{
    const char *a[2] = { search.c_str(), letter.c_str() };
    const bool has_bool = true;
    call_strings("beginCatalogue", a, 2, &has_bool, roms_only);
}

void media_begin_artwork(const std::string &slug, const std::string &preview)
{
    const char *a[2] = { slug.c_str(), preview.c_str() };
    call_strings("beginArtwork", a, 2);
}

void media_begin_download(const std::string &slug, const std::string &dest_dir)
{
    const char *a[2] = { slug.c_str(), dest_dir.c_str() };
    call_strings("beginDownload", a, 2);
}

bool media_poll(MediaResult &out)
{
    const std::string raw = call_string("poll");
    if (raw.empty()) return false;
    return parse(raw, out);
}

std::string media_last_email(void) { return call_string("lastEmail"); }
std::string media_progress(void)   { return call_string("progress"); }

} /* namespace retrodos */

#elif !defined(RETRODOS_MEDIA_HTTP)

namespace retrodos {

/* No client on this platform: retrodos_media_http.cpp supplies one wherever
 * libcurl is available, and where nothing does, media_available() being false
 * keeps the pages hidden rather than showing controls that would silently do
 * nothing. */
bool media_available(void) { return false; }
bool media_downloads_available(void) { return false; }

void media_begin_status(void) {}
void media_begin_login(const std::string &, const std::string &) {}
void media_begin_login_key(const std::string &) {}
void media_begin_logout(void) {}
void media_begin_catalogue(const std::string &, const std::string &, bool) {}
void media_begin_artwork(const std::string &, const std::string &) {}
void media_begin_download(const std::string &, const std::string &) {}
bool media_poll(MediaResult &) { return false; }
std::string media_last_email(void) { return std::string(); }
std::string media_progress(void)   { return std::string(); }

} /* namespace retrodos */

#endif

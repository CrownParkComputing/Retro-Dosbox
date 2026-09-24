/*
 * retro-dosbox — the RetroMedia client for platforms that have libcurl.
 *
 * The Android build does this work in Kotlin (MediaBridge), because the
 * platform hands it TLS, an HTTP stack and an image decoder for free. Nothing
 * else has that, so media_available() has always been false off Android and
 * the Artwork and Downloads pages were simply absent from the desktop build --
 * which is why signing in to RetroMedia could not be tested anywhere except on
 * a phone.
 *
 * This is the same client against the same endpoints, in C++ over libcurl. It
 * is a straight port of MediaBridge.kt rather than a second design: where the
 * two disagree about a URL, a header or an error message, that is a bug here.
 *
 * Compiled only when RETRODOS_MEDIA_HTTP is defined, which the desktop build
 * script does and the Android and iOS builds do not -- so both of those keep
 * globbing the frontend sources with no build-file change and get an empty
 * translation unit.
 */
#if defined(RETRODOS_MEDIA_HTTP)

#include "retrodos_media.h"
#include "retrodos_brand.h"

/* minizip, which the DOSBox-X core already vendors and this binary already
 * links -- so unpacking a downloaded game needs no new dependency. */
#include "unzip.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace retrodos {
namespace {

const char *const kBase   = "https://media.crownparkcomputing.com";
const char *const kSystem = "dos";
const char *const kAgent  = RETRODOS_APP_NAME "/1.0 RetroMedia client";

const size_t kJsonCap = 8u  << 20;
const size_t kArtCap  = 32u << 20;

/* ---------------------------------------------------------------------- */
/* Just enough JSON                                                        */
/* ---------------------------------------------------------------------- */
/*
 * A reader, not a library. It parses into a small tagged node and offers the
 * three lookups this file actually performs -- a string, a number, a bool by
 * key, and an array of objects -- because the alternative was scanning for
 * substrings, and a title containing `"slug":` would have been enough to break
 * that in a way nobody would ever reproduce.
 */
struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj } type = Type::Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json *find(const std::string &key) const {
        if (type != Type::Obj) return nullptr;
        for (const auto &kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string s(const std::string &key) const {
        const Json *j = find(key);
        return (j && j->type == Type::Str) ? j->str : std::string();
    }
    bool flag(const std::string &key) const {
        const Json *j = find(key);
        if (!j) return false;
        if (j->type == Type::Bool) return j->b;
        if (j->type == Type::Num)  return j->num != 0.0;
        return false;
    }
    long long i(const std::string &key) const {
        const Json *j = find(key);
        return (j && j->type == Type::Num) ? (long long)j->num : 0;
    }
};

struct JsonParser {
    const char *p = nullptr;
    const char *end = nullptr;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool lit(const char *s) {
        const size_t n = strlen(s);
        if ((size_t)(end - p) < n || memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }

    bool str(std::string &out) {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p != '\\') { out.push_back(*p++); continue; }
            if (++p >= end) return false;
            const char c = *p++;
            switch (c) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                if (end - p < 4) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = p[k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                p += 4;
                /* Surrogate halves are passed through as the replacement
                 * character: this reads titles, not arbitrary text, and a
                 * half-formed pair is not worth a UTF-16 decoder. */
                if (cp >= 0xD800 && cp <= 0xDFFF) { out += "\xEF\xBF\xBD"; break; }
                if (cp < 0x80) out.push_back((char)cp);
                else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: out.push_back(c); break;
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }

    bool value(Json &out) {
        /* Depth is bounded because the input is remote: a few thousand open
         * brackets would otherwise be a stack overflow rather than a parse
         * error. */
        if (depth > 32) return false;
        ws();
        if (p >= end) return false;

        if (*p == '"') { out.type = Json::Type::Str; return str(out.str); }
        if (*p == '{') {
            ++p; ++depth;
            out.type = Json::Type::Obj;
            ws();
            if (p < end && *p == '}') { ++p; --depth; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                ++p;
                Json v;
                if (!value(v)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; --depth; return true; }
                return false;
            }
        }
        if (*p == '[') {
            ++p; ++depth;
            out.type = Json::Type::Arr;
            ws();
            if (p < end && *p == ']') { ++p; --depth; return true; }
            for (;;) {
                Json v;
                if (!value(v)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; --depth; return true; }
                return false;
            }
        }
        if (lit("true"))  { out.type = Json::Type::Bool; out.b = true;  return true; }
        if (lit("false")) { out.type = Json::Type::Bool; out.b = false; return true; }
        if (lit("null"))  { out.type = Json::Type::Null; return true; }

        const char *start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' ||
                           *p == 'E' || *p == '-' || *p == '+')) ++p;
        if (p == start) return false;
        out.type = Json::Type::Num;
        out.num = strtod(std::string(start, p).c_str(), nullptr);
        return true;
    }
};

Json parse_json(const std::string &text)
{
    Json root;
    JsonParser jp;
    jp.p = text.data();
    jp.end = text.data() + text.size();
    if (!jp.value(root)) return Json();
    return root;
}

std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)c);
                out += buf;
            } else out.push_back(c);
        }
    }
    return out;
}

/* ---------------------------------------------------------------------- */
/* Where the session lives                                                 */
/* ---------------------------------------------------------------------- */
/*
 * A file in the app's own config directory, mode 0600.
 *
 * Android encrypts this with a Keystore key; there is no equivalent on a
 * desktop that is worth the pretence, so it is written in the clear and said
 * so plainly. What it must never be is a build artefact or anything inside the
 * repository -- a session cookie and an API key are credentials, and they stay
 * in the user's own profile.
 *
 * The password is never stored on either platform.
 */
/*
 * Owned by the worker thread once it is running, with one exception: g_email
 * is read by media_last_email() to prefill the sign-in form, so it alone is
 * guarded. The session and the key are touched only by the worker and by the
 * one-time load that happens before it starts.
 */
std::string g_dir;
std::string g_session;      /* rm_session=... cookie pair                   */
std::string g_api_key;      /* rmk_...                                      */
std::mutex  g_email_m;
std::string g_email;

void set_email(const std::string &e)
{
    std::lock_guard<std::mutex> lock(g_email_m);
    g_email = e;
}
std::string get_email()
{
    std::lock_guard<std::mutex> lock(g_email_m);
    return g_email;
}

std::string config_dir()
{
    if (!g_dir.empty()) return g_dir;
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", RETRODOS_APP_NAME)) {
        g_dir = pref;
        SDL_free(pref);
    }
    return g_dir;
}

std::string creds_path() { return config_dir() + "retromedia.cred"; }

void creds_load()
{
    SDL_IOStream *in = SDL_IOFromFile(creds_path().c_str(), "rb");
    if (!in) return;
    const Sint64 size = SDL_GetIOSize(in);
    if (size <= 0 || size > (1 << 16)) { SDL_CloseIO(in); return; }
    std::string text((size_t)size, '\0');
    const size_t got = SDL_ReadIO(in, text.data(), text.size());
    SDL_CloseIO(in);
    if (got != text.size()) return;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if      (k == "session") g_session = v;
        else if (k == "apikey")  g_api_key = v;
        else if (k == "email")   set_email(v);
    }
}

void creds_save()
{
    const std::string path = creds_path();
    std::string text;
    const std::string email = get_email();
    if (!g_session.empty()) text += "session=" + g_session + "\n";
    if (!g_api_key.empty()) text += "apikey="  + g_api_key + "\n";
    if (!email.empty())     text += "email="   + email     + "\n";

    if (text.empty()) { SDL_RemovePath(path.c_str()); return; }

    /* Created with the mode rather than chmod'ed afterwards: a credential file
     * that exists world-readable for even an instant is a credential file that
     * leaked. */
#if !defined(_WIN32)
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); return; }
#else
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
#endif
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

void creds_clear()
{
    g_session.clear();
    g_api_key.clear();
    creds_save();
}

/* ---------------------------------------------------------------------- */
/* HTTP                                                                    */
/* ---------------------------------------------------------------------- */

struct Resp {
    long        code = 0;
    std::string body;
    std::string session;      /* rm_session pair from Set-Cookie, if any */
    std::string transport;    /* libcurl's own failure, when code is 0   */

    Json json() const { return parse_json(body); }
    std::string error() const {
        const Json j = json();
        const std::string e = j.s("error");
        if (!e.empty()) return e;
        if (!transport.empty()) return transport;
        char buf[32];
        snprintf(buf, sizeof buf, "HTTP %ld", code);
        return buf;
    }
};

struct Sink {
    std::string body;
    size_t      cap = 0;
    FILE       *file = nullptr;
};

struct Sink;

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Sink *sink = (Sink *)ud;
    const size_t n = size * nmemb;
    if (sink->file) return fwrite(ptr, 1, n, sink->file);
    /* Returning short is how libcurl is told to abort, and it is the only cap
     * that applies when the server declares no Content-Length. */
    if (sink->body.size() + n > sink->cap) return 0;
    sink->body.append(ptr, n);
    return n;
}

size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Resp *r = (Resp *)ud;
    const size_t n = size * nmemb;
    const std::string line(ptr, n);
    if (SDL_strncasecmp(line.c_str(), "set-cookie:", 11) != 0) return n;

    /* Only name=value is kept. Path, HttpOnly and Max-Age are instructions to
     * a browser, and a server will reject them echoed back on a request. */
    const size_t at = line.find("rm_session=");
    if (at == std::string::npos) return n;
    size_t stop = line.find(';', at);
    if (stop == std::string::npos) stop = line.find_first_of("\r\n", at);
    if (stop == std::string::npos) stop = line.size();
    r->session = line.substr(at, stop - at);
    return n;
}

/* Live progress of a download, sampled by the UI every frame rather than
 * queued: a 1 GB title is minutes of otherwise silent work, and the screen
 * wants the latest figure, not every figure. */
struct Progress {
    std::mutex  m;
    std::string text;
};
Progress g_progress;

void set_progress(const std::string &t)
{
    std::lock_guard<std::mutex> lock(g_progress.m);
    g_progress.text = t;
}

int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    const char *title = (const char *)ud;
    static curl_off_t last = 0;
    /* A new transfer starts from zero, so the mark has to go back with it --
     * without this the second download reports nothing until it passes the
     * size of the first. */
    if (dlnow < last) last = 0;
    /* Formatting a string per callback would cost more than the transfer; once
     * every 4 MB is plenty for something a person is watching. */
    if (dlnow != 0 && dlnow - last < (4 << 20)) return 0;
    last = dlnow;

    char buf[160];
    const double mb = (double)dlnow / (1024.0 * 1024.0);
    if (dltotal > 0)
        snprintf(buf, sizeof buf, "%s  %.0f / %.0f MB  (%d%%)", title ? title : "",
                 mb, (double)dltotal / (1024.0 * 1024.0),
                 (int)(dlnow * 100 / dltotal));
    else
        snprintf(buf, sizeof buf, "%s  %.0f MB", title ? title : "", mb);
    set_progress(buf);
    return 0;
}

struct Req {
    std::string path;             /* absolute when it starts with http      */
    const char *method = "GET";
    std::string body;
    const char *content_type = nullptr;
    bool   auth = true;
    size_t cap  = kJsonCap;

    /* When set, the body is streamed straight to this file instead of being
     * accumulated in memory -- a downloaded game is routinely larger than this
     * process should ever hold. */
    FILE       *sink_file = nullptr;
    const char *progress_title = nullptr;
    long        timeout_s = 60;
};

Resp http(const Req &req)
{
    Resp r;
    CURL *c = curl_easy_init();
    if (!c) { r.transport = "curl unavailable"; return r; }

    const bool absolute = req.path.compare(0, 4, "http") == 0;
    const std::string url = absolute ? req.path : (std::string(kBase) + req.path);

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    Sink sink;
    sink.cap  = req.cap;
    sink.file = req.sink_file;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r);
    /* Refuse a body larger than the caller expects before it is downloaded,
     * where the server declares one. The write callback is not a second
     * defence here -- it is the only one when there is no Content-Length --
     * so the cap is checked there too, below. */
    if (!req.sink_file)
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)req.cap);

    if (req.progress_title) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)req.progress_title);
    }

    struct curl_slist *headers = nullptr;
    if (req.content_type)
        headers = curl_slist_append(headers,
                    (std::string("Content-Type: ") + req.content_type).c_str());
    if (req.auth) {
        if (!g_api_key.empty())
            headers = curl_slist_append(headers,
                        ("Authorization: Bearer " + g_api_key).c_str());
        else if (!g_session.empty())
            curl_easy_setopt(c, CURLOPT_COOKIE, g_session.c_str());
    }
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

    if (strcmp(req.method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    }

    const CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.transport = curl_easy_strerror(rc);
    else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    r.body = std::move(sink.body);
    return r;
}

/* Percent-encode one path segment or query value. The catalogue's preview
 * paths contain spaces and parentheses and are split on '/' before this is
 * applied, because encoding the separators too would ask the server for one
 * long filename. */
std::string enc(const std::string &s)
{
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string enc_path(const std::string &path)
{
    std::string out;
    size_t pos = 0;
    for (;;) {
        const size_t slash = path.find('/', pos);
        const std::string seg = path.substr(pos, slash == std::string::npos
                                                 ? std::string::npos : slash - pos);
        out += enc(seg);
        if (slash == std::string::npos) break;
        out.push_back('/');
        pos = slash + 1;
    }
    return out;
}

/* ---------------------------------------------------------------------- */
/* The work queue                                                          */
/* ---------------------------------------------------------------------- */
/*
 * One worker thread. The frontend draws at 60fps and must never block on the
 * network, so every media_begin_*() posts a job and returns; the outcome
 * arrives through media_poll(), which the frame loop drains.
 *
 * Serial rather than a pool, deliberately: the jobs share a session cookie and
 * a credentials file, and a fetch of artwork for a whole library is hundreds of
 * requests that the server should not receive all at once.
 */
std::mutex                        g_m;
std::condition_variable           g_cv;
std::deque<std::function<void()>> g_jobs;
std::deque<MediaResult>           g_results;
std::thread                       g_worker;
std::atomic<bool>                 g_stop{false};
bool                              g_started = false;

void finish(MediaResult r)
{
    std::lock_guard<std::mutex> lock(g_m);
    g_results.push_back(std::move(r));
}

void worker_main()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(g_m);
            g_cv.wait(lock, [] { return g_stop.load() || !g_jobs.empty(); });
            if (g_stop.load() && g_jobs.empty()) return;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
    }
}

/*
 * A joinable std::thread left alive at static destruction calls terminate(),
 * so the worker is stopped on the way out. Queued jobs are dropped rather than
 * run: nothing is waiting for them, and an artwork sweep would otherwise hold
 * the process open for as long as the library is long.
 */
struct WorkerStopper {
    ~WorkerStopper() {
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_started) return;
            g_jobs.clear();
            g_stop.store(true);
        }
        g_cv.notify_all();
        if (g_worker.joinable()) g_worker.join();
        curl_global_cleanup();
    }
};
WorkerStopper g_stopper;

void submit(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_started) {
            g_started = true;
            curl_global_init(CURL_GLOBAL_DEFAULT);
            creds_load();
            g_worker = std::thread(worker_main);
        }
        g_jobs.push_back(std::move(job));
    }
    g_cv.notify_one();
}

/* ---------------------------------------------------------------------- */
/* Operations                                                              */
/* ---------------------------------------------------------------------- */

MediaAccount account_from(const Json &acct)
{
    MediaAccount a;
    a.signed_in = true;
    a.email     = acct.s("email");
    a.is_admin  = acct.flag("isAdmin");
    a.credits   = (int)acct.i("credits");
    a.free_remaining = (int)acct.i("freeRemainingToday");
    return a;
}

/* /api/me is the only thing that decides whether a stored credential is still
 * good, so status is also what login reports once it has one. */
MediaResult do_status(MediaOp as)
{
    MediaResult r;
    r.op = as;
    if (g_session.empty() && g_api_key.empty()) {
        r.ok = true;
        r.message = "signed out";
        return r;
    }

    Req q; q.path = "/api/me";
    const Resp resp = http(q);
    if (resp.code == 401 || resp.code == 403) {
        /* Expired or revoked. Drop it, or every later call fails the same way
         * and the account looks permanently broken. */
        creds_clear();
        r.message = "session expired -- please sign in again";
        return r;
    }
    if (resp.code != 200) { r.message = resp.error(); return r; }

    const Json j = resp.json();
    const Json *acct = j.find("account");
    if (!acct) { r.message = "the server sent no account"; return r; }
    r.ok = true;
    r.account = account_from(*acct);
    if (!r.account.email.empty() && r.account.email != get_email()) {
        set_email(r.account.email);
        creds_save();
    }
    return r;
}

MediaResult do_login(const std::string &email, const std::string &password)
{
    MediaResult r;
    r.op = MediaOp::Login;

    Req cq; cq.path = "/api/auth/config"; cq.auth = false;
    const Resp cfg = http(cq);
    if (cfg.code != 200) {
        r.message = "cannot reach RetroMedia (" + cfg.error() + ")";
        return r;
    }
    const Json cj = cfg.json();
    const Json *fb = cj.find("firebase");

    std::string cookie;
    if (fb && fb->flag("enabled")) {
        /*
         * The `providers` list is not a list of what is allowed.
         *
         * An earlier version read providers ["google"] as "this server takes
         * Google only" and refused a password sign-in before trying it. That
         * is wrong: the field names the social buttons the website offers, and
         * email and password work perfectly well alongside them. Refusing on
         * the strength of it locked people out of an account that was fine.
         *
         * So the attempt is made, and Firebase's own answer is what the user
         * is told.
         */
        const std::string key = fb->s("apiKey");
        if (key.empty()) { r.message = "server did not supply a Firebase key"; return r; }

        Req fr;
        fr.path = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key="
                + enc(key);
        fr.method = "POST";
        fr.auth = false;
        fr.content_type = "application/json; charset=utf-8";
        fr.body = "{\"email\":\"" + json_escape(email) + "\",\"password\":\""
                + json_escape(password) + "\",\"returnSecureToken\":true}";
        const Resp fres = http(fr);
        if (fres.code != 200) {
            const Json fj = fres.json();
            const Json *err = fj.find("error");
            const std::string m = err ? err->s("message") : fres.error();
            if (m == "INVALID_LOGIN_CREDENTIALS" || m == "INVALID_PASSWORD")
                r.message = "Wrong email or password";
            else if (m == "EMAIL_NOT_FOUND")
                r.message = "No account for that email";
            else
                /* An account that signs in with Google has no password to
                 * check against, and says so here rather than anywhere more
                 * helpful. The API key below is the route for one. */
                r.message = m;
            return r;
        }
        const std::string id_token = fres.json().s("idToken");
        if (id_token.empty()) { r.message = "no token returned"; return r; }

        Req ex;
        ex.path = "/api/auth/firebase";
        ex.method = "POST";
        ex.auth = false;
        ex.content_type = "application/json; charset=utf-8";
        ex.body = "{\"id_token\":\"" + json_escape(id_token) + "\"}";
        const Resp exr = http(ex);
        if (exr.code != 200) { r.message = exr.error(); return r; }
        if (exr.session.empty()) { r.message = "server returned no session"; return r; }
        cookie = exr.session;
    } else {
        Req lr;
        lr.path = "/api/auth/login";
        lr.method = "POST";
        lr.auth = false;
        lr.content_type = "application/json; charset=utf-8";
        lr.body = "{\"email\":\"" + json_escape(email) + "\",\"password\":\""
                + json_escape(password) + "\"}";
        const Resp lres = http(lr);
        if (lres.code != 200) { r.message = lres.error(); return r; }
        if (lres.session.empty()) { r.message = "server returned no session"; return r; }
        cookie = lres.session;
    }

    g_session = cookie;
    g_api_key.clear();
    set_email(email);
    creds_save();
    return do_status(MediaOp::Login);
}

MediaResult do_login_key(const std::string &raw)
{
    MediaResult r;
    r.op = MediaOp::Login;

    std::string key = raw;
    while (!key.empty() && isspace((unsigned char)key.front())) key.erase(key.begin());
    while (!key.empty() && isspace((unsigned char)key.back()))  key.pop_back();
    if (key.compare(0, 4, "rmk_") != 0) {
        r.message = "an API key starts with rmk_";
        return r;
    }

    /* Stored first, because /api/me is what validates it -- then rolled back if
     * the server rejects it, so a bad key is never left behind. */
    const std::string prev_session = g_session;
    const std::string prev_key     = g_api_key;
    g_api_key = key;
    g_session.clear();
    creds_save();

    Req q; q.path = "/api/me";
    const Resp resp = http(q);
    if (resp.code != 200) {
        g_session = prev_session;
        g_api_key = prev_key;
        creds_save();
        r.message = (resp.code == 401) ? "that API key was rejected" : resp.error();
        return r;
    }

    const Json j = resp.json();
    const Json *acct = j.find("account");
    if (!acct) { r.message = "the server sent no account"; return r; }
    r.ok = true;
    r.account = account_from(*acct);
    set_email(r.account.email);
    creds_save();
    return r;
}

MediaResult do_logout()
{
    Req q; q.path = "/api/auth/logout"; q.method = "POST";
    http(q);   /* best effort: the local credential goes either way */
    creds_clear();

    MediaResult r;
    r.op = MediaOp::Logout;
    r.ok = true;
    r.message = "signed out";
    return r;
}

MediaResult do_catalogue(const std::string &search, const std::string &letter,
                         bool roms_only)
{
    MediaResult r;
    r.op = MediaOp::Catalogue;

    int page = 1;
    long long total = 0;
    int seen = 0;
    const int limit = 200;

    while (page <= 20) {
        std::string path = std::string("/api/systems/") + kSystem + "/games?limit="
                         + std::to_string(limit) + "&page=" + std::to_string(page);
        if (!search.empty()) path += "&search=" + enc(search);
        if (!letter.empty()) path += "&letter=" + enc(letter);
        if (roms_only)       path += "&category=rom";

        Req q; q.path = path;
        const Resp resp = http(q);
        if (resp.code != 200) { r.message = resp.error(); return r; }

        const Json j = resp.json();
        total = j.i("total");
        const Json *games = j.find("games");
        if (!games || games->type != Json::Type::Arr || games->arr.empty()) break;

        for (const Json &g : games->arr) {
            MediaGame mg;
            mg.slug  = g.s("slug");
            mg.title = g.s("title");
            if (mg.title.empty()) mg.title = g.s("name");
            mg.preview = g.s("preview");
            mg.bytes   = g.i("totalBytes");
            if (const Json *avail = g.find("availability"))
                mg.rom_files = (int)avail->i("romFiles");
            r.games.push_back(std::move(mg));
        }
        seen += (int)games->arr.size();
        if (total > 0 && seen >= (int)total) break;
        ++page;
    }

    r.ok = true;
    r.message = std::to_string(seen) + " of " + std::to_string(total);
    return r;
}

/* A stable filename for a slug. FNV-1a rather than a hash library: this names
 * a cache file, it is not defending anything. */
std::string cache_name(const std::string &slug)
{
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : slug) { h ^= c; h *= 1099511628211ull; }
    char buf[32];
    snprintf(buf, sizeof buf, "%016llx.rda", h);
    return buf;
}

bool write_rda(const std::string &path, int w, int h,
               const std::vector<unsigned char> &rgba)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    unsigned char hdr[12] = { 'R', 'D', 'A', '1',
        (unsigned char)(w >> 24), (unsigned char)(w >> 16),
        (unsigned char)(w >> 8),  (unsigned char)w,
        (unsigned char)(h >> 24), (unsigned char)(h >> 16),
        (unsigned char)(h >> 8),  (unsigned char)h };
    const bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
                    fwrite(rgba.data(), 1, rgba.size(), f) == rgba.size();
    fclose(f);
    if (!ok) SDL_RemovePath(path.c_str());
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Downloading a game                                                      */
/* ---------------------------------------------------------------------- */

/* Anything that is a path, a control character, or illegal on the FAT volumes
 * these libraries usually live on. */
std::string sanitise(const std::string &in)
{
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) out.push_back('_');
        else out.push_back((char)c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    size_t b = 0;
    while (b < out.size() && out[b] == ' ') ++b;
    return out.substr(b);
}

std::string base_of(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool starts_with_zip_magic(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char m[4] = {0};
    const bool got = fread(m, 1, 4, f) == 4;
    fclose(f);
    return got && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4;
}

/*
 * Unpack one zip into [dir], flat.
 *
 * Flat on purpose: DOSBox-X mounts the game's folder as a drive and the
 * launcher looks for a .BAT/.COM/.EXE directly in it, so a title buried two
 * directories down would list as a game with nothing to run. Entry names are
 * reduced to their last component, which also disposes of the "../" escape a
 * crafted archive would otherwise use to write outside the folder.
 */
int unzip_into(const std::string &zip_path, const std::string &dir,
               const std::string &title)
{
    unzFile z = unzOpen64(zip_path.c_str());
    if (!z) return -1;

    int written = 0;
    std::vector<char> buf(256 * 1024);
    if (unzGoToFirstFile(z) == UNZ_OK) {
        do {
            unz_file_info64 info;
            char name[512] = {0};
            if (unzGetCurrentFileInfo64(z, &info, name, sizeof(name) - 1,
                                        nullptr, 0, nullptr, 0) != UNZ_OK)
                break;

            /*
             * Keep the archive's folders. Reducing every entry to its last
             * component (which this used to do) flattens the zip -- and a
             * game whose dosbox.conf mounts a subfolder ("cd \\game", "imgmount
             * d .\\cd\\disc.cue") then finds nothing there and cannot start.
             * Each path component is sanitised and "." / ".." are dropped, so
             * a crafted entry still cannot escape [dir]; the nesting is what
             * is preserved, not arbitrary paths.
             */
            std::string rel;
            {
                std::string comp;
                std::string raw = name;
                for (char &ch : raw) if (ch == '\\') ch = '/';
                size_t i = 0;
                while (i <= raw.size()) {
                    if (i == raw.size() || raw[i] == '/') {
                        const std::string c = sanitise(comp);
                        if (!c.empty() && c != "." && c != "..") {
                            if (!rel.empty()) rel += "/";
                            rel += c;
                        }
                        comp.clear();
                    } else comp.push_back(raw[i]);
                    ++i;
                }
            }
            if (rel.empty()) continue;             /* a directory entry */
            if (unzOpenCurrentFile(z) != UNZ_OK) continue;

            const std::string out_path = dir + "/" + rel;
            /* Make the parent folders the entry names. */
            for (size_t p2 = out_path.find('/', dir.size() + 1);
                 p2 != std::string::npos; p2 = out_path.find('/', p2 + 1))
                mkdir(out_path.substr(0, p2).c_str(), 0755);
            FILE *out = fopen(out_path.c_str(), "wb");
            if (out) {
                for (;;) {
                    const int n = unzReadCurrentFile(z, buf.data(), (unsigned)buf.size());
                    if (n <= 0) break;
                    if (fwrite(buf.data(), 1, (size_t)n, out) != (size_t)n) break;
                }
                fclose(out);
                ++written;
                set_progress(title + "  unpacking " + base_of(rel));
            }
            unzCloseCurrentFile(z);
        } while (unzGoToNextFile(z) == UNZ_OK);
    }
    unzClose(z);
    return written;
}

/* Every .zip sitting directly in [dir], unpacked and then removed.
 *
 * The catalogue ships games as archives, and an archive left as-is appears in
 * the library as a title with nothing to run. Top level only and not
 * recursive: a game that legitimately ships an archive as data keeps it. */
void extract_archives(const std::string &dir, const std::string &title)
{
    std::vector<std::string> zips;
    int n = 0;
    if (char **found = SDL_GlobDirectory(dir.c_str(), "*.zip",
                                         SDL_GLOB_CASEINSENSITIVE, &n)) {
        for (int i = 0; i < n && found[i]; ++i) {
            if (SDL_strchr(found[i], '/')) continue;
            zips.push_back(dir + "/" + found[i]);
        }
        SDL_free(found);
    }
    for (const std::string &z : zips) {
        if (unzip_into(z, dir, title) > 0) SDL_RemovePath(z.c_str());
    }
}

void remove_tree(const std::string &dir)
{
    int n = 0;
    if (char **found = SDL_GlobDirectory(dir.c_str(), "*", 0, &n)) {
        for (int i = 0; i < n && found[i]; ++i) {
            const std::string p = dir + "/" + found[i];
            SDL_PathInfo info;
            if (SDL_GetPathInfo(p.c_str(), &info) &&
                info.type == SDL_PATHTYPE_DIRECTORY)
                remove_tree(p);
            else
                SDL_RemovePath(p.c_str());
        }
        SDL_free(found);
    }
    SDL_RemovePath(dir.c_str());
}

MediaResult do_download(const std::string &slug, const std::string &dest_dir)
{
    MediaResult r;
    r.op = MediaOp::Download;

    {   /* The server enforces this too -- rom media is stripped from every
         * non-admin response -- but saying so here is the difference between
         * a clear refusal and a 403 the user has to interpret. */
        Req q; q.path = "/api/me";
        const Resp me = http(q);
        if (me.code != 200) { r.message = "sign in first"; return r; }
        const Json j = me.json();
        const Json *acct = j.find("account");
        if (!acct || !acct->flag("isAdmin")) {
            r.message = "downloading games needs an administrator account";
            return r;
        }
    }

    Req dq;
    dq.path = std::string("/api/systems/") + kSystem + "/games/" + enc(slug);
    const Resp d = http(dq);
    if (d.code != 200) { r.message = d.error(); return r; }
    const Json detail = d.json();

    const Json *roms = detail.find("roms");
    const size_t rom_count = (roms && roms->type == Json::Type::Arr) ? roms->arr.size() : 0;
    if (rom_count == 0) { r.message = "no downloadable files for this game"; return r; }

    std::string title = detail.s("title");
    if (title.empty()) title = detail.s("name");
    if (title.empty()) title = slug;
    title = sanitise(title);
    if (title.empty()) title = slug;

    /*
     * Downloaded somewhere the library does not look, and moved in when it is
     * finished.
     *
     * Building it in place put a folder in the library the moment the download
     * started, so a title that was still arriving -- or one whose download died
     * with the app -- was listed as a game, and starting it mounted an empty
     * directory and dropped the user at a C:\> prompt. The scanner skips names
     * beginning with a dot, so a staging folder under one is invisible until
     * there is something worth seeing.
     */
    const std::string stage_root = dest_dir + "/.downloading";
    const std::string out_dir    = stage_root + "/" + title;
    const std::string final_dir  = dest_dir + "/" + title;
    remove_tree(out_dir);              /* anything a previous attempt left */
    SDL_CreateDirectory(stage_root.c_str());
    SDL_CreateDirectory(out_dir.c_str());

    /* Straight to disk. The body is the zip the server builds, and a game is
     * routinely larger than anything this process should hold in memory. */
    const std::string tmp = out_dir + "/.download";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { r.message = "could not write to " + out_dir; return r; }

    Req zq;
    zq.path = std::string("/api/systems/") + kSystem + "/games/" + enc(slug)
            + "/zip?types=rom";
    zq.sink_file = f;
    zq.progress_title = title.c_str();
    zq.timeout_s = 3600;
    const Resp zr = http(zq);
    fclose(f);
    set_progress(std::string());

    if (zr.code != 200) {
        SDL_RemovePath(tmp.c_str());
        remove_tree(out_dir);
        /* 402 is the metering answer and deserves its own words: it is not a
         * failure of the app or the network. */
        r.message = (zr.code == 402) ? ("out of credits: " + zr.error()) : zr.error();
        return r;
    }

    int written = 0;
    if (starts_with_zip_magic(tmp)) {
        written = unzip_into(tmp, out_dir, title);
        SDL_RemovePath(tmp.c_str());
    } else {
        /* A single rom comes back as the file itself, with no name on it. The
         * catalogue entry is where the name lives. */
        std::string name;
        if (roms && !roms->arr.empty()) name = sanitise(base_of(roms->arr[0].s("file")));
        if (name.empty()) name = slug + ".zip";
        const std::string dst = out_dir + "/" + name;
        SDL_RemovePath(dst.c_str());
        written = SDL_RenamePath(tmp.c_str(), dst.c_str()) ? 1 : 0;
    }

    if (written <= 0) {
        remove_tree(out_dir);
        r.message = "nothing was written";
        return r;
    }

    /* An archive inside the archive: the catalogue ships plenty of them, and
     * one left packed is a title the launcher cannot start. */
    extract_archives(out_dir, title);
    set_progress(std::string());

    /* Only now does it become a game. Replacing an earlier copy is deliberate
     * -- re-downloading a title should give you the title, not fail because
     * you already had it -- and it happens after the new one is complete, so a
     * failed re-download never costs the copy that worked. */
    remove_tree(final_dir);
    if (!SDL_RenamePath(out_dir.c_str(), final_dir.c_str())) {
        r.message = std::string("could not move it into the library: ") + SDL_GetError();
        remove_tree(out_dir);
        return r;
    }

    r.ok = true;
    r.path = final_dir;
    r.message = title + ": " + std::to_string(written) +
                (written == 1 ? " file" : " files");
    return r;
}

MediaResult do_artwork(const std::string &slug, const std::string &preview)
{
    MediaResult r;
    r.op = MediaOp::Artwork;
    r.art.slug = slug;

    if (preview.empty()) { r.message = "no artwork for " + slug; return r; }

    const std::string dir = config_dir() + "media-art";
    SDL_CreateDirectory(dir.c_str());
    const std::string cache = dir + "/" + cache_name(slug);

    {   /* Already have it: the launcher asks for every title in the library
         * each time artwork is fetched, and re-downloading what is on disk
         * would make a second run as slow as the first. */
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        if (media_read_art(cache, w, h, rgba)) {
            r.ok = true;
            r.art.path = cache;
            r.art.w = w;
            r.art.h = h;
            return r;
        }
    }

    /* The per-file media route with ?size=480 is FREE and unmetered, unlike
     * /zip?types=, which costs a credit per game. For art across hundreds of
     * titles that is the difference between usable and unaffordable. */
    Req q;
    q.path = std::string("/api/systems/") + kSystem + "/games/" + enc(slug)
           + "/media/" + enc_path(preview) + "?size=480";
    q.cap = kArtCap;
    const Resp resp = http(q);
    if (resp.code != 200) { r.message = resp.error(); return r; }

    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!decode_image((const unsigned char *)resp.body.data(), resp.body.size(),
                      360, 500, w, h, rgba)) {
        r.message = "unreadable image for " + slug;
        return r;
    }
    if (!write_rda(cache, w, h, rgba)) { r.message = "could not cache " + slug; return r; }

    r.ok = true;
    r.art.path = cache;
    r.art.w = w;
    r.art.h = h;
    return r;
}

} /* namespace */

/* ---------------------------------------------------------------------- */
/* The interface                                                           */
/* ---------------------------------------------------------------------- */

bool media_available(void) { return true; }

/*
 * Downloads are offered here.
 *
 * This is a platform question, not a permission one: the account's isAdmin
 * flag decides whether the server will serve a game, and it is checked again
 * before every download. What this answers is whether the app should offer the
 * page at all -- and on a desktop it should. (On iOS it must not: the App
 * Store does not permit a storefront for the games themselves, which is why
 * this lives behind the same build switch as the rest of the client.)
 */
bool media_downloads_available(void) { return true; }

void media_begin_status(void)
{
    submit([] { finish(do_status(MediaOp::Status)); });
}

void media_begin_login(const std::string &email, const std::string &password)
{
    submit([email, password] { finish(do_login(email, password)); });
}

void media_begin_login_key(const std::string &api_key)
{
    submit([api_key] { finish(do_login_key(api_key)); });
}

void media_begin_logout(void)
{
    submit([] { finish(do_logout()); });
}

void media_begin_catalogue(const std::string &search, const std::string &letter,
                           bool roms_only)
{
    submit([search, letter, roms_only] {
        finish(do_catalogue(search, letter, roms_only));
    });
}

void media_begin_artwork(const std::string &slug, const std::string &preview)
{
    submit([slug, preview] { finish(do_artwork(slug, preview)); });
}

void media_begin_download(const std::string &slug, const std::string &dest_dir)
{
    submit([slug, dest_dir] { finish(do_download(slug, dest_dir)); });
}

bool media_poll(MediaResult &out)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (g_results.empty()) return false;
    out = std::move(g_results.front());
    g_results.pop_front();
    return true;
}

std::string media_last_email(void)
{
    {   /* The form can be drawn before anything has been asked of the network,
         * so the stored email may not have been read yet. */
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_started) creds_load();
    }
    return get_email();
}

std::string media_progress(void)
{
    std::lock_guard<std::mutex> lock(g_progress.m);
    return g_progress.text;
}

} /* namespace retrodos */

#endif /* RETRODOS_MEDIA_HTTP */

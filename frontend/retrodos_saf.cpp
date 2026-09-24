/*
 * retro-dosbox — native side of the Storage Access Framework bridge.
 *
 * Android will not grant broad file access to an app like this
 * (MANAGE_EXTERNAL_STORAGE is reserved for file managers), so a library kept
 * outside the app's own directories is reachable only as a document tree the
 * user explicitly picks. SAF then hands back `content://` URIs -- and DOSBox-X
 * mounts a DIRECTORY BY PATH. There is no `mount C content://...`.
 *
 * So the split is: ENUMERATE over SAF (enough to build the launcher list), and
 * STAGE only the game being launched into the app's own directory, which is a
 * real path. A DOS game is a few megabytes; nothing else is ever copied.
 *
 * On anything other than Android these are stubs, so the frontend can call
 * them unconditionally.
 */
#include "retrodos_saf.h"

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <jni.h>

namespace retrodos {
namespace {

/* Every call needs the class; caching the JNIEnv would be wrong, because it is
 * per-thread and the frontend runs on SDL's thread, not the UI thread. */
jclass find_bridge(JNIEnv *env)
{
    jclass cls = env->FindClass("com/crownparkcomputing/retrodos/SafBridge");
    if (!cls) {
        env->ExceptionClear();
        SDL_Log("retrodos: SafBridge class not found");
    }
    return cls;
}

bool call_void(const char *name)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return false;
    jclass cls = find_bridge(env);
    if (!cls) return false;
    jmethodID m = env->GetStaticMethodID(cls, name, "()V");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return false; }
    env->CallStaticVoidMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(cls);
    return true;
}

std::string jstring_to_std(JNIEnv *env, jstring s)
{
    if (!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

} /* namespace */

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

void saf_pick_folder(void)
{
    call_void("pick");
}

void        saf_pick_root(void)   { call_void("pickRoot"); }
void        saf_clear_root(void)  { call_void("clearRoot"); }
std::string saf_root_path(void)   { return call_string("rootPath"); }
std::string saf_root_label(void)  { return call_string("rootLabel"); }

bool saf_has_grant(void)
{
    return !saf_tree_uri().empty();
}

std::string saf_tree_uri(void)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return std::string();
    jclass cls = find_bridge(env);
    if (!cls) return std::string();

    jmethodID m = env->GetStaticMethodID(cls, "treeUri", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return std::string(); }

    jstring js = (jstring)env->CallStaticObjectMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    std::string out = jstring_to_std(env, js);
    if (js) env->DeleteLocalRef(js);
    env->DeleteLocalRef(cls);
    return out;
}

std::vector<std::string> saf_list_games(void)
{
    std::vector<std::string> out;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return out;
    jclass cls = find_bridge(env);
    if (!cls) return out;

    jmethodID m = env->GetStaticMethodID(cls, "listGames", "()[Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return out; }

    jobjectArray arr = (jobjectArray)env->CallStaticObjectMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cls); return out; }
    if (arr) {
        const jsize n = env->GetArrayLength(arr);
        out.reserve((size_t)n);
        for (jsize i = 0; i < n; ++i) {
            jstring js = (jstring)env->GetObjectArrayElement(arr, i);
            out.push_back(jstring_to_std(env, js));
            if (js) env->DeleteLocalRef(js);
        }
        env->DeleteLocalRef(arr);
    }
    env->DeleteLocalRef(cls);
    return out;
}

void saf_pick_game(const std::string &dest_root)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return;
    jclass cls = find_bridge(env);
    if (!cls) return;

    jmethodID m = env->GetStaticMethodID(cls, "pickGame", "(Ljava/lang/String;)V");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return; }

    jstring jd = env->NewStringUTF(dest_root.c_str());
    env->CallStaticVoidMethod(cls, m, jd);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(jd);
    env->DeleteLocalRef(cls);
}

std::string saf_install_status(void)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return std::string();
    jclass cls = find_bridge(env);
    if (!cls) return std::string();

    jmethodID m = env->GetStaticMethodID(cls, "installStatus", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return std::string(); }

    jstring js = (jstring)env->CallStaticObjectMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    std::string out = jstring_to_std(env, js);
    if (js) env->DeleteLocalRef(js);
    env->DeleteLocalRef(cls);
    return out;
}

bool saf_stage_game(const std::string &name, const std::string &dest_dir)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return false;
    jclass cls = find_bridge(env);
    if (!cls) return false;

    jmethodID m = env->GetStaticMethodID(
        cls, "stage", "(Ljava/lang/String;Ljava/lang/String;)Z");
    if (!m) { env->ExceptionClear(); env->DeleteLocalRef(cls); return false; }

    jstring jn = env->NewStringUTF(name.c_str());
    jstring jd = env->NewStringUTF(dest_dir.c_str());
    jboolean ok = env->CallStaticBooleanMethod(cls, m, jn, jd);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); ok = JNI_FALSE; }
    env->DeleteLocalRef(jn);
    env->DeleteLocalRef(jd);
    env->DeleteLocalRef(cls);
    return ok == JNI_TRUE;
}

bool android_restart_app(void)
{
    /* The Java side finishes the activity and exits the process, so on
     * success this call never really "returns" for long -- the caller should
     * simply stop doing work. */
    return call_void("restart");
}

} /* namespace retrodos */

#else /* !__ANDROID__ */

namespace retrodos {
void saf_pick_folder(void) {}
void saf_pick_root(void) {}
void saf_clear_root(void) {}
std::string saf_root_path(void) { return std::string(); }
std::string saf_root_label(void) { return std::string(); }
bool saf_has_grant(void) { return false; }
std::string saf_tree_uri(void) { return std::string(); }
std::vector<std::string> saf_list_games(void) { return {}; }
bool saf_stage_game(const std::string &, const std::string &) { return false; }
void saf_pick_game(const std::string &) {}
std::string saf_install_status(void) { return std::string(); }
bool android_restart_app(void) { return false; }
} /* namespace retrodos */

#endif

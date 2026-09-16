#include "retrodos_brand.h"

#include "imgui.h"

#define STB_IMAGE_IMPLEMENTATION
/* Only PNG, and no stdio. The wordmark is a PNG, the other decoders are code
 * we would ship and never run, and stb's file helpers would bypass SDL's asset
 * routing -- which is the one thing this file exists to get right. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cstring>

namespace retrodos {

namespace {

SDL_Texture *g_wordmark = nullptr;
int g_wordmark_w = 0, g_wordmark_h = 0;
bool g_wordmark_tried = false;

} /* namespace */

std::vector<unsigned char> read_asset(const std::string &name)
{
    /*
     * The Android case is the reason this is not one line.
     *
     * An APK asset has no filesystem path, and SDL routes a BARE RELATIVE name
     * to the asset manager. Everywhere else the assets sit beside the
     * executable (or inside the bundle on iOS), so they need SDL_GetBasePath
     * in front -- and on a desktop the process's working directory is wherever
     * the user happened to be, so a relative path there finds nothing.
     */
    std::string path = "assets/" + name;
#if !defined(__ANDROID__)
    if (const char *base = SDL_GetBasePath()) path = std::string(base) + path;
#endif

    SDL_IOStream *in = SDL_IOFromFile(path.c_str(), "rb");
    if (!in) {
        SDL_Log("retrodos: no bundled asset %s (%s)", path.c_str(), SDL_GetError());
        return {};
    }
    const Sint64 size = SDL_GetIOSize(in);
    if (size <= 0 || size > (32 << 20)) { SDL_CloseIO(in); return {}; }

    std::vector<unsigned char> bytes((size_t)size);
    const size_t got = SDL_ReadIO(in, bytes.data(), bytes.size());
    SDL_CloseIO(in);
    if (got != bytes.size()) {
        SDL_Log("retrodos: short read on %s", path.c_str());
        return {};
    }
    return bytes;
}

ImFont *load_ui_font(float size_px)
{
    std::vector<unsigned char> ttf = read_asset("ui/ui-font.ttf");
    if (ttf.empty()) return nullptr;

    ImFontConfig cfg;
    cfg.SizePixels = size_px;
    /*
     * ImGui takes ownership of this buffer and frees it when the atlas is
     * destroyed, so it must be a raw allocation that outlives this function
     * -- handing it the vector's storage would be a double free the moment
     * the vector went out of scope. Deliberate leak of exactly one buffer for
     * the lifetime of the process, which is what the atlas wants.
     */
    void *owned = IM_ALLOC(ttf.size());
    if (!owned) return nullptr;
    std::memcpy(owned, ttf.data(), ttf.size());

    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        owned, (int)ttf.size(), size_px, &cfg);
}

SDL_Texture *wordmark(SDL_Renderer *renderer, int *out_w, int *out_h)
{
    if (!g_wordmark && !g_wordmark_tried) {
        /* Tried once. A missing asset is a permanent condition, and retrying
         * every frame would log the same failure sixty times a second. */
        g_wordmark_tried = true;

        std::vector<unsigned char> png = read_asset("ui/wordmark.png");
        if (!png.empty()) {
            int w = 0, h = 0, comp = 0;
            stbi_uc *rgba = stbi_load_from_memory(png.data(), (int)png.size(),
                                                  &w, &h, &comp, 4);
            if (rgba) {
                g_wordmark = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STATIC, w, h);
                if (g_wordmark) {
                    SDL_UpdateTexture(g_wordmark, nullptr, rgba, w * 4);
                    /* The wordmark is drawn smaller than it was authored, so
                     * it is filtered rather than nearest -- the opposite of
                     * the emulator's framebuffer, which is hard pixels. */
                    SDL_SetTextureScaleMode(g_wordmark, SDL_SCALEMODE_LINEAR);
                    SDL_SetTextureBlendMode(g_wordmark, SDL_BLENDMODE_BLEND);
                    g_wordmark_w = w;
                    g_wordmark_h = h;
                }
                stbi_image_free(rgba);
            } else {
                SDL_Log("retrodos: wordmark did not decode: %s", stbi_failure_reason());
            }
        }
    }
    if (out_w) *out_w = g_wordmark_w;
    if (out_h) *out_h = g_wordmark_h;
    return g_wordmark;
}

void brand_shutdown(void)
{
    if (g_wordmark) {
        SDL_DestroyTexture(g_wordmark);
        g_wordmark = nullptr;
    }
    g_wordmark_w = g_wordmark_h = 0;
    g_wordmark_tried = false;
}

} /* namespace retrodos */

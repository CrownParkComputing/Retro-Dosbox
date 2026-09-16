/*
 * retro-dos — the bundled interface assets: the UI font and the wordmark.
 *
 * Both are read the same way, and it is the way retrodos_demo.cpp already
 * reads its content: SDL_IOFromFile with a path that is RELATIVE on Android,
 * because a bare relative name is what SDL routes to the APK asset manager,
 * and absolute via SDL_GetBasePath everywhere else. That one detail is why
 * this is a module rather than two lines inline -- get it wrong and the app
 * works on the desktop and silently loses both on a phone.
 *
 * Neither is required. A missing font falls back to ImGui's built-in bitmap
 * face and a missing wordmark falls back to drawing the app's name as text:
 * an asset that failed to package should make the app look plainer, never
 * stop it starting.
 */
#ifndef RETRODOS_BRAND_H
#define RETRODOS_BRAND_H

#include <SDL3/SDL.h>

#include <string>
#include <vector>

struct ImFont;

namespace retrodos {

/*
 * Reads a bundled asset into memory. Empty on failure, which every caller
 * here treats as "do without it".
 */
std::vector<unsigned char> read_asset(const std::string &name);

/*
 * Installs the bundled UI font at [size_px], replacing whatever ImGui has.
 *
 * Returns the font, or nullptr when the asset is missing -- in which case the
 * caller must still add ImGui's default, because clearing the atlas and then
 * building nothing leaves no font at all and every subsequent draw asserts.
 *
 * ImGui takes ownership of the memory it is given, so the bytes are handed
 * over with FontDataOwnedByAtlas left true and deliberately not freed here.
 */
ImFont *load_ui_font(float size_px);

/*
 * The wordmark, as a texture ready to draw. NULL when the asset is missing or
 * cannot be decoded. Cached: call it every frame.
 *
 * Owned by this module and released by brand_shutdown(); the renderer must
 * still exist at that point.
 */
SDL_Texture *wordmark(SDL_Renderer *renderer, int *out_w, int *out_h);

void brand_shutdown(void);

} /* namespace retrodos */

#endif /* RETRODOS_BRAND_H */

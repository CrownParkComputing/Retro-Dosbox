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

/*
 * What this app is called, and what it is built on.
 *
 * Android and the desktop ship as Retro-DOS. iOS ships as a separate app with
 * its own name, because Apple rejected the Retro-* family under guideline 4.3
 * for being too alike -- so the iOS build is not a rename of this one, it is
 * its own thing, and it says so.
 *
 * One define rather than a dozen string literals: a second identity that only
 * half-applies is worse than none, and a window title that still says
 * Retro-DOS is exactly the kind of detail App Review reads as the same app
 * submitted twice.
 *
 * RETRODOS_APP_CORE is not decoration. Whatever the app is called, the
 * emulator inside it is DOSBox-X and the credit for it is not optional --
 * legally, because of the GPL, and plainly, because it is theirs.
 */
#ifndef RETRODOS_APP_NAME
#  define RETRODOS_APP_NAME "Retro-DOS"
#endif
#ifndef RETRODOS_APP_CORE
#  define RETRODOS_APP_CORE "DOSBox-X"
#endif

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

/*
 * Decode a PNG or JPEG into RGBA8, shrunk to fit within [max_w] x [max_h]
 * with its aspect kept. False when the bytes are not an image this build
 * understands.
 *
 * It lives here because stb_image can only be instantiated once in a binary,
 * and this is the file that instantiates it. The RetroMedia client is the
 * other caller: box art arrives as PNG or JPEG and has to reach the frontend
 * as pixels.
 */
bool decode_image(const unsigned char *data, size_t size,
                  int max_w, int max_h, int &w, int &h,
                  std::vector<unsigned char> &rgba);

void brand_shutdown(void);

} /* namespace retrodos */

#endif /* RETRODOS_BRAND_H */

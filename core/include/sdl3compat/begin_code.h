/* SDL1/2 spelling of SDL3/SDL_begin_code.h.
 *
 * DECLSPEC is defined here rather than in our SDL.h shim on purpose: the
 * vendored SDL2-era headers (include/SDL_ttf.h, libs/decoders/SDL_sound.h)
 * include "begin_code.h" and then use DECLSPEC in every prototype, without
 * necessarily having gone through our SDL.h first. Defining it here is
 * ordering-correct by construction.
 *
 * SDL3 namespaced the macro to SDL_DECLSPEC. SNDDECLSPEC is defined in terms
 * of DECLSPEC, so this covers SDL_sound.h too -- together ~85% of the initial
 * SDL3 build errors in this tree.
 */
#include <SDL3/SDL_begin_code.h>

#ifndef DECLSPEC
#define DECLSPEC SDL_DECLSPEC
#endif

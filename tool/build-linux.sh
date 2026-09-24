#!/usr/bin/env bash
# Build Retro-DOS for a Linux desktop: the same SDL3 + Dear ImGui frontend the
# phone runs, over the same DOSBox-X core, as one native executable.
#
#   ./tool/build-linux.sh              # -> ~/.cache/retro-dosbox-linux/retrodos
#   OUT=/somewhere ./tool/build-linux.sh
#
# Shaped after Retro-PSX's tool/build-linux.sh and this repo's own
# android/build-core.sh. The Android script cross-compiles and links a shared
# library for SDLActivity to dlopen; here the frontend owns main() and the
# engine is linked straight into it, so there is one file to run and nothing
# to install. Needs: sdl3 (pkg-config), autotools, g++, and optionally
# libcurl + minizip for the RetroMedia client.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"
ROOT="$APP/core"
OUT="${OUT:-$HOME/.cache/retro-dosbox-linux}"
TREE="$OUT/tree"
JOBS="${JOBS:-$(nproc)}"
CXX="${CXX:-g++}"

[ -f "$ROOT/configure.ac" ] || {
    echo "error: core/ is empty. Run: git submodule update --init" >&2; exit 1; }
pkg-config --exists sdl3 || { echo "error: sdl3 not found by pkg-config" >&2; exit 1; }
echo "==> SDL3 $(pkg-config --modversion sdl3)"
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# 1. The engine, in a private tree
# ---------------------------------------------------------------------------
# Tracked files only, at their working-tree content, synced EVERY run: a plain
# copy drags build products along, and a one-off copy silently stops picking
# up edits. tar keeps mtimes, so make rebuilds only what changed. (Same
# reasoning, at length, in android/build-core.sh.)
echo "==> syncing the engine tree"
mkdir -p "$TREE"
git -C "$ROOT" ls-files -z | tar -C "$ROOT" --null -T - -cf - | tar -C "$TREE" -xf -

cd "$TREE"
[ -f configure ] || ./autogen.sh
if [ ! -f config.h ]; then
    echo "==> configure"
    # RETRODOS_HOST_PROVIDES_MAIN: the frontend has main(); the engine's
    # sdlmain.cpp keeps its own out of the link when this is set.
    ./configure \
        --enable-sdl3 \
        --disable-opengl --disable-printer --disable-sdlnet \
        --disable-freetype --disable-libfluidsynth \
        --disable-alsa-midi --disable-avcodec \
        --disable-libslirp \
        --disable-screenshots \
        CFLAGS="-fPIC -O2 -g0" \
        CXXFLAGS="-fPIC -O2 -g0 -DRETRODOS_HOST_PROVIDES_MAIN" \
        || { echo "configure failed; see $TREE/config.log" >&2; exit 1; }
fi
grep -q '^#define C_SDL3 1' config.h || { echo "error: SDL3 not selected" >&2; exit 1; }

# -k: the tree's own `all` ends by linking the dosbox-x executable, which has
# no main() under RETRODOS_HOST_PROVIDES_MAIN and is not wanted anyway. The
# archives are what matter, and the check below is what decides success.
echo "==> building the engine ($JOBS jobs)"
make -k -j"$JOBS" > "$OUT/engine-build.log" 2>&1 || true
[ -f src/gui/libgui.a ] || {
    echo "error: engine archives were not built; see $OUT/engine-build.log" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 2. The frontend
# ---------------------------------------------------------------------------
echo "==> building the frontend"
FE="$OUT/frontend"; mkdir -p "$FE"
CXXFLAGS="-std=gnu++17 -O2 -g0 -fPIC -Wall -Wno-unused-parameter"
MEDIA_CFLAGS=""; MEDIA_LIBS=""
if pkg-config --exists libcurl minizip; then
    CXXFLAGS="$CXXFLAGS -DRETRODOS_MEDIA_HTTP=1"
    MEDIA_CFLAGS="$(pkg-config --cflags libcurl minizip)"
    MEDIA_LIBS="$(pkg-config --libs libcurl minizip)"
    echo "==> RetroMedia: libcurl $(pkg-config --modversion libcurl), minizip $(pkg-config --modversion minizip)"
else
    echo "==> RetroMedia: off (libcurl/minizip not found)"
fi
INCS="-I$ROOT/include -I$APP/frontend/imgui $(pkg-config --cflags sdl3) $MEDIA_CFLAGS"

OBJS=""
for src in "$APP"/frontend/*.cpp "$APP"/frontend/imgui/*.cpp; do
    obj="$FE/$(basename "${src%.cpp}").o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] \
       || [ -n "$(find "$APP/frontend" -name '*.h' -newer "$obj" | head -1)" ]; then
        # shellcheck disable=SC2086
        "$CXX" $CXXFLAGS $INCS -c -o "$obj" "$src" \
            || { echo "error: frontend compile failed on $src" >&2; exit 1; }
    fi
    OBJS="$OBJS $obj"
done

# ---------------------------------------------------------------------------
# 3. Link
# ---------------------------------------------------------------------------
# The engine is pulled in whole, for the reason android/build-core.sh gives:
# nothing in the host API references most of it directly, so without
# --whole-archive the linker would throw the emulator away.
echo "==> linking retrodos"
LDADD_RAW="$(make -C src -s --eval='__print_ldadd:; @echo $(dosbox_x_LDADD)' __print_ldadd)"
[ -n "$LDADD_RAW" ] || { echo "error: could not read dosbox_x_LDADD" >&2; exit 1; }
LIBS_LINE="$(sed -n 's/^LIBS = //p' src/Makefile | head -1)"
ARCHIVES=""; EXTRA=""; seen=" "
for a in $LDADD_RAW; do
    case "$a" in
        *.a) case "$seen" in *" $a "*) continue ;; esac
             seen="$seen$a "; ARCHIVES="$ARCHIVES $TREE/src/$a" ;;
        -l*|-L*) EXTRA="$EXTRA $a" ;;
    esac
done
# shellcheck disable=SC2086
"$CXX" -o "$OUT/retrodos" $OBJS \
    -Wl,--whole-archive $ARCHIVES "$TREE"/src/*.o -Wl,--no-whole-archive \
    $(pkg-config --libs sdl3) $LIBS_LINE $EXTRA $MEDIA_LIBS \
    -lz -lm -ldl -lpthread

# ---------------------------------------------------------------------------
# 4. What the binary reads beside itself (SDL_GetBasePath)
# ---------------------------------------------------------------------------
# assets/ui for the font and wordmark; demo/ for the bundled DOS program and
# the FreeDOS floppy. The floppy's canonical copy lives with the Android
# assets, which is where iOS takes it from too.
mkdir -p "$OUT/assets/ui" "$OUT/demo"
cp -f "$APP"/assets/ui/* "$OUT/assets/ui/"
cp -f "$APP"/android/app/src/main/assets/demo/* "$OUT/demo/"

echo
echo "built: $OUT/retrodos"
ls -lh "$OUT/retrodos"

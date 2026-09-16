#!/usr/bin/env bash
# Build the DOS core for Android: libretrodos.so, exposing the plain-C host
# API in include/retrodos_host.h.
#
#   ANDROID_ABI=arm64-v8a ./android/build-core.sh
#   ANDROID_ABI=x86_64    ./android/build-core.sh    # emulator
#
# Shaped after retro-x86's core-as-shared-library: the emulator becomes a .so
# with one plain-C door, and every host (SDL3+ImGui app, test harness) is a
# dlopen client of that door.
#
# Two things this deliberately does NOT do, both of which the previous
# Retro-Dosbox Android build did and paid for:
#
#  1. It does not borrow prebuilt SDL/libpng out of a retired app's source
#     tree. That made the build unreproducible on any other machine and
#     impossible in CI. SDL3 is built from source here.
#  2. It does not share a configured tree with the host build. Autotools trees
#     are single-config: an Android ./configure over a host tree leaves stale
#     objects that link with "incompatible with aarch64linux", or silently
#     compiles host sources with the NDK compiler.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"          # this repository: the application
ROOT="$APP/core"                       # the DOSBox-X SDL3 fork, as a submodule

# The core is a submodule, so a fresh clone has an empty directory here until
# it is initialised. Say so plainly rather than failing later with a confusing
# "no such file" from the compiler.
if [ ! -f "$ROOT/include/retrodos_host.h" ]; then
    echo "error: the core is missing at $ROOT" >&2
    echo "       run: git submodule update --init --recursive" >&2
    exit 1
fi

ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_API="${ANDROID_API:-28}"   # bionic gained iconv_open at 28; DOSBox-X needs it
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.14904198}"
SDL3_TAG="${SDL3_TAG:-release-3.2.20}"
JOBS="${JOBS:-$(nproc)}"

OUT="$HERE/build/$ANDROID_ABI"
SDL3_SRC="$HERE/build/SDL"
SDL3_PREFIX="$OUT/sdl3"
TREE="$HERE/build/tree-$ANDROID_ABI"   # its own configured tree, never shared

case "$ANDROID_ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    x86_64)      TRIPLE=x86_64-linux-android ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
    *) echo "error: unsupported ANDROID_ABI '$ANDROID_ABI'" >&2; exit 1 ;;
esac

[ -d "$ANDROID_NDK" ] || { echo "error: no NDK at $ANDROID_NDK" >&2; exit 1; }

TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
export CC="$TOOLCHAIN/bin/${TRIPLE}${ANDROID_API}-clang"
export CXX="$TOOLCHAIN/bin/${TRIPLE}${ANDROID_API}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"

[ -x "$CC" ] || { echo "error: no compiler at $CC" >&2; exit 1; }

mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# 1. SDL3 for Android
# ---------------------------------------------------------------------------
if [ ! -f "$SDL3_PREFIX/lib/libSDL3.so" ]; then
    echo "==> SDL3 ($SDL3_TAG) for $ANDROID_ABI"
    [ -d "$SDL3_SRC" ] || git clone --depth 1 --branch "$SDL3_TAG" \
        https://github.com/libsdl-org/SDL.git "$SDL3_SRC"
    cmake -S "$SDL3_SRC" -B "$OUT/sdl3-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-$ANDROID_API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL3_PREFIX" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF
    cmake --build "$OUT/sdl3-build" -j"$JOBS"
    cmake --install "$OUT/sdl3-build"
fi
echo "==> SDL3: $SDL3_PREFIX/lib/libSDL3.so"

# SDL3 has no sdl3-config; the tree finds it through pkg-config.
export PKG_CONFIG_PATH="$SDL3_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$SDL3_PREFIX/lib/pkgconfig"

# ---------------------------------------------------------------------------
# 2. DOSBox-X tree, configured for Android
# ---------------------------------------------------------------------------
# Copy the TRACKED files only, at their current working-tree content. A
# plain recursive copy also drags in the host build's config.h, Makefiles
# and .o files -- and then the `[ ! -f config.h ]` guard below skips
# configure entirely, so the Android compiler happily rebuilds everything
# against the HOST configuration. That fails at the very end with
#   ld.lld: error: unable to find library -lslirp
# which reads like a missing Android dependency and is nothing of the kind.
# Build artifacts are untracked, so listing tracked files excludes them
# while still picking up uncommitted source edits.
#
# EVERY run, not only the first. The tree used to be staged once and left
# alone, which meant an edit to core/src after the first build was silently
# never compiled: make saw an old copy, said "Nothing to be done", and the
# script went on to package a library that did not contain the change. tar
# preserves source mtimes, so unchanged files stay old and make still
# rebuilds only what actually changed.
echo "==> syncing the private tree for $ANDROID_ABI"
mkdir -p "$TREE"
git -C "$ROOT" ls-files -z \
    | tar -C "$ROOT" --null -T - -cf - \
    | tar -C "$TREE" -xf -

cd "$TREE"
[ -f configure ] || ./autogen.sh

if [ ! -f config.h ]; then
    echo "==> configure for $TRIPLE (API $ANDROID_API)"
    # -fPIC everywhere: these objects go into a shared library.
    # Feature set matches the known-good SDL3 config; TTF and the OpenGL
    # output backend are not ported to SDL3 yet.
    ./configure \
        --host="$TRIPLE" \
        --enable-sdl3 \
        --disable-opengl --disable-printer --disable-sdlnet \
        --disable-freetype --disable-libfluidsynth \
        --disable-alsa-midi --disable-avcodec \
        --disable-libslirp \
        --disable-screenshots \
        CFLAGS="-fPIC -O2 -g0 -DANDROID" \
        CXXFLAGS="-fPIC -O2 -g0 -DANDROID" \
        || { echo "configure failed; see $TREE/config.log" >&2; exit 1; }
fi

grep -q '^#define C_SDL3 1' config.h || { echo "error: SDL3 not selected" >&2; exit 1; }

# Bionic folds librt and iconv into libc, so there are no separate -lrt/-liconv
# to link against. configure adds them for Linux hosts regardless, and the
# result is a link that dies with "unable to find library -lrt" AFTER every
# object has compiled successfully.
echo "==> dropping -lrt/-liconv (bionic has both in libc)"
find . -name Makefile -exec sed -i 's/ -lrt\b//g; s/ -liconv\b//g' {} +

# The engine's own `all` target ends by linking the dosbox-x EXECUTABLE, which
# is meaningless on Android (no main, and we want a library). -k lets the
# archives finish; the shared library is linked from them below, and the ABI
# check at the end is what actually decides whether this build succeeded.
echo "==> building the engine ($JOBS jobs)"
make -k -j"$JOBS" || true

for a in $(cd src && ls */lib*.a */*/lib*.a 2>/dev/null); do :; done
[ -f src/gui/libgui.a ] || { echo "error: engine archives were not built" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3. Link the engine into one shared library
# ---------------------------------------------------------------------------
# The engine is pulled in WHOLE: nothing in the host API references most of
# DOSBox-X directly -- the mainloop pulls it in at runtime -- so without
# --whole-archive the linker would discard almost the entire emulator.
#
# The archive list comes from make expanding dosbox_x_LDADD, not from a glob:
# a glob over src/*/lib*.a silently misses the nested ones (libs/gui_tk,
# hardware/mame, hardware/reSID) and the failure only appears at dlopen as an
# undefined symbol. LDADD also repeats libgui.a, which is harmless normally
# but is a duplicate-symbol error under --whole-archive, so it is deduped.
# -lz because the engine uses zlib (zipfile, ZMBV) and configure only adds it
# when its own probe finds it, which a cross build may not.
#
# -Wl,--no-undefined is the important one. A shared library links happily with
# unresolved symbols by default, so a missing -l shows up on the DEVICE as
#   dlopen failed: cannot locate symbol "inflateInit2_"
# and the app dies instantly with no crash and no stack. Failing the LINK
# instead turns a device-only mystery into a build error naming the symbol.
# ---------------------------------------------------------------------------
# 2b. The SDL3 + ImGui frontend
# ---------------------------------------------------------------------------
# The frontend owns the window; the engine runs headless behind it through the
# Game Link output. It provides main(), which <SDL3/SDL_main.h> renames to
# SDL_main -- that is the symbol SDLActivity looks up, and it does not clash
# with the engine's own main().
echo "==> building the SDL3 + ImGui frontend"
FE_OUT="$OUT/frontend"
mkdir -p "$FE_OUT"
FE_OBJS=""
FE_FLAGS="-fPIC -O2 -g0 -DANDROID -std=gnu++17
          -I$ROOT/include -I$APP/frontend/imgui
          -I$SDL3_PREFIX/include"
for src in "$APP"/frontend/*.cpp "$APP"/frontend/imgui/*.cpp; do
    obj="$FE_OUT/$(basename "${src%.cpp}").o"
    # shellcheck disable=SC2086
    "$CXX" $FE_FLAGS -c -o "$obj" "$src" || {
        echo "error: frontend compile failed on $src" >&2; exit 1; }
    FE_OBJS="$FE_OBJS $obj"
done

echo "==> linking libretrodos.so"
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
"$CXX" -shared -fPIC -o "$OUT/libretrodos.so" \
    $FE_OBJS \
    -Wl,--whole-archive $ARCHIVES "$TREE"/src/*.o -Wl,--no-whole-archive \
    -L"$SDL3_PREFIX/lib" -lSDL3 \
    $LIBS_LINE $EXTRA \
    -lm -ldl -llog -landroid -lz \
    -Wl,--no-undefined -Wl,--no-undefined-version

# ---------------------------------------------------------------------------
# 4. Prove the ABI is actually there
# ---------------------------------------------------------------------------
echo "==> verifying the host ABI"
"$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$OUT/libretrodos.so" > "$OUT/exported.syms"
missing=0
for sym in $(grep -oE '\bretrodos_host_[a-z_]+\(' "$ROOT/include/retrodos_host.h" \
             | tr -d '(' | sort -u); do
    grep -q " $sym\$" "$OUT/exported.syms" || { echo "    MISSING: $sym"; missing=1; }
done
[ "$missing" = 0 ] || { echo "error: the host ABI is incomplete" >&2; exit 1; }

echo "==> SDL linkage:"
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT/libretrodos.so" | grep -i 'NEEDED.*SDL' || true

# Everything the app has to package. libc++_shared is a real runtime
# dependency of the core (NEEDED in the ELF header), not an optional extra --
# leaving it out fails at System.loadLibrary time, not at build time.
cp -f "$SDL3_PREFIX/lib/libSDL3.so" "$OUT/"
CXX_SHARED="$TOOLCHAIN/sysroot/usr/lib/$TRIPLE/libc++_shared.so"
[ -f "$CXX_SHARED" ] || CXX_SHARED="$(find "$TOOLCHAIN" -name libc++_shared.so -path "*$TRIPLE*" | head -1)"
[ -f "$CXX_SHARED" ] && cp -f "$CXX_SHARED" "$OUT/" || echo "warning: libc++_shared.so not found" >&2

# The SDL3 Java glue an SDL3 Android activity needs.
find "$SDL3_PREFIX" -name 'SDL3-*.jar' ! -name '*sources*' -exec cp -f {} "$OUT/SDL3.jar" \; 2>/dev/null || true

# Install into the directory Gradle actually packages.
#
# This is not a convenience. Gradle reads native libraries from
# app/src/main/jniLibs (see app/build.gradle.kts), NOT from this script's output
# directory, and its mergeDebugNativeLibs task reports UP-TO-DATE when nothing
# there has changed. Leaving the copy as a manual step therefore produces a
# BUILD SUCCESSFUL that silently ships the previous .so -- the build looks
# clean, the app runs, and none of the new code is in it.
# $HERE, not a path relative to $0: the script cds into the build tree well
# before this point, so a relative destination would quietly create the
# directory somewhere inside the build output and leave the real one stale.
JNI_LIBS="$HERE/app/src/main/jniLibs/$ANDROID_ABI"
mkdir -p "$JNI_LIBS"
cp -f "$OUT/libretrodos.so" "$OUT/libSDL3.so" "$JNI_LIBS/"
[ -f "$OUT/libc++_shared.so" ] && cp -f "$OUT/libc++_shared.so" "$JNI_LIBS/"

# SDL3.jar likewise: app/build.gradle.kts declares it as a file dependency, so
# a tree without it does not fail at run time but at CONFIGURATION time, with
# a transform error that says nothing about the core never having been built.
APP_LIBS="$HERE/app/libs"
mkdir -p "$APP_LIBS"
[ -f "$OUT/SDL3.jar" ] && cp -f "$OUT/SDL3.jar" "$APP_LIBS/"

echo "==> done ($ANDROID_ABI):"
ls -lh "$OUT"/libretrodos.so "$OUT"/libSDL3.so "$OUT"/libc++_shared.so "$OUT"/SDL3.jar 2>/dev/null
echo "==> installed for packaging:"
ls -lh "$JNI_LIBS"/*.so

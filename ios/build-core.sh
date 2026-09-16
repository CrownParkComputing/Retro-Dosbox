#!/usr/bin/env bash
# Build the DOS core and the SDL3 + ImGui frontend for iOS, and link them into
# one static library the app target links whole.
#
#   ios/build-core.sh                  # device, arm64
#   IOS_PLATFORM=iphonesimulator ios/build-core.sh
#
# WHY THIS IS NOT android/build-core.sh WITH DIFFERENT FLAGS
#
#  1. Android links a SHARED libretrodos.so and dlopens it. iOS links
#     statically into the app executable: Apple rejects a bare dylib at
#     install (ApplicationVerificationFailed), and the frontend owns main()
#     anyway, so there is nothing to dlopen.
#  2. configure.ac cannot tell iOS from macOS -- both are *-*-darwin* and it
#     says so itself ("For now I am lazy and do not add proper detection
#     code"). ios/apply-ios-configure.py adds that detection and MUST run
#     before autogen.sh turns configure.ac into configure.
#  3. MACOSX stays defined on iOS, deliberately: endianness, paths and dyld are
#     equally true there. The cost is that iOS takes every macOS branch, which
#     is where Carbon, Cocoa, IOKit and the CoreAudio AudioUnits live. Those
#     are guarded in the core with `defined(MACOSX) && !defined(IPHONEOS)`.
#  4. No JIT. --disable-dynamic-core/-dynamic-x86/-dynrec, because iOS forbids
#     the writable-executable memory they need. The interpreter is the only
#     core iOS can run, so the conf must say core=auto, never core=dynamic.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(dirname "$HERE")"
CORE="$APP/core"

[ -f "$CORE/include/retrodos_host.h" ] || {
    echo "error: the core submodule is not initialised." >&2
    echo "       git submodule update --init" >&2; exit 1; }

IOS_PLATFORM="${IOS_PLATFORM:-iphoneos}"
DEPLOY="${IOS_DEPLOYMENT_TARGET:-15.0}"
SDL3_TAG="${SDL3_TAG:-release-3.2.20}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

case "$IOS_PLATFORM" in
    iphoneos)        TRIPLE="arm64-apple-ios${DEPLOY}" ;;
    iphonesimulator) TRIPLE="arm64-apple-ios${DEPLOY}-simulator" ;;
    *) echo "error: IOS_PLATFORM must be iphoneos or iphonesimulator" >&2; exit 1 ;;
esac

BUILD="$HERE/build/$IOS_PLATFORM"
SDL3_PREFIX="$BUILD/sdl3"
TREE="$BUILD/tree"          # its own configured tree, never shared with a host build
OUT="$BUILD/out"
mkdir -p "$BUILD" "$OUT"

IOS_SDK="$(xcrun --sdk "$IOS_PLATFORM" --show-sdk-path)"
export CC="$(xcrun --sdk "$IOS_PLATFORM" -f clang)"
export CXX="$(xcrun --sdk "$IOS_PLATFORM" -f clang++)"
export AR="$(xcrun --sdk "$IOS_PLATFORM" -f ar)"
export RANLIB="$(xcrun --sdk "$IOS_PLATFORM" -f ranlib)"
export NM="$(xcrun --sdk "$IOS_PLATFORM" -f nm)"
IOS_CFLAGS="-target $TRIPLE -isysroot $IOS_SDK -fPIC -O2"

# autoconf needs GNU m4; macOS ships BSD m4 and autoconf refuses it. A CI
# runner has these from Homebrew; a machine without admin rights can build them
# into $HOME (see docs/IOS_BUILD.md).
export PATH="${AUTOTOOLS_PREFIX:-$HOME/opt/autotools}/bin:$PATH"
command -v autoconf >/dev/null || { echo "error: no autoconf on PATH" >&2; exit 1; }

# ---------------------------------------------------------------- 1. SDL3
if [ ! -f "$SDL3_PREFIX/lib/libSDL3.a" ]; then
    echo "==> SDL3 ($SDL3_TAG) for $IOS_PLATFORM"
    [ -d "$BUILD/SDL" ] || git clone --depth 1 --branch "$SDL3_TAG" \
        https://github.com/libsdl-org/SDL.git "$BUILD/SDL"
    cmake -S "$BUILD/SDL" -B "$BUILD/sdl3-build" -G "Unix Makefiles" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
        -DCMAKE_OSX_SYSROOT="$IOS_SDK" \
        -DCMAKE_INSTALL_PREFIX="$SDL3_PREFIX" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$BUILD/sdl3-build" -j"$JOBS"
    cmake --install "$BUILD/sdl3-build"
fi
echo "==> SDL3: $SDL3_PREFIX/lib/libSDL3.a"

# ---------------------------------------------------- 2. the engine, for iOS
if [ ! -d "$TREE" ]; then
    echo "==> staging a private tree"
    mkdir -p "$TREE"
    # TRACKED files only. A plain copy drags in a host build's config.h and
    # Makefiles, the `[ ! -f config.h ]` guard below then skips configure, and
    # the iOS compiler happily rebuilds against the HOST configuration.
    git -C "$CORE" ls-files -z | tar -C "$CORE" --null -T - -cf - | tar -C "$TREE" -xf -
    python3 "$HERE/apply-ios-configure.py" "$TREE"
fi

cd "$TREE"
[ -f configure ] || ./autogen.sh

if [ ! -f config.h ]; then
    echo "==> configure for $TRIPLE"
    # configure's SDL3 test is literally `test -n "$SDL3_LIBS"`, so setting
    # these directly is enough -- no pkg-config needed anywhere.
    export SDL3_CFLAGS="-I$SDL3_PREFIX/include"
    export SDL3_LIBS="-L$SDL3_PREFIX/lib -lSDL3"
    export CFLAGS="$IOS_CFLAGS" CXXFLAGS="$IOS_CFLAGS -DRETRODOS_HOST_PROVIDES_MAIN"
    export OBJCFLAGS="$IOS_CFLAGS" OBJCXXFLAGS="$IOS_CFLAGS"
    # --build MUST differ from --host. On an Apple Silicon Mac the build
    # machine IS aarch64-apple-darwin, cross mode never engages, and configure
    # hangs forever on "checking whether we are cross compiling".
    # ac_cv_* overrides, not a source change: configure AUTO-DETECTS libpcap
    # and defines C_PCAP when it finds a header and a library, and the iOS SDK
    # has both -- so ethernet pass-through compiles in with no flag asked for
    # and nothing in the build saying so. That is exactly the shape of the
    # rejection Retro-Amiga took under guideline 5.6: review notes that said
    # "no network features of any kind" over a binary that imported sockets.
    # Forcing the two cache variables makes the test fail, which is what
    # --disable-pcap would do if it existed.
    export ac_cv_header_pcap_h=no
    export ac_cv_lib_pcap_pcap_open_live=no

    ./configure \
        --host=aarch64-apple-darwin --build=x86_64-apple-darwin \
        --enable-sdl3 \
        --disable-opengl --disable-dynamic-core --disable-dynamic-x86 \
        --disable-dynrec --disable-printer --disable-sdlnet --disable-freetype \
        --disable-libfluidsynth --disable-alsa-midi --disable-avcodec \
        --disable-libslirp --disable-screenshots \
        || { echo "configure failed; see $TREE/config.log" >&2; exit 1; }
fi

grep -q '^#define C_SDL3 1'  config.h || { echo "error: SDL3 not selected" >&2; exit 1; }
grep -q '^#define IPHONEOS 1' config.h || { echo "error: iOS not detected -- did apply-ios-configure.py run?" >&2; exit 1; }
grep -q '^#define C_DYNREC'   config.h && { echo "error: a JIT core is enabled; iOS forbids it" >&2; exit 1; }

echo "==> building the engine ($JOBS jobs)"
# -k, and the exit status is ignored on purpose: `all` ends by linking the
# dosbox-x EXECUTABLE, which is meaningless here and cannot succeed once the
# host provides main(). The archives are what matter and are checked next.
make -k -j"$JOBS" || true
[ -f src/gui/libgui.a ] || { echo "error: the engine did not build" >&2; exit 1; }

# ------------------------------------------------------------ 3. frontend
echo "==> building the SDL3 + ImGui frontend"
FE="$BUILD/frontend"; mkdir -p "$FE"
# The app's name reaches the C++ through these two, and the frontend is
# compiled into the same library for every app built from this tree -- so they
# are set HERE as well as on the Xcode target, or the library keeps the default
# and the running app calls itself Retro-DOS while its icon says otherwise.
APP_NAME="${APP_NAME:-Autoexec}"
APP_CORE="${APP_CORE:-DOSBox-X}"
FE_FLAGS="$IOS_CFLAGS -std=gnu++17 -I$CORE/include -I$APP/frontend/imgui -I$SDL3_PREFIX/include"
for src in "$APP"/frontend/*.cpp "$APP"/frontend/imgui/*.cpp; do
    # The two identity defines are written out HERE, quoted at the point of
    # use, rather than folded into $FE_FLAGS: that variable is deliberately
    # unquoted so it word-splits, and a value with quotes in it does not
    # survive that intact.
    # shellcheck disable=SC2086
    "$CXX" $FE_FLAGS \
        -DRETRODOS_APP_NAME="\"$APP_NAME\"" \
        -DRETRODOS_APP_CORE="\"$APP_CORE\"" \
        -c -o "$FE/$(basename "${src%.cpp}").o" "$src" \
        || { echo "error: frontend compile failed on $src" >&2; exit 1; }
done

# ------------------------------------------------- 4. one static library
# The archive list comes from dosbox_x_LDADD, not a glob: a glob over
# src/*/lib*.a misses the nested ones (libs/gui_tk, hardware/mame,
# hardware/reSID). LDADD also repeats libgui.a, libdos.a and libints.a, which
# is a duplicate-symbol error once the app links this with -all_load.
echo "==> collecting archives"
python3 - "$TREE/src" "$OUT/archives.txt" <<'PY'
import os, re, sys, pathlib
srcdir, outfile = sys.argv[1], sys.argv[2]
mk = pathlib.Path(srcdir, "Makefile").read_text()
def var(n):
    m = re.search(rf'^{n} *= *((?:.*\\\n)*.*)$', mk, re.M)
    return m.group(1).replace("\\\n", " ") if m else ""
raw = var("dosbox_x_LDADD")
for ap in re.findall(r'\$\((am__append_\d+)\)', raw):
    raw = raw.replace(f"$({ap})", var(ap))
seen, out = set(), []
for t in raw.split():
    if t.endswith(".a") and t not in seen:
        seen.add(t); out.append(os.path.join(srcdir, t))
missing = [a for a in out if not os.path.exists(a)]
if missing:
    sys.exit("archives named by LDADD but not built:\n  " + "\n  ".join(missing))
pathlib.Path(outfile).write_text("\n".join(out) + "\n")
print(f"    {len(out)} archives")
PY

echo "==> linking libretrodos.a"
# libtool, not ar: it merges archives-of-archives correctly and keeps the
# member names unique, which plain `ar` does not.
rm -f "$OUT/libretrodos.a"
xcrun --sdk "$IOS_PLATFORM" libtool -static -no_warning_for_no_symbols \
    -o "$OUT/libretrodos.a" \
    "$FE"/*.o $(tr '\n' ' ' < "$OUT/archives.txt") "$TREE"/src/*.o

cp -f "$SDL3_PREFIX/lib/libSDL3.a" "$OUT/"
# --------------------------------------------- 5. no networking, checked
#
# Apple rejected Retro-Amiga under guideline 5.6 -- "features that appear to
# have been intentionally hidden during review" -- because the review notes
# said the app had no networking and the shipped binary imported the BSD
# socket API anyway. Nobody put it there on purpose; a core option was on by
# default and nothing in the build said so.
#
# The same thing is true here and was measured, not guessed: the desktop build
# of this very core links libpcap and carries 64 NE2000 symbols, entirely from
# autodetection, with no flag asking for it. Hence the ac_cv_* overrides
# above -- and hence this, because a configure flag is a claim and a symbol
# table is the fact.
#
# DOSBox-X's networking is IPX over UDP, the NE2000 card over libpcap or
# libslirp, and a "null modem" serial port over TCP. All of it is off above.
echo "==> checking the library imports nothing that can reach a network"
BAD=$(nm -u "$OUT/libretrodos.a" 2>/dev/null \
      | grep -oE '_(socket|connect|bind|listen|accept|getaddrinfo|gethostbyname|sendto|recvfrom|pcap_[a-z_]+)$' \
      | sort -u || true)
if [ -n "$BAD" ]; then
    echo "error: this build can reach the network, and an App Store build must not:" >&2
    echo "$BAD" | sed 's/^/    /' >&2
    echo "  (see the comment above -- most likely C_PCAP came back)" >&2
    exit 1
fi
echo "    clean: no socket or pcap imports"

echo "==> done:"
ls -lh "$OUT/libretrodos.a" "$OUT/libSDL3.a"
echo "    link the app against BOTH, with -all_load on libretrodos.a."

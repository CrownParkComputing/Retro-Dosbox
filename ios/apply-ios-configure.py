#!/usr/bin/env python3
"""Teach DOSBox-X's configure.ac to tell iOS apart from macOS.

Both report `*-*-darwin*`, and configure.ac says so itself:

    dnl We have a problem here: both Mac OS X and Darwin report
    dnl the same signature ... For now I am lazy and do not add
    dnl proper detection code.

So an iOS cross build takes the macOS branch and picks up Carbon, AppKit,
ApplicationServices and IOKit -- none of which exist on iOS. Every link probe
configure runs then fails, which surfaces as nonsense like "Can't find libpng"
even when libpng is sitting right there in the prefix.

The host triple cannot carry the distinction (config.sub does not accept an
`ios` OS), so the discriminator is the -target flag already in CFLAGS, which is
what actually decides what is being built.

MACOSX stays defined: the source uses it for Darwin-common things (endianness,
paths, dyld) that are equally true on iOS. Only the framework list changes.
Idempotent; the anchor is the framework line itself.
"""
import os
import sys

TREE = sys.argv[1] if len(sys.argv) > 1 else "."
path = os.path.join(TREE, "configure.ac")

MAC_LIBS = ('       LIBS="$LIBS -framework Carbon -framework CoreFoundation '
            '-framework CoreMIDI -framework AudioUnit -framework AudioToolbox '
            '-framework ApplicationServices -framework AppKit -framework IOKit "')

REPLACEMENT = '''       dnl iOS reports the same *-*-darwin* signature as macOS, so the only
       dnl reliable discriminator is the -target the compiler was given.
       dnl Carbon, AppKit, ApplicationServices and IOKit are macOS-only; on iOS
       dnl they make every configure link probe fail.
       case "$CFLAGS$CXXFLAGS" in
         *-target\\ arm64-apple-ios*|*apple-ios*)
           iphoneos=1
           AC_DEFINE(IPHONEOS, 1, [Compiling for iOS])
           LIBS="$LIBS -framework CoreFoundation -framework CoreMIDI -framework AudioToolbox -framework UIKit "
           ;;
         *)
           LIBS="$LIBS -framework Carbon -framework CoreFoundation -framework CoreMIDI -framework AudioUnit -framework AudioToolbox -framework ApplicationServices -framework AppKit -framework IOKit "
           ;;
       esac'''

METAL_ANCHOR = 'if test "x$have_metal" = "xyes"; then'
METAL_NEW = '''dnl Metal itself exists on iOS, so this probe succeeds -- but DOSBox-X's Metal
dnl backend (output_metal.mm) is AppKit-based, and AppKit is macOS-only. Left
dnl enabled, C_METAL compiles the callers and the link then fails on
dnl OUTPUT_Metal_Select and friends. This project renders through Game Link
dnl into a buffer anyway and never uses a window backend.
case "$CFLAGS$CXXFLAGS" in
  *apple-ios*) have_metal=no ;;
esac

if test "x$have_metal" = "xyes"; then'''

with open(path) as f:
    text = f.read()

if "Compiling for iOS" in text:
    print("    configure.ac: already patched")
elif MAC_LIBS not in text:
    sys.exit("error: framework anchor not found in configure.ac - has upstream moved?")
else:
    text = text.replace(MAC_LIBS, REPLACEMENT, 1)
    if METAL_ANCHOR in text:
        text = text.replace(METAL_ANCHOR, METAL_NEW, 1)
    with open(path, "w") as f:
        f.write(text)
    print("    configure.ac: patched (frameworks + metal)")


# ---------------------------------------------------------------------------
# Drop opusfile's HTTP client from the build.
#
# An App Store build of this app must not be able to reach the network -- the
# review notes say so in as many words, and build-core.sh fails the build if
# the finished library imports a socket call. Bringing the core in-tree pulled
# in a newer opusfile whose http.c is an HTTP/HTTPS client for streaming Opus
# over the network, compiled unconditionally into libopusint.a. That is where
# _accept, _bind, _connect, _listen and _socket were coming from.
#
# Nothing uses it. http.c's only public symbols are op_url_stream_create and
# op_url_stream_vcreate, and neither is referenced anywhere else in the tree:
# DOSBox-X decodes Opus from files on disk, never from a URL. This vendored
# copy predates upstream opusfile's OP_DISABLE_HTTP guard, so there is no
# define to set -- the file has to leave the source list.
#
# Patched in the staged tree rather than in core/, so the in-tree import stays
# a clean copy of upstream and `tool/update-core.sh` keeps working.
decoders = os.path.join(TREE, "src", "libs", "decoders", "internal", "Makefile.am")
HTTP_LINE = "    opusfile/http.c \\\n"

if not os.path.exists(decoders):
    sys.exit(f"error: {decoders} not found - has upstream moved the decoders?")

with open(decoders) as f:
    dec = f.read()

if HTTP_LINE not in dec:
    print("    decoders/Makefile.am: opusfile/http.c already gone")
else:
    with open(decoders, "w") as f:
        f.write(dec.replace(HTTP_LINE, "", 1))
    print("    decoders/Makefile.am: dropped opusfile/http.c (no network in an App Store build)")


# ---------------------------------------------------------------------------
# Stub out the local-pipe transport on iOS.
#
# The other half of the network-guard failure. localpipe.cpp opens an AF_UNIX
# SOCK_STREAM socket -- local IPC that cannot reach a network, but it imports
# _socket, _bind, _listen, _accept and _connect all the same, and build-core.sh
# fails on the symbols because it cannot tell an address family from a name.
#
# The feature is dead weight on iOS regardless. It exists so the emulated
# parallel port (parport/extlpt.cpp, transport:pipe) and the serial namedpipe
# type can hand bytes to another program on the same machine. A sandboxed iOS
# app has no other program to hand them to.
#
# Dropping the file is not an option the way it was for opusfile/http.c:
# extlpt.cpp calls LocalPipe_Open unconditionally, outside any C_MODEM guard,
# so the symbols have to keep existing. They are replaced with stubs that fail
# cleanly instead -- LocalPipe_Open returns LOCALPIPE_INVALID with an error,
# which is exactly the path extlpt.cpp already takes when a pipe cannot be
# opened, so the feature reports itself unavailable rather than misbehaving.
pipe_src = os.path.join(TREE, "src", "misc", "localpipe.cpp")
POSIX_ANCHOR = "#else // !defined(WIN32) - POSIX AF_UNIX stream socket\n"

IOS_STUBS = '''#elif defined(RETRODOS_NO_LOCALPIPE) // iOS: no second process to pipe to

// Stubs, not an implementation. See ios/apply-ios-configure.py: an App Store
// build must not import socket calls, and nothing on iOS can use this anyway.

LocalPipeOpenResult LocalPipe_Open(const std::string &, bool, unsigned) {
\tLocalPipeOpenResult r;
\tr.error = "local pipes are not available on this platform";
\treturn r;
}

void LocalPipe_Close(localpipe_t) {}
void LocalPipe_Unlink(const std::string &) {}

localpipe_t LocalPipe_Listen(const std::string &, std::string *error) {
\tif (error) *error = "local pipes are not available on this platform";
\treturn LOCALPIPE_INVALID;
}

localpipe_t LocalPipe_TryAccept(localpipe_t, bool *listenerConsumed) {
\tif (listenerConsumed) *listenerConsumed = false;
\treturn LOCALPIPE_INVALID;
}

void LocalPipe_SetNonBlocking(localpipe_t) {}
long LocalPipe_Recv(localpipe_t, void *, size_t) { return -1; }
long LocalPipe_Send(localpipe_t, const void *, size_t) { return -1; }

#else // !defined(WIN32) - POSIX AF_UNIX stream socket
'''

if not os.path.exists(pipe_src):
    sys.exit(f"error: {pipe_src} not found - has upstream moved localpipe?")

with open(pipe_src) as f:
    pipe = f.read()

if "RETRODOS_NO_LOCALPIPE" in pipe:
    print("    misc/localpipe.cpp: already stubbed")
elif POSIX_ANCHOR not in pipe:
    sys.exit("error: localpipe.cpp POSIX anchor not found - has upstream moved?")
else:
    with open(pipe_src, "w") as f:
        f.write(pipe.replace(POSIX_ANCHOR, IOS_STUBS, 1))
    print("    misc/localpipe.cpp: AF_UNIX transport stubbed out for iOS")

# Autoexec — App Review notes

Paste the whole of this into **Review Notes** on **every** submission, not just
the first. App Review bounces an emulator submission that arrives without the
4.7.4 index, and it does not carry over from the previous version.

---

## What this app is

Autoexec is a DOS emulator for iPhone and iPad. The emulation is
[DOSBox-X](https://github.com/joncampbell123/dosbox-x), GNU GPL v2, used
unmodified in every respect that matters to the emulation; the interface around
it is ours. The app credits DOSBox-X on its first screen, under the name, and
again in full on the About screen with a link to the project and to our source.

It is not a version of our Android app "Retro-DOS". It is a separate app with
its own name, icon, identifier, listing and interface, sharing only the
open-source engine underneath. (This is the answer to the 4.3 rejection of the
Retro-* family: not a rename, a different app.)

## 4.7.4 — index of software offered in the app

**There is none.** The app offers no catalogue, no store, no downloads and no
links to obtain software. It runs software the user already has.

Two pieces of software are **embedded** in the app, and neither is offered as a
download:

| Embedded | What it is | Licence |
|---|---|---|
| `FREEDOS.IMG` | A FreeDOS 1.3 boot floppy image, so the app can start a DOS prompt with nothing installed | GNU GPL v2, freedos.org |
| `DEMO.COM` | A small demonstration program we wrote (354 bytes) | Ours, GPL v2 with the app |

Everything else a user runs is a file they supply themselves, through the Files
app, from their own media. The app has no means of acquiring software.

## 2.5.2 / JIT

No just-in-time compilation. The core is built with `--disable-dynamic-core
--disable-dynamic-x86 --disable-dynrec`; the interpreter is the only CPU
implementation in the binary. No writable-executable memory is requested and
`com.apple.security.cs.allow-jit` is not claimed.

## 5.6 / networking

**The app has no network features of any kind.** There is no account, no sign
in, no analytics, no update check, no telemetry.

This is enforced in the build rather than asserted: DOSBox-X can emulate IPX
over UDP, an NE2000 card over libpcap or libslirp, and a serial "null modem"
over TCP. All are configured off, `libpcap` autodetection is forced to fail,
and `ios/build-core.sh` **fails the build** if the finished static library
imports any of `socket`, `connect`, `bind`, `listen`, `accept`, `getaddrinfo`,
`gethostbyname`, `sendto`, `recvfrom` or `pcap_*`.

(The desktop build of the same app does have an optional artwork feature that
uses the network. It is compiled out here — the code is behind
`RETRODOS_MEDIA_HTTP`, which this build does not define, and libcurl is not
linked.)

## Privacy

Nothing is collected, because nothing leaves the device. No data is transmitted
anywhere; there is nowhere for it to go.

## How to try it

1. Open the app. It starts on the DOS Library, which is empty on a fresh
   install.
2. Tap **Demo** to run the bundled demonstration program, or **Demo → FreeDOS**
   for FreeDOS, then answer N to its installer for a DOS prompt. Neither needs any file from the reviewer.
3. To run your own software, copy a folder of DOS files into the app's folder
   in the Files app; it appears in the DOS Library.

The Demo page is deliberately the fastest path to something running, so that a
reviewer with no DOS software can see the app work in two taps.

## Copyright

No proprietary software is bundled, downloaded or linked to. The app does not
supply MS-DOS, Windows, or any game. The Windows Setup walkthrough explains how
to install Windows 98 from an installation CD the user already owns; it supplies
no disc and cannot obtain one. The FreeDOS Setup walkthrough does the same for
FreeDOS 1.3 (GNU GPL) from a CD image the user places in the games folder. It
contains no links and downloads nothing.

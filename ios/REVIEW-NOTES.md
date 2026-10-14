# DOSDeck — App Review notes

Everything BELOW the rule is the reviewer-facing text, and it goes into
**Review Notes** on **every** submission, not just the first: App Review
bounces an emulator submission that arrives without the 4.7.4 index, and the
field does not carry over from the previous version. This paragraph is a note
to ourselves and must not be sent -- `tool/set-review-notes.sh` splits on the
rule so the reviewer never reads our instructions to each other.

---

## What this app is

DOSDeck is a DOS and Windows 98 machine for iPhone and iPad. The emulation is
[DOSBox-X](https://github.com/joncampbell123/dosbox-x), GNU GPL v2, used
unmodified in every respect that matters to the emulation. The app credits
DOSBox-X on its first screen, under the name, and again in full on the About
screen.

## 4.3 — this is not another copy of our other apps

This was the reason for the previous rejection, and the answer is not a rename.
**The entire interface has been rewritten in Swift and SwiftUI for iOS**, and
shares no screen with anything else we publish.

What the app looked like before was a cross-platform C++ interface drawn with
Dear ImGui -- the same code our Android app uses, a left rail beside a list of
titles. None of it is in this build. The iOS build compiles no ImGui at all.
What is shared with the other builds is the GPL emulator underneath, which is
credited, and two headless files that generate a DOSBox-X configuration file.

The app you are reviewing is organised around a question no other app of ours
asks: **Windows or DOS**. That is the first screen. Each answer leads to a
walkthrough written for this app -- a DOS machine with the full set of
DOSBox-X hardware options, or a step-by-step Windows 98 installation that
tracks its own progress and then lets you change the CD in the drive while
Windows runs. There is no sidebar, no library rail and no shared artwork.

## 4.7.4 — index of software offered in the app

**There is none.** The app offers no catalogue, no store, no downloads and no
links to obtain software. It runs software the user already has.

Two pieces of software are **embedded** in the app, and neither is offered as
a download. On first launch they are copied into the app's own folders, where
they appear as ordinary content:

| Embedded | What it is | Licence |
|---|---|---|
| `DEMO.COM` | A small demonstration program we wrote (354 bytes) | Ours, GPL v2 with the app |
| `FREEDOS.IMG` | A FreeDOS 1.3 boot floppy image | GNU GPL v2, freedos.org |

Everything else a user runs is a file they supply themselves, through the
Files app, from their own media. The app has no means of acquiring software.

## 2.5.2 / JIT

No just-in-time compilation. The core is built with `--disable-dynamic-core
--disable-dynamic-x86 --disable-dynrec`; the interpreter is the only CPU
implementation in the binary. No writable-executable memory is requested and
`com.apple.security.cs.allow-jit` is not claimed.

## 5.6 / networking

**The app has no network features of any kind.** There is no account, no sign
in, no analytics, no update check, no telemetry.

This is enforced in the build rather than asserted. DOSBox-X can emulate IPX
over UDP, an NE2000 card over libpcap or libslirp, and a serial "null modem"
over TCP. All are configured off, `libpcap` autodetection is forced to fail,
and `ios/build-core.sh` **fails the build** if the finished static library
imports any of `socket`, `connect`, `bind`, `listen`, `accept`, `getaddrinfo`,
`gethostbyname`, `sendto`, `recvfrom` or `pcap_*`.

The optional artwork feature that uses the network on desktop is not merely
compiled out here -- the file that implements it is not given to the compiler
at all.

## Privacy

Nothing is collected, because nothing leaves the device. No data is
transmitted anywhere; there is nowhere for it to go.

## How to try it, with no files of your own

1. Open the app. The first screen asks **Windows or DOS**.
2. Choose **DOS**. The list already contains **Demo**, which ships inside the
   app.
3. Tap **Demo**, then **Start**. A DOS machine boots and runs it. Nothing has
   to be downloaded or copied in first.
4. While it runs, the **Keys** chip gives you a PC keyboard and the **Run**
   chip starts something else without restarting the machine.

To see the Windows half without a Windows CD, choose **Windows** at the first
screen: the wizard explains each step of an installation and shows what is
missing, which in your case will be the disc. DOSDeck does not supply Windows,
cannot download it, and has nowhere to download it from.

## Copyright

No proprietary software is bundled, downloaded or linked to. The app does not
supply MS-DOS, Windows, or any game. The Windows walkthrough explains how to
install Windows 98 from an installation CD the user already owns; it supplies
no disc and cannot obtain one.

## One machine per launch, which is deliberate

DOSBox-X initialises its globals once per process, so a second emulator
session in the same process cannot be started. Rather than restart itself --
which on iOS would mean calling `exit()` -- DOSDeck is built so that it never
needs to: you choose a machine at the first screen, and everything else
happens inside that machine. Mounting another folder, running another program,
changing the disc in the drive and moving a Windows installation to its next
step are all done to the running machine. Closing and reopening the app
returns you to the first screen.

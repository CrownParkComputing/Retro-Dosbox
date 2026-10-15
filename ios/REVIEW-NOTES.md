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
DOSBox-X, GNU GPL v2, used unmodified. The app credits it on its first screen,
under the name, and in full on the About screen.

## 4.3 - this is not another copy of our other apps

This was the reason for the previous rejection, and the answer is not a
rename. **The entire interface has been rewritten in Swift and SwiftUI for
iOS**, and shares no screen with anything else we publish.

What you saw before was a cross-platform C++ interface drawn with Dear ImGui -
the same code our Android app uses, a left rail beside a list of titles. None
of it is in this build; the iOS build compiles no ImGui at all. What is shared
with our other builds is the GPL emulator underneath, which is credited, and
two headless files that generate a DOSBox-X configuration file.

This app is organised around a question no other app of ours asks: **Windows
or DOS**. That is the first screen. Each answer leads to a walkthrough written
for this app - a DOS machine with the full set of DOSBox-X hardware options,
or a step-by-step Windows 98 installation that tracks its own progress and
then lets you change the CD while Windows runs. There is no sidebar, no
library rail and no shared artwork.

## 4.7.4 - index of software offered in the app

**There is none.** No catalogue, no store, no downloads, no links to obtain
software. It runs software the user already has.

Two items are embedded in the app and neither is offered as a download. On
first launch they are copied into the app's own folders, where they appear as
ordinary content:

- DEMO.COM - a 354-byte demonstration program we wrote (ours, GPL v2)
- FREEDOS.IMG - a FreeDOS 1.3 boot floppy (GNU GPL v2, freedos.org)

Everything else is a file the user supplies through the Files app. The app has
no means of acquiring software.

## 2.5.2 / JIT

No just-in-time compilation. The core is built with --disable-dynamic-core
--disable-dynamic-x86 --disable-dynrec; the interpreter is the only CPU
implementation in the binary. No writable-executable memory is requested.

## 5.6 / networking

**The app has no network features of any kind.** No account, no sign in, no
analytics, no update check, no telemetry.

This is enforced in the build rather than asserted. DOSBox-X can emulate IPX
over UDP, an NE2000 card and a serial null modem over TCP; all are configured
off, libpcap autodetection is forced to fail, and the build FAILS if the
finished library imports any BSD socket call or pcap_*. The desktop-only
artwork downloader is not given to the compiler at all.

## Privacy

Nothing is collected, because nothing leaves the device.
## How to try it, with no files of your own

1. Open the app. The first screen asks Windows or DOS.
2. Choose DOS. The list already contains Demo, which ships inside the app.
3. Tap Demo, then Start. A DOS machine boots and runs it. Nothing has to be
   downloaded or copied in first.
4. While it runs, Keys gives you a PC keyboard and Run starts something else
   without restarting the machine.

To see the Windows half without a Windows CD, choose Windows at the first
screen: the wizard explains each step and shows what is missing, which in your
case is the disc. DOSDeck does not supply Windows and cannot obtain it.

## Copyright

No proprietary software is bundled, downloaded or linked to. The app does not
supply MS-DOS, Windows, or any game. The Windows walkthrough explains how to
install Windows 98 from a CD the user already owns; it supplies no disc.

## One machine per launch

DOSBox-X initialises its globals once per process, so a second emulator
session cannot be started in it. DOSDeck is built so that it never needs one:
you choose a machine at the first screen and everything else happens inside it
- mounting another folder, running another program, changing the disc. Closing
and reopening the app returns you to the first screen.

# Retro-DOS

A DOS games launcher and emulator for handhelds, built on **DOSBox-X** with an
**SDL3 + Dear ImGui** frontend.

This replaces the earlier Flutter application entirely. The Flutter build put a
UI toolkit between the player and a 320x200 picture that has to arrive sixty
times a second, and kept the emulator behind a plugin boundary that made the
things a launcher actually needs — mounting a game, remapping a key while it
runs, reading a frame — awkward or impossible. The frontend is now native and
in the same process as the core.

## Layout

    core/       DOSBox-X, ported to SDL3   (submodule: CrownParkComputing/dosbox-x-sdl3)
    frontend/   the launcher: library, settings, on-screen controls, RetroMedia
    android/    the Android application and its build script
    ios/        the iOS application, its build script and its signing material
    demo/       bundled content, and the source that generates it

The core is a **submodule rather than a copy**. It is a fork of DOSBox-X with
upstream still attached, which is what keeps its changes mergeable; vendoring
the tree into this repository would end that and duplicate several gigabytes of
sources for nothing.

## Where things live

The setup wizard asks for **one folder** and keeps everything under it:

    <folder>/games/           one sub-folder or .zip per DOS game
    <folder>/apps/            DOS applications, the same way
    <folder>/discs/dos/       CDs and floppies for DOS games and applications
    <folder>/discs/windows/   the Windows install CD and Windows game CDs
    <folder>/discs/freedos/   the FreeDOS CD
    <folder>/machines/        Windows 98/ and FreeDOS/, each with its hard disk image

The rail is Launch, Library, Media, Windows, FreeDOS. Library lists games and
applications together with an A-Z / 0-9 strip and a search modal; tapping a
title loads it onto Launch. Launch draws the PC, lists its spec (video, CPU,
memory, sound) and its drives, and has the one button that starts it, as
Retro-Saturn's does. Drives are added, not fixed: a floppy drive, CD-ROM
drive or hard disk gets the letter a PC would give it, and Media -- three
tabs, CDs, Floppies and Hard disks, each with initials and search -- puts an
image in it. A hard disk in C: with no title loaded boots that disk. Windows
and FreeDOS show their machine when it exists and its setup walkthrough until
then.

On Android that folder is either one the user picks with the system folder
picker -- granted read/write and persisted, resolved to a real path because
DOSBox-X mounts paths, not `content://` URIs -- or the app's own storage on
any volume, which needs no permission. A picked folder that Android will not
let the app write to directly is reported in the wizard rather than failing
later. "Run setup again" on the Library page goes back through the wizard;
nothing is moved when the folder changes.

## Building

    git clone --recurse-submodules https://github.com/CrownParkComputing/Retro-Dosbox.git
    cd Retro-Dosbox/android
    ./build-core.sh          # cross-compiles the core and the frontend
    gradle assembleDebug

`build-core.sh` builds SDL3 and DOSBox-X for the target ABI, compiles the
frontend, links `libretrodos.so`, and installs the result into
`app/src/main/jniLibs` — the directory Gradle actually packages. That last step
is not a convenience: without it Gradle reports BUILD SUCCESSFUL while shipping
the previous library.

If the submodule has not been initialised the script stops and says so, rather
than failing later inside the compiler.

### Linux desktop

    ./tool/build-linux.sh                # -> ~/.cache/retro-dosbox-linux/retrodos

The same frontend and core as the phone, linked into one native executable
with the system SDL3. Nothing is installed: run the binary where it was built,
since it reads `assets/` and `demo/` from beside itself. With libcurl and
minizip present the RetroMedia client is compiled in; without them the pages
are simply hidden. The games folder is chosen on first run, the same as on a
handheld, and the Windows and FreeDOS setup walkthroughs work identically --
which makes this the quickest place to try a change to either before a device
build.

### iOS

    cd Retro-Dosbox
    ./ios/build-core.sh                      # device; IOS_PLATFORM=iphonesimulator for the Simulator
    cmake -S ios -B ios/build/app -G Xcode \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
    cmake --build ios/build/app --config Release

Needs autoconf, automake, libtool and cmake — and **GNU** m4, because macOS
ships the BSD one and autoconf refuses it.

It is not the Android script with different flags. Android links a shared
`libretrodos.so` and dlopens it; iOS links **statically** into the app, because
Apple rejects a bare dylib at install and the frontend owns `main()` anyway.
The engine also has to be told that iOS is not macOS — both report
`*-*-darwin*`, and DOSBox-X's `configure.ac` says outright that it does not
distinguish them — which is what `ios/apply-ios-configure.py` is for, and it
must run before `autogen.sh`.

No JIT. iOS does not permit memory that is both writable and executable, so the
core is built `--disable-dynamic-core --disable-dynamic-x86 --disable-dynrec`
and the generated conf asks for `core=auto`, never `core=dynamic` — naming a
core that is not in the binary is how this failed before.

Signing happens in CI, never on a workstation. `.github/workflows/ios.yml`
decrypts `ios/signing/*.enc` with one repository secret, `SIGNING_PASSPHRASE`,
then archives, signs and uploads. See `ios/signing/README.md`.

## How a game is run

Each game is a directory, mounted as `C:`. Where a game ships its own
`dosbox.conf` — the collections in this format all do — its `[autoexec]` and
sound settings are used verbatim, because the game states how it starts far
better than we can infer it: Descent's folder alone holds `ASKECHO.COM`,
`JCHOICE.EXE`, `network.bat` and `run.bat`, and only one of those starts the
game. Failing that, a runnable is chosen from the directory, preferring
`run`/`start` over installers and setup programs.

## Controls

A physical gamepad is used when one is connected and on-screen controls appear
when none is. Both feed the same button state, so nothing downstream knows
which was used. What each button *sends* is a per-game setting, because DOS
never agreed on a control scheme — Keen jumps on Ctrl and pogos on Alt, Descent
wants the arrows and six degrees of freedom. Presets are provided and the
mapping can be changed from the in-game menu, while the game is on screen.

## RetroMedia

Signing in to `media.crownparkcomputing.com` fetches box art. Administrators
can also download games. Downloads are gated on the platform as well as the
account: an App Store build does not offer them at all.

## Licensing

DOSBox-X is GPL; see `core/COPYING`. The bundled FreeDOS image and the
demonstration program have their own terms, recorded in `demo/NOTICE.md`.

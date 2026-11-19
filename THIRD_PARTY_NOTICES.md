# Third-Party Notices

Retro-DOS is distributed under the GNU General Public License version 2.
See [LICENSE](LICENSE).

## DOSBox-X

The emulator core. This project uses DOSBox-X ported to SDL3, kept in the
`core/` directory of this repository; `core/UPSTREAM` records the upstream
release it is based on.

- Project: https://dosbox-x.com/
- Upstream source: https://github.com/joncampbell123/dosbox-x
- Modified source: the `core/` directory of https://github.com/CrownParkComputing/Retro-Dosbox
- License: GNU General Public License version 2

DOSBox-X is itself based on DOSBox and carries further third-party components
and credits; see `core/COPYING` and the acknowledgements in that tree.

## SDL3

Window, input, audio and rendering, on both the frontend and the core.

- Project: https://libsdl.org/
- License: zlib

## Dear ImGui

The frontend's user interface. Vendored in `frontend/imgui/`.

- Project: https://github.com/ocornut/imgui
- License: MIT

## Bundled native libraries

The Android package contains:

- `libretrodos.so` — the DOSBox-X core and this project's frontend, GPLv2
- `libSDL3.so` — SDL3, zlib licence
- `libc++_shared.so` — the NDK C++ runtime

## Bundled content

The app ships a FreeDOS boot floppy and a small demonstration program so it is
usable with no games installed. FreeDOS is included verbatim under the GPL, and
the demonstration program was written for this project. Provenance, licences
and the source that regenerates the demo are in [demo/NOTICE.md](demo/NOTICE.md).

FreeDOS is a trademark of Jim Hall. This project is not affiliated with,
endorsed by, or sponsored by the FreeDOS project.

## Source availability

Because GPL binaries are redistributed, the corresponding source must be
available to anyone who receives them. It is:

- This application: https://github.com/CrownParkComputing/Retro-Dosbox
- The emulator core: the `core/` directory of the repository above
- FreeDOS: https://www.freedos.org/download/

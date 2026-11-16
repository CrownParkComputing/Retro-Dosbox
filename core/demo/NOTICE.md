# Bundled demo content — licensing

Retro-DOS ships two pieces of content so the app is usable, and reviewable,
on a device that has no games on it.

## FREEDOS.IMG — FreeDOS 1.3

A 1.44 MB bootable floppy image, taken unmodified from the **FreeDOS 1.3
Floppy Edition** (`144m/x86BOOT.img`):

    https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip

FreeDOS is free software. Its kernel and the utilities on this image are
distributed under the **GNU General Public License**; a few components carry
other free licences, each documented by the FreeDOS project.

Because binaries are redistributed here, the corresponding source must be
available to anyone who receives them. It is, from the FreeDOS project itself:

    https://www.freedos.org/download/
    https://github.com/FDOS

The image is included **verbatim and unmodified**. Retro-DOS does not link
against it, patch it, or derive from it — it is data the emulator boots, in the
same way a user's own floppy image would be. Nothing in this repository is a
derived work of FreeDOS.

FreeDOS is a trademark of Jim Hall. Retro-DOS is not affiliated with, endorsed
by, or sponsored by the FreeDOS project.

## DEMO.COM — the built-in demonstration

Written for this project; see `demo/make_demo.py`, which is its source and
regenerates it byte-for-byte:

    python3 demo/make_demo.py demo/DEMO.COM

354 bytes of 16-bit x86 that prints a message through INT 21h, waits on INT 16h
and animates a 320x200 VGA screen locked to the vertical retrace.

It is deliberately ours rather than a well-known shareware episode. Shareware
titles come with redistribution terms that have to be honoured per-title and
trademarks that an app store will notice, and none of that effort would make
the demo any better at its actual job: proving the emulator runs.

Copyright © Crown Park Computing. Distributed under the same licence as the
rest of this repository.

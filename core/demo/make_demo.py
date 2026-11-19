#!/usr/bin/env python3
"""
Build DEMO.COM -- the bundled Retro-DOS demonstration program.

WHY THIS EXISTS
---------------
The app has to do something on a device with no games on it. That is both a
usability point (a launcher showing an empty list looks broken) and an App
Store one: a review of an emulator with no content to run is a review of a
blank screen, and reviews like that fail.

The obvious fix -- ship a well-known shareware episode -- carries licensing and
trademark risk we do not need. So the demo is ours: a few hundred bytes of
16-bit x86 that exercises the parts of the emulator a reviewer can see working
(DOS text output, INT 21h, INT 16h keyboard, and a VGA mode 13h animation
synchronised to the retrace). No third-party code, nothing to license.

WHY A PYTHON ASSEMBLER
----------------------
There is no assembler in the build environment, and committing a binary blob
with no source is exactly the sort of thing nobody can later audit or change.
This script IS the source: the byte encodings are written out explicitly and
the two-pass label resolution below removes the hand-arithmetic that makes
hand-assembly unreliable.

Usage:  python3 make_demo.py [output.com]
"""

import struct
import sys

ORG = 0x100          # a .COM image is loaded at CS:0100


class Asm:
    """Two-pass assembler: emit bytes, mark labels, patch jumps at the end.

    Only what this demo needs -- 8-bit relative jumps and one absolute
    16-bit address for the message. Pass 1 records where each label and each
    fixup lands; pass 2 fills the holes in, so no offset is ever counted by
    hand.
    """

    def __init__(self):
        self.buf = bytearray()
        self.labels = {}
        self.fixups = []      # (position, label, kind)

    def db(self, *vals):
        for v in vals:
            self.buf.append(v & 0xFF)

    def label(self, name):
        self.labels[name] = len(self.buf)

    def rel8(self, opcode, name):
        """Conditional/short jump; the displacement is from the NEXT
        instruction, which is why the fixup is recorded after the opcode."""
        self.db(*opcode) if isinstance(opcode, (list, tuple)) else self.db(opcode)
        self.fixups.append((len(self.buf), name, "rel8"))
        self.db(0)

    def abs16(self, opcode, name):
        self.db(*opcode)
        self.fixups.append((len(self.buf), name, "abs16"))
        self.db(0, 0)

    def link(self):
        for pos, name, kind in self.fixups:
            target = self.labels[name]
            if kind == "rel8":
                disp = target - (pos + 1)
                if not -128 <= disp <= 127:
                    raise ValueError(f"jump to {name} out of rel8 range ({disp})")
                self.buf[pos] = disp & 0xFF
            else:
                struct.pack_into("<H", self.buf, pos, ORG + target)
        return bytes(self.buf)


def build():
    a = Asm()

    # ---- text splash -------------------------------------------------
    # INT 21h/AH=09 prints a '$'-terminated string. Cheap, and it proves the
    # DOS layer and text mode are alive before anything touches the VGA.
    a.abs16([0xBA], "msg")            # mov dx, msg
    a.db(0xB4, 0x09)                  # mov ah, 09h
    a.db(0xCD, 0x21)                  # int 21h

    a.db(0xB4, 0x00)                  # mov ah, 00h   -- wait for a keypress
    a.db(0xCD, 0x16)                  # int 16h

    # ---- VGA mode 13h: 320x200, 256 colours, one byte per pixel -------
    a.db(0xB8, 0x13, 0x00)            # mov ax, 0013h
    a.db(0xCD, 0x10)                  # int 10h
    a.db(0xB8, 0x00, 0xA0)            # mov ax, A000h
    a.db(0x8E, 0xC0)                  # mov es, ax    -- ES = video segment
    # STOSB steps DI by the direction flag. DOS conventionally hands control
    # over with DF clear, but "conventionally" is not "always" -- and with DF
    # set this loop would walk backwards out of the video window and scribble
    # over the program itself. One byte to not depend on it.
    a.db(0xFC)                        # cld
    a.db(0x31, 0xDB)                  # xor bx, bx    -- BL is the frame counter

    a.label("frame")

    # Wait for the vertical retrace before drawing. Without this the screen
    # tears, and tearing is the first thing that makes an emulator look broken.
    # Bit 3 of the input status register at 3DAh is set during the retrace, so
    # wait for it to clear and then for it to rise: that is the START of a
    # retrace rather than the middle of one already in progress.
    a.db(0xBA, 0xDA, 0x03)            # mov dx, 03DAh
    a.label("vr_wait_end")
    a.db(0xEC)                        # in al, dx
    a.db(0xA8, 0x08)                  # test al, 8
    a.rel8(0x75, "vr_wait_end")       # jnz -- still retracing, keep waiting
    a.label("vr_wait_start")
    a.db(0xEC)                        # in al, dx
    a.db(0xA8, 0x08)                  # test al, 8
    a.rel8(0x74, "vr_wait_start")     # jz  -- not yet started

    # Classic XOR texture, scrolled by the frame counter: colour = (x^y)+frame.
    # Chosen because it fills every pixel every frame from a tight loop, so a
    # stall or a bad blit is immediately visible as a stutter or a torn band.
    a.db(0x31, 0xFF)                  # xor di, di    -- offset into video RAM
    a.db(0x31, 0xD2)                  # xor dx, dx    -- y = 0
    a.label("row")
    a.db(0x31, 0xC9)                  # xor cx, cx    -- x = 0
    a.label("col")
    a.db(0x88, 0xC8)                  # mov al, cl
    a.db(0x30, 0xD0)                  # xor al, dl
    a.db(0x00, 0xD8)                  # add al, bl
    a.db(0xAA)                        # stosb         -- ES:[DI++] = AL
    a.db(0x41)                        # inc cx
    a.db(0x81, 0xF9, 0x40, 0x01)      # cmp cx, 320
    a.rel8(0x72, "col")               # jb col
    a.db(0x42)                        # inc dx
    a.db(0x81, 0xFA, 0xC8, 0x00)      # cmp dx, 200
    a.rel8(0x72, "row")               # jb row

    a.db(0xFE, 0xC3)                  # inc bl        -- advance the animation

    # INT 16h/AH=01 peeks at the keyboard without blocking; ZF set means no key
    # is waiting, so the demo keeps running until the user asks it to stop.
    a.db(0xB4, 0x01)                  # mov ah, 01h
    a.db(0xCD, 0x16)                  # int 16h
    a.rel8(0x74, "frame")             # jz frame

    a.db(0x31, 0xC0)                  # xor ax, ax    -- consume the keystroke
    a.db(0xCD, 0x16)                  # int 16h

    # Always restore text mode before exiting: leaving the guest in mode 13h
    # would hand FreeDOS's prompt back on a graphics screen.
    a.db(0xB8, 0x03, 0x00)            # mov ax, 0003h
    a.db(0xCD, 0x10)                  # int 10h
    a.db(0xB8, 0x00, 0x4C)            # mov ax, 4C00h -- terminate, code 0
    a.db(0xCD, 0x21)                  # int 21h

    # ---- data --------------------------------------------------------
    a.label("msg")
    text = ("\r\n"
            "  Retro-DOS\r\n"
            "  ---------\r\n"
            "  DOSBox-X core, SDL3 frontend.\r\n\r\n"
            "  This is the built-in demo, shown because no games\r\n"
            "  were found. Add your DOS games from the library\r\n"
            "  screen to play them here.\r\n\r\n"
            "  Press any key to start the demo,\r\n"
            "  then any key again to return.\r\n$")
    a.db(*text.encode("ascii"))

    return a.link()


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "DEMO.COM"
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out}: {len(data)} bytes")

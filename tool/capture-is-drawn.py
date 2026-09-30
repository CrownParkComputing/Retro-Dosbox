#!/usr/bin/env python3
"""Has the app actually drawn, or is this iOS's launch screen?

    tool/capture-is-drawn.py <screenshot.png>      # exit 0 = the app drew

`xcrun simctl io screenshot` will happily photograph an app that has not
finished starting, and the result is `UILaunchScreen`'s default: a plain
white frame. It is a valid PNG of the right size with a perfectly normal
amount of variance in it (the camera cutout alone gives a standard deviation
around 25), so nothing about the file says it is wrong. One reached a
finished, correctly-sized, store-ready set and was only caught by a human
opening it.

MEAN BRIGHTNESS is what separates them, and not marginally. This interface
clears to (12, 12, 16) and every real screen measures a mean of 2 to 30; the
launch screen measures 252. The threshold sits in the middle of a gap two
hundred levels wide, so it is not a tuned number and does not need to be.

This would not work for a light-themed app. It works here because the app is
near-black by design, which is worth saying out loud in case the theme ever
changes.
"""
import sys

from PIL import Image, ImageStat

# Measured: real screens 2-30, launch screen 252.
MAX_MEAN = 120


def main(argv) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    try:
        mean = ImageStat.Stat(Image.open(argv[1]).convert("L")).mean[0]
    except Exception as exc:                      # unreadable/truncated capture
        print(f"{argv[1]}: cannot read ({exc})", file=sys.stderr)
        return 1
    if mean > MAX_MEAN:
        print(f"{argv[1]}: mean brightness {mean:.0f} — still the launch screen",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

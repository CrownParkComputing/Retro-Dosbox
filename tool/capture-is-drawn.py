#!/usr/bin/env python3
"""Has the app actually drawn, or is this iOS's launch screen?

    tool/capture-is-drawn.py <screenshot.png>      # exit 0 = the app drew

`xcrun simctl io screenshot` will happily photograph an app that has not
finished starting, and the result is `UILaunchScreen`'s default: a plain
white frame. It is a valid PNG of the right size with a perfectly ordinary
amount of variance in it — the camera cutout alone gives a standard
deviation around 25 — so nothing about the file says it is wrong. One
reached a finished, correctly-sized, store-ready set in the sibling app and
was only caught by a human opening it.

WHY NOT MEAN BRIGHTNESS, which is what this file used to do. The first
version flagged a frame as blank when its mean brightness exceeded 120, on
the reasoning that this interface is an amber prompt on near-black: real
screens measured 2-30 and the launch screen measured 252.

That was wrong, and it was wrong here and not just in theory. The iPad
"run" capture — the demo program's own bright VGA output — measures a mean
of 157 and the old test called it a launch screen. It only ever passed
because the frame the next run happened to catch was darker. A capture job
that rejects good pictures depending on timing is worse than no check.

So the test is the launch screen's SHAPE rather than its brightness: it is
almost entirely ONE very light colour — 99% of its pixels are near-white,
where that bright demo frame is 0.6%. A real screen, light or dark, has
text and chrome in it and never comes close.
"""
import sys

from PIL import Image, ImageStat

# A pixel this light is "blank page" rather than "interface".
WHITE = 245
# The launch screen measures ~0.97 by this; a GEM desktop full of icons,
# a menu bar and a title bar measures far less even though it is pale.
MAX_WHITE_FRACTION = 0.90


def main(argv) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    try:
        grey = Image.open(argv[1]).convert("L")
    except Exception as exc:                      # unreadable/truncated capture
        print(f"{argv[1]}: cannot read ({exc})", file=sys.stderr)
        return 1

    total = grey.width * grey.height
    # histogram()[n] is the count of pixels with luminance n.
    white = sum(grey.histogram()[WHITE:])
    fraction = white / total
    if fraction > MAX_WHITE_FRACTION:
        print(f"{argv[1]}: {fraction:.0%} of pixels are near-white "
              f"— still the launch screen", file=sys.stderr)
        return 1

    # A completely uniform frame of any colour is also not an interface:
    # that is a cleared framebuffer, which is what a crashed or still-booting
    # emulator leaves behind.
    if ImageStat.Stat(grey).stddev[0] < 2.0:
        print(f"{argv[1]}: the frame is a single flat colour — nothing drawn",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

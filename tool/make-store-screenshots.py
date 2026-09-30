#!/usr/bin/env python3
"""Turn raw simulator captures into App Store sized images.

    tool/make-store-screenshots.py store/screenshots-raw-iphone
    tool/make-store-screenshots.py store/screenshots-raw-ipad

Reads every PNG in the given directory and writes store-ready copies into
store/screenshots/<target>/.

WHY THIS IS NOT JUST A RESIZE. App Store Connect rejects a screenshot whose
dimensions are not exactly one of the sizes it lists, and it rejects an image
carrying an alpha channel. A simulator capture is opaque but not necessarily
the right size, so each image is scaled to fit and then padded to the exact
dimensions on the app's own background colour, which is invisible against
these screens. Stretching to fit instead distorts the picture and looks it.

LANDSCAPE ONLY, because the app is: Info.plist declares
UISupportedInterfaceOrientations as the two landscape cases and nothing else,
so there is no portrait screen to photograph and no portrait target here. A
portrait frame could only be produced by padding a landscape capture into it,
which is a legal upload that looks exactly like what it is.
"""
import pathlib
import sys

from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent

# SDL_SetRenderDrawColor(ren, 12, 12, 16, 255) -- the frontend's own clear
# colour, so padding does not read as a border.
BACKDROP = (12, 12, 16)

TARGETS = {
    # App Store Connect: "iPhone 6.9-inch display", landscape. An iPhone 17
    # Pro Max simulator captures at exactly this, so these pass through
    # unscaled -- the run still matters, because it flattens the alpha channel
    # a capture can carry and App Store Connect rejects.
    "ios-iphone-69-landscape": (2868, 1320),
    # App Store Connect: "iPad 13-inch display", landscape. Required for an
    # iPad app whatever you captured on.
    "ios-ipad-13-landscape": (2752, 2064),
}

# Sources are NOT interchangeable, which is the whole reason this mapping
# exists: padding a 4:3 iPad capture into a 2.2:1 iPhone frame leaves bars
# down both sides and looks exactly like what it is. Each source folder may
# only produce the family it was shot on.
SOURCES = {
    "screenshots-raw-iphone": ["ios-iphone-69-landscape"],
    "screenshots-raw-ipad": ["ios-ipad-13-landscape"],
}


def upright(img: Image.Image, size: tuple) -> Image.Image:
    """Rotate a capture whose orientation does not match the target.

    `xcrun simctl io screenshot` photographs the DEVICE framebuffer, and the
    simulator boots portrait. A landscape-only app is rotated inside that
    frame, so the raw file is 1320x2868 with the interface lying on its side.
    Scaling that into a landscape target without rotating produces a legal
    upload of a sideways app -- which is how it went unnoticed the first time.

    Counter-clockwise, because the home indicator ends up on the right and the
    camera cutout on the left: iOS landscape-left, which is what the simulator
    rotates a landscape-only app into.
    """
    if (img.width < img.height) != (size[0] < size[1]):
        return img.rotate(90, expand=True)
    return img


def fit(img: Image.Image, size: tuple) -> Image.Image:
    img = upright(img, size)
    tw, th = size
    scale = min(tw / img.width, th / img.height)
    w, h = max(1, round(img.width * scale)), max(1, round(img.height * scale))
    canvas = Image.new("RGB", size, BACKDROP)
    canvas.paste(img.resize((w, h), Image.LANCZOS), ((tw - w) // 2, (th - h) // 2))
    return canvas


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 1
    src = pathlib.Path(argv[1])
    if not src.is_dir():
        print(f"error: no such directory {src}", file=sys.stderr)
        return 1
    shots = sorted(src.glob("*.png"))
    if not shots:
        print(f"error: no PNGs in {src}", file=sys.stderr)
        return 1

    targets = SOURCES.get(src.name)
    if targets is None:
        print(
            f"error: {src.name} is not a known source folder. Add it to "
            f"SOURCES, naming the sizes it may produce -- an iPad capture "
            f"must never be padded into an iPhone frame.",
            file=sys.stderr,
        )
        return 1

    out_root = REPO / "store/screenshots"
    for target in targets:
        size = TARGETS[target]
        out = out_root / target
        out.mkdir(parents=True, exist_ok=True)
        native = 0
        for shot in shots:
            img = Image.open(shot).convert("RGB")
            if upright(img, size).size == size:
                native += 1
            fit(img, size).save(out / shot.name)
        print(
            f"{target}: {len(shots)} x {size[0]}x{size[1]} -> {out}"
            f"{f'  ({native} native, unscaled)' if native else ''}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

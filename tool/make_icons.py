#!/usr/bin/env python3
"""Builds the launcher icon and every size Android and iOS ask for.

The same three-part icon as the Amiga, C64 and Saturn front ends - the Retro
script cut from the Retro Recompilation logo, the machine's name under it, and
the machine's own mark below that - so the whole family sits together on a home
screen and reads as one set. What differs is the mark: an Amiga has its boot
tick, a C64 the screen it wakes up on, a Saturn its swirl, and DOS the one
thing it has ever put on a screen unprompted - an amber C:\\> and a cursor.

    python3 tool/make_icons.py

Run from the repository root. Overwrites the mipmaps and the iOS icon
set.
"""

from __future__ import annotations

import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO = os.path.join(HERE, "assets", "brand", "retro_recomp_logo.png")
FONT_CANDIDATES = [
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
]
FONT = next((f for f in FONT_CANDIDATES if os.path.exists(f)), None)
if FONT is None:
    raise SystemExit("no bold sans found; looked for:\n  " + "\n  ".join(FONT_CANDIDATES))
MONO_CANDIDATES = [
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
]
MONO = next((f for f in MONO_CANDIDATES if os.path.exists(f)), None)
if MONO is None:
    raise SystemExit("no bold mono found; looked for:\n  " + "\n  ".join(MONO_CANDIDATES))

SIZE = 1024

BG_TOP = (24, 18, 10)
BG_BOTTOM = (6, 5, 4)

# The wordmark's blue, top to bottom: white highlight into deep blue.
CHROME = [
    (232, 244, 255),
    (150, 205, 250),
    (56, 120, 220),
    (26, 60, 160),
    (120, 180, 240),
]

# Amber phosphor, straight off the app's own palette
# (RetroDosboxTheme.accentAmber) rather than a colour invented for the icon.
AMBER = (255, 176, 0)
AMBER_DIM = (150, 100, 0)
SCREEN = (12, 9, 6)
BEZEL = (58, 54, 50)
BEZEL_LIP = (96, 90, 82)


def vertical_gradient(size, colours):
    width, height = size
    grad = Image.new("RGB", (1, height))
    pixels = grad.load()
    steps = len(colours) - 1
    for y in range(height):
        position = y / max(1, height - 1) * steps
        index = min(int(position), steps - 1)
        blend = position - index
        start, end = colours[index], colours[index + 1]
        pixels[0, y] = tuple(
            int(start[c] + (end[c] - start[c]) * blend) for c in range(3)
        )
    return grad.resize((width, height))


def background():
    canvas = vertical_gradient((SIZE, SIZE), [BG_TOP, BG_BOTTOM]).convert("RGBA")
    glow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)
    draw.ellipse((60, 250, SIZE - 60, SIZE - 120), fill=(190, 110, 20, 110))
    draw.ellipse((200, 520, SIZE - 200, SIZE - 60), fill=(255, 150, 30, 85))
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    return Image.alpha_composite(canvas, glow)


def retro_script(width):
    """The Retro script, cut out of the logo rather than redrawn."""
    logo = Image.open(LOGO).convert("RGBA")
    script = logo.crop((168, 0, 578, 92))
    # The bracket rules either side of the script poke into the crop. Every
    # pixel of the script is warm, so anything bluer than it is red is a rule.
    pixels = script.load()
    for y in range(script.height):
        for x in range(script.width):
            r, g, b, a = pixels[x, y]
            if a and b > r:
                pixels[x, y] = (r, g, b, 0)
    height = round(script.height * width / script.width)
    return script.resize((width, height), Image.LANCZOS)


def chrome_text(text, width, height):
    """[text] in the wordmark's blue, with the dark outline it has."""
    size = 10
    font = ImageFont.truetype(FONT, size)
    while True:
        probe = ImageFont.truetype(FONT, size + 4)
        box = probe.getbbox(text)
        if box[2] - box[0] > width or box[3] - box[1] > height:
            break
        size += 4
        font = probe

    box = font.getbbox(text)
    pad = 18
    layer = Image.new("RGBA", (box[2] - box[0] + pad * 2, box[3] - box[1] + pad * 2))
    ImageDraw.Draw(layer).text(
        (pad - box[0], pad - box[1]), text, font=font, fill=(255, 255, 255, 255)
    )

    mask = layer.split()[3]
    fill = vertical_gradient(layer.size, CHROME).convert("RGBA")
    fill.putalpha(mask)

    outline = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    outline.paste((12, 20, 48, 255), (0, 0), mask.filter(ImageFilter.MaxFilter(9)))
    return Image.alpha_composite(outline, fill)


def prompt_screen(width):
    """A DOS prompt on an amber screen: C:\\> and the block cursor after it.

    Not a wall of boot text. At 48 pixels a paragraph is grey mush, whereas a
    drive letter and a waiting block still read as a machine at a command line.
    """
    height = round(width * 0.62)
    scale = 4
    w, h = width * scale, height * scale
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)

    bezel = round(width * 0.075) * scale
    radius = round(width * 0.06) * scale
    draw.rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, fill=BEZEL + (255,))
    # A lit top edge on the bezel, so the panel reads as a monitor rather than
    # a flat grey rectangle at small sizes.
    draw.rounded_rectangle(
        (0, 0, w - 1, h - 1),
        radius=radius,
        outline=BEZEL_LIP + (255,),
        width=max(1, round(scale * 1.5)),
    )
    draw.rounded_rectangle(
        (bezel, bezel, w - 1 - bezel, h - 1 - bezel),
        radius=round(radius * 0.5),
        fill=SCREEN + (255,),
    )

    text = "C:\\>"
    inner = w - bezel * 2
    size = 10
    font = ImageFont.truetype(MONO, size)
    while True:
        probe = ImageFont.truetype(MONO, size + 4)
        box = probe.getbbox(text)
        if box[2] - box[0] > inner * 0.60:
            break
        size += 4
        font = probe

    box = font.getbbox(text)
    left = bezel + round(inner * 0.11)
    top = bezel + round((h - bezel * 2) * 0.30)
    draw.text((left - box[0], top - box[1]), text, font=font, fill=AMBER + (255,))

    # The cursor, on the same line: a solid block, which is what DOS leaves
    # sitting there waiting for you.
    cell_w = (box[2] - box[0]) / len(text)
    cell_h = box[3] - box[1]
    cursor_left = left + round(cell_w * (len(text) + 0.35))
    draw.rectangle(
        (cursor_left, top, cursor_left + round(cell_w), top + round(cell_h * 1.05)),
        fill=AMBER + (255,),
    )

    # Phosphor bloom: the amber lifted off the glass, the way the real thing
    # smears on a CRT. Blurred hard so it survives the downsample as warmth.
    glow = layer.copy().filter(ImageFilter.GaussianBlur(round(scale * 6)))
    lit = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    lit.paste(AMBER_DIM + (90,), (0, 0), glow.split()[3].point(lambda v: v // 3))
    stamped = Image.alpha_composite(layer, Image.alpha_composite(lit, layer))
    return stamped.resize((width, height), Image.LANCZOS)


def artwork(width):
    layer = Image.new("RGBA", (width, width), (0, 0, 0, 0))

    script = retro_script(round(width * 0.80))
    name = chrome_text("DOS", round(width * 0.46), round(width * 0.19))
    screen = prompt_screen(round(width * 0.62))

    stack = script.height + name.height + screen.height + round(width * 0.05)
    top = max(0, (width - stack) // 2)

    layer.alpha_composite(script, ((width - script.width) // 2, top))
    top += script.height + round(width * 0.015)
    layer.alpha_composite(name, ((width - name.width) // 2, top))
    top += name.height + round(width * 0.035)
    layer.alpha_composite(screen, ((width - screen.width) // 2, top))
    return layer


def wordmark(width=520):
    """The wide wordmark the app draws at the top of its navigation rail.

    The same three pieces as the launcher icon and in the same order -- Retro
    script, DOS, the amber prompt -- but laid out across instead of down,
    because a 200px rail is wide and short where an icon is square. Built from
    the identical helpers so the two can never drift into being different
    logos: change chrome_text and both follow.

    Transparent, so it sits on whatever the UI's background happens to be.
    """
    script = retro_script(round(width * 0.52))
    name = chrome_text("DOS", round(width * 0.30), round(width * 0.17))
    screen = prompt_screen(round(width * 0.30))

    gap = round(width * 0.035)
    left_w = max(script.width, name.width)
    height = max(script.height + name.height + round(width * 0.01), screen.height)

    layer = Image.new("RGBA", (left_w + gap + screen.width, height), (0, 0, 0, 0))

    top = (height - (script.height + name.height + round(width * 0.01))) // 2
    layer.alpha_composite(script, ((left_w - script.width) // 2, top))
    layer.alpha_composite(name, ((left_w - name.width) // 2,
                                 top + script.height + round(width * 0.01)))
    layer.alpha_composite(screen, (left_w + gap, (height - screen.height) // 2))
    return layer


def master():
    canvas = background()
    art = artwork(round(SIZE * 0.86))
    canvas.alpha_composite(art, ((SIZE - art.width) // 2, (SIZE - art.width) // 2))
    return canvas


def foreground():
    """Everything inside the middle two-thirds, where the launcher's mask
    cannot eat it."""
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    art = artwork(round(SIZE * 0.62))
    layer.alpha_composite(art, ((SIZE - art.width) // 2, (SIZE - art.width) // 2))
    return layer


def rounded(image, radius_fraction=0.22):
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, image.width - 1, image.height - 1),
        radius=round(image.width * radius_fraction),
        fill=255,
    )
    out = image.copy()
    out.putalpha(mask)
    return out


def main():
    # The app loads this at run time and draws it in the rail, so it is a
    # build output like the mipmaps rather than something hand-drawn and
    # checked in: regenerate it and the UI follows.
    mark = wordmark()
    brand = os.path.join(HERE, "assets", "brand")
    os.makedirs(brand, exist_ok=True)
    mark.save(os.path.join(brand, "retrodos_wordmark.png"))
    print("wordmark: %dx%d" % mark.size)

    icon = master()
    fore = foreground()
    back = background()

    res = os.path.join(HERE, "android", "app", "src", "main", "res")
    legacy = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
    layers = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
    for density, px in legacy.items():
        folder = os.path.join(res, f"mipmap-{density}")
        os.makedirs(folder, exist_ok=True)
        icon.resize((px, px), Image.LANCZOS).save(
            os.path.join(folder, "ic_launcher.png")
        )
        rounded(icon.resize((px, px), Image.LANCZOS), 0.5).save(
            os.path.join(folder, "ic_launcher_round.png")
        )
        fore.resize((layers[density],) * 2, Image.LANCZOS).save(
            os.path.join(folder, "ic_launcher_foreground.png")
        )
        back.convert("RGB").resize((layers[density],) * 2, Image.LANCZOS).save(
            os.path.join(folder, "ic_launcher_background.png")
        )

    ios = os.path.join(HERE, "ios", "Assets.xcassets", "AppIcon.appiconset")
    sizes = {
        "Icon-App-20x20@1x.png": 20,
        "Icon-App-20x20@2x.png": 40,
        "Icon-App-20x20@3x.png": 60,
        "Icon-App-29x29@1x.png": 29,
        "Icon-App-29x29@2x.png": 58,
        "Icon-App-29x29@3x.png": 87,
        "Icon-App-40x40@1x.png": 40,
        "Icon-App-40x40@2x.png": 80,
        "Icon-App-40x40@3x.png": 120,
        "Icon-App-60x60@2x.png": 120,
        "Icon-App-60x60@3x.png": 180,
        "Icon-App-76x76@1x.png": 76,
        "Icon-App-76x76@2x.png": 152,
        "Icon-App-83.5x83.5@2x.png": 167,
        "Icon-App-1024x1024@1x.png": 1024,
    }
    if os.path.isdir(ios):
        for name, px in sizes.items():
            icon.convert("RGB").resize((px, px), Image.LANCZOS).save(
                os.path.join(ios, name)
            )

    # The store listing keeps its own copies; regenerate them here rather than
    # letting them drift from the launcher.
    store = os.path.join(HERE, "play-store", "icon.png")
    if os.path.isdir(os.path.dirname(store)):
        icon.convert("RGB").resize((512, 512), Image.LANCZOS).save(store)
    icon.convert("RGB").resize((512, 512), Image.LANCZOS).save(
        os.path.join(HERE, "android", "app", "src", "ic_launcher-playstore.png")
    )

    # The adaptive icon composes a colour resource, not the gradient layer
    # written above, so derive that colour from the same BG_TOP the icon is
    # built on. Left to drift, the masked foreground floats on a plate that
    # does not match the icon beside it.
    plate = "#%02X%02X%02X" % BG_TOP
    for bucket in ("values", "values-night"):
        path = os.path.join(res, bucket, "ic_launcher_background.xml")
        if os.path.isfile(path):
            with open(path, "w") as handle:
                handle.write(
                    '<?xml version="1.0" encoding="utf-8"?>\n'
                    "<resources>\n"
                    '    <color name="ic_launcher_background">'
                    + plate
                    + "</color>\n"
                    "</resources>\n"
                )

    print("icons written")


if __name__ == "__main__":
    main()

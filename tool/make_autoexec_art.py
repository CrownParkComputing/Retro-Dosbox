#!/usr/bin/env python3
"""Icon and wordmark for Autoexec, the iOS app.

    python3 tool/make_autoexec_art.py

DELIBERATELY SHARES NO ARTWORK WITH THE RETRO-* FAMILY.

tool/make_icons.py builds the family look on purpose -- the Retro script cut
from the Retro Recompilation logo, the machine name under it, the machine's own
mark below that -- so that "the whole family sits together on a home screen and
reads as one set". That is a good goal and it is exactly what Apple rejected the
family for under guideline 4.3: several apps from one developer, too alike.

So Autoexec does not use any of it. No Retro script, no blue chrome, no shared
composition. What it uses instead is the thing it is named after: the line DOS
runs when it starts. An amber phosphor screen, a prompt, and the name being
typed at it. Nobody looking at the two icons would take one for a version of
the other, which is the whole point.

The name credits nothing by itself, so the app says "powered by DOSBox-X" under
the wordmark on its first screen -- in the interface, where it can be read, not
baked into an image at 40 pixels tall.
"""

from __future__ import annotations

import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MONO_CANDIDATES = [
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
]
MONO = next((f for f in MONO_CANDIDATES if os.path.exists(f)), None)
if MONO is None:
    raise SystemExit("no bold mono found; looked for:\n  " + "\n  ".join(MONO_CANDIDATES))

SIZE = 1024

# A CRT, not a logo. The screen is nearly black with a warm cast, the type is
# the amber phosphor a real monochrome monitor had, and the glow is the type
# blurred behind itself rather than a drop shadow.
SCREEN_TOP = (16, 11, 4)
SCREEN_BOTTOM = (6, 4, 2)
AMBER = (255, 176, 0)
AMBER_DIM = (156, 104, 8)
CURSOR = (255, 208, 96)


def vertical_gradient(size, top, bottom):
    w, h = size
    img = Image.new("RGB", (1, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        px[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    return img.resize((w, h), Image.BILINEAR).convert("RGBA")


def scanlines(img, spacing, strength):
    """Every [spacing] rows darkened. A CRT reads as a CRT because of these,
    and at icon sizes they survive downscaling as a faint texture rather than
    disappearing, which a sharper effect would not."""
    dark = Image.new("RGBA", img.size, (0, 0, 0, strength))
    mask = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(mask)
    for y in range(0, img.size[1], spacing):
        d.line([(0, y), (img.size[0], y)], fill=255, width=max(1, spacing // 3))
    img.paste(dark, (0, 0), mask)
    return img


def glowing_text(text, font, colour, glow_radius):
    """Type with its own light. Drawn twice: a blurred copy behind, dimmer and
    warmer, then the type itself on top."""
    tmp = Image.new("RGBA", (10, 10))
    box = ImageDraw.Draw(tmp).textbbox((0, 0), text, font=font)
    pad = glow_radius * 3
    layer = Image.new("RGBA", (box[2] - box[0] + pad * 2, box[3] - box[1] + pad * 2),
                      (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.text((pad - box[0], pad - box[1]), text, font=font, fill=colour + (255,))

    glow = layer.filter(ImageFilter.GaussianBlur(glow_radius))
    out = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    out.alpha_composite(glow)
    out.alpha_composite(glow)          # twice: one pass is too faint to see
    out.alpha_composite(layer)
    return out


def fit_font(lines, width, extra_cells, start_px):
    """The largest size at which the longest line still fits the width.

    Guessed sizes do not survive a change of text: the first version of this
    put 8 characters on a 1024px canvas at a size chosen by eye, and
    "C:\\>TYPE" and "AUTOEXEC" both ran off the right-hand edge. Measuring is
    the fix, and [extra_cells] reserves room for the cursor, which is drawn
    after the text and would otherwise be the thing that falls off."""
    px = start_px
    while px > 8:
        font = ImageFont.truetype(MONO, px)
        cell = ImageDraw.Draw(Image.new("RGBA", (4, 4))).textlength("M", font=font)
        widest = max(ImageDraw.Draw(Image.new("RGBA", (4, 4))).textlength(t, font=font)
                     for t in lines)
        if widest + cell * extra_cells <= width:
            return font, px
        px -= 2
    return ImageFont.truetype(MONO, 8), 8


def screen(size, lines, cursor_after, font_px, pad_frac=0.13):
    """The whole icon: a screen with [lines] typed on it and a block cursor."""
    w = h = size
    img = vertical_gradient((w, h), SCREEN_TOP, SCREEN_BOTTOM)

    pad = round(w * pad_frac)
    font, font_px = fit_font(lines, w - pad * 2, 1.0, font_px)
    line_h = round(font_px * 1.32)
    y = (h - line_h * len(lines)) // 2
    for i, text in enumerate(lines):
        colour = AMBER if i == len(lines) - 1 else AMBER_DIM
        art = glowing_text(text, font, colour, max(2, round(font_px * 0.10)))
        img.alpha_composite(art, (pad - round(font_px * 0.30), y - round(font_px * 0.30)))
        if i == cursor_after:
            adv = ImageDraw.Draw(img).textlength(text, font=font)
            cur = Image.new("RGBA", (round(font_px * 0.60), round(font_px * 1.02)),
                            CURSOR + (255,))
            cur_glow = Image.new("RGBA", cur.size, (0, 0, 0, 0))
            cur_glow.alpha_composite(cur)
            cur_glow = cur_glow.filter(ImageFilter.GaussianBlur(font_px * 0.08))
            img.alpha_composite(cur_glow, (pad + round(adv) + round(font_px * 0.12), y))
            img.alpha_composite(cur, (pad + round(adv) + round(font_px * 0.12), y))
        y += line_h

    img = scanlines(img, max(3, round(h / 150)), 46)

    # A soft vignette, because a tube is brighter in the middle.
    vign = Image.new("L", (w, h), 0)
    ImageDraw.Draw(vign).ellipse(
        (-w * 0.25, -h * 0.25, w * 1.25, h * 1.25), fill=255)
    vign = vign.filter(ImageFilter.GaussianBlur(w * 0.12))
    dark = Image.new("RGBA", (w, h), (0, 0, 0, 120))
    img.paste(dark, (0, 0), ImageEval_invert(vign))
    return img


def ImageEval_invert(mask):
    from PIL import ImageOps
    return ImageOps.invert(mask)


def icon():
    # Three lines, because one word floating in the middle is a logo and this
    # is meant to look like a machine that has just been switched on.
    return screen(SIZE,
                  ["C:\\>TYPE", "AUTOEXEC", ".BAT"],
                  cursor_after=2,
                  font_px=round(SIZE * 0.20))


def wordmark(width=560):
    """Wide and short, for the navigation rail: the prompt and the name on one
    line, with the cursor after it."""
    h = round(width * 0.30)
    img = vertical_gradient((width, h), SCREEN_TOP, SCREEN_BOTTOM)
    pad = round(width * 0.06)
    # Two cells spare: the cursor sits after the name and is easy to lose off
    # the end, which is the one character that has to be there.
    font, font_px = fit_font(["C:\\>AUTOEXEC"], width - pad * 2, 2.0,
                             round(h * 0.42))
    prompt = glowing_text("C:\\>", font, AMBER_DIM, round(font_px * 0.10))
    name = glowing_text("AUTOEXEC", font, AMBER, round(font_px * 0.10))

    y = (h - font_px) // 2
    img.alpha_composite(prompt, (pad - round(font_px * 0.3), y - round(font_px * 0.3)))
    x = pad + round(ImageDraw.Draw(img).textlength("C:\\>", font=font))
    img.alpha_composite(name, (x - round(font_px * 0.3), y - round(font_px * 0.3)))

    x += round(ImageDraw.Draw(img).textlength("AUTOEXEC", font=font) + font_px * 0.2)
    cur = Image.new("RGBA", (round(font_px * 0.60), round(font_px * 1.02)),
                    CURSOR + (255,))
    img.alpha_composite(cur, (x, y))

    img = scanlines(img, max(2, round(h / 60)), 40)

    # Rounded, so it reads as a screen sitting in the rail rather than a
    # rectangle of artwork that happens to be dark.
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, img.width - 1, img.height - 1), radius=round(h * 0.16), fill=255)
    img.putalpha(mask)
    return img


IOS_SIZES = [
    ("Icon-App-20x20@1x.png", 20), ("Icon-App-20x20@2x.png", 40),
    ("Icon-App-20x20@3x.png", 60), ("Icon-App-29x29@1x.png", 29),
    ("Icon-App-29x29@2x.png", 58), ("Icon-App-29x29@3x.png", 87),
    ("Icon-App-40x40@1x.png", 40), ("Icon-App-40x40@2x.png", 80),
    ("Icon-App-40x40@3x.png", 120), ("Icon-App-60x60@2x.png", 120),
    ("Icon-App-60x60@3x.png", 180), ("Icon-App-76x76@1x.png", 76),
    ("Icon-App-76x76@2x.png", 152), ("Icon-App-83.5x83.5@2x.png", 167),
    ("Icon-App-1024x1024@1x.png", 1024),
]


def main():
    master = icon()

    out = os.path.join(HERE, "ios", "Assets.xcassets", "AppIcon.appiconset")
    os.makedirs(out, exist_ok=True)
    for name, px in IOS_SIZES:
        # RGB, not RGBA: the App Store marketing icon is rejected outright for
        # carrying an alpha channel, and there is no reason for the others to
        # differ from it.
        master.resize((px, px), Image.LANCZOS).convert("RGB").save(
            os.path.join(out, name))
    print("icons: %d sizes -> %s" % (len(IOS_SIZES), out))

    mark = wordmark()
    dest = os.path.join(HERE, "assets", "ui", "wordmark-autoexec.png")
    mark.save(dest)
    print("wordmark: %dx%d -> %s" % (mark.size[0], mark.size[1], dest))


if __name__ == "__main__":
    main()

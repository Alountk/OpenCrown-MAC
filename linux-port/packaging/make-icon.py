#!/usr/bin/env python3
"""Draws AppIcon.png (1024x1024), the OpenCrown app icon, from the project's own logo artwork, with the artwork's own background:
the picture is cropped around the logo, scaled to fit and placed, unedited, on a rounded square (the macOS icon grid) filled with the
colour of the picture's background.
    python3 make-icon.py <logo image> [AppIcon.png [x0,y0,x1,y1]]
The optional box (source pixels) picks one logo out of a sheet of several.
Needs Pillow; run it when the artwork changes. make-app-mac.sh only uses the PNG it wrote."""
import sys

from PIL import Image, ImageChops, ImageDraw

S, SS = 1024, 2  # canvas, and the factor the body is drawn at before reducing (smooth edges)


def main(src, dst, area=None):
    im = Image.open(src).convert("RGB")
    if area:
        im = im.crop(area)
    bg = im.getpixel((2, 2))  # the picture's background colour, from its corner
    # The logo's box: everything that is not (nearly) the background colour, with a margin.
    diff = ImageChops.difference(im, Image.new("RGB", im.size, bg)).convert("L").point(lambda v: 255 if v > 24 else 0)
    box = diff.getbbox()
    pad = int(0.04 * im.width)
    box = (max(0, box[0] - pad), max(0, box[1] - pad), min(im.width, box[2] + pad), min(im.height, box[3] + pad))
    logo = im.crop(box)

    big = S * SS
    m = 100 * SS  # the macOS icon grid: an 824 px body inside the 1024 px canvas
    body = Image.new("RGB", (big, big), bg)
    width = big - 2 * m
    logo = logo.resize((width, int(logo.height * width / logo.width)), Image.LANCZOS)
    body.paste(logo, (m, (big - logo.height) // 2))
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((m, m, big - m, big - m), radius=185 * SS, fill=255)
    out = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    out.paste(body, (0, 0), mask)
    out.resize((S, S), Image.LANCZOS).save(dst, optimize=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    box = tuple(int(v) for v in sys.argv[3].split(",")) if len(sys.argv) > 3 else None
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "AppIcon.png", box)

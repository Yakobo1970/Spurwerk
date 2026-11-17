"""Draws the Spurwerk logo (tracker pattern columns with a play cursor) as the app icon.
Usage: python make_icon.py <out.png>   (1024 x 1024, used by CMake ICON_BIG / ICON_SMALL)"""
import sys
from PIL import Image, ImageDraw

S = 1024
SS = 4
k = SS
img = Image.new("RGBA", (S * k, S * k), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# rounded square background (Kreisel theme "Nacht")
d.rounded_rectangle([0, 0, S * k - 1, S * k - 1], radius=220 * k, fill=(24, 27, 36, 255))
d.rounded_rectangle([18 * k, 18 * k, (S - 18) * k, (S - 18) * k], radius=205 * k, outline=(35, 40, 56, 255), width=10 * k)

accent = (124, 156, 255, 255)
track = [(255, 95, 109, 255), (255, 169, 77, 255), (105, 219, 124, 255), (77, 171, 247, 255)]
dim = (58, 64, 84, 255)

# four pattern columns with "notes" (bars) of different lengths
cols = 4
x0, x1 = 170, 854
cw = (x1 - x0) / cols
rows = 9
y0, y1 = 200, 830
rh = (y1 - y0) / rows
pattern = [
    [1, 0, 2, 0, 1, 0, 3, 0, 1],
    [0, 3, 0, 0, 2, 0, 0, 3, 0],
    [2, 0, 0, 1, 0, 0, 2, 0, 0],
    [0, 0, 3, 0, 0, 1, 0, 0, 2],
]
for c in range(cols):
    for r in range(rows):
        x = x0 + c * cw + 18
        y = y0 + r * rh + 14
        w = cw - 36
        h = rh - 28
        v = pattern[c][r]
        if v:
            ww = w * (0.45 + 0.18 * v)
            d.rounded_rectangle([x * k, y * k, (x + ww) * k, (y + h) * k], radius=12 * k, fill=track[c])
        else:
            d.rounded_rectangle([x * k, (y + h * 0.35) * k, (x + w * 0.3) * k, (y + h * 0.65) * k], radius=6 * k, fill=dim)

# play cursor row
cy = y0 + 4 * rh
d.rounded_rectangle([(x0 - 30) * k, (cy + 4) * k, (x1 + 30) * k, (cy + rh - 4) * k], radius=18 * k, outline=accent, width=14 * k)

img = img.resize((S, S), Image.LANCZOS)
img.save(sys.argv[1] if len(sys.argv) > 1 else "icon.png")

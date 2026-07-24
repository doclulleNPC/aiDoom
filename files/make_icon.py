#!/usr/bin/env python3
"""Generate buddydoom.ico -- a stylized DOOM demon/imp face on a dark badge."""
from PIL import Image, ImageDraw

S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# --- rounded dark badge background with a dark-red vignette ---
def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

cx, cy = S / 2, S / 2
maxr = (S / 2) * 1.42
bg = Image.new("RGBA", (S, S))
bp = bg.load()
for y in range(S):
    for x in range(S):
        t = min(1.0, ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / maxr)
        bp[x, y] = lerp((60, 14, 10, 255), (8, 4, 4, 255), t)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([6, 6, S - 6, S - 6], radius=44, fill=255)
img.paste(bg, (0, 0), mask)
d = ImageDraw.Draw(img)
# subtle inner border
d.rounded_rectangle([6, 6, S - 6, S - 6], radius=44, outline=(120, 30, 24, 255), width=4)

RED = (176, 28, 22, 255)
RED_D = (110, 16, 14, 255)
EYE = (255, 214, 90, 255)
EYE_HOT = (255, 120, 40, 255)
BONE = (235, 222, 200, 255)

# --- horns ---
d.polygon([(60, 96), (40, 30), (96, 78)], fill=RED, outline=RED_D)
d.polygon([(196, 96), (216, 30), (160, 78)], fill=RED, outline=RED_D)

# --- face (a downward shield / heart-ish demon head) ---
face = [
    (72, 78), (184, 78),            # brow line
    (206, 120), (188, 168),         # right cheek
    (150, 210), (128, 226),         # chin
    (106, 210), (68, 168),          # left cheek
    (50, 120),
]
d.polygon(face, fill=RED, outline=RED_D)

# --- angry brow ridge ---
d.polygon([(72, 92), (184, 92), (150, 116), (106, 116)], fill=RED_D)

# --- glowing slanted eyes ---
d.polygon([(86, 120), (124, 110), (122, 134), (90, 140)], fill=EYE)
d.polygon([(170, 120), (132, 110), (134, 134), (166, 140)], fill=EYE)
d.polygon([(96, 122), (120, 116), (118, 130), (100, 134)], fill=EYE_HOT)
d.polygon([(160, 122), (136, 116), (138, 130), (156, 134)], fill=EYE_HOT)

# --- snarling fanged mouth ---
d.polygon([(98, 168), (158, 168), (146, 196), (110, 196)], fill=(20, 6, 6, 255))
for i, x in enumerate(range(104, 156, 13)):
    d.polygon([(x, 168), (x + 13, 168), (x + 6, 184 + (i % 2) * 4)], fill=BONE)  # upper fangs
for i, x in enumerate(range(110, 150, 13)):
    d.polygon([(x, 196), (x + 13, 196), (x + 6, 182 - (i % 2) * 3)], fill=BONE)  # lower fangs

# --- save multi-resolution .ico ---
sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
img.save("../buddydoom.ico", sizes=sizes)
print("wrote ../buddydoom.ico", sizes)

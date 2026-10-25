"""Generates the Display Hawk app icon: a hawk head watching a glowing LED panel.

Run from anywhere:  python assets/icon/make_icon.py
Writes displayhawk.png (1024 px), displayhawk.ico (Windows) and displayhawk.icns (macOS)
next to this script. Requires Pillow.
"""
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SS = 4               # supersampling factor for smooth edges
N = 1024             # design grid
S = N * SS
OUT = Path(__file__).resolve().parent

# Body: a rounded square with a margin, which suits both the macOS grid and Windows tiles.
MARGIN, RADIUS = 72, 196
BODY = (MARGIN, MARGIN, N - MARGIN, N - MARGIN)

# Hawk head in profile facing right (design-grid coordinates); the neck runs off the bottom.
HEAD = [(214, 960), (204, 700), (232, 520), (300, 380), (408, 288), (540, 246), (652, 262),
        (744, 318), (800, 360), (858, 420), (884, 486), (876, 556), (842, 590), (822, 548),
        (780, 520), (722, 536), (668, 586), (612, 680), (572, 800), (556, 960)]
BEAK = [(744, 318), (800, 360), (858, 420), (884, 486), (876, 556), (842, 590), (822, 548),
        (780, 520), (722, 536), (706, 450)]
# Pale cheek and throat running down the front of the neck.
CHEEK = [(640, 470), (712, 452), (736, 540), (668, 586), (612, 680), (572, 800), (556, 960), (452, 960),
         (474, 800), (520, 660), (580, 540)]
# Yellow cere where the beak meets the head.
CERE = [(736, 312), (772, 338), (738, 520), (706, 456)]
BROW = [(530, 352), (640, 318), (748, 330), (720, 372), (632, 372), (560, 396)]
EYE_CENTRE, EYE_R = (640, 410), 40
# Feather chevrons on the back of the head and neck.
FEATHERS = [[(262, 560), (330, 610), (300, 660)], [(246, 690), (316, 740), (286, 790)],
            [(312, 450), (380, 490), (354, 540)]]

AMBER_TOP, AMBER_BOTTOM = (240, 150, 52), (170, 78, 26)
CHEEK_TOP, CHEEK_BOTTOM = (252, 238, 214), (226, 196, 160)
BEAK_TOP, BEAK_BOTTOM = (60, 60, 68), (24, 24, 30)


def s(points):
    return [(x * SS, y * SS) for x, y in points]


def vertical_gradient(size, top, bottom):
    w, h = size
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        grad.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return grad.resize((w, h))


def body_mask():
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle([v * SS for v in BODY], RADIUS * SS, fill=255)
    return m


def led_panel():
    """Grid of LED pixels, brightest where the hawk is looking (upper right)."""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cols, pitch, dot = 11, 76, 50
    start = (N - cols * pitch) / 2 + (pitch - dot) / 2
    focus = (820, 300)
    for r in range(cols):
        for c in range(cols):
            x, y = start + c * pitch, start + r * pitch
            dist = math.hypot(x + dot / 2 - focus[0], y + dot / 2 - focus[1]) / 900
            glow = max(0.0, 1.0 - dist)
            colour = (int(20 + 10 * glow), int(80 + 150 * glow), int(140 + 115 * glow), int(70 + 185 * glow))
            d.rounded_rectangle([x * SS, y * SS, (x + dot) * SS, (y + dot) * SS], 10 * SS, fill=colour)
    halo = layer.filter(ImageFilter.GaussianBlur(14 * SS))
    return Image.alpha_composite(halo, layer)


def build():
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    background = vertical_gradient((S, S), (26, 30, 40), (10, 11, 16)).convert("RGBA")
    canvas.paste(background, (0, 0), body_mask())
    canvas = Image.alpha_composite(canvas, led_panel())

    # Soft shadow behind the hawk so it separates from the LEDs.
    shadow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow).polygon(s([(x + 14, y + 18) for x, y in HEAD]), fill=170)
    shadow = shadow.filter(ImageFilter.GaussianBlur(22 * SS))
    canvas = Image.alpha_composite(canvas, Image.merge("RGBA", (*[Image.new("L", (S, S), 0)] * 3, shadow)))

    head_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(head_mask).polygon(s(HEAD), fill=255)
    canvas.paste(vertical_gradient((S, S), AMBER_TOP, AMBER_BOTTOM).convert("RGBA"), (0, 0), head_mask)

    cheek_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(cheek_mask).polygon(s(CHEEK), fill=255)
    canvas.paste(vertical_gradient((S, S), CHEEK_TOP, CHEEK_BOTTOM).convert("RGBA"), (0, 0),
                 ImageChops.multiply(cheek_mask, head_mask))

    d = ImageDraw.Draw(canvas)
    for chevron in FEATHERS:
        d.line(s(chevron), fill=(120, 52, 18, 255), width=14 * SS, joint="curve")

    beak_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(beak_mask).polygon(s(BEAK), fill=255)
    canvas.paste(vertical_gradient((S, S), BEAK_TOP, BEAK_BOTTOM).convert("RGBA"), (0, 0), beak_mask)
    # Highlight along the top of the beak.
    d.line(s([(752, 330), (806, 372), (856, 432)]), fill=(150, 150, 165, 255), width=8 * SS, joint="curve")

    d.polygon(s(CERE), fill=(255, 206, 50, 255))
    d.polygon(s(BROW), fill=(40, 30, 26, 255))
    ex, ey = EYE_CENTRE
    d.ellipse([(ex - EYE_R) * SS, (ey - EYE_R) * SS, (ex + EYE_R) * SS, (ey + EYE_R) * SS], fill=(255, 226, 120, 255))
    pr = EYE_R * 0.58
    d.ellipse([(ex - pr + 6) * SS, (ey - pr) * SS, (ex + pr + 6) * SS, (ey + pr) * SS], fill=(14, 14, 18, 255))
    d.ellipse([(ex + 4) * SS, (ey - 20) * SS, (ex + 18) * SS, (ey - 6) * SS], fill=(255, 255, 255, 255))

    # Everything stays inside the rounded body; add a faint top rim light.
    canvas.putalpha(ImageChops.multiply(canvas.getchannel("A"), body_mask()))
    rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([v * SS for v in BODY], RADIUS * SS, outline=(255, 255, 255, 40),
                                          width=4 * SS)
    canvas = Image.alpha_composite(canvas, rim)

    return canvas.resize((N, N), Image.LANCZOS)


def main():
    icon = build()
    icon.save(OUT / "displayhawk.png")
    icon.save(OUT / "displayhawk.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    icon.save(OUT / "displayhawk.icns")
    print("Wrote", ", ".join(p.name for p in sorted(OUT.glob("displayhawk.*"))))


if __name__ == "__main__":
    main()

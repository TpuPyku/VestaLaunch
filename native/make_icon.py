"""Draws the program icon and writes app.ico (16, 32, 48, 64 px, 32-bit with AND mask).

Skin concept v0.1: orange rounded square with a faint triangle mosaic, white line sign.
VestaHome: a house; VestaLaunch (its copy of this script): the 2 x 2 "apps" grid.
Windows CE can't read PNG-compressed icon entries, so every size is a plain BMP entry; where CE ignores
the alpha channel, the AND mask (alpha < 50 %) still cuts the shape out.

    python make_icon.py            -> app.ico next to this script
"""
import os
import struct
from PIL import Image, ImageDraw

SIGN = 'apps'                                 # 'home' (VestaHome) or 'apps' (VestaLaunch)
TOP, BOTTOM = (255, 107, 26), (194, 74, 11)   # #ff6b1a -> #c24a0b
SIZES = (16, 32, 48, 64)
PANEL = (0x17, 0x19, 0x1c)                    # tile colour of VestaHome/VestaLaunch: edges blend into it
S = 512                                       # drawing canvas, downscaled for every size


def draw():
    k = S / 256
    bg = Image.new('RGBA', (S, S))
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        ImageDraw.Draw(bg).line([(0, y), (S, y)], fill=c + (255,))
    # faint triangle mosaic, as on the Vesta trim
    tri = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(tri)
    w, h = 96 * k, 84 * k
    for row in range(4):
        for col in range(-1, 7):
            x, y0 = col * w / 2, row * h
            if (row + col) % 2 == 0:
                d.polygon([(x, y0 + h), (x + w / 2, y0), (x + w, y0 + h)], fill=(255, 255, 255, 22))
            else:
                d.polygon([(x, y0), (x + w, y0), (x + w / 2, y0 + h)], fill=(0, 0, 0, 18))
    bg = Image.alpha_composite(bg, tri)
    # rounded square
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([8 * k, 8 * k, 248 * k, 248 * k], radius=52 * k, fill=255)
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    img.paste(bg, (0, 0), mask)
    # the sign: thick white lines, round joints
    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    lw = int(22 * k)

    def line(pts):
        d.line([(x * k, y * k) for x, y in pts], fill=white, width=lw, joint='curve')
        for x, y in (pts[0], pts[-1]):
            d.ellipse([x * k - lw / 2, y * k - lw / 2, x * k + lw / 2, y * k + lw / 2], fill=white)

    if SIGN == 'home':
        line([(52, 128), (128, 60), (204, 128)])
        line([(76, 108), (76, 200), (180, 200), (180, 108)])
    else:
        for x, y in ((62, 62), (138, 62), (62, 138), (138, 138)):
            d.rounded_rectangle([x * k, y * k, (x + 56) * k, (y + 56) * k], radius=12 * k, outline=white,
                                width=int(18 * k))
    return img


def entry(img, n):
    im = img.resize((n, n), Image.LANCZOS)
    px = im.load()
    xor = bytearray()
    for y in range(n - 1, -1, -1):            # bottom-up
        for x in range(n):
            r, g, b, a = px[x, y]
            if a < 128:                       # cut out by the AND mask: XOR must be 0, or CE (which ignores
                xor += bytes((0, 0, 0, 0))    # the alpha) XORs the colour into the screen - dirty corners
            else:                             # no half-transparent edge: blended over the tiles' colour
                xor += bytes(int(c * a / 255 + p * (255 - a) / 255 + 0.5) for c, p in ((b, PANEL[2]),
                             (g, PANEL[1]), (r, PANEL[0]))) + bytes((255,))
    stride = ((n + 31) // 32) * 4
    andm = bytearray()
    for y in range(n - 1, -1, -1):
        row = bytearray(stride)
        for x in range(n):
            if px[x, y][3] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        andm += row
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0)
    return hdr + xor + andm


def main():
    img = draw()
    data = [entry(img, n) for n in SIZES]
    out = struct.pack('<HHH', 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for n, e in zip(SIZES, data):
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(e), offset)
        offset += len(e)
    for e in data:
        out += e
    here = os.path.dirname(os.path.abspath(__file__))
    open(os.path.join(here, 'app.ico'), 'wb').write(out)
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(here, 'obj', 'app_preview.png'))
    print(os.path.join(here, 'app.ico'), len(out))


if __name__ == '__main__':
    main()

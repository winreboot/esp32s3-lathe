"""Make the anti-aliased text fonts for the display's labels and buttons (VLW format, see make_vlw.py for the layout).

Printable ASCII without the space (LovyanGFX advances a fixed width for characters it has no glyph for).
Writes fonts/t_big.vlw (titles, button labels), fonts/t_mid.vlw (values), fonts/t_small.vlw (small print) and
fonts/t_xl.vlw (thread names and the large numbers of the tool screens: capitals, digits and a few signs only).

Usage: make_vlw_text.py <pylib or .> <font.ttf> <out dir>
"""
import sys, struct, os
sys.path.insert(0, sys.argv[1])
from PIL import Image, ImageDraw, ImageFont

TTF, OUT = sys.argv[2], sys.argv[3]
CHARS = ''.join(chr(c) for c in range(33, 127))      # in code-point order: the library finds glyphs by binary search
SIZES = {'t_big': 22, 't_mid': 18, 't_small': 15, 't_xl': 40}   # em size in pixels
XL = ''.join(sorted('#%-./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZx'))

for name, em in SIZES.items():
    CHARS = XL if name == 't_xl' else ''.join(chr(c) for c in range(33, 127))
    f = ImageFont.truetype(TTF, em)
    asc, _ = f.getmetrics()
    glyphs = []
    for ch in CHARS:
        l, t, r, b = f.getbbox(ch)
        w, h = max(1, r - l), max(1, b - t)
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-l, -t), ch, font=f, fill=255)
        glyphs.append((ord(ch), h, w, round(f.getlength(ch)), asc - t, l, im.tobytes()))
    # Font height from the letters people read (capitals, digits, descenders), not from tall brackets, so a
    # middle datum centres ordinary text.
    cap = max(g[4] for g in glyphs if chr(g[0]) in 'HEX0189')
    desc = max([g[1] - g[4] for g in glyphs if chr(g[0]) in 'gpqy'] or [0])
    out = struct.pack('>6I', len(glyphs), 11, cap + desc, 0, cap, desc)
    for u, h, w, adv, dy, dx, _ in glyphs:
        out += struct.pack('>7I', u, h, w, adv, dy & 0xffffffff, dx & 0xffffffff, 0)
    for g in glyphs: out += g[6]
    open(os.path.join(OUT, name + '.vlw'), 'wb').write(out)
    print('%s em %d: cap %d px, descent %d px, "SPINDLE" %d px wide, %d bytes' % (name, em, cap, desc, round(f.getlength('SPINDLE')), len(out)))

# The RPM view's speed: digits 80 px tall, between the mid (70) and big (120) digit fonts of make_vlw.py.
DIG = "-.0123456789"
for em in range(200, 20, -1):
    f = ImageFont.truetype(TTF, em); asc, _ = f.getmetrics()
    if max(asc - f.getbbox(c)[1] for c in "0123456789") <= 80: break
glyphs = []
for ch in DIG:
    l, t, r, b = f.getbbox(ch); im = Image.new('L', (r - l, b - t), 0)
    ImageDraw.Draw(im).text((-l, -t), ch, font=f, fill=255)
    glyphs.append((ord(ch), b - t, r - l, round(f.getlength(ch)), asc - t, l, im.tobytes()))
fa = max(g[4] for g in glyphs); fd = max(0, max(g[1] - g[4] for g in glyphs))
out = struct.pack('>6I', len(glyphs), 11, fa + fd, 0, fa, fd)
for u, h, w, adv, dy, dx, _ in glyphs: out += struct.pack('>7I', u, h, w, adv, dy & 0xffffffff, dx & 0xffffffff, 0)
for g in glyphs: out += g[6]
open(os.path.join(OUT, 'd_rpm.vlw'), 'wb').write(out)
print('d_rpm em %d: digits %d px tall, "2500" %d px wide, %d bytes' % (em, fa, round(f.getlength('2500')), len(out)))

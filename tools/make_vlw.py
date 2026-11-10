"""Make anti-aliased digit fonts (0-9 . -) in VLW format for LovyanGFX, from a TrueType font.

VLW layout (as LovyanGFX's VLWfont::loadFont reads it), all 32-bit big-endian:
  header: glyph count, version, size, unused, ascent, descent
  per glyph: unicode, height, width, x advance, dY (baseline to glyph top), dX, unused
  then the 8-bit alpha bitmaps, one after another in glyph order.
The font-level ascent is set to the digit height and descent to what hangs below the baseline, so a
middle text datum centres the digits exactly.

Usage: make_vlw.py <pylib> <font.ttf> <out dir> <preview.png>
"""
import sys, struct, os
sys.path.insert(0, sys.argv[1])
from PIL import Image, ImageDraw, ImageFont

TTF, OUT, PREVIEW = sys.argv[2], sys.argv[3], sys.argv[4]
CHARS = "0123456789.-"

def measure(em):
    f = ImageFont.truetype(TTF, em)
    asc, desc = f.getmetrics()
    g = {}
    for ch in CHARS:
        l, t, r, b = f.getbbox(ch)
        g[ch] = dict(l=l, t=t, r=r, b=b, adv=f.getlength(ch))
    digit_h = max(asc - g[c]['t'] for c in "0123456789")
    return f, asc, g, digit_h

def width(g, s): return sum(g[c]['adv'] for c in s)

def fit(max_h, limits):
    """Largest em whose digits are <= max_h tall and whose test strings fit their widths."""
    best = None
    for em in range(20, 260):
        f, asc, g, dh = measure(em)
        if dh > max_h or any(width(g, s) > w for s, w in limits): break
        best = em
    return best

def write_vlw(em, path):
    f, asc, g, dh = measure(em)
    glyphs = []
    for ch in CHARS:
        m = g[ch]
        w, h = m['r'] - m['l'], m['b'] - m['t']
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-m['l'], -m['t']), ch, font=f, fill=255)
        glyphs.append((ord(ch), h, w, round(m['adv']), asc - m['t'], m['l'], im.tobytes()))
    font_asc = max(gl[4] for gl in glyphs)
    font_desc = max(0, max(gl[1] - gl[4] for gl in glyphs))
    out = struct.pack('>6I', len(glyphs), 11, font_asc + font_desc, 0, font_asc, font_desc)
    for u, h, w, adv, dy, dx, _ in glyphs:
        assert w < 256 and adv < 256 and -128 <= dx < 128, (u, w, adv, dx)
        out += struct.pack('>7I', u, h, w, adv, dy & 0xffffffff, dx & 0xffffffff, 0)
    for gl in glyphs: out += gl[6]
    open(path, 'wb').write(out)
    return f, g, dh, len(out)

os.makedirs(OUT, exist_ok=True)
sizes = {
    'big':   fit(120, [("8888", 300)]),                      # RPM only: four digits across the screen
    'mid':   fit(72,  [("-8.8888", 270)]),                    # the 80 px rows, and the single-axis views
    'small': fit(58,  [("-88.8888", 270)]),                   # fallback when a value has two digits before the point
}
print('em sizes', sizes)
fonts = {}
for name, em in sizes.items():
    f, g, dh, n = write_vlw(em, os.path.join(OUT, 'd_%s.vlw' % name))
    fonts[name] = (f, g, dh)
    print('%-5s em %3d  digit height %3d px  "0.4820" %3d px  "-1.2530" %3d px  "1240" %3d px  file %d bytes' %
          (name, em, dh, width(g, "0.4820"), width(g, "-1.2530"), width(g, "1240"), n))

# Preview: what the three sizes look like on the display's dark background
pv = Image.new('RGB', (660, 420), (10, 16, 32))
d = ImageDraw.Draw(pv)
y = 10
for name, s in (('big', '1240'), ('mid', '0.4820'), ('mid', '-1.2530'), ('small', '-12.3456')):
    f, g, dh = fonts[name]
    d.text((20, y), s, font=f, fill=(56, 189, 248) if name != 'big' else (34, 197, 94))
    y += int(f.size * 1.1)
pv.save(PREVIEW)
print('preview', PREVIEW)

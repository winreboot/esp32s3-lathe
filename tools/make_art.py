#!/usr/bin/env python3
"""Draws the display's background artwork: art/*.png, 320 x 240, one per screen.

The firmware cannot afford soft glows and gradients at run time, so everything that never changes on a screen
(the glowing background, panels, buttons, icons, fixed labels) is drawn here once, and the firmware paints the
live numbers, values and the graph on top. Coordinates here and in src/main.cpp (the draw...Art functions) must
agree; both are in display pixels.

    python tools/make_art.py <Oswald-Bold.ttf> [preview dir]

With a preview dir it also writes each screen with sample live values drawn in, to check the look on a PC.
Needs Pillow and numpy.
"""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
import numpy as np

TTF = sys.argv[1]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'art')
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None
S = 4                                    # drawn at 4x and scaled down, for smooth edges
W, H = 320, 240

BLUE, GREEN, RED = (46, 132, 230), (40, 235, 110), (240, 70, 70)
BLACK = False                            # set per pass: the black theme keeps the glowing edges on a black ground
def deep(): return ((7, 9, 13), (2, 3, 5)) if BLACK else ((8, 22, 46), (5, 14, 32))      # recessed panels (graph, info)
WHITE, AMBER, MUTED = (236, 242, 250), (251, 191, 36), (143, 161, 194)


def font(px): return ImageFont.truetype(TTF, int(px * S))


class Art:
    def __init__(self):
        # background: dark navy, lighter in the middle, with a soft blue glow from the top
        y, x = np.mgrid[0:H * S, 0:W * S].astype(np.float32)
        cx, cy = W * S / 2, H * S * 0.42
        d = np.sqrt(((x - cx) / (W * S * 0.75)) ** 2 + ((y - cy) / (H * S * 0.85)) ** 2)
        g = np.clip(1 - d, 0, 1) ** 1.6
        base = np.stack([0 + 3 * g, 0 + 6 * g, 0 + 12 * g], axis=-1) if BLACK else np.stack([4 + 6 * g, 10 + 22 * g, 24 + 44 * g], axis=-1)
        self.im = Image.fromarray(base.astype(np.uint8), 'RGB')
        self.glow = Image.new('RGB', (W * S, H * S), (0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
        self.gd = ImageDraw.Draw(self.glow)

    def _box(self, x, y, w, h): return [x * S, y * S, (x + w) * S - 1, (y + h) * S - 1]

    def panel(self, x, y, w, h, r=6, col=BLUE, strength=1.0, top=None, bot=None, accent=None):
        """A raised panel: vertical gradient fill, bright thin edge, soft outer glow in the edge colour."""
        top = top or tuple(int(c * (0.11 if BLACK else 0.20) + b) for c, b in zip(col, (6, 8, 12) if BLACK else (4, 12, 30)))
        bot = bot or tuple(int(c * (0.03 if BLACK else 0.07) + b) for c, b in zip(col, (1, 2, 3) if BLACK else (3, 8, 20)))
        grad = np.linspace(0, 1, h * S, dtype=np.float32)[:, None, None]
        fill = (np.array(top, np.float32) * (1 - grad) + np.array(bot, np.float32) * grad).repeat(w * S, axis=1)
        tile = Image.fromarray(fill.astype(np.uint8), 'RGB')
        mask = Image.new('L', (w * S, h * S), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, w * S - 1, h * S - 1], r * S, fill=255)
        self.im.paste(tile, (x * S, y * S), mask)
        self.d = ImageDraw.Draw(self.im)
        edge = tuple(min(255, int(c * 0.82 + 30)) for c in col)
        self.d.rounded_rectangle(self._box(x, y, w, h), r * S, outline=edge, width=max(1, S * 3 // 4))
        # a faint highlight under the top edge makes it look lit from above
        self.d.line([(x + r) * S, (y + 1.4) * S, (x + w - r) * S, (y + 1.4) * S], fill=tuple(min(255, int(c * 0.45 + 40)) for c in col), width=max(1, S // 3))
        self.gd.rounded_rectangle(self._box(x, y, w, h), r * S, outline=tuple(int(c * 0.9 * strength) for c in col), width=S * 2)
        if accent:
            self.d.rounded_rectangle(self._box(x + 3, y + 4, 3, h - 8), 1.5 * S, fill=accent)
            self.gd.rounded_rectangle(self._box(x + 2, y + 3, 5, h - 6), 2 * S, fill=tuple(int(c * 0.8) for c in accent))

    def divider(self, x, y0, y1, col=BLUE):
        self.d.line([x * S, y0 * S, x * S, y1 * S], fill=tuple(int(c * 0.7) for c in col), width=max(1, S // 2))

    def text(self, s, x, y, px, col=WHITE, anchor='lm', glow=0.0, spacing=0):
        f = font(px)
        if spacing:
            tw = sum(self.d.textlength(ch, font=f) for ch in s) + spacing * S * (len(s) - 1)
            cx = x * S - (tw / 2 if anchor[0] == 'm' else tw if anchor[0] == 'r' else 0)
            for ch in s:
                self.d.text((cx, y * S), ch, font=f, fill=col, anchor='l' + anchor[1]); cx += self.d.textlength(ch, font=f) + spacing * S
            return
        self.d.text((x * S, y * S), s, font=f, fill=col, anchor=anchor)
        if glow: self.gd.text((x * S, y * S), s, font=f, fill=tuple(int(c * glow) for c in col), anchor=anchor)

    def width(self, s, px): return self.d.textlength(s, font=font(px)) / S

    # ---- icons: thin line drawings, centred on (cx, cy)
    def _ln(self, pts, col, w=1.5):
        self.d.line([(px * S, py * S) for px, py in pts], fill=col, width=int(w * S), joint='curve')
    def _arc(self, cx, cy, r, a0, a1, col, w=1.5):
        self.d.arc([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], a0, a1, fill=col, width=int(w * S))
    def _ring(self, cx, cy, r, col, w=1.5): self._arc(cx, cy, r, 0, 360, col, w)
    def _dot(self, cx, cy, r, col): self.d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=col)

    def icon(self, name, cx, cy, col=WHITE, k=1.0):
        c = col
        if name == 'gear':
            for i in range(8):
                a = i * math.pi / 4
                self._ln([(cx + 6.2 * k * math.cos(a), cy + 6.2 * k * math.sin(a)), (cx + 9.5 * k * math.cos(a), cy + 9.5 * k * math.sin(a))], c, 2.6)
            self._ring(cx, cy, 6.8 * k, c, 2.2); self._ring(cx, cy, 2.8 * k, c, 1.4)
        elif name == 'gauge':
            self._arc(cx, cy + 3, 10 * k, 180, 360, c, 1.8)
            for a in (200, 235, 270, 305, 340):
                r = math.radians(a); self._ln([(cx + 7.2 * k * math.cos(r), cy + 3 + 7.2 * k * math.sin(r)), (cx + 8.6 * k * math.cos(r), cy + 3 + 8.6 * k * math.sin(r))], c, 1.2)
            self._ln([(cx, cy + 3), (cx + 5 * k, cy - 3 * k)], c, 1.8); self._dot(cx, cy + 3, 1.8 * k, c)
        elif name == 'view':
            self.d.rounded_rectangle([(cx - 10) * S, (cy - 8) * S, (cx + 10) * S, (cy + 5) * S], 2 * S, outline=c, width=int(1.5 * S))
            self._ln([(cx - 3, cy + 5), (cx - 4, cy + 9), (cx + 4, cy + 9), (cx + 3, cy + 5)], c, 1.4)
            self._ln([(cx - 6, cy + 1), (cx - 2, cy - 2), (cx + 1, cy), (cx + 6, cy - 5)], AMBER, 1.6)
        elif name == 'palette':
            self._arc(cx, cy, 9.5 * k, 20, 320, c, 1.7)
            self._ln([(cx + 9, cy + 3.2), (cx + 4, cy + 3), (cx + 3.5, cy + 6), (cx + 7.2, cy + 6.3)], c, 1.7)
            for dx, dy in ((-4.5, -3), (0, -5), (4.5, -3)): self._dot(cx + dx, cy + dy, 1.5, AMBER)
            self._dot(cx - 4.5, cy + 2.5, 1.5, (120, 190, 255))
        elif name in ('zero_x', 'zero_z'):
            self._ring(cx, cy, 6.5, c, 1.5)
            for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)): self._ln([(cx + dx * 6.5, cy + dy * 6.5), (cx + dx * 10.5, cy + dy * 10.5)], c, 1.5)
            self.text(name[-1].upper(), cx, cy, 9.5, c, 'mm')
        elif name == 'span':
            self._ln([(cx - 9, cy - 6), (cx - 9, cy + 6)], c); self._ln([(cx + 9, cy - 6), (cx + 9, cy + 6)], c)
            self._ln([(cx - 7, cy), (cx + 7, cy)], c); self._ln([(cx - 4, cy - 3), (cx - 7, cy), (cx - 4, cy + 3)], c); self._ln([(cx + 4, cy - 3), (cx + 7, cy), (cx + 4, cy + 3)], c)
        elif name == 'screen':
            self.d.rounded_rectangle([(cx - 5) * S, (cy - 8) * S, (cx + 10) * S, (cy + 2) * S], 1.5 * S, outline=c, width=int(1.4 * S))
            self.d.rounded_rectangle([(cx - 10) * S, (cy - 3) * S, (cx + 5) * S, (cy + 8) * S], 1.5 * S, outline=c, width=int(1.4 * S), fill=(4, 6, 9) if BLACK else (8, 22, 46))
        elif name == 'touch':
            self._arc(cx - 1, cy - 3, 8.5, 150, 30, c, 1.4); self._arc(cx - 1, cy - 3, 5, 150, 30, c, 1.4)
            self._ln([(cx - 1, cy - 3), (cx - 1, cy + 5)], c, 2.8)
            self._ln([(cx - 1, cy + 4), (cx + 6, cy + 4.5), (cx + 6, cy + 9), (cx - 3, cy + 9), (cx - 6, cy + 5)], c, 1.5)
        elif name == 'info':
            self._ring(cx, cy, 9.5 * k, c, 1.7); self._dot(cx, cy - 4.5 * k, 1.4 * k, c); self._ln([(cx, cy - 1.2 * k), (cx, cy + 5 * k)], c, 2.2 * k)
        elif name == 'spindle':
            self.d.rounded_rectangle([(cx - 7) * S, (cy - 7) * S, (cx + 3) * S, (cy + 7) * S], 2 * S, outline=c, width=int(1.5 * S), fill=(30, 70, 120))
            self._ln([(cx - 10, cy - 4), (cx - 10, cy + 4)], c, 1.6); self._ln([(cx - 4, cy - 6), (cx - 4, cy + 6)], c, 1.0)
            self.d.rectangle([(cx + 3) * S, (cy - 2.5) * S, (cx + 10) * S, (cy + 2.5) * S], outline=c, width=int(1.4 * S), fill=(30, 70, 120))
        elif name == 'threads':
            for i in range(5): self._ln([(cx - 7, cy - 6 + i * 3.4), (cx + 7, cy - 8 + i * 3.4)], c, 1.5)
        elif name == 'menu':
            for i in (-1, 0, 1): self._ln([(cx - 7, cy + i * 4.5), (cx + 7, cy + i * 4.5)], c, 1.8)
        elif name == 'close':
            self._ln([(cx - 5, cy - 5), (cx + 5, cy + 5)], c, 1.8); self._ln([(cx - 5, cy + 5), (cx + 5, cy - 5)], c, 1.8)
        elif name == 'back':
            self._ln([(cx + 6, cy), (cx - 6, cy)], c, 1.8); self._ln([(cx - 1, cy - 5), (cx - 6, cy), (cx - 1, cy + 5)], c, 1.8)
        elif name == 'power':
            self._arc(cx, cy + 1, 7.5, -60, 240, c, 1.8); self._ln([(cx, cy - 8), (cx, cy)], c, 1.8)

    def finish(self):
        """Adds the glow layer and scales down to the display size."""
        g = self.glow.filter(ImageFilter.GaussianBlur(3.2 * S))
        g2 = self.glow.filter(ImageFilter.GaussianBlur(1.2 * S))
        im = ImageChops.add(self.im, Image.eval(g, lambda v: int(v * 0.55)))
        im = ImageChops.add(im, Image.eval(g2, lambda v: int(v * 0.30)))
        return im.resize((W, H), Image.LANCZOS)


# ------------------------------------------------------------------ the screens (coordinates shared with main.cpp)
MB = [(6 + (i % 3) * 104, 36 + (i // 3) * 67, 100, 63) for i in range(9)]
KP = [(6 + (i % 4) * 78, 64 + (i // 4) * 44, 74, 40) for i in range(16)]
VB = [(6, 6, 152, 52), (162, 6, 152, 52), (6, 64, 152, 52), (162, 64, 152, 52), (6, 122, 152, 52), (162, 122, 152, 52)]


def plain():
    return Art()

def rpm():
    a = Art()
    a.panel(4, 4, 312, 32, 7, accent=GREEN)
    a.icon('spindle', 24, 20); a.text('SPINDLE', 40, 20, 20, WHITE, 'lm', glow=.25)
    a.divider(120, 10, 30); a.divider(196, 10, 30)
    a.text('RPM', 160, 132, 10, (200, 214, 235), 'mm', spacing=2.2)
    for x0, x1 in ((58, 136), (184, 262)):
        for i in range(x0, x1): a.d.point((0, 0))
        a.d.line([x0 * S, 132 * S, x1 * S, 132 * S], fill=(30, 84, 150), width=S // 2)
    a.panel(4, 142, 312, 64, 6, strength=.8, top=deep()[0], bot=deep()[1])
    for i in range(1, 8):                                           # dotted time lines inside the plot area
        x = 34 + 276 * i // 8
        for y in range(149, 200, 3): a.d.point((x * S, y * S), fill=(26, 58, 104)); a.d.rectangle([x * S, y * S, x * S + S // 2, y * S + S // 2], fill=(26, 58, 104))
    for y in (148, 200): a.d.line([34 * S, y * S, 310 * S, y * S], fill=(24, 60, 112), width=S // 2)
    a.panel(4, 210, 312, 26, 6, strength=.8)
    a.icon('threads', 17, 223, (190, 210, 235), .9)
    a.divider(180, 215, 231); a.text('LOAD', 188, 223, 14, WHITE, 'lm')
    a.divider(274, 215, 231)
    a.panel(280, 213, 32, 20, 5, strength=.9); a.icon('menu', 296, 223, (150, 205, 255))
    return a

def menu():
    a = Art()
    a.panel(4, 3, 150, 30, 7, accent=GREEN)
    a.icon('gear', 24, 18, (190, 220, 255), .95); a.divider(38, 9, 27); a.text('SETTINGS', 46, 18, 18, WHITE, 'lm', glow=.25)
    a.panel(158, 3, 74, 28, 7, GREEN, 1.1); a.text('TOOLS', 195, 17, 15, WHITE, 'mm')
    a.panel(236, 3, 78, 28, 7); a.icon('close', 252, 17, WHITE); a.text('CLOSE', 264, 17, 14, WHITE, 'lm')
    cells = [('gauge', 'SET RPM', True, GREEN), ('view', 'VIEW', True, BLUE), ('palette', 'COLOURS', True, BLUE), ('zero_x', 'ZERO X', False, BLUE), ('zero_z', 'ZERO Z', False, BLUE),
             ('span', 'X SHOWS', True, BLUE), ('screen', 'SCREEN', True, BLUE), ('touch', 'TOUCH', True, BLUE), ('info', 'INFO', True, BLUE)]
    for (x, y, w, h), (ic, label, has_value, col) in zip(MB, cells):
        a.panel(x, y, w, h, 7, col, strength=1.2 if col == GREEN else 1.0)
        a.icon(ic, x + 17, y + h // 2, (170, 255, 200) if col == GREEN else (200, 225, 255))
        a.divider(x + 32, y + 12, y + h - 12, col)
        a.text(label, x + 38, y + (22 if has_value else h // 2), 15 if has_value else 17, WHITE, 'lm')
    return a

def keypad():
    a = Art()
    a.panel(6, 4, 308, 56, 8, accent=GREEN); a.text('SET RPM', 18, 21, 22, WHITE, 'lm', glow=.25); a.divider(196, 12, 52)
    lbl = ['7', '8', '9', 'CLEAR', '4', '5', '6', 'KNOB', '1', '2', '3', 'CANCEL', '0', '00', 'DEL', 'OK']
    for (x, y, w, h), l in zip(KP, lbl):
        if l == 'OK': a.panel(x, y, w, h, 7, GREEN, 1.3, top=(22, 150, 66), bot=(10, 84, 38)); a.text(l, x + w / 2, y + h / 2, 23, WHITE, 'mm')
        else:
            a.panel(x, y, w, h, 7, BLUE, 1.2 if l == 'KNOB' else 0.9)
            a.text(l, x + w / 2, y + h / 2, 24 if len(l) <= 2 else 15, AMBER if l == 'KNOB' else WHITE, 'mm')
    return a

def info():
    a = Art()
    a.panel(4, 3, 312, 30, 7, accent=GREEN); a.icon('info', 24, 18, (190, 220, 255), .9); a.divider(38, 9, 27); a.text('ESP32 LATHE', 46, 18, 20, WHITE, 'lm', glow=.25)
    a.panel(4, 38, 312, 142, 7, strength=.8, top=deep()[0], bot=deep()[1])
    a.panel(6, 186, 152, 48, 8); a.icon('back', 52, 210); a.text('BACK', 68, 210, 20, WHITE, 'lm')
    a.panel(162, 186, 152, 48, 8, RED, 1.1); a.icon('power', 204, 210); a.text('REBOOT', 220, 210, 20, WHITE, 'lm')
    return a

def view():
    a = Art()
    for (x, y, w, h), l in zip(VB, ['EVERYTHING', 'RPM ONLY', 'X ONLY', 'Z ONLY', 'X AND Z', 'BACK']):
        a.panel(x, y, w, h, 8); a.text(l, x + w / 2, y + h / 2, 19, WHITE, 'mm')
    a.text('Pick what fills the screen. Tap it to switch.', 8, 196, 13, MUTED, 'lm')
    a.text('No axis encoders fitted yet? Choose RPM only.', 8, 216, 13, MUTED, 'lm')
    return a

def main_rows():
    a = Art()
    for i, col in enumerate(((56, 189, 248), (56, 189, 248), GREEN)):
        a.panel(2, i * 80 + 2, 316, 76, 7, strength=.8, accent=col)
    return a

def xz_rows():
    a = Art()
    for i in range(2): a.panel(2, i * 122 + 2, 316, 114, 7, strength=.8, accent=(56, 189, 248))
    return a


def titlebar(a, icon, text, px=18, right_div=None):
    a.panel(4, 3, 312, 28, 7, accent=GREEN); a.icon(icon, 22, 17, (190, 220, 255), .85); a.divider(36, 8, 26)
    a.text(text, 43, 17, px, WHITE, 'lm', glow=.25)
    if right_div: a.divider(right_div, 8, 26)

def lab(a, s, x, y, anchor='lm', px=10): a.text(s, x, y, px, (170, 190, 220), anchor, spacing=.4)

def thread():
    a = Art(); titlebar(a, 'threads', 'THREAD ASSIST', 18, 222)
    a.panel(4, 35, 312, 48, 7); a.icon('threads', 30, 59, (200, 225, 255), 1.0); a.divider(58, 42, 76)
    a.panel(4, 87, 312, 36, 6, strength=.8)
    for i, t in enumerate(('MAJOR DIA', 'PITCH', 'LENGTH', 'PASSES')):
        lab(a, t, 10 + i * 78, 95)
        if i: a.divider(4 + i * 78, 92, 118)
    a.panel(4, 127, 100, 60, 6, strength=.8, top=deep()[0], bot=deep()[1]); lab(a, 'THREAD DEPTH', 10, 135)
    a.panel(108, 127, 208, 60, 6, strength=.8, top=deep()[0], bot=deep()[1]); lab(a, 'CUT DEPTH AFTER EACH PASS', 114, 135)
    for i, (t, col) in enumerate((('PRESETS', BLUE), ('PASSES', BLUE), ('SEND', GREEN), ('BACK', BLUE))):
        x = 4 + i * 78
        if col == GREEN: a.panel(x, 206, 76, 30, 7, GREEN, 1.3, top=(22, 150, 66), bot=(10, 84, 38))
        else: a.panel(x, 206, 76 if i < 3 else 78, 30, 7)
        a.text(t, x + 38, 221, 16, WHITE, 'mm')
    return a

def diag():
    a = Art(); titlebar(a, 'spindle', 'SPINDLE DIAGNOSTICS', 16, 196)
    a.panel(4, 35, 312, 58, 7); a.icon('gauge', 27, 54, (200, 225, 255), .9); a.text('RPM', 27, 78, 12, WHITE, 'mm'); a.divider(50, 43, 85); a.divider(206, 43, 85)
    lab(a, 'DEVIATION', 261, 47, 'mm')
    a.panel(4, 97, 312, 50, 6, strength=.8, top=deep()[0], bot=deep()[1])
    for y in (102, 142): a.d.line([34 * S, y * S, 310 * S, y * S], fill=(24, 60, 112), width=S // 2)
    for i, t in enumerate(('STABILITY', 'SENSOR', 'NOISE', 'MISSED', 'ACCEL', 'LOAD')):
        x, y = 4 + (i % 3) * 105, 151 + (i // 3) * 28
        a.panel(x, y, 102, 25, 5, strength=.7); lab(a, t, x + 6, y + 13)
    a.panel(240, 207, 76, 29, 7); a.icon('back', 258, 221); a.text('BACK', 270, 221, 15, WHITE, 'lm')
    return a

def mat():
    a = Art(); titlebar(a, 'view', 'MATERIAL & SPEED', 18, 222)
    for i, t in enumerate(('ALUMINUM', 'STEEL', 'STAINLESS', 'BRASS')):
        a.panel(4 + i * 79, 35, 75, 30, 6); a.text(t, 4 + i * 79 + 37.5, 50, 13, WHITE, 'mm')
    for i, t in enumerate(('WORK DIA', 'TOOL', 'CUT')):
        a.panel(4 + i * 106, 69, 100, 44, 6, strength=.8); lab(a, t, 4 + i * 106 + 50, 78, 'mm')
    a.panel(4, 117, 312, 84, 7); a.text('SUGGESTED', 14, 137, 13, WHITE, 'lm'); a.text('RPM', 14, 157, 22, WHITE, 'lm'); a.divider(96, 124, 168)
    a.panel(10, 174, 300, 22, 5, strength=.6, top=deep()[0], bot=deep()[1])
    a.panel(4, 206, 100, 30, 7, GREEN, 1.3, top=(22, 150, 66), bot=(10, 84, 38)); a.text('APPLY', 54, 221, 16, WHITE, 'mm')
    a.panel(108, 206, 104, 30, 7); a.text('USE X DRO', 160, 221, 15, WHITE, 'mm')
    a.panel(216, 206, 100, 30, 7); a.text('BACK', 266, 221, 16, WHITE, 'mm')
    return a

def numpad():
    a = Art()
    a.panel(6, 4, 308, 56, 8, accent=GREEN); a.divider(196, 12, 52)
    lbl = ['7', '8', '9', 'CLEAR', '4', '5', '6', '', '1', '2', '3', 'CANCEL', '0', '.', 'DEL', 'OK']
    for (x, y, w, h), l in zip(KP, lbl):
        if l == 'OK': a.panel(x, y, w, h, 7, GREEN, 1.3, top=(22, 150, 66), bot=(10, 84, 38)); a.text(l, x + w / 2, y + h / 2, 23, WHITE, 'mm')
        else: a.panel(x, y, w, h, 7, BLUE, 0.9); a.text(l, x + w / 2, y + h / 2, 24 if len(l) <= 2 else 15, WHITE, 'mm')
    return a


def tools():
    a = Art(); titlebar(a, 'gear', 'TOOLS', 18)
    rows = (('threads', 'THREAD ASSIST', 'Pick a thread, send the program to UCCNC'), ('spindle', 'SPINDLE DIAGNOSTICS', 'Speed hold, sensor health, missed pulses'),
            ('view', 'MATERIAL & SPEED', 'Suggested RPM for the material and the tool'))
    for i, (ic, t, sub) in enumerate(rows):
        y = 36 + i * 56
        a.panel(4, y, 312, 52, 7); a.icon(ic, 30, y + 26, (200, 225, 255), 1.0); a.divider(56, y + 8, y + 44)
        a.text(t, 66, y + 18, 20, WHITE, 'lm', glow=.2); a.text(sub, 66, y + 38, 13, (251, 191, 36), 'lm')
    a.panel(4, 206, 312, 30, 7); a.icon('back', 132, 221); a.text('BACK', 146, 221, 16, WHITE, 'lm')
    return a

def pick():
    a = Art(); titlebar(a, 'threads', 'THREAD PRESETS', 18)
    for i, t in enumerate(('METRIC', 'INCH', 'BACK')):
        a.panel(4 + i * 104, 35, 100 if i < 2 else 104, 28, 6); a.text(t, 4 + i * 104 + 50, 49, 15, WHITE, 'mm')
    for i in range(12): a.panel(4 + (i % 4) * 78, 68 + (i // 4) * 56, 76, 52, 6, strength=.8)
    return a


SCREENS = dict(plain=plain, rpm=rpm, menu=menu, keypad=keypad, info=info, view=view, main=main_rows, xz=xz_rows, thread=thread, diag=diag, mat=mat, numpad=numpad, tools=tools, pick=pick)


# ------------------------------------------------------------------ previews: the art with sample live values, as the firmware draws them
def preview(name, im, alarm=None):
    im = im.resize((W * S, H * S), Image.LANCZOS); d = ImageDraw.Draw(im)
    T = lambda s, x, y, px, col, an='lm': d.text((x * S, y * S), s, font=font(px), fill=col, anchor=an)
    def num(s, x, y, hpx, col, an):
        size = hpx * 1.6
        while True:
            f = font(size); b = f.getbbox('0')
            if b[3] - b[1] <= hpx * S: break
            size -= .5
        d.text((x * S, y * S), s, font=f, fill=col, anchor=an)
    if name == 'rpm':
        d.rounded_rectangle([126 * S, 9 * S, 188 * S, 31 * S], 11 * S, fill=(16, 92, 44), outline=(40, 235, 110), width=S)
        T('FWD', 157, 20, 15, WHITE, 'mm'); T('800', 308, 20, 22, WHITE, 'rm'); T('PRESET', 308 - d.textlength('800', font=font(22)) / S - 7, 21, 15, AMBER, 'rm')
        num('798', 160, 81, 80, (34, 230, 100), 'mm')
        rp, ld = [], []
        for i in range(150):
            t = i / 149; cut = 1 / (1 + math.exp(-(t - .45) * 40)) - 1 / (1 + math.exp(-(t - .82) * 40))
            droop = .07 * (math.exp(-max(0, t - .45) * 14) if t >= .45 else 0) - .05 * (math.exp(-max(0, t - .82) * 14) if t >= .82 else 0)
            rp.append(800 * (1 - droop) + math.sin(i * 1.7) * 3); ld.append(30 + 34 * cut + math.sin(i * .9) * 1.5)
        px0, pw, py0, ph = 34, 276, 148, 52
        for x in range(px0, px0 + pw, 6): d.rectangle([x * S, (py0 + ph // 2) * S, (x + 3) * S, (py0 + ph // 2) * S + S - 1], fill=(148, 163, 184))
        X = lambda i: (px0 + (pw - 1) * i / 149) * S
        d.line([(X(i), (py0 + ph - 1 - ld[i] * (ph - 1) / 100) * S) for i in range(150)], fill=(245, 158, 11), width=S)
        d.line([(X(i), (py0 + ph - 1 - (min(960, max(640, rp[i])) - 640) / 320 * (ph - 1)) * S) for i in range(150)], fill=(34, 230, 100), width=2 * S)
        T('960', 10, 152, 9, MUTED); T('640', 10, 196, 9, MUTED)
        T(alarm or '11 ER 1.25 ISO', 30, 223, 15, (251, 146, 60) if alarm else AMBER); T('31%', 270, 223, 20, AMBER, 'rm')
    if name == 'menu':
        for (x, y, w, h), v in zip(MB, ['800 rpm', 'RPM only', 'dark blue', None, None, 'diameter', 'normal', 'calibrate', 'reboot']):
            if v: T(v, x + 38, y + 43, 15, AMBER)
    if name == 'keypad':
        T('60 to 2500. Knob starts it.', 18, 44, 15, AMBER); num('800', 306, 32, 44, (34, 230, 100), 'rm')
    if name == 'info':
        for i, (k, v) in enumerate([('Firmware', 'v1.14.0'), ('Wi-Fi', '192.168.1.50   -48 dBm'), ('Address', 'http://esp32-lathe.local'), ('CNC', 'online'), ('Spindle run', '152 h 36 min'), ('Powered on', '487 h 18 min')]):
            T(k, 14, 54 + i * 22, 15, MUTED); T(v, 120, 54 + i * 22, 15, WHITE)
    if name == 'view':
        x, y, w, h = VB[1]; d.rounded_rectangle([x * S, y * S, (x + w) * S, (y + h) * S], 8 * S, outline=(40, 235, 110), width=2 * S)
    if name == 'main':
        for i, (n, sub, v, col) in enumerate([('X', 'DIA', '1.2480', (56, 189, 248)), ('Z', 'LONG', '-0.8750', (56, 189, 248)), ('S', 'FWD', '1196', (34, 230, 100))]):
            y = i * 80; T(n, 13, y + 25, 22, col); T(sub, 13, y + 58, 15, WHITE)
            num(v, 314 if i < 2 else 210, y + 40, 58 if i < 2 else 58, col, 'rm')
        T('SET 1200', 216, 178, 15, WHITE); d.rounded_rectangle([216 * S, 192 * S, 312 * S, 202 * S], 3 * S, fill=(30, 42, 68)); d.rounded_rectangle([216 * S, 192 * S, 262 * S, 202 * S], 3 * S, fill=(34, 230, 100)); T('CNMG 432', 216, 221, 15, AMBER)
    if name == 'thread':
        T('RPM', 310 - d.textlength('300', font=font(22)) / S - 6, 18, 15, AMBER, 'rm'); T('300', 310, 17, 22, WHITE, 'rm')
        T('M10 x 1.25', 186, 59, 40, (34, 230, 100), 'mm')
        for i, v in enumerate(('0.3937"', '1.25 mm', '0.750"', '11 + 2')): T(v, 10 + i * 78, 110, 18, AMBER)
        T('0.0302 in', 10, 155, 22, (34, 230, 100)); T('0.767 mm', 10, 175, 15, MUTED)
        for i, v in enumerate('.0091 .0129 .0158 .0182 .0204 .0223 .0241 .0257 .0273 .0288 .0302'.split()): T(v, 114 + (i % 5) * 40, 150 + (i // 5) * 13, 15, AMBER)
        T('Sent as job 1. In UCCNC run M20200.', 160, 196, 15, (34, 230, 100), 'mm')
    if name == 'diag':
        T('TARGET', 310 - d.textlength('800', font=font(22)) / S - 6, 18, 15, AMBER, 'rm'); T('800', 310, 17, 22, WHITE, 'rm')
        T('798', 128, 64, 40, (34, 230, 100), 'mm'); T('-0.3%', 261, 70, 22, (34, 230, 100), 'mm')
        px0, pw, py0, ph = 34, 276, 102, 40
        d.line([px0 * S, (py0 + ph / 2) * S, (px0 + pw) * S, (py0 + ph / 2) * S], fill=(245, 158, 11), width=S)
        d.line([((px0 + i * pw / 149) * S, (py0 + ph / 2 + math.sin(i * .21) * 2.2 + math.sin(i * 1.3) * .8) * S) for i in range(150)], fill=(34, 230, 100), width=2 * S)
        T('960', 9, 106, 12, MUTED); T('640', 9, 138, 12, MUTED)
        for i, (v, col) in enumerate((('99.2%', AMBER), ('HEALTHY', (34, 230, 100)), ('LOW', (34, 230, 100)), ('0', AMBER), ('1.2 s', AMBER), ('31%', AMBER))):
            T(v, 4 + (i % 3) * 105 + 96, 151 + (i // 3) * 28 + 13, 15, col, 'rm')
        d.rounded_rectangle([4 * S, 207 * S, 234 * S, 236 * S], 7 * S, fill=(12, 74, 36), outline=(40, 235, 110), width=S)
        T('WITHIN TOLERANCE', 119, 221, 20, (40, 235, 110), 'mm')
    if name == 'mat':
        T('PRESET', 310 - d.textlength('800', font=font(22)) / S - 6, 18, 15, AMBER, 'rm'); T('800', 310, 17, 22, WHITE, 'rm')
        d.rounded_rectangle([83 * S, 35 * S, 158 * S, 65 * S], 6 * S, outline=(40, 235, 110), width=2 * S)
        for i, v in enumerate(('1.500 in', 'CARBIDE', 'FINISH')): T(v, 4 + i * 106 + 50, 97, 20, (34, 230, 100) if i == 0 else AMBER, 'mm')
        T('2160', 196, 146, 40, (34, 230, 100), 'mm')
        T('850 SFM. Feed .002-.004/rev, cut .005-.010 in', 160, 185, 15, WHITE, 'mm')
    if name == 'numpad':
        T('WORK DIAMETER', 18, 21, 22, WHITE); T('inches', 18, 44, 15, AMBER); num('1.500', 306, 32, 40, (34, 230, 100), 'rm')
        x, y, w, h = KP[7]; T('X DRO', x + w / 2, y + h / 2, 15, AMBER, 'mm')
    if name == 'pick':
        d.rounded_rectangle([4 * S, 35 * S, 104 * S, 63 * S], 6 * S, outline=(40, 235, 110), width=2 * S)
        P = (('M3', '0.5'), ('M4', '0.7'), ('M5', '0.8'), ('M6', '1.0'), ('M8', '1.25'), ('M8', '1.0'), ('M10', '1.5'), ('M10', '1.25'), ('M12', '1.75'), ('M12', '1.25'), ('M14', '2.0'), ('M16', '2.0'))
        for i, (n, pt) in enumerate(P):
            x, y = 4 + (i % 4) * 78, 68 + (i // 4) * 56
            T(n, x + 38, y + 18, 22, WHITE, 'mm'); T(pt + ' mm', x + 38, y + 38, 15, AMBER, 'mm')
        d.rounded_rectangle([238 * S, 124 * S, 314 * S, 176 * S], 6 * S, outline=(40, 235, 110), width=2 * S)
    if name == 'xz':
        for i, (n, sub, v) in enumerate([('X', 'DIA', '1.2480'), ('Z', 'LONG', '-0.8750')]):
            y = i * 122; T(n, 13, y + 25, 22, (56, 189, 248)); T(sub, 13, y + 52, 15, WHITE); num(v, 314, y + 59, 70, (56, 189, 248), 'rm')
    return im.resize((W * 2, H * 2), Image.LANCZOS)


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    if PREVIEW: os.makedirs(PREVIEW, exist_ok=True)
    for BLACK, suffix in ((False, ''), (True, '_k')):          # dark blue, then black: art/<name>.png and art/<name>_k.png
        for name, fn in SCREENS.items():
            im = fn().finish()
            p = os.path.join(OUT, name + suffix + '.png'); im.save(p, optimize=True)
            print('%-9s %6d bytes' % (name + suffix, os.path.getsize(p)))
            if PREVIEW: preview(name, im).save(os.path.join(PREVIEW, name + suffix + '.png'))
            if PREVIEW and name == 'rpm': preview(name, im, 'Soft limit (Z) close').save(os.path.join(PREVIEW, 'rpm_alarm' + suffix + '.png'))

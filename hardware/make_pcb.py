"""Interface board for the ESP32-S3 lathe controller: layout, design-rule checks and fabrication files.

Two-sided board for CNC isolation milling, without plated holes. Components sit on the top.
Bottom: every signal trace, and a ground fill (the copper left over after isolating every other net),
which reaches every GND pad without traces.
Top: two traces that carry +12V and 3V3 across the bottom traces (they replace the old wire links),
joined to the bottom through via pins (a bare wire soldered on both sides), and a ground fill with
a copper-free ring round every hole that is not GND, so no lead can short to it. The top fill
is tied to bottom ground by GND via pins. Neither side has copper within CREEP of the lathe side, and
the straight-line gap through the slot is at least AIR.
Two alignment holes on the vertical centre line locate the board when it is flipped for the second side;
they are not symmetric about the board centre, so the board only fits the pins the right way round.

Run:  python make_pcb.py [path-to-folder-containing-shapely]
Needs: shapely (pip install shapely)

Writes, next to this file (REV is the board revision letter):
  board_revX_bottom.dxf   side 1: bottom copper (mirrored, as seen from below), drills, alignment holes, slot, outline
  board_revX_top.dxf      side 2: top copper (as seen from above), the same other layers
  board_revX_bottom.svg   side 1 as an SVG at true size in mm, for CAM that imports SVG
  board_revX_top.svg      side 2 as an SVG at true size in mm
  board_revX_parts.dxf    top view with both coppers, component outlines and labels: for checking
  board_revX_preview.svg  colored top-view picture, not to scale
  board_revX_gerber.zip   Gerber (top and bottom copper, outline) and Excellon drill files, for FlatCAM and similar
  board_revX_report.txt   design-rule check results and part list
and copies every file into ../data/hw/ so the ESP32 serves them from Help (DXFs and SVGs as .gz, served plain).

Mill-file layers (DXF and SVG), each one a single machining operation, so nothing has to be interpreted:
  ISO_CENTER    cutter centre lines for a TOOL wide cutter: follow them with no offset (engrave / on-line)
  CLEAR_POCKET  closed areas to pocket out completely, any end mill; the CAM offsets for its own tool
  COPPER_REF    the copper that remains, for reference only: do not cut it (its holes are drawn too)
  DRILL_0P8, DRILL_1P3, ALIGN_3P0   hole circles at true size, one layer per drill
  SLOT          closed outline of the isolation slot: pocket with an end mill of 1.5 mm or less, before OUTLINE
  OUTLINE       board edge, a plain rectangle: cut last
  NOTES         text, not for cutting
"""
import sys, os, re, math, zipfile, shutil, gzip
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
from shapely.geometry import Point, LineString, Polygon, box
from shapely.ops import unary_union, nearest_points

HERE = os.path.dirname(os.path.abspath(__file__))
# Board revision. Bump it (A, B, C, ...) for every change that alters the board, and list it in CHANGELOG.md.
# History: A 165 x 118 single-sided (web 1.13.0); B 144 x 94 single-sided (1.14.0); C two-sided (1.15.0);
# D pull-ups spread to 3.81 / 7.62 mm and no ground slivers on the bottom (1.16.0);
# E 1.0 mm minimum gap everywhere, so CAM cutter paths never merge (1.17.0);
# F one operation per layer (ISO_CENTER, CLEAR_POCKET, COPPER_REF, drills by size, SLOT, OUTLINE), 6 mm air gap
#   through the slot, pads grown where there is room, asymmetric alignment holes, 2.0 mm slot (1.18.0).
REV = 'F'
REV_NOTE = ('Mill files redrawn as one operation per layer, after a CAM review: ISO_CENTER is the cutter centre line for a 0.5 mm cutter '
            '(follow it, no offset), CLEAR_POCKET is pocketed, COPPER_REF is only for reference, drills are split by size, the slot is its own '
            'layer and OUTLINE is a plain rectangle. Small pads grow up to 2.2 mm where there is room. Straight-line gap through the slot '
            'to the lathe side at least 6 mm. Alignment holes no longer symmetric, so the board cannot be fitted upside down. Slot 2.0 mm wide.')
W, H = 144.0, 94.0           # board size, mm: fits a 150 x 100 blank with 3 mm to spare all round
EDGE = 1.0                   # no copper within this distance of the board edge
CL = 1.0                     # isolation gap between different nets: wide enough that the cutter paths on
                             # either side of a gap never overlap (cutters up to TOOL), so CAM gives clean closed loops
TOOL = 0.5                   # isolation cutter width the ISO_CENTER lines are drawn for (a narrower cutter also works,
                             # since the CLEAR pockets take out whatever a narrower groove leaves in the gaps)
CREEP = 8.0                  # lathe side to ESP32 side along the surface, except across the slot under U1
AIR = 6.0                    # lathe side to ESP32 side in a straight line through the air, across the slot
MIN_FILL = 0.4               # top ground fill narrower than this is dropped (the strips between lead rings are 0.74 mm)
MIN_FILL_BOT = 1.0           # bottom ground fill narrower than this is dropped: no slivers or bubbles squeezed between pads
TW_SIG, TW_PWR = 1.0, 1.2    # trace widths
P = 5.08                     # terminal pitch
PAD_T, HOLE_T = 1.5, 1.3     # screw terminal pad radius, hole diameter
PAD, HOLE = 0.75, 0.8        # resistor, transistor, optocoupler and capacitor pads: the smallest radius, where neighbours are 2.54 mm away
PAD_MAX = 1.1                # ... and the radius they grow to where there is room (ring round the hole 0.7 mm instead of 0.35)
PAD_V, HOLE_V = 1.0, 0.8     # via pads: a bare wire (a resistor leg offcut) soldered on both sides
ALIGN_D = 3.0                # alignment pin holes (3 mm dowel or drill shank), on the flip line x = W/2
ASYM = 5.0                   # the second alignment hole is at least this far from the first one's image in the board centre
GVIA_SPACING = 45.0         # GND vias tying the top fill to the bottom fill: about this far apart
LATHE_NETS = {'HIGH', 'WIPER', 'LOW'}
RES = 8                      # segments per quarter circle: 32-point pads, within 0.005 mm of a true circle

pads, traces, silk, top_traces = [], [], [], []

def pad(x, y, net, ref, pin, r=PAD, hole=HOLE, via=False):
    pads.append(dict(x=x, y=y, net=net, ref=ref, pin=pin, r=r, hole=hole, via=via))
def tr(net, pts, w=TW_SIG):
    traces.append(dict(net=net, pts=pts, w=w))
def via(ref, x, y, net):
    pad(x, y, net, ref, '', PAD_V, HOLE_V, via=True)
def top_tr(ref, net, pts, w=TW_PWR):
    """Trace on the top (component) side, from via to via."""
    top_traces.append(dict(ref=ref, net=net, pts=pts, w=w))

# ---------- footprints ----------
def terminal(ref, x, ys, nets, names, side='left'):
    for i, (y, n, nm) in enumerate(zip(ys, nets, names)):
        pad(x, y, n, ref, nm, PAD_T, HOLE_T)
    d = 3.8
    x0, x1 = (x - d, x + d)
    silk.append(('rect', ref, x0, min(ys) - 2.6, x1, max(ys) + 2.6))
    if side == 'left':      # reference reads up the left edge of the block, beside the screws
        silk.append(('label', ref, x - d - 1.4, (min(ys) + max(ys)) / 2, ref, 'middle', 90))
    else:
        silk.append(('label', ref, x, max(ys) + 4.2, ref))
    for y, nm in zip(ys, names):
        silk.append(('pinlabel', ref, x + (7.0 if side == 'left' else -5.0), y, nm, side))

def resistor(ref, a, b, na, nb, value, standing=False, lab=None):
    """Lying flat between a and b, or standing on end with the body over pad a. lab = label offset from the centre."""
    pad(*a, na, ref, 1); pad(*b, nb, ref, 2)
    mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
    if standing:
        silk.append(('circle', ref, a[0], a[1], 1.6)); silk.append(('line', ref, a[0], a[1], b[0], b[1]))
        silk.append(('label', ref, a[0] + 2.0, a[1], ref, 'start'))
    else:
        vertical = abs(a[1] - b[1]) > 0.01
        if vertical:
            silk.append(('rect', ref, mx - 1.2, my - 3.2, mx + 1.2, my + 3.2))
        else:
            silk.append(('rect', ref, mx - 3.2, my - 1.2, mx + 3.2, my + 1.2))
        dx, dy = lab or ((-2.2, 0) if vertical else (0, 2.4))
        silk.append(('label', ref, mx + dx, my + dy, ref, 'end' if vertical and not lab else 'middle'))
    silk.append(('value', ref, value))

def to92(ref, x, yb, d, ne, nb, nc):
    """TO-92 standing, pins in a column: base at yb, collector toward the 3V3 tooth (direction d), emitter away."""
    pad(x, yb - d * 2.54, ne, ref, 'E'); pad(x, yb, nb, ref, 'B'); pad(x, yb + d * 2.54, nc, ref, 'C')
    silk.append(('to92', ref, x, yb, d)); silk.append(('label', ref, x - 4.0, yb, ref))
    silk.append(('value', ref, '2N3904'))

def dip4(ref, x, y, n1, n2, n3, n4):
    pad(x, y, n1, ref, 1); pad(x, y - 2.54, n2, ref, 2); pad(x + 7.62, y - 2.54, n3, ref, 3); pad(x + 7.62, y, n4, ref, 4)
    silk.append(('rect', ref, x + 1.4, y - 4.0, x + 6.2, y + 1.5)); silk.append(('circle', ref, x + 2.3, y + 0.6, 0.35))
    silk.append(('label', ref, x + 3.81, y + 3.0, ref)); silk.append(('value', ref, 'PC817'))

def cap(ref, a, b, na, nb, value, radial=True, lab=(3.6, 0)):
    pad(*a, na, ref, '+' if radial else 1); pad(*b, nb, ref, '-' if radial else 2)
    mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
    silk.append(('circle', ref, mx, my, 2.6 if radial else 1.6))
    if radial: silk.append(('plus', ref, a[0] - 1.6, a[1]))
    silk.append(('label', ref, mx + lab[0], my + lab[1], ref)); silk.append(('value', ref, value))


# ---------- placement and routing ----------
# Sized for a 150 x 100 mm copper-clad blank: the board is 144 x 94, leaving 3 mm all round for clamping
# and for the outline cut.
XT = 8.0                      # left terminal column
YT = H - 4.0                  # top terminal row
# Left column, top to bottom, as one continuous strip of 5.08 mm blocks: J3 spindle encoder, J6 charge pump,
# J1 X encoder, J2 Z encoder. Each cable's shield shares the 0V screw with the blue wire.
J3y = [80.82 - i * P for i in range(5)]
J6y = [J3y[-1] - P, J3y[-1] - 2 * P]
J1y = [J6y[-1] - P - i * P for i in range(4)]
J2y = [J1y[-1] - P - i * P for i in range(4)]
terminal('J3', XT, J3y, ['+12V', 'GND', 'SPA', 'SPB', 'SPZ'], ['+12V', '0V/SH', 'A', 'B', 'Z'])
terminal('J6', XT, J6y, ['CPIN', 'GND'], ['16', 'GND'])
terminal('J1', XT, J1y, ['+12V', 'XA', 'GND', 'XB'], ['+12V', 'A', '0V/SH', 'B'])
terminal('J2', XT, J2y, ['+12V', 'ZA', 'GND', 'ZB'], ['+12V', 'A', '0V/SH', 'B'])

def top_block(ref, xs, nets, names):
    """Terminal block along the top edge, screws facing up."""
    for x, n, nm in zip(xs, nets, names):
        pad(x, YT, n, ref, nm, PAD_T, HOLE_T)
    silk.append(('rect', ref, xs[0] - 2.6, YT - 3.8, xs[-1] + 2.6, YT + 3.8))
    for x, nm in zip(xs, names): silk.append(('pinlabel', ref, x, YT - 5.6, nm, 'below'))

# J4 12 V in, top left corner. +12V runs straight down into J3, then a top-side trace through vias V1-V3
# takes it over the signal traces to J1 and J2.
J4x = [XT, XT + P]
top_block('J4', J4x, ['+12V', 'GND'], ['+12V', 'GND'])
silk.append(('label', 'J4', J4x[0] - 4.6, YT - 6.0, 'J4'))
XJ = 13.0
tr('+12V', [(XT, YT), (XT, J3y[0]), (XJ, J3y[0])], TW_PWR)
tr('+12V', [(XT, J1y[0]), (XJ, J1y[0])], TW_PWR)
tr('+12V', [(XT, J2y[0]), (XJ, J2y[0])], TW_PWR)
via('V1', XJ, J3y[0], '+12V'); via('V2', XJ, J1y[0], '+12V'); via('V3', XJ, J2y[0], '+12V')
top_tr('+12V top', '+12V', [(XJ, J3y[0]), (XJ, J2y[0])])

# J5 to the G540, on the top edge. Spindle lines branch up to it through the gaps under R5, R7 and R9.
J5x = [27.0 + i * P for i in range(4)]
top_block('J5', J5x, ['SPA', 'SPB', 'SPZ', 'GND'], ['T2', 'T3', 'T1', 'T12'])
silk.append(('label', 'J5', J5x[0] - 5.2, YT, 'J5'))

yA, yB, yZ, yC = J3y[2], J3y[3], J3y[4], J6y[0]
# Base resistors in line with each row; the branch to J5 rises left of each resistor
resistor('R5', (18.0, yA), (28.16, yA), 'SPA', 'QAB', '10k')
resistor('R7', (22.0, yB), (32.16, yB), 'SPB', 'QBB', '10k')
resistor('R9', (25.5, yZ), (35.66, yZ), 'SPZ', 'QZB', '10k')
resistor('R11', (20.0, yC), (30.16, yC), 'CPIN', 'QCB', '10k')
tr('SPA', [(XT, yA), (18.0, yA)]); tr('SPB', [(XT, yB), (22.0, yB)]); tr('SPZ', [(XT, yZ), (25.5, yZ)]); tr('CPIN', [(XT, yC), (20.0, yC)])
tr('SPA', [(16.5, yA), (16.5, YT - 7.0), (J5x[0], YT - 7.0), (J5x[0], YT)])
tr('SPB', [(20.5, yB), (20.5, YT - 9.5), (J5x[1], YT - 9.5), (J5x[1], YT)])
tr('SPZ', [(24.8, yZ), (24.8, YT - 12.0), (J5x[2], YT - 12.0), (J5x[2], YT)])

# Inverters, all in one column at XQ. Pair (A, B) shares 3V3 tooth T1, pair (Z, CP) shares T2.
# Collectors sit 2.54 mm either side of their tooth; the pull-ups stand between collector and tooth.
XQ = 52.0
tT1, tT2 = YT - 9.62, YT - 27.4
qA, qB = tT1 + 5.08, tT1 - 5.08     # base heights
qZ, qC = tT2 + 5.08, tT2 - 5.08
# Base leads climb in columns 40-46, each turning right above the last, so none cross
tr('QAB', [(28.16, yA), (40.0, yA), (40.0, qA), (XQ, qA)])
tr('QBB', [(32.16, yB), (42.0, yB), (42.0, qB), (XQ, qB)])
tr('QZB', [(35.66, yZ), (44.0, yZ), (44.0, qZ), (XQ, qZ)])
tr('QCB', [(30.16, yC), (46.0, yC), (46.0, qC), (XQ, qC)])
to92('Q1', XQ, qA, -1, 'GND', 'QAB', 'OA')
to92('Q2', XQ, qB, +1, 'GND', 'QBB', 'OB')
to92('Q3', XQ, qZ, -1, 'GND', 'QZB', 'OZ')
to92('Q4', XQ, qC, +1, 'GND', 'QCB', 'OC')
oA, oB, oZ, oC = qA - 2.54, qB + 2.54, qZ - 2.54, qC + 2.54
# Pull-ups stand 3.81 and 7.62 mm right of the transistors (not 2.54 and 5.08), so ground copper fits
# between them and the transistor pins instead of breaking into slivers.
XP1, XP2 = XQ + 3.81, XQ + 7.62
resistor('R6', (XP1, oA), (XP1, tT1), 'OA', '3V3', '10k', standing=True)
resistor('R8', (XP2, oB), (XP2, tT1), 'OB', '3V3', '10k', standing=True)
resistor('R10', (XP1, oZ), (XP1, tT2), 'OZ', '3V3', '10k', standing=True)
resistor('R12', (XP2, oC), (XP2, tT2), 'OC', '3V3', '10k', standing=True)

# J7 (to ESP32, spindle and charge pump) and J8 (to ESP32, axes, 3V3, GND, GPIO 2) on the right edge.
# J7 lines up with the A and B collectors; Z and CP step up to it.
XR = 98.0
J7y = [oA - i * P for i in range(4)]
xA, xB, zA, zB = J1y[1], J1y[3], J2y[1], J2y[3]
tT3, tT4 = J1y[2], J2y[2]
J8y = [xA, tT3, xB, xB - P, zA, tT4, zB]
terminal('J7', XR, J7y, ['OA', 'OB', 'OZ', 'OC'], ['GPIO4', 'GPIO5', 'GPIO6', 'GPIO18'], side='right')
terminal('J8', XR, J8y, ['GPIO2', 'GND', 'XA', 'XB', 'ZA', '3V3', 'ZB'], ['GPIO2', 'GND', 'GPIO7', 'GPIO15', 'GPIO16', '3V3', 'GPIO17'], side='right')
tr('OA', [(XQ, oA), (XR, J7y[0])])
tr('OB', [(XQ, oB), (XR, J7y[1])])
tr('OZ', [(XQ, oZ), (88.0, oZ), (88.0, J7y[2]), (XR, J7y[2])])
tr('OC', [(XQ, oC), (92.0, oC), (92.0, J7y[3]), (XR, J7y[3])])

# Axis encoders: pull-ups R1-R4 stand between each line and the 3V3 tooth between the pair.
# X A and B step down two rows to make room for GPIO2 and GND at the top of J8.
resistor('R1', (30.0, xA), (30.0, tT3), 'XA', '3V3', '2.2k', standing=True)
resistor('R2', (34.0, xB), (34.0, tT3), 'XB', '3V3', '2.2k', standing=True)
resistor('R3', (30.0, zA), (30.0, tT4), 'ZA', '3V3', '2.2k', standing=True)
resistor('R4', (34.0, zB), (34.0, tT4), 'ZB', '3V3', '2.2k', standing=True)
tr('XA', [(XT, xA), (88.0, xA), (88.0, J8y[2]), (XR, J8y[2])])
tr('XB', [(XT, xB), (84.0, xB), (84.0, J8y[3]), (XR, J8y[3])])
tr('ZA', [(XT, zA), (XR, zA)])
tr('ZB', [(XT, zB), (XR, zB)])

# 3V3 teeth, joined at XW by a top-side trace through vias V4-V7. T4 runs straight into J8 3V3.
XW = 76.0
tr('3V3', [(XP1, tT1), (XW, tT1)], TW_PWR)
tr('3V3', [(XP1, tT2), (XW, tT2)], TW_PWR)
tr('3V3', [(30.0, tT3), (XW, tT3)], TW_PWR)
tr('3V3', [(30.0, tT4), (XR, tT4)], TW_PWR)
for i, yv in enumerate([tT1, tT2, tT3, tT4]): via('V%d' % (4 + i), XW, yv, '3V3')
top_tr('3V3 top', '3V3', [(XW, tT1), (XW, tT4)])
cap('C2', (84.0, tT4), (84.0, tT4 - 2.54), '3V3', 'GND', '100uF', lab=(0, 5.0))
cap('C3', (66.0, tT4), (66.0, tT4 - 2.54), '3V3', 'GND', '100nF', radial=False, lab=(0, 4.0))

# Spindle control, top right corner, isolated from the rest by a slot through the board under U1.
# R13 and U1 pins 1-2 are on the ESP32 side; U1 pins 3-4, R14, C1 and J9 are on the lathe side.
XU = XR + 7.0; YU = H - 10.0
SLOT = dict(x=XU + 3.81, y0=YU - 14.0, y1=H + 2.0, w=2.0)   # open at the top edge: the only way round is the bottom end.
                                                              # 2.0 mm wide: pocket it with a 1.5 mm (1/16 in) or smaller end mill
XG = XR + 4.5                # GPIO2 and U1A climb the column right of J7 and J8, leaving a ground channel beside J7
yR13 = J8y[0] + 8.0
tr('GPIO2', [(XR, J8y[0]), (XG, J8y[0]), (XG, yR13)])
resistor('R13', (XG, yR13), (XG, yR13 + 10.16), 'GPIO2', 'U1A', '330', lab=(2.2, 0))
tr('U1A', [(XG, yR13 + 10.16), (XG, YU), (XU, YU)])
dip4('U1', XU, YU, 'U1A', 'GND', 'WIPER', 'HIGH')
XJ9 = XU + 33.0
J9y = [YU + 5.0, YU + 5.0 - P, YU + 5.0 - 2 * P]
terminal('J9', XJ9, J9y, ['HIGH', 'WIPER', 'LOW'], ['HIGH', 'WIPER', 'LOW'], side='right')
tr('HIGH', [(XU + 7.62, YU), (XU + 7.62, J9y[0]), (XJ9, J9y[0])])
tr('WIPER', [(XU + 7.62, YU - 2.54), (XJ9 - 6.0, YU - 2.54), (XJ9, J9y[1])])
yR14 = YU - 8.0
xR14 = XU + 15.0
resistor('R14', (xR14, yR14), (xR14 + 10.16, yR14), 'WIPER', 'LOW', '4.7k', lab=(0, -2.4))
tr('WIPER', [(xR14, YU - 2.54), (xR14, yR14)])
cap('C1', (xR14 + 5.0, YU - 2.54), (xR14 + 5.0, YU - 5.08), 'WIPER', 'LOW', '10uF 25V', lab=(0, 4.4))
tr('LOW', [(xR14 + 5.0, YU - 5.08), (xR14 + 10.16, YU - 5.08), (xR14 + 10.16, yR14)])
tr('LOW', [(xR14 + 10.16, yR14), (XJ9, yR14), (XJ9, J9y[2])])

# ---------- geometry ----------
board = box(0, 0, W, H)
inner = board.buffer(-EDGE, join_style=2)
slot = LineString([(SLOT['x'], SLOT['y0']), (SLOT['x'], SLOT['y1'])]).buffer(SLOT['w'] / 2)
outline = board                            # OUTLINE layer: a plain rectangle, the slot is its own layer
profile = board.difference(slot)           # the finished shape, for the Gerber edge cuts
slot_cut = slot.intersection(box(0, -1, W, H + 1.5))   # SLOT layer: runs 1.5 mm past the top edge so it breaks out
slot_in = slot.intersection(board)
no_copper = board.difference(slot)         # where copper can exist at all

# Small pads grow from PAD towards PAD_MAX where their neighbours leave room, so the copper ring round the
# hole is as wide as it can be (a drill that lands a little off still has copper to solder to). Two small
# pads of different nets share the room between them equally; a pad next to a terminal, via or trace of
# another net takes what is left after the gap. Lathe nets keep AIR from everything else, other nets CL.
# Where ground copper runs between two features at the small size, it must still be MIN_FILL_BOT wide
# after they grow, so no ground channel is pinched off (and no two ground shapes end up nearly touching).
def gap_needed(a, b): return AIR if (a in LATHE_NETS) != (b in LATHE_NETS) else CL
def room(dist, gap, other_r):
    """Centre-to-edge distance a small pad may grow into, given the distance to another feature of another net."""
    if gap == CL and dist - PAD - other_r - 2 * CL >= MIN_FILL_BOT: return dist - other_r - 2 * CL - MIN_FILL_BOT
    return dist - other_r - gap
trace_g = {}
for t in traces:
    trace_g[t['net']] = unary_union([trace_g.get(t['net'], Polygon()), LineString(t['pts']).buffer(t['w'] / 2, RES)])
small = [p for p in pads if p['r'] == PAD]
grown = {}
for p in small:
    c = Point(p['x'], p['y'])
    r = min(PAD_MAX, inner.exterior.distance(c), c.distance(slot) - CL)
    for q in pads:
        if q is p or q['net'] == p['net']: continue
        d = math.dist((p['x'], p['y']), (q['x'], q['y']))
        if q['r'] == PAD: r = min(r, (room(d, gap_needed(p['net'], q['net']), PAD) + PAD) / 2)   # both grow: share the room
        else: r = min(r, room(d, gap_needed(p['net'], q['net']), q['r']))
    for n, g in trace_g.items():
        if n != p['net']: r = min(r, room(c.distance(g), gap_needed(p['net'], n), 0))
    grown[id(p)] = max(PAD, math.floor(r * 20 + 1e-6) / 20)
for p in small: p['r'] = grown[id(p)]

def copper_of(net):
    g = [Point(p['x'], p['y']).buffer(p['r'], RES) for p in pads if p['net'] == net]
    g += [LineString(t['pts']).buffer(t['w'] / 2, RES) for t in traces if t['net'] == net]
    return unary_union(g)

nets = sorted({p['net'] for p in pads} | {t['net'] for t in traces})
islands = {n: copper_of(n) for n in nets if n != 'GND'}
lathe = unary_union([islands[n] for n in LATHE_NETS])
# Copper is removed within CREEP of the lathe nets on the lathe side of the slot. Left of the slot the
# slot itself provides the creepage, since any path around it is longer than CREEP; there the copper stays
# AIR away from the lathe nets, so the straight-line gap through the slot is at least AIR too.
creep_clear = lathe.buffer(CREEP, RES).intersection(box(SLOT['x'] - SLOT['w'] / 2, 0, W, H))
air_clear = lathe.buffer(AIR, RES)
blocked = unary_union([g.buffer(CL, RES) for g in islands.values()] + [creep_clear, air_clear, slot.buffer(CL, RES)])
gnd_pads = unary_union([Point(p['x'], p['y']).buffer(p['r'], RES) for p in pads if p['net'] == 'GND'])
fill = inner.difference(blocked).union(gnd_pads)
fill = fill.buffer(-MIN_FILL_BOT / 2, RES).buffer(MIN_FILL_BOT / 2, RES).union(gnd_pads)       # drop slivers too thin to mill
fill = fill.intersection(inner)
pieces = list(fill.geoms) if fill.geom_type == 'MultiPolygon' else [fill]
dropped = sum(1 for q in pieces if not q.intersects(gnd_pads))     # copper islands with no GND pad would float
pieces = [q for q in pieces if q.intersects(gnd_pads)]
fill = unary_union(pieces)
copper = unary_union([fill] + list(islands.values()))

# ---------- top side ----------
# Holes are not plated, and component leads are soldered underneath only. On top, every hole that is
# not GND simply sits in a small copper-free circle (RING beyond the hole edge) in the ground copper:
# the isolation cut runs once round that circle, which leaves one clean groove round the lead. The
# circles never touch each other, so each lead has its own groove. Only vias have real top pads,
# because they are soldered on top; only top traces run between vias.
# RING = TOOL: the ISO_CENTER circle sits TOOL/2 inside the copper edge, so a TOOL wide cutter takes the
# copper off right up to the hole, and no loose ring of copper is left round the lead.
RING = TOOL
top_traces_g = {}
for t in top_traces:
    top_traces_g[t['net']] = unary_union([top_traces_g.get(t['net'], Polygon()), LineString(t['pts']).buffer(t['w'] / 2, RES)])
top_islands = {}
for p in pads:
    if p['net'] != 'GND' and p['via']:
        top_islands[p['net']] = unary_union([top_islands.get(p['net'], Polygon()), Point(p['x'], p['y']).buffer(p['r'], RES)])
for n, g in top_traces_g.items(): top_islands[n] = unary_union([top_islands.get(n, Polygon()), g])
lead_holes = [p for p in pads if p['net'] != 'GND' and not p['via']]
discs = unary_union([Point(p['x'], p['y']).buffer(p['hole'] / 2 + RING, RES) for p in lead_holes])
top_lathe = unary_union([Point(p['x'], p['y']).buffer(p['r'], RES) for p in lead_holes if p['net'] in LATHE_NETS])

def free_spot(x, y, r):
    """True when a hole of radius r at (x, y) keeps CL from all non-GND copper on both sides and from other holes."""
    c = Point(x, y).buffer(r + CL, RES)
    if not inner.contains(Point(x, y).buffer(r + 0.5, RES)): return False
    if any(c.intersects(g) for g in list(islands.values()) + list(top_islands.values()) + [discs]): return False
    if c.buffer(0.0).intersects(creep_clear) or c.intersects(slot): return False
    return all(math.dist((x, y), (p['x'], p['y'])) >= r + p['r'] + CL for p in pads)

# Alignment holes, on the flip line x = W/2: one near the bottom edge, one near the top. The top one is
# kept at least ASYM from the bottom one's image in the board centre, so the pair is not symmetric: the
# board goes onto the pins flipped left to right, and cannot be fitted upside down (rotated 180 degrees).
ys_up = [2.0 + 0.25 * i for i in range(int((H - 4) / 0.25))]
y_bot = next(y for y in ys_up if free_spot(W / 2, y, ALIGN_D / 2 + 0.5))
y_top = next(y for y in reversed(ys_up) if abs(y - (H - y_bot)) >= ASYM and free_spot(W / 2, y, ALIGN_D / 2 + 0.5))
align = [(W / 2, y_bot), (W / 2, y_top)]
align_g = unary_union([Point(a).buffer(ALIGN_D / 2, RES) for a in align])

top_blocked = unary_union([g.buffer(CL, RES) for g in top_islands.values()] + [discs, creep_clear, air_clear, slot.buffer(CL, RES)])
top_raw = inner.difference(top_blocked)
top_raw = top_raw.buffer(-MIN_FILL / 2, RES).buffer(MIN_FILL / 2, RES).intersection(inner)
top_raw_pieces = [q for q in (list(top_raw.geoms) if top_raw.geom_type == 'MultiPolygon' else [top_raw]) if q.area > 1.0]

# GND vias: inside both ground fills, clear of everything else. One for every top fill piece large
# enough to matter, then more until no part of a piece is more than GVIA_SPACING from a via.
from shapely.prepared import prep
both = prep(fill.buffer(-(PAD_V + 0.15), RES).intersection(top_raw.buffer(-(PAD_V + 0.15), RES)))
cands = [(x * 1.0, y * 1.0) for x in range(2, int(W) - 1) for y in range(2, int(H) - 1)
         if both.contains(Point(x, y)) and all(math.dist((x, y), a) >= ALIGN_D / 2 + PAD_V + 1.0 for a in align)
         and all(math.dist((x, y), (p['x'], p['y'])) >= p['r'] + PAD_V + 1.0 for p in pads)]
gvias = []
for q in sorted(top_raw_pieces, key=lambda q: -q.area):
    if q.area < 40: continue
    cq = [c for c in cands if q.contains(Point(c))]
    if not cq: continue
    rp = q.representative_point()
    chosen = [min(cq, key=lambda c: math.dist(c, (rp.x, rp.y)))]
    while True:
        far_c = max(cq, key=lambda c: min(math.dist(c, v) for v in chosen))
        if min(math.dist(far_c, v) for v in chosen) <= GVIA_SPACING: break
        chosen.append(far_c)
    gvias += chosen
# Every bottom ground piece also needs a GND via, so the top ground joins pieces the bottom traces cut apart.
fine = [(x * 0.5, y * 0.5) for x in range(4, int(W * 2) - 3) for y in range(4, int(H * 2) - 3)]
fine = [c for c in fine if both.contains(Point(c)) and all(math.dist(c, a) >= ALIGN_D / 2 + PAD_V + 1.0 for a in align)
        and all(math.dist(c, (p['x'], p['y'])) >= p['r'] + PAD_V + 1.0 for p in pads)]
for q in pieces:
    if any(q.contains(Point(v)) for v in gvias): continue
    cq = [c for c in fine if q.contains(Point(c))]
    if cq:
        rp = q.representative_point()
        gvias.append(min(cq, key=lambda c: math.dist(c, (rp.x, rp.y))))
for i, (x, y) in enumerate(sorted(gvias, key=lambda c: (-c[1], c[0]))):
    via('V%d' % (8 + i), x, y, 'GND')
gvia_pts = [(p['x'], p['y']) for p in pads if p['via'] and p['net'] == 'GND']
top_pieces = [q for q in top_raw_pieces if any(q.contains(Point(v)) for v in gvia_pts)]
top_dropped = len(top_raw_pieces) - len(top_pieces)
top_fill = unary_union(top_pieces)
top_copper = unary_union([top_fill] + list(top_islands.values()))

# CLEAR: copper to remove completely, not just isolate. Isolation cutting only cuts a groove round
# each copper outline, so without this, copper that is in no outline (the 8 mm zone round the lathe
# side, the board edge margin, ground the fill dropped) would stay on the board as floating islands.
# A floating island can bridge the lathe side to the ESP32 side, so the CLEAR areas must be cleared.
# Gaps narrower than 0.8 mm are left to the isolation cut.
# On top, a lead's own copper-free circle surrounded by ground copper is left alone: the isolation cut
# round it is all it needs. (Circles in open areas, such as the lathe corner, are cleared with the rest.)
# The pockets run 1 mm past the board edge (the blank is 3 mm bigger all round), so the edge margin is
# cleared right to the corners and no copper is left along the cut edge.
clear_base = board.buffer(1.0, join_style=2).difference(slot)
def clear_of(cu, keep_rings=False):
    free = clear_base.difference(cu)
    if keep_rings:
        free = unary_union([q for q in geoms(free) if q.difference(discs).area > 0.05])
    return free.buffer(-0.4, RES).buffer(0.4, RES).intersection(clear_base)
geoms = lambda g: list(g.geoms) if g.geom_type in ('MultiPolygon', 'GeometryCollection') else ([g] if not g.is_empty else [])
clear_bot, clear_top = clear_of(copper), clear_of(top_copper, keep_rings=True)
ring_only = [q for q in geoms(no_copper.difference(top_copper)) if q.difference(discs).area <= 0.05]
# ISO_CENTER: where the centre of a TOOL wide cutter runs to cut a groove along every copper edge, with
# the groove just outside the copper. This is the copper grown by half the cutter width: its outline.
iso_bot, iso_top = copper.buffer(TOOL / 2, RES), top_copper.buffer(TOOL / 2, RES)

# ---------- design-rule checks ----------
report, errors = [], []
def check(name, ok, detail=''):
    report.append(('PASS ' if ok else 'FAIL ') + name + (': ' + detail if detail else ''))
    if not ok: errors.append(name)

# Each net is one piece of copper: bottom pieces, plus top traces, joined only where a via pin passes
# through both. (Top pads round component leads join nothing: those leads are soldered underneath.)
geoms = lambda g: list(g.geoms) if g.geom_type == 'MultiPolygon' else ([g] if not g.is_empty else [])
for n, g in islands.items():
    vp = [Point(p['x'], p['y']).buffer(p['r'], RES) for p in pads if p['via'] and p['net'] == n]
    parts_ = geoms(g) + geoms(unary_union([top_traces_g.get(n, Polygon())] + vp)) if n in top_traces_g else geoms(g)
    nb = len(geoms(g))
    group = list(range(len(parts_)))
    def find(i):
        while group[i] != i: i = group[i]
        return i
    for p in pads:
        if not (p['via'] and p['net'] == n): continue
        hit = [i for i, q in enumerate(parts_) if q.contains(Point(p['x'], p['y']))]
        if len([i for i in hit if i < nb]) and len([i for i in hit if i >= nb]):
            for i in hit[1:]: group[find(i)] = find(hit[0])
    roots = {find(i) for i in range(len(parts_))}
    check('net %s connected' % n, len(roots) == 1, '%d separate pieces' % len(roots) if len(roots) > 1 else '')
# Top side isolation
tn = list(top_islands)
tw = (99, '')
for i in range(len(tn)):
    for k in range(i + 1, len(tn)):
        d = top_islands[tn[i]].distance(top_islands[tn[k]])
        if d < tw[0]: tw = (d, tn[i] + '/' + tn[k])
check('top: isolation between nets >= %.2f mm' % CL, tw[0] >= CL - 1e-6, 'closest %.2f mm (%s)' % tw)
dtop = min(g.distance(top_fill) for g in top_islands.values())
check('top: top traces and via pads isolated from the top ground fill by >= %.2f mm' % CL, dtop >= CL - 0.05, 'closest %.2f mm' % dtop)
shared = []
for q in ring_only:
    inside = [p for p in lead_holes if q.contains(Point(p['x'], p['y']))]
    if len(inside) != 1: shared.append('+'.join('%s.%s' % (p['ref'], p['pin']) for p in inside) or 'empty ring')
loose_l = ['%s.%s' % (p['ref'], p['pin']) for p in lead_holes
           if not any(q.contains(Point(p['x'], p['y'])) for q in ring_only) and not clear_top.contains(Point(p['x'], p['y']).buffer(p['hole'] / 2 + RING - 0.05, RES))]
check('top: every non-GND lead has its own isolation ring (or sits in a cleared area)', not shared and not loose_l,
      ', '.join(shared + loose_l) or '%d rings, %d leads in cleared areas' % (len(ring_only), len(lead_holes) - len(ring_only)))
gap = min((Point(a['x'], a['y']).distance(Point(b['x'], b['y'])) - a['hole'] / 2 - b['hole'] / 2 - 2 * RING
           for i, a in enumerate(lead_holes) for b in lead_holes[i + 1:]), default=99)
check('top: ground copper between neighbouring rings >= %.2f mm' % MIN_FILL, gap >= MIN_FILL - 1e-6, 'narrowest %.2f mm' % gap)
check('top: every piece of top ground fill has a GND via', True, '%d piece(s), %d GND via(s); %d small piece(s) with no room for a via left to CLEAR' % (len(top_pieces), len(gvia_pts), top_dropped))
tfar = unary_union([top_fill] + [g for n, g in top_islands.items() if n not in LATHE_NETS]).difference(box(SLOT['x'] - 6, SLOT['y0'] - 1, SLOT['x'] + 0.1, SLOT['y1'] + 1))
dct = top_lathe.distance(tfar)
check('top: lathe side to ESP32 side >= %.1f mm (other than around the slot)' % CREEP, dct >= CREEP - 0.05, '%.1f mm' % dct)
# CAM check: grow every copper outline by the radius of a 0.5 mm cutter, as an outside isolation cut does.
# If two outlines' cutter paths overlapped, the grown shapes would merge or a hole would vanish, and the CAM
# would draw the merged "bubble" and "bone" loops instead of one closed path round each shape.
def cutter_paths_separate(cu, g):
    rings_ = lambda x: sum(1 + len(p.interiors) for p in geoms(x))
    return len(geoms(g)) == len(geoms(cu)) and rings_(g) == rings_(cu), (len(geoms(cu)), len(geoms(g)), rings_(cu), rings_(g))
for side_, cu_, iso_ in (('bottom', copper, iso_bot), ('top', top_copper, iso_top)):
    ok_, n_ = cutter_paths_separate(cu_, iso_)
    check('%s: ISO_CENTER has one separate closed path round every copper outline (%.1f mm cutter)' % (side_, TOOL), ok_, '%d shapes -> %d paths, %d rings -> %d' % n_)
# The ISO groove and the CLEAR pockets together must take away every bit of copper outside the kept copper,
# except the lead rings on top (one groove each, by design): nothing may be left floating between them.
for side_, cu_, cl_, iso_ in (('bottom', copper, clear_bot, iso_bot), ('top', top_copper, clear_top, iso_top)):
    groove = iso_.difference(cu_)                       # the copper the ISO cut removes with a TOOL wide cutter
    left = no_copper.difference(unary_union([cu_, cl_, groove]))
    if side_ == 'top': left = left.difference(discs)
    left = [q for q in geoms(left) if q.area > 0.02]
    check('%s: ISO groove plus CLEAR pockets remove all copper that is not kept' % side_, not left,
          '%d piece(s) left, largest %.2f mm2' % (len(left), max(q.area for q in left)) if left else '')
# Holes: alignment holes clear of every net, and no two holes closer than 0.5 mm edge to edge
ab = [a for a in align if not free_spot(*a, ALIGN_D / 2)]
check('alignment holes clear of all non-GND copper', not ab, ', '.join('(%.1f, %.1f)' % a for a in ab) or 'at ' + ', '.join('(%.1f, %.1f)' % a for a in align))
hg = (99, '')
for i in range(len(pads)):
    for k in range(i + 1, len(pads)):
        a, b = pads[i], pads[k]
        d = math.dist((a['x'], a['y']), (b['x'], b['y'])) - a['hole'] / 2 - b['hole'] / 2
        if d < hg[0]: hg = (d, '%s.%s/%s.%s' % (a['ref'], a['pin'], b['ref'], b['pin']))
check('holes >= 0.5 mm apart', hg[0] >= 0.5, 'closest %.2f mm (%s)' % hg)
# Isolation between different nets
names = list(islands)
worst = (99, '')
close = []
for i in range(len(names)):
    for k in range(i + 1, len(names)):
        d = islands[names[i]].distance(islands[names[k]])
        if d < worst[0]: worst = (d, names[i] + '/' + names[k])
        if d < CL - 1e-6:
            a, b = nearest_points(islands[names[i]], islands[names[k]])
            close.append('%s/%s %.2f mm at (%.1f, %.1f)' % (names[i], names[k], d, (a.x + b.x) / 2, (a.y + b.y) / 2))
check('isolation between nets >= %.2f mm' % CL, worst[0] >= CL - 1e-6, 'closest %.2f mm (%s)' % worst + ('; too close: ' + '; '.join(close) if close else ''))
dmin = min(g.distance(fill) for g in islands.values())
check('isolation from ground fill >= %.2f mm' % CL, dmin >= CL - 0.05, 'closest %.2f mm' % dmin)
# Every GND pad on the same piece of ground fill
# Bottom ground pieces count as joined when a GND via links each of them to the top ground (one piece).
gp = [p for p in pads if p['net'] == 'GND']
host = [next((i for i, q in enumerate(pieces) if q.buffer(0.01).contains(Point(p['x'], p['y']))), None) for p in gp]
linked = {i for i, q in enumerate(pieces) if any(q.contains(Point(v)) for v in gvia_pts)}
main = max(set(h for h in host if h is not None), key=host.count)
loose = ['%s.%s' % (p['ref'], p['pin']) for p, h in zip(gp, host) if h is None or (h != main and not (h in linked and main in linked))]
check('all GND pads joined (bottom ground pieces linked by GND vias through the top ground)', not loose and len(top_pieces) == 1, ', '.join(loose))
# Creepage from the lathe side
far = unary_union([fill] + [g for n, g in islands.items() if n not in LATHE_NETS]).difference(box(SLOT['x'] - 6, SLOT['y0'] - 1, SLOT['x'] + 0.1, SLOT['y1'] + 1))
dcreep = lathe.distance(far)
from shapely.ops import nearest_points
np = nearest_points(lathe, far)[1]
check('lathe side to ESP32 side >= %.1f mm (other than around the slot)' % CREEP, dcreep >= CREEP - 0.05, '%.1f mm, closest at (%.1f, %.1f)' % (dcreep, np.x, np.y))
ends = [Point(SLOT['x'], SLOT['y0'] - SLOT['w'] / 2)] + ([Point(SLOT['x'], SLOT['y1'] + SLOT['w'] / 2)] if SLOT['y1'] < H else [])
around = min(lathe.distance(e) for e in ends)
check('path around the slot ends >= %.1f mm' % CREEP, around >= CREEP, '%.1f mm to the nearer end' % around)
# Straight-line (air) clearance from the lathe nets to everything else, including across the slot
dair = lathe.distance(unary_union([fill] + [g for n, g in islands.items() if n not in LATHE_NETS]))
dair_t = top_lathe.distance(unary_union([top_fill] + [g for n, g in top_islands.items() if n not in LATHE_NETS]))
check('lathe side to ESP32 side in a straight line (through the slot) >= %.1f mm' % AIR, min(dair, dair_t) >= AIR - 0.05, 'bottom %.1f mm, top %.1f mm; the slot is %.1f mm wide' % (dair, dair_t, SLOT['w']))
dslot = min(g.distance(slot) for g in list(islands.values()) + list(top_islands.values()) + [fill, top_fill])
check('copper clear of the slot by >= %.2f mm' % CL, dslot >= CL - 0.05, 'closest %.2f mm' % dslot)
# Annular rings: copper left round each hole on the bottom, after the pads grew where there was room
ring_w = {('%.2f' % (p['r'] - p['hole'] / 2)): 0 for p in pads if not p['via'] and p['r'] < PAD_T}
for p in pads:
    if not p['via'] and p['r'] < PAD_T: ring_w['%.2f' % (p['r'] - p['hole'] / 2)] += 1
check('bottom: small pads grown where there is room', True, 'ring round the hole: ' + ', '.join('%d x %s mm' % (ring_w[k], k) for k in sorted(ring_w)) + ' (pads %.1f-%.1f mm across)' % (2 * PAD, 2 * PAD_MAX))
# Edge clearance
bad_edge = ['%s.%s' % (p['ref'], p['pin']) for p in pads if not inner.contains(Point(p['x'], p['y']).buffer(p['r']))]
check('pads inside the board edge margin', not bad_edge, ', '.join(bad_edge))
check('every bottom ground piece has a GND via', all(i in linked for i in range(len(pieces))) or len(pieces) == 1,
      '%d piece(s); %d floating island(s) removed' % (len(pieces), dropped))

# ---------- writers ----------
def rings(geom, tol=0):
    if tol: geom = geom.simplify(tol)
    polys = list(geom.geoms) if geom.geom_type == 'MultiPolygon' else [geom]
    for p in polys:
        yield list(p.exterior.coords)
        for r in p.interiors: yield list(r.coords)

SIDE_NOTE = {'bottom': 'SIDE 1 OF 2: BOTTOM COPPER, MIRRORED (COPPER SIDE UP). DRILL ALL HOLES HERE',
             'top': 'SIDE 2 OF 2: TOP COPPER, NOT MIRRORED (COMPONENT SIDE UP). POCKET THE SLOT, THEN CUT THE OUTLINE LAST',
             'parts': 'TOP VIEW, COMPONENT SIDE: FOR CHECKING, NOT FOR MILLING'}
LAYER_NOTE = ('ISO_CENTER: CUTTER CENTRE LINES FOR A %.1f MM CUTTER, FOLLOW WITH NO OFFSET.  CLEAR_POCKET: POCKET OUT (RUNS 1 MM PAST THE EDGE).  COPPER_REF: REFERENCE ONLY, DO NOT CUT.  '
              'DRILL_xPx: HOLES BY SIZE.  ALIGN_3P0: ALIGNMENT PINS.  SLOT: POCKET WITH AN END MILL 1.5 MM OR LESS.  OUTLINE: CUT LAST' % TOOL)
drill_layer = lambda d: 'DRILL_' + ('%.1f' % d).replace('.', 'P')
drill_sizes = sorted({p['hole'] for p in pads})
def dxf(path, side):
    mirror = side == 'bottom'
    mx = (lambda x: W - x) if mirror else (lambda x: x)
    layers = [('OUTLINE', 7), ('SLOT', 7)] + [(drill_layer(d), 5) for d in drill_sizes] + [('ALIGN_3P0', 6), ('NOTES', 2)]
    layers += [('COPPER_TOP', 1), ('COPPER_BOTTOM', 4), ('SILK', 3)] if side == 'parts' else [('ISO_CENTER', 1), ('CLEAR_POCKET', 8), ('COPPER_REF', 9)]
    # R12 header plus $INSUNITS 4 (millimetres) and $MEASUREMENT 1 (metric), which later readers look for
    out = ['0', 'SECTION', '2', 'HEADER', '9', '$ACADVER', '1', 'AC1009', '9', '$INSUNITS', '70', '4', '9', '$MEASUREMENT', '70', '1',
           '9', '$EXTMIN', '10', '-1', '20', '-12', '9', '$EXTMAX', '10', str(W + 1), '20', str(H + 2), '0', 'ENDSEC',
           '0', 'SECTION', '2', 'TABLES', '0', 'TABLE', '2', 'LAYER', '70', str(len(layers))]
    for name, col in layers:
        out += ['0', 'LAYER', '2', name, '70', '0', '62', str(col), '6', 'CONTINUOUS']
    out += ['0', 'ENDTAB', '0', 'ENDSEC', '0', 'SECTION', '2', 'ENTITIES']
    def poly(layer, pts, closed=True):
        nonlocal out
        out += ['0', 'POLYLINE', '8', layer, '66', '1', '70', '1' if closed else '0']
        for x, y in pts[:-1] if closed else pts:
            out += ['0', 'VERTEX', '8', layer, '10', '%.3f' % mx(x), '20', '%.3f' % y]
        out += ['0', 'SEQEND', '8', layer]
    def circle(layer, x, y, r):
        nonlocal out
        out += ['0', 'CIRCLE', '8', layer, '10', '%.3f' % mx(x), '20', '%.3f' % y, '40', '%.3f' % r]
    def line(layer, x1, y1, x2, y2):
        nonlocal out
        out += ['0', 'LINE', '8', layer, '10', '%.3f' % mx(x1), '20', '%.3f' % y1, '11', '%.3f' % mx(x2), '21', '%.3f' % y2]
    def text(layer, x, y, s, hgt=1.6, rot=0, anchor='start'):
        nonlocal out
        # DXF R12 text: 72 = horizontal justification (0 left, 1 centre, 2 right), 73 = 2 middle; 11/21 = alignment point
        hj = {'start': 0, 'middle': 1, 'end': 2}[anchor]
        out += ['0', 'TEXT', '8', layer, '10', '%.3f' % mx(x), '20', '%.3f' % y, '40', str(hgt), '1', s, '50', str(rot),
                '72', str(hj), '73', '2', '11', '%.3f' % mx(x), '21', '%.3f' % y]
    poly('OUTLINE', list(outline.exterior.coords))
    poly('SLOT', list(slot_cut.exterior.coords))
    if side == 'parts':
        for r in rings(top_copper): poly('COPPER_TOP', r)
        for r in rings(copper): poly('COPPER_BOTTOM', r)
    else:
        for r in rings(iso_bot if side == 'bottom' else iso_top, 0.005): poly('ISO_CENTER', r)
        for r in rings(clear_bot if side == 'bottom' else clear_top): poly('CLEAR_POCKET', r)
        for r in rings(copper if side == 'bottom' else top_copper): poly('COPPER_REF', r)
    for p in pads: circle(drill_layer(p['hole']), p['x'], p['y'], p['hole'] / 2)
    for a in align: circle('ALIGN_3P0', a[0], a[1], ALIGN_D / 2)
    if side == 'parts':
        for s in silk:
            k = s[0]
            if k == 'rect': poly('SILK', [(s[2], s[3]), (s[4], s[3]), (s[4], s[5]), (s[2], s[5]), (s[2], s[3])])
            elif k == 'circle': circle('SILK', s[2], s[3], s[4])
            elif k == 'line': line('SILK', s[2], s[3], s[4], s[5])
            elif k == 'label': text('SILK', s[2], s[3], s[4], 1.6, s[6] if len(s) > 6 else 0, s[5] if len(s) > 5 else 'middle')
            elif k == 'pinlabel': text('SILK', s[2], s[3], s[4], 1.2, 0, {'left': 'start', 'right': 'end', 'below': 'middle'}[s[5]])
            elif k == 'to92':
                line('SILK', s[2] - 1.4, s[3] - 3.0, s[2] - 1.4, s[3] + 3.0)
                arc = [(s[2] - 1.4 + 3.0 * math.sin(t * math.pi / 12), s[3] + 3.0 * math.cos(t * math.pi / 12)) for t in range(13)]
                poly('SILK', arc, closed=False)
    text('NOTES', 2, -6, 'INTERFACE BOARD REV %s  ' % REV + SIDE_NOTE[side] + '  UNITS MM  BOARD %gx%g' % (W, H), 2.0)
    if side != 'parts': text('NOTES', 2, -9.5, LAYER_NOTE, 1.6)
    out += ['0', 'ENDSEC', '0', 'EOF']
    open(path, 'w').write('\n'.join(out) + '\n')

def cam_svg(path, side):
    """One side at true size in millimetres for CAM programs that import SVG (Easel, Carbide Create, Inkscape...).
    Layers are groups with the same names and meaning as the DXF layers. ISO_CENTER is drawn as a stroke
    TOOL wide, so the picture shows the groove the isolation cut actually makes; CLEAR_POCKET is light grey,
    COPPER_REF black, so black plus white is what stays on the board."""
    mirror = side == 'bottom'
    mx = (lambda x: W - x) if mirror else (lambda x: x)
    P = lambda x, y: '%.3f,%.3f' % (mx(x), H - y)          # SVG y runs down, so flip; origin stays bottom-left
    path_d = lambda geom, tol=0: ''.join('M' + ' L'.join(P(x, y) for x, y in r[:-1]) + 'Z' for r in rings(geom, tol))
    lay = lambda name, attrs: '<g id="%s" inkscape:groupmode="layer" inkscape:label="%s"%s>' % (name, name, attrs)
    o = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<!-- Interface board rev %s, %s. Units mm, %g x %g. %s -->' % (REV, SIDE_NOTE[side].lower(), W, H, LAYER_NOTE.lower()),
         '<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="%gmm" height="%gmm" viewBox="0 0 %g %g">' % (W, H, W, H),
         lay('CLEAR_POCKET', '') + '<path d="%s" fill="#d4d4d4" fill-rule="evenodd" stroke="none"/></g>' % path_d(clear_bot if mirror else clear_top),
         lay('COPPER_REF', '') + '<path d="%s" fill="#000" fill-rule="evenodd" stroke="none"/></g>' % path_d(copper if mirror else top_copper),
         lay('ISO_CENTER', ' fill="none" stroke="#808080" stroke-width="%.2f" stroke-linejoin="round"' % TOOL) + '<path d="%s"/></g>' % path_d(iso_bot if mirror else iso_top, 0.005)]
    for d in drill_sizes:
        o += [lay(drill_layer(d), ' fill="none" stroke="#0000ff" stroke-width="0.05"')]
        o += ['<circle cx="%.3f" cy="%.3f" r="%.3f"/>' % (mx(p['x']), H - p['y'], p['hole'] / 2) for p in pads if p['hole'] == d]
        o += ['</g>']
    o += [lay('ALIGN_3P0', ' fill="none" stroke="#ff00ff" stroke-width="0.1"')]
    o += ['<circle cx="%.3f" cy="%.3f" r="%.3f"/>' % (mx(a[0]), H - a[1], ALIGN_D / 2) for a in align]
    o += ['</g>', lay('SLOT', ' fill="none" stroke="#ff8800" stroke-width="0.1"'), '<path d="%s"/>' % path_d(slot_cut), '</g>',
          lay('OUTLINE', ' fill="none" stroke="#ff0000" stroke-width="0.1"'),
          '<path d="%s"/>' % path_d(outline), '</g>', '</svg>']
    open(path, 'w', encoding='utf-8').write('\n'.join(o) + '\n')

def gerbers(path):
    f = lambda v: '%d' % round(v * 1e6)
    def gcu(geom, title):
        cu = ['G04 Interface board rev %s, %s, viewed from the top*' % (REV, title), '%FSLAX46Y46*%', '%MOMM*%', '%LPD*%']
        polys = list(geom.geoms) if geom.geom_type == 'MultiPolygon' else [geom]
        for p in sorted(polys, key=lambda q: -q.area):
            for i, r in enumerate([list(p.exterior.coords)] + [list(x.coords) for x in p.interiors]):
                cu.append('%LPD*%' if i == 0 else '%LPC*%'); cu.append('G36*')
                cu.append('X%sY%sD02*' % (f(r[0][0]), f(r[0][1])))
                cu += ['X%sY%sD01*' % (f(x), f(y)) for x, y in r[1:]]
                cu.append('G37*')
        return cu + ['%LPD*%', 'M02*']
    cu, cut = gcu(copper, 'bottom copper'), gcu(top_copper, 'top copper')
    ol = ['G04 Interface board rev %s outline and slot*' % REV, '%FSLAX46Y46*%', '%MOMM*%', '%ADD10C,0.100*%', 'D10*']
    for r in [list(profile.exterior.coords)]:
        ol.append('X%sY%sD02*' % (f(r[0][0]), f(r[0][1]))); ol += ['X%sY%sD01*' % (f(x), f(y)) for x, y in r[1:]]
    ol.append('M02*')
    holes = [(p['x'], p['y'], p['hole']) for p in pads] + [(a[0], a[1], ALIGN_D) for a in align]
    sizes = sorted({h[2] for h in holes})
    dr = ['M48', 'METRIC,TZ'] + ['T%dC%.3f' % (i + 1, s) for i, s in enumerate(sizes)] + ['%', 'G90', 'G05']
    for i, s in enumerate(sizes):
        dr.append('T%d' % (i + 1)); dr += ['X%.3fY%.3f' % (x, y) for x, y, d in holes if d == s]
    dr.append('M30')
    # Gerbers only: the DXFs and SVGs are separate downloads, and repeating them here would double the
    # space the board files take on the ESP32.
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr('interface_board-F_Cu.gtl', '\n'.join(cut) + '\n')
        z.writestr('interface_board-B_Cu.gbl', '\n'.join(cu) + '\n')
        z.writestr('interface_board-Edge_Cuts.gm1', '\n'.join(ol) + '\n')
        z.writestr('interface_board.drl', '\n'.join(dr) + '\n')
        z.writestr('README.txt',
                   'Interface board rev %s as Gerber + Excellon files, for PCB CAM programs such as FlatCAM. These are not DXF files:\n'
                   'for DXF or SVG CAM use %s and %s (or the .svg versions).\n\n' % (REV, FN('bottom.dxf'), FN('top.dxf')) +
                   'Two-sided board, no plated holes. Both coppers are drawn viewed from the top, as Gerber requires,\n'
                   'so mirror the bottom (B_Cu) when you mill it copper side up. The drill file includes the two\n'
                   '%.1f mm alignment holes on the flip line x = %.1f. The edge cuts include the %.1f mm wide slot\n'
                   'under U1: cut it with an end mill of 1.5 mm or less.\n\n' % (ALIGN_D, W / 2, SLOT['w']) +
                   'Units mm. Drill sizes: ' + ', '.join('%.1f mm' % s for s in sizes) + '\n')

NET_COL = {'+12V': '#f59e0b', '3V3': '#ef4444', 'SPA': '#facc15', 'SPB': '#22c55e', 'SPZ': '#fb923c', 'CPIN': '#2dd4bf',
           'XA': '#38bdf8', 'XB': '#818cf8', 'ZA': '#f472b6', 'ZB': '#c084fc', 'GPIO2': '#fb7185', 'U1A': '#fb7185',
           'HIGH': '#fdba74', 'WIPER': '#fef08a', 'LOW': '#a8a29e'}
def svg(path, assembly):
    S = 6.0
    pad_l, pad_r, pad_t, pad_b = (230, 300, 40, 40) if assembly else (20, 20, 20, 40)
    VW, VH = W * S + pad_l + pad_r, H * S + pad_t + pad_b
    X = lambda x: pad_l + x * S
    Y = lambda y: pad_t + (H - y) * S
    pts = lambda r: ' '.join('%.1f,%.1f' % (X(x), Y(y)) for x, y in r)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" font-family="Segoe UI,Roboto,Helvetica,Arial,sans-serif">' % (VW, VH),
         '<rect width="%d" height="%d" fill="#0a1020"/>' % (VW, VH),
         '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="6" fill="#14532d" stroke="#22c55e"/>' % (X(0), Y(H), W * S, H * S)]
    # Copper on the far side, drawn faint; signal nets tinted by color
    d = ''.join('M' + pts(r) + 'Z' for r in rings(fill, 0.08))
    o.append('<path d="%s" fill="#b45309" fill-opacity=".22" fill-rule="evenodd"/>' % d)
    for n, g in islands.items():
        d = ''.join('M' + pts(r) + 'Z' for r in rings(g, 0.08))
        o.append('<path d="%s" fill="%s" fill-opacity=".55" fill-rule="evenodd"/>' % (d, NET_COL.get(n, '#e6edf7')))
    if not assembly:
        o.append('<path d="M%s Z" fill="none" stroke="#f59e0b" stroke-dasharray="6 5"/>' % pts(list(creep_clear.exterior.coords)) if creep_clear.geom_type == 'Polygon' else '')
    # Top ground fill as a light sheen with its isolation rings, top traces solid on top of it
    d = ''.join('M' + pts(r) + 'Z' for r in rings(top_fill, 0.08))
    o.append('<path d="%s" fill="#fde68a" fill-opacity=".10" stroke="#fde68a" stroke-opacity=".35" stroke-width=".8" fill-rule="evenodd"/>' % d)
    for t in top_traces:
        o.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (pts(t['pts']), NET_COL[t['net']], t['w'] * S))
    o.append('<path d="M%sZ" fill="#0a1020" stroke="#f59e0b"/>' % pts(list(slot_in.exterior.coords)))
    for a in align:
        o.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#0a1020" stroke="#e879f9" stroke-width="2"/>' % (X(a[0]), Y(a[1]), ALIGN_D / 2 * S))
        o.append('<text x="%.1f" y="%.1f" font-size="11" fill="#e879f9" text-anchor="end" dominant-baseline="middle">align pin</text>' % (X(a[0]) - ALIGN_D / 2 * S - 4, Y(a[1])))
    for p in pads:
        ring = '#fbbf24' if p['via'] else '#cbd5e1'
        o.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/><circle cx="%.1f" cy="%.1f" r="%.1f" fill="#0a1020"/>' % (X(p['x']), Y(p['y']), p['r'] * S, ring, X(p['x']), Y(p['y']), p['hole'] / 2 * S))
    for s in silk:
        k = s[0]
        if k == 'rect': o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="none" stroke="#f8fafc" stroke-width="1.4"/>' % (X(s[2]), Y(s[5]), (s[4] - s[2]) * S, (s[5] - s[3]) * S))
        elif k == 'circle': o.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="#f8fafc" stroke-width="1.4"/>' % (X(s[2]), Y(s[3]), s[4] * S))
        elif k == 'line': o.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#f8fafc" stroke-width="1.4"/>' % (X(s[2]), Y(s[3]), X(s[4]), Y(s[5])))
        elif k == 'to92':
            x, y, dd = s[2], s[3], s[4]
            o.append('<path d="M%.1f %.1f A%.1f %.1f 0 0 %d %.1f %.1f Z" fill="none" stroke="#f8fafc" stroke-width="1.4"/>' % (X(x - 1.4), Y(y + 3.0), 3.0 * S, 3.0 * S, 1, X(x - 1.4), Y(y - 3.0)))
        elif k == 'label':
            anchor = s[5] if len(s) > 5 else 'middle'; rot = s[6] if len(s) > 6 else 0
            o.append('<text x="%.1f" y="%.1f" font-size="13" font-weight="700" fill="#f8fafc" text-anchor="%s" dominant-baseline="middle"%s>%s</text>' %
                     (X(s[2]), Y(s[3]), anchor, ' transform="rotate(%d %.1f %.1f)"' % (-rot, X(s[2]), Y(s[3])) if rot else '', s[4]))
        elif k == 'pinlabel':
            anchor = {'left': 'start', 'right': 'end', 'below': 'middle'}[s[5]]
            dx = 0 if s[5] == 'below' else 0
            o.append('<text x="%.1f" y="%.1f" font-size="11" fill="#bbf7d0" text-anchor="%s" dominant-baseline="middle">%s</text>' % (X(s[2]) + dx, Y(s[3]), anchor, s[4]))
        elif k == 'plus': o.append('<text x="%.1f" y="%.1f" font-size="12" fill="#f8fafc" text-anchor="middle" dominant-baseline="middle">+</text>' % (X(s[2]), Y(s[3])))
    o.append('<text x="%.1f" y="%.1f" font-size="12" fill="#f59e0b" text-anchor="middle">slot</text>' % (X(SLOT['x']), Y(SLOT['y0']) + 16))
    o.append('<text x="%.1f" y="%.1f" font-size="13" font-weight="700" fill="#8fa1c2">Interface board rev %s · %g × %g mm · two-sided</text>' % (X(0), Y(0) + 24, REV, W, H))
    if assembly:
        o += assembly_parts(X, Y, S, VW, VH)
    o.append('</svg>')
    open(path, 'w', encoding='utf-8').write('\n'.join(o))

def assembly_parts(X, Y, S, VW, VH):
    """External connections around the board: the three encoders, supply, G540, DB25, ESP32 and lathe."""
    o = []
    def wire(x1, y1, x2, y2, col, w=3.5):
        halo = '<path d="M%.1f %.1f C%.1f %.1f %.1f %.1f %.1f %.1f" stroke="#e6edf7" stroke-width="%.1f" fill="none"/>' % (x1, y1, (x1 + x2) / 2, y1, (x1 + x2) / 2, y2, x2, y2, w + 2) if col == '#0b0f19' else ''
        return halo + '<path d="M%.1f %.1f C%.1f %.1f %.1f %.1f %.1f %.1f" stroke="%s" stroke-width="%.1f" fill="none"/>' % (x1, y1, (x1 + x2) / 2, y1, (x1 + x2) / 2, y2, x2, y2, col, w)
    def encoder(cx, cy, title, sub):
        return ('<circle cx="%d" cy="%d" r="34" fill="#334155" stroke="#94a3b8" stroke-width="2"/><circle cx="%d" cy="%d" r="7" fill="#94a3b8"/>' % (cx, cy, cx, cy) +
                '<rect x="%d" y="%d" width="10" height="16" fill="#64748b"/>' % (cx + 32, cy - 8) +
                '<text x="%d" y="%d" font-size="14" font-weight="700" fill="#e6edf7" text-anchor="middle">%s</text>' % (cx, cy - 44, title) +
                '<text x="%d" y="%d" font-size="11" fill="#8fa1c2" text-anchor="middle">%s</text>' % (cx, cy + 50, sub))
    cols = {'+12V': '#b45309', '0V': '#3b82f6', 'A': '#0b0f19', 'B': '#f8fafc', 'Z': '#f97316', 'SH': '#94a3b8'}
    def feed(cx, cy, ref, names):
        s = ''
        for nm in names:
            pin = '0V/SH' if nm in ('0V', 'SH') else nm          # the shield shares the 0V screw
            p = next(p for p in pads if p['ref'] == ref and p['pin'] == pin)
            s += wire(cx + 42, cy, X(p['x']) - 22, Y(p['y']), cols[nm])
            s += '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="3.5"/>' % (X(p['x']) - 22, Y(p['y']), X(p['x']) - 8, Y(p['y']), cols[nm] if cols[nm] != '#0b0f19' else '#e6edf7')
        return s
    o.append(encoder(70, Y(J3y[2]), 'Spindle encoder', '100 P/R') + feed(70, Y(J3y[2]), 'J3', ['+12V', '0V', 'A', 'B', 'Z', 'SH']))
    o.append(encoder(70, Y(J1y[2]), 'X encoder', '600 P/R') + feed(70, Y(J1y[2]), 'J1', ['+12V', '0V', 'A', 'B', 'SH']))
    o.append(encoder(70, Y(J2y[2]) - 6, 'Z encoder', '600 P/R') + feed(70, Y(J2y[2]) - 6, 'J2', ['+12V', '0V', 'A', 'B', 'SH']))
    # 12 V supply to J4 and DB25 to J6
    o.append('<rect x="20" y="4" width="120" height="30" rx="6" fill="#1e293b" stroke="#475569"/><text x="80" y="24" font-size="13" font-weight="700" fill="#e6edf7" text-anchor="middle">12 V supply</text>')
    for x, yy, col in [(J4x[0], 13, '#b45309'), (J4x[1], 25, '#3b82f6')]:
        o.append('<path d="M140 %d H%.1f V%.1f" stroke="%s" stroke-width="3.5" fill="none"/>' % (yy, X(x), Y(YT) - 9, col))
    o.append('<rect x="20" y="%d" width="120" height="36" rx="6" fill="#1e293b" stroke="#475569"/><text x="80" y="%d" font-size="12" font-weight="700" fill="#e6edf7" text-anchor="middle">DB25 16 / 18</text><text x="80" y="%d" font-size="10" fill="#8fa1c2" text-anchor="middle">optional</text>' % (Y(J6y[0]) - 14, Y(J6y[0]) + 2, Y(J6y[0]) + 15))
    o.append(wire(140, Y(J6y[0]), X(XT) - 8, Y(J6y[0]), '#2dd4bf') + wire(140, Y(J6y[0]) + 8, X(XT) - 8, Y(J6y[1]), '#64748b'))
    # G540 above J5
    o.append('<rect x="%.1f" y="4" width="%.1f" height="26" rx="6" fill="#1e293b" stroke="#475569"/><text x="%.1f" y="22" font-size="13" font-weight="700" fill="#e6edf7" text-anchor="middle">To G540 terminals 2, 3, 1 and 12</text>' % (X(J5x[0]) - 40, X(J5x[3]) - X(J5x[0]) + 80, (X(J5x[0]) + X(J5x[3])) / 2))
    # ESP32 and lathe on the right
    ex = X(W) + 40
    # J7 wires drop down beside the isolated corner before turning right, so they never cross the lathe side
    j7t = [Y(SLOT['y0']) + 26 + 14 * k for k in range(4)]
    top, bot = j7t[0] - 14, Y(J8y[-1]) + 44
    o.append('<rect x="%.1f" y="%.1f" width="140" height="%.1f" rx="8" fill="#123027" stroke="#475569"/><text x="%.1f" y="%.1f" font-size="15" font-weight="700" fill="#e6edf7" text-anchor="middle">ESP32-S3</text><text x="%.1f" y="%.1f" font-size="11" fill="#8fa1c2" text-anchor="middle">DevKitC-1 header pins</text>' % (ex + 110, top, bot - top, ex + 180, bot - 24, ex + 180, bot - 9))
    gcol = {'GPIO4': '#facc15', 'GPIO5': '#22c55e', 'GPIO6': '#fb923c', 'GPIO18': '#2dd4bf', 'GPIO2': '#fb7185', 'GND': '#64748b', 'GPIO7': '#38bdf8', 'GPIO15': '#818cf8', 'GPIO16': '#f472b6', '3V3': '#ef4444', 'GPIO17': '#c084fc'}
    for p in pads:
        if p['ref'] == 'J7':
            k = J7y.index(p['y']); xv = X(XR) + 12 + 4 * (3 - k)     # in the gap between J7 and the GPIO2 column
            o.append('<path d="M%.1f %.1f H%.1f V%.1f H%.1f" stroke="%s" stroke-width="3" fill="none"/>' % (X(p['x']) + 8, Y(p['y']), xv, j7t[k], ex + 110, gcol[p['pin']]))
            o.append('<text x="%.1f" y="%.1f" font-size="11" fill="#e6edf7" dominant-baseline="middle">%s</text>' % (ex + 116, j7t[k], p['pin']))
        elif p['ref'] == 'J8':
            o.append(wire(X(p['x']) + 8, Y(p['y']), ex + 110, Y(p['y']), gcol[p['pin']], 3))
            o.append('<text x="%.1f" y="%.1f" font-size="11" fill="#e6edf7" dominant-baseline="middle">%s</text>' % (ex + 116, Y(p['y']), p['pin']))
    lt = Y(J9y[0]) - 46
    o.append('<rect x="%.1f" y="%.1f" width="140" height="%.1f" rx="8" fill="#3b1a1a" stroke="#7f1d1d"/><text x="%.1f" y="%.1f" font-size="13" font-weight="700" fill="#e6edf7" text-anchor="middle">Lathe speed pot</text><text x="%.1f" y="%.1f" font-size="11" fill="#f87171" text-anchor="middle">may be at mains voltage</text>' % (ex + 110, lt, top - 8 - lt, ex + 180, lt + 17, ex + 180, lt + 32))
    for p, col in zip([p for p in pads if p['ref'] == 'J9'], ['#fdba74', '#fef08a', '#a8a29e']):
        o.append(wire(X(p['x']) + 8, Y(p['y']), ex + 110, Y(p['y']), col, 3))
        o.append('<text x="%.1f" y="%.1f" font-size="11" fill="#e6edf7" dominant-baseline="middle">%s</text>' % (ex + 116, Y(p['y']), p['pin']))
    return o

# ---------- outputs ----------
# Every file name carries the board revision (board_revD_bottom.dxf ...), so files from different
# revisions can't be mixed up. Files of any other revision are deleted here and on the ESP32.
# Names stay short: mklittlefs can't store a name longer than 31 characters (32 with the terminator),
# and the ESP32 copies are 3 characters longer (.gz).
FN = lambda part: 'board_rev%s_%s' % (REV, part)
for f in os.listdir(HERE):
    if f.startswith(('interface_board', 'board_rev')) and os.path.isfile(os.path.join(HERE, f)): os.remove(os.path.join(HERE, f))
for side in ('bottom', 'top', 'parts'): dxf(os.path.join(HERE, FN(side + '.dxf')), side)
svg(os.path.join(HERE, FN('preview.svg')), assembly=False)
cam_svg(os.path.join(HERE, FN('bottom.svg')), 'bottom')
cam_svg(os.path.join(HERE, FN('top.svg')), 'top')

bom = {}
for s in silk:
    if s[0] == 'value': bom.setdefault(s[2], []).append(s[1])
vias_ = [p for p in pads if p['via']]
nh = {}
for d in [p['hole'] for p in pads] + [ALIGN_D] * len(align): nh[d] = nh.get(d, 0) + 1
lines = ['Interface board rev %s, %g x %g mm, two-sided, no plated holes' % (REV, W, H), REV_NOTE, ''] + report + ['', 'Holes: ' + ', '.join('%d x %.1f mm' % (nh[h], h) for h in sorted(nh)), '',
         'Parts:'] + ['  %-10s %s' % (v, ', '.join(sorted(r, key=lambda q: (q[0], int(''.join(c for c in q if c.isdigit()) or 0))))) for v, r in sorted(bom.items())] + \
        ['', 'Via pins (bare wire through the hole, soldered top and bottom): %d' % len(vias_)] + \
        ['  %-4s %-5s (%.1f, %.1f)' % (p['ref'], p['net'], p['x'], p['y']) for p in vias_] + \
        ['', 'Alignment holes, %.1f mm, on the flip line x = %.1f: ' % (ALIGN_D, W / 2) + ', '.join('(%.1f, %.1f)' % a for a in align)]
open(os.path.join(HERE, FN('report.txt')), 'w').write('\n'.join(lines) + '\n')
gerbers(os.path.join(HERE, FN('gerber.zip')))

# Keep Help's download links on this revision: the page builds every board file name from BOARD_REV.
page = os.path.join(HERE, '..', 'data', 'index.html')
src_ = open(page, encoding='utf-8', newline='').read()
new_ = re.sub(r"const BOARD_REV='[^']*'", "const BOARD_REV='%s'" % REV, src_)
if new_ != src_: open(page, 'w', encoding='utf-8', newline='').write(new_)

# Every file also goes on the ESP32 (data/hw), so the dashboard can hand them out. The DXFs are stored
# gzip-compressed: the web server answers a request for x.dxf with x.dxf.gz and gzip encoding, and the
# browser saves an ordinary x.dxf.
hw = os.path.join(HERE, '..', 'data', 'hw')
os.makedirs(hw, exist_ok=True)
for f in os.listdir(hw): os.remove(os.path.join(hw, f))
svg(os.path.join(hw, 'board_assembly.svg'), assembly=True)
for f in [FN('preview.svg'), FN('gerber.zip'), FN('report.txt')]:
    shutil.copy(os.path.join(HERE, f), os.path.join(hw, f))
for f in [FN(x) for x in ('bottom.dxf', 'top.dxf', 'parts.dxf', 'bottom.svg', 'top.svg')]:
    with open(os.path.join(HERE, f), 'rb') as src, gzip.GzipFile(os.path.join(hw, f + '.gz'), 'wb', 9, mtime=0) as dst:
        dst.write(src.read())
too_long = [f for f in os.listdir(hw) if len(f) > 31]
check('file names on the ESP32 fit LittleFS (31 characters or fewer)', not too_long, ', '.join(too_long))
if too_long: print('FAIL file names too long for LittleFS: ' + ', '.join(too_long))
total = sum(os.path.getsize(os.path.join(hw, f)) for f in os.listdir(hw))
print('\nCopied to data/hw: ' + ', '.join(sorted(os.listdir(hw))) + ' (%d KB)' % (total // 1024))
print('\n'.join(lines))
sys.exit(1 if errors else 0)

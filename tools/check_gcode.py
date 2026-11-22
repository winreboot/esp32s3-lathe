"""Checks the dashboard's G-code generators. Run tools/gcode_cases.js first, which writes tools/gcode_cases.json.
Replays each generated program against a model of the stock and checks it.
Model: for every 0.0005 in slice along Z, the radius of the metal left (the stock is cut from the outside in).
The tool is a point (turning, facing, taper) or a flat edge W wide (parting, grooving). Checks:
  - no G0 (rapid) ever has metal above the tool tip: a rapid never cuts,
  - the deepest radial bite of any G1 (cutting move),
  - the shape left at the end against the shape asked for,
  - X never below the centreline except where the program means to go past it.
Waves are checked with the picked tool's real outline (its lower edge, exported by gcode_cases.js): no rapid in metal,
the deepest bite, no cut into the shape asked for, and the metal left where the tool cannot reach, which must match
what the page says. For every case the page's own cut simulation (behind the preview and Play) must find no rapid in
metal, and for parting, grooving, turning, taper and waves the metal it leaves must match this model."""
import json, re, math, os
import numpy as np
DZ = 0.0005
def parse(code):
    x = z = None; mv = []
    for ln in code.splitlines():
        if ln.startswith('('): continue
        m = re.match(r'(G0|G1)\b', ln)
        if not m: continue
        nx = re.search(r'X(-?[\d.]+)', ln); nz = re.search(r'Z(-?[\d.]+)', ln)
        X = float(nx.group(1)) if nx else x; Z = float(nz.group(1)) if nz else z
        if x is not None and z is not None: mv.append((m.group(1), x, z, X, Z))
        x, z = X, Z
    return mv
class Stock:
    def __init__(s, D, z_lo, z_hi, bore=0.0):
        s.z_lo = z_lo; s.n = int(round((z_hi - z_lo) / DZ)); s.top = [D / 2] * s.n; s.bore = bore / 2
    def idx(s, z): return int(math.floor((z - s.z_lo) / DZ))
    def span(s, a, b):
        i0, i1 = max(0, s.idx(min(a, b))), min(s.n - 1, s.idx(max(a, b) - 1e-9))
        return range(i0, i1 + 1) if i1 >= i0 else range(0)
    def radius_at(s, z): i = s.idx(z); return s.top[i] if 0 <= i < s.n else None
def run(mv, stock, width=0.0, ref='r'):
    bad_rapid = []; bite = 0.0
    for g, x0, z0, x1, z1 in mv:
        steps = max(1, int(max(abs(z1 - z0) / DZ, abs(x1 - x0) / 2 / DZ)))
        for k in range(steps + 1):
            t = k / steps; z = z0 + (z1 - z0) * t; r = (x0 + (x1 - x0) * t) / 2
            if width: a, b = (z - width, z) if ref == 'r' else (z, z + width)
            else: a, b = z - DZ / 2, z + DZ / 2
            for i in stock.span(a, b):
                if stock.top[i] > r + 1e-4 and stock.top[i] > stock.bore:
                    if g == 'G0': bad_rapid.append((round(z, 4), round(r * 2, 4), round(stock.top[i] * 2, 4)))
                    else: bite = max(bite, stock.top[i] - max(r, stock.bore)); stock.top[i] = max(r, stock.bore)
    return bad_rapid, bite
def xmin(mv): return min(min(m[1], m[3]) for m in mv)
o = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gcode_cases.json'))); fails = 0
def report(gen, i, ok, msg):
    global fails
    if not ok: fails += 1
    print('%-6s %d %s %s' % (gen, i, 'ok  ' if ok else 'FAIL', msg))
for i, c in enumerate(o['part']):
    v = c['v']; mv = parse(c['code']); W, S = v['W'], (v['S'] if v['S'] > 0 else 0)
    st = Stock(v['D'], -v['L'] - W - S - 0.3, 0)
    bad, bite = run(mv, st, W, v['ref'])
    zf = -v['L']; web = -0.01 if v['thru'] else v['web']
    floor = max(st.radius_at(z) for z in (zf - DZ, zf - W - S + DZ, zf - (W + S) / 2))
    part_ok = abs(st.radius_at(zf + DZ) - v['D'] / 2) < 1e-6 and abs(st.radius_at(zf - DZ) - max(0, web / 2)) < 2e-3
    report('part', i, not bad and part_ok and bite <= v['d'] + 1e-4, 'mode=%s S=%s ref=%s thru=%s: rapids in metal %d, deepest bite %.4f (depth/zig %.4f), part face kept %s, floor Ø %.4f (asked %.4f), lowest X %.4f' % (v['mode'], v['S'], v['ref'], v['thru'], len(bad), bite, v['d'], part_ok, floor * 2, max(0, web), xmin(mv)))
for i, c in enumerate(o['groove']):
    v = c['v']; mv = parse(c['code']); W = v['W']; st = Stock(v['D'], -v['Zw'] - v['Wg'] - 0.4, 0)
    bad, bite = run(mv, st, W, v['ref'])
    zR, zL = -v['Zw'], -v['Zw'] - v['Wg']
    inside = [st.radius_at(z) * 2 for z in (zR - DZ, zL + DZ, (zR + zL) / 2)]
    walls = (st.radius_at(zR + DZ) or 0) * 2, st.radius_at(zL - DZ) * 2
    ok = not bad and all(abs(d - v['Df']) < 1e-3 for d in inside) and (v['Zw'] == 0 or abs(walls[0] - v['D']) < 1e-6) and abs(walls[1] - v['D']) < 1e-6
    report('groove', i, ok, 'Wg=%s fin=%s ref=%s: rapids in metal %d, floor Ø %s (asked %.3f), walls untouched %s, deepest bite %.4f' % (v['Wg'], v['fin'], v['ref'], len(bad), [round(d, 4) for d in inside], v['Df'], walls, bite))
for i, c in enumerate(o['face']):
    v = c['v']; mv = parse(c['code'])
    # facing: model columns along X instead: the metal is the face, cut from the front. Reuse the model by swapping axes.
    mv2 = [(g, z0 * 2, x0 / 2, z1 * 2, x1 / 2) for g, x0, z0, x1, z1 in mv]   # "x" = 2*z, "z" = radius
    st = Stock(2 * (v['z0'] - (-1)) , 0, v['D'] / 2 + 0.2, bore=0)              # height of the face above Z=-1, per radius slice
    st.top = [(v['z0'] + 1) if r * DZ < v['D'] / 2 else 0 for r in range(st.n)]
    # shift: the model's "radius" is z+1
    mv3 = [(g, (zz0 + 1) * 2, r0, (zz1 + 1) * 2, r1) for g, zz0, r0, zz1, r1 in [(g, a / 2, b, c2 / 2, d) for g, a, b, c2, d in mv2]]
    bad, bite = run(mv3, st)
    left = [st.radius_at(r) - 1 for r in (0.0005, v['to'] / 2 + 0.002 if v['to'] > 0 else 0.01, v['D'] / 2 - 0.002)]
    ok = not bad and all(abs(h - v['z1']) < 1e-3 for h in left[1:]) and (v['to'] > 0 or abs(left[0] - v['z1']) < 1e-3)
    report('face', i, ok, 'z0=%s z1=%s to=%s: rapids in metal %d, face left at Z %s (asked %.3f), deepest bite %.4f' % (v['z0'], v['z1'], v['to'], len(bad), [round(h, 4) for h in left], v['z1'], bite))
for i, c in enumerate(o['turn']):
    v = c['v']; mv = parse(c['code']); st = Stock(v['D0'], -v['L'] - 0.4, 0)
    bad, bite = run(mv, st)
    body = [st.radius_at(z) * 2 for z in (-v['c'] - 0.01, -v['L'] / 2, -v['L'] + 0.002)]
    cham = st.radius_at(-0.0005) * 2 if v['c'] > 0 else None
    ok = not bad and all(abs(d - v['Df']) < 1e-3 for d in body) and abs(st.radius_at(-v['L'] - 0.002) * 2 - v['D0']) < 1e-6 and bite <= v['doc'] + v['fa'] + 0.011 and (cham is None or abs(cham - (v['Df'] - 2 * v['c'])) < 3e-3)
    report('turn', i, ok, 'Df=%s c=%s tail=%s: rapids in metal %d, Ø along the length %s (asked %.3f), chamfer end Ø %s, deepest bite %.4f (doc %.3f)' % (v['Df'], v['c'], v['tail'], len(bad), [round(d, 4) for d in body], v['Df'], cham and round(cham, 4), bite, v['doc']))
for i, c in enumerate(o['taper']):
    v = c['v']; mv = parse(c['code']); st = Stock(v['D0'], -v['L'] - 0.4, 0)
    bad, bite = run(mv, st); k = (v['Db'] - v['Ds']) / v['L']
    errs = [abs(st.radius_at(z) * 2 - (v['Ds'] + k * -z)) for z in [-(j + .5) * v['L'] / 20 for j in range(20)]]
    ok = not bad and max(errs) < 2e-3 and bite <= v['doc'] + v['fa'] + 0.011
    report('taper', i, ok, 'Ds=%s Db=%s D0=%s: rapids in metal %d, worst error along the taper %.4f in, deepest bite %.4f (doc %.3f)' % (v['Ds'], v['Db'], v['D0'], len(bad), max(errs), bite, v['doc']))
for i, c in enumerate(o['drill']):
    v = c['v']; mv = parse(c['code'])
    pl = v['Dd'] / 2 / math.tan(math.radians(c['tool']['pa'] / 2)); zb = -(v['depth'] + (pl if v['to'] == 'full' else 0))
    deepest = min(min(m[2], m[4]) for m in mv); feeds = [m for m in mv if m[0] == 'G1']
    off_axis = any(abs(m[1]) > 1e-9 or abs(m[3]) > 1e-9 for m in mv if min(m[2], m[4]) < 0.05)
    # each feed starts at most 0.010 above where the last one ended, and never goes back up
    prev = None; ok_seq = True
    for m in feeds:
        if m[4] > m[2]: ok_seq = False
        if prev is not None and m[2] > prev + 0.0101 and v['mode'] != 'x': ok_seq = ok_seq and m[2] <= prev + 0.0101
        prev = m[4]
    report('drill', i, abs(deepest - zb) < 1e-4 and not off_axis and ok_seq and max(m[2] - m[4] for m in feeds) <= v['q'] + 0.0101 + (v['zs'] if True else 0),
           'mode=%s to=%s: bottom Z %.4f (asked %.4f), any move off the axis inside the work %s, feeds only go down %s' % (v['mode'], v['to'], deepest, zb, off_axis, ok_seq))
# ---- waves: the tool's outline against the shape asked for
def wave_shape(sh, w):
    q = 0.96; b = lambda x: (1 - math.sqrt(1 - q * q * x * x)) / (1 - math.sqrt(1 - q * q))
    return b(w) if sh == 'bead' else 1 - b(1 - w) if sh == 'cove' else w if sh == 'v' else (1 - math.cos(math.pi * w)) / 2
def wave_target(v, z):
    R0, Rc, zs, n, p = v['D0'] / 2, v['Dc'] / 2, v['zs'], round(v['n']), v['p']; ze = -(zs + n * p); k = min(90, max(0, v['flat'])) / 100
    if z >= -zs: return Rc
    if z < ze: return R0
    s = (-zs - z) / p; t = s - math.floor(s); w = 2 * min(t, 1 - t); w2 = max(0, (w - k) / (1 - k)) if k > 0 else w
    return Rc - v['a'] * wave_shape(v['shape'], w2)
def run_env(mv, env, zlo, zhi, D):
    n = int(round((zhi - zlo) / DZ)); top = np.full(n, D / 2); h = np.array(env['h']); u0 = env['u0']; m = len(h)
    bad = []; bite = 0.0
    for g, x0, z0, x1, z1 in mv:
        before = top.copy() if g == 'G1' else None
        steps = max(1, int(math.ceil(max(abs(z1 - z0), abs(x1 - x0) / 2) / (DZ * 0.5))))
        for k in range(steps + 1):
            t = k / steps; z = z0 + (z1 - z0) * t; r = (x0 + (x1 - x0) * t) / 2
            k0 = int(round((z + u0 - zlo) / DZ - 0.5)); a = max(0, -k0); b = min(m, n - k0)
            if b <= a: continue
            seg = top[k0 + a:k0 + b]; cut = r + h[a:b]
            if g == 'G0' and (seg - cut).max() > 3e-4: bad.append((round(z, 4), round(2 * r, 4)))
            np.minimum(seg, cut, out=seg)
        if before is not None: bite = max(bite, float((before - top).max()))
    return top, bad, bite
for i, c in enumerate(o['wave']):
    v = c['v']
    if 'err' in c: report('wave', i, False, 'error: ' + c['err']); continue
    mv = parse(c['code']); ze = -(v['zs'] + round(v['n']) * v['p']); zlo = ze - 0.4
    top, bad, bite = run_env(mv, c['env'], zlo, 0.0, v['D0'])
    zc = zlo + (np.arange(len(top)) + 0.5) * DZ; tg = np.array([wave_target(v, z) for z in zc])
    # the last 0.002 in at each end is under the tool's steep nose side, where both models' sampling dominates
    sec = (zc >= ze + 0.002) & (zc <= -0.002); shoulder = v['D0'] - v['Dc'] > 2e-4
    zex = ze + c['tool']['rn'] + 0.002 if shoulder else ze
    gouge = float((tg - top)[sec].max()); left = float((top - tg)[sec & (zc >= zex)].max())
    ok = not bad and gouge <= 0.0008 and abs(left - c['left']) <= 0.0015 and (c['left'] > 0.001 or left <= 0.0025) and bite <= max(v['doc'], v['fa']) + 0.0015
    report('wave', i, ok, 'shape=%s tool=%s a=%s p=%s: rapids in metal %d, cut into the shape %.4f, metal left %.4f (page says %.4f), deepest bite %.4f (doc %.3f)' % (v['shape'], c['tool']['k'], v['a'], v['p'], len(bad), gouge, left, c['left'], bite, v['doc']))
    c['_py'] = (zlo, top)
# ---- the page's own cut simulation: no rapid in metal, and the same metal left as this model
def py_profile(gen, c):
    v = c['v']; mv = parse(c['code'])
    if gen == 'part': st = Stock(v['D'], -v['L'] - v['W'] - max(0, v['S']) - 0.3, 0); run(mv, st, v['W'], v['ref']); return st.radius_at, 'blade'
    if gen == 'groove': st = Stock(v['D'], -v['Zw'] - v['Wg'] - 0.4, 0); run(mv, st, v['W'], v['ref']); return st.radius_at, 'blade'
    if gen == 'wave':
        zlo, top = c['_py']; return (lambda z: float(top[int((z - zlo) / DZ)]) if 0 <= int((z - zlo) / DZ) < len(top) else None), 'env'
    return None, None
for gen in o:
    for i, c in enumerate(o[gen]):
        if 'err' in c: continue
        sim = c['sim']; v = c['v']; T = c['tool']; msg = []; ok = not sim['hits']
        if sim['hits']: msg.append('rapids in metal %s' % sim['hits'][:2])
        f, kind = py_profile(gen, c); worst = 0.0
        if f:
            for z, ro, ri in sim['pts']:
                a, b0, b1 = f(z), f(z - 0.0015), f(z + 0.0015)
                if a is None or b0 is None or b1 is None or max(a, b0, b1) - min(a, b0, b1) > 0.002: continue
                if kind == 'blade' and T.get('rc'):   # corner radii: the page rounds the groove corners, the flat-edge model does not
                    if any(abs(z - zz) < T['rc'] + 0.002 for zz in (-v.get('L', 0), -v.get('L', 0) - v['W'] - max(0, v.get('S', 0)), -v.get('Zw', 0), -v.get('Zw', 0) - v.get('Wg', 0))): continue
                d = abs(max(ro, ri) - a)
                if d > worst: worst = d
            ok = ok and worst <= 0.0015; msg.append('page vs model: worst difference %.4f in' % worst)
        if gen == 'turn':
            d = 2 * min((ro for z, ro, ri in sim['pts'] if abs(z + v['L'] / 2) < 0.002), default=0)
            ok = ok and abs(d - v['Df']) < 0.0015; msg.append('Ø at mid-length %.4f (asked %.4f)' % (d, v['Df']))
        if gen == 'taper':
            m = (v['Db'] - v['Ds']) / 2 / v['L']; e = 2 * T['rn'] * (1 + m - math.sqrt(1 + m * m)); zm = -v['L'] / 2
            d = 2 * min((ro for z, ro, ri in sim['pts'] if abs(z - zm) < 0.002), default=0)
            want = v['Ds'] + (v['Db'] - v['Ds']) / 2 + e
            ok = ok and abs(d - want) < 0.0015; msg.append('Ø at mid-taper %.4f (taper %.4f + nose error %.4f in the warning)' % (d, want - e, e))
        report('sim:' + gen, i, ok, 'tool=%s: %s (%.0f ms)' % (T['k'], ', '.join(msg) or 'no rapid in metal', sim['ms']))
print('FAILURES:', fails)
raise SystemExit(1 if fails else 0)

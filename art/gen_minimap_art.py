"""Generate src/minimap_art.h: the minimap frame, tray, N medallion and weather icons as solid rects.

The SVG art is rendered with headless Edge (its lighting filters need a browser), then each image
is cut into same-colour runs that are merged down the rows. Pieces:

  frame buckets   whole frame at small sizes; the mod draws the nearest bucket at or above the size
  frame 370       the frame at its design size, with stretch zones on plain stretches of rail, so
                  past 370 px the ornaments keep their size and only the rails get longer; the
                  zones themselves are left empty and filled from
  rail strips     300 px of straight, aged rail per zone, so long rails keep their grime and chips
                  instead of smearing one stretched pixel column
  tray            the slate control tray at its one fixed size, with a stretch zone in the middle
  medallion       the N medallion back (the letter is drawn live, localized)
  icons           sun, moon, rain, storm, snow, fog

Rect coordinates are pixels of a 370 px render of the 300-unit frame grid (or of the bucket).
usage: python gen_minimap_art.py   (from this folder; writes ..\\src\\minimap_art.h)
"""
import os
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_frame as bf   # noqa: E402
import scene as sc         # noqa: E402
from svgshot import shot   # noqa: E402

DESIGN = 370
K = DESIGN / 300.0
BUCKETS = [150, 180, 215, 255, 300, 340]
STRIP = 300   # px of rail per stretch zone: covers the zone plus the 265 px it can grow by

# Stretch zones (frame units) on plain rail: top/bottom rails, then the left/right sides.
FRAME_ZONES_X = [(100, 112), (188, 200)]
FRAME_ZONES_Y = [(100, 118), (160, 178)]
TRAY_ZONES_X = [(100, 200)]

ICON_DEFS = '''<defs>
  <linearGradient id="goldIcon" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f3e5ad"/><stop offset="1" stop-color="#b8954c"/></linearGradient>
  <linearGradient id="gearFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff6d8"/><stop offset="1" stop-color="#e2cf98"/></linearGradient>
  <linearGradient id="cloud" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f1e3ad"/><stop offset="1" stop-color="#b9974f"/></linearGradient>
  <linearGradient id="stormCloud" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#b9b09a"/><stop offset="1" stop-color="#6f6754"/></linearGradient>
</defs>'''

def svg(body, defs=''):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">{defs}{body}</svg>'

def tray_svg():
    under, _ = sc.tray_parts(layout='B')
    L = dict(x0=40.5, x1=259.5, y0=262.0, y1=293.0)
    capL, capR = L['x0'] + 24, L['x1'] - 42
    ym = (L['y0'] + L['y1']) / 2
    statics = (sc.gear_glyph((L['x0'] + capL) / 2 + 2.4, ym + 0.6, 1.02) +
               f'<rect x="{capR + 19 - 0.35}" y="{L["y0"] + 9}" width="0.7" height="{L["y1"] - L["y0"] - 13}" '
               f'fill="{sc.GOLD_LINE}" opacity="0.8"/>')
    return svg(under + statics, ICON_DEFS)

def medallion_svg():
    m = sc.n_medallion()
    m = m[:m.index('<text')]
    frame_defs = bf.build()
    defs = frame_defs[frame_defs.index('<defs>'):frame_defs.index('</defs>') + 7]
    return svg(m, defs)

def cloud(fill):
    return (f'<path d="M-6.6,3.2 C-9.4,3.2 -9.6,-1.4 -6.4,-1.6 C-6.2,-5.4 -1.6,-6.8 0.6,-3.6 '
            f'C2.4,-6.4 7.4,-5.2 6.6,-1.0 C9.6,-0.6 9.2,3.2 6.4,3.2 Z" fill="{fill}" stroke="#1a140b" stroke-width="0.7"/>')

def icon_body(name):
    if name == 'sun':
        return sc.sun(150, 150, 1.15)
    if name == 'moon':
        return sc.moon(150, 150, 1.1)
    g = '<g transform="translate(150 150)">'
    if name == 'rain':
        drops = ''.join(f'<path d="M{x},5.2 l-1.2,3.4" stroke="#cfe0e8" stroke-width="1.3" stroke-linecap="round"/>'
                        for x in (-3.6, 0.2, 4.0))
        return g + f'<g transform="translate(0 -1.6)">{cloud("url(#cloud)")}</g>{drops}</g>'
    if name == 'storm':
        bolt = '<path d="M0.8,1.6 L-2.4,6.4 L0,6.4 L-1.4,10 L3.2,4.6 L0.8,4.6 L2.4,1.6Z" fill="#f6e08a" stroke="#1a140b" stroke-width="0.6"/>'
        return g + f'<g transform="translate(0 -2.2)">{cloud("url(#stormCloud)")}</g>{bolt}</g>'
    if name == 'snow':
        arms = ''.join(f'<g transform="rotate({a})"><path d="M0,-6.6 L0,6.6" stroke="#1a140b" stroke-width="2.6" stroke-linecap="round"/>'
                       f'<path d="M0,-6.2 L0,6.2" stroke="#dcecf4" stroke-width="1.3" stroke-linecap="round"/></g>' for a in (0, 60, 120))
        return g + arms + '<circle r="1.4" fill="#f3e5ad"/></g>'
    if name == 'fog':
        bars = ''.join(f'<rect x="{x0}" y="{y - 1.1}" width="{x1 - x0}" height="2.2" rx="1.1" fill="#d8dace" stroke="#1a140b" stroke-width="0.6" opacity="{o}"/>'
                       for y, x0, x1, o in ((-3.8, -6, 4.6, 0.85), (0, -4.4, 6.6, 1), (3.8, -6.8, 3.6, 0.8)))
        return g + bars + '</g>'
    raise ValueError(name)

ICONS = ['sun', 'moon', 'rain', 'storm', 'snow', 'fog']

# ------------------------------------------------------------------ rects

def to_rects(png, cuts_x=(), cuts_y=(), q=24):
    """Same-colour runs per row (split at cuts_x), merged down rows (split at cuts_y)."""
    a = np.array(Image.open(png).convert('RGBA')).astype(np.int64)
    H, W = a.shape[:2]
    al = a[..., 3] // 48
    qq = a[..., :3] // q
    key = np.where(al > 0, (al << 15) | (qq[..., 0] << 10) | (qq[..., 1] << 5) | qq[..., 2], -1)
    xb = set(cuts_x)
    yb = set(cuts_y)
    open_runs = {}
    done = []
    for y in range(H):
        row = key[y]
        cur = {}
        x = 0
        while x < W:
            if row[x] < 0:
                x += 1
                continue
            x2 = x + 1
            while x2 < W and row[x2] == row[x] and x2 not in xb:
                x2 += 1
            k = (x, x2, int(row[x]))
            if k in open_runs and open_runs[k][1] == y - 1 and y not in yb:
                cur[k] = (open_runs[k][0], y)
            else:
                cur[k] = (y, y)
            x = x2
        for k, v in open_runs.items():
            if k not in cur or cur[k][0] != v[0]:
                done.append((k, v))
        open_runs = cur
    done += list(open_runs.items())
    out = []
    for (x, x2, _), (y0, y1) in done:
        c = a[y0:y1 + 1, x:x2].reshape(-1, 4).mean(0).round().astype(int)
        if c[3] < 6:
            continue
        out.append((x, y0, x2 - x, y1 - y0 + 1, (int(c[3]) << 24) | (int(c[2]) << 16) | (int(c[1]) << 8) | int(c[0])))
    return out

def px(u):
    return int(round(u * K))

def main():
    tmp = tempfile.mkdtemp(prefix='mmart_')
    def render(name, text, size, height=None):
        p = os.path.join(tmp, name + '.svg')
        with open(p, 'w', encoding='utf-8') as f:
            f.write(text)
        out = os.path.join(tmp, f'{name}_{size}.png')
        shot(p, out, size, height=height)
        return out

    def shifted(rects, dx=0, dy=0):
        return [(x + dx, y + dy, w, h, c) for x, y, w, h, c in rects]

    frame = bf.build()
    sprites = []
    for b in BUCKETS:
        sprites.append((f'FrameB{b}', to_rects(render('frame', frame, b)), [], []))
    zx = [v for z in FRAME_ZONES_X for v in (px(z[0]), px(z[1]))]
    zy = [v for z in FRAME_ZONES_Y for v in (px(z[0]), px(z[1]))]
    inside = lambda a, w, zones: any(zones[i] <= a and a + w <= zones[i + 1] for i in range(0, len(zones), 2))
    full = to_rects(render('frame', frame, DESIGN), zx, zy)
    sprites.append(('Frame370', [r for r in full if not inside(r[0], r[2], zx) and not inside(r[1], r[3], zy)], zx, zy))
    for i in range(0, len(zx), 2):
        vb = f'{zx[i] / K:.5f} 0 {STRIP / K:.5f} 300'
        sprites.append((f'StripX{i // 2}', shifted(to_rects(render(f'stripx{i}', bf.rails_svg('x', vb), STRIP, DESIGN)), dx=zx[i]), [], []))
    for i in range(0, len(zy), 2):
        vb = f'0 {zy[i] / K:.5f} 300 {STRIP / K:.5f}'
        sprites.append((f'StripY{i // 2}', shifted(to_rects(render(f'stripy{i}', bf.rails_svg('y', vb), DESIGN, STRIP)), dy=zy[i]), [], []))
    tx = [v for z in TRAY_ZONES_X for v in (px(z[0]), px(z[1]))]
    sprites.append(('Tray', to_rects(render('tray', tray_svg(), DESIGN), tx), tx, []))
    sprites.append(('Medallion', to_rects(render('medallion', medallion_svg(), DESIGN)), [], []))
    for name in ICONS:
        sprites.append((f'Icon{name.capitalize()}', to_rects(render(name, svg(icon_body(name), ICON_DEFS), DESIGN)), [], []))

    lines = ['// Generated by art/gen_minimap_art.py -- do not edit by hand.',
             '#pragma once', '#include <cstdint>', '',
             'struct MinimapArtRect { uint16_t x, y, w, h; uint32_t color; };',
             'struct MinimapArtSprite { const MinimapArtRect* rects; uint32_t count; uint16_t zonesX[4]; uint16_t zonesY[4]; uint8_t nZonesX, nZonesY; };',
             '',
             f'constexpr float kMinimapArtDesign = {DESIGN}.0f;            // px of the 370 renders',
             f'constexpr float kMinimapArtUnit = {K:.6f}f;                // px per frame unit at the design size',
             f'constexpr int kMinimapFrameBucketCount = {len(BUCKETS)};',
             'constexpr float kMinimapFrameBuckets[] = {' + ', '.join(f'{b}.0f' for b in BUCKETS) + '};', '']
    total = 0
    for name, rects, zx_, zy_ in sprites:
        total += len(rects)
        lines.append(f'constexpr MinimapArtRect kArt{name}Rects[] = {{')
        lines += [f'    {{{x}, {y}, {w}, {h}, 0x{c:08X}u}},' for x, y, w, h, c in rects]
        lines.append('};')
    lines.append('')
    for name, rects, zx_, zy_ in sprites:
        zxs = ', '.join(str(v) for v in (zx_ + [0, 0, 0, 0])[:4])
        zys = ', '.join(str(v) for v in (zy_ + [0, 0, 0, 0])[:4])
        lines.append(f'constexpr MinimapArtSprite kArt{name} = {{kArt{name}Rects, {len(rects)}u, {{{zxs}}}, {{{zys}}}, '
                     f'{len(zx_) // 2}, {len(zy_) // 2}}};')
    lines.append('constexpr const MinimapArtSprite* kArtFrameBuckets[] = {' +
                 ', '.join(f'&kArtFrameB{b}' for b in BUCKETS) + '};')
    lines.append('// Aged rail to fill each stretch zone of kArtFrame370 (same pixel space; start at the zone).')
    lines.append('constexpr const MinimapArtSprite* kArtFrameStripsX[] = {&kArtStripX0, &kArtStripX1};')
    lines.append('constexpr const MinimapArtSprite* kArtFrameStripsY[] = {&kArtStripY0, &kArtStripY1};')
    lines.append('// Weather icon by type: 0 sun, 1 moon, 2 rain, 3 storm, 4 snow, 5 fog; centred on frame unit (150, 150).')
    lines.append('constexpr const MinimapArtSprite* kArtIcons[] = {' +
                 ', '.join(f'&kArtIcon{n.capitalize()}' for n in ICONS) + '};')
    out = os.path.join(HERE, '..', 'src', 'minimap_art.h')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('rects:', {n: len(r) for n, r, *_ in sprites}, 'total', total)

if __name__ == '__main__':
    main()

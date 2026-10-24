"""Minimap frame rebuilt as vector shapes (300 x 300 units, same grid as kMinimapFrameRects).

Geometry is measured from the pixel art. The top-left quadrant is drawn once and mirrored; the
left side, top (compass) and bottom (diamond) pieces are mirrored left/right only. Every layer is
lit by the same filter after mirroring, so light always comes from the top-left.
"""
import math

MX = 'matrix(-1 0 0 1 300 0)'      # mirror left/right about x = 150
MY = 'matrix(1 0 0 -1 0 295)'      # mirror top/bottom about y = 147.5 (rails sit symmetric about it)
MXY = 'matrix(-1 0 0 -1 300 295)'

def f(v):
    return f'{v:.2f}'.rstrip('0').rstrip('.')

def rect(x0, y0, x1, y1):
    return f'<rect x="{f(x0)}" y="{f(y0)}" width="{f(x1 - x0)}" height="{f(y1 - y0)}"/>'

def poly(*pts):
    return '<polygon points="' + ' '.join(f'{f(x)},{f(y)}' for x, y in pts) + '"/>'

def circle(cx, cy, r):
    return f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(r)}"/>'

def _circ_path(cx, cy, r):
    return (f'M{f(cx + r)},{f(cy)}A{f(r)},{f(r)} 0 1 1 {f(cx - r)},{f(cy)}'
            f'A{f(r)},{f(r)} 0 1 1 {f(cx + r)},{f(cy)}Z')

def ring(cx, cy, ro, ri, icx=None, icy=None):
    """Annulus; the inner circle may sit off-centre for a crescent-weighted band."""
    icx = cx if icx is None else icx
    icy = cy if icy is None else icy
    return f'<path fill-rule="evenodd" d="{_circ_path(cx, cy, ro)}{_circ_path(icx, icy, ri)}"/>'

def arc(cx, cy, ro, ri, a0, a1):
    """Band between radii ri..ro from angle a0 to a1 (degrees, 0 = +x, clockwise on screen)."""
    p = lambda r, a: (cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
    large = 1 if (a1 - a0) % 360 > 180 else 0
    (x0, y0), (x1, y1) = p(ro, a0), p(ro, a1)
    (x2, y2), (x3, y3) = p(ri, a1), p(ri, a0)
    return (f'<path d="M{f(x0)},{f(y0)}A{f(ro)},{f(ro)} 0 {large} 1 {f(x1)},{f(y1)}'
            f'L{f(x2)},{f(y2)}A{f(ri)},{f(ri)} 0 {large} 0 {f(x3)},{f(y3)}Z"/>')

def path(d):
    return f'<path d="{d}"/>'

def xmark(cx, cy, w, h, t=1.25):
    hw, hh, k = w / 2, h / 2, t / 2
    return (poly((cx - hw, cy - hh), (cx - hw + t, cy - hh), (cx + hw, cy + hh), (cx + hw - t, cy + hh)) +
            poly((cx + hw - t, cy - hh), (cx + hw, cy - hh), (cx - hw + t, cy + hh), (cx - hw, cy + hh)))

def kite(cx, cy, ang, length, half_w, base_r):
    a = math.radians(ang)
    ux, uy = math.cos(a), math.sin(a)
    px, py = -uy, ux
    bx, by = cx + ux * base_r, cy + uy * base_r
    return poly((cx + ux * length, cy + uy * length), (bx + px * half_w, by + py * half_w),
                (cx - ux * 0.5, cy - uy * 0.5), (bx - px * half_w, by - py * half_w))

# ------------------------------------------------------------------ geometry
C = (33.5, 41.5)  # top-left rosette centre

LAYERS = ('rails', 'deco', 'recess', 'rings', 'shafts', 'hubs')
quad = {k: [] for k in LAYERS}
left = {k: [] for k in quad}
top = {k: [] for k in quad}
bottom = {k: [] for k in quad}

# rails (half of each run; the mirrors complete them)
quad['rails'] += [
    rect(44, 29, 150.5, 32),        # top outer
    rect(44, 39, 150.5, 42),        # top inner
    rect(25, 52, 28, 148),          # left outer
    rect(34, 70, 36, 148),          # left inner
]
# blade under the top outer rail, and the inner left rail widening into the corner
quad['deco'] += [
    poly((44, 31.5), (66.5, 31.5), (59, 38.6), (44, 38.6)),
    poly((30.6, 57), (36, 57), (36, 80), (34, 80), (34, 78.5), (30.6, 74.5)),
    xmark(94.5, 35.5, 7, 5.6, 1.5),
]
# rosette
quad['recess'] += [ring(*C, 10.8, 6.2)]
quad['rings'] += [
    ring(*C, 17.2, 12.85, C[0] + 1.45, C[1]),               # outer ring, heavier on the outside
    arc(*C, 12.0, 9.6, -100, 100),                          # inner arc on the rail side
    ring(25.5, 64.5, 4.3, 1.9),                             # small ring on the outer rail
]
quad['shafts'] += [
    rect(30.6, 21, 33.4, 60),                               # spike shaft, laid over the rings
    poly((32, 15.2), (34.9, 19.3), (33.4, 21.6), (30.6, 21.6), (29.1, 19.3)),   # arrow tip
]
quad['hubs'] += [ring(*C, 5.3, 3.7), circle(*C, 2.7)]

# left side ornaments
left['deco'] += [
    xmark(31, 94, 5.2, 7, 1.5), xmark(31, 183.5, 5.2, 7, 1.5),
    rect(27.5, 122.8, 31.5, 125.4),                          # tie from outer rail to the ring
    path('M27.6,131.5C32.6,134.5 34,137.6 34,140.8C34,144 32.6,147.3 27.6,150.5Z'),   # leaf
    poly((36, 151.8), (36, 157.2), (30.4, 154.5)),           # arrow on the inner rail
]
left['rings'] += [ring(34.5, 124, 3.7, 1.6), ring(25.5, 154.5, 3.8, 1.6)]

# compass rose (top centre)
CC = (150, 30.5)
top['rings'] += [
    ring(*CC, 17, 13.7),
    poly((150, 0.8), (152.3, 9), (152.3, 14.5), (147.7, 14.5), (147.7, 9)),           # north spike
    poly((148.4, 46), (151.6, 46), (151.6, 48), (154.4, 52.6), (150, 60.8), (145.6, 52.6), (148.4, 48)),
    rect(148.5, 14, 151.5, 47),
] + [kite(*CC, a, 13.6, 3.0, 3.8) for a in (90, 270)] \
  + [kite(*CC, a, 23, 3.0, 4.4) for a in (0, 180)] \
  + [kite(*CC, a, 13.0, 2.0, 3.4) for a in (45, 135, 225, 315)]
top['rings'] += [ring(119, 30.5, 5, 2.2)]
top['hubs'] += [circle(*CC, 3.3), poly((150, 50.2), (152.2, 52.8), (150, 56.6), (147.8, 52.8))]

# bottom diamond
DC = (150, 259.5)
bottom['deco'] += [poly((117, 253), (142.5, 255.4), (142.5, 258.6), (117, 256))]   # inner rail sweeping into the diamond
bottom['rings'] += [
    poly((DC[0], DC[1] - 12.8), (DC[0] + 9.6, DC[1]), (DC[0], DC[1] + 12.8), (DC[0] - 9.6, DC[1])),
    ring(127.5, 263.5, 4.5, 1.9),   # (no pendant under the diamond: the control tray hangs there)
]
bottom['hubs'] += [poly((DC[0], DC[1] - 7.2), (DC[0] + 5.2, DC[1]), (DC[0], DC[1] + 7.2), (DC[0] - 5.2, DC[1]))]

# ------------------------------------------------------------------ svg
GOLD = {  # diffuse light 0..1 -> colour, per channel
    'R': [72, 126, 168, 198, 216, 232],
    'G': [57, 104, 145, 178, 198, 216],
    'B': [28, 54, 80, 108, 128, 150],
}
# Vanilla-matched gold: shadows lean orange-amber, highlights go pale cream, and the flat face sits
# where the game's own gold does.
GOLD_SOFT = {
    'R': [70, 124, 172, 204, 228, 250],
    'G': [40, 82, 128, 168, 202, 236],
    'B': [12, 28, 54, 92, 132, 186],
}
SOFT = True   # softer bevel, warm glow and the wider ramp; False gives the earlier, harder metal
RECESS = {k: [round(v * 0.5) for v in vals] for k, vals in GOLD.items()}

def metal_filter(fid, ramp, dil=0.5, blur=0.55, scale=1.8, aged=True, soft=None):
    """Lit gold with a dark rim. Aged: the edges wobble a little (hand-made, like the game's own
    gold), small chips are knocked out of the edges, and patchy grime dulls the metal in places.
    The noise is in frame units, so every render size and every rail strip shares one pattern."""
    soft = SOFT if soft is None else soft
    if soft:
        # broad, low bump and a gentle highlight: painted gold rather than cast metal
        ramp = GOLD_SOFT if ramp is GOLD else {k: [round(v * 0.5) for v in vals] for k, vals in GOLD_SOFT.items()}
        blur, scale = blur + 0.3, scale * 0.6
    tv = lambda ch: ' '.join(f'{v / 255:.3f}' for v in ramp[ch])
    if aged:
        shape = f'''
  <feTurbulence type="fractalNoise" baseFrequency="0.55" numOctaves="2" seed="11" result="wobbleNoise"/>
  <feDisplacementMap in="SourceAlpha" in2="wobbleNoise" scale="0.55" xChannelSelector="R" yChannelSelector="G" result="wobbled"/>
  <feTurbulence type="fractalNoise" baseFrequency="1.1" numOctaves="1" seed="5" result="chipNoise"/>
  <feComponentTransfer in="chipNoise" result="chips">
    <feFuncA type="discrete" tableValues="0 0 0 0 0 0 0 0 0 0 0 1"/>
  </feComponentTransfer>
  <feMorphology in="wobbled" operator="erode" radius="0.7" result="core"/>
  <feComposite in="wobbled" in2="core" operator="out" result="edgeBand"/>
  <feComposite in="chips" in2="edgeBand" operator="in" result="nicks"/>
  <feComposite in="wobbled" in2="nicks" operator="out" result="body"/>'''
        grime = '''
  <feTurbulence type="fractalNoise" baseFrequency="0.16" numOctaves="3" seed="23" result="grimeNoise"/>
  <feColorMatrix in="grimeNoise" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  -1.5 0 0 0 0.62" result="grimeA"/>
  <feFlood flood-color="#3a2a12" result="grimeCol"/>
  <feComposite in="grimeCol" in2="grimeA" operator="in" result="grimeTint"/>
  <feComposite in="grimeTint" in2="body" operator="in" result="grimeIn"/>'''
        body, final = 'body', '<feMergeNode in="metal"/><feMergeNode in="grimeIn"/>'
    else:
        shape, grime, body, final = '', '', 'SourceAlpha', '<feMergeNode in="metal"/>'
    return f'''<filter id="{fid}" x="-4%" y="-4%" width="108%" height="108%" color-interpolation-filters="sRGB">{shape}
  <feMorphology in="{body}" operator="dilate" radius="{dil}" result="dil"/>
  <feFlood flood-color="{'#2a1a0b' if soft else '#1c130a'}" flood-opacity="{0.72 if soft else 1}" result="dk"/>
  <feComposite in="dk" in2="dil" operator="in" result="outline"/>
  <feGaussianBlur in="{body}" stdDeviation="1.4" result="halo"/>
  <feFlood flood-color="#e2a646" flood-opacity="{0.42 if soft else 0}"/>
  <feComposite in2="halo" operator="in" result="glow"/>
  <feGaussianBlur in="{body}" stdDeviation="{blur}" result="bump"/>
  <feDiffuseLighting in="bump" surfaceScale="{scale}" diffuseConstant="1" lighting-color="#ffffff" result="diff">
    <feDistantLight azimuth="225" elevation="58"/>
  </feDiffuseLighting>
  <feComponentTransfer in="diff" result="tint">
    <feFuncR type="table" tableValues="{tv('R')}"/>
    <feFuncG type="table" tableValues="{tv('G')}"/>
    <feFuncB type="table" tableValues="{tv('B')}"/>
  </feComponentTransfer>
  <feSpecularLighting in="bump" surfaceScale="{scale}" specularConstant="{0.2 if soft else 0.3}" specularExponent="{11 if soft else 20}" lighting-color="#f6e7bd" result="spec">
    <feDistantLight azimuth="225" elevation="50"/>
  </feSpecularLighting>
  <feComposite in="tint" in2="spec" operator="arithmetic" k2="1" k3="1" result="lit"/>
  <feComposite in="lit" in2="{body}" operator="in" result="metal"/>{grime}
  <feMerge><feMergeNode in="glow"/><feMergeNode in="outline"/>{final}</feMerge>
</filter>'''

def build():
    defs = [metal_filter('metal', GOLD), metal_filter('metalRecess', RECESS, scale=1.4)]
    body = []
    for layer in LAYERS:
        for name, src in (('quad', quad), ('left', left), ('top', top), ('bottom', bottom)):
            if src[layer]:
                defs.append(f'<g id="{name}-{layer}">' + ''.join(src[layer]) + '</g>')
        uses = []
        if quad[layer]:
            uses += [f'<use href="#quad-{layer}"/>', f'<use href="#quad-{layer}" transform="{MX}"/>',
                     f'<use href="#quad-{layer}" transform="{MY}"/>', f'<use href="#quad-{layer}" transform="{MXY}"/>']
        for name, src in (('left', left), ('top', top), ('bottom', bottom)):
            if src[layer]:
                uses += [f'<use href="#{name}-{layer}"/>', f'<use href="#{name}-{layer}" transform="{MX}"/>']
        fid = 'metalRecess' if layer == 'recess' else 'metal'
        body.append(f'<g id="{layer}" filter="url(#{fid})" fill="#fff">' + ''.join(uses) + '</g>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 300 300">\n'
            '<defs>\n' + '\n'.join(defs) + '\n</defs>\n' + '\n'.join(body) + '\n</svg>\n')

def rails_svg(axis, view_box):
    """Just the straight rails, run far past the frame, lit and aged exactly like the frame (the
    noise is in frame units, so a strip continues the frame's own grime). axis 'x': top and bottom
    rails; 'y': the side rails."""
    if axis == 'x':
        rails = [rect(-100, 29, 700, 32), rect(-100, 39, 700, 42), rect(-100, 253, 700, 256), rect(-100, 263, 700, 266)]
    else:
        rails = [rect(25, -100, 28, 700), rect(34, -100, 36, 700), rect(264, -100, 266, 700), rect(272, -100, 275, 700)]
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view_box}"><defs>{metal_filter("metal", GOLD)}</defs>'
            f'<g filter="url(#metal)" fill="#fff">' + ''.join(rails) + '</g></svg>')

if __name__ == '__main__':
    import sys
    open(sys.argv[1] if len(sys.argv) > 1 else 'frame.svg', 'w').write(build())

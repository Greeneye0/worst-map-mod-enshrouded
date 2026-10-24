"""Minimap mockup as one vector scene over a clean in-game capture (map + markers only).

Frame units are the 300-unit grid; the frame sits at (29, 35) and 370 px wide in the capture.
"""
import base64, os, subprocess, sys
sys.path.insert(0, '.')
import build_frame as bf

EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
FX, FY, FS = 29, 35, 370

GOLD_LINE = '#c4a862'
CREAM = '#fcefc6'
SLATE_TOP, SLATE_MID, SLATE_BOT = '#6c6e7e', '#56586a', '#43444f'
INDIGO = '#3b3f56'

def stone_filter():
    return '''<filter id="stone" x="-3%" y="-10%" width="106%" height="120%" color-interpolation-filters="sRGB">
  <feMorphology in="SourceAlpha" operator="dilate" radius="0.55" result="dil"/>
  <feFlood flood-color="#141218"/><feComposite in2="dil" operator="in" result="outline"/>
  <feGaussianBlur in="SourceAlpha" stdDeviation="1.1" result="bump"/>
  <feDiffuseLighting in="bump" surfaceScale="2.2" diffuseConstant="1" lighting-color="#ffffff" result="diff">
    <feDistantLight azimuth="250" elevation="60"/>
  </feDiffuseLighting>
  <feComposite in="SourceGraphic" in2="diff" operator="arithmetic" k1="1.25" result="shaded"/>
  <feTurbulence type="fractalNoise" baseFrequency="0.35 0.9" numOctaves="3" seed="31" result="grain"/>
  <feColorMatrix in="grain" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  -0.9 0 0 0 0.5" result="grainA"/>
  <feFlood flood-color="#1b1a22"/><feComposite in2="grainA" operator="in" result="grainTint"/>
  <feComposite in="grainTint" in2="SourceAlpha" operator="in" result="grainIn"/>
  <feComposite in="shaded" in2="SourceAlpha" operator="in" result="body"/>
  <feMerge><feMergeNode in="outline"/><feMergeNode in="body"/><feMergeNode in="grainIn"/></feMerge>
</filter>
<filter id="wornLine" x="-2%" y="-20%" width="104%" height="140%" color-interpolation-filters="sRGB">
  <feTurbulence type="fractalNoise" baseFrequency="0.7" numOctaves="2" seed="17" result="n"/>
  <feColorMatrix in="n" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  3.2 0 0 0 -0.75" result="keep"/>
  <feComposite in="SourceGraphic" in2="keep" operator="in"/>
</filter>'''

def tray_parts(time_text='10:42', ampm='PM', weather='Clear', icon='moon', layout='A'):
    x0, x1, y0, y1 = 40.5, 259.5, 262.0, 293.0
    ym = (y0 + y1) / 2
    tip = 9.5
    hexpts = [(x0 + tip, y0), (x1 - tip, y0), (x1, ym), (x1 - tip, y1), (x0 + tip, y1), (x0, ym)]
    hp = ' '.join(f'{x},{y}' for x, y in hexpts)
    ins = 2.6
    inner = [(x0 + tip + 0.6, y0 + ins + 3.2), (x1 - tip - 0.6, y0 + ins + 3.2), (x1 - ins - 1.2, ym),
             (x1 - tip - 0.6, y1 - ins), (x0 + tip + 0.6, y1 - ins), (x0 + ins + 1.2, ym)]
    ip = ' '.join(f'{x:.2f},{y:.2f}' for x, y in inner)
    capL, capR = x0 + 24, (x1 - 24 if layout == 'A' else x1 - 42)
    # flourishes: two arcs per end, bowing toward the centre, like the native buttons
    def flour(x, sgn):
        return (f'<path d="M{x},{y0 + 4.5} Q{x + sgn * 4.2},{ym} {x},{y1 - 3}" />'
                f'<path d="M{x - sgn * 2.4},{y0 + 6.5} Q{x + sgn * 1.4},{ym} {x - sgn * 2.4},{y1 - 5}" />')
    under = f'''
<defs>
  <linearGradient id="slate" x1="0" y1="{y0}" x2="0" y2="{y1}" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="{SLATE_TOP}"/><stop offset="0.45" stop-color="{SLATE_MID}"/><stop offset="1" stop-color="{SLATE_BOT}"/>
  </linearGradient>
  {stone_filter()}
</defs>
<polygon points="{hp}" fill="url(#slate)" filter="url(#stone)"/>
<g fill="none" stroke="{GOLD_LINE}" stroke-width="0.8" stroke-linejoin="round" opacity="0.95" filter="url(#wornLine)">
  <polygon points="{ip}"/>
  {flour(capL, 1)}{flour(capR, -1)}
</g>'''
    # content (drawn over the frame so the pendant can't hide it)
    cy = ym + 2.0
    left_c = (capL + 146.5) / 2 + 1
    right_c = (153.5 + capR) / 2 - 1
    ic = {'moon': moon, 'sun': sun}[icon]
    if layout == 'A':
        controls = (f'<text x="{(x0 + capL) / 2 + 2.2}" y="{cy - 0.6}" font-size="15" font-weight="700">&#8722;</text>'
                    f'<text x="{(capR + x1) / 2 - 2.2}" y="{cy - 0.6}" font-size="15" font-weight="700">+</text>')
    else:
        mid = capR + 19
        controls = (gear_glyph((x0 + capL) / 2 + 2.4, ym + 0.6, 1.02) +
                    f'<text x="{mid - 8.5}" y="{cy - 0.6}" font-size="15" font-weight="700">&#8722;</text>'
                    f'<rect x="{mid - 0.35}" y="{y0 + 9}" width="0.7" height="{y1 - y0 - 13}" fill="{GOLD_LINE}" opacity="0.8"/>'
                    f'<text x="{mid + 8.5}" y="{cy - 0.6}" font-size="15" font-weight="700">+</text>')
    over = f'''
<g font-family="Alegreya Sans, sans-serif" fill="{CREAM}" text-anchor="middle" dominant-baseline="central">
  {controls}
  {ic(left_c - 31, cy - 0.4, 1.0)}
  <text x="{left_c + 5}" y="{cy}" font-size="14.2" font-weight="500" letter-spacing="0.2">{time_text}<tspan font-size="9.4" dx="1.6" dy="0.6">{ampm}</tspan></text>
  <text x="{right_c}" y="{cy}" font-size="14.2" font-weight="500" letter-spacing="0.3">{weather}</text>
</g>'''
    return under, over

def moon(cx, cy, s):
    return (f'<g transform="translate({cx} {cy}) scale({s})">'
            f'<path d="M2.6,-5.6 A6,6 0 1 0 5.9,2.2 A4.6,4.6 0 1 1 2.6,-5.6Z" fill="url(#goldIcon)" stroke="#1a140b" stroke-width="0.7"/>'
            f'<path d="M5.3,-6.2 l0.9,1.4 l-0.9,1.4 l-0.9,-1.4Z" fill="#e9dca6"/></g>')

def sun(cx, cy, s):
    rays = ''.join(f'<polygon points="0,-7.4 1.2,-4.6 -1.2,-4.6" transform="rotate({a})"/>' for a in range(0, 360, 45))
    return (f'<g transform="translate({cx} {cy}) scale({s})"><g fill="#c9a95c" stroke="#1a140b" stroke-width="0.4">{rays}</g>'
            f'<circle r="4" fill="url(#goldIcon)" stroke="#1a140b" stroke-width="0.7"/></g>')

def n_medallion():
    cx, cy = 150, 30.5
    return f'''
<g filter="url(#metal)" fill="#fff"><circle cx="{cx}" cy="{cy}" r="8.6"/></g>
<circle cx="{cx}" cy="{cy}" r="6.7" fill="{INDIGO}" stroke="#120f0a" stroke-width="0.6"/>
<circle cx="{cx}" cy="{cy}" r="5.9" fill="none" stroke="{GOLD_LINE}" stroke-width="0.35" opacity="0.6"/>
<text x="{cx}" y="{cy + 0.5}" font-family="Alegreya Sans, sans-serif" font-weight="800" font-size="10.5"
      fill="url(#goldText)" text-anchor="middle" dominant-baseline="central">N</text>'''

def gear_glyph(cx, cy, s):
    teeth = ''.join(f'<path d="M-1.55,-7.1 L1.55,-7.1 L1.9,-4.2 L-1.9,-4.2Z" transform="rotate({a})"/>' for a in range(0, 360, 45))
    return (f'<g transform="translate({cx} {cy}) scale({s})"><g fill="url(#gearFill)" stroke="#1d1912" stroke-width="0.4">'
            f'{teeth}<circle r="4.9"/></g><circle r="2.0" fill="#4a4c5c" stroke="#1d1912" stroke-width="0.4"/></g>')

def gear_slate():
    """Gear on a round slate button (the tray's material) over the bottom-left ring."""
    cx, cy = bf.C[0], 295 - bf.C[1]
    return f'''
<defs><radialGradient id="slateDisc" cx="0.42" cy="0.32" r="0.75">
  <stop offset="0" stop-color="#7a7c8e"/><stop offset="0.6" stop-color="{SLATE_MID}"/><stop offset="1" stop-color="{SLATE_BOT}"/></radialGradient></defs>
<circle cx="{cx}" cy="{cy + 0.8}" r="12.4" fill="#000" opacity="0.45"/>
<circle cx="{cx}" cy="{cy}" r="11.2" fill="url(#slateDisc)" filter="url(#stone)"/>
<circle cx="{cx}" cy="{cy}" r="9.4" fill="none" stroke="{GOLD_LINE}" stroke-width="0.75" opacity="0.95"/>
{gear_glyph(cx, cy, 1.1)}'''

def gear_medallion():
    cx, cy = bf.C[0], 295 - bf.C[1]   # bottom-left rosette centre (bottom-right is the resize grip)
    teeth = ''.join(f'<path d="M-1.55,-7.1 L1.55,-7.1 L1.9,-4.2 L-1.9,-4.2Z" transform="rotate({a})"/>' for a in range(0, 360, 45))
    return f'''
<circle cx="{cx}" cy="{cy}" r="11.6" fill="#0d0b07" opacity="0.55"/>
<g filter="url(#metal)" fill="#fff"><circle cx="{cx}" cy="{cy}" r="10.4"/></g>
<circle cx="{cx}" cy="{cy}" r="8.3" fill="{INDIGO}" stroke="#120f0a" stroke-width="0.7"/>
<circle cx="{cx}" cy="{cy}" r="7.6" fill="none" stroke="{GOLD_LINE}" stroke-width="0.4" opacity="0.7"/>
<g transform="translate({cx} {cy})" fill="url(#gearFill)" stroke="#2a2418" stroke-width="0.35">{teeth}<circle r="4.9"/></g>
<circle cx="{cx}" cy="{cy}" r="2.0" fill="{INDIGO}" stroke="#2a2418" stroke-width="0.35"/>'''

def frame_inner():
    s = bf.build()
    return s[s.index('>') + 1: s.rindex('</svg>')]

def scene_html(capture, layout='A', **kw):
    with open(capture, 'rb') as fh:
        b64 = base64.b64encode(fh.read()).decode()
    under, over = tray_parts(layout=layout, **kw)
    k = FS / 300
    grads = '''<defs>
  <linearGradient id="goldText" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f4e6b0"/><stop offset="1" stop-color="#c9a95c"/></linearGradient>
  <linearGradient id="gearFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff6d8"/><stop offset="1" stop-color="#e2cf98"/></linearGradient>
  <linearGradient id="goldIcon" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f3e5ad"/><stop offset="1" stop-color="#b8954c"/></linearGradient>
</defs>'''
    return f'''<!doctype html><html><head>
<link href="https://fonts.googleapis.com/css2?family=Alegreya+Sans:wght@500;700;800&display=block" rel="stylesheet">
<style>html,body{{margin:0;background:#000}} svg{{display:block}}</style></head><body>
<svg xmlns="http://www.w3.org/2000/svg" width="440" height="510" viewBox="0 0 440 510">
<image href="data:image/png;base64,{b64}" x="0" y="0" width="440" height="510"/>
<g transform="translate({FX} {FY}) scale({k:.6f})">
{grads}
{under}
{frame_inner()}
{n_medallion()}
{gear_slate() if layout == 'A' else ''}
{over}
</g></svg></body></html>'''

def render(html_path, png, scale=2):
    subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars', f'--force-device-scale-factor={scale}',
                    '--virtual-time-budget=8000', '--window-size=440,510', f'--screenshot={os.path.abspath(png)}',
                    'file:///' + os.path.abspath(html_path).replace(os.sep, '/')], check=True, capture_output=True, timeout=90)

if __name__ == '__main__':
    for lay in ('A', 'B'):
        open(f'scene_{lay}.html', 'w', encoding='utf-8').write(scene_html('mm_clean.png', layout=lay))
        render(f'scene_{lay}.html', f'scene_{lay}_2x.png', 2)

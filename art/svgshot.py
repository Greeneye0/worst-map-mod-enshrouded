"""Rasterize an SVG with headless Edge (filters included). usage: svgshot.py in.svg out.png size [bg]"""
import os, subprocess, sys, tempfile
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'

def shot(svg, out, size, bg='transparent', height=None):
    width, height = size, (height or size)
    svg = os.path.abspath(svg)
    html = tempfile.NamedTemporaryFile('w', suffix='.html', delete=False, dir=os.path.dirname(svg))
    html.write(f'<!doctype html><html><body style="margin:0;background:{bg}">'
               f'<img src="file:///{svg.replace(os.sep, "/")}" style="display:block;width:{width}px;height:{height}px"></body></html>')
    html.close()
    try:
        subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                        '--default-background-color=00000000', f'--window-size={width},{height}',
                        f'--screenshot={os.path.abspath(out)}', 'file:///' + html.name.replace(os.sep, '/')],
                       check=True, capture_output=True, timeout=60)
    finally:
        os.unlink(html.name)

if __name__ == '__main__':
    shot(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else 'transparent')

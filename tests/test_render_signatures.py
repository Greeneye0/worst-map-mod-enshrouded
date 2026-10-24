"""Regression: each native terrain/opacity lookup must resolve exactly once.
Run with python tests/test_render_signatures.py [game.exe ...].
The former PopUiClipRect signature matched an inline copy as well as the function.
"""
from pathlib import Path
import re
import sys

source = (Path(__file__).resolve().parents[1] / "src/map_key_panel.cpp").read_text(encoding="utf-8")
names = "DrawWorldMapCanvas|GetUiDrawList|PushUiClipRect|PopUiClipRect|PushUiOpacity|PopUiOpacity"
sites = re.findall(r'uint8_t\* (fn(?:' + names + r')) = FindUnique\("([^"]+)"\)', source)
assert len(sites) == 6, "Missing render signature coverage"
hint_source = (Path(__file__).resolve().parents[1] / "src/map_hints.inl").read_text(encoding="utf-8")
hint_sites = re.findall(r'auto\* (bar|one) = FindUnique\("([^"]+)"\)', hint_source)
assert len(hint_sites) == 2, "Missing native hint signature coverage"
sites += hint_sites
paths = sys.argv[1:] or [
    r"C:\temp\enshrouded-re\bin\enshrouded.exe",
    r"C:\Program Files (x86)\Steam\steamapps\common\Enshrouded\enshrouded.exe",
]
for path in paths:
    data = Path(path).read_bytes()
    for name, signature in sites:
        pattern = b"".join(b"." if byte == "??" else re.escape(bytes.fromhex(byte)) for byte in signature.split())
        matches = list(re.finditer(pattern, data, re.DOTALL))
        assert len(matches) == 1, f"{path}: {name}: expected 1 match, found {len(matches)}"
    print(f"PASS: all {len(sites)} render/hint signatures unique in {path}")

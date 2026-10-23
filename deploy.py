"""Installs Unlock Map Markers into the Greeneye Loader (game closed).

  python deploy.py

Copies bin\\loader\\ into <game>\\greeneye\\unlock-map-markers\\, carries minimap_config.ini over
from the old Shroudtopia folder (mods\\unlock_map_markers\\) once, and turns the Shroudtopia copy
off in shroudtopia.json: two copies would both patch the map. While the game runs, use hot reload
instead: EnshroudedMods\\tools\\hot-reload.ps1 unlock-map-markers.
"""
import json
import os
import shutil
import subprocess
import sys

GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Enshrouded"
MOD_ID = "unlock-map-markers"
MOD_DIR = os.path.join(GAME_DIR, "greeneye", MOD_ID)
OLD_MOD_DIR = os.path.join(GAME_DIR, "mods", "unlock_map_markers")
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(HERE, "bin", "loader")


def game_running():
    """True if an enshrouded.exe from the game folder runs (other copies share the name)."""
    ps = (
        "Get-Process enshrouded -ErrorAction SilentlyContinue | "
        f"Where-Object {{ $_.Path -like '{GAME_DIR}\\*' }} | Measure-Object | "
        "ForEach-Object { $_.Count }"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    return out.stdout.strip() not in ("", "0")


def disable_shroudtopia_copy():
    path = os.path.join(GAME_DIR, "shroudtopia.json")
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8-sig") as f:
        cfg = json.load(f)
    mods = cfg.get("mods")
    entry = mods.get("unlock_map_markers") if isinstance(mods, dict) else None
    if isinstance(entry, dict) and entry.get("active", False):
        entry["active"] = False
        with open(path, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=4)
        print(f"Turned off unlock_map_markers in {path}")


def main():
    if game_running():
        print("Enshrouded is running: close it first, or hot reload an installed copy with "
              "EnshroudedMods\\tools\\hot-reload.ps1 unlock-map-markers")
        return 1
    if not os.path.isdir(os.path.join(GAME_DIR, "greeneye")):
        print(f"No Greeneye Loader in {GAME_DIR} (greeneye\\ is missing)")
        return 1

    os.makedirs(MOD_DIR, exist_ok=True)
    for name in (f"{MOD_ID}.dll", "mod.json"):
        shutil.copy2(os.path.join(BUILD_DIR, name), os.path.join(MOD_DIR, name))
        print(f"Copied {name} -> {MOD_DIR}")

    old_cfg = os.path.join(OLD_MOD_DIR, "minimap_config.ini")
    new_cfg = os.path.join(MOD_DIR, "minimap_config.ini")
    if os.path.exists(old_cfg) and not os.path.exists(new_cfg):
        shutil.copy2(old_cfg, new_cfg)
        print("Carried minimap_config.ini over from the Shroudtopia folder")

    disable_shroudtopia_copy()
    print("Installed. Start the game; greeneye\\loader.log and "
          f"greeneye\\{MOD_ID}\\unlock_map_markers.log show it starting.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# In Game Map Markers and Mini Map — The Worst Map Mod (Enshrouded)

![The Worst Map Mod](dist/nexus/images/00-header.jpg)

![The world map with every marker and the Map Key & Filters panel](dist/nexus/images/01-full-map.jpg)

![Mining: every ore deposit](dist/nexus/images/12-mining.jpg)

![Trees: forests by type](dist/nexus/images/15-trees.jpg)

![Fog modes](dist/nexus/images/05-fog-modes.jpg)

![Minimap](dist/nexus/images/06-minimap-setup.jpg)

- **Every marker on the world map**: spires, altars, sanctums, vaults, lairs, towns, camps, mines, caves, garrisons, quests, lore rumors, graves, and all 448 gold and 1032 silver chests.
- **Mining, Materials, Plants, Bait and Trees**: every ore deposit (21 kinds), material, plant patch and bait spot, read from the game's own world files, on the world map, the minimap and in Find; every forest drawn as a shape in its tree type's colour.
- **Map Key & Filters panel** drawn with the game's own UI pieces: icon, count and ON/OFF per marker type, whole categories, custom pin filters, completed quests, SHOW ALL / HIDE ALL. It scrolls; Tab folds it away.
- **Two more zoom steps out** on the world map.
- **Find on the map** (Shift+F): type a name or a kind of place and the map moves to show the matches.
- **Settings** (the cog): fog modes (EXPLORED, HYBRID, REVEAL ALL), pin modes (pins only where you've been, grayed in unexplored land, or all), custom marker size and minimap options (a thin border, and hiding the info bar under it).
- **Looted chests**: opening a chest marks it (hidden or grayed with a check mark); chests come back after 30 minutes of play outside build zones. Saved per world, and shared with other players who have the mod.
- **HUD minimap** with the World Map's terrain, a compass, the time of day or the weather, other players as colored arrows, and up/down arrows on nearby chests above or below you: hold Shift + Alt to drag it, resize it from the bottom-right corner and zoom with the wheel. `[` and `]` zoom any time.
- **All 15 game languages**: the mod's text follows the game's language (or `Language=` in `minimap_config.ini`). Translations are my own; corrections welcome.
- Client only. Works in single player, hosted games and on any server, with nothing installed on the server.

Download and install instructions: [Nexus Mods page](https://www.nexusmods.com/enshrouded/mods/220). The player-facing readme is [dist/README.txt](dist/README.txt).

## Building

Needs Visual Studio 2022 Build Tools (x64).

```
build.bat   -> bin\loader\unlock-map-markers.dll and mod.json, then runs tests\test_patches.cpp
               (checks every byte patch site against the installed game and server executables)
deploy.py   -> installs the build into the game's greeneye\unlock-map-markers\ (game closed)
```

The mod runs through the Greeneye Loader (API 2, `src/greeneye.h`). All hooks go through the loader, which chains onto other mods' hooks instead of overwriting them (`src/jumppatch.h`). Every detour holds a `greeneye::Lease`, so the mod can be hot reloaded while idle. Code sites are found by unique signature. A site that doesn't match is skipped and logged instead of patched.

- `src/main.cpp`: start-up, byte patches (`src/patches.h`) that send every marker to the map UI.
- `src/map_key_panel.cpp`: the panel, Find, fog modes and the minimap.
- `src/lang.cpp`: the mod's text in every language, language detection (Steam's game language) and text widths.
- `src/chest_spawns.h`: chest spawn table.
- `src/minimap_art.h`: the minimap frame, tray, N medallion and weather icons as solid rects, generated from
  the vector art in `art/` by `art/gen_minimap_art.py` (needs Microsoft Edge to render the SVG).

## License

MIT, see [LICENSE](LICENSE). Made with the help of AI (Claude by Anthropic, OpenAI Codex and Google Antigravity).

## Keys and controller

`[Keys]` in `minimap_config.ini` holds every key and controller button; edits apply within two seconds. A value is keys
with `Shift+`, `Ctrl+`, `Alt+`, then `Pad:` chords after a comma (`G, Pad:LB+A`); `NoPad`, `NoKeys`, `None` turn things off.

| Setting | Default | Does |
|---|---|---|
| `PanelToggle` | `Tab` | fold the Map Key |
| `Glow` | `G, Pad:LB+A` | pressed twice: outline nearby chests |
| `MinimapMouse` | `Shift+Alt` | held: the minimap takes the mouse |
| `FindOnMap` | `Shift+F, Pad:LS+X` | open Find on the World Map |
| `MapZoomIn`, `MapZoomOut` | `NumPlus, PageUp`; `NumMinus, PageDown` | zoom the World Map |
| `MinimapZoomIn`, `MinimapZoomOut` | `], NumPlus, Pad:RS+Up`; `[, NumMinus, Pad:RS+Down` | zoom the minimap |
| `FilterUndo`, `FilterRedo` | `Ctrl+Z, Pad:RS+Left`; `Ctrl+Y, Ctrl+Shift+Z, Pad:RS+Right` | undo and redo Map Key changes; a message names the filters changed and ON or OFF |
| `HallRecenter` | `C, Pad:LS` | recentre on you in a dungeon map |
| `ModOnOff` | `Ctrl+Shift+F12` | switch the mod off and on |
| `ChestClick`, `HideRowClick`, `RegionOnlyClick` | `Ctrl`, `Alt`, `Ctrl` | held with a click: mark a chest, boss, lore note or unique item (`ChestClick` on any other marker hides its row, like `HideRowClick`), hide a marker's row, show one region; marks and hiding show a message of what changed |
| `CopyPosition` | `Mouse3` | copy the map position |
| `MarkChestPad`, `HideRowPad` | `Pad:RS+Up`, `Pad:RS+Down` | on the World Map: what `ChestClick` and `HideRowClick` do, at the middle of the view |
| `MapKeyEnter`, `MapKeyBack`, `MapKeyClick` | `View`, `B`, `A` | the Map Key on a controller |
| `MapKeyUp`, `MapKeyDown`, `MapKeyLeft`, `MapKeyRight` | D-pad | move through the Map Key |
| `FindClose`, `FindConfirm` | `Esc, Pad:B`; `Enter, Pad:A` | in Find |
| `FindNext`, `FindPrevious`, `FindUp`, `FindDown`, `FindPageUp`, `FindPageDown`, `FindFilters` | `Tab`, `Shift+Tab`, `Up, Pad:Up`, `Down, Pad:Down`, `PageUp`, `PageDown`, `F2, Ctrl+F` | in Find |

`[Chests] InteractKey` is the key that marks a chest looted; on a controller it follows the game's Interact button.
Other clicks and drags on the map and minimap stay on the mouse.

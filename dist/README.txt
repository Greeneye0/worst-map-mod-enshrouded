IN GAME MAP MARKERS AND MINI MAP - THE WORST MAP MOD  -  Enshrouded mod, version 1.0.0
=========================================================================================

(It's the best map mod. The name is the joke.)

Every marker on the world map, a Map Key & Filters panel, Find on the map, three fog modes
and a minimap on your HUD.


INSTALL
-------
1. Close Enshrouded.
2. In Steam: right-click Enshrouded > Manage > Browse local files.
   This opens the game folder (the one with enshrouded.exe), usually
   C:\Program Files (x86)\Steam\steamapps\common\Enshrouded
3. Copy everything from this zip into that folder, keeping the folders:
       dbgcore.dll                                        next to enshrouded.exe
       greeneye\unlock-map-markers\unlock-map-markers.dll
       greeneye\unlock-map-markers\mod.json
4. Start the game and open the map (M).

Client only: install it on your own game. It works in single player, when you host, and on
any dedicated server, with nothing installed on the server. Other players don't need it.

Install it by hand: Vortex does not know the Greeneye Loader yet.


THE MAP KEY & FILTERS PANEL
---------------------------
On the right of the world map.
- Every marker type has a row with its icon, how many there are, and an ON/OFF switch.
  Click a row to show or hide that type.
- Click a category header (FAST TRAVEL & ALTARS, SHROUD & VAULTS, WORLD & RESOURCES,
  QUESTS & RUMORS) to switch the whole category, or + / - to fold it.
- SHOW ALL / HIDE ALL switch everything at once.
- CUSTOM PINS: click a pin icon to show or hide your own map pins of that kind.
- Tab or the - button at the top folds the whole panel away.
- Every gold and silver chest in the world is included: all 448 gold and 1032 silver
  chests have their own filter rows.


FIND ON THE MAP (Shift+F)
-------------------------
On the world map, press Shift+F or click FIND. Type a name or a kind of place ("gold",
"spire", "garrison"). The list shows every match with its region, its distance from you,
and [Locked] if you haven't unlocked it yet. Click a result twice, double-click it, or press
Enter, and the map moves to show it. "All Matching Places - Zoom to fit" (or Enter with
nothing selected) shows every match at once.
- F2 (or Ctrl+F) switches whether Find skips marker types you've hidden with the filters.
- Esc or Shift+F closes it.


FOG
---
The FOG: row on the panel:
- EXPLORED     the game's own fog: you only see where you've been.
- HYBRID       the whole map is drawn, unexplored land under a light fog.
- REVEAL ALL   no fog at all.
The mode is remembered.


MINIMAP
-------
A minimap with a compass, the time of day and its zoom level on your HUD, under the
Journal [J] tab at first.
- Hold Shift + Alt: the mouse cursor appears and the camera stops turning, so you can use
  the minimap without swinging your view or attacking.
    drag the minimap                 move it anywhere on screen
    drag its bottom-right corner     make it bigger or smaller
    mouse wheel                      zoom
    double-click the top star        put it back in its default place
- Zoom any time with [ and ] (or numpad - and +), or the - / + buttons under it.
- Other Players (in the panel's QUESTS & RUMORS group) shows other players on the map and the
  minimap.
- Position, size and zoom are saved in greeneye\unlock-map-markers\minimap_config.ini.


UNINSTALL
---------
Delete greeneye\unlock-map-markers. If no other mod is left in the greeneye folder, delete
dbgcore.dll and the greeneye folder too.


GOOD TO KNOW
------------
- dbgcore.dll is the Greeneye Loader: it loads the mods in the greeneye folder (Find, Quick
  Deposit, Continue, Embervale Teleport and others share it) and forwards everything else to
  Windows' own dbgcore.dll.
- Works next to other mods. Where another mod hooks the same game function, this one chains
  onto its hook instead of replacing it.
- Made for the current game version. If a game update changes the code it patches, it
  skips that part instead of crashing, and the log says so.
- With the Find mod installed, Find on the map uses Find's own search box.


TROUBLESHOOTING
---------------
If something doesn't work, please post these files with your report (all in the game folder):
- greeneye\unlock-map-markers\unlock_map_markers.log   what the mod found and patched
- greeneye\loader.log                                  what the Greeneye Loader did


CHANGES
-------
1.0.0
- First release.

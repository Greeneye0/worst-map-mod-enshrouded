IN GAME MAP MARKERS AND MINI MAP - THE WORST MAP MOD  -  Enshrouded mod, version 1.2.0
=========================================================================================

Every marker on the world map, a Map Key & Filters panel, Find on the map, fog and pin
modes, looted chests and a minimap on your HUD.


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
- COMPLETED QUESTS (off at first) shows the quests you've finished.
- CUSTOM PINS: click a pin icon to show or hide your own map pins of that kind.
- Tab or the - button at the top folds the whole panel away. The cog opens SETTINGS.
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


SETTINGS
--------
The cog on the Map Key (and the one under the minimap) opens SETTINGS:
- SHOW ALL / HIDE ALL switch every marker type at once.
- FOG:
    EXPLORED     the game's own fog: you only see where you've been.
    HYBRID       the whole map is drawn, unexplored land under a light fog.
    REVEAL ALL   no fog at all.
- PINS:
    EXPLORED     pins only where you've been.
    HYBRID       pins in unexplored land are grayed out.
    REVEAL ALL   every pin.
- Custom Marker Size   the size of your own pins.
- Show Minimap, Minimap Terrain, Minimap Icon (sun/moon or weather).
- Looted Chests        hide them or gray them out.
Everything is remembered.


LOOTED CHESTS
-------------
Open a gold or silver chest and the mod marks it looted: hidden, or grayed out with a check
mark (Settings > Looted Chests). On the world map, Ctrl + left click a chest to mark it or
clear the mark. A looted chest comes back after 30 minutes of play unless it's in a Flame
Altar's build zone. Looted chests are saved per world in
greeneye\unlock-map-markers\looted_chests.ini. In minimap_config.ini:
    [Chests]
    InteractKey=E          the key you open chests with
    RespawnMinutes=30


MINIMAP
-------
A minimap with the World Map's terrain, a compass, the time of day (or the weather) and its
zoom level on your HUD, under the Journal [J] tab at first. Cleared places and looted chests
show a check mark.
- Hold Shift + Alt: the mouse cursor appears and the camera stops turning, so you can use
  the minimap without swinging your view or attacking.
    drag the minimap                 move it anywhere on screen
    drag its bottom-right corner     make it bigger or smaller
    mouse wheel                      zoom
    double-click the top star        put it back in its default place
- Zoom any time with [ and ] (or numpad - and +), or the - / + buttons under it. The cog
  next to them opens Settings (the cursor appears while it's open).
- Other Players (in the panel's QUESTS & RUMORS group) shows other players on the map, and on
  the minimap as colored arrows that point where they're heading.
- Position, size and zoom are saved in greeneye\unlock-map-markers\minimap_config.ini.


LANGUAGES
---------
The mod's text follows the game's language: English, German, French, Italian, Spanish,
Portuguese (Brazil), Polish, Russian, Ukrainian, Turkish, Japanese, Korean, Simplified Chinese,
Traditional Chinese and Thai. Marker and region names come from the game itself.
To use another language, set it in greeneye\unlock-map-markers\minimap_config.ini:
    [General]
    Language=auto          auto follows the game; or english, german, french, italian,
                           spanish, brazilian, polish, russian, ukrainian, turkish, japanese,
                           koreana, schinese, tchinese, thai
Restart the game after changing it. Translations are the author's own: corrections welcome.


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
1.2.0
- Settings (the cog on the Map Key, and under the minimap): Show All / Hide All, fog, the new
  pin modes, custom marker size, minimap on or off, minimap terrain, sun/moon or weather, and
  looted chests hidden or grayed out.
- Pin modes: EXPLORED shows pins only where you've been, HYBRID grays out pins in unexplored
  land, REVEAL ALL shows them all. Requested by dremen and KazarusKain.
- Completed quests have their own filter (off at first), so Main Quest and Side Quest show
  only the quests you're on. Requested by inShaneity, KazarusKain and Deadcow67.
- Looted chests: open a gold or silver chest and it's marked looted, hidden or grayed out with
  a check mark. Ctrl + left click a chest on the map to mark it or clear the mark. Chests come
  back after 30 minutes of play unless they're in a Flame Altar's build zone. Saved per world.
  Requested by inShaneity and FWSWBN.
- Minimap: the World Map's terrain under it and check marks on cleared places (requested by
  inShaneity), weather in place of the sun or moon (requested by Loling91), an option to hide
  it (requested by FWSWBN), and other players as colored arrows that show where they're
  heading.
- Custom Marker Size for your own pins. Requested by dremen.
- The map's hint bar shows the mod's keys: Ctrl+click, Shift+F and Tab.
- Reported bugs fixed: none.

1.1.0
- The mod's text in all 15 languages the game supports, following the game's language
  (or Language= in minimap_config.ini). The map key widens to fit longer labels.
  Requested by Loling91 on Nexus.
- Find ignores upper and lower case in every script, finds places by their English type name
  in any language, and no longer cuts names in the middle of a character.
- Reported bugs fixed: none.

1.0.0
- First release.

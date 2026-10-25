IN GAME MAP MARKERS AND MINI MAP - THE WORST MAP MOD  -  Enshrouded mod, version 1.3.7
=========================================================================================

Every marker on the world map, every ore deposit, material, plant, bait spot and forest, lore
notes and bosses, a Map Key & Filters panel, Find on the map, fog and pin modes, looted chests,
a minimap on your HUD, and G G to make what's nearby glow through walls.


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
- SHOW ALL / HIDE ALL at the top switch every marker type at once.
- Click a category header (FAST TRAVEL & ALTARS, SHROUD & VAULTS, WORLD & RESOURCES, MINING,
  MATERIALS, PLANTS, BAIT, TREES, QUESTS & RUMORS) to switch the whole category, or + / - to
  fold it.
- The mouse wheel or the scroll bar scrolls the list.
- CUSTOM PINS: click a pin icon to show or hide your own map pins of that kind.
- Tab or the - button at the top folds the whole panel away. The cog opens SETTINGS.
- Every gold and silver chest in the world is included: all 448 gold and 1032 silver
  chests have their own filter rows.
- FILTER LOCKS: the padlock left of a switch locks that filter. A locked filter keeps its ON
  or OFF through SHOW ALL / HIDE ALL, reloads and updates (a locked category also keeps it
  open or closed); you can still click it. Hold SHOW ALL or HIDE ALL to change locked
  filters too. Settings > Filter Locks > Remove All unlocks everything.
- LORE NOTES (QUESTS & RUMORS): every note and diary in the world, 574. Notes you've read
  are hidden or grayed out (Settings > Looted Chests). Hover one for its title.
- BOSSES (WORLD & RESOURCES): the world's bosses, named like their health bar; beaten ones
  are hidden or grayed out.
- RETURN BEACONS (SHROUD & VAULTS): all 282.
- WATER SOURCE also shows the world's wells and water barrels you can drink from, and the
  wells and barrels you build (remembered per world).


G G: WHAT'S NEARBY GLOWS
------------------------
Press G twice quickly while playing. For 14 seconds everything within 75 m that isn't done
yet glows through walls and floors: unlooted gold and silver chests (in gold and silver),
unread lore notes and bosses you haven't beaten, while their filters are on. They pulse on
the world map and the minimap too.


MINING, MATERIALS, PLANTS, BAIT AND TREES
-----------------------------------------
Read from the game's own world files, so every spot is where the game put it.
- MINING: every ore deposit, 21 kinds (copper, iron, silver, gold, sulfur, salt...), 7,452 in
  all.
- MATERIALS: farm soil, fertilised farm soil, flower soil, twigs, bones, tar, toxic slime, luminous growth, ice,
  gold coins, archaic essence.
- PLANTS: 30 harvestable plants, as patches.
- BAIT: fireflies, grasshoppers, pillbugs, frogs and more.
- TREES: each forest drawn as one shape in its tree type's colour, 18 types; the big ones
  carry their name.
Each kind has its own row, all off at first. Hover an icon on the world map for its name.
Deposits, materials, plants and bait also show on the minimap and in Find.


FIND ON THE MAP (Shift+F)
-------------------------
On the world map, press Shift+F or click FIND. Type a name or a kind of place ("gold",
"spire", "garrison"). The list shows every match with its region, its distance from you,
and [Locked] if you haven't unlocked it yet. Click a result twice, double-click it, or press
Enter, and the map moves to show it. "All Matching Places - Zoom to fit" (or Enter with
nothing selected) shows every match at once.
- F2 (or Ctrl+F) switches whether Find skips marker types you've hidden with the filters.
  Pick a result a filter hides and the Map Key opens its row and flashes it.
- Lore notes and bosses are found by their own names.
- Esc or Shift+F closes it.


SETTINGS
--------
The cog on the Map Key (and the one under the minimap) opens SETTINGS:
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
- Minimap Border: ORNATE or THIN (a fine line, so the map takes less room).
- Minimap Info Bar: off hides the bar under the minimap; the cog moves to the map's
  bottom-left corner, and [ and ] still zoom. With Show Minimap off and the info bar on, the
  bar (time, weather, cog) stays on screen by itself.
- Looted Chests        hide them or gray them out (read lore notes and beaten bosses too).
- Completed Places     SHOW or HIDE the places the game marks done (cleared roots, used
                       obelisks, cleared vaults).
- Filter Locks         how many filters are locked; REMOVE ALL unlocks them.
Everything is remembered.


LOOTED CHESTS
-------------
Open a gold or silver chest and the mod marks it looted: hidden, or grayed out with a check
mark (Settings > Looted Chests). On the world map, Ctrl + left click a chest to mark it or
clear the mark. Players who have the mod share their looted chests: open one and it's marked
on their maps too. A looted chest comes back after 30 minutes of play with every player at
least 500 m away (coming closer starts the 30 minutes over), unless it's in a Flame Altar's
build zone. Looted chests are saved per world in
greeneye\unlock-map-markers\looted_chests.ini. In minimap_config.ini:
    [Chests]
    InteractKey=E          the key you open chests with
    RespawnMinutes=30
    ResetRadius=500        meters; a player this close starts the 30 minutes over


MINIMAP
-------
A minimap with the World Map's terrain, a compass, the time of day (or the weather) and its
zoom level on your HUD, under the Journal [J] tab at first. Cleared places and looted chests
show a check mark. A gold or silver chest, lore note or boss within 75 m that is 4 m or more
above or below you gets a small green up or orange down caret.
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
- Ctrl + Shift + F12 switches the whole mod off and on, to compare with the game as it is
  (best with the world map open).


TROUBLESHOOTING
---------------
If something doesn't work, please post these files with your report (all in the game folder):
- greeneye\unlock-map-markers\unlock_map_markers.log   what the mod found and patched
- greeneye\loader.log                                  what the Greeneye Loader did


CHANGES
-------
1.3.7
- Press G twice while playing: everything near you that isn't done yet glows through walls
  and floors for 14 seconds (unlooted gold and silver chests, unread lore notes, bosses you
  haven't beaten), and pulses on the map and minimap. Requested by nys4nd3r.
- Lore Notes: every note and diary in the world on the map (574); the ones you've read are
  hidden or grayed out like looted chests. Requested by Azirael.
- Bosses: the world's bosses on the map, named like their health bar; beaten ones are hidden
  or grayed out. Requested by apcallover.
- Return Beacons: all 282 in the Shroud group. Requested by MrKrabbe.
- Water Source now also shows the world's wells and water barrels, and the ones you build.
- Filter locks: a padlock keeps a filter through Show All / Hide All, reloads and updates.
- Settings > Completed Places: hide places the game marks done. Requested by DrSmiley79.
- Find: picking a hidden result flashes its row in the Map Key; notes and bosses by name.
- Lore notes and bosses get the minimap's up or down caret too.
- Reported bugs fixed: mining, material and plant names were in English in other languages
  (reported by apcallover); the World Map jumped back to you after Alt-Tab; looted chests
  carried over into a new world; the chest filters turned back on after a restart.

1.3.6
- Finished quests leave the map: a quest you've completed no longer keeps its pin, a main
  quest's area circle goes with it, and a lore rumor about something you've already found is
  gone too (reported by inShaneity).
- The Completed Quests filter is removed: finished quests are always hidden, as in the game.

1.3.5
- Minimap weather now shows the game's real weather: Rain, Snow, and Storm for a blizzard (it
  always said Clear). The game has no fog weather.
- Looted chests stay marked while a player is within 500 m; the game only refills a chest
  after everyone has been that far away for 30 minutes (found by TheSefer). The distance is
  ResetRadius= in minimap_config.ini.
- Smaller text in the Map Key: filter rows, category titles and ON / OFF fit their rows.
  Requested by ZharonJoy.
- Ctrl + Shift + F12 switches the whole mod off and on, to compare with the game as it is.
- Reported bugs fixed:
  - Show on Map for a quest opened the map on the player again (reported by HadToRegister).
  - With the thin minimap border the info bar overlapped the border and its cog was hard to
    reach (reported by HadToRegister and ZharonJoy).
  - Hollow Halls and lore rumors showed with Pins: EXPLORED (reported by ZharonJoy).
  - Clicks over the minimap in the backpack started dragging it, so items stuck to the cursor
    (reported by Jowdan).
  - Find on the map: Tab typed a space instead of picking a result, and Escape could stop
    working.

1.3.4
- Show Minimap off with Minimap Info Bar on keeps the info bar (time, weather, cog) on screen
  by itself, and the two settings sit together. Requested by FWSWBN.
- Reported bugs fixed: none.

1.3.3
- Fertilised Farm Soil has its own row under Materials (it was counted as Farm Soil).
- Zoomed out, forests draw as merged strips and share one budget with the deposit icons, so the
  map keeps drawing with everything on.
- Reported bugs fixed:
  - The game could freeze or crash on the World Map with tree types on, when forest names
    landed on each other (reported by fostot).

1.3.2
- Minimap Border: Thin swaps the ornate frame for a fine line, so the map takes less room.
  Requested by fraidnot, HadToRegister and fostot.
- Minimap Info Bar: turn off the bar under the minimap (time, weather, zoom buttons); the cog
  moves to the map's bottom-left corner. Requested by fostot.
- Reported bugs fixed: none.

1.3.1
- Minimap: nearby chests above or below you get a green up or orange down arrow. Requested
  by Lomack.
- Minimap: other players show in the Hollow Halls too.
- Reported bugs fixed: none.

1.3.0
- Mining: every ore deposit in the world, 21 kinds, on the world map, the minimap and in Find,
  each with its own row in the Map Key.
- Materials, Plants and Bait: farm soil, twigs, tar, ice, 30 plants, fireflies, frogs and
  more, the same way.
- Trees: every forest drawn as a shape in its tree type's colour, with the big ones named.
- Two more zoom steps out on the world map. Requested by HadToRegister.
- The Map Key scrolls (mouse wheel or scroll bar), SHOW ALL / HIDE ALL are back at its top,
  and the category bars look like the game's own.
- Looted chests are shared: players who have the mod see the chests the others open.
- Reported bugs fixed:
  - Some quests and lore rumors hid under other icons (reported by Deadcow67 and
    KazarusKain).
  - Show on Map (F) in the Journal opened the map on the player instead of the quest
    (reported by sandesign, Loling91 and KazarusKain).
  - With a gamepad, the map cursor didn't move after tabbing to the map (reported by
    Chocobostew).

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

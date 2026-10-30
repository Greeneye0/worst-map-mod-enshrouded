IN GAME MAP MARKERS AND MINI MAP - THE WORST MAP MOD  -  Enshrouded mod, version 1.3.15
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
- Search this list, under them: type part of a row's name and only the matching rows show,
  their sections opened (folded or locked ones too). Esc or the x clears it. From 3 letters it
  also finds places by their name (a lore note, a boss, a place, a pin): the row they're in
  shows with that name after it, even when the row is off. Find (Shift + F) takes you there.
- REGIONS, under them (with Settings > Advanced > Region Filter on): a row per region in level
  order (Springlands, Revelwood, Blackmire, Nomad Highlands, Kindlewastes, Albaneve Summits,
  Veilwater Basin, Cinder's Cradle), each with its own ON / OFF, in a drawer + / - folds away.
  Click the row's name to switch them all on or all off; Ctrl + click a region to show only that
  one. Pins outside the regions that are on are dimmed (DIM) or not shown at all (HIDE), and their
  borders are drawn as a dotted gold line. Quest pins, lore rumors and your own pins always show.
- Click a category header (FAST TRAVEL & ALTARS, SHROUD & VAULTS, WORLD & RESOURCES, MINING,
  MATERIALS, PLANTS, BAIT, TREES, QUESTS & RUMORS) to switch the whole category, or + / - to
  fold it; folded categories stay folded the next time you play. Drag a category header up
  or down to move the category; the order is remembered.
- The rows in each category are sorted by name in your language.
- The mouse wheel or the scroll bar scrolls the list.
- Hide a marker's row from the map: Alt + left click the marker. Ctrl+Z undoes a filter change and Ctrl+Y redoes it.
- CUSTOM PINS: click a pin icon to show or hide your own map pins of that kind.
- Tab or the - button at the top folds the whole panel away. The cog opens SETTINGS.
  To use another key than Tab, set PanelToggle= under [Keys] in
  greeneye\unlock-map-markers\minimap_config.ini: a letter, a digit, F1 to F12, CapsLock,
  Backquote, Insert, Delete, Home, End, or None for no key.
- Every gold and silver chest in the world is included: all 448 gold and 1032 silver
  chests have their own filter rows. WOODEN CHEST (World & Resources, off at first): the
  world's 1562 wooden chests, wooden sarcophagi and locked wooden chests.
- FILTER LOCKS: the padlock left of a switch locks that filter. A locked filter keeps its ON
  or OFF through SHOW ALL / HIDE ALL, reloads and updates (a locked category also keeps it
  open or closed); you can still click it. Hold SHOW ALL or HIDE ALL to change locked
  filters too. Settings > Filter Locks > Remove All unlocks everything.
- LORE NOTES (QUESTS & RUMORS): every note and diary in the world, 574. Notes you've read
  are hidden or grayed out (Settings > Looted Chests); unread ones are drawn bigger on an
  amber ring so they stand out. Hover one for its title.
- BOSSES (WORLD & RESOURCES): the world's bosses, named like their health bar. A boss you've
  beaten gets a check mark for good; it's hidden or grayed out only while it's dead, until it
  comes back by the same rule as a looted chest (its 256 m square reset 30 minutes after everyone
  left). A boss you kill is seen dying when you're there, so beating one again grays it too. The
  game keeps no record of beating some of them, and the mod can't see the Fell Critter Queens and
  the Fog Predator: Ctrl + left click a boss on the map to step it: beaten (grayed), came back
  (check mark only), never beaten. The beaten marks are saved per character, the timers per world.
- UNIQUE WEAPONS and UNIQUE EQUIPMENT (WORLD & RESOURCES): the one-of-a-kind weapons (40, like
  the Wailing Blade and Talion's Bow) and rings and gear (16, like the Gemini Ring and the
  Flaming Gauntlets), each at the chest or place it is in, with its own icon and name. One in
  a gold or silver chest counts as found once that chest is looted; for the others (in
  barrels, crates or lying in the world) Ctrl + left click it to mark it found, saved per
  character. Find (Shift + F) finds them by name.
- COLLECTIBLES (WORLD & RESOURCES): the journal's collectibles, 133, lying in the world or in a
  chest: fossils, skeleton pieces, the bard's paintings, Drak reliefs, mosaic and map pieces,
  meteorites and more, each with its own icon and name. One in a chest is found once the chest is
  looted; Ctrl + left click any other to mark it found (saved per character). Find finds them by
  name.
- FELL SUMMONING STONES (WORLD & RESOURCES): the 45 stones where Shroud Spores conjure a Fell
  enemy, one tier per region.
- RETURN BEACONS (SHROUD & VAULTS): all 282.
- WATER SOURCE (WORLD & RESOURCES) also shows the world's wells and water barrels you can
  drink from, and the wells and barrels you build (remembered per world).


G G: WHAT'S NEARBY GLOWS
------------------------
Press G twice quickly while playing. For 14 seconds everything within 75 m that isn't done
yet glows through walls and floors: unlooted gold, silver and wooden chests, unread lore notes,
bosses you haven't beaten, and what the quests you're on want there (the quest's item or
place, with its quest icon), while their filters are on. They pulse on the world map and the
minimap too.
Vein sight: the ore and materials of the Mining and Materials rows you have on show for 30
seconds too, for the deposits the map shows near you. Within 12 m their edges glow on the
rock itself, in the game's build zone colour (dig in and the glow on the walls of your hole
shows where the ore ends); farther out (50 m) each vein is outlined on screen in its ore's
colour, with the deposit's icon in its middle.


MINING, MATERIALS, PLANTS, BAIT AND TREES
-----------------------------------------
Read from the game's own world files, so every spot is where the game put it.
- MINING: every ore deposit, 21 kinds (copper, iron, silver, gold, sulfur, salt...), 7,452 in
  all.
- MATERIALS: farm soil, fertilised farm soil, flower soil, twigs, bones, tar, toxic slime, luminous growth, ice,
  gold coins, archaic essence.
- PLANTS: 30 harvestable plants, as patches, and each region's night plants (Midnight Lily,
  Gloomberry, Calla Luna, Shadowbloom, Glimmer Gentian, Stapelia Starflower, Passionflower,
  Gleam Root) where they grow at night.
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
- Esc closes it (Shift+F types a capital F in the search).


WORLD MAP ZOOM KEYS
-------------------
On the world map, Page Up / Page Down (or numpad + and -) zoom in and out one step, keeping
the map where it is.


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
  EXPLORED also keeps everything the game itself shows in land still fogged: places it
  knows of, places you've discovered, and lore rumor hints. With EXPLORED and HYBRID, a place
  you haven't discovered yet (the game would hide it) is grayed out, on the world map and the
  minimap, even where the fog is cleared; REVEAL ALL shows it in full colour.
- Custom Marker Size   the size of your own pins.
- Show Minimap, Minimap Terrain, Minimap Icon (sun/moon or weather).
- Time Format: 1:00 PM or 13:00 for the minimap's clock (at first it follows Windows).
- Minimap Border: ORNATE or THIN (a fine line, so the map takes less room).
- Minimap Info Bar: off hides the bar under the minimap; the cog moves to the map's
  bottom-left corner, and [ and ] still zoom. With Show Minimap off and the info bar on, the
  bar (time, weather, cog) stays on screen by itself.
- Looted Chests        hide them or gray them out (read lore notes and beaten bosses too).
- Completed Places     SHOW or HIDE the places the game marks done (cleared roots, used
                       obelisks, cleared vaults) and the Flame and Night Sanctums you've
                       visited. Shown, visited sanctums get a check mark.
- Filter Locks         how many filters are locked; REMOVE ALL unlocks them.
- ADVANCED > Position (X, Y, Z), off at first: your position under the minimap (Y is your
  height) and, on the world map, the X and Z under the cursor. Click the copy button (or
  middle click the world map) to copy them, like "X 3215, Z 2051".
- ADVANCED > Region Filter, off at first: the Map Key's REGIONS row (see above).
- ADVANCED > Region Colors, off at first: each region tinted in its own colour on the world map.
- ADVANCED > Vanilla World Map, off at first: the world map exactly as the game draws it, as your
  friends without the mod see it (the game's markers, fog, zoom and filter menu; no Map Key or
  added pins). The minimap stays, and its cog is then the way to Settings.
- ADVANCED > Vanilla Minimap, under it: the minimap shows only what the game's world map shows,
  the game's own markers and the players, and none of the mod's chests, resources, lore notes,
  portals and the like.
Everything is remembered.


LOOTED CHESTS
-------------
Open a gold or silver chest and the mod marks it looted: hidden, or grayed out with a check
mark (Settings > Looted Chests). Once it refills it's drawn as usual again but keeps its check
mark, so you can see which chests you've looted at least once. On the world map, Ctrl + left
click a chest to step it: looted, refilled (check mark only), never looted. Chests you opened
before installing the mod aren't known to it: they show until you open them again or Ctrl +
left click them. Players who have the mod share their looted chests: open one and it's marked
on their maps too.
A looted chest refills by the game's own rule: the world is cut into 256 m squares, and 30
minutes after every player has left a square's range (258 m from each of its corners) the
game resets it. Its chests refill and its bosses come back. Chests in a Flame Altar's build
zone never come back. On a dedicated server the time you're away counts too, if the server's
clock kept running (one that stands still while nobody is on counts nothing). Looted chests
are saved per world in greeneye\unlock-map-markers\looted_chests.ini. In minimap_config.ini:
    [Chests]
    InteractKey=E          the key you open chests with
    RespawnMinutes=30
    ResetRadius=0          meters; a player this close also holds the 30 minutes (0: only
                           the game's rule)


MINIMAP
-------
A minimap with the World Map's terrain, a compass, the time of day (or the weather) and its
zoom level on your HUD, under the Journal [J] tab at first. Cleared places and looted chests
show a check mark. A gold or silver chest, lore note, boss or mining or materials deposit
within 75 m that is 4 m or more above or below you gets a small green up or orange down caret,
and so do another player and a player grave (on the world map too).
- Hold Shift + Alt: the mouse cursor appears and the camera stops turning, so you can use
  the minimap without swinging your view or attacking.
    drag the minimap                 move it anywhere on screen
    drag its bottom-right corner     make it bigger or smaller
    mouse wheel                      zoom
    double-click the top star        put it back in its default place
- Zoom any time with [ and ] (or numpad - and +), or the - / + buttons under it. The cog
  next to them opens Settings (the cursor appears while it's open).
- Other Players (in the panel's QUESTS & RUMORS group) shows other players on the map, and on
  the minimap as colored arrows that point where they're heading. Each player has the same
  colour on every screen: the first player in the server yellow, the second blue, then red,
  green, purple and on.
- Town Portals (FAST TRAVEL & ALTARS): with the Town Portal mod, its open portals show on the
  map and the minimap in their owner's colour, the same colour as that player's arrow.
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
1.3.22
- Bosses gray out by themselves when you beat them, also when you beat one again after it came
  back (the game only records the first kill): the mod sees the boss alive at its spot and gone
  after the fight. Not for the Fell Critter Queens and the Fog Predator, which still need a Ctrl +
  click. Reported by GARONNN.
- A place the game never marks discovered (like a Scavenger Camp west of Pikemead) stops being
  grayed out once you've been there. Reported by GARONNN.
- Fixed: lore rumor "?" pins disappeared where the place they point to has its row switched off.
  They stay now. Reported by GARONNN.

1.3.21
- Settings > Advanced > Vanilla World Map: the world map exactly as the game draws it, so it
  matches what your friends without the mod see, while the minimap stays. Vanilla Minimap, under
  it, makes the minimap show only the game's own markers too. Requested by menelothar.

1.3.20
- Collectibles: 32 more, the ones that sit in a chest or container instead of lying loose: Flame
  Mosaic Piece I, the four Dragon's Map pieces, the Embervale Miniature pieces, the orrery rings
  and a few more (133 in all). One in a chest counts as found once the chest is looted, and G G
  lights them like the rest. Reported by TheSefer.
- The bosses and items you marked by hand are now saved by where they are, so adding pins can't
  move your marks to other pins. Your marks from earlier versions carry over.

1.3.19
- Regions: show several regions at once. The Region row now opens a drawer with every region and
  an ON / OFF switch each, so you can keep Springlands and Revelwood on and add the next region
  when you're ready for it. Click the row's name to switch them all on or all off, Ctrl + click a
  region to show only that one, and + / - folds the drawer away. Pins outside the regions that are
  on are dimmed or hidden as before, and each one's border is drawn. Your region from 1.3.18
  carries over. Requested by FrostyBou.
- Town Portals and the water sources you build now follow the Region filter too.
- Fixed: Ctrl + Shift + F12 could crash the game, rarely, when switching the mod off or on (the
  map's code was rewritten while the game was running it). Reported by secretroff.
- Fixed: with a controller that isn't an XInput pad (a PlayStation controller, for one), the world
  map could stop answering the stick after the Map Key was clicked with the mouse, until the mouse
  moved. Reported by FamanFortune.

1.3.18
- The Hide button under a marker's right-click menu is gone, so the game's menu shows only its own
  buttons. Alt + left click a marker still turns its row off.
- Fixed: the minimap showed locked Ancient Spires and Fast Travel Points as active until you had
  opened the world map that session. It now grays them out like the world map does.

1.3.17
- Wooden chests: all 1562 of the world's wooden chests, wooden sarcophagi and locked wooden
  chests, under World & Resources > Wooden Chest (off at first, switch it on). Looted ones are
  hidden or grayed out and keep their check mark like gold and silver, they glow with G G, and
  Find finds them. Requested by AnkuSol.
- G G also lights what the quests you're on want near you: the quest's item or place, with its
  quest icon, while the Main Quest / Side Quest rows are on. Requested by Gadn.
- Search this list finds places by their name too: type 3 letters or more of a lore note, boss,
  place or pin and its row shows with that name ("Lore Note: Song Of ..."), even when the row is
  off.
- Pins: EXPLORED no longer hides what the game itself shows: places it knows of in land still
  fogged (drawn faded), places you've discovered, and lore rumor hints such as "Undiscovered:
  Shroud Root"; towns like the Alchemist Tower and cave passages get their rumor "?" too. A
  started quest's search area (Cat Search, the Customization quests) shows under its quest's row.
  Reported by secretroff.
- Pins: REVEAL ALL shows every place in full colour, the ones you haven't discovered yet too
  (EXPLORED and HYBRID still gray those out). Locked spires and fast travel points keep their
  gray.
- Faster: the minimap and the world map do about half the work they did each frame, and joining a
  world with many locked filters no longer freezes the game for seconds. Reported by kingkongcs.
- Fixed: with a controller, the world map could stop answering it after the mouse was used, until
  the mouse moved again. Reported by FamanFortune.

1.3.16
- Chests you've looted keep their check mark for good: a looted chest is grayed out only until it
  respawns, then shows in full colour with its check mark, so you can see which chests you've
  looted at least once. Requested by izisot.
- Chests and bosses come back by the game's own rule: 30 minutes after every player has left the
  256 m square of the world they're in (ResetRadius=0, the new default, means this).
- Bosses you've beaten keep their check mark too, grayed out until they respawn by the same rule.
- Right-click a marker on the world map: a Hide button under the game's menu turns that marker's
  Map Key row off. Alt + left click a marker does it straight away. Works on players, Town
  Portals, quests and rumors too.
- Ctrl+Z / Ctrl+Y on the world map undo and redo filter changes (up to 50), shown on the hint bar
  while there is one to undo or redo.
- Search this list: a box under Show All / Hide All narrows the Map Key to the rows that match
  what you type, opening their sections (folded or locked ones too). Esc clears it. It doesn't
  search the map; Find (Shift+F) does.
- Town Portals (Fast Travel & Altars): with the Town Portal mod, its open portals show on the map
  and the minimap in their owner's colour.
- Other players: each player has the same colour on every screen, and sees themselves in it (the
  first player in the server yellow, the second blue, then red, green, purple and on).
- Fixed: chests near you that were marked looted came back as not looted a moment later when the
  game still had their loot (a chest hidden with Ctrl + left click, or emptied by another mod).
  Only a chest the game refilled is unmarked now. Reported by izisot and Loling91.
- Fixed: on some setups looted chests never grayed on their own: the mod couldn't tell which of
  two matching chest states to read. Reported by ryuskyn.
- Fixed: Find's results list (Shift+F) didn't scroll with the mouse wheel, and its scroll bar
  couldn't be grabbed. The wheel scrolls it now, and you can drag the scroll bar or click on it to
  jump. Reported by Lomack.

1.3.15
- Fixed: the fog mode (Explored / Hybrid / Reveal All) went back to Explored every time you
  started the game. Reported by Gadn.
- Fixed: Revelwood's Abandoned Farms and Abandoned Windmills, which the game never puts on the
  map, showed grayed out for good. They stay hidden now, like in the vanilla game. Reported by
  BlaineCraner.
- Fixed: a section's ON / OFF stayed ON when one of its rows was locked ON. The switch now changes
  every row in its section, locked rows too; Show All / Hide All still leave locked rows alone.

1.3.14
- Unique Weapons (40) and Unique Equipment (16): every one-of-a-kind weapon, ring and piece of
  gear at the chest or place it is in. Requested by Gadn.
- Collectibles (101): fossils, skeleton pieces, the bard's paintings, Drak reliefs, mosaic
  pieces, meteorites and more. Requested by TheSefer and BlaineCraner.
- Fell Summoning Stones (45): where Shroud Spores conjure a Fell enemy.
- Night plants: each region's plants that grow only at night, as Plants rows.
- Settings > Advanced > Region Filter: a Region row in the Map Key shows one region, the others
  dimmed or hidden, with its border drawn. Requested by VintageMarble.
- Settings > Advanced > Region Colors: each region tinted on the world map. Requested by izisot.
- Settings > Advanced > Position: your X, Y and Z under the minimap, the X and Z under the
  cursor on the world map, with a copy button. Requested by Loling91.
- G G vein sight: the ore of the Mining and Materials rows you have on is outlined, on the rock
  within 12 m and on screen out to 50 m.
- On the minimap, a mining or materials deposit above or below you gets the up or down caret.
- A boss and the unique item in its chest are drawn side by side.
- Fixed: the mod's icons showed on top of a marker's menu.
- Fixed: a capital F typed into a custom pin's name opened Find and closed the name box.
  Reported by dremen.
- The new rows and settings start off.

1.3.13
- Folded Map Key categories stay folded the next time you play. Requested by HadToRegister.
- Ctrl + left click works on every boss: a beaten one can be marked not beaten (with Looted
  Chests: Grayed Out they show to click). Requested by Delacroix.
- Places you haven't discovered yet are grayed out, even where the fog is cleared, so you can
  tell which ones you still have to find. Requested by Delacroix.
- On a dedicated server, a lore note you read hides right away, from what the server sends,
  instead of after the server saves.
- On a dedicated server, time away only brings chests back if the server's clock kept
  running: one that stands still while nobody is on no longer clears your looted chests.

1.3.12
- The key that folds the Map Key panel can be changed: PanelToggle= under [Keys] in
  minimap_config.ini (Tab at first; None for no key). Requested by KazarusKain.
- Bosses the game keeps no record of beating (the Dire Wolf, the Critter Queens, the Fog
  Predator and others): Ctrl + left click one on the map to mark it beaten.
- A player grave 4 m or more above or below you gets the up or down caret, on the minimap and
  the world map.

1.3.11
- World map: Page Up / Page Down (or numpad + and -) zoom in and out one step.
- Map Key: drag a category header to move the category; the rows in each category are sorted
  by name in your language. Water Source moved to World & Resources.
- Settings > Time Format: 1:00 PM or 13:00 for the minimap's clock. Requested by Kriiks.
- Unread lore notes are drawn bigger on an amber ring so they stand out, and Flame and Night
  Sanctums you've visited get a check mark (hidden with Completed Places: HIDE). Requested by
  TheSefer.
- Minimap: another player 4 m or more above or below you gets the up or down caret.
- Reported bugs fixed:
  - The active quest's marker (like the Crucible) hid under Fog: EXPLORED (reported by
    HadToRegister).
  - Shift+F closed Find while typing a capital F, and typing it into the game's text boxes
    (like a pin's name) opened Find (reported by HadToRegister).
  - The Fog Dragon didn't show as beaten.

1.3.10
- Read lore notes and beaten bosses now show on a dedicated server too (they were only
  tracked in single player and when you host).
- On a dedicated server, chests looted before you were away come back on your map when you
  return, since the server kept running (before, only time in that world counted).
- A chest lit by G G and by Find or Quick Deposit at the same time no longer stays lit.

1.3.9
- Reported bug fixed: a locked chest wasn't marked looted after you picked its lock and looted
  it (reported by izisot).

1.3.8
- Reported bug fixed: the quest circle for your first base stayed on the minimap (and in Find)
  after that quest was done (reported by izisot).

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

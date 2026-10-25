#pragma once
// The mod's own text in every language Enshrouded supports. The language follows the game (Steam's
// game language), or Language= in minimap_config.ini [General] (auto, english, german, ...).
// Marker and region names that come from the game are already in the game's language.

#include <cstddef>
#include <string>

namespace umm_lang {

enum class Lang : int {
    English, German, French, Italian, Spanish, Brazilian, Polish, Russian, Ukrainian, Turkish,
    Japanese, Korean, SChinese, TChinese, Thai,
    Count
};

enum class Str : int {
    MapKeyTitle, Find, FindTitle, Cancel, ShowAll, HideAll,
    Fog, FogExplored, FogHybrid, FogRevealAll, On, Off,
    CatFastTravel, CatShroud, CatWorld, CatQuests, CatCustomPins,
    AncientSpire, FlameAltarHome, FlameSanctum, NightSanctum, GemForge, AncientObelisk, WaterSource,
    FastTravelPoint, ElixirWell, CatacombsTomb, AncientVault, ShroudLair, HollowHalls, HiddenTomb,
    GoldChest, SilverChest, TownSettlement, Farmstead, SurvivorCamp, MineQuarry, CavePassage,
    EnemyEncampment, BalefireGarrison, MainQuest, SideQuest, CompletedQuests, LoreRumor, PlayerGrave, OtherPlayers,
    GoldSarcophagus, SilverSarcophagus, ChestLocked, CustomPin, MapLocation, OtherPlayer,
    TypeToSearch, MatchesFmt, HiddenFmt, FiltersFmt, AllMatchingFmt, AllNamedFmt, ZeroVisibleFmt,
    LockedTag, HiddenTag,
    Day, Dawn, Dusk, Night, ZoomFmt, Terrain, CompassN, CompassS, CompassW, CompassE,
    Pins, SettingsTitle, PinSize, ShowMinimap, MinimapTerrain, ClockIcon, SunMoon, Weather,
    LootedChests, Hide, GreyedOut, WeatherClear, WeatherRain, WeatherStorm, WeatherSnow, WeatherFog,
    HintClearChest, HintToggleKey, HintFind, CtrlClick, CatMining, CatMaterials, CatPlants, CatBait, CatTrees,
    MinimapBorder, BorderOrnate, BorderThin, MinimapInfoBar,
    FilterLocks, RemoveAllLocks, HoldForLocked,
    Count
};

// Picks the language once (config override, then Steam, then English). Safe to call every frame.
void EnsureLanguage(const char* configPath);
bool Decided();
// True when the language came from the game (Steam), not from Language= in the ini.
bool FollowsGame();
// True when the game's own text (item names) is in the mod's language: it follows the game, or the ini names
// the game's language.
bool GameTextInCurrentLanguage();
Lang Current();
const char* SteamCode(Lang lang);

// The text for the current language (English when a translation is missing).
const char* T(Str s);

// A resource row's name (ore_deposits.h kOreLabels order) in the current language.
size_t ResourceNameCount();
const char* ResourceName(size_t index);

// "1 match" / "12 matches" in English, "Treffer: 12" style elsewhere (no plural rules needed).
std::string FormatMatches(size_t count);

// English shows 12-hour time with AM/PM; every other language 24-hour.
bool Uses12HourClock();

// Japanese, Korean, Chinese and Thai glyphs stand taller than Latin ones: the panel's rows get more height.
bool TallScript();

// Approximate width of a label at the game's small text size, in UI units (calibrated against
// in-game measurements of the English labels; CJK and Thai estimated from the same em size).
float TextWidth(const char* utf8);

// Cuts a UTF-8 string to at most maxColumns (CJK characters count as two) without splitting a
// character, adding ".." when it was cut.
std::string TruncateColumns(const std::string& utf8, size_t maxColumns);
size_t Columns(const std::string& utf8);

// Lower-cases any script (Latin, Cyrillic, Greek...) for search; CJK and Thai pass through.
std::string ToLowerUtf8(const std::string& utf8);

} // namespace umm_lang

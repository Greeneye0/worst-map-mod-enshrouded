#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include <ctime>
#include <windows.h>
#include <psapi.h>
#include <xinput.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <utility>
#include <atomic>
#include <thread>
#include <wincodec.h>
#include <memory>
#include <mutex>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <vector>

#include "map_key_panel.h"
#include "input_binds.h"
#include "game_bindings.h"
#include "game_hints.h"
#include "async_io.h" // the saves are written off the game's thread
#include "map_control.h"
#include "hd_pool_policy.h"
#include "map_natives.h"
#include "ui_mesh.h" // smooth chest silhouettes (shared with Find and Quick Deposit)
#include "player_markers.h"
#include "filter_notice.h"
#include "chest_spawns.h"
#include "chest_halls.h"
#include "chest_loot_ids.h"
#include "ore_deposits.h"
#include "creature_templates.h"
#include <iterator>
#include <limits>
#include "ore_shading.h" // GG's vein surfaces (vein_surface.inl)
#include "ore_upload.h"
#include "ore_shader_bytes.h"
#include "knowledge_legacy.h"
#include "regions.h"
#include "levels.h"
#include "sanctum_spots.h"
#include "minimap_art.h"
#include "scan.h"
#include "greeneye.h"
#include "jumppatch.h"
#include <intrin.h>
#include "find_match.h"
#include "lang.h"

namespace unlock_map_markers {
namespace {

// Dev builds: the time each per-frame part takes, averaged per frame and logged every 5 s (UMM_PERF(Part) at the top of
// a scope times the rest of it). Release builds: nothing.
#ifdef UMM_DEV_BRIDGE
enum PerfPart { PerfHud, PerfLoot, PerfGlowDraw, PerfMinimap, PerfMmTerrain, PerfMmMarkers, PerfMmChests, PerfMmOres, PerfMapKey, PerfPreSync, PerfIconOverride, PerfGlowNext, PerfPlate, PerfMarkerType, PerfCount };
constexpr const char* kPerfNames[PerfCount] = {"hud+minimap", "loot tracking", "glow draw", "minimap", "mm terrain", "mm markers", "mm chests", "mm resources", "map key+overlay", "map pre-sync", "icon override",
                                               "glow next", "plate", "marker type"};
std::atomic<uint64_t> g_perfTicks[PerfCount]{}, g_perfCalls[PerfCount]{}, g_perfMax[PerfCount]{};
void PerfAdd(PerfPart part, uint64_t ticks) {
    g_perfTicks[part].fetch_add(ticks, std::memory_order_relaxed);
    g_perfCalls[part].fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = g_perfMax[part].load(std::memory_order_relaxed);
    while (ticks > prev && !g_perfMax[part].compare_exchange_weak(prev, ticks, std::memory_order_relaxed)) {
    }
}
// Section times inside one function: UMM_LAP_START, then UMM_LAP(Part) at the end of each section
struct PerfLapper {
    LARGE_INTEGER last;
    PerfLapper() { QueryPerformanceCounter(&last); }
    void Mark(PerfPart part) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        PerfAdd(part, static_cast<uint64_t>(now.QuadPart - last.QuadPart));
        last = now;
    }
};
struct PerfScope {
    PerfPart part;
    LARGE_INTEGER start;
    explicit PerfScope(PerfPart p) : part(p) { QueryPerformanceCounter(&start); }
    ~PerfScope() {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        PerfAdd(part, static_cast<uint64_t>(end.QuadPart - start.QuadPart));
    }
};
#define UMM_PERF(part) PerfScope umm_perf_scope_(part)
#define UMM_LAP_START PerfLapper umm_perf_lap_
#define UMM_LAP(part) umm_perf_lap_.Mark(part)
#else
#define UMM_PERF(part) ((void)0)
#define UMM_LAP_START ((void)0)
#define UMM_LAP(part) ((void)0)
#endif

constexpr uint32_t kGoldChestTypeHash = 0x601dcbe5u;
constexpr uint32_t kSilverChestTypeHash = 0x511fcbe5u;
constexpr uint32_t kWoodChestTypeHash = 0x711fcbe5u; // the mod's own row and marker type, like the two above
constexpr uint32_t kSpoilsTypeHash = 0x711fcbe6u;    // Dungeon Spoils: the Hollow Halls' end-room chests (the mod's own row)
constexpr uint32_t kOtherPlayerTypeHash = 0x26d18a06u;
constexpr uint32_t kTownPortalTypeHash = 0x7b0e7a11u; // the mod's own row (no game marker type): Town Portal's portals
constexpr uint32_t kCompletedQuestTypeHash = 0x9c4e7b12u;
constexpr uint32_t kLockedFastTravelTypeHash = 0x1095684fu;
constexpr uint32_t kChestMarkerUidPrefix = 0x70000000u;
constexpr uint32_t kModIconMarkerUidPrefix = 0x7FFE0000u; // the stand-in marker a mod icon's click menu is opened on

// Mining: one made-up marker type per ore. The game never sees these; the mod draws the deposits itself
// (7452 of them, far more than the marker arrays the chests borrow can hold).
constexpr uint32_t kOreTypeHashBase = 0x0e5a0000u;
constexpr size_t kOreCount = static_cast<size_t>(Ore::Count);
constexpr uint8_t kMiningCategory = 5;    // ores
constexpr uint8_t kMaterialsCategory = 6; // farm soil, bones, tar, archaic essence...
constexpr uint8_t kPlantsCategory = 7;    // harvestable plants
constexpr uint8_t kBaitCategory = 8;      // fishing bait critters
constexpr uint8_t kTreesCategory = 9;     // tree areas (coloured, no icons)
constexpr uint8_t kCreaturesCategory = 10; // every creature's spawn points
constexpr uint8_t kGroundCategory = 11;    // regional rock and sand (coloured areas, ground_areas.inl)
bool IsResourceCategory(uint8_t c) { return c >= kMiningCategory && c <= kGroundCategory; }
static_assert(sizeof(kOreCategory) / sizeof(kOreCategory[0]) == static_cast<size_t>(Ore::Count), "one category per row");
static_assert(sizeof(kOreAreaColors) / sizeof(kOreAreaColors[0]) == static_cast<size_t>(Ore::Count), "one colour per row");
static_assert(sizeof(kOreLabels) / sizeof(kOreLabels[0]) == kOreCount, "one label per ore");
static_assert(sizeof(kOreIcons) / sizeof(kOreIcons[0]) == kOreCount, "one icon per ore");
static_assert(sizeof(kOreNameLocaIds) / sizeof(kOreNameLocaIds[0]) == kOreCount, "one name per ore");
bool IsOreHash(uint32_t h) { return h >= kOreTypeHashBase && h < kOreTypeHashBase + kOreCount; }
size_t OreIndexOf(uint32_t h) { return h - kOreTypeHashBase; }
uint32_t OreHash(size_t ore) { return kOreTypeHashBase + static_cast<uint32_t>(ore); }
bool g_oreEnabled[kOreCount]{};
const char* OreName(size_t ore);
int KnowledgeRemaining(size_t ore); // knowledge.inl
namespace chest_glow { void UpdateKey(uint8_t* client, bool appFocused); void DrawMarkers(uint8_t* client); } // chest_glow.inl
bool IsKnowledgeDeposit(size_t d);
const char* KnowledgeSpotName(size_t d);
bool ReadLocaText(uint32_t locaId, char* out, size_t cap);

// A kStaticItems item's name in the game's language (its markup dropped: "<keyitem>Kettle</keyitem>"), "" if none
const char* StaticItemName(size_t i) {
    static char names[kStaticItemCount][80]{};
    static uint8_t state[kStaticItemCount]{}; // 0 not read yet, 1 have it, 2 none
    static int builtFor = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (builtFor != lang) {
        std::memset(state, 0, sizeof(state));
        builtFor = lang;
    }
    if (i >= kStaticItemCount) return "";
    if (state[i] == 0) {
        state[i] = ReadLocaText(kStaticItems[i].nameLoca, names[i], sizeof(names[i])) ? 1 : 2;
        char* out = names[i];
        for (const char* in = names[i]; *in; ++in) {
            if (*in == '<') {
                if (const char* close = std::strchr(in, '>')) {
                    in = close;
                    continue;
                }
            }
            *out++ = *in;
        }
        *out = 0;
    }
    return state[i] == 1 ? names[i] : "";
}
void UpdateKnowledge();
void UpdateManualBosses(uint8_t* client);
void NoteBossKills(uint8_t* client);
namespace boss_live { void Update(uint8_t* client); } // knowledge.inl
void NoteVisitedPlaces(uint8_t* client);
bool PlaceVisited(int32_t x, int32_t z);
bool PlaceMarkedDone(int32_t x, int32_t z);              // knowledge.inl
bool TogglePlaceDone(int32_t x, int32_t z, bool& nowDone); // knowledge.inl
std::string BossTimersText();
void LoadBossTimers(const char* text);
extern std::unordered_map<uint16_t, double> g_bossDead;
struct BuildZoneBox;
void AdvanceBossTimers(double dtSec, double respawnSec, const BuildZoneBox* zones, size_t numZones, bool allAway);
bool ToggleManualBossNear(float wx, float wz, float maxRadiusMeters, float shiftMeters);
bool ToggleUniqueFoundNear(float wx, float wz, float maxRadiusMeters, float shiftMeters);
bool ToggleLoreReadNear(float wx, float wz, float maxRadiusMeters, float shiftMeters, float chestRadiusMeters);
int StackSide(size_t d); // knowledge.inl: -1 / +1 for icons drawn aside, off the boss or chest they stand on
// Where the World Map shows the game's lore "?" (a place with an unread rumor, or a rumor pin), from the last marker
// pass: the mod's own icons there are drawn aside so they don't cover it (StackSide)
std::vector<std::pair<int32_t, int32_t>> g_rumorSpots;
const void* DepositIcon(size_t d); // knowledge.inl: a unique item's own icon, else its row's
bool SanctumVisitedNear(float x, float z); // knowledge.inl

// A place marked done by hand (Ctrl + click a town, a camp, a sanctum...): a check mark like a completed place, and
// like one it follows Completed Places (Show / Hide)
extern bool g_hideCompletedPlaces;
uint32_t NormalizeFilterTypeHash(uint32_t rawHash);
constexpr uint32_t kElixirWellRowHash = 0xffe5e444u; // Elixir Well / Root: the game checks them off itself
bool IsHandDoneMarker(const uint8_t* m, uint32_t origHash) {
    return origHash != 0xafce0c5fu && m[0x34] != 3 && NormalizeFilterTypeHash(origHash) != kElixirWellRowHash &&
           PlaceMarkedDone(*reinterpret_cast<const int32_t*>(m + 0x14), *reinterpret_cast<const int32_t*>(m + 0x24));
}
bool HandDoneHidden(const uint8_t* m, uint32_t origHash) { return g_hideCompletedPlaces && IsHandDoneMarker(m, origHash); }

// A Flame or Night Sanctum marker this character has been to: shown with a check mark like a completed place.
bool IsVisitedSanctumMarker(const uint8_t* m, uint32_t origHash) {
    if (origHash != 0x7b4c9e16u && origHash != 0x6df85109u) return false;
    return SanctumVisitedNear(static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)),
                              static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24)));
}
extern int g_worldHostState; // knowledge.inl: 1 hosted here, 0 on a server, -1 not known yet
extern bool g_worldHostRecheck;
extern bool g_knowledgeValid;
extern uint32_t g_knowledgeReplicated;
extern uint64_t g_knowledgeSlotKeys;

int OreDepositCount(size_t ore) {
    static int counts[kOreCount]{};
    static bool built = false;
    if (!built) {
        for (size_t d = 0; d < kOreDepositCount; ++d) ++counts[kOreDeposits[d].ore];
        built = true;
    }
    return ore < kOreCount ? counts[ore] : 0;
}

bool AnyOreEnabled() {
    for (bool on : g_oreEnabled) if (on) return true;
    return false;
}

// Marker slots whose original type (m+0x3c) and quest id (m+0x4c) the mod has saved, with the slot's uid at
// the time. The game gives a slot a new uid whenever it repopulates it, so a uid mismatch means the saved values
// belong to an older marker. (Nothing is stamped into the slot: m+0x68..0x7f is the marker's name text, and an
// earlier stamp at m+0x78 overwrote the name's hash, so every chest showed one chest's name.)
// The game can also change a marker in place, keeping its uid: finishing a quest turns its pin into another
// type. So the type (m+0x30) and quest id (m+0x08) the slot held after the mod's last pass are kept too, and a
// slot that no longer holds them was changed by the game: its saved values are stale (a finished quest stayed a
// quest "!").
struct SavedSlot {
    uint32_t uid;
    uint32_t hash; // m+0x30 after the mod's last pass
    uint32_t id08; // m+0x08 after the mod's last pass
};
std::unordered_map<const uint8_t*, SavedSlot> g_savedSlotUid;

bool IsSlotSaved(const uint8_t* m) {
    auto it = g_savedSlotUid.find(m);
    return it != g_savedSlotUid.end() && it->second.uid == *reinterpret_cast<const uint32_t*>(m) &&
           it->second.hash == *reinterpret_cast<const uint32_t*>(m + 0x30) &&
           it->second.id08 == *reinterpret_cast<const uint32_t*>(m + 0x08);
}

// After the mod has written a saved slot's type or quest id: what it holds now is the mod's, not a game change.
void NoteSlotPass(const uint8_t* m) {
    auto it = g_savedSlotUid.find(m);
    if (it == g_savedSlotUid.end() || it->second.uid != *reinterpret_cast<const uint32_t*>(m)) return;
    it->second.hash = *reinterpret_cast<const uint32_t*>(m + 0x30);
    it->second.id08 = *reinterpret_cast<const uint32_t*>(m + 0x08);
}

// Places the game itself never puts on the map, by their name: Revelwood's Abandoned Farms and Abandoned Windmills.
// Their visibility never changes from 0, so the unlock patches showed them grayed for good. Reported by BlaineCraner.
bool GameNeverShowsPlace(uint32_t nameId) {
    return nameId == 0x94eb5847u || nameId == 0x55641bdbu;
}

enum class PinRevealMode : int {
    ExploredOnly = 0, // Only show pins in areas visited by the character
    Hybrid       = 1, // Show unexplored pins greyed out, explored pins in full color
    RevealAll    = 2  // Show all pins in full color
};

constexpr float kMarkerScaleLevels[] = {0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f};
constexpr const char* kMarkerScaleLabels[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "1.75x", "2.0x"};
constexpr int kMarkerScaleCount = static_cast<int>(sizeof(kMarkerScaleLevels) / sizeof(kMarkerScaleLevels[0]));

bool g_goldChestEnabled = true;
bool g_silverChestEnabled = true;
bool g_woodChestEnabled = false; // off at first: 1562 more icons on a map that had none
bool g_spoilsEnabled = true;      // the Hollow Halls' end-room chests (Dungeon Spoils)

// A kChestSpawns index: gold chests, then silver, then wooden (kWoodChestFirst on), then the Hollow Halls' end-room
// sarcophagi (kEndChestFirst on), which count as gold
bool IsGoldChest(size_t c) {
    return c < kGoldChestCount || c >= kEndChestFirst;
}
bool IsWoodChest(size_t c) {
    return c >= kWoodChestFirst && c < kEndChestFirst;
}
bool ChestRowEnabled(size_t c) {
    if (c >= kEndChestFirst) return g_spoilsEnabled;
    return IsGoldChest(c) ? g_goldChestEnabled : !IsWoodChest(c) ? g_silverChestEnabled : g_woodChestEnabled;
}
uint32_t ChestTypeHash(size_t c) {
    if (c >= kEndChestFirst) return kSpoilsTypeHash;
    return IsGoldChest(c) ? kGoldChestTypeHash : !IsWoodChest(c) ? kSilverChestTypeHash : kWoodChestTypeHash;
}
bool AnyChestRowEnabled() {
    return g_goldChestEnabled || g_silverChestEnabled || g_woodChestEnabled || g_spoilsEnabled;
}
bool g_otherPlayersEnabled = true;
bool g_townPortalsEnabled = true; // Fast Travel & Altars > Town Portals (town_portals.inl)
size_t TownPortalCount();
bool g_minimapShowTerrain = true;
bool g_minimapThinBorder = false; // a thin line instead of the ornate frame
bool g_minimapHeadingUp = false;  // Settings > Minimap Rotation: the map turns so the way you face is up (HeadingUp=1)
// This frame's minimap turn (identity when north is up): MmRotate takes a world offset or direction (x east, z north)
// to the minimap's view (x right, z up), MmUnrotate back
float g_mmRotC = 1.0f, g_mmRotS = 0.0f;
inline void MmRotate(float& dx, float& dz) {
    const float x = dx * g_mmRotC + dz * g_mmRotS;
    dz = dz * g_mmRotC - dx * g_mmRotS;
    dx = x;
}
inline void MmUnrotate(float& dx, float& dz) {
    const float x = dx * g_mmRotC - dz * g_mmRotS;
    dz = dz * g_mmRotC + dx * g_mmRotS;
    dx = x;
}
bool g_minimapShowPosition = false; // Settings > Advanced: your X, Y (height), Z under the minimap
bool g_regionFilterEnabled = false; // Settings > Advanced: the Map Key's Region row (regions.inl)
bool g_levelAreas = false;          // Settings > Advanced: the World Map in level areas (levels.inl)
int g_levelTintPct = 50;            // its tint's strength, 10 to 80 % (requested by dremen); borders and badges stay
bool g_regionNames = true;          // Settings > Advanced: the regions' names on the World Map (regions.inl)
float g_overlayRootRect[4]{}; // the Map Key overlay's screen rect, last frame
bool g_minimapInfoBar = true;     // the tray under the minimap (time, weather, zoom); off puts the cog on the map
bool g_minimapVisible = true;
bool g_minimapClockWeatherMode = false; // false = Sun/Moon, true = Weather Status
int g_lootedChestMode = 0;              // 0 = Hide, 1 = Greyed Out
bool g_hideCompletedPlaces = false;     // places the game shows with a check mark (cleared roots, used obelisks)
int g_clockFormat = -1;                 // -1 by language, 0 12-hour (1:00 PM), 1 24-hour (13:00)
int g_customMarkerScaleIdx = 2;         // Default 1.0x
PinRevealMode g_pinRevealMode = PinRevealMode::RevealAll;
bool g_settingsOpen = false;
bool g_minimapSettingsOpen = false;
bool g_bridgeRefreshMap = false;
#ifdef UMM_DEV_BRIDGE
bool g_dumpMarkersRequested = false; // the next world-map marker pass writes markers_dump.txt
int DumpMarkersToFile(uint8_t* client); // map_native_filters.inl
#endif
bool g_bridgeRecenterMap = false;
float g_bridgeZoomSet = 0.0f; // dev: the map op zoomset
float g_devDot[2] = {-1.0f, -1.0f}; // dev: the map op dot
float g_devNear[2] = {-1.0f, -1.0f}; // dev: the map op near
#ifdef UMM_DEV_BRIDGE
extern float g_devNearPlate[2];
#endif
float g_bridgeCenterX = -1.0f, g_bridgeCenterZ = -1.0f; // the bridge's centerat: the World Map centred there next frame
float g_chestRespawnMinutes = 30.0f;
bool g_chestsStayLooted = false; // [Chests] RespawnMinutes=0: a looted chest stays marked looted, refilled or not
float g_chestResetRadiusMeters = 0.0f; // [Chests] ResetRadius: a player this close also holds the respawn timer

// The game's own respawn rule (server loop FUN_140a1ebb0): the world is cut into 256 m square scene chunks. One stays
// loaded while any player is within 242 m of one of its four corners, and unloads once every player is more than
// 258 m from all four. 30 minutes of server time after it unloads, a chunk with changes is reset: its looted chests
// refill and its bosses come back.
constexpr int kSceneChunkSide = 40; // 10240 m / 256 m
constexpr float kSceneChunkMeters = 256.0f;
bool g_sceneChunkLoaded[kSceneChunkSide * kSceneChunkSide]{};

void UpdateSceneChunks(const float (*players)[2], size_t numPlayers) {
    for (int cz = 0; cz < kSceneChunkSide; ++cz) {
        for (int cx = 0; cx < kSceneChunkSide; ++cx) {
            float nearest = 1e12f; // squared distance to the nearest corner of the nearest player
            for (size_t i = 0; i < numPlayers; ++i) {
                for (int k = 0; k < 4; ++k) {
                    const float dx = players[i][0] - (cx + (k & 1)) * kSceneChunkMeters;
                    const float dz = players[i][1] - (cz + (k >> 1)) * kSceneChunkMeters;
                    nearest = std::min(nearest, dx * dx + dz * dz);
                }
            }
            bool& loaded = g_sceneChunkLoaded[cz * kSceneChunkSide + cx];
            loaded = nearest < 58564.0f || (loaded && nearest <= 66564.0f);
        }
    }
}

bool SceneChunkLoadedAt(float x, float z) {
    const int cx = static_cast<int>(x / kSceneChunkMeters), cz = static_cast<int>(z / kSceneChunkMeters);
    if (cx < 0 || cz < 0 || cx >= kSceneChunkSide || cz >= kSceneChunkSide) return false;
    return g_sceneChunkLoaded[cz * kSceneChunkSide + cx];
}

struct ChestLootState {
    bool looted;
    double elapsedOutOfBuildZoneSec;
    bool everLooted; // looted at least once in this world: keeps its check mark after it refills
};
ChestLootState g_chestLootStates[kTotalChestCount]{};
bool g_chestLootDirty = false;
uint64_t g_lastChestUpdateTick = 0;
bool g_interactKeyWasDown = false;
bool g_worldMapChestClickWasDown = false;

#pragma pack(push, 1)
struct CustomMapMarkerType {
    uint8_t pad00[0x10];
    uint32_t markerId;               // +0x10
    uint8_t sortingCategory;         // +0x14
    uint8_t sortingWeight;           // +0x15
    uint8_t isHighlightable;         // +0x16
    uint8_t canBeFiltered;           // +0x17
    uint8_t isFastTravelDestination; // +0x18
    uint8_t isCustomMarkerType;      // +0x19
    uint8_t pad1A[2];                // +0x1a
    uint8_t icon[16];                // +0x1c
    uint8_t mutedIcon[16];           // +0x2c
    float iconDisplaySize;           // +0x3c
    uint8_t showAboveFogOfWar;       // +0x40
    uint8_t showLabel;               // +0x41
    uint8_t showRange;               // +0x42
    uint8_t minimizeAboveZoomStep;   // +0x43
    uint8_t hideAboveZoomStep;       // +0x44
    uint8_t pad45[3];                // +0x45
    uint32_t minimizedColor;         // +0x48
    uint8_t scaleWithZoom;           // +0x4c
    uint8_t pad4D[3];                // +0x4d
};
#pragma pack(pop)
static_assert(sizeof(CustomMapMarkerType) == 0x50, "CustomMapMarkerType must be 0x50 bytes");

CustomMapMarkerType g_goldChestMarkerType = {
    {},
    kGoldChestTypeHash,
    3, 0, 1, 0, 0, 1,
    {},
    {0xc9, 0xdb, 0xcb, 0xda, 0x12, 0x0a, 0xc2, 0x4f, 0xa9, 0xda, 0xfb, 0xcb, 0x29, 0x04, 0x10, 0x29},
    {0xc9, 0xdb, 0xcb, 0xda, 0x12, 0x0a, 0xc2, 0x4f, 0xa9, 0xda, 0xfb, 0xcb, 0x29, 0x04, 0x10, 0x29},
    23.0f,
    1, 1, 0, 2, 10,
    {},
    0xff38c8ffu,
    1,
    {}
};

CustomMapMarkerType g_silverChestMarkerType = {
    {},
    kSilverChestTypeHash,
    3, 0, 1, 0, 0, 1,
    {},
    {0xc9, 0xdb, 0xcb, 0xda, 0x12, 0x0a, 0xc2, 0x4f, 0xa9, 0xda, 0xfb, 0xcb, 0x29, 0x04, 0x10, 0x29},
    {0xc9, 0xdb, 0xcb, 0xda, 0x12, 0x0a, 0xc2, 0x4f, 0xa9, 0xda, 0xfb, 0xcb, 0x29, 0x04, 0x10, 0x29},
    21.0f,
    1, 1, 0, 2, 10,
    {},
    0xffe8e4dcu,
    1,
    {}
};

// Wooden chests are drawn by the mod (the game's marker arrays hold 1024 each, full with gold and silver); the type
// gives the Map Key row its icon
CustomMapMarkerType g_woodChestMarkerType = {
    {},
    kWoodChestTypeHash,
    3, 0, 1, 0, 0, 1,
    {},
    {0xc9, 0xdb, 0xcb, 0xda, 0x12, 0x0a, 0xc2, 0x4f, 0xa9, 0xda, 0xfb, 0xcb, 0x29, 0x04, 0x10, 0x29},
    {0xc9, 0xdb, 0xcb, 0xda, 0x12, 0x0a, 0xc2, 0x4f, 0xa9, 0xda, 0xfb, 0xcb, 0x29, 0x04, 0x10, 0x29},
    19.0f,
    1, 1, 0, 2, 10,
    {},
    0xff4a86c8u,
    1,
    {}
};
constexpr uint32_t kWoodChestTint = 0xff4a86c8u, kWoodChestDimTint = 0xff5a7088u; // 0xAABBGGRR: oak brown

CustomMapMarkerType g_lockedFastTravelMarkerType = {
    {0x27, 0x59, 0x2e, 0x0c, 0x65, 0xc2, 0xad, 0x4b, 0xba, 0xd7, 0x5f, 0xcb, 0xc5, 0xe2, 0x58, 0xec},
    kLockedFastTravelTypeHash,
    1, 0, 1, 0, 0, 0,
    {},
    {0xbb, 0x61, 0x43, 0x44, 0x91, 0x59, 0xdb, 0x42, 0xb1, 0xcd, 0x60, 0x75, 0x57, 0xbc, 0xcc, 0xa8},
    {0xbb, 0x61, 0x43, 0x44, 0x91, 0x59, 0xdb, 0x42, 0xb1, 0xcd, 0x60, 0x75, 0x57, 0xbc, 0xcc, 0xa8},
    70.0f,
    1, 0, 0, 10, 10,
    {},
    0xff5fd4eeu,
    0,
    {}
};


struct UiText {
    const char* utf8;
    size_t len;
    uint64_t hash;
};

UiText MakeUiText(const char* s) {
    if (!s) s = "";
    size_t len = std::strlen(s);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(s[i])));
        h *= 0x100000001b3ull;
    }
    return UiText{s, len, h};
}

using BeginWidgetFn = void (*)(void* widget, uint64_t parent);
using EndWidgetFn = void (*)(void* widget);
using SetSizeFn = void (*)(void* handle, uint64_t packedSize);
using UiCtxFn = void* (*)();
using ImageFn = uint32_t* (*)(void* ctx, void* tmp, const void* ref);
using DrawImageFn = void (*)(void* widget, const float* rect, const uint32_t* image, const uint64_t* colour);
using BeginLabelFn = void (*)(uint8_t* label, const UiText* text, const uint8_t* style, uint8_t flag);
using EndLabelFn = void (*)(uint8_t* label);
using BeginLayerFn = void* (*)(void* layer, const UiText* name, uint8_t fullScreen, uint32_t order, int32_t a, uint32_t b);
using EndLayerFn = void (*)(void* layer);
using NamedWidgetFn = void* (*)(void* widget, const UiText* name);
using OpaqueFn = void* (*)(void* widget);
using DrawMapFilterMenuFn = void (*)(void* mapState);
using SetTypeFilterFn = bool (*)(void* g, uint32_t typeHash, uint8_t disableFlag);
using ApplyTypeFilterDirectFn = void (*)(void* markerTypeMgr, uint32_t typeHash, uint8_t disableFlag);
using GetMarkerTypeFn = uint8_t* (*)(uint32_t typeHash, void* markerTypeMgr);
using GetMarkerTitleTextFn = UiText* (*)(UiText* outText, const void* uiMarker);
using LocaIdToTextFn = UiText* (*)(UiText* outText, uint32_t locaId);
using GuidToLocaIdFn = uint32_t (*)(const void* guid16);
using CheckWidgetButtonFn = uint32_t* (*)(void* widgetRaw, uint32_t* outState, uint8_t buttonId, uint8_t subId, uint8_t mask);
using CenterMapOnPosFn = void (*)(void* mapView, const float* targetCanvasPos2D, uint8_t immediate);
using UpdateMapViewportFn = void (*)(void* mapView, void* outAction, uint8_t suppressInput);
using WorldMapInputFn = void (*)(void* mapRootWidget, uint8_t* mapState);
using DialogInitFn = void (*)(void* dialogPart);
using TextDialogFn = int (*)(void* textDialog);

using NineSliceFn = void (*)(void* widget, const float* rect, const void* imageRef, const float* borders, uint64_t colour);
using DrawInGameMapPulseFn = void (*)(uint64_t packedPosXY, float baseSize, float expandSize);
using GetQuestByMarkerIdFn = void* (*)(uint32_t id08);
using IsLoreRumorMarkerIdFn = bool (*)(uint32_t id08);
using ResolveAssetSpecFn = uint32_t (*)(void* assetRepo, const void* assetSpec32, uint8_t loadSync);
using DrawImageSubUvFn = void (*)(void* widget, const float* rect, uint32_t imageId, uint64_t packedUv0, uint64_t packedUv1, uint64_t packedColor);
using DrawWorldMapCanvasFn = void (*)(void* canvasWidget, void* mapView);
using GetUiDrawListFn = void* (*)(void* uiLayer);
using PushUiClipRectFn = void (*)(void* uiCtx, const float* rect4, uint8_t intersect);
using PopUiClipRectFn = void (*)(void* uiCtx);
using PushUiOpacityFn = void (*)(void* uiCtx, float opacity);
using PopUiOpacityFn = void (*)(void* uiCtx);
using DrawWorldMapMarkerIconFn = void (*)(void* mapState, uint8_t* m, uint8_t* drawParams);
using DrawWorldMapMarkerPlateFn = void (*)(void* widget, uint8_t* entry, const float* rect4);

using FindCreateDialogFn = uint8_t* (*)(const char* title, const char* okLabel);
using FindResetDialogFn = void (*)(uint8_t* dlg);
using FindGetQueryStrFn = const char* (*)(uint8_t* dlg, size_t* outLen);
using FindEnsureFocusFn = void (*)();
using FindRunDialogFrameFn = int (*)(uint8_t* dlg);
using FindGetUiCursorPosFn = bool (*)(float* outX, float* outY);
using FindHitTestRowFn = int (*)(uint8_t* dlg, int visCount, float cursorX, float cursorY);
using FindCloseDialogFn = void (*)();
using FindSetTextFieldValueFn = void (*)(uint8_t* dlg, const std::string* str);

LogFn g_log = nullptr;
void** g_uiRoot = nullptr;
BeginWidgetFn g_beginWidget = nullptr;
EndWidgetFn g_endWidget = nullptr;
SetSizeFn g_setSize = nullptr;
UiCtxFn g_uiCtx = nullptr;
ImageFn g_image = nullptr;
DrawImageFn g_drawImage = nullptr;
BeginLabelFn g_beginLabel = nullptr;
EndLabelFn g_endLabel = nullptr;
BeginLayerFn g_beginLayer = nullptr;
EndLayerFn g_endLayer = nullptr;
NamedWidgetFn g_namedWidget = nullptr;
OpaqueFn g_opaque = nullptr;
SetTypeFilterFn g_setTypeFilter = nullptr;
ApplyTypeFilterDirectFn g_applyTypeFilterDirect = nullptr;
GetMarkerTypeFn g_getMarkerType = nullptr;
GetMarkerTitleTextFn g_getMarkerTitleText = nullptr;
LocaIdToTextFn g_locaIdToText = nullptr;
GuidToLocaIdFn g_guidToLocaId = nullptr;
CheckWidgetButtonFn g_checkWidgetButton = nullptr;
CenterMapOnPosFn g_centerMapOnPos = nullptr;
UpdateMapViewportFn g_updateMapViewport = nullptr;
DialogInitFn g_dialogInit = nullptr;
TextDialogFn g_textDialog = nullptr;
NineSliceFn g_nineSlice = nullptr;
DrawInGameMapPulseFn g_drawInGameMapPulse = nullptr;
GetQuestByMarkerIdFn g_getActiveQuestForMarkerId = nullptr;
GetQuestByMarkerIdFn g_getQuestByMarkerId = nullptr;
IsLoreRumorMarkerIdFn g_isLoreRumorMarkerId = nullptr;
ResolveAssetSpecFn g_resolveAssetSpec = nullptr;
DrawImageSubUvFn g_drawImageSubUv = nullptr;
DrawWorldMapCanvasFn g_drawWorldMapCanvas = nullptr;
GetUiDrawListFn g_getUiDrawList = nullptr;
PushUiClipRectFn g_pushUiClipRect = nullptr;
PopUiClipRectFn g_popUiClipRect = nullptr;
PushUiOpacityFn g_pushUiOpacity = nullptr;
PopUiOpacityFn g_popUiOpacity = nullptr;

bool g_findDllResolved = false;
FindCreateDialogFn g_findCreateDialog = nullptr;
FindResetDialogFn g_findResetDialog = nullptr;
FindGetQueryStrFn g_findGetQueryStr = nullptr;
FindEnsureFocusFn g_findEnsureFocus = nullptr;
FindRunDialogFrameFn g_findRunDialogFrame = nullptr;
FindGetUiCursorPosFn g_findGetUiCursorPos = nullptr;
FindHitTestRowFn g_findHitTestRow = nullptr;
FindCloseDialogFn g_findCloseDialog = nullptr;
FindSetTextFieldValueFn g_findSetTextFieldValue = nullptr;
volatile LONG* g_findWheelDeltaPtr = nullptr;
// Find's Exact match checkbox, drawn by Find in its dialog (exported by name from Find's next release; without it the
// switch still works by its keys)
using FindDialogCheckboxFn = void (*)(void* dlg, bool shown, bool on, const char* label, const char* keys);
using FindDialogCheckboxAtFn = bool (*)(void* dlg, float x, float y);
FindDialogCheckboxFn g_findDialogCheckbox = nullptr;
FindDialogCheckboxAtFn g_findDialogCheckboxAt = nullptr;

// Hooks go through the Greeneye Loader (scan.h InstallHook): it owns the patched bytes for the life
// of the process and swaps detours on a hot reload. Each one hooks whole instructions only.
uint8_t* g_hookTarget = nullptr;
uint8_t* g_getMarkerTypeHookTarget = nullptr;
constexpr uint8_t kCanonicalGetMarkerTypePrologue[13] = {
    0x85, 0xC9,             // test ecx, ecx
    0x74, 0x13,             // je +0x13 (xor eax, eax; ret)
    0x48, 0x8B, 0x42, 0x20, // mov rax, [rdx + 0x20]
    0x48, 0x85, 0xC0,       // test rax, rax
    0x74, 0x0A              // je +0x0A (xor eax, eax; ret)
};
// The hooked 13 bytes for the trampoline, their two short jumps rebuilt: on to the original's
// "mov edx, ecx; mov rcx, rax; jmp lookup" at +13, or "return null" when a je is taken.
constexpr uint8_t kGetMarkerTypeTrampolineCode[30] = {
    0x85, 0xC9,                         // test ecx, ecx
    0x74, 0x17,                         // je .null (+0x17)
    0x48, 0x8B, 0x42, 0x20,             // mov rax, [rdx + 0x20]
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x0E,                         // je .null (+0x0E)
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, // jmp [rip+0]
    0, 0, 0, 0, 0, 0, 0, 0,             //   fn + 13, filled in at install
    0x33, 0xC0,                         // .null: xor eax, eax
    0xC3                                // ret
};
constexpr uint8_t kCanonicalMapFilterPrologue[20] = {
    0x48, 0x8B, 0xC4,                               // mov rax, rsp
    0x55,                                           // push rbp
    0x53,                                           // push rbx
    0x56,                                           // push rsi
    0x57,                                           // push rdi
    0x41, 0x54,                                     // push r12
    0x41, 0x56,                                     // push r14
    0x41, 0x57,                                     // push r15
    0x48, 0x8D, 0xA8, 0x68, 0xF8, 0xFF, 0xFF        // lea rbp, [rax - 0x798] (only the first 13 bytes are checked)
};
constexpr uint8_t kCanonicalWorldMapInputPrologue[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,                   // mov [rsp+8], rbx
    0x55,                                           // push rbp
    0x56,                                           // push rsi
    0x57,                                           // push rdi
    0x41, 0x54,                                     // push r12
    0x41, 0x55,                                     // push r13
    0x41, 0x56,                                     // push r14
    0x41, 0x57                                      // push r15
};
constexpr uint8_t kCanonicalPlayStatePrologue[18] = {
    0x4C, 0x8B, 0xDC,                               // mov r11, rsp
    0x55,                                           // push rbp
    0x49, 0x8D, 0xAB, 0xE8, 0xFD, 0xFF, 0xFF,       // lea rbp, [r11 - 0x218]
    0x48, 0x81, 0xEC, 0x10, 0x03, 0x00, 0x00        // sub rsp, 0x310
};
constexpr uint8_t kCanonicalBuildWorldMapPlayersPrologue[15] = {
    0x40, 0x55,                                     // push rbp
    0x56,                                           // push rsi
    0x57,                                           // push rdi
    0x41, 0x54,                                     // push r12
    0x41, 0x55,                                     // push r13
    0x41, 0x57,                                     // push r15
    0x48, 0x8D, 0x6C, 0x24, 0xB8                    // lea rbp, [rsp - 0x48]
};
constexpr uint8_t kCanonicalDrawWorldMapMarkerIconPrologue[19] = {
    0x48, 0x8B, 0xC4,                               // mov rax, rsp
    0x55,                                           // push rbp
    0x53,                                           // push rbx
    0x48, 0x8D, 0xA8, 0x58, 0xFE, 0xFF, 0xFF,       // lea rbp, [rax - 0x1a8]
    0x48, 0x81, 0xEC, 0x98, 0x02, 0x00, 0x00        // sub rsp, 0x298
};
constexpr uint8_t kCanonicalDrawWorldMapMarkerPlatePrologue[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,                   // mov [rsp+8], rbx
    0x48, 0x89, 0x74, 0x24, 0x20,                   // mov [rsp+20h], rsi
    0x57,                                           // push rdi
    0x48, 0x83, 0xEC, 0x50                          // sub rsp, 50h
};
using DoPlayStateUiFn = void (*)();
using BuildWorldMapPlayersFn = void (*)(void* outVec, void* mapState);
DrawMapFilterMenuFn g_origDrawMapFilterMenu = nullptr;
bool g_modOff = false;             // Ctrl + Shift + F12: the whole mod switched off (SetModOff)
// Settings > Advanced > Vanilla World Map: the World Map as the game draws it, as players without the mod see it; the
// minimap and the rest stay on. g_worldMapModded is what is in force: the marker patches on and the mod's World Map
// hooks at work. It follows !g_modOff && !g_vanillaWorldMap once the patches could be switched (SwitchWorldMap).
bool g_vanillaWorldMap = false;
// Settings > Advanced > Dungeon Maps: inside a Hollow Hall, Ancient Vault or Spire the World Map and minimap show its
// own floor plan (halls.inl); off, they show the overworld as the game does
bool g_dungeonMaps = true;
// Settings > Advanced > Nearby Creatures Only: the creature rows' spawn points are left off the minimap, which shows the
// live creatures within 75 m instead (creature_live.inl), and G G lights them; the World Map keeps the spawn points
// ("creatures should still be visible on the main map", 2026-11-11)
bool g_nearCreatures = false;
bool CreatureSpawnHidden(size_t ore) { return g_nearCreatures && ore >= kCreaturesFirst && ore < kCreaturesFirst + kCreaturesCount; }
// ... and under it, Hide on World Map: the spawn points off the World Map (and Find) too
bool g_nearCreaturesHideMap = false;
// (unless Find is looking for them: the creature rows among its results show while it is open and for 60 s after,
// g_findShownOres, set in the World Map frame)
std::vector<uint8_t> g_findShownOres;
uint64_t g_findShownUntil = 0;
bool CreatureOffWorldMap(size_t ore) {
    if (!g_nearCreaturesHideMap || !CreatureSpawnHidden(ore)) return false;
    return !(GetTickCount64() < g_findShownUntil && ore < g_findShownOres.size() && g_findShownOres[ore]);
}
// Settings > Advanced > Vanilla Minimap, under it (only with Vanilla World Map on): the minimap shows what the game's World
// Map shows, the game's own markers and the players, and none of the mod's (chests, resources, lore, portals...)
bool g_vanillaMinimap = false;
bool VanillaMinimapData() { return g_vanillaWorldMap && g_vanillaMinimap; }
bool g_worldMapModded = true;
uint64_t g_mapMenuDrawnTick = 0; // GetTickCount64 when the World Map last drew its filter menu (map_hints.inl)
// The World Map is up: screen 5 from M, screen 4 when a controller opened the menu and its World tab is showing
bool MapScreen(uint8_t screen) {
    return screen == 5 || (screen == 4 && GetTickCount64() - g_mapMenuDrawnTick < 250);
}
uint8_t* g_lastMapState = nullptr; // the World Map's state, last seen by DrawMapKeyOverlay
GetMarkerTypeFn g_origGetMarkerType = nullptr;
uint8_t* g_worldMapInputHookTarget = nullptr;
WorldMapInputFn g_origWorldMapInput = nullptr;
uint8_t* g_playStateHookTarget = nullptr;
DoPlayStateUiFn g_origPlayStateUi = nullptr;
uint8_t* g_buildWorldMapPlayersHookTarget = nullptr;
BuildWorldMapPlayersFn g_origBuildWorldMapPlayers = nullptr;
uint8_t* g_drawWorldMapMarkerIconHookTarget = nullptr;
DrawWorldMapMarkerIconFn g_origDrawWorldMapMarkerIcon = nullptr;
uint8_t* g_drawWorldMapMarkerPlateHookTarget = nullptr;
DrawWorldMapMarkerPlateFn g_origDrawWorldMapMarkerPlate = nullptr;
bool g_hookInstalled = false;

// What outlives a hot reload (host->keep): the fog mode patched into the game's code.
KeptState* g_kept = nullptr;

enum class FogRevealMode : int {
    ExploredOnly = 0, // Vanilla: only explored areas visible (100% opaque fog)
    Hybrid       = 1, // Hybrid: map revealed, unexplored areas shaded with semi-transparent fog
    RevealAll    = 2  // Reveal All: entire map 100% revealed (0% fog)
};

FogRevealMode g_fogRevealMode = FogRevealMode::ExploredOnly;
bool g_fogModeFromHotReload = false; // the running mode came from the previous generation (else minimap_config.ini's)
uint8_t* g_fogColorSite = nullptr;
uint8_t* g_fogAlphaSite = nullptr;

void ApplyFogRevealMode(FogRevealMode mode) {
    g_fogRevealMode = mode;
    if (g_kept) {
        g_kept->fogMode = static_cast<int>(mode);
        g_kept->valid = 1;
    }
    if (!g_worldMapModded) mode = FogRevealMode::ExploredOnly; // the game's own fog; the choice is kept for later
    uint32_t colR = 0x3e2cacadu;
    uint32_t colG = 0x3e2cacadu;
    uint32_t colB = 0x3e2cacadu;
    uint32_t quadRgba = 0xffffffffu;
    uint32_t quadBright = 0x3f800000u;

    if (mode == FogRevealMode::Hybrid) {
        colR = 0x3d8f5c29u; // 0.07f
        colG = 0x3da3d70au; // 0.08f
        colB = 0x3dccccccu; // 0.10f
        quadRgba = 0x96ffffffu; // ~59% opacity shroud over unexplored areas; 0% over explored areas
    } else if (mode == FogRevealMode::RevealAll) {
        quadRgba = 0x00ffffffu; // 0% opacity -> skips drawing Fog of War quad completely
    }

    if (g_fogColorSite) {
        DWORD oldProt = 0;
        if (VirtualProtect(g_fogColorSite, 36, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memcpy(g_fogColorSite + 17, &colR, 4);
            std::memcpy(g_fogColorSite + 24, &colG, 4);
            std::memcpy(g_fogColorSite + 31, &colB, 4);
            FlushInstructionCache(GetCurrentProcess(), g_fogColorSite, 36);
            DWORD dummy = 0;
            VirtualProtect(g_fogColorSite, 36, oldProt, &dummy);
        }
    }
    if (g_fogAlphaSite) {
        DWORD oldProt = 0;
        if (VirtualProtect(g_fogAlphaSite, 24, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memcpy(g_fogAlphaSite + 6, &quadRgba, 4);
            std::memcpy(g_fogAlphaSite + 20, &quadBright, 4);
            FlushInstructionCache(GetCurrentProcess(), g_fogAlphaSite, 24);
            DWORD dummy = 0;
            VirtualProtect(g_fogAlphaSite, 24, oldProt, &dummy);
        }
    }
}

uint8_t* CustomMarkerType(uint32_t typeHash) {
    if (typeHash == kGoldChestTypeHash) {
        return reinterpret_cast<uint8_t*>(&g_goldChestMarkerType);
    }
    if (typeHash == kSilverChestTypeHash) {
        return reinterpret_cast<uint8_t*>(&g_silverChestMarkerType);
    }
    if (typeHash == kWoodChestTypeHash) {
        return reinterpret_cast<uint8_t*>(&g_woodChestMarkerType);
    }
    if (typeHash == kLockedFastTravelTypeHash) {
        return reinterpret_cast<uint8_t*>(&g_lockedFastTravelMarkerType);
    }
    return nullptr;
}

// Our marker types, every other one from the game (or a mod that hooked the lookup before us).
uint8_t* GetMarkerTypeHook(uint32_t typeHash, void* mgr) {
    UMM_PERF(PerfMarkerType);
    {
        greeneye::Lease lease; // an older generation after a hot reload only calls on
        if (lease && !g_modOff) {
            if (uint8_t* custom = CustomMarkerType(typeHash)) return custom;
        }
    }
    return g_origGetMarkerType ? g_origGetMarkerType(typeHash, mgr) : nullptr;
}

// The game's GUID -> loca id: 32-bit FNV-1a over the 16 bytes (two identical copies in the exe).
uint32_t GuidToLocaId(const void* guid16) {
    const auto* b = static_cast<const uint8_t*>(guid16);
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 16; ++i) {
        h = (h ^ b[i]) * 0x01000193u;
    }
    return h;
}

constexpr size_t kPrimitivesOffset = 0xd3d0;
constexpr size_t kCirclePrimitive = kPrimitivesOffset + 0x00;
constexpr size_t kCircleOutlinePrimitive = kPrimitivesOffset + 0x10;
constexpr size_t kSoftGlowPrimitive = kPrimitivesOffset + 0x20;
constexpr size_t kMapPulseColorOffset = 0xd640;
constexpr size_t kMapPulseRingPrimitive = 0xee70;
constexpr size_t kCheckmarkPrimitive = 0xef30;
// Nine-slice boxes of the game's FbUiPrimitives (the boxes vanilla menus and dialogs are made of).
constexpr size_t kBoxInnerGlowPrimitive = kPrimitivesOffset + 0x120;
constexpr size_t kGlowBoxSmallPrimitive = kPrimitivesOffset + 0x140;
constexpr size_t kRoundedBoxPrimitive = kPrimitivesOffset + 0x180;
constexpr size_t kSmallTextStyleOffset = 0x10bbc;
// FbUiBundle.namedStyles (0x10bbc) .h3 (+0x318): the game's bold 24 px heading, the base of the deposit title
constexpr size_t kH3StyleOffset = 0x10bbc + 0x318;
// FbUiBundle.namedStyles .map (+0x1340) .locationName (+0x58): the map's region names (uppercase, outlined)
constexpr size_t kMapLocationNameStyleOffset = 0x10bbc + 0x1340 + 0x58;
constexpr size_t kUiCharacterStyleSize = 0x58;
constexpr size_t kStyleFontSize = 0x20; // UiCharacterStyle.fontSize
constexpr float kFilterRowFontScale = 0.86f; // the Map Key's filter rows, a little smaller so long lists do not crowd
constexpr float kCategoryTitleFontScale = 0.9f; // the category bars' titles, inside their bar
constexpr float kBadgeFontScale = 0.78f;        // ON / OFF, inside the 18 px badge
constexpr size_t kHintListOffset = 0x32e30;
constexpr size_t kHintCountOffset = 0x480;
constexpr uint32_t kLayerOrder = 0x16;

struct FilterEntry {
    uint32_t typeHash;
    uint8_t category;
    uint8_t enabled;
    uint16_t pad;
};

struct MarkerRowSpec {
    uint32_t typeHash;
    uint32_t pairedHash;
    uint8_t category;
    const char* label;       // English: logs and English searches
    umm_lang::Str text;      // what the panel shows
    uint32_t pairedHash2 = 0u; // a second type the row switches with it (Water Source: the world's water)
};

constexpr uint32_t kWaterSourceTypeHash = 0x4293eae5u;

constexpr MarkerRowSpec kCategory1Rows[] = {
    {0xa17072beu, 0u, 1, "Ancient Spire", umm_lang::Str::AncientSpire},
    {0xa447cba3u, 0x6a56c5b3u, 1, "Flame Altar (Home)", umm_lang::Str::FlameAltarHome},
    {0x7b4c9e16u, 0u, 1, "Flame Sanctum", umm_lang::Str::FlameSanctum},
    {0x6df85109u, 0u, 1, "Night Sanctum", umm_lang::Str::NightSanctum},
    {0xf455a765u, 0u, 1, "Gem Forge", umm_lang::Str::GemForge},
    {0xad6a9d9fu, 0u, 1, "Ancient Obelisk", umm_lang::Str::AncientObelisk},
    {0x1095684eu, 0u, 1, "Fast Travel Point", umm_lang::Str::FastTravelPoint},
    {kTownPortalTypeHash, 0u, 1, "Town Portals", umm_lang::Str::TownPortals},
};

constexpr MarkerRowSpec kCategory2Rows[] = {
    {0xffe5e444u, 0u, 2, "Elixir Well / Root", umm_lang::Str::ElixirWell},
    {0xb824ea45u, 0u, 2, "Catacombs / Tomb", umm_lang::Str::CatacombsTomb},
    {0x549e3dbau, 0u, 2, "Ancient Vault", umm_lang::Str::AncientVault},
    {0x2756cf65u, 0u, 2, "Shroud Lair", umm_lang::Str::ShroudLair},
    {0x4ece72ccu, 0u, 2, "Hollow Halls", umm_lang::Str::HollowHalls},
    {0x41a86870u, 0u, 2, "Hidden Tomb", umm_lang::Str::HiddenTomb},
    // Return Beacons: a resource row (ore_deposits.h), drawn by the mod like the deposits; text from OreName
    {kOreTypeHashBase + static_cast<uint32_t>(kBeaconsFirst), 0u, 2, kOreLabels[kBeaconsFirst], umm_lang::Str::CatShroud},
};
static_assert(kBeaconsCount == 1, "one Return Beacon row");

constexpr MarkerRowSpec kCategory3Rows[] = {
    {kGoldChestTypeHash, 0u, 3, "Gold Chest", umm_lang::Str::GoldChest},
    {kSilverChestTypeHash, 0u, 3, "Silver Chest", umm_lang::Str::SilverChest},
    {kWoodChestTypeHash, 0u, 3, "Wooden Chest", umm_lang::Str::WoodChest},
    {kSpoilsTypeHash, 0u, 3, "Dungeon Spoils", umm_lang::Str::DungeonSpoils},
    // Bosses: a resource row (ore_deposits.h), defeated ones hidden or greyed like looted chests
    {kOreTypeHashBase + static_cast<uint32_t>(kBossesFirst), 0u, 3, kOreLabels[kBossesFirst], umm_lang::Str::CatWorld},
    // Unique Weapons and Unique Equipment: resource rows (ore_deposits.h), each item at the chest or place it is in,
    // found ones hidden or greyed like looted chests
    {kOreTypeHashBase + static_cast<uint32_t>(kUniqueFirst), 0u, 3, kOreLabels[kUniqueFirst], umm_lang::Str::CatWorld},
    {kOreTypeHashBase + static_cast<uint32_t>(kUniqueFirst + 1), 0u, 3, kOreLabels[kUniqueFirst + 1], umm_lang::Str::CatWorld},
    // Collectibles: the journal's fossils, paintings, mosaic and map pieces and the like, found with their chest or by hand
    {kOreTypeHashBase + static_cast<uint32_t>(kUniqueFirst + 2), 0u, 3, kOreLabels[kUniqueFirst + 2], umm_lang::Str::CatWorld},
    // Cosmetics: the cosmetic set pieces placed in the world, found like the unique items (requested by FrostyBou)
    {kOreTypeHashBase + static_cast<uint32_t>(kCosmeticFirst), 0u, 3, kOreLabels[kCosmeticFirst], umm_lang::Str::CatWorld},
    // Fell Summoning Stones: where Shroud Spores conjure a Fell enemy
    {kOreTypeHashBase + static_cast<uint32_t>(kSummonsFirst), 0u, 3, kOreLabels[kSummonsFirst], umm_lang::Str::CatWorld},
    // The game's Water Source marker is only the water players build; the row also switches the world's Water Wells
    // and Water Barrels (resource rows in ore_deposits.h, drawn by the mod) and counts them. The game files it under
    // Fast Travel (its filter category 1); the Map Key shows it with the world (FilterCategory)
    {kWaterSourceTypeHash, kOreTypeHashBase + static_cast<uint32_t>(kWaterFirst), 3, "Water Source", umm_lang::Str::WaterSource,
     kOreTypeHashBase + static_cast<uint32_t>(kWaterFirst + 1)},
    {0xb5d01766u, 0u, 3, "Town / Settlement", umm_lang::Str::TownSettlement},
    {0x5104e8d1u, 0u, 3, "Farmstead", umm_lang::Str::Farmstead},
    {0x6aaf002cu, 0u, 3, "Survivor Camp", umm_lang::Str::SurvivorCamp},
    {0x689e3a29u, 0u, 3, "Mine / Quarry", umm_lang::Str::MineQuarry},
    {0xdb17bf33u, 0u, 3, "Cave Passage", umm_lang::Str::CavePassage},
    {0xa13a918cu, 0u, 3, "Enemy Encampment", umm_lang::Str::EnemyEncampment},
    {0x9f3c320eu, 0u, 3, "Balefire Garrison", umm_lang::Str::BalefireGarrison},
};

constexpr MarkerRowSpec kCategory0Rows[] = {
    {0xa7cadc08u, 0u, 0, "Main Quest", umm_lang::Str::MainQuest},
    {0xc52e92e5u, 0u, 0, "Side Quest", umm_lang::Str::SideQuest},
    {0x8ba7c122u, 0u, 0, "Lore Rumor (?)", umm_lang::Str::LoreRumor},
    // Lore Notes: a resource row (ore_deposits.h), read ones hidden or greyed like looted chests
    {kOreTypeHashBase + static_cast<uint32_t>(kLoreFirst), 0u, 0, kOreLabels[kLoreFirst], umm_lang::Str::CatQuests},
    {0x1ad8f96eu, 0u, 0, "Player Grave", umm_lang::Str::PlayerGrave},
    {kOtherPlayerTypeHash, 0u, 0, "Other Players", umm_lang::Str::OtherPlayers},
};

constexpr uint32_t kCustomPinHashes[10] = {
    0x0e6558ffu, 0x5eba3084u, 0xeeb09abeu, 0xfe2fd02du, 0x1db9432du,
    0x28a5fb2cu, 0xd9b19aecu, 0xa1c4ece9u, 0x3a761350u, 0x7103269eu
};

// Resource rows (Mining, Materials, Plants, Bait, Trees) from ore_deposits.h, built at compile time; their text
// comes from OreName (the game's item name), not from .text
template <size_t First, uint8_t Category, umm_lang::Str Title, size_t... I>
constexpr std::array<MarkerRowSpec, sizeof...(I)> MakeResourceRows(std::index_sequence<I...>) {
    return {{MarkerRowSpec{kOreTypeHashBase + static_cast<uint32_t>(First + I), 0u, Category, kOreLabels[First + I], Title}...}};
}
constexpr auto kMiningRows = MakeResourceRows<kMiningFirst, kMiningCategory, umm_lang::Str::CatMining>(
    std::make_index_sequence<kMiningCount>{});
constexpr auto kMaterialsRows = MakeResourceRows<kMaterialsFirst, kMaterialsCategory, umm_lang::Str::CatMaterials>(
    std::make_index_sequence<kMaterialsCount>{});
// The Plants rows, then Gloomberry (a night plant, added after the other rows so their index stays)
template <size_t A, size_t B>
constexpr std::array<MarkerRowSpec, A + B> JoinRows(const std::array<MarkerRowSpec, A>& a, const std::array<MarkerRowSpec, B>& b) {
    std::array<MarkerRowSpec, A + B> out{};
    for (size_t i = 0; i < A; ++i) out[i] = a[i];
    for (size_t i = 0; i < B; ++i) out[A + i] = b[i];
    return out;
}
constexpr auto kPlantsRows = JoinRows(JoinRows(JoinRows(
    MakeResourceRows<kPlantsFirst, kPlantsCategory, umm_lang::Str::CatPlants>(std::make_index_sequence<kPlantsCount>{}),
    MakeResourceRows<kNightPlantsFirst, kPlantsCategory, umm_lang::Str::CatPlants>(std::make_index_sequence<kNightPlantsCount>{})),
    MakeResourceRows<kLatePlantsFirst, kPlantsCategory, umm_lang::Str::CatPlants>(std::make_index_sequence<kLatePlantsCount>{})),
    MakeResourceRows<kLatePlants2First, kPlantsCategory, umm_lang::Str::CatPlants>(std::make_index_sequence<kLatePlants2Count>{}));
constexpr auto kBaitRows = JoinRows(
    MakeResourceRows<kBaitFirst, kBaitCategory, umm_lang::Str::CatBait>(std::make_index_sequence<kBaitCount>{}),
    MakeResourceRows<kLateBaitFirst, kBaitCategory, umm_lang::Str::CatBait>(std::make_index_sequence<kLateBaitCount>{}));
constexpr auto kTreesRows = MakeResourceRows<kTreesFirst, kTreesCategory, umm_lang::Str::CatTrees>(
    std::make_index_sequence<kTreesCount>{});
constexpr auto kCreaturesRows = MakeResourceRows<kCreaturesFirst, kCreaturesCategory, umm_lang::Str::CatCreatures>(
    std::make_index_sequence<kCreaturesCount>{});
// The Ground rows, then Roads and Surface Water (added after every other row so their index stays; roads.inl)
constexpr auto kGroundRows = JoinRows(
    MakeResourceRows<kGroundFirst, kGroundCategory, umm_lang::Str::CatGround>(std::make_index_sequence<kGroundCount>{}),
    MakeResourceRows<kSurfaceFirst, kGroundCategory, umm_lang::Str::CatGround>(std::make_index_sequence<kSurfaceCount>{}));
static_assert(kWaterCount == 2, "Water Well and Water Barrel rows");
static_assert(kLoreCount == 1 && kBossesCount == 1, "one Lore Note row, one Boss row");
static_assert(kUniqueCount == 3, "Unique Weapons, Unique Equipment and Collectibles rows");
static_assert(kSummonsCount == 1, "one Fell Summoning Stone row");
static_assert(kCosmeticCount == 1, "one Cosmetics row");
static_assert(kMiningCount + kMaterialsCount + kPlantsCount + kBaitCount + kTreesCount + kBeaconsCount + kWaterCount +
                  kLoreCount + kBossesCount + kUniqueCount + kSummonsCount + kNightPlantsCount + kCreaturesCount +
                  kCosmeticCount + kGroundCount + kLatePlantsCount + kLateBaitCount + kLatePlants2Count + kSurfaceCount == kOreCount,
              "every row in one category");

struct CategoryGroupSpec {
    uint8_t category;
    umm_lang::Str title;
    const MarkerRowSpec* rows;
    size_t rowCount;
};

constexpr CategoryGroupSpec kCategoryGroups[] = {
    {1, umm_lang::Str::CatFastTravel, kCategory1Rows, sizeof(kCategory1Rows) / sizeof(kCategory1Rows[0])},
    {2, umm_lang::Str::CatShroud, kCategory2Rows, sizeof(kCategory2Rows) / sizeof(kCategory2Rows[0])},
    {3, umm_lang::Str::CatWorld, kCategory3Rows, sizeof(kCategory3Rows) / sizeof(kCategory3Rows[0])},
    {kMiningCategory, umm_lang::Str::CatMining, kMiningRows.data(), kMiningRows.size()},
    {kMaterialsCategory, umm_lang::Str::CatMaterials, kMaterialsRows.data(), kMaterialsRows.size()},
    {kPlantsCategory, umm_lang::Str::CatPlants, kPlantsRows.data(), kPlantsRows.size()},
    {kBaitCategory, umm_lang::Str::CatBait, kBaitRows.data(), kBaitRows.size()},
    {kTreesCategory, umm_lang::Str::CatTrees, kTreesRows.data(), kTreesRows.size()},
    {0, umm_lang::Str::CatQuests, kCategory0Rows, sizeof(kCategory0Rows) / sizeof(kCategory0Rows[0])},
    {4, umm_lang::Str::CatCustomPins, nullptr,        0},
    // last, so the saved category order and the hash list keep their places
    {kCreaturesCategory, umm_lang::Str::CatCreatures, kCreaturesRows.data(), kCreaturesRows.size()},
    {kGroundCategory, umm_lang::Str::CatGround, kGroundRows.data(), kGroundRows.size()},
};

bool g_panelCollapsed = false;
float g_mapKeyScroll = 0.0f; // how far the Map Key's list is scrolled (UI units)
bool g_mapKeyDragging = false;
float g_mapKeyDragStartMouseY = 0.0f;
float g_mapKeyDragStartScroll = 0.0f;

// Mouse wheel over the Map Key scrolls it. A WH_GETMESSAGE hook on the game's window thread takes the wheel
// messages while the cursor is over the panel and the World Map is up (the map's own zoom is blocked there).
HHOOK g_mapKeyWheelHook = nullptr;
// The Map Key's search box: what is typed while it has the focus comes in as WM_CHAR through the same hook (the game's
// own keys are off meanwhile: the text input flag is set, as for the game's text fields)
bool g_keySearchFocused = false;
std::mutex g_keySearchMutex;
std::wstring g_keySearchTyped; // taken by the panel each frame
volatile LONG g_mapKeyWheelDelta = 0;
volatile bool g_mapKeyWheelCapture = false;
volatile uint64_t g_mapKeyLastFrameTick = 0;
volatile uint64_t g_playingKeysTick = 0; // the last frame of play (no menu, no map) the preset keys were read in

bool g_dungeonMapUp = false; // a Hollow Hall's dungeon map is what the World Map shows (set each map frame)
bool PresetKeyGoingDown(int vk); // a filter preset key (load or save) goes down with this key: kept from the game

LRESULT CALLBACK MapKeyWheelMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE && lParam) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == WM_MOUSEWHEEL && g_mapKeyWheelCapture && GetTickCount64() - g_mapKeyLastFrameTick < 250) {
            InterlockedExchangeAdd(&g_mapKeyWheelDelta, GET_WHEEL_DELTA_WPARAM(msg->wParam));
            msg->message = WM_NULL;
        }
        // While typing, the game's keys stay off: it reads the keyboard as raw input (WM_INPUT), so key presses are taken
        // from it (Q / E switch tabs, C recenters, Esc closes the map); releases go through so no key stays down. The
        // letters still come as WM_CHAR from the ordinary key messages.
        if (msg->message == WM_INPUT && g_keySearchFocused && GetTickCount64() - g_mapKeyLastFrameTick < 250) {
            RAWINPUT ri{};
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    static_cast<UINT>(-1) &&
                ri.header.dwType == RIM_TYPEKEYBOARD && !(ri.data.keyboard.Flags & RI_KEY_BREAK)) {
                msg->message = WM_NULL;
            }
        }
        // On a dungeon map the game's 1 (People & Places, the overworld's filters) is taken: it filters the world map
        if (msg->message == WM_INPUT && g_dungeonMapUp && GetTickCount64() - g_mapKeyLastFrameTick < 250) {
            RAWINPUT ri{};
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    static_cast<UINT>(-1) &&
                ri.header.dwType == RIM_TYPEKEYBOARD && !(ri.data.keyboard.Flags & RI_KEY_BREAK) &&
                (ri.data.keyboard.VKey == '1' || ri.data.keyboard.VKey == VK_NUMPAD1)) {
                msg->message = WM_NULL;
            }
        }
        // The filter preset keys (Alt+1..5 apply, Ctrl+Alt+1..5 save; [Keys] Preset1.. / SavePreset1..): their key must
        // not reach the game too (on the map its 1 is People & Places; while playing 1..5 use the hotbar's items)
        if (msg->message == WM_INPUT && (GetTickCount64() - g_mapKeyLastFrameTick < 250 ||
                                         GetTickCount64() - g_playingKeysTick < 250)) {
            RAWINPUT ri{};
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    static_cast<UINT>(-1) &&
                ri.header.dwType == RIM_TYPEKEYBOARD && !(ri.data.keyboard.Flags & RI_KEY_BREAK)) {
                if (PresetKeyGoingDown(ri.data.keyboard.VKey)) msg->message = WM_NULL;
            }
        }
        // Esc while typing clears the box, and must not reach the game (it would close the map): taken as a key
        if ((msg->message == WM_KEYDOWN || msg->message == WM_KEYUP) && msg->wParam == VK_ESCAPE && g_keySearchFocused &&
            GetTickCount64() - g_mapKeyLastFrameTick < 250) {
            if (msg->message == WM_KEYDOWN) {
                std::lock_guard<std::mutex> lock(g_keySearchMutex);
                g_keySearchTyped.push_back(static_cast<wchar_t>(0x1b));
            }
            msg->message = WM_NULL;
        }
        if (msg->message == WM_CHAR && g_keySearchFocused && GetTickCount64() - g_mapKeyLastFrameTick < 250) {
            std::lock_guard<std::mutex> lock(g_keySearchMutex);
            g_keySearchTyped.push_back(static_cast<wchar_t>(msg->wParam));
            msg->message = WM_NULL;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void EnsureMapKeyWheelHook(HWND hwnd) {
    g_mapKeyLastFrameTick = GetTickCount64();
    if (g_mapKeyWheelHook || !hwnd) return;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || tid == 0) return;
    g_mapKeyWheelHook = SetWindowsHookExW(WH_GETMESSAGE, &MapKeyWheelMsgProc, nullptr, tid);
}

void RemoveMapKeyWheelHook() {
    g_mapKeyWheelCapture = false;
    if (g_mapKeyWheelHook) {
        UnhookWindowsHookEx(g_mapKeyWheelHook);
        g_mapKeyWheelHook = nullptr;
    }
}
constexpr size_t kCategoryGroupCount = sizeof(kCategoryGroups) / sizeof(kCategoryGroups[0]);
constexpr size_t MaxCategoryRows() {
    size_t n = 0;
    for (const auto& grp : kCategoryGroups) n = grp.rowCount > n ? grp.rowCount : n;
    return n;
}
constexpr size_t kMaxCategoryRows = MaxCategoryRows();
constexpr size_t TotalCategoryRows() {
    size_t n = 0;
    for (const auto& grp : kCategoryGroups) n += grp.rowCount;
    return n;
}
constexpr size_t kTotalCategoryRows = TotalCategoryRows();
const char* RowText(const MarkerRowSpec& row);

// The Map Key's search: a row shows when its name (shown or English) or its section's title holds the text typed, or
// when a place on the map by that name is in it (KeySearchPlaces: "song of" finds the Lore Note row); a section shows,
// open, when any of its rows does. Recomputed when the text, the language or the places found change.
std::string g_keySearch; // UTF-8
struct KeySearchMatch {
    bool cat[kCategoryGroupCount]{};
    bool row[kCategoryGroupCount][kMaxCategoryRows]{};
    std::string place[kCategoryGroupCount][kMaxCategoryRows]; // the place named so that brought the row in
    int any = 0;
};
KeySearchMatch g_keySearchMatch;
// Places on the map whose name holds the search text: their row's type and the nearest one's name
std::vector<std::pair<uint32_t, std::string>> g_keySearchPlaces;
int g_keySearchPlacesVersion = 0;

void UpdateKeySearchMatch() {
    static std::string s_for;
    static int s_lang = -1, s_places = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (s_for == g_keySearch && s_lang == lang && s_places == g_keySearchPlacesVersion) return;
    s_for = g_keySearch;
    s_lang = lang;
    s_places = g_keySearchPlacesVersion;
    g_keySearchMatch = {};
    const std::string q = umm_lang::ToLowerUtf8(g_keySearch);
    auto has = [&](const char* s) { return s && umm_lang::ToLowerUtf8(s).find(q) != std::string::npos; };
    for (size_t c = 0; c < kCategoryGroupCount; ++c) {
        const auto& grp = kCategoryGroups[c];
        const bool title = has(umm_lang::T(grp.title));
        if (grp.category == 4) g_keySearchMatch.cat[c] = title || has("custom pin");
        for (size_t r = 0; r < grp.rowCount; ++r) {
            bool m = title || has(RowText(grp.rows[r])) || has(grp.rows[r].label);
            if (!m) {
                const MarkerRowSpec& row = grp.rows[r];
                for (const auto& [hash, name] : g_keySearchPlaces) {
                    if (hash != row.typeHash && (!row.pairedHash || hash != row.pairedHash) &&
                        (!row.pairedHash2 || hash != row.pairedHash2)) continue;
                    m = true;
                    g_keySearchMatch.place[c][r] = name;
                    break;
                }
            }
            g_keySearchMatch.row[c][r] = m;
            if (m) g_keySearchMatch.cat[c] = true;
        }
        g_keySearchMatch.any += g_keySearchMatch.cat[c] ? 1 : 0;
    }
}
bool KeySearching() { return !g_keySearch.empty(); }
bool KeySearchCatShown(size_t c) { return !KeySearching() || g_keySearchMatch.cat[c]; }
bool KeySearchRowShown(size_t c, size_t r) { return !KeySearching() || g_keySearchMatch.row[c][r]; }
size_t KeySearchRowCount(size_t c) {
    size_t n = 0;
    for (size_t r = 0; r < kCategoryGroups[c].rowCount; ++r) n += KeySearchRowShown(c, r) ? 1 : 0;
    return n;
}
// Sets the game's text input flag (UI owner + 0x2fe70) so the map's keys stay off while the box is typed in
void SetTextInputFlag();
bool g_categoryCollapsed[kCategoryGroupCount]{};
// The Map Key's categories in the order shown (indexes into kCategoryGroups); drag a category's title to move it.
// Saved as CategoryOrder in minimap_config.ini.
size_t g_categoryOrder[kCategoryGroupCount]{};
bool g_categoryOrderSet = false;

void ResetCategoryOrder() {
    for (size_t i = 0; i < kCategoryGroupCount; ++i) g_categoryOrder[i] = i;
    g_categoryOrderSet = true;
}

// "3,1,2,...": every category once; anything else keeps the default order
void ParseCategoryOrder(const char* text) {
    ResetCategoryOrder();
    size_t parsed[kCategoryGroupCount]{};
    bool seen[kCategoryGroupCount]{};
    size_t n = 0;
    for (const char* t = text; *t && n < kCategoryGroupCount;) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(t, &end, 10);
        if (end == t || v >= kCategoryGroupCount || seen[v]) return;
        seen[v] = true;
        parsed[n++] = v;
        t = *end == ',' ? end + 1 : end;
    }
    // a category added since the order was saved (Creatures) goes at the end
    for (size_t c = 0; c < kCategoryGroupCount && n > 0; ++c) {
        if (!seen[c]) parsed[n++] = c;
    }
    if (n != kCategoryGroupCount) return;
    std::memcpy(g_categoryOrder, parsed, sizeof(parsed));
}

// Where category c's rows start in the hash list (built in kCategoryGroups order)
size_t CategoryHashBase(size_t c) {
    size_t base = 0;
    for (size_t i = 0; i < c; ++i) base += kCategoryGroups[i].rowCount;
    return base;
}

// Filter locks: a locked row (or every row of a locked category) keeps its ON / OFF through Show All / Hide All, and
// through reloads, updates and world changes: its state is saved whenever a filter changes and put back once the
// world is recognised. A locked category's open / closed state is kept the same way. The player can still click
// them; holding Show All / Hide All passes every lock. Saved in minimap_config.ini [Locks], which updates leave alone.
constexpr size_t kMaxCategoryId = 16;
std::unordered_set<uint32_t> g_lockedRows; // row type hashes (a paired hash goes with its row)
bool g_lockedCategories[kMaxCategoryId]{};
std::unordered_map<uint32_t, bool> g_lockedStates; // the locked rows' ON / OFF as last saved
bool g_applyingLockedStates = false;               // ApplyLockedStates saves once, at the end
void SaveFilterLocks();
void SaveCollapsedSections();

bool IsCategoryLocked(uint8_t category) { return category < kMaxCategoryId && g_lockedCategories[category]; }

// The row a hash belongs to: its own, or the row it is paired with (Flame Altar's second type).
uint32_t RowHashOf(uint32_t typeHash) {
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (grp.rows[r].pairedHash != 0u && grp.rows[r].pairedHash == typeHash) return grp.rows[r].typeHash;
            if (grp.rows[r].pairedHash2 != 0u && grp.rows[r].pairedHash2 == typeHash) return grp.rows[r].typeHash;
        }
    }
    return typeHash;
}

bool IsRowLocked(uint32_t typeHash) { return !g_lockedRows.empty() && g_lockedRows.count(RowHashOf(typeHash)) != 0; }
bool IsFilterLocked(uint32_t typeHash, uint8_t category) { return IsCategoryLocked(category) || IsRowLocked(typeHash); }

size_t FilterLockCount() {
    size_t n = g_lockedRows.size();
    for (bool locked : g_lockedCategories) n += locked ? 1 : 0;
    return n;
}

void ToggleRowLock(uint32_t typeHash) {
    if (!g_lockedRows.erase(typeHash)) g_lockedRows.insert(typeHash);
    SaveFilterLocks();
}

void ToggleCategoryLock(uint8_t category) {
    if (category >= kMaxCategoryId) return;
    g_lockedCategories[category] = !g_lockedCategories[category];
    SaveFilterLocks();
}

void ClearFilterLocks() {
    g_lockedRows.clear();
    for (bool& locked : g_lockedCategories) locked = false;
    SaveFilterLocks();
}
void DrawCategoryBarAbs(void* widgetRaw, float x, float y, float w, float h, bool hovered);
bool g_leftWasDown = false;
bool g_toggleKeyWasDown = false;
// The key that folds the panel ([Keys] PanelToggle in the ini; None: no key), as a Windows virtual key and as the
// game's own key code for the map's hint bar (USB HID usage: A = 4, 1 = 0x1e, Tab = 0x2b, F1 = 0x3a)
int g_panelToggleVk = VK_TAB;
uint16_t g_panelToggleHid = 0x2b;
// Every key and controller button of the mod, [Keys] in the ini (common/input_binds.h). Glow is pressed twice and
// MinimapMouse is held with the mouse (requested by izisot). Read again when the file changes.
namespace inp = greeneye::input;
struct MapKeys {
    inp::Binding panelToggle, glow, minimapMouse, findOpen, mapZoomIn, mapZoomOut, miniZoomIn, miniZoomOut, undo, redo,
        hallRecenter, modOff, chestClick, hideRowClick, regionClick, copyPosition, padMapKey, padBack, padClick, padUp,
        padMarkChest, padHideRow,
        padDown, padLeft, padRight, findClose, findConfirm, findNext, findPrevious, findUp, findDown, findPageUp,
        findPageDown, findFilters, findExact, interact, floorUp, floorDown, preset1, preset2, preset3, preset4, preset5,
        presetNext, presetPrev, savePreset1, savePreset2, savePreset3, savePreset4, savePreset5;
    const inp::Binding& Preset(int p) const {
        const inp::Binding* all[5] = {&preset1, &preset2, &preset3, &preset4, &preset5};
        return *all[p];
    }
    const inp::Binding& SavePreset(int p) const {
        const inp::Binding* all[5] = {&savePreset1, &savePreset2, &savePreset3, &savePreset4, &savePreset5};
        return *all[p];
    }
} g_keys;
struct MapKeySetting { const char* name; const char* defaultText; inp::Binding MapKeys::* field; };
constexpr MapKeySetting kMapKeySettings[] = {
    {"PanelToggle", "Tab", &MapKeys::panelToggle},
    {"Glow", "G, Pad:LB+A", &MapKeys::glow},
    {"MinimapMouse", "Shift+Alt", &MapKeys::minimapMouse},
    {"FindOnMap", "Shift+F, Pad:LS+X", &MapKeys::findOpen},
    {"MapZoomIn", "NumPlus, PageUp", &MapKeys::mapZoomIn},
    {"MapZoomOut", "NumMinus, PageDown", &MapKeys::mapZoomOut},
    {"MinimapZoomIn", "], NumPlus, Pad:RS+Up", &MapKeys::miniZoomIn},
    {"MinimapZoomOut", "[, NumMinus, Pad:RS+Down", &MapKeys::miniZoomOut},
    {"FilterUndo", "Ctrl+Z, Pad:RS+Left", &MapKeys::undo},
    {"FilterRedo", "Ctrl+Y, Ctrl+Shift+Z, Pad:RS+Right", &MapKeys::redo},
    {"HallRecenter", "C, Pad:LS", &MapKeys::hallRecenter},
    {"ModOnOff", "Ctrl+Shift+F12", &MapKeys::modOff},
    {"ChestClick", "Ctrl", &MapKeys::chestClick},
    {"HideRowClick", "Alt", &MapKeys::hideRowClick},
    {"RegionOnlyClick", "Ctrl", &MapKeys::regionClick},
    {"CopyPosition", "Mouse3", &MapKeys::copyPosition},
    {"MarkChestPad", "NoKeys, Pad:RS+Up", &MapKeys::padMarkChest},
    {"HideRowPad", "NoKeys, Pad:RS+Down", &MapKeys::padHideRow},
    {"MapKeyEnter", "NoKeys, Pad:View", &MapKeys::padMapKey},
    {"MapKeyBack", "NoKeys, Pad:B", &MapKeys::padBack},
    {"MapKeyClick", "NoKeys, Pad:A", &MapKeys::padClick},
    {"MapKeyUp", "NoKeys, Pad:Up", &MapKeys::padUp},
    {"MapKeyDown", "NoKeys, Pad:Down", &MapKeys::padDown},
    {"MapKeyLeft", "NoKeys, Pad:Left", &MapKeys::padLeft},
    {"MapKeyRight", "NoKeys, Pad:Right", &MapKeys::padRight},
    {"FindClose", "Esc, Pad:B", &MapKeys::findClose},
    {"FindConfirm", "Enter, Pad:A", &MapKeys::findConfirm},
    {"FindNext", "Tab", &MapKeys::findNext},
    {"FindPrevious", "Shift+Tab", &MapKeys::findPrevious},
    {"FindUp", "Up, Pad:Up", &MapKeys::findUp},
    {"FindDown", "Down, Pad:Down", &MapKeys::findDown},
    {"FindPageUp", "PageUp", &MapKeys::findPageUp},
    {"FindPageDown", "PageDown", &MapKeys::findPageDown},
    {"FindFilters", "F2, Ctrl+F", &MapKeys::findFilters},
    {"FindExactMatch", "Ctrl+E, Pad:Y", &MapKeys::findExact}, // Find's Exact match switch
    // A vault's or spire's map: the floor shown, one up / down (PageUp / PageDown zoom, RS + the D-pad are taken)
    {"FloorUp", "Ctrl+Up, Pad:LT+Up", &MapKeys::floorUp},
    {"FloorDown", "Ctrl+Down, Pad:LT+Down", &MapKeys::floorDown},
    // Filter presets on the World Map and while playing (keyboard only there): apply slot 1..5, or the next / previous
    // one on a controller
    {"Preset1", "Alt+1", &MapKeys::preset1},
    {"Preset2", "Alt+2", &MapKeys::preset2},
    {"Preset3", "Alt+3", &MapKeys::preset3},
    {"Preset4", "Alt+4", &MapKeys::preset4},
    {"Preset5", "Alt+5", &MapKeys::preset5},
    {"PresetNext", "NoKeys, Pad:LT+Right", &MapKeys::presetNext},
    {"PresetPrevious", "NoKeys, Pad:LT+Left", &MapKeys::presetPrev},
    // ... and save the filters shown into slot 1..5 (as a Ctrl + click on the slot)
    {"SavePreset1", "Ctrl+Alt+1", &MapKeys::savePreset1},
    {"SavePreset2", "Ctrl+Alt+2", &MapKeys::savePreset2},
    {"SavePreset3", "Ctrl+Alt+3", &MapKeys::savePreset3},
    {"SavePreset4", "Ctrl+Alt+4", &MapKeys::savePreset4},
    {"SavePreset5", "Ctrl+Alt+5", &MapKeys::savePreset5},
};
bool KeyIsDown(int vk) { return inp::KeyDown(vk); }
// A binding as text on the panel: the controller's chord when the game is showing controller tips.
std::string CurrentKeyText(const inp::Binding& b) {
    inp::Binding one;
    const bool pad = greeneye::hints::PadInUse() && b.padCount;
    if (pad) { one.pads[0] = b.pads[0]; one.padCount = 1; }
    else if (b.keyCount) { one.keys[0] = b.keys[0]; one.keyCount = 1; }
    std::string text = inp::BindingToText(one);
    const size_t cut = text.find(", ");
    if (pad) return cut == std::string::npos ? text : text.substr(cut + 2 + 4); // past "NoKeys, Pad:"
    return cut == std::string::npos ? text : text.substr(0, cut);
}
// The search's line names its filter key F2 in every language; here the key it has now.
void ShowFiltersKey(char* line, size_t size) {
    const char* at = std::strstr(line, "F2");
    if (!at || !g_keys.findFilters.keyCount) return;
    const std::string rest(at + 2);
    const std::string key = CurrentKeyText(g_keys.findFilters);
    std::snprintf(line + (at - line), size - size_t(at - line), "%s%s", key.c_str(), rest.c_str());
}
bool BindingDown(const inp::Binding& b) { return inp::KeysHeld(b, KeyIsDown) || (b.padCount && inp::PadHeld(b, inp::ReadPad())); }

// A key's name ("Tab", "G", "5", "F3", "CapsLock", "None"...) to its virtual key and HID code; false if not known.
bool ParseKeyName(const char* name, int& vk, uint16_t& hid) {
    struct Named { const char* name; int vk; uint16_t hid; };
    static constexpr Named kNamed[] = {
        {"None", 0, 0},          {"Tab", VK_TAB, 0x2b},       {"CapsLock", VK_CAPITAL, 0x39},
        {"Backquote", VK_OEM_3, 0x35}, {"Insert", VK_INSERT, 0x49}, {"Delete", VK_DELETE, 0x4c},
        {"Home", VK_HOME, 0x4a},    {"End", VK_END, 0x4d},       {"Space", VK_SPACE, 0x2c},
    };
    for (const Named& n : kNamed) {
        if (_stricmp(name, n.name) == 0) {
            vk = n.vk;
            hid = n.hid;
            return true;
        }
    }
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (c && !name[1]) {
        if (c >= 'A' && c <= 'Z') { vk = c; hid = static_cast<uint16_t>(4 + c - 'A'); return true; }
        if (c >= '1' && c <= '9') { vk = c; hid = static_cast<uint16_t>(0x1e + c - '1'); return true; }
        if (c == '0') { vk = c; hid = 0x27; return true; }
    }
    if (c == 'F' && name[1] >= '1' && name[1] <= '9') {
        const int f = std::atoi(name + 1);
        if (f >= 1 && f <= 12) { vk = VK_F1 + f - 1; hid = static_cast<uint16_t>(0x3a + f - 1); return true; }
    }
    return false;
}

// The minimap's mouse keys all held (none set: never)
bool MinimapMouseKeysHeld() {
    return inp::KeysHeld(g_keys.minimapMouse, KeyIsDown);
}

uint8_t* ScanPattern(uint8_t* base, size_t size, const char* pattern) {
    uint8_t bytes[128]{};
    bool mask[128]{};
    size_t patLen = 0;

    const char* p = pattern;
    while (*p && patLen < 128) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            bytes[patLen] = 0;
            mask[patLen] = false;
            p += 2;
        } else {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            bytes[patLen] = static_cast<uint8_t>((hexVal(p[0]) << 4) | hexVal(p[1]));
            mask[patLen] = true;
            p += 2;
        }
        ++patLen;
    }

    if (patLen == 0 || size < patLen) return nullptr;
    const size_t limit = size - patLen;
    for (size_t i = 0; i <= limit; ++i) {
        bool ok = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] && base[i + j] != bytes[j]) {
                ok = false;
                break;
            }
        }
        if (ok) return base + i;
    }
    return nullptr;
}

void* GetUiG() {
    return g_uiRoot ? *g_uiRoot : nullptr;
}

uint8_t* GetUiAssets() {
    auto* g = static_cast<uint8_t*>(GetUiG());
    return g ? *reinterpret_cast<uint8_t**>(g + 200) : nullptr;
}

uint8_t* GetUiClient() {
    auto* g = static_cast<uint8_t*>(GetUiG());
    if (!g) return nullptr;
    auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0);
    if (!pD0 || !*pD0) return nullptr;
    return *reinterpret_cast<uint8_t**>(*pD0 + 0x38);
}

uint8_t* GetClientWorld(uint8_t* client) {
    return client ? *reinterpret_cast<uint8_t**>(client + 0x47d2f0) : nullptr;
}

uint8_t* GetHintCountPtr() {
    auto* g = static_cast<uint8_t*>(GetUiG());
    auto* ui = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    return (ui && *reinterpret_cast<void**>(ui)) ? (ui + kHintListOffset + kHintCountOffset) : nullptr;
}

// The waypoint set on a place: the UI state keeps a copy of that marker's slot (0xA0) here, uid at +0 and position
// at +0x10, both 0 when no waypoint is set (found 2026-11-09 by setting and clearing one on a read obelisk)
constexpr size_t kWaypointSlotOffset = 0x14020;

bool IsWaypointMarker(const uint8_t* m) {
    auto* g = static_cast<uint8_t*>(GetUiG());
    auto* ui = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    if (!m || !ui || !*reinterpret_cast<void**>(ui)) return false;
    const uint8_t* wp = ui + kWaypointSlotOffset;
    const uint64_t uid = *reinterpret_cast<const uint64_t*>(wp);
    return uid != 0 && uid == *reinterpret_cast<const uint64_t*>(m) &&
           *reinterpret_cast<const uint64_t*>(wp + 0x10) == *reinterpret_cast<const uint64_t*>(m + 0x10);
}

uint64_t PackXY(float x, float y) {
    uint64_t v = 0;
    float xy[2] = {x, y};
    std::memcpy(&v, xy, 8);
    return v;
}

struct Widget {
    alignas(16) uint8_t raw[0x80]{};
    uint8_t* Handle() { return *reinterpret_cast<uint8_t**>(raw + 8); }
};

uint64_t MakePackedColor(uint32_t rgba, float alpha, float brightness = 1.0f) {
    float clampedAlpha = std::max(0.0f, std::min(1.0f, alpha));
    uint32_t aByte = static_cast<uint32_t>(((rgba >> 24) & 0xffu) * clampedAlpha + 0.5f);
    struct {
        uint32_t packedRgba;
        float bright;
    } col{(rgba & 0x00ffffffu) | (aByte << 24), brightness};
    uint64_t packedCol = 0;
    std::memcpy(&packedCol, &col, 8);
    return packedCol;
}

// Draws a 100% solid flat rectangle at absolute UI coordinates (absX, absY, w, h)
// without creating or shifting any child widgets.
// A screen box the mod's map drawing stays out of while set (a marker's menu on the World Map, drawn before the mod's
// icons): anything touching it is not drawn
struct DrawExclusion {
    bool active = false;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};
DrawExclusion g_drawExclusion;
// The game's key hints along the bottom of the World Map (Mark Chest Looted, Hide Filter, ...): drawn before the mod's
// icons, so while those draw they stay out of its strip, or they cover the hints
DrawExclusion g_hintStrip;
// How far from a game marker's spot an Alt / Ctrl + click still takes it (UI px): its icon's edge, not only its middle (a
// spire's diamond is ~50 px across; 16 missed clicks on it). The nearest marker wins, so crowded spots are as before.
constexpr float kMarkerReach = 26.0f;
char g_hoverTitleText[128]{}; // the game's hover title (MarkerTitleTextHook) and when it was asked for
uint64_t g_hoverTitleTick = 0;
DrawExclusion g_modTitleBox; // the mod's own hover title, drawn last frame (DrawMapHoverTitle)
uint64_t g_modTitleBoxTick = 0;
// The game's marker under the cursor (its hover title's marker, MarkerTitleTextHook), and where its icon was drawn
// (DrawWorldMapMarkerIconHook: its square, UI px), this frame or the last; the cursor (the World Map's frame)
const uint8_t* g_hoveredGameMarker = nullptr;
uint64_t g_hoveredGameMarkerTick = 0, g_hoveredIconTick = 0;
float g_hoveredIcon[3]{}; // centre x, y, half size
bool g_mapCursorIn = false;
float g_mapCursor[2]{};
// The game's hovered marker, when the cursor is on its icon as drawn: it is over the mod's icons (a Wooden Chest
// beside an Ancient Spire), so it gets the hover title and Alt / Ctrl + click; off its icon, the mod's icon does
// The marker the game shows its hover title for, this frame or the last (some, a rumor's "?", are drawn away from their
// marker's own spot and not through the icon hook: no icon square to test). Its spot.
bool GameTitledMarker(int32_t& x, int32_t& z) {
    if (!g_hoveredGameMarker || GetTickCount64() - g_hoveredGameMarkerTick > 150ULL) return false;
    __try {
        x = *reinterpret_cast<const int32_t*>(g_hoveredGameMarker + 0x14);
        z = *reinterpret_cast<const int32_t*>(g_hoveredGameMarker + 0x24);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ModIconHovered(); // the mod's own icon under the cursor this frame (g_hoveredDepositRow)
// The game's hovered marker is a quest / rumor pin
bool QuestPinHovered() {
    __try {
        return g_hoveredGameMarker && *reinterpret_cast<const uint32_t*>(g_hoveredGameMarker + 0x30) == 0xafce0c5fu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool GameIconUnderCursor(int32_t* x = nullptr, int32_t* z = nullptr) {
    const uint64_t now = GetTickCount64();
    if (!g_hoveredGameMarker || !g_mapCursorIn || now - g_hoveredGameMarkerTick > 150ULL || now - g_hoveredIconTick > 150ULL)
        return false;
    // (inside its diamond: the place icons are diamonds, and the round ones fill about as much; a square would take the
    // corners, where the icon under it shows)
    // (a quest's "!" is hovered anywhere in its search circle and drawn enlarged: only its own diamond counts, so a
    // collectible beside it in the circle keeps its name and its Alt / Ctrl + click, 2026-11-12)
    // (its spot is the diamond's bottom tip, where an icon beside it sits: the mod's icon under the cursor wins)
    float reach = g_hoveredIcon[2];
    if (QuestPinHovered()) {
        if (ModIconHovered()) return false;
        reach *= 0.6f;
    }
    if (std::fabs(g_mapCursor[0] - g_hoveredIcon[0]) + std::fabs(g_mapCursor[1] - g_hoveredIcon[1]) > reach)
        return false;
    __try {
        if (x) *x = *reinterpret_cast<const int32_t*>(g_hoveredGameMarker + 0x14);
        if (z) *z = *reinterpret_cast<const int32_t*>(g_hoveredGameMarker + 0x24);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The game's "People & Places" legend at the top left (its row of icons under the title): the same
DrawExclusion g_legendBox;
// The players' locators on the World Map (the game's pins, drawn under the mod's icons): no mod icon over them
// The game draws its markers after them too, so DrawWorldMapMarkerIconHook skips one whose spot is in a locator's box
// (g_locatorKeep: last frame's boxes, kept while the map is drawn)
DrawExclusion g_locators[8], g_locatorKeep[8];
uint64_t g_locatorKeepTick = 0;
float g_locatorRingZoom = 0.0f; // (dev op locatorzoom) the game draws the ring only on hover: zoomed all the way out it is the bare arrow too
float g_locatorBox[4] = {-0.45f, -0.45f, 0.45f, 0.5f}; // round a player's map spot, in the game's marker icon sizes
// (their drawn square, ~53 px with its padding): left, top, right, bottom. The arrow itself, ~half one across: a box the
// size of the locator's ring left a gap round you where creatures and the like weren't drawn.
float g_markerIconSize = 53.0f; // the game's marker icons drawn this frame (DrawWorldMapMarkerIconHook), UI px
// The hover title's area (the game's, to the right of its hovered marker, or the mod's own): a tooltip panel is drawn
// behind it, like the game's gear tooltips, so the name reads on a crowded map. Set before the icon passes.
DrawExclusion g_titleBox;
bool MapLayerReady(); // below: the map-layer pass is running (Map Desk on)
// Where the mod's drawing would sit over the game's UI (a marker's menu column, the key hints, the legend): with the Map
// Desk on, drawn under it in the map layer (UnderRing); without, left out as before
bool OverGameUi(float x, float y, float w, float h) {
    auto in = [&](const DrawExclusion& e) { return e.active && x < e.x1 && x + w > e.x0 && y < e.y1 && y + h > e.y0; };
    return in(g_drawExclusion) || in(g_hintStrip) || in(g_legendBox);
}
bool g_ringReplayFlag = false; // (DrawUnderRing replaying: nothing excluded)
bool Excluded(float x, float y, float w, float h) {
    return !g_ringReplayFlag && !MapLayerReady() && OverGameUi(x, y, w, h);
}
// The mod's icons and check marks over the "player over here" ring: not dropped, drawn later in the map's own layer, under
// the game's markers and that ring (desk.inl DrawUnder calls DrawUnderRing; without the Map Desk they draw as before)
// The same for whole passes that belong with the game's markers (check marks on their corners, the mod's map words):
// with g_toMapLayer set they all go into the map's layer, under the game's markers, labels and the ring.
struct RingDraw {
    int kind; // 0 asset, 1 icon, 2 label, 3 nine-slice box (border in w2), 4 solid rect
    size_t asset;
    uint8_t iconRef[16];
    float x, y, w, h;
    uint32_t rgba;
    float alpha, brightness;
    std::string text;
    int hAlign = 0, vAlign = 0;
    const uint8_t* style = nullptr;
    float opacity = 1.0f; // the UI opacity to draw it at (a faint place name's)
    float border = 0.0f;
};
float g_queueOpacity = 1.0f; // set by a caller that would have lowered the UI opacity (regions.inl DrawFaintPlaceName)
std::vector<RingDraw> g_ringQueue, g_ringDrawing;
extern bool g_mapDesk; // desk.inl
bool g_ringReplay = false;
bool g_toMapLayer = false;
bool g_roadsUnder = false;          // the roads wait for the map's layer (DrawUnderRing): the view they were set for
uint8_t* g_roadsMapState = nullptr;
float g_roadsRect[4] = {};
uint64_t g_mapLayerTick = 0; // the desk's map-layer pass ran recently (DrawUnderRing): the queue gets drawn
bool MapLayerReady() { return g_mapDesk && GetTickCount64() - g_mapLayerTick <= 250; }
bool g_arrowHovered = false;  // the cursor on your arrow (SetLocatorExclusions)
DrawExclusion g_arrowBox;      // the player's bare arrow: the mod's icons go under it (not the check marks on game markers)
bool g_markerChecks = false;   // drawing the check marks on the game's markers (they'd sink under their own marker)
bool UnderRing(float x, float y, float w, float h) {
    if (g_ringReplay || !MapLayerReady()) return false;
    if (g_toMapLayer) return true;
    // (only icon-sized things move under: a level-area tint strip or a forest run crossing the arrow or the hints, moved
    // whole into the map layer, drew a dark band across the map)
    if (w > 120.0f || h > 120.0f) return false;
    if (OverGameUi(x, y, w, h)) return true;
    if (!g_markerChecks && g_arrowBox.active && x < g_arrowBox.x1 && x + w > g_arrowBox.x0 && y < g_arrowBox.y1 && y + h > g_arrowBox.y0)
        return true;
    for (const DrawExclusion& e : g_locators)
        if (e.active && x < e.x1 && x + w > e.x0 && y < e.y1 && y + h > e.y0) return true;
    return false;
}

bool LevelMapView(uint8_t* mapState, float& zoom, float& originX, float& originY, float& mapWorldH); // levels.inl
// The locators of the players the World Map shows (you, and the others unless Other Players is off), from the client's
// player list (client + 0x4642c0, 0x240 apart, live position at +0x10 / +0x18)
void SetLocatorExclusions(uint8_t* client, uint8_t* mapState) {
    for (DrawExclusion& e : g_locators) e.active = false;
    for (DrawExclusion& e : g_locatorKeep) e.active = false;
    g_arrowBox.active = false;
    g_locatorKeepTick = GetTickCount64();
    float zoom = 0.0f, ox = 0.0f, oy = 0.0f, worldH = 0.0f;
    if (!client || !LevelMapView(mapState, zoom, ox, oy, worldH)) return;
    uint8_t* players = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
    const uint64_t count = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
    const uint32_t localId = *reinterpret_cast<uint32_t*>(client + 0x18);
    if (!players || count > 64) return;
    size_t n = 0;
    for (uint64_t i = 0; i < count && n < std::size(g_locators); ++i) {
        const uint8_t* p = players + i * 0x240;
        const uint32_t id = *reinterpret_cast<const uint32_t*>(p);
        if (!id || (id != localId && !g_otherPlayersEnabled)) continue;
        float sx = ox + *reinterpret_cast<const float*>(p + 0x10) * zoom;
        float sy = oy + (worldH - *reinterpret_cast<const float*>(p + 0x18)) * zoom;
        // off the view, the game pins your locator, as its ring ("player over here"), at the view's edge (measured: with you
        // below the view its middle is 170 UI px above the bottom, straight above you)
        bool offView = false;
        if (id == localId) {
            const float* r = g_overlayRootRect;
            const float minX = r[0] + 70.0f, maxX = r[0] + r[2] - 70.0f, minY = r[1] + 180.0f, maxY = r[1] + r[3] - 170.0f;
            offView = sx < minX || sx > maxX || sy < minY || sy > maxY;
            sx = std::clamp(sx, minX, maxX), sy = std::clamp(sy, minY, maxY);
        }
        // (the locator is drawn the same size at any zoom, the pins aren't: in UI px, measured at the pins' 53)
        const float u = 53.0f;
        // the cursor on the arrow: the game draws its ring and point over its own pins, the mod's icons and check marks
        // (drawn after) are kept out of it; the game's pins under it aren't hidden (the box UnderLocator reads stays
        // the arrow's: hiding them made the pins round you vanish on hover)
        static const float kRing[4] = {-0.95f, -0.95f, 0.95f, 1.5f};
        // (the game shows the ring with the cursor anywhere on it: its whole width, the point too)
        // the "player over here" ring: the game opens it with the cursor on the arrow itself and keeps it while the cursor
        // stays on the ring (near the arrow without touching it, there is no ring and nothing may be hidden)
        static bool s_ringOpen = false;
        const float cdx = g_mapCursor[0] - sx, cdy = g_mapCursor[1] - sy;
        const bool onArrow = g_mapCursorIn && std::fabs(cdx) < 0.45f * u && std::fabs(cdy) < 0.45f * u;
        const bool onRing = g_mapCursorIn && cdx > kRing[0] * u && cdx < kRing[2] * u && cdy > kRing[1] * u && cdy < kRing[3] * u;
        if (id == localId) s_ringOpen = onArrow || (s_ringOpen && onRing), g_arrowHovered = onArrow;
        const bool ringed = id == localId && s_ringOpen;
        // zoomed out the game draws your locator as that ring and point all the time; zoomed in it is the bare arrow, and
        // nothing round it is hidden (the user: pins round you must stay)
        const bool ringShown = id == localId && (ringed || offView || zoom < g_locatorRingZoom);
        const float* box = g_locatorBox;
        g_locators[n++] = {true, sx + box[0] * u, sy + box[1] * u, sx + box[2] * u, sy + box[3] * u};
        // nothing round the player is hidden, ever (the user: "stop hiding stuff around the player"): the game's pins
        // are never skipped; only while you hover the locator are the mod's own icons kept out of its ring
        g_locatorKeep[n - 1].active = false;
        // only while the ring shows are the mod's icons and check marks kept out of it (they drew over it); the game's
        // pins are never skipped
        // (these boxes don't hide anything: what the mod draws in them goes under, in the map layer: the player's arrow on
        // top of everything, its ring too)
        if (ringShown) g_locators[n - 1] = {true, sx + kRing[0] * u, sy + kRing[1] * u, sx + kRing[2] * u, sy + kRing[3] * u};
        else {
            if (id == localId) g_arrowBox = g_locators[n - 1]; // the bare arrow: g_arrowBox (check marks on a marker under it
            g_locators[n - 1].active = false;                  // stay with their marker)
        }
    }
}

void DrawSolidRectAbs(void* parentWidgetRaw, float absX, float absY, float w, float h, uint32_t rgba, float alpha, float brightness = 1.0f) {
    if (!parentWidgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.0f || Excluded(absX, absY, w, h)) return;
    if (UnderRing(absX, absY, w, h)) {
        if (g_ringQueue.size() < 4096) g_ringQueue.push_back({4, 0, {}, absX, absY, w, h, rgba, alpha, brightness});
        return;
    }
    float rect[4] = {absX, absY, w, h};
    uint32_t zeroImage = 0; // Image ID 0 is the engine's built-in solid white quad texture
    uint64_t packedCol = MakePackedColor(rgba, alpha, brightness);
    g_drawImage(parentWidgetRaw, rect, &zeroImage, &packedCol);
}

// Draws an asset primitive (e.g. circle) at absolute UI coordinates (absX, absY, w, h)
// without creating or shifting any child widgets.
void DrawAssetRectAbs(void* parentWidgetRaw, size_t assetOffset, float absX, float absY, float w, float h, uint32_t rgba, float alpha, float brightness = 1.0f) {
    uint8_t* assets = GetUiAssets();
    if (!assets || !parentWidgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.0f || Excluded(absX, absY, w, h)) return;
    if (UnderRing(absX, absY, w, h)) {
        if (g_ringQueue.size() < 4096) g_ringQueue.push_back({0, assetOffset, {}, absX, absY, w, h, rgba, alpha, brightness});
        return;
    }
    float rect[4] = {absX, absY, w, h};
    uint64_t packedCol = MakePackedColor(rgba, alpha, brightness);
    alignas(16) uint8_t tmp[16]{};
    g_drawImage(parentWidgetRaw, rect, g_image(g_uiCtx(), tmp, assets + assetOffset), &packedCol);
}

// Draws a 16-byte UiImageReference icon at absolute UI coordinates (absX, absY, w, h)
// without creating or shifting any child widgets.
// The boss icon (the user's horned helmet art, reduced to 24x24 and 14 colours with the eyes' red kept), drawn as solid runs:
// {x, y, width, colour 0xAABBGGRR}.
constexpr int kBossIconSize = 24;
struct PixelRun { uint8_t x, y, w; uint32_t color; };
constexpr PixelRun kBossIconRuns[] = {
    {3, 1, 1, 0xFFCBDFEBu},
    {20, 1, 1, 0xFFCBDFEBu},
    {2, 2, 1, 0xFFE2FAFEu},
    {3, 2, 1, 0xFFCBDFEBu},
    {20, 2, 1, 0xFFCBDFEBu},
    {21, 2, 1, 0xFFE2FAFEu},
    {1, 3, 1, 0xFFCBDFEBu},
    {2, 3, 1, 0xFFE2FAFEu},
    {21, 3, 2, 0xFFE2FAFEu},
    {0, 4, 1, 0xFFCBDFEBu},
    {1, 4, 2, 0xFFE2FAFEu},
    {3, 4, 1, 0xFFCBDFEBu},
    {20, 4, 1, 0xFFBBCADAu},
    {21, 4, 2, 0xFFE2FAFEu},
    {23, 4, 1, 0xFFCBDFEBu},
    {0, 5, 2, 0xFFCBDFEBu},
    {2, 5, 1, 0xFFE2FAFEu},
    {3, 5, 1, 0xFFCBDFEBu},
    {11, 5, 2, 0xFFCBDFEBu},
    {20, 5, 1, 0xFFCBDFEBu},
    {21, 5, 1, 0xFFE2FAFEu},
    {22, 5, 2, 0xFFCBDFEBu},
    {0, 6, 1, 0xFFBBCADAu},
    {1, 6, 1, 0xFFCBDFEBu},
    {2, 6, 2, 0xFFE2FAFEu},
    {4, 6, 1, 0xFFCBDFEBu},
    {6, 6, 1, 0xFF9CA4AFu},
    {8, 6, 1, 0xFFBBCADAu},
    {9, 6, 2, 0xFF9CA4AFu},
    {11, 6, 2, 0xFFBBCADAu},
    {13, 6, 2, 0xFF9CA4AFu},
    {15, 6, 1, 0xFFBBCADAu},
    {17, 6, 1, 0xFF9CA4AFu},
    {19, 6, 1, 0xFFCBDFEBu},
    {20, 6, 2, 0xFFE2FAFEu},
    {22, 6, 1, 0xFFCBDFEBu},
    {23, 6, 1, 0xFFBBCADAu},
    {0, 7, 2, 0xFFBBCADAu},
    {2, 7, 1, 0xFFCBDFEBu},
    {3, 7, 1, 0xFFE2FAFEu},
    {4, 7, 1, 0xFFBBCADAu},
    {5, 7, 1, 0xFF9CA4AFu},
    {6, 7, 1, 0xFF818286u},
    {7, 7, 1, 0xFF9CA4AFu},
    {8, 7, 1, 0xFF818286u},
    {9, 7, 1, 0xFF625F6Bu},
    {10, 7, 1, 0xFF818286u},
    {11, 7, 2, 0xFFBBCADAu},
    {13, 7, 1, 0xFF818286u},
    {14, 7, 1, 0xFF625F6Bu},
    {15, 7, 1, 0xFF818286u},
    {16, 7, 1, 0xFF9CA4AFu},
    {17, 7, 1, 0xFF818286u},
    {18, 7, 1, 0xFF9CA4AFu},
    {19, 7, 1, 0xFFBBCADAu},
    {20, 7, 1, 0xFFE2FAFEu},
    {21, 7, 1, 0xFFCBDFEBu},
    {22, 7, 2, 0xFFBBCADAu},
    {0, 8, 1, 0xFF9CA4AFu},
    {1, 8, 1, 0xFFBBCADAu},
    {2, 8, 1, 0xFFCBDFEBu},
    {3, 8, 1, 0xFFBBCADAu},
    {4, 8, 1, 0xFF818286u},
    {5, 8, 1, 0xFF625F6Bu},
    {6, 8, 1, 0xFF818286u},
    {7, 8, 1, 0xFF717072u},
    {8, 8, 2, 0xFF625F6Bu},
    {10, 8, 1, 0xFF818286u},
    {11, 8, 2, 0xFFBBCADAu},
    {13, 8, 1, 0xFF818286u},
    {14, 8, 2, 0xFF625F6Bu},
    {16, 8, 1, 0xFF717072u},
    {17, 8, 1, 0xFF818286u},
    {18, 8, 1, 0xFF625F6Bu},
    {19, 8, 1, 0xFF818286u},
    {20, 8, 3, 0xFFBBCADAu},
    {23, 8, 1, 0xFF9CA4AFu},
    {1, 9, 1, 0xFF9CA4AFu},
    {2, 9, 1, 0xFFBBCADAu},
    {3, 9, 1, 0xFF9CA4AFu},
    {4, 9, 1, 0xFF3E3C50u},
    {5, 9, 2, 0xFF585454u},
    {7, 9, 2, 0xFF625F6Bu},
    {9, 9, 1, 0xFF585454u},
    {10, 9, 1, 0xFF818286u},
    {11, 9, 2, 0xFF9CA4AFu},
    {13, 9, 1, 0xFF818286u},
    {14, 9, 1, 0xFF585454u},
    {15, 9, 2, 0xFF625F6Bu},
    {17, 9, 2, 0xFF585454u},
    {19, 9, 1, 0xFF3E3C50u},
    {20, 9, 1, 0xFF9CA4AFu},
    {21, 9, 1, 0xFFBBCADAu},
    {22, 9, 1, 0xFF9CA4AFu},
    {2, 10, 1, 0xFF818286u},
    {3, 10, 1, 0xFF3E3C50u},
    {4, 10, 2, 0xFF1E1B1Cu},
    {6, 10, 1, 0xFF3E3C50u},
    {7, 10, 2, 0xFF585454u},
    {9, 10, 2, 0xFF3E3C50u},
    {11, 10, 1, 0xFF9CA4AFu},
    {12, 10, 1, 0xFF818286u},
    {13, 10, 2, 0xFF3E3C50u},
    {15, 10, 2, 0xFF585454u},
    {17, 10, 1, 0xFF3E3C50u},
    {18, 10, 2, 0xFF1E1B1Cu},
    {20, 10, 1, 0xFF3E3C50u},
    {21, 10, 1, 0xFF9CA4AFu},
    {3, 11, 3, 0xFF1E1B1Cu},
    {6, 11, 5, 0xFF3E3C50u},
    {11, 11, 1, 0xFFBBCADAu},
    {12, 11, 1, 0xFF9CA4AFu},
    {13, 11, 5, 0xFF3E3C50u},
    {18, 11, 3, 0xFF1E1B1Cu},
    {4, 12, 1, 0xFF1E1B1Cu},
    {5, 12, 1, 0xFF585454u},
    {6, 12, 1, 0xFF625F6Bu},
    {7, 12, 1, 0xFF585454u},
    {8, 12, 1, 0xFF3E3C50u},
    {9, 12, 1, 0xFF000100u},
    {10, 12, 1, 0xFF3E3C50u},
    {11, 12, 2, 0xFF9CA4AFu},
    {13, 12, 1, 0xFF3E3C50u},
    {14, 12, 1, 0xFF000100u},
    {15, 12, 1, 0xFF3E3C50u},
    {16, 12, 1, 0xFF585454u},
    {17, 12, 1, 0xFF625F6Bu},
    {18, 12, 1, 0xFF585454u},
    {19, 12, 1, 0xFF1E1B1Cu},
    {5, 13, 2, 0xFF585454u},
    {7, 13, 2, 0xFF717072u},
    {9, 13, 1, 0xFF585454u},
    {10, 13, 1, 0xFF1E1B1Cu},
    {11, 13, 2, 0xFF818286u},
    {13, 13, 1, 0xFF1E1B1Cu},
    {14, 13, 1, 0xFF585454u},
    {15, 13, 2, 0xFF717072u},
    {17, 13, 2, 0xFF585454u},
    {5, 14, 1, 0xFF585454u},
    {6, 14, 1, 0xFF1E1B1Cu},
    {7, 14, 1, 0xFF3E3C50u},
    {8, 14, 1, 0xFF585454u},
    {9, 14, 1, 0xFF717072u},
    {10, 14, 1, 0xFF3E3C50u},
    {11, 14, 1, 0xFF818286u},
    {12, 14, 1, 0xFF717072u},
    {13, 14, 1, 0xFF3E3C50u},
    {14, 14, 1, 0xFF717072u},
    {15, 14, 1, 0xFF585454u},
    {16, 14, 1, 0xFF3E3C50u},
    {17, 14, 1, 0xFF1E1B1Cu},
    {18, 14, 1, 0xFF585454u},
    {5, 15, 1, 0xFF625F6Bu},
    {6, 15, 1, 0xFF000000u},
    {7, 15, 1, 0xFF2846EBu},
    {8, 15, 3, 0xFF3E3C50u},
    {11, 15, 1, 0xFF818286u},
    {12, 15, 1, 0xFF717072u},
    {13, 15, 3, 0xFF3E3C50u},
    {16, 15, 1, 0xFF2846EBu},
    {17, 15, 1, 0xFF000000u},
    {18, 15, 1, 0xFF585454u},
    {4, 16, 1, 0xFF625F6Bu},
    {5, 16, 1, 0xFF717072u},
    {6, 16, 2, 0xFF1E1B1Cu},
    {8, 16, 1, 0xFF2846EBu},
    {9, 16, 1, 0xFF3E3C50u},
    {10, 16, 1, 0xFF000100u},
    {11, 16, 1, 0xFF818286u},
    {12, 16, 1, 0xFF717072u},
    {13, 16, 1, 0xFF000100u},
    {14, 16, 1, 0xFF3E3C50u},
    {15, 16, 1, 0xFF2846EBu},
    {16, 16, 2, 0xFF1E1B1Cu},
    {18, 16, 2, 0xFF625F6Bu},
    {4, 17, 2, 0xFF625F6Bu},
    {6, 17, 1, 0xFF717072u},
    {7, 17, 1, 0xFF3E3C50u},
    {8, 17, 1, 0xFF000000u},
    {9, 17, 1, 0xFF000100u},
    {10, 17, 1, 0xFF000000u},
    {11, 17, 2, 0xFF585454u},
    {13, 17, 1, 0xFF000000u},
    {14, 17, 1, 0xFF000100u},
    {15, 17, 1, 0xFF000000u},
    {16, 17, 1, 0xFF3E3C50u},
    {17, 17, 1, 0xFF717072u},
    {18, 17, 1, 0xFF585454u},
    {19, 17, 1, 0xFF625F6Bu},
    {5, 18, 3, 0xFF625F6Bu},
    {8, 18, 1, 0xFF000100u},
    {9, 18, 4, 0xFF000000u},
    {13, 18, 1, 0xFF3E3C50u},
    {14, 18, 1, 0xFF717072u},
    {15, 18, 1, 0xFF000000u},
    {16, 18, 1, 0xFF717072u},
    {17, 18, 2, 0xFF625F6Bu},
    {6, 19, 1, 0xFF625F6Bu},
    {7, 19, 1, 0xFF717072u},
    {8, 19, 1, 0xFF1E1B1Cu},
    {9, 19, 1, 0xFF000100u},
    {10, 19, 1, 0xFF1E1B1Cu},
    {11, 19, 1, 0xFF3E3C50u},
    {12, 19, 1, 0xFF1E1B1Cu},
    {13, 19, 2, 0xFF9CA4AFu},
    {15, 19, 1, 0xFF000000u},
    {16, 19, 1, 0xFF717072u},
    {17, 19, 1, 0xFF625F6Bu},
    {7, 20, 1, 0xFF717072u},
    {8, 20, 1, 0xFF1E1B1Cu},
    {9, 20, 1, 0xFF3E3C50u},
    {10, 20, 3, 0xFF585454u},
    {13, 20, 1, 0xFF625F6Bu},
    {14, 20, 1, 0xFF585454u},
    {15, 20, 1, 0xFF1E1B1Cu},
    {16, 20, 1, 0xFF717072u},
    {8, 21, 1, 0xFF585454u},
    {10, 21, 4, 0xFF3E3C50u},
    {15, 21, 1, 0xFF585454u},
    {11, 22, 2, 0xFF3E3C50u},
};

// Multiplies two 0xAABBGGRR colours channel by channel (a tint on the pixel icon).
uint32_t MultiplyColor(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const uint32_t ca = (a >> shift) & 0xffu, cb = (b >> shift) & 0xffu;
        out |= ((ca * cb + 127u) / 255u) << shift;
    }
    return out;
}

void DrawIconRectAbs(void* parentWidgetRaw, const void* iconRef16, float absX, float absY, float w, float h, uint32_t rgba, float alpha, float brightness = 1.0f) {
    if (!parentWidgetRaw || !iconRef16 || w <= 0.0f || h <= 0.0f || alpha <= 0.0f || Excluded(absX, absY, w, h)) return;
    if (UnderRing(absX, absY, w, h)) {
        if (g_ringQueue.size() < 4096) {
            RingDraw d{1, 0, {}, absX, absY, w, h, rgba, alpha, brightness};
            std::memcpy(d.iconRef, iconRef16, 16);
            g_ringQueue.push_back(d);
        }
        return;
    }
    // The Boss row's icon is drawn by the mod from kBossIconRuns (its icon GUID in ore_deposits.h is only a stand-in)
    if (iconRef16 == static_cast<const void*>(kOreIcons[kBossesFirst])) {
        const float px = w / kBossIconSize, py = h / kBossIconSize;
        for (const PixelRun& r : kBossIconRuns) {
            DrawSolidRectAbs(parentWidgetRaw, absX + r.x * px, absY + r.y * py, r.w * px + 0.35f, py + 0.35f,
                             rgba == 0xffffffffu ? r.color : MultiplyColor(r.color, rgba), alpha, brightness);
        }
        return;
    }
    const auto* bytes = static_cast<const uint64_t*>(iconRef16);
    if (bytes[0] == 0 && bytes[1] == 0) return;
    float rect[4] = {absX, absY, w, h};
    uint64_t packedCol = MakePackedColor(rgba, alpha, brightness);
    alignas(16) uint8_t tmp[16]{};
    g_drawImage(parentWidgetRaw, rect, g_image(g_uiCtx(), tmp, iconRef16), &packedCol);
}

// Draws one of the game's nine-slice boxes (kRoundedBoxPrimitive, kBoxInnerGlowPrimitive,
// kGlowBoxSmallPrimitive) at absolute UI coordinates; border is the corner size. The box takes its
// transparency from the colour's alpha byte. Without the game's nine-slice function a rounded box
// falls back to a flat rectangle and the glows are left out.
void DrawBoxAbs(void* parentWidgetRaw, size_t assetOffset, float absX, float absY, float w, float h,
                uint32_t rgba, float alpha, float border, float brightness = 1.0f) {
    uint8_t* assets = GetUiAssets();
    if (!assets || !parentWidgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.0f || Excluded(absX, absY, w, h)) return;
    if (UnderRing(absX, absY, w, h)) {
        if (g_ringQueue.size() < 4096) {
            RingDraw d{3, assetOffset, {}, absX, absY, w, h, rgba, alpha, brightness};
            d.border = border;
            g_ringQueue.push_back(std::move(d));
        }
        return;
    }
    if (!g_nineSlice) {
        if (assetOffset == kRoundedBoxPrimitive) {
            DrawSolidRectAbs(parentWidgetRaw, absX, absY, w, h, rgba, alpha, brightness);
        }
        return;
    }
    const float b = std::max(1.0f, std::min(border, std::min(w, h) * 0.5f));
    float rect[4] = {absX, absY, w, h};
    float borders[4] = {b, b, b, b};
    g_nineSlice(parentWidgetRaw, rect, assets + assetOffset, borders, MakePackedColor(rgba, alpha, brightness));
}

// A framed box: a rounded box in the frame colour with the fill colour 1px inside it.
void DrawFramedBoxAbs(void* parentWidgetRaw, float absX, float absY, float w, float h,
                      uint32_t fillRgba, float fillAlpha, uint32_t frameRgba, float frameAlpha, float border = 4.0f) {
    DrawBoxAbs(parentWidgetRaw, kRoundedBoxPrimitive, absX, absY, w, h, frameRgba, frameAlpha, border);
    DrawBoxAbs(parentWidgetRaw, kRoundedBoxPrimitive, absX + 1.0f, absY + 1.0f, w - 2.0f, h - 2.0f,
               fillRgba, fillAlpha, border - 1.0f);
}

enum class TextAlignH : uint8_t { Left = 1, Center = 2, Right = 3 };
enum class TextAlignV : uint8_t { Top = 1, Center = 2, Bottom = 3 };

// The game's white-to-transparent vertical gradient, UiTexture "gradient_white_alpha_to_bottom"
constexpr uint8_t kGradientToBottomTexture[16] = {0xf0, 0x6a, 0xae, 0x8f, 0xe8, 0x2a, 0xae, 0x4a,
                                                  0x8e, 0xbc, 0x5d, 0xaa, 0x33, 0xe0, 0xcc, 0x48};

// Draws a single text label at absolute UI coordinates (absX, absY, w, h)
// with explicit horizontal and vertical alignment inside the bounding box.
void DrawLabelAbs(float absX, float absY, float w, float h, const char* utf8,
                  uint32_t rgba = 0xffffffffu, float alpha = 1.0f,
                  TextAlignH hAlign = TextAlignH::Left, TextAlignV vAlign = TextAlignV::Center,
                  const uint8_t* style = nullptr, float fontScale = 1.0f) {
    uint8_t* assets = GetUiAssets();
    if (!assets || !utf8 || !*utf8 || Excluded(absX, absY, w, h)) return;
    if (UnderRing(absX, absY, w, h)) {
        if (g_ringQueue.size() < 4096) {
            RingDraw d{2, 0, {}, absX, absY, w, h, rgba, alpha, fontScale};
            d.text = utf8, d.hAlign = static_cast<int>(hAlign), d.vAlign = static_cast<int>(vAlign), d.style = style; d.opacity = g_queueOpacity;
            g_ringQueue.push_back(std::move(d));
        }
        return;
    }
    UiText text = MakeUiText(utf8);
    alignas(16) uint8_t label[0x400]{};
    g_beginLabel(label, &text, style ? style : assets + kSmallTextStyleOffset, 0);
    if (uint8_t* lh = *reinterpret_cast<uint8_t**>(label + 8)) {
        // Optical cap-height centering adjustment (-1.5px compensates for font top leading)
        const float opticalY = (vAlign == TextAlignV::Center) ? (absY - 1.5f) : absY;
        *reinterpret_cast<float*>(lh + 0x114) = absX;
        *reinterpret_cast<float*>(lh + 0x118) = opticalY;
        *reinterpret_cast<float*>(lh + 0x11c) = w;
        *reinterpret_cast<float*>(lh + 0x120) = h;
    }
    label[0x38] = static_cast<uint8_t>(hAlign);
    label[0x39] = static_cast<uint8_t>(vAlign);
    if (fontScale != 1.0f) {
        // The label's font size (+0x7c, from the game's text size setting, flag 8 at +0x90); the style's own size
        // field is only a cap
        *reinterpret_cast<float*>(label + 0x7c) *= fontScale;
        *reinterpret_cast<uint16_t*>(label + 0x90) |= 8;
    }
    label[0x3c] = 0; // Single-line (do not word-wrap)
    label[0x3d] = 1;
    *reinterpret_cast<uint32_t*>(label + 0xb0) = rgba;
    *reinterpret_cast<float*>(label + 0xb4) = alpha;
    g_endLabel(label);
}

// Map key colours (0xAABBGGRR, as the game's UI takes them): dark panels with gold frames, in the
// style of the game's own menus.
namespace key_style {
constexpr uint32_t kPanelFill = 0xff14100eu;     // #0e1014
constexpr uint32_t kGold = 0xff5ca4c9u;          // #c9a45c frames, glows, selected
constexpr uint32_t kGoldDim = 0xff345a6bu;       // #6b5a34 resting panel frame
constexpr uint32_t kTitle = 0xff9ad3e9u;         // #e9d39a headings
constexpr uint32_t kText = 0xffb6cfd9u;          // #d9cfb6 button labels
constexpr uint32_t kTextMuted = 0xff80949cu;     // #9c9480
constexpr uint32_t kButtonFill = 0xff1e1917u;    // #17191e
constexpr uint32_t kButtonFrame = 0xff30414au;   // #4a4130
constexpr uint32_t kActiveFill = 0xff14222au;    // #2a2214
constexpr uint32_t kActiveText = 0xffa6dff3u;    // #f3dfa6
constexpr uint32_t kHoverFill = 0xff121c21u;     // #211c12
constexpr uint32_t kCategoryFill = 0xff1d1816u;  // #16181d
constexpr uint32_t kCategoryFrame = 0xff222a2eu; // #2e2a22
constexpr uint32_t kOnFill = 0xff0f2018u;        // #18200f
constexpr uint32_t kOnFrame = 0xff447a5du;       // #5d7a44
constexpr uint32_t kOnText = 0xff8ee0b8u;        // #b8e08e
constexpr uint32_t kOffFrame = 0xff28363au;      // #3a3628
constexpr uint32_t kOffText = 0xff66747au;       // #7a7466
constexpr uint32_t kDotOn = 0xff6ad18fu;         // #8fd16a
constexpr uint32_t kDotOff = 0xff3c464au;        // #4a463c
constexpr uint32_t kBadgeFill = 0xff3e3a2bu;     // #2b3a3e
constexpr uint32_t kBadgeRing = 0xff9ac8d8u;     // #d8c89a
} // namespace key_style

// A map key button: dark rounded box in a bronze frame; gold frame when hovered, gold frame with
// an inner gold glow when it is the active choice.
void DrawKeyButtonAbs(void* parentWidgetRaw, float absX, float absY, float w, float h, bool hovered, bool active) {
    using namespace key_style;
    const uint32_t fill = active ? kActiveFill : (hovered ? kHoverFill : kButtonFill);
    const uint32_t frame = (active || hovered) ? kGold : kButtonFrame;
    DrawFramedBoxAbs(parentWidgetRaw, absX, absY, w, h, fill, 0.96f, frame, active ? 0.95f : 0.85f);
    if (active) {
        DrawBoxAbs(parentWidgetRaw, kBoxInnerGlowPrimitive, absX + 1.0f, absY + 1.0f, w - 2.0f, h - 2.0f,
                   kGold, 0.45f, 6.0f);
    }
}

// Width of the ON / OFF badge: fits the longer of the two words in the current language, so every
// row's badge lines up (50 in English, where "OFF" is ~38 at the game's text size).
float OnOffBadgeWidth() {
    const float longer = std::max(umm_lang::TextWidth(umm_lang::T(umm_lang::Str::On)),
                                  umm_lang::TextWidth(umm_lang::T(umm_lang::Str::Off)));
    return std::max(50.0f, longer + 14.0f);
}

// The ON / OFF badge of rows and category headers, right-aligned in the box ending at rightX.
void DrawOnOffBadgeAbs(void* parentWidgetRaw, float rightX, float absY, float rowH, bool on, float alpha = 1.0f) {
    using namespace key_style;
    const float bw = OnOffBadgeWidth();
    const float bh = std::min(18.0f, rowH - 3.0f);
    const float bx = rightX - bw;
    const float by = absY + (rowH - bh) * 0.5f;
    if (on) {
        DrawFramedBoxAbs(parentWidgetRaw, bx, by, bw, bh, kOnFill, 0.95f * alpha, kOnFrame, 0.95f * alpha, 3.0f);
    } else {
        DrawFramedBoxAbs(parentWidgetRaw, bx, by, bw, bh, kPanelFill, 0.6f * alpha, kOffFrame, 0.9f * alpha, 3.0f);
    }
    DrawLabelAbs(bx, by, bw, bh, umm_lang::T(on ? umm_lang::Str::On : umm_lang::Str::Off),
                 on ? kOnText : kOffText, (on ? 0.98f : 0.8f) * alpha,
                 TextAlignH::Center, TextAlignV::Center, nullptr, kBadgeFontScale);
}

// The game's padlock (UiTexture "locked"), drawn left of a row's ON / OFF badge.
constexpr uint8_t kPadlockIcon[16] = {0x22, 0x65, 0x3d, 0xf8, 0xc1, 0xd0, 0xcf, 0x4b,
                                      0x89, 0x16, 0xe7, 0x94, 0x79, 0xa0, 0x10, 0xaa};
constexpr float kLockSize = 14.0f, kLockGap = 5.0f;
float LockSlotWidth() { return kLockSize + kLockGap; }
float LockLeftX(float badgeRightX) { return badgeRightX - OnOffBadgeWidth() - kLockGap - kLockSize; }

// Gold while locked (dimmer when the lock comes from the row's category), a faint grey one on a hovered row to
// show where to click; nothing otherwise.
void DrawFilterLockAbs(void* parentWidgetRaw, float badgeRightX, float absY, float rowH, bool locked, bool inherited,
                       bool rowHovered, bool lockHovered) {
    using namespace key_style;
    if (!locked && !inherited && !rowHovered) return;
    const float x = LockLeftX(badgeRightX), y = absY + (rowH - kLockSize) * 0.5f;
    uint32_t tint = 0xff8c9296u;
    float alpha = lockHovered ? 0.95f : 0.45f;
    if (locked) {
        tint = lockHovered ? kActiveText : kTitle;
        alpha = 1.0f;
    } else if (inherited) {
        tint = kGold;
        alpha = lockHovered ? 0.95f : 0.55f;
    }
    DrawIconRectAbs(parentWidgetRaw, kPadlockIcon, x, y, kLockSize, kLockSize, tint, alpha);
}

// A Map Key category bar in the slate indigo of the game's menu headers: darker at the top, lighter below
// (the game's gradient texture over a flat fill), in a frame that turns gold on hover.
void DrawCategoryBarAbs(void* widgetRaw, float x, float y, float w, float h, bool hovered) {
    using namespace key_style;
    DrawFramedBoxAbs(widgetRaw, x, y, w, h, 0xff382624u, 0.97f, hovered ? kGold : 0xff6e5050u,
                     hovered ? 0.85f : 0.9f, 4.0f); // #242638 fill, #50506e frame
    DrawIconRectAbs(widgetRaw, kGradientToBottomTexture, x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f,
                    hovered ? 0xff6a4c4cu : 0xff5c4242u, 0.85f); // #42425c, #4c4c6a on hover
}

// The Map Key category of one of the game's filter rows: the game's own, except Water Source, shown with the world.
uint8_t FilterCategory(const FilterEntry& e) {
    return e.typeHash == kWaterSourceTypeHash ? 3 : e.category;
}

bool IsFilterEnabled(FilterEntry* entries, size_t count, uint32_t typeHash) {
    if (typeHash == kGoldChestTypeHash) return g_goldChestEnabled;
    if (typeHash == kSilverChestTypeHash) return g_silverChestEnabled;
    if (typeHash == kWoodChestTypeHash) return g_woodChestEnabled;
    if (typeHash == kSpoilsTypeHash) return g_spoilsEnabled;
    if (typeHash == kOtherPlayerTypeHash) return g_otherPlayersEnabled;
    if (typeHash == kTownPortalTypeHash) return g_townPortalsEnabled;
    if (typeHash == kCompletedQuestTypeHash) return false; // finished quests are never shown
    if (IsOreHash(typeHash)) return g_oreEnabled[OreIndexOf(typeHash)];
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].typeHash == typeHash) {
            return entries[i].enabled != 0;
        }
    }
    return true;
}

void ApplyFilterChange(uint8_t* client, uint32_t typeHash, bool enable) {
    if (typeHash == kGoldChestTypeHash || typeHash == kSilverChestTypeHash || typeHash == kWoodChestTypeHash ||
        typeHash == kSpoilsTypeHash || typeHash == kOtherPlayerTypeHash || typeHash == kTownPortalTypeHash ||
        typeHash == kCompletedQuestTypeHash || IsOreHash(typeHash)) {
        return;
    }
    uint8_t disableFlag = enable ? 0 : 1;
    void* mgr = client ? *reinterpret_cast<void**>(client + 0x4667f0) : nullptr;
    if (g_applyTypeFilterDirect && mgr) {
        g_applyTypeFilterDirect(mgr, typeHash, disableFlag);
    } else if (g_setTypeFilter && GetUiG()) {
        g_setTypeFilter(GetUiG(), typeHash, disableFlag);
    }
}

void SaveMinimapConfig();

void SetSingleFilterState(uint8_t* client, uint8_t* mapState, uint32_t typeHash, bool enable) {
    if (!client) return;
    if (typeHash == kGoldChestTypeHash) {
        g_goldChestEnabled = enable;
    } else if (typeHash == kSilverChestTypeHash) {
        g_silverChestEnabled = enable;
    } else if (typeHash == kWoodChestTypeHash) {
        g_woodChestEnabled = enable;
    } else if (typeHash == kSpoilsTypeHash) {
        g_spoilsEnabled = enable;
    } else if (typeHash == kOtherPlayerTypeHash) {
        g_otherPlayersEnabled = enable;
        SaveMinimapConfig();
    } else if (typeHash == kTownPortalTypeHash) {
        g_townPortalsEnabled = enable;
        SaveMinimapConfig();
    } else if (IsOreHash(typeHash)) {
        g_oreEnabled[OreIndexOf(typeHash)] = enable;
        SaveMinimapConfig();
    } else {
        auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
        size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].typeHash == typeHash) {
                entries[i].enabled = enable ? 1 : 0;
            }
        }
        ApplyFilterChange(client, typeHash, enable);
    }
    if (typeHash == kGoldChestTypeHash || typeHash == kSilverChestTypeHash || typeHash == kWoodChestTypeHash ||
        typeHash == kSpoilsTypeHash)
        SaveMinimapConfig();
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
    }
    if (FilterLockCount() > 0) SaveFilterLocks();
}

// A category's switch: every row in it, the locked ones too (a click on the switch is a click on each of its rows; a
// row locked ON kept the switch ON for good).
void SetCategoryFilterState(uint8_t* client, uint8_t* mapState, uint8_t category, bool enable) {
    if (!client) return;
    if (category == 3) {
        g_goldChestEnabled = enable;
        g_silverChestEnabled = enable;
        g_woodChestEnabled = enable;
        g_spoilsEnabled = enable;
    } else if (category == 0) {
        g_otherPlayersEnabled = enable;
        SaveMinimapConfig();
    } else if (category == 1) {
        g_townPortalsEnabled = enable;
        SaveMinimapConfig();
    }
    // Resource rows: the resource groups, and the Return Beacons in Shroud
    bool anyResource = false;
    for (size_t o = 0; o < kOreCount; ++o) {
        if (kOreCategory[o] == category) {
            g_oreEnabled[o] = enable;
            anyResource = true;
        }
    }
    if (anyResource) SaveMinimapConfig();
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    for (size_t i = 0; i < count; ++i) {
        if (FilterCategory(entries[i]) == category) {
            entries[i].enabled = enable ? 1 : 0;
            ApplyFilterChange(client, entries[i].typeHash, enable);
        }
    }
    if (category == 3) SaveMinimapConfig();
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
    }
    if (FilterLockCount() > 0) SaveFilterLocks();
}

// Show All / Hide All: every row, except the locked rows and categories unless passLocks (the buttons held down).
void SetAllFiltersState(uint8_t* client, uint8_t* mapState, bool enable, bool passLocks = false) {
    if (!client) return;
    auto open = [&](uint32_t typeHash, uint8_t category) { return passLocks || !IsFilterLocked(typeHash, category); };
    if (open(kGoldChestTypeHash, 3)) g_goldChestEnabled = enable;
    if (open(kSilverChestTypeHash, 3)) g_silverChestEnabled = enable;
    if (open(kWoodChestTypeHash, 3)) g_woodChestEnabled = enable;
    if (open(kSpoilsTypeHash, 3)) g_spoilsEnabled = enable;
    if (open(kOtherPlayerTypeHash, 0)) g_otherPlayersEnabled = enable;
    if (open(kTownPortalTypeHash, 1)) g_townPortalsEnabled = enable;
    for (size_t o = 0; o < kOreCount; ++o) {
        if (open(OreHash(o), kOreCategory[o])) g_oreEnabled[o] = enable;
    }
    SaveMinimapConfig();
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    for (size_t i = 0; i < count; ++i) {
        if (!open(entries[i].typeHash, FilterCategory(entries[i]))) continue;
        entries[i].enabled = enable ? 1 : 0;
        ApplyFilterChange(client, entries[i].typeHash, enable);
    }
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
    }
    if (FilterLockCount() > 0) SaveFilterLocks();
}

// Resource rows a game row switches with it (Water Source's wells and barrels) follow that row's state, which the
// game keeps; called once the world is up.
void SyncLinkedResourceRows(uint8_t* client) {
    if (!client) return;
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    if (count == 0) return;
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            const auto& row = grp.rows[r];
            if (IsOreHash(row.typeHash)) continue;
            const bool on = IsFilterEnabled(entries, count, row.typeHash);
            for (uint32_t linked : {row.pairedHash, row.pairedHash2}) {
                if (IsOreHash(linked)) g_oreEnabled[OreIndexOf(linked)] = on;
            }
        }
    }
}

// Ctrl+Z / Ctrl+Y on the World Map: undo and redo filter changes. Every frame the map is up, the filters' ON / OFF
// are compared with the last frame's; a change from anywhere (a row, a section, Show All, a marker's Hide button) is
// one step. Another world's filters start the history over.
struct FilterSnapshot {
    std::vector<std::pair<uint32_t, bool>> states; // the mod's own rows first, then the game's entries in order
    uint64_t seq = 0; // in the undo / redo history: when it got there (g_historySeq), against the marks' steps
    bool operator==(const FilterSnapshot& o) const { return states == o.states; }
};

FilterSnapshot TakeFilterSnapshot(uint8_t* client) {
    FilterSnapshot s;
    s.states.reserve(160);
    s.states.emplace_back(kGoldChestTypeHash, g_goldChestEnabled);
    s.states.emplace_back(kSilverChestTypeHash, g_silverChestEnabled);
    s.states.emplace_back(kWoodChestTypeHash, g_woodChestEnabled);
    s.states.emplace_back(kOtherPlayerTypeHash, g_otherPlayersEnabled);
    s.states.emplace_back(kTownPortalTypeHash, g_townPortalsEnabled);
    s.states.emplace_back(kSpoilsTypeHash, g_spoilsEnabled);
    for (size_t o = 0; o < kOreCount; ++o) s.states.emplace_back(OreHash(o), g_oreEnabled[o]);
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    for (size_t i = 0; i < count; ++i) s.states.emplace_back(entries[i].typeHash, entries[i].enabled != 0);
    return s;
}

// Same rows in the same order: the same world's filters
bool SameFilterRows(const FilterSnapshot& a, const FilterSnapshot& b) {
    if (a.states.size() != b.states.size()) return false;
    for (size_t i = 0; i < a.states.size(); ++i) {
        if (a.states[i].first != b.states[i].first) return false;
    }
    return true;
}

void RestoreFilterSnapshot(uint8_t* client, uint8_t* mapState, const FilterSnapshot& s) {
    const FilterSnapshot now = TakeFilterSnapshot(client);
    if (!SameFilterRows(now, s)) return;
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t fixed = 6 + kOreCount;
    for (size_t i = 0; i < s.states.size(); ++i) {
        const auto [hash, on] = s.states[i];
        if (now.states[i].second == on) continue;
        if (i == 0) g_goldChestEnabled = on;
        else if (i == 1) g_silverChestEnabled = on;
        else if (i == 2) g_woodChestEnabled = on;
        else if (i == 3) g_otherPlayersEnabled = on;
        else if (i == 4) g_townPortalsEnabled = on;
        else if (i == 5) g_spoilsEnabled = on;
        else if (i < fixed) g_oreEnabled[i - 6] = on;
        else {
            entries[i - fixed].enabled = on ? 1 : 0;
            ApplyFilterChange(client, hash, on);
        }
    }
    SaveMinimapConfig();
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
    }
    if (FilterLockCount() > 0) SaveFilterLocks();
}

// The message on the World Map for a hide click or an undo / redo: which rows changed and how (filter_notice.h).
const char* RowText(const MarkerRowSpec& row);
std::string g_filterNotice;
uint64_t g_filterNoticeTick = 0;
bool g_filterNoticeOnHud = false; // from a click on the minimap: shown beside it, not on the World Map

void ShowMapNotice(std::string text) {
    if (text.empty()) return;
    if (g_log) g_log("Map: notice: %s", text.c_str());
    g_filterNotice = std::move(text);
    g_filterNoticeTick = GetTickCount64();
    g_filterNoticeOnHud = false;
}

// A Ctrl + click's mark. Words: 0 looted, 1 read, 2 beaten, from the Settings line.
void AnnounceMark(const char* name, size_t word, bool on) {
    if (!name) return;
    const std::string state = filter_notice::WordOf(umm_lang::T(umm_lang::Str::LootedChests), word);
    ShowMapNotice(filter_notice::Mark(name, state, umm_lang::T(on ? umm_lang::Str::On : umm_lang::Str::Off)));
}

// Ctrl + click on a marker that can't be marked: "Elixir Well / Root - Can't be marked"
const char* FilterRowName(uint32_t hash, const void** key = nullptr); // below
void AnnounceCantMark(uint32_t rowHash) {
    const char* name = FilterRowName(rowHash);
    ShowMapNotice(std::string(name ? name : "") + (name ? " - " : "") + umm_lang::T(umm_lang::Str::CantMark));
}

// A place marked done by hand, or the mark cleared: "Night Sanctum - Done: ON"
void AnnounceMarkDone(const char* name, bool on) {
    if (name) ShowMapNotice(filter_notice::Mark(name, umm_lang::T(umm_lang::Str::PlaceDone), umm_lang::T(on ? umm_lang::Str::On : umm_lang::Str::Off)));
}

void AnnounceMarkState(const char* name, umm_lang::Str state) {
    if (name) ShowMapNotice(filter_notice::Mark(name, umm_lang::T(state)));
}

// The name a row shows for one of its types, and that row: null when no row has the type
const char* FilterRowName(uint32_t hash, const void** key) {
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            const MarkerRowSpec& row = grp.rows[r];
            if (row.typeHash != hash && row.pairedHash != hash && row.pairedHash2 != hash) continue;
            if (key) *key = &row;
            return RowText(row);
        }
    }
    return nullptr;
}

void AnnounceFilterChange(const FilterSnapshot& before, const FilterSnapshot& after) {
    if (!SameFilterRows(before, after)) return;
    std::vector<std::pair<std::string, bool>> changes;
    std::vector<const void*> named; // a row switching several types counts once
    for (size_t i = 0; i < after.states.size(); ++i) {
        const auto [hash, on] = after.states[i];
        if (before.states[i].second == on) continue;
        const void* key = nullptr;
        const char* name = FilterRowName(hash, &key);
        if (!name && std::find(std::begin(kCustomPinHashes), std::end(kCustomPinHashes), hash) !=
                         std::end(kCustomPinHashes)) {
            key = kCustomPinHashes; // the map key has one line for all of them
            name = umm_lang::T(umm_lang::Str::CustomPin);
        }
        if (!name || std::find(named.begin(), named.end(), key) != named.end()) continue;
        named.push_back(key);
        changes.emplace_back(name, on);
    }
    ShowMapNotice(filter_notice::Format(changes, umm_lang::T(umm_lang::Str::On), umm_lang::T(umm_lang::Str::Off)));
}

std::vector<FilterSnapshot> g_filterUndo, g_filterRedo;
FilterSnapshot g_filterLastSeen;

// Ctrl + click marks (a chest looted, a note read, a boss beaten, an item found, a place done) share Ctrl+Z / Ctrl+Y
// with the filters: each is a step, and undo / redo takes the latest of either kind (by seq). Marks toggle, so undoing
// or redoing one is the same toggle again at the same spot (a place by its marker's position). Kept for the session.
struct MarkStep {
    int kind;            // 1 boss, 2 unique item, 3 lore note, 4 chest, 5 place
    float wx, wz;        // the click (a place: its marker)
    float r, shift, chestR;
    bool onMap;          // from the World Map (only what it draws can be marked), else the minimap
    uint64_t seq;
};
std::vector<MarkStep> g_markUndo, g_markRedo;
#ifdef UMM_DEV_BRIDGE
std::atomic<int> g_devHistoryStep{0}; // 1 undo, 2 redo, from the bridge
#endif
uint64_t g_historySeq = 1;
bool ToggleChestClearedNearWorldPos(float wx, float wz, float maxRadiusMeters);
bool TogglePlaceDone(int32_t x, int32_t z, bool& nowDone);
extern bool g_markClickOnWorldMap;
void AnnounceMarkDone(const char* name, bool on);
const char* PlaceRowNameAt(uint8_t* client, int32_t x, int32_t z);

// A mark just made: a new step, and nothing left to redo
void NoteMarkStep(int kind, float wx, float wz, float r, float shift, float chestR, bool onMap) {
    if (!kind) return;
    g_markUndo.push_back({kind, wx, wz, r, shift, chestR, onMap, ++g_historySeq});
    if (g_markUndo.size() > 50) g_markUndo.erase(g_markUndo.begin());
    g_markRedo.clear();
    g_filterRedo.clear();
}

// The same toggle again (undo or redo)
bool ReplayMark(uint8_t* client, const MarkStep& s) {
    g_markClickOnWorldMap = s.onMap;
    bool done = false;
    switch (s.kind) {
    case 1: done = ToggleManualBossNear(s.wx, s.wz, s.r, s.shift); break;
    case 2: done = ToggleUniqueFoundNear(s.wx, s.wz, s.r, s.shift); break;
    case 3: done = ToggleLoreReadNear(s.wx, s.wz, s.r, s.shift, s.chestR); break;
    case 4: done = ToggleChestClearedNearWorldPos(s.wx, s.wz, s.chestR); break;
    case 5: {
        bool nowDone = false;
        done = TogglePlaceDone(static_cast<int32_t>(s.wx), static_cast<int32_t>(s.wz), nowDone);
        if (done) AnnounceMarkDone(PlaceRowNameAt(client, static_cast<int32_t>(s.wx), static_cast<int32_t>(s.wz)), nowDone);
        break;
    }
    default: break;
    }
    g_markClickOnWorldMap = false;
    g_chestLootDirty = true;
    return done;
}

// The history is kept in filter_history.txt, so Ctrl+Z still reaches back after a hot reload or a restart: the rows'
// hashes once, then each snapshot as one 0/1 per row (the last one seen, the undo steps oldest first, the redo steps).
// A file for other rows (another world, a build with new rows) starts the history over like a world change.
std::string FilterHistoryPath() { return greeneye::DataPath("filter_history.txt"); }

void SaveFilterHistory() {
    if (g_filterLastSeen.states.empty()) return;
    std::string out = "UMMFH1\n";
    char hex[12];
    for (const auto& [hash, on] : g_filterLastSeen.states) {
        std::snprintf(hex, sizeof(hex), "%08x,", hash);
        out += hex;
    }
    out += '\n';
    auto bits = [&](char tag, const FilterSnapshot& f) {
        out += tag;
        out += ' ';
        for (const auto& [hash, on] : f.states) out += on ? '1' : '0';
        out += '\n';
    };
    bits('L', g_filterLastSeen);
    for (const auto& f : g_filterUndo) bits('U', f);
    for (const auto& f : g_filterRedo) bits('R', f);
    FILE* f = nullptr;
    if (fopen_s(&f, FilterHistoryPath().c_str(), "wb") == 0 && f) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

void LoadFilterHistory() {
    FILE* f = nullptr;
    if (fopen_s(&f, FilterHistoryPath().c_str(), "rb") != 0 || !f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        const size_t end = text.find('\n', at);
        lines.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? text.size() : end + 1;
    }
    if (lines.size() < 3 || lines[0] != "UMMFH1") return;
    std::vector<uint32_t> hashes;
    for (size_t at = 0; at < lines[1].size();) {
        const size_t comma = lines[1].find(',', at);
        if (comma == std::string::npos) break;
        hashes.push_back(static_cast<uint32_t>(std::strtoul(lines[1].substr(at, comma - at).c_str(), nullptr, 16)));
        at = comma + 1;
    }
    FilterSnapshot last;
    std::vector<FilterSnapshot> undo, redo;
    for (size_t i = 2; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.size() != hashes.size() + 2 || l[1] != ' ') continue;
        FilterSnapshot snap;
        snap.states.reserve(hashes.size());
        for (size_t r = 0; r < hashes.size(); ++r) snap.states.emplace_back(hashes[r], l[2 + r] == '1');
        if (l[0] == 'L') last = std::move(snap);
        else if (l[0] == 'U') undo.push_back(std::move(snap));
        else if (l[0] == 'R') redo.push_back(std::move(snap));
    }
    if (last.states.empty()) return;
    g_filterLastSeen = std::move(last);
    g_filterUndo = std::move(undo);
    g_filterRedo = std::move(redo);
    if (g_log) g_log("Map Key: filter history loaded (%zu to undo, %zu to redo)", g_filterUndo.size(), g_filterRedo.size());
}

std::atomic<bool> g_padKeyFocus{false};

// Once a frame with the map up: notes a change, and runs Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) or their buttons when keysFree
void UpdateFilterHistory(uint8_t* client, uint8_t* mapState, bool keysFree) {
    constexpr size_t kMaxSteps = 50;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        LoadFilterHistory();
    }
    FilterSnapshot now = TakeFilterSnapshot(client);
    bool changed = false;
    if (!SameFilterRows(now, g_filterLastSeen)) {
        changed = !g_filterUndo.empty() || !g_filterRedo.empty() || !g_filterLastSeen.states.empty();
        g_filterUndo.clear();
        g_filterRedo.clear();
    } else if (!(now == g_filterLastSeen)) {
        g_filterLastSeen.seq = ++g_historySeq;
        g_filterUndo.push_back(std::move(g_filterLastSeen));
        if (g_filterUndo.size() > kMaxSteps) g_filterUndo.erase(g_filterUndo.begin());
        g_filterRedo.clear();
        g_markRedo.clear();
        changed = true;
    }
    const bool first = g_filterLastSeen.states.empty();
    g_filterLastSeen = now;
    if (changed || first) SaveFilterHistory();

    static bool undoWasDown = false, redoWasDown = false;
    const inp::PadState pad = keysFree && !g_padKeyFocus ? inp::ReadPad() : inp::PadState{};
    const bool undoDown = keysFree && (inp::KeysHeld(g_keys.undo, KeyIsDown) || inp::PadHeld(g_keys.undo, pad));
    const bool redoDown = keysFree && (inp::KeysHeld(g_keys.redo, KeyIsDown) || inp::PadHeld(g_keys.redo, pad));
    bool undo = undoDown && !undoWasDown;
    bool redo = redoDown && !redoWasDown;
#ifdef UMM_DEV_BRIDGE
    if (const int dev = g_devHistoryStep.exchange(0)) (dev == 1 ? undo : redo) = true; // mapfilters undo / redo
#endif
    undoWasDown = undoDown;
    redoWasDown = redoDown;
    if (!undo && !redo) return;
    // A mark's step, if it is later than the filters' one
    {
        auto& mFrom = undo ? g_markUndo : g_markRedo;
        auto& mTo = undo ? g_markRedo : g_markUndo;
        const auto& fFrom = undo ? g_filterUndo : g_filterRedo;
        if (!mFrom.empty() && (fFrom.empty() || mFrom.back().seq > fFrom.back().seq)) {
            MarkStep s = mFrom.back();
            mFrom.pop_back();
            ReplayMark(client, s);
            s.seq = ++g_historySeq;
            mTo.push_back(s);
            if (mapState) {
                *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0; // the marker pass runs again
                *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
            }
            if (g_log) g_log("Map Key: %s a mark (kind %d)", undo ? "undid" : "redid", s.kind);
            return;
        }
    }
    auto& from = undo ? g_filterUndo : g_filterRedo;
    auto& to = undo ? g_filterRedo : g_filterUndo;
    if (from.empty()) return;
    now.seq = ++g_historySeq;
    to.push_back(now);
    RestoreFilterSnapshot(client, mapState, from.back());
    from.pop_back();
    g_filterLastSeen = TakeFilterSnapshot(client);
    AnnounceFilterChange(now, g_filterLastSeen);
    SaveFilterHistory();
    if (g_log) g_log("Map Key: %s a filter change (%zu more to undo, %zu to redo)", undo ? "undid" : "redid",
                     g_filterUndo.size(), g_filterRedo.size());
}

// ---- Filter presets: five slots on the Map Key under Show All / Hide All. A click applies one (one undo step, locked
// rows included), Ctrl + click saves the filters shown into it, a right click renames it; Alt+1..5 on the map,
// LT + Left / Right on a controller. A slot nobody saved is a built-in default, worked out from the rows each time.
// Saved in [Presets] of minimap_config.ini by type hash: a row the preset doesn't know keeps its state.
std::string GetMinimapConfigPath();
constexpr int kPresetCount = 5;
struct PresetSettings {
    bool has = false; // saved with the preset (a built-in one leaves the settings as they are)
    int fog = 0, pins = 0, markerScale = 2, looted = 0, levelTint = 50;
    bool hideCompleted = false, regionFilter = false, regionHide = false, regionNames = true, levelAreas = false;
    uint32_t regionMask = 0xffffffffu;
    bool operator==(const PresetSettings& o) const {
        return has == o.has && fog == o.fog && pins == o.pins && markerScale == o.markerScale && looted == o.looted &&
               levelTint == o.levelTint && hideCompleted == o.hideCompleted && regionFilter == o.regionFilter &&
               regionHide == o.regionHide && regionNames == o.regionNames && levelAreas == o.levelAreas &&
               regionMask == o.regionMask;
    }
};
PresetSettings CurrentPresetSettings();                              // defined with the settings card
void ApplyPresetSettings(uint8_t* client, const PresetSettings& s); // the same
struct FilterPreset {
    std::string name;                         // empty: the default's name in the current language
    std::unordered_map<uint32_t, bool> states; // empty: the built-in default
    PresetSettings settings;
};
FilterPreset g_presets[kPresetCount];
bool g_presetsLoaded = false;
int g_presetRename = -1;      // the slot being renamed, -1 none
std::string g_presetRenameText;
int g_presetLastApplied = -1; // for the controller's next / previous
constexpr umm_lang::Str kPresetDefaultNames[kPresetCount] = {umm_lang::Str::PresetChests, umm_lang::Str::PresetMining,
                                                            umm_lang::Str::PresetExploring, umm_lang::Str::PresetBosses,
                                                            umm_lang::Str::PresetPlants};

const char* PresetName(int p) {
    return g_presets[p].name.empty() ? umm_lang::T(kPresetDefaultNames[p]) : g_presets[p].name.c_str();
}

std::string FillName(umm_lang::Str fmt, const char* name) {
    std::string s = umm_lang::T(fmt);
    const size_t at = s.find("%s");
    if (at != std::string::npos) s.replace(at, 2, name);
    return s;
}

// The Map Key category of row i of a FilterSnapshot (TakeFilterSnapshot's order)
uint8_t SnapshotRowCategory(uint8_t* client, size_t i) {
    if (i < 3 || i == 5) return 3; // gold, silver, wooden chests, Dungeon Spoils
    if (i == 3) return 0;          // other players
    if (i == 4) return 1;          // Town Portals
    if (i < 6 + kOreCount) return kOreCategory[i - 6];
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    return FilterCategory(entries[i - 6 - kOreCount]);
}

// The built-in defaults. Every one keeps other players, Town Portals, custom pins and the main quest on.
bool DefaultPresetOn(int p, uint32_t hash, uint8_t cat) {
    auto ore = [](size_t o) { return kOreTypeHashBase + static_cast<uint32_t>(o); };
    if (hash == kOtherPlayerTypeHash || hash == kTownPortalTypeHash || cat == 4 || hash == 0xa7cadc08u) return true;
    switch (p) {
    case 0: // Chests: every chest, the end-room spoils, and the unique items and collectibles found in them
        return hash == kGoldChestTypeHash || hash == kSilverChestTypeHash || hash == kWoodChestTypeHash ||
               hash == kSpoilsTypeHash || hash == ore(kUniqueFirst) || hash == ore(kUniqueFirst + 1) ||
               hash == ore(kUniqueFirst + 2) || hash == ore(kCosmeticFirst);
    case 1: return cat == kMiningCategory || cat == kMaterialsCategory;
    case 2: return cat == 0 || cat == 1 || cat == 2 || cat == 3; // places, quests and lore, chests
    case 3: return hash == ore(kBossesFirst) || cat == kCreaturesCategory;
    case 4: return cat == kPlantsCategory || cat == kBaitCategory;
    }
    return false;
}

// The filters slot p would give, from the filters now. Locked rows too: a preset is a whole choice (Show All and Hide
// All skip them; a lock then keeps what the preset set, RestoreFilterSnapshot saves the locked states)
FilterSnapshot PresetTarget(uint8_t* client, int p, const FilterSnapshot& now) {
    FilterSnapshot t = now;
    const auto& saved = g_presets[p].states;
    for (size_t i = 0; i < t.states.size(); ++i) {
        auto& [hash, on] = t.states[i];
        const uint8_t cat = SnapshotRowCategory(client, i);
        if (saved.empty()) {
            on = DefaultPresetOn(p, hash, cat);
        } else {
            const auto it = saved.find(hash);
            if (it != saved.end()) on = it->second;
        }
    }
    return t;
}

void LoadFilterPresets() {
    if (g_presetsLoaded) return;
    g_presetsLoaded = true;
    async_io::Flush();
    const std::string cfg = GetMinimapConfigPath();
    std::vector<char> buf(32768);
    for (int p = 0; p < kPresetCount; ++p) {
        char key[24];
        std::snprintf(key, sizeof(key), "Preset%dName", p + 1);
        char name[128]{};
        GetPrivateProfileStringA("Presets", key, "", name, sizeof(name), cfg.c_str());
        g_presets[p].name = name;
        std::snprintf(key, sizeof(key), "Preset%d", p + 1);
        buf[0] = 0;
        GetPrivateProfileStringA("Presets", key, "", buf.data(), static_cast<DWORD>(buf.size()), cfg.c_str());
        std::snprintf(key, sizeof(key), "Preset%dSettings", p + 1);
        char set[160]{};
        GetPrivateProfileStringA("Presets", key, "", set, sizeof(set), cfg.c_str());
        PresetSettings& st = g_presets[p].settings;
        st = PresetSettings{};
        int v[11]{};
        unsigned mask = 0;
        // fog,pins,markerScale,looted,hideCompleted,regionFilter,regionMask(hex),regionHide,regionNames,levelAreas,tint
        if (sscanf_s(set, "%d,%d,%d,%d,%d,%d,%x,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &mask, &v[7],
                     &v[8], &v[9], &v[10]) == 11) {
            st.has = true;
            st.fog = v[0];
            st.pins = v[1];
            st.markerScale = std::max(0, std::min(kMarkerScaleCount - 1, v[2]));
            st.looted = v[3] ? 1 : 0;
            st.hideCompleted = v[4] != 0;
            st.regionFilter = v[5] != 0;
            st.regionMask = mask;
            st.regionHide = v[7] != 0;
            st.regionNames = v[8] != 0;
            st.levelAreas = v[9] != 0;
            st.levelTint = v[10];
        }
        g_presets[p].states.clear();
        // hhhhhhhh1,hhhhhhhh0,...: a type hash and ON (1) / OFF (0)
        for (const char* s = buf.data(); *s;) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(s, &end, 16);
            if (end == s || end - s != 9) break;
            g_presets[p].states[static_cast<uint32_t>(v >> 4)] = (v & 0xf) != 0;
            s = *end == ',' ? end + 1 : end;
        }
    }
}

void SaveFilterPreset(int p) {
    const std::string cfg = GetMinimapConfigPath();
    char key[24];
    std::snprintf(key, sizeof(key), "Preset%dName", p + 1);
    async_io::IniWrite("Presets", key, g_presets[p].name.empty() ? nullptr : g_presets[p].name.c_str(), cfg.c_str());
    std::string out;
    out.reserve(g_presets[p].states.size() * 10);
    std::vector<std::pair<uint32_t, bool>> rows(g_presets[p].states.begin(), g_presets[p].states.end());
    std::sort(rows.begin(), rows.end());
    char item[16];
    for (const auto& [hash, on] : rows) {
        std::snprintf(item, sizeof(item), "%s%08x%d", out.empty() ? "" : ",", hash, on ? 1 : 0);
        out += item;
    }
    std::snprintf(key, sizeof(key), "Preset%d", p + 1);
    async_io::IniWrite("Presets", key, out.empty() ? nullptr : out.c_str(), cfg.c_str());
    const PresetSettings& s = g_presets[p].settings;
    char set[160];
    std::snprintf(set, sizeof(set), "%d,%d,%d,%d,%d,%d,%x,%d,%d,%d,%d", s.fog, s.pins, s.markerScale, s.looted,
                  s.hideCompleted ? 1 : 0, s.regionFilter ? 1 : 0, s.regionMask, s.regionHide ? 1 : 0,
                  s.regionNames ? 1 : 0, s.levelAreas ? 1 : 0, s.levelTint);
    std::snprintf(key, sizeof(key), "Preset%dSettings", p + 1);
    async_io::IniWrite("Presets", key, s.has ? set : nullptr, cfg.c_str());
}

void ApplyFilterPreset(uint8_t* client, uint8_t* mapState, int p) {
    if (!client || p < 0 || p >= kPresetCount) return;
    LoadFilterPresets();
    const FilterSnapshot now = TakeFilterSnapshot(client);
    ApplyPresetSettings(client, g_presets[p].settings);
    RestoreFilterSnapshot(client, mapState, PresetTarget(client, p, now));
    g_presetLastApplied = p;
    ShowMapNotice(FillName(umm_lang::Str::PresetApplied, PresetName(p)));
}

void StoreFilterPreset(uint8_t* client, int p) {
    if (!client || p < 0 || p >= kPresetCount) return;
    LoadFilterPresets();
    g_presets[p].states.clear();
    for (const auto& [hash, on] : TakeFilterSnapshot(client).states) g_presets[p].states[hash] = on;
    g_presets[p].settings = CurrentPresetSettings();
    SaveFilterPreset(p);
    g_presetLastApplied = p;
    ShowMapNotice(FillName(umm_lang::Str::PresetSaved, PresetName(p)));
}

// Which slot gives the filters shown now (-1 none): the slot drawn lit
int MatchingFilterPreset(uint8_t* client) {
    LoadFilterPresets();
    const FilterSnapshot now = TakeFilterSnapshot(client);
    const PresetSettings settings = CurrentPresetSettings();
    auto matches = [&](int p) {
        const PresetSettings& s = g_presets[p].settings;
        return (!s.has || s == settings) && PresetTarget(client, p, now) == now;
    };
    if (g_presetLastApplied >= 0 && matches(g_presetLastApplied)) return g_presetLastApplied;
    for (int p = 0; p < kPresetCount; ++p) {
        if (matches(p)) return p;
    }
    return -1;
}

bool PresetKeyGoingDown(int vk) {
    auto heldWith = [vk](int k) { return k == vk || KeyIsDown(k); }; // the key itself is going down now
    for (int p = 0; p < kPresetCount; ++p)
        if (inp::KeysHeld(g_keys.Preset(p), heldWith) || inp::KeysHeld(g_keys.SavePreset(p), heldWith)) return true;
    return false;
}

void StoreFilterPreset(uint8_t* client, int p);
// Alt+1..5 and the controller's next / previous, once a frame with the map up (and while playing: keysOnly, the
// keyboard's only, as the controller's LT + D-pad is the game's there); Ctrl+Alt+1..5 save into the slot
void UpdatePresetKeys(uint8_t* client, uint8_t* mapState, bool keysFree, bool keysOnly = false) {
    auto isDown = [keysOnly](const inp::Binding& b) { return keysOnly ? inp::KeysHeld(b, KeyIsDown) : BindingDown(b); };
    static bool saveWas[kPresetCount]{};
    for (int i = 0; i < kPresetCount; ++i) {
        const bool down = keysFree && isDown(g_keys.SavePreset(i));
        if (down && !saveWas[i]) StoreFilterPreset(client, i);
        saveWas[i] = down;
    }
    static bool was[kPresetCount + 2]{};
    for (int i = 0; i < kPresetCount + 2; ++i) {
        const inp::Binding& b = i < kPresetCount ? g_keys.Preset(i) : (i == kPresetCount ? g_keys.presetNext : g_keys.presetPrev);
        const bool down = keysFree && isDown(b);
        const bool edge = down && !was[i];
        was[i] = down;
        if (!edge) continue;
        if (i < kPresetCount) ApplyFilterPreset(client, mapState, i);
        else {
            const int step = i == kPresetCount ? 1 : kPresetCount - 1;
            const int from = g_presetLastApplied >= 0 ? g_presetLastApplied : (i == kPresetCount ? kPresetCount - 1 : 0);
            ApplyFilterPreset(client, mapState, (from + step) % kPresetCount);
        }
    }
}

// After a load, a reload or a world change: the locked rows back to their saved ON / OFF.
void ApplyLockedStates(uint8_t* client) {
    if (!client) return;
    if (g_lockedStates.empty()) { // nothing saved yet (locks set before this version): keep the states as they are
        if (FilterLockCount() > 0) SaveFilterLocks();
        return;
    }
    size_t applied = 0;
    // A copy: each SetSingleFilterState below saves the locks, which rebuilds g_lockedStates
    const auto states = g_lockedStates;
    g_applyingLockedStates = true;
    for (const auto& [hash, on] : states) {
        uint8_t category = 0xff;
        uint32_t paired = 0u, paired2 = 0u;
        for (const auto& grp : kCategoryGroups) {
            for (size_t r = 0; r < grp.rowCount; ++r) {
                if (grp.rows[r].typeHash == hash) {
                    category = grp.category;
                    paired = grp.rows[r].pairedHash;
                    paired2 = grp.rows[r].pairedHash2;
                }
            }
            if (grp.category == 4 && std::find(std::begin(kCustomPinHashes), std::end(kCustomPinHashes), hash) !=
                                         std::end(kCustomPinHashes)) {
                category = 4;
            }
        }
        if (category == 0xff || !IsFilterLocked(hash, category)) continue;
        SetSingleFilterState(client, nullptr, hash, on);
        if (paired != 0u) SetSingleFilterState(client, nullptr, paired, on);
        if (paired2 != 0u) SetSingleFilterState(client, nullptr, paired2, on);
        ++applied;
    }
    g_applyingLockedStates = false;
    SaveFilterLocks();
    if (applied) SaveMinimapConfig();
    if (g_log) g_log("Filter locks: restored %zu locked filters", applied);
}

bool IsCategoryEnabled(FilterEntry* entries, size_t count, uint8_t category) {
    if (category == 3 && AnyChestRowEnabled()) {
        return true;
    }
    if (category == 0 && g_otherPlayersEnabled) {
        return true;
    }
    if (category == 1 && g_townPortalsEnabled) {
        return true;
    }
    for (size_t o = 0; o < kOreCount; ++o) {
        if (kOreCategory[o] == category && g_oreEnabled[o]) return true;
    }
    if (IsResourceCategory(category)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (FilterCategory(entries[i]) == category && entries[i].enabled != 0) {
            return true;
        }
    }
    return false;
}

const void* GetIconRefForType(uint8_t* client, uint32_t typeHash) {
    if (typeHash == kGoldChestTypeHash) return g_goldChestMarkerType.icon;
    if (typeHash == kSilverChestTypeHash) return g_silverChestMarkerType.icon;
    if (typeHash == kWoodChestTypeHash) return g_woodChestMarkerType.icon;
    if (typeHash == kSpoilsTypeHash) return g_goldChestMarkerType.icon; // gold sarcophagi
    if (IsOreHash(typeHash)) return kOreIcons[OreIndexOf(typeHash)];
    if (typeHash == 0x26d18a06u) {
        if (uint8_t* assets = GetUiAssets()) {
            const auto* bytes = reinterpret_cast<const uint64_t*>(assets + 0xe0e0);
            if (bytes[0] != 0 || bytes[1] != 0) return assets + 0xe0e0;
        }
    }
    if (!client || !g_getMarkerType) return nullptr;
    void* mgr = *reinterpret_cast<void**>(client + 0x4667f0);
    if (!mgr) return nullptr;
    if (typeHash == kCompletedQuestTypeHash) {
        if (uint8_t* sqType = g_getMarkerType(0xc52e92e5u, mgr)) {
            const auto* mutedBytes = reinterpret_cast<const uint64_t*>(sqType + 0x2c);
            if (mutedBytes[0] != 0 || mutedBytes[1] != 0) return sqType + 0x2c;
            return sqType + 0x1c;
        }
    }
    uint8_t* markerType = g_getMarkerType(typeHash, mgr);
    if (!markerType) return nullptr;
    return markerType + 0x1c;
}

bool ReadLocaText(uint32_t locaId, char* out, size_t cap);

// "Wooden Chest": the game's own text while the mod follows the game's language, else the mod's
const char* WoodChestName() {
    static char game[96]{};
    static int state = 0, builtFor = -1; // 0 not tried, 1 have it, 2 none
    const int lang = static_cast<int>(umm_lang::Current());
    if (builtFor != lang) state = 0, builtFor = lang;
    if (umm_lang::FollowsGame() && umm_lang::Current() != umm_lang::Lang::English) {
        if (state == 0 && GetUiClient()) state = ReadLocaText(725466433u, game, sizeof(game)) ? 1 : 2;
        if (state == 1) return game;
    }
    return umm_lang::T(umm_lang::Str::WoodChest);
}

// The chest titles shown on the map, in the current language. Marker slots keep pointers to these
// texts, so they live in fixed buffers that are rewritten in place when the language is decided.
const UiText& GetChestSubtypeTitle(uint16_t subtype) {
    using umm_lang::Str;
    static char buffers[10][128]{};
    static UiText titles[10]{};
    static int builtFor = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (builtFor != lang) {
        const char* const gold = umm_lang::T(Str::GoldChest);
        const char* const silver = umm_lang::T(Str::SilverChest);
        const char* const locked = umm_lang::T(Str::ChestLocked);
        std::snprintf(buffers[0], sizeof(buffers[0]), "%s", gold);
        std::snprintf(buffers[1], sizeof(buffers[1]), "%s", umm_lang::T(Str::GoldSarcophagus));
        std::snprintf(buffers[2], sizeof(buffers[2]), "%s%s", gold, locked);
        std::snprintf(buffers[3], sizeof(buffers[3]), "%s", silver);
        std::snprintf(buffers[4], sizeof(buffers[4]), "%s", umm_lang::T(Str::SilverSarcophagus));
        std::snprintf(buffers[5], sizeof(buffers[5]), "%s%s", silver, locked);
        const char* const wood = WoodChestName();
        std::snprintf(buffers[6], sizeof(buffers[6]), "%s", wood);
        std::snprintf(buffers[7], sizeof(buffers[7]), "%s", umm_lang::T(Str::WoodSarcophagus));
        std::snprintf(buffers[8], sizeof(buffers[8]), "%s%s", wood, locked);
        std::snprintf(buffers[9], sizeof(buffers[9]), "%s", umm_lang::T(Str::DungeonSpoils));
        for (int i = 0; i < 10; ++i) titles[i] = MakeUiText(buffers[i]);
        builtFor = lang;
    }
    return titles[subtype < 10 ? subtype : 0];
}

bool IsChestCurrentlyCleared(size_t chestIdx);
uint32_t GetMarkerOrigTypeHash(const uint8_t* m);
bool IsPositionExploredByCharacter(uint8_t* client, int32_t wx, int32_t wz);

void WriteChestMarkerSlot(uint8_t* m, size_t chestIdx) {
    std::memset(m, 0, 0xA0);
    const ChestSpawn& sp = kChestSpawns[chestIdx];
    bool isGold = IsGoldChest(chestIdx);
    uint32_t typeHash = isGold ? kGoldChestTypeHash : kSilverChestTypeHash;
    bool cleared = IsChestCurrentlyCleared(chestIdx);

    *reinterpret_cast<uint32_t*>(m + 0x00) = kChestMarkerUidPrefix | static_cast<uint32_t>(chestIdx + 1);
    *reinterpret_cast<uint32_t*>(m + 0x08) = 0u;
    *reinterpret_cast<uint64_t*>(m + 0x10) = (static_cast<uint64_t>(sp.px1) << 32) | (static_cast<uint64_t>(sp.px0) << 16);
    *reinterpret_cast<uint64_t*>(m + 0x18) = (static_cast<uint64_t>(sp.py1) << 32) | (static_cast<uint64_t>(sp.py0) << 16);
    *reinterpret_cast<uint64_t*>(m + 0x20) = (static_cast<uint64_t>(sp.pz1) << 32) | (static_cast<uint64_t>(sp.pz0) << 16);
    // IMPORTANT: Never set m+0x30 to 0 in arr1/arr2 because 0x140d8f833 does not null-check
    // MapMarkerType* before 0x140c24bf0 dereferences [r15 + 0x42]. Instead, omit disabled chests from arr1/arr2.
    *reinterpret_cast<uint32_t*>(m + 0x30) = typeHash;
    m[0x34] = cleared ? 3 : 0;
    m[0x35] = 0xff; // no discovery state (patches.h site 2)
    *reinterpret_cast<uint32_t*>(m + 0x3c) = typeHash;
    *reinterpret_cast<UiText*>(m + 0x68) = GetChestSubtypeTitle(sp.subtype);
    m[0x80] = isGold ? 4 : 0; // 4 = Yellow/Gold, 0 = White/Silver
}

struct BuildZoneBox {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

size_t CollectActiveBuildZones(uint8_t* client, BuildZoneBox* outZones, size_t maxZones) {
    if (!client || !outZones || maxZones == 0) return 0;
    size_t count = 0;
    if (uint8_t* clientWorld = GetClientWorld(client)) {
        if (*reinterpret_cast<uint8_t**>(clientWorld + 0x3db8) == client ||
            *reinterpret_cast<uint8_t**>(clientWorld + 0x3da8) == client) {
            if (uint8_t* baseReg = *reinterpret_cast<uint8_t**>(clientWorld + 0x3ec8)) {
                uint32_t baseCount = *reinterpret_cast<uint32_t*>(baseReg + 0x24);
                if (baseCount <= 64u) {
                    for (uint32_t b = 0; b < baseCount && count < maxZones; ++b) {
                        uint8_t* entry = baseReg + 0x30 + static_cast<size_t>(b) * 0x130;
                        const float* bMin = reinterpret_cast<const float*>(entry + 0xf0);
                        const float* bMax = reinterpret_cast<const float*>(entry + 0x100);
                        if (std::isfinite(bMin[0]) && std::isfinite(bMax[0]) &&
                            bMax[0] > bMin[0] && bMax[2] > bMin[2]) {
                            outZones[count++] = BuildZoneBox{
                                bMin[0], bMin[1], bMin[2],
                                bMax[0], bMax[1], bMax[2]
                            };
                        }
                    }
                }
            }
        }
    }
    if (count == 0) {
        // Fallback: check Flame Altar markers in arr0 with standard 40m radius (80m x 80m x 80m)
        uint8_t* arrBase = *reinterpret_cast<uint8_t**>(client + 0x36ea00);
        uint64_t arrCount = *reinterpret_cast<uint64_t*>(client + 0x36ea08);
        if (arrBase && arrCount > 0 && arrCount <= 10000) {
            for (uint64_t i = 0; i < arrCount && count < maxZones; ++i) {
                uint8_t* m = arrBase + i * 0xA0;
                uint32_t orig = *reinterpret_cast<uint32_t*>(m + 0x3c);
                if (orig == 0u) orig = *reinterpret_cast<uint32_t*>(m + 0x30);
                if (orig == 0xa447cba3u || orig == 0x6a56c5b3u) {
                    float ax = static_cast<float>(*reinterpret_cast<int32_t*>(m + 0x14));
                    float ay = static_cast<float>(*reinterpret_cast<int32_t*>(m + 0x1c));
                    float az = static_cast<float>(*reinterpret_cast<int32_t*>(m + 0x24));
                    if (ax > 100.0f && az > 100.0f) {
                        outZones[count++] = BuildZoneBox{
                            ax - 40.0f, ay - 40.0f, az - 40.0f,
                            ax + 40.0f, ay + 40.0f, az + 40.0f
                        };
                    }
                }
            }
        }
    }
    return count;
}

bool IsChestInBuildZone(size_t chestIdx, const BuildZoneBox* zones, size_t numZones) {
    if (chestIdx >= kTotalChestCount || numZones == 0) return false;
    const ChestSpawn& sp = kChestSpawns[chestIdx];
    float cx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
    float cy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
    float cz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
    for (size_t i = 0; i < numZones; ++i) {
        const auto& z = zones[i];
        if (cx >= z.minX && cx <= z.maxX &&
            cy >= z.minY && cy <= z.maxY &&
            cz >= z.minZ && cz <= z.maxZ) {
            return true;
        }
    }
    return false;
}

bool IsChestCurrentlyCleared(size_t chestIdx) {
    return (chestIdx < kTotalChestCount) && g_chestLootStates[chestIdx].looted;
}

// Looted chests are saved per world in looted_chests.ini. The game exposes no world id here, so a world is
// recognised by its Flame Altar positions: the saved record that shares an altar with the current world is
// the same world. Leaving to the main menu saves and clears the state, so another world starts clean.
struct LootWorld {
    std::string section;
    std::vector<std::pair<int32_t, int32_t>> altars;
};
std::string g_lootSection;              // empty: no world matched yet
std::vector<std::pair<int32_t, int32_t>> g_lootAltars;
bool g_lootSaveDirty = false;
uint64_t g_lootLastSaveTick = 0;
double g_lootOfflineSec = 0.0;          // time since this world was last played here, not yet counted (LastPlayed)
uint64_t g_lootOfflineTick = 0;         // when that was read
// The world's time of day (seconds into the game's day) when it was last left (TimeOfDay), read on joining, and now.
// A server nobody is on can stand still: then the time of day picks up where it was left and no time is counted.
double g_lootLeftDaySec = -1.0, g_lootJoinDaySec = -1.0, g_lootDaySec = -1.0;

// The game's time of day in this world, in seconds into its day (it runs about 32 times real time).
bool WorldTimeOfDaySec(uint8_t* client, double& outSec) {
    uint8_t* daytime = *reinterpret_cast<uint8_t**>(client + 0x466ff0);
    if (!daytime) return false;
    constexpr int64_t kDayNanoseconds = 86400000000000LL;
    const int64_t ns = *reinterpret_cast<int64_t*>(daytime + 0x48);
    if (ns < 0 || ns > kDayNanoseconds * 2) return false;
    outSec = static_cast<double>(ns % kDayNanoseconds) / 1e9;
    return true;
}
uint64_t g_lootLastMatchTick = 0;
uint8_t g_lootPrevScreen = 0;
uint8_t* g_lootClientWorld = nullptr;   // the game's world object the state belongs to
uint64_t g_lootLastAltarCheckTick = 0;
size_t g_lootPendingChest = kTotalChestCount; // chest the interact key was pressed at
uint64_t g_lootPendingTick = 0;

std::string GetLootedChestsPath() {
    return greeneye::DataPath("looted_chests.ini");
}

void CollectAltarPositions(uint8_t* client, std::vector<std::pair<int32_t, int32_t>>& out) {
    out.clear();
    uint8_t* arrBase = *reinterpret_cast<uint8_t**>(client + 0x36ea00);
    uint64_t arrCount = *reinterpret_cast<uint64_t*>(client + 0x36ea08);
    if (!arrBase || arrCount == 0 || arrCount > 10000) return;
    for (uint64_t i = 0; i < arrCount; ++i) {
        uint8_t* m = arrBase + i * 0xA0;
        uint32_t orig = GetMarkerOrigTypeHash(m);
        if (orig != 0xa447cba3u && orig != 0x6a56c5b3u) continue;
        int32_t x = *reinterpret_cast<int32_t*>(m + 0x14);
        int32_t z = *reinterpret_cast<int32_t*>(m + 0x24);
        if (x <= 100 || z <= 100) continue;
        auto p = std::make_pair(x, z);
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
}

bool AltarsShared(const std::vector<std::pair<int32_t, int32_t>>& a, const std::vector<std::pair<int32_t, int32_t>>& b) {
    for (const auto& p : a) {
        for (const auto& q : b) {
            // An altar's marker can move by a metre or two between sessions
            if (std::abs(p.first - q.first) <= 4 && std::abs(p.second - q.second) <= 4) return true;
        }
    }
    return false;
}

std::vector<std::pair<int32_t, int32_t>> ParseAltars(const char* text) {
    std::vector<std::pair<int32_t, int32_t>> out;
    int x = 0, z = 0, used = 0;
    while (*text && sscanf_s(text, "%d,%d%n", &x, &z, &used) == 2) {
        out.emplace_back(x, z);
        text += used;
        if (*text == ';') ++text;
    }
    return out;
}

#include "built_water.inl"

void SaveLootedChests() {
    g_lootSaveDirty = false;
    g_lootLastSaveTick = GetTickCount64();
    if (g_lootSection.empty()) return;
    const std::string path = GetLootedChestsPath();
    std::string altars, looted, opened;
    char buf[48];
    for (const auto& p : g_lootAltars) {
        std::snprintf(buf, sizeof(buf), "%s%d,%d", altars.empty() ? "" : ";", p.first, p.second);
        altars += buf;
    }
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        if (!g_chestLootStates[c].looted) continue;
        std::snprintf(buf, sizeof(buf), "%s%zu:%d", looted.empty() ? "" : ";", c,
                      static_cast<int>(g_chestLootStates[c].elapsedOutOfBuildZoneSec));
        looted += buf;
    }
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        if (!g_chestLootStates[c].everLooted) continue;
        std::snprintf(buf, sizeof(buf), "%s%zu", opened.empty() ? "" : ";", c);
        opened += buf;
    }
    async_io::IniWrite(g_lootSection.c_str(), "Altars", altars.c_str(), path.c_str());
    async_io::IniWrite(g_lootSection.c_str(), "Looted", looted.c_str(), path.c_str());
    async_io::IniWrite(g_lootSection.c_str(), "Opened", opened.c_str(), path.c_str());
    async_io::IniWrite(g_lootSection.c_str(), "Bosses", BossTimersText().c_str(), path.c_str());
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::time(nullptr)));
    async_io::IniWrite(g_lootSection.c_str(), "LastPlayed", buf, path.c_str());
    if (g_lootDaySec >= 0.0) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_lootDaySec));
        async_io::IniWrite(g_lootSection.c_str(), "TimeOfDay", buf, path.c_str());
    }
    SaveBuiltWater(path);
}

// How many of a saved world's altars stand here now (within 4 m: a marker can move by a metre or two)
size_t SavedAltarsHere(const std::vector<std::pair<int32_t, int32_t>>& saved, const std::vector<std::pair<int32_t, int32_t>>& now) {
    size_t n = 0;
    for (const auto& p : saved) {
        for (const auto& q : now) {
            if (std::abs(p.first - q.first) <= 4 && std::abs(p.second - q.second) <= 4) {
                ++n;
                break;
            }
        }
    }
    return n;
}

// Finds this world's saved record (or starts one) and merges its looted chests into the current state.
// A record is this world when at least half its altars stand here, or three of them do (a base moved since). One altar
// in a common spot isn't enough: a new world's first altar is often put where an old world's was, near the start,
// and then the new world got the old one's looted chests (reported by FWSWBN). The record most of whose altars are
// here wins.
void MatchLootWorld(const std::vector<std::pair<int32_t, int32_t>>& altars) {
    async_io::Flush(); // the writer's queue on disk before reading
    const std::string path = GetLootedChestsPath();
    std::vector<char> names(64 * 1024);
    DWORD len = GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), path.c_str());
    std::string best;
    double bestScore = 0.0;
    for (const char* name = names.data(); len && *name; name += std::strlen(name) + 1) {
        std::vector<char> text(64 * 1024);
        GetPrivateProfileStringA(name, "Altars", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        const auto saved = ParseAltars(text.data());
        const size_t here = SavedAltarsHere(saved, altars);
        if (here == 0 || (here * 2 < saved.size() && here < 3)) {
            if (here && g_log) g_log("Looted chests: %s shares %zu of its %zu altars only; not this world", name, here, saved.size());
            continue;
        }
        const double score = static_cast<double>(here) / static_cast<double>(saved.size()) + 0.001 * static_cast<double>(here);
        if (score > bestScore) {
            bestScore = score;
            best = name;
        }
    }
    if (!best.empty()) {
        const char* name = best.c_str();
        std::vector<char> text(64 * 1024);
        GetPrivateProfileStringA(name, "Altars", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        auto saved = ParseAltars(text.data());
        g_lootSection = name;
        g_lootAltars = saved;
        LoadBuiltWater(name, path);
        // A server keeps running while you're away: its chests come back meanwhile (counted once we know it's one)
        const long long lastPlayed = GetPrivateProfileIntA(name, "LastPlayed", 0, path.c_str()); // unix seconds
        const long long away = static_cast<long long>(std::time(nullptr)) - lastPlayed;
        g_lootOfflineSec = lastPlayed > 0 && away > 0 ? static_cast<double>(away) : 0.0;
        g_lootOfflineTick = GetTickCount64();
        g_lootLeftDaySec = static_cast<double>(GetPrivateProfileIntA(name, "TimeOfDay", -1, path.c_str()));
        g_lootJoinDaySec = g_lootDaySec = -1.0;
        g_worldHostState = -1; // the last world's answer does not hold here
        g_worldHostRecheck = true;
        GetPrivateProfileStringA(name, "Looted", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        const char* t = text.data();
        size_t idx = 0;
        int secs = 0, used = 0;
        while (*t && sscanf_s(t, "%zu:%d%n", &idx, &secs, &used) == 2) {
            if (idx < kTotalChestCount && !g_chestLootStates[idx].looted) {
                g_chestLootStates[idx].looted = true;
                g_chestLootStates[idx].everLooted = true;
                g_chestLootStates[idx].elapsedOutOfBuildZoneSec = std::max(0, secs);
            }
            t += used;
            if (*t == ';') ++t;
        }
        GetPrivateProfileStringA(name, "Bosses", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        LoadBossTimers(text.data());
        // Opened = every chest looted here at least once (before 1.3.16: the ones looted then, above)
        GetPrivateProfileStringA(name, "Opened", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        for (t = text.data(); *t && sscanf_s(t, "%zu%n", &idx, &used) == 1;) {
            if (idx < kTotalChestCount) g_chestLootStates[idx].everLooted = true;
            t += used;
            if (*t == ';') ++t;
        }
    }
    if (g_lootSection.empty()) {
        char name[32];
        std::snprintf(name, sizeof(name), "World_%08X%04X", static_cast<unsigned>(altars[0].first * 2654435761u),
                      static_cast<unsigned>(GetTickCount64() & 0xffffu));
        g_lootSection = name;
    }
    for (const auto& p : altars) {
        if (std::find(g_lootAltars.begin(), g_lootAltars.end(), p) == g_lootAltars.end()) g_lootAltars.push_back(p);
    }
    g_chestLootDirty = true;
    SaveLootedChests();
    if (g_log) g_log("Looted chests: world %s", g_lootSection.c_str());
}

size_t NearestChestTo(float px, float py, float pz) {
    float bestDistSq = 42.25f; // 4.24 m, or 6.5 m for a locked chest (below)
    size_t bestChest = kTotalChestCount;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        const ChestSpawn& sp = kChestSpawns[c];
        float cx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        float cy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
        float cz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        if (std::abs(cy - py) > 4.5f) continue;
        float d2 = (cx - px) * (cx - px) + (cz - pz) * (cz - pz);
        // A locked chest opens from farther away (chest 567 from 5.5 m), so it counts up to 6.5 m
        const bool locked = sp.subtype == 2 || sp.subtype == 5 || sp.subtype == 8;
        if (locked && d2 >= 42.25f) continue;
        if (!locked && d2 >= 18.0f) continue;
        if (d2 < bestDistSq) {
            bestDistSq = d2;
            bestChest = c;
        }
    }
    return bestChest;
}

// ---------------------------------------------------------------------------------------------------------
// Looted chests in multiplayer. A chest this player opens (or marks / clears with Ctrl+click) is told to the
// other players as a chat line of entry type 255: "/ummc1 L <x> <z>" (looted) or "/ummc1 U <x> <z>" (cleared),
// the chest's position in whole meters. The game draws chat types 0-4 only, and every server relays the others
// unchanged, so players without the mod never see these lines (Embervale Teleport uses the same channel with its
// own /mt prefixes). Each receiving mod marks the chest nearest that position; the position, not a table index,
// keeps it working between different mod versions.
using SendCommandFn = void (*)(void* gameClient, void* command);
using ChatAppendFn = uint8_t* (*)(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b,
                                  uint32_t c, uint64_t account, uint64_t* time);
SendCommandFn g_sendCommand = nullptr;
ChatAppendFn g_origChatAppend = nullptr;
std::atomic<void*> g_localChatStore{nullptr}; // the local client's chat store (game client + 0xf8)
constexpr uint8_t kChestChatType = 255;

struct ChestNetEvent {
    bool looted;
    int32_t x, z;
    bool wood; // a wooden chest: "W" / "V", which mod versions before wooden chests skip
};
std::mutex g_chestNetLock;
std::vector<ChestNetEvent> g_chestNetInbox;

// The live game client is the UI client's owner: GameClient + 0x1f0 == UI client (see IsPositionExploredByCharacter).
uint8_t* GameClientOf(uint8_t* uiClient) {
    return uiClient ? uiClient - 0x1f0 : nullptr;
}

void ShareChestState(size_t chestIdx, bool looted) {
    uint8_t* gameClient = GameClientOf(GetUiClient());
    if (!g_sendCommand || !gameClient || chestIdx >= kTotalChestCount) return;
    const ChestSpawn& sp = kChestSpawns[chestIdx];
    char line[64];
    const char kind = IsWoodChest(chestIdx) ? (looted ? 'W' : 'V') : (looted ? 'L' : 'U');
    const int len = std::snprintf(line, sizeof(line), "/ummc1 %c %u %u", kind, sp.px1, sp.pz1);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) return;
    // The game's chat command: 0x74, the entry type, the text, and its length at +0x402
    alignas(8) uint8_t command[0x410]{};
    command[0] = 0x74;
    command[1] = kChestChatType;
    std::memcpy(command + 2, line, static_cast<size_t>(len));
    *reinterpret_cast<uint16_t*>(command + 0x402) = static_cast<uint16_t>(len);
    g_sendCommand(gameClient, command);
    if (g_log) g_log("Looted chests: told the other players chest %zu is %s", chestIdx, looted ? "looted" : "cleared");
}

// 1: a chest line for this player (consume it), 0: anything else.
int ReadChestChatLine(void* store, uint8_t type, const uint64_t* text) {
    if (type != kChestChatType || !text || !text[0] || text[1] < 10 || text[1] > 48) return 0;
    const char* line = reinterpret_cast<const char*>(text[0]);
    if (std::strncmp(line, "/ummc1 ", 7) != 0) return 0;
    // Only the local client's own chat store: when this player hosts, the host's store must keep the line so
    // the game relays it to everyone else.
    if (store != g_localChatStore.load()) return 0;
    char buf[49]{};
    std::memcpy(buf, line, static_cast<size_t>(text[1]));
    char kind = 0;
    unsigned x = 0, z = 0;
    if (sscanf_s(buf + 7, "%c %u %u", &kind, 1u, &x, &z) != 3 || !std::strchr("LUWV", kind) || !kind || x > 20000 ||
        z > 20000) {
        return 1;
    }
    std::lock_guard<std::mutex> guard(g_chestNetLock);
    if (g_chestNetInbox.size() < 256) {
        g_chestNetInbox.push_back({kind == 'L' || kind == 'W', static_cast<int32_t>(x), static_cast<int32_t>(z),
                                   kind == 'W' || kind == 'V'});
    }
    return 1;
}

int SafeReadChestChatLine(void* store, uint8_t type, const uint64_t* text) {
    __try {
        return ReadChestChatLine(store, type, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t* ChatAppendHook(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b,
                        uint32_t c, uint64_t account, uint64_t* time) {
    {
        greeneye::Lease lease;
        if (lease && !g_modOff && SafeReadChestChatLine(store, type, text) == 1) {
            std::memset(result, 0, 8);
            return result;
        }
    }
    return g_origChatAppend(result, store, type, text, a, b, c, account, time);
}

// Applies what other players told us, on the UI thread.
void ApplySharedChestStates() {
    std::vector<ChestNetEvent> events;
    {
        std::lock_guard<std::mutex> guard(g_chestNetLock);
        events.swap(g_chestNetInbox);
    }
    for (const ChestNetEvent& e : events) {
        size_t best = kTotalChestCount;
        int64_t bestD2 = 6 * 6; // within 6 m of the position sent
        for (size_t c = 0; c < kTotalChestCount; ++c) {
            if (IsWoodChest(c) != e.wood) continue;
            const int64_t dx = static_cast<int64_t>(kChestSpawns[c].px1) - e.x;
            const int64_t dz = static_cast<int64_t>(kChestSpawns[c].pz1) - e.z;
            if (dx * dx + dz * dz <= bestD2) {
                bestD2 = dx * dx + dz * dz;
                best = c;
            }
        }
        if (g_log) g_log("Looted chests: chat line received (%s chest at %d,%d -> %s)", e.looted ? "looted" : "cleared",
                         e.x, e.z, best < kTotalChestCount ? "matched" : "no chest there");
        if (best >= kTotalChestCount) continue;
        auto& st = g_chestLootStates[best];
        if (e.looted) st.elapsedOutOfBuildZoneSec = 0.0; // the respawn timer starts from their opening
        if (st.looted != e.looted) {
            st.looted = e.looted;
            g_chestLootDirty = true;
            g_lootSaveDirty = true;
            if (g_log) g_log("Looted chests: another player %s chest %zu", e.looted ? "opened" : "cleared", best);
        }
    }
}

bool GetHudPlayerWorldPos(uint8_t* client, float& outX, float& outZ);

// The world was left: save it and start the next world clean (its record is matched again by its altars).
void ResetLootWorld(const char* why) {
    if (!g_lootSection.empty()) SaveLootedChests();
    if (g_log && !g_lootSection.empty()) g_log("Looted chests: left world %s (%s)", g_lootSection.c_str(), why);
    g_lootSection.clear();
    g_lootAltars.clear();
    g_lootJoinDaySec = g_lootDaySec = -1.0;
    ResetBuiltWater();
    for (auto& s : g_chestLootStates) s = ChestLootState{};
    g_bossDead.clear();
    g_chestLootDirty = true;
    g_lootLastMatchTick = 0;
}

#include "chest_live.inl"

void UpdateChestLootTracking(uint8_t* client, bool appFocused) {
    if (!client) return;
    uint8_t screen = client[0x1cd8];
    const uint64_t nowTick = GetTickCount64();
    // Another world loaded without the mod seeing the main menu (it draws nothing there): the game's world
    // object is a new one
    if (uint8_t* clientWorld = GetClientWorld(client)) {
        if (g_lootClientWorld && clientWorld != g_lootClientWorld) ResetLootWorld("new world object");
        g_lootClientWorld = clientWorld;
    }
    if (screen == 0) {
        // Main menu: the world was left. Save it and start the next world clean.
        if (g_lootPrevScreen != 0) ResetLootWorld("main menu");
        g_lootPrevScreen = 0;
        g_lastChestUpdateTick = 0;
        g_lootPendingChest = kTotalChestCount;
        return;
    }

    double dtSec = 0.0;
    if (g_lastChestUpdateTick != 0 && nowTick > g_lastChestUpdateTick) {
        dtSec = static_cast<double>(nowTick - g_lastChestUpdateTick) * 0.001;
        if (dtSec > 0.5) dtSec = 0.5;
    }
    g_lastChestUpdateTick = nowTick;

    if (uint8_t* gameClient = GameClientOf(client)) {
        g_localChatStore.store(*reinterpret_cast<void**>(gameClient + 0xf8));
    }
    if (!g_lootSection.empty()) ApplySharedChestStates();

    // A matched world whose altars are all gone is another world
    if (!g_lootSection.empty() && screen == 1 && nowTick - g_lootLastAltarCheckTick > 5000ULL) {
        g_lootLastAltarCheckTick = nowTick;
        std::vector<std::pair<int32_t, int32_t>> altars;
        CollectAltarPositions(client, altars);
        if (!altars.empty() && !AltarsShared(altars, g_lootAltars)) ResetLootWorld("altars differ");
    }
    if (g_lootSection.empty() && nowTick - g_lootLastMatchTick > 2000ULL) {
        g_lootLastMatchTick = nowTick;
        std::vector<std::pair<int32_t, int32_t>> altars;
        CollectAltarPositions(client, altars);
        if (!altars.empty()) {
            MatchLootWorld(altars);
            SyncLinkedResourceRows(client);
            ApplyLockedStates(client); // the world is up: its filters are the game's, put the locked ones back
        }
    }
    UpdateBuiltWater(client);
    chest_live::Update(nowTick, screen);
    // Two minutes of play on an untested game build: nothing that runs early crashed it (map_natives.h)
    {
        static uint64_t playingSince = 0;
        if (screen != 1) playingSince = 0;
        else if (!playingSince) playingSince = nowTick;
        else if (nowTick - playingSince > 120000ULL) map_natives::NoteStablePlay();
    }
    UpdateKnowledge();
    UpdateManualBosses(client);
    NoteBossKills(client);
    boss_live::Update(client);
    NoteVisitedPlaces(client);
    chest_glow::UpdateKey(client, appFocused);
    // The filter preset keys while playing too (requested 2026-11-11): the minimap and the World Map change together,
    // the notice beside the minimap
    if (client[0x1cd8] == 1 && appFocused) {
        g_playingKeysTick = nowTick;
        const uint64_t noticeBefore = g_filterNoticeTick;
        UpdatePresetKeys(client, nullptr, true, true);
        if (g_filterNoticeTick != noticeBefore) g_filterNoticeOnHud = true;
    } else {
        g_playingKeysTick = 0;
    }

    // 1. A chest counts as looted when the interact key is pressed next to it and the game switches to its
    //    chest screen (21) right after. The key alone, or the inventory (9) opened by itself, marks nothing.
    //    A locked chest is unlocked first, so its chest screen can come several seconds later: the wait is
    //    10 s, starts over while the key is held, and the chest screen can come after another screen.
    // On a controller: InteractKey's Pad: part, or the game's own Interact (Y unless rebound).
    const bool interactDown = appFocused && screen == 1 && [] {
        if (inp::KeysHeld(g_keys.interact, KeyIsDown)) return true;
        const inp::PadState pad = inp::ReadPad();
        if (g_keys.interact.padCount) return inp::PadHeld(g_keys.interact, pad);
        const auto game = greeneye::bindings::GameInteractKeys().pad;
        return (pad.buttons & game.buttons) != 0 || (game.leftTrigger && pad.lt) || (game.rightTrigger && pad.rt);
    }();
    if (interactDown && !g_interactKeyWasDown) {
        g_lootPendingChest = kTotalChestCount;
        if (uint8_t* clientWorld = GetClientWorld(client)) {
            int64_t rawX = *reinterpret_cast<int64_t*>(clientWorld + 0x2c18);
            int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c20);
            int64_t rawZ = *reinterpret_cast<int64_t*>(clientWorld + 0x2c28);
            if (rawX != 0 || rawZ != 0) {
                g_lootPendingChest = NearestChestTo(static_cast<float>(static_cast<double>(rawX) * 2.3283064365386963e-10),
                                                    static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10),
                                                    static_cast<float>(static_cast<double>(rawZ) * 2.3283064365386963e-10));
                g_lootPendingTick = nowTick;
                if (g_lootPendingChest < kTotalChestCount && g_log)
                    g_log("Looted chests: interact next to chest %zu, waiting for its chest screen", g_lootPendingChest);
                if (g_lootPendingChest == kTotalChestCount && g_log) {
                    // No chest close enough: name the nearest one within 15 m, to see how far off it is
                    const float px = static_cast<float>(static_cast<double>(rawX) * 2.3283064365386963e-10);
                    const float py = static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10);
                    const float pz = static_cast<float>(static_cast<double>(rawZ) * 2.3283064365386963e-10);
                    for (size_t c = 0; c < kTotalChestCount; ++c) {
                        const ChestSpawn& sp = kChestSpawns[c];
                        const float dx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f - px;
                        const float dy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f - py;
                        const float dz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f - pz;
                        if (dx * dx + dz * dz < 225.0f)
                            g_log("Looted chests: interact at %.1f,%.1f,%.1f; chest %zu is %.1f m off, %.1f m up",
                                  px, py, pz, c, std::sqrt(dx * dx + dz * dz), dy);
                    }
                }
            }
        }
    }
    if (interactDown && g_interactKeyWasDown && g_lootPendingChest < kTotalChestCount) g_lootPendingTick = nowTick;
    g_interactKeyWasDown = interactDown;
    if (g_lootPendingChest < kTotalChestCount) {
        if (screen != g_lootPrevScreen && g_log)
            g_log("Looted chests: screen %u -> %u near chest %zu", g_lootPrevScreen, screen, g_lootPendingChest);
        if (screen == 21 && g_lootPrevScreen != 21) {
            auto& st = g_chestLootStates[g_lootPendingChest];
            if (!st.looted) {
                if (g_log) g_log("Looted chests: chest %zu opened", g_lootPendingChest);
                st.looted = true;
                g_chestLootDirty = true;
                g_lootSaveDirty = true;
            }
            st.elapsedOutOfBuildZoneSec = 0.0;
            ShareChestState(g_lootPendingChest, true);
            g_lootPendingChest = kTotalChestCount;
        } else if (nowTick - g_lootPendingTick > 10000ULL) {
            if (g_log) g_log("Looted chests: no chest screen for chest %zu, stopped waiting", g_lootPendingChest);
            g_lootPendingChest = kTotalChestCount;
        }
    }
    g_lootPrevScreen = screen;

    // 2. Respawn: a looted chest outside every Flame Altar build zone comes back after RespawnMinutes of
    //    play time (time in a world, counted by this mod; it is not the game's clock). Like the game's own
    //    timer, it only runs while every player is ResetRadius (500 m) away: coming closer starts it over.
    if (dtSec > 0.0) {
        BuildZoneBox zones[64]{};
        size_t numZones = CollectActiveBuildZones(client, zones, 64);
        float players[65][2]{};
        size_t numPlayers = 0;
        float px = 0.0f, pz = 0.0f;
        if (GetHudPlayerWorldPos(client, px, pz)) {
            players[numPlayers][0] = px;
            players[numPlayers][1] = pz;
            ++numPlayers;
        }
        uint8_t* playerArray = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
        const uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
        for (uint64_t i = 0; playerArray && playerCount <= 64 && i < playerCount; ++i) {
            uint8_t* pl = playerArray + i * 0x240;
            const float ox = *reinterpret_cast<float*>(pl + 0x10), oz = *reinterpret_cast<float*>(pl + 0x18);
            if (*reinterpret_cast<uint32_t*>(pl) != 0 && std::isfinite(ox) && std::isfinite(oz)) {
                players[numPlayers][0] = ox;
                players[numPlayers][1] = oz;
                ++numPlayers;
            }
        }
        UpdateSceneChunks(players, numPlayers);
        const float resetSq = g_chestResetRadiusMeters * g_chestResetRadiusMeters;
        auto playerNear = [&](size_t c) {
            const ChestSpawn& sp = kChestSpawns[c];
            const float cx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
            const float cz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
            if (SceneChunkLoadedAt(cx, cz)) return true;
            for (size_t i = 0; resetSq > 0.0f && i < numPlayers; ++i) {
                const float dx = players[i][0] - cx, dz = players[i][1] - cz;
                if (dx * dx + dz * dz <= resetSq) return true;
            }
            return false;
        };
        const double bossRespawnSec = std::max(1.0, static_cast<double>(g_chestRespawnMinutes) * 60.0);
        const double respawnSec = g_chestsStayLooted ? 1e15 : bossRespawnSec; // RespawnMinutes=0: chests never come back
        double daySec = 0.0;
        if (!g_lootSection.empty() && WorldTimeOfDaySec(client, daySec)) {
            g_lootDaySec = daySec;
            if (g_lootJoinDaySec < 0.0) {
                g_lootJoinDaySec = daySec;
                if (g_log && g_lootLeftDaySec >= 0.0)
                    g_log("Looted chests: world clock %02d:%02d when last left, %02d:%02d now (%.0f min of real time)",
                          static_cast<int>(g_lootLeftDaySec) / 3600, static_cast<int>(g_lootLeftDaySec) / 60 % 60,
                          static_cast<int>(daySec) / 3600, static_cast<int>(daySec) / 60 % 60, g_lootOfflineSec / 60.0);
            }
        }
        // Time away from a world on someone else's server counts too, everyone away: it ran on without us. A world
        // hosted here was stopped meanwhile. Not known within three minutes: not counted.
        if (g_lootOfflineSec > 0.0 && (g_worldHostState != -1 || nowTick - g_lootOfflineTick > 180000ULL)) {
            // The server stood still while everyone was away: the time of day went on from where it was left (at
            // most two game hours on, about four minutes of saving and loading)
            const double dayMoved = g_lootLeftDaySec >= 0.0 && g_lootJoinDaySec >= 0.0
                                        ? std::fmod(g_lootJoinDaySec - g_lootLeftDaySec + 86400.0, 86400.0)
                                        : -1.0;
            if (g_worldHostState == 0 && dayMoved >= 0.0 && dayMoved <= 7200.0) {
                if (g_log)
                    g_log("Looted chests: away from this server world for %.0f min, but its clock moved only %.0f game "
                          "min: it stood still meanwhile; no chests came back",
                          g_lootOfflineSec / 60.0, dayMoved / 60.0);
            } else if (g_worldHostState == 0) {
                size_t back = 0;
                for (size_t c = 0; c < kTotalChestCount; ++c) {
                    auto& st = g_chestLootStates[c];
                    if (!st.looted || IsChestInBuildZone(c, zones, numZones)) continue;
                    st.elapsedOutOfBuildZoneSec += g_lootOfflineSec;
                    if (st.elapsedOutOfBuildZoneSec >= respawnSec) {
                        st.looted = false;
                        st.elapsedOutOfBuildZoneSec = 0.0;
                        ++back;
                    }
                }
                AdvanceBossTimers(g_lootOfflineSec, bossRespawnSec, zones, numZones, true);
                g_chestLootDirty = true;
                g_lootSaveDirty = true;
                if (g_log) g_log("Looted chests: away from this server world for %.0f min; %zu chests came back meanwhile",
                                 g_lootOfflineSec / 60.0, back);
            }
            if (g_worldHostState == -1 && g_log)
                g_log("Looted chests: after 3 min still not known whether this world is on a server (knowledge %s, "
                      "player slot %llu keys, replicated %u); the %.0f min away not counted",
                      g_knowledgeValid ? "read" : "not read", static_cast<unsigned long long>(g_knowledgeSlotKeys),
                      g_knowledgeReplicated, g_lootOfflineSec / 60.0);
            g_lootOfflineSec = 0.0;
        }
        for (size_t c = 0; c < kTotalChestCount; ++c) {
            if (!g_chestLootStates[c].looted) continue;
            if (IsChestInBuildZone(c, zones, numZones) || playerNear(c)) {
                g_chestLootStates[c].elapsedOutOfBuildZoneSec = 0.0;
            } else {
                g_chestLootStates[c].elapsedOutOfBuildZoneSec += dtSec;
                if (g_chestLootStates[c].elapsedOutOfBuildZoneSec >= respawnSec) {
                    g_chestLootStates[c].looted = false;
                    g_chestLootStates[c].elapsedOutOfBuildZoneSec = 0.0;
                    g_chestLootDirty = true;
                    g_lootSaveDirty = true;
                }
            }
        }
        AdvanceBossTimers(dtSec, bossRespawnSec, zones, numZones, false);
    }

    for (auto& st : g_chestLootStates) {
        if (st.looted && !st.everLooted) {
            st.everLooted = true;
            g_lootSaveDirty = true;
        }
    }

    // Save changes within a few seconds, and the respawn timers once a minute
    const uint64_t sinceSave = nowTick - g_lootLastSaveTick;
    if (!g_lootSection.empty() && ((g_lootSaveDirty && sinceSave > 3000ULL) || sinceSave > 60000ULL)) {
        SaveLootedChests();
    }
}

// A Ctrl + click on the World Map is only for what that map draws: not a chest or spot hidden by Map Key > Region > Hide,
// in the Hollow Halls' corner of the overworld map, or in another hall. The minimap's clicks aren't under these rules.
namespace halls {
bool MapHides(float x, float z);
bool MapHidesChest(size_t chest);
bool MapHidesAt(float x, float y, float z, bool wide);
}
bool RegionHidesAt(float x, float z); // regions.inl
bool g_markClickOnWorldMap = false;

bool MarkClickSkipsChest(size_t c) {
    if (!g_markClickOnWorldMap) return false;
    const ChestSpawn& sp = kChestSpawns[c];
    const float x = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
    const float z = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
    return RegionHidesAt(x, z) || halls::MapHidesChest(c);
}

bool MarkClickSkipsSpot(float x, float y, float z) {
    return g_markClickOnWorldMap && (RegionHidesAt(x, z) || halls::MapHidesAt(x, y, z, true));
}

bool ToggleChestClearedNearWorldPos(float wx, float wz, float maxRadiusMeters) {
    float bestDistSq = maxRadiusMeters * maxRadiusMeters;
    size_t bestChest = kTotalChestCount;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        if (!ChestRowEnabled(c) || MarkClickSkipsChest(c)) continue;
        if (g_chestLootStates[c].looted && g_lootedChestMode == 0) continue;

        const ChestSpawn& sp = kChestSpawns[c];
        float cx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        float cz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        float dx = cx - wx;
        float dz = cz - wz;
        float d2 = dx * dx + dz * dz;
        if (d2 < bestDistSq) {
            bestDistSq = d2;
            bestChest = c;
        }
    }
    if (bestChest < kTotalChestCount) {
        // Steps: looted -> refilled (its check mark stays) -> never looted -> looted
        auto& st = g_chestLootStates[bestChest];
        const char* chestName = FilterRowName(ChestTypeHash(bestChest));
        if (st.looted) {
            st.looted = false;
            AnnounceMarkState(chestName, umm_lang::Str::MarkRefilled);
        } else if (st.everLooted) {
            st.everLooted = false;
            AnnounceMark(chestName, 0, false);
        } else {
            st.looted = st.everLooted = true;
            AnnounceMark(chestName, 0, true);
        }
        st.elapsedOutOfBuildZoneSec = 0.0;
        g_chestLootDirty = true;
        g_lootSaveDirty = true;
        ShareChestState(bestChest, g_chestLootStates[bestChest].looted);
        return true;
    }
    return false;
}


void SyncChestsInUiArrays(uint8_t* client, bool& dirty) {
    if (!client) return;

    // We distribute only the currently enabled & visible chest markers across:
    // - arr2 (client + 0x3bea50, Hollow Halls array: 6 vanilla entries, 1024 capacity -> up to 1018 chest slots)
    // - arr1 (client + 0x396a28, Custom Pins array: 0..10 vanilla entries, 1024 capacity -> remaining chest slots)
    constexpr size_t kOffsets[2] = { 0x3bea50, 0x396a28 };
    uint8_t* bases[2]{};
    uint64_t counts[2]{};
    uint64_t caps[2]{};
    uint64_t vanillaCounts[2]{};

    for (int a = 0; a < 2; ++a) {
        bases[a] = *reinterpret_cast<uint8_t**>(client + kOffsets[a]);
        counts[a] = *reinterpret_cast<uint64_t*>(client + kOffsets[a] + 8);
        caps[a] = *reinterpret_cast<uint64_t*>(client + kOffsets[a] + 16);
        if (!bases[a] || caps[a] == 0 || counts[a] > caps[a]) return;

        uint64_t vCnt = 0;
        for (uint64_t i = 0; i < counts[a]; ++i) {
            uint8_t* m = bases[a] + i * 0xA0;
            uint32_t uid = *reinterpret_cast<uint32_t*>(m + 0x00);
            if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) {
                break;
            }
            ++vCnt;
        }
        vanillaCounts[a] = vCnt;
    }

    size_t activeIndices[kTotalChestCount]{};
    size_t activeCount = 0;
    const bool exploredOnly = (g_pinRevealMode == PinRevealMode::ExploredOnly);
    const bool keepLootedAsGrey = (g_lootedChestMode == 1);
    if (g_goldChestEnabled) {
        for (size_t c = 0; c < kTotalChestCount; ++c) {
            if (!IsGoldChest(c)) continue;
            if (IsChestCurrentlyCleared(c) && !keepLootedAsGrey) continue;
            if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
            if (RegionHidesAt(kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue; // Map Key > Region > Hide: not there at all
            activeIndices[activeCount++] = c;
        }
    }
    if (g_silverChestEnabled) {
        for (size_t c = kGoldChestCount; c < kWoodChestFirst; ++c) { // wooden chests: DrawWoodChestsOnMap
            if (IsChestCurrentlyCleared(c) && !keepLootedAsGrey) continue;
            if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
            if (RegionHidesAt(kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
            activeIndices[activeCount++] = c;
        }
    }

    size_t avail0 = (caps[0] > vanillaCounts[0]) ? static_cast<size_t>(caps[0] - vanillaCounts[0]) : 0;
    size_t avail1 = (caps[1] > vanillaCounts[1]) ? static_cast<size_t>(caps[1] - vanillaCounts[1]) : 0;
    size_t assign0 = std::min(activeCount, avail0);
    size_t assign1 = std::min(activeCount - assign0, avail1);

    size_t assigns[2] = { assign0, assign1 };
    size_t curLogicalOffset = 0;
    const bool forceChestRewrite = g_chestLootDirty;
    g_chestLootDirty = false;

    for (int a = 0; a < 2; ++a) {
        uint64_t desiredTotal = vanillaCounts[a] + assigns[a];
        bool needsRewrite = forceChestRewrite || (counts[a] != desiredTotal);
        if (!needsRewrite && assigns[a] > 0) {
            uint8_t* firstM = bases[a] + vanillaCounts[a] * 0xA0;
            size_t firstChestIdx = activeIndices[curLogicalOffset];
            uint32_t expectedFirstUid = kChestMarkerUidPrefix | static_cast<uint32_t>(firstChestIdx + 1);
            uint8_t expectedFirstState = IsChestCurrentlyCleared(firstChestIdx) ? 3 : 0;
            if (*reinterpret_cast<uint32_t*>(firstM + 0x00) != expectedFirstUid ||
                firstM[0x34] != expectedFirstState) {
                needsRewrite = true;
            }
        }

        if (needsRewrite) {
            static uint64_t s_lastSig[2]{};
            uint64_t sig = 0xcbf29ce484222325ULL ^ assigns[a];
            for (size_t k = 0; k < assigns[a]; ++k) {
                size_t chestIdx = activeIndices[curLogicalOffset + k];
                WriteChestMarkerSlot(bases[a] + (vanillaCounts[a] + k) * 0xA0, chestIdx);
                sig = (sig ^ (chestIdx * 2 + (IsChestCurrentlyCleared(chestIdx) ? 1 : 0))) * 0x100000001b3ULL;
            }
            *reinterpret_cast<uint64_t*>(client + kOffsets[a] + 8) = desiredTotal;
            // The game trims the Hollow Halls array back to its own entries every frame; putting the same chests back
            // needs no marker rebuild (asking for one makes the game trim the array again, every frame).
            if (forceChestRewrite || sig != s_lastSig[a]) {
                s_lastSig[a] = sig;
                dirty = true;
            }
        }
        curLogicalOffset += assigns[a];
    }
}

void RemoveChestsFromUiArrays(uint8_t* client) {
    if (!client) return;
    constexpr size_t kOffsets[2] = { 0x3bea50, 0x396a28 };
    for (int a = 0; a < 2; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kOffsets[a]);
        uint64_t count = *reinterpret_cast<uint64_t*>(client + kOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        uint64_t vCnt = 0;
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t uid = *reinterpret_cast<uint32_t*>(base + i * 0xA0);
            if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) break;
            ++vCnt;
        }
        if (vCnt != count) {
            *reinterpret_cast<uint64_t*>(client + kOffsets[a] + 8) = vCnt;
        }
    }
}

namespace halls { bool ExploredInHall(float x, float z, bool& inCorner); } // halls.inl

bool IsPositionExploredByCharacter(uint8_t* client, int32_t wx, int32_t wz) {
    if (!client) return true;
    // The Hollow Halls are carved in the world's north-west corner, where the game's fog never clears: a hall counts
    // as explored once you're in it, so its chests and notes show there as the island's do where you've been
    bool inCorner = false;
    const bool hallExplored = halls::ExploredInHall(static_cast<float>(wx), static_cast<float>(wz), inCorner);
    if (inCorner) return hallExplored;
    // GameClient is at (client - 0x1f0), so GameClient + 0x47d4e0 == client + 0x47d2f0 holds clientWorld;
    // [clientWorld + 0x3da8] == client, and fow is pointed to by [clientWorld + 0x3dc0].
    uint8_t* clientWorld = GetClientWorld(client);
    if (!clientWorld) return true;
    if (*reinterpret_cast<uint8_t**>(clientWorld + 0x3da8) != client &&
        *reinterpret_cast<uint8_t**>(clientWorld + 0x3db8) != client) {
        return true;
    }
    uint8_t* fow = *reinterpret_cast<uint8_t**>(clientWorld + 0x3dc0);
    if (!fow) return true;

    float worldZ = *reinterpret_cast<float*>(fow + 0x04);
    uint32_t chunksX = *reinterpret_cast<uint32_t*>(fow + 0x08);
    uint32_t chunksZ = *reinterpret_cast<uint32_t*>(fow + 0x0c);
    uint8_t* bitmap = *reinterpret_cast<uint8_t**>(fow + 0x18);
    uint64_t totalChunks = *reinterpret_cast<uint64_t*>(fow + 0x20);
    if (!bitmap || chunksX == 0 || chunksZ == 0 || chunksX > 256 || chunksZ > 256 ||
        totalChunks < static_cast<uint64_t>(chunksX) * chunksZ) {
        return true;
    }

    int32_t px = wx >> 3;
    int32_t pz = static_cast<int32_t>(worldZ - static_cast<float>(wz)) >> 3;
    int32_t maxPx = static_cast<int32_t>(chunksX << 5);
    int32_t maxPz = static_cast<int32_t>(chunksZ << 5);
    if (px < 0 || pz < 0 || px >= maxPx || pz >= maxPz) return false;

    uint32_t cx = static_cast<uint32_t>(px) >> 5;
    uint32_t cz = static_cast<uint32_t>(pz) >> 5;
    uint32_t lx = static_cast<uint32_t>(px) & 0x1fu;
    uint32_t lz = static_cast<uint32_t>(pz) & 0x1fu;
    size_t offset = (static_cast<size_t>(cz * chunksX + cx) << 10) + (static_cast<size_t>(lz) << 5) + lx;
    return bitmap[offset] >= 64u;
}

// The quest answers below are the game's own lookups, asked for every marker every frame (the World Map's pass, the
// minimap, Find): kept for half a second, quests don't change faster than that.
struct QuestAnswer {
    uint32_t value;
    uint64_t tick;
};
template <typename Compute>
uint32_t CachedQuestAnswer(std::unordered_map<uint64_t, QuestAnswer>& cache, uint64_t key, Compute compute) {
    const uint64_t now = GetTickCount64();
    if (cache.size() > 8192) cache.clear();
    auto it = cache.find(key);
    if (it != cache.end() && now - it->second.tick < 500) return it->second.value;
    const uint32_t value = compute();
    cache[key] = {value, now};
    return value;
}

uint32_t GetQuestFilterHashForMarkerIdUncached(uint32_t id08, bool activeObjectiveOnly);

// Returns 0xa7cadc08u (Main Quest) or 0xc52e92e5u (Side Quest) if id08 belongs to a quest, else 0u.
uint32_t GetQuestFilterHashForMarkerId(uint32_t id08, bool activeObjectiveOnly) {
    if (id08 == 0u) return 0u;
    static std::unordered_map<uint64_t, QuestAnswer> cache;
    return CachedQuestAnswer(cache, (static_cast<uint64_t>(id08) << 1) | (activeObjectiveOnly ? 1u : 0u),
                             [&] { return GetQuestFilterHashForMarkerIdUncached(id08, activeObjectiveOnly); });
}

uint32_t GetQuestFilterHashForMarkerIdUncached(uint32_t id08, bool activeObjectiveOnly) {
    if (id08 == 0u) return 0u;
    const uint8_t* quest = nullptr;
    if (activeObjectiveOnly) {
        if (g_getActiveQuestForMarkerId) {
            quest = static_cast<const uint8_t*>(g_getActiveQuestForMarkerId(id08));
        }
    } else {
        if (g_getQuestByMarkerId) {
            quest = static_cast<const uint8_t*>(g_getQuestByMarkerId(id08));
        }
        if (!quest && g_getActiveQuestForMarkerId) {
            quest = static_cast<const uint8_t*>(g_getActiveQuestForMarkerId(id08));
        }
    }
    if (!quest) return 0u;
    // 0x141205680 (IsMainQuest): quest->category (+0x18) >= 0x64 (100)
    uint64_t questCat = *reinterpret_cast<const uint64_t*>(quest + 0x18);
    return (questCat >= 0x64ULL) ? 0xa7cadc08u : 0xc52e92e5u;
}

// The id of the quest pin (a marker of type 0xafce0c5f of its own) of the quest that names a place's marker id, or 0:
// the quests' pins by quest, gathered every 2 s from the marker arrays
uint32_t QuestPinIdForPlaceInner(uint8_t* client, uint32_t placeId) {
    static std::unordered_map<const void*, uint32_t> pins;
    static uint64_t built = 0;
    if (!g_getQuestByMarkerId || !client || !placeId) return 0u;
    if (GetTickCount64() - built > 2000) {
        built = GetTickCount64();
        pins.clear();
        constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
        for (size_t a = 0; a < 4; ++a) {
            uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
            const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
            if (!base || count == 0 || count > 10000) continue;
            for (uint64_t i = 0; i < count; ++i) {
                const uint8_t* m = base + i * 0xA0;
                if (GetMarkerOrigTypeHash(m) != 0xafce0c5fu) continue;
                uint32_t id = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
                if (!id) id = *reinterpret_cast<const uint32_t*>(m + 0x08);
                if (const void* q = id ? g_getQuestByMarkerId(id) : nullptr) pins.emplace(q, id);
            }
        }
    }
    const void* q = g_getQuestByMarkerId(placeId);
    if (!q) return 0u;
    auto it = pins.find(q);
    return it != pins.end() && it->second != placeId ? it->second : 0u;
}
uint32_t QuestPinIdForPlace(uint8_t* client, uint32_t placeId) {
    __try {
        return QuestPinIdForPlaceInner(client, placeId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
}

// The lore list FUN_140d50dd0 (IsLoreRumorMarkerId) walks: client + 0xb3d40 groups (count at +0xb3d48, 0x22f00
// bytes each, items at +0x18 / count +0x20), items of 0x1748 bytes whose entries (0xb8 bytes, count at item +0x171c)
// start at item +0x58 and list marker ids (16 bytes apart, count at entry +0x78). FUN_140d1fd90 reads the byte
// 0x34 before an entry as "found": a rumor whose entry is found points at something already discovered.
// Returns 1 found, 0 not found yet, -1 not a lore rumor id.
int LoreRumorFound(uint8_t* client, uint32_t id08) {
    if (!client || id08 == 0u) return -1;
    __try {
        const uint64_t groups = *reinterpret_cast<uint64_t*>(client + 0xb3d48);
        uint8_t* g = *reinterpret_cast<uint8_t**>(client + 0xb3d40);
        if (!g || groups > 64) return -1;
        for (uint64_t gi = 0; gi < groups; ++gi) {
            uint8_t* grp = g + gi * 0x22f00;
            uint8_t* items = *reinterpret_cast<uint8_t**>(grp + 0x18);
            const uint64_t nItems = *reinterpret_cast<uint64_t*>(grp + 0x20);
            if (!items || nItems > 4096) continue;
            for (uint64_t ii = 0; ii < nItems; ++ii) {
                uint8_t* item = items + ii * 0x1748;
                const uint8_t nEntries = item[0x171c];
                for (uint8_t ei = 0; ei < nEntries; ++ei) {
                    uint8_t* entry = item + 0x58 + static_cast<size_t>(ei) * 0xb8;
                    const uint8_t nIds = entry[0x78];
                    for (uint8_t k = 0; k < nIds; ++k) {
                        if (*reinterpret_cast<uint32_t*>(entry + static_cast<size_t>(k) * 16) == id08) return entry[-0x34] != 0 ? 1 : 0;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

// Classifies a 0xafce0c5f marker into active Main Quest (0xa7cadc08u), active Side Quest (0xc52e92e5u),
// finished (kCompletedQuestTypeHash; never shown), Lore Rumor (0x8ba7c122u), or 0u (inactive NPC / hidden).
// vis35 is the marker's game visibility byte (+0x35, patches.h site 2).
// Done or not comes from the quest itself, not the pin's state (a finished quest's pin often still reads 2):
// FUN_140d34ef0 (active quest for a marker) returns the quest only while it is started, not finished and this
// marker's objective is open. A quest found for the marker (FUN_140d26fc0) that is not active for it is done
// when its last objective is (FUN_141205790: objectives at +0x28, count +0x30, 0x50 bytes each; the last one's
// state at +0x18, or +0x1c for the character, is 2). Anything else has not been reached yet and stays hidden.
// Done for you: the character's state decides when it has one. On a server someone else can finish a quest you
// haven't (the Journal's Missed list: Follow the Melody read world 2, you 1 on its last objectives), and that one is
// still yours to do. With no character state (0) the world's counts.
bool QuestFinished(const uint8_t* quest) {
    const uint64_t n = *reinterpret_cast<const uint64_t*>(quest + 0x30);
    const uint8_t* objs = *reinterpret_cast<uint8_t* const*>(quest + 0x28);
    if (!objs || n == 0 || n > 256) return false;
    const uint8_t* last = objs + (n - 1) * 0x50;
    const int32_t world = *reinterpret_cast<const int32_t*>(last + 0x18), mine = *reinterpret_cast<const int32_t*>(last + 0x1c);
    return mine == 2 || (mine == 0 && world == 2);
}

// Missed: finished for the world by someone else, not by you (the Journal's Missed list)
bool QuestMissed(const uint8_t* quest) {
    const uint64_t n = *reinterpret_cast<const uint64_t*>(quest + 0x30);
    const uint8_t* objs = *reinterpret_cast<uint8_t* const*>(quest + 0x28);
    if (!objs || n == 0 || n > 256) return false;
    const uint8_t* last = objs + (n - 1) * 0x50;
    return *reinterpret_cast<const int32_t*>(last + 0x18) == 2 && !QuestFinished(quest);
}

// The game itself draws this marker: its visibility for the character (MapMarkerVisibilityType; patches.h site 2
// keeps it at +0x35 as 0x10 | state) is 1 (Hint: drawn faded, named "Undiscovered"), 2 (Visible) or 3 (Completed), or
// it has no visibility at all (0xff, always drawn). Vanilla skips only 0, Hidden (site 1).
bool GameShowsMarker(uint8_t vis35) {
    return (vis35 >= 0x11u && vis35 <= 0x13u) || vis35 == 0xffu;
}

// Not discovered yet: Hidden (0x10) or only a Hint (0x11); 2 Visible and 3 Completed are discovered (the game's
// knowledge_query_mapmarker_visibility_state: the marker's own completed / visible / hint knowledge queries)
bool MarkerNotDiscovered(uint8_t vis35) {
    return vis35 == 0x10u || vis35 == 0x11u;
}

uint32_t ClassifyQuestOrRumorMarkerUncached(uint32_t id08, uint8_t vis35);

uint32_t ClassifyQuestOrRumorMarker(uint32_t id08, uint8_t vis35) {
    if (id08 == 0u) return 0u;
    static std::unordered_map<uint64_t, QuestAnswer> cache;
    return CachedQuestAnswer(cache, (static_cast<uint64_t>(id08) << 8) | vis35,
                             [&] { return ClassifyQuestOrRumorMarkerUncached(id08, vis35); });
}

uint32_t ClassifyQuestOrRumorMarkerUncached(uint32_t id08, uint8_t vis35) {
    if (id08 == 0u) return 0u;
    if (uint32_t activeQ = GetQuestFilterHashForMarkerId(id08, true)) return activeQ;
    if (g_isLoreRumorMarkerId && g_isLoreRumorMarkerId(id08)) {
        // A rumor about something already found (its lore entry is found) is done and hidden, as the game drops it
        // (a found Cosmetic Item Location kept its "?")
        return LoreRumorFound(GetUiClient(), id08) == 1 ? kCompletedQuestTypeHash : 0x8ba7c122u;
    }
    if (const auto* quest = g_getQuestByMarkerId ? static_cast<const uint8_t*>(g_getQuestByMarkerId(id08)) : nullptr) {
        if (QuestFinished(quest)) return kCompletedQuestTypeHash;
        // A missed quest: the game hides its pins (the world has it done), the mod shows them under its quest's row
        if (QuestMissed(quest)) return *reinterpret_cast<const uint64_t*>(quest + 0x18) >= 0x64ULL ? 0xa7cadc08u : 0xc52e92e5u;
        // Not this marker's open objective, but the game shows it (a side quest's search area, like Cat Search at
        // the Alchemist Tower, or a Customization quest's): under its quest's row, as vanilla. Reported by secretroff.
        if (!GameShowsMarker(vis35)) return 0u;
        return *reinterpret_cast<const uint64_t*>(quest + 0x18) >= 0x64ULL ? 0xa7cadc08u : 0xc52e92e5u;
    }
    return g_isLoreRumorMarkerId ? 0u : 0x8ba7c122u;
}

// A quest area circle (0x454e24f2, Plains For First Base) is shown only while its quest is on: hidden once it is
// done or before it begins.
bool IsActiveQuestArea(uint32_t id08, uint8_t vis35) {
    const uint32_t areaHash = ClassifyQuestOrRumorMarker(id08, vis35);
    return areaHash == 0xa7cadc08u || areaHash == 0xc52e92e5u;
}

bool IsMarkerTypeAlwaysRevealed(uint32_t origHash, uint8_t b48, uint64_t lastChg) {
    if (origHash == 0xa447cba3u || origHash == 0x6a56c5b3u ||
        origHash == 0x1ad8f96eu || origHash == kOtherPlayerTypeHash) {
        return true;
    }
    for (uint32_t ph : kCustomPinHashes) {
        if (origHash == ph) return true;
    }
    if (b48 != 0 && lastChg != 0 && (origHash == 0xa17072beu || origHash == 0x1095684eu)) {
        return true;
    }
    return false;
}

// A place this character hasn't discovered yet, drawn grayed out: the game would hide it (patches.h site 2 keeps its
// visibility byte at +0x35 as 0x10 | state, 0x10 not discovered). Quests and rumors, and a place an active quest points
// to, stand out as they are. Pins: REVEAL ALL shows every place in full colour (locked Fast Travel Points and Spires
// keep their own gray).
bool IsMarkerUndiscovered(const uint8_t* m, uint32_t origHash) {
    if (g_pinRevealMode == PinRevealMode::RevealAll) return false;
    if (!m || !MarkerNotDiscovered(m[0x35])) return false;
    // A place the character has been at, though the game never marked it discovered (knowledge.inl)
    if (PlaceVisited(*reinterpret_cast<const int32_t*>(m + 0x14), *reinterpret_cast<const int32_t*>(m + 0x24))) return false;
    for (const auto& row : kCategory0Rows) {
        if (row.typeHash == origHash) return false;
    }
    uint32_t id08 = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
    if (id08 == 0u) id08 = *reinterpret_cast<const uint32_t*>(m + 0x08);
    return id08 == 0u || GetQuestFilterHashForMarkerId(id08, true) == 0u;
}

bool IsMarkerExplored(uint8_t* client, const uint8_t* m, uint32_t origHash) {
    if (!client || !m) return true;
    // A quest you have (its pin or its area circle) shows even in unexplored fog, like vanilla: it is where to go
    if (origHash == 0xafce0c5fu || origHash == 0x454e24f2u) {
        uint32_t id08 = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
        if (id08 == 0u) id08 = *reinterpret_cast<const uint32_t*>(m + 0x08);
        if (IsActiveQuestArea(id08, m[0x35])) return true;
    }
    uint32_t uid = *reinterpret_cast<const uint32_t*>(m + 0x00);
    if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) {
        int32_t cx = *reinterpret_cast<const int32_t*>(m + 0x14);
        int32_t cz = *reinterpret_cast<const int32_t*>(m + 0x24);
        return IsPositionExploredByCharacter(client, cx, cz);
    }
    if (IsMarkerTypeAlwaysRevealed(origHash, m[0x48], *reinterpret_cast<const uint64_t*>(m + 0x40))) {
        return true;
    }
    int32_t mx = *reinterpret_cast<const int32_t*>(m + 0x14);
    int32_t mz = *reinterpret_cast<const int32_t*>(m + 0x24);
    return IsPositionExploredByCharacter(client, mx, mz);
}

// Pins: EXPLORED shows a marker where you have been, and never hides one the game itself shows (a place it
// knows of in land still fogged). Reported by secretroff.
bool IsMarkerShownForExploredPins(uint8_t* client, const uint8_t* m, uint32_t origHash) {
    return GameShowsMarker(m[0x35]) || IsMarkerExplored(client, m, origHash);
}

uint32_t GetMarkerOrigTypeHash(const uint8_t* m) {
    if (IsSlotSaved(m)) {
        uint32_t savedOrig = *reinterpret_cast<const uint32_t*>(m + 0x3c);
        if (savedOrig != 0u && savedOrig != kLockedFastTravelTypeHash) {
            return savedOrig;
        }
    }
    uint32_t curHash = *reinterpret_cast<const uint32_t*>(m + 0x30);
    if (curHash != 0u && curHash != kLockedFastTravelTypeHash) {
        return curHash;
    }
    if (m[0x48] != 0u) {
        uint32_t tag38 = *reinterpret_cast<const uint32_t*>(m + 0x38);
        if (tag38 == 0xe3a3e3c1u || tag38 == 0xe623c0fbu) {
            return 0x1095684eu;
        }
        if (tag38 == 0x5e74cd45u || tag38 == 0x4f301a2au ||
            tag38 == 0x06e3fedau || tag38 == 0x7312df52u ||
            tag38 == 0x8235997bu || tag38 == 0xb487f1bbu ||
            tag38 == 0x69adaed5u || tag38 == 0xa15c18ebu) {
            return 0xa17072beu;
        }
    }
    return 0u;
}

void SyncSpiresAndCountMarkers(uint8_t* client, uint8_t* mapState, const uint32_t* queryHashes, size_t numQueries, int* outCounts) {
    for (size_t q = 0; q < numQueries; ++q) {
        outCounts[q] = 0;
    }
    if (!client) return;

    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    bool spireFilterOn = IsFilterEnabled(filterEntries, filterCount, 0xa17072beu);
    bool fastTravelFilterOn = IsFilterEnabled(filterEntries, filterCount, 0x1095684eu);
    bool mainQuestFilterOn = IsFilterEnabled(filterEntries, filterCount, 0xa7cadc08u);
    bool sideQuestFilterOn = IsFilterEnabled(filterEntries, filterCount, 0xc52e92e5u);
    bool rumorFilterOn = IsFilterEnabled(filterEntries, filterCount, 0x8ba7c122u);
    bool townFilterOn = IsFilterEnabled(filterEntries, filterCount, 0xb5d01766u);
    bool passageFilterOn = IsFilterEnabled(filterEntries, filterCount, 0xdb17bf33u);
    const bool exploredOnly = (g_pinRevealMode == PinRevealMode::ExploredOnly);

    uint8_t* arrBase = *reinterpret_cast<uint8_t**>(client + 0x36ea00);
    uint64_t arrCount = *reinterpret_cast<uint64_t*>(client + 0x36ea08);

    struct SpirePos2D { int32_t x; int32_t z; };
    SpirePos2D discoveredSpires[32]{};
    size_t numDiscoveredSpires = 0;

    static bool s_initialRefresh = true;
    bool dirty = s_initialRefresh;
    s_initialRefresh = false;
    SyncChestsInUiArrays(client, dirty);

    auto computeMarkerKey = [](const uint8_t* m) -> uint64_t {
        uint64_t entityAndB48 = static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(m + 0x04)) |
                                (static_cast<uint64_t>(m[0x48]) << 32);
        uint64_t posX = *reinterpret_cast<const uint64_t*>(m + 0x10);
        uint64_t posY = *reinterpret_cast<const uint64_t*>(m + 0x18);
        uint64_t posZ = *reinterpret_cast<const uint64_t*>(m + 0x20);
        uint64_t tag38Val = *reinterpret_cast<const uint32_t*>(m + 0x38);
        uint64_t h = entityAndB48 * 0x9e3779b97f4a7c15ULL;
        h ^= posX * 0x9ddfea08eb382d69ULL;
        h ^= posY * 0xc2b2ae3d27d4eb4fULL;
        h ^= posZ * 0x165667b19e3779f9ULL;
        h ^= tag38Val * 0xcbf29ce484222325ULL;
        return h | 1ULL;
    };

    // replicate_map_markers_for_ui leaves m+0x3c and m+0x4c untouched when it repopulates a slot in arr0.
    // Reading them only while the slot keeps the uid they were saved under (IsSlotSaved) stops moving Base
    // NPCs from inheriting stale POI hashes left in m+0x3c.
    auto resolveMarkerMeta = [&](uint8_t* m, uint32_t& outOrigHash, uint32_t& outSavedId08) {
        if (IsSlotSaved(m)) {
            outOrigHash = *reinterpret_cast<uint32_t*>(m + 0x3c);
            outSavedId08 = *reinterpret_cast<uint32_t*>(m + 0x4c);
            return;
        }
        uint32_t tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
        uint32_t curId08 = *reinterpret_cast<uint32_t*>(m + 0x08);
        if ((tHash == 0u || tHash == kLockedFastTravelTypeHash) && m[0x48] != 0u) {
            uint32_t tag38 = *reinterpret_cast<uint32_t*>(m + 0x38);
            if (tag38 == 0xe3a3e3c1u || tag38 == 0xe623c0fbu) {
                tHash = 0x1095684eu;
            } else if (tag38 == 0x5e74cd45u || tag38 == 0x4f301a2au ||
                       tag38 == 0x06e3fedau || tag38 == 0x7312df52u ||
                       tag38 == 0x8235997bu || tag38 == 0xb487f1bbu ||
                       tag38 == 0x69adaed5u || tag38 == 0xa15c18ebu) {
                tHash = 0xa17072beu;
            }
        }
        *reinterpret_cast<uint32_t*>(m + 0x3c) = tHash;
        *reinterpret_cast<uint32_t*>(m + 0x4c) = curId08;
        g_savedSlotUid[m] = SavedSlot{*reinterpret_cast<uint32_t*>(m), *reinterpret_cast<uint32_t*>(m + 0x30),
                                      *reinterpret_cast<uint32_t*>(m + 0x08)};
        outOrigHash = tHash;
        outSavedId08 = curId08;
    };

    auto applyMarkerVisibility = [&](uint8_t* m, uint32_t origHash, uint32_t savedId08, bool show,
                                     bool showSubpinAsQuest = false, bool allowSubpinWhenShown = true) {
        uint32_t desiredHash = show ? origHash : (showSubpinAsQuest ? 0xafce0c5fu : 0u);
        uint32_t desiredId08 = (show ? allowSubpinWhenShown : showSubpinAsQuest) ? savedId08 : 0u;
        if (*reinterpret_cast<uint32_t*>(m + 0x30) != desiredHash) {
            *reinterpret_cast<uint32_t*>(m + 0x30) = desiredHash;
            dirty = true;
        }
        if (savedId08 != 0 && *reinterpret_cast<uint32_t*>(m + 0x08) != desiredId08) {
            *reinterpret_cast<uint32_t*>(m + 0x08) = desiredId08;
            dirty = true;
        }
        NoteSlotPass(m);
    };

    auto addCountForHash = [&](uint32_t h) {
        if (h == 0u) return;
        for (size_t q = 0; q < numQueries; ++q) {
            if (queryHashes[q] == h) {
                ++outCounts[q];
                break;
            }
        }
    };

    uint64_t arr0Sig = 0xcbf29ce484222325ULL;
    if (arrBase && arrCount > 0 && arrCount <= 10000) {
        for (uint64_t i = 0; i < arrCount; ++i) {
            uint8_t* m = arrBase + i * 0xA0;
            uint32_t origHash = 0u;
            uint32_t savedId08 = 0u;
            resolveMarkerMeta(m, origHash, savedId08);
            uint32_t tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            uint8_t b48 = m[0x48];
            uint64_t lastChg = *reinterpret_cast<uint64_t*>(m + 0x40);
            const bool revealedForPinMode = !exploredOnly || IsMarkerShownForExploredPins(client, m, origHash);

            uint32_t subpinQuestHash = (origHash != 0xafce0c5fu && origHash != 0x454e24f2u)
                                           ? GetQuestFilterHashForMarkerId(savedId08, true)
                                           : 0u;
            bool subpinQuestOn = (subpinQuestHash == 0xa7cadc08u)   ? mainQuestFilterOn
                               : (subpinQuestHash == 0xc52e92e5u) ? sideQuestFilterOn
                                                                  : false;
            bool allowSubpinId = (subpinQuestHash == 0u) || subpinQuestOn;

            if (b48 != 0 && origHash == 0xa17072beu) {
                bool unlockedForChar = (lastChg != 0);
                applyMarkerVisibility(m, 0xa17072beu, savedId08, spireFilterOn && unlockedForChar && revealedForPinMode,
                                      unlockedForChar && subpinQuestOn && revealedForPinMode, allowSubpinId);
                tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
                if (unlockedForChar && numDiscoveredSpires < 32) {
                    discoveredSpires[numDiscoveredSpires].x = *reinterpret_cast<int32_t*>(m + 0x14);
                    discoveredSpires[numDiscoveredSpires].z = *reinterpret_cast<int32_t*>(m + 0x24);
                    ++numDiscoveredSpires;
                }
            } else if (origHash == 0x1095684eu) {
                bool unlockedForChar = (b48 == 0) || (lastChg != 0);
                uint32_t desiredHash = !revealedForPinMode
                                           ? 0u
                                           : (fastTravelFilterOn
                                                  ? (unlockedForChar ? 0x1095684eu : kLockedFastTravelTypeHash)
                                                  : (subpinQuestOn ? 0xafce0c5fu : 0u));
                uint32_t desiredId08 = (revealedForPinMode && (fastTravelFilterOn ? allowSubpinId : subpinQuestOn)) ? savedId08 : 0u;
                if (*reinterpret_cast<uint32_t*>(m + 0x30) != desiredHash) {
                    *reinterpret_cast<uint32_t*>(m + 0x30) = desiredHash;
                    dirty = true;
                }
                if (savedId08 != 0 && *reinterpret_cast<uint32_t*>(m + 0x08) != desiredId08) {
                    *reinterpret_cast<uint32_t*>(m + 0x08) = desiredId08;
                    dirty = true;
                }
                NoteSlotPass(m);
                if (m[0x9c] != 0u) {
                    m[0x9c] = 0u;
                    dirty = true;
                }
                tHash = desiredHash;
            } else if (origHash == 0xa17072beu && numDiscoveredSpires < 32) {
                discoveredSpires[numDiscoveredSpires].x = *reinterpret_cast<int32_t*>(m + 0x14);
                discoveredSpires[numDiscoveredSpires].z = *reinterpret_cast<int32_t*>(m + 0x24);
                ++numDiscoveredSpires;
            }
        }

        g_rumorSpots.clear();
        auto noteRumor = [&](const uint8_t* rm) {
            g_rumorSpots.emplace_back(*reinterpret_cast<const int32_t*>(rm + 0x14), *reinterpret_cast<const int32_t*>(rm + 0x24));
        };
        for (uint64_t i = 0; i < arrCount; ++i) {
            uint8_t* m = arrBase + i * 0xA0;
            uint32_t origHash = 0u;
            uint32_t savedId08 = 0u;
            resolveMarkerMeta(m, origHash, savedId08);
            uint32_t tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            uint8_t b48 = m[0x48];
            uint64_t lastChg = *reinterpret_cast<uint64_t*>(m + 0x40);
            const bool revealedForPinMode = !exploredOnly || IsMarkerShownForExploredPins(client, m, origHash);

            uint32_t subpinQuestHash = (origHash != 0xafce0c5fu && origHash != 0x454e24f2u)
                                           ? GetQuestFilterHashForMarkerId(savedId08, true)
                                           : 0u;
            bool subpinQuestOn = (subpinQuestHash == 0xa7cadc08u)   ? mainQuestFilterOn
                               : (subpinQuestHash == 0xc52e92e5u) ? sideQuestFilterOn
                                                                  : false;
            bool allowSubpinId = (subpinQuestHash == 0u) || subpinQuestOn;
            // A place with an unread lore rumor about it shows as the game's "?" in its stead, exactly as vanilla does
            // (the marker's icon lookup, FUN_140d1fd90, site 3): while the game draws the place at all (any state but
            // Hidden: completed and discovered places too), no quest objective is on it, and this character doesn't
            // have the rumor's lore entry yet; none at all while the game's client +0x4642b1 switch is on. With the
            // "?" row off the place shows as itself (reported by GARONNN; checked against the game's code)
            const bool gameRumorSwapOff = client[0x4642b1] != 0;
            auto isSubpinRumor = [&](bool) {
                return GameShowsMarker(m[0x35]) && !subpinQuestHash && rumorFilterOn && !gameRumorSwapOff &&
                       g_isLoreRumorMarkerId && savedId08 != 0u && g_isLoreRumorMarkerId(savedId08) &&
                       LoreRumorFound(client, savedId08) != 1;
            };
            // The id a rumor "?" carries: a place with a rumor that a quest also names (the Sunken Ancient City: a clothes
            // rumor and Follow The Melody) gets the game's quest "!" but the rumor's card on hover. It carries the quest
            // pin's id instead, so hovering it shows the quest (requested by the user)
            auto rumorCardId = [&](bool rumor) {
                const uint32_t pinId = rumor ? QuestPinIdForPlace(client, savedId08) : 0u;
                return pinId ? pinId : savedId08;
            };

            if (origHash == 0xa17072beu) {
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if ((b48 == 0 || lastChg != 0) && revealedForPinMode) {
                    addCountForHash(0xa17072beu);
                    addCountForHash(subpinQuestHash);
                }
                continue;
            }

            if (origHash == 0x98b2d65du) {
                int32_t mx = *reinterpret_cast<int32_t*>(m + 0x14);
                int32_t mz = *reinterpret_cast<int32_t*>(m + 0x24);
                bool alreadyUnlockedHere = false;
                for (size_t s = 0; s < numDiscoveredSpires && !alreadyUnlockedHere; ++s) {
                    int64_t dx = static_cast<int64_t>(mx) - static_cast<int64_t>(discoveredSpires[s].x);
                    int64_t dz = static_cast<int64_t>(mz) - static_cast<int64_t>(discoveredSpires[s].z);
                    if (dx * dx + dz * dz < 250LL * 250LL) {
                        alreadyUnlockedHere = true;
                    }
                }
                applyMarkerVisibility(m, 0x98b2d65du, savedId08, spireFilterOn && !alreadyUnlockedHere && revealedForPinMode,
                                      !alreadyUnlockedHere && subpinQuestOn && revealedForPinMode, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (!alreadyUnlockedHere && revealedForPinMode) {
                    addCountForHash(0xa17072beu);
                    addCountForHash(subpinQuestHash);
                }
                continue;
            }

            if (origHash == 0x1095684eu || origHash == kLockedFastTravelTypeHash) {
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) {
                    addCountForHash(0x1095684eu);
                    addCountForHash(subpinQuestHash);
                }
                continue;
            }

            // Manage non-filterable (filt == 0) marker types so HIDE ALL and category filters control them
            if (origHash == 0xafce0c5fu) {
                uint32_t effHash = ClassifyQuestOrRumorMarker(savedId08, m[0x35]);
                bool showPin = (effHash == 0u)                     ? false
                             : (effHash == 0xa7cadc08u)            ? mainQuestFilterOn
                             : (effHash == 0xc52e92e5u)            ? sideQuestFilterOn
                             : (effHash == kCompletedQuestTypeHash) ? false // finished: never shown
                                                                   : rumorFilterOn;
                applyMarkerVisibility(m, origHash, savedId08, showPin && revealedForPinMode);
                if (showPin && revealedForPinMode && effHash == 0x8ba7c122u) noteRumor(m);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode && effHash != 0u && effHash != kCompletedQuestTypeHash) {
                    addCountForHash(effHash);
                }
                continue;
            }
            if (origHash == 0x454e24f2u) {
                // A main quest's area circle (Plains For First Base): like the quest's own pin, with Main Quest while
                // the quest is on, hidden once it is done or before it begins
                const bool areaQuestActive = IsActiveQuestArea(savedId08, m[0x35]);
                applyMarkerVisibility(m, origHash, savedId08, areaQuestActive && mainQuestFilterOn && revealedForPinMode);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode && areaQuestActive) addCountForHash(0xa7cadc08u);
                continue;
            }
            if (origHash == 0x813080bcu) {
                applyMarkerVisibility(m, origHash, savedId08, fastTravelFilterOn && revealedForPinMode,
                                      subpinQuestOn && revealedForPinMode, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) {
                    addCountForHash(0x1095684eu);
                    addCountForHash(subpinQuestHash);
                }
                continue;
            }
            if (origHash == 0x7b87ef66u || origHash == 0xd11531d2u || origHash == 0x111adadau) {
                // An active quest objective here shows as the quest (vanilla shows the quest, not the place), and an
                // unread rumor as the "?"
                const bool rumor = isSubpinRumor(townFilterOn);
                applyMarkerVisibility(m, origHash, rumorCardId(rumor && !subpinQuestOn),
                                      townFilterOn && revealedForPinMode && !subpinQuestOn && !rumor,
                                      subpinQuestOn || rumor, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) addCountForHash(subpinQuestHash);
                if (rumor) {
                    addCountForHash(0x8ba7c122u);
                    noteRumor(m);
                }
                continue;
            }
            if (origHash == 0x9786a9f3u) {
                const bool rumor = isSubpinRumor(passageFilterOn);
                applyMarkerVisibility(m, origHash, rumorCardId(rumor && !subpinQuestOn),
                                      passageFilterOn && revealedForPinMode && !subpinQuestOn && !rumor,
                                      subpinQuestOn || rumor, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) addCountForHash(subpinQuestHash);
                if (rumor) {
                    addCountForHash(0x8ba7c122u);
                    noteRumor(m);
                }
                continue;
            }

            if (origHash != 0u) {
                uint32_t poiFilterHash = (origHash == 0x6a56c5b3u) ? 0xa447cba3u : origHash;
                bool poiFilterOn = IsFilterEnabled(filterEntries, filterCount, poiFilterHash);
                // A place with an active quest objective (Helping Hands at a Hidden Tomb) or an unread rumor
                // about a place not yet explored (or whose row is off) shows the quest / "?" like vanilla, not the
                // place's icon that the unlock patches reveal; the quest even where the map is not explored yet.
                const bool subpinRumor = isSubpinRumor(poiFilterOn);
                if (subpinQuestOn || subpinRumor) {
                    // a place with a rumor that a quest also names (the Sunken Ancient City: a clothes rumor and Follow
                    // The Melody): the game drew the quest's "!" with the rumor's card. It carries the quest pin's id, so
                    // its card is the quest's (requested by the user)
                    applyMarkerVisibility(m, origHash, rumorCardId(subpinRumor && !subpinQuestOn), false, true, true);
                    if (subpinRumor) {
                        addCountForHash(0x8ba7c122u);
                        noteRumor(m);
                    }
                } else if (!revealedForPinMode || !poiFilterOn || GameNeverShowsPlace(savedId08) ||
                           (((g_hideCompletedPlaces && (m[0x34] == 3 || IsVisitedSanctumMarker(m, origHash))) ||
                             HandDoneHidden(m, origHash)) &&
                            !IsWaypointMarker(m))) {
                    // (state 3: the game's completed place, drawn with a check mark; hidden with Completed Places: Hide,
                    // like a sanctum you have been to, unless your waypoint is on it: hidden, it can't be clicked to
                    // clear the waypoint, which stays an empty diamond)
                    applyMarkerVisibility(m, origHash, savedId08, false, false, false);
                } else {
                    applyMarkerVisibility(m, origHash, savedId08, true, false, allowSubpinId);
                }
                tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            }

            arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ tHash ^ *reinterpret_cast<uint32_t*>(m + 0x08)) * 0x100000001b3ULL;
            if (origHash == 0u || !revealedForPinMode || GameNeverShowsPlace(savedId08)) continue;
            uint32_t countHash = (origHash == 0x6a56c5b3u) ? 0xa447cba3u : origHash;
            addCountForHash(countHash);
            addCountForHash(subpinQuestHash);
        }
    }

#ifdef UMM_DEV_BRIDGE
    if (g_dumpMarkersRequested) {
        g_dumpMarkersRequested = false;
        const int dumped = DumpMarkersToFile(client);
        if (g_log) g_log("Dev: markers_dump.txt written (%d markers)", dumped);
    }
#endif
    static uint64_t s_lastArr0Sig = 0ULL;
    if (s_lastArr0Sig != arr0Sig) {
        if (s_lastArr0Sig != 0ULL) {
            dirty = true;
        }
        s_lastArr0Sig = arr0Sig;
    }

    // Count markers in the other 3 UI marker arrays:
    // +0x396a28 (custom pins / waypoints), +0x3bea50 (Hollow Halls), +0x3e6a78 (Player Graves)
    constexpr size_t kExtraArrayOffsets[3] = { 0x396a28, 0x3bea50, 0x3e6a78 };
    for (size_t a = 0; a < 3; ++a) {
        uint8_t* extraBase = *reinterpret_cast<uint8_t**>(client + kExtraArrayOffsets[a]);
        uint64_t extraCount = *reinterpret_cast<uint64_t*>(client + kExtraArrayOffsets[a] + 8);
        if (!extraBase || extraCount == 0 || extraCount > 10000) continue;
        for (uint64_t i = 0; i < extraCount; ++i) {
            uint8_t* m = extraBase + i * 0xA0;
            uint32_t uid = *reinterpret_cast<uint32_t*>(m + 0x00);
            if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) continue;
            uint32_t countHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            if (a == 1) {
                // Hollow Halls: the game draws them from this array, so Pins: EXPLORED hides them here as in arr0
                uint32_t origHash = 0u, savedId08 = 0u;
                resolveMarkerMeta(m, origHash, savedId08);
                if (origHash == 0u) continue;
                const bool revealed = !exploredOnly || IsMarkerShownForExploredPins(client, m, origHash);
                applyMarkerVisibility(m, origHash, savedId08, revealed);
                if (!revealed) continue;
                countHash = origHash;
            }
            if (countHash == 0) continue;
            if (exploredOnly && !IsMarkerShownForExploredPins(client, m, countHash)) continue;
            for (size_t q = 0; q < numQueries; ++q) {
                if (queryHashes[q] == countHash) {
                    ++outCounts[q];
                    break;
                }
            }
        }
    }

    int otherPlayerCount = 0;
    {
        uint8_t* playerArray = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
        uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
        uint32_t localPlayerId = *reinterpret_cast<uint32_t*>(client + 0x18);
        if (playerArray && playerCount > 0 && playerCount <= 64) {
            for (uint64_t i = 0; i < playerCount; ++i) {
                uint8_t* p = playerArray + i * 0x240;
                uint32_t pid = *reinterpret_cast<uint32_t*>(p + 0x00);
                if (pid == 0 || pid == localPlayerId || p[0x207] != 0) continue;
                ++otherPlayerCount;
            }
        }
    }

    int unlootedGoldCount = 0;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        if (!IsGoldChest(c) || IsChestCurrentlyCleared(c)) continue;
        if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
        ++unlootedGoldCount;
    }
    int unlootedSilverCount = 0;
    for (size_t c = kGoldChestCount; c < kWoodChestFirst; ++c) {
        if (IsChestCurrentlyCleared(c)) continue;
        if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
        ++unlootedSilverCount;
    }
    int unlootedWoodCount = 0;
    for (size_t c = kWoodChestFirst; c < kEndChestFirst; ++c) {
        if (IsChestCurrentlyCleared(c)) continue;
        if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
        ++unlootedWoodCount;
    }

    for (size_t q = 0; q < numQueries; ++q) {
        if (queryHashes[q] == kGoldChestTypeHash) {
            outCounts[q] = unlootedGoldCount;
        } else if (queryHashes[q] == kSilverChestTypeHash) {
            outCounts[q] = unlootedSilverCount;
        } else if (queryHashes[q] == kWoodChestTypeHash) {
            outCounts[q] = unlootedWoodCount;
        } else if (queryHashes[q] == kOtherPlayerTypeHash) {
            outCounts[q] = otherPlayerCount;
        } else if (queryHashes[q] == kTownPortalTypeHash) {
            outCounts[q] = static_cast<int>(TownPortalCount());
        } else if (IsOreHash(queryHashes[q])) {
            const size_t ore = OreIndexOf(queryHashes[q]);
            outCounts[q] = (ore == kLoreFirst || ore == kBossesFirst || ore == kCosmeticFirst ||
                            (ore >= kUniqueFirst && ore < kUniqueFirst + kUniqueCount))
                               ? KnowledgeRemaining(ore)
                               : OreDepositCount(ore);
        }
    }

    if (dirty) {
        ++(*reinterpret_cast<uint32_t*>(client + 0x36e9fc));
        if (mapState) {
            *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
            *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
        }
    }
}

struct PulsingPlace {
    float wx;
    float wz;
};

struct FindPlaceEntry {
    float wx;
    float wz;
    float distMeters;
    uint32_t iconTypeHash;
    uint32_t filterTypeHash;
    bool isFilterOn;
    bool isLocked;
    int matchTier;
    std::string title;
    std::string typeLabel;
};

struct FindDisplayRow {
    bool isGroupRow;
    bool isToggleFilterRow;
    std::string displayLine;
    std::vector<size_t> matchIndices;
};

constexpr int kMaxFindVisibleRows = 7;

bool g_findDialogOpen = false;
bool g_findInputReady = false;
bool g_findRespectFilters = true;
bool g_findExactMatch = false; // Find's Exact match: only names holding the whole text ([MinimapV2] FindExactMatch)
int g_findExactHidden = 0;     // how many matches Exact match left out
int g_findClosingFrames = 0;
bool g_waitReleaseAfterFindClose = false;
bool g_panelClickLatch = false;
bool g_panelCurrentlyHovered = false;

// Gamepad or mouse? The Map Key blocks the map's input while the mouse cursor is over it, so a click on the panel
// does not also click the map. With a gamepad the hidden Windows cursor stays wherever it was last; after tabbing
// to the map through the game menu it can sit over the panel, and the block then froze the gamepad's map cursor.
// So the panel only blocks while the mouse is the device in use: the one that moved or clicked last (XInput pads).
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
std::string GetMinimapConfigPath();
// [General] WindowsJoysticks=false: pads that are not XInput are not looked for at all (written as true the first time,
// so it can be found in the file)
bool WindowsJoysticksInConfig() {
    async_io::Flush(); // the writer's queue on disk before reading
    const std::string cfg = GetMinimapConfigPath();
    char v[16]{};
    GetPrivateProfileStringA("General", "WindowsJoysticks", "", v, sizeof(v), cfg.c_str());
    if (!v[0]) {
        WritePrivateProfileStringA("General", "WindowsJoysticks", "true", cfg.c_str());
        return true;
    }
    return !(_strnicmp(v, "false", 5) == 0 || v[0] == '0' || _strnicmp(v, "no", 2) == 0);
}

bool MapHintBarWorks();
// The words after the button in each part of a pad strip's line ("A: Select   B: Back": Select, Back).
std::vector<std::string> PadStripLabels(const char* line) {
    std::vector<std::string> labels;
    std::string text(line);
    while (!text.empty()) {
        const size_t gap = text.find("   ");
        std::string part = text.substr(0, gap);
        text = gap == std::string::npos ? std::string() : text.substr(gap + 3);
        size_t colon = part.rfind(':');
        size_t after = colon == std::string::npos ? std::string::npos : colon + 1;
        const size_t wide = part.rfind("\xEF\xBC\x9A"); // the full-width colon
        if (wide != std::string::npos && (after == std::string::npos || wide + 3 > after)) after = wide + 3;
        if (after == std::string::npos) return {};
        while (after < part.size() && part[after] == ' ') ++after;
        labels.push_back(part.substr(after));
    }
    return labels;
}

// The Map Key with a controller: View moves the pad into the panel, the D-pad steps between its controls, A clicks the
// one picked, B goes back to the map. The game reads Xbox-style pads (XInput, Steam Input's too) with XInputGetState,
// hooked so that while the panel has the pad the game sees it idle, and View is the panel's on the World Map.
std::atomic<uint16_t> g_padSwallow[XUSER_MAX_COUNT]{}; // buttons still down when the panel let go: hidden until released
XInputGetStateFn g_origXInputGetState = nullptr;      // the real XInputGetState once hooked
uint8_t* g_xinputHookTarget = nullptr;

XInputGetStateFn RealXInputGetState() {
    if (g_origXInputGetState) return g_origXInputGetState;
    static XInputGetStateFn fn = [] {
        HMODULE x = LoadLibraryW(L"xinput1_4.dll");
        if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
        return x ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(x, "XInputGetState")) : nullptr;
    }();
    return fn;
}

XInputGetStateFn g_origXInputGetStateEx = nullptr; // ordinal 100: the same, with the Guide button
std::atomic<uint32_t> g_xinputCalls{0}, g_xinputExCalls{0};
inp::SharedBinding g_padChords[4]; // glow, Find on the map, the minimap's zoom in and out
inp::SharedBinding g_padEnter;     // the button that enters the Map Key
inp::SharedBinding g_padMapChords[4]; // on the map only: mark a chest, hide a row, undo, redo
std::atomic<uint16_t> g_padSearchHide{0};

// Both read functions: while the panel has the pad the game gets it idle
DWORD PadFilterState(DWORD r, DWORD user, XINPUT_STATE* st) {
#ifdef GREENEYE_INPUT_TEST
    // tools/inputtest.py --emulate: this hook is the lowest in the chain, so the pad starts here (common/input_binds.h)
    if (st && r != ERROR_SUCCESS && inp::EmulatingPad(user)) {
        inp::EmulatedState(st);
        r = ERROR_SUCCESS;
    }
#endif
    if (r != ERROR_SUCCESS || !st || user >= XUSER_MAX_COUNT) return r;
    static uint16_t held[XUSER_MAX_COUNT]{};
    XINPUT_GAMEPAD& g = st->Gamepad;
    // (the filter menu's tick: the World Map tab, the game's own map with Vanilla World Map too)
    const bool onMap = GetTickCount64() - std::max<uint64_t>(static_cast<uint64_t>(g_mapKeyLastFrameTick), g_mapMenuDrawnTick) < 250;
    if (onMap && g_padKeyFocus) {
        held[user] = g.wButtons;
        std::memset(&g, 0, sizeof(g));
        return r;
    }
    // The chords' buttons once their modifiers are held, and the open search's: hidden until they come up
    const inp::PadState pad{g.wButtons, g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD, g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD};
    const uint16_t chords = static_cast<uint16_t>((onMap ? g_padSearchHide.load() | inp::PadHideMask(g_padMapChords[0].Get(), pad) | inp::PadHideMask(g_padMapChords[1].Get(), pad) |
                                                                    inp::PadHideMask(g_padMapChords[2].Get(), pad) | inp::PadHideMask(g_padMapChords[3].Get(), pad) : uint16_t(0)) | inp::PadHideMask(g_padChords[0].Get(), pad) |
                                                  inp::PadHideMask(g_padChords[1].Get(), pad) |
                                                  inp::PadHideMask(g_padChords[2].Get(), pad) |
                                                  inp::PadHideMask(g_padChords[3].Get(), pad));
    held[user] = static_cast<uint16_t>((held[user] | g_padSwallow[user].exchange(0) | chords) & g.wButtons);
    g.wButtons &= static_cast<WORD>(~held[user]);
    if (onMap) g.wButtons &= static_cast<WORD>(~inp::PadButtons(g_padEnter.Get()));
    return r;
}
DWORD WINAPI XInputGetStateHook(DWORD user, XINPUT_STATE* st) {
    greeneye::Lease lease;
    const DWORD r = g_origXInputGetState(user, st);
    if (!lease) return r;
    ++g_xinputCalls;
    return PadFilterState(r, user, st);
}
DWORD WINAPI XInputGetStateExHook(DWORD user, XINPUT_STATE* st) {
    greeneye::Lease lease;
    const DWORD r = g_origXInputGetStateEx(user, st);
    if (!lease) return r;
    ++g_xinputExCalls;
    return PadFilterState(r, user, st);
}

// xinput1_4's XInputGetState: "mov [rsp+8], rbx; push rsi; push rdi; push r14; sub rsp, 30h" (13 bytes)
void InstallXInputHook() {
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");
    auto* fn = x ? reinterpret_cast<uint8_t*>(GetProcAddress(x, "XInputGetState")) : nullptr;
    static const uint8_t kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
    if (fn && StartsAsExpected(fn, kPrologue, sizeof(kPrologue))) {
        g_origXInputGetState = reinterpret_cast<XInputGetStateFn>(
            InstallHook(fn, reinterpret_cast<void*>(&XInputGetStateHook), sizeof(kPrologue)));
        if (g_origXInputGetState) {
            g_xinputHookTarget = fn;
            inp::PadReadFn() = g_origXInputGetState; // the mod's own reads, past its hook
        }
    }
    // ordinal 100: "mov [rsp+8], rbx; mov [rsp+10h], rbp; push rdi; sub rsp, 30h" (15 bytes)
    auto* fnEx = x ? reinterpret_cast<uint8_t*>(GetProcAddress(x, reinterpret_cast<LPCSTR>(100))) : nullptr;
    static const uint8_t kPrologueEx[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30};
    if (g_origXInputGetState && fnEx && StartsAsExpected(fnEx, kPrologueEx, sizeof(kPrologueEx))) {
        g_origXInputGetStateEx = reinterpret_cast<XInputGetStateFn>(
            InstallHook(fnEx, reinterpret_cast<void*>(&XInputGetStateExHook), sizeof(kPrologueEx)));
    }
    if (g_log) g_log("Controllers: Map Key by controller %s", g_origXInputGetState ? "on" : "off (XInputGetState not as expected)");
}

// winmm's joystick calls, kept apart so they can run under SEH: a fault in one (a pad switching off while it is read)
// turns the Windows joystick check off for the session instead of taking the game down
using JoyGetPosExFn = MMRESULT(WINAPI*)(UINT, JOYINFOEX*);
using JoyGetDevCapsFn = MMRESULT(WINAPI*)(UINT_PTR, JOYCAPSW*, UINT);
MMRESULT GuardedJoyGetPos(JoyGetPosExFn fn, UINT j, JOYINFOEX* ji, bool& faulted) {
    __try {
        return fn(j, ji);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return JOYERR_UNPLUGGED;
    }
}
MMRESULT GuardedJoyGetDevCaps(JoyGetDevCapsFn fn, UINT j, JOYCAPSW* caps, bool& faulted) {
    __try {
        return fn(j, caps, sizeof(*caps));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return JOYERR_UNPLUGGED;
    }
}

bool UsingGamepad() {
    const XInputGetStateFn getState = RealXInputGetState();
    static uint64_t padTick = 0, mouseTick = 1;
    static POINT lastCursor{};
    static uint64_t lastPoll = 0;
    const uint64_t now = GetTickCount64();
    if (now - lastPoll < 15) return padTick > mouseTick;
    lastPoll = now;
    if (getState) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE st{};
            if (getState(i, &st) != ERROR_SUCCESS) continue;
            const XINPUT_GAMEPAD& g = st.Gamepad;
            const bool active = g.wButtons != 0 || g.bLeftTrigger > 30 || g.bRightTrigger > 30 ||
                                std::abs(g.sThumbLX) > 8000 || std::abs(g.sThumbLY) > 8000 ||
                                std::abs(g.sThumbRX) > 8000 || std::abs(g.sThumbRY) > 8000;
            // A stick held steady sends no new packets: held counts as in use too. Only new packets counted, the
            // mouse took over a quarter second into a steady push (the game moves the hidden cursor meanwhile), and
            // when that cursor sat on the Map Key the map's input was blocked, the pad frozen until the mouse moved
            // (reported by FamanFortune)
            if (active) padTick = now;
        }
    }
    // Pads that are not XInput (a DualShock / DualSense without Steam's XInput, most others) only show as Windows
    // joysticks: a button down or an axis off where it rested when first seen. The empty slots are asked once, then
    // again only when Windows' list of input devices changes: asking one makes Windows look over its devices, which
    // played the device connect / disconnect sound on every map open for TillYouCry (and their game crashed after an
    // Alt+Tab with the map, not without the mod), while it asked an empty slot every 200 ms (1.3.19 to 1.3.29).
    // A pad switching off with the map open crashed the game (microsoftenator, 1.3.30): no pad is read while Windows'
    // device list is changing (2 s after it last changed), Microsoft's pads (Xbox, and Steam Input's virtual ones) are
    // left to XInput above, and a fault inside winmm turns the check off for the session.
    static const bool windowsJoysticks = WindowsJoysticksInConfig();
    static bool joyFaulted = false;
    if (windowsJoysticks && !joyFaulted) {
        static JoyGetPosExFn joyGetPos = nullptr;
        static JoyGetDevCapsFn joyGetDevCaps = nullptr;
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            if (HMODULE w = LoadLibraryW(L"winmm.dll")) {
                joyGetPos = reinterpret_cast<JoyGetPosExFn>(GetProcAddress(w, "joyGetPosEx"));
                joyGetDevCaps = reinterpret_cast<JoyGetDevCapsFn>(GetProcAddress(w, "joyGetDevCapsW"));
            }
        }
        constexpr UINT kJoys = 16;
        static bool present[kJoys]{};
        static bool restKnown[kJoys]{};
        static DWORD rest[kJoys][6]{};
        static UINT deviceCount = UINT_MAX; // Windows' input devices when the slots were last asked
        static uint64_t settleUntil = 0;    // the list changed: no pad is asked before this
        static bool scanPending = true;
        if (joyGetPos) {
            UINT n = 0;
            if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1) &&
                n != deviceCount) {
                if (deviceCount != UINT_MAX) settleUntil = now + 2000;
                deviceCount = n;
                scanPending = true;
                std::memset(present, 0, sizeof(present));
                std::memset(restKnown, 0, sizeof(restKnown));
            }
            const bool scan = scanPending && now >= settleUntil; // ask the empty slots this time
            if (scan) scanPending = false;
            for (UINT j = 0; j < kJoys && now >= settleUntil && !joyFaulted; ++j) {
                if (!present[j] && !scan) continue;
                JOYINFOEX ji{};
                ji.dwSize = sizeof(ji);
                ji.dwFlags = JOY_RETURNALL;
                const bool was = present[j];
                present[j] = GuardedJoyGetPos(joyGetPos, j, &ji, joyFaulted) == JOYERR_NOERROR;
                if (present[j] && !was && getState && joyGetDevCaps) {
                    JOYCAPSW caps{};
                    if (GuardedJoyGetDevCaps(joyGetDevCaps, j, &caps, joyFaulted) == JOYERR_NOERROR && caps.wMid == 0x045E) {
                        present[j] = false; // a Microsoft pad: XInput reads it
                    }
                }
                if (joyFaulted && g_log) g_log("Controllers: Windows joystick %u faulted; the Windows joystick check is off", j);
                if (!present[j]) {
                    restKnown[j] = false;
                    continue;
                }
                const DWORD axes[6] = {ji.dwXpos, ji.dwYpos, ji.dwZpos, ji.dwRpos, ji.dwUpos, ji.dwVpos};
                if (!restKnown[j]) {
                    std::memcpy(rest[j], axes, sizeof(axes));
                    restKnown[j] = true;
                    continue;
                }
                bool active = ji.dwButtons != 0 || (ji.dwPOV != JOY_POVCENTERED && ji.dwPOV != 0xffffu);
                for (int a = 0; a < 6 && !active; ++a) {
                    active = std::abs(static_cast<long>(axes[a]) - static_cast<long>(rest[j][a])) > 12000;
                }
                if (active) padTick = now;
            }
        }
    }
    POINT pt{};
    const bool buttons = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                         (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
    if (GetCursorPos(&pt)) {
        const bool moved = pt.x != lastCursor.x || pt.y != lastCursor.y;
        lastCursor = pt;
        // The game may move the hidden cursor itself while the pad steers: a move right after pad input is the pad's
        if (buttons || (moved && now - padTick > 250)) mouseTick = now;
    }
    return padTick > mouseTick;
}

// The panel's controls, as the hover tests of this frame met them (each isHovered / isHov call): the D-pad steps
// between last frame's. The pad's cursor is the picked one's centre, so hover, highlight and click go through the
// mouse code as they are.
extern float g_lastPanelX, g_lastPanelY, g_lastPanelW, g_lastPanelH; // below
extern float g_lastSettingsX, g_lastSettingsY, g_lastSettingsW, g_lastSettingsH;
struct PadTarget { float x, y, w, h; };
std::vector<PadTarget> g_padTargets, g_padTargetsNow;
float g_padSelX = -1.0f, g_padSelY = -1.0f;
float g_padColX = 0.0f; // the column the D-pad keeps going up and down
// Vanilla World Map on: View on the game's map brings the mod's back (switched from the HUD pass, PlayStateUiHook),
// and the pad goes into its Map Key
bool g_padVanillaOffPending = false;
bool g_padEnterPending = false;

void PadTargetSeen(float x, float y, float w, float h) {
    if (!g_padKeyFocus || w < 6.0f || h < 6.0f || h > 44.0f) return; // not the panel's whole box, the scroll track
    for (const PadTarget& t : g_padTargetsNow)
        if (std::fabs(t.x - x) < 0.5f && std::fabs(t.y - y) < 0.5f && std::fabs(t.w - w) < 0.5f && std::fabs(t.h - h) < 0.5f) return;
    g_padTargetsNow.push_back({x, y, w, h});
}

struct PadInput {
    uint16_t pressed = 0;
    inp::PadState state; // what those presses were read from
    int pads = 0;        // connected
};
// New presses on the connected pads taken together, the D-pad repeating while held; read past the hook. Together, so a
// press that reaches two sources a moment apart (Steam Input's virtual pad beside the real one, the dev build's test
// input) counts once: counted per source it is View into the Map Key and straight out again.
PadInput ReadPadPresses() {
    PadInput in;
    const XInputGetStateFn get = RealXInputGetState();
    if (!get) return in;
    static uint16_t prev = 0;
    static uint64_t repeatAt = 0;
    const uint64_t now = GetTickCount64();
    uint16_t down = 0;
    for (DWORD u = 0; u < XUSER_MAX_COUNT; ++u) {
        XINPUT_STATE st{};
        if (get(u, &st) != ERROR_SUCCESS) continue;
        ++in.pads;
        down |= st.Gamepad.wButtons;
        in.state.lt |= st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        in.state.rt |= st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    }
#ifdef GREENEYE_INPUT_TEST
    {
        down |= inp::ReadPad().buttons;
        in.state.lt |= inp::ReadPad().lt;
        in.state.rt |= inp::ReadPad().rt;
    }
#endif
    in.pressed = down & static_cast<uint16_t>(~prev);
    prev = down;
    constexpr uint16_t kDpad = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                               XINPUT_GAMEPAD_DPAD_RIGHT;
    if (in.pressed & kDpad) repeatAt = now + 380;
    else if ((down & kDpad) && now >= repeatAt) {
        in.pressed |= down & kDpad;
        repeatAt = now + 110;
    }
    in.state.buttons = down;
    return in;
}

// Each map frame before the panel's hover tests: View in and B out, the D-pad's step. True while the pad drives the
// panel; then mx, my are the picked control's centre and clicked is A.
bool PadKeyFrame(bool allowed, float& mx, float& my, bool& clicked, float& scroll) {
    g_padTargets.swap(g_padTargetsNow);
    g_padTargetsNow.clear();
    if (!g_origXInputGetState) return false;
    PadInput raw = ReadPadPresses();
    // The rebindable buttons ([Keys] MapKey*) as the ones the code below moves by.
    PadInput in;
    {
        // Held from the same read as the presses: inp::ReadPad is cached for the clock's tick (some 16 ms), so at a high
        // frame rate it could still say up for a button just pressed, and the press was lost
        auto set = [&](const inp::Binding& b, uint16_t canonical) {
            if (inp::PadPressed(b, raw.pressed, raw.state)) in.pressed |= canonical;
        };
        set(g_keys.padMapKey, XINPUT_GAMEPAD_BACK);
        set(g_keys.padBack, XINPUT_GAMEPAD_B);
        set(g_keys.padClick, XINPUT_GAMEPAD_A);
        set(g_keys.padUp, XINPUT_GAMEPAD_DPAD_UP);
        set(g_keys.padDown, XINPUT_GAMEPAD_DPAD_DOWN);
        set(g_keys.padLeft, XINPUT_GAMEPAD_DPAD_LEFT);
        set(g_keys.padRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    }
    auto leave = [&](const char* why) {
        g_padKeyFocus = false;
        for (auto& sw : g_padSwallow) sw = 0xFFFF; // what is still down stays the panel's until released
        if (g_log) g_log("Controllers: left the Map Key (%s)", why);
    };
    if (!allowed) {
        if (g_padKeyFocus) leave("not available now");
        return false;
    }
    if (!g_padKeyFocus) {
        if (!(in.pressed & XINPUT_GAMEPAD_BACK) && !g_padEnterPending) return false;
        g_padEnterPending = false;
        g_padKeyFocus = true;
        g_panelCollapsed = false;
        if (g_log) g_log("Controllers: in the Map Key (View); the game's pad reads so far: %u XInputGetState, %u Ex%s; %d pads",
                         g_xinputCalls.load(), g_xinputExCalls.load(), g_origXInputGetStateEx ? "" : " (not hooked)", raw.pads);
        g_padSelX = g_lastPanelX + 60.0f; // the first section's row
        g_padSelY = g_lastPanelY + 120.0f;
        g_padColX = 0.0f;
        g_padTargets.clear();
    } else if (in.pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) {
        leave("button");
        return false;
    }
    // The controls, minus a row that holds a choice of several (Settings' segmented rows: their segments are the
    // stops), each in its zone: the Settings card, the panel, or elsewhere. The D-pad goes row by row within a zone,
    // keeping its column; left / right along a row, and past its end into the zone beside it.
    std::vector<PadTarget> stops;
    for (const PadTarget& t : g_padTargets) {
        int inside = 0;
        for (const PadTarget& o : g_padTargets)
            if (&o != &t && o.x >= t.x - 0.5f && o.y >= t.y - 0.5f && o.x + o.w <= t.x + t.w + 0.5f &&
                o.y + o.h <= t.y + t.h + 0.5f && (o.w < t.w - 1.0f || o.h < t.h - 1.0f)) ++inside;
        if (inside < 2) stops.push_back(t);
    }
    auto zoneOf = [](float x, float y) {
        if (g_lastSettingsW > 0.0f && x >= g_lastSettingsX && x <= g_lastSettingsX + g_lastSettingsW &&
            y >= g_lastSettingsY && y <= g_lastSettingsY + g_lastSettingsH) return 1;
        if (x >= g_lastPanelX && x <= g_lastPanelX + g_lastPanelW && y >= g_lastPanelY && y <= g_lastPanelY + g_lastPanelH)
            return 2;
        return 0;
    };
    auto cxOf = [](const PadTarget& t) { return t.x + t.w * 0.5f; };
    auto cyOf = [](const PadTarget& t) { return t.y + t.h * 0.5f; };
    // The control under the picked spot, else the nearest (rows slide under it as the list scrolls)
    int cur = -1;
    float best = 1e30f;
    for (size_t i = 0; i < stops.size(); ++i) {
        const PadTarget& t = stops[i];
        const bool under = g_padSelX >= t.x && g_padSelX <= t.x + t.w && g_padSelY >= t.y && g_padSelY <= t.y + t.h;
        const float dx = cxOf(t) - g_padSelX, dy = cyOf(t) - g_padSelY;
        const float d = under ? -1.0f : dx * dx + dy * dy;
        if (d < best) { best = d; cur = static_cast<int>(i); }
    }
    float& colX = g_padColX;
    if (cur >= 0) {
        const PadTarget& c = stops[cur];
        const float cx = cxOf(c), cy = cyOf(c);
        const int zone = zoneOf(cx, cy);
        constexpr float kBand = 6.0f; // centres this close in height: one row
        const bool vert = (in.pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) != 0;
        const bool down = (in.pressed & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        const bool horiz = !vert && (in.pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) != 0;
        const bool right = (in.pressed & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        int next = -1;
        if (vert) {
            float rowY = 1e30f; // the next row's height
            for (const PadTarget& t : stops) {
                if (zoneOf(cxOf(t), cyOf(t)) != zone) continue;
                const float dy = down ? cyOf(t) - cy : cy - cyOf(t);
                if (dy > kBand && dy < std::fabs(rowY - cy)) rowY = cyOf(t);
            }
            float bestDx = 1e30f;
            for (size_t i = 0; rowY < 1e29f && i < stops.size(); ++i) {
                const PadTarget& t = stops[i];
                if (zoneOf(cxOf(t), cyOf(t)) != zone || std::fabs(cyOf(t) - rowY) > kBand) continue;
                const float dx = colX >= t.x && colX <= t.x + t.w ? 0.0f : std::fabs(cxOf(t) - colX);
                if (dx < bestDx) { bestDx = dx; next = static_cast<int>(i); }
            }
            if (next < 0 && zone == 2) scroll += down ? 23.0f : -23.0f; // the list's end in view: it scrolls on
            // Settings wraps around: up from its top row to its bottom one, down from the bottom to the top
            if (next < 0 && zone == 1) {
                float edgeY = down ? 1e30f : -1e30f;
                for (const PadTarget& t : stops) {
                    if (zoneOf(cxOf(t), cyOf(t)) != 1) continue;
                    if (down ? cyOf(t) < edgeY : cyOf(t) > edgeY) edgeY = cyOf(t);
                }
                float wrapDx = 1e30f;
                for (size_t i = 0; std::fabs(edgeY) < 1e29f && i < stops.size(); ++i) {
                    const PadTarget& t = stops[i];
                    if (zoneOf(cxOf(t), cyOf(t)) != 1 || std::fabs(cyOf(t) - edgeY) > kBand ||
                        std::fabs(cyOf(t) - cy) <= kBand) continue;
                    const float dx = colX >= t.x && colX <= t.x + t.w ? 0.0f : std::fabs(cxOf(t) - colX);
                    if (dx < wrapDx) { wrapDx = dx; next = static_cast<int>(i); }
                }
            }
        } else if (horiz) {
            float bestDx = 1e30f;
            for (size_t i = 0; i < stops.size(); ++i) {
                const PadTarget& t = stops[i];
                const float dx = right ? cxOf(t) - cx : cx - cxOf(t);
                if (dx > 3.0f && zoneOf(cxOf(t), cyOf(t)) == zone && std::fabs(cyOf(t) - cy) <= kBand && dx < bestDx) {
                    bestDx = dx;
                    next = static_cast<int>(i);
                }
            }
            // Past the row's end: the zone beside it, at the nearest height
            float bestScore = 1e30f;
            for (size_t i = 0; next < 0 && i < stops.size(); ++i) {
                const PadTarget& t = stops[i];
                const float dx = right ? cxOf(t) - cx : cx - cxOf(t);
                if (dx <= 3.0f || zoneOf(cxOf(t), cyOf(t)) == zone) continue;
                const float score = std::fabs(cyOf(t) - cy) * 4.0f + dx;
                if (score < bestScore) { bestScore = score; next = static_cast<int>(i); }
            }
        }
        const PadTarget& to = next >= 0 ? stops[next] : c;
        g_padSelX = cxOf(to);
        g_padSelY = cyOf(to);
        if (horiz && next >= 0) colX = g_padSelX;
        if (colX == 0.0f) colX = g_padSelX;
    }
    mx = g_padSelX;
    my = g_padSelY;
    clicked = (in.pressed & XINPUT_GAMEPAD_A) != 0;
    return true;
}
float g_lastRootX = 0.0f;
float g_lastRootY = 0.0f;
float g_lastRootW = 1920.0f;
float g_lastRootH = 1080.0f;
float g_lastPanelX = 0.0f;
float g_lastPanelY = 0.0f;
float g_lastPanelW = 0.0f;
float g_lastPanelH = 0.0f;
float g_lastSettingsX = 0.0f;
float g_lastSettingsY = 0.0f;
float g_lastSettingsW = 0.0f;
float g_lastSettingsH = 0.0f;
std::string g_findQuery;
int g_findSelectedIndex = -1;
int g_findHoveredIndex = -1;
int g_findScrollOffset = 0;
bool g_findScrollDragging = false; // dragging the result list's scroll bar
int g_findFilteredOutCount = 0;
std::vector<FindPlaceEntry> g_findMatchedPlaces;
std::vector<FindDisplayRow> g_findDisplayRows;
std::vector<PulsingPlace> g_pulsingPlaces;
uint64_t g_pulseStartTick = 0;
bool g_shiftFWasDown = false;
bool g_findKeysWasDown[256]{};
uint64_t g_lastClickTick = 0;
int g_lastClickedRow = -1;

uint8_t* g_mapFindDialog = nullptr;
char g_suggestionBufs[8][192]{};
char g_hintLineBuf[256]{};

UiText MakeRuntimeUiText(const char* s) {
    if (!s) return UiText{"", 0, 0xcbf29ce484222325ull};
    size_t len = 0;
    while (s[len] != '\0') ++len;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(s[i]));
        h *= 0x100000001b3ull;
    }
    return UiText{s, len, h};
}

// Find hot reloaded keeps its old copies loaded: the newest (loaded last) is the one to use. True when it changed.
HMODULE g_findModule = nullptr;
bool EnsureFindDialogResolved() {
    auto tryResolveModule = [](HMODULE mod, const wchar_t* modPath) -> bool {
        if (!mod) return false;
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) return false;
        auto* fbase = static_cast<uint8_t*>(mi.lpBaseOfDll);
        size_t fsize = static_cast<size_t>(mi.SizeOfImage);
        if (!fbase || fsize < 0x10000) return false;

        uint8_t* fnCreate = ScanPattern(fbase, fsize, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 70 48 8B F2 48 8B F9");
        // From Find 1.2.10 by name (its bytes change with the compiler's inlining). Find 1.2.9 has neither this name nor the
        // bytes: its dialog's fields moved, so it must not be used, and the map's own dialog stands in.
        auto* fnReset = reinterpret_cast<uint8_t*>(GetProcAddress(mod, "FindMapDialogReset"));
        if (!fnReset) fnReset = ScanPattern(fbase, fsize, "48 89 5C 24 10 57 48 81 EC 80 00 00 00 0F 29 74 24 70 48 8B D9 48 8D 81 98 07 00 00");
        uint8_t* fnGetQuery = ScanPattern(fbase, fsize, "48 8B 81 08 06 00 00 41 B8 3F 00 00 00 48 83 F8 40");
        if (!fnCreate || !fnReset || !fnGetQuery) return false;

        // Find's DrawDialog by its own code: up to Find 1.2.2 it sat right after DialogText (+0x30), in 1.2.3 the
        // linker put another function there, and calling that froze the map's Find (reported by ecktos1)
        uint8_t* fnRunFrame = ScanPattern(fbase, fsize,
            "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 48 89 0D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 FF D0 "
            "48 C7 05 ?? ?? ?? ?? 00 00 00 00 85 C0");
        if (!fnRunFrame) {
            if (g_log) g_log("MapKeyPanel: Find's dialog draw not found in %ls; the map uses its own search box",
                             modPath ? modPath : L"<module>");
            return false;
        }
        if (fnRunFrame[6] == 0x48 && fnRunFrame[7] == 0x8B && fnRunFrame[8] == 0x05) {
            int32_t tdDisp = 0;
            std::memcpy(&tdDisp, fnRunFrame + 9, 4);
            void* findTdPtr = *reinterpret_cast<void**>(fnRunFrame + 13 + tdDisp);
            if (!findTdPtr) {
                return false;
            }
        }

        uint8_t* fnGetCursor = ScanPattern(fbase, fsize, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B F2 48 8B F9");
        uint8_t* fnHitTest = ScanPattern(fbase, fsize, "48 83 EC 78 F3 0F 10 05 ?? ?? ?? ?? 0F 28 EA F3 0F 10 25");
        uint8_t* fnCloseMid = ScanPattern(fbase, fsize, "0F B6 80 70 FE 02 00 84 C0");
        uint8_t* fnSetText = ScanPattern(fbase, fsize, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 BF 3F 00 00 00 48 8B F1");
        uint8_t* fnWheelXchg = ScanPattern(fbase, fsize, "87 0D ?? ?? ?? ?? 85 C9 74 ?? 83 ?? 07");

        g_findCreateDialog = reinterpret_cast<FindCreateDialogFn>(fnCreate);
        g_findResetDialog = reinterpret_cast<FindResetDialogFn>(fnReset);
        g_findGetQueryStr = reinterpret_cast<FindGetQueryStrFn>(fnGetQuery);
        g_findEnsureFocus = reinterpret_cast<FindEnsureFocusFn>(fnGetQuery + 0x20);
        g_findRunDialogFrame = reinterpret_cast<FindRunDialogFrameFn>(fnRunFrame);
        g_findGetUiCursorPos = reinterpret_cast<FindGetUiCursorPosFn>(fnGetCursor);
        g_findHitTestRow = reinterpret_cast<FindHitTestRowFn>(fnHitTest);
        g_findCloseDialog = fnCloseMid ? reinterpret_cast<FindCloseDialogFn>(fnCloseMid - 0x13) : nullptr;
        g_findSetTextFieldValue = reinterpret_cast<FindSetTextFieldValueFn>(fnSetText);
        g_findWheelDeltaPtr = nullptr;
        if (fnWheelXchg) {
            int32_t disp = 0;
            std::memcpy(&disp, fnWheelXchg + 2, 4);
            g_findWheelDeltaPtr = reinterpret_cast<volatile LONG*>(fnWheelXchg + 6 + disp);
        }
        g_findDialogCheckbox = reinterpret_cast<FindDialogCheckboxFn>(GetProcAddress(mod, "FindMapDialogCheckbox"));
        g_findDialogCheckboxAt = reinterpret_cast<FindDialogCheckboxAtFn>(GetProcAddress(mod, "FindMapDialogCheckboxAt"));
        if (g_log) g_log("MapKeyPanel: Find's Exact match checkbox %s", g_findDialogCheckbox ? "found" : "not in this Find (keys only)");
        g_findDllResolved = true;
        if (g_log) {
            g_log("MapKeyPanel: Resolved FindDialog from %ls (create=%p runFrame=%p hitTest=%p cursor=%p wheel=%p)",
                  modPath ? modPath : L"<module>", fnCreate, fnRunFrame, fnHitTest, fnGetCursor,
                  fnWheelXchg ? (fnWheelXchg + 6 + *reinterpret_cast<int32_t*>(fnWheelXchg + 2)) : nullptr);
        }
        return true;
    };

    std::vector<std::pair<HMODULE, std::wstring>> candidates; // in load order
    HMODULE hMods[1024]{};
    DWORD cbNeeded = 0;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        size_t count = std::min<size_t>(cbNeeded / sizeof(HMODULE), 1024);
        for (size_t i = 0; i < count; ++i) {
            wchar_t pathBuf[MAX_PATH]{};
            if (!GetModuleFileNameW(hMods[i], pathBuf, MAX_PATH)) continue;
            const wchar_t* baseName = pathBuf;
            for (const wchar_t* p = pathBuf; *p; ++p) {
                if (*p == L'\\' || *p == L'/') baseName = p + 1;
            }
            std::wstring lowerName = baseName;
            for (wchar_t& ch : lowerName) {
                if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
            bool isFindCandidate = (lowerName == L"find.dll" || lowerName == L"wsock32.dll" ||
                                    (lowerName.rfind(L"find.", 0) == 0 &&
                                     lowerName.compare(lowerName.size() - 4, 4, L".dll") == 0));
            if (isFindCandidate) candidates.emplace_back(hMods[i], pathBuf);
        }
    }
    if (candidates.empty()) {
        if (HMODULE m = GetModuleHandleW(L"find.dll")) candidates.emplace_back(m, L"find.dll");
    }
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (g_findDllResolved && it->first == g_findModule) return false;
        if (tryResolveModule(it->first, it->second.c_str())) {
            const bool changed = g_findModule != nullptr;
            g_findModule = it->first;
            return changed;
        }
    }
    return false;
}

uint8_t* CreateNativeFindDialogFallback(const char* title, const char* okLabel) {
    if (!g_dialogInit) return nullptr;
    auto* dlg = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x8d0));
    if (!dlg) return nullptr;

    for (size_t i = 0; i < 8; ++i) {
        *reinterpret_cast<size_t*>(dlg + 0x698 + i * 0x20 + 0x18) = 0x0f;
    }
    *reinterpret_cast<size_t*>(dlg + 0x870) = 0x0f;
    *reinterpret_cast<int32_t*>(dlg + 0x89c) = -1;
    *reinterpret_cast<int32_t*>(dlg + 0x8a0) = -1;

    *reinterpret_cast<UiText*>(dlg + 0x668) = MakeRuntimeUiText(title);
    *reinterpret_cast<UiText*>(dlg + 0x680) = MakeRuntimeUiText(okLabel);

    g_dialogInit(dlg);

    *reinterpret_cast<UiText*>(dlg + 0xa0) = *reinterpret_cast<UiText*>(dlg + 0x668);
    *reinterpret_cast<float*>(dlg + 0x164) = 800.0f;

    *reinterpret_cast<char**>(dlg + 0x600) = reinterpret_cast<char*>(dlg + 0x628);
    *reinterpret_cast<size_t*>(dlg + 0x608) = 0;
    *reinterpret_cast<size_t*>(dlg + 0x610) = 0x40;
    *reinterpret_cast<uint64_t*>(dlg + 0x618) = 0;
    *reinterpret_cast<uint64_t*>(dlg + 0x620) = 0;
    *reinterpret_cast<uint8_t**>(dlg + 0x18) = dlg + 0x600;

    uint8_t* okBtn = dlg + 0x400;
    std::memset(okBtn, 0, 0x100);
    *reinterpret_cast<uint32_t*>(okBtn + 0x04) = 0x10202u;
    *reinterpret_cast<uint16_t*>(okBtn + 0x08) = 0x0100u;
    *reinterpret_cast<uint32_t*>(okBtn + 0x10) = 3u;
    *reinterpret_cast<float*>(okBtn + 0x4c) = 10.0f;
    *reinterpret_cast<float*>(okBtn + 0x50) = 5.0f;
    *reinterpret_cast<float*>(okBtn + 0x54) = 1.0f;
    okBtn[0x58] = 1;
    *reinterpret_cast<UiText*>(okBtn + 0x78) = *reinterpret_cast<UiText*>(dlg + 0x680);

    uint8_t* cancelBtn = dlg + 0x500;
    std::memset(cancelBtn, 0, 0x100);
    *reinterpret_cast<uint32_t*>(cancelBtn + 0x04) = 0x10202u;
    *reinterpret_cast<uint16_t*>(cancelBtn + 0x08) = 0x0100u;
    *reinterpret_cast<uint32_t*>(cancelBtn + 0x10) = 3u;
    *reinterpret_cast<float*>(cancelBtn + 0x4c) = 10.0f;
    *reinterpret_cast<float*>(cancelBtn + 0x50) = 5.0f;
    *reinterpret_cast<float*>(cancelBtn + 0x54) = 1.0f;
    cancelBtn[0x58] = 1;
    *reinterpret_cast<UiText*>(cancelBtn + 0x78) = MakeRuntimeUiText(umm_lang::T(umm_lang::Str::Cancel));

    void** btnArr = *reinterpret_cast<void***>(dlg + 0x20);
    size_t btnCap = *reinterpret_cast<size_t*>(dlg + 0x30);
    if (btnArr && btnCap >= 2) {
        btnArr[0] = okBtn;
        btnArr[1] = cancelBtn;
        *reinterpret_cast<size_t*>(dlg + 0x28) = 2;
    }

    *reinterpret_cast<UiText**>(dlg + 0xd0) = reinterpret_cast<UiText*>(dlg + 0x798);
    *reinterpret_cast<size_t*>(dlg + 0xd8) = 0;
    *reinterpret_cast<size_t*>(dlg + 0xe0) = 8;

    return dlg;
}

void ResetNativeFindDialog(uint8_t* dlg) {
    if (!dlg) return;
    if (g_findResetDialog) {
        g_findResetDialog(dlg);
    } else {
        *reinterpret_cast<UiText**>(dlg + 0xd0) = reinterpret_cast<UiText*>(dlg + 0x798);
        *reinterpret_cast<size_t*>(dlg + 0xd8) = 0;
        *reinterpret_cast<size_t*>(dlg + 0xe0) = 8;
        *reinterpret_cast<char**>(dlg + 0x600) = reinterpret_cast<char*>(dlg + 0x628);
        *reinterpret_cast<size_t*>(dlg + 0x608) = 0;
        dlg[0x628] = 0;
        dlg[0x407] = 0;
        dlg[0x507] = 0;
    }
    *reinterpret_cast<int32_t*>(dlg + 0x890) = 0;
    *reinterpret_cast<int32_t*>(dlg + 0x894) = 0;
    *reinterpret_cast<int32_t*>(dlg + 0x898) = 0;
    *reinterpret_cast<int32_t*>(dlg + 0x89c) = -1;
    *reinterpret_cast<int32_t*>(dlg + 0x8a0) = -1;
    *reinterpret_cast<int32_t*>(dlg + 0x8a4) = 0;
}

int RunNativeFindDialogFrame(uint8_t* dlg) {
    if (!dlg) return 1;
    if (g_findRunDialogFrame) {
        return g_findRunDialogFrame(dlg);
    }
    if (!g_textDialog) return 1;
    int r = g_textDialog(dlg);
    if (r != 0) return 1;
    if (dlg[0x407] != 0) return 2;
    if (dlg[0x507] != 0) return 1;
    return 0;
}

const char* GetNativeFindQueryStr(uint8_t* dlg, size_t* outLen) {
    if (!dlg || !outLen) return nullptr;
    if (g_findGetQueryStr) {
        return g_findGetQueryStr(dlg, outLen);
    }
    size_t len = *reinterpret_cast<size_t*>(dlg + 0x608);
    if (len > 63) len = 63;
    *outLen = len;
    return *reinterpret_cast<const char**>(dlg + 0x600);
}

void SetNativeFindTextFieldValue(uint8_t* dlg, const std::string& val) {
    if (!dlg) return;
    if (g_findSetTextFieldValue) {
        g_findSetTextFieldValue(dlg, &val);
    } else {
        size_t len = std::min<size_t>(val.size(), 63);
        std::memcpy(dlg + 0x628, val.data(), len);
        dlg[0x628 + len] = 0;
        *reinterpret_cast<char**>(dlg + 0x600) = reinterpret_cast<char*>(dlg + 0x628);
        *reinterpret_cast<size_t*>(dlg + 0x608) = len;
    }
}

// The game's text editor sets UI owner + 0x2fe70 while a field is being typed in, and the game ignores Escape
// while it is set. Find's own close clears it only for the field it recorded; Tab in the field can leave it set.
// (The owner is *(G + 0xd0) itself, as in Find's ReleaseDialogInput.)
bool ClearTextInputFlag() {
    auto* g = static_cast<uint8_t*>(GetUiG());
    uint8_t* owner = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    if (!owner || !*reinterpret_cast<void**>(owner) || !owner[0x2fe70]) return false;
    owner[0x2fe70] = 0;
    return true;
}

void SetTextInputFlag() {
    auto* g = static_cast<uint8_t*>(GetUiG());
    uint8_t* owner = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    if (owner && *reinterpret_cast<void**>(owner)) owner[0x2fe70] = 1;
}

// True while one of the game's own text fields (a pin's name, ...) is being typed in.
bool GameTextInputActive() {
    auto* g = static_cast<uint8_t*>(GetUiG());
    uint8_t* owner = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    return owner && *reinterpret_cast<void**>(owner) && owner[0x2fe70];
}

void CloseNativeFindDialog() {
    if (g_findCloseDialog) g_findCloseDialog();
    if (ClearTextInputFlag() && g_log) g_log("Find on the map: the game's text input was still on after closing; turned it off");
    g_findDialogOpen = false;
    g_findClosingFrames = 15;
    g_waitReleaseAfterFindClose = true;
}

void SyncNativeFindDialogState() {
    if (!g_mapFindDialog) return;

    std::string queryTrim = g_findQuery;
    while (!queryTrim.empty() && queryTrim.front() == ' ') queryTrim.erase(queryTrim.begin());
    while (!queryTrim.empty() && queryTrim.back() == ' ') queryTrim.pop_back();

    auto* sugArray = reinterpret_cast<UiText*>(g_mapFindDialog + 0x798);
    *reinterpret_cast<UiText**>(g_mapFindDialog + 0xd0) = sugArray;
    *reinterpret_cast<size_t*>(g_mapFindDialog + 0xe0) = 8;

    // Wait until the user types something before filling suggestions!
    if (queryTrim.empty()) {
        *reinterpret_cast<size_t*>(g_mapFindDialog + 0xd8) = 0;
        *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x890) = 0;
        *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x894) = 0;
        *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x898) = 0;
        *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x89c) = -1;
        *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x8a0) = -1;
        *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x8a4) = 0;

        char filters[96]{};
        std::snprintf(filters, sizeof(filters), umm_lang::T(umm_lang::Str::FiltersFmt),
                      umm_lang::T(g_findRespectFilters ? umm_lang::Str::On : umm_lang::Str::Off));
        ShowFiltersKey(filters, sizeof(filters));
        std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf), "%s  |  %s",
                      umm_lang::T(umm_lang::Str::TypeToSearch), filters);
        *reinterpret_cast<UiText*>(g_mapFindDialog + 0x878) = MakeRuntimeUiText(g_hintLineBuf);
        return;
    }

    int totalRows = static_cast<int>(g_findDisplayRows.size());
    int visCount = std::min(kMaxFindVisibleRows, std::max(0, totalRows - g_findScrollOffset));

    for (int i = 0; i < visCount; ++i) {
        const auto& row = g_findDisplayRows[g_findScrollOffset + i];
        std::snprintf(g_suggestionBufs[i], sizeof(g_suggestionBufs[i]), "%s", row.displayLine.c_str());
        sugArray[i] = MakeRuntimeUiText(g_suggestionBufs[i]);
    }
    *reinterpret_cast<size_t*>(g_mapFindDialog + 0xd8) = static_cast<size_t>(visCount);

    int selVisIdx = (g_findSelectedIndex >= g_findScrollOffset && g_findSelectedIndex < g_findScrollOffset + visCount)
        ? (g_findSelectedIndex - g_findScrollOffset)
        : -1;
    int hovVisIdx = (g_findHoveredIndex >= g_findScrollOffset && g_findHoveredIndex < g_findScrollOffset + visCount)
        ? (g_findHoveredIndex - g_findScrollOffset)
        : -1;

    *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x89c) = selVisIdx;
    *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x8a0) = hovVisIdx;
    *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x8a4) = visCount;

    *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x890) = g_findScrollOffset;
    *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x894) = visCount;
    *reinterpret_cast<int32_t*>(g_mapFindDialog + 0x898) = totalRows;

    // "12 matches (3 hidden)  |  F2: Filters [ON]", in the current language
    char hidden[64]{};
    if (g_findRespectFilters && g_findFilteredOutCount > 0) {
        std::snprintf(hidden, sizeof(hidden), umm_lang::T(umm_lang::Str::HiddenFmt), g_findFilteredOutCount);
    }
    char filters[96]{};
    std::snprintf(filters, sizeof(filters), umm_lang::T(umm_lang::Str::FiltersFmt),
                  umm_lang::T(g_findRespectFilters ? umm_lang::Str::On : umm_lang::Str::Off));
    ShowFiltersKey(filters, sizeof(filters));
    // Exact match left nothing: how many it hides ("Exact match hides 32 results.")
    char exactHidden[128]{};
    if (g_findExactMatch && g_findMatchedPlaces.empty() && g_findExactHidden > 0) {
        if (umm_lang::Current() == umm_lang::Lang::English && g_findExactHidden == 1) {
            std::snprintf(exactHidden, sizeof(exactHidden), "  |  Exact match hides 1 result.");
        } else {
            std::snprintf(exactHidden, sizeof(exactHidden), "  |  ");
            std::snprintf(exactHidden + 5, sizeof(exactHidden) - 5, umm_lang::T(umm_lang::Str::ExactHiddenFmt), g_findExactHidden);
        }
    }
    std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf), "%s%s%s  |  %s",
                  umm_lang::FormatMatches(g_findMatchedPlaces.size()).c_str(), hidden, exactHidden, filters);
    *reinterpret_cast<UiText*>(g_mapFindDialog + 0x878) = MakeRuntimeUiText(g_hintLineBuf);
}

std::string ToLowerAscii(const std::string& s) {
    return umm_lang::ToLowerUtf8(s); // any script, not only A-Z, so non-English searches match
}

uint32_t NormalizeFilterTypeHash(uint32_t rawHash) {
    if (rawHash == 0x98b2d65du) return 0xa17072beu;
    if (rawHash == kLockedFastTravelTypeHash || rawHash == 0x813080bcu) return 0x1095684eu;
    if (rawHash == 0x6a56c5b3u) return 0xa447cba3u;
    if (rawHash == 0xafce0c5fu) return 0x8ba7c122u;
    if (rawHash == 0x454e24f2u) return 0xa7cadc08u;
    if (rawHash == 0x7b87ef66u || rawHash == 0xd11531d2u || rawHash == 0x111adadau) return 0xb5d01766u;
    if (rawHash == 0x9786a9f3u) return 0xdb17bf33u;
    return rawHash;
}

const char* GetTypeLabelForHash(uint32_t filterHash) {
    if (IsOreHash(filterHash)) return OreName(OreIndexOf(filterHash));
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (grp.rows[r].typeHash == filterHash) {
                return umm_lang::T(grp.rows[r].text);
            }
        }
    }
    for (size_t p = 0; p < 10; ++p) {
        if (kCustomPinHashes[p] == filterHash) {
            return umm_lang::T(umm_lang::Str::CustomPin);
        }
    }
    return umm_lang::T(umm_lang::Str::MapLocation);
}

bool GetHudPlayerWorldPos(uint8_t* client, float& outX, float& outZ);

void GetPlayerOrMapCenterWorldPos(uint8_t* mapState, float& outX, float& outZ) {
    outX = 5120.0f;
    outZ = 5120.0f;
    if (GetHudPlayerWorldPos(GetUiClient(), outX, outZ)) {
        return;
    }
    if (mapState) {
        if (uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8)) {
            float mapH = *reinterpret_cast<float*>(mapView + 0x3c);
            if (mapH <= 100.0f) mapH = 10240.0f;
            float cx = *reinterpret_cast<float*>(mapView + 0x20);
            float cy = *reinterpret_cast<float*>(mapView + 0x24);
            if (cx > 0.0f && cy > 0.0f) {
                outX = cx;
                outZ = mapH - cy;
            }
        }
    }
}

std::string CleanMarkerTitle(const char* rawUtf8, size_t rawLen, const char* fallbackLabel) {
    if (!rawUtf8 || rawLen == 0) return fallbackLabel ? fallbackLabel : umm_lang::T(umm_lang::Str::MapLocation);
    std::string s(rawUtf8, std::min<size_t>(rawLen, 160));
    // Strip trailing " is Ready" if present on Ancient Spire titles
    const std::string kReadySuffix = " is Ready";
    if (s.size() > kReadySuffix.size() &&
        s.compare(s.size() - kReadySuffix.size(), kReadySuffix.size(), kReadySuffix) == 0) {
        s.resize(s.size() - kReadySuffix.size());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s.empty() ? (fallbackLabel ? fallbackLabel : umm_lang::T(umm_lang::Str::MapLocation)) : s;
}

// The English row label for a filter type (the panel shows the translated one).
const char* GetEnglishTypeLabelForHash(uint32_t filterHash) {
    if (IsOreHash(filterHash)) return kOreLabels[OreIndexOf(filterHash)];
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (grp.rows[r].typeHash == filterHash) return grp.rows[r].label;
        }
    }
    return nullptr;
}

// How a name matches the typed text, as Find's own dialog does it (find_match.h, a copy of Find's): 0 exact, 1 the name
// starts with it, 2 a word of the name starts with it, 3 anywhere in it (or the type starts with it), 4 the type holds
// it; then text of two or more words in any order: 5 every word starts a word of the name, 6 every word is in the name,
// 7 every word is in the name or the type ("mana ring": the Rings of Mana); 10 + the words missing: only some of them
// (words under 3 letters never match on their own). -1 no match. Exact match keeps 0-4 only.
constexpr int kTierWholeText = 4, kTierAllWords = 7;
int ComputeNameMatchTier(const std::string& queryLower, const std::string& titleLower, const std::string& typeLower) {
    if (queryLower.empty()) return 0;
    const int r = match::Rank(titleLower, queryLower);
    if (r >= 0) return r;
    if (!typeLower.empty()) {
        if (typeLower == queryLower || typeLower.compare(0, queryLower.size(), queryLower) == 0) return 3;
        if (typeLower.find(queryLower) != std::string::npos) return 4;
    }
    return match::WordsRank(titleLower, typeLower, match::Words(queryLower));
}

void RebuildFindResults(uint8_t* client, uint8_t* mapState);

// The Map Key search's places: Find's own scan (every marker, chest, resource, lore note and boss, by its name; rows
// switched off too) for the text typed, once each time it changes. 3 letters at least, so a letter or two doesn't
// bring in every row. Find's results stay as they were.
void KeySearchPlaces(uint8_t* client, uint8_t* mapState) {
    static std::string s_for;
    static int s_lang = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (!client || (s_for == g_keySearch && s_lang == lang)) return;
    s_for = g_keySearch;
    s_lang = lang;
    g_keySearchPlaces.clear();
    ++g_keySearchPlacesVersion;
    std::string q = g_keySearch;
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.size() < 3) return;
    const std::string query = g_findQuery;
    const bool respect = g_findRespectFilters;
    const int selected = g_findSelectedIndex, hovered = g_findHoveredIndex, scroll = g_findScrollOffset;
    const int filteredOut = g_findFilteredOutCount;
    std::vector<FindPlaceEntry> places;
    std::vector<FindDisplayRow> rows;
    places.swap(g_findMatchedPlaces);
    rows.swap(g_findDisplayRows);
    g_findQuery = q;
    g_findRespectFilters = false;
    RebuildFindResults(client, mapState);
    const std::string queryLower = ToLowerAscii(q);
    std::vector<const FindPlaceEntry*> named;
    for (const FindPlaceEntry& e : g_findMatchedPlaces) {
        // By its own name only, not the type label Find matches too (the row names already count)
        const int t = ComputeNameMatchTier(queryLower, ToLowerAscii(e.title), std::string());
        if (t >= 0 && t <= kTierAllWords) named.push_back(&e); // (not only some of the words)
    }
    std::sort(named.begin(), named.end(), [](const FindPlaceEntry* a, const FindPlaceEntry* b) {
        return a->distMeters < b->distMeters;
    });
    for (const FindPlaceEntry* e : named) {
        bool seen = false;
        for (const auto& pl : g_keySearchPlaces) seen = seen || pl.first == e->filterTypeHash;
        if (!seen) g_keySearchPlaces.emplace_back(e->filterTypeHash, e->title);
    }
    g_findMatchedPlaces.swap(places);
    g_findDisplayRows.swap(rows);
    g_findQuery = query;
    g_findRespectFilters = respect;
    g_findSelectedIndex = selected;
    g_findHoveredIndex = hovered;
    g_findScrollOffset = scroll;
    g_findFilteredOutCount = filteredOut;
}

void RebuildFindResults(uint8_t* client, uint8_t* mapState) {
    g_findMatchedPlaces.clear();
    g_findDisplayRows.clear();
    g_findFilteredOutCount = 0;
    g_findExactHidden = 0;
    if (!client) return;

    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);

    float refX = 5120.0f, refZ = 5120.0f;
    GetPlayerOrMapCenterWorldPos(mapState, refX, refZ);

    std::string queryTrim = g_findQuery;
    while (!queryTrim.empty() && queryTrim.front() == ' ') queryTrim.erase(queryTrim.begin());
    while (!queryTrim.empty() && queryTrim.back() == ' ') queryTrim.pop_back();
    if (queryTrim.empty()) {
        g_findSelectedIndex = -1;
        g_findHoveredIndex = -1;
        g_findScrollOffset = 0;
        return;
    }
    std::string queryLower = ToLowerAscii(queryTrim);

    auto processCandidate = [&](float wx, float wz, uint32_t iconHash, uint32_t filterHash,
                                bool isFilterOn, bool isLocked, const std::string& title, const char* typeLabel,
                                bool listAlways = false) {
        std::string titleLower = ToLowerAscii(title);
        // The type in the current language, plus its English name so English searches work in any language
        std::string typeLower = ToLowerAscii(typeLabel ? typeLabel : "");
        if (const char* english = GetEnglishTypeLabelForHash(filterHash)) {
            if (umm_lang::Current() != umm_lang::Lang::English) typeLower += " | " + ToLowerAscii(english);
        }
        int tier = ComputeNameMatchTier(queryLower, titleLower, typeLower);
        if (tier < 0) return;
        if (g_findExactMatch && tier > kTierWholeText) { // Exact match: only names holding the whole text
            ++g_findExactHidden;
            return;
        }

        if (!isFilterOn && g_findRespectFilters && !listAlways) {
            ++g_findFilteredOutCount;
            return;
        }

        float dx = wx - refX;
        float dz = wz - refZ;
        float dist = std::sqrt(dx * dx + dz * dz);

        FindPlaceEntry entry{};
        entry.wx = wx;
        entry.wz = wz;
        entry.distMeters = dist;
        entry.iconTypeHash = iconHash;
        entry.filterTypeHash = filterHash;
        entry.isFilterOn = isFilterOn;
        entry.isLocked = isLocked;
        entry.matchTier = tier;
        entry.title = title;
        entry.typeLabel = typeLabel ? typeLabel : umm_lang::T(umm_lang::Str::MapLocation);
        g_findMatchedPlaces.push_back(std::move(entry));
    };

    // 1. Scan arr0, arr1, arr2, arr3 for all non-chest markers
    constexpr size_t kArrayOffsets[4] = { 0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78 };
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;

        for (uint64_t i = 0; i < count; ++i) {
            uint8_t* m = base + i * 0xA0;
            uint32_t uid = *reinterpret_cast<uint32_t*>(m + 0x00);
            if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) continue;

            uint32_t origHash = GetMarkerOrigTypeHash(m);
            if (origHash == 0u) continue;
            // Skip duplicate grayed-out spire twin (0x98b2d65d) because 0xa17072be already holds the named spire
            if (origHash == 0x98b2d65du) continue;

            uint32_t savedId08 = IsSlotSaved(m)
                                     ? *reinterpret_cast<uint32_t*>(m + 0x4c)
                                     : 0u;
            if (savedId08 == 0u) savedId08 = *reinterpret_cast<uint32_t*>(m + 0x08);

            uint32_t filterHash = (origHash == 0xafce0c5fu)
                                      ? ClassifyQuestOrRumorMarker(savedId08, m[0x35])
                                      : NormalizeFilterTypeHash(origHash);
            if (filterHash == 0u || GameNeverShowsPlace(savedId08)) continue;
            if (origHash == 0x454e24f2u && !IsActiveQuestArea(savedId08, m[0x35])) continue; // its quest is done
            bool isFilterOn = IsFilterEnabled(filterEntries, filterCount, filterHash);
            if (!isFilterOn && origHash != 0xafce0c5fu) {
                uint32_t subpinQuestHash = GetQuestFilterHashForMarkerId(savedId08, true);
                if (subpinQuestHash != 0u && IsFilterEnabled(filterEntries, filterCount, subpinQuestHash)) {
                    isFilterOn = true;
                }
            }
            if (g_pinRevealMode == PinRevealMode::ExploredOnly && !IsMarkerShownForExploredPins(client, m, origHash)) {
                isFilterOn = false;
            }
            const char* typeLabel = GetTypeLabelForHash(filterHash);

            int32_t mx = *reinterpret_cast<int32_t*>(m + 0x14);
            int32_t mz = *reinterpret_cast<int32_t*>(m + 0x24);
            float wx = static_cast<float>(mx);
            float wz = static_cast<float>(mz);
            if (wx <= 1.0f && wz <= 1.0f) continue;

            uint8_t b48 = m[0x48];
            uint64_t lastChg = *reinterpret_cast<uint64_t*>(m + 0x40);
            bool isLocked = false;
            if (b48 != 0 && (filterHash == 0xa17072beu || filterHash == 0x1095684eu)) {
                isLocked = (lastChg == 0);
            }

            std::string title;
            if (g_getMarkerTitleText) {
                uint32_t prevHash = *reinterpret_cast<uint32_t*>(m + 0x30);
                uint32_t prevId08 = *reinterpret_cast<uint32_t*>(m + 0x08);
                if (prevHash == 0u) *reinterpret_cast<uint32_t*>(m + 0x30) = (origHash == 0xafce0c5fu) ? 0xafce0c5fu : filterHash;
                if (prevId08 == 0u && savedId08 != 0u) *reinterpret_cast<uint32_t*>(m + 0x08) = savedId08;
                UiText outText{};
                g_getMarkerTitleText(&outText, m);
                if (prevHash == 0u) *reinterpret_cast<uint32_t*>(m + 0x30) = prevHash;
                if (prevId08 == 0u && savedId08 != 0u) *reinterpret_cast<uint32_t*>(m + 0x08) = prevId08;
                title = CleanMarkerTitle(outText.utf8, outText.len, typeLabel);
            } else {
                title = typeLabel;
            }

            processCandidate(wx, wz, filterHash, filterHash, isFilterOn, isLocked, title, typeLabel);
        }
    }

    // 2. Scan Gold & Silver Chests directly from kChestSpawns so filtered-out counts work even when disabled
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        if (IsChestCurrentlyCleared(c)) continue;
        const ChestSpawn& sp = kChestSpawns[c];
        uint32_t chestHash = ChestTypeHash(c);
        bool isFilterOn = ChestRowEnabled(c);
        if (g_pinRevealMode == PinRevealMode::ExploredOnly && !IsPositionExploredByCharacter(client, sp.px1, sp.pz1)) {
            isFilterOn = false;
        }
        const char* typeLabel = IsGoldChest(c)        ? umm_lang::T(umm_lang::Str::GoldChest)
                                : !IsWoodChest(c)       ? umm_lang::T(umm_lang::Str::SilverChest)
                                                        : WoodChestName();
        const UiText& subTitle = GetChestSubtypeTitle(sp.subtype);
        float wx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        float wz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        processCandidate(wx, wz, chestHash, chestHash, isFilterOn, false,
                          std::string(subTitle.utf8, subTitle.len), typeLabel);
    }

    // 2b. Ore deposits (Mining), from the voxel world data
    for (size_t d = 0; d < kOreDepositCount; ++d) {
        const OreDeposit& dep = kOreDeposits[d];
        bool isFilterOn = g_oreEnabled[dep.ore];
        if (g_pinRevealMode == PinRevealMode::ExploredOnly && !IsPositionExploredByCharacter(client, dep.x, dep.z)) {
            isFilterOn = false;
        }
        // Lore notes and bosses by their own names ("Armored Fell Thunderbrute"), under their row's name
        const char* name = IsKnowledgeDeposit(d) ? KnowledgeSpotName(d) : OreName(dep.ore);
        processCandidate(static_cast<float>(dep.x), static_cast<float>(dep.z), OreHash(dep.ore), OreHash(dep.ore),
                         isFilterOn, false, name, OreName(dep.ore));
    }

    // 2c. Items the game always puts in one container (Ghost Glider, recipes, blocks, tools...) by their own names, at
    // that container: a gold, silver or wooden chest by its kind, any other as a container. Listed whatever the filters
    // (an item searched for by name); picking one turns on the row that shows its container.
    for (size_t i = 0; i < kStaticItemCount; ++i) {
        const StaticItem& it = kStaticItems[i];
        const char* name = StaticItemName(i);
        if (!*name) continue;
        float wx = static_cast<float>(it.x), wz = static_cast<float>(it.z);
        uint32_t icon = OreHash(static_cast<size_t>(Ore::UniqueGear));
        bool rowOn = g_oreEnabled[static_cast<size_t>(Ore::UniqueGear)];
        const char* where = umm_lang::T(umm_lang::Str::Container);
        if (it.chest < kTotalChestCount) {
            const ChestSpawn& sp = kChestSpawns[it.chest];
            wx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
            wz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
            icon = ChestTypeHash(it.chest);
            rowOn = ChestRowEnabled(it.chest);
            where = IsGoldChest(it.chest) ? umm_lang::T(umm_lang::Str::GoldChest)
                    : !IsWoodChest(it.chest) ? umm_lang::T(umm_lang::Str::SilverChest)
                                             : WoodChestName();
        }
        processCandidate(wx, wz, icon, icon, rowOn, false, name, where, true);
    }

    // 3. Scan Other Players in multiplayer from client + 0x4642c0
    {
        uint8_t* playerArray = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
        uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
        uint32_t localPlayerId = *reinterpret_cast<uint32_t*>(client + 0x18);
        if (playerArray && playerCount > 0 && playerCount <= 64) {
            for (uint64_t i = 0; i < playerCount; ++i) {
                uint8_t* p = playerArray + i * 0x240;
                uint32_t pid = *reinterpret_cast<uint32_t*>(p + 0x00);
                if (pid == 0 || pid == localPlayerId || p[0x207] != 0) continue;

                float ox = *reinterpret_cast<float*>(p + 0x10);
                float oz = *reinterpret_cast<float*>(p + 0x18);
                if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f) && p[0x68] != 0) {
                    int32_t rx = *reinterpret_cast<int32_t*>(p + 0x54);
                    int32_t rz = *reinterpret_cast<int32_t*>(p + 0x64);
                    ox = static_cast<float>(rx) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x50)) / 65536.0f;
                    oz = static_cast<float>(rz) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x60)) / 65536.0f;
                }
                if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f)) continue;

                const char* namePtr = *reinterpret_cast<const char**>(p + 0xc0);
                size_t nameLen = *reinterpret_cast<size_t*>(p + 0xc8);
                if (!namePtr || nameLen == 0 || nameLen > 64) {
                    namePtr = *reinterpret_cast<const char**>(p + 0xd8);
                    nameLen = *reinterpret_cast<size_t*>(p + 0xe0);
                }
                std::string playerName = (namePtr && nameLen > 0 && nameLen <= 48)
                    ? std::string(namePtr, nameLen)
                    : std::string(umm_lang::T(umm_lang::Str::OtherPlayer));
                processCandidate(ox, oz, kOtherPlayerTypeHash, kOtherPlayerTypeHash,
                                  g_otherPlayersEnabled, false, playerName,
                                  umm_lang::T(umm_lang::Str::OtherPlayers));
            }
        }
    }

    // Sort: 1. Name match tier ascending, 2. Distance ascending, 3. Title alphabetical
    std::sort(g_findMatchedPlaces.begin(), g_findMatchedPlaces.end(),
              [](const FindPlaceEntry& a, const FindPlaceEntry& b) {
                  if (a.matchTier != b.matchTier) return a.matchTier < b.matchTier;
                  if (std::fabs(a.distMeters - b.distMeters) > 1.0f) return a.distMeters < b.distMeters;
                  return a.title < b.title;
              });

    // Build display rows for native FindDialog (strictly single-line so row highlight & hit-testing never drift!):
    constexpr size_t kMaxSingleLineChars = 46;

    // If multiple places match, Row 0 is "All Matching Places (N) - Zoom to fit"
    if (g_findMatchedPlaces.size() >= 2) {
        FindDisplayRow allRow{};
        allRow.isGroupRow = true;
        allRow.isToggleFilterRow = false;
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), umm_lang::T(umm_lang::Str::AllMatchingFmt), g_findMatchedPlaces.size());
        allRow.displayLine = buf;
        allRow.matchIndices.resize(g_findMatchedPlaces.size());
        for (size_t i = 0; i < g_findMatchedPlaces.size(); ++i) allRow.matchIndices[i] = i;
        g_findDisplayRows.push_back(std::move(allRow));

        // If multiple places share the top match's exact title (and it's a subset of all matches), offer a group row for that name
        const std::string& topTitle = g_findMatchedPlaces[0].title;
        std::vector<size_t> sameTitleIndices;
        for (size_t i = 0; i < g_findMatchedPlaces.size(); ++i) {
            if (g_findMatchedPlaces[i].title == topTitle) {
                sameTitleIndices.push_back(i);
            }
        }
        if (sameTitleIndices.size() >= 2 && sameTitleIndices.size() < g_findMatchedPlaces.size()) {
            FindDisplayRow grpRow{};
            grpRow.isGroupRow = true;
            grpRow.isToggleFilterRow = false;
            const std::string shortTitle = umm_lang::TruncateColumns(topTitle, 24);
            std::snprintf(buf, sizeof(buf), umm_lang::T(umm_lang::Str::AllNamedFmt),
                          shortTitle.c_str(), sameTitleIndices.size());
            grpRow.displayLine = buf;
            grpRow.matchIndices = std::move(sameTitleIndices);
            g_findDisplayRows.push_back(std::move(grpRow));
        }
    }

    for (size_t i = 0; i < g_findMatchedPlaces.size(); ++i) {
        const auto& p = g_findMatchedPlaces[i];
        FindDisplayRow row{};
        row.isGroupRow = false;
        row.isToggleFilterRow = false;
        const char* lockTag = p.isLocked ? umm_lang::T(umm_lang::Str::LockedTag) : "";
        const char* filtTag = !p.isFilterOn ? umm_lang::T(umm_lang::Str::HiddenTag) : "";

        char distBuf[32]{};
        std::snprintf(distBuf, sizeof(distBuf), " (%.0fm)", p.distMeters);
        std::string distStr = distBuf;

        std::string body = p.title + lockTag + filtTag;
        if (p.title != p.typeLabel && !p.typeLabel.empty()) {
            body += " - " + p.typeLabel;
        }
        // Counted in columns, not bytes (a CJK character is two columns and up to four bytes)
        if (umm_lang::Columns(body) + distStr.size() > kMaxSingleLineChars) {
            const size_t maxBody = (kMaxSingleLineChars > distStr.size() + 2)
                ? (kMaxSingleLineChars - distStr.size())
                : 14;
            body = umm_lang::TruncateColumns(body, maxBody);
        }
        row.displayLine = body + distStr;
        row.matchIndices.push_back(i);
        g_findDisplayRows.push_back(std::move(row));
    }

    if (g_findDisplayRows.empty() && g_findRespectFilters && g_findFilteredOutCount > 0) {
        FindDisplayRow toggleRow{};
        toggleRow.isGroupRow = false;
        toggleRow.isToggleFilterRow = true;
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), umm_lang::T(umm_lang::Str::ZeroVisibleFmt), g_findFilteredOutCount);
        toggleRow.displayLine = buf;
        g_findDisplayRows.push_back(std::move(toggleRow));
    }

    if (g_findSelectedIndex >= static_cast<int>(g_findDisplayRows.size())) {
        g_findSelectedIndex = -1;
    }
    int maxScroll = std::max(0, static_cast<int>(g_findDisplayRows.size()) - kMaxFindVisibleRows);
    if (g_findScrollOffset > maxScroll) {
        g_findScrollOffset = maxScroll;
    }
}

// Find showed places a filter hides (F2): their Map Key rows are brought into view (panel and category expanded, list
// scrolled) and flashed for a few seconds, so the player sees which filter to switch on.
std::unordered_set<uint32_t> g_flashRows;   // row type hashes
uint64_t g_flashRowsUntil = 0;
uint32_t g_scrollToRow = 0;                 // scrolled to on the next Map Key frame

void RevealFilteredFindRows(const std::vector<size_t>& matchIndices) {
    g_flashRows.clear();
    for (size_t idx : matchIndices) {
        if (idx >= g_findMatchedPlaces.size() || g_findMatchedPlaces[idx].isFilterOn) continue;
        const uint32_t row = RowHashOf(g_findMatchedPlaces[idx].filterTypeHash);
        for (size_t c = 0; c < kCategoryGroupCount; ++c) {
            const auto& grp = kCategoryGroups[c];
            bool here = false;
            for (size_t r = 0; r < grp.rowCount && !here; ++r) here = grp.rows[r].typeHash == row;
            if (grp.category == 4) {
                for (uint32_t pin : kCustomPinHashes) here = here || pin == row;
            }
            if (!here) continue;
            g_categoryCollapsed[c] = false;
            g_panelCollapsed = false;
            if (g_flashRows.empty()) g_scrollToRow = row;
            g_flashRows.insert(row);
        }
    }
    if (!g_flashRows.empty()) g_flashRowsUntil = GetTickCount64() + 14000ULL; // as long as the map's pulses
}

void FocusMapOnWorldPositions(uint8_t* client, uint8_t* mapState, const std::vector<size_t>& matchIndices) {
    if (!mapState || matchIndices.empty()) return;
    RevealFilteredFindRows(matchIndices);

    g_pulsingPlaces.clear();
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    std::unordered_set<uint32_t> switchedOn; // once a row: its places all carry the state from before the pick

    for (size_t idx : matchIndices) {
        if (idx >= g_findMatchedPlaces.size()) continue;
        const auto& place = g_findMatchedPlaces[idx];
        // If user searched with Respect Filters OFF and picked a filtered-out place, enable its filter so it shows up.
        // Each switch saves the config: once per place froze the game on 549 hidden Saffron patches.
        if (!place.isFilterOn && client && switchedOn.insert(place.filterTypeHash).second) {
            SetSingleFilterState(client, mapState, place.filterTypeHash, true);
        }
        g_pulsingPlaces.push_back(PulsingPlace{place.wx, place.wz});
        minX = std::min(minX, place.wx);
        maxX = std::max(maxX, place.wx);
        minZ = std::min(minZ, place.wz);
        maxZ = std::max(maxZ, place.wz);
    }
    if (g_pulsingPlaces.empty()) return;
    g_pulseStartTick = GetTickCount64();

    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;

    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;

    float centerX = (minX + maxX) * 0.5f;
    float centerZ = (minZ + maxZ) * 0.5f;
    float targetCanvasPos2D[2] = { centerX, mapWorldH - centerZ };

    uint8_t numSteps = *reinterpret_cast<uint8_t*>(mapView + 0x54);
    float baseZoomFactor = *reinterpret_cast<float*>(mapView + 0x58);
    if (baseZoomFactor <= 0.0001f) baseZoomFactor = 1.0f;
    size_t* zoomStepPtr = *reinterpret_cast<size_t**>(mapView + 0x258);

    float targetZoom = *reinterpret_cast<float*>(mapView + 0x10);
    if (numSteps > 0 && numSteps <= 16) {
        size_t minStep = 0;
        size_t maxStep = 0;
        float minStepZoom = 1e9f;
        float maxStepZoom = -1e9f;
        for (uint8_t s = 0; s < numSteps; ++s) {
            float sz = *reinterpret_cast<float*>(mapView + 0x40 + s * 4) * baseZoomFactor;
            if (sz < minStepZoom) {
                minStepZoom = sz;
                minStep = s;
            }
            if (sz > maxStepZoom) {
                maxStepZoom = sz;
                maxStep = s;
            }
        }

        size_t targetStep = maxStep;
        targetZoom = maxStepZoom;

        if (g_pulsingPlaces.size() > 1) {
            float viewW = *reinterpret_cast<float*>(mapView + 0x1e8);
            float viewH = *reinterpret_cast<float*>(mapView + 0x1ec);
            if (viewW <= 100.0f) viewW = 1920.0f;
            if (viewH <= 100.0f) viewH = 1080.0f;
            float usableW = std::max(400.0f, viewW - 520.0f) * 0.72f;
            float usableH = std::max(300.0f, viewH - 220.0f) * 0.72f;
            float spanX = std::max(maxX - minX, 250.0f);
            float spanZ = std::max(maxZ - minZ, 250.0f);
            float maxZoomAllowed = std::min(usableW / spanX, usableH / spanZ);

            targetStep = minStep;
            targetZoom = minStepZoom;
            for (uint8_t s = 0; s < numSteps; ++s) {
                float sz = *reinterpret_cast<float*>(mapView + 0x40 + s * 4) * baseZoomFactor;
                if (sz <= maxZoomAllowed && sz >= targetZoom) {
                    targetZoom = sz;
                    targetStep = s;
                }
            }
        }

        if (zoomStepPtr) {
            *zoomStepPtr = targetStep;
        }
        *reinterpret_cast<uint8_t*>(mapView + 0x250) = 1;
        *reinterpret_cast<uint8_t*>(mapView + 0x260) = 0;
        *reinterpret_cast<float*>(mapView + 0x268) = 0.0f;
        *reinterpret_cast<float*>(mapView + 0x10) = targetZoom;
        *reinterpret_cast<float*>(mapView + 0x238) = targetZoom;
    }

    *reinterpret_cast<uint64_t*>(mapView + 0x18c) = 0;

    if (g_centerMapOnPos) {
        g_centerMapOnPos(mapView, targetCanvasPos2D, 1);
    }

    // Ensure immediate frame-accurate zoom & center state even when mapState[3] is 0 while dialog closes
    *reinterpret_cast<float*>(mapView + 0x10) = targetZoom;
    *reinterpret_cast<float*>(mapView + 0x238) = targetZoom;
    *reinterpret_cast<float*>(mapView + 0x20) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x24) = targetCanvasPos2D[1];
    // NOTE: mapView + 0x28/+0x2c is the right-click waypoint target pos (0x140c269e1), NOT camera center!
    *reinterpret_cast<uint64_t*>(mapView + 0x28) = 0;
    *reinterpret_cast<float*>(mapView + 0x30) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x34) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x1a8) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1ac) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x1b0) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1b4) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x198) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x19c) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x1f8) = targetZoom * targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1fc) = targetZoom * targetCanvasPos2D[1];
    uint64_t panTs = *reinterpret_cast<uint64_t*>(mapView + 0x1a0);
    *reinterpret_cast<uint64_t*>(mapView + 0x08) = (panTs != 0) ? panTs : *reinterpret_cast<uint64_t*>(mapView + 0x00);

    // Clear any latched click state on mapView/mapState so releasing the mouse or pressing Ctrl cannot open the Waypoint menu
    *reinterpret_cast<uint8_t*>(mapState + 0xf8) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x542e8) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x48) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x50) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x58) = 0;
    mapView[0x64] = 0;
    mapView[0x65] = 0;
    mapView[0x66] = 0;
    mapView[0x67] = 0;
    mapView[0x68] = 0;
    *reinterpret_cast<uint64_t*>(mapView + 0x158) = 0;
    *reinterpret_cast<uint64_t*>(mapView + 0x160) = 0;

    if (auto* g = static_cast<uint8_t*>(GetUiG())) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                d0[0x140e0] = 0;
                *reinterpret_cast<float*>(d0 + 0x31f38) = targetCanvasPos2D[0];
                *reinterpret_cast<float*>(d0 + 0x31f3c) = targetCanvasPos2D[1];
            }
        }
    }
}

// True from load: the per-frame screen check clears it whenever the map is not up, so a map already open when the mod
// (re)loads is not taken for a new open (which would centre it on the player)
// Alt + click a marker: turns its Map Key row off, so a row buried in a collapsed section needn't be found
struct HideRowTarget {
    uint32_t hashes[3]{}; // the row's type and the types it switches with it
};

const char* RowText(const MarkerRowSpec& row);
bool MarkerHiddenOnMap(const uint8_t* m); // regions.inl

// A quest / rumor pin (type 0xafce0c5f, search radius at +0x28) whose circle holds (wx, wz) while no other shown marker is
// within reach of it: its spot (the smallest circle if several). False if none.
bool RumorCircleAtInner(uint8_t* client, float wx, float wz, float reach, int32_t& outX, int32_t& outZ) {
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    float bestR = 1e9f;
    bool nearOther = false;
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            const uint32_t type = *reinterpret_cast<const uint32_t*>(m + 0x30);
            if (type == 0u || MarkerHiddenOnMap(m)) continue;
            const float dx = *reinterpret_cast<const int32_t*>(m + 0x14) - wx, dz = *reinterpret_cast<const int32_t*>(m + 0x24) - wz;
            const float d2 = dx * dx + dz * dz;
            if (type != 0xafce0c5fu) {
                if (d2 < reach * reach) nearOther = true;
                continue;
            }
            const float r = std::max(*reinterpret_cast<const float*>(m + 0x28), *reinterpret_cast<const float*>(m + 0x2c));
            if (!(r > 0.5f && r < 5000.0f) || d2 > r * r || r >= bestR) continue;
            bestR = r;
            outX = *reinterpret_cast<const int32_t*>(m + 0x14), outZ = *reinterpret_cast<const int32_t*>(m + 0x24);
        }
    }
    return bestR < 1e9f && !nearOther;
}
bool RumorCircleAt(uint8_t* client, float wx, float wz, float reach, int32_t& outX, int32_t& outZ) {
    __try {
        return RumorCircleAtInner(client, wx, wz, reach, outX, outZ);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The Map Key row of the marker clicked: the shown marker nearest the spot, within maxDist metres. The mod's own icons
// (lore notes, ores, ...) aren't markers of the game's: knownRow is the one under the cursor.
bool HideRowTargetAt(uint8_t* client, int32_t wx, int32_t wz, float maxDist, uint32_t knownRow, HideRowTarget& out,
                     bool gameFirst = false) {
    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    // The game's markers are drawn over the mod's icons, so one near the spot wins over knownRow (small ore or chest
    // icons sit under an Ancient Spire: Alt + click on the spire went to their row, often already off)
    // (gameFirst: Alt + click; Ctrl + click marks the chest icon under the cursor)
    uint32_t h = gameFirst ? 0u : knownRow;
    if (!h) {
    float best = maxDist * maxDist;
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            const uint32_t origHash = GetMarkerOrigTypeHash(m);
            // Only what the map draws: the type at +0x30 is 0 for a marker its row or its reveal rule took off the map
            // (an Ancient Spire's second marker, once you have the spire)
            if (origHash == 0u || *reinterpret_cast<const uint32_t*>(m + 0x30) == 0u || MarkerHiddenOnMap(m)) continue;
            const float dx = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14) - wx);
            const float dz = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24) - wz);
            const float d2 = dx * dx + dz * dz;
            if (d2 > best) continue;
            uint32_t savedId08 = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
            if (savedId08 == 0u) savedId08 = *reinterpret_cast<const uint32_t*>(m + 0x08);
            uint32_t fh = origHash == 0xafce0c5fu ? ClassifyQuestOrRumorMarker(savedId08, m[0x35])
                                                  : NormalizeFilterTypeHash(origHash);
            // A place an active quest or a lore rumor points to shows as a "?" with that row, though its own is off
            // (a cave passage, an enemy encampment): that row is the one to hide, as the map draws it
            if (fh != 0u && !IsFilterEnabled(filterEntries, filterCount, fh) && origHash != 0xafce0c5fu) {
                uint32_t questRow = GetQuestFilterHashForMarkerId(savedId08, true);
                if ((questRow == 0u || !IsFilterEnabled(filterEntries, filterCount, questRow)) &&
                    *reinterpret_cast<const uint32_t*>(m + 0x30) == 0xafce0c5fu) {
                    questRow = ClassifyQuestOrRumorMarker(savedId08, m[0x35]);
                }
                if (questRow != 0u && IsFilterEnabled(filterEntries, filterCount, questRow)) fh = questRow;
            }
            if (fh == 0u || !IsFilterEnabled(filterEntries, filterCount, fh)) continue;
            best = d2;
            h = fh;
        }
    }
    // Other players: the client's player list (client + 0x4642c0, 0x240 apart, live position at +0x10)
    uint8_t* players = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
    const uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
    const uint32_t localId = *reinterpret_cast<uint32_t*>(client + 0x18);
    if (g_otherPlayersEnabled && players && playerCount <= 64) {
        for (uint64_t i = 0; i < playerCount; ++i) {
            const uint8_t* p = players + i * 0x240;
            const uint32_t pid = *reinterpret_cast<const uint32_t*>(p);
            if (pid == 0 || pid == localId || halls::MapHides(*reinterpret_cast<const float*>(p + 0x10),
                                                              *reinterpret_cast<const float*>(p + 0x18))) continue;
            const float dx = *reinterpret_cast<const float*>(p + 0x10) - static_cast<float>(wx);
            const float dz = *reinterpret_cast<const float*>(p + 0x18) - static_cast<float>(wz);
            if (dx * dx + dz * dz > best) continue;
            best = dx * dx + dz * dz;
            h = kOtherPlayerTypeHash;
        }
    }
    }
    if (h == 0u) h = knownRow;
    if (h == 0u) return false;
    const char* name = nullptr;
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount && !name; ++r) {
            const MarkerRowSpec& row = grp.rows[r];
            if (row.typeHash != h && row.pairedHash != h && row.pairedHash2 != h) continue;
            out.hashes[0] = row.typeHash;
            out.hashes[1] = row.pairedHash;
            out.hashes[2] = row.pairedHash2;
            name = RowText(row);
        }
    }
    if (!name && std::find(std::begin(kCustomPinHashes), std::end(kCustomPinHashes), h) != std::end(kCustomPinHashes)) {
        out.hashes[0] = h;
        out.hashes[1] = out.hashes[2] = 0u;
        name = umm_lang::T(umm_lang::Str::CustomPin);
    }
    return name != nullptr;
}

// Turns the clicked marker's row off, with the message
void HideClickedRow(uint8_t* client, uint8_t* mapState, const HideRowTarget& target, const char* how) {
    const FilterSnapshot before = TakeFilterSnapshot(client);
    for (uint32_t h : target.hashes) {
        if (h != 0u) SetSingleFilterState(client, mapState, h, false);
    }
    AnnounceFilterChange(before, TakeFilterSnapshot(client));
    if (g_log) g_log("Map: hid the row %08x (%s)", target.hashes[0], how);
}

void AnnounceMarkDone(const char* name, bool on);

// F on a Journal entry opens the map centred on it: when its Map Key row is off, the row turns on (with the notice),
// or the map showed an empty spot. The game's marker nearest that centre within 12 m, a quest / rumor pin first
// (filtered off ones too: their type at +0x30 is 0 then). False if nothing there or its row is already on.
bool ShowJournalTargetRowInner(uint8_t* client, uint8_t* mapState, float wx, float wz) {
    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    float best = 12.0f * 12.0f;
    uint32_t h = 0u;
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            const uint32_t origHash = GetMarkerOrigTypeHash(m);
            if (origHash == 0u || MarkerHiddenOnMap(m)) continue;
            const float dx = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)) - wx;
            const float dz = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24)) - wz;
            uint32_t savedId08 = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
            if (savedId08 == 0u) savedId08 = *reinterpret_cast<const uint32_t*>(m + 0x08);
            uint32_t fh = origHash == 0xafce0c5fu ? ClassifyQuestOrRumorMarker(savedId08, m[0x35])
                                                  : NormalizeFilterTypeHash(origHash);
            // A place a quest or an unread lore rumor points to is drawn as that "!" / "?" (Follow the Melody's Cave
            // Passage): that row, not the place's own (the map rule in the minimap's and World Map's marker pass)
            bool questish = origHash == 0xafce0c5fu;
            if (!questish && origHash != 0x454e24f2u && savedId08 != 0u) {
                if (const uint32_t q = GetQuestFilterHashForMarkerId(savedId08, true)) fh = q, questish = true;
                else if (g_isLoreRumorMarkerId && g_isLoreRumorMarkerId(savedId08) && LoreRumorFound(client, savedId08) != 1)
                    fh = 0x8ba7c122u, questish = true;
            }
            // (a quest's or rumor's counts as nearer: the Journal is about them)
            const float d2 = (dx * dx + dz * dz) * (questish ? 0.25f : 1.0f);
            if (d2 > best) continue;
#ifdef UMM_DEV_BRIDGE // (dev: each marker weighed, and a quest pin's objectives: world / your state)
            if (g_log) g_log("Map:   Journal target candidate orig %08x id %08x row %08x %.1f m vis %02x state %u", origHash, savedId08, fh,
                             std::sqrt(dx * dx + dz * dz), m[0x35], m[0x34]);
            if (origHash == 0xafce0c5fu && g_getQuestByMarkerId && g_log) {
                if (const auto* quest = static_cast<const uint8_t*>(g_getQuestByMarkerId(savedId08))) {
                    const uint64_t on = *reinterpret_cast<const uint64_t*>(quest + 0x30);
                    const uint8_t* objs = *reinterpret_cast<uint8_t* const*>(quest + 0x28);
                    std::string st;
                    for (uint64_t k = 0; objs && k < on && k < 16; ++k) {
                        char one[24];
                        std::snprintf(one, sizeof(one), " %d/%d", *reinterpret_cast<const int32_t*>(objs + k * 0x50 + 0x18),
                                      *reinterpret_cast<const int32_t*>(objs + k * 0x50 + 0x1c));
                        st += one;
                    }
                    std::string raw;
                    for (int k = 0; k < 0x28; k += 4) {
                        char one[12];
                        std::snprintf(one, sizeof(one), " %08x", *reinterpret_cast<const uint32_t*>(quest + k));
                        raw += one;
                    }
                    g_log("Map:     quest %p kind %llu objectives %llu:%s head%s", quest,
                          static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(quest + 0x18)),
                          static_cast<unsigned long long>(on), st.c_str(), raw.c_str());
                }
            }
#endif
            if (fh == 0u || fh == kCompletedQuestTypeHash) continue;
            best = d2;
            h = fh;
        }
    }
    if (g_log) g_log("Map: Journal target at %.0f,%.0f: row %08x (%s)", wx, wz, h,
                     h == 0u ? "nothing there" : IsFilterEnabled(filterEntries, filterCount, h) ? "on" : "off: turning it on");
    if (h == 0u || IsFilterEnabled(filterEntries, filterCount, h)) return false;
    HideRowTarget target;
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount && !target.hashes[0]; ++r) {
            const MarkerRowSpec& row = grp.rows[r];
            if (row.typeHash != h && row.pairedHash != h && row.pairedHash2 != h) continue;
            target.hashes[0] = row.typeHash, target.hashes[1] = row.pairedHash, target.hashes[2] = row.pairedHash2;
        }
    }
    if (!target.hashes[0]) target.hashes[0] = h;
    const FilterSnapshot before = TakeFilterSnapshot(client);
    for (uint32_t t : target.hashes) {
        if (t != 0u) SetSingleFilterState(client, mapState, t, true);
    }
    AnnounceFilterChange(before, TakeFilterSnapshot(client));
    return true;
}
bool ShowJournalTargetRow(uint8_t* client, uint8_t* mapState, float wx, float wz) {
    __try {
        return ShowJournalTargetRowInner(client, mapState, wx, wz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The row name of the game's marker at exactly (x, z), for a place mark undone or redone
const char* PlaceRowNameAt(uint8_t* client, int32_t x, int32_t z) {
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            if (*reinterpret_cast<const int32_t*>(m + 0x14) != x || *reinterpret_cast<const int32_t*>(m + 0x24) != z) continue;
            const uint32_t origHash = GetMarkerOrigTypeHash(m);
            if (origHash == 0u || origHash == 0xafce0c5fu) continue;
            if (const char* n = FilterRowName(NormalizeFilterTypeHash(origHash))) return n;
        }
    }
    return nullptr;
}

// Ctrl + click on a place the game shows (a town, a camp, a sanctum, a cave...): marked done by hand, or the mark
// cleared. Not chests (looted), Elixir Wells and Roots (the game checks them off), players, portals, altars or the
// mod's own icons. The nearest such marker the map draws
// within maxDist metres; false if none.
bool ToggleDonePlaceAt(uint8_t* client, float wx, float wz, float maxDist, int32_t* hitX = nullptr, int32_t* hitZ = nullptr) {
    auto placeRow = [](uint32_t fh) {
        if (fh == kGoldChestTypeHash || fh == kSilverChestTypeHash || fh == kWoodChestTypeHash || fh == kSpoilsTypeHash ||
            fh == kOtherPlayerTypeHash || fh == kTownPortalTypeHash || fh == kWaterSourceTypeHash || fh == 0xa447cba3u ||
            fh == kElixirWellRowHash || IsOreHash(fh)) // (Elixir Wells and Roots: the game checks them off)
            return false;
        for (const MarkerRowSpec& r : kCategory1Rows) if (r.typeHash == fh) return true;
        for (const MarkerRowSpec& r : kCategory2Rows) if (r.typeHash == fh) return true;
        for (const MarkerRowSpec& r : kCategory3Rows) if (r.typeHash == fh) return true;
        return false;
    };
    float best = maxDist * maxDist;
    const uint8_t* hit = nullptr;
    uint32_t hitRow = 0;
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            const uint32_t origHash = GetMarkerOrigTypeHash(m);
            if (origHash == 0u || origHash == 0xafce0c5fu || *reinterpret_cast<const uint32_t*>(m + 0x30) == 0u || MarkerHiddenOnMap(m))
                continue;
            if ((*reinterpret_cast<const uint32_t*>(m) & 0xFFFF0000u) == kChestMarkerUidPrefix) continue;
            const float dx = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)) - wx;
            const float dz = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24)) - wz;
            const float d2 = dx * dx + dz * dz;
            if (d2 > best) continue;
            const uint32_t fh = NormalizeFilterTypeHash(origHash);
            if (!placeRow(fh)) continue;
            // only a place shown as itself: one drawn as a quest's "!" or a rumor's "?" (its own row off, the Cave
            // Passage under "Emerald Shores And Sanguine Seas") is the quest's, not a place to check off
            if (*reinterpret_cast<const uint32_t*>(m + 0x30) == 0xafce0c5fu) continue;
            best = d2;
            hit = m;
            hitRow = fh;
        }
    }
    if (!hit) return false;
    const int32_t x = *reinterpret_cast<const int32_t*>(hit + 0x14), z = *reinterpret_cast<const int32_t*>(hit + 0x24);
    bool nowDone = false;
    if (!TogglePlaceDone(x, z, nowDone)) return false;
    if (hitX) *hitX = x;
    if (hitZ) *hitZ = z;
    AnnounceMarkDone(FilterRowName(hitRow), nowDone);
    if (g_log) g_log("Map: place %08x at %d,%d marked %s by hand", hitRow, x, z, nowDone ? "done" : "not done");
    return true;
}

bool g_worldMapWasOpen = true;
int g_worldMapOpenCenterFrames = 0;
uint8_t g_screenBeforeMap = 1; // the last screen before the World Map (1 gameplay, 9 inventory / journal)
uint64_t g_lastWorldMapFrameTick = 0;

void ClearMapViewClickLatch(uint8_t* mapState) {
    if (!mapState) return;
    *reinterpret_cast<uint8_t*>(mapState + 0xf8) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x542e8) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x48) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x50) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x58) = 0;
    if (uint8_t* mv = *reinterpret_cast<uint8_t**>(mapState + 8)) {
        *reinterpret_cast<uint64_t*>(mv + 0x28) = 0;
        mv[0x64] = 0;
        mv[0x65] = 0;
        mv[0x66] = 0;
        mv[0x67] = 0;
        mv[0x68] = 0;
        *reinterpret_cast<uint64_t*>(mv + 0x158) = 0;
        *reinterpret_cast<uint64_t*>(mv + 0x160) = 0;
    }
}

void CenterMapViewOnWorldPos(uint8_t* mapState, float playerX, float playerZ);

void CenterMapViewOnPlayer(uint8_t* client, uint8_t* mapState) {
    if (!client || !mapState) return;
    float playerX = 5120.0f, playerZ = 5120.0f;
    if (!GetHudPlayerWorldPos(client, playerX, playerZ)) return;
    CenterMapViewOnWorldPos(mapState, playerX, playerZ);
}

void CenterMapViewOnWorldPos(uint8_t* mapState, float playerX, float playerZ) {
    if (!mapState) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;

    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    float targetCanvasPos2D[2] = { playerX, mapWorldH - playerZ };

    float curZoom = *reinterpret_cast<float*>(mapView + 0x10);
    if (curZoom <= 0.01f) curZoom = 1.0f;

    *reinterpret_cast<uint64_t*>(mapView + 0x18c) = 0;
    if (g_centerMapOnPos) {
        g_centerMapOnPos(mapView, targetCanvasPos2D, 1);
    }

    *reinterpret_cast<float*>(mapView + 0x20) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x24) = targetCanvasPos2D[1];
    // Clear right-click waypoint target pos (mapView + 0x28/+0x2c) so opening the map doesn't select a waypoint on the player
    *reinterpret_cast<uint64_t*>(mapView + 0x28) = 0;
    *reinterpret_cast<float*>(mapView + 0x30) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x34) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x1a8) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1ac) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x1b0) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1b4) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x198) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x19c) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x1f8) = curZoom * targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1fc) = curZoom * targetCanvasPos2D[1];
    uint64_t panTs = *reinterpret_cast<uint64_t*>(mapView + 0x1a0);
    *reinterpret_cast<uint64_t*>(mapView + 0x08) = (panTs != 0) ? panTs : *reinterpret_cast<uint64_t*>(mapView + 0x00);

    ClearMapViewClickLatch(mapState);

    if (auto* g = static_cast<uint8_t*>(GetUiG())) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                d0[0x140e0] = 0;
                *reinterpret_cast<float*>(d0 + 0x31f38) = targetCanvasPos2D[0];
                *reinterpret_cast<float*>(d0 + 0x31f3c) = targetCanvasPos2D[1];
            }
        }
    }
}

void DrawPulsingMapHighlights(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (g_pulsingPlaces.empty() || !mapState || !widgetRaw) return;
    uint64_t now = GetTickCount64();
    constexpr uint64_t kPulseDurationMs = 14000;
    if (now < g_pulseStartTick || (now - g_pulseStartTick) > kPulseDurationMs) {
        g_pulsingPlaces.clear();
        return;
    }

    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;

    float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    float originX = *reinterpret_cast<float*>(mapView + 0x1f0) +
                    *reinterpret_cast<float*>(mapView + 0x18c) -
                    *reinterpret_cast<float*>(mapView + 0x1f8);
    float originY = *reinterpret_cast<float*>(mapView + 0x1f4) +
                    *reinterpret_cast<float*>(mapView + 0x190) -
                    *reinterpret_cast<float*>(mapView + 0x1fc);

    const float rootX = rootRect ? rootRect[0] : 0.0f;
    const float rootY = rootRect ? rootRect[1] : 0.0f;
    const float baseSize = 28.0f;
    const float expandSize = (g_pulsingPlaces.size() == 1) ? 200.0f : 120.0f;

    float elapsedSec = static_cast<float>(now - g_pulseStartTick) * 0.001f;
    float phase = std::fmod(elapsedSec, 2.25f) / 2.25f;
    float t = std::min(1.0f, phase * 2.0f);
    float oneMinusT = 1.0f - t;
    float ease = 1.0f - oneMinusT * oneMinusT;
    float fallbackDiam = baseSize + ease * expandSize;
    float fallbackAlpha = oneMinusT * 0.70f;
    uint32_t pulseCol = 0xff6be0ffu;
    if (uint8_t* assets = GetUiAssets()) {
        pulseCol = *reinterpret_cast<uint32_t*>(assets + kMapPulseColorOffset) | 0xff000000u;
    }

    // Use the game's native World Map marker pulse function (0x140cf63e0, assets+0xee70 with blend mode 4)
    for (const auto& p : g_pulsingPlaces) {
        float sx = originX + p.wx * zoom;
        float sy = originY + (mapWorldH - p.wz) * zoom;
        if (sx < -120.0f || sx > 4000.0f || sy < -120.0f || sy > 2400.0f) continue;

        if (g_drawInGameMapPulse) {
            g_drawInGameMapPulse(PackXY(sx - rootX, sy - rootY), baseSize, expandSize);
        } else if (fallbackAlpha > 0.01f) {
            DrawAssetRectAbs(widgetRaw, kMapPulseRingPrimitive,
                             sx - fallbackDiam * 0.5f, sy - fallbackDiam * 0.5f,
                             fallbackDiam, fallbackDiam, pulseCol, fallbackAlpha, 1.0f);
        }
    }
}

void LoadMinimapConfig();

// The game's own name for a marker type, in the game's language (its name GUID at +0 -> loca id ->
// text). Only a few types have one (Ancient Spire, Flame Altar, Water Source, Fast Travel Point).
bool ReadGameMarkerTypeNameInner(uint32_t typeHash, char* out, size_t cap) {
    uint8_t* client = GetUiClient();
    if (!client || !g_getMarkerType || !g_locaIdToText) return false;
    void* mgr = *reinterpret_cast<void**>(client + 0x4667f0);
    if (!mgr) return false;
    const uint8_t* type = g_getMarkerType(typeHash, mgr);
    if (!type) return false;
    static const uint8_t kNoGuid[16]{};
    if (std::memcmp(type, kNoGuid, 16) == 0) return false;
    UiText text{};
    g_locaIdToText(&text, GuidToLocaId(type));
    if (!text.utf8 || text.len == 0 || text.len >= cap) return false;
    std::memcpy(out, text.utf8, text.len);
    out[text.len] = '\0';
    return true;
}

bool ReadGameMarkerTypeName(uint32_t typeHash, char* out, size_t cap) {
    __try {
        return ReadGameMarkerTypeNameInner(typeHash, out, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLocaTextInner(uint32_t locaId, char* out, size_t cap) {
    UiText text{};
    g_locaIdToText(&text, locaId);
    if (!text.utf8 || text.len == 0 || text.len >= cap) return false;
    std::memcpy(out, text.utf8, text.len);
    out[text.len] = '\0';
    return true;
}

bool ReadLocaText(uint32_t locaId, char* out, size_t cap) {
    if (!g_locaIdToText) return false;
    __try {
        return ReadLocaTextInner(locaId, out, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// An ore's name: the game's own item name ("Kupfererz") when the game's text is in the mod's language,
// otherwise (no single item, the lookup failed, or the ini names another language) the mod's translation.
const char* OreName(size_t ore) {
    if (ore >= kOreCount) return "";
    static char names[kOreCount][96]{};
    static int state[kOreCount]{}; // 0 not tried, 1 have it, 2 none
    static int builtFor = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (builtFor != lang) {
        std::memset(state, 0, sizeof(state));
        builtFor = lang;
    }
    if (umm_lang::Current() == umm_lang::Lang::English) return kOreLabels[ore];
    if (umm_lang::GameTextInCurrentLanguage() && kOreNameLocaIds[ore] != 0u) {
        if (state[ore] == 0 && GetUiClient()) {
            state[ore] = ReadLocaText(kOreNameLocaIds[ore], names[ore], sizeof(names[ore])) ? 1 : 2;
            static bool loggedMiss = false;
            if (state[ore] == 2 && g_log && !loggedMiss) {
                loggedMiss = true;
                g_log("Resource names: the game has no %s text for %s, using the mod's", umm_lang::SteamCode(umm_lang::Current()),
                      kOreLabels[ore]);
            }
        }
        if (state[ore] == 1) return names[ore];
    }
    const char* own = ore >= kLatePlantsFirst ? umm_lang::LatePlantName(ore - kLatePlantsFirst) : umm_lang::ResourceName(ore);
    return own ? own : kOreLabels[ore];
}

// The enabled tree areas on the World Map: each forest as one filled shape, named in the bigger ones.
// Returns how many shapes it drew, so the deposit icons can share one budget with it.
bool OutsideFocusedRegion(float x, float z); // regions.inl
extern bool g_regionOutsideHide;
bool g_treesAsImages = false; // this frame's forests are painted into the area images (ground_areas.inl): labels only

int DrawTreeAreasOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    bool any = false;
    for (size_t o = kTreesFirst; o < kTreesFirst + kTreesCount; ++o) any = any || g_oreEnabled[o];
    if (!widgetRaw || !mapState || !any) return 0;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return 0;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return 0;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) +
                          *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) +
                          *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1] + 114.0f; // below the menu banner
    const float right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    // Each forest is one filled shape in its type's colour (rectangles of 32 m cells that together trace the outline),
    // and the bigger forests carry their type's name in the map's own location-name style, like its region labels.
    // Zoomed out, the 2,120 rectangles are each a few pixels: thousands of draws that overflow the game's UI
    // renderer with everything else on (the map stops drawing, then the game can crash). There they are painted
    // into a grid of kTreeGridPx screen cells and drawn as one strip per run of same-type cells in a row.
    constexpr int kMaxTreeRectsPerFrame = 1500;
    constexpr float kTreeGridPx = 6.0f;
    const float cellPx = static_cast<float>(kTreeShapeCellMeters) * zoom;
    // Map Key > Region: a forest piece outside the region (by its middle) is dimmed or not drawn
    auto outside = [](const TreeShapeRect& t) {
        return OutsideFocusedRegion((t.x0 + t.x1 + 1) * 0.5f * kTreeShapeCellMeters,
                                    (t.z0 + t.z1 + 1) * 0.5f * kTreeShapeCellMeters);
    };
    int drawn = 0;
    if (g_treesAsImages) {
        // painted with the Ground areas, a few image draws (ground_areas.inl)
    } else if (cellPx < kTreeGridPx) {
        const int gw = static_cast<int>((right - left) / kTreeGridPx) + 1;
        const int gh = static_cast<int>((bottom - top) / kTreeGridPx) + 1;
        static std::vector<uint8_t> grid; // ore index + 1 (+ 0x80 outside the focused region), 0 = no forest
        grid.assign(static_cast<size_t>(gw) * gh, 0);
        for (size_t i = 0; i < kTreeShapeRectCount; ++i) {
            const TreeShapeRect& t = kTreeShapeRects[i];
            if (!g_oreEnabled[t.ore]) continue;
            const bool out = outside(t);
            if (out && g_regionOutsideHide) continue;
            const float x0 = originX + t.x0 * cellPx, x1 = originX + (t.x1 + 1) * cellPx;
            const float y1 = originY + (mapWorldH - t.z0 * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            const float y0 = originY + (mapWorldH - (t.z1 + 1) * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            if (x1 < left || x0 > right || y1 < top || y0 > bottom) continue;
            const int gx0 = std::max(0, static_cast<int>((x0 - left) / kTreeGridPx));
            const int gx1 = std::min(gw - 1, static_cast<int>((x1 - left) / kTreeGridPx));
            const int gy0 = std::max(0, static_cast<int>((y0 - top) / kTreeGridPx));
            const int gy1 = std::min(gh - 1, static_cast<int>((y1 - top) / kTreeGridPx));
            for (int gy = gy0; gy <= gy1; ++gy)
                std::memset(&grid[static_cast<size_t>(gy) * gw + gx0], (t.ore + 1) | (out ? 0x80 : 0),
                            static_cast<size_t>(gx1 - gx0 + 1));
        }
        for (int gy = 0; gy < gh && drawn < kMaxTreeRectsPerFrame; ++gy) {
            const uint8_t* row = &grid[static_cast<size_t>(gy) * gw];
            for (int gx = 0; gx < gw && drawn < kMaxTreeRectsPerFrame;) {
                const uint8_t v = row[gx];
                int end = gx + 1;
                while (end < gw && row[end] == v) ++end;
                if (v) {
                    DrawSolidRectAbs(widgetRaw, left + gx * kTreeGridPx, top + gy * kTreeGridPx,
                                     (end - gx) * kTreeGridPx, kTreeGridPx, kOreAreaColors[(v & 0x7f) - 1],
                                     (v & 0x80) ? 0.1f : 0.34f);
                    ++drawn;
                }
                gx = end;
            }
        }
    } else {
        for (size_t i = 0; i < kTreeShapeRectCount && drawn < kMaxTreeRectsPerFrame; ++i) {
            const TreeShapeRect& t = kTreeShapeRects[i];
            if (!g_oreEnabled[t.ore]) continue;
            const bool out = outside(t);
            if (out && g_regionOutsideHide) continue;
            const float x0 = originX + t.x0 * cellPx, x1 = originX + (t.x1 + 1) * cellPx;
            const float y1 = originY + (mapWorldH - t.z0 * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            float y0 = originY + (mapWorldH - (t.z1 + 1) * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            if (x1 < left || x0 > right || y1 < top || y0 > bottom) continue;
            y0 = std::max(y0, top);
            if (y1 - y0 < 0.5f) continue;
            DrawSolidRectAbs(widgetRaw, x0, y0, x1 - x0, y1 - y0, kOreAreaColors[t.ore], out ? 0.1f : 0.34f);
            ++drawn;
        }
    }
    // Forests of different types can share a centre (mixed woods), so a label that would land on one already drawn
    // is stacked below it instead.
    float placed[kTreeLabelCount][4];
    size_t placedCount = 0;
    for (size_t i = 0; i < kTreeLabelCount; ++i) {
        const TreeLabel& t = kTreeLabels[i];
        if (!g_oreEnabled[t.ore]) continue;
        const bool labelOut = OutsideFocusedRegion(static_cast<float>(t.x), static_cast<float>(t.z));
        if (labelOut && g_regionOutsideHide) continue;
        if (t.x < 2700 && t.z > 7900) continue; // the Hollow Halls' corner (halls::InCorner): nothing of the overworld
        const float cx = originX + t.x * zoom;
        float cy = originY + (mapWorldH - t.z) * zoom;
        if (cx < left || cx > right || cy < top || cy > bottom) continue;
        const char* name = OreName(t.ore);
        const float w = umm_lang::TextWidth(name) * 1.6f + 24.0f, h = 28.0f;
        // The 1 px gap keeps float rounding from leaving it overlapping by a hair (that looped forever), and the
        // pass limit is a second guard.
        bool moved = true;
        for (size_t pass = 0; moved && pass <= kTreeLabelCount; ++pass) {
            moved = false;
            for (size_t p = 0; p < placedCount; ++p) {
                const float* r = placed[p];
                if (cx - w * 0.5f < r[0] + r[2] && cx + w * 0.5f > r[0] && cy - h * 0.5f < r[1] + r[3] &&
                    cy + h * 0.5f > r[1]) {
                    cy = r[1] + r[3] + h * 0.5f + 1.0f;
                    moved = true;
                }
            }
        }
        placed[placedCount][0] = cx - w * 0.5f;
        placed[placedCount][1] = cy - h * 0.5f;
        placed[placedCount][2] = w;
        placed[placedCount][3] = h;
        ++placedCount;
        DrawLabelAbs(cx - w * 0.5f, cy - h * 0.5f, w, h, name, 0xffffffffu, labelOut ? 0.3f : 0.9f, TextAlignH::Center,
                     TextAlignV::Center, GetUiAssets() ? GetUiAssets() + kMapLocationNameStyleOffset : nullptr);
    }
    return drawn;
}

#include "knowledge.inl"
#include "creature_live.inl"
#include "regions.inl"
#include "levels.inl"
#include "hd_pool.inl"
#include "halls.inl"
#include "ground_areas.inl"
#include "roads.inl"
void DrawUnderRing(void* widget); // the mod's icons from over the player's ring (desk.inl calls it)
#include "desk.inl"
#include "hall_pan_clamp.inl"
#include "map_shader.inl"
#include "cursor_position.inl"

// The message's plate width, for placing it beside the minimap
float FilterNoticeWidth(float rootW) {
    return std::min(rootW - 80.0f, umm_lang::TextWidth(g_filterNotice.c_str()) * 1.2f + 32.0f);
}

// The message: a plate, for three seconds, fading in the last half second. By default over the Position plate's place
// on the World Map; onHud, at the left edge given (the minimap's side).
void DrawFilterNotice(void* widgetRaw, const float* rootRect, bool onHud = false, float hudX = 0.0f, float hudY = 0.0f) {
    if (g_filterNotice.empty() || !widgetRaw || onHud != g_filterNoticeOnHud) return;
    constexpr uint64_t kShowMs = 3000, kFadeMs = 500;
    const uint64_t age = GetTickCount64() - g_filterNoticeTick;
    if (age >= kShowMs) {
        g_filterNotice.clear();
        return;
    }
    using namespace key_style;
    const float alpha = age + kFadeMs > kShowMs ? static_cast<float>(kShowMs - age) / kFadeMs : 1.0f;
    const float rootW = rootRect[2] > 100.0f ? rootRect[2] : 1920.0f;
    const float rootH = rootRect[3] > 100.0f ? rootRect[3] : 1080.0f;
    constexpr float kPlateH = 30.0f;
    const float plateW = FilterNoticeWidth(rootW);
    const float plateX = onHud ? hudX : rootRect[0] + rootW * 0.5f - plateW * 0.5f;
    const float plateY = onHud ? hudY : rootRect[1] + rootH - 130.0f;
    constexpr uint32_t kTraySlate = 0xFF6C5A58u, kTrayEdge = 0xFF2A2020u, kTrayGoldLine = 0xFF4986A0u;
    DrawFramedBoxAbs(widgetRaw, plateX, plateY, plateW, kPlateH, kTraySlate, 0.97f * alpha, kTrayEdge, 0.95f * alpha, 6.0f);
    DrawFramedBoxAbs(widgetRaw, plateX + 2.0f, plateY + 2.0f, plateW - 4.0f, kPlateH - 4.0f, kTraySlate, alpha,
                     kTrayGoldLine, 0.75f * alpha, 5.0f);
    DrawLabelAbs(plateX, plateY, plateW, kPlateH, g_filterNotice.c_str(), 0xFFC6EFFCu, 0.97f * alpha, TextAlignH::Center,
                 TextAlignV::Center, nullptr, 0.9f);
}
#include "chest_silhouette.inl"
#include "vein_sight.inl"
#include "chest_glow.inl"

// The enabled ore deposits on the world map, drawn by the mod (the game's marker arrays are far too small for
// them). Same world -> screen mapping as the Find pulses. Hovering one names it.
// A check mark on each Flame and Night Sanctum this character has been to (the game marks none), at the marker
// icon's bottom right like the game's completed places.
void DrawVisitedSanctumsOnMap(void* widgetRaw, const float* rootRect, uint8_t* client, uint8_t* mapState) {
    if (!widgetRaw || !client || !mapState || g_hideCompletedPlaces) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) + *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) + *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    const bool flameOn = IsFilterEnabled(filterEntries, filterCount, NormalizeFilterTypeHash(0x7b4c9e16u));
    const bool nightOn = IsFilterEnabled(filterEntries, filterCount, NormalizeFilterTypeHash(0x6df85109u));
    constexpr float kIconHalf = 13.0f, kCheck = 14.0f;
    for (size_t i = 0; i < kSanctumSpotCount; ++i) {
        const SanctumSpot& sp = kSanctumSpots[i];
        if (!SanctumVisitedNear(sp.x, sp.z)) continue;
        // Night Sanctums come after the 63 Flame ones in sanctum_spots.h
        if (!(i < 63 ? flameOn : nightOn)) continue;
        const float sx = originX + sp.x * zoom, sy = originY + (mapWorldH - sp.z) * zoom;
        if (sx < left || sx > right || sy < top + 114.0f || sy > bottom) continue;
        // Its icon's rules: Region > Hide takes the mark too, Dim fades it like the icon (izisot)
        if (RegionHidesAt(sp.x, sp.z) || halls::MapHides(sp.x, sp.z)) continue;
        const float a = OutsideFocusedRegion(sp.x, sp.z) ? 0.3f : 1.0f;
        const float cx = sx + kIconHalf - kCheck * 0.7f, cy = sy + kIconHalf - kCheck * 0.7f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 1.0f, cy - 1.0f, kCheck + 2.0f, kCheck + 2.0f, 0xFF0A0E10u, 0.85f * a);
        DrawAssetRectAbs(widgetRaw, kCheckmarkPrimitive, cx, cy, kCheck, kCheck, 0xFFFFFFFFu, 0.98f * a, 1.12f);
    }
    // The places marked done by hand, where the map draws their marker (not the game's own completed ones: theirs is
    // drawn already)
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t arr = 0; arr < 4; ++arr) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[arr]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[arr] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            const int32_t px = *reinterpret_cast<const int32_t*>(m + 0x14), pz = *reinterpret_cast<const int32_t*>(m + 0x24);
            if (m[0x34] == 3 || *reinterpret_cast<const uint32_t*>(m + 0x30) == 0u || !PlaceMarkedDone(px, pz)) continue;
            const uint32_t origHash = GetMarkerOrigTypeHash(m);
            if (origHash == 0u || origHash == 0xafce0c5fu || MarkerHiddenOnMap(m) ||
                NormalizeFilterTypeHash(origHash) == kElixirWellRowHash) continue;
            // Only where the place's own row shows it (its marker stays in the game's list with the row off)
            if (!IsFilterEnabled(filterEntries, filterCount, NormalizeFilterTypeHash(origHash))) continue;
            if ((origHash == 0x7b4c9e16u || origHash == 0x6df85109u) &&
                SanctumVisitedNear(static_cast<float>(px), static_cast<float>(pz))) continue; // drawn above
            const float fx = static_cast<float>(px), fz = static_cast<float>(pz);
            const float sx = originX + fx * zoom, sy = originY + (mapWorldH - fz) * zoom;
            if (sx < left || sx > right || sy < top + 114.0f || sy > bottom) continue;
            if (RegionHidesAt(fx, fz) || halls::MapHides(fx, fz)) continue;
            const float a = OutsideFocusedRegion(fx, fz) ? 0.3f : 1.0f;
            const float cx = sx + kIconHalf - kCheck * 0.7f, cy = sy + kIconHalf - kCheck * 0.7f;
            DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 1.0f, cy - 1.0f, kCheck + 2.0f, kCheck + 2.0f, 0xFF0A0E10u, 0.85f * a);
            DrawAssetRectAbs(widgetRaw, kCheckmarkPrimitive, cx, cy, kCheck, kCheck, 0xFFFFFFFFu, 0.98f * a, 1.12f);
        }
    }
}

// A check mark on each gold and silver chest looted here once that has refilled since: the looted ones carry the
// game's own (completed state), these keep theirs at the same corner of the icon.
void DrawRefilledChestsOnMap(void* widgetRaw, const float* rootRect, uint8_t* client, uint8_t* mapState) {
    if (!widgetRaw || !client || !mapState || (!g_goldChestEnabled && !g_silverChestEnabled)) return; // wood: its own
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) + *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) + *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    const bool exploredOnly = g_pinRevealMode == PinRevealMode::ExploredOnly;
    constexpr float kIconHalf = 13.0f, kCheck = 14.0f;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        if (IsWoodChest(c)) continue;
        const ChestLootState& st = g_chestLootStates[c];
        if (!st.everLooted || st.looted) continue;
        if (!ChestRowEnabled(c)) continue;
        const ChestSpawn& sp = kChestSpawns[c];
        const float x = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        const float z = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        const float sx = originX + x * zoom, sy = originY + (mapWorldH - z) * zoom;
        if (sx < left || sx > right || sy < top + 114.0f || sy > bottom) continue;
        if (exploredOnly && !IsPositionExploredByCharacter(client, sp.px1, sp.pz1)) continue;
        // Its icon's rules: Region > Hide, and the Hollow Halls' corner (or another hall on a dungeon map)
        if (RegionHidesAt(x, z) || halls::MapHidesChest(c)) continue;
        const float a = OutsideFocusedRegion(x, z) ? 0.3f : 1.0f; // Region > Dim, as the icon
        const float cx = sx + kIconHalf - kCheck * 0.7f, cy = sy + kIconHalf - kCheck * 0.7f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 1.0f, cy - 1.0f, kCheck + 2.0f, kCheck + 2.0f, 0xFF0A0E10u, 0.85f * a);
        DrawAssetRectAbs(widgetRaw, kCheckmarkPrimitive, cx, cy, kCheck, kCheck, 0xFFFFFFFFu, 0.98f * a, 1.12f);
    }
}

// A squat caret, wider than tall, flat filled on a dark edge: unlike the players' slender pointers.
// Up: green, centred on (ax, ay) pointing up. Down: orange, pointing down.
void DrawHeightCaretAbs(void* widgetRaw, float ax, float ay, bool up, float alpha, float k) {
    const uint32_t col = up ? 0xFF7CE08Cu : 0xFF4C9CF0u;
    // On whole pixels: 1 px rows, whole-pixel widths, the caret snapped to the pixel grid. Rows of 0.6 px at
    // fractional positions rounded differently each frame as the minimap moved under you: it shimmered (2026-11-09)
    ax = std::round(ax);
    ay = std::round(ay);
    auto drawCaret = [&](float halfBase, float height, uint32_t c, float a) {
        const int rows = std::max(3, static_cast<int>(std::lround(height)));
        for (int row = 0; row < rows; ++row) {
            const float w = std::max(1.0f, 2.0f * std::round(halfBase * (row + 1) / rows)); // narrow at the point
            const float y = up ? ay - std::floor(rows * 0.5f) + row : ay + std::ceil(rows * 0.5f) - (row + 1);
            DrawSolidRectAbs(widgetRaw, ax - w * 0.5f, y, w, 1.0f, c, a);
        }
    };
    drawCaret(5.6f * k, 5.4f * k, 0xFF0A0E10u, 0.9f * alpha); // the dark edge
    drawCaret(4.2f * k, 3.8f * k, col, alpha);
}

// The local player's height (m), false before the world is in
bool LocalPlayerY(uint8_t* client, float& y) {
    uint8_t* clientWorld = GetClientWorld(client);
    if (!clientWorld) return false;
    const int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c20);
    y = static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10);
    return rawY != 0 && std::isfinite(y);
}

// A player grave (client + 0x3e6a78) 4 m or more above or below you: +1 / -1, else 0
int8_t GraveHeightDir(const uint8_t* m, bool haveY, float playerY) {
    if (!haveY) return 0;
    const float gy = static_cast<float>(static_cast<double>(*reinterpret_cast<const int64_t*>(m + 0x18)) *
                                        2.3283064365386963e-10);
    const float dy = gy - playerY;
    if (!std::isfinite(dy) || std::abs(dy) >= 2000.0f) return 0;
    return dy >= 4.0f ? 1 : dy <= -4.0f ? -1 : 0;
}

// A minimap marker's height caret (+1 above you, -1 below, 0 neither) with a dead zone: on at `on` m, off again
// only under `on` - 0.5 m. Rooms and floors step in whole 4 m blocks: at 4 m a chest exactly 4 m under your walkway
// (Veilwater spire, 2026-11-09) blinked with every stair step. key: the marker (chest index, or 0x10000 + deposit)
int8_t HeightCaret(uint32_t key, float dy, float on) {
    static std::unordered_map<uint32_t, int8_t> last;
    int8_t& l = last[key];
    const float off = on - 0.5f;
    const int8_t d = dy >= on ? 1 : dy <= -on ? -1 : (l == 1 && dy >= off) ? 1 : (l == -1 && dy <= -off) ? -1 : 0;
    l = d;
    return d;
}

// The height caret on each player grave on the world map, at the grave icon's top right (up) or bottom right (down)
void DrawGraveHeightsOnMap(void* widgetRaw, const float* rootRect, uint8_t* client, uint8_t* mapState) {
    if (!widgetRaw || !client || !mapState) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    float playerY = 0.0f;
    if (!LocalPlayerY(client, playerY)) return;
    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    if (!IsFilterEnabled(filterEntries, filterCount, NormalizeFilterTypeHash(0x1ad8f96eu))) return;
    uint8_t* base = *reinterpret_cast<uint8_t**>(client + 0x3e6a78);
    const uint64_t count = *reinterpret_cast<uint64_t*>(client + 0x3e6a78 + 8);
    if (!base || count == 0 || count > 10000) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) + *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) + *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    constexpr float kGraveHalf = 20.0f; // the grave's diamond
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* m = base + i * 0xA0;
        const int8_t dir = GraveHeightDir(m, true, playerY);
        if (dir == 0) continue;
        const float wx = static_cast<float>(static_cast<double>(*reinterpret_cast<const int64_t*>(m + 0x10)) *
                                            2.3283064365386963e-10);
        const float wz = static_cast<float>(static_cast<double>(*reinterpret_cast<const int64_t*>(m + 0x20)) *
                                            2.3283064365386963e-10);
        if (wx <= 1.0f && wz <= 1.0f) continue;
        const float sx = originX + wx * zoom, sy = originY + (mapWorldH - wz) * zoom;
        if (sx < left || sx > right || sy < top + 114.0f || sy > bottom) continue;
        if (MarkerHiddenOnMap(m)) continue; // the grave's own rules: Region > Hide, another Hollow Hall
        DrawHeightCaretAbs(widgetRaw, sx + kGraveHalf * 0.75f, sy + (dir > 0 ? -0.75f : 0.75f) * kGraveHalf, dir > 0,
                           MarkerOutsideFocusedRegion(m) ? 0.3f : 1.0f, 1.6f);
    }
}

// When the mod last drew a hover title: the game's own (MarkerTitleTextHook) stays away meanwhile, so an item drawn
// beside its chest doesn't print its name over "Silver Chest"
uint64_t g_modHoverTitleTick = 0;

// Like the game's own hover title ("Gem Forge"): bold and large, starting at x and centred on y, on a soft shadow.
// A copy of the game's bold h3 style at 40 px, the game's own title's size (its identifier string dropped: it is a
// relative pointer).
bool GameTitleReplaced(const void* m); // with the title hook
// the name of the quest whose place pin is under the cursor, drawn by the mod (HideRumorsInner)
char g_questTitle[160]{};
uint64_t g_questTitleTick = 0;
const uint8_t* g_questTitleFor = nullptr;
const uint8_t* g_questPinNamed = nullptr; // a hidden quest / rumor pin under the cursor: the mod names it (see PublishCursorIcon)
uint64_t g_questPinNamedTick = 0;
void PublishCursorIcon();
#ifdef UMM_DEV_BRIDGE
void RunQuestAt(uint8_t* client);
#endif
bool ModHoverTitleShowing();
void* g_hoverTitleWidget = nullptr; // the World Map overlay's root this frame: the mod's titles' panels go in it
// The hover names asked for this frame: drawn last (FlushMapHoverTitles), after every icon pass, or the icons drawn
// after them (the Wooden Chests, the portals) showed over the name (2026-11-12)
struct PendingTitle {
    float x, y;
    std::string name;
    int kind; // 0 the mod's own icon, 1 a game marker's title, 2 a quest pin's (its card covered)
};
std::vector<PendingTitle> g_pendingTitles;
void DrawMapHoverTitleNow(float x, float y, const char* name, float minText = 0.0f);
// The mod's icon under the cursor: its centre on the screen (its hover passes note it) and its name, for its click menu
struct ModHoverIcon {
    float sx = 0.0f, sy = 0.0f;
    uint64_t tick = 0;
    std::string name;
} g_modHoverIcon;
void NoteModHoverIcon(float sx, float sy) { g_modHoverIcon.sx = sx, g_modHoverIcon.sy = sy, g_modHoverIcon.tick = GetTickCount64(); }
void DrawMapHoverTitle(float x, float y, const char* name, bool gameTitle) {
    // the game's icon is over the mod's: its title (not a quest's "!": it is hovered anywhere in its circle, and its
    // spot is its diamond's tip, where an icon beside it sits; FlushMapHoverTitles picks between them)
    if (!GetUiAssets() || !name || !*name || (!gameTitle && !QuestPinHovered() && GameIconUnderCursor())) return;
    // A marker's menu is open and the cursor is on its column: the pins behind it get no name (their names came
    // through over Send Ping / Remove Marker, 2026-11-12)
    // (with any marker's menu open, no name at all: one put by a quest's card ran over the menu, 2026-11-12)
    if (g_drawExclusion.active) return;
    if (!gameTitle) g_modHoverTitleTick = GetTickCount64(), g_modHoverIcon.name = name;
    const int kind = !gameTitle ? 0 : name == g_questTitle ? 2 : 1;
    if (g_pendingTitles.size() < 4) g_pendingTitles.push_back({x, y, name, kind});
}
// A quest's card is up (the cursor anywhere in its search circle) and the mod's icon is under the cursor too (a
// collectible inside the circle, 2026-11-12): the icon's name (its title only gets here while the cursor is on it), put
// where the game's card is and at least as wide, so the card's plain text stays covered
void FlushMapHoverTitles() {
    const PendingTitle* quest = nullptr;
    const PendingTitle* mine = nullptr;
    for (const PendingTitle& t : g_pendingTitles) {
        if (t.kind == 2 && !quest) quest = &t;
        if (t.kind == 0 && !mine) mine = &t;
    }
#ifdef UMM_DEV_BRIDGE
    static uint64_t lgT = 0;
    if (g_log && !g_pendingTitles.empty() && GetTickCount64() - lgT > 400) {
        lgT = GetTickCount64();
        std::string w;
        for (const PendingTitle& t : g_pendingTitles) w += " [" + std::to_string(t.kind) + "] " + t.name;
        g_log("Map: hover names:%s; on game icon %d", w.c_str(), GameIconUnderCursor());
    }
#endif
    if (quest && mine) { // the mod's icon is under the cursor: its name, over the quest's card
        DrawMapHoverTitleNow(quest->x, quest->y, mine->name.c_str(), umm_lang::TextWidth(quest->name.c_str()));
    } else {
        for (const PendingTitle& t : g_pendingTitles) DrawMapHoverTitleNow(t.x, t.y, t.name.c_str());
    }
    g_pendingTitles.clear();
}
void DrawMapHoverTitleNow(float x, float y, const char* name, float minText) {
    static uint8_t titleStyle[kUiCharacterStyleSize]{};
    static uint8_t* titleStyleFor = nullptr;
    uint8_t* assets = GetUiAssets();
    if (!assets) return;
    if (titleStyleFor != assets) {
        std::memcpy(titleStyle, assets + kH3StyleOffset, sizeof(titleStyle));
        std::memset(titleStyle, 0, 8);
        const float titleSize = 40.0f;
        std::memcpy(titleStyle + kStyleFontSize, &titleSize, sizeof(titleSize));
        titleStyleFor = assets;
    }
    const float fontScale = 40.0f / 15.0f; // TextWidth is calibrated on the small text
    const float h = 18.0f * fontScale, w = umm_lang::TextWidth(name) * fontScale + 12.0f;
    const float by = y - h * 0.5f;
    g_modTitleBox = {true, x - 4.0f, by - 4.0f, x + w * 1.45f + 8.0f, by + h + 4.0f};
    g_modTitleBoxTick = GetTickCount64();
    // a dark tooltip panel behind it, like the game's titles get (SafeDrawTitlePanel)
    {
        // (the drawn title measured on screen: ~0.62 of TextWidth at this scale; no soft shadow: it showed as a smear)
        const float pw = std::max(umm_lang::TextWidth(name), minText) * fontScale * 0.68f + 44.0f, ph = h * 0.95f; // ("Gold Chest (Locked)" ran past 0.62)
        const float px = x - 18.0f, py = y - ph * 0.5f;
        DrawFramedBoxAbs(g_hoverTitleWidget, px, py, pw, ph, 0xff15191au, 0.93f, 0xff6c8c96u, 0.45f, 6.0f);
    }
    // No band behind it (the soft glow reads as a dark oval on the map): a faint 1 px drop shadow, like the game's own
    // hover title (Silver Chest)
    // The style's size only caps the label's: the label is scaled up to the game's title (measured: 1.45x)
    constexpr float kTitleScale = 1.45f;
    DrawLabelAbs(x + 1.0f, by + 1.0f, w, h, name, 0xff000000u, 0.5f, TextAlignH::Left, TextAlignV::Center, titleStyle,
                 kTitleScale);
    DrawLabelAbs(x, by, w, h, name, 0xffffffffu, 1.0f, TextAlignH::Left, TextAlignV::Center, titleStyle, kTitleScale);
}

namespace halls {
bool NearDungeon(float x, float z, int d); // halls.inl
}
// A Dungeon Spoils chest under the cursor: its sarcophagus' name (Ranger / Mage / Warrior Chest, or the plain one) and,
// under it, the ectoplasm it takes to open: the hall's grade (the game's item name), 7 for a plain one, 45 or 60 for a
// class one (silver or gold, which the hall rolls). Halls in kHalls order: Springlands and Revelwood fragments, Nomad
// Highlands shards, Veilwater Basin aggregate, Albaneve Summits clusters, Kindlewastes crystals.
void DrawSpoilsHover(size_t c, float x, float y) {
    using umm_lang::Str;
    const size_t e = c - kEndChestFirst;
    if (e >= sizeof(kEndChestKind)) return;
    static const Str kName[4] = {Str::WoodSarcophagus, Str::RangerChest, Str::MageChest, Str::WarriorChest};
    DrawMapHoverTitle(x, y, umm_lang::T(kName[kEndChestKind[e]]));
    const ChestSpawn& sp = kChestSpawns[c];
    const float wx = sp.px1 + sp.px0 / 65536.0f, wz = sp.pz1 + sp.pz0 / 65536.0f;
    static const uint32_t kEctoplasmName[6] = {2943239867u, 2943239867u, 1035568991u, 583593149u, 1647084269u, 2309456180u};
    int hall = -1;
    for (int d = 0; d < 6 && hall < 0; ++d)
        if (halls::NearDungeon(wx, wz, d)) hall = d;
    if (hall < 0) return;
    static char names[6][96]{};
    static int namesLang = -1;
    if (namesLang != static_cast<int>(umm_lang::Current())) {
        for (auto& n : names) n[0] = 0;
        namesLang = static_cast<int>(umm_lang::Current());
    }
    if (!names[hall][0] && !ReadLocaText(kEctoplasmName[hall], names[hall], sizeof(names[hall])))
        std::snprintf(names[hall], sizeof(names[hall]), "Ectoplasm");
    char line[160];
    if (kEndChestKind[e] == 0) std::snprintf(line, sizeof(line), "7 \xc3\x97 %s", names[hall]);
    else std::snprintf(line, sizeof(line), "45 / 60 \xc3\x97 %s", names[hall]);
    DrawLabelAbs(x + 1.0f, y + 23.0f, umm_lang::TextWidth(line) * 1.6f + 20.0f, 26.0f, line, 0xff000000u, 0.5f,
                 TextAlignH::Left, TextAlignV::Center, nullptr, 1.25f);
    DrawLabelAbs(x, y + 22.0f, umm_lang::TextWidth(line) * 1.6f + 20.0f, 26.0f, line, 0xffd6e6eeu, 1.0f, TextAlignH::Left,
                 TextAlignV::Center, nullptr, 1.25f);
}

uint32_t g_hoveredDepositRow = 0u; // the row of the mod's icon under the cursor (a lore note, an ore, ...), or 0
// (any of the mod's icons with its name up: lore notes and collectibles like the Ripped Alchemist's Wall Hanging have no
// row here)
bool ModIconHovered() { return g_hoveredDepositRow != 0u || GetTickCount64() - g_modHoverTitleTick < 100ULL; }

// The map's one-icon-per-cell thinning: a cell of the grid laid over the screen, from an icon's place on the map in
// pixels and the screen edge's. The cells stay put on the map, so panning doesn't change which icons share one.
int MapThinCell(float mapPx, float edgePx, float cell) {
    return static_cast<int>(std::floor(mapPx / cell)) - static_cast<int>(std::floor(edgePx / cell));
}

// Your position from the client's player list (client + 0x4642c0, 0x240 apart, live position at +0x10 / +0x18)
bool LocalPlayerXZ(uint8_t* client, float& x, float& z) {
    if (!client) return false;
    uint8_t* players = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
    const uint64_t count = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
    const uint32_t localId = *reinterpret_cast<uint32_t*>(client + 0x18);
    if (!players || count > 64) return false;
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* p = players + i * 0x240;
        if (*reinterpret_cast<const uint32_t*>(p) != localId) continue;
        x = *reinterpret_cast<const float*>(p + 0x10);
        z = *reinterpret_cast<const float*>(p + 0x18);
        return true;
    }
    return false;
}

void DrawOreDepositsOnMap(void* widgetRaw, const float* rootRect, uint8_t* client, uint8_t* mapState,
                          bool haveCursor, float mx, float my, int maxIcons) {
    g_hoveredDepositRow = 0u;
    if (!widgetRaw || !mapState || !AnyOreEnabled()) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) +
                          *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) +
                          *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);

    constexpr float kMapBannerBottom = 114.0f; // the SOCIAL / WORLD / JOURNAL menu banner's gold edge
    // Small when zoomed out, up to a chest-sized icon close in
    const float size = std::max(12.0f, std::min(22.0f, 9.0f + zoom * 6.0f));
    const float half = size * 0.5f;
    size_t hovered = kOreDepositCount;
    float hoveredSx = 0.0f, hoveredSy = 0.0f;
    // Thousands of icons in one frame overflow the game's UI renderer (the map's own tiles stop drawing), so:
    // one icon per screen cell (where they would overlap anyway), no backing disc at small sizes, and a cap
    // (shared with the forests). The deposits are stored ore by ore; they are visited in a scattered order (i * 7919 mod N, a permutation
    // since 7919 is prime and does not divide N) so the cap and the shared cells thin every ore alike.
    const float cell = std::max(8.0f, size * 0.9f);
    const int gridW = std::max(1, static_cast<int>((right - left) / cell) + 2);
    const int gridH = std::max(1, static_cast<int>((bottom - top) / cell) + 2);
    static std::vector<uint8_t> taken;
    taken.assign(static_cast<size_t>(gridW) * gridH, 0);
    const bool backing = size > 14.0f;
    int drawn = 0;
    static_assert(kOreDepositCount % 7919 != 0, "7919 must not divide the deposit count");
    // Three passes: the dense resource rows (thinned to one per cell, capped), then the sparse point rows (water,
    // beacons), then lore notes and bosses: never thinned or capped, drawn on top, so a barrel beside a boss can't
    // hide it.
    // The capped pass: the dense rows on screen, nearest the player first when there are more than the cap allows (the
    // creatures alone can be thousands), so what is around you is never the part thinned away; scattered order beyond
    // that, as before.
    static std::vector<std::pair<float, uint32_t>> closeDeps;
    closeDeps.clear();
    float px = 0.0f, pz = 0.0f;
    const bool havePlayer = LocalPlayerXZ(client, px, pz);
    for (size_t i = 0; i < kOreDepositCount; ++i) {
        const size_t d = (i * 7919) % kOreDepositCount;
        const OreDeposit& dep = kOreDeposits[d];
        if (!g_oreEnabled[dep.ore] || kOreAreaColors[dep.ore] != 0 || !IsResourceCategory(kOreCategory[dep.ore]) || CreatureOffWorldMap(dep.ore)) continue;
        const float sx = originX + dep.x * zoom, sy = originY + (mapWorldH - dep.z) * zoom;
        if (sx < left - size || sx > right + size || sy < top - size || sy > bottom + size) continue;
        const float dx = dep.x - px, dz = dep.z - pz;
        closeDeps.emplace_back(havePlayer ? dx * dx + dz * dz : 0.0f, static_cast<uint32_t>(d));
    }
    if (havePlayer && closeDeps.size() > static_cast<size_t>(maxIcons))
        std::stable_sort(closeDeps.begin(), closeDeps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int pass = 0; pass < 3; ++pass)
    for (size_t i = 0; i < (pass == 0 ? closeDeps.size() : kOreDepositCount) && (pass > 0 || drawn < maxIcons); ++i) {
        const size_t d = pass == 0 ? closeDeps[i].second : (i * 7919) % kOreDepositCount;
        const OreDeposit& dep = kOreDeposits[d];
        if (!g_oreEnabled[dep.ore] || kOreAreaColors[dep.ore] != 0 || CreatureOffWorldMap(dep.ore)) continue;
        const int depPass = IsResourceCategory(kOreCategory[dep.ore]) ? 0 : (IsKnowledgeDeposit(d) ? 2 : 1);
        if (depPass != pass) continue;
        const float sx = originX + dep.x * zoom + (pass == 2 ? StackSide(d) * size * 0.7f : 0.0f);
        const float sy = originY + (mapWorldH - dep.z) * zoom;
        if (sx < left - size || sx > right + size || sy < top - size || sy > bottom + size) continue;
        const bool unexplored = g_pinRevealMode != PinRevealMode::RevealAll &&
                                !IsPositionExploredByCharacter(client, dep.x, dep.z);
        if (unexplored && g_pinRevealMode == PinRevealMode::ExploredOnly) continue;
        // A lore note read / a boss defeated: the check mark; hidden or greyed like looted chests while it stays so (a
        // boss that came back keeps its check mark, not greyed)
        const bool checked = KnowledgeDone(d), done = checked && KnowledgeGrayed(d);
        if (done && g_lootedChestMode == 0) continue;
        const bool outside = OutsideFocusedRegion(dep.x, dep.z); // Map Key > Region
        if ((outside && g_regionOutsideHide) || halls::MapHidesAt(dep.x, dep.topY, dep.z, IsKnowledgeDeposit(d))) continue; // or another Hollow Hall
        if (pass == 0) {
            const int gx = MapThinCell(dep.x * zoom, left - originX, cell);
            const int gy = MapThinCell((mapWorldH - dep.z) * zoom, top - originY, cell);
            if (gx >= 0 && gy >= 0 && gx < gridW && gy < gridH) {
                uint8_t& t = taken[static_cast<size_t>(gy) * gridW + gx];
                if (t) continue;
                t = 1;
            }
        }
        ++drawn;
        // Under the menu banner along the top the game's own pins show through dimmed; so do these
        const bool underBanner = sy < top + kMapBannerBottom;
        const float alpha = (unexplored ? 0.55f : 1.0f) * (underBanner ? 0.22f : 1.0f) * (outside ? 0.3f : 1.0f);
        // A lore note or boss not done yet stands out: a third bigger, on an amber ring (they are few and far apart); in
        // unexplored fog (Pins: HYBRID) the ring is gray like the pin, so it still shows among the grayed chests
        // A boss stays easy to find once beaten: still a little bigger, on a gray ring, its icon gray but solid (a small
        // gray disc at 0.6 read as gone on the gray map: izisot's beaten Fell Critter Queen, Cyclops and Sicklescythe,
        // 2026-11-12); one that came back (checked, alive) stands out again like a new one
        const bool boss = pass == 2 && IsBossSpot(d);
        const bool standOut = pass == 2 && (!checked || (boss && !done));
        const bool bossDone = boss && done;
        const float iconSz = standOut ? size * 1.35f : bossDone ? size * 1.2f : size, iconHalf = iconSz * 0.5f;
        if (standOut || bossDone) {
            DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - iconHalf - 4.0f, sy - iconHalf - 4.0f, iconSz + 8.0f,
                             iconSz + 8.0f, (unexplored || bossDone) ? 0xff8c9296u : 0xff3cc8f8u, (bossDone ? 0.8f : 0.9f) * alpha);
        }
        if (backing || standOut || bossDone) {
            DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - iconHalf - 2.0f, sy - iconHalf - 2.0f, iconSz + 4.0f,
                             iconSz + 4.0f, 0xff0a0e10u, (standOut || bossDone ? 0.85f : 0.55f) * alpha);
        }
        DrawIconRectAbs(widgetRaw, DepositIcon(d), sx - iconHalf, sy - iconHalf, iconSz, iconSz,
                        (unexplored || done) ? 0xff8c9296u : 0xffffffffu, done ? alpha * (bossDone ? 0.9f : 0.6f) : alpha);
        if (checked) {
            const float checkSz = std::max(9.0f, size * 0.6f);
            DrawAssetRectAbs(widgetRaw, kCheckmarkPrimitive, sx + half - checkSz * 0.7f, sy + half - checkSz * 0.7f, checkSz,
                             checkSz, 0xFFFFFFFFu, 0.95f * alpha, 1.12f);
        }
        if (!underBanner && haveCursor && std::fabs(mx - sx) <= iconHalf + 2.0f && std::fabs(my - sy) <= iconHalf + 2.0f) {
            hovered = d;
            hoveredSx = sx;
            hoveredSy = sy;
        }
    }
    // Built water (built_water.inl), with the world's wells and barrels under Water Source
    const char* hoveredName = hovered >= kOreDepositCount ? nullptr
                              : IsKnowledgeDeposit(hovered)  ? KnowledgeSpotName(hovered)
                                                             : OreName(kOreDeposits[hovered].ore);
    if (g_oreEnabled[kWaterFirst]) {
        for (const auto& w : g_builtWater) { // a handful: never capped, like the other point rows
            const float sx = originX + w.x * zoom;
            const float sy = originY + (mapWorldH - w.z) * zoom;
            if (sx < left - size || sx > right + size || sy < top - size || sy > bottom + size) continue;
            const bool outside = OutsideFocusedRegion(w.x, w.z); // Map Key > Regions
            if ((outside && g_regionOutsideHide) || halls::MapHides(w.x, w.z)) continue;
            const bool underBanner = sy < top + kMapBannerBottom;
            const float alpha = underBanner ? 0.22f : outside ? 0.3f : 1.0f;
            ++drawn;
            if (backing) {
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - half - 2.0f, sy - half - 2.0f, size + 4.0f, size + 4.0f,
                                 0xff0a0e10u, 0.55f * alpha);
            }
            DrawIconRectAbs(widgetRaw, kOreIcons[kWaterFirst], sx - half, sy - half, size, size, 0xffffffffu, alpha);
            if (!underBanner && haveCursor && std::fabs(mx - sx) <= half + 2.0f && std::fabs(my - sy) <= half + 2.0f) {
                hoveredName = umm_lang::T(umm_lang::Str::WaterSource);
                hoveredSx = sx;
                hoveredSy = sy;
                g_hoveredDepositRow = OreHash(kWaterFirst);
            }
        }
    }
    if (hovered < kOreDepositCount && !g_hoveredDepositRow) g_hoveredDepositRow = OreHash(kOreDeposits[hovered].ore);
    // On a Hollow Hall's dungeon map a Return Beacon's numbered badge names it (halls::DrawMapBeacons)
    const bool beaconOnDungeonMap = halls::MapActive() && hovered < kOreDepositCount && kOreDeposits[hovered].ore == kBeaconsFirst;
    if (hoveredName && !beaconOnDungeonMap) NoteModHoverIcon(hoveredSx, hoveredSy), DrawMapHoverTitle(hoveredSx + half + 10.0f, hoveredSy, hoveredName);
}

// Wooden chests on the World Map, drawn by the mod: the game's two marker arrays the gold and silver chests go into
// hold 1024 each and are nearly full with them. One icon per screen cell (where they would overlap anyway), as the
// resources; looted ones hidden or greyed like the others, with the check mark once looted here; a name on hover.
// On the dungeon map the gold and silver chests too: its floor plan is drawn over the game's own markers. A hall has
// few enough chests to draw them all, so none is thinned there.
void DrawWoodChestsOnMap(void* widgetRaw, const float* rootRect, uint8_t* client, uint8_t* mapState, bool haveCursor,
                         float mx, float my) {
    if (!widgetRaw || !client || !mapState) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) + *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) + *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    const void* icon = GetIconRefForType(client, kWoodChestTypeHash);
    const bool dungeonMap = halls::MapActive();
    const void* goldIcon = dungeonMap ? GetIconRefForType(client, kGoldChestTypeHash) : nullptr;
    const void* silverIcon = dungeonMap ? GetIconRefForType(client, kSilverChestTypeHash) : nullptr;
    constexpr float kMapBannerBottom = 114.0f;
    const float size = std::max(11.0f, std::min(19.0f, 8.0f + zoom * 5.0f)), half = size * 0.5f;
    const float cell = std::max(8.0f, size * 0.9f);
    const int gridW = std::max(1, static_cast<int>((right - left) / cell) + 2);
    const int gridH = std::max(1, static_cast<int>((bottom - top) / cell) + 2);
    static std::vector<uint8_t> taken;
    taken.assign(static_cast<size_t>(gridW) * gridH, 0);
    const bool backing = size > 13.0f;
    size_t hovered = kTotalChestCount;
    float hoveredSx = 0.0f, hoveredSy = 0.0f;
    for (size_t c = dungeonMap ? 0 : kWoodChestFirst; c < kTotalChestCount; ++c) {
        const bool wood = IsWoodChest(c);
        if (!ChestRowEnabled(c) || (!wood && !dungeonMap)) continue;
        const void* const rowIcon = wood ? icon : IsGoldChest(c) ? goldIcon : silverIcon;
        if (!rowIcon) continue;
        const ChestSpawn& sp = kChestSpawns[c];
        const float x = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        const float z = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        const float sx = originX + x * zoom, sy = originY + (mapWorldH - z) * zoom;
        if (sx < left - size || sx > right + size || sy < top - size || sy > bottom + size) continue;
        const bool cleared = IsChestCurrentlyCleared(c);
        if (cleared && g_lootedChestMode == 0) continue;
        const bool unexplored = g_pinRevealMode != PinRevealMode::RevealAll &&
                                !IsPositionExploredByCharacter(client, sp.px1, sp.pz1);
        if (unexplored && g_pinRevealMode == PinRevealMode::ExploredOnly) continue;
        const bool outside = OutsideFocusedRegion(x, z); // Map Key > Region
        if ((outside && g_regionOutsideHide) || halls::MapHidesChest(c)) continue;
        const int gx = MapThinCell(x * zoom, left - originX, cell);
        const int gy = MapThinCell((mapWorldH - z) * zoom, top - originY, cell);
        if (!dungeonMap && gx >= 0 && gy >= 0 && gx < gridW && gy < gridH) {
            uint8_t& t = taken[static_cast<size_t>(gy) * gridW + gx];
            if (t) continue;
            t = 1;
        }
        const bool underBanner = sy < top + kMapBannerBottom;
        const bool dim = cleared || (unexplored && g_pinRevealMode == PinRevealMode::Hybrid);
        const float alpha = (dim ? 0.55f : 1.0f) * (underBanner ? 0.22f : 1.0f) * (outside ? 0.3f : 1.0f);
        if (backing) {
            DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - half - 2.0f, sy - half - 2.0f, size + 4.0f, size + 4.0f,
                             0xff0a0e10u, 0.55f * alpha);
        }
        const uint32_t tint = wood ? (dim ? kWoodChestDimTint : kWoodChestTint)
                              : IsGoldChest(c) ? (dim ? 0xff3a7f9au : 0xff3cc8ffu) // 0xAABBGGRR: gold
                                               : (dim ? 0xff7f7f7fu : 0xffd9d9d9u); // silver
        DrawIconRectAbs(widgetRaw, rowIcon, sx - half, sy - half, size, size, tint, alpha);
        if (cleared || g_chestLootStates[c].everLooted) {
            const float checkSz = std::max(8.0f, size * 0.6f);
            DrawAssetRectAbs(widgetRaw, kCheckmarkPrimitive, sx + half - checkSz * 0.7f, sy + half - checkSz * 0.7f,
                             checkSz, checkSz, 0xFFFFFFFFu, 0.95f * alpha, 1.12f);
        }
        if (!underBanner && haveCursor && std::fabs(mx - sx) <= half + 2.0f && std::fabs(my - sy) <= half + 2.0f) {
            hovered = c;
            hoveredSx = sx;
            hoveredSy = sy;
        }
    }
    if (hovered < kTotalChestCount) {
        if (!g_hoveredDepositRow) g_hoveredDepositRow = ChestTypeHash(hovered); // Alt + click hides the row
        NoteModHoverIcon(hoveredSx, hoveredSy);
        if (hovered >= kEndChestFirst) {
            DrawSpoilsHover(hovered, hoveredSx + half + 10.0f, hoveredSy);
        } else {
            const UiText& title = GetChestSubtypeTitle(kChestSpawns[hovered].subtype);
            DrawMapHoverTitle(hoveredSx + half + 10.0f, hoveredSy, title.utf8);
        }
    }
}

#include "town_portals.inl"

// What a marker row is called in the panel. Rows whose game name is exactly the row's meaning use
// the game's official translation while the mod follows the game's language; every other row (and
// a language set in the ini, which the game's text would not match) uses the mod's translation.
// a before b by name, ignoring case and accents, in the user's sort order (UTF-8 in)
bool NameLess(const char* a, const char* b) {
    wchar_t wa[128]{}, wb[128]{};
    MultiByteToWideChar(CP_UTF8, 0, a, -1, wa, 127);
    MultiByteToWideChar(CP_UTF8, 0, b, -1, wb, 127);
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, wa, -1, wb, -1,
                           nullptr, nullptr, 0) == CSTR_LESS_THAN;
}

const char* RowText(const MarkerRowSpec& row) {
    struct GameName {
        uint32_t typeHash;
        char text[96];
        int state; // 0 not tried, 1 have it, 2 none
    };
    static GameName names[] = {{0xa17072beu, {}, 0}, {0x4293eae5u, {}, 0}, {0x1095684eu, {}, 0}};
    if (IsOreHash(row.typeHash)) return OreName(OreIndexOf(row.typeHash));
    if (umm_lang::FollowsGame() && umm_lang::Current() != umm_lang::Lang::English) {
        for (auto& name : names) {
            if (name.typeHash != row.typeHash) continue;
            if (name.state == 0 && GetUiClient()) {
                name.state = ReadGameMarkerTypeName(name.typeHash, name.text, sizeof(name.text)) ? 1 : 2;
            }
            if (name.state == 1) return name.text;
        }
    }
    return umm_lang::T(row.text);
}

// The map key's widths for the current language. English keeps the values measured in game; other
// languages size each part from its text (umm_lang::TextWidth) and widen the panel to fit, up to
// kMaxPanelW, so a long German or Russian label does not run out of its button.
struct PanelLayout {
    float panelW = 470.0f; // room for the title, FIND, the cog and the fold button
    float findBtnW = 160.0f;
    float findTextW = 58.0f; // the "FIND" part of the find button
    bool showShortcut = true; // "Shift+F" inside the find button
    float fogLabelW = 66.0f; // "FOG:" / "PINS:" plus its gap; the buttons start after it
    float fogBtnW[3] = {122.0f, 94.0f, 138.0f};
};

void ApplyCustomMarkerScale() {
    g_customMarkerScaleIdx = std::max(0, std::min(kMarkerScaleCount - 1, g_customMarkerScaleIdx));
    // Custom Marker Size only scales the player's Custom Pins (kCustomPinHashes[0..9]),
    // keeping preloaded Gold & Silver Chests at their fixed sizes.
    g_goldChestMarkerType.iconDisplaySize = 23.0f;
    g_silverChestMarkerType.iconDisplaySize = 21.0f;
    g_woodChestMarkerType.iconDisplaySize = 19.0f;
}

// The map settings a filter preset carries with its filters (the World Map's look, not the HUD's): Fog, Pins,
// Custom Marker Size, Looted / Read / Beaten, Completed Places, Region Filter with its regions and Dim / Hide, Region
// Names, Level Areas and its tint
PresetSettings CurrentPresetSettings() {
    PresetSettings s;
    s.has = true;
    s.fog = static_cast<int>(g_fogRevealMode);
    s.pins = static_cast<int>(g_pinRevealMode);
    s.markerScale = g_customMarkerScaleIdx;
    s.looted = g_lootedChestMode;
    s.hideCompleted = g_hideCompletedPlaces;
    s.regionFilter = g_regionFilterEnabled;
    s.regionMask = g_regionMask & kAllRegionsMask;
    s.regionHide = g_regionOutsideHide;
    s.regionNames = g_regionNames;
    s.levelAreas = g_levelAreas;
    s.levelTint = g_levelTintPct;
    return s;
}

void ApplyPresetSettings(uint8_t* client, const PresetSettings& s) {
    if (!s.has) return; // a built-in preset: filters only
    const FogRevealMode fog = static_cast<FogRevealMode>(std::max(0, std::min(2, s.fog)));
    if (fog != g_fogRevealMode) ApplyFogRevealMode(fog);
    const auto pins = static_cast<PinRevealMode>(std::max(0, std::min(2, s.pins)));
    const int looted = s.looted ? 1 : 0;
    if (pins != g_pinRevealMode || looted != g_lootedChestMode) g_chestLootDirty = true;
    g_pinRevealMode = pins;
    g_lootedChestMode = looted;
    if (s.markerScale != g_customMarkerScaleIdx) {
        g_customMarkerScaleIdx = s.markerScale;
        ApplyCustomMarkerScale();
    }
    g_hideCompletedPlaces = s.hideCompleted;
    g_regionFilterEnabled = s.regionFilter;
    g_regionMask = s.regionMask & kAllRegionsMask;
    g_regionOutsideHide = s.regionHide;
    UpdateFocusedRegion(client);
    g_regionNames = s.regionNames;
    g_levelAreas = s.levelAreas;
    g_levelTintPct = std::max(10, std::min(80, s.levelTint / 10 * 10));
    SaveMinimapConfig();
}

// Draws a crisp vector gear/cog icon at (cx, cy) using engine primitives so it never relies on missing font glyphs.
void DrawGearIconAbs(void* widgetRaw, float cx, float cy, float r, uint32_t col, float alpha, uint32_t holeCol = 0xFF14181Bu) {
    const float toothW = std::max(2.0f, std::round(r * 0.36f));
    const float diagSpan = r * 0.72f;
    const float diagSz = std::max(2.2f, std::round(r * 0.38f));
    // 4 cardinal gear teeth (N, S, E, W)
    DrawSolidRectAbs(widgetRaw, cx - toothW * 0.5f, cy - r, toothW, r * 2.0f, col, alpha);
    DrawSolidRectAbs(widgetRaw, cx - r, cy - toothW * 0.5f, r * 2.0f, toothW, col, alpha);
    // 4 diagonal gear teeth (NE, NW, SE, SW)
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            DrawSolidRectAbs(widgetRaw,
                             cx + static_cast<float>(sx) * diagSpan - diagSz * 0.5f,
                             cy + static_cast<float>(sy) * diagSpan - diagSz * 0.5f,
                             diagSz, diagSz, col, alpha);
        }
    }
    // Gear body disc + center axle hole
    const float bodyR = r * 0.78f;
    const float holeR = r * 0.36f;
    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f, col, alpha);
    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - holeR, cy - holeR, holeR * 2.0f, holeR * 2.0f, holeCol, 0.98f);
    DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f, col, alpha);
}

const PanelLayout& CurrentPanelLayout();

constexpr float kSettingsCardH = 898.0f; // (four section headers: World Map, Minimap, Look, Filters)

float SettingsSegWidth() {
    using umm_lang::Str;
    using umm_lang::T;
    using umm_lang::TextWidth;
    float w0 = std::max(TextWidth(T(Str::SunMoon)), TextWidth(T(Str::Weather))) + 36.0f;
    float w1 = std::max({TextWidth(T(Str::Hide)), TextWidth(T(Str::GreyedOut)), TextWidth(T(Str::Show))}) + 36.0f;
    float w2 = std::max({TextWidth(T(Str::BorderOrnate)), TextWidth(T(Str::BorderThin)), TextWidth(T(Str::NorthUp)),
                         TextWidth(T(Str::HeadingUp))}) + 36.0f;
    float w3 = TextWidth(T(Str::RemoveAllLocks)) + 36.0f;
    return std::max(118.0f, std::max({w0, w1, w2, w3}));
}

float SettingsCardWidth() {
    using umm_lang::Str;
    using umm_lang::T;
    using umm_lang::TextWidth;
    const float segW = SettingsSegWidth();
    float maxLbl = std::max({
        196.0f,
        TextWidth(T(Str::PinSize)),
        TextWidth(T(Str::ShowMinimap)),
        TextWidth(T(Str::MinimapTerrain)),
        TextWidth(T(Str::ClockIcon)),
        TextWidth(T(Str::LootedChests)) * 1.6f, // drawn wider than the estimate; its two buttons share the row
        TextWidth(T(Str::MinimapBorder)),
        TextWidth(T(Str::MinimapRotation)),
        TextWidth(T(Str::MinimapInfoBar)),
        TextWidth(T(Str::CompletedPlaces)),
        TextWidth(T(Str::TimeFormat)),
        TextWidth(T(Str::ShowPosition)),
        TextWidth(T(Str::RegionFilter)),
        TextWidth(T(Str::LevelAreas)),
        TextWidth(T(Str::LevelTint)) + 14.0f, // indented under Level Areas
        TextWidth(T(Str::RegionNames)),
        TextWidth(T(Str::VanillaWorldMap)),
        TextWidth(T(Str::VanillaMinimap)) + 14.0f, // indented under Vanilla World Map
        TextWidth(T(Str::DungeonMaps)),
        TextWidth(T(Str::MapDesk)),
        TextWidth(T(Str::NearbyCreatures)),
        TextWidth(T(Str::HideOnWorldMap)) + 14.0f, // indented under Nearby Creatures Only
        TextWidth(T(Str::FilterLocks)) + 40.0f // " (12)"
    });
    return std::max(CurrentPanelLayout().panelW, std::min(640.0f, std::max(448.0f, 12.0f + maxLbl + 18.0f + 2.0f * segW + 4.0f + 12.0f)));
}

bool SetVanillaWorldMap(bool on); // below
void SaveMinimapConfig();
// Dungeon Maps on or off; off, an open dungeon map turns back into the overworld at once
void SetDungeonMaps(bool on) {
    g_dungeonMaps = on;
    if (!on) {
        halls::g_mapDungeon = -1;
        halls::g_mapOverworld = false;
    }
    SaveMinimapConfig();
    if (g_log) g_log("Dungeon Maps %s", on ? "on" : "off: the overworld map in halls, vaults and spires");
}

// Draws the Settings modal card (accessible from the [⚙] cog on both the World Map and HUD Minimap).
// Returns true if any setting was changed this frame.
bool DrawSettingsCardAbs(void* widgetRaw, float sx, float sy,
                         float mx, float my, bool haveCursor, bool clicked,
                         bool& ioOpenFlag, uint8_t* mapState = nullptr) {
    using namespace key_style;
    using umm_lang::Str;
    using umm_lang::T;

    const float cardW = SettingsCardWidth();
    const float segW = SettingsSegWidth();

    bool changed = false;
    auto isHov = [&](float rx, float ry, float rw, float rh) -> bool {
        PadTargetSeen(rx, ry, rw, rh);
        return haveCursor && mx >= rx && mx <= rx + rw && my >= ry && my <= ry + rh;
    };

    DrawFramedBoxAbs(widgetRaw, sx, sy, cardW, kSettingsCardH, kPanelFill, 0.97f, kGold, 0.96f, 8.0f);
    DrawBoxAbs(widgetRaw, kBoxInnerGlowPrimitive, sx + 1.0f, sy + 1.0f, cardW - 2.0f, kSettingsCardH - 2.0f,
               kGold, 0.25f, 10.0f);

    // Header bar: Gear icon + SETTINGS title + [X] close button
    constexpr float kHdrH = 28.0f;
    DrawGearIconAbs(widgetRaw, sx + 18.0f, sy + kHdrH * 0.5f, 6.5f, kTitle, 0.95f, kPanelFill);
    DrawLabelAbs(sx + 30.0f, sy, cardW - 72.0f, kHdrH,
                 T(Str::SettingsTitle), kTitle, 1.0f, TextAlignH::Left, TextAlignV::Center);

    const float closeX = sx + cardW - 34.0f, closeY = sy + 4.0f, closeW = 26.0f, closeH = 20.0f;
    bool hovClose = isHov(closeX, closeY, closeW, closeH);
    DrawKeyButtonAbs(widgetRaw, closeX, closeY, closeW, closeH, hovClose, false);
    DrawLabelAbs(closeX, closeY, closeW, closeH, "X", hovClose ? kActiveText : kText, 0.95f,
                 TextAlignH::Center, TextAlignV::Center);
    if (clicked && hovClose) {
        ioOpenFlag = false;
    }

    DrawSolidRectAbs(widgetRaw, sx + 8.0f, sy + kHdrH, cardW - 16.0f, 1.0f, kGoldDim, 0.65f);

    constexpr float kRowH = 30.0f;
    float ry = sy + kHdrH + 6.0f;
    const float ctrlRight = sx + cardW - 12.0f;

    const PanelLayout& lay = CurrentPanelLayout();
    (void)mapState; // was for Show All / Hide All, which moved to the Map Key

    const float actionY = ry - sy;

    // Fog of War / Map Reveal Mode Selector (EXPLORED | HYBRID | REVEAL ALL)
    const float fogY = actionY;
    const float fogBtnH = kRowH - 2.0f;
    DrawLabelAbs(sx + 10.0f, sy + fogY, lay.fogLabelW - 6.0f, fogBtnH,
        umm_lang::T(umm_lang::Str::Fog), kTextMuted, 0.95f, TextAlignH::Left, TextAlignV::Center);

    struct FogModeButton {
        FogRevealMode mode;
        umm_lang::Str label;
    };
    constexpr FogModeButton kFogBtns[3] = {
        { FogRevealMode::ExploredOnly, umm_lang::Str::FogExplored },
        { FogRevealMode::Hybrid,       umm_lang::Str::FogHybrid },
        { FogRevealMode::RevealAll,    umm_lang::Str::FogRevealAll },
    };
    const float fogBtnStartX = 10.0f + lay.fogLabelW;
    const float fogBtnGap = 4.0f;
    float fbx = fogBtnStartX;
    for (int fm = 0; fm < 3; ++fm) {
        const float fogBtnW = lay.fogBtnW[fm];
        bool active = (g_fogRevealMode == kFogBtns[fm].mode);
        bool hovFm = isHov(sx + fbx, sy + fogY, fogBtnW, fogBtnH);
        DrawKeyButtonAbs(widgetRaw, sx + fbx, sy + fogY, fogBtnW, fogBtnH, hovFm, active);
        uint32_t txtCol = active ? kActiveText : (hovFm ? kText : kTextMuted);
        DrawLabelAbs(sx + fbx, sy + fogY, fogBtnW, fogBtnH - 1.0f,
            umm_lang::T(kFogBtns[fm].label), txtCol, active ? 1.0f : 0.85f,
            TextAlignH::Center, TextAlignV::Center);
        if (clicked && hovFm) {
            ApplyFogRevealMode(kFogBtns[fm].mode);
            SaveMinimapConfig(); // FogRevealMode: kept the next time you play
            changed = true;
        }
        fbx += fogBtnW + fogBtnGap;
    }

    // Pin Reveal Mode Selector (PINS: EXPLORED | HYBRID | REVEAL ALL)
    const float pinsY = fogY + kRowH + 2.0f;
    const float pinsBtnH = kRowH - 2.0f;
    DrawLabelAbs(sx + 10.0f, sy + pinsY, lay.fogLabelW - 6.0f, pinsBtnH,
        umm_lang::T(umm_lang::Str::Pins), kTextMuted, 0.95f, TextAlignH::Left, TextAlignV::Center);

    struct PinModeButton {
        PinRevealMode mode;
        umm_lang::Str label;
    };
    constexpr PinModeButton kPinBtns[3] = {
        { PinRevealMode::ExploredOnly, umm_lang::Str::FogExplored },
        { PinRevealMode::Hybrid,       umm_lang::Str::FogHybrid },
        { PinRevealMode::RevealAll,    umm_lang::Str::FogRevealAll },
    };
    float pbx = fogBtnStartX;
    for (int pm = 0; pm < 3; ++pm) {
        const float pinBtnW = lay.fogBtnW[pm];
        bool active = (g_pinRevealMode == kPinBtns[pm].mode);
        bool hovPm = isHov(sx + pbx, sy + pinsY, pinBtnW, pinsBtnH);
        DrawKeyButtonAbs(widgetRaw, sx + pbx, sy + pinsY, pinBtnW, pinsBtnH, hovPm, active);
        uint32_t txtCol = active ? kActiveText : (hovPm ? kText : kTextMuted);
        DrawLabelAbs(sx + pbx, sy + pinsY, pinBtnW, pinsBtnH - 1.0f,
            umm_lang::T(kPinBtns[pm].label), txtCol, active ? 1.0f : 0.85f,
            TextAlignH::Center, TextAlignV::Center);
        if (clicked && hovPm && g_pinRevealMode != kPinBtns[pm].mode) {
            g_pinRevealMode = kPinBtns[pm].mode;
            g_chestLootDirty = true;
            SaveMinimapConfig();
            changed = true;
        }
        pbx += pinBtnW + fogBtnGap;
    }

    ry += 2.0f * kRowH + 6.0f; // the FOG and PINS rows

    // Section: the World Map
    {
        DrawSolidRectAbs(widgetRaw, sx + 8.0f, ry + 4.0f, cardW - 16.0f, 1.0f, kGoldDim, 0.65f);
        DrawLabelAbs(sx + 12.0f, ry + 4.0f, cardW - 24.0f, kRowH - 6.0f, T(Str::SecMap), kTitle, 0.9f,
                     TextAlignH::Left, TextAlignV::Center);
        ry += kRowH - 2.0f;
    }

    // Row 1: Custom Marker Size: [-] [1.0x] [+]
    {
        const float stepBtnW = 26.0f, stepBtnH = 22.0f, badgeW = 66.0f, gap = 4.0f;
        const float plusX = ctrlRight - stepBtnW;
        const float badgeX = plusX - gap - badgeW;
        const float minusX = badgeX - gap - stepBtnW;
        const float by = ry + (kRowH - stepBtnH) * 0.5f;

        DrawLabelAbs(sx + 12.0f, ry, minusX - (sx + 12.0f) - 10.0f, kRowH, T(Str::PinSize), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);

        bool hovMinus = isHov(minusX, by, stepBtnW, stepBtnH);
        bool hovPlus  = isHov(plusX, by, stepBtnW, stepBtnH);
        DrawKeyButtonAbs(widgetRaw, minusX, by, stepBtnW, stepBtnH, hovMinus, false);
        DrawLabelAbs(minusX, by, stepBtnW, stepBtnH, "-",
                     (g_customMarkerScaleIdx > 0) ? (hovMinus ? kActiveText : kText) : kOffText, 0.95f,
                     TextAlignH::Center, TextAlignV::Center);

        DrawFramedBoxAbs(widgetRaw, badgeX, by, badgeW, stepBtnH, kCategoryFill, 0.95f, kButtonFrame, 0.9f, 3.0f);
        DrawLabelAbs(badgeX, by, badgeW, stepBtnH, kMarkerScaleLabels[g_customMarkerScaleIdx],
                     kActiveText, 0.98f, TextAlignH::Center, TextAlignV::Center);

        DrawKeyButtonAbs(widgetRaw, plusX, by, stepBtnW, stepBtnH, hovPlus, false);
        DrawLabelAbs(plusX, by, stepBtnW, stepBtnH, "+",
                     (g_customMarkerScaleIdx + 1 < kMarkerScaleCount) ? (hovPlus ? kActiveText : kText) : kOffText, 0.95f,
                     TextAlignH::Center, TextAlignV::Center);

        if (clicked && hovMinus && g_customMarkerScaleIdx > 0) {
            --g_customMarkerScaleIdx;
            ApplyCustomMarkerScale();
            SaveMinimapConfig();
            changed = true;
        } else if (clicked && hovPlus && g_customMarkerScaleIdx + 1 < kMarkerScaleCount) {
            ++g_customMarkerScaleIdx;
            ApplyCustomMarkerScale();
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 5: Looted / Read / Beaten: [Hide] | [Greyed Out]
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;

        // A long name (the card's width is capped) is drawn a little smaller instead of running under the buttons
        const float labelW = btn0X - (sx + 12.0f) - 10.0f;
        const float textW = umm_lang::TextWidth(T(Str::LootedChests)) * 1.6f; // the estimate, in this font
        const float scale = textW > labelW ? std::max(0.7f, labelW / textW) : 1.0f;
        DrawLabelAbs(sx + 12.0f, ry, labelW, kRowH, T(Str::LootedChests), 0xffd6e6eeu, 0.96f, TextAlignH::Left,
                     TextAlignV::Center, nullptr, scale);

        bool hov0 = isHov(btn0X, by, segW, segH);
        bool hov1 = isHov(btn1X, by, segW, segH);
        bool act0 = (g_lootedChestMode == 0);
        bool act1 = (g_lootedChestMode == 1);

        DrawKeyButtonAbs(widgetRaw, btn0X, by, segW, segH, hov0, act0);
        DrawLabelAbs(btn0X, by, segW, segH, T(Str::Hide), act0 ? kActiveText : (hov0 ? kText : kTextMuted),
                     act0 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);

        DrawKeyButtonAbs(widgetRaw, btn1X, by, segW, segH, hov1, act1);
        DrawLabelAbs(btn1X, by, segW, segH, T(Str::GreyedOut), act1 ? kActiveText : (hov1 ? kText : kTextMuted),
                     act1 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);

        if (clicked && hov0 && g_lootedChestMode != 0) {
            g_lootedChestMode = 0;
            g_chestLootDirty = true;
            SaveMinimapConfig();
            changed = true;
        } else if (clicked && hov1 && g_lootedChestMode != 1) {
            g_lootedChestMode = 1;
            g_chestLootDirty = true;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 6: Completed Places: [Show] | [Hide] (places the game shows with a check mark)
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;
        DrawLabelAbs(sx + 12.0f, ry, btn0X - (sx + 12.0f) - 10.0f, kRowH, T(Str::CompletedPlaces), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        const bool hov0 = isHov(btn0X, by, segW, segH), hov1 = isHov(btn1X, by, segW, segH);
        const bool act0 = !g_hideCompletedPlaces, act1 = g_hideCompletedPlaces;
        DrawKeyButtonAbs(widgetRaw, btn0X, by, segW, segH, hov0, act0);
        DrawLabelAbs(btn0X, by, segW, segH, T(Str::Show), act0 ? kActiveText : (hov0 ? kText : kTextMuted),
                     act0 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        DrawKeyButtonAbs(widgetRaw, btn1X, by, segW, segH, hov1, act1);
        DrawLabelAbs(btn1X, by, segW, segH, T(Str::Hide), act1 ? kActiveText : (hov1 ? kText : kTextMuted),
                     act1 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        if (clicked && (hov0 || hov1) && g_hideCompletedPlaces != hov1) {
            g_hideCompletedPlaces = hov1;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row A3: Region Names (the regions' names on the World Map) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::RegionNames), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_regionNames);
        if (clicked && hovRow) {
            g_regionNames = !g_regionNames;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row A3b: Level Areas (the World Map in the areas of the enemies' levels) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::LevelAreas), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_levelAreas);
        if (clicked && hovRow) {
            g_levelAreas = !g_levelAreas;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row A3c: Tint Strength, under Level Areas and only with it: [-] [50%] [+] in steps of 10 %
    {
        const bool usable = g_levelAreas;
        const float stepBtnW = 26.0f, stepBtnH = 22.0f, badgeW = 66.0f, gap = 4.0f;
        const float plusX = ctrlRight - stepBtnW;
        const float badgeX = plusX - gap - badgeW;
        const float minusX = badgeX - gap - stepBtnW;
        const float by = ry + (kRowH - stepBtnH) * 0.5f;
        DrawLabelAbs(sx + 26.0f, ry, minusX - (sx + 26.0f) - 10.0f, kRowH, T(Str::LevelTint),
                     usable ? 0xffd6e6eeu : kOffText, usable ? 0.96f : 0.6f, TextAlignH::Left, TextAlignV::Center);
        const bool canLess = usable && g_levelTintPct > 10, canMore = usable && g_levelTintPct < 80;
        const bool hovMinus = usable && isHov(minusX, by, stepBtnW, stepBtnH);
        const bool hovPlus = usable && isHov(plusX, by, stepBtnW, stepBtnH);
        DrawKeyButtonAbs(widgetRaw, minusX, by, stepBtnW, stepBtnH, hovMinus, false);
        DrawLabelAbs(minusX, by, stepBtnW, stepBtnH, "-", canLess ? (hovMinus ? kActiveText : kText) : kOffText, 0.95f,
                     TextAlignH::Center, TextAlignV::Center);
        char pct[8];
        std::snprintf(pct, sizeof(pct), "%d%%", g_levelTintPct);
        DrawFramedBoxAbs(widgetRaw, badgeX, by, badgeW, stepBtnH, kCategoryFill, 0.95f, kButtonFrame, 0.9f, 3.0f);
        DrawLabelAbs(badgeX, by, badgeW, stepBtnH, pct, usable ? kActiveText : kOffText, 0.98f, TextAlignH::Center,
                     TextAlignV::Center);
        DrawKeyButtonAbs(widgetRaw, plusX, by, stepBtnW, stepBtnH, hovPlus, false);
        DrawLabelAbs(plusX, by, stepBtnW, stepBtnH, "+", canMore ? (hovPlus ? kActiveText : kText) : kOffText, 0.95f,
                     TextAlignH::Center, TextAlignV::Center);
        if (clicked && ((hovMinus && canLess) || (hovPlus && canMore))) {
            g_levelTintPct += hovPlus ? 10 : -10;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Section: the minimap
    {
        DrawSolidRectAbs(widgetRaw, sx + 8.0f, ry + 4.0f, cardW - 16.0f, 1.0f, kGoldDim, 0.65f);
        DrawLabelAbs(sx + 12.0f, ry + 4.0f, cardW - 24.0f, kRowH - 6.0f, T(Str::SecMinimap), kTitle, 0.9f,
                     TextAlignH::Left, TextAlignV::Center);
        ry += kRowH - 2.0f;
    }

    // Row 2: Show Minimap [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::ShowMinimap), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_minimapVisible);
        // With Vanilla World Map on, the minimap's cog is the only way to Settings: it stays on
        if (clicked && hovRow && !(g_vanillaWorldMap && g_minimapVisible)) {
            g_minimapVisible = !g_minimapVisible;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 2b: Minimap Info Bar [ON / OFF], next to Show Minimap (with the minimap off it stays alone)
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::MinimapInfoBar), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_minimapInfoBar);
        if (clicked && hovRow) {
            g_minimapInfoBar = !g_minimapInfoBar;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 3: Minimap Terrain [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::MinimapTerrain), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_minimapShowTerrain);
        if (clicked && hovRow) {
            g_minimapShowTerrain = !g_minimapShowTerrain;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 4a: Minimap Border: [Ornate] | [Thin]
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;
        DrawLabelAbs(sx + 12.0f, ry, btn0X - (sx + 12.0f) - 10.0f, kRowH, T(Str::MinimapBorder), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        const bool hov0 = isHov(btn0X, by, segW, segH), hov1 = isHov(btn1X, by, segW, segH);
        const bool act0 = !g_minimapThinBorder, act1 = g_minimapThinBorder;
        DrawKeyButtonAbs(widgetRaw, btn0X, by, segW, segH, hov0, act0);
        DrawLabelAbs(btn0X, by, segW, segH, T(Str::BorderOrnate), act0 ? kActiveText : (hov0 ? kText : kTextMuted),
                     act0 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        DrawKeyButtonAbs(widgetRaw, btn1X, by, segW, segH, hov1, act1);
        DrawLabelAbs(btn1X, by, segW, segH, T(Str::BorderThin), act1 ? kActiveText : (hov1 ? kText : kTextMuted),
                     act1 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        if (clicked && (hov0 || hov1) && g_minimapThinBorder != hov1) {
            g_minimapThinBorder = hov1;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 4a2: Minimap Rotation: [North Up] | [Facing Up] (the map turns round you, your arrow always up)
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;
        DrawLabelAbs(sx + 12.0f, ry, btn0X - (sx + 12.0f) - 10.0f, kRowH, T(Str::MinimapRotation), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        const bool hov0 = isHov(btn0X, by, segW, segH), hov1 = isHov(btn1X, by, segW, segH);
        const bool act0 = !g_minimapHeadingUp, act1 = g_minimapHeadingUp;
        DrawKeyButtonAbs(widgetRaw, btn0X, by, segW, segH, hov0, act0);
        DrawLabelAbs(btn0X, by, segW, segH, T(Str::NorthUp), act0 ? kActiveText : (hov0 ? kText : kTextMuted),
                     act0 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        DrawKeyButtonAbs(widgetRaw, btn1X, by, segW, segH, hov1, act1);
        DrawLabelAbs(btn1X, by, segW, segH, T(Str::HeadingUp), act1 ? kActiveText : (hov1 ? kText : kTextMuted),
                     act1 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        if (clicked && (hov0 || hov1) && g_minimapHeadingUp != hov1) {
            g_minimapHeadingUp = hov1;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 4: Minimap Icon: [Sun / Moon] | [Weather]
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;

        DrawLabelAbs(sx + 12.0f, ry, btn0X - (sx + 12.0f) - 10.0f, kRowH, T(Str::ClockIcon), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);

        bool hov0 = isHov(btn0X, by, segW, segH);
        bool hov1 = isHov(btn1X, by, segW, segH);
        bool act0 = !g_minimapClockWeatherMode;
        bool act1 = g_minimapClockWeatherMode;

        DrawKeyButtonAbs(widgetRaw, btn0X, by, segW, segH, hov0, act0);
        DrawLabelAbs(btn0X, by, segW, segH, T(Str::SunMoon), act0 ? kActiveText : (hov0 ? kText : kTextMuted),
                     act0 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);

        DrawKeyButtonAbs(widgetRaw, btn1X, by, segW, segH, hov1, act1);
        DrawLabelAbs(btn1X, by, segW, segH, T(Str::Weather), act1 ? kActiveText : (hov1 ? kText : kTextMuted),
                     act1 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);

        if (clicked && hov0 && g_minimapClockWeatherMode) {
            g_minimapClockWeatherMode = false;
            SaveMinimapConfig();
            changed = true;
        } else if (clicked && hov1 && !g_minimapClockWeatherMode) {
            g_minimapClockWeatherMode = true;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 4b: Time Format: [1:00 PM] | [13:00] (the minimap's clock; by language until one is picked)
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;
        DrawLabelAbs(sx + 12.0f, ry, btn0X - (sx + 12.0f) - 10.0f, kRowH, T(Str::TimeFormat), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        const bool hov0 = isHov(btn0X, by, segW, segH), hov1 = isHov(btn1X, by, segW, segH);
        const bool act0 = umm_lang::Uses12HourClock(), act1 = !act0;
        DrawKeyButtonAbs(widgetRaw, btn0X, by, segW, segH, hov0, act0);
        DrawLabelAbs(btn0X, by, segW, segH, "1:00 PM", act0 ? kActiveText : (hov0 ? kText : kTextMuted),
                     act0 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        DrawKeyButtonAbs(widgetRaw, btn1X, by, segW, segH, hov1, act1);
        DrawLabelAbs(btn1X, by, segW, segH, "13:00", act1 ? kActiveText : (hov1 ? kText : kTextMuted),
                     act1 ? 1.0f : 0.85f, TextAlignH::Center, TextAlignV::Center);
        if (clicked && (hov0 || hov1) && g_clockFormat != (hov1 ? 1 : 0)) {
            g_clockFormat = hov1 ? 1 : 0;
            umm_lang::SetClockFormat(g_clockFormat);
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row A1: Position (X, Y, Z) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::ShowPosition), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_minimapShowPosition);
        if (clicked && hovRow) {
            g_minimapShowPosition = !g_minimapShowPosition;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row A8: Nearby Creatures Only (the creature rows' spawn points off the maps; the live creatures near you on the
    // minimap, creature_live.inl) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::NearbyCreatures), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_nearCreatures);
        if (clicked && hovRow) {
            g_nearCreatures = !g_nearCreatures;
            SaveMinimapConfig();
            if (g_log) g_log("Nearby Creatures Only %s", g_nearCreatures ? "on" : "off");
            changed = true;
        }
        ry += kRowH;
    }

    // Row A8b: Hide on World Map, under Nearby Creatures Only and only with it: the creature spawn points off the World
    // Map too
    {
        const bool usable = g_nearCreatures;
        bool hovRow = usable && isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 26.0f, ry, cardW - 50.0f - OnOffBadgeWidth(), kRowH, T(Str::HideOnWorldMap),
                     usable ? 0xffd6e6eeu : kOffText, usable ? 0.96f : 0.6f, TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, usable && g_nearCreaturesHideMap);
        if (clicked && hovRow) {
            g_nearCreaturesHideMap = !g_nearCreaturesHideMap;
            SaveMinimapConfig();
            if (g_log) g_log("Nearby Creatures: hide on World Map %s", g_nearCreaturesHideMap ? "on" : "off");
            changed = true;
        }
        ry += kRowH;
    }

    // Section: how the maps look
    {
        DrawSolidRectAbs(widgetRaw, sx + 8.0f, ry + 4.0f, cardW - 16.0f, 1.0f, kGoldDim, 0.65f);
        DrawLabelAbs(sx + 12.0f, ry + 4.0f, cardW - 24.0f, kRowH - 6.0f, T(Str::SecLook), kTitle, 0.9f,
                     TextAlignH::Left, TextAlignV::Center);
        ry += kRowH - 2.0f;
    }

    // Row A7: Map Desk (the World Map lies on a cartographer's desk, desk.inl) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::MapDesk), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_mapDesk);
        if (clicked && hovRow) {
            g_mapDesk = !g_mapDesk;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row A6: Dungeon Maps (a Hollow Hall's, vault's or spire's own map on the World Map and minimap) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::DungeonMaps), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_dungeonMaps);
        if (clicked && hovRow) {
            SetDungeonMaps(!g_dungeonMaps);
            changed = true;
        }
        ry += kRowH;
    }

    // Row A4: Vanilla World Map (the World Map as the game draws it; the minimap stays) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::VanillaWorldMap), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_vanillaWorldMap);
        // (with a controller, View on the game's map brings the mod's back)
        if (clicked && hovRow && SetVanillaWorldMap(!g_vanillaWorldMap)) changed = true;
        ry += kRowH;
    }

    // Row A5: Vanilla Minimap, under Vanilla World Map and only with it: the minimap with the game's markers only
    {
        const bool usable = g_vanillaWorldMap;
        bool hovRow = usable && isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 26.0f, ry, cardW - 50.0f - OnOffBadgeWidth(), kRowH, T(Str::VanillaMinimap),
                     usable ? 0xffd6e6eeu : kOffText, usable ? 0.96f : 0.6f, TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, usable && g_vanillaMinimap);
        if (clicked && hovRow) {
            g_vanillaMinimap = !g_vanillaMinimap;
            SaveMinimapConfig();
            if (g_log) g_log("Vanilla Minimap %s", g_vanillaMinimap ? "on: the game's markers only" : "off");
            changed = true;
        }
        ry += kRowH;
    }

    // Section: the Map Key's filters
    {
        DrawSolidRectAbs(widgetRaw, sx + 8.0f, ry + 4.0f, cardW - 16.0f, 1.0f, kGoldDim, 0.65f);
        DrawLabelAbs(sx + 12.0f, ry + 4.0f, cardW - 24.0f, kRowH - 6.0f, T(Str::SecFilters), kTitle, 0.9f,
                     TextAlignH::Left, TextAlignV::Center);
        ry += kRowH - 2.0f;
    }

    // Row A2: Region Filter (the Map Key's Region row) [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::RegionFilter), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_regionFilterEnabled);
        if (clicked && hovRow) {
            g_regionFilterEnabled = !g_regionFilterEnabled;
            SaveMinimapConfig();
            changed = true;
        }
        ry += kRowH;
    }

    // Row 7: Filter Locks (n): [Remove All]
    {
        const float segH = 22.0f;
        const float btnX = ctrlRight - segW;
        const float by = ry + (kRowH - segH) * 0.5f;
        const size_t lockCount = FilterLockCount();
        char label[128]{};
        std::snprintf(label, sizeof(label), "%s (%zu)", T(Str::FilterLocks), lockCount);
        DrawLabelAbs(sx + 12.0f, ry, btnX - (sx + 12.0f) - 10.0f, kRowH, label, 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        const bool any = lockCount > 0;
        const bool hov = any && isHov(btnX, by, segW, segH);
        DrawKeyButtonAbs(widgetRaw, btnX, by, segW, segH, hov, false);
        DrawLabelAbs(btnX, by, segW, segH, T(Str::RemoveAllLocks), any ? (hov ? kActiveText : kText) : kOffText,
                     any ? 0.95f : 0.7f, TextAlignH::Center, TextAlignV::Center);
        if (clicked && hov) {
            ClearFilterLocks();
            changed = true;
        }
        ry += kRowH;
    }

    return changed;
}

const PanelLayout& CurrentPanelLayout() {
    using umm_lang::Str;
    using umm_lang::TextWidth;
    using umm_lang::T;
    static PanelLayout layout;
    static int builtFor = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (builtFor == lang) return layout;
    builtFor = lang;
    layout = PanelLayout{};
    if (umm_lang::Current() == umm_lang::Lang::English) return layout;

    constexpr float kMinPanelW = 470.0f;
    constexpr float kMaxPanelW = 600.0f;
    constexpr float kShortcutW = 84.0f; // "Shift+F"
    const float badgeW = OnOffBadgeWidth();

    layout.findTextW = TextWidth(T(Str::Find)) + 10.0f;
    layout.findBtnW = 10.0f + layout.findTextW + 10.0f + kShortcutW + 10.0f;
    const float titleW = TextWidth(T(Str::MapKeyTitle));
    float need = 12.0f + titleW + 10.0f + layout.findBtnW + 4.0f + 28.0f + 4.0f + 32.0f + 8.0f;

    const float showHideW = std::max(TextWidth(T(Str::ShowAll)), TextWidth(T(Str::HideAll))) + 28.0f;
    need = std::max(need, 2.0f * showHideW + 22.0f);

    layout.fogLabelW = std::max(TextWidth(T(Str::Fog)), TextWidth(T(Str::Pins))) + 18.0f;
    const Str fogStr[3] = {Str::FogExplored, Str::FogHybrid, Str::FogRevealAll};
    float fogW = 10.0f + layout.fogLabelW;
    for (int i = 0; i < 3; ++i) {
        layout.fogBtnW[i] = TextWidth(T(fogStr[i])) + 18.0f;
        fogW += layout.fogBtnW[i] + 4.0f;
    }
    need = std::max(need, fogW + 4.0f);

    for (const auto& grp : kCategoryGroups) {
        need = std::max(need, 36.0f + TextWidth(T(grp.title)) + badgeW + 30.0f);
        for (size_t r = 0; r < grp.rowCount; ++r) {
            // the row's label plus a count like " (1032)"
            need = std::max(need, 52.0f + TextWidth(RowText(grp.rows[r])) + 55.0f + badgeW + 22.0f);
        }
    }
    layout.panelW = std::min(kMaxPanelW, std::max(kMinPanelW, need));

    // Still too narrow for the title and the whole find button: drop "Shift+F" from the button.
    if (12.0f + titleW + 10.0f + layout.findBtnW + 76.0f > layout.panelW) {
        layout.showShortcut = false;
        layout.findBtnW = layout.findTextW + 20.0f;
    }
    // The fog/pins buttons share any room the panel cannot give them.
    const float fogRoom = layout.panelW - 8.0f - (10.0f + layout.fogLabelW) - 8.0f;
    const float fogSum = layout.fogBtnW[0] + layout.fogBtnW[1] + layout.fogBtnW[2];
    if (fogSum > fogRoom) {
        for (float& w : layout.fogBtnW) w *= fogRoom / fogSum;
    }
    return layout;
}

// World Map zoom steps. The map view keeps its steps in a float[5] at mapView+0x40, the count at +0x54 and the
// current step's index at **(mapView+0x258); the zoom is step * mapView+0x58 (0.2). The game fills in 16, 8, 4, 2
// (it leaves out its 0.7, which would make the map narrower than the screen), and a sixth step has no room. So the
// mod keeps the whole list and shows the view five of them at a time: when the view reaches the window's last (or
// first) step and there are more, the window slides one along and the index moves back one, which leaves the
// current zoom unchanged; the next wheel turn goes on to the next step. Every step is 2x like vanilla, down to a
// last one where the whole world fits between the menu banner and the key hints.
// Two closer steps first (64, 32) only for a Hollow Hall's dungeon map, whose HD map has the detail for them; the
// overworld map starts at the game's own closest (16, kOverworldFirstStep).
constexpr float kMapZoomSteps[] = {64.0f, 32.0f, 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.42f};
constexpr int kMapZoomStepCount = static_cast<int>(sizeof(kMapZoomSteps) / sizeof(kMapZoomSteps[0]));
constexpr int kMapZoomWindow = 5;
constexpr int kOverworldFirstStep = 2;
int g_mapZoomWindowStart = kOverworldFirstStep;

// Zooms the World Map to the closest step that fits w x h metres into areaW x areaH (UI units); the window of steps
// slides along when that step is outside it
void FitMapZoom(uint8_t* mapState, float w, float h, float areaW, float areaH) {
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView || mapView[0x54] != kMapZoomWindow || w <= 0.0f || h <= 0.0f) return;
    uint64_t* idx = *reinterpret_cast<uint64_t**>(mapView + 0x258);
    const float scale = *reinterpret_cast<float*>(mapView + 0x58);
    if (!idx || !(scale > 0.0f)) return;
    const float fit = std::min(areaW / w, areaH / h) * 0.92f;
    int j = 0;
    while (j + 1 < kMapZoomStepCount && kMapZoomSteps[j] * scale > fit) ++j;
    if (j < g_mapZoomWindowStart) g_mapZoomWindowStart = j;
    if (j > g_mapZoomWindowStart + kMapZoomWindow - 1) g_mapZoomWindowStart = j - kMapZoomWindow + 1;
    std::memcpy(mapView + 0x40, kMapZoomSteps + g_mapZoomWindowStart, kMapZoomWindow * sizeof(float));
    *idx = static_cast<uint64_t>(j - g_mapZoomWindowStart);
    *reinterpret_cast<float*>(mapView + 0x10) = kMapZoomSteps[j] * scale;
    halls::g_mapFitStep = j;
}

void ApplyMapZoomSteps(uint8_t* mapState) {
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView) return;
    auto* steps = reinterpret_cast<float*>(mapView + 0x40);
    uint64_t* idx = *reinterpret_cast<uint64_t**>(mapView + 0x258);
    if (!idx) return;
    auto windowMatches = [&](int start) {
        for (int i = 0; i < kMapZoomWindow; ++i) {
            if (steps[i] != kMapZoomSteps[start + i]) return false;
        }
        return true;
    };
    constexpr int g0 = kOverworldFirstStep;
    if (mapView[0x54] == 4 && steps[0] == kMapZoomSteps[g0] && steps[3] == kMapZoomSteps[g0 + 3]) {
        // The game's own steps (a newly opened map): start at the window that shares its first four
        g_mapZoomWindowStart = g0;
        steps[4] = kMapZoomSteps[g0 + 4];
        mapView[0x54] = kMapZoomWindow;
    } else if (mapView[0x54] == kMapZoomWindow && steps[0] == kMapZoomSteps[g0] && steps[3] == kMapZoomSteps[g0 + 3] &&
               steps[4] < kMapZoomSteps[g0 + 3] && !windowMatches(g0)) {
        // An earlier build's single extra step after 2: take the view over at that window
        g_mapZoomWindowStart = g0;
        steps[4] = kMapZoomSteps[g0 + 4];
    } else if (mapView[0x54] != kMapZoomWindow) {
        return; // not the steps we know
    } else if (!windowMatches(g_mapZoomWindowStart)) {
        // Another generation slid the window (a hot reload): find where it is
        int found = -1;
        for (int start = 0; start + kMapZoomWindow <= kMapZoomStepCount && found < 0; ++start) {
            if (windowMatches(start)) found = start;
        }
        if (found < 0) return;
        g_mapZoomWindowStart = found;
    }
    // The dungeon map zooms out no further than the whole dungeon
    if (halls::MapActive() && halls::g_mapFitStep >= 0 && g_mapZoomWindowStart + static_cast<int>(*idx) > halls::g_mapFitStep) {
        if (halls::g_mapFitStep < g_mapZoomWindowStart) {
            g_mapZoomWindowStart = halls::g_mapFitStep;
            std::memcpy(steps, kMapZoomSteps + g_mapZoomWindowStart, kMapZoomWindow * sizeof(float));
        }
        *idx = static_cast<uint64_t>(halls::g_mapFitStep - g_mapZoomWindowStart);
        return;
    }
    // The overworld map zooms in no closer than the game's own closest step
    const int firstStep = halls::MapActive() ? 0 : kOverworldFirstStep;
    if (g_mapZoomWindowStart + static_cast<int>(*idx) < firstStep) {
        if (g_mapZoomWindowStart < firstStep) {
            g_mapZoomWindowStart = firstStep;
            std::memcpy(steps, kMapZoomSteps + g_mapZoomWindowStart, kMapZoomWindow * sizeof(float));
        }
        *idx = 0;
        return;
    }
    const uint64_t i = *idx;
    if (i == kMapZoomWindow - 1 && g_mapZoomWindowStart + kMapZoomWindow < kMapZoomStepCount) {
        ++g_mapZoomWindowStart;
        *idx = i - 1;
    } else if (i == 0 && g_mapZoomWindowStart > firstStep) {
        --g_mapZoomWindowStart;
        *idx = 1;
    } else {
        return;
    }
    std::memcpy(steps, kMapZoomSteps + g_mapZoomWindowStart, kMapZoomWindow * sizeof(float));
}

void DrawDungeonMapPlayers(void* widget, const float* rootRect, uint8_t* client, uint8_t* mapState); // below

// The dungeon map is put on its hall's box when it opens, at each zoom step and once when a drag ends (never during a
// drag or the zoom's animation: holding the view fought them): when the whole box fits on the screen, in the middle;
// zoomed in closer, within the box's edges. The map area is the screen between the menu banner and the key hints.
// Moved through the same re-centring as the zoom steps above.
void KeepDungeonInView(uint8_t* mapState, const float* rootRect) {
    float minX, minZ, maxX, maxZ;
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView || !rootRect || !g_centerMapOnPos || !halls::MapBounds(minX, minZ, maxX, maxZ)) return;
    const float dragX = *reinterpret_cast<float*>(mapView + 0x18c), dragY = *reinterpret_cast<float*>(mapView + 0x190);
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    // Only when the map has just opened on this hall, the zoom step has changed or the map was moved and has come to
    // rest with the mouse button up (the game moves the view itself while you drag; +0x18c keeps an offset a zoom left)
    uint64_t* stepIdx = *reinterpret_cast<uint64_t**>(mapView + 0x258);
    const int step = g_mapZoomWindowStart + (stepIdx ? static_cast<int>(*stepIdx) : 0);
    static int s_dungeon = -1, s_step = -1;
    static bool s_dragging = false, s_pending = false, s_moved = false;
    static uint64_t s_lastFrame = 0, s_movedTick = 0;
    static float s_cx = 0.0f, s_cy = 0.0f;
    const uint64_t now = GetTickCount64();
    const float cx = *reinterpret_cast<float*>(mapView + 0x1f8), cy = *reinterpret_cast<float*>(mapView + 0x1fc);
    if (std::fabs(cx - s_cx) > 0.5f || std::fabs(cy - s_cy) > 0.5f) {
        s_moved = true;
        s_movedTick = now;
    }
    s_cx = cx, s_cy = cy;
    const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, dragging = buttonDown;
    if (s_dungeon != halls::g_mapDungeon || now - s_lastFrame > 500 || step != s_step || (s_dragging && !dragging))
        s_pending = true;
    if (s_moved && !buttonDown && now - s_movedTick > 150) {
        s_moved = false;
        s_pending = true;
    }
    s_lastFrame = now;
    s_dungeon = halls::g_mapDungeon;
    s_step = step;
    s_dragging = dragging;
    // ... and the view has settled: no drag, the zoom at its step (the game animates it there over some frames)
    const float base = *reinterpret_cast<float*>(mapView + 0x58);
    const float stepZoom = stepIdx && *stepIdx < 5 ? *reinterpret_cast<float*>(mapView + 0x40 + *stepIdx * 4) * base : zoom;
    if (!s_pending || dragging || std::fabs(zoom - stepZoom) > stepZoom * 0.01f) return;
    s_pending = false;
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0001f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) + dragX - *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) + dragY - *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float top = rootRect[1] + 88.0f, bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f) - 64.0f;
    // What the screen shows, on the map's canvas (x east, y south from the world's north edge), and the hall there
    const float v0x = (left - originX) / zoom, v1x = (right - originX) / zoom;
    const float v0y = (top - originY) / zoom, v1y = (bottom - originY) / zoom;
    const float h0y = mapWorldH - maxZ, h1y = mapWorldH - minZ;
    auto shift = [](float v0, float v1, float h0, float h1) {
        if (h1 - h0 <= v1 - v0) return (h0 + h1) * 0.5f - (v0 + v1) * 0.5f; // fits: in the middle
        if (v0 < h0) return h0 - v0;                                       // past the hall's start
        if (v1 > h1) return h1 - v1;                                       // past its end
        return 0.0f;
    };
    const float dx = shift(v0x, v1x, minX, maxX), dy = shift(v0y, v1y, h0y, h1y);
    if (std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f) return;
    // What is in the middle now (as the zoom steps work it out), moved by the difference; the offset cleared first
    float center[2] = {(*reinterpret_cast<float*>(mapView + 0x1f8) - dragX) / zoom + dx,
                       (*reinterpret_cast<float*>(mapView + 0x1fc) - dragY) / zoom + dy};
    *reinterpret_cast<uint64_t*>(mapView + 0x18c) = 0;
    // The game's re-centring also sets the zoom: kept as it is (+0x10 now, +0x238 the target), as the zoom steps do
    const float zoomTarget = *reinterpret_cast<float*>(mapView + 0x238);
    g_centerMapOnPos(mapView, center, 1);
    *reinterpret_cast<float*>(mapView + 0x10) = zoom;
    *reinterpret_cast<float*>(mapView + 0x238) = zoomTarget;
    *reinterpret_cast<float*>(mapView + 0x20) = center[0];
    *reinterpret_cast<float*>(mapView + 0x24) = center[1];
    *reinterpret_cast<float*>(mapView + 0x30) = center[0];
    *reinterpret_cast<float*>(mapView + 0x34) = center[1];
    *reinterpret_cast<float*>(mapView + 0x1a8) = center[0];
    *reinterpret_cast<float*>(mapView + 0x1ac) = center[1];
}

#ifdef UMM_DEV_BRIDGE
// MapNativeFilters "find": a search and its first result's pick (as Enter would), no dialog or focus.
std::string g_devFindQuery, g_devFindResult = "{}";
bool g_devFindPending = false;
bool g_devFindHidden = false; // findhidden: filters off, as F2 in the dialog, so hidden places are picked too
void RunDevFind(uint8_t* client, uint8_t* mapState) {
    if (!g_devFindPending) return;
    g_devFindPending = false;
    const std::string kept = g_findQuery;
    g_findQuery = g_devFindQuery;
    g_findRespectFilters = !g_devFindHidden;
    RebuildFindResults(client, mapState);
    const FindDisplayRow* pick = nullptr;
    for (const auto& row : g_findDisplayRows) {
        if (!row.isToggleFilterRow && !row.matchIndices.empty()) { pick = &row; break; }
    }
    std::string title, where;
    bool filterOnBefore = false;
    if (pick && pick->matchIndices[0] < g_findMatchedPlaces.size()) {
        const auto& place = g_findMatchedPlaces[pick->matchIndices[0]];
        title = place.title;
        where = place.typeLabel;
        filterOnBefore = place.isFilterOn;
    }
    const size_t matches = g_findMatchedPlaces.size(), filtered = g_findFilteredOutCount;
    const size_t pickedPlaces = pick ? pick->matchIndices.size() : 0;
    LARGE_INTEGER pickFreq{}, pickStart{}, pickEnd{};
    QueryPerformanceFrequency(&pickFreq);
    QueryPerformanceCounter(&pickStart);
    if (pick) FocusMapOnWorldPositions(client, mapState, pick->matchIndices);
    QueryPerformanceCounter(&pickEnd);
    const double pickMs = 1000.0 * static_cast<double>(pickEnd.QuadPart - pickStart.QuadPart) / static_cast<double>(pickFreq.QuadPart);
    auto clean =[](std::string s) { for (char& c : s) if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = ' '; return s; };
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"query\":\"%s\",\"matches\":%zu,\"filteredOut\":%zu,\"picked\":\"%s\",\"where\":\"%s\",\"filterOnBefore\":%s,\"goldChest\":%s,\"silverChest\":%s,\"woodChest\":%s}",
                  clean(g_devFindQuery).c_str(), matches, filtered, clean(title).c_str(), clean(where).c_str(),
                  filterOnBefore ? "true" : "false", g_goldChestEnabled ? "true" : "false",
                  g_silverChestEnabled ? "true" : "false", g_woodChestEnabled ? "true" : "false");
    g_devFindResult = buffer;
    if (!g_devFindResult.empty() && g_devFindResult.back() == '}') {
        char extra[64];
        std::snprintf(extra, sizeof(extra), ",\"pickedPlaces\":%zu,\"pickMs\":%.2f}", pickedPlaces, pickMs);
        g_devFindResult.pop_back();
        g_devFindResult += extra;
    }
    g_findRespectFilters = true;
    g_findQuery = kept;
    g_findMatchedPlaces.clear();
    g_findDisplayRows.clear();
}
#endif

void DrawMapKeyOverlay(void* mapStateVoid) {
    g_lastMapState = static_cast<uint8_t*>(mapStateVoid);
    // A generation's first map frame: the game's text input flag an earlier generation left on (its search box typed
    // in at the reload) goes off, or Escape stays dead everywhere
    static bool s_inputChecked = false;
    if (!s_inputChecked) {
        s_inputChecked = true;
        if (!g_keySearchFocused && ClearTextInputFlag() && g_log)
            g_log("Map Key: the game's text input was left on by the previous load; turned it off (Escape works again)");
    }
    auto* client = GetUiClient();
    auto* mapState = static_cast<uint8_t*>(mapStateVoid);
    if (!client || !mapState) return;

    LoadMinimapConfig();
    ApplyMapZoomSteps(mapState);

    // Detect World Map opening so we always center on the player's actual location when opening the map
    if (g_bridgeRefreshMap) {
        *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
        g_bridgeRefreshMap = false;
    }
    if (g_bridgeRecenterMap) {
        g_worldMapOpenCenterFrames = 2;
        g_bridgeRecenterMap = false;
    }
#ifdef UMM_DEV_BRIDGE
    if (g_bridgeZoomSet > 0.0f) {
        if (uint8_t* mv = *reinterpret_cast<uint8_t**>(mapState + 8)) *reinterpret_cast<float*>(mv + 0x10) = g_bridgeZoomSet;
        g_bridgeZoomSet = 0.0f;
    }
#endif
    if (g_bridgeCenterX >= 0.0f) {
        CenterMapViewOnWorldPos(mapState, g_bridgeCenterX, g_bridgeCenterZ);
        g_bridgeCenterX = -1.0f;
    }
    uint64_t nowMapTick = GetTickCount64();
    // A new open is only the screen changing to the map (cleared in the per-frame screen check), not a pause in its
    // frames: Alt-Tab or a hitch while the map stays open used to centre it on the player again
    static bool s_openOnEntrance = false; // Dungeon Maps off, opened in a Hollow Hall: centred on its entrance
    static float s_entranceX = 0.0f, s_entranceZ = 0.0f;
    static int s_journalTargetFrames = 0; // opened from a menu (F on a Journal entry): its row checked a few frames on
    if (!g_worldMapWasOpen) {
        g_worldMapWasOpen = true;
        // In a Hollow Hall the map shows the dungeon; with Dungeon Maps off, the overworld centred on the hall's
        // entrance (where the game puts you on it; your own spot is the empty rock of the halls' corner)
        s_openOnEntrance = false;
        halls::OnMapOpen(client);
        if (!g_dungeonMaps) {
            float ex = 0.0f, ez = 0.0f;
            halls::g_mapOverworld = true; // MapFocus: the entrance
            if (halls::g_mapDungeon >= 0 && halls::g_mapDungeon < static_cast<int>(halls::kHallCount) && halls::MapFocus(ex, ez)) {
                s_openOnEntrance = true;
                s_entranceX = ex;
                s_entranceZ = ez;
            }
            halls::g_mapDungeon = -1;
            halls::g_mapOverworld = false;
        }
        // Center on the player only when opened from play (screen 1, the M key). From any menu (the backpack
        // or journal 9, Show on Map for a quest 7, ...) the game has already centered the map on what was asked
        // for, so leave it there. A dungeon map always shows its whole hall (back from another tab with Q / E it was
        // left wherever the game had it)
        g_worldMapOpenCenterFrames = (g_screenBeforeMap == 1 || halls::MapActive() || s_openOnEntrance) ? 2 : 0;
        s_journalTargetFrames = g_worldMapOpenCenterFrames == 0 ? 3 : 0;
        // At most once a second: a screen flipping with the map every frame wrote ~11000 lines a minute
        static uint64_t s_lastOpenLog = 0;
        const bool logOpen = nowMapTick - s_lastOpenLog >= 1000ULL;
        if (logOpen) s_lastOpenLog = nowMapTick;
        if (g_log && logOpen) g_log("World Map opened from screen %u (%s)", g_screenBeforeMap,
                         g_worldMapOpenCenterFrames ? "centering on player" : "keeping the game's view");
#ifdef UMM_DEV_BRIDGE
        if (g_log && logOpen) g_log("World Map state at %p (dev)", mapState);
#endif
    }
    // Back on the World tab after another one (Q / E: the screen stays the map's, but the map isn't drawn there): a
    // dungeon map shows its whole hall again
    if (halls::MapActive() && g_lastWorldMapFrameTick && nowMapTick - g_lastWorldMapFrameTick > 300 &&
        g_worldMapOpenCenterFrames == 0)
        g_worldMapOpenCenterFrames = 2;
    g_lastWorldMapFrameTick = nowMapTick;
    g_dungeonMapUp = halls::MapActive();
    // The map opened on what a menu asked for (F on a Journal entry): its row on if it was off. Not where you stand (a
    // tab switch with Q / E shows the map round you)
    if (s_journalTargetFrames > 0 && --s_journalTargetFrames == 0) {
        float cx = 0.0f, cz = 0.0f, px = 0.0f, pz = 0.0f;
        if (uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8)) {
            float mapH = *reinterpret_cast<float*>(mapView + 0x3c);
            if (mapH <= 100.0f) mapH = 10240.0f;
            cx = *reinterpret_cast<float*>(mapView + 0x20);
            cz = mapH - *reinterpret_cast<float*>(mapView + 0x24);
        }
        const bool atPlayer = GetHudPlayerWorldPos(client, px, pz) && std::fabs(px - cx) < 15.0f && std::fabs(pz - cz) < 15.0f;
        if (g_log) g_log("Map: opened from screen %u on %.0f,%.0f (you at %.0f,%.0f)", g_screenBeforeMap, cx, cz, px, pz);
        if (cx > 0.0f && !atPlayer) ShowJournalTargetRow(client, mapState, cx, cz);
    }
    if (g_worldMapOpenCenterFrames > 0) {
        --g_worldMapOpenCenterFrames;
        float focusX = 0.0f, focusZ = 0.0f, extentW = 0.0f, extentH = 0.0f;
        if (g_worldMapOpenCenterFrames == 1 && halls::MapExtent(extentW, extentH)) {
            // The whole dungeon in view: between the menu banner and the key hints, left of the Map Key
            FitMapZoom(mapState, extentW, extentH, g_lastRootW - 520.0f, g_lastRootH - 114.0f - 70.0f);
        }
        if (halls::MapFocus(focusX, focusZ)) CenterMapViewOnWorldPos(mapState, focusX, focusZ); // the dungeon / its entrance
        else if (s_openOnEntrance) CenterMapViewOnWorldPos(mapState, s_entranceX, s_entranceZ); // Dungeon Maps off, in a hall
        else CenterMapViewOnPlayer(client, mapState);
    }

    HWND foreground = GetForegroundWindow();
    DWORD fgPid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
    bool focused = (fgPid == GetCurrentProcessId()) || inp::GameHasFocus();

    // The dungeon map: C (Recenter) goes where the game shows you on the overworld (the hall's entrance); centred on
    // where you are in the hall instead, for a few frames while the game's own move runs out
    {
        static bool s_cWasDown = false;
        static int s_hallRecenterFrames = 0;
        const bool cDown = focused && !g_keySearchFocused && BindingDown(g_keys.hallRecenter);
        if (cDown && !s_cWasDown && halls::MapActive() && !halls::g_mapOverworld) s_hallRecenterFrames = 6;
        s_cWasDown = cDown;
        // A vault's or spire's map: the floor shown, one up / down
        static bool s_upWas = false, s_downWas = false;
        const bool upDown = focused && !g_keySearchFocused && !g_findDialogOpen && BindingDown(g_keys.floorUp);
        const bool downDown = focused && !g_keySearchFocused && !g_findDialogOpen && BindingDown(g_keys.floorDown);
        if (upDown && !s_upWas) halls::StepFloor(+1);
        if (downDown && !s_downWas) halls::StepFloor(-1);
        s_upWas = upDown;
        s_downWas = downDown;
        float px = 0.0f, pz = 0.0f;
        if (s_hallRecenterFrames > 0) {
            --s_hallRecenterFrames;
            if (GetHudPlayerWorldPos(client, px, pz)) CenterMapViewOnWorldPos(mapState, px, pz);
        }
    }

    UpdateChestLootTracking(client, focused);

    uint32_t allHashes[kTotalCategoryRows]{};
    size_t numHashes = 0;
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (numHashes < sizeof(allHashes) / sizeof(allHashes[0])) allHashes[numHashes++] = grp.rows[r].typeHash;
        }
    }
    int hashCounts[kTotalCategoryRows]{};
    SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);

    auto openFindDialog = [&]() {
        // A dialog made by an older Find copy is left behind (its functions are that copy's)
        if (EnsureFindDialogResolved()) g_mapFindDialog = nullptr;
        if (!g_mapFindDialog) {
            if (g_findCreateDialog) {
                g_mapFindDialog = g_findCreateDialog(umm_lang::T(umm_lang::Str::FindTitle), umm_lang::T(umm_lang::Str::FindTitle));
            } else {
                g_mapFindDialog = CreateNativeFindDialogFallback(umm_lang::T(umm_lang::Str::FindTitle), umm_lang::T(umm_lang::Str::FindTitle));
            }
        }
        if (!g_mapFindDialog) return;

        ResetNativeFindDialog(g_mapFindDialog);
        g_findDialogOpen = true;
        g_findInputReady = false;
        g_findQuery.clear();
        g_findMatchedPlaces.clear();
        g_findDisplayRows.clear();
        g_findFilteredOutCount = 0;
        g_findSelectedIndex = -1;
        g_findHoveredIndex = -1;
        g_findScrollOffset = 0;
        g_lastClickedRow = -1;
        g_lastClickTick = 0;
        if (g_findWheelDeltaPtr) {
            InterlockedExchange(g_findWheelDeltaPtr, 0);
        }
        for (int vk = 0; vk < 256; ++vk) {
            g_findKeysWasDown[vk] = ((GetAsyncKeyState(vk) & 0x8000) != 0);
        }
        SyncNativeFindDialogState();
    };

    // Find searches the overworld's places: not on a dungeon's own map (its Overworld view, yes). Only this World Map
    // frame's Shift+F is skipped; the skill tree's Shift+F is Find's own, on its own screen (2026-11-09)
    const bool findHere = !(halls::MapActive() && !halls::g_mapOverworld);
    bool shiftFNow = focused && findHere && BindingDown(g_keys.findOpen);
    // A capital F typed into a pin's name is not Shift+F; neither is Tab typed there. The game's text input flag is
    // not set by every field (not by a new custom pin's name), so a dialog open on the map counts too: mapState +0xf8
    // is 0 with none, 3 for a marker's menu, 4 for naming a custom pin
    const bool typingInGameField =
        !g_findDialogOpen && (GameTextInputActive() || *reinterpret_cast<uint32_t*>(mapState + 0xf8) != 0);
    // Shift+F only opens Find: once it is open a capital F is part of the search ("Flame"); Esc closes it
    if (shiftFNow && !g_shiftFWasDown && !typingInGameField && !g_findDialogOpen) {
        openFindDialog();
    }
    g_shiftFWasDown = shiftFNow;

    bool toggleKeyDown = focused && !g_findDialogOpen && !typingInGameField && BindingDown(g_keys.panelToggle);
    if (toggleKeyDown && !g_toggleKeyWasDown) {
        g_panelCollapsed = !g_panelCollapsed;
    }
    g_toggleKeyWasDown = toggleKeyDown;

    // Numpad + / - (or Page Up / Page Down) zoom the World Map one step in / out about the middle of the view (the
    // wheel zooms about the cursor). The view's centre in map canvas units is (mapView+0x1f8 - drag) / zoom; the step
    // is set as Find does.
    {
        static bool plusWasDown = false, minusWasDown = false;
        const bool keysFree = focused && !g_findDialogOpen && !typingInGameField;
        UpdateFilterHistory(client, mapState, keysFree);
        UpdatePresetKeys(client, mapState, keysFree && !g_keySearchFocused);
        const bool plusDown = keysFree && BindingDown(g_keys.mapZoomIn);
        const bool minusDown = keysFree && BindingDown(g_keys.mapZoomOut);
        const int dir = (plusDown && !plusWasDown) ? -1 : (minusDown && !minusWasDown) ? 1 : 0; // -1 closer
        plusWasDown = plusDown;
        minusWasDown = minusDown;
        uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
        uint64_t* idx = mapView ? *reinterpret_cast<uint64_t**>(mapView + 0x258) : nullptr;
        const uint8_t numSteps = mapView ? mapView[0x54] : 0;
        const float zoom = mapView ? *reinterpret_cast<float*>(mapView + 0x10) : 0.0f;
        if (dir != 0 && idx && numSteps > 0 && numSteps <= 16 && *idx < numSteps && zoom > 0.0001f) {
            const int64_t next = static_cast<int64_t>(*idx) + dir;
            if (next >= 0 && next < numSteps) {
                float base = *reinterpret_cast<float*>(mapView + 0x58);
                if (base <= 0.0001f) base = 1.0f;
                const float target = *reinterpret_cast<float*>(mapView + 0x40 + next * 4) * base;
                // (mapView+0x18c is a drag still in progress, in screen units: part of what is in the middle now)
                float center[2] = {(*reinterpret_cast<float*>(mapView + 0x1f8) - *reinterpret_cast<float*>(mapView + 0x18c)) / zoom,
                                   (*reinterpret_cast<float*>(mapView + 0x1fc) - *reinterpret_cast<float*>(mapView + 0x190)) / zoom};
                *idx = static_cast<uint64_t>(next);
                *reinterpret_cast<uint8_t*>(mapView + 0x250) = 1;
                *reinterpret_cast<uint8_t*>(mapView + 0x260) = 0;
                *reinterpret_cast<float*>(mapView + 0x268) = 0.0f;
                *reinterpret_cast<float*>(mapView + 0x10) = target;
                *reinterpret_cast<float*>(mapView + 0x238) = target;
                *reinterpret_cast<uint64_t*>(mapView + 0x18c) = 0;
                if (g_centerMapOnPos) g_centerMapOnPos(mapView, center, 1);
                *reinterpret_cast<float*>(mapView + 0x10) = target;
                *reinterpret_cast<float*>(mapView + 0x238) = target;
                *reinterpret_cast<float*>(mapView + 0x20) = center[0];
                *reinterpret_cast<float*>(mapView + 0x24) = center[1];
                *reinterpret_cast<float*>(mapView + 0x30) = center[0];
                *reinterpret_cast<float*>(mapView + 0x34) = center[1];
                *reinterpret_cast<float*>(mapView + 0x1a8) = center[0];
                *reinterpret_cast<float*>(mapView + 0x1ac) = center[1];
                *reinterpret_cast<float*>(mapView + 0x1b0) = center[0];
                *reinterpret_cast<float*>(mapView + 0x1b4) = center[1];
                *reinterpret_cast<float*>(mapView + 0x198) = 0.0f;
                *reinterpret_cast<float*>(mapView + 0x19c) = 0.0f;
                *reinterpret_cast<float*>(mapView + 0x1f8) = target * center[0];
                *reinterpret_cast<float*>(mapView + 0x1fc) = target * center[1];
                const uint64_t panTs = *reinterpret_cast<uint64_t*>(mapView + 0x1a0);
                *reinterpret_cast<uint64_t*>(mapView + 0x08) = panTs ? panTs : *reinterpret_cast<uint64_t*>(mapView + 0x00);
                // Centring leaves a waypoint target (+0x28) and a latched click behind: cleared as after Find, and the
                // map's clicks stay latched off for a few frames
                *reinterpret_cast<uint64_t*>(mapView + 0x28) = 0;
                ClearMapViewClickLatch(mapState);
                g_findClosingFrames = std::max(g_findClosingFrames, 8);
            }
        }
    }

    uint8_t* hints = GetHintCountPtr();
    if (hints && *hints >= 8) *hints = 0;
    uint8_t hintsBefore = hints ? *hints : 0;

    bool leftDown = focused && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    bool clicked = leftDown && !g_leftWasDown;

    static const UiText kLayerName = MakeUiText("mapKeyFilterLayer");
    alignas(16) uint8_t layer[0x400]{};
    g_beginLayer(layer, &kLayerName, 1, kLayerOrder, -1, 0);
    {
        Widget root;
        g_namedWidget(root.raw, &kLayerName);
        g_opaque(root.raw);
        float rootRect[4]{};
        if (root.Handle()) {
            std::memcpy(rootRect, root.Handle() + 0x114, sizeof(rootRect));
        }
        std::memcpy(g_overlayRootRect, rootRect, sizeof(rootRect));

        const float rootW = (rootRect[2] > 100.0f) ? rootRect[2] : 1920.0f;
        const float rootH = (rootRect[3] > 100.0f) ? rootRect[3] : 1080.0f;

        // Draw animated pulse rings on the map for any places selected via Find
        DrawPulsingMapHighlights(root.raw, rootRect, mapState);


        POINT cursor{};
        RECT clientRect{};
        bool haveCursor = focused && GetCursorPos(&cursor) && ScreenToClient(foreground, &cursor) &&
                          GetClientRect(foreground, &clientRect) && clientRect.right > 0 && clientRect.bottom > 0;
        float mx = haveCursor ? (rootRect[0] + cursor.x * rootW / static_cast<float>(clientRect.right)) : -1.0f;
        float my = haveCursor ? (rootRect[1] + cursor.y * rootH / static_cast<float>(clientRect.bottom)) : -1.0f;
        // A controller in the Map Key (View): its cursor and A stand in for the mouse's; a mouse click takes over
        const bool padAllowed = focused && !g_findDialogOpen && !g_keySearchFocused && !leftDown &&
                                *reinterpret_cast<uint32_t*>(mapState + 0xf8) == 0;
        const bool padDrives = PadKeyFrame(padAllowed, mx, my, clicked, g_mapKeyScroll);
        if (padDrives) {
            haveCursor = true;
            leftDown = clicked;
        }
        // A controller has no pointer on the map: its mark and hide act on the middle of the view, which the left stick
        // pans. For the frame of the press the cursor stands there, so the icons under it are found as for a click.
        // Held and its edge, not PadPressed: that repeats on the D-pad, and a held button would keep toggling.
        const inp::PadState padMapNow = inp::ReadPad();
        const bool padMapFree = focused && !padDrives && !g_padKeyFocus && !g_findDialogOpen && !g_keySearchFocused &&
                                *reinterpret_cast<uint32_t*>(mapState + 0xf8) == 0;
        static bool s_padMarkWas = false, s_padHideWas = false;
        const bool padMarkHeld = padMapFree && inp::PadHeld(g_keys.padMarkChest, padMapNow);
        const bool padHideHeld = padMapFree && inp::PadHeld(g_keys.padHideRow, padMapNow);
        const bool padMarkChest = padMarkHeld && !s_padMarkWas;
        const bool padHideRow = padHideHeld && !s_padHideWas;
        s_padMarkWas = padMarkHeld;
        s_padHideWas = padHideHeld;
        if (padMarkChest || padHideRow) {
            if (uint8_t* view = *reinterpret_cast<uint8_t**>(mapState + 8)) {
                const float zoom = *reinterpret_cast<float*>(view + 0x10);
                mx = *reinterpret_cast<float*>(view + 0x1f0) + *reinterpret_cast<float*>(view + 0x18c) -
                     *reinterpret_cast<float*>(view + 0x1f8) + *reinterpret_cast<float*>(view + 0x30) * zoom;
                my = *reinterpret_cast<float*>(view + 0x1f4) + *reinterpret_cast<float*>(view + 0x190) -
                     *reinterpret_cast<float*>(view + 0x1fc) + *reinterpret_cast<float*>(view + 0x34) * zoom;
                haveCursor = true;
                if (g_log) g_log("Map: controller at the middle of the view, %.0f, %.0f on screen (root %.0f x %.0f)", mx, my, rootW, rootH);
            }
        }

        auto isHovered = [&](float absX, float absY, float rw, float rh) -> bool {
            PadTargetSeen(absX, absY, rw, rh);
            return haveCursor && mx >= absX && mx <= absX + rw && my >= absY && my <= absY + rh;
        };

        // Forests and deposits share one budget of extra draws a frame (see DrawTreeAreasOnMap)
        UpdateFocusedRegion(client);
#ifdef UMM_DEV_BRIDGE
        RunDevFind(client, mapState);
        hdpool::DrawTest(root.raw, rootRect);
        halls::DrawPoolTest(root.raw, rootRect);
#endif
        // (KeepDungeonInView: moving the view from here fought the game's own drag and zoom; not used)
        hall_pan::Update(mapState, rootRect); // the game's pan clamp: the hall's box on a dungeon map
        halls::DrawMapPlan(root.raw, rootRect, mapState); // in a Hollow Hall: its floor plan over the empty corner
        if (!halls::MapActive()) DrawLevelAreasOnMap(root.raw, rootRect, mapState);
        // A marker's menu (Fast Travel / Send Ping / Set as Waypoint) is open: the mod's icons are drawn after the
        // game's, so they stay out of its box (mapState +0xf8 is 3 and +0x542e8 holds the marker while it is open; the
        // menu opens beside the marker, on its right unless that runs off the screen)
        g_drawExclusion.active = false;
        if (*reinterpret_cast<uint32_t*>(mapState + 0xf8) == 3) {
            const uint8_t* menuMarker = *reinterpret_cast<uint8_t**>(mapState + 0x542e8);
            uint8_t* view = *reinterpret_cast<uint8_t**>(mapState + 8);
            if (menuMarker && view) {
                const float zoom = *reinterpret_cast<float*>(view + 0x10);
                float worldH = *reinterpret_cast<float*>(view + 0x3c);
                if (worldH <= 100.0f) worldH = 10240.0f;
                const float ox = *reinterpret_cast<float*>(view + 0x1f0) + *reinterpret_cast<float*>(view + 0x18c) -
                                 *reinterpret_cast<float*>(view + 0x1f8);
                const float sx = ox + static_cast<float>(*reinterpret_cast<const int32_t*>(menuMarker + 0x14)) * zoom;
                constexpr float kMenuW = 354.0f, kGap = 45.0f, kMargin = 12.0f;
                const float rightEdge = rootRect[0] + rootW, bottomEdge = rootRect[1] + rootH;
                float left = sx + kGap;
                if (left + kMenuW > rightEdge) left = sx - kGap - kMenuW;
                // Its height varies (other mods add buttons: Hollow Halls Progression's Return Beacons), so the mod's
                // icons stay out of its whole column
                g_drawExclusion = {zoom > 0.0f, left - kMargin, rootRect[1], left + kMenuW + kMargin, bottomEdge};
#ifdef UMM_DEV_BRIDGE
                static uint64_t lg = 0;
                if (g_log && GetTickCount64() - lg > 1000)
                    lg = GetTickCount64(), g_log("Map: menu column on (marker %p type %08x): icons off from x %.0f to %.0f", menuMarker,
                                                 *reinterpret_cast<const uint32_t*>(menuMarker + 0x30), left - kMargin, left + kMenuW + kMargin);
#endif
            }
        }
        // The key hints' strip at the bottom: no mod icon over it (the layers' images below are drawn another way)
        g_hintStrip = {true, rootRect[0], rootRect[1] + rootH - 92.0f, rootRect[0] + rootW, rootRect[1] + rootH};
        SetLocatorExclusions(client, mapState);
        // Find's results: the creature rows it lists stay on the World Map with Hide on World Map (CreatureOffWorldMap)
        if (g_findDialogOpen && g_nearCreaturesHideMap) {
            std::vector<uint8_t> was = GetTickCount64() < g_findShownUntil ? g_findShownOres : std::vector<uint8_t>();
            g_findShownOres.assign(kOreCount, 0);
            for (const FindPlaceEntry& e : g_findMatchedPlaces)
                if (IsOreHash(e.filterTypeHash)) g_findShownOres[OreIndexOf(e.filterTypeHash)] = 1;
            g_findShownUntil = GetTickCount64() + 60000;
            // the setting's change noted (as the filter changes are): "Boar - Hide on World Map: OFF", "+ 2 more"
            std::vector<size_t> fresh;
            for (size_t o = 0; o < g_findShownOres.size(); ++o)
                if (g_findShownOres[o] && CreatureSpawnHidden(o) && (o >= was.size() || !was[o])) fresh.push_back(o);
            if (!fresh.empty()) {
                std::string name = OreName(fresh[0]);
                if (fresh.size() > 1) name += " +" + std::to_string(fresh.size() - 1);
                ShowMapNotice(filter_notice::Mark(name, umm_lang::T(umm_lang::Str::HideOnWorldMap), umm_lang::T(umm_lang::Str::Off)));
            }
        }
        g_hoverTitleWidget = root.raw;
        g_mapCursorIn = haveCursor, g_mapCursor[0] = mx, g_mapCursor[1] = my;
        PublishCursorIcon();
#ifdef UMM_DEV_BRIDGE
        if (g_devNear[0] > 0.0f) { // dev: the markers projected within 60 UI px of a screen point
            float zz = 0, dox = 0, doy = 0, dwh = 0;
            if (LevelMapView(mapState, zz, dox, doy, dwh) && g_log) {
                constexpr size_t kAO[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
                for (size_t a = 0; a < 4; ++a) {
                    uint8_t* base = *reinterpret_cast<uint8_t**>(client + kAO[a]);
                    const uint64_t n = *reinterpret_cast<uint64_t*>(client + kAO[a] + 8);
                    if (!base || n > 10000) continue;
                    for (uint64_t i = 0; i < n; ++i) {
                        const uint8_t* m = base + i * 0xA0;
                        const float sx = dox + *reinterpret_cast<const int32_t*>(m + 0x14) * zz;
                        const float sy = doy + (dwh - *reinterpret_cast<const int32_t*>(m + 0x24)) * zz;
                        if (std::fabs(sx - g_devNear[0]) > 60.0f || std::fabs(sy - g_devNear[1]) > 60.0f) continue;
                        g_log("Near: a%zu#%llu at %d,%d screen %.0f,%.0f orig %08x type %08x id %08x r %.0f", a, static_cast<unsigned long long>(i),
                              *reinterpret_cast<const int32_t*>(m + 0x14), *reinterpret_cast<const int32_t*>(m + 0x24), sx, sy,
                              GetMarkerOrigTypeHash(m), *reinterpret_cast<const uint32_t*>(m + 0x30), *reinterpret_cast<const uint32_t*>(m + 0x08),
                              *reinterpret_cast<const float*>(m + 0x28));
                    }
                }
                g_log("Near: zoom %.3f", zz);
                // the game's own list for this frame (mapState +0x542f0: data, +0x542f8: count), its raw entries
                const uint8_t* list = *reinterpret_cast<uint8_t* const*>(mapState + 0x542f0);
                const uint64_t ln = *reinterpret_cast<const uint64_t*>(mapState + 0x542f8);
                g_log("Near: list %p count %llu", list, static_cast<unsigned long long>(ln));
                for (uint64_t i = 0; list && i < ln && i < 4000; ++i) {
                    const uint8_t* e = list + i * 0x40;
                    const uint8_t* m = *reinterpret_cast<uint8_t* const*>(e + 8);
                    std::string w;
                    for (int k = 0; k < 0x40; k += 4) {
                        char one[12];
                        std::snprintf(one, sizeof(one), " %08x", *reinterpret_cast<const uint32_t*>(e + k));
                        w += one;
                    }
                    // entries with no marker, or their marker near the point
                    bool hit = !m;
                    if (m) {
                        const float sx = dox + *reinterpret_cast<const int32_t*>(m + 0x14) * zz;
                        const float sy = doy + (dwh - *reinterpret_cast<const int32_t*>(m + 0x24)) * zz;
                        hit = std::fabs(sx - g_devNear[0]) < 60.0f && std::fabs(sy - g_devNear[1]) < 60.0f;
                    }
                    if (hit) g_log("Near: entry %llu marker %p:%s", static_cast<unsigned long long>(i), m, w.c_str());
                }
            }
            g_devNear[0] = -1.0f;
        }
        if (g_devDot[0] > 0.0f) {
            float zz = 0, dox = 0, doy = 0, dwh = 0;
            if (LevelMapView(mapState, zz, dox, doy, dwh)) {
                const float sx = dox + g_devDot[0] * zz, sy = doy + (dwh - g_devDot[1]) * zz;
                DrawSolidRectAbs(root.raw, sx - 6.0f, sy - 6.0f, 12.0f, 12.0f, 0xffff00ffu, 1.0f);
                static uint64_t lg = 0;
                if (g_log && GetTickCount64() - lg > 1000) lg = GetTickCount64(), g_log("Map: dot at %.0f,%.0f (cursor %.0f,%.0f)", sx, sy, mx, my);
            }
        }
#endif
#ifdef UMM_DEV_BRIDGE
        RunQuestAt(client);
#endif
        {
            // the hover title's area: the game's to the right of its hovered icon (its name, the region under it), or
            // the mod's own as drawn last frame
            const uint64_t now = GetTickCount64();
            g_titleBox.active = false;
            if (GameIconUnderCursor() && now - g_hoverTitleTick < 150ULL && g_hoverTitleText[0]) {
                const float half = g_hoveredIcon[2] * 0.6f;
                const float w = umm_lang::TextWidth(g_hoverTitleText) * (40.0f / 15.0f) * 1.45f + 16.0f;
                g_titleBox = {true, g_hoveredIcon[0] + half, g_hoveredIcon[1] - 30.0f,
                              g_hoveredIcon[0] + half + std::max(w, 260.0f), g_hoveredIcon[1] + 58.0f};
            } else if (now - g_modTitleBoxTick < 150ULL) {
                g_titleBox = g_modTitleBox;
            }
        }
        g_legendBox = {!halls::MapActive(), rootRect[0] + 30.0f, rootRect[1] + 112.0f, rootRect[0] + 340.0f, rootRect[1] + 232.0f};
        // The overworld's layers stay off the dungeon map
        const bool overworldLayers = !halls::MapActive();
        if (overworldLayers) DrawRegionBorderOnMap(root.raw, rootRect, mapState);
        if (overworldLayers) DrawLevelBordersOnMap(root.raw, rootRect, mapState);
        if (overworldLayers) ground::DrawOnMap(root.raw, rootRect, mapState); // under the forests
        const int treeDraws = overworldLayers ? DrawTreeAreasOnMap(root.raw, rootRect, mapState) : 0;
        // Roads and Surface Water: in the map's own layer (DrawUnderRing, next frame) when it's up, under every pin, the
        // key hints and a marker's menu; drawn here over the map they covered them
        if (overworldLayers && MapLayerReady()) {
            g_roadsUnder = true, g_roadsMapState = mapState;
            std::memcpy(g_roadsRect, rootRect, sizeof(g_roadsRect));
        } else if (overworldLayers) roads::DrawOnMap(root.raw, rootRect, mapState);
        g_toMapLayer = true; // the mod's map words and the check marks on the game's markers: under the game's markers
        if (overworldLayers && g_regionNames) DrawRegionNamesOnMap(root.raw, rootRect, mapState);
        g_toMapLayer = false;
        DrawOreDepositsOnMap(root.raw, rootRect, client, mapState, haveCursor, mx, my,
                             std::max(500, std::min(1500, 2000 - treeDraws)));
        // (the check marks stay over their marker: in the map layer the game's marker covered them; only where the ring,
        // the hints, the legend or a menu are over them do they go under, UnderRing)
        g_markerChecks = true;
        DrawVisitedSanctumsOnMap(root.raw, rootRect, client, mapState);
        DrawRefilledChestsOnMap(root.raw, rootRect, client, mapState);
        g_markerChecks = false;
        if (g_woodChestEnabled || halls::MapActive()) DrawWoodChestsOnMap(root.raw, rootRect, client, mapState, haveCursor, mx, my);
        DrawGraveHeightsOnMap(root.raw, rootRect, client, mapState);
        if (g_townPortalsEnabled) DrawTownPortalsOnMap(root.raw, rootRect, mapState, haveCursor, mx, my); // Town Portal's open portals
        // a quest's place pin under the cursor: the quest's name on the tooltip panel (its card blanked: HideRumorsInner)
        if (g_questTitleFor && g_questTitleFor == g_hoveredGameMarker && GetTickCount64() - g_questTitleTick < 150ULL && g_questTitle[0])
            DrawMapHoverTitle(g_hoveredIcon[0] + g_hoveredIcon[2] * 0.65f + 26.0f, g_hoveredIcon[1], g_questTitle, true);
#ifdef UMM_DEV_BRIDGE
        {
            static uint64_t lgH = 0;
            if (g_log && haveCursor && GetTickCount64() - lgH > 250) {
                lgH = GetTickCount64();
                const uint8_t* hm = g_hoveredGameMarker;
                g_log("Map: hovered %p type %08x orig %08x vis %02x at %.0f,%.0f half %.1f; replaced %d modTitle %d titleAge %llu '%s'; questFor %p",
                      hm, hm ? *reinterpret_cast<const uint32_t*>(hm + 0x30) : 0u, hm ? GetMarkerOrigTypeHash(hm) : 0u, hm ? hm[0x35] : 0,
                      g_hoveredIcon[0], g_hoveredIcon[1], g_hoveredIcon[2], hm && GameTitleReplaced(hm), ModHoverTitleShowing(),
                      static_cast<unsigned long long>(GetTickCount64() - g_hoverTitleTick), g_hoverTitleText, g_questTitleFor);
            }
        }
#endif
        // a hidden quest / rumor pin under the cursor (the game shows nothing for it): its own title on the panel
        if (g_questPinNamed && g_questPinNamed == g_hoveredGameMarker && GetTickCount64() - g_hoverTitleTick < 150ULL && g_hoverTitleText[0] &&
            !(g_questTitleFor == g_hoveredGameMarker && GetTickCount64() - g_questTitleTick < 150ULL))
            DrawMapHoverTitle(g_hoveredIcon[0] + g_hoveredIcon[2] * 0.65f + 26.0f, g_hoveredIcon[1], g_hoverTitleText, true);
        // the game's hover title, drawn by the mod on its tooltip panel (the game's own is left empty: MarkerTitleTextHook)
        if (g_hoveredGameMarker && GameTitleReplaced(g_hoveredGameMarker) && !ModHoverTitleShowing() &&
            GetTickCount64() - g_hoverTitleTick < 150ULL && g_hoverTitleText[0])
            DrawMapHoverTitle(g_hoveredIcon[0] + g_hoveredIcon[2] * 0.65f + 26.0f, g_hoveredIcon[1], g_hoverTitleText, true); // (past the icon as the game's: its visible half and a gap)
        g_hintStrip.active = false;
        for (DrawExclusion& e : g_locators) e.active = false;
        g_legendBox.active = false;
        DrawMapCursorPosition(root.raw, rootRect, mapState, haveCursor, mx, my, false);
        DrawFilterNotice(root.raw, rootRect);
        // Hollow Halls Progression: a reached beacon's badge travels there
        if (halls::DrawMapBeacons(root.raw, rootRect, mapState, haveCursor, mx, my, clicked)) ClearMapViewClickLatch(mapState);
        DrawDungeonMapPlayers(root.raw, rootRect, client, mapState);
        FlushMapHoverTitles(); // over every icon
        g_drawExclusion.active = false; // the Map Key and the rest draw as usual
        // In a Hollow Hall: a button between the dungeon's map and the overworld's (centred on the hall's entrance)
        if (halls::g_mapDungeon >= 0) {
            const char* label = umm_lang::T(halls::g_mapOverworld ? umm_lang::Str::DungeonMap : umm_lang::Str::OverworldMap);
            const float bw = umm_lang::TextWidth(label) * 1.6f + 40.0f, bh = 34.0f;
            const float bx = rootRect[0] + rootW * 0.5f - bw * 0.5f, by = rootRect[1] + 124.0f;
            const bool hov = isHovered(bx, by, bw, bh);
            DrawKeyButtonAbs(root.raw, bx, by, bw, bh, hov, false);
            DrawLabelAbs(bx, by, bw, bh, label, hov ? key_style::kActiveText : key_style::kText, hov ? 1.0f : 0.92f,
                         TextAlignH::Center, TextAlignV::Center);
            if (hov) ClearMapViewClickLatch(mapState); // not a click on the map under it
            if (clicked && hov) {
                halls::g_mapOverworld = !halls::g_mapOverworld;
                g_worldMapOpenCenterFrames = 2;
            }
            // A vault's or spire's floors: [-] Floor 2 / 3 [+] under the button (or Ctrl + Up / Down, LT + Up / Down)
            const int floors = halls::FloorCount(halls::g_mapDungeon);
            if (halls::MapActive() && floors > 1 && halls::g_mapFloor >= 0) {
                char text[64];
                std::snprintf(text, sizeof(text), "%s %d / %d", umm_lang::T(umm_lang::Str::Floor), halls::g_mapFloor + 1, floors);
                const float lw = umm_lang::TextWidth(text) * 1.6f + 24.0f, sb = 34.0f, gap = 6.0f;
                const float rowW = sb + gap + lw + gap + sb, rx = rootRect[0] + rootW * 0.5f - rowW * 0.5f, ry = by + bh + 8.0f;
                const bool canDown = halls::g_mapFloor > 0, canUp = halls::g_mapFloor < floors - 1;
                auto stepButton = [&](float x, const char* sign, bool enabled, int dir) {
                    const bool h = enabled && isHovered(x, ry, sb, sb);
                    DrawKeyButtonAbs(root.raw, x, ry, sb, sb, h, false);
                    DrawLabelAbs(x, ry, sb, sb, sign, h ? key_style::kActiveText : key_style::kText, enabled ? (h ? 1.0f : 0.92f) : 0.35f,
                                 TextAlignH::Center, TextAlignV::Center);
                    if (isHovered(x, ry, sb, sb)) ClearMapViewClickLatch(mapState);
                    if (clicked && h) halls::StepFloor(dir);
                };
                stepButton(rx, "-", canDown, -1);
                DrawKeyButtonAbs(root.raw, rx + sb + gap, ry, lw, sb, false, false);
                DrawLabelAbs(rx + sb + gap, ry, lw, sb, text, key_style::kText, 0.95f, TextAlignH::Center, TextAlignV::Center);
                if (isHovered(rx + sb + gap, ry, lw, sb)) ClearMapViewClickLatch(mapState);
                stepButton(rx + sb + gap + lw + gap, "+", canUp, +1);
            }
        }
        {
            // Alt + click a marker: its Map Key row off
            const bool altDown = focused && inp::KeysHeld(g_keys.hideRowClick, KeyIsDown);
            uint8_t* view = *reinterpret_cast<uint8_t**>(mapState + 8);
            const bool overPanel = mx >= g_lastPanelX && mx <= g_lastPanelX + g_lastPanelW && my >= g_lastPanelY &&
                                   my <= g_lastPanelY + g_lastPanelH;
            if (((clicked && altDown) || padHideRow) && haveCursor && !overPanel && !g_findDialogOpen && view) {
                const float zoom = *reinterpret_cast<float*>(view + 0x10);
                float worldH = *reinterpret_cast<float*>(view + 0x3c);
                if (worldH <= 100.0f) worldH = 10240.0f;
                const float ox = *reinterpret_cast<float*>(view + 0x1f0) + *reinterpret_cast<float*>(view + 0x18c) -
                                 *reinterpret_cast<float*>(view + 0x1f8);
                const float oy = *reinterpret_cast<float*>(view + 0x1f4) + *reinterpret_cast<float*>(view + 0x190) -
                                 *reinterpret_cast<float*>(view + 0x1fc);
                HideRowTarget target;
                // the game's marker under the cursor (exactly its spot); else the nearest within reach
                int32_t hx = 0, hz = 0;
                // the game's marker if the cursor is on its icon, or it shows the title of one and no icon of the mod's
                // is under the cursor; else the nearest within reach
                const bool onGameIcon = !padHideRow && (GameIconUnderCursor(&hx, &hz) || (!g_hoveredDepositRow && GameTitledMarker(hx, hz)));
                // nothing under the cursor: a quest / rumor pin whose search circle holds the click (its "?" is drawn
                // shifted inside that circle, away from its own spot)
                int32_t cx = 0, cz = 0;
                const bool inCircle = !onGameIcon && zoom > 0.0f && !g_hoveredDepositRow &&
                    RumorCircleAt(client, (mx - ox) / zoom, worldH - (my - oy) / zoom, kMarkerReach / zoom, cx, cz);
                if (zoom > 0.0f && (onGameIcon ? HideRowTargetAt(client, hx, hz, 1.0f, 0u, target)
                                    : inCircle ? HideRowTargetAt(client, cx, cz, 1.0f, 0u, target)
                                               : HideRowTargetAt(client, static_cast<int32_t>((mx - ox) / zoom),
                        static_cast<int32_t>(worldH - (my - oy) / zoom), kMarkerReach / zoom, g_hoveredDepositRow, target))) {
                    HideClickedRow(client, mapState, target, padHideRow ? "controller" : "Alt+click");
                    ClearMapViewClickLatch(mapState); // no menu, no waypoint
                    g_panelClickLatch = true;
                    g_findClosingFrames = std::max(g_findClosingFrames, 8);
                }
#ifdef UMM_DEV_BRIDGE
                else if (g_log && zoom > 0.0f) { // dev: what was near the click
                    const float wx = (mx - ox) / zoom, wz = worldH - (my - oy) / zoom;
                    float best = 1e12f; uint32_t bo = 0, b30 = 0; int bx = 0, bz = 0; int bh = 0;
                    constexpr size_t kOff[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
                    for (size_t a = 0; a < 4; ++a) {
                        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kOff[a]);
                        const uint64_t n = *reinterpret_cast<uint64_t*>(client + kOff[a] + 8);
                        if (!base || n > 10000) continue;
                        for (uint64_t k = 0; k < n; ++k) {
                            const uint8_t* m = base + k * 0xA0;
                            const float dx = *reinterpret_cast<const int32_t*>(m + 0x14) - wx, dz = *reinterpret_cast<const int32_t*>(m + 0x24) - wz;
                            if (dx * dx + dz * dz >= best || *reinterpret_cast<const uint32_t*>(m + 0x30) == 0u) continue;
                            best = dx * dx + dz * dz; bo = GetMarkerOrigTypeHash(m); b30 = *reinterpret_cast<const uint32_t*>(m + 0x30);
                            bx = *reinterpret_cast<const int32_t*>(m + 0x14); bz = *reinterpret_cast<const int32_t*>(m + 0x24);
                            bh = MarkerHiddenOnMap(m) ? 1 : 0;
                        }
                    }
                    g_log("Map: Alt+click at %.0f,%.0f found nothing within %.1f m (zoom %.3f, hovered row %08x); nearest drawn %08x/%08x at %d,%d, %.1f m, hidden %d",
                          wx, wz, kMarkerReach / zoom, zoom, g_hoveredDepositRow, bo, b30, bx, bz, std::sqrt(best), bh);
                }
#endif
            }
        }

        const PanelLayout& lay = CurrentPanelLayout();
        const float kPanelW = lay.panelW;
        constexpr float kTitleBarH = 30.0f;
        const float kCatRowH = umm_lang::TallScript() ? 24.0f : 23.0f;
        constexpr float kPinRowH = 28.0f;
        constexpr float kScrollBarRoom = 10.0f;
        // SHOW ALL / HIDE ALL, under the title bar, and the filter presets' strip under them (neither scrolls)
        constexpr float kActionRowH = 30.0f + 30.0f;
        constexpr float kPresetRowY = 30.0f; // the strip, from the top of the action rows
        // Regions: [+/-] [ the regions shown ] [Dim | Hide], and unfolded a drawer with a row per region, under it (does
        // not scroll), with Settings > Advanced > Region Filter on
        constexpr float kRegionLineH = 21.0f;
        const float kRegionRowH = !g_regionFilterEnabled ? 0.0f
                                  : 30.0f + (g_regionDrawerOpen ? kRegionCount * kRegionLineH + 4.0f : 0.0f);
        constexpr float kSearchRowH = 30.0f; // the search box, under them (does not scroll)
        constexpr float kItemRowH0 = 20.0f;
        if (KeySearching()) KeySearchPlaces(client, mapState);
        UpdateKeySearchMatch();
        const float panelX = rootRect[0] + rootW - kPanelW - 24.0f;
        const float panelY = rootRect[1] + 104.0f;

        auto panelHeight = [&](float itemRowH) {
            float h = kTitleBarH + 6.0f;
            if (!g_panelCollapsed) {
                h += 6.0f + kActionRowH + kRegionRowH + kSearchRowH;
                if (KeySearching() && g_keySearchMatch.any == 0) h += kItemRowH0 + 4.0f; // "No matching rows"
                for (size_t c = 0; c < kCategoryGroupCount; ++c) {
                    if (!KeySearchCatShown(c)) continue;
                    h += kCatRowH + 2.0f;
                    if (KeySearching() || !g_categoryCollapsed[c]) {
                        if (kCategoryGroups[c].category == 4) {
                            h += kPinRowH + 4.0f;
                        } else {
                            h += KeySearchRowCount(c) * itemRowH + 4.0f;
                        }
                    }
                }
                h += 6.0f;
            }
            return h;
        };
        // Japanese, Korean, Chinese and Thai rows get a little more height, but never so much that the
        // panel reaches the game's key hints along the bottom of the map.
        float kItemRowH = 20.0f;
        if (umm_lang::TallScript()) {
            const float maxH = rootH - 104.0f - 72.0f;
            kItemRowH = 22.0f;
            while (kItemRowH > 20.0f && panelHeight(kItemRowH) > maxH) kItemRowH -= 0.5f;
        }
        // Taller than the screen allows: the panel stops above the key hints and its list scrolls
        const float contentH = panelHeight(kItemRowH);
        const float totalH = std::min(contentH, std::max(200.0f, rootH - 104.0f - 72.0f));
        const float scrollMax = contentH - totalH;
        g_mapKeyScroll = std::max(0.0f, std::min(scrollMax, g_mapKeyScroll));
        if (scrollMax <= 0.0f) g_mapKeyDragging = false;

        const float settingsCardW = SettingsCardWidth();
        const float settingsX = std::max(rootRect[0] + 12.0f, panelX - settingsCardW - 10.0f);
        const float settingsY = panelY;
        const bool settingsHovered = !g_findDialogOpen && g_settingsOpen &&
                                     isHovered(settingsX, settingsY, settingsCardW, kSettingsCardH);

        // Block map & map marker hover/click interaction behind the panel or when Find dialog is open
        const bool panelHovered = !g_findDialogOpen && (padDrives || !UsingGamepad()) &&
                                  (isHovered(panelX, panelY, kPanelW, totalH) || settingsHovered);
        const bool rightDown = focused && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        if (panelHovered && (leftDown || rightDown || clicked)) {
            g_panelClickLatch = true;
            g_findClosingFrames = std::max(g_findClosingFrames, 8);
        }

        // Ctrl + left click on the World Map marks a chest looted, or clears the mark
        {
            const bool ctrlDown = focused && inp::KeysHeld(g_keys.chestClick, KeyIsDown);
            const bool clearClickNow = !g_findDialogOpen && !panelHovered && haveCursor && ctrlDown && leftDown;
            const bool clearClickEdge = (clearClickNow && !g_worldMapChestClickWasDown) || (padMarkChest && haveCursor && !panelHovered);
            g_worldMapChestClickWasDown = clearClickNow;
            if (clearClickEdge) {
                if (uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8)) {
                    float zoom = *reinterpret_cast<float*>(mapView + 0x10);
                    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
                    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
                    float originX = *reinterpret_cast<float*>(mapView + 0x1f0) +
                                    *reinterpret_cast<float*>(mapView + 0x18c) -
                                    *reinterpret_cast<float*>(mapView + 0x1f8);
                    float originY = *reinterpret_cast<float*>(mapView + 0x1f4) +
                                    *reinterpret_cast<float*>(mapView + 0x190) -
                                    *reinterpret_cast<float*>(mapView + 0x1fc);
                    if (zoom > 0.01f) {
                        float clickWx = (mx - originX) / zoom;
                        float clickWz = mapWorldH - ((my - originY) / zoom);
                        float hitRadiusMeters = std::max(8.0f, 18.0f / zoom);
                        // A boss first: its icon is bigger than a chest's, so a click on it is meant for it
                        const float iconSize = std::max(12.0f, std::min(22.0f, 9.0f + zoom * 6.0f));
                        const float shiftM = iconSize * 0.7f / zoom; // as DrawOreDepositsOnMap draws them aside
                        // A marker under the cursor that can't be marked (a Shroud Root, an ore): its row off. The
                        // wider search for chests and the like below would otherwise catch one beside it.
                        HideRowTarget under;
                        // Ctrl + click only marks, it never hides a row (Alt + click does that). A marker under the
                        // cursor that isn't a chest, note, boss or item: a place (a town, a camp, a sanctum) is marked
                        // done; anything else (a quest, an ore, a creature) is left as it is, and the wider search for
                        // chests and the like below doesn't catch one beside it.
                        // (the game's marker under the cursor exactly, when the mod's icon isn't; else the nearest within reach)
                        int32_t hx = 0, hz = 0;
                        const bool hovered = !padMarkChest && (GameIconUnderCursor(&hx, &hz) || (!g_hoveredDepositRow && GameTitledMarker(hx, hz)));
                        if (hovered) g_hoveredDepositRow = 0u; // (the game's icon is over the mod's)
                        const float atX = hovered ? static_cast<float>(hx) : clickWx, atZ = hovered ? static_cast<float>(hz) : clickWz;
                        const float reach = hovered ? 1.0f : kMarkerReach / zoom;
                        bool unmarkable = HideRowTargetAt(client, static_cast<int32_t>(atX), static_cast<int32_t>(atZ),
                                                          reach, g_hoveredDepositRow, under) &&
                                          !RowCanBeMarked(under.hashes[0]);
                        int32_t placeX = 0, placeZ = 0;
                        const bool placeDone = unmarkable && g_hoveredDepositRow == 0u &&
                                               ToggleDonePlaceAt(client, atX, atZ, reach, &placeX, &placeZ);
                        if (placeDone) {
                            unmarkable = false;
                            NoteMarkStep(5, static_cast<float>(placeX), static_cast<float>(placeZ), 0, 0, 0, true);
                        }
                        if (unmarkable) {
                            if (g_log) g_log("Map: Ctrl+click on %08x: nothing to mark there", under.hashes[0]);
                            AnnounceCantMark(under.hashes[0]);
                            ClearMapViewClickLatch(mapState);
                            g_panelClickLatch = true;
                            g_findClosingFrames = std::max(g_findClosingFrames, 8);
                        }
                        g_markClickOnWorldMap = true;
                        const float nearR = std::max(10.0f, 16.0f / zoom);
                        int kind = 0;
                        if (!placeDone && !unmarkable) {
                            kind = ToggleManualBossNear(clickWx, clickWz, nearR, shiftM)                        ? 1
                                   : ToggleUniqueFoundNear(clickWx, clickWz, nearR, shiftM)                     ? 2
                                   : ToggleLoreReadNear(clickWx, clickWz, nearR, shiftM, hitRadiusMeters)       ? 3
                                   : ToggleChestClearedNearWorldPos(clickWx, clickWz, hitRadiusMeters)          ? 4
                                                                                                                : 0;
                            NoteMarkStep(kind, clickWx, clickWz, nearR, shiftM, hitRadiusMeters, true);
                        }
                        const bool marked = placeDone || kind != 0;
                        g_markClickOnWorldMap = false;
                        if (marked) {
                            SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                            ClearMapViewClickLatch(mapState);
                            g_findClosingFrames = std::max(g_findClosingFrames, 8);
                        }
                    }
                }
            }
        }
        g_panelCurrentlyHovered = panelHovered;
        EnsureMapKeyWheelHook(foreground);
        g_mapKeyWheelCapture = (panelHovered && !settingsHovered && scrollMax > 0.0f) || g_findDialogOpen;
        if (!g_findDialogOpen) { // while Find is open its list takes the wheel (below)
            const LONG wheel = InterlockedExchange(&g_mapKeyWheelDelta, 0);
            if (wheel != 0 && scrollMax > 0.0f) {
                g_mapKeyScroll = std::max(0.0f, std::min(scrollMax, g_mapKeyScroll - wheel / 120.0f * 60.0f));
            }
        }
        g_lastRootX = rootRect[0];
        g_lastRootY = rootRect[1];
        g_lastRootW = rootW;
        g_lastRootH = rootH;
        g_lastPanelX = panelX;
        g_lastPanelY = panelY;
        g_lastPanelW = kPanelW;
        g_lastPanelH = totalH;
        g_lastSettingsX = settingsX;
        g_lastSettingsY = settingsY;
        g_lastSettingsW = g_settingsOpen ? settingsCardW : 0.0f;
        g_lastSettingsH = g_settingsOpen ? kSettingsCardH : 0.0f;

        if (!g_findDialogOpen && g_findClosingFrames > 0) {
            --g_findClosingFrames;
            // (not while the Map Key's search box is typed in: it keeps the flag on itself)
            if (!g_keySearchFocused && ClearTextInputFlag() && g_log)
                g_log("Find on the map: the game's text input came back on after closing; turned it off");
        }
        if (g_findDialogOpen || g_findClosingFrames > 0 || g_waitReleaseAfterFindClose ||
            g_panelClickLatch || panelHovered) {
            ClearMapViewClickLatch(mapState);
        }

        static const UiText kPanelBoxName = MakeUiText("mapKeyFilterPanelBox");
        Widget anchor;
        g_namedWidget(anchor.raw, &kPanelBoxName);
        if (uint8_t* ah = anchor.Handle()) {
            g_setSize(ah, PackXY(kPanelW, totalH));
            *reinterpret_cast<uint64_t*>(ah + 0x70) = PackXY(panelX, panelY);
            *reinterpret_cast<float*>(ah + 0x114) = panelX;
            *reinterpret_cast<float*>(ah + 0x118) = panelY;
            *reinterpret_cast<float*>(ah + 0x11c) = kPanelW;
            *reinterpret_cast<float*>(ah + 0x120) = totalH;
            if (g_checkWidgetButton) {
                uint32_t btnDummy = 0;
                g_checkWidgetButton(anchor.raw, &btnDummy, 1, 0, 0x7b);
                g_checkWidgetButton(anchor.raw, &btnDummy, 2, 0, 0x7b);
            }

            using namespace key_style;

            // Panel: the game's rounded box, dark, in a bronze frame with a faint gold inner glow
            DrawFramedBoxAbs(anchor.raw, panelX, panelY, kPanelW, totalH, kPanelFill, 0.95f, kGoldDim, 0.95f, 8.0f);
            DrawBoxAbs(anchor.raw, kBoxInnerGlowPrimitive, panelX + 1.0f, panelY + 1.0f, kPanelW - 2.0f, totalH - 2.0f,
                kGold, 0.22f, 10.0f);

            // Row 1: Title + [FIND (Shift+F)] Button + [⚙] Settings Button + Collapse Button [ - / + ]
            const float btnColX = kPanelW - 40.0f, btnColY = 4.0f, btnColW = 32.0f, btnColH = 22.0f;
            const float btnGearW = 28.0f, btnGearH = 22.0f, btnGearX = btnColX - 4.0f - btnGearW, btnGearY = 4.0f;
            const float btnFindW = lay.findBtnW, btnFindX = btnGearX - 4.0f - btnFindW, btnFindY = 4.0f, btnFindH = 22.0f;
            DrawLabelAbs(panelX + 12.0f, panelY, btnFindX - 16.0f, kTitleBarH,
                umm_lang::T(umm_lang::Str::MapKeyTitle), kTitle, 1.0f, TextAlignH::Left, TextAlignV::Center);

            bool hovFindBtn = findHere && !g_findDialogOpen && isHovered(panelX + btnFindX, panelY + btnFindY, btnFindW, btnFindH);
            DrawKeyButtonAbs(anchor.raw, panelX + btnFindX, panelY + btnFindY, btnFindW, btnFindH, hovFindBtn, g_findDialogOpen);
            {
                const bool lit = g_findDialogOpen || hovFindBtn;
                const float bx = panelX + btnFindX, by = panelY + btnFindY;
                DrawLabelAbs(bx + 10.0f, by, btnFindW - 20.0f, btnFindH,
                    umm_lang::T(umm_lang::Str::Find), lit ? kActiveText : kText, findHere ? 0.96f : 0.35f, // greyed on a dungeon's map
                    lay.showShortcut ? TextAlignH::Left : TextAlignH::Center, TextAlignV::Center);
                if (lay.showShortcut) {
                    DrawLabelAbs(bx + 10.0f, by, btnFindW - 20.0f, btnFindH,
                        CurrentKeyText(g_keys.findOpen).c_str(), lit ? kTitle : 0xff6fa9c2u, !findHere ? 0.3f : lit ? 0.98f : 0.9f,
                        TextAlignH::Right, TextAlignV::Center); // #c2a96f
                }
            }
            if (clicked && hovFindBtn) {
                openFindDialog();
            }

            // [⚙] Settings Cog Button
            bool hovGear = !g_findDialogOpen && isHovered(panelX + btnGearX, panelY + btnGearY, btnGearW, btnGearH);
            DrawKeyButtonAbs(anchor.raw, panelX + btnGearX, panelY + btnGearY, btnGearW, btnGearH, hovGear, g_settingsOpen);
            {
                const bool gearLit = g_settingsOpen || hovGear;
                const uint32_t gearFill = hovGear ? kHoverFill : (g_settingsOpen ? kActiveFill : kButtonFill);
                DrawGearIconAbs(anchor.raw,
                                panelX + btnGearX + btnGearW * 0.5f,
                                panelY + btnGearY + btnGearH * 0.5f,
                                6.5f, gearLit ? kActiveText : kText, gearLit ? 1.0f : 0.92f, gearFill);
            }
            if (clicked && hovGear) {
                g_settingsOpen = !g_settingsOpen;
            }

            bool hovCol = !g_findDialogOpen && isHovered(panelX + btnColX, panelY + btnColY, btnColW, btnColH);
            DrawKeyButtonAbs(anchor.raw, panelX + btnColX, panelY + btnColY, btnColW, btnColH, hovCol, false);
            DrawLabelAbs(panelX + btnColX, panelY + btnColY, btnColW, btnColH,
                g_panelCollapsed ? "+" : "-", hovCol ? kTitle : kText, hovCol ? 1.0f : 0.9f,
                TextAlignH::Center, TextAlignV::Center);
            if (clicked && hovCol) {
                g_panelCollapsed = !g_panelCollapsed;
            }

            if (!g_panelCollapsed) {
                // The tooltip strip drawn over the list this frame (Show All / Hide All, or a hovered padlock)
                const char* hintText = nullptr;
                float hintY = 0.0f;
                // SHOW ALL / HIDE ALL
                {
                    const float btnY = kTitleBarH + 4.0f, btnH = kPresetRowY - 4.0f;
                    const float btnW = (kPanelW - 22.0f) * 0.5f;
                    const float xs[2] = {8.0f, 14.0f + btnW};
                    const umm_lang::Str labels[2] = {umm_lang::Str::ShowAll, umm_lang::Str::HideAll};
                    // A click (on release) skips the locked filters; holding the button passes them (a gold bar
                    // fills along its bottom). Hovering either button says so.
                    static int s_allPress = -1;
                    static uint64_t s_allPressTick = 0;
                    static bool s_allPressFired = false;
                    constexpr uint64_t kHoldMs = 700;
                    const uint64_t nowTick = GetTickCount64();
                    bool hovBtn[2]{};
                    for (int i = 0; i < 2; ++i) {
                        const bool hov = !g_findDialogOpen && isHovered(panelX + xs[i], panelY + btnY, btnW, btnH);
                        hovBtn[i] = hov;
                        const bool pressing = s_allPress == i && !s_allPressFired;
                        DrawKeyButtonAbs(anchor.raw, panelX + xs[i], panelY + btnY, btnW, btnH, hov, s_allPress == i);
                        if (pressing) {
                            const float held = std::min(1.0f, static_cast<float>(nowTick - s_allPressTick) / kHoldMs);
                            DrawSolidRectAbs(anchor.raw, panelX + xs[i] + 3.0f, panelY + btnY + btnH - 5.0f,
                                (btnW - 6.0f) * held, 3.0f, kGold, 0.95f);
                        }
                        DrawLabelAbs(panelX + xs[i], panelY + btnY, btnW, btnH, umm_lang::T(labels[i]),
                            hov ? kActiveText : kText, hov ? 1.0f : 0.92f, TextAlignH::Center, TextAlignV::Center);
                        if (clicked && hov) {
                            s_allPress = i;
                            s_allPressTick = nowTick;
                            s_allPressFired = false;
                        }
                    }
                    if (s_allPress >= 0) {
                        if (leftDown) {
                            if (!s_allPressFired && nowTick - s_allPressTick >= kHoldMs) {
                                SetAllFiltersState(client, mapState, s_allPress == 0, true);
                                SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                                s_allPressFired = true;
                            }
                        } else {
                            if (!s_allPressFired && hovBtn[s_allPress]) {
                                SetAllFiltersState(client, mapState, s_allPress == 0);
                                SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                            }
                            s_allPress = -1;
                        }
                    }
                    if ((hovBtn[0] || hovBtn[1]) && s_allPress < 0) {
                        hintText = umm_lang::T(umm_lang::Str::HoldForLocked);
                        hintY = panelY + btnY + btnH + 5.0f;
                    }
                }

                // PRESETS: [1][2][3][4][5] and a name: the hovered slot's, else the one the filters match. Click applies,
                // Ctrl + click saves the filters shown into it, a right click renames it (typed in place of the name)
                {
                    LoadFilterPresets();
                    const float rowY = panelY + kTitleBarH + 4.0f + kPresetRowY, btnH = kPresetRowY - 4.0f;
                    const float slotW = 26.0f, gap = 4.0f;
                    const float nameX = panelX + 8.0f + kPresetCount * (slotW + gap) + 2.0f;
                    const float nameW = panelX + kPanelW - 8.0f - nameX;
                    static bool s_rightWas = false;
                    const bool rightClick = rightDown && !s_rightWas;
                    s_rightWas = rightDown;
                    const bool ctrl = inp::KeysHeld(g_keys.chestClick, KeyIsDown);
                    auto endRename = [&](bool keep) {
                        if (g_presetRename < 0) return;
                        if (keep) {
                            std::string t = g_presetRenameText;
                            while (!t.empty() && t.back() == ' ') t.pop_back();
                            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
                            // the default's own name stays the default (it follows the language)
                            g_presets[g_presetRename].name = t == umm_lang::T(kPresetDefaultNames[g_presetRename]) ? "" : t;
                            SaveFilterPreset(g_presetRename);
                        }
                        g_presetRename = -1;
                        g_keySearchFocused = false;
                        ClearTextInputFlag();
                    };
                    if (g_presetRename >= 0 && !g_keySearchFocused) g_presetRename = -1; // the map closed meanwhile
                    if (g_presetRename >= 0 && (clicked || rightClick) && !isHovered(nameX, rowY, nameW, btnH)) endRename(true);
                    if (g_presetRename >= 0) {
                        SetTextInputFlag();
                        std::wstring typed;
                        {
                            std::lock_guard<std::mutex> lock(g_keySearchMutex);
                            typed.swap(g_keySearchTyped);
                        }
                        for (wchar_t ch : typed) {
                            if (g_presetRename < 0) break;
                            if (ch == 0x1b) {
                                endRename(false);
                                g_findClosingFrames = std::max(g_findClosingFrames, 15);
                            } else if (ch == 0x0d) {
                                endRename(true);
                            } else if (ch == 0x08 || ch == 0x7f) {
                                auto& s = g_presetRenameText;
                                while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xc0) == 0x80) s.pop_back();
                                if (!s.empty()) s.pop_back();
                            } else if (ch >= 0x20) {
                                char u[8]{};
                                const int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, u, sizeof(u), nullptr, nullptr);
                                if (n > 0 && g_presetRenameText.size() + n <= 32) g_presetRenameText.append(u, n);
                            }
                        }
                    }
                    const int match = MatchingFilterPreset(client);
                    int hovSlot = -1;
                    for (int p = 0; p < kPresetCount; ++p) {
                        const float bx = panelX + 8.0f + p * (slotW + gap);
                        const bool hov = !g_findDialogOpen && isHovered(bx, rowY, slotW, btnH);
                        if (hov) hovSlot = p;
                        DrawKeyButtonAbs(anchor.raw, bx, rowY, slotW, btnH, hov, match == p || g_presetRename == p);
                        const char digit[2] = {static_cast<char>('1' + p), 0};
                        DrawLabelAbs(bx, rowY, slotW, btnH, digit, (hov || match == p) ? kActiveText : kText,
                                     (hov || match == p) ? 1.0f : 0.9f, TextAlignH::Center, TextAlignV::Center);
                        if (!hov || g_presetRename >= 0) continue;
                        if (clicked) {
                            if (ctrl) StoreFilterPreset(client, p);
                            else {
                                ApplyFilterPreset(client, mapState, p);
                                SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                            }
                        } else if (rightClick) {
                            g_presetRename = p;
                            g_presetRenameText = PresetName(p);
                            g_keySearchFocused = true; // the search box's typing: keys go to the name, not the game
                            std::lock_guard<std::mutex> lock(g_keySearchMutex);
                            g_keySearchTyped.clear();
                        }
                    }
                    if (g_presetRename >= 0) {
                        DrawFramedBoxAbs(anchor.raw, nameX, rowY, nameW, btnH, kHoverFill, 0.95f, kGold, 0.95f);
                        const bool caretOn = (GetTickCount64() / 530) % 2 == 0;
                        const std::string shown = g_presetRenameText + (caretOn ? "_" : "");
                        DrawLabelAbs(nameX + 6.0f, rowY, nameW - 12.0f, btnH, shown.c_str(), kActiveText, 1.0f,
                                     TextAlignH::Left, TextAlignV::Center, nullptr, kFilterRowFontScale);
                        hintText = umm_lang::T(umm_lang::Str::PresetRenameHint);
                        hintY = rowY + btnH + 5.0f;
                    } else {
                        const int shownSlot = hovSlot >= 0 ? hovSlot : match;
                        DrawLabelAbs(nameX + 4.0f, rowY, nameW - 8.0f, btnH,
                                     shownSlot >= 0 ? PresetName(shownSlot) : umm_lang::T(umm_lang::Str::Presets),
                                     shownSlot >= 0 ? (hovSlot >= 0 ? kActiveText : kGold) : kTextMuted, 0.95f,
                                     TextAlignH::Left, TextAlignV::Center, nullptr, kFilterRowFontScale);
                        if (hovSlot >= 0) {
                            hintText = umm_lang::T(umm_lang::Str::PresetTip);
                            hintY = rowY + btnH + 5.0f;
                        }
                    }
                }

                // REGIONS: [+/-] [ the regions shown ]   [Dim | Hide]; unfolded, a row per region: on / off each
                if (g_regionFilterEnabled) {
                    const float rowY = kTitleBarH + 4.0f + kActionRowH, btnH = 26.0f;
                    const float segW = std::max(umm_lang::TextWidth(umm_lang::T(umm_lang::Str::Dim)),
                                                umm_lang::TextWidth(umm_lang::T(umm_lang::Str::Hide))) + 22.0f;
                    const float foldW = 26.0f;
                    const float selX = 8.0f, selW = kPanelW - 16.0f - 2.0f * segW - 10.0f;
                    const float seg0X = selX + selW + 6.0f, seg1X = seg0X + segW + 4.0f;
                    const uint32_t mask = g_regionMask & kAllRegionsMask;
                    const bool any = mask != kAllRegionsMask;
                    int shown = 0, first = -1;
                    for (int r = 0; r < static_cast<int>(kRegionCount); ++r) {
                        if (!((mask >> r) & 1u)) continue;
                        ++shown;
                        if (first < 0) first = r;
                    }
                    // All of them, one by name, the first by level and how many more, or none
                    char label[96];
                    if (!any) {
                        std::snprintf(label, sizeof(label), "%s", umm_lang::T(umm_lang::Str::AllRegions));
                    } else if (shown == 0) {
                        std::snprintf(label, sizeof(label), "0 / %d", static_cast<int>(kRegionCount));
                    } else if (shown == 1) {
                        std::snprintf(label, sizeof(label), "%s", RegionName(first));
                    } else {
                        std::snprintf(label, sizeof(label), "%s +%d", RegionName(first), shown - 1);
                    }
                    auto setMask = [&](uint32_t m) {
                        g_regionMask = m & kAllRegionsMask;
                        UpdateFocusedRegion(client);
                        SaveMinimapConfig();
                    };
                    const float fx = panelX + selX, by = panelY + rowY;
                    const float lx = fx + foldW + 3.0f, lw = selW - foldW - 3.0f;
                    const bool hovFold = !g_findDialogOpen && isHovered(fx, by, foldW, btnH);
                    const bool hovLabel = !g_findDialogOpen && isHovered(lx, by, lw, btnH);
                    DrawKeyButtonAbs(anchor.raw, fx, by, foldW, btnH, hovFold, false);
                    DrawLabelAbs(fx, by, foldW, btnH, g_regionDrawerOpen ? "-" : "+", hovFold ? kActiveText : kGold, 1.0f,
                                 TextAlignH::Center, TextAlignV::Center);
                    DrawKeyButtonAbs(anchor.raw, lx, by, lw, btnH, hovLabel, any);
                    DrawLabelAbs(lx + 4.0f, by, lw - 8.0f, btnH, label, any ? kActiveText : (hovLabel ? kText : kTextMuted),
                                 any ? 1.0f : 0.9f, TextAlignH::Center, TextAlignV::Center, nullptr, kFilterRowFontScale);
                    if (clicked && hovFold) {
                        g_regionDrawerOpen = !g_regionDrawerOpen;
                        SaveMinimapConfig();
                    }
                    // The name, like a section's title: every region on, or (all on already) every region off
                    if (clicked && hovLabel) setMask(mask == kAllRegionsMask ? 0u : kAllRegionsMask);
                    const umm_lang::Str segLabels[2] = {umm_lang::Str::Dim, umm_lang::Str::Hide};
                    const float segXs[2] = {seg0X, seg1X};
                    for (int i = 0; i < 2; ++i) {
                        const bool act = g_regionOutsideHide == (i == 1);
                        const bool hov = !g_findDialogOpen && isHovered(panelX + segXs[i], by, segW, btnH);
                        DrawKeyButtonAbs(anchor.raw, panelX + segXs[i], by, segW, btnH, hov, act && any);
                        DrawLabelAbs(panelX + segXs[i], by, segW, btnH, umm_lang::T(segLabels[i]),
                                     act ? (any ? kActiveText : kText) : (hov ? kText : kOffText), act ? 1.0f : 0.85f,
                                     TextAlignH::Center, TextAlignV::Center);
                        if (clicked && hov && !act) {
                            g_regionOutsideHide = i == 1;
                            SaveMinimapConfig();
                        }
                    }
                    if (g_regionDrawerOpen) {
                        const float rowX = panelX + 8.0f, rowW = kPanelW - 16.0f, badgeRight = rowX + rowW - 4.0f;
                        for (int r = 0; r < static_cast<int>(kRegionCount); ++r) {
                            const float ry = by + 30.0f + static_cast<float>(r) * kRegionLineH;
                            const bool on = (mask >> r) & 1u;
                            const bool hov = !g_findDialogOpen && isHovered(rowX, ry, rowW, kRegionLineH);
                            if (hov) {
                                DrawBoxAbs(anchor.raw, kGlowBoxSmallPrimitive, rowX - 3.0f, ry - 3.0f, rowW + 6.0f,
                                           kRegionLineH + 6.0f, kGold, 0.35f, 8.0f);
                                DrawFramedBoxAbs(anchor.raw, rowX, ry, rowW, kRegionLineH, kHoverFill, 0.92f, kGold, 0.85f, 4.0f);
                            }
                            const float dotCx = rowX + 10.5f, dotCy = ry + kRegionLineH * 0.5f;
                            if (on) {
                                DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, dotCx - 7.0f, dotCy - 7.0f, 14.0f, 14.0f,
                                                 kDotOn, 0.45f);
                            }
                            DrawAssetRectAbs(anchor.raw, kCirclePrimitive, dotCx - 3.5f, dotCy - 3.5f, 7.0f, 7.0f,
                                             on ? kDotOn : kDotOff, on ? 0.98f : 0.8f);
                            // Its colour (Settings > Advanced > Region Colors), in the badge
                            const float bx = rowX + 19.0f, bby = ry + (kRegionLineH - 18.0f) * 0.5f;
                            DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx, bby, 18.0f, 18.0f, kBadgeFill, on ? 0.95f : 0.45f);
                            DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, bx, bby, 18.0f, 18.0f, kBadgeRing,
                                             on ? 0.9f : 0.35f);
                            DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx + 4.0f, bby + 4.0f, 10.0f, 10.0f,
                                             kRegionTint[r], on ? 0.95f : 0.35f);
                            const float nameX = rowX + 42.0f;
                            const float nameW = rowW - 42.0f - OnOffBadgeWidth() - 8.0f;
                            DrawLabelAbs(nameX, ry, nameW, kRegionLineH, RegionName(r),
                                         on ? (hov ? kActiveText : kText) : kOffText, on ? 1.0f : 0.85f,
                                         TextAlignH::Left, TextAlignV::Center, nullptr, kFilterRowFontScale);
                            DrawOnOffBadgeAbs(anchor.raw, badgeRight, ry, kRegionLineH, on);
                            // A click turns it on or off; Ctrl + click shows only this region
                            if (clicked && hov) {
                                const bool ctrl = inp::KeysHeld(g_keys.regionClick, KeyIsDown);
                                setMask(ctrl ? (1u << r) : (mask ^ (1u << r)));
                            }
                        }
                    }
                }

                // SEARCH: narrows the list below (Find, Shift+F, searches the map's places instead). Click to type;
                // Esc clears it, Enter or a click elsewhere keeps it
                {
                    static uint64_t s_lastFrame = 0;
                    const uint64_t now = GetTickCount64();
                    if (now - s_lastFrame > 1000) { // the map was closed: start empty
                        g_keySearch.clear();
                        g_keySearchFocused = false;
                    }
                    s_lastFrame = now;
                    const float bx = panelX + 8.0f, by = panelY + kTitleBarH + 4.0f + kActionRowH + kRegionRowH;
                    const float bw = kPanelW - 16.0f, bh = kSearchRowH - 4.0f;
                    const bool hovBox = !g_findDialogOpen && isHovered(bx, by, bw, bh);
                    const bool hovClear = KeySearching() && !g_findDialogOpen && isHovered(bx + bw - bh, by, bh, bh);
                    if (clicked && g_presetRename < 0) { // (a preset's name being typed has the focus)
                        if (hovClear) {
                            g_keySearch.clear();
                            g_mapKeyScroll = 0.0f;
                        } else if (hovBox && !g_keySearchFocused) {
                            g_keySearchFocused = true;
                            std::lock_guard<std::mutex> lock(g_keySearchMutex);
                            g_keySearchTyped.clear();
                        } else if (!hovBox && g_keySearchFocused) {
                            g_keySearchFocused = false;
                            ClearTextInputFlag();
                        }
                    }
                    if (g_findDialogOpen && g_keySearchFocused) g_keySearchFocused = false;
                    const bool ownFocus = g_keySearchFocused && g_presetRename < 0;
                    if (ownFocus) {
                        SetTextInputFlag();
                        std::wstring typed;
                        {
                            std::lock_guard<std::mutex> lock(g_keySearchMutex);
                            typed.swap(g_keySearchTyped);
                        }
                        const std::string before = g_keySearch;
                        std::wstring plain;
                        auto flush = [&]() {
                            if (plain.empty()) return;
                            const int n = WideCharToMultiByte(CP_UTF8, 0, plain.data(), static_cast<int>(plain.size()),
                                                              nullptr, 0, nullptr, nullptr);
                            std::string u(static_cast<size_t>(std::max(0, n)), '\0');
                            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, plain.data(), static_cast<int>(plain.size()), u.data(),
                                                           n, nullptr, nullptr);
                            if (g_keySearch.size() + u.size() <= 40) g_keySearch += u;
                            plain.clear();
                        };
                        auto popChar = [&]() {
                            while (!g_keySearch.empty() && (static_cast<unsigned char>(g_keySearch.back()) & 0xc0) == 0x80)
                                g_keySearch.pop_back();
                            if (!g_keySearch.empty()) g_keySearch.pop_back();
                        };
                        for (wchar_t ch : typed) {
                            if (ch == 0x1b) { // Esc: clear and leave (the map's input stays off while Esc is down)
                                plain.clear();
                                g_keySearch.clear();
                                g_keySearchFocused = false;
                                g_findClosingFrames = std::max(g_findClosingFrames, 15);
                                break;
                            }
                            if (ch == 0x0d) { // Enter: keep and leave
                                flush();
                                g_keySearchFocused = false;
                                break;
                            }
                            if (ch == 0x08 || ch == 0x7f) { // Backspace, Ctrl+Backspace (the whole word)
                                flush();
                                popChar();
                                if (ch == 0x7f) {
                                    while (!g_keySearch.empty() && g_keySearch.back() == ' ') popChar();
                                    while (!g_keySearch.empty() && g_keySearch.back() != ' ') popChar();
                                }
                                continue;
                            }
                            if (ch >= 0x20) plain.push_back(ch);
                        }
                        flush();
                        if (!g_keySearchFocused) ClearTextInputFlag();
                        if (g_keySearch != before) g_mapKeyScroll = 0.0f;
                    }
                    DrawFramedBoxAbs(anchor.raw, bx, by, bw, bh, ownFocus ? kHoverFill : kPanelFill, 0.95f,
                                     (ownFocus || hovBox) ? kGold : kButtonFrame, ownFocus ? 0.95f : 0.8f);
                    // A small magnifier: a ring and its handle
                    const float mcx = bx + 13.0f, mcy = by + bh * 0.5f - 1.5f;
                    DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, mcx - 5.0f, mcy - 5.0f, 10.0f, 10.0f,
                                     ownFocus ? kGold : kTextMuted, 0.95f, 1.6f);
                    for (int i = 0; i < 4; ++i) {
                        DrawSolidRectAbs(anchor.raw, mcx + 3.0f + i, mcy + 3.0f + i, 2.0f, 2.0f,
                                         ownFocus ? kGold : kTextMuted, 0.95f);
                    }
                    const float tx = bx + 26.0f, tw = bw - 26.0f - bh;
                    // The caret is an "_" after the text, so it sits where the text ends whatever the font (the game's text drops "|")
                    const bool caretOn = ownFocus && (now / 530) % 2 == 0;
                    if (KeySearching() || caretOn) {
                        const std::string shown = g_keySearch + (caretOn ? "_" : "");
                        DrawLabelAbs(tx, by, tw, bh, shown.c_str(), kActiveText, 1.0f, TextAlignH::Left,
                                     TextAlignV::Center, nullptr, kFilterRowFontScale);
                    }
                    if (KeySearching()) {
                        // Clear: an x
                        const float cx = bx + bw - bh * 0.5f, cy = by + bh * 0.5f;
                        for (int i = -4; i <= 4; ++i) {
                            DrawSolidRectAbs(anchor.raw, cx + i - 1.0f, cy + i - 1.0f, 2.0f, 2.0f, hovClear ? kActiveText : kTextMuted, 0.95f);
                            DrawSolidRectAbs(anchor.raw, cx + i - 1.0f, cy - i - 1.0f, 2.0f, 2.0f, hovClear ? kActiveText : kTextMuted, 0.95f);
                        }
                    } else if (!ownFocus) {
                        DrawLabelAbs(tx, by, tw, bh, umm_lang::T(umm_lang::Str::SearchList), kOffText, 0.9f,
                                     TextAlignH::Left, TextAlignV::Center, nullptr, kFilterRowFontScale);
                    }
                }

                auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
                size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);

                // The list scrolls under the title bar; a row is drawn (and clickable) only while fully inside it
                const float contentTop = kTitleBarH + 6.0f + kActionRowH + kRegionRowH + kSearchRowH;
                const float contentBottom = totalH - 6.0f;
                const float py = panelY - g_mapKeyScroll; // screen y of content y 0
                auto inView = [&](float y, float h) {
                    return y - g_mapKeyScroll >= contentTop - 0.5f && y + h - g_mapKeyScroll <= contentBottom + 0.5f;
                };
                const float rowRight = scrollMax > 0.0f ? 14.0f + kScrollBarRoom : 14.0f; // badges clear the scroll bar

                float curY = contentTop;
                if (KeySearching() && g_keySearchMatch.any == 0) {
                    if (inView(curY, kItemRowH0)) {
                        DrawLabelAbs(panelX + 16.0f, py + curY, kPanelW - 32.0f, kItemRowH0,
                            umm_lang::T(umm_lang::Str::NoListMatches), kTextMuted, 0.9f, TextAlignH::Left,
                            TextAlignV::Center, nullptr, kFilterRowFontScale);
                    }
                    curY += kItemRowH0 + 4.0f;
                }
                if (!g_categoryOrderSet) ResetCategoryOrder();
                // Dragging a category's title moves it: a press there starts it, moving 6 px makes it a drag, and the
                // release drops it before the category under the cursor (a press without a drag switches it as before)
                static int catPressed = -1;           // the category pressed (index into kCategoryGroups)
                static float catPressY = 0.0f;
                static bool catDragging = false;
                float barTopY[kCategoryGroupCount]{}; // screen y of each category bar, in shown order
                const bool released = !leftDown && g_leftWasDown;

                for (size_t oi = 0; oi < kCategoryGroupCount; ++oi) {
                    const size_t c = g_categoryOrder[oi];
                    const auto& grp = kCategoryGroups[c];
                    const size_t hashIdx = CategoryHashBase(c);
                    barTopY[oi] = py + curY;
                    if (!KeySearchCatShown(c)) continue;
                    const bool catOpen = KeySearching() || !g_categoryCollapsed[c];
                    bool catOn = IsCategoryEnabled(filterEntries, filterCount, grp.category);

                    if (inView(curY, kCatRowH)) {
                        const float barW = kPanelW - 8.0f - (rowRight - 6.0f);
                        const bool catLocked = IsCategoryLocked(grp.category);
                        const float catLockX = LockLeftX(panelX + kPanelW - rowRight);
                        const bool hovCatLock = isHovered(catLockX - 3.0f, py + curY, kLockSize + 6.0f, kCatRowH);
                        bool hovCatCollapse = isHovered(panelX + 8.0f, py + curY, 34.0f, kCatRowH);
                        bool hovCatToggle = !hovCatLock && isHovered(panelX + 42.0f, py + curY, barW - 34.0f, kCatRowH);
                        const bool hovCat = hovCatCollapse || hovCatToggle || hovCatLock;
                        DrawCategoryBarAbs(anchor.raw, panelX + 8.0f, py + curY, barW, kCatRowH, hovCat);

                        // Collapse marker (+ / -) in gold, then the group's title
                        DrawLabelAbs(panelX + 12.0f, py + curY, 22.0f, kCatRowH,
                            catOpen ? "-" : "+", hovCatCollapse ? kActiveText : kGold, 1.0f,
                            TextAlignH::Center, TextAlignV::Center);
                        DrawLabelAbs(panelX + 36.0f, py + curY,
                            kPanelW - 50.0f - rowRight - OnOffBadgeWidth() - LockSlotWidth(), kCatRowH,
                            umm_lang::T(grp.title), catOn ? kTitle : 0xff948c88u, 1.0f,
                            TextAlignH::Left, TextAlignV::Center, nullptr, kCategoryTitleFontScale);

                        DrawFilterLockAbs(anchor.raw, panelX + kPanelW - rowRight, py + curY, kCatRowH, catLocked, false,
                            hovCat, hovCatLock);
                        DrawOnOffBadgeAbs(anchor.raw, panelX + kPanelW - rowRight, py + curY, kCatRowH, catOn);

                        if (hovCatLock) {
                            hintText = umm_lang::T(umm_lang::Str::LockExempt);
                            hintY = py + curY + kCatRowH + 4.0f;
                        }
                        if (clicked && hovCatLock) {
                            ToggleCategoryLock(grp.category);
                        } else if (clicked && hovCatCollapse && !KeySearching()) {
                            g_categoryCollapsed[c] = !g_categoryCollapsed[c];
                            SaveCollapsedSections();
                            if (catLocked) SaveFilterLocks();
                        } else if (clicked && hovCatToggle) {
                            catPressed = static_cast<int>(c);
                            catPressY = my;
                            catDragging = false;
                        } else if (released && catPressed == static_cast<int>(c) && !catDragging && hovCatToggle) {
                            SetCategoryFilterState(client, mapState, grp.category, !catOn);
                            filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
                        }
                    }

                    curY += kCatRowH + 2.0f;

                    if (!catOpen) continue;

                    if (grp.category == 4) {
                        if (inView(curY, kPinRowH)) {
                            const float stripX = 8.0f;
                            const float pinGap = 3.0f;
                            const float slotW = (kPanelW - 8.0f - (rowRight - 6.0f) - stripX - pinGap * 9.0f) / 10.0f;
                            const float slotH = kPinRowH - 2.0f;
                            for (size_t p = 0; p < 10; ++p) {
                                uint32_t pinHash = kCustomPinHashes[p];
                                bool pinOn = IsFilterEnabled(filterEntries, filterCount, pinHash);
                                float px = stripX + p * (slotW + pinGap);
                                bool hovPin = isHovered(panelX + px, py + curY, slotW, slotH);

                                DrawFramedBoxAbs(anchor.raw, panelX + px, py + curY, slotW, slotH,
                                    hovPin ? kHoverFill : (pinOn ? kButtonFill : kPanelFill), 0.95f,
                                    hovPin ? kGold : (pinOn ? kButtonFrame : kOffFrame), hovPin ? 0.9f : 0.85f);

                                if (const void* iconRef = GetIconRefForType(client, pinHash)) {
                                    DrawIconRectAbs(anchor.raw, iconRef,
                                        panelX + px + (slotW - 18.0f) * 0.5f,
                                        py + curY + (slotH - 18.0f) * 0.5f, 18.0f, 18.0f,
                                        pinOn ? 0xffffffffu : 0xff666666u, pinOn ? 1.0f : 0.38f, pinOn ? 1.15f : 0.7f);
                                }
                                if (clicked && hovPin) {
                                    SetSingleFilterState(client, mapState, pinHash, !pinOn);
                                }
                            }
                        }
                        curY += kPinRowH + 4.0f;
                    } else {
                        // Rows by name in the current language (counts stay in kCategoryGroups order)
                        size_t rowOrder[kMaxCategoryRows]{};
                        const size_t rowN = grp.rowCount;
                        for (size_t r = 0; r < rowN; ++r) rowOrder[r] = r;
                        std::sort(rowOrder, rowOrder + rowN, [&](size_t a, size_t b) {
                            return NameLess(RowText(grp.rows[a]), RowText(grp.rows[b]));
                        });
                        for (size_t k = 0; k < rowN; ++k) {
                            const size_t r = rowOrder[k];
                            if (!KeySearchRowShown(c, r)) continue;
                            const auto& rowSpec = grp.rows[r];
                            int count = hashCounts[hashIdx + r];
                            if (g_scrollToRow != 0 && rowSpec.typeHash == g_scrollToRow) {
                                // Find revealed this row: scroll it to the upper part of the list (clamped below)
                                g_mapKeyScroll = std::max(0.0f, curY - contentTop - kItemRowH * 2.0f);
                                g_scrollToRow = 0;
                            }
                            for (uint32_t linked : {rowSpec.pairedHash, rowSpec.pairedHash2}) {
                                if (IsOreHash(linked)) count += OreDepositCount(OreIndexOf(linked));
                                if (linked == OreHash(kWaterFirst)) count += static_cast<int>(g_builtWater.size());
                            }
                            if (!inView(curY, kItemRowH)) {
                                curY += kItemRowH;
                                continue;
                            }
                            bool rowOn = IsFilterEnabled(filterEntries, filterCount, rowSpec.typeHash);
                            const float rowW = kPanelW - 8.0f - (rowRight - 6.0f);
                            bool hovRow = isHovered(panelX + 8.0f, py + curY, rowW, kItemRowH);
                            if (!g_flashRows.empty() && g_flashRows.count(rowSpec.typeHash)) {
                                const uint64_t nowFlash = GetTickCount64();
                                if (nowFlash >= g_flashRowsUntil) {
                                    g_flashRows.clear();
                                } else {
                                    // A gold glow box around the row, pulsing with the found places on the map: bright
                                    // at the start of each 2.25 s pulse, fading over its first half
                                    const float elapsed = 14.0f - static_cast<float>(g_flashRowsUntil - nowFlash) * 0.001f;
                                    const float phase = std::fmod(std::max(0.0f, elapsed), 2.25f) / 2.25f;
                                    const float fade = 1.0f - std::min(1.0f, phase * 2.0f);
                                    const float pulse = 0.25f + 0.75f * fade;
                                    DrawBoxAbs(anchor.raw, kGlowBoxSmallPrimitive, panelX + 4.0f, py + curY - 4.0f,
                                        rowW + 8.0f, kItemRowH + 8.0f, kGold, 0.35f + 0.55f * pulse, 8.0f);
                                    DrawFramedBoxAbs(anchor.raw, panelX + 8.0f, py + curY, rowW, kItemRowH,
                                        kHoverFill, 0.6f * pulse, kGold, 0.95f, 4.0f);
                                }
                            }
                            const bool rowLocked = IsRowLocked(rowSpec.typeHash);
                            const bool rowCatLocked = IsCategoryLocked(grp.category);
                            const float rowLockX = LockLeftX(panelX + kPanelW - rowRight);
                            const bool hovRowLock = hovRow && isHovered(rowLockX - 3.0f, py + curY, kLockSize + 6.0f, kItemRowH);

                            if (hovRow) {
                                // The game's gold glow box around the row, as Find lights up chests
                                DrawBoxAbs(anchor.raw, kGlowBoxSmallPrimitive, panelX + 5.0f, py + curY - 3.0f,
                                    rowW + 6.0f, kItemRowH + 6.0f, kGold, 0.35f, 8.0f);
                                DrawFramedBoxAbs(anchor.raw, panelX + 8.0f, py + curY, rowW, kItemRowH,
                                    kHoverFill, 0.92f, kGold, 0.85f, 4.0f);
                            }

                            // Status dot with a soft halo while on
                            const float dotCx = panelX + 18.5f, dotCy = py + curY + kItemRowH * 0.5f;
                            if (rowOn) {
                                DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, dotCx - 7.0f, dotCy - 7.0f, 14.0f, 14.0f,
                                    kDotOn, 0.45f);
                            }
                            DrawAssetRectAbs(anchor.raw, kCirclePrimitive, dotCx - 3.5f, dotCy - 3.5f, 7.0f, 7.0f,
                                rowOn ? kDotOn : kDotOff, rowOn ? 0.98f : 0.8f);

                            // Round badge behind the marker icon: dark disc in a parchment ring
                            {
                                const float bx = panelX + 27.0f, by = py + curY + (kItemRowH - 20.0f) * 0.5f;
                                DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx, by, 20.0f, 20.0f,
                                    kBadgeFill, rowOn ? 0.95f : 0.45f);
                                DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, bx, by, 20.0f, 20.0f,
                                    kBadgeRing, rowOn ? 0.9f : 0.35f);
                            }

                            if (rowSpec.typeHash == kTownPortalTypeHash) {
                                // A portal: a violet disc in a bright rim, like the portals on the map
                                const float bx = panelX + 30.0f, by = py + curY + (kItemRowH - 14.0f) * 0.5f;
                                DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx, by, 14.0f, 14.0f, 0xffe050b4u,
                                    rowOn ? 0.55f : 0.2f);
                                DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, bx, by, 14.0f, 14.0f, 0xfffff0ffu,
                                    rowOn ? 0.95f : 0.35f, 1.2f);
                            } else if (IsOreHash(rowSpec.typeHash) && kOreAreaColors[OreIndexOf(rowSpec.typeHash)] != 0) {
                                // Tree areas have no icon: their colour on the map, in the badge
                                const float bx = panelX + 31.0f, by = py + curY + (kItemRowH - 12.0f) * 0.5f;
                                DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx, by, 12.0f, 12.0f,
                                    kOreAreaColors[OreIndexOf(rowSpec.typeHash)], rowOn ? 0.95f : 0.35f);
                            } else if (const void* iconRef = GetIconRefForType(client, rowSpec.typeHash)) {
                                uint32_t iconTint = rowOn ? 0xffffffffu : 0xff707070u;
                                if (rowSpec.typeHash == kGoldChestTypeHash || rowSpec.typeHash == kSpoilsTypeHash) {
                                    iconTint = rowOn ? 0xff38c8ffu : 0xff406880u;
                                } else if (rowSpec.typeHash == kSilverChestTypeHash) {
                                    iconTint = rowOn ? 0xffe8e4dcu : 0xff707070u;
                                } else if (rowSpec.typeHash == kWoodChestTypeHash) {
                                    iconTint = rowOn ? kWoodChestTint : 0xff707070u;
                                }
                                // Ore icons are full-colour inventory art: a little larger in the badge
                                const float iconSz = IsOreHash(rowSpec.typeHash) ? 16.0f : 14.0f;
                                DrawIconRectAbs(anchor.raw, iconRef,
                                    panelX + 37.0f - iconSz * 0.5f, py + curY + (kItemRowH - iconSz) * 0.5f, iconSz, iconSz,
                                    iconTint, rowOn ? 1.0f : 0.40f, rowOn ? 1.15f : 0.7f);
                            }

                            char rowText[192]{};
                            if (count > 0) {
                                std::snprintf(rowText, sizeof(rowText), "%s (%d)", RowText(rowSpec), count);
                            } else {
                                std::snprintf(rowText, sizeof(rowText), "%s", RowText(rowSpec));
                            }
                            // Brought in by a place's name: which one ("Lore Note (12): Song of the ...")
                            if (KeySearching() && !g_keySearchMatch.place[c][r].empty()) {
                                const size_t used = std::strlen(rowText);
                                std::snprintf(rowText + used, sizeof(rowText) - used, ": %s",
                                              g_keySearchMatch.place[c][r].c_str());
                            }
                            uint32_t textCol = rowOn ? 0xffd6e6eeu : kOffText; // #eee6d6 parchment
                            if (rowOn && rowSpec.typeHash == kGoldChestTypeHash) {
                                textCol = 0xff78dcffu;
                            }
                            // a name too long for the row (the constructs' "Ancient G.A.R.D. Construct (Dozer)") is
                            // drawn smaller instead of running under the ON / OFF badge
                            const float labelW = kPanelW - 46.0f - rowRight - OnOffBadgeWidth() - LockSlotWidth();
                            // (TextWidth runs ~15% short of the drawn text: long names ran under the lock and ON / OFF)
                            const float textW = umm_lang::TextWidth(rowText) * kFilterRowFontScale * 1.35f;
                            const float rowFont = textW > labelW
                                ? std::max(kFilterRowFontScale * 0.55f, kFilterRowFontScale * labelW / textW)
                                : kFilterRowFontScale;
                            DrawLabelAbs(panelX + 52.0f, py + curY, labelW, kItemRowH,
                                rowText, textCol, rowOn ? 0.96f : 0.75f,
                                TextAlignH::Left, TextAlignV::Center, nullptr, rowFont);

                            DrawFilterLockAbs(anchor.raw, panelX + kPanelW - rowRight, py + curY, kItemRowH, rowLocked,
                                rowCatLocked, hovRow, hovRowLock);
                            DrawOnOffBadgeAbs(anchor.raw, panelX + kPanelW - rowRight, py + curY, kItemRowH, rowOn,
                                rowOn ? 1.0f : 0.85f);

                            if (hovRowLock) {
                                hintText = umm_lang::T(umm_lang::Str::LockExempt);
                                hintY = py + curY + kItemRowH + 4.0f;
                                if (hintY + 36.0f > panelY + contentBottom) hintY = py + curY - 36.0f;
                            }
                            if (clicked && hovRowLock) {
                                ToggleRowLock(rowSpec.typeHash);
                            } else if (clicked && hovRow) {
                                bool nextState = !rowOn;
                                SetSingleFilterState(client, mapState, rowSpec.typeHash, nextState);
                                if (rowSpec.pairedHash != 0) {
                                    SetSingleFilterState(client, mapState, rowSpec.pairedHash, nextState);
                                }
                                if (rowSpec.pairedHash2 != 0) {
                                    SetSingleFilterState(client, mapState, rowSpec.pairedHash2, nextState);
                                }
                            }

                            curY += kItemRowH;
                        }
                        curY += 4.0f;
                    }
                }

                // Category drag: a gold line where it would go; the release moves it there and saves the order
                if (catPressed >= 0) {
                    if (!catDragging && leftDown && std::fabs(my - catPressY) > 6.0f) catDragging = true;
                    size_t slot = kCategoryGroupCount; // before the category shown in this slot (or at the end)
                    for (size_t oi = 0; oi < kCategoryGroupCount; ++oi) {
                        if (my < barTopY[oi] + kCatRowH * 0.5f) {
                            slot = oi;
                            break;
                        }
                    }
                    if (catDragging && leftDown) {
                        const float lineY = slot < kCategoryGroupCount ? barTopY[slot] - 1.5f : py + curY - 2.0f;
                        if (lineY > panelY + contentTop - 2.0f && lineY < panelY + contentBottom + 2.0f) {
                            DrawSolidRectAbs(anchor.raw, panelX + 8.0f, lineY - 1.0f, kPanelW - 8.0f - (rowRight - 6.0f),
                                             3.0f, kGold, 0.95f);
                        }
                    }
                    if (!leftDown) {
                        if (catDragging) {
                            size_t from = 0;
                            while (from < kCategoryGroupCount && g_categoryOrder[from] != static_cast<size_t>(catPressed)) {
                                ++from;
                            }
                            if (from < kCategoryGroupCount && slot != from && slot != from + 1) {
                                const size_t moved = g_categoryOrder[from];
                                const size_t to = slot > from ? slot - 1 : slot;
                                std::memmove(g_categoryOrder + from, g_categoryOrder + from + 1,
                                             (kCategoryGroupCount - from - 1) * sizeof(size_t));
                                std::memmove(g_categoryOrder + to + 1, g_categoryOrder + to,
                                             (kCategoryGroupCount - to - 1) * sizeof(size_t));
                                g_categoryOrder[to] = moved;
                                SaveMinimapConfig();
                            }
                        }
                        catPressed = -1;
                        catDragging = false;
                    }
                }

                // Scroll bar: wheel over the panel, drag the thumb, or click the track to page
                if (scrollMax > 0.0f) {
                    const float trackX = panelX + kPanelW - kScrollBarRoom - 4.0f;
                    const float trackY = panelY + contentTop;
                    const float trackH = contentBottom - contentTop;
                    const float thumbH = std::max(28.0f, trackH * (totalH / contentH));
                    const float thumbY = trackY + (trackH - thumbH) * (g_mapKeyScroll / scrollMax);
                    const bool hovTrack = isHovered(trackX - 3.0f, trackY, kScrollBarRoom + 2.0f, trackH);
                    const bool hovThumb = isHovered(trackX - 3.0f, thumbY, kScrollBarRoom + 2.0f, thumbH);
                    if (clicked && hovThumb) {
                        g_mapKeyDragging = true;
                        g_mapKeyDragStartMouseY = my;
                        g_mapKeyDragStartScroll = g_mapKeyScroll;
                    } else if (clicked && hovTrack) {
                        g_mapKeyScroll += (my < thumbY ? -1.0f : 1.0f) * (trackH - kItemRowH);
                    }
                    if (g_mapKeyDragging) {
                        if (!leftDown) {
                            g_mapKeyDragging = false;
                        } else if (trackH > thumbH) {
                            g_mapKeyScroll = g_mapKeyDragStartScroll +
                                (my - g_mapKeyDragStartMouseY) * scrollMax / (trackH - thumbH);
                        }
                    }
                    g_mapKeyScroll = std::max(0.0f, std::min(scrollMax, g_mapKeyScroll));
                    const float drawThumbY = trackY + (trackH - thumbH) * (g_mapKeyScroll / scrollMax);
                    DrawBoxAbs(anchor.raw, kRoundedBoxPrimitive, trackX, trackY, 6.0f, trackH, kCategoryFrame, 0.9f, 3.0f);
                    const bool lit = g_mapKeyDragging || hovThumb;
                    DrawBoxAbs(anchor.raw, kRoundedBoxPrimitive, trackX, drawThumbY, 6.0f, thumbH,
                        lit ? kTitle : kGold, lit ? 0.95f : 0.8f, 3.0f);
                }
                if (hintText) {
                    // A tooltip strip, the panel's full width so every language fits: opaque, gold-framed, on a soft
                    // shadow, the text centred. It shrinks if a translation is long (TextWidth runs ~15% short of
                    // the drawn text, hence the 1.15).
                    // A "\n" in the text starts another line (the presets' tip); each line fits on its own
                    std::vector<std::string> lines;
                    for (const char* s = hintText;;) {
                        const char* nl = std::strchr(s, '\n');
                        lines.emplace_back(s, nl ? static_cast<size_t>(nl - s) : std::strlen(s));
                        if (!nl) break;
                        s = nl + 1;
                    }
                    const float holdHintY = hintY;
                    constexpr float kLineH = 26.0f;
                    const float hintX = panelX + 8.0f, hintW = kPanelW - 16.0f;
                    // A line too long even at 0.8 is wrapped at its spaces (the presets' Ctrl + click line ran out of
                    // the box): no text under the frame in any language
                    float rawW = 0.0f;
                    for (const auto& l : lines) rawW = std::max(rawW, umm_lang::TextWidth(l.c_str()) * 1.15f);
                    const float room = hintW - 28.0f;
                    const float scale = std::max(0.8f, std::min(0.92f, room / std::max(1.0f, rawW)));
                    {
                        std::vector<std::string> wrapped;
                        for (const auto& l : lines) {
                            std::string cur;
                            size_t at = 0;
                            while (at <= l.size()) {
                                size_t sp = l.find(' ', at);
                                if (sp == std::string::npos) sp = l.size();
                                const std::string word = l.substr(at, sp - at);
                                const std::string tryLine = cur.empty() ? word : cur + " " + word;
                                if (!cur.empty() && umm_lang::TextWidth(tryLine.c_str()) * 1.15f * scale > room) {
                                    wrapped.push_back(cur);
                                    cur = word;
                                } else {
                                    cur = tryLine;
                                }
                                at = sp + 1;
                            }
                            wrapped.push_back(cur);
                        }
                        lines.swap(wrapped);
                    }
                    const float kHintH = 32.0f + kLineH * static_cast<float>(lines.size() - 1);
                    DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, hintX - 16.0f, holdHintY - 10.0f, hintW + 32.0f,
                        kHintH + 26.0f, 0xff000000u, 0.8f);
                    DrawFramedBoxAbs(anchor.raw, hintX, holdHintY, hintW, kHintH, kPanelFill, 1.0f, kGold, 0.95f, 5.0f);
                    DrawSolidRectAbs(anchor.raw, hintX + 2.0f, holdHintY + 2.0f, hintW - 4.0f, kHintH - 4.0f, kPanelFill, 1.0f);
                    DrawBoxAbs(anchor.raw, kBoxInnerGlowPrimitive, hintX + 1.0f, holdHintY + 1.0f, hintW - 2.0f, kHintH - 2.0f,
                        kGold, 0.3f, 6.0f);
                    for (size_t i = 0; i < lines.size(); ++i) {
                        const float textW = umm_lang::TextWidth(lines[i].c_str()) * 1.15f * scale;
                        const float textX = hintX + (hintW - textW) * 0.5f;
                        DrawLabelAbs(textX, holdHintY + 3.0f + kLineH * static_cast<float>(i), textW + 12.0f, kLineH,
                            lines[i].c_str(), kActiveText, 1.0f, TextAlignH::Left, TextAlignV::Center, nullptr, scale);
                    }
                }
            }
            // With a controller: a strip under the panel says how it gets in (View), and once in, how to move
            // (only without the game's hint bar, which shows these with its own button icons: map_hints.inl)
            if (g_origXInputGetState && !g_findDialogOpen && !MapHintBarWorks() && (padDrives || UsingGamepad())) {
                const char* padHint = umm_lang::T(padDrives ? umm_lang::Str::PadKeyNav : umm_lang::Str::PadOpenKey);
                constexpr float kPadHintH = 28.0f;
                const float phX = panelX + 8.0f, phW = kPanelW - 16.0f, phY = panelY + totalH + 8.0f;
                const float rawW = umm_lang::TextWidth(padHint) * 1.15f;
                const float scale = std::max(0.55f, std::min(0.85f, (phW - 20.0f) / std::max(1.0f, rawW)));
                DrawFramedBoxAbs(anchor.raw, phX, phY, phW, kPadHintH, kPanelFill, 0.95f, padDrives ? kGold : kGoldDim, 0.9f, 5.0f);
                DrawLabelAbs(phX, phY, phW, kPadHintH, padHint, padDrives ? kActiveText : kText, 1.0f, TextAlignH::Center,
                             TextAlignV::Center, nullptr, scale);
            }
            if (g_settingsOpen && !g_findDialogOpen) {
                if (DrawSettingsCardAbs(anchor.raw, settingsX, settingsY, mx, my, haveCursor, clicked, g_settingsOpen, mapState)) {
                    SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                    *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
                    *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
                }
            }

            g_endWidget(anchor.raw);
        }

        g_endWidget(root.raw);
    }
    g_endLayer(layer);

    if (!g_findDialogOpen) g_padSearchHide = 0;
    // Run the exact native FindDialog (TextDialog) window used by the example Find mod
    if (g_findDialogOpen && g_mapFindDialog) {
        auto keyPressedEdge = [&](int vk) -> bool {
            if (!focused) return false;
            bool down = ((GetAsyncKeyState(vk) & 0x8000) != 0);
            bool edge = down && !g_findKeysWasDown[vk];
            g_findKeysWasDown[vk] = down;
            return edge;
        };

        // A key's edge or a controller press, for each of the search's keys
        static inp::PadPresses s_padFind;
        const inp::PadState padFind = inp::ReadPad();
        const uint16_t padFindPressed = s_padFind.Update(padFind.buttons, GetTickCount64());
        auto findEdge = [&](const inp::Binding& b, int which) {
            static bool was[12] = {};
            const bool down = focused && inp::KeysHeld(b, KeyIsDown);
            const bool fresh = down && !was[which];
            was[which] = down;
            return fresh || (focused && inp::PadPressed(b, padFindPressed, padFind));
        };
        g_padSearchHide = static_cast<uint16_t>(inp::PadButtons(g_keys.findClose) | inp::PadButtons(g_keys.findUp) |
                                                inp::PadButtons(g_keys.findDown) |
                                                (g_findSelectedIndex >= 0 ? inp::PadButtons(g_keys.findConfirm) : 0));
        bool escEdge = findEdge(g_keys.findClose, 0);
        // The search field types Tab as a space (a frame or so later); Tab picks from the list instead.
        static ULONGLONG s_tabTick = 0;
        const bool nextEdge = findEdge(g_keys.findNext, 1), previousEdge = findEdge(g_keys.findPrevious, 2);
        if (nextEdge || previousEdge) s_tabTick = GetTickCount64();

        SyncNativeFindDialogState();
        if (g_findDialogCheckbox && g_findRunDialogFrame) {
            static std::string s_exactKeys;
            s_exactKeys = inp::BindingToText(g_keys.findExact);
            g_findDialogCheckbox(g_mapFindDialog, true, g_findExactMatch, umm_lang::T(umm_lang::Str::ExactMatch), s_exactKeys.c_str());
        }
        int dlgResult = RunNativeFindDialogFrame(g_mapFindDialog);
        if (escEdge || dlgResult == 1) {
            CloseNativeFindDialog();
        } else {
            size_t rawLen = 0;
            const char* rawPtr = GetNativeFindQueryStr(g_mapFindDialog, &rawLen);
            std::string curQuery = (rawPtr && rawLen > 0) ? std::string(rawPtr, rawLen) : std::string();

            // Strip any accidental 'F' keystroke on the opening frame when Shift+F is held
            if (!g_findInputReady) {
                if (!shiftFNow) {
                    g_findInputReady = true;
                }
                if (!curQuery.empty()) {
                    curQuery.clear();
                    SetNativeFindTextFieldValue(g_mapFindDialog, curQuery);
                }
            }

            // Strip any control characters (e.g. 0x7f from Ctrl+Backspace or \t from Tab)
            bool hadCtrlChar = false;
            std::string cleaned;
            cleaned.reserve(curQuery.size());
            for (char c : curQuery) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (uc == 0x7fu) {
                    hadCtrlChar = true;
                    while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
                    while (!cleaned.empty() && cleaned.back() != ' ') cleaned.pop_back();
                } else if (uc < 0x20u) {
                    hadCtrlChar = true;
                } else {
                    cleaned.push_back(c);
                }
            }
            if (hadCtrlChar) {
                curQuery = cleaned;
                SetNativeFindTextFieldValue(g_mapFindDialog, curQuery);
            }
            if (s_tabTick && GetTickCount64() - s_tabTick < 400 && curQuery.size() == g_findQuery.size() + 1 &&
                curQuery.back() == ' ' && curQuery.compare(0, g_findQuery.size(), g_findQuery) == 0) {
                curQuery = g_findQuery;
                SetNativeFindTextFieldValue(g_mapFindDialog, curQuery);
                s_tabTick = 0;
            }

            bool forceRebuild = false;
            if (findEdge(g_keys.findFilters, 3)) {
                g_findRespectFilters = !g_findRespectFilters;
                forceRebuild = true;
            }
            // Exact match: its keys, or a click on Find's checkbox
            bool exactClick = false;
            if (clicked && g_findDialogCheckboxAt && g_findGetUiCursorPos) {
                float cx = -1.0f, cy = -1.0f;
                exactClick = g_findGetUiCursorPos(&cx, &cy) && g_findDialogCheckboxAt(g_mapFindDialog, cx, cy);
            }
            const bool exactKey = findEdge(g_keys.findExact, 9); // (its own slot: 8 is Enter's, and sharing it re-fired the key every frame)
            if (exactKey || exactClick) {
                g_findExactMatch = !g_findExactMatch;
                SaveMinimapConfig();
                forceRebuild = true;
                if (g_log) g_log("Find on the map: Exact match %s (%s), query '%s'", g_findExactMatch ? "ON" : "OFF",
                                 exactKey ? "key" : "click", curQuery.c_str());
            }
#ifdef UMM_DEV_BRIDGE
            {
                static bool s_ctrlE = false;
                const bool ctrlE = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('E') & 0x8000);
                if (ctrlE != s_ctrlE && g_log) g_log("Find on the map: Ctrl+E %s (dialog result %d, binding keys %d)", ctrlE ? "down" : "up",
                                                     dlgResult, g_keys.findExact.keyCount);
                s_ctrlE = ctrlE;
            }
#endif

            if (curQuery != g_findQuery || forceRebuild) {
                g_findQuery = curQuery;
                g_findSelectedIndex = -1;
                g_findHoveredIndex = -1;
                g_findScrollOffset = 0;
                RebuildFindResults(client, mapState);
            }

            int totalRows = static_cast<int>(g_findDisplayRows.size());
            bool downEdge = findEdge(g_keys.findDown, 4);
            bool upEdge = findEdge(g_keys.findUp, 5);
            bool pgDnEdge = findEdge(g_keys.findPageDown, 6);
            bool pgUpEdge = findEdge(g_keys.findPageUp, 7);

            if (totalRows > 0) {
                if (downEdge || nextEdge) {
                    g_findSelectedIndex = (g_findSelectedIndex < 0)
                        ? 0
                        : ((g_findSelectedIndex + 1) % totalRows);
                } else if (upEdge || previousEdge) {
                    g_findSelectedIndex = (g_findSelectedIndex < 0)
                        ? (totalRows - 1)
                        : ((g_findSelectedIndex - 1 + totalRows) % totalRows);
                } else if (pgDnEdge) {
                    g_findSelectedIndex = std::min(totalRows - 1, std::max(0, g_findSelectedIndex) + kMaxFindVisibleRows);
                } else if (pgUpEdge) {
                    g_findSelectedIndex = std::max(0, g_findSelectedIndex - kMaxFindVisibleRows);
                }

                if (g_findSelectedIndex >= 0) {
                    if (g_findSelectedIndex < g_findScrollOffset) {
                        g_findScrollOffset = g_findSelectedIndex;
                    } else if (g_findSelectedIndex >= g_findScrollOffset + kMaxFindVisibleRows) {
                        g_findScrollOffset = g_findSelectedIndex - kMaxFindVisibleRows + 1;
                    }
                }
            }

            // The wheel: Find's own wheel catch only runs for its own search, so the Map Key's wheel hook takes it here
            EnsureMapKeyWheelHook(foreground);
            g_mapKeyWheelCapture = true;
            {
                LONG wheelDelta = InterlockedExchange(&g_mapKeyWheelDelta, 0);
                if (g_findWheelDeltaPtr) wheelDelta += InterlockedExchange(g_findWheelDeltaPtr, 0);
                if (wheelDelta != 0 && totalRows > kMaxFindVisibleRows) {
                    int step = -(wheelDelta / 120);
                    if (step == 0) step = (wheelDelta > 0) ? -1 : 1;
                    int maxScroll = totalRows - kMaxFindVisibleRows;
                    g_findScrollOffset = std::max(0, std::min(maxScroll, g_findScrollOffset + step));
                }
            }

            int visCount = std::min(kMaxFindVisibleRows, std::max(0, totalRows - g_findScrollOffset));
            g_findHoveredIndex = -1;
            if (visCount > 0 && g_findHitTestRow && focused) {
                float uiCurX = -1.0f, uiCurY = -1.0f;
                bool gotCur = false;
                if (g_findGetUiCursorPos) {
                    gotCur = g_findGetUiCursorPos(&uiCurX, &uiCurY);
                } else {
                    POINT pt{};
                    RECT cr{};
                    if (GetCursorPos(&pt) && ScreenToClient(foreground, &pt) &&
                        GetClientRect(foreground, &cr) && cr.bottom > 0) {
                        float scale = 1080.0f / static_cast<float>(cr.bottom);
                        uiCurX = pt.x * scale;
                        uiCurY = pt.y * scale;
                        gotCur = true;
                    }
                }
                if (gotCur) {
                    int hitVis = g_findHitTestRow(g_mapFindDialog, visCount, uiCurX, uiCurY);
                    if (hitVis >= 0 && hitVis < visCount) {
                        g_findHoveredIndex = g_findScrollOffset + hitVis;
                    }
                    // The scroll bar, just right of the lines (Find draws it there): press on it, or drag its
                    // thumb, and the list follows the cursor between the top and bottom line.
                    const int maxScroll = totalRows - kMaxFindVisibleRows;
                    if (maxScroll > 0) {
                        const float probeX = uiCurX - 40.0f;
                        auto lineAt = [&](float y) { return g_findHitTestRow(g_mapFindDialog, visCount, probeX, y); };
                        const bool onBar = hitVis < 0 && lineAt(uiCurY) >= 0;
                        if (clicked && onBar) g_findScrollDragging = true;
                        if (!leftDown) g_findScrollDragging = false;
                        if (g_findScrollDragging) {
                            // The lines' top and bottom, found from Find's own hit test along the probe column
                            float top = uiCurY, bottom = uiCurY;
                            for (float y = uiCurY; y > uiCurY - 600.0f && lineAt(y - 2.0f) >= 0; y -= 2.0f) top = y - 2.0f;
                            for (float y = uiCurY; y < uiCurY + 600.0f && lineAt(y + 2.0f) >= 0; y += 2.0f) bottom = y + 2.0f;
                            static float s_top = 0.0f, s_bottom = 0.0f;
                            if (bottom - top > 20.0f) { s_top = top; s_bottom = bottom; } // off the bar: keep the last
                            if (s_bottom - s_top > 20.0f) {
                                const float t = std::max(0.0f, std::min(1.0f, (uiCurY - s_top) / (s_bottom - s_top)));
                                g_findScrollOffset = static_cast<int>(t * maxScroll + 0.5f);
                            }
                            g_findHoveredIndex = -1;
                        }
                    }
                }
            }
            if (!leftDown) g_findScrollDragging = false;

            auto confirmFindSelection = [&]() {
                if (g_findSelectedIndex >= 0 && g_findSelectedIndex < static_cast<int>(g_findDisplayRows.size())) {
                    const auto& selRow = g_findDisplayRows[g_findSelectedIndex];
                    if (selRow.isToggleFilterRow) {
                        g_findRespectFilters = false;
                        g_findSelectedIndex = -1;
                        g_findScrollOffset = 0;
                        RebuildFindResults(client, mapState);
                        return;
                    }
                    if (!selRow.matchIndices.empty()) {
                        FocusMapOnWorldPositions(client, mapState, selRow.matchIndices);
                        CloseNativeFindDialog();
                        return;
                    }
                }
                if (!g_findMatchedPlaces.empty()) {
                    // Enter lights the best group only (as Find): the whole-text matches if any, else every all-words
                    // match, else the some-words ones with the most words
                    int best = INT_MAX;
                    for (const FindPlaceEntry& e : g_findMatchedPlaces) best = std::min(best, e.matchTier);
                    auto inGroup = [best](int t) {
                        if (best <= kTierWholeText) return t <= kTierWholeText;
                        if (best <= kTierAllWords) return t > kTierWholeText && t <= kTierAllWords;
                        return t == best;
                    };
                    std::vector<size_t> allIndices;
                    for (size_t i = 0; i < g_findMatchedPlaces.size(); ++i)
                        if (inGroup(g_findMatchedPlaces[i].matchTier)) allIndices.push_back(i);
                    FocusMapOnWorldPositions(client, mapState, allIndices);
                    CloseNativeFindDialog();
                } else if (g_findRespectFilters && g_findFilteredOutCount > 0) {
                    g_findRespectFilters = false;
                    g_findSelectedIndex = -1;
                    g_findScrollOffset = 0;
                    RebuildFindResults(client, mapState);
                }
            };

            if (clicked && !g_findScrollDragging && g_findHoveredIndex >= 0 && g_findHoveredIndex < totalRows) {
                uint64_t nowTick = GetTickCount64();
                bool alreadySelected = (g_findSelectedIndex == g_findHoveredIndex);
                bool isDoubleClick = (g_findHoveredIndex == g_lastClickedRow) && ((nowTick - g_lastClickTick) < 600);
                g_lastClickedRow = g_findHoveredIndex;
                g_lastClickTick = nowTick;
                g_findSelectedIndex = g_findHoveredIndex;
                if (g_findDisplayRows[g_findSelectedIndex].isToggleFilterRow || isDoubleClick || alreadySelected) {
                    confirmFindSelection();
                }
            }

            if (g_findDialogOpen && (findEdge(g_keys.findConfirm, 8) || dlgResult == 2)) {
                confirmFindSelection();
            }

            if (g_findDialogOpen) {
                SyncNativeFindDialogState();
            }
        }
    }

    g_leftWasDown = leftDown;

    if (hints) *hints = hintsBefore;
}

// A left press on a pin, then a move or a short hold: the map pans, not the pin's click. The game pans only from a press
// on bare map; on a busy map most presses land on a pin and the map stayed put (2026-11-12). After a press on the map
// (not the Map Key, Find or a marker's menu, no Ctrl / Alt / Shift: those clicks are the mod's), once the mouse has gone
// 6 UI px or 0.25 s has passed with the game's view not moving, the mod takes the drag: the game's map input is skipped
// (no pin click, no menu on release) and the view's anchor follows the mouse, the point pressed staying under it (the
// game's view update and the pan bounds still run). A few frames past the release stay blocked, so the release isn't
// a click.
struct PinPan {
    bool pressed = false, taking = false;
    uint64_t pressTick = 0;
    float mouse0[2]{}, anchor0[2]{};
    int tailFrames = 0;
} g_pinPan;
bool PinPressPan(uint8_t* mapState, bool focused, HWND foreground, bool otherOwner) {
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView || !focused || UsingGamepad()) {
        g_pinPan = {};
        return false;
    }
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                      (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    float mx = 0.0f, my = 0.0f;
    {
        POINT pt{};
        RECT cr{};
        if (!GetCursorPos(&pt) || !ScreenToClient(foreground, &pt) || !GetClientRect(foreground, &cr) || cr.right <= 0 ||
            cr.bottom <= 0 || g_lastRootW <= 0.0f)
            return false;
        mx = g_lastRootX + pt.x * g_lastRootW / static_cast<float>(cr.right);
        my = g_lastRootY + pt.y * g_lastRootH / static_cast<float>(cr.bottom);
    }
    float* anchor = reinterpret_cast<float*>(mapView + 0x1b0);
    if (!down) {
        const bool was = g_pinPan.taking;
        g_pinPan.pressed = g_pinPan.taking = false;
        if (was) g_pinPan.tailFrames = 4;
        if (g_pinPan.tailFrames > 0) {
            --g_pinPan.tailFrames;
            ClearMapViewClickLatch(mapState);
            return true;
        }
        return false;
    }
    if (!g_pinPan.pressed) { // the press: on the map, nothing else's
        if (otherOwner || mods || my < g_lastRootY + 114.0f || my > g_lastRootY + g_lastRootH - 92.0f) {
            g_pinPan.pressed = true, g_pinPan.taking = false, g_pinPan.pressTick = 0; // (held, not ours)
            return false;
        }
        g_pinPan.pressed = true, g_pinPan.taking = false, g_pinPan.pressTick = GetTickCount64();
        g_pinPan.mouse0[0] = mx, g_pinPan.mouse0[1] = my;
        g_pinPan.anchor0[0] = anchor[0], g_pinPan.anchor0[1] = anchor[1];
        return false;
    }
    if (!g_pinPan.pressTick) return false; // a press that wasn't ours
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    if (!(zoom > 0.0001f)) return false;
    if (!g_pinPan.taking) {
        const float moved = std::fabs(mx - g_pinPan.mouse0[0]) + std::fabs(my - g_pinPan.mouse0[1]);
        const bool gamePans = std::fabs(anchor[0] - g_pinPan.anchor0[0]) + std::fabs(anchor[1] - g_pinPan.anchor0[1]) > 0.01f;
        if (gamePans) { // the game has the drag (bare map): leave it
            g_pinPan.pressTick = 0;
            return false;
        }
        if (moved < 6.0f && GetTickCount64() - g_pinPan.pressTick < 250) return false;
        g_pinPan.taking = true;
        if (g_log) g_log("Map: a press on a pin pans the map");
    }
    // the point pressed stays under the mouse: the anchor (and the view's copies of it) follow
    const float ax = g_pinPan.anchor0[0] - (mx - g_pinPan.mouse0[0]) / zoom;
    const float ay = g_pinPan.anchor0[1] - (my - g_pinPan.mouse0[1]) / zoom;
    for (int f : {0x1b0, 0x20, 0x30, 0x1a8}) {
        *reinterpret_cast<float*>(mapView + f) = ax;
        *reinterpret_cast<float*>(mapView + f + 4) = ay;
    }
    ClearMapViewClickLatch(mapState);
    return true;
}

// A left click on one of the mod's own icons (a collectible, an ore, a Wooden Chest, a Town Portal): the game's own menu
// (Send Ping, Set as Waypoint), opened on a stand-in marker the mod fills with the icon's spot and name. The game opens a
// marker's menu by keeping the marker at mapState +0x542e8 (its pin's button: FUN_140bcd... then +0xe0 cleared) and
// builds it from the marker's slot (its place, its type, its title at +0x68). Before, the click went to whatever the
// game had under the cursor: a quest's menu when the icon sat in its search circle (2026-11-12). Ctrl / Alt / Shift
// clicks stay the mod's marks; a drag pans (PinPressPan).
alignas(16) uint8_t g_modIconSlot[0xA0]{};
std::string g_modIconSlotName;
void OpenModIconMenu(uint8_t* mapState, uint8_t* client) {
    float zoom = 0.0f, ox = 0.0f, oy = 0.0f, worldH = 0.0f;
    if (!mapState || !LevelMapView(mapState, zoom, ox, oy, worldH) || !(zoom > 0.0001f)) return;
    const double wx = (g_modHoverIcon.sx - ox) / zoom, wz = worldH - (g_modHoverIcon.sy - oy) / zoom;
    int64_t rawY = 0;
    if (uint8_t* clientWorld = client ? GetClientWorld(client) : nullptr) rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c20);
    g_modIconSlotName = g_modHoverIcon.name;
    uint8_t* m = g_modIconSlot;
    std::memset(m, 0, sizeof(g_modIconSlot));
    static uint32_t s_serial = 0;
    s_serial = (s_serial + 1) & 0xFFFFu;
    *reinterpret_cast<uint32_t*>(m + 0x00) = kModIconMarkerUidPrefix | s_serial;
    *reinterpret_cast<int64_t*>(m + 0x10) = static_cast<int64_t>(wx * 4294967296.0);
    *reinterpret_cast<int64_t*>(m + 0x18) = rawY;
    *reinterpret_cast<int64_t*>(m + 0x20) = static_cast<int64_t>(wz * 4294967296.0);
    *reinterpret_cast<uint32_t*>(m + 0x30) = kSilverChestTypeHash; // (a type the game has: the mod's chest pins')
    m[0x34] = 0;
    m[0x35] = 0xff;
    *reinterpret_cast<uint32_t*>(m + 0x3c) = kSilverChestTypeHash;
    *reinterpret_cast<UiText*>(m + 0x68) = MakeUiText(g_modIconSlotName.c_str());
    // the menu's anchor: the clicked pin's box on the screen (x, y, width, height), as its button reports it (zeroed,
    // the menu opened in the top left corner)
    const float box[4] = {g_modHoverIcon.sx - 12.0f, g_modHoverIcon.sy - 12.0f, 24.0f, 24.0f};
    std::memcpy(mapState + 0xe0, box, sizeof(box));
    *reinterpret_cast<uint8_t**>(mapState + 0x542e8) = m;
    if (g_log) g_log("Map: click menu for %s at %.0f,%.0f", g_modIconSlotName.c_str(), wx, wz);
}
// A hidden quest / rumor pin the mod names (g_questPinNamed): the game's own menu opened on that marker itself, so the
// game's Go to Journal is there (the game never takes a click on a pin it keeps hidden, 2026-11-12)
const uint8_t* g_clickPin = nullptr;
float g_clickPinAt[2]{};
void OpenPinMenu(uint8_t* mapState) {
    if (!mapState || !g_clickPin) return;
    const float box[4] = {g_clickPinAt[0] - 14.0f, g_clickPinAt[1] - 14.0f, 28.0f, 28.0f};
    std::memcpy(mapState + 0xe0, box, sizeof(box));
    *reinterpret_cast<const uint8_t**>(mapState + 0x542e8) = g_clickPin;
    if (g_log) g_log("Map: click menu for the hidden pin %p", g_clickPin);
}
bool ModIconClick(uint8_t* mapState, bool focused, bool otherOwner) {
    static bool s_mine = false, s_wasDown = false, s_openNext = false;
    static float s_at[2]{};
    // (opened the frame after the release: that frame's input block clears the map's click state, the menu's too)
    if (s_openNext && !s_mine) {
        s_openNext = false;
        if (!otherOwner) {
            __try {
                if (g_clickPin) OpenPinMenu(mapState);
                else OpenModIconMenu(mapState, GetUiClient());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    const bool down = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                      (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool block = false;
    const bool pinNamed = g_questPinNamed && g_questPinNamed == g_hoveredGameMarker && GetTickCount64() - g_questPinNamedTick < 150ULL;
    if (down && !s_wasDown && !otherOwner && !mods && g_mapCursorIn &&
        (pinNamed || (ModIconHovered() && !GameIconUnderCursor() && GetTickCount64() - g_modHoverIcon.tick < 150ULL &&
                      !g_modHoverIcon.name.empty()))) {
        s_mine = true;
        s_at[0] = g_mapCursor[0], s_at[1] = g_mapCursor[1];
        g_clickPin = pinNamed ? g_questPinNamed : nullptr;
        if (pinNamed) g_clickPinAt[0] = g_hoveredIcon[0], g_clickPinAt[1] = g_hoveredIcon[1];
    }
    if (s_mine) {
        block = true;
        ClearMapViewClickLatch(mapState);
        if (!down) {
            s_mine = false;
            const bool moved = std::fabs(g_mapCursor[0] - s_at[0]) + std::fabs(g_mapCursor[1] - s_at[1]) > 6.0f;
            if (!moved && !g_pinPan.taking) s_openNext = true;
        }
    }
    s_wasDown = down;
    return block;
}

// True while the Find dialog, its closing frames or our panel own the mouse and keys: the game's
// world map input is skipped for this frame.
bool BlockWorldMapInput(uint8_t* mapState) {
    bool blockMapInput = false;
    __try {
        HWND foreground = GetForegroundWindow();
        DWORD fgPid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
        bool focused = (fgPid == GetCurrentProcessId()) || inp::GameHasFocus();
        // A dialog open on the map (a marker's menu, a custom pin's name: mapState +0xf8 not 0) is the game's: Shift+F
        // there is typing, and clearing the click state below would close it
        const bool mapDialogOpen = mapState && *reinterpret_cast<uint32_t*>(mapState + 0xf8) != 0;
        const bool findHere = !(halls::MapActive() && !halls::g_mapOverworld); // not on a dungeon's own map
        bool shiftFNow = focused && !mapDialogOpen && findHere && BindingDown(g_keys.findOpen);

        if (g_waitReleaseAfterFindClose) {
            bool anyHeld = focused && (
                ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) ||
                ((GetAsyncKeyState('F') & 0x8000) != 0));
            if (anyHeld) {
                g_findClosingFrames = std::max(g_findClosingFrames, 8);
            } else if (g_findClosingFrames <= 0) {
                g_waitReleaseAfterFindClose = false;
            }
        }

        bool cursorInPanel = g_panelCurrentlyHovered;
        const bool padInUse = UsingGamepad();
        if (padInUse) cursorInPanel = false; // the hidden mouse cursor is not where the player is pointing
        if (focused && !padInUse && g_lastPanelW > 0.0f && g_lastPanelH > 0.0f) {
            POINT pt{};
            RECT cr{};
            if (GetCursorPos(&pt) && ScreenToClient(foreground, &pt) &&
                GetClientRect(foreground, &cr) && cr.right > 0 && cr.bottom > 0) {
                float mx = g_lastRootX + pt.x * g_lastRootW / static_cast<float>(cr.right);
                float my = g_lastRootY + pt.y * g_lastRootH / static_cast<float>(cr.bottom);
                cursorInPanel = (mx >= g_lastPanelX && mx <= g_lastPanelX + g_lastPanelW &&
                                 my >= g_lastPanelY && my <= g_lastPanelY + g_lastPanelH) ||
                                (g_lastSettingsW > 0.0f &&
                                 mx >= g_lastSettingsX && mx <= g_lastSettingsX + g_lastSettingsW &&
                                 my >= g_lastSettingsY && my <= g_lastSettingsY + g_lastSettingsH);
            }
        }

        bool mouseBtnHeld = focused && (
            ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0));
        if (cursorInPanel && mouseBtnHeld) {
            g_panelClickLatch = true;
            g_findClosingFrames = std::max(g_findClosingFrames, 8);
        } else if (g_panelClickLatch) {
            if (mouseBtnHeld) {
                g_findClosingFrames = std::max(g_findClosingFrames, 6);
            } else if (g_findClosingFrames <= 0) {
                g_panelClickLatch = false;
            }
        }

        if (PinPressPan(mapState, focused, foreground, cursorInPanel || g_findDialogOpen || shiftFNow || mapDialogOpen))
            blockMapInput = true;
        if (ModIconClick(mapState, focused, cursorInPanel || mapDialogOpen || g_findDialogOpen)) blockMapInput = true;

        // Typing in the Map Key's search box: the map's keys (C recenters, Esc closes) are off, as with Find
        if (g_findDialogOpen || g_findClosingFrames > 0 || g_waitReleaseAfterFindClose ||
            g_panelClickLatch || shiftFNow || cursorInPanel || g_keySearchFocused) {
            blockMapInput = true;
        }

        if (blockMapInput && mapState) {
            if (g_updateMapViewport) {
                void* mapView = *reinterpret_cast<void**>(mapState + 8);
                if (mapView) {
                    alignas(16) uint8_t dummyAction[64]{};
                    g_updateMapViewport(mapView, dummyAction, 1);
                }
            }
            // Clear any latched click state set inside 0x140d3bf80 before suppressInput was checked (not a dialog's)
            if (!mapDialogOpen) ClearMapViewClickLatch(mapState);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        blockMapInput = false;
    }
    return blockMapInput && mapState;
}


#ifdef UMM_DEV_BRIDGE
// dev (map op "questat"): the markers within 30 m of a spot, their id and the quests the game has for them; run in a
// World Map frame (the game's quest lookups aren't safe from the bridge)
float g_questAt[2]{};
bool g_questAtPending = false;
// The u32s in a quest's first 0x200 bytes that read as game text (its name among them), and the strings it points at
void DumpQuestText(const uint8_t* q) {
    for (int off = 0; off < 0x200; off += 4) {
        __try {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(q + off);
            char text[160]{};
            if (v > 0x10000 && ReadLocaText(v, text, sizeof(text)) && text[0] && g_log) g_log("Quest at:   +%x = %08x: %s", off, v, text);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void RunQuestAt(uint8_t* client) {
    if (!g_questAtPending || !client) return;
    g_questAtPending = false;
    const float qx = g_questAt[0], qz = g_questAt[1];
    __try {
            constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
            for (size_t a = 0; a < 4; ++a) {
                uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
                const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
                if (!base || count == 0 || count > 10000) continue;
                for (uint64_t i = 0; i < count; ++i) {
                    const uint8_t* m = base + i * 0xA0;
                    const float dx = *reinterpret_cast<const int32_t*>(m + 0x14) - qx, dz = *reinterpret_cast<const int32_t*>(m + 0x24) - qz;
                    if (dx * dx + dz * dz > 30.0f * 30.0f) continue;
                    uint32_t id = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
                    if (!id) id = *reinterpret_cast<const uint32_t*>(m + 0x08);
                    const void* act = g_getActiveQuestForMarkerId && id ? g_getActiveQuestForMarkerId(id) : nullptr;
                    const void* any = g_getQuestByMarkerId && id ? g_getQuestByMarkerId(id) : nullptr;
                    if (g_log) g_log("Quest at: a%zu#%llu orig %08x type %08x id %08x at %d,%d: active %p any %p rumor %d", a,
                                     static_cast<unsigned long long>(i), GetMarkerOrigTypeHash(m), *reinterpret_cast<const uint32_t*>(m + 0x30), id,
                                     *reinterpret_cast<const int32_t*>(m + 0x14), *reinterpret_cast<const int32_t*>(m + 0x24), act, any,
                                     LoreRumorFound(client, id));
                    if (any) DumpQuestText(static_cast<const uint8_t*>(any));
                }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_log) g_log("Quest at: faulted");
    }
}
#endif

// A dungeon map: the menu's grey over the whole screen in the map's own root widget, under the menu bars (the game's
// overworld terrain is off there, see hall_pan::InstallTerrainSkip, so the bars show grey through)
void DrawUnderBars(void* mapRootWidget) {
    __try {
        if (g_overlayRootRect[2] > 100.0f)
            DrawSolidRectAbs(mapRootWidget, g_overlayRootRect[0], g_overlayRootRect[1], g_overlayRootRect[2],
                             g_overlayRootRect[3], 0xff2b2b2bu, 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void WorldMapInputHook(void* mapRootWidget, uint8_t* mapState) {
    {
        greeneye::Lease lease; // released before the game's code runs on
        if (lease && g_worldMapModded && BlockWorldMapInput(mapState)) return;
    }
    if (g_origWorldMapInput) {
        g_origWorldMapInput(mapRootWidget, mapState);
    }
    greeneye::Lease lease;
    if (lease && halls::MapActive()) DrawUnderBars(mapRootWidget);
}

// A fault skips the rest of the overlay that frame: logged once with its address, so the cause can be found
void LogOverlayFault(unsigned code, const void* at) {
    static bool logged = false;
    if (logged || !g_log) return;
    logged = true;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(at), &mod);
    g_log("Map Key: the overlay faulted (%08x at %p, %s+0x%llx); the rest of it skipped", code, at,
          mod == reinterpret_cast<HMODULE>(base) ? "enshrouded.exe" : "a module",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(mod)));
}
void SafeDrawMapKeyOverlayInner(void* mapState) {
    EXCEPTION_POINTERS* info = nullptr;
    __try {
        DrawMapKeyOverlay(mapState);
    } __except (info = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER) {
        if (info && info->ExceptionRecord)
            LogOverlayFault(info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    }
}
void SafeDrawMapKeyOverlay(void* mapState) {
    UMM_PERF(PerfMapKey);
    SafeDrawMapKeyOverlayInner(mapState);
}


uint64_t g_deskHintsTick = 0; // when the key-hint hook last drew the desk
void* g_deskMapState = nullptr;
// The map desk under the map (desk.inl), kept apart so a fault can't take the map with it
float* UiOpacityTop(void* widget); // regions.inl
void ReplayLabel(void* widget, const RingDraw& d) {
    float* op = d.opacity < 1.0f ? UiOpacityTop(widget) : nullptr;
    const float was = op ? *op : 1.0f;
    if (op) *op = was * d.opacity;
    __try {
        DrawLabelAbs(d.x, d.y, d.w, d.h, d.text.c_str(), d.rgba, d.alpha, static_cast<TextAlignH>(d.hAlign),
                     static_cast<TextAlignV>(d.vAlign), d.style, d.brightness);
    } __finally {
        if (op) *op = was;
    }
}
// The icons held back from over the ring (UnderRing), drawn into the map layer's widget at the desk's sort key
void DrawUnderRing(void* widget) {
    g_ringDrawing.swap(g_ringQueue);
    g_ringQueue.clear();
    g_ringReplay = g_ringReplayFlag = true;
    g_mapLayerTick = GetTickCount64();
    if (g_roadsUnder && g_roadsMapState && g_worldMapModded) roads::DrawOnMap(widget, g_roadsRect, g_roadsMapState); // first: under the icons
    g_roadsUnder = false;
    for (const RingDraw& d : g_ringDrawing) {
        if (d.kind == 1) DrawIconRectAbs(widget, d.iconRef, d.x, d.y, d.w, d.h, d.rgba, d.alpha, d.brightness);
        else if (d.kind == 0) DrawAssetRectAbs(widget, d.asset, d.x, d.y, d.w, d.h, d.rgba, d.alpha, d.brightness);
        else if (d.kind == 3) DrawBoxAbs(widget, d.asset, d.x, d.y, d.w, d.h, d.rgba, d.alpha, d.border, d.brightness);
        else if (d.kind == 4) DrawSolidRectAbs(widget, d.x, d.y, d.w, d.h, d.rgba, d.alpha, d.brightness);
        else ReplayLabel(widget, d);
    }
    g_ringReplay = g_ringReplayFlag = false;
    g_ringDrawing.clear();
}

void SafeDeskDraw(void* mapState) {
    __try {
        desk::DrawUnder(static_cast<uint8_t*>(mapState), g_overlayRootRect);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_mapDesk = false;
        if (g_log) g_log("Map desk: a fault while drawing; off for this session");
    }
}
void DrawMapFilterMenuHook(void* mapState) {
    // Note: We intentionally do NOT call g_origDrawMapFilterMenu(mapState) so the vanilla
    // "Filter Map Markers" / "Hidden types: 0" box is cleanly replaced by our Map Key & Filters panel
    // instead of rendering and highlighting behind it. An older generation after a hot reload
    // calls on instead.
    {
        greeneye::Lease lease;
        if (lease) g_mapMenuDrawnTick = GetTickCount64();
        if (lease && !g_worldMapModded) desk::HideGameEdge(false); // the game's own map: its edge as it was
        if (lease && !g_worldMapModded && g_vanillaWorldMap && !g_modOff && g_origXInputGetState) {
            if (g_padKeyFocus) { // it was the pad that switched to the game's map
                g_padKeyFocus = false;
                for (auto& sw : g_padSwallow) sw = 0xFFFF;
            }
            if (ReadPadPresses().pressed & inp::PadButtons(g_keys.padMapKey)) g_padVanillaOffPending = true;
        }
        if (lease && g_worldMapModded) {
            // the desk: from the key-hint hook when the hints draw this frame (before them, so their key caps come after
            // it), else here
            g_deskMapState = mapState;
            if (GetTickCount64() - g_deskHintsTick > 80) SafeDeskDraw(mapState); // (no hints this frame: Find open)
            SafeDrawMapKeyOverlay(mapState);
            return;
        }
    }
    if (g_origDrawMapFilterMenu) {
        g_origDrawMapFilterMenu(mapState);
    }
}

// ============================================================================
// HUD Minimap & In-Game Time of Day (PlayState UI Hook)
// ============================================================================

struct MinimapZoomSpec {
    float zoomMult;
    float radiusMeters;
    const char* label;
};

constexpr MinimapZoomSpec kMinimapZoomLevels[] = {
    {0.50f, 600.0f, "0.5x"},
    {0.75f, 400.0f, "0.75x"},
    {1.00f, 275.0f, "1.0x"},
    {1.50f, 185.0f, "1.5x"},
    {2.00f, 135.0f, "2.0x"},
    {3.00f,  90.0f, "3.0x"},
    {4.00f,  60.0f, "4.0x"},
};
constexpr int kMinimapZoomCount = static_cast<int>(sizeof(kMinimapZoomLevels) / sizeof(kMinimapZoomLevels[0]));
// In an Ancient Spire (some 70 m across) the minimap has its own scale: its widest is the overworld's closest, and it
// zooms in further (requested 2026-11-09). Kept apart from the overworld's zoom, saved as SpireZoomIdx.
constexpr float kSpireZoomRadii[] = {60.0f, 45.0f, 32.0f, 22.0f, 15.0f};
constexpr int kSpireZoomCount = static_cast<int>(sizeof(kSpireZoomRadii) / sizeof(kSpireZoomRadii[0]));
int g_minimapSpireZoomIdx = 1;
bool g_minimapInSpire = false; // last frame's (the minimap's zoom is read before this frame's check)

bool g_minimapConfigLoaded = false;
bool g_minimapCustomPos = false;
float g_minimapPosX = 0.0f;
float g_minimapPosY = 0.0f;
float g_minimapSize = 236.0f;
constexpr float kMinimapMaxSize = 900.0f;
int g_minimapZoomIdx = 2; // Default 1.0x (275m radius)
int& MinimapZoom() { return g_minimapInSpire ? g_minimapSpireZoomIdx : g_minimapZoomIdx; }
int MinimapZoomCount() { return g_minimapInSpire ? kSpireZoomCount : kMinimapZoomCount; }
float MinimapRadius() { return g_minimapInSpire ? kSpireZoomRadii[g_minimapSpireZoomIdx] : kMinimapZoomLevels[g_minimapZoomIdx].radiusMeters; }

bool g_minimapDraggingMove = false;
bool g_minimapDraggingResize = false;
float g_minimapDragStartMouseX = 0.0f;
float g_minimapDragStartMouseY = 0.0f;
float g_minimapDragStartPosX = 0.0f;
float g_minimapDragStartPosY = 0.0f;
float g_minimapDragStartSize = 236.0f;
bool g_minimapMouseWasDown = false;
bool g_minimapZoomInKeyWasDown = false;
bool g_minimapZoomOutKeyWasDown = false;
uint64_t g_minimapLastHeaderClickTick = 0;

// Shift+Alt mouse control state (locks camera rotation and swallows game clicks)
HWND g_minimapSubclassedHwnd = nullptr;
WNDPROC g_minimapOrigWndProc = nullptr;
bool g_minimapMouseLocked = false;
float g_minimapSavedMouseScale = 0.0017453292f;
uint8_t g_minimapSavedCursorIndex = 0; // winObj + 0xa8 before the minimap took the mouse (0 play, 1 menus)
volatile LONG g_minimapWheelDelta = 0;

LRESULT CALLBACK MinimapWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_minimapMouseLocked) {
        switch (msg) {
            case WM_MOUSEWHEEL:
                InterlockedExchangeAdd(&g_minimapWheelDelta, GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
                return 0;
            default:
                break;
        }
    }
    if (g_minimapOrigWndProc) {
        return CallWindowProcW(g_minimapOrigWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SetMinimapMouseControl(bool enable, HWND hwnd) {
    if (!enable) {
        if (!g_minimapMouseLocked && !g_minimapSubclassedHwnd) return;
        HWND targetHwnd = g_minimapSubclassedHwnd ? g_minimapSubclassedHwnd : hwnd;
        if (g_minimapMouseLocked && targetHwnd) {
            auto* winObj = reinterpret_cast<uint8_t*>(GetWindowLongPtrW(targetHwnd, 0));
            if (winObj && *reinterpret_cast<HWND*>(winObj + 8) == targetHwnd) {
                if (*reinterpret_cast<float*>(winObj + 0x68) == 0.0f) {
                    *reinterpret_cast<float*>(winObj + 0x68) = g_minimapSavedMouseScale;
                }
                // Give back the cursor state it had: hidden / locked while playing (0), the menu cursor (1) in the
                // backpack and other menus
                winObj[0xa8] = g_minimapSavedCursorIndex;
                if (auto* cursorTable = *reinterpret_cast<uint8_t**>(winObj)) {
                    SetCursor(*reinterpret_cast<HCURSOR*>(cursorTable + (g_minimapSavedCursorIndex ? 0x298 : 0x290)));
                } else {
                    SetCursor(nullptr);
                }
            }
            g_minimapMouseLocked = false;
        }
        if (g_minimapSubclassedHwnd && g_minimapOrigWndProc) {
            SetWindowLongPtrW(g_minimapSubclassedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_minimapOrigWndProc));
            g_minimapOrigWndProc = nullptr;
            g_minimapSubclassedHwnd = nullptr;
        }
        return;
    }

    if (!hwnd) return;
    if (g_minimapSubclassedHwnd != hwnd) {
        if (g_minimapSubclassedHwnd && g_minimapOrigWndProc) {
            SetWindowLongPtrW(g_minimapSubclassedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_minimapOrigWndProc));
        }
        g_minimapOrigWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MinimapWindowProc)));
        g_minimapSubclassedHwnd = hwnd;
    }
    auto* winObj = reinterpret_cast<uint8_t*>(GetWindowLongPtrW(hwnd, 0));
    if (winObj && *reinterpret_cast<HWND*>(winObj + 8) == hwnd) {
        float curScale = *reinterpret_cast<float*>(winObj + 0x68);
        if (curScale > 1e-6f) {
            g_minimapSavedMouseScale = curScale;
        }
        *reinterpret_cast<float*>(winObj + 0x68) = 0.0f;
        if (!g_minimapMouseLocked) g_minimapSavedCursorIndex = winObj[0xa8];

        // Set winObj[0xa8] = 1 (the game's native Backpack/UI cursor index!):
        // 1) Prevents WM_MOUSEMOVE (0x140f905d1) from calling SetCursorPos to lock the cursor in place
        // 2) Causes the game to display its native Backpack HCURSOR from *(*winObj + 0x298)
        winObj[0xa8] = 1;
        if (auto* cursorTable = *reinterpret_cast<uint8_t**>(winObj)) {
            if (HCURSOR hBackpackCursor = *reinterpret_cast<HCURSOR*>(cursorTable + 0x298)) {
                SetCursor(hBackpackCursor);
            }
        }
    }
    g_minimapMouseLocked = true;
    ClipCursor(nullptr);
}

// The mod's folder, not the DLL's: a hot-reloaded generation runs from a shadow copy in .hot\.
std::string GetMinimapConfigPath() {
    return greeneye::DataPath("minimap_config.ini");
}

// [Keys] from the file; writeMissing (the first read) adds the ones it lacks, so they can be found there.
void ReadMapKeys(bool writeMissing) {
    const std::string cfg = GetMinimapConfigPath();
    static char section[16384];
    section[0] = 0;
    GetPrivateProfileSectionA("Keys", section, sizeof(section), cfg.c_str());
    auto lookup = [&](const char* name) -> const char* {
        const size_t len = std::strlen(name);
        for (const char* e = section; *e; e += std::strlen(e) + 1)
            if (_strnicmp(e, name, len) == 0 && e[len] == '=') return e + len + 1;
        return nullptr;
    };
    MapKeys fresh;
    for (const MapKeySetting& s : kMapKeySettings) {
        const inp::Binding fallback = inp::MakeBinding(s.defaultText);
        const char* text = lookup(s.name);
        fresh.*s.field = fallback;
        if (!text) {
            if (writeMissing) WritePrivateProfileStringA("Keys", s.name, s.defaultText, cfg.c_str());
            continue;
        }
        std::string why;
        if (!inp::ParseBinding(text, fallback, fresh.*s.field, &why) && g_log)
            g_log("Settings: [Keys] %s: '%s' is not a key or button this mod knows; using %s", s.name, why.c_str(), s.defaultText);
    }
    // The key that marks a chest looted: E, or as the game's Interact was set; [Chests] from before [Keys].
    char interact[128]{};
    if (!GetPrivateProfileStringA("Chests", "InteractKey", "", interact, sizeof(interact), cfg.c_str()) && writeMissing) {
        WritePrivateProfileStringA("Chests", "InteractKey", "E", cfg.c_str());
        WritePrivateProfileStringA("Chests", "RespawnMinutes", "30", cfg.c_str());
    }
    const inp::Binding interactFallback = inp::MakeBinding("E");
    if (!inp::ParseBinding(interact, interactFallback, fresh.interact)) fresh.interact = interactFallback;
    g_keys = fresh;
    // The panel key's code for the map's hint bar; a key it has no code for shows no hint
    int vk = 0;
    uint16_t hid = 0;
    if (fresh.panelToggle.keyCount) ParseKeyName(inp::KeyToText(fresh.panelToggle.keys[0].vk).c_str(), vk, hid);
    g_panelToggleVk = hid ? vk : 0;
    g_panelToggleHid = hid;
    g_padChords[0].Set(fresh.glow);
    g_padChords[1].Set(fresh.findOpen);
    g_padChords[2].Set(fresh.miniZoomIn);
    g_padChords[3].Set(fresh.miniZoomOut);
    g_padEnter.Set(fresh.padMapKey);
    g_padMapChords[0].Set(fresh.padMarkChest);
    g_padMapChords[1].Set(fresh.padHideRow);
    g_padMapChords[2].Set(fresh.undo);
    g_padMapChords[3].Set(fresh.redo);
}


// The game's graphics preset is Ultra: "render": { "preset": "Ultra" } in local_settings.json
// (Saved Games, keen games, Enshrouded; the Map Desk's large images are meant for it)
bool GameGraphicsUltra() {
    PWSTR saved = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) || !saved) return false;
    std::wstring path = std::wstring(saved) + L"\\keen games\\Enshrouded\\local_settings.json";
    CoTaskMemFree(saved);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::string text(65536, '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f));
    std::fclose(f);
    const size_t render = text.find("\"render\"");
    const size_t preset = render == std::string::npos ? render : text.find("\"preset\"", render);
    if (preset == std::string::npos) return false;
    const size_t open = text.find('"', text.find(':', preset));
    if (open == std::string::npos) return false;
    const bool ultra = text.compare(open + 1, 6, "Ultra\"") == 0;
    if (g_log) g_log("Map desk: the game's graphics preset is %sUltra: Map Desk %s by default", ultra ? "" : "not ",
                     ultra ? "on" : "off");
    return ultra;
}

void LoadMinimapConfig() {
    // A hand edit of [Keys] applies within two seconds. The file is only looked at that often, and read once.
    {
        static uint64_t keysCheckedAt = 0;
        static inp::FileWatch keysWatch;
        const uint64_t tick = GetTickCount64();
        if (g_minimapConfigLoaded && tick - keysCheckedAt >= 2000) {
            keysCheckedAt = tick;
            if (keysWatch.Changed(GetMinimapConfigPath().c_str())) ReadMapKeys(false);
        }
    }
    // The text language: decided once Steam is up (every frame until then), then logged once.
    static bool languageLogged = false;
    if (!languageLogged) {
        const std::string cfgPath = GetMinimapConfigPath();
        umm_lang::EnsureLanguage(cfgPath.c_str());
        if (umm_lang::Decided()) {
            languageLogged = true;
            if (g_log) g_log("Language: %s", umm_lang::SteamCode(umm_lang::Current()));
        }
    }
    if (g_minimapConfigLoaded) return;
    g_minimapConfigLoaded = true;
    // The writer's queue on disk before reading. Here and not above: this function runs every frame, and a flush holds the
    // game's thread until the queue is written, which froze the game and undid writing in the background.
    async_io::Flush();
    std::string cfg = GetMinimapConfigPath();
    // Show the setting in the file: Language=auto follows the game, or a Steam code such as german
    char language[32]{};
    if (GetPrivateProfileStringA("General", "Language", "", language, sizeof(language), cfg.c_str()) == 0) {
        WritePrivateProfileStringA("General", "Language", "auto", cfg.c_str());
    }
    int customPos = GetPrivateProfileIntA("MinimapV2", "CustomPos", 0, cfg.c_str());
    // As text: GetPrivateProfileInt reads a negative value (dragged past the left or top edge) as 0
    constexpr int kNoPos = -100000;
    auto readPos = [&](const char* key) {
        char text[24]{};
        GetPrivateProfileStringA("MinimapV2", key, "", text, sizeof(text), cfg.c_str());
        return text[0] ? std::atoi(text) : kNoPos;
    };
    int posX = readPos("PosX");
    int posY = readPos("PosY");
    int sizePx = GetPrivateProfileIntA("MinimapV2", "Size", 236, cfg.c_str());
    int zoomIdx = GetPrivateProfileIntA("MinimapV2", "ZoomIdx", 2, cfg.c_str());
    int otherPlayers = GetPrivateProfileIntA("MinimapV2", "OtherPlayers", 1, cfg.c_str());
    const int townPortals = GetPrivateProfileIntA("MinimapV2", "TownPortals", 1, cfg.c_str());
    int showTerrain = GetPrivateProfileIntA("MinimapV2", "ShowTerrain", 1, cfg.c_str());
    int thinBorder = GetPrivateProfileIntA("MinimapV2", "ThinBorder", 0, cfg.c_str());
    const int headingUp = GetPrivateProfileIntA("MinimapV2", "HeadingUp", 0, cfg.c_str());
    int infoBar = GetPrivateProfileIntA("MinimapV2", "InfoBar", 1, cfg.c_str());
    g_minimapShowPosition = GetPrivateProfileIntA("MinimapV2", "ShowPosition", 0, cfg.c_str()) != 0;
    g_regionFilterEnabled = GetPrivateProfileIntA("MinimapV2", "RegionFilter", 0, cfg.c_str()) != 0;
    // Level Areas took the place of Region Colors (1.3.26): whoever had the regions tinted gets the level areas
    g_levelAreas = GetPrivateProfileIntA("MinimapV2", "LevelAreas",
                                         GetPrivateProfileIntA("MinimapV2", "RegionColors", 0, cfg.c_str()), cfg.c_str()) != 0;
    g_levelTintPct = static_cast<int>(GetPrivateProfileIntA("MinimapV2", "LevelTint", 50, cfg.c_str())) / 10 * 10;
    g_levelTintPct = std::max(10, std::min(80, g_levelTintPct));
    g_regionNames = GetPrivateProfileIntA("MinimapV2", "RegionNames", 1, cfg.c_str()) != 0;
    g_vanillaWorldMap = GetPrivateProfileIntA("MinimapV2", "VanillaWorldMap", 0, cfg.c_str()) != 0;
    g_vanillaMinimap = GetPrivateProfileIntA("MinimapV2", "VanillaMinimap", 0, cfg.c_str()) != 0;
    g_dungeonMaps = GetPrivateProfileIntA("MinimapV2", "DungeonMaps", 1, cfg.c_str()) != 0;
    g_nearCreatures = GetPrivateProfileIntA("MinimapV2", "NearbyCreatures", 0, cfg.c_str()) != 0;
    g_nearCreaturesHideMap = GetPrivateProfileIntA("MinimapV2", "NearbyCreaturesHideMap", 0, cfg.c_str()) != 0;
    g_findExactMatch = GetPrivateProfileIntA("MinimapV2", "FindExactMatch", 0, cfg.c_str()) != 0;
    // Map Desk: off unless the game's graphics are on Ultra, until the player sets it
    const int mapDesk = static_cast<int>(GetPrivateProfileIntA("MinimapV2", "MapDesk", -1, cfg.c_str()));
    g_mapDesk = mapDesk < 0 ? GameGraphicsUltra() : mapDesk != 0;
    // The regions shown, a bit each; before 1.3.19 one region (RegionFocus, -1 all)
    const int regionMask = static_cast<int>(GetPrivateProfileIntA("MinimapV2", "RegionMask", -1, cfg.c_str()));
    if (regionMask >= 0) {
        g_regionMask = static_cast<uint32_t>(regionMask) & kAllRegionsMask;
    } else {
        const int focus = static_cast<int>(GetPrivateProfileIntA("MinimapV2", "RegionFocus", -1, cfg.c_str()));
        g_regionMask = focus >= 0 && focus < static_cast<int>(kRegionCount) ? 1u << focus : kAllRegionsMask;
    }
    g_regionDrawerOpen = GetPrivateProfileIntA("MinimapV2", "RegionDrawerOpen", 1, cfg.c_str()) != 0;
    g_regionOutsideHide = GetPrivateProfileIntA("MinimapV2", "RegionOutsideHide", 0, cfg.c_str()) != 0;
    int showMinimap = GetPrivateProfileIntA("MinimapV2", "ShowMinimap", 1, cfg.c_str());
    int clockWeather = GetPrivateProfileIntA("MinimapV2", "ClockWeather", 0, cfg.c_str());
    int lootedChestMode = GetPrivateProfileIntA("MinimapV2", "LootedChestMode", 0, cfg.c_str());
    int markerScaleIdx = GetPrivateProfileIntA("MinimapV2", "CustomMarkerScaleIdx", 2, cfg.c_str());
    int pinRevealMode = GetPrivateProfileIntA("MinimapV2", "PinRevealMode", 2, cfg.c_str());
    // The fog mode as last chosen (a hot reload keeps the running one instead)
    const int fogMode = GetPrivateProfileIntA("MinimapV2", "FogRevealMode", -1, cfg.c_str());
    if (fogMode >= 0 && fogMode <= 2 && !g_fogModeFromHotReload) ApplyFogRevealMode(static_cast<FogRevealMode>(fogMode));
    int respawnMins = GetPrivateProfileIntA("Chests", "RespawnMinutes", 30, cfg.c_str());
    // ResetRadius: before 1.3.16 the mod's stand-in for the game's chunk rule (500 written as the default); now only an
    // extra hold on top of that rule, 0 by default
    int resetRadius = GetPrivateProfileIntA("Chests", "ResetRadius", -1, cfg.c_str());
    if (resetRadius < 0 || resetRadius == 500) {
        resetRadius = 0;
        WritePrivateProfileStringA("Chests", "ResetRadius", "0", cfg.c_str());
    }
    // [Mining] Ores = one 0/1 per ore, in the Map Key's order
    char ores[kOreCount + 64]{};
    GetPrivateProfileStringA("Mining", "Ores", "", ores, sizeof(ores), cfg.c_str());
    // 1.3.3 split Fertilised Farm Soil off Farm Soil, one row after it: an older 86-row string gets that row's bit
    // copied from Farm Soil so every later row keeps its setting. Rows added at the end since (Return Beacon) are
    // simply off in an older, shorter string.
    constexpr size_t kRowsBefore133 = 86;
    if (std::strlen(ores) == kRowsBefore133) {
        constexpr size_t kFertilised = static_cast<size_t>(Ore::FertilisedFarmSoil);
        static_assert(kFertilised == static_cast<size_t>(Ore::FarmSoil) + 1, "Fertilised follows Farm Soil");
        std::memmove(ores + kFertilised + 1, ores + kFertilised, kRowsBefore133 - kFertilised + 1);
        ores[kFertilised] = ores[kFertilised - 1];
    }
    for (size_t o = 0; o < kOreCount; ++o) g_oreEnabled[o] = ores[o] == '1';
    ReadMapKeys(true);

    // Any place it was moved to, the screen's edges too (it was dropped within 10 px of the left or top edge: a minimap
    // in the lower-left corner went back to its default place on every start; reported by Grezexx)
    g_minimapCustomPos = (customPos != 0 && posX > kNoPos && posY > kNoPos);
    if (g_minimapCustomPos) {
        g_minimapPosX = static_cast<float>(posX);
        g_minimapPosY = static_cast<float>(posY);
    }
    g_minimapSize = std::max(150.0f, std::min(kMinimapMaxSize, static_cast<float>(sizePx)));
    g_minimapZoomIdx = std::max(0, std::min(kMinimapZoomCount - 1, zoomIdx));
    g_minimapSpireZoomIdx = std::max(0, std::min(kSpireZoomCount - 1, static_cast<int>(GetPrivateProfileIntA("MinimapV2", "SpireZoomIdx", 1, cfg.c_str()))));
    g_otherPlayersEnabled = (otherPlayers != 0);
    g_townPortalsEnabled = (townPortals != 0);
    g_goldChestEnabled = GetPrivateProfileIntA("MinimapV2", "GoldChests", 1, cfg.c_str()) != 0;
    g_silverChestEnabled = GetPrivateProfileIntA("MinimapV2", "SilverChests", 1, cfg.c_str()) != 0;
    g_woodChestEnabled = GetPrivateProfileIntA("MinimapV2", "WoodChests", 0, cfg.c_str()) != 0;
    g_spoilsEnabled = GetPrivateProfileIntA("MinimapV2", "DungeonSpoils", 1, cfg.c_str()) != 0;
    g_minimapShowTerrain = (showTerrain != 0);
    g_minimapThinBorder = (thinBorder != 0);
    g_minimapHeadingUp = (headingUp != 0);
    g_minimapInfoBar = (infoBar != 0);
    g_minimapVisible = showMinimap != 0 || g_vanillaWorldMap; // Vanilla World Map: the minimap's cog leads to Settings
    g_minimapClockWeatherMode = (clockWeather != 0);
    g_lootedChestMode = (lootedChestMode != 0) ? 1 : 0;
    g_hideCompletedPlaces = GetPrivateProfileIntA("MinimapV2", "HideCompletedPlaces", 0, cfg.c_str()) != 0;
    {
        char order[128]{};
        GetPrivateProfileStringA("MinimapV2", "CategoryOrder", "", order, sizeof(order), cfg.c_str());
        ParseCategoryOrder(order);
    }
    g_clockFormat = std::max(-1, std::min(1, static_cast<int>(GetPrivateProfileIntA("MinimapV2", "ClockFormat", -1, cfg.c_str()))));
    umm_lang::SetClockFormat(g_clockFormat);
    g_customMarkerScaleIdx = std::max(0, std::min(kMarkerScaleCount - 1, markerScaleIdx));
    g_pinRevealMode = static_cast<PinRevealMode>(std::max(0, std::min(2, pinRevealMode)));
    g_chestsStayLooted = respawnMins == 0;
    g_chestRespawnMinutes = respawnMins == 0 ? 30.0f : std::max(1.0f, static_cast<float>(respawnMins));
    g_chestResetRadiusMeters = std::max(0.0f, static_cast<float>(resetRadius));
    ApplyCustomMarkerScale();

    // [Locks] Rows = locked rows' type hashes, Categories = locked category ids, Collapsed = which of those are closed
    char locks[2048]{};
    GetPrivateProfileStringA("Locks", "Rows", "", locks, sizeof(locks), cfg.c_str());
    g_lockedRows.clear();
    for (char *ctx = nullptr, *tok = strtok_s(locks, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) {
        const uint32_t hash = static_cast<uint32_t>(std::strtoul(tok, nullptr, 16));
        if (hash != 0u) g_lockedRows.insert(hash);
    }
    char cats[128]{}, collapsed[128]{};
    GetPrivateProfileStringA("Locks", "Categories", "", cats, sizeof(cats), cfg.c_str());
    GetPrivateProfileStringA("Locks", "Collapsed", "", collapsed, sizeof(collapsed), cfg.c_str());
    for (bool& locked : g_lockedCategories) locked = false;
    for (char *ctx = nullptr, *tok = strtok_s(cats, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) {
        const unsigned long id = std::strtoul(tok, nullptr, 10);
        if (id < kMaxCategoryId) g_lockedCategories[id] = true;
    }
    for (char *ctx = nullptr, *tok = strtok_s(collapsed, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) {
        const unsigned long id = std::strtoul(tok, nullptr, 10);
        for (size_t c = 0; c < kCategoryGroupCount; ++c) {
            if (kCategoryGroups[c].category == id && IsCategoryLocked(kCategoryGroups[c].category)) g_categoryCollapsed[c] = true;
        }
    }
    // Closed sections (before 1.3.13 only a locked category's were kept, above)
    char closedSections[128]{};
    GetPrivateProfileStringA("MinimapV2", "CollapsedSections", "", closedSections, sizeof(closedSections), cfg.c_str());
    if (closedSections[0]) {
        for (bool& closed : g_categoryCollapsed) closed = false;
        for (char *ctx = nullptr, *tok = strtok_s(closedSections, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) {
            if (*tok < '0' || *tok > '9') continue; // "none"
            const unsigned long id = std::strtoul(tok, nullptr, 10);
            for (size_t c = 0; c < kCategoryGroupCount; ++c) {
                if (kCategoryGroups[c].category == id) g_categoryCollapsed[c] = true;
            }
        }
    }
    // A section added since the config was saved starts closed (Creatures, 136 rows): KnownSections lists the sections
    // there were when it was last saved (none saved: every one before Creatures)
    char known[128]{};
    GetPrivateProfileStringA("MinimapV2", "KnownSections", "", known, sizeof(known), cfg.c_str());
    bool knownIds[kMaxCategoryId]{};
    if (known[0]) {
        for (char *ctx = nullptr, *tok = strtok_s(known, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) {
            const unsigned long id = std::strtoul(tok, nullptr, 10);
            if (id < kMaxCategoryId) knownIds[id] = true;
        }
    } else {
        for (const auto& grp : kCategoryGroups) knownIds[grp.category] = grp.category != kCreaturesCategory;
    }
    for (size_t c = 0; c < kCategoryGroupCount; ++c) {
        if (!knownIds[kCategoryGroups[c].category]) g_categoryCollapsed[c] = true;
    }
    // RowStates = hash:1 / hash:0 for every locked row
    char states[4096]{};
    GetPrivateProfileStringA("Locks", "RowStates", "", states, sizeof(states), cfg.c_str());
    g_lockedStates.clear();
    for (char *ctx = nullptr, *tok = strtok_s(states, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) {
        char* colon = std::strchr(tok, ':');
        if (!colon) continue;
        const uint32_t hash = static_cast<uint32_t>(std::strtoul(tok, nullptr, 16));
        if (hash != 0u) g_lockedStates[hash] = colon[1] == '1';
    }
}

// [MinimapV2] CollapsedSections = the category ids of the Map Key's closed sections, kept between game sessions
void SaveCollapsedSections() {
    std::string ids;
    char buf[16]{};
    for (size_t c = 0; c < kCategoryGroupCount; ++c) {
        if (!g_categoryCollapsed[c]) continue;
        std::snprintf(buf, sizeof(buf), "%s%u", ids.empty() ? "" : ",", static_cast<unsigned>(kCategoryGroups[c].category));
        ids += buf;
    }
    async_io::IniWrite("MinimapV2", "CollapsedSections", ids.empty() ? "none" : ids.c_str(),
                               GetMinimapConfigPath().c_str());
    std::string known;
    for (const auto& grp : kCategoryGroups) {
        std::snprintf(buf, sizeof(buf), "%s%u", known.empty() ? "" : ",", static_cast<unsigned>(grp.category));
        known += buf;
    }
    async_io::IniWrite("MinimapV2", "KnownSections", known.c_str(), GetMinimapConfigPath().c_str());
}

void SaveFilterLocks() {
    if (g_applyingLockedStates) return;
    const std::string cfg = GetMinimapConfigPath();
    std::string rows, cats, collapsed;
    char buf[16]{};
    for (uint32_t hash : g_lockedRows) {
        std::snprintf(buf, sizeof(buf), "%s%08x", rows.empty() ? "" : ",", hash);
        rows += buf;
    }
    for (size_t c = 0; c < kCategoryGroupCount; ++c) {
        const uint8_t id = kCategoryGroups[c].category;
        if (!IsCategoryLocked(id)) continue;
        std::snprintf(buf, sizeof(buf), "%s%u", cats.empty() ? "" : ",", static_cast<unsigned>(id));
        cats += buf;
        if (g_categoryCollapsed[c]) {
            std::snprintf(buf, sizeof(buf), "%s%u", collapsed.empty() ? "" : ",", static_cast<unsigned>(id));
            collapsed += buf;
        }
    }
    async_io::IniWrite("Locks", "Rows", rows.c_str(), cfg.c_str());
    async_io::IniWrite("Locks", "Categories", cats.c_str(), cfg.c_str());
    async_io::IniWrite("Locks", "Collapsed", collapsed.c_str(), cfg.c_str());

    // The locked rows' ON / OFF, read from the live filters (kept as last saved while there is no world)
    uint8_t* client = GetUiClient();
    if (!client || *reinterpret_cast<uint8_t*>(client + 0x40ebe0) == 0) return;
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    g_lockedStates.clear();
    for (const auto& grp : kCategoryGroups) {
        if (grp.category == 4 && IsCategoryLocked(4)) {
            for (uint32_t pin : kCustomPinHashes) g_lockedStates[pin] = IsFilterEnabled(entries, count, pin);
        }
        for (size_t r = 0; r < grp.rowCount; ++r) {
            const uint32_t hash = grp.rows[r].typeHash;
            if (IsFilterLocked(hash, grp.category)) g_lockedStates[hash] = IsFilterEnabled(entries, count, hash);
        }
    }
    std::string states;
    for (const auto& [hash, on] : g_lockedStates) {
        std::snprintf(buf, sizeof(buf), "%s%08x:%d", states.empty() ? "" : ",", hash, on ? 1 : 0);
        states += buf;
    }
    async_io::IniWrite("Locks", "RowStates", states.c_str(), cfg.c_str());
}

void SaveMinimapConfig() {
    // Restoring the locked rows on joining a world: once, at the end (a save per row rewrote the file some 30 times a
    // row, 3 s for 129 locked rows)
    if (g_applyingLockedStates) return;
    std::string cfg = GetMinimapConfigPath();
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%d", g_minimapCustomPos ? 1 : 0);
    async_io::IniWrite("MinimapV2", "CustomPos", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_minimapPosX + 0.5f));
    async_io::IniWrite("MinimapV2", "PosX", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_minimapPosY + 0.5f));
    async_io::IniWrite("MinimapV2", "PosY", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_minimapSize + 0.5f));
    async_io::IniWrite("MinimapV2", "Size", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapZoomIdx);
    async_io::IniWrite("MinimapV2", "ZoomIdx", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapSpireZoomIdx);
    async_io::IniWrite("MinimapV2", "SpireZoomIdx", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_otherPlayersEnabled ? 1 : 0);
    async_io::IniWrite("MinimapV2", "OtherPlayers", buf, cfg.c_str());
    async_io::IniWrite("MinimapV2", "TownPortals", g_townPortalsEnabled ? "1" : "0", cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapShowTerrain ? 1 : 0);
    async_io::IniWrite("MinimapV2", "ShowTerrain", buf, cfg.c_str());
    async_io::IniWrite("MinimapV2", "ThinBorder", g_minimapThinBorder ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "HeadingUp", g_minimapHeadingUp ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "InfoBar", g_minimapInfoBar ? "1" : "0", cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_fogRevealMode));
    async_io::IniWrite("MinimapV2", "FogRevealMode", buf, cfg.c_str());
    async_io::IniWrite("MinimapV2", "ShowPosition", g_minimapShowPosition ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "RegionFilter", g_regionFilterEnabled ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "LevelAreas", g_levelAreas ? "1" : "0", cfg.c_str());
    {
        char tint[8];
        std::snprintf(tint, sizeof(tint), "%d", g_levelTintPct);
        async_io::IniWrite("MinimapV2", "LevelTint", tint, cfg.c_str());
    }
    async_io::IniWrite("MinimapV2", "RegionNames", g_regionNames ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "VanillaWorldMap", g_vanillaWorldMap ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "VanillaMinimap", g_vanillaMinimap ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "DungeonMaps", g_dungeonMaps ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "NearbyCreatures", g_nearCreatures ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "NearbyCreaturesHideMap", g_nearCreaturesHideMap ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "FindExactMatch", g_findExactMatch ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "MapDesk", g_mapDesk ? "1" : "0", cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%u", g_regionMask & kAllRegionsMask);
    async_io::IniWrite("MinimapV2", "RegionMask", buf, cfg.c_str());
    async_io::IniWrite("MinimapV2", "RegionFocus", nullptr, cfg.c_str());
    async_io::IniWrite("MinimapV2", "RegionDrawerOpen", g_regionDrawerOpen ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "RegionOutsideHide", g_regionOutsideHide ? "1" : "0", cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapVisible ? 1 : 0);
    async_io::IniWrite("MinimapV2", "ShowMinimap", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapClockWeatherMode ? 1 : 0);
    async_io::IniWrite("MinimapV2", "ClockWeather", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_lootedChestMode);
    async_io::IniWrite("MinimapV2", "LootedChestMode", buf, cfg.c_str());
    async_io::IniWrite("MinimapV2", "HideCompletedPlaces", g_hideCompletedPlaces ? "1" : "0", cfg.c_str());
    {
        std::string order;
        if (!g_categoryOrderSet) ResetCategoryOrder();
        for (size_t i = 0; i < kCategoryGroupCount; ++i) order += (i ? "," : "") + std::to_string(g_categoryOrder[i]);
        async_io::IniWrite("MinimapV2", "CategoryOrder", order.c_str(), cfg.c_str());
    }
    std::snprintf(buf, sizeof(buf), "%d", g_clockFormat);
    async_io::IniWrite("MinimapV2", "ClockFormat", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_customMarkerScaleIdx);
    async_io::IniWrite("MinimapV2", "CustomMarkerScaleIdx", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_pinRevealMode));
    async_io::IniWrite("MinimapV2", "PinRevealMode", buf, cfg.c_str());
    async_io::IniWrite("MinimapV2", "GoldChests", g_goldChestEnabled ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "SilverChests", g_silverChestEnabled ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "WoodChests", g_woodChestEnabled ? "1" : "0", cfg.c_str());
    async_io::IniWrite("MinimapV2", "DungeonSpoils", g_spoilsEnabled ? "1" : "0", cfg.c_str());
    char ores[kOreCount + 1]{};
    for (size_t o = 0; o < kOreCount; ++o) ores[o] = g_oreEnabled[o] ? '1' : '0';
    async_io::IniWrite("Mining", "Ores", ores, cfg.c_str());
}

// A position anywhere on the world (a Hollow Hall reaches the west edge, x under 100 m: no minimap there before)
bool GetHudPlayerWorldPos(uint8_t* client, float& outX, float& outZ) {
    if (!client) return false;
    // 1. Primary live player world position from clientWorld (GetUiG() + 0x47d4e0)
    if (uint8_t* clientWorld = GetClientWorld(client)) {
        int64_t rawX = *reinterpret_cast<int64_t*>(clientWorld + 0x2c18);
        int64_t rawZ = *reinterpret_cast<int64_t*>(clientWorld + 0x2c28);
        if (rawX != 0 || rawZ != 0) {
            float wx = static_cast<float>(static_cast<double>(rawX) * 2.3283064365386963e-10);
            float wz = static_cast<float>(static_cast<double>(rawZ) * 2.3283064365386963e-10);
            if (wx > 0.0f && wx < 12000.0f && wz > 0.0f && wz < 12000.0f) {
                outX = wx;
                outZ = wz;
                return true;
            }
        }
    }
    // 2. Fallback to UI player coordinates in d0 (+0x14030 / +0x14040)
    if (auto* g = static_cast<uint8_t*>(GetUiG())) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                int64_t rawPx = *reinterpret_cast<int64_t*>(d0 + 0x14030);
                int64_t rawPz = *reinterpret_cast<int64_t*>(d0 + 0x14040);
                if (rawPx != 0 || rawPz != 0) {
                    float px = static_cast<float>(rawPx) / 65536.0f;
                    float pz = static_cast<float>(rawPz) / 65536.0f;
                    if (px > 0.0f && px < 12000.0f && pz > 0.0f && pz < 12000.0f) {
                        outX = px;
                        outZ = pz;
                        return true;
                    }
                }
            }
        }
    }
    // 3. Fallback to client + 0x38 / +0x40 (world map local player float coords)
    float cpx = *reinterpret_cast<float*>(client + 0x38);
    float cpz = *reinterpret_cast<float*>(client + 0x40);
    if (std::isfinite(cpx) && std::isfinite(cpz) &&
        cpx > 0.0f && cpx < 12000.0f && cpz > 0.0f && cpz < 12000.0f) {
        outX = cpx;
        outZ = cpz;
        return true;
    }
    return false;
}

void DrawPlayerTriangle(void* widget, float cx, float cy, float dirX, float dirZ, float scale, uint32_t wingColor, uint32_t coreColor, float opacity) {
    // Convert world (X=East, Z=North) to screen 2D (X=Right, Y=Down):
    const float faceFx = dirX;
    const float faceFy = -dirZ;
    const float facePx = -faceFy;
    const float facePy = faceFx;

    // Compact directional triangle / pointer centered on (cx, cy):
    // Dark outline pass (pass 0) followed by crisp warm antique-gold / bright-gold fill pass (pass 1)
    for (int pass = 0; pass < 2; ++pass) {
        const bool isOutline = (pass == 0);
        const float pad = isOutline ? (1.25f * scale) : 0.0f;
        const uint32_t wingCol = isOutline ? 0xFF0A0E10u : wingColor;
        const uint32_t coreCol = isOutline ? 0xFF0A0E10u : coreColor;
        const float alpha = (isOutline ? 0.95f : 1.0f) * opacity;
        const float bright = isOutline ? 1.0f : 1.15f;

        for (int i = 0; i <= 10; ++i) {
            float t = static_cast<float>(i) / 10.0f; // 0.0 = rear base (-5.0px), 1.0 = sharp tip (+7.0px)
            float dist = (-5.0f + t * 12.0f) * scale;
            float halfW = (1.0f - t) * 4.4f * scale;
            float dotSz = ((2.2f - t * 0.7f) * scale) + pad * 2.0f;

            // Left & right triangle edges
            for (int side = -1; side <= 1; side += 2) {
                float wx = cx + faceFx * dist + facePx * (static_cast<float>(side) * halfW);
                float wy = cy + faceFy * dist + facePy * (static_cast<float>(side) * halfW);
                DrawAssetRectAbs(widget, kCirclePrimitive,
                                 wx - dotSz * 0.5f, wy - dotSz * 0.5f,
                                 dotSz, dotSz, wingCol, alpha, bright);
            }
            // Solid interior fill & notched rear base (i >= 2 fills from slight chevron notch to tip)
            if (i >= 2) {
                float innerW = halfW * 0.48f;
                for (int side = -1; side <= 1; side += 2) {
                    float ix = cx + faceFx * dist + facePx * (static_cast<float>(side) * innerW);
                    float iy = cy + faceFy * dist + facePy * (static_cast<float>(side) * innerW);
                    DrawAssetRectAbs(widget, kCirclePrimitive,
                                     ix - dotSz * 0.5f, iy - dotSz * 0.5f,
                                     dotSz, dotSz, coreCol, alpha, bright);
                }
                float dotX = cx + faceFx * dist;
                float dotY = cy + faceFy * dist;
                DrawAssetRectAbs(widget, kCirclePrimitive,
                                 dotX - dotSz * 0.5f, dotY - dotSz * 0.5f,
                                 dotSz, dotSz, coreCol, alpha, bright);
            }
        }
    }

}

void GetHudPlayerFacingDirs(uint8_t* client, float playerX, float playerZ,
                            float& outCamDirX, float& outCamDirZ,
                            float& outFaceDirX, float& outFaceDirZ) {
    static float s_lastCamDirX = 0.0f;
    static float s_lastCamDirZ = 1.0f;
    static float s_lastFaceDirX = 0.0f;
    static float s_lastFaceDirZ = 1.0f;
    static float s_lastPlayerX = 0.0f;
    static float s_lastPlayerZ = 0.0f;
    static bool s_hasLastPos = false;

    float camDirX = s_lastCamDirX;
    float camDirZ = s_lastCamDirZ;
    bool gotCam = false;

    if (client) {
        // Read active 4x4 View Matrix at client + 0x1d20
        float v[16]{};
        std::memcpy(v, client + 0x1d20, sizeof(v));
        float camX = -(v[0] * v[12] + v[1] * v[13] + v[2] * v[14]);
        float camZ = -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]);
        if (std::isfinite(camX) && std::isfinite(camZ)) {
            float dx = playerX - camX;
            float dz = playerZ - camZ;
            float lenSq = dx * dx + dz * dz;
            if (lenSq > 0.04f && lenSq < 2500.0f) {
                float invLen = 1.0f / std::sqrt(lenSq);
                camDirX = dx * invLen;
                camDirZ = dz * invLen;
                gotCam = true;
            }
        }
        if (!gotCam) {
            // Fallback to View Matrix forward row (-v[2], -v[10])
            float fx = -v[2];
            float fz = -v[10];
            float fLenSq = fx * fx + fz * fz;
            if (std::isfinite(fLenSq) && fLenSq > 0.01f) {
                float invLen = 1.0f / std::sqrt(fLenSq);
                camDirX = fx * invLen;
                camDirZ = fz * invLen;
                gotCam = true;
            }
        }
    }

    float camDelta = std::abs(camDirX - s_lastCamDirX) + std::abs(camDirZ - s_lastCamDirZ);
    s_lastCamDirX = camDirX;
    s_lastCamDirZ = camDirZ;

    // Default player facing direction tracks camera look direction immediately when turning the camera
    // or standing still, and blends smoothly toward movement heading if walking in another direction.
    float faceDirX = camDirX;
    float faceDirZ = camDirZ;

    if (s_hasLastPos) {
        float mdx = playerX - s_lastPlayerX;
        float mdz = playerZ - s_lastPlayerZ;
        float mDistSq = mdx * mdx + mdz * mdz;
        if (mDistSq > 0.0012f && mDistSq < 100.0f) {
            float invM = 1.0f / std::sqrt(mDistSq);
            float moveX = mdx * invM;
            float moveZ = mdz * invM;
            if (camDelta < 0.006f) {
                float bx = s_lastFaceDirX * 0.65f + moveX * 0.35f;
                float bz = s_lastFaceDirZ * 0.65f + moveZ * 0.35f;
                float bLenSq = bx * bx + bz * bz;
                if (bLenSq > 0.01f) {
                    float invB = 1.0f / std::sqrt(bLenSq);
                    faceDirX = bx * invB;
                    faceDirZ = bz * invB;
                }
            }
        } else if (camDelta < 0.003f) {
            // Keep last facing direction if neither moving nor rotating camera
            faceDirX = s_lastFaceDirX;
            faceDirZ = s_lastFaceDirZ;
        }
    }

    // The way the character really faces (its own transform's rotation), when it can be read: the guess above followed
    // the camera when standing still ("the arrow points the way the camera faces, sometimes": 2026-11-09)
    float realX = 0.0f, realZ = 0.0f;
    if (halls::PlayerFacing(playerX, playerZ, realX, realZ)) {
        faceDirX = realX;
        faceDirZ = realZ;
    }

    s_lastPlayerX = playerX;
    s_lastPlayerZ = playerZ;
    s_hasLastPos = true;
    s_lastFaceDirX = faceDirX;
    s_lastFaceDirZ = faceDirZ;

    outCamDirX = camDirX;
    outCamDirZ = camDirZ;
    outFaceDirX = faceDirX;
    outFaceDirZ = faceDirZ;
}

// The players on a Hollow Hall's dungeon map, drawn as on the minimap (the game's own arrows are under its dark ground):
// you and everyone in this hall with you, each a triangle in their colour facing their way, the others named
void DrawDungeonMapPlayers(void* widget, const float* rootRect, uint8_t* client, uint8_t* mapState) {
    if (!halls::MapActive() || !widget || !client) return;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH)) return;
    const float top = rootRect[1] + 114.0f;
    constexpr float kScale = 1.25f;
    uint8_t* playerArray = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
    const uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
    const uint32_t localPlayerId = *reinterpret_cast<uint32_t*>(client + 0x18);
    static minimap_players::Roster roster;
    uint8_t localHidden = 0; // p[0x207]: in a hidden place (the Hollow Halls); the players in there with you
    uint32_t activeIds[64]{};
    size_t activeCount = 0;
    if (playerArray && playerCount <= 64) {
        for (uint64_t i = 0; i < playerCount; ++i) {
            auto* p = playerArray + i * 0x240;
            if (*reinterpret_cast<uint32_t*>(p) == localPlayerId) localHidden = p[0x207];
        }
        for (uint64_t i = 0; i < playerCount; ++i) {
            auto* p = playerArray + i * 0x240;
            const uint32_t id = *reinterpret_cast<uint32_t*>(p);
            if (id && id != localPlayerId && p[0x207] == localHidden) activeIds[activeCount++] = id;
        }
    }
    roster.Retain(activeIds, activeCount);
    if (g_otherPlayersEnabled && playerArray && playerCount > 0 && playerCount <= 64) {
        for (uint64_t i = 0; i < playerCount; ++i) {
            uint8_t* p = playerArray + i * 0x240;
            const uint32_t pid = *reinterpret_cast<uint32_t*>(p);
            if (pid == 0 || pid == localPlayerId || p[0x207] != localHidden) continue;
            float ox = *reinterpret_cast<float*>(p + 0x10);
            float oz = *reinterpret_cast<float*>(p + 0x18);
            if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f) && p[0x68] != 0) {
                ox = static_cast<float>(*reinterpret_cast<int32_t*>(p + 0x54)) +
                     static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x50)) / 65536.0f;
                oz = static_cast<float>(*reinterpret_cast<int32_t*>(p + 0x64)) +
                     static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x60)) / 65536.0f;
            }
            if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f) || halls::MapHides(ox, oz)) continue;
            const float sx = originX + ox * zoom, sy = originY + (mapWorldH - oz) * zoom;
            if (sy < top) continue;
            auto& marker = roster.Update(pid, ox, oz);
            DrawPlayerTriangle(widget, sx, sy, marker.dirX, marker.dirZ, kScale, marker.color, marker.color, 1.0f);
            const char* namePtr = *reinterpret_cast<const char**>(p + 0xc0);
            size_t nameLen = *reinterpret_cast<size_t*>(p + 0xc8);
            if (!namePtr || nameLen == 0 || nameLen > 64) {
                namePtr = *reinterpret_cast<const char**>(p + 0xd8);
                nameLen = *reinterpret_cast<size_t*>(p + 0xe0);
            }
            if (namePtr && nameLen > 0 && nameLen <= 48) {
                char nameBuf[56]{};
                std::memcpy(nameBuf, namePtr, nameLen);
                const float lblW = 140.0f, lblH = 18.0f, lblY = sy - 14.0f * kScale - lblH;
                DrawLabelAbs(sx - lblW * 0.5f + 1.0f, lblY + 1.0f, lblW, lblH, nameBuf, 0xFF080A0Cu, 0.9f, TextAlignH::Center,
                             TextAlignV::Center);
                DrawLabelAbs(sx - lblW * 0.5f, lblY, lblW, lblH, nameBuf, 0xFFF4ECDCu, 1.0f, TextAlignH::Center, TextAlignV::Center);
            }
        }
    }
    // You, in your own colour (as every other player sees you), facing your way
    float px = 0.0f, pz = 0.0f;
    if (!GetHudPlayerWorldPos(client, px, pz)) return;
    float camDirX = 0.0f, camDirZ = 1.0f, faceDirX = 0.0f, faceDirZ = 1.0f;
    GetHudPlayerFacingDirs(client, px, pz, camDirX, camDirZ, faceDirX, faceDirZ);
    const float sx = originX + px * zoom, sy = originY + (mapWorldH - pz) * zoom;
    if (sy < top) return;
    const uint32_t ownColor = minimap_players::ColorFor(localPlayerId);
    if (halls::PlayerOnShownFloor(client)) {
        DrawPlayerTriangle(widget, sx, sy, faceDirX, faceDirZ, kScale, ownColor, ownColor, 1.0f);
        return;
    }
    // On another floor than the one shown: a dotted outline of the arrow (where you are, not on this floor)
    const float fx = faceDirX, fy = -faceDirZ, qx = -fy, qy = fx;
    // (a bit bigger than the solid arrow so the dashes read: dashes of 4 px, gaps of 3, along the outline, on a faint
    // dark band so the shape is still one arrow)
    constexpr float kOutline = 1.7f;
    const float tipD = 8.0f * kOutline, baseD = -5.5f * kOutline, halfW = 5.2f * kOutline;
    const float corners[3][2] = {{sx + fx * tipD, sy + fy * tipD},
                                 {sx + fx * baseD + qx * halfW, sy + fy * baseD + qy * halfW},
                                 {sx + fx * baseD - qx * halfW, sy + fy * baseD - qy * halfW}};
    for (int pass = 0; pass < 2; ++pass) {
        const float dot = pass == 0 ? 4.6f : 2.6f;
        const uint32_t col = pass == 0 ? 0xFF0A0E10u : ownColor;
        float run = 0.0f; // distance along the outline, for the dash pattern
        for (int e = 0; e < 3; ++e) {
            const float* a = corners[e];
            const float* b = corners[(e + 1) % 3];
            const float len = std::hypot(b[0] - a[0], b[1] - a[1]);
            for (float m = 0.0f; m < len; m += 1.0f, run += 1.0f) {
                const bool dash = std::fmod(run, 7.0f) < 4.0f;
                if (pass == 1 && !dash) continue;
                const float x = a[0] + (b[0] - a[0]) * m / len, y = a[1] + (b[1] - a[1]) * m / len;
                DrawAssetRectAbs(widget, kCirclePrimitive, x - dot * 0.5f, y - dot * 0.5f, dot, dot, col,
                                 pass == 0 ? 0.45f : 1.0f);
            }
        }
    }
}

// Queries Enshrouded's WeatherSystem (client + 0x466fe8) and WeatherVolumeManager for active
// weather volumes overlapping (playerX, playerZ).
// Returns: 0 = Clear, 1 = Rain, 2 = Storm, 3 = Snow, 4 = Fog
// The weather at the player, asked of the game itself: FUN_140a9d530(out[4], weatherSystem, position, time)
// is what its gameplay uses (wet status, fires going out). It walks the WeatherSystem's instances, samples each
// one's rain mask, fades it in and out, and turns rain to snow by altitude, giving clear / rain / snow /
// blizzard shares. Decoded 2026-10-24 (game build of Oct 21).
//
// It needs the weather clock, which the game keeps in an ECS time resource the mod cannot reach. Every moving
// instance gives it back exactly: the renderer holds where the instance is now, and it moves in a straight
// line from its start position since its start time, so now = start + distance / speed.
//   WeatherSystem (client + 0x466fe8): +0x28 page records (0x40: +8 slots, +0x10 slot count), +0x30 page count,
//     +0x50 page shift, +0x60 slot mask, +0x88 instance handles (u32: index low 16, generation high 16), +0x90
//     handle count, +0x148 the renderer. A 0x50-byte slot: generation (u32 & 0x7fffffff), then the instance:
//     +8 / +0x18 start x / z (32.32), +0x28 speed (m/s), +0x30 start time (ns), +0x40 render handle.
//   Render instances (*(*(renderer + 0x98) + 0x9d0)): +0x50 page records, +0x58 page count, +0x78 shift,
//     +0x88 mask, 0x68-byte slots; the data at +4 holds the current x / z floats at +0x34 / +0x38.
namespace {
using WeatherAtFn = float* (*)(float* out, void* weatherSystem, const int64_t* position, int64_t timeNs);

uint8_t* PagedSlot(uint8_t* pages, uint64_t pageCount, uint8_t shift, uint64_t mask, size_t stride, uint32_t h) {
    if (!pages || h == 0 || shift > 31) return nullptr;
    const uint32_t idx = h & 0xFFFFu;
    const uint64_t page = idx >> shift;
    if (page >= pageCount) return nullptr;
    uint8_t* rec = pages + page * 0x40;
    uint8_t* slots = *reinterpret_cast<uint8_t**>(rec + 8);
    const uint64_t slot = idx & mask;
    if (!slots || slot >= *reinterpret_cast<uint64_t*>(rec + 0x10)) return nullptr;
    uint8_t* s = slots + slot * stride;
    return (*reinterpret_cast<uint32_t*>(s) & 0x7fffffffu) == (h >> 16) ? s : nullptr;
}

bool WeatherClockNs(uint8_t* weatherSys, int64_t& outNs) {
    uint8_t* renderer = *reinterpret_cast<uint8_t**>(weatherSys + 0x148);
    uint8_t* r98 = renderer ? *reinterpret_cast<uint8_t**>(renderer + 0x98) : nullptr;
    uint8_t* mgr = r98 ? *reinterpret_cast<uint8_t**>(r98 + 0x9d0) : nullptr;
    const uint32_t* handles = *reinterpret_cast<const uint32_t**>(weatherSys + 0x88);
    const uint64_t count = *reinterpret_cast<uint64_t*>(weatherSys + 0x90);
    if (!mgr || !handles || count == 0 || count > 256) return false;
    float bestSpeed = 0.5f; // the fastest instance gives the finest clock
    bool found = false;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t* s = PagedSlot(*reinterpret_cast<uint8_t**>(weatherSys + 0x28), *reinterpret_cast<uint64_t*>(weatherSys + 0x30),
                               weatherSys[0x50], *reinterpret_cast<uint64_t*>(weatherSys + 0x60), 0x50, handles[i]);
        if (!s) continue;
        uint8_t* inst = s + 8;
        const float speed = *reinterpret_cast<float*>(inst + 0x28);
        if (!(speed > bestSpeed)) continue;
        uint8_t* r = PagedSlot(*reinterpret_cast<uint8_t**>(mgr + 0x50), *reinterpret_cast<uint64_t*>(mgr + 0x58),
                               mgr[0x78], *reinterpret_cast<uint64_t*>(mgr + 0x88), 0x68,
                               *reinterpret_cast<uint32_t*>(inst + 0x40));
        if (!r) continue;
        const double x0 = static_cast<double>(*reinterpret_cast<int64_t*>(inst + 8)) * 2.3283064365386963e-10;
        const double z0 = static_cast<double>(*reinterpret_cast<int64_t*>(inst + 0x18)) * 2.3283064365386963e-10;
        const double x = *reinterpret_cast<float*>(r + 4 + 0x34), z = *reinterpret_cast<float*>(r + 4 + 0x38);
        const double moved = std::sqrt((x - x0) * (x - x0) + (z - z0) * (z - z0));
        if (!std::isfinite(moved)) continue;
        outNs = *reinterpret_cast<int64_t*>(inst + 0x30) + static_cast<int64_t>(moved / speed * 1e9);
        bestSpeed = speed;
        found = true;
    }
    return found;
}
} // namespace

int GetActivePlayerWeather(uint8_t* client, float /*playerX*/, float /*playerZ*/) {
    static WeatherAtFn weatherAt = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        weatherAt = reinterpret_cast<WeatherAtFn>(FindUnique(
            "48 8B C4 48 89 58 18 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 20 01 00 00 "
            "0F 29 70 B8 4D 8B E8 44 8B 42 0C"));
        if (g_log) g_log("Minimap weather: %s", weatherAt ? "the game's weather-at-position found" : "weather-at-position not found, weather stays Clear");
    }
    if (!client || !weatherAt) return 0;
    __try {
        uint8_t* weatherSys = *reinterpret_cast<uint8_t**>(client + 0x466fe8);
        uint8_t* clientWorld = GetClientWorld(client);
        if (!weatherSys || !clientWorld) return 0;
        int64_t nowNs = 0;
        if (!WeatherClockNs(weatherSys, nowNs)) return 0;
        const int64_t* pos = reinterpret_cast<const int64_t*>(clientWorld + 0x2c18); // x, y, z (32.32)
        float mix[4]{}; // clear, rain, snow, blizzard
        weatherAt(mix, weatherSys, pos, nowNs);
        // 1 rain, 2 storm (the game's blizzard), 3 snow
        int best = 0;
        float bestShare = 0.1f;
        const int types[3] = {1, 3, 2};
        for (int k = 0; k < 3; ++k) {
            if (std::isfinite(mix[k + 1]) && mix[k + 1] > bestShare) {
                bestShare = mix[k + 1];
                best = types[k];
            }
        }
        return best;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Minimap art (minimap_art.h, generated by art/gen_minimap_art.py) is drawn as solid rects in the
// pixels of a 370 px render. A sprite's stretch zones take extra length: everything past a zone
// shifts by the zone's share, and rects inside it stretch, so ornaments keep their size.
float MapArtAxis(float v, const uint16_t* zones, int zoneCount, float extraPerZone) {
    float out = v;
    for (int i = 0; i < zoneCount; ++i) {
        const float z0 = zones[i * 2], z1 = zones[i * 2 + 1];
        if (v >= z1) {
            out += extraPerZone;
        } else if (v > z0) {
            out += extraPerZone * (v - z0) / (z1 - z0);
        }
    }
    return out;
}

void DrawArtSprite(void* widgetRaw, const MinimapArtSprite& sprite, float originX, float originY, float scale,
                   float extraPerZoneX = 0.0f, float extraPerZoneY = 0.0f) {
    // Scaled sprites get a hair of overlap so neighbouring rects never leave a seam.
    const float pad = (scale == 1.0f) ? 0.0f : 0.12f;
    for (uint32_t i = 0; i < sprite.count; ++i) {
        const MinimapArtRect& r = sprite.rects[i];
        const float x0 = MapArtAxis(r.x, sprite.zonesX, sprite.nZonesX, extraPerZoneX);
        const float x1 = MapArtAxis(static_cast<float>(r.x + r.w), sprite.zonesX, sprite.nZonesX, extraPerZoneX);
        const float y0 = MapArtAxis(r.y, sprite.zonesY, sprite.nZonesY, extraPerZoneY);
        const float y1 = MapArtAxis(static_cast<float>(r.y + r.h), sprite.zonesY, sprite.nZonesY, extraPerZoneY);
        DrawSolidRectAbs(widgetRaw, originX + x0 * scale, originY + y0 * scale, (x1 - x0) * scale + pad,
                         (y1 - y0) * scale + pad, r.color | 0xFF000000u, static_cast<float>(r.color >> 24) / 255.0f);
    }
}

// Fills stretch zone `zone` of the design-size frame with its strip of aged rail, cut to the
// zone's stretched length (the frame sprite leaves its zones empty so the grime never smears).
// scale: the frame drawn smaller than its design size (no stretch then), like DrawArtSprite's.
void DrawArtZoneStrip(void* widgetRaw, const MinimapArtSprite& frame, const MinimapArtSprite& strip, bool alongX,
                      int zone, float frameX, float frameY, float extraPerZoneX, float extraPerZoneY,
                      float scale = 1.0f) {
    const float pad = (scale == 1.0f) ? 0.0f : 0.12f;
    const uint16_t* zones = alongX ? frame.zonesX : frame.zonesY;
    const int zoneCount = alongX ? frame.nZonesX : frame.nZonesY;
    const float extra = alongX ? extraPerZoneX : extraPerZoneY;
    const float z0 = zones[zone * 2];
    const float zoneEnd = zones[zone * 2 + 1] + extra;   // in strip pixels, measured from the zone start
    const float shift = MapArtAxis(z0, zones, zoneCount, extra) - z0;
    for (uint32_t i = 0; i < strip.count; ++i) {
        const MinimapArtRect& r = strip.rects[i];
        const float a0 = alongX ? r.x : r.y;
        const float a1 = std::min(a0 + (alongX ? r.w : r.h), zoneEnd);
        if (a0 >= zoneEnd) continue;
        // The other axis lies outside that axis's zones, so the frame's own mapping just shifts it.
        const float b0 = alongX ? r.y : r.x;
        const float b1 = b0 + (alongX ? r.h : r.w);
        const float m0 = MapArtAxis(b0, alongX ? frame.zonesY : frame.zonesX, alongX ? frame.nZonesY : frame.nZonesX,
                                    alongX ? extraPerZoneY : extraPerZoneX);
        const float m1 = m0 + (b1 - b0);
        const float x = frameX + (alongX ? a0 + shift : m0) * scale;
        const float y = frameY + (alongX ? m0 : a0 + shift) * scale;
        DrawSolidRectAbs(widgetRaw, x, y, (alongX ? a1 - a0 : m1 - m0) * scale + pad,
                         (alongX ? m1 - m0 : a1 - a0) * scale + pad, r.color | 0xFF000000u,
                         static_cast<float>(r.color >> 24) / 255.0f);
    }
}

void FormatInGameTimeOfDay(uint8_t* client, float playerX, float playerZ,
                           char* outText, size_t outCap, bool& outIsDay, int& outPhase, int& outWeather) {
    outIsDay = true;
    outPhase = 1; // 0 = Dawn, 1 = Day, 2 = Dusk, 3 = Night
    outWeather = g_minimapClockWeatherMode ? GetActivePlayerWeather(client, playerX, playerZ) : 0;
    std::snprintf(outText, outCap, "12:00 PM  -  Day");
    if (!client) return;

    uint8_t* daytime = *reinterpret_cast<uint8_t**>(client + 0x466ff0);
    if (!daytime) return;

    constexpr int64_t kDayNanoseconds = 86400000000000LL;
    int64_t curNs = *reinterpret_cast<int64_t*>(daytime + 0x48);
    int64_t sunriseNs = *reinterpret_cast<int64_t*>(daytime + 0x28);
    int64_t sunsetNs = *reinterpret_cast<int64_t*>(daytime + 0x30);
    uint8_t isDayFlag = daytime[0x58];

    if (curNs < 0 || curNs > kDayNanoseconds * 2) return;
    curNs %= kDayNanoseconds;
    if (curNs < 0) curNs += kDayNanoseconds;

    int totalMinutes = static_cast<int>((curNs * 1440LL) / kDayNanoseconds);
    totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;
    int hour24 = totalMinutes / 60;
    int minute = totalMinutes % 60;
    bool isPm = (hour24 >= 12);
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    if (sunriseNs <= 0 || sunriseNs >= kDayNanoseconds) sunriseNs = 6LL * 3600LL * 1000000000LL;
    if (sunsetNs <= 0 || sunsetNs >= kDayNanoseconds) sunsetNs = 20LL * 3600LL * 1000000000LL;

    constexpr int64_t kTwilightHalfWindowNs = 45LL * 60LL * 1000000000LL; // +/- 45 in-game mins
    const char* statusStr = umm_lang::T(umm_lang::Str::Day);
    if (std::abs(curNs - sunriseNs) <= kTwilightHalfWindowNs) {
        statusStr = umm_lang::T(umm_lang::Str::Dawn);
        outIsDay = true;
        outPhase = 0;
    } else if (std::abs(curNs - sunsetNs) <= kTwilightHalfWindowNs) {
        statusStr = umm_lang::T(umm_lang::Str::Dusk);
        outIsDay = true;
        outPhase = 2;
    } else if ((curNs >= sunriseNs && curNs < sunsetNs) || isDayFlag != 0) {
        if (curNs >= sunriseNs && curNs < sunsetNs) {
            statusStr = umm_lang::T(umm_lang::Str::Day);
            outIsDay = true;
            outPhase = 1;
        } else {
            statusStr = umm_lang::T(umm_lang::Str::Night);
            outIsDay = false;
            outPhase = 3;
        }
    } else {
        statusStr = umm_lang::T(umm_lang::Str::Night);
        outIsDay = false;
        outPhase = 3;
    }

    if (g_minimapClockWeatherMode) {
        switch (outWeather) {
            case 1: statusStr = umm_lang::T(umm_lang::Str::WeatherRain);  break;
            case 2: statusStr = umm_lang::T(umm_lang::Str::WeatherStorm); break;
            case 3: statusStr = umm_lang::T(umm_lang::Str::WeatherSnow);  break;
            case 4: statusStr = umm_lang::T(umm_lang::Str::WeatherFog);   break;
            default: statusStr = umm_lang::T(umm_lang::Str::WeatherClear); break;
        }
    }

    if (umm_lang::Uses12HourClock()) {
        std::snprintf(outText, outCap, "%d:%02d %s  |  %s", hour12, minute, isPm ? "PM" : "AM", statusStr);
    } else {
        std::snprintf(outText, outCap, "%d:%02d  |  %s", hour24, minute, statusStr);
    }
}

struct MinimapMarkerDraw {
    float sx;
    float sy;
    float size;
    const void* iconRef;
    uint32_t color;
    float alpha;
    uint8_t sortCat;
    bool isPulsing;
    bool isCleared;
    int8_t heightDir = 0; // nearby chests: +1 above the player, -1 below
    uint32_t ring = 0;    // a live creature fighting / alerted: a ring of this colour around it (0: none)
};

struct AssetSpec32 {
    uint64_t guidLo;
    uint64_t guidHi;
    uint32_t typeHash;
    uint32_t subIndex;
    uint64_t flags;
};

bool IsAssetSpecInRepo(void* assetRepoRaw, const AssetSpec32& spec) {
    if (!assetRepoRaw) return false;
    auto* repo = static_cast<uint8_t*>(assetRepoRaw);
    uint32_t count = *reinterpret_cast<uint32_t*>(repo + 0x359c);
    uint64_t cap = *reinterpret_cast<uint64_t*>(repo + 0x3558);
    uint64_t* bitmask = *reinterpret_cast<uint64_t**>(repo + 0x3550);
    const uint8_t* keys = *reinterpret_cast<const uint8_t**>(repo + 0x3570);
    const uint32_t* values = *reinterpret_cast<const uint32_t**>(repo + 0x3588);
    const uint8_t* entries = *reinterpret_cast<const uint8_t**>(repo + 0x3538);
    if (count == 0 || cap == 0 || (cap & (cap - 1)) != 0 || !bitmask || !keys || !values || !entries) return false;

    uint32_t r10d = spec.typeHash;
    uint32_t r11d = spec.subIndex;
    uint32_t h = (r10d & 0xFFu) ^ static_cast<uint32_t>(spec.guidLo);
    h = (h * 0x1000193u) ^ ((r10d >> 8) & 0xFFu);
    h = (h * 0x1000193u) ^ ((r10d >> 16) & 0xFFu);
    h = (h * 0x1000193u) ^ (r10d >> 24);
    h = (h * 0x1000193u) ^ (r11d & 0xFFu);
    h = (h * 0x1000193u) ^ ((r11d >> 8) & 0xFFu);
    h = (h * 0x1000193u) ^ ((r11d >> 16) & 0xFFu);
    h = (h * 0x1000193u) ^ (r11d >> 24);
    uint64_t idx = static_cast<uint64_t>(h * 0x1000193u) & (cap - 1);

    for (uint64_t step = 0; step < cap; ++step) {
        if (((bitmask[idx >> 6] >> (idx & 63)) & 1ULL) == 0ULL) {
            return false;
        }
        const auto* entry = reinterpret_cast<const AssetSpec32*>(keys + (idx << 5));
        if (entry->guidLo == spec.guidLo &&
            entry->guidHi == spec.guidHi &&
            entry->typeHash == spec.typeHash &&
            entry->subIndex == spec.subIndex) {
            return true;
        }
        idx = (idx + 1 < cap) ? (idx + 1) : 0;
    }
    return false;
}

uint32_t SafeResolveAssetSpec(void* assetRepo, const AssetSpec32& spec) {
    if (!g_resolveAssetSpec || !IsAssetSpecInRepo(assetRepo, spec)) return 0u;
    __try {
        return g_resolveAssetSpec(assetRepo, &spec, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
}

// Heading-up minimap: what the terrain pass draws (the game's World Map canvas, the hall plans, Roads and Surface Water)
// is drawn north up over a square that covers the turned view, then its vertices are turned round the player in the
// UI draw list's vertex buffer (+0x35280 base, +0x352c0 byte cursor, 24-byte vertices x, y first: the allocator
// 0x14119e510 is the only writer), and the clip rects pushed meanwhile (+0x31110 list of x, y, w, h; draws keep an index
// into it) are cut back to the minimap's own square.
#ifdef UMM_DEV_BRIDGE
// dev: [MinimapV2] TurnDebug in minimap_config.ini (read each second): 1 no vertex turn, 2 no clip cut, 4 no bigger
// square, 8 log the vertices
int MmTurnDebug() {
    static int v = 0;
    static uint64_t last = 0;
    if (GetTickCount64() - last > 1000) {
        last = GetTickCount64();
        v = GetPrivateProfileIntA("MinimapV2", "TurnDebug", 0, GetMinimapConfigPath().c_str());
    }
    return v;
}
#else
constexpr int MmTurnDebug() { return 0; }
#endif
float g_mmDebugPlayer[2] = {};
struct MmTurnWindow {
    void* uiCtx = nullptr;
    uint8_t* dl = nullptr;
    uint8_t* base = nullptr;
    uint64_t cursor = 0, clips = 0;
    bool pushed = false;
    // UI units to the draw list's pixels (the vertices and clip rects are stored scaled: x * scale + offX), read back
    // from the clip rect pushed here
    float scale = 1.0f, offX = 0.0f, offY = 0.0f;
    float sqX = 0.0f, sqY = 0.0f, sqW = 0.0f, sqH = 0.0f; // the square drawn north up (UI units)
    void Begin(void* widgetRaw, float x, float y, float w, float h) {
        uiCtx = widgetRaw ? *reinterpret_cast<void**>(widgetRaw) : nullptr;
        if (!uiCtx || !g_pushUiClipRect || !g_popUiClipRect) return;
        dl = g_getUiDrawList ? static_cast<uint8_t*>(g_getUiDrawList(uiCtx)) : nullptr;
        if (dl) {
            base = *reinterpret_cast<uint8_t**>(dl + 0x35280);
            cursor = *reinterpret_cast<uint64_t*>(dl + 0x352c0);
            clips = *reinterpret_cast<uint64_t*>(dl + 0x31118);
        }
        sqX = x, sqY = y, sqW = w, sqH = h;
        float rect[4] = {x, y, w, h};
        g_pushUiClipRect(uiCtx, rect, 0); // not cut by the minimap's parent: the corners of the square are needed
        pushed = true;
        if (dl && w > 1.0f) {
            __try {
                const float* list = *reinterpret_cast<float**>(dl + 0x31110);
                const uint64_t n = *reinterpret_cast<uint64_t*>(dl + 0x31118);
                if (list && n == clips + 1) {
                    const float* r = list + clips * 4;
                    const float a = r[2] / w;
                    if (std::isfinite(a) && a > 0.05f && a < 20.0f) scale = a, offX = r[0] - x * a, offY = r[1] - y * a;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    // The game's World Map canvas writes its vertices in UI units (its own vertex shader scales them), unlike the rest:
    // those are turned round the player's spot in UI units
    uint8_t* nativeBase = nullptr;
    uint64_t nativeFrom = 0, nativeTo = 0;
    void NativeStart() {
        if (dl) nativeBase = *reinterpret_cast<uint8_t**>(dl + 0x35280), nativeFrom = *reinterpret_cast<uint64_t*>(dl + 0x352c0);
    }
    void NativeEnd() {
        if (!dl || !nativeBase) return;
        if (*reinterpret_cast<uint8_t**>(dl + 0x35280) != nativeBase) nativeBase = nullptr; // (a new buffer: left as is)
        else nativeTo = *reinterpret_cast<uint64_t*>(dl + 0x352c0);
    }
    void End(float vx, float vy, float vw, float vh, float cx, float cy, float c, float s) {
        const float uiCx = cx, uiCy = cy;
        vx = vx * scale + offX, vy = vy * scale + offY, vw *= scale, vh *= scale;
        cx = cx * scale + offX, cy = cy * scale + offY;
        if (dl) {
            __try {
                uint8_t* const nowBase = *reinterpret_cast<uint8_t**>(dl + 0x35280);
                const uint64_t nowCursor = *reinterpret_cast<uint64_t*>(dl + 0x352c0);
                // (a full buffer starts a new one: the vertices left in the old one stay as they are this frame)
                uint64_t from = nowBase == base ? (cursor + 23) / 24 * 24 : 0;
                if (MmTurnDebug() & 8) {
                    static uint64_t s_logTick = 0;
                    if (g_log && GetTickCount64() - s_logTick > 3000) {
                        s_logTick = GetTickCount64();
                        g_log("Minimap turn: %llu verts, clips %llu->%llu, scale %.3f off %.1f,%.1f pivot %.1f,%.1f (render %.1f,%.1f) player %.2f,%.2f",
                              (nowCursor - from) / 24, clips, *reinterpret_cast<uint64_t*>(dl + 0x31118), scale, offX, offY,
                              cx, cy, cx * scale + offX, cy * scale + offY, g_mmDebugPlayer[0], g_mmDebugPlayer[1]);
                        for (uint64_t o = from; o + 24 <= nowCursor && o < from + 24 * 30; o += 24) {
                            const float* v = reinterpret_cast<const float*>(nowBase + o);
                            g_log("  v %.1f,%.1f col %08x uv %.4f,%.4f g %.3f", v[0], v[1],
                                  reinterpret_cast<const uint32_t*>(v)[2], v[3], v[4], v[5]);
                        }
                        const float* list = *reinterpret_cast<float**>(dl + 0x31110);
                        for (uint64_t i = clips; list && i < *reinterpret_cast<uint64_t*>(dl + 0x31118); ++i)
                            g_log("  clip %llu: %.1f,%.1f %.1fx%.1f", i, list[i * 4], list[i * 4 + 1], list[i * 4 + 2], list[i * 4 + 3]);
                    }
                }
                if ((MmTurnDebug() & 2048) && nowBase && nowCursor >= from + 24 * 24) // dev: mirror vertices 16-23 left-right
                    for (uint64_t k = 16; k < 24; ++k) {
                        float* v = reinterpret_cast<float*>(nowBase + from + k * 24);
                        v[0] = 2.0f * (cx * scale + offX) - v[0];
                    }
                if ((MmTurnDebug() & 256) && nowBase && nowCursor >= from + 20 * 24) // dev: shift vertices 16-19 by 60 px
                    for (uint64_t k = 16; k < 20; ++k) reinterpret_cast<float*>(nowBase + from + k * 24)[0] += 60.0f;
                if (!(MmTurnDebug() & 1) && nowBase && nowCursor > from && nowCursor - from < (64ull << 20)) {
                    for (uint64_t o = from; o + 24 <= nowCursor; o += 24) {
                        const uint64_t k = (o - from) / 24; // dev: 32 skips vertices 16-19, 64 20-23, 128 0-15
                        if (((MmTurnDebug() & 32) && k >= 16 && k < 20) || ((MmTurnDebug() & 64) && k >= 20 && k < 24) ||
                            ((MmTurnDebug() & 128) && k < 16))
                            continue;
                        float* v = reinterpret_cast<float*>(nowBase + o);
                        const bool native = nowBase == nativeBase && o >= nativeFrom && o < nativeTo;
                        const float px = native ? uiCx : cx, py = native ? uiCy : cy;
                        const float x = v[0] - px, y = v[1] - py;
                        const float sk = s;
                        v[0] = px + x * c - y * sk;
                        v[1] = py + x * sk + y * c;
                    }
                }
                float* list = *reinterpret_cast<float**>(dl + 0x31110);
                const uint64_t n = *reinterpret_cast<uint64_t*>(dl + 0x31118);
                if (!(MmTurnDebug() & 2) && list && n > clips && n - clips < 4096) {
                    for (uint64_t i = clips; i < n; ++i) {
                        float* r = list + i * 4;
                        const float x0 = std::max(r[0], vx), y0 = std::max(r[1], vy);
                        const float x1 = std::min(r[0] + r[2], vx + vw), y1 = std::min(r[1] + r[3], vy + vh);
                        r[0] = x0, r[1] = y0, r[2] = std::max(0.0f, x1 - x0), r[3] = std::max(0.0f, y1 - y0);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (pushed) g_popUiClipRect(uiCtx);
        pushed = false;
    }
};

// The turn, eased: the minimap follows the way the character faces (radians, the screen's y down)
float MinimapTurnAngle(float faceDirX, float faceDirZ, bool on, float rate) {
    static float s_angle = 0.0f;
    static uint64_t s_last = 0;
    // a turn that takes the facing (x, -z on screen) to straight up
    const float target = -1.5707963f - std::atan2(-faceDirZ, faceDirX);
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = s_last && freq.QuadPart ? static_cast<float>(now.QuadPart - s_last) / freq.QuadPart : 1.0f;
    s_last = now.QuadPart;
    float d = std::remainder(target - s_angle, 6.2831853f);
    if (!on || !std::isfinite(d) || !std::isfinite(s_angle)) {
        s_angle = std::isfinite(target) ? target : 0.0f;
        return on ? s_angle : 0.0f;
    }
    s_angle += d * (1.0f - std::exp(-std::min(dt, 0.25f) * rate));
    s_angle = std::remainder(s_angle, 6.2831853f);
    return s_angle;
}

void DrawMinimapTerrain(uint8_t* client, void* widgetRaw,
                        float vx, float vy, float vw, float vh,
                        float vcx, float vcy,
                        float playerX, float playerZ,
                        float pxPerMeterX, float pxPerMeterY) {
    // Primary path: invoke the game's native World Map tile, shroud, deadly-shroud & fog renderer (0x140d67fc0)
    // inside a UI clip rect (0x1411a8270) so the minimap terrain + shroud + fog matches the World Map 1:1!
    // IMPORTANT: 0x140d67fc0 must run on ALL 4 UI passes (not just pass 3 when drawList != nullptr) so its
    // widget state (0x141185580) and WorldMapData / tile asset handles (0xe10655ee / 0xf9e86997) stay active!
    static bool diagnostic = false;
    volatile int stage = 0;
    if (client && widgetRaw && g_drawWorldMapCanvas && g_getUiDrawList && g_pushUiClipRect && g_popUiClipRect) {
        __try {
            stage = 1;
            void* uiCtx = *reinterpret_cast<void**>(widgetRaw);
            uint8_t* anchorNode = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(widgetRaw) + 8);
            void* drawList = g_getUiDrawList(uiCtx);
            // 0x141197d80(uiCtx) retrieves the current layer's root node from the layer stack at uiCtx + 0x4690 / +0x4698
            uint64_t layerStackCount = uiCtx ? *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(uiCtx) + 0x4698) : 0;
            uint8_t* layerStackBase = uiCtx ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(uiCtx) + 0x4690) : nullptr;
            uint8_t* curLayerEntry = (layerStackBase && layerStackCount > 0)
                                         ? *reinterpret_cast<uint8_t**>(layerStackBase + layerStackCount * 24 - 0x18)
                                         : nullptr;
            uint8_t* layerRootNode = curLayerEntry ? *reinterpret_cast<uint8_t**>(curLayerEntry + 0x50) : nullptr;
            if (!diagnostic && g_log) g_log("Terrain context: ctx=%p anchor=%p root=%p draw=%p", uiCtx, anchorNode, layerRootNode, drawList);
            if (uiCtx && anchorNode && layerRootNode && anchorNode != layerRootNode) {
                constexpr float kWorldSize = 10240.0f;
                const float zoom = pxPerMeterX;
                const float fullW = kWorldSize * pxPerMeterX;
                const float fullH = kWorldSize * pxPerMeterY;
                const float originX = vcx - playerX * pxPerMeterX;
                const float originY = vcy - (kWorldSize - playerZ) * pxPerMeterY;

                // 0x140d67fc0 reads the layer root node's +0x114 rect (via 0x141197d80) as the viewport culling rect
                // (xmm14) and the passed widget node's +0x114 rect as the full 10240x10240 canvas rect ([rbp+0x60]).
                float savedLayerRootRect[4]{};
                float savedAnchorRect[4]{};
                std::memcpy(savedLayerRootRect, layerRootNode + 0x114, sizeof(savedLayerRootRect));
                std::memcpy(savedAnchorRect, anchorNode + 0x114, sizeof(savedAnchorRect));

                float vpRect[4] = {vx, vy, vw, vh};
                float scRect[4] = {originX, originY, fullW, fullH};
                std::memcpy(layerRootNode + 0x114, vpRect, sizeof(vpRect));
                std::memcpy(anchorNode + 0x114, scRect, sizeof(scRect));

                alignas(16) uint8_t dummyMapView[0x220]{};
                *reinterpret_cast<float*>(dummyMapView + 0x10) = zoom;
                *reinterpret_cast<float*>(dummyMapView + 0x14) = zoom;
                *reinterpret_cast<float*>(dummyMapView + 0x38) = kWorldSize;
                *reinterpret_cast<float*>(dummyMapView + 0x3c) = kWorldSize;
                // Set base zoom scale (+0x58) so zoom / [dummyMapView + 0x58] selects the high-detail zoomed-in
                // cliff/terrain relief weights in 0x140d680e2!
                *reinterpret_cast<float*>(dummyMapView + 0x58) = std::max(0.05f, zoom * 0.35f);

                float clipRect[4] = {vx, vy, vw, vh};
                bool clipPushed = false;
                __try {
                    stage = 2;
                    g_pushUiClipRect(uiCtx, clipRect, 1);
                    clipPushed = true;
                    stage = 3;
                    g_drawWorldMapCanvas(widgetRaw, dummyMapView);
                    stage = 4;
                } __finally {
                    if (clipPushed) g_popUiClipRect(uiCtx);
                    std::memcpy(anchorNode + 0x114, savedAnchorRect, sizeof(savedAnchorRect));
                    std::memcpy(layerRootNode + 0x114, savedLayerRootRect, sizeof(savedLayerRootRect));
                }

                // On the render pass (when drawList != nullptr), 0x140d67fc0 draws terrain+shroud at sub-layer 0x80
                // and fog of war at sub-layer 0x82. Advance the current layer's sub-layer to 0x84 so all subsequent
                // minimap icons, frame elements, and labels sort cleanly above both terrain and fog!
                if (drawList) {
                    uint16_t curSortKey = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(uiCtx) + 0x6ce8);
                    uint16_t aboveFogKey = static_cast<uint16_t>((curSortKey & 0xFF00u) | 0x0084u);
                    *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(uiCtx) + 0x6ce8) = aboveFogKey;
                    *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(drawList) + 0x35214) = aboveFogKey;
                    *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(drawList) + 0x352d2) = 0;
                }
                if (!diagnostic && g_log) g_log("Terrain: native World Map renderer active");
                diagnostic = true;
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!diagnostic && g_log) g_log("Terrain failed: stage=%d exception=%08x", stage, GetExceptionCode());
        }
    }
    diagnostic = true;

    // Leave the neutral map background on unsupported builds. Raw height tiles are shader inputs,
    // not a usable map image; drawing them directly misrepresents the terrain.
}

// The position plate's copy button: "X 3178, Y 828, Z 2003" on the clipboard
uint64_t g_positionCopiedTick = 0;
void CopyTextToClipboard(const char* text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t n = std::strlen(text) + 1;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n)) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, text, n);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_TEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void DrawHudMinimapLayerBody(uint8_t* client, HWND foreground, bool appFocused, uint8_t screen, float playerX, float playerZ, Widget& anchor) {

    float rootRect[4]{};
    if (anchor.Handle()) {
        std::memcpy(rootRect, anchor.Handle() + 0x114, sizeof(rootRect));
    }

    // Fallback to UI window canvas dimensions if rootRect is ever uninitialized
    if (rootRect[2] <= 100.0f || rootRect[3] <= 100.0f) {
        if (auto* g = static_cast<uint8_t*>(GetUiG())) {
            if (uint8_t* uiWin = *reinterpret_cast<uint8_t**>(g + 0x10)) {
                float winW = *reinterpret_cast<float*>(uiWin + 0x20);
                float winH = *reinterpret_cast<float*>(uiWin + 0x24);
                if (winW > 100.0f && winH > 100.0f) {
                    rootRect[0] = 0.0f;
                    rootRect[1] = 0.0f;
                    rootRect[2] = winW;
                    rootRect[3] = winH;
                }
            }
        }
    }

    const float rootX = rootRect[0];
    const float rootY = rootRect[1];
    const float rootW = (rootRect[2] > 100.0f) ? rootRect[2] : 1920.0f;
    const float rootH = (rootRect[3] > 100.0f) ? rootRect[3] : 1080.0f;

    float frameSize = std::max(150.0f, std::min(kMinimapMaxSize, g_minimapSize));
    float frameX = g_minimapPosX;
    float frameY = g_minimapPosY;

    // Frame units (the art's 300-unit grid) to px. Up to the 370 px design size the whole frame
    // scales; past it the ornaments keep their size and only the rails get longer.
    const float artUnit = std::min(frameSize, kMinimapArtDesign) / 300.0f;
    const float U = kMinimapArtUnit; // the medallion always draws at the design size
    char timeBuf[96]{};
    bool isDay = true;
    int phase = 1;
    int weatherType = 0;
    FormatInGameTimeOfDay(client, playerX, playerZ, timeBuf, sizeof(timeBuf), isDay, phase, weatherType);
    char timeText[48]{};
    const char* statusText = "";
    if (const char* bar = std::strstr(timeBuf, "  |  ")) {
        std::snprintf(timeText, sizeof(timeText), "%.*s", static_cast<int>(bar - timeBuf), timeBuf);
        statusText = bar + 5;
    } else {
        std::snprintf(timeText, sizeof(timeText), "%s", timeBuf);
    }
    // The info bar: at the design size down to the default minimap (236 px), smaller with a smaller one (requested by
    // yassifecade): art, text, icons and buttons together, and never wider than the minimap (its own edge with the thin
    // border, a little inside the corner rings with the ornate one)
    const float fullTimeW = umm_lang::TextWidth(timeText) * 1.12f + 2.0f;
    const float fullStatusW = umm_lang::TextWidth(statusText) * 1.12f + 2.0f;
    const float fullTrayW = std::max(2.0f * std::max(28.2f * U + 6.0f + 22.0f + fullTimeW + 10.0f,
                                                      46.2f * U + 6.0f + fullStatusW + 10.0f),
                                     129.0f * U);
    const float fitW = frameSize * (g_minimapThinBorder ? 1.0f : 0.84f); // the corner rings span about 85% of the frame
    const float TS = std::max(0.35f, std::min({1.0f, frameSize / 236.0f, fitW / fullTrayW}));
    const float UT = U * TS;
    // The tray hangs from the bottom rail: its art (31 units tall) starts just under the rail top
    // (263 units), so its top edge stays hidden behind the rail.
    const bool thinBorder = g_minimapThinBorder;
    const bool infoBar = g_minimapInfoBar;
    // With the thin border there is no rail to tuck it behind: it hangs from the map's bottom edge instead.
    constexpr float kThinTrayTuck = 2.4f; // the art units the rail would hide
    const float trayTopRel = thinBorder ? frameSize - kThinTrayTuck * UT : frameSize - 37.0f * artUnit + 0.6f * UT;
    constexpr float kPositionLineH = 26.0f;
    const float trayBottomRel = (infoBar ? trayTopRel + 31.0f * UT : frameSize) +
                                (g_minimapShowPosition ? kPositionLineH + 4.0f : 0.0f);

    if (!g_minimapCustomPos) {
        // Enshrouded constrains its HUD inside a 21:9 safe area centered on ultra-wide monitors
        float hudW = std::min(rootW, rootH * (21.0f / 9.0f));
        float hudRight = rootX + 0.5f * (rootW + hudW) - 24.0f;
        frameX = hudRight - frameSize;
        frameY = rootY + 132.0f;
        g_minimapPosX = frameX;
        g_minimapPosY = frameY;
    }

    // Clamp inside screen bounds
    frameX = std::max(rootX - frameSize * 0.05f, std::min(rootX + rootW - frameSize * 0.95f, frameX));
    frameY = std::max(rootY - frameSize * 0.05f, std::min(rootY + rootH - trayBottomRel - 6.0f, frameY));

    // Inner map viewport: the inner edges of the frame's inner rails, or almost the whole frame with the thin border.
    constexpr float kThinInset = 3.0f;
    const float vx = thinBorder ? frameX + kThinInset : frameX + 36.0f * artUnit;
    const float vy = thinBorder ? frameY + kThinInset : frameY + 41.4f * artUnit;
    const float vw = thinBorder ? frameSize - 2.0f * kThinInset : frameSize - 72.0f * artUnit;
    const float vh = thinBorder ? frameSize - 2.0f * kThinInset : frameSize - 86.4f * artUnit;
    const float vcx = vx + vw * 0.5f;
    const float vcy = vy + vh * 0.5f;

    // Markers and the player arrow grow with the frame only up to the design size.
    const float btnScale = std::max(0.82f, std::min(frameSize, kMinimapArtDesign) / 236.0f);

    // Bottom-right corner resize grip (over the bottom-right golden corner ring)
    // (with the thin border: the bottom-right corner of the map)
    const float resizeGripX0 = thinBorder ? frameX + frameSize - 24.0f : frameX + frameSize - 72.0f * artUnit;
    const float resizeGripY0 = thinBorder ? frameY + frameSize - 24.0f : frameY + frameSize - 81.0f * artUnit;
    const float resizeGripX1 = thinBorder ? frameX + frameSize : frameX + frameSize - 12.0f * artUnit;
    const float resizeGripY1 = thinBorder ? frameY + frameSize : frameY + frameSize - 21.0f * artUnit;

    // Control tray, centred under the frame and tucked behind its bottom rail, one size at any
    // frame size:  [gear | icon time  <frame pendant>  weather | - | +]
    const float frameCx = frameX + frameSize * 0.5f;
    if (!g_filterNotice.empty() && g_filterNoticeOnHud) { // left of the minimap, or right where there is no room
        const float noticeW = FilterNoticeWidth(rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
        const float noticeX = frameX - noticeW - 10.0f >= rootRect[0] ? frameX - noticeW - 10.0f : frameX + frameSize + 10.0f;
        DrawFilterNotice(anchor.raw, rootRect, true, noticeX, frameY + frameSize * 0.5f - 15.0f);
    }
    const float compassCy = frameY + 30.5f * artUnit;
    const float trayY = frameY + trayTopRel;
    const float trayH = 31.0f * UT;
    const float trayContentCy = trayY + 17.5f * UT;    // middle of the part below the rail
    const float gearCapW = 28.2f * UT;                  // tray point to the end of the left flourish
    const float zoomCapW = 46.2f * UT;                  // right flourish to the tray point
    const float trayIconW = 22.0f * TS;
    const float trayCentreGap = 10.0f * TS;             // each side of the tray's centre divider
    // TextWidth is a per-glyph estimate; the tray font runs about a tenth wider, so leave room.
    const float timeTextW = fullTimeW * TS;
    const float statusTextW = fullStatusW * TS;
    const float trayHalfW = std::max(gearCapW + 6.0f * TS + trayIconW + timeTextW + trayCentreGap,
                                     zoomCapW + 6.0f * TS + statusTextW + trayCentreGap);
    const float trayBaseW = 219.0f * UT;                // the art's own width, point to point
    const float trayW = std::max(trayHalfW * 2.0f, trayBaseW - 90.0f * UT);
    const float trayX = frameCx - trayW * 0.5f;
    const float trayRight = trayX + trayW;
    const float trayButtonsY = thinBorder ? frameY + frameSize : frameY + frameSize - 34.0f * artUnit; // below the rail
    const float trayButtonsH = trayY + trayH - trayButtonsY;
    // Without the info bar the cog sits in the map's bottom-left corner.
    constexpr float kCornerGearSz = 24.0f;
    const float gearBtnX = infoBar ? trayX + 2.0f : vx + 4.0f;
    const float gearBtnW = infoBar ? gearCapW - 2.0f : kCornerGearSz;
    const float gearBtnY = infoBar ? trayButtonsY : vy + vh - 4.0f - kCornerGearSz;
    const float gearBtnH = infoBar ? trayButtonsH : kCornerGearSz;
    const float minusBtnX = trayRight - 42.0f * UT;
    const float plusBtnX = trayRight - 23.0f * UT;
    const float zoomBtnW = 19.0f * UT;

    // Settings > Advanced > Position: a plate under the tray and a little narrower, three equal cells and a copy button
    constexpr float kCopyBtnW = 26.0f;
    const float posPlateW = trayW / TS - 8.0f * U; // the bar's full size: its numbers stay readable
    const float posCellW = (posPlateW - 8.0f - kCopyBtnW) / 3.0f;
    const float posLetterW = 13.0f;
    const float posDigitsW = posCellW - 6.0f - posLetterW - 6.0f;
    const float posPlateX = frameCx - posPlateW * 0.5f;
    const float posPlateY = frameY + trayBottomRel - kPositionLineH;
    const float copyBtnX = posPlateX + posPlateW - 4.0f - kCopyBtnW;

    const float mmSettingsW = SettingsCardWidth();
    const float mmSettingsX = (frameX - mmSettingsW - 10.0f >= rootX + 10.0f)
                                  ? (frameX - mmSettingsW - 10.0f)
                                  : std::min(rootX + rootW - mmSettingsW - 10.0f, frameX + frameSize + 10.0f);
    const float mmSettingsY = std::max(rootY + 20.0f, std::min(rootY + rootH - kSettingsCardH - 20.0f, frameY));

    // Hotkeys: '[' (zoom out) and ']' (zoom in), or Numpad '-' / '+'
    if (appFocused && !g_findDialogOpen) {
        bool zoomOutKey = BindingDown(g_keys.miniZoomOut);
        bool zoomInKey  = BindingDown(g_keys.miniZoomIn);
        if (zoomOutKey && !g_minimapZoomOutKeyWasDown) {
            if (MinimapZoom() > 0) {
                --MinimapZoom();
                SaveMinimapConfig();
            }
        }
        if (zoomInKey && !g_minimapZoomInKeyWasDown) {
            if (MinimapZoom() + 1 < MinimapZoomCount()) {
                ++MinimapZoom();
                SaveMinimapConfig();
            }
        }
        g_minimapZoomOutKeyWasDown = zoomOutKey;
        g_minimapZoomInKeyWasDown = zoomInKey;
    } else {
        g_minimapZoomOutKeyWasDown = false;
        g_minimapZoomInKeyWasDown = false;
    }

    // Holding Shift + Alt ([Keys] MinimapMouse; or having the Minimap Settings card open) unlocks the mouse cursor
    const bool shiftAltHeld = appFocused && MinimapMouseKeysHeld();
    const bool lockCameraForMouse = appFocused &&
                                    (shiftAltHeld || g_minimapSettingsOpen || g_minimapDraggingMove || g_minimapDraggingResize);
    SetMinimapMouseControl(lockCameraForMouse, foreground);

    // Handle mouse wheel zoom while Shift+Alt is held
    LONG wheelDelta = InterlockedExchange(&g_minimapWheelDelta, 0);
    if (shiftAltHeld && wheelDelta != 0) {
        if (wheelDelta > 0 && MinimapZoom() + 1 < MinimapZoomCount()) {
            ++MinimapZoom();
            SaveMinimapConfig();
        } else if (wheelDelta < 0 && MinimapZoom() > 0) {
            --MinimapZoom();
            SaveMinimapConfig();
        }
    }

    float mouseX = -10000.0f, mouseY = -10000.0f;
    bool cursorVisible = false;
    if (appFocused) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0) {
            cursorVisible = true;
        }
        const bool minimapHasMouse = shiftAltHeld || g_minimapSettingsOpen || g_minimapDraggingMove || g_minimapDraggingResize;
        if (minimapHasMouse) {
            cursorVisible = true;
        } else if (screen != 1) {
            // In the backpack, journal and other menus the mouse is theirs: a click over the minimap must not start
            // a drag (which swallowed the game's button-up, so items stuck to the cursor). Shift + Alt still works.
            cursorVisible = false;
        }
        if (cursorVisible) {
            POINT pt{};
            RECT cr{};
            if (GetCursorPos(&pt) && ScreenToClient(foreground, &pt) &&
                GetClientRect(foreground, &cr) && cr.right > 0 && cr.bottom > 0) {
                mouseX = rootX + pt.x * rootW / static_cast<float>(cr.right);
                mouseY = rootY + pt.y * rootH / static_cast<float>(cr.bottom);
            }
        }
    }

    const float radiusMeters = MinimapRadius();
    // Settings > Minimap Rotation > Facing Up: the map turns round you (one scale both ways, so it doesn't stretch)
    const bool turnMap = g_minimapHeadingUp;
    const float pxPerMeterX = (vw * 0.5f) / radiusMeters;
    const float pxPerMeterY = turnMap ? pxPerMeterX : (vh * 0.5f) / radiusMeters;
    float camDirX = 0.0f, camDirZ = 1.0f;
    float faceDirX = 0.0f, faceDirZ = 1.0f;
    GetHudPlayerFacingDirs(client, playerX, playerZ, camDirX, camDirZ, faceDirX, faceDirZ);
    // (by the character's facing, eased: it snaps round when it strafes or attacks)
    float mmTurn = MinimapTurnAngle(faceDirX, faceDirZ, turnMap, 16.0f);
#ifdef UMM_DEV_BRIDGE
    map_shader::LogCalls();
#endif
    if (turnMap && (MmTurnDebug() & 512)) mmTurn = static_cast<float>(MmTurnDebug() >> 12) * 0.05f; // dev: a fixed turn (bits 12+: 0.05 rad steps)
    g_mmRotC = std::cos(mmTurn);
    g_mmRotS = std::sin(mmTurn);

    const bool leftDown = appFocused && cursorVisible && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    const bool leftPressed = leftDown && !g_minimapMouseWasDown;
    const bool leftReleased = !leftDown && g_minimapMouseWasDown;
    g_minimapMouseWasDown = leftDown;

    auto inRect = [&](float rx, float ry, float rw, float rh) -> bool {
        return mouseX >= rx && mouseX <= (rx + rw) && mouseY >= ry && mouseY <= (ry + rh);
    };

    const bool hoverSettingsCard = cursorVisible && g_minimapSettingsOpen &&
                                   inRect(mmSettingsX, mmSettingsY, mmSettingsW, kSettingsCardH);

    // Show Minimap off with the info bar on keeps the bar alone (time, weather, cog), where the minimap's bottom was.
    const bool barOnly = !g_minimapVisible && infoBar;

    // If the user turned "Show Minimap" OFF while the Minimap's Settings card is open,
    // hide the minimap immediately while keeping the Settings card open until closed!
    if (!g_minimapVisible && !barOnly) {
        g_minimapDraggingMove = false;
        g_minimapDraggingResize = false;
        if (leftPressed && !hoverSettingsCard) {
            g_minimapSettingsOpen = false;
        }
        // Keep the child mmCanvas widget in the UI tree so pass 0..3 stay balanced
        static const UiText kMmCanvasName = MakeUiText("minimapHudCanvas");
        Widget mmCanvas{};
        g_namedWidget(mmCanvas.raw, &kMmCanvasName);
        g_setSize(mmCanvas.Handle(), 0);
        g_endWidget(mmCanvas.raw);

        if (g_minimapSettingsOpen) {
            DrawSettingsCardAbs(anchor.raw, mmSettingsX, mmSettingsY,
                                mouseX, mouseY, cursorVisible, leftPressed, g_minimapSettingsOpen);
        }
        return;
    }

    const bool hoverMinus   = infoBar && cursorVisible && !hoverSettingsCard && inRect(minusBtnX, trayButtonsY, zoomBtnW, trayButtonsH);
    const bool hoverPlus    = infoBar && cursorVisible && !hoverSettingsCard && inRect(plusBtnX, trayButtonsY, zoomBtnW, trayButtonsH);
    const bool hoverGear    = cursorVisible && !hoverSettingsCard && inRect(gearBtnX, gearBtnY, gearBtnW, gearBtnH);
    const bool hoverCopy    = g_minimapShowPosition && cursorVisible && !hoverSettingsCard &&
                              inRect(copyBtnX, posPlateY, kCopyBtnW, kPositionLineH);
    const bool hoverResize  = !barOnly && cursorVisible && !hoverSettingsCard && !hoverPlus && !hoverMinus && !hoverGear &&
                              (mouseX >= resizeGripX0 && mouseX <= resizeGripX1 &&
                               mouseY >= resizeGripY0 && mouseY <= resizeGripY1);
    const bool hoverWholeWidget = cursorVisible && !hoverSettingsCard && !hoverMinus && !hoverPlus && !hoverGear && !hoverCopy &&
                                  ((!barOnly && (thinBorder ? inRect(frameX, frameY, frameSize, frameSize)
                                                            : inRect(frameX + frameSize * 0.06f, frameY + frameSize * 0.04f,
                                                                     frameSize * 0.88f, frameSize * 0.90f))) ||
                                   (infoBar && inRect(trayX, trayButtonsY, trayW, trayButtonsH)));

    if (leftPressed && !hoverSettingsCard) {
        if (hoverMinus) {
            if (MinimapZoom() > 0) {
                --MinimapZoom();
                SaveMinimapConfig();
            }
        } else if (hoverPlus) {
            if (MinimapZoom() + 1 < MinimapZoomCount()) {
                ++MinimapZoom();
                SaveMinimapConfig();
            }
        } else if (hoverGear) {
            g_minimapSettingsOpen = !g_minimapSettingsOpen;
        } else if (hoverCopy) {
            float y = 0.0f;
            if (LocalPlayerY(client, y)) {
                char text[64]{};
                std::snprintf(text, sizeof(text), "X %ld, Y %ld, Z %ld", std::lround(playerX), std::lround(y),
                              std::lround(playerZ));
                CopyTextToClipboard(text);
                g_positionCopiedTick = GetTickCount64();
            }
        } else if (hoverResize) {
            g_minimapDraggingResize = true;
            g_minimapDragStartMouseX = mouseX;
            g_minimapDragStartMouseY = mouseY;
            g_minimapDragStartSize = frameSize;
            g_minimapDragStartPosX = frameX;
            g_minimapDragStartPosY = frameY;
        } else if (hoverWholeWidget) {
            // Ctrl + Left-Click on a chest icon inside the minimap viewport clears/restores it
            bool toggledChest = false;
            if (!barOnly && inp::KeysHeld(g_keys.chestClick, KeyIsDown) && inRect(vx, vy, vw, vh) && pxPerMeterX > 0.01f) {
                float clickDx = (mouseX - vcx) / pxPerMeterX, clickDz = -(mouseY - vcy) / pxPerMeterY;
                MmUnrotate(clickDx, clickDz); // (a turned minimap: back to the world's way round)
                float clickWx = playerX + clickDx;
                float clickWz = playerZ + clickDz;
                float maxRadiusM = std::max(12.0f, 14.0f / pxPerMeterX);
                const float shiftM = 13.0f * btnScale * 0.6f / pxPerMeterX; // as the deposits are drawn aside
                const float nearR = std::max(10.0f, 10.0f / pxPerMeterX);
                int32_t placeX = 0, placeZ = 0;
                int kind = ToggleManualBossNear(clickWx, clickWz, nearR, shiftM)                   ? 1
                                 : ToggleUniqueFoundNear(clickWx, clickWz, nearR, shiftM)                ? 2
                                 : ToggleLoreReadNear(clickWx, clickWz, nearR, shiftM, maxRadiusM)       ? 3
                                 : ToggleChestClearedNearWorldPos(clickWx, clickWz, maxRadiusM)          ? 4
                                 : ToggleDonePlaceAt(client, clickWx, clickWz, std::max(8.0f, 12.0f / pxPerMeterX), &placeX, &placeZ) ? 5
                                                                                                         : 0;
                if (kind == 5) NoteMarkStep(5, static_cast<float>(placeX), static_cast<float>(placeZ), 0, 0, 0, false);
                else NoteMarkStep(kind, clickWx, clickWz, nearR, shiftM, maxRadiusM, false);
                // Nothing marked, but a marker under the cursor: it can't be marked (a quest, an Elixir Well, an ore)
                HideRowTarget under;
                if (kind == 0 && HideRowTargetAt(client, static_cast<int32_t>(clickWx), static_cast<int32_t>(clickWz),
                                                 std::max(6.0f, 9.0f / pxPerMeterX), 0u, under)) {
                    AnnounceCantMark(under.hashes[0]);
                    kind = -1;
                }
                if (kind != 0) {
                    toggledChest = true;
                    g_filterNoticeOnHud = true;
                }
            }
            if (!toggledChest) {
                uint64_t nowTick = GetTickCount64();
                // Double-clicking the top compass star resets to default position under Journal [J]
                if (inRect(frameCx - 30.0f * artUnit, frameY, 60.0f * artUnit, 50.0f * artUnit) &&
                    (nowTick - g_minimapLastHeaderClickTick) < 400ULL) {
                    g_minimapCustomPos = false;
                    SaveMinimapConfig();
                } else {
                    g_minimapLastHeaderClickTick = nowTick;
                    g_minimapDraggingMove = true;
                    g_minimapDragStartMouseX = mouseX;
                    g_minimapDragStartMouseY = mouseY;
                    g_minimapDragStartPosX = frameX;
                    g_minimapDragStartPosY = frameY;
                }
            }
        } else if (g_minimapSettingsOpen) {
            // Clicking outside the settings card and outside the minimap closes the settings card
            g_minimapSettingsOpen = false;
        }
    }

    if (g_minimapDraggingResize) {
        if (leftDown) {
            float delta = std::max(mouseX - g_minimapDragStartMouseX, mouseY - g_minimapDragStartMouseY);
            g_minimapSize = std::max(150.0f, std::min(kMinimapMaxSize, g_minimapDragStartSize + delta));
            if (!g_minimapCustomPos) {
                // Keep top-right anchored while resizing in default slot
                g_minimapPosX = g_minimapDragStartPosX + (g_minimapDragStartSize - g_minimapSize);
                g_minimapPosY = g_minimapDragStartPosY;
            }
        }
        if (leftReleased) {
            g_minimapDraggingResize = false;
            SaveMinimapConfig();
        }
    } else if (g_minimapDraggingMove) {
        if (leftDown) {
            float dx = mouseX - g_minimapDragStartMouseX;
            float dy = mouseY - g_minimapDragStartMouseY;
            if (std::abs(dx) > 2.0f || std::abs(dy) > 2.0f) {
                g_minimapCustomPos = true;
                g_minimapPosX = g_minimapDragStartPosX + dx;
                g_minimapPosY = g_minimapDragStartPosY + dy;
            }
        }
        if (leftReleased) {
            g_minimapDraggingMove = false;
            SaveMinimapConfig();
        }
    }

    if (barOnly) {
        // Keep the child mmCanvas widget in the UI tree so pass 0..3 stay balanced
        static const UiText kMmCanvasName = MakeUiText("minimapHudCanvas");
        Widget mmCanvas{};
        g_namedWidget(mmCanvas.raw, &kMmCanvasName);
        g_setSize(mmCanvas.Handle(), 0);
        g_endWidget(mmCanvas.raw);
        DrawArtSprite(anchor.raw, kArtTray, trayX - 40.5f * UT, trayY - 262.0f * UT, TS, (trayW - trayBaseW) / TS);
    } else {
        // ------------------------------------------------------------------------
        // 1. Draw Inner Map Viewport Background & Optional Terrain + Shroud + Fog
        // ------------------------------------------------------------------------
        DrawSolidRectAbs(anchor.raw, vx - 2.0f, vy - 2.0f, vw + 4.0f, vh + 4.0f, 0xFF141816u, 0.94f);
        DrawSolidRectAbs(anchor.raw, vx, vy, vw, vh, 0xFF1C211Eu, 0.92f);

        // Named child canvas widget for DrawMinimapTerrain (created on every pass so the UI tree stays balanced,
        // its WidgetNode is distinct from the layer root node anchor.Handle(), and its ID at +0x30 persists for 0x141185580!)
        static const UiText kMmCanvasName = MakeUiText("minimapHudCanvas");
        Widget mmCanvas{};
        g_namedWidget(mmCanvas.raw, &kMmCanvasName);
        g_setSize(mmCanvas.Handle(), 0);
        UMM_LAP_START;
        // Facing Up: drawn north up over the square round the turned view, then turned (MmTurnWindow)
        const float turnHalf = (MmTurnDebug() & 4) ? std::max(vw, vh) * 0.5f : std::sqrt(vw * vw + vh * vh) * 0.5f + 2.0f;
        const float tx = turnMap ? vcx - turnHalf : vx, ty = turnMap ? vcy - turnHalf : vy;
        const float tw = turnMap ? 2.0f * turnHalf : vw, th = turnMap ? 2.0f * turnHalf : vh;
        MmTurnWindow turnWindow;
        g_mmDebugPlayer[0] = playerX, g_mmDebugPlayer[1] = playerZ;
        if (turnMap) turnWindow.Begin(mmCanvas.raw, tx, ty, tw, th);
        // In a Hollow Hall the dungeon's floor plan and your footprints, by level (halls.inl); elsewhere the terrain
        float hallY = 0.0f;
        bool mmInVault = false; // in an Ancient Vault or Spire: its fast travel point isn't drawn (a one-way pad there)
        char floorText[64]{};
        if (g_dungeonMaps && halls::Update(client, playerX, playerZ, g_lootSection, hallY)) {
            g_minimapInSpire = halls::InSpire(playerX, hallY, playerZ);
            mmInVault = halls::VaultAt(playerX, hallY, playerZ) >= 0;
            halls::Draw(mmCanvas.raw, tx, ty, tw, th, vcx, vcy, playerX, hallY, playerZ, pxPerMeterX, pxPerMeterY);
            // In a vault or spire: the floor you're on, top left ("Floor 2 / 3"; drawn after the turned part)
            int floor = 0, floors = 0;
            if (halls::MinimapFloor(playerX, hallY, playerZ, floor, floors))
                std::snprintf(floorText, sizeof(floorText), "%s %d / %d", umm_lang::T(umm_lang::Str::Floor), floor + 1, floors);
        } else if ((g_minimapInSpire = false), g_minimapShowTerrain) {
            if (turnMap) turnWindow.NativeStart();
            DrawMinimapTerrain(client, mmCanvas.raw, tx, ty, tw, th, vcx, vcy, playerX, playerZ, pxPerMeterX, pxPerMeterY);
            if (turnMap) turnWindow.NativeEnd();
        }
        if (!g_minimapInSpire && !halls::InCorner(playerX, playerZ) && halls::VaultAt(playerX, hallY, playerZ) < 0)
            roads::DrawOnMinimap(mmCanvas.raw, tx, ty, tw, th, vcx, vcy, playerX, playerZ, pxPerMeterX); // Roads, Surface Water
        if (turnMap) turnWindow.End(vx, vy, vw, vh, vcx, vcy, g_mmRotC, g_mmRotS);
        if (floorText[0]) {
            const float fw = umm_lang::TextWidth(floorText) * 1.25f + 16.0f, fh = 24.0f;
            DrawSolidRectAbs(mmCanvas.raw, vx + 6.0f, vy + 6.0f, fw, fh, 0xFF0A0C0Bu, 0.7f);
            DrawLabelAbs(vx + 6.0f, vy + 6.0f, fw, fh, floorText, 0xFFE8E4DCu, 0.95f, TextAlignH::Center, TextAlignV::Center);
        }
        g_endWidget(mmCanvas.raw);
        UMM_LAP(PerfMmTerrain);

        // Subtle inner vignette / shadow along viewport edges
        DrawSolidRectAbs(anchor.raw, vx, vy, vw, 5.0f, 0xFF0A0C0Bu, 0.55f);
        DrawSolidRectAbs(anchor.raw, vx, vy + vh - 5.0f, vw, 5.0f, 0xFF0A0C0Bu, 0.55f);
        DrawSolidRectAbs(anchor.raw, vx, vy, 5.0f, vh, 0xFF0A0C0Bu, 0.55f);
        DrawSolidRectAbs(anchor.raw, vx + vw - 5.0f, vy, 5.0f, vh, 0xFF0A0C0Bu, 0.55f);

        // ------------------------------------------------------------------------
        // 2. Collect & Draw Real World Map Markers (Synced with Big Map Filters)
        // ------------------------------------------------------------------------
        void* mgr = *reinterpret_cast<void**>(client + 0x4667f0);
        auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
        size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
        const bool hideClearedOnBigMap = (client[0x4642b1] != 0);
        const float customScale = kMarkerScaleLevels[g_customMarkerScaleIdx];
        // Vanilla Minimap: the game's markers as its World Map shows them (every pin it has, no Completed Places: Hide)
        const bool vanillaData = VanillaMinimapData();
        const PinRevealMode pinMode = vanillaData ? PinRevealMode::RevealAll : g_pinRevealMode;

        uint64_t nowTick = GetTickCount64();
        const bool pulseActive = !g_pulsingPlaces.empty() && (nowTick - g_pulseStartTick <= 14000ULL);

        std::vector<MinimapMarkerDraw> drawList;
        drawList.reserve(96);

        constexpr size_t kMarkerArrays[4] = { 0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78 };
        const float iconPad = 10.0f * btnScale;

        float graveY = 0.0f;
        const bool haveGraveY = LocalPlayerY(client, graveY);
        // 2a. Non-chest markers from arr0..arr3 (read-only so arrays never flicker!)
        for (size_t a = 0; a < 4; ++a) {
            uint8_t* base = *reinterpret_cast<uint8_t**>(client + kMarkerArrays[a]);
            uint64_t count = *reinterpret_cast<uint64_t*>(client + kMarkerArrays[a] + 8);
            if (!base || count == 0 || count > 10000) continue;

            for (uint64_t i = 0; i < count; ++i) {
                uint8_t* m = base + i * 0xA0;
                uint32_t uid = *reinterpret_cast<uint32_t*>(m + 0x00);
                // Skip injected chest markers here; we render chests directly from kChestSpawns below so they never flicker!
                if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) continue;

                uint32_t origHash = GetMarkerOrigTypeHash(m);
                if (origHash == 0u) continue;
                // Skip duplicate grayed-out spire twin (0x98b2d65d) because 0xa17072be holds the spire
                if (origHash == 0x98b2d65du) continue;

                // On the minimap at all? First: the quest lookups below are the game's own calls, for every marker
                int32_t rawMx = *reinterpret_cast<int32_t*>(m + 0x14);
                int32_t rawMz = *reinterpret_cast<int32_t*>(m + 0x24);
                float mx = static_cast<float>(rawMx) +
                           static_cast<float>(*reinterpret_cast<uint16_t*>(m + 0x10)) / 65536.0f;
                float mz = static_cast<float>(rawMz) +
                           static_cast<float>(*reinterpret_cast<uint16_t*>(m + 0x20)) / 65536.0f;
                if (mx <= 1.0f && mz <= 1.0f) continue;

                float dxMeters = mx - playerX;
                float dzMeters = mz - playerZ;
                MmRotate(dxMeters, dzMeters); // (the minimap's view: turned with Facing Up)
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) {
                    continue;
                }

                float sx = vcx + dxMeters * pxPerMeterX;
                float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                    sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                    continue;
                }

                const bool isSlotSaved = IsSlotSaved(m);
                uint32_t savedId08 = isSlotSaved ? *reinterpret_cast<uint32_t*>(m + 0x4c)
                                                 : *reinterpret_cast<uint32_t*>(m + 0x08);
                if (savedId08 == 0u) savedId08 = *reinterpret_cast<uint32_t*>(m + 0x08);
                const uint32_t markerState = static_cast<uint32_t>(m[0x34]);

                uint32_t normHash = (origHash == 0xafce0c5fu)
                                        ? ClassifyQuestOrRumorMarker(savedId08, m[0x35])
                                        : NormalizeFilterTypeHash(origHash);
                if (normHash == 0u || GameNeverShowsPlace(savedId08)) continue;
                // A main quest's area circle (Plains For First Base): only while its quest is on, like the world map
                if (origHash == 0x454e24f2u && !IsActiveQuestArea(savedId08, m[0x35])) continue;
                const bool visitedSanctum = IsVisitedSanctumMarker(m, origHash);
                const bool handDone = IsHandDoneMarker(m, origHash);
                // A place with an unread lore rumor about it: the game's "?" in its stead, by the game's rule (as on the
                // World Map), before its own row or Completed Places is asked
                const uint32_t placeQuestHash = (origHash != 0xafce0c5fu && origHash != 0x454e24f2u)
                                                    ? GetQuestFilterHashForMarkerId(savedId08, true)
                                                    : 0u;
                const bool rumorHere = origHash != 0xafce0c5fu && origHash != 0x454e24f2u && placeQuestHash == 0u &&
                                       !hideClearedOnBigMap && GameShowsMarker(m[0x35]) &&
                                       IsFilterEnabled(filterEntries, filterCount, 0x8ba7c122u) && g_isLoreRumorMarkerId &&
                                       savedId08 != 0u && g_isLoreRumorMarkerId(savedId08) && LoreRumorFound(client, savedId08) != 1;
                // Inside a vault or spire: no fast travel point or spire marker (the spire's top one sits on its one-way
                // teleport pad, which the dungeon view shows; 2026-11-09)
                if (mmInVault && (origHash == 0x1095684eu || origHash == kLockedFastTravelTypeHash || origHash == 0xa17072beu ||
                                  origHash == 0x98b2d65du || normHash == 0x1095684eu || normHash == 0xa17072beu)) continue;
                if (rumorHere) {
                    normHash = 0x8ba7c122u;
                } else {
                    if (!vanillaData && ((g_hideCompletedPlaces && (markerState == 3u || visitedSanctum)) ||
                                         (handDone && g_hideCompletedPlaces)) && origHash != 0xafce0c5fu &&
                        !IsWaypointMarker(m)) continue; // Completed Places: Hide, a place marked done by hand too
                                                        // (not the place your waypoint is on)
                    if (!IsFilterEnabled(filterEntries, filterCount, normHash)) {
                        if (placeQuestHash != 0u && IsFilterEnabled(filterEntries, filterCount, placeQuestHash)) normHash = placeQuestHash;
                        else continue;
                    }
                }

                // Respect PinRevealMode (Explored / Hybrid / Reveal All) for unexplored pins
                const bool isUnexploredPin = (pinMode != PinRevealMode::RevealAll) &&
                                             !IsMarkerExplored(client, m, origHash);
                if (pinMode == PinRevealMode::ExploredOnly && !IsMarkerShownForExploredPins(client, m, origHash)) {
                    continue;
                }

                // In Enshrouded (0x140d1394d: cmp dword ptr [rdi+34h], 3), markerState == 3 is Completed/Cleared!
                const bool isCleared = !rumorHere && ((markerState == 3u) || (normHash == kCompletedQuestTypeHash) || visitedSanctum || (handDone && !vanillaData));
                if (hideClearedOnBigMap && isCleared && normHash != kCompletedQuestTypeHash) continue;

                uint32_t displayHash = (normHash == kCompletedQuestTypeHash)
                                           ? GetQuestFilterHashForMarkerId(savedId08, false)
                                           : normHash;
                if (displayHash == 0u || displayHash == 0x8ba7c122u) {
                    if (normHash == kCompletedQuestTypeHash) displayHash = 0xc52e92e5u;
                }
                // Locked like on the World Map: the game rewrites m+0x40 and m+0x48 on every pass, so a slot the
                // World Map hasn't saved yet (just loaded, or the map not opened) is locked by the same test
                if (m[0x48] != 0 && (normHash == 0xa17072beu || normHash == 0x1095684eu)) {
                    uint64_t lastChg = *reinterpret_cast<uint64_t*>(m + 0x40);
                    bool locked = (lastChg == 0);
                    if (locked) {
                        displayHash = (normHash == 0xa17072beu) ? 0x98b2d65du : kLockedFastTravelTypeHash;
                    }
                }

                uint8_t* markerType = GetMarkerTypeHook(displayHash, mgr);
                if (!markerType && g_getMarkerType && mgr) {
                    markerType = g_getMarkerType(displayHash, mgr);
                }
                if (!markerType) continue;

                const bool useMuted = isCleared || (normHash == 0x8ba7c122u);
                const void* iconRef = markerType + (useMuted ? 0x2c : 0x1c);
                const auto* iconBytes = static_cast<const uint64_t*>(iconRef);
                if (iconBytes[0] == 0 && iconBytes[1] == 0) {
                    iconRef = markerType + 0x1c;
                }

                uint8_t sortCat = markerType[0x14];
                float iconSize = 18.0f * btnScale;
                uint32_t tint = 0xFFFFFFFFu;
                float alpha = 0.96f;

                if (displayHash == 0x98b2d65du || displayHash == kLockedFastTravelTypeHash) {
                    // Locked Spire or Locked Fast Travel Point: dimmed gray
                    tint = 0xFF9E9E9Eu;
                    alpha = 0.85f;
                    iconSize = 20.0f * btnScale;
                } else if (displayHash == 0xa17072beu) {
                    iconSize = 22.0f * btnScale;
                } else if (displayHash == 0xa447cba3u || displayHash == 0x6a56c5b3u || displayHash == 0x1095684eu) {
                    iconSize = 20.0f * btnScale;
                } else if (isCleared) {
                    alpha = 0.88f;
                }

                if (sortCat == 4) {
                    iconSize *= customScale;
                }
                if ((isUnexploredPin && pinMode == PinRevealMode::Hybrid) || IsMarkerUndiscovered(m, origHash)) {
                    tint = 0xFF8C9296u;
                    alpha = 0.52f;
                }

                bool isPulsing = false;
                if (pulseActive) {
                    for (const auto& pp : g_pulsingPlaces) {
                        float pdx = pp.wx - mx;
                        float pdz = pp.wz - mz;
                        if (pdx * pdx + pdz * pdz <= 9.0f) {
                            isPulsing = true;
                            break;
                        }
                    }
                }

                // A player grave above or below you gets the chests' height caret
                const int8_t heightDir = a == 3 ? GraveHeightDir(m, haveGraveY, graveY) : 0;
                drawList.push_back(
                    MinimapMarkerDraw{sx, sy, iconSize, iconRef, tint, alpha, sortCat, isPulsing, isCleared, heightDir});
            }
        }

        UMM_LAP(PerfMmMarkers);
        // 2b. Gold, Silver & Wooden Chests directly from kChestSpawns (100% flicker-free!)
        if (!vanillaData && AnyChestRowEnabled()) {
            const void* goldIconRef = g_goldChestEnabled ? GetIconRefForType(client, kGoldChestTypeHash) : nullptr;
            const void* silverIconRef = g_silverChestEnabled ? GetIconRefForType(client, kSilverChestTypeHash) : nullptr;
            const void* woodIconRef = g_woodChestEnabled ? GetIconRefForType(client, kWoodChestTypeHash) : nullptr;
            // Nearby chests get an arrow saying whether they're above or below you (cellars, towers, cliffs).
            constexpr float kChestHeightNearMeters = 75.0f, kChestHeightMinMeters = 3.5f; // (HeightCaret)
            float playerY = 0.0f;
            bool havePlayerY = false;
            if (uint8_t* clientWorld = GetClientWorld(client)) {
                const int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c20);
                playerY = static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10);
                havePlayerY = rawY != 0 && std::isfinite(playerY);
            }

            for (size_t c = 0; c < kTotalChestCount; ++c) {
                const bool chestCleared = IsChestCurrentlyCleared(c);
                if (chestCleared && g_lootedChestMode == 0) continue;
                const bool isGold = IsGoldChest(c), isWood = IsWoodChest(c);
                if (!ChestRowEnabled(c)) continue;

                const void* iconRef = isGold ? goldIconRef : isWood ? woodIconRef : silverIconRef;
                if (!iconRef) continue;

                const ChestSpawn& sp = kChestSpawns[c];
                float mx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
                float mz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;

                float dxMeters = mx - playerX;
                float dzMeters = mz - playerZ;
                MmRotate(dxMeters, dzMeters); // (the minimap's view: turned with Facing Up)
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) {
                    continue;
                }
                if (halls::MinimapHidesChest(c, playerX, playerY, playerZ)) continue; // another Hollow Hall's

                const bool chestUnexplored = (g_pinRevealMode != PinRevealMode::RevealAll) &&
                                             !IsPositionExploredByCharacter(client, sp.px1, sp.pz1);
                if (chestUnexplored && g_pinRevealMode == PinRevealMode::ExploredOnly) {
                    continue;
                }

                float sx = vcx + dxMeters * pxPerMeterX;
                float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                    sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                    continue;
                }

                float iconSize = (isGold ? 16.5f : isWood ? 14.0f : 15.5f) * btnScale;
                uint32_t tint = isGold ? 0xFF38C8FFu : isWood ? kWoodChestTint : 0xFFE8E4DCu;
                float alpha = 0.96f;
                if (chestCleared || (chestUnexplored && g_pinRevealMode == PinRevealMode::Hybrid)) {
                    tint = isGold ? 0xFF6894ACu : isWood ? kWoodChestDimTint : 0xFF909498u;
                    alpha = chestCleared ? 0.50f : 0.55f;
                }

                bool isPulsing = false;
                if (pulseActive) {
                    for (const auto& pp : g_pulsingPlaces) {
                        float pdx = pp.wx - mx;
                        float pdz = pp.wz - mz;
                        if (pdx * pdx + pdz * pdz <= 9.0f) {
                            isPulsing = true;
                            break;
                        }
                    }
                }

                int8_t heightDir = 0;
                if (havePlayerY && dxMeters * dxMeters + dzMeters * dzMeters <= kChestHeightNearMeters * kChestHeightNearMeters) {
                    const float dy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f - playerY;
                    heightDir = HeightCaret(static_cast<uint32_t>(c), dy, kChestHeightMinMeters);
                }

                drawList.push_back(MinimapMarkerDraw{sx, sy, iconSize, iconRef, tint, alpha, 3, isPulsing,
                                                     chestCleared || g_chestLootStates[c].everLooted, heightDir});
            }
        }

        UMM_LAP(PerfMmChests);
        // 2c. Ore deposits (Mining), under everything else
        if (!vanillaData && AnyOreEnabled()) {
            // Lore notes, bosses and mining / materials deposits nearby get the chests' height caret (many notes are in
            // cellars, crypts, towers; ores in caves and on cliffs)
            float knowPlayerY = 0.0f;
            bool knowHavePlayerY = false;
            if (uint8_t* clientWorld = GetClientWorld(client)) {
                const int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c20);
                knowPlayerY = static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10);
                knowHavePlayerY = rawY != 0 && std::isfinite(knowPlayerY);
            }
            for (size_t d = 0; d < kOreDepositCount; ++d) {
                const OreDeposit& dep = kOreDeposits[d];
                if (!g_oreEnabled[dep.ore] || CreatureSpawnHidden(dep.ore)) continue;
                float dxMeters = dep.x - playerX;
                float dzMeters = dep.z - playerZ;
                MmRotate(dxMeters, dzMeters);
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) continue;
                // in a vault or spire: only your floor's (as its chests)
                if (knowHavePlayerY && halls::MinimapOtherFloor(dep.x, dep.topY, dep.z, playerX, knowPlayerY, playerZ)) continue;
                const bool unexplored = (g_pinRevealMode != PinRevealMode::RevealAll) &&
                                        !IsPositionExploredByCharacter(client, dep.x, dep.z);
                if (unexplored && g_pinRevealMode == PinRevealMode::ExploredOnly) continue;
                const float sx = vcx + dxMeters * pxPerMeterX + StackSide(d) * 13.0f * btnScale * 0.6f;
                const float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                    sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                    continue;
                }
                // read / defeated: the check mark; hidden or greyed like looted chests (not a boss that came back)
                const bool checked = KnowledgeDone(d), done = checked && KnowledgeGrayed(d);
                if (done && g_lootedChestMode == 0) continue;
                int8_t heightDir = 0;
                const bool heightCaret = IsKnowledgeDeposit(d) || kOreCategory[dep.ore] == kMiningCategory ||
                                         kOreCategory[dep.ore] == kMaterialsCategory;
                if (heightCaret && knowHavePlayerY && dxMeters * dxMeters + dzMeters * dzMeters <= 75.0f * 75.0f) {
                    const float dy = static_cast<float>(dep.topY) - knowPlayerY;
                    heightDir = HeightCaret(0x10000u + static_cast<uint32_t>(d), dy, 3.5f);
                }
                // Return Beacons and the other world rows layer with their pins; deposits stay under everything
                drawList.push_back(MinimapMarkerDraw{sx, sy, 13.0f * btnScale, DepositIcon(d),
                    (unexplored || done) ? 0xFF8C9296u : 0xFFFFFFFFu, (unexplored || done) ? 0.55f : 0.95f,
                    IsResourceCategory(kOreCategory[dep.ore]) ? kMiningCategory : kOreCategory[dep.ore], false, checked,
                    heightDir});
            }
        }

        // 2c2. Live creatures (Settings > Nearby Creatures Only): the creatures within the height-arrow range, where they
        // are now, each under its first row that is on, with the chests' height caret; a red ring while it fights, an amber
        // one while it is alerted (looking for you)
        if (!vanillaData && g_nearCreatures) {
            float liveY = 0.0f;
            const bool haveLiveY = LocalPlayerY(client, liveY);
            for (const creature_live::LiveCreature& c : creature_live::Nearby(playerX, playerZ, 75.0f)) {
                const int row = creature_live::ShownRow(c.model);
                if (row < 0) continue;
                float dxMeters = c.x - playerX, dzMeters = c.z - playerZ;
                MmRotate(dxMeters, dzMeters);
                if (dxMeters * dxMeters + dzMeters * dzMeters > 75.0f * 75.0f) continue;
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) continue;
                if (haveLiveY && halls::MinimapOtherFloor(c.x, c.y, c.z, playerX, liveY, playerZ)) continue;
                const float sx = vcx + dxMeters * pxPerMeterX;
                const float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f || sy < vy + iconPad * 0.5f ||
                    sy > vy + vh - iconPad * 0.5f)
                    continue;
                const int8_t heightDir = haveLiveY ? HeightCaret(0x40000000u ^ c.entity, c.y - liveY, 3.5f) : 0;
                const int aggro = creature_live::Aggro(c);
                drawList.push_back(MinimapMarkerDraw{sx, sy, 13.0f * btnScale, kOreIcons[row], 0xFFFFFFFFu, 0.95f,
                                                     kOreCategory[row], false, false, heightDir,
                                                     aggro == 2 ? 0xff2828ffu : aggro == 1 ? 0xff20b4ffu : 0u});
            }
        }

        // 2d. Built water (built_water.inl), with the world's wells and barrels under Water Source
        if (!vanillaData && g_oreEnabled[kWaterFirst]) {
            for (const auto& w : g_builtWater) {
                float dxMeters = w.x - playerX;
                float dzMeters = w.z - playerZ;
                MmRotate(dxMeters, dzMeters);
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) continue;
                const float sx = vcx + dxMeters * pxPerMeterX;
                const float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                    sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                    continue;
                }
                drawList.push_back(MinimapMarkerDraw{sx, sy, 13.0f * btnScale, kOreIcons[kWaterFirst], 0xFFFFFFFFu, 0.95f,
                                                     kOreCategory[kWaterFirst], false, false});
            }
        }

        UMM_LAP(PerfMmOres);
        // Sort so POIs/Chests (cat 3) render below Vaults (cat 2), Fast Travel/Spires (cat 1), Quests (cat 0), Custom Pins (cat 4)
        auto catPriority = [](uint8_t cat) -> int {
            switch (cat) {
                case kMiningCategory: return -1;
                case 3: return 0;
                case 2: return 1;
                case 1: return 2;
                case 0: return 3;
                case 4: return 4;
                default: return 1;
            }
        };
        std::stable_sort(drawList.begin(), drawList.end(), [&](const MinimapMarkerDraw& a, const MinimapMarkerDraw& b) {
            return catPriority(a.sortCat) < catPriority(b.sortCat);
        });

        float elapsedPulseSec = pulseActive ? (static_cast<float>(nowTick - g_pulseStartTick) * 0.001f) : 0.0f;
        float pulsePhase = std::fmod(elapsedPulseSec, 2.25f) / 2.25f;
        float pulseT = std::min(1.0f, pulsePhase * 2.0f);
        float pulseOneMinusT = 1.0f - pulseT;
        float pulseEase = 1.0f - pulseOneMinusT * pulseOneMinusT;
        float pulseAlpha = pulseOneMinusT * 0.70f;
        uint32_t pulseCol = 0xff6be0ffu;
        if (uint8_t* assets = GetUiAssets()) {
            pulseCol = *reinterpret_cast<uint32_t*>(assets + kMapPulseColorOffset) | 0xff000000u;
        }

        auto drawHeightCaret = [&](float ax, float ay, bool up, float alpha) {
            DrawHeightCaretAbs(anchor.raw, ax, ay, up, alpha, btnScale);
        };

        for (const auto& md : drawList) {
            if (md.isPulsing && pulseAlpha > 0.01f) {
                float ringSz = md.size + pulseEase * 32.0f;
                DrawAssetRectAbs(anchor.raw, kMapPulseRingPrimitive,
                                 md.sx - ringSz * 0.5f, md.sy - ringSz * 0.5f,
                                 ringSz, ringSz, pulseCol, pulseAlpha, 1.0f);
            }
            DrawIconRectAbs(anchor.raw, md.iconRef,
                            md.sx - md.size * 0.5f, md.sy - md.size * 0.5f,
                            md.size, md.size, md.color, md.alpha);
            if (md.ring) {
                const float ringSz = md.size * 1.45f;
                DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, md.sx - ringSz * 0.5f, md.sy - ringSz * 0.5f, ringSz,
                                 ringSz, md.ring, 0.95f, 1.0f);
            }
            if (md.isCleared) {
                // Draw the native completed checkmark (assets + 0xef30) on completed Shroud Roots, Vaults, POIs, or looted chests
                float checkSz = std::max(10.0f, md.size * 0.62f);
                float checkX = md.sx + md.size * 0.45f - checkSz * 0.72f;
                float checkY = md.sy + md.size * 0.45f - checkSz * 0.72f;
                DrawAssetRectAbs(anchor.raw, kCirclePrimitive,
                                 checkX - 1.0f, checkY - 1.0f,
                                 checkSz + 2.0f, checkSz + 2.0f,
                                 0xFF0A0E10u, 0.85f * md.alpha, 1.0f);
                DrawAssetRectAbs(anchor.raw, kCheckmarkPrimitive,
                                 checkX, checkY,
                                 checkSz, checkSz,
                                 0xFFFFFFFFu, 0.98f, 1.12f);
            }
            if (md.heightDir != 0) {
                // At the icon's top-right corner (up) or bottom-right corner (down); a looted chest's check mark
                // (bottom right) pushes it past the icon's edge
                const bool up = md.heightDir > 0;
                drawHeightCaret(md.sx + md.size * 0.5f + (md.isCleared ? 4.5f : 1.0f) * btnScale,
                                md.sy + (up ? -0.32f : 0.32f) * md.size, up, std::max(0.7f, md.alpha));
            }
        }

        // Town Portal's open portals (town_portals.inl), under the players
        if (!vanillaData && g_townPortalsEnabled)
            DrawTownPortalsOnMinimap(anchor.raw, vcx, vcy, playerX, playerZ, radiusMeters, pxPerMeterX, pxPerMeterY, btnScale);

        // Draw other players in multiplayer from client + 0x4642c0 (count at +0x4642c8, stride 0x240); the game's
        // World Map always shows them
        if (g_otherPlayersEnabled || vanillaData) {
            uint8_t* playerArray = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
            uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
            uint32_t localPlayerId = *reinterpret_cast<uint32_t*>(client + 0x18);
            static minimap_players::Roster roster;
            // p[0x207] is set for players in a hidden place, such as the Hollow Halls (the game moves them to their own
            // part of the world, top left). Show the players on the same side of it as you: everyone outside, or the
            // ones in there with you.
            uint8_t localHidden = 0;
            if (playerArray && playerCount <= 64) {
                for (uint64_t i = 0; i < playerCount; ++i) {
                    auto* p = playerArray + i * 0x240;
                    if (*reinterpret_cast<uint32_t*>(p) == localPlayerId) localHidden = p[0x207];
                }
            }
            uint32_t activeIds[64]{};
            size_t activeCount = 0;
            if (playerArray && playerCount <= 64) {
                for (uint64_t i = 0; i < playerCount; ++i) {
                    auto* p = playerArray + i * 0x240;
                    const uint32_t id = *reinterpret_cast<uint32_t*>(p);
                    if (id && id != localPlayerId && p[0x207] == localHidden) activeIds[activeCount++] = id;
                }
            }
            roster.Retain(activeIds, activeCount);
            // A player 4 m or more above or below you gets the chests' height caret, like a chest
            float localY = 0.0f;
            bool haveLocalY = false;
            if (uint8_t* clientWorld = GetClientWorld(client)) {
                const int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c20);
                localY = static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10);
                haveLocalY = rawY != 0 && std::isfinite(localY);
            }

            if (playerArray && playerCount > 0 && playerCount <= 64) {
                const float clampLimit = radiusMeters * 0.91f;
                for (uint64_t i = 0; i < playerCount; ++i) {
                    uint8_t* p = playerArray + i * 0x240;
                    uint32_t pid = *reinterpret_cast<uint32_t*>(p + 0x00);
                    if (pid == 0 || pid == localPlayerId) continue;
                    if (p[0x207] != localHidden) continue;

                    // Live float world coordinates at p + 0x10 {x, y, z} (0x140bd282a, 0x140d557fd)
                    float ox = *reinterpret_cast<float*>(p + 0x10);
                    float oy = *reinterpret_cast<float*>(p + 0x14);
                    float oz = *reinterpret_cast<float*>(p + 0x18);
                    if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f) && p[0x68] != 0) {
                        int32_t rx = *reinterpret_cast<int32_t*>(p + 0x54);
                        int32_t ry = *reinterpret_cast<int32_t*>(p + 0x5c);
                        int32_t rz = *reinterpret_cast<int32_t*>(p + 0x64);
                        ox = static_cast<float>(rx) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x50)) / 65536.0f;
                        oy = static_cast<float>(ry) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x58)) / 65536.0f;
                        oz = static_cast<float>(rz) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x60)) / 65536.0f;
                    }
                    if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f)) continue;

                    float dxM = ox - playerX;
                    float dzM = oz - playerZ;
                    MmRotate(dxM, dzM);
                    bool isClamped = false;
                    float maxAxis = std::max(std::abs(dxM), std::abs(dzM));
                    if (maxAxis > clampLimit && maxAxis > 0.001f) {
                        float scale = clampLimit / maxAxis;
                        dxM *= scale;
                        dzM *= scale;
                        isClamped = true;
                    }

                    float osx = vcx + dxM * pxPerMeterX;
                    float osy = vcy - dzM * pxPerMeterY;
                    float pSz = (isClamped ? 16.5f : 20.5f) * btnScale;
                    float pAlpha = isClamped ? 0.86f : 1.0f;

                    auto& marker = roster.Update(pid, ox, oz);
                    float otherDirX = marker.dirX, otherDirZ = marker.dirZ;
                    MmRotate(otherDirX, otherDirZ);
                    DrawPlayerTriangle(anchor.raw, osx, osy, otherDirX, otherDirZ,
                                       btnScale * (isClamped ? 0.85f : 1.0f),
                                       marker.color, marker.color, pAlpha);
                    if (haveLocalY && std::isfinite(oy) && std::abs(oy - localY) >= 4.0f && std::abs(oy - localY) < 2000.0f) {
                        const bool up = oy > localY;
                        drawHeightCaret(osx + pSz * 0.5f + 1.0f * btnScale, osy + (up ? -0.32f : 0.32f) * pSz, up, pAlpha);
                    }

                    // Draw player character/display name above their marker
                    const char* namePtr = *reinterpret_cast<const char**>(p + 0xc0);
                    size_t nameLen = *reinterpret_cast<size_t*>(p + 0xc8);
                    if (!namePtr || nameLen == 0 || nameLen > 64) {
                        namePtr = *reinterpret_cast<const char**>(p + 0xd8);
                        nameLen = *reinterpret_cast<size_t*>(p + 0xe0);
                    }
                    if (namePtr && nameLen > 0 && nameLen <= 48) {
                        char nameBuf[56]{};
                        std::memcpy(nameBuf, namePtr, nameLen);
                        nameBuf[nameLen] = '\0';
                        float lblW = 110.0f * btnScale;
                        float lblH = 14.0f * btnScale;
                        float lblY = osy - pSz * 0.5f - lblH - 1.0f * btnScale;
                        if (lblY < vy + 2.0f * btnScale) {
                            lblY = osy + pSz * 0.5f + 1.0f * btnScale;
                        }
                        // Subtle shadow + crisp bright label
                        DrawLabelAbs(osx - lblW * 0.5f + 1.0f, lblY + 1.0f, lblW, lblH,
                                     nameBuf, 0xFF080A0Cu, 0.90f * pAlpha, TextAlignH::Center, TextAlignV::Center);
                        DrawLabelAbs(osx - lblW * 0.5f, lblY, lblW, lblH,
                                     nameBuf, 0xFFF4ECDCu, pAlpha, TextAlignH::Center, TextAlignV::Center);
                    }
                }
            }
        }

        // Also draw active multiplayer map pings if present (*(g + 0xd0) + 0x15130, count at +0x15b30)
        if (auto* g = static_cast<uint8_t*>(GetUiG())) {
            if (uint8_t* d0 = *reinterpret_cast<uint8_t**>(g + 0xd0)) {
                uint8_t pingCount = d0[0x15b30];
                if (pingCount > 0 && pingCount <= 16) {
                    const void* pingIcon = GetIconRefForType(client, 0x83405288u);
                    if (!pingIcon) pingIcon = GetIconRefForType(client, 0x26d18a06u);
                    for (uint8_t p = 0; p < pingCount; ++p) {
                        uint8_t* pm = d0 + 0x15130 + static_cast<size_t>(p) * 0xA0;
                        float ox = static_cast<float>(*reinterpret_cast<int32_t*>(pm + 0x14)) +
                                   static_cast<float>(*reinterpret_cast<uint16_t*>(pm + 0x10)) / 65536.0f;
                        float oz = static_cast<float>(*reinterpret_cast<int32_t*>(pm + 0x24)) +
                                   static_cast<float>(*reinterpret_cast<uint16_t*>(pm + 0x20)) / 65536.0f;
                        float dxM = ox - playerX;
                        float dzM = oz - playerZ;
                        MmRotate(dxM, dzM);
                        if (std::abs(dxM) <= radiusMeters && std::abs(dzM) <= radiusMeters && pingIcon) {
                            float osx = vcx + dxM * pxPerMeterX;
                            float osy = vcy - dzM * pxPerMeterY;
                            float pSz = 20.0f * btnScale;
                            DrawIconRectAbs(anchor.raw, pingIcon, osx - pSz * 0.5f, osy - pSz * 0.5f, pSz, pSz, 0xFFFFFFFFu, 1.0f);
                        }
                    }
                }
            }
        }

        // Draw local Player Marker at exact center (vcx, vcy) as a clean, compact directional triangle (facing read above;
        // with the map turning it points up, give or take the easing)
        MmRotate(faceDirX, faceDirZ);

        // Your own colour, as every other player sees you (see minimap_players::ColorFor).
        const uint32_t ownColor = minimap_players::ColorFor(*reinterpret_cast<uint32_t*>(client + 0x18));
        DrawPlayerTriangle(anchor.raw, vcx, vcy, faceDirX, faceDirZ, btnScale, ownColor, ownColor, 1.0f);

        if (thinBorder) {
            // Thin border: a dark outline and a fine gold line round the map, and a small N at the top. The bar
            // hangs below it, drawn first so the border's bottom line covers its top edge.
            if (infoBar) DrawArtSprite(anchor.raw, kArtTray, trayX - 40.5f * UT, trayY - 262.0f * UT, TS, (trayW - trayBaseW) / TS);
            const float x0 = frameX, y0 = frameY, sz = frameSize;
            DrawSolidRectAbs(anchor.raw, x0, y0, sz, 3.0f, 0xFF0A0E10u, 0.9f);
            DrawSolidRectAbs(anchor.raw, x0, y0 + sz - 3.0f, sz, 3.0f, 0xFF0A0E10u, 0.9f);
            DrawSolidRectAbs(anchor.raw, x0, y0, 3.0f, sz, 0xFF0A0E10u, 0.9f);
            DrawSolidRectAbs(anchor.raw, x0 + sz - 3.0f, y0, 3.0f, sz, 0xFF0A0E10u, 0.9f);
            constexpr uint32_t kThinGold = 0xFF7CC4DCu;
            DrawSolidRectAbs(anchor.raw, x0 + 1.0f, y0 + 1.0f, sz - 2.0f, 1.0f, kThinGold, 0.9f);
            DrawSolidRectAbs(anchor.raw, x0 + 1.0f, y0 + sz - 2.0f, sz - 2.0f, 1.0f, kThinGold, 0.9f);
            DrawSolidRectAbs(anchor.raw, x0 + 1.0f, y0 + 1.0f, 1.0f, sz - 2.0f, kThinGold, 0.9f);
            DrawSolidRectAbs(anchor.raw, x0 + sz - 2.0f, y0 + 1.0f, 1.0f, sz - 2.0f, kThinGold, 0.9f);
            if (!turnMap) { // (turned: the N goes round the edge, below)
                DrawLabelAbs(frameCx - 15.0f + 1.0f, vy + 1.0f + 1.0f, 30.0f, 18.0f, umm_lang::T(umm_lang::Str::CompassN),
                             0xFF080A0Cu, 0.85f, TextAlignH::Center, TextAlignV::Center);
                DrawLabelAbs(frameCx - 15.0f, vy + 1.0f, 30.0f, 18.0f, umm_lang::T(umm_lang::Str::CompassN),
                             0xFF9AD3E9u, 0.98f, TextAlignH::Center, TextAlignV::Center);
            }
        } else {
            // ------------------------------------------------------------------------
            // 3. Frame art: the tray first (it tucks behind the bottom rail), then the frame and the N
            //    medallion set into the compass rose
            // ------------------------------------------------------------------------
            if (infoBar) DrawArtSprite(anchor.raw, kArtTray, trayX - 40.5f * UT, trayY - 262.0f * UT, TS, (trayW - trayBaseW) / TS);
            if (frameSize >= kMinimapArtDesign) {
                const float grow = (frameSize - kMinimapArtDesign) * 0.5f;
                DrawArtSprite(anchor.raw, kArtFrame370, frameX, frameY, 1.0f, grow, grow);
                for (int z = 0; z < kArtFrame370.nZonesX; ++z)
                    DrawArtZoneStrip(anchor.raw, kArtFrame370, *kArtFrameStripsX[z], true, z, frameX, frameY, grow, grow);
                for (int z = 0; z < kArtFrame370.nZonesY; ++z)
                    DrawArtZoneStrip(anchor.raw, kArtFrame370, *kArtFrameStripsY[z], false, z, frameX, frameY, grow, grow);
            } else {
                // The smallest pre-rendered size at or above this one, scaled down a little
                const MinimapArtSprite* bucket = &kArtFrame370;
                float bucketSize = kMinimapArtDesign;
                for (int i = 0; i < kMinimapFrameBucketCount; ++i) {
                    if (kMinimapFrameBuckets[i] >= frameSize) {
                        bucket = kArtFrameBuckets[i];
                        bucketSize = kMinimapFrameBuckets[i];
                        break;
                    }
                }
                DrawArtSprite(anchor.raw, *bucket, frameX, frameY, frameSize / bucketSize);
                // The design-size frame leaves its rails' middles to the strips (between the largest bucket and
                // the design size, 341 - 369 px, the rails had gaps there)
                if (bucket == &kArtFrame370) {
                    const float scale = frameSize / bucketSize;
                    for (int z = 0; z < kArtFrame370.nZonesX; ++z)
                        DrawArtZoneStrip(anchor.raw, kArtFrame370, *kArtFrameStripsX[z], true, z, frameX, frameY, 0.0f,
                                         0.0f, scale);
                    for (int z = 0; z < kArtFrame370.nZonesY; ++z)
                        DrawArtZoneStrip(anchor.raw, kArtFrame370, *kArtFrameStripsY[z], false, z, frameX, frameY, 0.0f,
                                         0.0f, scale);
                }
            }
            DrawArtSprite(anchor.raw, kArtMedallion, frameCx - 150.0f * U, compassCy - 30.5f * U, 1.0f);
            if (!turnMap)
                DrawLabelAbs(frameCx - 15.0f, compassCy - 10.0f, 30.0f, 20.0f, umm_lang::T(umm_lang::Str::CompassN),
                             0xFF9AD3E9u, 0.98f, TextAlignH::Center, TextAlignV::Center);
        }
        if (turnMap) {
            // Facing Up: the N on the map's edge, the way north is
            float nx = 0.0f, nz = 1.0f;
            MmRotate(nx, nz);
            const float dx = nx, dy = -nz, margin = 12.0f * btnScale;
            float t = 1e9f;
            if (std::abs(dx) > 1e-4f) t = std::min(t, (vw * 0.5f - margin) / std::abs(dx));
            if (std::abs(dy) > 1e-4f) t = std::min(t, (vh * 0.5f - margin) / std::abs(dy));
            if (t > 1e8f) t = 0.0f;
            const float ncx = vcx + dx * t, ncy = vcy + dy * t, chip = 19.0f * btnScale;
            DrawAssetRectAbs(anchor.raw, kCirclePrimitive, ncx - chip * 0.5f, ncy - chip * 0.5f, chip, chip, 0xFF0A0E10u, 0.8f, 1.0f);
            DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, ncx - chip * 0.5f, ncy - chip * 0.5f, chip, chip, 0xFF7CC4DCu,
                             0.9f, 1.0f);
            DrawLabelAbs(ncx - 15.0f, ncy - 9.0f, 30.0f, 18.0f, umm_lang::T(umm_lang::Str::CompassN), 0xFF9AD3E9u, 0.98f,
                         TextAlignH::Center, TextAlignV::Center);
        }
    }

    // Highlight resize grip when hovered or actively resizing
    if (!barOnly && (hoverResize || g_minimapDraggingResize || shiftAltHeld)) {
        const uint32_t gripCol = (hoverResize || g_minimapDraggingResize) ? 0xFFA2EBEFu : 0xFF7EBEC2u;
        DrawSolidRectAbs(anchor.raw, resizeGripX0 + 6.0f, resizeGripY1 - 4.0f, 12.0f, 2.0f, gripCol, 0.92f);
        DrawSolidRectAbs(anchor.raw, resizeGripX1 - 4.0f, resizeGripY0 + 6.0f, 2.0f, 12.0f, gripCol, 0.92f);
    }

    if (!infoBar) {
        // The cog alone, in the map's bottom-left corner
        const float gcx = gearBtnX + gearBtnW * 0.5f, gcy = gearBtnY + gearBtnH * 0.5f;
        DrawAssetRectAbs(anchor.raw, kCirclePrimitive, gearBtnX, gearBtnY, gearBtnW, gearBtnH, 0xFF0A0E10u, 0.78f, 1.0f);
        if (hoverGear || g_minimapSettingsOpen)
            DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, gcx - 16.0f, gcy - 16.0f, 32.0f, 32.0f, 0xFFC6EFFCu,
                             hoverGear ? 0.45f : 0.3f);
        DrawGearIconAbs(anchor.raw, gcx, gcy, 7.0f, 0xFFC6EFFCu, 0.95f, 0xFF0A0E10u);
    } else {
        // ------------------------------------------------------------------------
        // 4. Tray contents: weather icon, time, weather word, zoom buttons, hover glows
        // ------------------------------------------------------------------------
        constexpr uint32_t kTrayText = 0xFFC6EFFCu;   // #fcefc6 cream, as on the game's buttons
        constexpr uint32_t kTrayMuted = 0xFF8E8A84u;
        auto trayGlow = [&](float cx, float r, float alpha) {
            DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, cx - r, trayContentCy - r, r * 2.0f, r * 2.0f, kTrayText, alpha);
        };
        if (hoverGear || g_minimapSettingsOpen) trayGlow(trayX + 14.4f * UT, 15.0f * TS, hoverGear ? 0.45f : 0.3f);
        if (hoverMinus && MinimapZoom() > 0) trayGlow(trayRight - 31.5f * UT, 13.0f * TS, 0.4f);
        if (hoverPlus && MinimapZoom() + 1 < MinimapZoomCount()) trayGlow(trayRight - 14.5f * UT, 13.0f * TS, 0.4f);

        // Centre divider, under the frame's bottom diamond
        DrawSolidRectAbs(anchor.raw, frameCx - 0.5f, trayContentCy - 8.0f * TS, 1.0f, 16.0f * TS, 0xFF62A8C4u, 0.75f);

        // Left half: icon + time, centred between the gear cap and the divider
        const float leftX0 = trayX + gearCapW + 6.0f * TS;
        const float leftX1 = frameCx - trayCentreGap;
        const float leftStart = (leftX0 + leftX1 - (trayIconW + timeTextW)) * 0.5f;
        int iconIdx = isDay ? 0 : 1;
        if (g_minimapClockWeatherMode && weatherType >= 1 && weatherType <= 4) iconIdx = 1 + weatherType;
        DrawArtSprite(anchor.raw, *kArtIcons[iconIdx], leftStart + trayIconW * 0.5f - 2.0f * TS - 150.0f * UT,
                      trayContentCy - 150.0f * UT, TS);
        DrawLabelAbs(leftStart + trayIconW, trayContentCy - 12.0f * TS, timeTextW + 6.0f * TS, 24.0f * TS, timeText,
                     kTrayText, 0.97f, TextAlignH::Left, TextAlignV::Center, nullptr, TS);
        // Right half: the weather (or day phase) word
        const float rightX0 = frameCx + trayCentreGap;
        const float rightX1 = trayRight - zoomCapW - 6.0f * TS;
        DrawLabelAbs(rightX0, trayContentCy - 12.0f * TS, rightX1 - rightX0, 24.0f * TS, statusText,
                     kTrayText, 0.97f, TextAlignH::Center, TextAlignV::Center, nullptr, TS);
        DrawLabelAbs(minusBtnX, trayContentCy - 12.0f * TS, zoomBtnW, 24.0f * TS, "-",
                     (MinimapZoom() > 0) ? kTrayText : kTrayMuted, 0.98f, TextAlignH::Center, TextAlignV::Center,
                     nullptr, TS);
        DrawLabelAbs(plusBtnX, trayContentCy - 12.0f * TS, zoomBtnW, 24.0f * TS, "+",
                     (MinimapZoom() + 1 < MinimapZoomCount()) ? kTrayText : kTrayMuted, 0.98f, TextAlignH::Center,
                     TextAlignV::Center, nullptr, TS);
    }

    // Settings > Advanced > Position: X, Y (height), Z on a framed plate under the tray (or the map), in the Map
    // Key's colours: gold axis letters, cream numbers in equal cells, the
    // tray's blue dividers between them
    float posY = 0.0f;
    if (g_minimapShowPosition && LocalPlayerY(client, posY)) {
        using namespace key_style;
        const int axisValue[3] = {static_cast<int>(std::lround(playerX)), static_cast<int>(std::lround(posY)),
                                  static_cast<int>(std::lround(playerZ))};
        static const char* const kAxis[3] = {"X", "Y", "Z"};
        const float letterW = posLetterW, digitsW = posDigitsW, cellW = posCellW;
        const float plateW = posPlateW, plateH = kPositionLineH, plateX = posPlateX, plateY = posPlateY;
        constexpr uint32_t kTraySlate = 0xFF6C5A58u, kTrayEdge = 0xFF2A2020u, kTrayGoldLine = 0xFF4986A0u;
        DrawFramedBoxAbs(anchor.raw, plateX, plateY, plateW, plateH, kTraySlate, 0.97f, kTrayEdge, 0.95f, 6.0f);
        DrawFramedBoxAbs(anchor.raw, plateX + 2.0f, plateY + 2.0f, plateW - 4.0f, plateH - 4.0f, kTraySlate, 1.0f,
                         kTrayGoldLine, 0.75f, 5.0f);
        DrawBoxAbs(anchor.raw, kBoxInnerGlowPrimitive, plateX + 3.0f, plateY + 3.0f, plateW - 6.0f, plateH - 6.0f,
                   0xFF000000u, 0.25f, 6.0f);
        for (int axis = 0; axis < 3; ++axis) {
            const float cx = plateX + 4.0f + cellW * axis;
            if (axis > 0)
                DrawSolidRectAbs(anchor.raw, cx - 0.5f, plateY + 5.0f, 1.0f, plateH - 10.0f, 0xFF62A8C4u, 0.7f);
            char value[16]{};
            std::snprintf(value, sizeof(value), "%d", axisValue[axis]);
            DrawLabelAbs(cx + 9.0f, plateY + 1.0f, letterW, plateH, kAxis[axis], 0xFF7FD3EEu, 0.8f, TextAlignH::Left,
                         TextAlignV::Center, nullptr, 0.72f);
            DrawLabelAbs(cx + 9.0f + letterW, plateY, digitsW + 6.0f, plateH, value, 0xFFC6EFFCu, 0.97f,
                         TextAlignH::Left, TextAlignV::Center, nullptr, 0.8f);
        }
        // Copy button: two overlapping sheets, a check mark for a moment once copied
        DrawSolidRectAbs(anchor.raw, copyBtnX - 0.5f, plateY + 5.0f, 1.0f, plateH - 10.0f, 0xFF62A8C4u, 0.7f);
        const float bcx = copyBtnX + kCopyBtnW * 0.5f + 1.0f, bcy = plateY + plateH * 0.5f;
        if (hoverCopy)
            DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, bcx - 13.0f, bcy - 13.0f, 26.0f, 26.0f, 0xFFC6EFFCu, 0.4f);
        const bool copied = g_positionCopiedTick && GetTickCount64() - g_positionCopiedTick < 1500;
        if (copied) {
            DrawAssetRectAbs(anchor.raw, kCheckmarkPrimitive, bcx - 8.0f, bcy - 8.0f, 16.0f, 16.0f, 0xFF8EE0B8u, 0.98f);
        } else {
            const uint32_t col = hoverCopy ? 0xFFFFFFFFu : 0xFFC6EFFCu;
            auto sheet = [&](float x, float y, float w, float h, bool fill) {
                if (fill) DrawSolidRectAbs(anchor.raw, x, y, w, h, 0xFF6C5A58u, 1.0f);
                DrawSolidRectAbs(anchor.raw, x, y, w, 1.5f, col, 0.95f);
                DrawSolidRectAbs(anchor.raw, x, y + h - 1.5f, w, 1.5f, col, 0.95f);
                DrawSolidRectAbs(anchor.raw, x, y, 1.5f, h, col, 0.95f);
                DrawSolidRectAbs(anchor.raw, x + w - 1.5f, y, 1.5f, h, col, 0.95f);
            };
            sheet(bcx - 6.0f, bcy - 7.0f, 9.0f, 11.0f, false);
            sheet(bcx - 3.0f, bcy - 4.0f, 9.0f, 11.0f, true);
        }
    }

    if (g_minimapSettingsOpen) {
        DrawSettingsCardAbs(anchor.raw, mmSettingsX, mmSettingsY,
                            mouseX, mouseY, cursorVisible, leftPressed, g_minimapSettingsOpen);
    }
}

void SafeRunHudMinimapLayerBody(uint8_t* client, HWND foreground, bool appFocused, uint8_t screen, float playerX, float playerZ, Widget& anchor) {
    __try {
        DrawHudMinimapLayerBody(client, foreground, appFocused, screen, playerX, playerZ, anchor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void DrawHudMinimapOverlay() {
    uint8_t* client = GetUiClient();
    if (!client) {
        SetMinimapMouseControl(false, nullptr);
        return;
    }

    LoadMinimapConfig();

    HWND foreground = GetForegroundWindow();
    DWORD fgPid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
    const bool appFocused = (fgPid == GetCurrentProcessId()) || inp::GameHasFocus();

    // Update chest loot detection and 30-game-minute respawn timer (even while inventory/loot UI is open)
    {
        UMM_PERF(PerfLoot);
        UpdateChestLootTracking(client, appFocused);
    }
    {
        UMM_PERF(PerfGlowDraw);
        chest_glow::DrawMarkers(client); // GG's chests on screen, through walls
    }

    // Screen state check:
    // 1 = Playing (normal HUD), 5 = World Map, 2 = Pause/System Menu, 9 = Inventory/Journal
    // Hide minimap when World Map (5) or System Menu (2) or Main Menu (0) is open, or when hidden in Settings.
    uint8_t screen = client[0x1cd8];
    if (!MapScreen(screen)) {
        g_worldMapWasOpen = false;
        g_screenBeforeMap = screen;
    }
    if (screen == 0 || screen == 2 || MapScreen(screen) || (!g_minimapVisible && !g_minimapInfoBar && !g_minimapSettingsOpen)) {
        g_minimapDraggingMove = false;
        g_minimapDraggingResize = false;
        g_minimapSettingsOpen = false;
        SetMinimapMouseControl(false, nullptr);
        return;
    }

    float playerX = 5120.0f, playerZ = 5120.0f;
    if (!GetHudPlayerWorldPos(client, playerX, playerZ)) {
        SetMinimapMouseControl(false, nullptr);
        return;
    }

    static const UiText kLayerName = MakeUiText("minimapHudLayer");

    // IMPORTANT: UiLayer is ~0x2e0 bytes; allocate 0x400 bytes to match DrawMapKeyOverlay!
    alignas(16) uint8_t layer[0x400]{};
    g_beginLayer(layer, &kLayerName, 1, 2, -1, 0);

    Widget anchor{};
    g_namedWidget(anchor.raw, &kLayerName);
    g_opaque(anchor.raw); // Required so the root widget receives the layer's full-screen rect at +0x114!

    {
        UMM_PERF(PerfMinimap);
        SafeRunHudMinimapLayerBody(client, foreground, appFocused, screen, playerX, playerZ, anchor);
    }

    g_endWidget(anchor.raw);
    g_endLayer(layer);
}

void SafeDrawHudMinimapOverlay() {
    __try {
        DrawHudMinimapOverlay();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Ctrl + Shift + F12 switches the whole mod off and on, to compare with the game as it is. Off: the hooks only
// call on, the byte patches and the fog get the game's own bytes back, the chest pins the mod added leave the
// game's marker arrays, every pin gets back the type and quest link the mod saved for it, and the map's zoom
// steps are the game's again; the map then rebuilds its markers. On: all of it comes back.
void RefreshMapMarkers() {
    if (uint8_t* mapState = g_lastMapState) {
        __try {
            *reinterpret_cast<uint64_t*>(mapState + 0x542f8) = 0;
            *reinterpret_cast<uint8_t*>(mapState + 0xc1b4) = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void RestoreGameMarkerArrays(uint8_t* client) {
    __try {
        constexpr size_t kArrays[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
        for (size_t at : kArrays) {
            uint8_t* base = *reinterpret_cast<uint8_t**>(client + at);
            uint64_t& count = *reinterpret_cast<uint64_t*>(client + at + 8);
            if (!base || count > 10000) continue;
            uint64_t kept = 0;
            for (uint64_t i = 0; i < count; ++i) {
                uint8_t* m = base + i * 0xA0;
                if ((*reinterpret_cast<uint32_t*>(m) & 0xFFFF0000u) == kChestMarkerUidPrefix) break;
                if (IsSlotSaved(m)) {
                    *reinterpret_cast<uint32_t*>(m + 0x30) = *reinterpret_cast<uint32_t*>(m + 0x3c);
                    *reinterpret_cast<uint32_t*>(m + 0x08) = *reinterpret_cast<uint32_t*>(m + 0x4c);
                }
                ++kept;
            }
            count = kept; // the mod's chest pins follow the game's own entries
        }
        g_savedSlotUid.clear();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RestoreGameZoomSteps() {
    if (uint8_t* mapState = g_lastMapState) {
        __try {
            if (uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8)) {
                auto* steps = reinterpret_cast<float*>(mapView + 0x40);
                for (int i = 0; i < 4; ++i) steps[i] = kMapZoomSteps[kOverworldFirstStep + i];
                mapView[0x54] = 4;
                if (uint64_t* idx = *reinterpret_cast<uint64_t**>(mapView + 0x258)) *idx = std::min<uint64_t>(*idx, 3);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

// The World Map with the mod (true) or as the game draws it. The game's code first (SetMarkerUnlockPatches): if its
// threads never leave it alone, nothing changes (false). Without the mod: the game's fog, the chest pins the mod added
// leave the game's marker arrays, every pin gets back its type and quest link, and the zoom steps are the game's.
bool SwitchWorldMap(bool modded) {
    if (modded == g_worldMapModded) return true;
    if (!SetMarkerUnlockPatches(modded)) return false;
    g_worldMapModded = modded;
    ApplyFogRevealMode(g_fogRevealMode);
    if (!modded) {
        if (uint8_t* client = GetUiClient()) RestoreGameMarkerArrays(client);
        RestoreGameZoomSteps();
    }
    RefreshMapMarkers();
    return true;
}

void SetModOff(bool off) {
    if (off == g_modOff) return;
    if (!SwitchWorldMap(!off && !g_vanillaWorldMap)) {
        if (g_log) g_log("Ctrl+Shift+F12: the game's marker code stayed busy; the mod stays %s, press it again",
                         g_modOff ? "OFF" : "ON");
        return;
    }
    g_modOff = off;
    if (off) PrepareMapKeyPanelForReload(); // the minimap's window procedure and mouse, the Find dialog
    if (g_log) g_log("Ctrl+Shift+F12: the mod is %s", off ? "OFF (the game as it is)" : "ON again");
}

// Settings > Advanced > Vanilla World Map. The minimap stays on with it: its cog is then the way back to Settings.
bool SetVanillaWorldMap(bool on) {
    if (on == g_vanillaWorldMap) return true;
    if (!SwitchWorldMap(!g_modOff && !on)) {
        if (g_log) g_log("Vanilla World Map: the game's marker code stayed busy; still %s, click it again", on ? "off" : "on");
        return false;
    }
    g_vanillaWorldMap = on;
    if (on) g_minimapVisible = true;
    SaveMinimapConfig();
    if (g_log) g_log("Vanilla World Map %s", on ? "on: the World Map as the game draws it" : "off: the mod's World Map");
    return true;
}

// At load with Vanilla World Map on, the patches can still be on (a hot reload from a generation without it): they
// come off as soon as the game's code allows, tried once a second.
void SyncWorldMapMode() {
    if (!g_minimapConfigLoaded) return; // Vanilla World Map isn't known yet
    const bool want = !g_modOff && !g_vanillaWorldMap;
    if (want == g_worldMapModded) return;
    static uint64_t s_nextTry = 0;
    const uint64_t now = GetTickCount64();
    if (now < s_nextTry) return;
    s_nextTry = now + 1000;
    if (SwitchWorldMap(want) && g_log) g_log("Vanilla World Map: the World Map is %s", want ? "the mod's" : "the game's");
}

void PollModOffKey() {
    static bool wasDown = false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool down = (pid == GetCurrentProcessId() || inp::GameHasFocus()) && BindingDown(g_keys.modOff);
    if (down && !wasDown) SetModOff(!g_modOff);
    wasDown = down;
}

#ifdef UMM_DEV_BRIDGE
void LogPerf(); // below
#endif

void PlayStateUiHook() {
    if (g_origPlayStateUi) {
        g_origPlayStateUi();
    }
    greeneye::Lease lease;
    if (lease) {
        PollModOffKey();
        SyncWorldMapMode();
        if (g_padVanillaOffPending && (!g_vanillaWorldMap || SetVanillaWorldMap(false))) {
            g_padVanillaOffPending = false;
            g_padEnterPending = true;
        }
        if (!g_modOff) {
            UMM_PERF(PerfHud);
            SafeDrawHudMinimapOverlay();
        }
#ifdef UMM_DEV_BRIDGE
        LogPerf();
#endif
    }
}

#ifdef UMM_DEV_BRIDGE
// Every 5 s: each part's time per frame (frames: this hook's calls) and per call
void LogPerf() {
    static uint64_t frames = 0, since = 0;
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    ++frames;
    const uint64_t now = GetTickCount64();
    if (!since) since = now;
    if (now - since < 5000) return;
    char line[1024];
    int len = std::snprintf(line, sizeof(line), "Perf (%llu frames, %.1f fps):", static_cast<unsigned long long>(frames),
                            frames * 1000.0 / static_cast<double>(now - since));
    for (int i = 0; i < PerfCount && len > 0 && len < static_cast<int>(sizeof(line)); ++i) {
        const uint64_t t = g_perfTicks[i].exchange(0), c = g_perfCalls[i].exchange(0), mx = g_perfMax[i].exchange(0);
        if (!c) continue;
        const double ms = t * 1000.0 / static_cast<double>(freq.QuadPart);
        len += std::snprintf(line + len, sizeof(line) - len, " | %s %.3f ms/frame, worst %.2f ms", kPerfNames[i],
                             ms / static_cast<double>(frames), mx * 1000.0 / static_cast<double>(freq.QuadPart));
    }
    if (g_log) g_log("%s", line);
    frames = 0;
    since = now;
}
#endif

void FilterWorldMapPlayerMarkers(void* outVec) {
    if (!outVec || g_otherPlayersEnabled) return;
    __try {
        auto* vecBytes = static_cast<uint8_t*>(outVec);
        uint8_t* data = *reinterpret_cast<uint8_t**>(vecBytes + 0x00);
        uint64_t count = *reinterpret_cast<uint64_t*>(vecBytes + 0x08);
        if (!data || count == 0 || count > 128) return;

        // Each 64-byte (0x40) entry has entry[0] == 1 for the local player and entry[0] == 0 for other players
        uint64_t writeIdx = 0;
        for (uint64_t i = 0; i < count; ++i) {
            uint8_t* src = data + i * 0x40;
            if (src[0] != 0) {
                if (writeIdx != i) {
                    std::memcpy(data + writeIdx * 0x40, src, 0x40);
                }
                ++writeIdx;
            }
        }
        *reinterpret_cast<uint64_t*>(vecBytes + 0x08) = writeIdx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SafePreSyncWorldMapMarkersInner() {
    __try {
        SyncSpiresAndCountMarkers(GetUiClient(), nullptr, nullptr, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void SafePreSyncWorldMapMarkers() {
    UMM_PERF(PerfPreSync);
    SafePreSyncWorldMapMarkersInner();
}


// A quest / rumor pin under the cursor that a quest names (the Sunken Ancient City's "!": Follow The Melody's place,
// with an unread rumor about it): the game showed the rumor's card for it, looked up by its id. While the cursor is on
// such a pin its id (+0x08) is 0 as the game builds its list (no card), back in the overlay pass, and the mod draws
// the quest's name on its tooltip panel (g_questTitle).
uint8_t* g_blankedPin = nullptr;
uint32_t g_blankedId = 0;
void HideRumorsInner(bool hide) {
    if (!hide) {
        if (g_blankedPin && *reinterpret_cast<uint32_t*>(g_blankedPin + 0x08) == 0u) *reinterpret_cast<uint32_t*>(g_blankedPin + 0x08) = g_blankedId;
        g_blankedPin = nullptr;
        return;
    }
    uint8_t* pin = const_cast<uint8_t*>(g_hoveredGameMarker);
    if (g_blankedPin || !pin || GetTickCount64() - g_hoveredGameMarkerTick > 150ULL ||
        *reinterpret_cast<const uint32_t*>(pin + 0x30) != 0xafce0c5fu || !g_getQuestByMarkerId)
        return;
    const uint32_t id = *reinterpret_cast<const uint32_t*>(pin + 0x08);
    if (!id) return;
    const uint8_t* quest = static_cast<const uint8_t*>(g_getQuestByMarkerId(id));
    if (!quest || !ReadLocaText(*reinterpret_cast<const uint32_t*>(quest + 4), g_questTitle, sizeof(g_questTitle)) || !g_questTitle[0])
        return;
    g_questTitleTick = GetTickCount64();
    g_questTitleFor = pin;
    g_blankedPin = pin, g_blankedId = id;
    *reinterpret_cast<uint32_t*>(pin + 0x08) = 0u;
}
void HideRumorsOverHoveredPin(bool hide) {
    __try {
        HideRumorsInner(hide);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void BuildWorldMapPlayersHook(void* outVec, void* mapState) {
    if (g_origBuildWorldMapPlayers) {
        g_origBuildWorldMapPlayers(outVec, mapState);
    }
    greeneye::Lease lease;
    if (lease && g_worldMapModded) {
        FilterWorldMapPlayerMarkers(outVec);
        // BuildWorldMapPlayers is called by 0x140d8f270 immediately before iterating arr0..arr3
        // to populate mapState+0x542f0. Syncing arr0 here ensures newly replicated ECS markers
        // are always filtered (hiding duplicate 0x98b2d65d spires and locked 0xa17072be spires)
        // before the World Map reads arr0.
        SafePreSyncWorldMapMarkers();
    }
}

struct WorldMapMarkerOverrideBackup {
    bool active = false;
    uint8_t saved80 = 0;
    float savedSize = 0.0f;
    uint8_t* savedType = nullptr;
    CustomMapMarkerType localType{};
    uint8_t savedIconRef[16]{};
    void* opacityUiCtx = nullptr;
};

bool ShouldDimWorldMapMarker(uint8_t* client, uint8_t* m, bool* outIsCustomPlayerPin = nullptr, bool* outIsChest = nullptr) {
    if (outIsCustomPlayerPin) *outIsCustomPlayerPin = false;
    if (outIsChest) *outIsChest = false;
    if (!m) return false;

    const uint32_t uid = *reinterpret_cast<uint32_t*>(m + 0x00);
    const uint32_t typeHash = *reinterpret_cast<uint32_t*>(m + 0x30);
    const uint32_t origHash = GetMarkerOrigTypeHash(m);
    const uint32_t markerState = static_cast<uint32_t>(m[0x34]);
    const bool isChest = ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix);
    if (outIsChest) *outIsChest = isChest;

    bool isCustomPlayerPin = false;
    for (uint32_t ph : kCustomPinHashes) {
        if (typeHash == ph || origHash == ph) {
            isCustomPlayerPin = true;
            break;
        }
    }
    if (outIsCustomPlayerPin) *outIsCustomPlayerPin = isCustomPlayerPin;

    if (!isCustomPlayerPin && MarkerOutsideFocusedRegion(m)) return true;
    if (isChest) {
        const bool lootedDim = (markerState == 3u && g_lootedChestMode == 1);
        const bool hybridDim = (g_pinRevealMode == PinRevealMode::Hybrid && client &&
                                !IsPositionExploredByCharacter(client,
                                                               *reinterpret_cast<int32_t*>(m + 0x14),
                                                               *reinterpret_cast<int32_t*>(m + 0x24)));
        return lootedDim || hybridDim;
    }
    if (isCustomPlayerPin) return false;
    if (IsMarkerUndiscovered(m, origHash)) return true;
    if (g_pinRevealMode == PinRevealMode::Hybrid && client) return !IsMarkerExplored(client, m, origHash);
    return false;
}

void ApplyWorldMapMarkerIconOverrides(uint8_t* m, uint8_t* drawParams, WorldMapMarkerOverrideBackup& bak) {
    if (!m || !drawParams) return;
    __try {
        bool custom = false, chest = false;
        const bool dim = ShouldDimWorldMapMarker(GetUiClient(), m, &custom, &chest);
        if (!custom && !dim) return;
        bak.saved80 = m[0x80];
        bak.savedSize = *reinterpret_cast<float*>(drawParams + 0x40);
        bak.savedType = *reinterpret_cast<uint8_t**>(drawParams);
        std::memcpy(bak.savedIconRef, drawParams + 0x30, 16);
        bak.active = true;
        if (custom) *reinterpret_cast<float*>(drawParams + 0x40) *= kMarkerScaleLevels[g_customMarkerScaleIdx];
        if (!dim) return;

        // Asset data is mapped read-only. Use a per-draw type copy, never edit the shared palette or type.
        if (bak.savedType) {
            std::memcpy(&bak.localType, bak.savedType, sizeof(bak.localType));
            bak.localType.minimizedColor = 0xFF969696u;
            *reinterpret_cast<CustomMapMarkerType**>(drawParams) = &bak.localType;
            if (!bak.localType.isCustomMarkerType) {
                uint64_t words[2]{};
                std::memcpy(words, bak.localType.mutedIcon, sizeof(words));
                if (words[0] || words[1]) std::memcpy(drawParams + 0x30, bak.localType.mutedIcon, 16);
            }
        }
        // Palette zero is the native neutral white-on-dark style (also used for silver chests).
        if (chest) m[0x80] = 0;
        auto* uiGlobal = g_uiCtx ? static_cast<uint8_t*>(g_uiCtx()) : nullptr;
        void* uiCtx = uiGlobal ? *reinterpret_cast<void**>(uiGlobal + 0x18) : nullptr;
        if (uiCtx && g_pushUiOpacity && g_popUiOpacity) {
            const float opacity = chest && static_cast<uint32_t>(m[0x34]) == 3u ? 0.36f : 0.48f;
            g_pushUiOpacity(uiCtx, opacity);
            bak.opacityUiCtx = uiCtx;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The caller still restores any partial override, including the opacity stack.
    }
}

void TimedApplyWorldMapMarkerIconOverrides(uint8_t* m, uint8_t* drawParams, WorldMapMarkerOverrideBackup& bak) {
    UMM_PERF(PerfIconOverride);
    ApplyWorldMapMarkerIconOverrides(m, drawParams, bak);
}

void DrawWorldMapMarkerIconWithOverrides(void* mapState, uint8_t* m, uint8_t* drawParams) {
    WorldMapMarkerOverrideBackup bak{};
    TimedApplyWorldMapMarkerIconOverrides(m, drawParams, bak);
    __try {
        g_origDrawWorldMapMarkerIcon(mapState, m, drawParams);
    } __finally {
        if (bak.opacityUiCtx) g_popUiOpacity(bak.opacityUiCtx);
        if (bak.active) {
            m[0x80] = bak.saved80;
            *reinterpret_cast<float*>(drawParams + 0x40) = bak.savedSize;
            *reinterpret_cast<uint8_t**>(drawParams) = bak.savedType;
            std::memcpy(drawParams + 0x30, bak.savedIconRef, 16);
        }
    }
}

// A game marker whose spot is under a player's locator (the game draws its markers over the locators)
bool UnderLocator(uint8_t* mapState, const uint8_t* m, const uint8_t* drawParams) {
    if (!m || GetTickCount64() - g_locatorKeepTick > 250) return false;
    float zoom = 0.0f, ox = 0.0f, oy = 0.0f, worldH = 0.0f;
    __try {
        if (!LevelMapView(mapState, zoom, ox, oy, worldH)) return false;
        const float sx = ox + static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)) * zoom;
        const float sy = oy + (worldH - static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24))) * zoom;
        // the marker's icon (its drawn size, half each way) overlapping the box
        float half = drawParams ? *reinterpret_cast<const float*>(drawParams + 0x40) * 0.5f : 10.0f;
        if (!(half > 2.0f && half < 100.0f)) half = 10.0f;
        g_markerIconSize = g_markerIconSize * 0.95f + half * 2.0f * 0.05f;
        for (const DrawExclusion& e : g_locatorKeep)
            if (e.active && sx + half > e.x0 && sx - half < e.x1 && sy + half > e.y0 && sy - half < e.y1) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The marker under the cursor, found while the game draws its icons: the one whose diamond holds the cursor, the
// biggest if several (the game draws the hovered one enlarged). The game asks for every marker's title each frame (some
// 80000 a second), so the title hook can't tell which one is hovered. Published once a frame (PublishCursorIcon).
struct CursorIcon {
    const uint8_t* m = nullptr;
    float x = 0, y = 0, half = 0;
};
CursorIcon g_cursorBest;
// Every game icon drawn this frame: the game draws the one it hovers enlarged, so it is found by its size against the
// others of its type (the game's own choice, for every kind of pin); the cursor test is the fallback
struct FrameIcon {
    const uint8_t* m;
    float x, y, half;
    uint32_t type;
};
std::vector<FrameIcon> g_frameIcons, g_questIcons;
bool g_gameQuestHover = false; // the game hovers a quest / rumor pin (drawn enlarged): its card shows, the mod's panel waits
int g_iconDrawsThisFrame = 0; // dev: icons through the hook a frame
float g_nearestIconD = 1e9f;   // dev: the nearest icon's distance from the cursor (UI px) and its type
uint32_t g_nearestIconType = 0;
void NoteCursorIcon(uint8_t* mapState, const uint8_t* m, const uint8_t* drawParams) {
    ++g_iconDrawsThisFrame;
    if (!g_mapCursorIn || !drawParams) return;
    float zoom = 0.0f, ox = 0.0f, oy = 0.0f, worldH = 0.0f;
    __try {
        if (!LevelMapView(mapState, zoom, ox, oy, worldH)) return;
        float half = *reinterpret_cast<const float*>(drawParams + 0x40) * 0.5f;
        if (!(half > 2.0f && half < 150.0f)) return;
        const float sx = ox + static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)) * zoom;
        const float sy = oy + (worldH - static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24))) * zoom;
        {
            const float d = std::fabs(g_mapCursor[0] - sx) + std::fabs(g_mapCursor[1] - sy);
            if (d < g_nearestIconD) g_nearestIconD = d, g_nearestIconType = *reinterpret_cast<const uint32_t*>(m + 0x30);
        }
        // (the game hovers a marker anywhere on its drawn square, at least ~22 UI px round small ones: the same here, or
        // near an icon's edge the game showed its plain title and the mod's panel didn't take over)
        // (a quest / rumor pin is skipped: the game keeps its own label for them, and one near a chest took the chest's
        // hover from the mod: the Gold Chest under a "!" showed its plain title)
        if (*reinterpret_cast<const uint32_t*>(m + 0x30) == 0xafce0c5fu) {
            if (g_questIcons.size() < 2000) g_questIcons.push_back({m, sx, sy, half, 0xafce0c5fu});
            return;
        }
        if (g_frameIcons.size() < 8000) g_frameIcons.push_back({m, sx, sy, half, *reinterpret_cast<const uint32_t*>(m + 0x30)});
        const float reachPx = std::max(half * 1.25f, 28.0f); // (a hovered icon is drawn enlarged; the game's reach is wider)
        if (std::fabs(g_mapCursor[0] - sx) > reachPx || std::fabs(g_mapCursor[1] - sy) > reachPx) return;
        if (g_cursorBest.m && half <= g_cursorBest.half) return;
        g_cursorBest = {m, sx, sy, half};
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// Once a World Map frame: last frame's marker under the cursor becomes the hovered one
void HideRumorsOverHoveredPin(bool hide);
// Quest and rumor pins ("!" / "?") aren't drawn through the icon hook: the nearest one's spot within ~30 UI px of the
// cursor, when no hooked icon is under it
float g_nearestPinD = 1e9f, g_nearestPinAt[2]{}; // dev: the nearest quest / rumor pin to the cursor
uint32_t g_nearestPinOrig = 0;
int g_questPinsSeen = 0;
void PickQuestPinUnderCursor() {
    uint8_t* client = GetUiClient();
    uint8_t* mapState = g_lastMapState;
    float zoom = 0.0f, ox = 0.0f, oy = 0.0f, worldH = 0.0f;
    if (!client || !mapState || !g_mapCursorIn || !LevelMapView(mapState, zoom, ox, oy, worldH)) return;
    constexpr float kHalf = 60.0f; // (a hovered "!" is drawn about twice its size: its whole diamond)
    float best = kHalf * kHalf;
    constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* m = base + i * 0xA0;
            if (*reinterpret_cast<const uint32_t*>(m + 0x30) != 0xafce0c5fu || MarkerHiddenOnMap(m)) continue;
            ++g_questPinsSeen;
            const float sx = ox + static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)) * zoom;
            const float sy = oy + (worldH - static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24))) * zoom;
            const float dx = g_mapCursor[0] - sx, dy = g_mapCursor[1] - sy;
            if (dx * dx + dy * dy < g_nearestPinD * g_nearestPinD)
                g_nearestPinD = std::sqrt(dx * dx + dy * dy), g_nearestPinAt[0] = sx, g_nearestPinAt[1] = sy, g_nearestPinOrig = GetMarkerOrigTypeHash(m);
            if (dx * dx + dy * dy >= best) continue;
            best = dx * dx + dy * dy;
            g_cursorBest = {m, sx, sy, kHalf};
        }
    }
}
void SafePickQuestPinUnderCursor() {
    __try {
        PickQuestPinUnderCursor();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The icon the game draws enlarged (its hover), near the cursor: over 1.12 times the smallest of its type this frame
const FrameIcon* GameEnlargedIcon() {
    std::unordered_map<uint32_t, float> smallest;
    for (const FrameIcon& f : g_frameIcons) {
        auto it = smallest.find(f.type);
        if (it == smallest.end() || f.half < it->second) smallest[f.type] = f.half;
    }
    const FrameIcon* best = nullptr;
    for (const FrameIcon& f : g_frameIcons) {
        if (f.half < smallest[f.type] * 1.12f) continue;
        const float reach = f.half * 2.5f + 20.0f;
        if (!g_mapCursorIn || std::fabs(g_mapCursor[0] - f.x) > reach || std::fabs(g_mapCursor[1] - f.y) > reach) continue;
        if (!best || f.half / smallest[f.type] > best->half / smallest[best->type]) best = &f;
    }
    return best;
}

// A clicked marker's menu: up to 16 entries of 0x48 bytes (count at +0x480), the action in each one's first byte. The
// game puts Remove Marker (action 0x4b) on the mod's Gold / Silver Chest pins too (they sit in the marker arrays like a
// custom pin), and a chest pin isn't the player's to remove (2026-11-12): taken off them
using MarkerMenuBuildFn = void (*)(void* entries, void* mapState, void* marker);
MarkerMenuBuildFn g_origMarkerMenuBuild = nullptr;
void DropChestRemove(uint8_t* entries, const uint8_t* marker) {
    __try {
        const uint32_t prefix = marker ? *reinterpret_cast<const uint32_t*>(marker) & 0xFFFF0000u : 0u;
        if (!entries || !marker || (prefix != kChestMarkerUidPrefix && prefix != kModIconMarkerUidPrefix)) return;
        uint8_t& count = entries[0x480];
        for (uint8_t i = 0; i < count && count <= 16;) {
            if (entries[static_cast<size_t>(i) * 0x48] == 0x4b) {
                std::memmove(entries + static_cast<size_t>(i) * 0x48, entries + static_cast<size_t>(i + 1) * 0x48,
                             static_cast<size_t>(count - i - 1) * 0x48);
                --count;
            } else {
                ++i;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void MarkerMenuBuildHook(void* entries, void* mapState, void* marker) {
    g_origMarkerMenuBuild(entries, mapState, marker);
    greeneye::Lease lease;
    if (lease && g_worldMapModded) DropChestRemove(static_cast<uint8_t*>(entries), static_cast<const uint8_t*>(marker));
}

void PublishCursorIcon() {
    HideRumorsOverHoveredPin(false);
    {
        // each quest pin against its own usual size (quest pins differ: a rumor's "?" in its circle isn't the size of a
        // main quest's "!"), kept per pin and reset when the zoom changes
        static std::unordered_map<const uint8_t*, float> usual;
        static float usualZoom = 0.0f;
        float zz = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        if (g_lastMapState && LevelMapView(g_lastMapState, zz, a1, a2, a3) && std::fabs(zz - usualZoom) > 1e-4f * zz)
            usual.clear(), usualZoom = zz;
        if (usual.size() > 4000) usual.clear();
        g_gameQuestHover = false;
        FrameIcon questHovered{};
        float questRatio = 0.0f;
        // (also against the other pins of its kind this frame, the smallest a little smaller or larger than it: zoomed
        // with the cursor on a rumor's "?", its own usual size was learnt enlarged and it never counted as hovered, so a
        // Silver Chest beside it took the hover, 2026-11-12)
        for (const FrameIcon& f : g_questIcons) {
            auto it = usual.find(f.m);
            if (it == usual.end()) it = usual.emplace(f.m, f.half).first;
            else it->second = std::min(it->second, f.half);
            float base = it->second;
            const auto kindOf = [](const uint8_t* m) {
                return ClassifyQuestOrRumorMarker(*reinterpret_cast<const uint32_t*>(m + 0x08), m[0x35]);
            };
            const uint32_t kind = kindOf(f.m);
            for (const FrameIcon& o : g_questIcons)
                if (o.m != f.m && o.half > f.half * 0.6f && o.half < base && kindOf(o.m) == kind) base = o.half;
            const float reach = f.half * 2.5f + 20.0f;
#ifdef UMM_DEV_BRIDGE
            {
                static uint64_t lgQ = 0;
                const float dq = std::fabs(g_mapCursor[0] - f.x) + std::fabs(g_mapCursor[1] - f.y);
                if (g_log && g_mapCursorIn && dq < 150.0f && GetTickCount64() - lgQ > 250) {
                    lgQ = GetTickCount64();
                    g_log("Map: quest pin %p at %.0f,%.0f d %.0f half %.1f usual %.1f base %.1f kind %08x", f.m, f.x, f.y, dq, f.half,
                          it->second, base, kind);
                }
            }
#endif
            // (its own usual size by 12 %, others' of its kind by 20 %: a rumor pin 19.7 against another's 17.5 passed,
            // 2026-11-12)
            const bool big = f.half >= it->second * 1.12f || f.half >= base * 1.2f;
            if (big && g_mapCursorIn && std::fabs(g_mapCursor[0] - f.x) < reach && std::fabs(g_mapCursor[1] - f.y) < reach) {
                g_gameQuestHover = true;
                if (f.half / base > questRatio) questRatio = f.half / base, questHovered = f;
            }
        }
        // A quest / rumor pin the game keeps hidden (shown by the mod's revealed pins): the game never hovers its icon,
        // only its search circle near the edge. With the cursor on its diamond (drawn above its spot, the diamond's tip)
        // and no card up, the mod names it on its panel (the pin's own title, Undiscovered: Trader Hat under the Hat Set
        // rumor's "?", 2026-11-12)
        // (left as it is on a pass with no icons drawn: two run a frame, one before the icons)
        if (!g_questIcons.empty()) g_questPinNamed = nullptr;
        if (!g_gameQuestHover && g_mapCursorIn && !g_questIcons.empty()) {
            for (const FrameIcon& f : g_questIcons) {
                // (the "?" sat ~47 UI px above its spot at half 19.7: the cursor on it read 1699,633 for a spot at 677)
                const float cy = f.y - f.half * 2.3f;
                if (std::fabs(g_mapCursor[0] - f.x) + std::fabs(g_mapCursor[1] - cy) > f.half * 1.2f) continue;
                if (!MarkerNotDiscovered(f.m[0x35])) continue; // (one the game shows: its own hover)
                g_questPinNamed = f.m;
                g_questPinNamedTick = GetTickCount64();
                questHovered = {f.m, f.x, cy, f.half, f.type};
                break;
            }
        }
        g_questIcons.clear();
        const FrameIcon* enlarged = GameEnlargedIcon();
#ifdef UMM_DEV_BRIDGE
        {
            static uint64_t lgE = 0;
            if (g_log && g_mapCursorIn && GetTickCount64() - lgE > 250) {
                lgE = GetTickCount64();
                g_log("Map: enlarged %p type %08x at %.0f,%.0f half %.1f; quest hover %d (%p); cursor %.0f,%.0f", enlarged ? enlarged->m : nullptr,
                      enlarged ? enlarged->type : 0u, enlarged ? enlarged->x : 0.0f, enlarged ? enlarged->y : 0.0f, enlarged ? enlarged->half : 0.0f,
                      g_gameQuestHover, questHovered.m, g_mapCursor[0], g_mapCursor[1]);
            }
        }
#endif
        if (enlarged) g_cursorBest = {enlarged->m, enlarged->x, enlarged->y, enlarged->half};
        // another pin the game draws enlarged with the cursor on it (a Silver Chest beside a rumor's "?"): that one
        if (g_gameQuestHover && enlarged && std::fabs(g_mapCursor[0] - enlarged->x) + std::fabs(g_mapCursor[1] - enlarged->y) <= enlarged->half)
            g_gameQuestHover = false;
        // the game's quest / rumor card is up: that pin is the hovered one (no other marker). The card has no panel
        // (Alchemic Wisdom, 2026-11-12): the mod draws the quest's name on its panel over it, in the same place. (Taking
        // the pin's id away so the game drew no card made the mod's own passes drop the pin too: it blinked.)
        if (g_questPinNamed) g_cursorBest = {questHovered.m, questHovered.x, questHovered.y, questHovered.half};
        if (g_gameQuestHover) {
            g_cursorBest = {questHovered.m, questHovered.x, questHovered.y, questHovered.half};
            const uint32_t qid = questHovered.m ? *reinterpret_cast<const uint32_t*>(questHovered.m + 0x08) : 0u;
            const uint8_t* quest = qid && g_getQuestByMarkerId ? static_cast<const uint8_t*>(g_getQuestByMarkerId(qid)) : nullptr;
            if (quest && ReadLocaText(*reinterpret_cast<const uint32_t*>(quest + 4), g_questTitle, sizeof(g_questTitle)) && g_questTitle[0]) {
                g_questTitleFor = questHovered.m;
                g_questTitleTick = GetTickCount64();
            }
        }
    }
    if (g_arrowHovered) g_cursorBest = {}; // the cursor on your arrow: the game shows its own label there (your place)
    g_frameIcons.clear();
#ifdef UMM_DEV_BRIDGE
    {
        static uint64_t last = 0;
        if (g_log && GetTickCount64() - last > 500 && g_mapCursorIn) {
            last = GetTickCount64();
            const uint8_t* b = g_cursorBest.m;
            const uint32_t id = b ? *reinterpret_cast<const uint32_t*>(b + 0x08) : 0u;
            const void* q = b && id && g_getQuestByMarkerId ? g_getQuestByMarkerId(id) : nullptr;
            g_log("Map: cursor %.0f,%.0f: %d icons drawn, nearest %.0f px (type %08x); picked %p type %08x orig %08x id %08x half %.1f quest %p; title for %p",
                  g_mapCursor[0], g_mapCursor[1], g_iconDrawsThisFrame, g_nearestIconD, g_nearestIconType, b,
                  b ? *reinterpret_cast<const uint32_t*>(b + 0x30) : 0u, b ? GetMarkerOrigTypeHash(b) : 0u, id, g_cursorBest.half, q,
                  g_questTitleFor);
            g_log("Map:   quest pins %d, nearest %.0f px at %.0f,%.0f (orig %08x)", g_questPinsSeen, g_nearestPinD, g_nearestPinAt[0],
                  g_nearestPinAt[1], g_nearestPinOrig);
        }
        g_iconDrawsThisFrame = 0, g_nearestIconD = 1e9f, g_nearestPinD = 1e9f, g_questPinsSeen = 0;
    }
#endif

    if (g_cursorBest.m) {
        g_hoveredGameMarker = g_cursorBest.m;
        g_hoveredGameMarkerTick = g_hoveredIconTick = GetTickCount64();
        g_hoveredIcon[0] = g_cursorBest.x, g_hoveredIcon[1] = g_cursorBest.y, g_hoveredIcon[2] = g_cursorBest.half;
    }
    g_cursorBest = {};
}

void DrawWorldMapMarkerIconHook(void* mapState, uint8_t* m, uint8_t* drawParams) {
    if (!g_origDrawWorldMapMarkerIcon) return;
    greeneye::Lease lease;
    if (lease && g_worldMapModded && MarkerHiddenOnMap(m)) return; // Map Key > Region: Hide, or another Hollow Hall
    if (lease && g_worldMapModded && UnderLocator(static_cast<uint8_t*>(mapState), m, drawParams)) return; // the locator on top
    if (lease && g_worldMapModded && m) NoteCursorIcon(static_cast<uint8_t*>(mapState), m, drawParams);
    if (lease && g_worldMapModded) DrawWorldMapMarkerIconWithOverrides(mapState, m, drawParams);
    else g_origDrawWorldMapMarkerIcon(mapState, m, drawParams);
}

bool TryPushMarkerPlateDimOpacity(uint8_t* entry, void*& outUiCtx) {
    outUiCtx = nullptr;
    if (!entry || !g_pushUiOpacity || !g_popUiOpacity || !g_uiCtx) return false;
    __try {
        uint8_t* m = *reinterpret_cast<uint8_t**>(entry + 8);
        if (!m) return false;
        uint8_t* client = GetUiClient();
        bool isChest = false;
        if (!ShouldDimWorldMapMarker(client, m, nullptr, &isChest)) return false;
        auto* uiGlobal = static_cast<uint8_t*>(g_uiCtx());
        void* uiCtx = uiGlobal ? *reinterpret_cast<void**>(uiGlobal + 0x18) : nullptr;
        if (!uiCtx) return false;
        const uint32_t markerState = static_cast<uint32_t>(m[0x34]);
        float dimAlpha = (isChest && markerState == 3u) ? 0.36f : 0.42f;
        if (MarkerOutsideFocusedRegion(m)) dimAlpha = MarkerHiddenOnMap(m) ? 0.0f : 0.3f; // its name plate too
        g_pushUiOpacity(uiCtx, dimAlpha);
        outUiCtx = uiCtx;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SafePopMarkerPlateDimOpacity(void* uiCtx) {
    if (!uiCtx || !g_popUiOpacity) return;
    __try {
        g_popUiOpacity(uiCtx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Pops the pushed opacity even when the game's draw faults (no C++ objects here, so __try is allowed)
void DrawMarkerPlateAndPop(void* widget, uint8_t* entry, const float* rect4, void* pushedUiCtx) {
    __try {
        g_origDrawWorldMapMarkerPlate(widget, entry, rect4);
    } __finally {
        SafePopMarkerPlateDimOpacity(pushedUiCtx);
    }
}

// The name the game shows over the marker under the cursor comes from its title text; a marker the World Map doesn't
// draw has none (the game picks the hovered marker from its list each frame, drawn or not): outside the region with
// Map Key > Region > Hide, or in the Hollow Halls' corner of the overworld map, or in another hall. Only while the World
// Map is up: the halls' rule is about that map. The mod's own calls (Find) go to the game's function itself
// (g_getMarkerTitleText is the original after the hook).
bool TitleHiddenByRegion(const void* m) {
    if (!m || GetTickCount64() - g_lastWorldMapFrameTick > 250ULL) return false;
    __try {
        return MarkerHiddenOnMap(static_cast<const uint8_t*>(m));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The mod's own hover title is showing (its icon is under the cursor): one name at a time. Not while a marker's menu
// is open (mapState +0xf8 is 3): that title is the menu's.
bool ModHoverTitleShowing() {
    if (GetTickCount64() - g_modHoverTitleTick > 100ULL) return false;
    const uint8_t* mapState = g_lastMapState;
    return !mapState || *reinterpret_cast<const uint32_t*>(mapState + 0xf8) != 3;
}

// The game's hover title's text, for its area (g_titleBox)
void NoteHoverTitle(const UiText* got) {
    __try {
        if (got && got->utf8 && got->len > 0 && got->len < sizeof(g_hoverTitleText)) {
            std::memcpy(g_hoverTitleText, got->utf8, got->len);
            g_hoverTitleText[got->len] = 0;
            g_hoverTitleTick = GetTickCount64();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The game's hover title the mod draws itself, on a tooltip panel: the hovered marker's, while its icon's place is known
// (drawn through the icon hook this frame or the last) and no menu is open on a marker
bool GameTitleReplaced(const void* m) {
    const uint64_t now = GetTickCount64();
    if (!m || m != g_hoveredGameMarker || now - g_hoveredIconTick > 150ULL) return false;
    // a quest or rumor pin ("!" / "?"): the game's label for it doesn't come from the title asked here (left empty, it
    // still showed), so the mod's panel only doubled it; the game's is kept
    if (*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m) + 0x30) == 0xafce0c5fu) return false;
    const uint8_t* mapState = g_lastMapState;
    return !mapState || *reinterpret_cast<const uint32_t*>(mapState + 0xf8) != 3;
}

UiText* MarkerTitleTextHook(UiText* outText, const void* uiMarker) {
    {
        greeneye::Lease lease;
        if (lease && g_worldMapModded && outText && TitleHiddenByRegion(uiMarker)) {
            *outText = MakeUiText("");
            return outText;
        }
    }
    UiText* got = g_getMarkerTitleText(outText, uiMarker);
    if (uiMarker && uiMarker == g_hoveredGameMarker) NoteHoverTitle(got); // the hovered one's text
    {
        // the mod's own title shows instead (its icon is under the cursor), or the mod draws this one itself, on a
        // tooltip panel (DrawGameHoverTitle): the game's left empty
        greeneye::Lease lease;
        // (a hidden quest pin named by the mod: the pin the game hovers under it, the Silver Chest beside the Hat Set
        // rumor, gets no plain title over it)
        const bool pinNamed = g_questPinNamed && uiMarker != g_questPinNamed && GetTickCount64() - g_questPinNamedTick < 150ULL &&
                              g_lastMapState && *reinterpret_cast<const uint32_t*>(g_lastMapState + 0xf8) != 3;
        if (lease && g_worldMapModded && got && (ModHoverTitleShowing() || GameTitleReplaced(uiMarker) || pinNamed)) *got = MakeUiText("");

    }
    return got;
}

#ifdef UMM_DEV_BRIDGE
// dev (map op "nearplate" x,y): the plates drawn within 120 UI px of a screen point, their entry's raw words
float g_devNearPlate[2] = {-1.0f, -1.0f};
void LogPlateNear(uint8_t* entry, const float* rect4) {
    __try {
        static int n = 0;
        static uint64_t since = 0;
        if (GetTickCount64() - since > 3000) since = GetTickCount64(), n = 0;
        if (!entry || !rect4 || n > 30 || !g_log) return;
        if (std::fabs(rect4[0] + rect4[2] * 0.5f - g_devNearPlate[0]) > 120.0f || std::fabs(rect4[1] + rect4[3] * 0.5f - g_devNearPlate[1]) > 120.0f) return;
        ++n;
        char w[512];
        int len = 0;
        for (int k = 0; k < 0x48 && len < 480; k += 4)
            len += std::snprintf(w + len, sizeof(w) - len, " %08x", *reinterpret_cast<const uint32_t*>(entry + k));
        g_log("Plate near: rect %.0f %.0f %.0f %.0f entry %p:%s", rect4[0], rect4[1], rect4[2], rect4[3], entry, w);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#endif

void DrawWorldMapMarkerPlateHook(void* widget, uint8_t* entry, const float* rect4) {
    if (!g_origDrawWorldMapMarkerPlate) return;

    greeneye::Lease lease;
    void* pushedUiCtx = nullptr;
    bool pushed = false;
    {
        if (lease && g_worldMapModded) {
            pushed = TryPushMarkerPlateDimOpacity(entry, pushedUiCtx);
        }
    }

#ifdef UMM_DEV_BRIDGE
    if (g_devNearPlate[0] > 0.0f) LogPlateNear(entry, rect4);
#endif
    DrawMarkerPlateAndPop(widget, entry, rect4, pushed ? pushedUiCtx : nullptr);
}

#include "map_hints.inl"

} // namespace

// Called on the UI/game thread by the authenticated local bridge. Older hot-reload
// generations decline the call, so the bridge can find the currently active DLL.
#ifdef UMM_DEV_BRIDGE
extern "C" __declspec(dllexport) int MapNativeControl(const char* action, const char* value, void* uiClient, MapControlState* out) {
    greeneye::Lease lease;
    if (!lease) return 0;
    if (!out || out->size != sizeof(MapControlState) || out->version != 1 || !action || !value) return -1;
    auto* client = static_cast<uint8_t*>(uiClient);
    if (!client) return -2;
    LoadMinimapConfig();
    const bool worldMap = MapScreen(client[0x1cd8]);
    const bool hud = client[0x1cd8] == 1;
    const bool status = std::strcmp(action, "status") == 0;
    if (!status && !worldMap && !hud) return -2;
    bool changed = false;
    const int mode = std::strcmp(value, "explored") == 0 ? 0 : std::strcmp(value, "hybrid") == 0 ? 1 : std::strcmp(value, "reveal_all") == 0 ? 2 : -1;
    if (status) {}
    else if (std::strcmp(action, "settings") == 0 && (std::strcmp(value, "open") == 0 || std::strcmp(value, "close") == 0)) {
        (worldMap ? g_settingsOpen : g_minimapSettingsOpen) = std::strcmp(value, "open") == 0;
    } else if (std::strcmp(action, "panel") == 0 && (std::strcmp(value, "expand") == 0 || std::strcmp(value, "collapse") == 0)) {
        g_panelCollapsed = std::strcmp(value, "collapse") == 0;
    } else if (std::strcmp(action, "fog") == 0 && mode >= 0) {
        ApplyFogRevealMode(static_cast<FogRevealMode>(mode));
        SaveMinimapConfig();
        changed = true;
    } else if (std::strcmp(action, "pins") == 0 && mode >= 0) {
        g_pinRevealMode = static_cast<PinRevealMode>(mode);
        g_chestLootDirty = true;
        changed = true;
    } else if (std::strcmp(action, "markers") == 0 && (std::strcmp(value, "show_all") == 0 || std::strcmp(value, "hide_all") == 0)) {
        SetAllFiltersState(client, nullptr, std::strcmp(value, "show_all") == 0);
        changed = true;
    } else if ((std::strcmp(action, "minimap") == 0 || std::strcmp(action, "terrain") == 0) && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        (std::strcmp(action, "minimap") == 0 ? g_minimapVisible : g_minimapShowTerrain) = std::strcmp(value, "on") == 0;
        changed = true;
    } else if (std::strcmp(action, "recenter") == 0 && worldMap) {
        g_bridgeRecenterMap = true;
#ifdef UMM_DEV_BRIDGE
    } else if (std::strcmp(action, "locatorzoom") == 0) {
        g_locatorRingZoom = static_cast<float>(std::atof(value)); // dev
    } else if (std::strcmp(action, "locatorbox") == 0) { // dev: "left,top,right,bottom" round a player's spot
        std::sscanf(value, "%f,%f,%f,%f", &g_locatorBox[0], &g_locatorBox[1], &g_locatorBox[2], &g_locatorBox[3]);
    } else if (std::strcmp(action, "nearplate") == 0) {
        sscanf_s(value, "%f,%f", &g_devNearPlate[0], &g_devNearPlate[1]); // dev: plates drawn near a screen point
    } else if (std::strcmp(action, "near") == 0) {
        sscanf_s(value, "%f,%f", &g_devNear[0], &g_devNear[1]); // dev: markers projected near a screen point
    } else if (std::strcmp(action, "dot") == 0) {
        // dev: value "x,z": a magenta dot drawn where the mod projects that world spot (0: off)
        g_devDot[0] = g_devDot[1] = -1.0f;
        sscanf_s(value, "%f,%f", &g_devDot[0], &g_devDot[1]);
    } else if (std::strcmp(action, "zoomset") == 0) {
        g_bridgeZoomSet = static_cast<float>(std::atof(value)); // dev: the World Map's zoom set next frame
    } else if (std::strcmp(action, "meshtest") == 0) {
        desk::g_meshTest = std::atoi(value); // dev: a textured spinning quad (desk.inl MeshTest)
    } else if (std::strcmp(action, "deskband") == 0) {
        desk::g_deskBandKey = std::atoi(value); // dev: the over layer's sub-layer
#endif
#ifdef UMM_DEV_BRIDGE
    } else if (std::strcmp(action, "eyelog") == 0) {
        chest_glow::g_eyeLogUntil.store(GetTickCount64() + 1000ULL * static_cast<uint64_t>(std::max(0, std::atoi(value))));
#endif
    } else if (std::strcmp(action, "proxyfinal") == 0) {
        InterlockedExchange(&chest_glow::g_proxyFinal, std::atoi(value) && chest_glow::g_stageNext ? 1 : 0); // copies placed before staging
    } else if (std::strcmp(action, "proxysmooth") == 0) {
        const double v = std::atof(value); // G G creature copies: 0 = the last camera step, up to 0.95 = a long average
        if (v >= 0.0 && v <= 0.95) chest_glow::g_proxySmooth = v;
    } else if (std::strcmp(action, "proxypredict") == 0) {
        InterlockedExchange(&chest_glow::g_proxyPredict, std::atoi(value) ? 1 : 0); // G G creature copies: camera one step ahead
    } else if (std::strcmp(action, "proxylate") == 0) {
        InterlockedExchange(&chest_glow::g_proxyLate, std::atoi(value) ? 1 : 0); // G G creature copies placed again late in the frame
    } else if (std::strcmp(action, "proxyeye") == 0) {
        chest_glow::g_proxyEye = std::atoi(value) == 0 ? 0 : 1; // G G creature copies: 1 client view, 0 renderer camera
        if (g_log) g_log("Chest glow: creature copies placed by the %s", chest_glow::g_proxyEye ? "client view" : "renderer camera");
    } else if (std::strcmp(action, "proxynear") == 0) {
        const float m = static_cast<float>(std::atof(value)); // G G creature copies: metres from the eye
        if (m >= 0.1f && m <= 10.0f) chest_glow::g_proxyNear = m;
        if (g_log) g_log("Chest glow: creature copies %.2f m from the eye", chest_glow::g_proxyNear);
    } else if (std::strcmp(action, "nearcreatures") == 0 && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        g_nearCreatures = std::strcmp(value, "on") == 0; // Settings > Advanced > Nearby Creatures Only
        SaveMinimapConfig();
    } else if (std::strcmp(action, "deskdim") == 0) {
        g_deskDim = std::clamp(static_cast<float>(std::atof(value)), 0.05f, 1.0f); // the desk art's brightness (trial)
    } else if (std::strcmp(action, "mapdesk") == 0 && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        g_mapDesk = std::strcmp(value, "on") == 0; // Settings > Advanced > Map Desk
        SaveMinimapConfig();
    } else if (std::strcmp(action, "centerat") == 0 && worldMap) {
        float x = 0.0f, z = 0.0f; // value "x,z": the World Map centred on that spot
        if (sscanf_s(value, "%f,%f", &x, &z) == 2) g_bridgeCenterX = x, g_bridgeCenterZ = z;
#ifdef UMM_DEV_BRIDGE
    } else if (std::strcmp(action, "movers") == 0 && client) {
        // dev: call twice; the second call logs the entities within 120 m that moved since the first, with their
        // components (type:size) and the first bytes of the small ones
        static std::unordered_map<uint64_t, std::array<float, 3>> before;
        const bool first = before.empty();
        float px = 0.0f, pz = 0.0f;
        int moved = 0, seen = 0;
        if (LocalPlayerXZ(client, px, pz)) {
            for (int ri = 0; ri < kMaxWaterRegistries; ++ri) {
                auto* reg = static_cast<uint8_t*>(g_waterRegistries[ri].load(std::memory_order_relaxed));
                if (!reg) continue;
                boss_live::RegistryTypes& t = boss_live::g_types[ri];
                if (!t.position) boss_live::DetectTypes(reg, t, px, pz);
                if (!t.position) continue;
                uint8_t* map = reg + 0x148;
                const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
                auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
                auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
                auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
                if (!used || !ids || !records || capacity > (1u << 22)) continue;
                for (uint64_t i = 0; i < capacity; ++i) {
                    if (!(used[i >> 6] >> (i & 63) & 1) || ids[i] < 0x400 || !records[i]) continue;
                    float pos[3];
                    if (!EcsPosition(records[i], t.position, pos) || (pos[0] - px) * (pos[0] - px) + (pos[2] - pz) * (pos[2] - pz) > 120.0f * 120.0f) continue;
                    ++seen;
                    const uint64_t key = (static_cast<uint64_t>(ri) << 32) | ids[i];
                    if (first) { before[key] = {pos[0], pos[1], pos[2]}; continue; }
                    auto it = before.find(key);
                    if (it == before.end() || std::hypot(pos[0] - it->second[0], pos[2] - it->second[2]) < 0.3f) continue;
                    if (++moved > 12) continue;
                    std::string comps;
                    const auto* bits = *reinterpret_cast<const uint64_t**>(records[i] + 0x18);
                    for (int ty = 0; ty < kEcsTypeBits; ++ty) {
                        if (!(bits[ty >> 6] >> (ty & 63) & 1)) continue;
                        uint16_t size = 0;
                        const uint8_t* c = EcsComponent(records[i], static_cast<uint16_t>(ty), &size);
                        char one[96];
                        int n = std::snprintf(one, sizeof(one), " %d:%u", ty, size);
                        if (c && size > 0 && size <= 16) {
                            n += std::snprintf(one + n, sizeof(one) - n, "=");
                            for (uint16_t k = 0; k < size && n < 90; ++k) n += std::snprintf(one + n, sizeof(one) - n, "%02x", c[k]);
                        }
                        comps += one;
                    }
                    if (g_log) g_log("Mover reg %d id %x at %.1f,%.1f,%.1f moved %.1f m:%s", ri, ids[i], pos[0], pos[1], pos[2],
                                     std::hypot(pos[0] - it->second[0], pos[2] - it->second[2]), comps.c_str());
                }
            }
        }
        if (!first) before.clear();
        if (g_log) g_log("Movers: %s, %d entities within 120 m, %d moved", first ? "first pass" : "second pass", seen, moved);
    } else if (std::strcmp(action, "creatures") == 0 && client) {
        // dev: the live creatures within 75 m (row, distance, entity) with their components of up to 32 bytes
        // (type:size=bytes); value "brief" leaves the components out
        if (std::strcmp(value, "watch") == 0 || std::strcmp(value, "unwatch") == 0) creature_live::g_watch = value[0] == 'w';
        float px = 0.0f, pz = 0.0f;
        std::vector<creature_live::LiveCreature> live;
        if (LocalPlayerXZ(client, px, pz) && creature_live::Scan(live, px, pz, 75.0f)) {
            const bool brief = std::strcmp(value, "brief") == 0;
            for (const auto& c : live) {
                std::string comps;
                if (!brief) {
                    {
                        auto* reg = static_cast<uint8_t*>(g_waterRegistries[c.registry].load(std::memory_order_relaxed));
                        creature_live::ForEachEntity(reg, [&](uint32_t id, uint8_t* rec) {
                            if (id != c.entity) return;
                            const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
                            for (int ty = 0; ty < kEcsTypeBits; ++ty) {
                                if (!(bits[ty >> 6] >> (ty & 63) & 1)) continue;
                                uint16_t size = 0;
                                const uint8_t* d = EcsComponent(rec, static_cast<uint16_t>(ty), &size);
                                char one[128];
                                int n = std::snprintf(one, sizeof(one), " %d:%u", ty, size);
                                if (d && size > 0 && size <= 32) {
                                    n += std::snprintf(one + n, sizeof(one) - n, "=");
                                    for (uint16_t k = 0; k < size && n < 124; ++k) n += std::snprintf(one + n, sizeof(one) - n, "%02x", d[k]);
                                }
                                comps += one;
                            }
                        });
                    }
                }
                std::string rows;
                creature_live::ForEachRow(c.model, [&](uint16_t r) { rows += rows.empty() ? "" : " / "; rows += OreName(r); });
                if (g_log) g_log("Creature %s reg %d id %x at %.1f,%.1f,%.1f (%.0f m):%s", rows.c_str(), c.registry, c.entity,
                                 c.x, c.y, c.z, std::hypot(c.x - px, c.z - pz), comps.c_str());
            }
        }
        if (g_log) g_log("Creatures: %zu live within 75 m", live.size());
    } else if (std::strcmp(action, "entdump") == 0 && client) {
        // dev: every entity within 75 m to the file named by value: per entity "E id reg x y z", then per component
        // "C type size hex" (up to 512 bytes)
        float px = 0.0f, pz = 0.0f;
        FILE* f = nullptr;
        int ents = 0;
        if (LocalPlayerXZ(client, px, pz) && fopen_s(&f, value, "w") == 0 && f) {
            for (int ri = 0; ri < kMaxWaterRegistries; ++ri) {
                auto* reg = static_cast<uint8_t*>(g_waterRegistries[ri].load(std::memory_order_relaxed));
                if (!reg) continue;
                boss_live::RegistryTypes& bt = boss_live::g_types[ri];
                if (!bt.position) boss_live::DetectTypes(reg, bt, px, pz);
                if (!bt.position) continue;
                creature_live::ForEachEntity(reg, [&](uint32_t id, uint8_t* rec) {
                    float p[3];
                    if (!EcsPosition(rec, bt.position, p) || (p[0] - px) * (p[0] - px) + (p[2] - pz) * (p[2] - pz) > 75.0f * 75.0f) return;
                    ++ents;
                    std::fprintf(f, "E %x %d %.2f %.2f %.2f\n", id, ri, p[0], p[1], p[2]);
                    const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
                    for (int ty = 0; ty < kEcsTypeBits; ++ty) {
                        if (!(bits[ty >> 6] >> (ty & 63) & 1)) continue;
                        uint16_t size = 0;
                        const uint8_t* d = EcsComponent(rec, static_cast<uint16_t>(ty), &size);
                        std::fprintf(f, "C %d %u ", ty, size);
                        for (uint16_t k = 0; d && k < size && k < 4096; ++k) std::fprintf(f, "%02x", d[k]);
                        std::fputc('\n', f);
                    }
                });
            }
            std::fclose(f);
        }
        if (g_log) g_log("Entity dump: %d entities to %s", ents, value);
    } else if (std::strcmp(action, "depnear") == 0 && client) {
        // dev: the creature spawns within 150 m of you and what the World Map's deposit pass makes of each
        float px = 0.0f, pz = 0.0f;
        if (LocalPlayerXZ(client, px, pz))
            for (size_t d = 0; d < kOreDepositCount; ++d) {
                const OreDeposit& dep = kOreDeposits[d];
                if (dep.ore < kCreaturesFirst || dep.ore >= kCreaturesFirst + kCreaturesCount) continue;
                const float dx = dep.x - px, dz = dep.z - pz;
                if (dx * dx + dz * dz > 150.0f * 150.0f) continue;
                if (g_log) g_log("Dep %zu %s at %d,%d (y %d): on %d area %d resCat %d explored %d done %d outside %d hallHides %d",
                                 d, OreName(dep.ore), dep.x, dep.z, dep.topY, g_oreEnabled[dep.ore] ? 1 : 0,
                                 kOreAreaColors[dep.ore] != 0, IsResourceCategory(kOreCategory[dep.ore]) ? 1 : 0,
                                 IsPositionExploredByCharacter(client, dep.x, dep.z) ? 1 : 0, KnowledgeDone(d) ? 1 : 0,
                                 OutsideFocusedRegion(dep.x, dep.z) ? 1 : 0,
                                 halls::MapHidesAt(dep.x, dep.topY, dep.z, IsKnowledgeDeposit(d)) ? 1 : 0);
            }
    } else if (std::strcmp(action, "questat") == 0 && client) {
        // dev: value "x,z": the markers there and the quests the game has for them, logged in the next map frame
        float qx = 0, qz = 0;
        if (sscanf_s(value, "%f,%f", &qx, &qz) == 2) g_questAt[0] = qx, g_questAt[1] = qz, g_questAtPending = true;
    } else if (std::strcmp(action, "rumors") == 0 && client) {
        // dev: the markers shown as a quest / rumor "?" (type afce0c5f), their whole 0xA0 bytes as u32 words
        constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
        int shown = 0;
        for (size_t a = 0; a < 4 && shown < 12; ++a) {
            uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
            const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
            if (!base || count == 0 || count > 10000) continue;
            for (uint64_t i = 0; i < count && shown < 12; ++i) {
                const uint8_t* m = base + i * 0xA0;
                if (*reinterpret_cast<const uint32_t*>(m + 0x30) != 0xafce0c5fu) continue;
                ++shown;
                std::string words;
                for (int w = 0; w < 0xA0; w += 4) {
                    char one[16];
                    std::snprintf(one, sizeof(one), "%s%08x", w % 32 ? " " : " |", *reinterpret_cast<const uint32_t*>(m + w));
                    words += one;
                }
                if (g_log) g_log("Rumor a%zu#%llu orig %08x at %d,%d:%s", a, static_cast<unsigned long long>(i), GetMarkerOrigTypeHash(m),
                                 *reinterpret_cast<const int32_t*>(m + 0x14), *reinterpret_cast<const int32_t*>(m + 0x24), words.c_str());
            }
        }
    } else if (std::strcmp(action, "spires") == 0 && client) {
        // dev: every spire marker (and its twin) in the log: where, the type drawn, the flags the Alt + click lookup reads
        constexpr size_t kArrayOffsets[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
        for (size_t a = 0; a < 4; ++a) {
            uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrayOffsets[a]);
            const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrayOffsets[a] + 8);
            if (!base || count == 0 || count > 10000) continue;
            for (uint64_t i = 0; i < count; ++i) {
                const uint8_t* m = base + i * 0xA0;
                const uint32_t orig = GetMarkerOrigTypeHash(m);
                if (orig != 0xa17072beu && orig != 0x98b2d65du) continue;
                if (g_log) g_log("Spire a%zu #%llu at %d,%d orig %08x type30 %08x b48 %u s34 %u s35 %02x hidden %d done %d",
                                 a, static_cast<unsigned long long>(i), *reinterpret_cast<const int32_t*>(m + 0x14),
                                 *reinterpret_cast<const int32_t*>(m + 0x24), orig, *reinterpret_cast<const uint32_t*>(m + 0x30),
                                 m[0x48], m[0x34], m[0x35], MarkerHiddenOnMap(m) ? 1 : 0,
                                 PlaceMarkedDone(*reinterpret_cast<const int32_t*>(m + 0x14), *reinterpret_cast<const int32_t*>(m + 0x24)) ? 1 : 0);
            }
        }
#endif
    } else if (std::strcmp(action, "hallsmap") == 0 && worldMap && halls::g_mapDungeon >= 0) {
        halls::g_mapOverworld = !halls::g_mapOverworld; // the Dungeon Map / Overworld Map button
        g_worldMapOpenCenterFrames = 2;
    } else if (std::strcmp(action, "modoff") == 0 && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        SetModOff(std::strcmp(value, "on") == 0); // Ctrl+Shift+F12 without the keys ("on": the mod off)
    } else if (std::strcmp(action, "vanilla") == 0 && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        SetVanillaWorldMap(std::strcmp(value, "on") == 0); // Settings > Advanced > Vanilla World Map
    } else if (std::strcmp(action, "vanillamm") == 0 && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        g_vanillaMinimap = std::strcmp(value, "on") == 0; // Settings > Advanced > Vanilla Minimap
        SaveMinimapConfig();
    } else if (std::strcmp(action, "dungeonmaps") == 0 && (std::strcmp(value, "on") == 0 || std::strcmp(value, "off") == 0)) {
        SetDungeonMaps(std::strcmp(value, "on") == 0); // Settings > Advanced > Dungeon Maps
    } else if (std::strcmp(action, "gg") == 0 && hud) {
        // G G without the keys: the chests and veins around you
        chest_glow::LightNearbyChests(client);
        vein_sight::LightNearby(client);
    } else if (std::strcmp(action, "regions") == 0) {
        // The Map Key's regions shown, as a bit mask ("3": Springlands and Revelwood)
        g_regionMask = static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) & kAllRegionsMask;
        UpdateFocusedRegion(client);
        changed = true;
    } else return -1;
    if (changed) {
        SaveMinimapConfig();
        g_bridgeRefreshMap = true;
    }
    out->settingsOpen = worldMap ? g_settingsOpen : g_minimapSettingsOpen;
    out->panelExpanded = !g_panelCollapsed;
    out->fog = static_cast<uint32_t>(g_fogRevealMode);
    out->pins = static_cast<uint32_t>(g_pinRevealMode);
    out->minimap = g_minimapVisible;
    out->terrain = g_minimapShowTerrain;
    return 1;
}
#endif
#include "map_native_filters.inl"

bool InstallMapKeyPanelHook(LogFn logger) {
    g_log = logger;
    if (g_hookInstalled) return true;
    if (!hdpool::Init() && g_log) g_log("Hollow Halls: reusable GPU images unavailable; plain maps");

    uint8_t* fnBeginWidget = FindUnique("40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 45 33 C0");
    uint8_t* fnEndWidgetTail = FindUnique("80 79 20 00 48 8B D9 74 6A 48");
    uint8_t* fnSetSize = FindUnique("48 89 54 24 08 0F 57 C0 F3 0F 10 54 24 08");
    uint8_t* fnUiCtx = FindUnique("48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 4C 8B 41 28");
    uint8_t* fnImageTail = FindUnique("41 0F 10 10 33 C0 C7 44 24 30 97 69 E8 F9 48 89 44 24 34 48 8B D9 49 8B 48 08 48 8B FA 89 44 24 3C 66 48 0F 7E D0 48 0B C8 0F 10 44 24 30 0F 11 54 24 40 0F 11 44 24 50 75 ?? 48 83 C3 10 EB ?? 48 8B 43 10 48 8D 54 24 40 48 83 C3 10 45 33 C0");
    uint8_t* fnDrawImage = FindUnique("4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC");
    uint8_t* fnBeginLabel = FindUnique("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 50 48 8B DA 41 0F B6 F9");
    uint8_t* fnEndLabel = FindUnique("40 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 0F 29 BC 24 B0 00 00 00");
    uint8_t* fnBeginLayer = FindUnique("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 45 8B D9 44 8B 8C 24 80 00 00 00 41 0F B6 D8");
    uint8_t* fnEndLayer = FindUnique("40 53 48 83 EC 20 48 89 74 24 38 48 8B D9 48 8B 35 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10 48 8B 09 E8");
    uint8_t* fnNamedWidget = FindUnique("48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B F9 48 8B DA 45 33 C0 48 8B 52 10 48 8B 48 18 48 89 0F 48 C7 47");
    uint8_t* fnOpaque = FindUnique("40 53 48 83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 48 8B D9 48 8B 49 08 0F 28 C1 0F 14 C1 66 48 0F 7E C2 E8");
    // drawMapFilterMenu, worldMapInput and doPlayStateUi are matched behind their prologues (the
    // bytes a hook replaces), so they are found whichever mod hooked them first.
    uint8_t* fnDrawMapFilterMenuTail = FindUnique("48 81 EC ?? 08 00 00 0F 29 70 B8 4C 8B F9 48 8B 0D");
    uint8_t* fnWorldMapInputTail = FindUnique("48 8D AC 24 A0 F8 FF FF 48 81 EC 60 08 00 00 48 8B 05");
    uint8_t* fnUpdateMapViewport = FindUnique("48 8B C4 48 89 58 20 55 56 57 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00");
    uint8_t* fnSetTypeFilterMid = FindUnique("8D 42 0C C6 06 44 89 47 40");
    uint8_t* fnApplyTypeFilterDirect = FindUnique("85 D2 0F 84 73 01 00 00 89 54 24 10 53 48 83 EC 20 48 8B D9 48 81 C1 30");
    uint8_t* fnGetMarkerType = FindUnique("85 C9 74 13 48 8B 42 20 48 85 C0 74 0A 8B D1 48 8B C8 E9");
    uint8_t* fnGetMarkerTitleText = FindUnique("48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 70 48 8B FA 48 8B D9 48 85 D2 75 27 0F 10 05");
    uint8_t* fnLocaIdToText = FindUnique("48 89 5C 24 10 48 89 74 24 18 55 57 41 55 41 56 41 57 48 8D AC 24 F0 F8 FF FF 48 81 EC 10 08 00");
    uint8_t* fnCheckWidgetButton = FindUnique("40 53 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 48 8B DA 48 8B 49 08 48 8B 50 10 0F B6 44 24 60 88 44 24 20 8B 52 44 E8");
    uint8_t* fnCenterMapOnPos = FindUnique("48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 41 0F B6 F0 48 8B 0D ?? ?? ?? ?? 48 8B FA");
    uint8_t* fnDialogInit = FindUnique("48 83 EC 28 0F 10 0D ?? ?? ?? ?? 48 C7 41 30 0A 00 00 00");
    uint8_t* fnTextDialog = FindUnique("48 8B C4 55 53 41 55 41 56 48 8D A8 18 F5 FF FF");
    // DrawNineSlice(widget, rect, imageRef, borders[4], colour): the map key's boxes and glows.
    uint8_t* fnNineSlice = FindUnique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 30 48 8B F2");
    uint8_t* fnDrawInGameMapPulse = FindUnique("48 8B C4 53 48 81 EC C0 00 00 00 0F 29 70 E8 B2 83 0F 29 78 D8 66 48 0F 6E F1");
    uint8_t* fnGetActiveQuestForMarkerId = FindUnique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 33 FF 8B D9 8B EF");
    uint8_t* fnIsLoreRumorMarkerId = FindUnique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 41 57 85 C9 0F 84");
    uint8_t* fnResolveAssetSpec = FindUnique("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 42 08 41 0F B6 F0 48 0B 02");
    uint8_t* fnDrawImageSubUv = FindUnique("48 83 EC 48 48 8B 41 08 48 8B 48 50 48 8B 49 18 48 85 C9 74 36 83 79 08 04");
    uint8_t* fnDrawWorldMapCanvas = FindUnique("40 55 53 56 57 41 55 41 56 48 8D AC 24 28 E9 FF FF B8 D8 17 00 00");
    uint8_t* fnGetUiDrawList = FindUnique("48 85 C9 74 0E 83 79 08 04 75 08 48 8B 81 E0 6C 00 00 C3 33 C0 C3");
    uint8_t* fnPushUiClipRect = FindUnique("48 89 5C 24 10 57 48 83 EC 60 0F 29 74 24 50 48 8B F9 48 8B 89 10 42 00 00 0F 57 E4");
    uint8_t* fnPopUiClipRect = FindUnique("48 FF 89 B0 01 00 00 83 79 08 04 75 28 4C 8B 81 E0 6C 00 00 49 FF 48 58 49 8B 50 58 49 8B 40 50 0F B7 54 50 FE 66 41 89 90 2A 52 03 00 41 C6 80 D2 52 03 00 00 E9 ?? ?? ?? ?? CC CC CC CC CC CC");
    uint8_t* fnPushUiOpacity = FindUnique("48 83 EC 38 83 79 08 04 0F 29 74 24 20 0F 28 F1 75 6A 48 8B 81 E0 6C 00 00 48 89 5C 24 30");
    uint8_t* fnPopUiOpacity = FindUnique("83 79 08 04 75 0E 48 8B 81 E0 6C 00 00 48 FF 88 C0 00 00 00 C3 CC");
    g_fogColorSite = FindUnique("66 48 0F 7E D0 F3 0F 10 55 30 66 0F 73 D8 08 C7 00 ?? ?? ?? ?? C7 40 04 ?? ?? ?? ?? C7 40 08 ?? ?? ?? ?? F3 0F 11 50 0C");
    g_fogAlphaSite = FindUnique("C7 85 10 17 00 00 ?? ?? ?? ?? 48 8D 55 60 C7 85 14 17 00 00 ?? ?? ?? ?? 44 8B C0 4C 8B 8D 10 17 00 00 48 8B CE");
    uint8_t* fnPlayStateUiTail = FindUnique("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00 48 8B 01 48 85 C0 0F 84 ?? ?? ?? ?? 4D 89 73 C8 4C 8B 70 38");
    uint8_t* fnBuildWorldMapPlayersTail = FindUnique("48 81 EC 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B F9 4C 8B A0 D0 00 00 00 4C 89 A5 90 00 00 00");
    uint8_t* fnDrawWorldMapMarkerIconTail = FindUnique("80 BA 9C 00 00 00 00 49 8B D8 F3 0F 10 05 ?? ?? ?? ?? 0F 29 70 B8 0F 29 78 A8");
    uint8_t* fnDrawWorldMapMarkerPlateTail = FindUnique("F3 0F 10 42 20 49 8B F8 F3 0F 10 4A 1C 48 8B F1 F3 0F 5C 42 28 F3 0F 5C 4A 24");

    if (!fnBeginWidget || !fnEndWidgetTail || !fnSetSize || !fnUiCtx || !fnImageTail || !fnDrawImage ||
        !fnBeginLabel || !fnEndLabel || !fnBeginLayer || !fnEndLayer || !fnNamedWidget || !fnOpaque ||
        !fnDrawMapFilterMenuTail || !fnSetTypeFilterMid || !fnGetMarkerType) {
        if (g_log) g_log("MapKeyPanel: One or more UI signatures not found.");
        return false;
    }

    uint8_t* fnDrawMapFilterMenu = fnDrawMapFilterMenuTail - 20;

    // Read through the loader: the game's bytes even if another mod hooked BeginWidget.
    int32_t gDisp = 0;
    ReadCode(fnBeginWidget + 9, &gDisp, 4);
    g_uiRoot = reinterpret_cast<void**>(fnBeginWidget + 13 + gDisp);

    g_beginWidget = reinterpret_cast<BeginWidgetFn>(fnBeginWidget);
    g_endWidget = reinterpret_cast<EndWidgetFn>(fnEndWidgetTail - 15);
    g_setSize = reinterpret_cast<SetSizeFn>(fnSetSize);
    g_uiCtx = reinterpret_cast<UiCtxFn>(fnUiCtx);
    g_image = reinterpret_cast<ImageFn>(fnImageTail - 15);
    g_drawImage = reinterpret_cast<DrawImageFn>(fnDrawImage);
    g_beginLabel = reinterpret_cast<BeginLabelFn>(fnBeginLabel);
    g_endLabel = reinterpret_cast<EndLabelFn>(fnEndLabel);
    g_beginLayer = reinterpret_cast<BeginLayerFn>(fnBeginLayer);
    g_endLayer = reinterpret_cast<EndLayerFn>(fnEndLayer);
    g_namedWidget = reinterpret_cast<NamedWidgetFn>(fnNamedWidget);
    g_opaque = reinterpret_cast<OpaqueFn>(fnOpaque);
    g_setTypeFilter = reinterpret_cast<SetTypeFilterFn>(fnSetTypeFilterMid - 0x54);
    g_applyTypeFilterDirect = reinterpret_cast<ApplyTypeFilterDirectFn>(fnApplyTypeFilterDirect);
    g_getMarkerType = reinterpret_cast<GetMarkerTypeFn>(fnGetMarkerType);
    g_getMarkerTitleText = reinterpret_cast<GetMarkerTitleTextFn>(fnGetMarkerTitleText);
    g_locaIdToText = reinterpret_cast<LocaIdToTextFn>(fnLocaIdToText);
    g_guidToLocaId = &GuidToLocaId;
    g_checkWidgetButton = reinterpret_cast<CheckWidgetButtonFn>(fnCheckWidgetButton);
    g_centerMapOnPos = reinterpret_cast<CenterMapOnPosFn>(fnCenterMapOnPos);
    g_updateMapViewport = reinterpret_cast<UpdateMapViewportFn>(fnUpdateMapViewport);
    g_dialogInit = reinterpret_cast<DialogInitFn>(fnDialogInit);
    g_textDialog = reinterpret_cast<TextDialogFn>(fnTextDialog);
    g_nineSlice = reinterpret_cast<NineSliceFn>(fnNineSlice);
    g_drawInGameMapPulse = reinterpret_cast<DrawInGameMapPulseFn>(fnDrawInGameMapPulse);
    g_getActiveQuestForMarkerId = reinterpret_cast<GetQuestByMarkerIdFn>(fnGetActiveQuestForMarkerId);
    g_isLoreRumorMarkerId = reinterpret_cast<IsLoreRumorMarkerIdFn>(fnIsLoreRumorMarkerId);
    g_resolveAssetSpec = reinterpret_cast<ResolveAssetSpecFn>(fnResolveAssetSpec);
    g_drawImageSubUv = reinterpret_cast<DrawImageSubUvFn>(fnDrawImageSubUv);
    g_drawWorldMapCanvas = reinterpret_cast<DrawWorldMapCanvasFn>(fnDrawWorldMapCanvas);
    g_getUiDrawList = reinterpret_cast<GetUiDrawListFn>(fnGetUiDrawList);
    g_pushUiClipRect = reinterpret_cast<PushUiClipRectFn>(fnPushUiClipRect);
    g_popUiClipRect = reinterpret_cast<PopUiClipRectFn>(fnPopUiClipRect);
    g_pushUiOpacity = reinterpret_cast<PushUiOpacityFn>(fnPushUiOpacity);
    g_popUiOpacity = reinterpret_cast<PopUiOpacityFn>(fnPopUiOpacity);
    if (fnGetActiveQuestForMarkerId && fnGetActiveQuestForMarkerId[0x36] == 0xE8) {
        int32_t rel = 0;
        ReadCode(fnGetActiveQuestForMarkerId + 0x37, &rel, 4);
        g_getQuestByMarkerId = reinterpret_cast<GetQuestByMarkerIdFn>(fnGetActiveQuestForMarkerId + 0x3B + rel);
    }
    if (!g_nineSlice && g_log) {
        g_log("MapKeyPanel: nine-slice signature not found; the map key draws flat boxes.");
    }
    EnsureFindDialogResolved();

    // drawMapFilterMenu: "mov rax, rsp; push rbp/rbx/rsi/rdi/r12/r14/r15" (13 bytes).
    if (!StartsAsExpected(fnDrawMapFilterMenu, kCanonicalMapFilterPrologue, 13)) {
        if (g_log) g_log("MapKeyPanel: drawMapFilterMenu at %p does not start as expected.", fnDrawMapFilterMenu);
        return false;
    }
    g_origDrawMapFilterMenu = reinterpret_cast<DrawMapFilterMenuFn>(
        InstallHook(fnDrawMapFilterMenu, reinterpret_cast<void*>(&DrawMapFilterMenuHook), 13));
    if (!g_origDrawMapFilterMenu) {
        if (g_log) g_log("MapKeyPanel: drawMapFilterMenu hook failed.");
        return false;
    }
    g_hookTarget = fnDrawMapFilterMenu;

    // getMarkerType: the trampoline carries the hooked 13 bytes with their short jumps rebuilt.
    if (StartsAsExpected(fnGetMarkerType, kCanonicalGetMarkerTypePrologue, sizeof(kCanonicalGetMarkerTypePrologue))) {
        uint8_t code[sizeof(kGetMarkerTypeTrampolineCode)];
        std::memcpy(code, kGetMarkerTypeTrampolineCode, sizeof(code));
        uint8_t* after = fnGetMarkerType + sizeof(kCanonicalGetMarkerTypePrologue);
        std::memcpy(code + 19, &after, sizeof(after));
        g_origGetMarkerType = reinterpret_cast<GetMarkerTypeFn>(
            InstallHook(fnGetMarkerType, reinterpret_cast<void*>(&GetMarkerTypeHook),
                        sizeof(kCanonicalGetMarkerTypePrologue), code, sizeof(code)));
        if (g_origGetMarkerType) g_getMarkerTypeHookTarget = fnGetMarkerType;
    }
    if (!g_getMarkerTypeHookTarget && g_log) {
        g_log("MapKeyPanel: getMarkerType hook failed; gold/silver chests and locked fast travel points stay hidden.");
    }

    // worldMapInput: "mov [rsp+8], rbx; push rbp/rsi/rdi/r12/r13" (12 bytes).
    if (fnWorldMapInputTail) {
        uint8_t* fn = fnWorldMapInputTail - 16;
        if (StartsAsExpected(fn, kCanonicalWorldMapInputPrologue, sizeof(kCanonicalWorldMapInputPrologue))) {
            g_origWorldMapInput = reinterpret_cast<WorldMapInputFn>(
                InstallHook(fn, reinterpret_cast<void*>(&WorldMapInputHook), 12));
            if (g_origWorldMapInput) g_worldMapInputHookTarget = fn;
        }
    }

    g_fogModeFromHotReload = g_kept && g_kept->valid;
    ApplyFogRevealMode((g_kept && g_kept->valid) ? static_cast<FogRevealMode>(g_kept->fogMode) : g_fogRevealMode);

    // doPlayStateUi: "mov r11, rsp; push rbp; lea rbp, [r11-218h]; sub rsp, 310h" (18 bytes).
    if (fnPlayStateUiTail) {
        uint8_t* fn = fnPlayStateUiTail - 18;
        if (StartsAsExpected(fn, kCanonicalPlayStatePrologue, sizeof(kCanonicalPlayStatePrologue))) {
            g_origPlayStateUi = reinterpret_cast<DoPlayStateUiFn>(
                InstallHook(fn, reinterpret_cast<void*>(&PlayStateUiHook), sizeof(kCanonicalPlayStatePrologue)));
            if (g_origPlayStateUi) g_playStateHookTarget = fn;
        }
    }

    // Shared looted chests: the game's chat command sender and its chat-entry append (the receive side).
    // Same functions and prologue length as Embervale Teleport, which hooks the append too; the loader chains both.
    g_sendCommand = reinterpret_cast<SendCommandFn>(FindUnique("4C 8B DC 55 56 57 49 8D AB 18 FF FF FF"));
    if (uint8_t* fnChatAppend = FindUnique("40 55 56 41 54 41 55 41 57 48 83 EC 30 49 8B 41 08 33 ED")) {
        g_origChatAppend = reinterpret_cast<ChatAppendFn>(
            InstallHook(fnChatAppend, reinterpret_cast<void*>(&ChatAppendHook), 13));
    }
    InstallBuiltWaterHooks();
    FindSave2Block();
    chest_glow::Install();
    if (!g_sendCommand || !g_origChatAppend) {
        g_sendCommand = nullptr; // never send what we could not receive
        if (g_log) g_log("Looted chests: chat functions not found; looted chests are not shared in multiplayer");
    } else if (g_log) {
        g_log("Looted chests: shared with other players through the hidden chat channel");
    }

    // buildWorldMapPlayers: "push rbp/rsi/rdi/r12/r13/r15; lea rbp, [rsp-48h]" (15 bytes).
    if (fnBuildWorldMapPlayersTail) {
        uint8_t* fn = fnBuildWorldMapPlayersTail - sizeof(kCanonicalBuildWorldMapPlayersPrologue);
        if (StartsAsExpected(fn, kCanonicalBuildWorldMapPlayersPrologue, sizeof(kCanonicalBuildWorldMapPlayersPrologue))) {
            g_origBuildWorldMapPlayers = reinterpret_cast<BuildWorldMapPlayersFn>(
                InstallHook(fn, reinterpret_cast<void*>(&BuildWorldMapPlayersHook), sizeof(kCanonicalBuildWorldMapPlayersPrologue)));
            if (g_origBuildWorldMapPlayers) g_buildWorldMapPlayersHookTarget = fn;
        }
    }

    // drawWorldMapMarkerIcon: "mov rax, rsp; push rbp/rbx; lea rbp, [rax-1a8h]; sub rsp, 298h" (19 bytes).
    if (fnDrawWorldMapMarkerIconTail) {
        uint8_t* fn = fnDrawWorldMapMarkerIconTail - sizeof(kCanonicalDrawWorldMapMarkerIconPrologue);
        if (StartsAsExpected(fn, kCanonicalDrawWorldMapMarkerIconPrologue, sizeof(kCanonicalDrawWorldMapMarkerIconPrologue))) {
            g_origDrawWorldMapMarkerIcon = reinterpret_cast<DrawWorldMapMarkerIconFn>(
                InstallHook(fn, reinterpret_cast<void*>(&DrawWorldMapMarkerIconHook), sizeof(kCanonicalDrawWorldMapMarkerIconPrologue)));
            if (g_origDrawWorldMapMarkerIcon) g_drawWorldMapMarkerIconHookTarget = fn;
        }
    }

    // drawWorldMapMarkerPlate: "mov [rsp+8], rbx; mov [rsp+20h], rsi; push rdi; sub rsp, 50h" (15 bytes).
    if (fnDrawWorldMapMarkerPlateTail) {
        uint8_t* fn = fnDrawWorldMapMarkerPlateTail - sizeof(kCanonicalDrawWorldMapMarkerPlatePrologue);
        if (StartsAsExpected(fn, kCanonicalDrawWorldMapMarkerPlatePrologue, sizeof(kCanonicalDrawWorldMapMarkerPlatePrologue))) {
            g_origDrawWorldMapMarkerPlate = reinterpret_cast<DrawWorldMapMarkerPlateFn>(
                InstallHook(fn, reinterpret_cast<void*>(&DrawWorldMapMarkerPlateHook), sizeof(kCanonicalDrawWorldMapMarkerPlatePrologue)));
            if (g_origDrawWorldMapMarkerPlate) g_drawWorldMapMarkerPlateHookTarget = fn;
        }
    }

    InstallNativeMapHints();
    InstallXInputHook();
    greeneye::hints::Init(greeneye::Host());
    hall_pan::Install();
    vein_sight::InstallZoneHook();
    vein_surface::Install();
    map_shader::Install();
    // The hovered marker's name (Map Key > Region > Hide drops it for hidden markers); 14 bytes: mov [rsp+10h], rbx;
    // mov [rsp+18h], rdi; push rbp; mov rbp, rsp
    if (fnGetMarkerTitleText) {
        if (auto* orig = reinterpret_cast<GetMarkerTitleTextFn>(
                InstallHook(fnGetMarkerTitleText, reinterpret_cast<void*>(&MarkerTitleTextHook), 14)))
            g_getMarkerTitleText = orig;
    }
    // The marker menu's builder (FUN_140d1dc50: entries, mapState, marker; Hollow Halls Progression adds its Return
    // Beacons through its call): the mod's chest pins lose Remove Marker
    if (uint8_t* fnMenu = FindUnique("48 89 5C 24 10 48 89 6C 24 20 56 57 41 57 48 81 EC 40 01 00 00")) {
        if (auto* orig = reinterpret_cast<MarkerMenuBuildFn>(InstallHook(fnMenu, reinterpret_cast<void*>(&MarkerMenuBuildHook), 14)))
            g_origMarkerMenuBuild = orig;
    }
    if (!g_origMarkerMenuBuild && g_log) g_log("MapKeyPanel: the marker menu's builder was not found; chest pins keep Remove Marker");
    g_hookInstalled = true;
    if (g_log) {
        g_log("MapKeyPanel: hooks installed%s: drawMapFilterMenu=%p worldMapInput=%p updateViewport=%p getMarkerType=%p playStateUi=%p markerIcon=%p",
            greeneye::Host() ? " through the Greeneye Loader" : "",
            g_hookTarget, g_worldMapInputHookTarget, g_updateMapViewport, g_getMarkerTypeHookTarget,
            g_playStateHookTarget, g_drawWorldMapMarkerIconHookTarget);
    }
    return true;
}

// At load (main.cpp): with Vanilla World Map saved on, the patches aren't written at all.
bool VanillaWorldMapInConfig() {
    async_io::Flush(); // the writer's queue on disk before reading
    return GetPrivateProfileIntA("MinimapV2", "VanillaWorldMap", 0, GetMinimapConfigPath().c_str()) != 0;
}

// [General] PotatoPC=true: the game's texture memory stays as the game sets it (main.cpp RaiseMapTextureBudget), for
// graphics cards short of memory. Written as false the first time, so it can be found in the file.
bool PotatoPcInConfig() {
    async_io::Flush(); // the writer's queue on disk before reading
    const std::string cfg = GetMinimapConfigPath();
    // The last PotatoPC= line in [General] counts: the mod writes PotatoPC=false there itself, and Windows reads the
    // first one, so a PotatoPC=true added under it was ignored (TillYouCry, 2026-11-01)
    std::vector<char> section(32768);
    const DWORD n = GetPrivateProfileSectionA("General", section.data(), static_cast<DWORD>(section.size()), cfg.c_str());
    const char* value = nullptr;
    for (const char* line = section.data(); line < section.data() + n && *line; line += std::strlen(line) + 1) {
        while (*line == ' ' || *line == '	') ++line;
        if (_strnicmp(line, "PotatoPC", 8) != 0) continue;
        const char* v = line + 8;
        while (*v == ' ' || *v == '	') ++v;
        if (*v != '=') continue;
        ++v;
        while (*v == ' ' || *v == '	') ++v;
        value = v;
    }
    if (!value) {
        WritePrivateProfileStringA("General", "PotatoPC", "false", cfg.c_str());
        return false;
    }
    return _strnicmp(value, "true", 4) == 0 || value[0] == '1' || _strnicmp(value, "yes", 3) == 0;
}
void SetWorldMapPatchedAtLoad(bool patched) {
    g_worldMapModded = patched;
}

void UseKeptState(KeptState* kept) {
    g_kept = kept;
}

uint64_t g_reloadAskedAt = 0;
bool MapKeyPanelReloadReady() {
    return chest_glow::g_proxyCount == 0 || GetTickCount64() - g_reloadAskedAt > 2000ULL;
}
void ResumeMapKeyPanelAfterFailedReload() { InterlockedExchange(&chest_glow::g_dropProxies, 0); }

void PrepareMapKeyPanelForReload() {
    InterlockedExchange(&chest_glow::g_dropProxies, 1); // G G's creature copies go with this generation
    g_reloadAskedAt = GetTickCount64();
    RemoveMapKeyWheelHook();
    desk::g_edgeStopping = true;
    desk::HideGameEdge(false); // the game's map edge back
    vein_surface::Stop();
    g_padKeyFocus = false; // the game reads the pad again
    hall_pan::Off(); // the stubs stay in the game; the next generation turns them on again
    // A mod icon's click menu open: its stand-in marker is this generation's memory, gone after the reload
    __try {
        if (g_lastMapState && *reinterpret_cast<uint8_t**>(g_lastMapState + 0x542e8) == g_modIconSlot)
            ClearMapViewClickLatch(g_lastMapState); // (the menu closed: +0xf8 and +0x542e8 both cleared)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    // The search box typed in when the reload comes: the game's text input flag goes off with it (left on, the game
    // ignored Escape everywhere until a restart)
    if (g_keySearchFocused) {
        g_keySearchFocused = false;
        ClearTextInputFlag();
    }
    SetMinimapMouseControl(false, nullptr);
    ReleaseTownPortalSource();
    if (g_findDialogOpen) {
        CloseNativeFindDialog();
    }
}

} // namespace unlock_map_markers

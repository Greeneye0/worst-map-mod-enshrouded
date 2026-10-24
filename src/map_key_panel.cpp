#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <utility>
#include <atomic>
#include <mutex>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "map_key_panel.h"
#include "map_control.h"
#include "player_markers.h"
#include "chest_spawns.h"
#include "ore_deposits.h"
#include "minimap_art.h"
#include "scan.h"
#include "greeneye.h"
#include "lang.h"

namespace unlock_map_markers {
namespace {

constexpr uint32_t kGoldChestTypeHash = 0x601dcbe5u;
constexpr uint32_t kSilverChestTypeHash = 0x511fcbe5u;
constexpr uint32_t kOtherPlayerTypeHash = 0x26d18a06u;
constexpr uint32_t kCompletedQuestTypeHash = 0x9c4e7b12u;
constexpr uint32_t kLockedFastTravelTypeHash = 0x1095684fu;
constexpr uint32_t kChestMarkerUidPrefix = 0x70000000u;

// Mining: one made-up marker type per ore. The game never sees these; the mod draws the deposits itself
// (7452 of them, far more than the marker arrays the chests borrow can hold).
constexpr uint32_t kOreTypeHashBase = 0x0e5a0000u;
constexpr size_t kOreCount = static_cast<size_t>(Ore::Count);
constexpr uint8_t kMiningCategory = 5;    // ores
constexpr uint8_t kMaterialsCategory = 6; // farm soil, bones, tar, archaic essence...
constexpr uint8_t kPlantsCategory = 7;    // harvestable plants
constexpr uint8_t kBaitCategory = 8;      // fishing bait critters
constexpr uint8_t kTreesCategory = 9;     // tree areas (coloured, no icons)
bool IsResourceCategory(uint8_t c) { return c >= kMiningCategory && c <= kTreesCategory; }
static_assert(sizeof(kOreCategory) / sizeof(kOreCategory[0]) == static_cast<size_t>(Ore::Count), "one category per row");
static_assert(sizeof(kOreAreaColors) / sizeof(kOreAreaColors[0]) == static_cast<size_t>(Ore::Count), "one colour per row");
static_assert(sizeof(kOreLabels) / sizeof(kOreLabels[0]) == kOreCount, "one label per ore");
static_assert(sizeof(kOreIcons) / sizeof(kOreIcons[0]) == kOreCount, "one icon per ore");
static_assert(sizeof(kOreNameTags) / sizeof(kOreNameTags[0]) == kOreCount, "one name per ore");
bool IsOreHash(uint32_t h) { return h >= kOreTypeHashBase && h < kOreTypeHashBase + kOreCount; }
size_t OreIndexOf(uint32_t h) { return h - kOreTypeHashBase; }
uint32_t OreHash(size_t ore) { return kOreTypeHashBase + static_cast<uint32_t>(ore); }
bool g_oreEnabled[kOreCount]{};
const char* OreName(size_t ore);

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
std::unordered_map<const uint8_t*, uint32_t> g_savedSlotUid;

bool IsSlotSaved(const uint8_t* m) {
    auto it = g_savedSlotUid.find(m);
    return it != g_savedSlotUid.end() && it->second == *reinterpret_cast<const uint32_t*>(m);
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
bool g_otherPlayersEnabled = true;
bool g_completedQuestsEnabled = false;
bool g_minimapShowTerrain = true;
bool g_minimapThinBorder = false; // a thin line instead of the ornate frame
bool g_minimapInfoBar = true;     // the tray under the minimap (time, weather, zoom); off puts the cog on the map
bool g_minimapVisible = true;
bool g_minimapClockWeatherMode = false; // false = Sun/Moon, true = Weather Status
int g_lootedChestMode = 0;              // 0 = Hide, 1 = Greyed Out
int g_customMarkerScaleIdx = 2;         // Default 1.0x
PinRevealMode g_pinRevealMode = PinRevealMode::RevealAll;
bool g_settingsOpen = false;
bool g_minimapSettingsOpen = false;
bool g_bridgeRefreshMap = false;
bool g_bridgeRecenterMap = false;
float g_chestRespawnMinutes = 30.0f;
float g_chestResetRadiusMeters = 500.0f; // a player this close restarts a looted chest's respawn timer

struct ChestLootState {
    bool looted;
    double elapsedOutOfBuildZoneSec;
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
    0x48, 0x8D, 0xA8, 0x98, 0xF8, 0xFF, 0xFF        // lea rbp, [rax - 0x768]
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
uint8_t* g_fogColorSite = nullptr;
uint8_t* g_fogAlphaSite = nullptr;

void ApplyFogRevealMode(FogRevealMode mode) {
    g_fogRevealMode = mode;
    if (g_kept) {
        g_kept->fogMode = static_cast<int>(mode);
        g_kept->valid = 1;
    }
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
    if (typeHash == kLockedFastTravelTypeHash) {
        return reinterpret_cast<uint8_t*>(&g_lockedFastTravelMarkerType);
    }
    return nullptr;
}

// Our marker types, every other one from the game (or a mod that hooked the lookup before us).
uint8_t* GetMarkerTypeHook(uint32_t typeHash, void* mgr) {
    {
        greeneye::Lease lease; // an older generation after a hot reload only calls on
        if (lease) {
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

constexpr size_t kPrimitivesOffset = 0xd250;
constexpr size_t kCirclePrimitive = kPrimitivesOffset + 0x00;
constexpr size_t kCircleOutlinePrimitive = kPrimitivesOffset + 0x10;
constexpr size_t kSoftGlowPrimitive = kPrimitivesOffset + 0x20;
constexpr size_t kMapPulseColorOffset = 0xd4c0;
constexpr size_t kMapPulseRingPrimitive = 0xecc0;
constexpr size_t kCheckmarkPrimitive = 0xed80;
// Nine-slice boxes of the game's FbUiPrimitives (the boxes vanilla menus and dialogs are made of).
constexpr size_t kBoxInnerGlowPrimitive = kPrimitivesOffset + 0x120;
constexpr size_t kGlowBoxSmallPrimitive = kPrimitivesOffset + 0x140;
constexpr size_t kRoundedBoxPrimitive = kPrimitivesOffset + 0x180;
constexpr size_t kSmallTextStyleOffset = 0x10a0c;
// FbUiBundle.namedStyles (0x10a0c) .h3 (+0x318): the game's bold 24 px heading, the base of the deposit title
constexpr size_t kH3StyleOffset = 0x10a0c + 0x318;
// FbUiBundle.namedStyles .map (+0x1340) .locationName (+0x58): the map's region names (uppercase, outlined)
constexpr size_t kMapLocationNameStyleOffset = 0x10a0c + 0x1340 + 0x58;
constexpr size_t kUiCharacterStyleSize = 0x58;
constexpr size_t kStyleFontSize = 0x20; // UiCharacterStyle.fontSize
constexpr size_t kHintListOffset = 0x32e18;
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
};

constexpr MarkerRowSpec kCategory1Rows[] = {
    {0xa17072beu, 0u, 1, "Ancient Spire", umm_lang::Str::AncientSpire},
    {0xa447cba3u, 0x6a56c5b3u, 1, "Flame Altar (Home)", umm_lang::Str::FlameAltarHome},
    {0x7b4c9e16u, 0u, 1, "Flame Sanctum", umm_lang::Str::FlameSanctum},
    {0x6df85109u, 0u, 1, "Night Sanctum", umm_lang::Str::NightSanctum},
    {0xf455a765u, 0u, 1, "Gem Forge", umm_lang::Str::GemForge},
    {0xad6a9d9fu, 0u, 1, "Ancient Obelisk", umm_lang::Str::AncientObelisk},
    {0x4293eae5u, 0u, 1, "Water Source", umm_lang::Str::WaterSource},
    {0x1095684eu, 0u, 1, "Fast Travel Point", umm_lang::Str::FastTravelPoint},
};

constexpr MarkerRowSpec kCategory2Rows[] = {
    {0xffe5e444u, 0u, 2, "Elixir Well / Root", umm_lang::Str::ElixirWell},
    {0xb824ea45u, 0u, 2, "Catacombs / Tomb", umm_lang::Str::CatacombsTomb},
    {0x549e3dbau, 0u, 2, "Ancient Vault", umm_lang::Str::AncientVault},
    {0x2756cf65u, 0u, 2, "Shroud Lair", umm_lang::Str::ShroudLair},
    {0x4ece72ccu, 0u, 2, "Hollow Halls", umm_lang::Str::HollowHalls},
    {0x41a86870u, 0u, 2, "Hidden Tomb", umm_lang::Str::HiddenTomb},
};

constexpr MarkerRowSpec kCategory3Rows[] = {
    {kGoldChestTypeHash, 0u, 3, "Gold Chest", umm_lang::Str::GoldChest},
    {kSilverChestTypeHash, 0u, 3, "Silver Chest", umm_lang::Str::SilverChest},
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
    {kCompletedQuestTypeHash, 0u, 0, "Completed Quests", umm_lang::Str::CompletedQuests},
    {0x8ba7c122u, 0u, 0, "Lore Rumor (?)", umm_lang::Str::LoreRumor},
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
constexpr auto kPlantsRows = MakeResourceRows<kPlantsFirst, kPlantsCategory, umm_lang::Str::CatPlants>(
    std::make_index_sequence<kPlantsCount>{});
constexpr auto kBaitRows = MakeResourceRows<kBaitFirst, kBaitCategory, umm_lang::Str::CatBait>(
    std::make_index_sequence<kBaitCount>{});
constexpr auto kTreesRows = MakeResourceRows<kTreesFirst, kTreesCategory, umm_lang::Str::CatTrees>(
    std::make_index_sequence<kTreesCount>{});
static_assert(kMiningCount + kMaterialsCount + kPlantsCount + kBaitCount + kTreesCount == kOreCount, "every row in one category");

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
};

bool g_panelCollapsed = false;
float g_mapKeyScroll = 0.0f; // how far the Map Key's list is scrolled (UI units)
bool g_mapKeyDragging = false;
float g_mapKeyDragStartMouseY = 0.0f;
float g_mapKeyDragStartScroll = 0.0f;

// Mouse wheel over the Map Key scrolls it. A WH_GETMESSAGE hook on the game's window thread takes the wheel
// messages while the cursor is over the panel and the World Map is up (the map's own zoom is blocked there).
HHOOK g_mapKeyWheelHook = nullptr;
volatile LONG g_mapKeyWheelDelta = 0;
volatile bool g_mapKeyWheelCapture = false;
volatile uint64_t g_mapKeyLastFrameTick = 0;

LRESULT CALLBACK MapKeyWheelMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE && lParam) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == WM_MOUSEWHEEL && g_mapKeyWheelCapture && GetTickCount64() - g_mapKeyLastFrameTick < 250) {
            InterlockedExchangeAdd(&g_mapKeyWheelDelta, GET_WHEEL_DELTA_WPARAM(msg->wParam));
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
bool g_categoryCollapsed[kCategoryGroupCount]{};
void DrawCategoryBarAbs(void* widgetRaw, float x, float y, float w, float h, bool hovered);
bool g_leftWasDown = false;
bool g_toggleKeyWasDown = false;

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
void DrawSolidRectAbs(void* parentWidgetRaw, float absX, float absY, float w, float h, uint32_t rgba, float alpha, float brightness = 1.0f) {
    if (!parentWidgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
    float rect[4] = {absX, absY, w, h};
    uint32_t zeroImage = 0; // Image ID 0 is the engine's built-in solid white quad texture
    uint64_t packedCol = MakePackedColor(rgba, alpha, brightness);
    g_drawImage(parentWidgetRaw, rect, &zeroImage, &packedCol);
}

// Draws an asset primitive (e.g. circle) at absolute UI coordinates (absX, absY, w, h)
// without creating or shifting any child widgets.
void DrawAssetRectAbs(void* parentWidgetRaw, size_t assetOffset, float absX, float absY, float w, float h, uint32_t rgba, float alpha, float brightness = 1.0f) {
    uint8_t* assets = GetUiAssets();
    if (!assets || !parentWidgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
    float rect[4] = {absX, absY, w, h};
    uint64_t packedCol = MakePackedColor(rgba, alpha, brightness);
    alignas(16) uint8_t tmp[16]{};
    g_drawImage(parentWidgetRaw, rect, g_image(g_uiCtx(), tmp, assets + assetOffset), &packedCol);
}

// Draws a 16-byte UiImageReference icon at absolute UI coordinates (absX, absY, w, h)
// without creating or shifting any child widgets.
void DrawIconRectAbs(void* parentWidgetRaw, const void* iconRef16, float absX, float absY, float w, float h, uint32_t rgba, float alpha, float brightness = 1.0f) {
    if (!parentWidgetRaw || !iconRef16 || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
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
    if (!assets || !parentWidgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
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
                  const uint8_t* style = nullptr) {
    uint8_t* assets = GetUiAssets();
    if (!assets || !utf8 || !*utf8) return;
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
                 TextAlignH::Center, TextAlignV::Center);
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

bool IsFilterEnabled(FilterEntry* entries, size_t count, uint32_t typeHash) {
    if (typeHash == kGoldChestTypeHash) return g_goldChestEnabled;
    if (typeHash == kSilverChestTypeHash) return g_silverChestEnabled;
    if (typeHash == kOtherPlayerTypeHash) return g_otherPlayersEnabled;
    if (typeHash == kCompletedQuestTypeHash) return g_completedQuestsEnabled;
    if (IsOreHash(typeHash)) return g_oreEnabled[OreIndexOf(typeHash)];
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].typeHash == typeHash) {
            return entries[i].enabled != 0;
        }
    }
    return true;
}

void ApplyFilterChange(uint8_t* client, uint32_t typeHash, bool enable) {
    if (typeHash == kGoldChestTypeHash || typeHash == kSilverChestTypeHash ||
        typeHash == kOtherPlayerTypeHash || typeHash == kCompletedQuestTypeHash || IsOreHash(typeHash)) {
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
    } else if (typeHash == kOtherPlayerTypeHash) {
        g_otherPlayersEnabled = enable;
        SaveMinimapConfig();
    } else if (typeHash == kCompletedQuestTypeHash) {
        g_completedQuestsEnabled = enable;
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
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x56320) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xe1dc) = 1;
    }
}

void SetCategoryFilterState(uint8_t* client, uint8_t* mapState, uint8_t category, bool enable) {
    if (!client) return;
    if (category == 3) {
        g_goldChestEnabled = enable;
        g_silverChestEnabled = enable;
    } else if (category == 0) {
        g_otherPlayersEnabled = enable;
        g_completedQuestsEnabled = enable;
        SaveMinimapConfig();
    } else if (IsResourceCategory(category)) {
        for (size_t o = 0; o < kOreCount; ++o) {
            if (kOreCategory[o] == category) g_oreEnabled[o] = enable;
        }
        SaveMinimapConfig();
    }
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].category == category) {
            entries[i].enabled = enable ? 1 : 0;
            ApplyFilterChange(client, entries[i].typeHash, enable);
        }
    }
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x56320) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xe1dc) = 1;
    }
}

void SetAllFiltersState(uint8_t* client, uint8_t* mapState, bool enable) {
    if (!client) return;
    g_goldChestEnabled = enable;
    g_silverChestEnabled = enable;
    g_otherPlayersEnabled = enable;
    g_completedQuestsEnabled = enable;
    for (bool& on : g_oreEnabled) on = enable;
    SaveMinimapConfig();
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    for (size_t i = 0; i < count; ++i) {
        entries[i].enabled = enable ? 1 : 0;
        ApplyFilterChange(client, entries[i].typeHash, enable);
    }
    if (mapState) {
        *reinterpret_cast<uint64_t*>(mapState + 0x56320) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xe1dc) = 1;
    }
}

bool IsCategoryEnabled(FilterEntry* entries, size_t count, uint8_t category) {
    if (category == 3 && (g_goldChestEnabled || g_silverChestEnabled)) {
        return true;
    }
    if (category == 0 && (g_otherPlayersEnabled || g_completedQuestsEnabled)) {
        return true;
    }
    if (IsResourceCategory(category)) {
        for (size_t o = 0; o < kOreCount; ++o) {
            if (kOreCategory[o] == category && g_oreEnabled[o]) return true;
        }
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].category == category && entries[i].enabled != 0) {
            return true;
        }
    }
    return false;
}

const void* GetIconRefForType(uint8_t* client, uint32_t typeHash) {
    if (typeHash == kGoldChestTypeHash) return g_goldChestMarkerType.icon;
    if (typeHash == kSilverChestTypeHash) return g_silverChestMarkerType.icon;
    if (IsOreHash(typeHash)) return kOreIcons[OreIndexOf(typeHash)];
    if (typeHash == 0x26d18a06u) {
        if (uint8_t* assets = GetUiAssets()) {
            const auto* bytes = reinterpret_cast<const uint64_t*>(assets + 0xdf60);
            if (bytes[0] != 0 || bytes[1] != 0) return assets + 0xdf60;
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

// The chest titles shown on the map, in the current language. Marker slots keep pointers to these
// texts, so they live in fixed buffers that are rewritten in place when the language is decided.
const UiText& GetChestSubtypeTitle(uint16_t subtype) {
    using umm_lang::Str;
    static char buffers[6][128]{};
    static UiText titles[6]{};
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
        for (int i = 0; i < 6; ++i) titles[i] = MakeUiText(buffers[i]);
        builtFor = lang;
    }
    return titles[subtype < 6 ? subtype : 0];
}

bool IsChestCurrentlyCleared(size_t chestIdx);
uint32_t GetMarkerOrigTypeHash(const uint8_t* m);
bool IsPositionExploredByCharacter(uint8_t* client, int32_t wx, int32_t wz);

void WriteChestMarkerSlot(uint8_t* m, size_t chestIdx) {
    std::memset(m, 0, 0xA0);
    const ChestSpawn& sp = kChestSpawns[chestIdx];
    bool isGold = (chestIdx < kGoldChestCount);
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
        if (*reinterpret_cast<uint8_t**>(clientWorld + 0x3da8) == client ||
            *reinterpret_cast<uint8_t**>(clientWorld + 0x3d98) == client) {
            if (uint8_t* baseReg = *reinterpret_cast<uint8_t**>(clientWorld + 0x3eb8)) {
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
uint64_t g_lootLastMatchTick = 0;
uint8_t g_lootPrevScreen = 0;
size_t g_lootPendingChest = kTotalChestCount; // chest the interact key was pressed at
uint64_t g_lootPendingTick = 0;
int g_interactVk = 'E';

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

void SaveLootedChests() {
    g_lootSaveDirty = false;
    g_lootLastSaveTick = GetTickCount64();
    if (g_lootSection.empty()) return;
    const std::string path = GetLootedChestsPath();
    std::string altars, looted;
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
    WritePrivateProfileStringA(g_lootSection.c_str(), "Altars", altars.c_str(), path.c_str());
    WritePrivateProfileStringA(g_lootSection.c_str(), "Looted", looted.c_str(), path.c_str());
}

// Finds this world's saved record (or starts one) and merges its looted chests into the current state.
void MatchLootWorld(const std::vector<std::pair<int32_t, int32_t>>& altars) {
    const std::string path = GetLootedChestsPath();
    std::vector<char> names(64 * 1024);
    DWORD len = GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), path.c_str());
    for (const char* name = names.data(); len && *name; name += std::strlen(name) + 1) {
        std::vector<char> text(64 * 1024);
        GetPrivateProfileStringA(name, "Altars", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        auto saved = ParseAltars(text.data());
        if (!AltarsShared(saved, altars)) continue;
        g_lootSection = name;
        g_lootAltars = saved;
        GetPrivateProfileStringA(name, "Looted", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
        const char* t = text.data();
        size_t idx = 0;
        int secs = 0, used = 0;
        while (*t && sscanf_s(t, "%zu:%d%n", &idx, &secs, &used) == 2) {
            if (idx < kTotalChestCount && !g_chestLootStates[idx].looted) {
                g_chestLootStates[idx].looted = true;
                g_chestLootStates[idx].elapsedOutOfBuildZoneSec = std::max(0, secs);
            }
            t += used;
            if (*t == ';') ++t;
        }
        break;
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
    float bestDistSq = 18.0f; // 4.24m
    size_t bestChest = kTotalChestCount;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        const ChestSpawn& sp = kChestSpawns[c];
        float cx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        float cy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
        float cz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        if (std::abs(cy - py) > 4.5f) continue;
        float d2 = (cx - px) * (cx - px) + (cz - pz) * (cz - pz);
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
    const int len = std::snprintf(line, sizeof(line), "/ummc1 %c %u %u", looted ? 'L' : 'U', sp.px1, sp.pz1);
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
    if (sscanf_s(buf + 7, "%c %u %u", &kind, 1u, &x, &z) != 3 || (kind != 'L' && kind != 'U') || x > 20000 || z > 20000) {
        return 1;
    }
    std::lock_guard<std::mutex> guard(g_chestNetLock);
    if (g_chestNetInbox.size() < 256) {
        g_chestNetInbox.push_back({kind == 'L', static_cast<int32_t>(x), static_cast<int32_t>(z)});
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
        if (lease && SafeReadChestChatLine(store, type, text) == 1) {
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

void UpdateChestLootTracking(uint8_t* client, bool appFocused) {
    if (!client) return;
    uint8_t screen = client[0x1cd8];
    const uint64_t nowTick = GetTickCount64();
    if (screen == 0) {
        // Main menu: the world was left. Save it and start the next world clean.
        if (g_lootPrevScreen != 0) {
            if (!g_lootSection.empty()) SaveLootedChests();
            g_lootSection.clear();
            g_lootAltars.clear();
            for (auto& s : g_chestLootStates) s = ChestLootState{};
            g_chestLootDirty = true;
        }
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

    if (g_lootSection.empty() && nowTick - g_lootLastMatchTick > 2000ULL) {
        g_lootLastMatchTick = nowTick;
        std::vector<std::pair<int32_t, int32_t>> altars;
        CollectAltarPositions(client, altars);
        if (!altars.empty()) MatchLootWorld(altars);
    }

    // 1. A chest counts as looted when the interact key is pressed next to it and the game switches to its
    //    chest screen (21) right after. The key alone, or the inventory (9) opened by itself, marks nothing.
    const bool interactDown = appFocused && screen == 1 && (GetAsyncKeyState(g_interactVk) & 0x8000) != 0;
    if (interactDown && !g_interactKeyWasDown) {
        g_lootPendingChest = kTotalChestCount;
        if (uint8_t* clientWorld = GetClientWorld(client)) {
            int64_t rawX = *reinterpret_cast<int64_t*>(clientWorld + 0x2c08);
            int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c10);
            int64_t rawZ = *reinterpret_cast<int64_t*>(clientWorld + 0x2c18);
            if (rawX != 0 || rawZ != 0) {
                g_lootPendingChest = NearestChestTo(static_cast<float>(static_cast<double>(rawX) * 2.3283064365386963e-10),
                                                    static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10),
                                                    static_cast<float>(static_cast<double>(rawZ) * 2.3283064365386963e-10));
                g_lootPendingTick = nowTick;
            }
        }
    }
    g_interactKeyWasDown = interactDown;
    if (g_lootPendingChest < kTotalChestCount) {
        if (screen == 21 && g_lootPrevScreen == 1) {
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
        } else if (nowTick - g_lootPendingTick > 2000ULL) {
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
        const float resetSq = g_chestResetRadiusMeters * g_chestResetRadiusMeters;
        auto playerNear = [&](size_t c) {
            const ChestSpawn& sp = kChestSpawns[c];
            const float cx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
            const float cz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
            for (size_t i = 0; i < numPlayers; ++i) {
                const float dx = players[i][0] - cx, dz = players[i][1] - cz;
                if (dx * dx + dz * dz <= resetSq) return true;
            }
            return false;
        };
        const double respawnSec = std::max(1.0, static_cast<double>(g_chestRespawnMinutes) * 60.0);
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
    }

    // Save changes within a few seconds, and the respawn timers once a minute
    const uint64_t sinceSave = nowTick - g_lootLastSaveTick;
    if (!g_lootSection.empty() && ((g_lootSaveDirty && sinceSave > 3000ULL) || sinceSave > 60000ULL)) {
        SaveLootedChests();
    }
}

bool ToggleChestClearedNearWorldPos(float wx, float wz, float maxRadiusMeters) {
    float bestDistSq = maxRadiusMeters * maxRadiusMeters;
    size_t bestChest = kTotalChestCount;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        const bool isGold = (c < kGoldChestCount);
        if (isGold && !g_goldChestEnabled) continue;
        if (!isGold && !g_silverChestEnabled) continue;
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
        g_chestLootStates[bestChest].looted = !g_chestLootStates[bestChest].looted;
        g_chestLootStates[bestChest].elapsedOutOfBuildZoneSec = 0.0;
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
        for (size_t c = 0; c < kGoldChestCount; ++c) {
            if (IsChestCurrentlyCleared(c) && !keepLootedAsGrey) continue;
            if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
            activeIndices[activeCount++] = c;
        }
    }
    if (g_silverChestEnabled) {
        for (size_t c = kGoldChestCount; c < kTotalChestCount; ++c) {
            if (IsChestCurrentlyCleared(c) && !keepLootedAsGrey) continue;
            if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
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

bool IsPositionExploredByCharacter(uint8_t* client, int32_t wx, int32_t wz) {
    if (!client) return true;
    // GameClient is at (client - 0x1f0), so GameClient + 0x47d4e0 == client + 0x47d2f0 holds clientWorld;
    // [clientWorld + 0x3d98] == client, and fow is pointed to by [clientWorld + 0x3db0].
    uint8_t* clientWorld = GetClientWorld(client);
    if (!clientWorld) return true;
    if (*reinterpret_cast<uint8_t**>(clientWorld + 0x3d98) != client &&
        *reinterpret_cast<uint8_t**>(clientWorld + 0x3da8) != client) {
        return true;
    }
    uint8_t* fow = *reinterpret_cast<uint8_t**>(clientWorld + 0x3db0);
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

// Returns 0xa7cadc08u (Main Quest) or 0xc52e92e5u (Side Quest) if id08 belongs to a quest, else 0u.
uint32_t GetQuestFilterHashForMarkerId(uint32_t id08, bool activeObjectiveOnly) {
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

// Classifies a 0xafce0c5f marker into active Main Quest (0xa7cadc08u), active Side Quest (0xc52e92e5u),
// Completed Quests (kCompletedQuestTypeHash), Lore Rumor (0x8ba7c122u), or 0u (inactive NPC / hidden).
uint32_t ClassifyQuestOrRumorMarker(uint32_t id08, uint8_t state34) {
    if (id08 == 0u) return 0u;
    if (state34 != 3) {
        uint32_t activeQ = GetQuestFilterHashForMarkerId(id08, true);
        if (activeQ != 0u) return activeQ;
    }
    if (g_isLoreRumorMarkerId && g_isLoreRumorMarkerId(id08)) {
        return 0x8ba7c122u;
    }
    uint32_t anyQ = GetQuestFilterHashForMarkerId(id08, false);
    if (anyQ != 0u) {
        return (state34 == 3) ? kCompletedQuestTypeHash : 0u;
    }
    return g_isLoreRumorMarkerId ? 0u : 0x8ba7c122u;
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

bool IsMarkerExplored(uint8_t* client, const uint8_t* m, uint32_t origHash) {
    if (!client || !m) return true;
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
        g_savedSlotUid[m] = *reinterpret_cast<uint32_t*>(m);
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
            const bool revealedForPinMode = !exploredOnly || IsMarkerExplored(client, m, origHash);

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

        for (uint64_t i = 0; i < arrCount; ++i) {
            uint8_t* m = arrBase + i * 0xA0;
            uint32_t origHash = 0u;
            uint32_t savedId08 = 0u;
            resolveMarkerMeta(m, origHash, savedId08);
            uint32_t tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            uint8_t b48 = m[0x48];
            uint64_t lastChg = *reinterpret_cast<uint64_t*>(m + 0x40);
            const bool revealedForPinMode = !exploredOnly || IsMarkerExplored(client, m, origHash);

            uint32_t subpinQuestHash = (origHash != 0xafce0c5fu && origHash != 0x454e24f2u)
                                           ? GetQuestFilterHashForMarkerId(savedId08, true)
                                           : 0u;
            bool subpinQuestOn = (subpinQuestHash == 0xa7cadc08u)   ? mainQuestFilterOn
                               : (subpinQuestHash == 0xc52e92e5u) ? sideQuestFilterOn
                                                                  : false;
            bool allowSubpinId = (subpinQuestHash == 0u) || subpinQuestOn;

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
                uint32_t effHash = ClassifyQuestOrRumorMarker(savedId08, m[0x34]);
                bool showPin = (effHash == 0u)                     ? false
                             : (effHash == 0xa7cadc08u)            ? mainQuestFilterOn
                             : (effHash == 0xc52e92e5u)            ? sideQuestFilterOn
                             : (effHash == kCompletedQuestTypeHash) ? g_completedQuestsEnabled
                                                                   : rumorFilterOn;
                applyMarkerVisibility(m, origHash, savedId08, showPin && revealedForPinMode);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode && effHash != 0u) {
                    addCountForHash(effHash);
                }
                continue;
            }
            if (origHash == 0x454e24f2u) {
                applyMarkerVisibility(m, origHash, savedId08, mainQuestFilterOn && revealedForPinMode);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) addCountForHash(0xa7cadc08u);
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
                // An active quest objective here shows as the quest (vanilla shows the quest, not the place)
                applyMarkerVisibility(m, origHash, savedId08, townFilterOn && revealedForPinMode && !subpinQuestOn,
                                      subpinQuestOn, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) addCountForHash(subpinQuestHash);
                continue;
            }
            if (origHash == 0x9786a9f3u) {
                applyMarkerVisibility(m, origHash, savedId08, passageFilterOn && revealedForPinMode && !subpinQuestOn,
                                      subpinQuestOn, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (revealedForPinMode) addCountForHash(subpinQuestHash);
                continue;
            }

            if (origHash != 0u) {
                uint32_t poiFilterHash = (origHash == 0x6a56c5b3u) ? 0xa447cba3u : origHash;
                bool poiFilterOn = IsFilterEnabled(filterEntries, filterCount, poiFilterHash);
                // A place with an active quest objective (Helping Hands at a Hidden Tomb) or an unread rumor
                // about a place not yet explored shows the quest / "?" like vanilla, not the place's icon
                // that the unlock patches reveal; the quest even where the map is not explored yet.
                const bool explored = IsMarkerExplored(client, m, origHash);
                const bool subpinRumor = !subpinQuestHash && rumorFilterOn && !explored && m[0x34] != 3 &&
                                         g_isLoreRumorMarkerId && savedId08 != 0u && g_isLoreRumorMarkerId(savedId08);
                if (subpinQuestOn || subpinRumor) {
                    applyMarkerVisibility(m, origHash, savedId08, false, true, true);
                    if (subpinRumor) addCountForHash(0x8ba7c122u);
                } else if (!revealedForPinMode || !poiFilterOn) {
                    applyMarkerVisibility(m, origHash, savedId08, false, false, false);
                } else {
                    applyMarkerVisibility(m, origHash, savedId08, true, false, allowSubpinId);
                }
                tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            }

            arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ tHash ^ *reinterpret_cast<uint32_t*>(m + 0x08)) * 0x100000001b3ULL;
            if (origHash == 0u || !revealedForPinMode) continue;
            uint32_t countHash = (origHash == 0x6a56c5b3u) ? 0xa447cba3u : origHash;
            addCountForHash(countHash);
            addCountForHash(subpinQuestHash);
        }
    }

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
            if (countHash == 0) continue;
            if (exploredOnly && !IsMarkerExplored(client, m, countHash)) continue;
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
    for (size_t c = 0; c < kGoldChestCount; ++c) {
        if (IsChestCurrentlyCleared(c)) continue;
        if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
        ++unlootedGoldCount;
    }
    int unlootedSilverCount = 0;
    for (size_t c = kGoldChestCount; c < kTotalChestCount; ++c) {
        if (IsChestCurrentlyCleared(c)) continue;
        if (exploredOnly && !IsPositionExploredByCharacter(client, kChestSpawns[c].px1, kChestSpawns[c].pz1)) continue;
        ++unlootedSilverCount;
    }

    for (size_t q = 0; q < numQueries; ++q) {
        if (queryHashes[q] == kGoldChestTypeHash) {
            outCounts[q] = unlootedGoldCount;
        } else if (queryHashes[q] == kSilverChestTypeHash) {
            outCounts[q] = unlootedSilverCount;
        } else if (queryHashes[q] == kOtherPlayerTypeHash) {
            outCounts[q] = otherPlayerCount;
        } else if (IsOreHash(queryHashes[q])) {
            outCounts[q] = OreDepositCount(OreIndexOf(queryHashes[q]));
        }
    }

    if (dirty) {
        ++(*reinterpret_cast<uint32_t*>(client + 0x36e9fc));
        if (mapState) {
            *reinterpret_cast<uint64_t*>(mapState + 0x56320) = 0;
            *reinterpret_cast<uint8_t*>(mapState + 0xe1dc) = 1;
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
int g_findClosingFrames = 0;
bool g_waitReleaseAfterFindClose = false;
bool g_panelClickLatch = false;
bool g_panelCurrentlyHovered = false;

// Gamepad or mouse? The Map Key blocks the map's input while the mouse cursor is over it, so a click on the panel
// does not also click the map. With a gamepad the hidden Windows cursor stays wherever it was last; after tabbing
// to the map through the game menu it can sit over the panel, and the block then froze the gamepad's map cursor.
// So the panel only blocks while the mouse is the device in use: the one that moved or clicked last (XInput pads).
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
bool UsingGamepad() {
    static XInputGetStateFn getState = [] {
        HMODULE x = LoadLibraryW(L"xinput1_4.dll");
        if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
        return x ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(x, "XInputGetState")) : nullptr;
    }();
    static DWORD lastPacket[XUSER_MAX_COUNT]{};
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
            if (st.dwPacketNumber != lastPacket[i] && active) padTick = now;
            lastPacket[i] = st.dwPacketNumber;
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

void EnsureFindDialogResolved() {
    if (g_findDllResolved) return;

    auto tryResolveModule = [](HMODULE mod, const wchar_t* modPath) -> bool {
        if (!mod) return false;
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) return false;
        auto* fbase = static_cast<uint8_t*>(mi.lpBaseOfDll);
        size_t fsize = static_cast<size_t>(mi.SizeOfImage);
        if (!fbase || fsize < 0x10000) return false;

        uint8_t* fnCreate = ScanPattern(fbase, fsize, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 70 48 8B F2 48 8B F9");
        uint8_t* fnReset = ScanPattern(fbase, fsize, "48 89 5C 24 10 57 48 81 EC 80 00 00 00 0F 29 74 24 70 48 8B D9 48 8D 81 98 07 00 00");
        uint8_t* fnGetQuery = ScanPattern(fbase, fsize, "48 8B 81 08 06 00 00 41 B8 3F 00 00 00 48 83 F8 40");
        if (!fnCreate || !fnReset || !fnGetQuery) return false;

        uint8_t* fnRunFrame = fnGetQuery + 0x30;
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
        if (fnCloseMid) {
            g_findCloseDialog = reinterpret_cast<FindCloseDialogFn>(fnCloseMid - 0x13);
        }
        g_findSetTextFieldValue = reinterpret_cast<FindSetTextFieldValueFn>(fnSetText);
        if (fnWheelXchg) {
            int32_t disp = 0;
            std::memcpy(&disp, fnWheelXchg + 2, 4);
            g_findWheelDeltaPtr = reinterpret_cast<volatile LONG*>(fnWheelXchg + 6 + disp);
        }
        g_findDllResolved = true;
        if (g_log) {
            g_log("MapKeyPanel: Resolved FindDialog from %ls (create=%p runFrame=%p hitTest=%p cursor=%p wheel=%p)",
                  modPath ? modPath : L"<module>", fnCreate, fnRunFrame, fnHitTest, fnGetCursor,
                  fnWheelXchg ? (fnWheelXchg + 6 + *reinterpret_cast<int32_t*>(fnWheelXchg + 2)) : nullptr);
        }
        return true;
    };

    HMODULE hMods[1024]{};
    DWORD cbNeeded = 0;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        size_t count = cbNeeded / sizeof(HMODULE);
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
            if (isFindCandidate) {
                tryResolveModule(hMods[i], pathBuf);
            }
        }
    }
    if (!g_findDllResolved) {
        if (HMODULE m = GetModuleHandleW(L"find.dll")) {
            tryResolveModule(m, L"find.dll");
        }
    }
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
    std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf), "%s%s  |  %s",
                  umm_lang::FormatMatches(g_findMatchedPlaces.size()).c_str(), hidden, filters);
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

int ComputeNameMatchTier(const std::string& queryLower, const std::string& titleLower, const std::string& typeLower) {
    if (queryLower.empty()) return 0;

    // Split query into whitespace-separated tokens; every token must match in titleLower or typeLower
    size_t pos = 0;
    while (pos < queryLower.size()) {
        while (pos < queryLower.size() && queryLower[pos] == ' ') ++pos;
        if (pos >= queryLower.size()) break;
        size_t end = queryLower.find(' ', pos);
        if (end == std::string::npos) end = queryLower.size();
        std::string tok = queryLower.substr(pos, end - pos);
        if (titleLower.find(tok) == std::string::npos && typeLower.find(tok) == std::string::npos) {
            return -1;
        }
        pos = end;
    }

    if (titleLower == queryLower) return 0;
    if (titleLower.compare(0, queryLower.size(), queryLower) == 0) return 1;

    size_t titleHit = titleLower.find(queryLower);
    if (titleHit != std::string::npos) {
        if (titleHit == 0 || !std::isalnum(static_cast<unsigned char>(titleLower[titleHit - 1]))) {
            return 2; // Word-start match in title
        }
        return 3; // Substring match in title
    }

    if (typeLower == queryLower || typeLower.compare(0, queryLower.size(), queryLower) == 0) {
        return 3;
    }
    return 4; // Multi-token or type-label match
}

void RebuildFindResults(uint8_t* client, uint8_t* mapState) {
    g_findMatchedPlaces.clear();
    g_findDisplayRows.clear();
    g_findFilteredOutCount = 0;
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
                                bool isFilterOn, bool isLocked, const std::string& title, const char* typeLabel) {
        std::string titleLower = ToLowerAscii(title);
        // The type in the current language, plus its English name so English searches work in any language
        std::string typeLower = ToLowerAscii(typeLabel ? typeLabel : "");
        if (const char* english = GetEnglishTypeLabelForHash(filterHash)) {
            if (umm_lang::Current() != umm_lang::Lang::English) typeLower += " | " + ToLowerAscii(english);
        }
        int tier = ComputeNameMatchTier(queryLower, titleLower, typeLower);
        if (tier < 0) return;

        if (!isFilterOn && g_findRespectFilters) {
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
                                      ? ClassifyQuestOrRumorMarker(savedId08, m[0x34])
                                      : NormalizeFilterTypeHash(origHash);
            if (filterHash == 0u) continue;
            bool isFilterOn = IsFilterEnabled(filterEntries, filterCount, filterHash);
            if (!isFilterOn && origHash != 0xafce0c5fu) {
                uint32_t subpinQuestHash = GetQuestFilterHashForMarkerId(savedId08, true);
                if (subpinQuestHash != 0u && IsFilterEnabled(filterEntries, filterCount, subpinQuestHash)) {
                    isFilterOn = true;
                }
            }
            if (g_pinRevealMode == PinRevealMode::ExploredOnly && !IsMarkerExplored(client, m, origHash)) {
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
        bool isGold = (c < kGoldChestCount);
        uint32_t chestHash = isGold ? kGoldChestTypeHash : kSilverChestTypeHash;
        bool isFilterOn = isGold ? g_goldChestEnabled : g_silverChestEnabled;
        if (g_pinRevealMode == PinRevealMode::ExploredOnly && !IsPositionExploredByCharacter(client, sp.px1, sp.pz1)) {
            isFilterOn = false;
        }
        const char* typeLabel = umm_lang::T(isGold ? umm_lang::Str::GoldChest : umm_lang::Str::SilverChest);
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
        const char* name = OreName(dep.ore);
        processCandidate(static_cast<float>(dep.x), static_cast<float>(dep.z), OreHash(dep.ore), OreHash(dep.ore),
                         isFilterOn, false, name, name);
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

void FocusMapOnWorldPositions(uint8_t* client, uint8_t* mapState, const std::vector<size_t>& matchIndices) {
    if (!mapState || matchIndices.empty()) return;

    g_pulsingPlaces.clear();
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;

    for (size_t idx : matchIndices) {
        if (idx >= g_findMatchedPlaces.size()) continue;
        const auto& place = g_findMatchedPlaces[idx];
        // If user searched with Respect Filters OFF and picked a filtered-out place, enable its filter so it shows up
        if (!place.isFilterOn && client) {
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
    size_t* zoomStepPtr = *reinterpret_cast<size_t**>(mapView + 0x248);

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
            float viewW = *reinterpret_cast<float*>(mapView + 0x1d8);
            float viewH = *reinterpret_cast<float*>(mapView + 0x1dc);
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
        *reinterpret_cast<uint8_t*>(mapView + 0x240) = 1;
        *reinterpret_cast<uint8_t*>(mapView + 0x250) = 0;
        *reinterpret_cast<float*>(mapView + 0x258) = 0.0f;
        *reinterpret_cast<float*>(mapView + 0x10) = targetZoom;
        *reinterpret_cast<float*>(mapView + 0x228) = targetZoom;
    }

    *reinterpret_cast<uint64_t*>(mapView + 0x17c) = 0;

    if (g_centerMapOnPos) {
        g_centerMapOnPos(mapView, targetCanvasPos2D, 1);
    }

    // Ensure immediate frame-accurate zoom & center state even when mapState[3] is 0 while dialog closes
    *reinterpret_cast<float*>(mapView + 0x10) = targetZoom;
    *reinterpret_cast<float*>(mapView + 0x228) = targetZoom;
    *reinterpret_cast<float*>(mapView + 0x20) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x24) = targetCanvasPos2D[1];
    // NOTE: mapView + 0x28/+0x2c is the right-click waypoint target pos (0x140c269e1), NOT camera center!
    *reinterpret_cast<uint64_t*>(mapView + 0x28) = 0;
    *reinterpret_cast<float*>(mapView + 0x30) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x34) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x198) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x19c) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x1a0) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1a4) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x188) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x18c) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x1e8) = targetZoom * targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1ec) = targetZoom * targetCanvasPos2D[1];
    uint64_t panTs = *reinterpret_cast<uint64_t*>(mapView + 0x190);
    *reinterpret_cast<uint64_t*>(mapView + 0x08) = (panTs != 0) ? panTs : *reinterpret_cast<uint64_t*>(mapView + 0x00);

    // Clear any latched click state on mapView/mapState so releasing the mouse or pressing Ctrl cannot open the Waypoint menu
    *reinterpret_cast<uint8_t*>(mapState + 0xf8) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x56310) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x48) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x50) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x58) = 0;
    mapView[0x64] = 0;
    mapView[0x65] = 0;
    mapView[0x66] = 0;
    mapView[0x67] = 0;
    mapView[0x68] = 0;
    *reinterpret_cast<uint64_t*>(mapView + 0x148) = 0;
    *reinterpret_cast<uint64_t*>(mapView + 0x150) = 0;

    if (auto* g = static_cast<uint8_t*>(GetUiG())) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                d0[0x140e0] = 0;
                *reinterpret_cast<float*>(d0 + 0x31f20) = targetCanvasPos2D[0];
                *reinterpret_cast<float*>(d0 + 0x31f24) = targetCanvasPos2D[1];
            }
        }
    }
}

bool g_worldMapWasOpen = false;
int g_worldMapOpenCenterFrames = 0;
uint8_t g_screenBeforeMap = 1; // the last screen before the World Map (1 gameplay, 9 inventory / journal)
uint64_t g_lastWorldMapFrameTick = 0;

void ClearMapViewClickLatch(uint8_t* mapState) {
    if (!mapState) return;
    *reinterpret_cast<uint8_t*>(mapState + 0xf8) = 0;
    *reinterpret_cast<uint64_t*>(mapState + 0x56310) = 0;
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
        *reinterpret_cast<uint64_t*>(mv + 0x148) = 0;
        *reinterpret_cast<uint64_t*>(mv + 0x150) = 0;
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

    *reinterpret_cast<uint64_t*>(mapView + 0x17c) = 0;
    if (g_centerMapOnPos) {
        g_centerMapOnPos(mapView, targetCanvasPos2D, 1);
    }

    *reinterpret_cast<float*>(mapView + 0x20) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x24) = targetCanvasPos2D[1];
    // Clear right-click waypoint target pos (mapView + 0x28/+0x2c) so opening the map doesn't select a waypoint on the player
    *reinterpret_cast<uint64_t*>(mapView + 0x28) = 0;
    *reinterpret_cast<float*>(mapView + 0x30) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x34) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x198) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x19c) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x1a0) = targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1a4) = targetCanvasPos2D[1];
    *reinterpret_cast<float*>(mapView + 0x188) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x18c) = 0.0f;
    *reinterpret_cast<float*>(mapView + 0x1e8) = curZoom * targetCanvasPos2D[0];
    *reinterpret_cast<float*>(mapView + 0x1ec) = curZoom * targetCanvasPos2D[1];
    uint64_t panTs = *reinterpret_cast<uint64_t*>(mapView + 0x190);
    *reinterpret_cast<uint64_t*>(mapView + 0x08) = (panTs != 0) ? panTs : *reinterpret_cast<uint64_t*>(mapView + 0x00);

    ClearMapViewClickLatch(mapState);

    if (auto* g = static_cast<uint8_t*>(GetUiG())) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                d0[0x140e0] = 0;
                *reinterpret_cast<float*>(d0 + 0x31f20) = targetCanvasPos2D[0];
                *reinterpret_cast<float*>(d0 + 0x31f24) = targetCanvasPos2D[1];
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
    float originX = *reinterpret_cast<float*>(mapView + 0x1e0) +
                    *reinterpret_cast<float*>(mapView + 0x17c) -
                    *reinterpret_cast<float*>(mapView + 0x1e8);
    float originY = *reinterpret_cast<float*>(mapView + 0x1e4) +
                    *reinterpret_cast<float*>(mapView + 0x180) -
                    *reinterpret_cast<float*>(mapView + 0x1ec);

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

    // Use the game's native World Map marker pulse function (0x140cf63e0, assets+0xecc0 with blend mode 4)
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

// An ore's name: the game's own item name ("Kupfererz") when the mod follows the game's language,
// otherwise (English, or a language set in the ini) the mod's English row name.
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
    if (umm_lang::FollowsGame() && umm_lang::Current() != umm_lang::Lang::English) {
        if (state[ore] == 0 && GetUiClient()) {
            state[ore] = ReadLocaText(GuidToLocaId(kOreNameTags[ore]), names[ore], sizeof(names[ore])) ? 1 : 2;
        }
        if (state[ore] == 1) return names[ore];
    }
    return kOreLabels[ore];
}

// The enabled tree areas on the World Map: each forest as one filled shape, named in the bigger ones.
// Returns how many shapes it drew, so the deposit icons can share one budget with it.
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
    const float originX = *reinterpret_cast<float*>(mapView + 0x1e0) +
                          *reinterpret_cast<float*>(mapView + 0x17c) -
                          *reinterpret_cast<float*>(mapView + 0x1e8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1e4) +
                          *reinterpret_cast<float*>(mapView + 0x180) -
                          *reinterpret_cast<float*>(mapView + 0x1ec);
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
    int drawn = 0;
    if (cellPx < kTreeGridPx) {
        const int gw = static_cast<int>((right - left) / kTreeGridPx) + 1;
        const int gh = static_cast<int>((bottom - top) / kTreeGridPx) + 1;
        static std::vector<uint8_t> grid; // ore index + 1, 0 = no forest
        grid.assign(static_cast<size_t>(gw) * gh, 0);
        for (size_t i = 0; i < kTreeShapeRectCount; ++i) {
            const TreeShapeRect& t = kTreeShapeRects[i];
            if (!g_oreEnabled[t.ore]) continue;
            const float x0 = originX + t.x0 * cellPx, x1 = originX + (t.x1 + 1) * cellPx;
            const float y1 = originY + (mapWorldH - t.z0 * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            const float y0 = originY + (mapWorldH - (t.z1 + 1) * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            if (x1 < left || x0 > right || y1 < top || y0 > bottom) continue;
            const int gx0 = std::max(0, static_cast<int>((x0 - left) / kTreeGridPx));
            const int gx1 = std::min(gw - 1, static_cast<int>((x1 - left) / kTreeGridPx));
            const int gy0 = std::max(0, static_cast<int>((y0 - top) / kTreeGridPx));
            const int gy1 = std::min(gh - 1, static_cast<int>((y1 - top) / kTreeGridPx));
            for (int gy = gy0; gy <= gy1; ++gy)
                std::memset(&grid[static_cast<size_t>(gy) * gw + gx0], t.ore + 1, static_cast<size_t>(gx1 - gx0 + 1));
        }
        for (int gy = 0; gy < gh && drawn < kMaxTreeRectsPerFrame; ++gy) {
            const uint8_t* row = &grid[static_cast<size_t>(gy) * gw];
            for (int gx = 0; gx < gw && drawn < kMaxTreeRectsPerFrame;) {
                const uint8_t v = row[gx];
                int end = gx + 1;
                while (end < gw && row[end] == v) ++end;
                if (v) {
                    DrawSolidRectAbs(widgetRaw, left + gx * kTreeGridPx, top + gy * kTreeGridPx,
                                     (end - gx) * kTreeGridPx, kTreeGridPx, kOreAreaColors[v - 1], 0.34f);
                    ++drawn;
                }
                gx = end;
            }
        }
    } else {
        for (size_t i = 0; i < kTreeShapeRectCount && drawn < kMaxTreeRectsPerFrame; ++i) {
            const TreeShapeRect& t = kTreeShapeRects[i];
            if (!g_oreEnabled[t.ore]) continue;
            const float x0 = originX + t.x0 * cellPx, x1 = originX + (t.x1 + 1) * cellPx;
            const float y1 = originY + (mapWorldH - t.z0 * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            float y0 = originY + (mapWorldH - (t.z1 + 1) * static_cast<float>(kTreeShapeCellMeters)) * zoom;
            if (x1 < left || x0 > right || y1 < top || y0 > bottom) continue;
            y0 = std::max(y0, top);
            if (y1 - y0 < 0.5f) continue;
            DrawSolidRectAbs(widgetRaw, x0, y0, x1 - x0, y1 - y0, kOreAreaColors[t.ore], 0.34f);
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
        DrawLabelAbs(cx - w * 0.5f, cy - h * 0.5f, w, h, name, 0xffffffffu, 0.9f, TextAlignH::Center, TextAlignV::Center,
                     GetUiAssets() ? GetUiAssets() + kMapLocationNameStyleOffset : nullptr);
    }
    return drawn;
}

// The enabled ore deposits on the world map, drawn by the mod (the game's marker arrays are far too small for
// them). Same world -> screen mapping as the Find pulses. Hovering one names it.
void DrawOreDepositsOnMap(void* widgetRaw, const float* rootRect, uint8_t* client, uint8_t* mapState,
                          bool haveCursor, float mx, float my, int maxIcons) {
    if (!widgetRaw || !mapState || !AnyOreEnabled()) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1e0) +
                          *reinterpret_cast<float*>(mapView + 0x17c) -
                          *reinterpret_cast<float*>(mapView + 0x1e8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1e4) +
                          *reinterpret_cast<float*>(mapView + 0x180) -
                          *reinterpret_cast<float*>(mapView + 0x1ec);
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
    const int gridW = std::max(1, static_cast<int>((right - left) / cell) + 1);
    const int gridH = std::max(1, static_cast<int>((bottom - top) / cell) + 1);
    static std::vector<uint8_t> taken;
    taken.assign(static_cast<size_t>(gridW) * gridH, 0);
    const bool backing = size > 14.0f;
    int drawn = 0;
    static_assert(kOreDepositCount % 7919 != 0, "7919 must not divide the deposit count");
    for (size_t i = 0; i < kOreDepositCount && drawn < maxIcons; ++i) {
        const size_t d = (i * 7919) % kOreDepositCount;
        const OreDeposit& dep = kOreDeposits[d];
        if (!g_oreEnabled[dep.ore] || kOreAreaColors[dep.ore] != 0) continue;
        const float sx = originX + dep.x * zoom;
        const float sy = originY + (mapWorldH - dep.z) * zoom;
        if (sx < left - size || sx > right + size || sy < top - size || sy > bottom + size) continue;
        const bool unexplored = g_pinRevealMode != PinRevealMode::RevealAll &&
                                !IsPositionExploredByCharacter(client, dep.x, dep.z);
        if (unexplored && g_pinRevealMode == PinRevealMode::ExploredOnly) continue;
        {
            const int gx = static_cast<int>((sx - left) / cell), gy = static_cast<int>((sy - top) / cell);
            if (gx >= 0 && gy >= 0 && gx < gridW && gy < gridH) {
                uint8_t& t = taken[static_cast<size_t>(gy) * gridW + gx];
                if (t) continue;
                t = 1;
            }
        }
        ++drawn;
        // Under the menu banner along the top the game's own pins show through dimmed; so do these
        const bool underBanner = sy < top + kMapBannerBottom;
        const float alpha = (unexplored ? 0.55f : 1.0f) * (underBanner ? 0.22f : 1.0f);
        if (backing) {
            DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - half - 2.0f, sy - half - 2.0f, size + 4.0f, size + 4.0f,
                             0xff0a0e10u, 0.55f * alpha);
        }
        DrawIconRectAbs(widgetRaw, kOreIcons[dep.ore], sx - half, sy - half, size, size,
                        unexplored ? 0xff8c9296u : 0xffffffffu, alpha);
        if (!underBanner && haveCursor && std::fabs(mx - sx) <= half + 2.0f && std::fabs(my - sy) <= half + 2.0f) {
            hovered = d;
            hoveredSx = sx;
            hoveredSy = sy;
        }
    }
    if (hovered < kOreDepositCount) {
        // Like the game's own hover title ("Gem Forge"): bold and large, right of the icon, on a soft shadow.
        // A copy of the game's bold h3 style at 28 px (its identifier string dropped: it is a relative pointer).
        static uint8_t titleStyle[kUiCharacterStyleSize]{};
        static uint8_t* titleStyleFor = nullptr;
        uint8_t* assets = GetUiAssets();
        if (!assets) return;
        if (titleStyleFor != assets) {
            std::memcpy(titleStyle, assets + kH3StyleOffset, sizeof(titleStyle));
            std::memset(titleStyle, 0, 8);
            const float titleSize = 28.0f;
            std::memcpy(titleStyle + kStyleFontSize, &titleSize, sizeof(titleSize));
            titleStyleFor = assets;
        }
        const char* name = OreName(kOreDeposits[hovered].ore);
        const float fontScale = 28.0f / 15.0f; // TextWidth is calibrated on the small text
        const float h = 18.0f * fontScale, w = umm_lang::TextWidth(name) * fontScale + 12.0f;
        const float bx = hoveredSx + half + 10.0f, by = hoveredSy - h * 0.5f;
        DrawAssetRectAbs(widgetRaw, kSoftGlowPrimitive, bx - 24.0f, by - h * 0.35f, w + 48.0f, h * 1.7f,
                         0xff000000u, 0.45f);
        DrawLabelAbs(bx, by, w, h, name, 0xffffffffu, 1.0f, TextAlignH::Left, TextAlignV::Center, titleStyle);
    }
}

// What a marker row is called in the panel. Rows whose game name is exactly the row's meaning use
// the game's official translation while the mod follows the game's language; every other row (and
// a language set in the ini, which the game's text would not match) uses the mod's translation.
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

constexpr float kSettingsCardH = 326.0f;

float SettingsSegWidth() {
    using umm_lang::Str;
    using umm_lang::T;
    using umm_lang::TextWidth;
    float w0 = std::max(TextWidth(T(Str::SunMoon)), TextWidth(T(Str::Weather))) + 36.0f;
    float w1 = std::max(TextWidth(T(Str::Hide)), TextWidth(T(Str::GreyedOut))) + 36.0f;
    float w2 = std::max(TextWidth(T(Str::BorderOrnate)), TextWidth(T(Str::BorderThin))) + 36.0f;
    return std::max(118.0f, std::max({w0, w1, w2}));
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
        TextWidth(T(Str::LootedChests)),
        TextWidth(T(Str::MinimapBorder)),
        TextWidth(T(Str::MinimapInfoBar))
    });
    return std::max(CurrentPanelLayout().panelW, std::min(580.0f, std::max(448.0f, 12.0f + maxLbl + 18.0f + 2.0f * segW + 4.0f + 12.0f)));
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

    // Row 2: Show Minimap [ON / OFF]
    {
        bool hovRow = isHov(sx + 8.0f, ry, cardW - 16.0f, kRowH);
        if (hovRow) {
            DrawFramedBoxAbs(widgetRaw, sx + 8.0f, ry, cardW - 16.0f, kRowH, kHoverFill, 0.85f, kGold, 0.7f, 4.0f);
        }
        DrawLabelAbs(sx + 12.0f, ry, cardW - 36.0f - OnOffBadgeWidth(), kRowH, T(Str::ShowMinimap), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);
        DrawOnOffBadgeAbs(widgetRaw, ctrlRight, ry, kRowH, g_minimapVisible);
        if (clicked && hovRow) {
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

    // Row 5: Looted Chests: [Hide] | [Greyed Out]
    {
        const float segH = 22.0f, gap = 4.0f;
        const float btn1X = ctrlRight - segW;
        const float btn0X = btn1X - gap - segW;
        const float by = ry + (kRowH - segH) * 0.5f;

        DrawLabelAbs(sx + 12.0f, ry, btn0X - (sx + 12.0f) - 10.0f, kRowH, T(Str::LootedChests), 0xffd6e6eeu, 0.96f,
                     TextAlignH::Left, TextAlignV::Center);

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
// current step's index at **(mapView+0x248); the zoom is step * mapView+0x58 (0.2). The game fills in 16, 8, 4, 2
// (it leaves out its 0.7, which would make the map narrower than the screen), and a sixth step has no room. So the
// mod keeps the whole list and shows the view five of them at a time: when the view reaches the window's last (or
// first) step and there are more, the window slides one along and the index moves back one, which leaves the
// current zoom unchanged; the next wheel turn goes on to the next step. Every step is 2x like vanilla, down to a
// last one where the whole world fits between the menu banner and the key hints.
constexpr float kMapZoomSteps[] = {16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.42f};
constexpr int kMapZoomStepCount = static_cast<int>(sizeof(kMapZoomSteps) / sizeof(kMapZoomSteps[0]));
constexpr int kMapZoomWindow = 5;
int g_mapZoomWindowStart = 0;

void ApplyMapZoomSteps(uint8_t* mapState) {
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView) return;
    auto* steps = reinterpret_cast<float*>(mapView + 0x40);
    uint64_t* idx = *reinterpret_cast<uint64_t**>(mapView + 0x248);
    if (!idx) return;
    auto windowMatches = [&](int start) {
        for (int i = 0; i < kMapZoomWindow; ++i) {
            if (steps[i] != kMapZoomSteps[start + i]) return false;
        }
        return true;
    };
    if (mapView[0x54] == 4 && steps[0] == kMapZoomSteps[0] && steps[3] == kMapZoomSteps[3]) {
        // The game's own steps (a newly opened map): start at the closest window, which shares its first four
        g_mapZoomWindowStart = 0;
        steps[4] = kMapZoomSteps[4];
        mapView[0x54] = kMapZoomWindow;
    } else if (mapView[0x54] == kMapZoomWindow && steps[0] == kMapZoomSteps[0] && steps[3] == kMapZoomSteps[3] &&
               steps[4] < kMapZoomSteps[3] && !windowMatches(0)) {
        // An earlier build's single extra step after 2: take the view over at the closest window
        g_mapZoomWindowStart = 0;
        steps[4] = kMapZoomSteps[4];
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
    const uint64_t i = *idx;
    if (i == kMapZoomWindow - 1 && g_mapZoomWindowStart + kMapZoomWindow < kMapZoomStepCount) {
        ++g_mapZoomWindowStart;
        *idx = i - 1;
    } else if (i == 0 && g_mapZoomWindowStart > 0) {
        --g_mapZoomWindowStart;
        *idx = 1;
    } else {
        return;
    }
    std::memcpy(steps, kMapZoomSteps + g_mapZoomWindowStart, kMapZoomWindow * sizeof(float));
}

void DrawMapKeyOverlay(void* mapStateVoid) {
    auto* client = GetUiClient();
    auto* mapState = static_cast<uint8_t*>(mapStateVoid);
    if (!client || !mapState) return;

    LoadMinimapConfig();
    ApplyMapZoomSteps(mapState);

    // Detect World Map opening so we always center on the player's actual location when opening the map
    if (g_bridgeRefreshMap) {
        *reinterpret_cast<uint64_t*>(mapState + 0x56320) = 0;
        *reinterpret_cast<uint8_t*>(mapState + 0xe1dc) = 1;
        g_bridgeRefreshMap = false;
    }
    if (g_bridgeRecenterMap) {
        g_worldMapOpenCenterFrames = 2;
        g_bridgeRecenterMap = false;
    }
    uint64_t nowMapTick = GetTickCount64();
    if (!g_worldMapWasOpen || (nowMapTick - g_lastWorldMapFrameTick) > 250ULL) {
        g_worldMapWasOpen = true;
        // Center on the player, except when opened from the journal (F on a quest) or the backpack: the game
        // has already centered the map on what was asked for, so leave it there.
        g_worldMapOpenCenterFrames = (g_screenBeforeMap == 9) ? 0 : 2;
        if (g_log) g_log("World Map opened from screen %u (%s)", g_screenBeforeMap,
                         g_worldMapOpenCenterFrames ? "centering on player" : "keeping the game's view");
    }
    g_lastWorldMapFrameTick = nowMapTick;
    if (g_worldMapOpenCenterFrames > 0) {
        --g_worldMapOpenCenterFrames;
        CenterMapViewOnPlayer(client, mapState);
    }

    HWND foreground = GetForegroundWindow();
    DWORD fgPid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
    bool focused = (fgPid == GetCurrentProcessId());

    UpdateChestLootTracking(client, focused);

    uint32_t allHashes[256]{};
    size_t numHashes = 0;
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (numHashes < sizeof(allHashes) / sizeof(allHashes[0])) allHashes[numHashes++] = grp.rows[r].typeHash;
        }
    }
    int hashCounts[256]{};
    SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);

    auto openFindDialog = [&]() {
        EnsureFindDialogResolved();
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

    bool shiftDown = focused && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    bool fDown = focused && ((GetAsyncKeyState('F') & 0x8000) != 0);
    bool shiftFNow = shiftDown && fDown;
    if (shiftFNow && !g_shiftFWasDown) {
        if (!g_findDialogOpen) {
            openFindDialog();
        } else {
            CloseNativeFindDialog();
        }
    }
    g_shiftFWasDown = shiftFNow;

    bool toggleKeyDown = focused && !g_findDialogOpen && ((GetAsyncKeyState(VK_TAB) & 0x8000) != 0);
    if (toggleKeyDown && !g_toggleKeyWasDown) {
        g_panelCollapsed = !g_panelCollapsed;
    }
    g_toggleKeyWasDown = toggleKeyDown;

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

        auto isHovered = [&](float absX, float absY, float rw, float rh) -> bool {
            return haveCursor && mx >= absX && mx <= absX + rw && my >= absY && my <= absY + rh;
        };

        // Forests and deposits share one budget of extra draws a frame (see DrawTreeAreasOnMap)
        const int treeDraws = DrawTreeAreasOnMap(root.raw, rootRect, mapState);
        DrawOreDepositsOnMap(root.raw, rootRect, client, mapState, haveCursor, mx, my,
                             std::max(500, std::min(1500, 2000 - treeDraws)));

        const PanelLayout& lay = CurrentPanelLayout();
        const float kPanelW = lay.panelW;
        constexpr float kTitleBarH = 30.0f;
        const float kCatRowH = umm_lang::TallScript() ? 24.0f : 23.0f;
        constexpr float kPinRowH = 28.0f;
        constexpr float kScrollBarRoom = 10.0f;
        constexpr float kActionRowH = 30.0f; // SHOW ALL / HIDE ALL, under the title bar (does not scroll)
        const float panelX = rootRect[0] + rootW - kPanelW - 24.0f;
        const float panelY = rootRect[1] + 104.0f;

        auto panelHeight = [&](float itemRowH) {
            float h = kTitleBarH + 6.0f;
            if (!g_panelCollapsed) {
                h += 6.0f + kActionRowH;
                for (size_t c = 0; c < kCategoryGroupCount; ++c) {
                    h += kCatRowH + 2.0f;
                    if (!g_categoryCollapsed[c]) {
                        if (kCategoryGroups[c].category == 4) {
                            h += kPinRowH + 4.0f;
                        } else {
                            h += kCategoryGroups[c].rowCount * itemRowH + 4.0f;
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
        const bool panelHovered = !g_findDialogOpen && !UsingGamepad() &&
                                  (isHovered(panelX, panelY, kPanelW, totalH) || settingsHovered);
        const bool rightDown = focused && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        if (panelHovered && (leftDown || rightDown || clicked)) {
            g_panelClickLatch = true;
            g_findClosingFrames = std::max(g_findClosingFrames, 8);
        }

        // Ctrl + left click on the World Map marks a chest looted, or clears the mark
        {
            const bool ctrlDown = focused && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
            const bool clearClickNow = !g_findDialogOpen && !panelHovered && haveCursor && ctrlDown && leftDown;
            const bool clearClickEdge = clearClickNow && !g_worldMapChestClickWasDown;
            g_worldMapChestClickWasDown = clearClickNow;
            if (clearClickEdge) {
                if (uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8)) {
                    float zoom = *reinterpret_cast<float*>(mapView + 0x10);
                    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
                    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
                    float originX = *reinterpret_cast<float*>(mapView + 0x1e0) +
                                    *reinterpret_cast<float*>(mapView + 0x17c) -
                                    *reinterpret_cast<float*>(mapView + 0x1e8);
                    float originY = *reinterpret_cast<float*>(mapView + 0x1e4) +
                                    *reinterpret_cast<float*>(mapView + 0x180) -
                                    *reinterpret_cast<float*>(mapView + 0x1ec);
                    if (zoom > 0.01f) {
                        float clickWx = (mx - originX) / zoom;
                        float clickWz = mapWorldH - ((my - originY) / zoom);
                        float hitRadiusMeters = std::max(8.0f, 18.0f / zoom);
                        if (ToggleChestClearedNearWorldPos(clickWx, clickWz, hitRadiusMeters)) {
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
        g_mapKeyWheelCapture = panelHovered && !settingsHovered && scrollMax > 0.0f;
        {
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
            if (ClearTextInputFlag() && g_log) g_log("Find on the map: the game's text input came back on after closing; turned it off");
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

            bool hovFindBtn = !g_findDialogOpen && isHovered(panelX + btnFindX, panelY + btnFindY, btnFindW, btnFindH);
            DrawKeyButtonAbs(anchor.raw, panelX + btnFindX, panelY + btnFindY, btnFindW, btnFindH, hovFindBtn, g_findDialogOpen);
            {
                const bool lit = g_findDialogOpen || hovFindBtn;
                const float bx = panelX + btnFindX, by = panelY + btnFindY;
                DrawLabelAbs(bx + 10.0f, by, btnFindW - 20.0f, btnFindH,
                    umm_lang::T(umm_lang::Str::Find), lit ? kActiveText : kText, 0.96f,
                    lay.showShortcut ? TextAlignH::Left : TextAlignH::Center, TextAlignV::Center);
                if (lay.showShortcut) {
                    DrawLabelAbs(bx + 10.0f, by, btnFindW - 20.0f, btnFindH,
                        "Shift+F", lit ? kTitle : 0xff6fa9c2u, lit ? 0.98f : 0.9f, TextAlignH::Right, TextAlignV::Center); // #c2a96f
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
                // SHOW ALL / HIDE ALL
                {
                    const float btnY = kTitleBarH + 4.0f, btnH = kActionRowH - 4.0f;
                    const float btnW = (kPanelW - 22.0f) * 0.5f;
                    const float xs[2] = {8.0f, 14.0f + btnW};
                    const umm_lang::Str labels[2] = {umm_lang::Str::ShowAll, umm_lang::Str::HideAll};
                    for (int i = 0; i < 2; ++i) {
                        const bool hov = !g_findDialogOpen && isHovered(panelX + xs[i], panelY + btnY, btnW, btnH);
                        DrawKeyButtonAbs(anchor.raw, panelX + xs[i], panelY + btnY, btnW, btnH, hov, false);
                        DrawLabelAbs(panelX + xs[i], panelY + btnY, btnW, btnH, umm_lang::T(labels[i]),
                            hov ? kActiveText : kText, hov ? 1.0f : 0.92f, TextAlignH::Center, TextAlignV::Center);
                        if (clicked && hov) {
                            SetAllFiltersState(client, mapState, i == 0);
                            SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                        }
                    }
                }

                auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
                size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);

                // The list scrolls under the title bar; a row is drawn (and clickable) only while fully inside it
                const float contentTop = kTitleBarH + 6.0f + kActionRowH;
                const float contentBottom = totalH - 6.0f;
                const float py = panelY - g_mapKeyScroll; // screen y of content y 0
                auto inView = [&](float y, float h) {
                    return y - g_mapKeyScroll >= contentTop - 0.5f && y + h - g_mapKeyScroll <= contentBottom + 0.5f;
                };
                const float rowRight = scrollMax > 0.0f ? 14.0f + kScrollBarRoom : 14.0f; // badges clear the scroll bar

                float curY = contentTop;
                size_t hashIdx = 0;

                for (size_t c = 0; c < kCategoryGroupCount; ++c) {
                    const auto& grp = kCategoryGroups[c];
                    bool catOn = IsCategoryEnabled(filterEntries, filterCount, grp.category);

                    if (inView(curY, kCatRowH)) {
                        const float barW = kPanelW - 8.0f - (rowRight - 6.0f);
                        bool hovCatCollapse = isHovered(panelX + 8.0f, py + curY, 34.0f, kCatRowH);
                        bool hovCatToggle = isHovered(panelX + 42.0f, py + curY, barW - 34.0f, kCatRowH);
                        const bool hovCat = hovCatCollapse || hovCatToggle;
                        DrawCategoryBarAbs(anchor.raw, panelX + 8.0f, py + curY, barW, kCatRowH, hovCat);

                        // Collapse marker (+ / -) in gold, then the group's title
                        DrawLabelAbs(panelX + 12.0f, py + curY, 22.0f, kCatRowH,
                            g_categoryCollapsed[c] ? "+" : "-", hovCatCollapse ? kActiveText : kGold, 1.0f,
                            TextAlignH::Center, TextAlignV::Center);
                        DrawLabelAbs(panelX + 36.0f, py + curY, kPanelW - 50.0f - rowRight - OnOffBadgeWidth(), kCatRowH,
                            umm_lang::T(grp.title), catOn ? kTitle : 0xff948c88u, 1.0f,
                            TextAlignH::Left, TextAlignV::Center);

                        DrawOnOffBadgeAbs(anchor.raw, panelX + kPanelW - rowRight, py + curY, kCatRowH, catOn);

                        if (clicked && hovCatCollapse) {
                            g_categoryCollapsed[c] = !g_categoryCollapsed[c];
                        } else if (clicked && hovCatToggle) {
                            SetCategoryFilterState(client, mapState, grp.category, !catOn);
                            filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
                        }
                    }

                    curY += kCatRowH + 2.0f;

                    if (g_categoryCollapsed[c]) {
                        hashIdx += grp.rowCount;
                        continue;
                    }

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
                        for (size_t r = 0; r < grp.rowCount; ++r) {
                            const auto& rowSpec = grp.rows[r];
                            int count = hashCounts[hashIdx++];
                            if (!inView(curY, kItemRowH)) {
                                curY += kItemRowH;
                                continue;
                            }
                            bool rowOn = IsFilterEnabled(filterEntries, filterCount, rowSpec.typeHash);
                            const float rowW = kPanelW - 8.0f - (rowRight - 6.0f);
                            bool hovRow = isHovered(panelX + 8.0f, py + curY, rowW, kItemRowH);

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

                            if (IsOreHash(rowSpec.typeHash) && kOreAreaColors[OreIndexOf(rowSpec.typeHash)] != 0) {
                                // Tree areas have no icon: their colour on the map, in the badge
                                const float bx = panelX + 31.0f, by = py + curY + (kItemRowH - 12.0f) * 0.5f;
                                DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx, by, 12.0f, 12.0f,
                                    kOreAreaColors[OreIndexOf(rowSpec.typeHash)], rowOn ? 0.95f : 0.35f);
                            } else if (const void* iconRef = GetIconRefForType(client, rowSpec.typeHash)) {
                                uint32_t iconTint = rowOn ? 0xffffffffu : 0xff707070u;
                                if (rowSpec.typeHash == kGoldChestTypeHash) {
                                    iconTint = rowOn ? 0xff38c8ffu : 0xff406880u;
                                } else if (rowSpec.typeHash == kSilverChestTypeHash) {
                                    iconTint = rowOn ? 0xffe8e4dcu : 0xff707070u;
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
                            uint32_t textCol = rowOn ? 0xffd6e6eeu : kOffText; // #eee6d6 parchment
                            if (rowOn && rowSpec.typeHash == kGoldChestTypeHash) {
                                textCol = 0xff78dcffu;
                            }
                            DrawLabelAbs(panelX + 52.0f, py + curY, kPanelW - 46.0f - rowRight - OnOffBadgeWidth(), kItemRowH,
                                rowText, textCol, rowOn ? 0.96f : 0.75f,
                                TextAlignH::Left, TextAlignV::Center);

                            DrawOnOffBadgeAbs(anchor.raw, panelX + kPanelW - rowRight, py + curY, kItemRowH, rowOn,
                                rowOn ? 1.0f : 0.85f);

                            if (clicked && hovRow) {
                                bool nextState = !rowOn;
                                SetSingleFilterState(client, mapState, rowSpec.typeHash, nextState);
                                if (rowSpec.pairedHash != 0) {
                                    SetSingleFilterState(client, mapState, rowSpec.pairedHash, nextState);
                                }
                            }

                            curY += kItemRowH;
                        }
                        curY += 4.0f;
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
            }
            if (g_settingsOpen && !g_findDialogOpen) {
                if (DrawSettingsCardAbs(anchor.raw, settingsX, settingsY, mx, my, haveCursor, clicked, g_settingsOpen, mapState)) {
                    SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);
                    *reinterpret_cast<uint64_t*>(mapState + 0x56320) = 0;
                    *reinterpret_cast<uint8_t*>(mapState + 0xe1dc) = 1;
                }
            }

            g_endWidget(anchor.raw);
        }

        g_endWidget(root.raw);
    }
    g_endLayer(layer);

    // Run the exact native FindDialog (TextDialog) window used by the example Find mod
    if (g_findDialogOpen && g_mapFindDialog) {
        auto keyPressedEdge = [&](int vk) -> bool {
            if (!focused) return false;
            bool down = ((GetAsyncKeyState(vk) & 0x8000) != 0);
            bool edge = down && !g_findKeysWasDown[vk];
            g_findKeysWasDown[vk] = down;
            return edge;
        };

        bool ctrlDown = focused && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        bool escEdge = keyPressedEdge(VK_ESCAPE);
        // The search field types Tab as a space (a frame or so later); Tab picks from the list instead.
        static ULONGLONG s_tabTick = 0;
        const bool tabEdge = keyPressedEdge(VK_TAB);
        if (tabEdge) s_tabTick = GetTickCount64();

        SyncNativeFindDialogState();
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
            if (keyPressedEdge(VK_F2) || (ctrlDown && keyPressedEdge('F'))) {
                g_findRespectFilters = !g_findRespectFilters;
                forceRebuild = true;
            }

            if (curQuery != g_findQuery || forceRebuild) {
                g_findQuery = curQuery;
                g_findSelectedIndex = -1;
                g_findHoveredIndex = -1;
                g_findScrollOffset = 0;
                RebuildFindResults(client, mapState);
            }

            int totalRows = static_cast<int>(g_findDisplayRows.size());
            bool downEdge = keyPressedEdge(VK_DOWN);
            bool upEdge = keyPressedEdge(VK_UP);
            bool pgDnEdge = keyPressedEdge(VK_NEXT);
            bool pgUpEdge = keyPressedEdge(VK_PRIOR);

            if (totalRows > 0) {
                if (downEdge || (tabEdge && !shiftDown)) {
                    g_findSelectedIndex = (g_findSelectedIndex < 0)
                        ? 0
                        : ((g_findSelectedIndex + 1) % totalRows);
                } else if (upEdge || (tabEdge && shiftDown)) {
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

            if (g_findWheelDeltaPtr) {
                LONG wheelDelta = InterlockedExchange(g_findWheelDeltaPtr, 0);
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
                }
            }

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
                    std::vector<size_t> allIndices(g_findMatchedPlaces.size());
                    for (size_t i = 0; i < g_findMatchedPlaces.size(); ++i) allIndices[i] = i;
                    FocusMapOnWorldPositions(client, mapState, allIndices);
                    CloseNativeFindDialog();
                } else if (g_findRespectFilters && g_findFilteredOutCount > 0) {
                    g_findRespectFilters = false;
                    g_findSelectedIndex = -1;
                    g_findScrollOffset = 0;
                    RebuildFindResults(client, mapState);
                }
            };

            if (clicked && g_findHoveredIndex >= 0 && g_findHoveredIndex < totalRows) {
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

            if (g_findDialogOpen && (keyPressedEdge(VK_RETURN) || dlgResult == 2)) {
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

// True while the Find dialog, its closing frames or our panel own the mouse and keys: the game's
// world map input is skipped for this frame.
bool BlockWorldMapInput(uint8_t* mapState) {
    bool blockMapInput = false;
    __try {
        HWND foreground = GetForegroundWindow();
        DWORD fgPid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
        bool focused = (fgPid == GetCurrentProcessId());
        bool shiftFNow = focused &&
                         ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) &&
                         ((GetAsyncKeyState('F') & 0x8000) != 0);

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

        if (g_findDialogOpen || g_findClosingFrames > 0 || g_waitReleaseAfterFindClose ||
            g_panelClickLatch || shiftFNow || cursorInPanel) {
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
            // Clear any latched click state set inside 0x140d3bf80 before suppressInput was checked
            ClearMapViewClickLatch(mapState);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        blockMapInput = false;
    }
    return blockMapInput && mapState;
}

void WorldMapInputHook(void* mapRootWidget, uint8_t* mapState) {
    {
        greeneye::Lease lease; // released before the game's code runs on
        if (lease && BlockWorldMapInput(mapState)) return;
    }
    if (g_origWorldMapInput) {
        g_origWorldMapInput(mapRootWidget, mapState);
    }
}

void SafeDrawMapKeyOverlay(void* mapState) {
    __try {
        DrawMapKeyOverlay(mapState);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void DrawMapFilterMenuHook(void* mapState) {
    // Note: We intentionally do NOT call g_origDrawMapFilterMenu(mapState) so the vanilla
    // "Filter Map Markers" / "Hidden types: 0" box is cleanly replaced by our Map Key & Filters panel
    // instead of rendering and highlighting behind it. An older generation after a hot reload
    // calls on instead.
    {
        greeneye::Lease lease;
        if (lease) {
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

bool g_minimapConfigLoaded = false;
bool g_minimapCustomPos = false;
float g_minimapPosX = 0.0f;
float g_minimapPosY = 0.0f;
float g_minimapSize = 236.0f;
constexpr float kMinimapMaxSize = 900.0f;
int g_minimapZoomIdx = 2; // Default 1.0x (275m radius)

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
                // Restore gameplay hidden/locked cursor state (index 0)
                winObj[0xa8] = 0;
                if (auto* cursorTable = *reinterpret_cast<uint8_t**>(winObj)) {
                    SetCursor(*reinterpret_cast<HCURSOR*>(cursorTable + 0x290));
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

void LoadMinimapConfig() {
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
    std::string cfg = GetMinimapConfigPath();
    // Show the setting in the file: Language=auto follows the game, or a Steam code such as german
    char language[32]{};
    if (GetPrivateProfileStringA("General", "Language", "", language, sizeof(language), cfg.c_str()) == 0) {
        WritePrivateProfileStringA("General", "Language", "auto", cfg.c_str());
    }
    int customPos = GetPrivateProfileIntA("MinimapV2", "CustomPos", 0, cfg.c_str());
    int posX = GetPrivateProfileIntA("MinimapV2", "PosX", -1, cfg.c_str());
    int posY = GetPrivateProfileIntA("MinimapV2", "PosY", -1, cfg.c_str());
    int sizePx = GetPrivateProfileIntA("MinimapV2", "Size", 236, cfg.c_str());
    int zoomIdx = GetPrivateProfileIntA("MinimapV2", "ZoomIdx", 2, cfg.c_str());
    int otherPlayers = GetPrivateProfileIntA("MinimapV2", "OtherPlayers", 1, cfg.c_str());
    int completedQuests = GetPrivateProfileIntA("MinimapV2", "CompletedQuests", 0, cfg.c_str());
    int showTerrain = GetPrivateProfileIntA("MinimapV2", "ShowTerrain", 1, cfg.c_str());
    int thinBorder = GetPrivateProfileIntA("MinimapV2", "ThinBorder", 0, cfg.c_str());
    int infoBar = GetPrivateProfileIntA("MinimapV2", "InfoBar", 1, cfg.c_str());
    int showMinimap = GetPrivateProfileIntA("MinimapV2", "ShowMinimap", 1, cfg.c_str());
    int clockWeather = GetPrivateProfileIntA("MinimapV2", "ClockWeather", 0, cfg.c_str());
    int lootedChestMode = GetPrivateProfileIntA("MinimapV2", "LootedChestMode", 0, cfg.c_str());
    int markerScaleIdx = GetPrivateProfileIntA("MinimapV2", "CustomMarkerScaleIdx", 2, cfg.c_str());
    int pinRevealMode = GetPrivateProfileIntA("MinimapV2", "PinRevealMode", 2, cfg.c_str());
    int respawnMins = GetPrivateProfileIntA("Chests", "RespawnMinutes", 30, cfg.c_str());
    int resetRadius = GetPrivateProfileIntA("Chests", "ResetRadius", -1, cfg.c_str());
    if (resetRadius < 0) {
        resetRadius = 500;
        WritePrivateProfileStringA("Chests", "ResetRadius", "500", cfg.c_str());
    }
    // [Mining] Ores = one 0/1 per ore, in the Map Key's order
    char ores[512]{};
    GetPrivateProfileStringA("Mining", "Ores", "", ores, sizeof(ores), cfg.c_str());
    // 1.3.3 split Fertilised Farm Soil off Farm Soil, one row after it: an older 86-row string gets that row's bit
    // copied from Farm Soil so every later row keeps its setting.
    if (std::strlen(ores) == kOreCount - 1) {
        constexpr size_t kFertilised = static_cast<size_t>(Ore::FertilisedFarmSoil);
        static_assert(kFertilised == static_cast<size_t>(Ore::FarmSoil) + 1, "Fertilised follows Farm Soil");
        std::memmove(ores + kFertilised + 1, ores + kFertilised, kOreCount - 1 - kFertilised + 1);
        ores[kFertilised] = ores[kFertilised - 1];
    }
    for (size_t o = 0; o < kOreCount; ++o) g_oreEnabled[o] = ores[o] == '1';
    char interactKey[8]{};
    GetPrivateProfileStringA("Chests", "InteractKey", "", interactKey, sizeof(interactKey), cfg.c_str());
    if (!interactKey[0]) {
        interactKey[0] = 'E';
        interactKey[1] = 0;
        WritePrivateProfileStringA("Chests", "InteractKey", interactKey, cfg.c_str());
        WritePrivateProfileStringA("Chests", "RespawnMinutes", "30", cfg.c_str());
    }
    const char keyChar = static_cast<char>(std::toupper(static_cast<unsigned char>(interactKey[0])));
    g_interactVk = (keyChar >= 'A' && keyChar <= 'Z') || (keyChar >= '0' && keyChar <= '9') ? keyChar : 'E';

    g_minimapCustomPos = (customPos != 0 && posX > 10 && posY > 10);
    if (g_minimapCustomPos) {
        g_minimapPosX = static_cast<float>(posX);
        g_minimapPosY = static_cast<float>(posY);
    }
    g_minimapSize = std::max(150.0f, std::min(kMinimapMaxSize, static_cast<float>(sizePx)));
    g_minimapZoomIdx = std::max(0, std::min(kMinimapZoomCount - 1, zoomIdx));
    g_otherPlayersEnabled = (otherPlayers != 0);
    g_completedQuestsEnabled = (completedQuests != 0);
    g_minimapShowTerrain = (showTerrain != 0);
    g_minimapThinBorder = (thinBorder != 0);
    g_minimapInfoBar = (infoBar != 0);
    g_minimapVisible = (showMinimap != 0);
    g_minimapClockWeatherMode = (clockWeather != 0);
    g_lootedChestMode = (lootedChestMode != 0) ? 1 : 0;
    g_customMarkerScaleIdx = std::max(0, std::min(kMarkerScaleCount - 1, markerScaleIdx));
    g_pinRevealMode = static_cast<PinRevealMode>(std::max(0, std::min(2, pinRevealMode)));
    g_chestRespawnMinutes = std::max(1.0f, static_cast<float>(respawnMins));
    g_chestResetRadiusMeters = std::max(0.0f, static_cast<float>(resetRadius));
    ApplyCustomMarkerScale();
}

void SaveMinimapConfig() {
    std::string cfg = GetMinimapConfigPath();
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%d", g_minimapCustomPos ? 1 : 0);
    WritePrivateProfileStringA("MinimapV2", "CustomPos", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_minimapPosX + 0.5f));
    WritePrivateProfileStringA("MinimapV2", "PosX", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_minimapPosY + 0.5f));
    WritePrivateProfileStringA("MinimapV2", "PosY", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_minimapSize + 0.5f));
    WritePrivateProfileStringA("MinimapV2", "Size", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapZoomIdx);
    WritePrivateProfileStringA("MinimapV2", "ZoomIdx", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_otherPlayersEnabled ? 1 : 0);
    WritePrivateProfileStringA("MinimapV2", "OtherPlayers", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_completedQuestsEnabled ? 1 : 0);
    WritePrivateProfileStringA("MinimapV2", "CompletedQuests", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapShowTerrain ? 1 : 0);
    WritePrivateProfileStringA("MinimapV2", "ShowTerrain", buf, cfg.c_str());
    WritePrivateProfileStringA("MinimapV2", "ThinBorder", g_minimapThinBorder ? "1" : "0", cfg.c_str());
    WritePrivateProfileStringA("MinimapV2", "InfoBar", g_minimapInfoBar ? "1" : "0", cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapVisible ? 1 : 0);
    WritePrivateProfileStringA("MinimapV2", "ShowMinimap", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_minimapClockWeatherMode ? 1 : 0);
    WritePrivateProfileStringA("MinimapV2", "ClockWeather", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_lootedChestMode);
    WritePrivateProfileStringA("MinimapV2", "LootedChestMode", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", g_customMarkerScaleIdx);
    WritePrivateProfileStringA("MinimapV2", "CustomMarkerScaleIdx", buf, cfg.c_str());
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(g_pinRevealMode));
    WritePrivateProfileStringA("MinimapV2", "PinRevealMode", buf, cfg.c_str());
    char ores[kOreCount + 1]{};
    for (size_t o = 0; o < kOreCount; ++o) ores[o] = g_oreEnabled[o] ? '1' : '0';
    WritePrivateProfileStringA("Mining", "Ores", ores, cfg.c_str());
}

bool GetHudPlayerWorldPos(uint8_t* client, float& outX, float& outZ) {
    if (!client) return false;
    // 1. Primary live player world position from clientWorld (GetUiG() + 0x47d4e0)
    if (uint8_t* clientWorld = GetClientWorld(client)) {
        int64_t rawX = *reinterpret_cast<int64_t*>(clientWorld + 0x2c08);
        int64_t rawZ = *reinterpret_cast<int64_t*>(clientWorld + 0x2c18);
        if (rawX != 0 || rawZ != 0) {
            float wx = static_cast<float>(static_cast<double>(rawX) * 2.3283064365386963e-10);
            float wz = static_cast<float>(static_cast<double>(rawZ) * 2.3283064365386963e-10);
            if (wx > 100.0f && wx < 12000.0f && wz > 100.0f && wz < 12000.0f) {
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
                    if (px > 100.0f && px < 12000.0f && pz > 100.0f && pz < 12000.0f) {
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
        cpx > 100.0f && cpx < 12000.0f && cpz > 100.0f && cpz < 12000.0f) {
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
        const int64_t* pos = reinterpret_cast<const int64_t*>(clientWorld + 0x2c08); // x, y, z (32.32)
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
void DrawArtZoneStrip(void* widgetRaw, const MinimapArtSprite& frame, const MinimapArtSprite& strip, bool alongX,
                      int zone, float frameX, float frameY, float extraPerZoneX, float extraPerZoneY) {
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
        const float x = alongX ? frameX + a0 + shift : frameX + m0;
        const float y = alongX ? frameY + m0 : frameY + a0 + shift;
        DrawSolidRectAbs(widgetRaw, x, y, alongX ? a1 - a0 : m1 - m0, alongX ? m1 - m0 : a1 - a0,
                         r.color | 0xFF000000u, static_cast<float>(r.color >> 24) / 255.0f);
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
    const float U = kMinimapArtUnit; // the tray, medallion and icons always draw at the design size
    // The tray hangs from the bottom rail: its art (31 units tall) starts just under the rail top
    // (263 units), so its top edge stays hidden behind the rail.
    const bool thinBorder = g_minimapThinBorder;
    const bool infoBar = g_minimapInfoBar;
    const float trayBottomRel = infoBar ? frameSize - 37.0f * artUnit + 0.6f * U + 31.0f * U : frameSize;

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
    const float frameCx = frameX + frameSize * 0.5f;
    const float compassCy = frameY + 30.5f * artUnit;
    const float trayY = frameY + frameSize - 37.0f * artUnit + 0.6f * U;
    const float trayH = 31.0f * U;
    const float trayContentCy = trayY + 17.5f * U;     // middle of the part below the rail
    const float gearCapW = 28.2f * U;                   // tray point to the end of the left flourish
    const float zoomCapW = 46.2f * U;                   // right flourish to the tray point
    const float trayIconW = 22.0f;
    const float trayCentreGap = 10.0f;                  // each side of the tray's centre divider
    // TextWidth is a per-glyph estimate; the tray font runs about a tenth wider, so leave room.
    const float timeTextW = umm_lang::TextWidth(timeText) * 1.12f + 2.0f;
    const float statusTextW = umm_lang::TextWidth(statusText) * 1.12f + 2.0f;
    const float trayHalfW = std::max(gearCapW + 6.0f + trayIconW + timeTextW + trayCentreGap,
                                     zoomCapW + 6.0f + statusTextW + trayCentreGap);
    const float trayBaseW = 219.0f * U;                 // the art's own width, point to point
    const float trayW = std::max(trayHalfW * 2.0f, trayBaseW - 90.0f * U);
    const float trayX = frameCx - trayW * 0.5f;
    const float trayRight = trayX + trayW;
    const float trayButtonsY = frameY + frameSize - 34.0f * artUnit; // below the rail
    const float trayButtonsH = trayY + trayH - trayButtonsY;
    // Without the info bar the cog sits in the map's bottom-left corner.
    constexpr float kCornerGearSz = 24.0f;
    const float gearBtnX = infoBar ? trayX + 2.0f : vx + 4.0f;
    const float gearBtnW = infoBar ? gearCapW - 2.0f : kCornerGearSz;
    const float gearBtnY = infoBar ? trayButtonsY : vy + vh - 4.0f - kCornerGearSz;
    const float gearBtnH = infoBar ? trayButtonsH : kCornerGearSz;
    const float minusBtnX = trayRight - 42.0f * U;
    const float plusBtnX = trayRight - 23.0f * U;
    const float zoomBtnW = 19.0f * U;

    const float mmSettingsW = SettingsCardWidth();
    const float mmSettingsX = (frameX - mmSettingsW - 10.0f >= rootX + 10.0f)
                                  ? (frameX - mmSettingsW - 10.0f)
                                  : std::min(rootX + rootW - mmSettingsW - 10.0f, frameX + frameSize + 10.0f);
    const float mmSettingsY = std::max(rootY + 20.0f, std::min(rootY + rootH - kSettingsCardH - 20.0f, frameY));

    // Hotkeys: '[' (zoom out) and ']' (zoom in), or Numpad '-' / '+'
    if (appFocused && !g_findDialogOpen) {
        bool zoomOutKey = ((GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0) || ((GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0);
        bool zoomInKey  = ((GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0) || ((GetAsyncKeyState(VK_ADD) & 0x8000) != 0);
        if (zoomOutKey && !g_minimapZoomOutKeyWasDown) {
            if (g_minimapZoomIdx > 0) {
                --g_minimapZoomIdx;
                SaveMinimapConfig();
            }
        }
        if (zoomInKey && !g_minimapZoomInKeyWasDown) {
            if (g_minimapZoomIdx + 1 < kMinimapZoomCount) {
                ++g_minimapZoomIdx;
                SaveMinimapConfig();
            }
        }
        g_minimapZoomOutKeyWasDown = zoomOutKey;
        g_minimapZoomInKeyWasDown = zoomInKey;
    } else {
        g_minimapZoomOutKeyWasDown = false;
        g_minimapZoomInKeyWasDown = false;
    }

    // Holding Shift + Alt (or having the Minimap Settings card open) unlocks the mouse cursor
    const bool shiftAltHeld = appFocused &&
                              ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) &&
                              ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    const bool lockCameraForMouse = appFocused &&
                                    (shiftAltHeld || g_minimapSettingsOpen || g_minimapDraggingMove || g_minimapDraggingResize);
    SetMinimapMouseControl(lockCameraForMouse, foreground);

    // Handle mouse wheel zoom while Shift+Alt is held
    LONG wheelDelta = InterlockedExchange(&g_minimapWheelDelta, 0);
    if (shiftAltHeld && wheelDelta != 0) {
        if (wheelDelta > 0 && g_minimapZoomIdx + 1 < kMinimapZoomCount) {
            ++g_minimapZoomIdx;
            SaveMinimapConfig();
        } else if (wheelDelta < 0 && g_minimapZoomIdx > 0) {
            --g_minimapZoomIdx;
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
        if (screen != 1 || shiftAltHeld || g_minimapSettingsOpen || g_minimapDraggingMove || g_minimapDraggingResize) {
            cursorVisible = true;
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

    const float radiusMeters = kMinimapZoomLevels[g_minimapZoomIdx].radiusMeters;
    const float pxPerMeterX = (vw * 0.5f) / radiusMeters;
    const float pxPerMeterY = (vh * 0.5f) / radiusMeters;

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
    const bool hoverResize  = !barOnly && cursorVisible && !hoverSettingsCard && !hoverPlus && !hoverMinus && !hoverGear &&
                              (mouseX >= resizeGripX0 && mouseX <= resizeGripX1 &&
                               mouseY >= resizeGripY0 && mouseY <= resizeGripY1);
    const bool hoverWholeWidget = cursorVisible && !hoverSettingsCard && !hoverMinus && !hoverPlus && !hoverGear &&
                                  ((!barOnly && (thinBorder ? inRect(frameX, frameY, frameSize, frameSize)
                                                            : inRect(frameX + frameSize * 0.06f, frameY + frameSize * 0.04f,
                                                                     frameSize * 0.88f, frameSize * 0.90f))) ||
                                   (infoBar && inRect(trayX, trayButtonsY, trayW, trayButtonsH)));

    if (leftPressed && !hoverSettingsCard) {
        if (hoverMinus) {
            if (g_minimapZoomIdx > 0) {
                --g_minimapZoomIdx;
                SaveMinimapConfig();
            }
        } else if (hoverPlus) {
            if (g_minimapZoomIdx + 1 < kMinimapZoomCount) {
                ++g_minimapZoomIdx;
                SaveMinimapConfig();
            }
        } else if (hoverGear) {
            g_minimapSettingsOpen = !g_minimapSettingsOpen;
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
            if (!barOnly && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) && inRect(vx, vy, vw, vh) && pxPerMeterX > 0.01f) {
                float clickWx = playerX + (mouseX - vcx) / pxPerMeterX;
                float clickWz = playerZ - (mouseY - vcy) / pxPerMeterY;
                float maxRadiusM = std::max(12.0f, 14.0f / pxPerMeterX);
                if (ToggleChestClearedNearWorldPos(clickWx, clickWz, maxRadiusM)) {
                    toggledChest = true;
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
        DrawArtSprite(anchor.raw, kArtTray, trayX - 40.5f * U, trayY - 262.0f * U, 1.0f, trayW - trayBaseW);
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
        if (g_minimapShowTerrain) {
            DrawMinimapTerrain(client, mmCanvas.raw, vx, vy, vw, vh, vcx, vcy, playerX, playerZ, pxPerMeterX, pxPerMeterY);
        }
        g_endWidget(mmCanvas.raw);

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

        uint64_t nowTick = GetTickCount64();
        const bool pulseActive = !g_pulsingPlaces.empty() && (nowTick - g_pulseStartTick <= 14000ULL);

        std::vector<MinimapMarkerDraw> drawList;
        drawList.reserve(96);

        constexpr size_t kMarkerArrays[4] = { 0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78 };
        const float iconPad = 10.0f * btnScale;

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

                const bool isSlotSaved = IsSlotSaved(m);
                uint32_t savedId08 = isSlotSaved ? *reinterpret_cast<uint32_t*>(m + 0x4c)
                                                 : *reinterpret_cast<uint32_t*>(m + 0x08);
                if (savedId08 == 0u) savedId08 = *reinterpret_cast<uint32_t*>(m + 0x08);
                const uint32_t markerState = *reinterpret_cast<uint32_t*>(m + 0x34);

                uint32_t normHash = (origHash == 0xafce0c5fu)
                                        ? ClassifyQuestOrRumorMarker(savedId08, static_cast<uint8_t>(markerState))
                                        : NormalizeFilterTypeHash(origHash);
                if (normHash == 0u) continue;
                if (!IsFilterEnabled(filterEntries, filterCount, normHash)) {
                    uint32_t subpinQuestHash = (origHash != 0xafce0c5fu)
                                                   ? GetQuestFilterHashForMarkerId(savedId08, true)
                                                   : 0u;
                    if (subpinQuestHash != 0u && IsFilterEnabled(filterEntries, filterCount, subpinQuestHash)) {
                        normHash = subpinQuestHash;
                    } else {
                        continue;
                    }
                }

                // Respect PinRevealMode (Explored / Hybrid / Reveal All) for unexplored pins
                const bool isUnexploredPin = (g_pinRevealMode != PinRevealMode::RevealAll) &&
                                             !IsMarkerExplored(client, m, origHash);
                if (isUnexploredPin && g_pinRevealMode == PinRevealMode::ExploredOnly) {
                    continue;
                }

                // In Enshrouded (0x140d1394d: cmp dword ptr [rdi+34h], 3), markerState == 3 is Completed/Cleared!
                const bool isCleared = (markerState == 3u) || (normHash == kCompletedQuestTypeHash);
                if (hideClearedOnBigMap && isCleared && normHash != kCompletedQuestTypeHash) continue;

                int32_t rawMx = *reinterpret_cast<int32_t*>(m + 0x14);
                int32_t rawMz = *reinterpret_cast<int32_t*>(m + 0x24);
                float mx = static_cast<float>(rawMx) +
                           static_cast<float>(*reinterpret_cast<uint16_t*>(m + 0x10)) / 65536.0f;
                float mz = static_cast<float>(rawMz) +
                           static_cast<float>(*reinterpret_cast<uint16_t*>(m + 0x20)) / 65536.0f;
                if (mx <= 1.0f && mz <= 1.0f) continue;

                float dxMeters = mx - playerX;
                float dzMeters = mz - playerZ;
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) {
                    continue;
                }

                float sx = vcx + dxMeters * pxPerMeterX;
                float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                    sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                    continue;
                }

                uint32_t displayHash = (normHash == kCompletedQuestTypeHash)
                                           ? GetQuestFilterHashForMarkerId(savedId08, false)
                                           : normHash;
                if (displayHash == 0u || displayHash == 0x8ba7c122u) {
                    if (normHash == kCompletedQuestTypeHash) displayHash = 0xc52e92e5u;
                }
                if (isSlotSaved && m[0x48] != 0 && (normHash == 0xa17072beu || normHash == 0x1095684eu)) {
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
                if (isUnexploredPin && g_pinRevealMode == PinRevealMode::Hybrid) {
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

                drawList.push_back(MinimapMarkerDraw{sx, sy, iconSize, iconRef, tint, alpha, sortCat, isPulsing, isCleared});
            }
        }

        // 2b. Gold & Silver Chests directly from kChestSpawns (100% flicker-free!)
        if (g_goldChestEnabled || g_silverChestEnabled) {
            const void* goldIconRef = g_goldChestEnabled ? GetIconRefForType(client, kGoldChestTypeHash) : nullptr;
            const void* silverIconRef = g_silverChestEnabled ? GetIconRefForType(client, kSilverChestTypeHash) : nullptr;
            // Nearby chests get an arrow saying whether they're above or below you (cellars, towers, cliffs).
            constexpr float kChestHeightNearMeters = 75.0f, kChestHeightMinMeters = 4.0f;
            float playerY = 0.0f;
            bool havePlayerY = false;
            if (uint8_t* clientWorld = GetClientWorld(client)) {
                const int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c10);
                playerY = static_cast<float>(static_cast<double>(rawY) * 2.3283064365386963e-10);
                havePlayerY = rawY != 0 && std::isfinite(playerY);
            }

            for (size_t c = 0; c < kTotalChestCount; ++c) {
                const bool chestCleared = IsChestCurrentlyCleared(c);
                if (chestCleared && g_lootedChestMode == 0) continue;
                const bool isGold = (c < kGoldChestCount);
                if (isGold && !g_goldChestEnabled) continue;
                if (!isGold && !g_silverChestEnabled) continue;

                const void* iconRef = isGold ? goldIconRef : silverIconRef;
                if (!iconRef) continue;

                const ChestSpawn& sp = kChestSpawns[c];
                float mx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
                float mz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;

                float dxMeters = mx - playerX;
                float dzMeters = mz - playerZ;
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) {
                    continue;
                }

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

                float iconSize = (isGold ? 16.5f : 15.5f) * btnScale;
                uint32_t tint = isGold ? 0xFF38C8FFu : 0xFFE8E4DCu;
                float alpha = 0.96f;
                if (chestCleared || (chestUnexplored && g_pinRevealMode == PinRevealMode::Hybrid)) {
                    tint = isGold ? 0xFF6894ACu : 0xFF909498u;
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
                    if (dy >= kChestHeightMinMeters) heightDir = 1;
                    else if (dy <= -kChestHeightMinMeters) heightDir = -1;
                }

                drawList.push_back(MinimapMarkerDraw{sx, sy, iconSize, iconRef, tint, alpha, 3, isPulsing, chestCleared, heightDir});
            }
        }

        // 2c. Ore deposits (Mining), under everything else
        if (AnyOreEnabled()) {
            for (size_t d = 0; d < kOreDepositCount; ++d) {
                const OreDeposit& dep = kOreDeposits[d];
                if (!g_oreEnabled[dep.ore]) continue;
                const float dxMeters = dep.x - playerX;
                const float dzMeters = dep.z - playerZ;
                if (std::abs(dxMeters) > radiusMeters * 1.04f || std::abs(dzMeters) > radiusMeters * 1.04f) continue;
                const bool unexplored = (g_pinRevealMode != PinRevealMode::RevealAll) &&
                                        !IsPositionExploredByCharacter(client, dep.x, dep.z);
                if (unexplored && g_pinRevealMode == PinRevealMode::ExploredOnly) continue;
                const float sx = vcx + dxMeters * pxPerMeterX;
                const float sy = vcy - dzMeters * pxPerMeterY;
                if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                    sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                    continue;
                }
                drawList.push_back(MinimapMarkerDraw{sx, sy, 13.0f * btnScale, kOreIcons[dep.ore],
                    unexplored ? 0xFF8C9296u : 0xFFFFFFFFu, unexplored ? 0.55f : 0.95f, kMiningCategory, false, false});
            }
        }

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
            if (md.isCleared) {
                // Draw the native completed checkmark (assets + 0xed80) on completed Shroud Roots, Vaults, POIs, or looted chests
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
                // Up: green, at the icon's top-right corner. Down: orange, at its bottom-right corner.
                const bool up = md.heightDir > 0;
                const float ax = md.sx + md.size * 0.5f;
                const float ay = md.sy + (up ? -0.32f : 0.32f) * md.size;
                DrawPlayerTriangle(anchor.raw, ax, ay, 0.0f, up ? 1.0f : -1.0f, 0.62f * btnScale,
                                   up ? 0xFF6CC87Cu : 0xFF3C88E8u, up ? 0xFF9CF0ACu : 0xFF64B4FFu,
                                   std::max(0.6f, md.alpha));
            }
        }

        // Draw other players in multiplayer from client + 0x4642c0 (count at +0x4642c8, stride 0x240)
        if (g_otherPlayersEnabled) {
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

            if (playerArray && playerCount > 0 && playerCount <= 64) {
                const float clampLimit = radiusMeters * 0.91f;
                for (uint64_t i = 0; i < playerCount; ++i) {
                    uint8_t* p = playerArray + i * 0x240;
                    uint32_t pid = *reinterpret_cast<uint32_t*>(p + 0x00);
                    if (pid == 0 || pid == localPlayerId) continue;
                    if (p[0x207] != localHidden) continue;

                    // Live float world coordinates at p + 0x10 {x, y, z} (0x140bd282a, 0x140d557fd)
                    float ox = *reinterpret_cast<float*>(p + 0x10);
                    float oz = *reinterpret_cast<float*>(p + 0x18);
                    if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f) && p[0x68] != 0) {
                        int32_t rx = *reinterpret_cast<int32_t*>(p + 0x54);
                        int32_t rz = *reinterpret_cast<int32_t*>(p + 0x64);
                        ox = static_cast<float>(rx) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x50)) / 65536.0f;
                        oz = static_cast<float>(rz) + static_cast<float>(*reinterpret_cast<uint16_t*>(p + 0x60)) / 65536.0f;
                    }
                    if (!(ox > 100.0f && ox < 12000.0f && oz > 100.0f && oz < 12000.0f)) continue;

                    float dxM = ox - playerX;
                    float dzM = oz - playerZ;
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
                    DrawPlayerTriangle(anchor.raw, osx, osy, marker.dirX, marker.dirZ,
                                       btnScale * (isClamped ? 0.85f : 1.0f),
                                       marker.color, marker.color, pAlpha);

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

        // Draw local Player Marker at exact center (vcx, vcy) as a clean, compact directional triangle
        float camDirX = 0.0f, camDirZ = 1.0f;
        float faceDirX = 0.0f, faceDirZ = 1.0f;
        GetHudPlayerFacingDirs(client, playerX, playerZ, camDirX, camDirZ, faceDirX, faceDirZ);

        DrawPlayerTriangle(anchor.raw, vcx, vcy, faceDirX, faceDirZ, btnScale, 0xFF63A1B8u, 0xFF8CC4DAu, 1.0f);

        if (thinBorder) {
            // Thin border: a dark outline and a fine gold line round the map, and a small N at the top
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
            DrawLabelAbs(frameCx - 15.0f + 1.0f, vy + 1.0f + 1.0f, 30.0f, 18.0f, umm_lang::T(umm_lang::Str::CompassN),
                         0xFF080A0Cu, 0.85f, TextAlignH::Center, TextAlignV::Center);
            DrawLabelAbs(frameCx - 15.0f, vy + 1.0f, 30.0f, 18.0f, umm_lang::T(umm_lang::Str::CompassN),
                         0xFF9AD3E9u, 0.98f, TextAlignH::Center, TextAlignV::Center);
        } else {
            // ------------------------------------------------------------------------
            // 3. Frame art: the tray first (it tucks behind the bottom rail), then the frame and the N
            //    medallion set into the compass rose
            // ------------------------------------------------------------------------
            if (infoBar) DrawArtSprite(anchor.raw, kArtTray, trayX - 40.5f * U, trayY - 262.0f * U, 1.0f, trayW - trayBaseW);
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
            }
            DrawArtSprite(anchor.raw, kArtMedallion, frameCx - 150.0f * U, compassCy - 30.5f * U, 1.0f);
            DrawLabelAbs(frameCx - 15.0f, compassCy - 10.0f, 30.0f, 20.0f, umm_lang::T(umm_lang::Str::CompassN),
                         0xFF9AD3E9u, 0.98f, TextAlignH::Center, TextAlignV::Center);
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
        if (hoverGear || g_minimapSettingsOpen) trayGlow(trayX + 14.4f * U, 15.0f, hoverGear ? 0.45f : 0.3f);
        if (hoverMinus && g_minimapZoomIdx > 0) trayGlow(trayRight - 31.5f * U, 13.0f, 0.4f);
        if (hoverPlus && g_minimapZoomIdx + 1 < kMinimapZoomCount) trayGlow(trayRight - 14.5f * U, 13.0f, 0.4f);

        // Centre divider, under the frame's bottom diamond
        DrawSolidRectAbs(anchor.raw, frameCx - 0.5f, trayContentCy - 8.0f, 1.0f, 16.0f, 0xFF62A8C4u, 0.75f);

        // Left half: icon + time, centred between the gear cap and the divider
        const float leftX0 = trayX + gearCapW + 6.0f;
        const float leftX1 = frameCx - trayCentreGap;
        const float leftStart = (leftX0 + leftX1 - (trayIconW + timeTextW)) * 0.5f;
        int iconIdx = isDay ? 0 : 1;
        if (g_minimapClockWeatherMode && weatherType >= 1 && weatherType <= 4) iconIdx = 1 + weatherType;
        DrawArtSprite(anchor.raw, *kArtIcons[iconIdx], leftStart + trayIconW * 0.5f - 2.0f - 150.0f * U,
                      trayContentCy - 150.0f * U, 1.0f);
        DrawLabelAbs(leftStart + trayIconW, trayContentCy - 12.0f, timeTextW + 6.0f, 24.0f, timeText,
                     kTrayText, 0.97f, TextAlignH::Left, TextAlignV::Center);
        // Right half: the weather (or day phase) word
        const float rightX0 = frameCx + trayCentreGap;
        const float rightX1 = trayRight - zoomCapW - 6.0f;
        DrawLabelAbs(rightX0, trayContentCy - 12.0f, rightX1 - rightX0, 24.0f, statusText,
                     kTrayText, 0.97f, TextAlignH::Center, TextAlignV::Center);
        DrawLabelAbs(minusBtnX, trayContentCy - 12.0f, zoomBtnW, 24.0f, "-",
                     (g_minimapZoomIdx > 0) ? kTrayText : kTrayMuted, 0.98f, TextAlignH::Center, TextAlignV::Center);
        DrawLabelAbs(plusBtnX, trayContentCy - 12.0f, zoomBtnW, 24.0f, "+",
                     (g_minimapZoomIdx + 1 < kMinimapZoomCount) ? kTrayText : kTrayMuted, 0.98f, TextAlignH::Center, TextAlignV::Center);
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
    const bool appFocused = (fgPid == GetCurrentProcessId());

    // Update chest loot detection and 30-game-minute respawn timer (even while inventory/loot UI is open)
    UpdateChestLootTracking(client, appFocused);

    // Screen state check:
    // 1 = Playing (normal HUD), 5 = World Map, 2 = Pause/System Menu, 9 = Inventory/Journal
    // Hide minimap when World Map (5) or System Menu (2) or Main Menu (0) is open, or when hidden in Settings.
    uint8_t screen = client[0x1cd8];
    if (screen != 5) {
        g_worldMapWasOpen = false;
        g_screenBeforeMap = screen;
    }
    if (screen == 0 || screen == 2 || screen == 5 || (!g_minimapVisible && !g_minimapInfoBar && !g_minimapSettingsOpen)) {
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

    SafeRunHudMinimapLayerBody(client, foreground, appFocused, screen, playerX, playerZ, anchor);

    g_endWidget(anchor.raw);
    g_endLayer(layer);
}

void SafeDrawHudMinimapOverlay() {
    __try {
        DrawHudMinimapOverlay();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void PlayStateUiHook() {
    if (g_origPlayStateUi) {
        g_origPlayStateUi();
    }
    greeneye::Lease lease;
    if (lease) {
        SafeDrawHudMinimapOverlay();
    }
}

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

void SafePreSyncWorldMapMarkers() {
    __try {
        SyncSpiresAndCountMarkers(GetUiClient(), nullptr, nullptr, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void BuildWorldMapPlayersHook(void* outVec, void* mapState) {
    if (g_origBuildWorldMapPlayers) {
        g_origBuildWorldMapPlayers(outVec, mapState);
    }
    greeneye::Lease lease;
    if (lease) {
        FilterWorldMapPlayerMarkers(outVec);
        // BuildWorldMapPlayers is called by 0x140d8f270 immediately before iterating arr0..arr3
        // to populate mapState+0x56318. Syncing arr0 here ensures newly replicated ECS markers
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
    const uint32_t markerState = *reinterpret_cast<uint32_t*>(m + 0x34);
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

    if (isChest) {
        const bool lootedDim = (markerState == 3u && g_lootedChestMode == 1);
        const bool hybridDim = (g_pinRevealMode == PinRevealMode::Hybrid && client &&
                                !IsPositionExploredByCharacter(client,
                                                               *reinterpret_cast<int32_t*>(m + 0x14),
                                                               *reinterpret_cast<int32_t*>(m + 0x24)));
        return lootedDim || hybridDim;
    }
    if (!isCustomPlayerPin && g_pinRevealMode == PinRevealMode::Hybrid && client) {
        return !IsMarkerExplored(client, m, origHash);
    }
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
            const float opacity = chest && *reinterpret_cast<uint32_t*>(m + 0x34) == 3u ? 0.36f : 0.48f;
            g_pushUiOpacity(uiCtx, opacity);
            bak.opacityUiCtx = uiCtx;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The caller still restores any partial override, including the opacity stack.
    }
}

void DrawWorldMapMarkerIconWithOverrides(void* mapState, uint8_t* m, uint8_t* drawParams) {
    WorldMapMarkerOverrideBackup bak{};
    ApplyWorldMapMarkerIconOverrides(m, drawParams, bak);
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

void DrawWorldMapMarkerIconHook(void* mapState, uint8_t* m, uint8_t* drawParams) {
    if (!g_origDrawWorldMapMarkerIcon) return;
    greeneye::Lease lease;
    if (lease) DrawWorldMapMarkerIconWithOverrides(mapState, m, drawParams);
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
        const uint32_t markerState = *reinterpret_cast<uint32_t*>(m + 0x34);
        const float dimAlpha = (isChest && markerState == 3u) ? 0.36f : 0.42f;
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

void DrawWorldMapMarkerPlateHook(void* widget, uint8_t* entry, const float* rect4) {
    if (!g_origDrawWorldMapMarkerPlate) return;

    greeneye::Lease lease;
    void* pushedUiCtx = nullptr;
    bool pushed = false;
    {
        if (lease) {
            pushed = TryPushMarkerPlateDimOpacity(entry, pushedUiCtx);
        }
    }

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
    const bool worldMap = client[0x1cd8] == 5;
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

bool InstallMapKeyPanelHook(LogFn logger) {
    g_log = logger;
    if (g_hookInstalled) return true;

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
    uint8_t* fnDrawMapFilterMenuTail = FindUnique("48 81 EC 30 08 00 00 0F 29 70 B8 4C 8B F9 48 8B 0D");
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
    g_hookInstalled = true;
    if (g_log) {
        g_log("MapKeyPanel: hooks installed%s: drawMapFilterMenu=%p worldMapInput=%p updateViewport=%p getMarkerType=%p playStateUi=%p markerIcon=%p",
            greeneye::Host() ? " through the Greeneye Loader" : "",
            g_hookTarget, g_worldMapInputHookTarget, g_updateMapViewport, g_getMarkerTypeHookTarget,
            g_playStateHookTarget, g_drawWorldMapMarkerIconHookTarget);
    }
    return true;
}

void UseKeptState(KeptState* kept) {
    g_kept = kept;
}

void PrepareMapKeyPanelForReload() {
    RemoveMapKeyWheelHook();
    SetMinimapMouseControl(false, nullptr);
    if (g_findDialogOpen) {
        CloseNativeFindDialog();
    }
}

} // namespace unlock_map_markers

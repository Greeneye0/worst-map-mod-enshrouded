#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "map_key_panel.h"
#include "chest_spawns.h"
#include "minimap_frame_spans.h"
#include "scan.h"
#include "greeneye.h"

namespace unlock_map_markers {
namespace {

constexpr uint32_t kGoldChestTypeHash = 0x601dcbe5u;
constexpr uint32_t kSilverChestTypeHash = 0x511fcbe5u;
constexpr uint32_t kOtherPlayerTypeHash = 0x26d18a06u;
constexpr uint32_t kLockedFastTravelTypeHash = 0x1095684fu;
constexpr uint32_t kChestMarkerUidPrefix = 0x70000000u;

bool g_goldChestEnabled = true;
bool g_silverChestEnabled = true;
bool g_otherPlayersEnabled = true;

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
// Nine-slice boxes of the game's FbUiPrimitives (the boxes vanilla menus and dialogs are made of).
constexpr size_t kBoxInnerGlowPrimitive = kPrimitivesOffset + 0x120;
constexpr size_t kGlowBoxSmallPrimitive = kPrimitivesOffset + 0x140;
constexpr size_t kRoundedBoxPrimitive = kPrimitivesOffset + 0x180;
constexpr size_t kSmallTextStyleOffset = 0x10a0c;
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
    const char* label;
};

constexpr MarkerRowSpec kCategory1Rows[] = {
    {0xa17072beu, 0u, 1, "Ancient Spire"},
    {0xa447cba3u, 0x6a56c5b3u, 1, "Flame Altar (Home)"},
    {0x7b4c9e16u, 0u, 1, "Flame Sanctum"},
    {0x6df85109u, 0u, 1, "Night Sanctum"},
    {0xf455a765u, 0u, 1, "Gem Forge"},
    {0xad6a9d9fu, 0u, 1, "Ancient Obelisk"},
    {0x4293eae5u, 0u, 1, "Water Source"},
    {0x1095684eu, 0u, 1, "Fast Travel Point"},
};

constexpr MarkerRowSpec kCategory2Rows[] = {
    {0xffe5e444u, 0u, 2, "Elixir Well / Root"},
    {0xb824ea45u, 0u, 2, "Catacombs / Tomb"},
    {0x549e3dbau, 0u, 2, "Ancient Vault"},
    {0x2756cf65u, 0u, 2, "Shroud Lair"},
    {0x4ece72ccu, 0u, 2, "Hollow Halls"},
    {0x41a86870u, 0u, 2, "Hidden Tomb"},
};

constexpr MarkerRowSpec kCategory3Rows[] = {
    {kGoldChestTypeHash, 0u, 3, "Gold Chest"},
    {kSilverChestTypeHash, 0u, 3, "Silver Chest"},
    {0xb5d01766u, 0u, 3, "Town / Settlement"},
    {0x5104e8d1u, 0u, 3, "Farmstead"},
    {0x6aaf002cu, 0u, 3, "Survivor Camp"},
    {0x689e3a29u, 0u, 3, "Mine / Quarry"},
    {0xdb17bf33u, 0u, 3, "Cave Passage"},
    {0xa13a918cu, 0u, 3, "Enemy Encampment"},
    {0x9f3c320eu, 0u, 3, "Balefire Garrison"},
};

constexpr MarkerRowSpec kCategory0Rows[] = {
    {0xa7cadc08u, 0u, 0, "Main Quest"},
    {0xc52e92e5u, 0u, 0, "Side Quest"},
    {0x8ba7c122u, 0u, 0, "Lore Rumor (?)"},
    {0x1ad8f96eu, 0u, 0, "Player Grave"},
    {kOtherPlayerTypeHash, 0u, 0, "Other Players"},
};

constexpr uint32_t kCustomPinHashes[10] = {
    0x0e6558ffu, 0x5eba3084u, 0xeeb09abeu, 0xfe2fd02du, 0x1db9432du,
    0x28a5fb2cu, 0xd9b19aecu, 0xa1c4ece9u, 0x3a761350u, 0x7103269eu
};

struct CategoryGroupSpec {
    uint8_t category;
    const char* title;
    const MarkerRowSpec* rows;
    size_t rowCount;
};

constexpr CategoryGroupSpec kCategoryGroups[5] = {
    {1, "FAST TRAVEL & ALTARS", kCategory1Rows, sizeof(kCategory1Rows) / sizeof(kCategory1Rows[0])},
    {2, "SHROUD & VAULTS",      kCategory2Rows, sizeof(kCategory2Rows) / sizeof(kCategory2Rows[0])},
    {3, "WORLD & RESOURCES",    kCategory3Rows, sizeof(kCategory3Rows) / sizeof(kCategory3Rows[0])},
    {0, "QUESTS & RUMORS",      kCategory0Rows, sizeof(kCategory0Rows) / sizeof(kCategory0Rows[0])},
    {4, "CUSTOM PINS",          nullptr,        0},
};

bool g_panelCollapsed = false;
bool g_categoryCollapsed[5] = {false, false, false, false, false};
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

// Draws a single text label at absolute UI coordinates (absX, absY, w, h)
// with explicit horizontal and vertical alignment inside the bounding box.
void DrawLabelAbs(float absX, float absY, float w, float h, const char* utf8,
                  uint32_t rgba = 0xffffffffu, float alpha = 1.0f,
                  TextAlignH hAlign = TextAlignH::Left, TextAlignV vAlign = TextAlignV::Center) {
    uint8_t* assets = GetUiAssets();
    if (!assets || !utf8 || !*utf8) return;
    UiText text = MakeUiText(utf8);
    alignas(16) uint8_t label[0x400]{};
    g_beginLabel(label, &text, assets + kSmallTextStyleOffset, 0);
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

// The ON / OFF badge of rows and category headers, right-aligned in the box ending at rightX.
void DrawOnOffBadgeAbs(void* parentWidgetRaw, float rightX, float absY, float rowH, bool on, float alpha = 1.0f) {
    using namespace key_style;
    const float bw = 50.0f; // fits "OFF" at the game's text size; ON and OFF line up
    const float bh = std::min(18.0f, rowH - 3.0f);
    const float bx = rightX - bw;
    const float by = absY + (rowH - bh) * 0.5f;
    if (on) {
        DrawFramedBoxAbs(parentWidgetRaw, bx, by, bw, bh, kOnFill, 0.95f * alpha, kOnFrame, 0.95f * alpha, 3.0f);
    } else {
        DrawFramedBoxAbs(parentWidgetRaw, bx, by, bw, bh, kPanelFill, 0.6f * alpha, kOffFrame, 0.9f * alpha, 3.0f);
    }
    DrawLabelAbs(bx, by, bw, bh, on ? "ON" : "OFF", on ? kOnText : kOffText, (on ? 0.98f : 0.8f) * alpha,
                 TextAlignH::Center, TextAlignV::Center);
}

bool IsFilterEnabled(FilterEntry* entries, size_t count, uint32_t typeHash) {
    if (typeHash == kGoldChestTypeHash) return g_goldChestEnabled;
    if (typeHash == kSilverChestTypeHash) return g_silverChestEnabled;
    if (typeHash == kOtherPlayerTypeHash) return g_otherPlayersEnabled;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].typeHash == typeHash) {
            return entries[i].enabled != 0;
        }
    }
    return true;
}

void ApplyFilterChange(uint8_t* client, uint32_t typeHash, bool enable) {
    if (typeHash == kGoldChestTypeHash || typeHash == kSilverChestTypeHash || typeHash == kOtherPlayerTypeHash) return;
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
    if (category == 0 && g_otherPlayersEnabled) {
        return true;
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
    if (typeHash == 0x26d18a06u) {
        if (uint8_t* assets = GetUiAssets()) {
            const auto* bytes = reinterpret_cast<const uint64_t*>(assets + 0xdf60);
            if (bytes[0] != 0 || bytes[1] != 0) return assets + 0xdf60;
        }
    }
    if (!client || !g_getMarkerType) return nullptr;
    void* mgr = *reinterpret_cast<void**>(client + 0x4667f0);
    if (!mgr) return nullptr;
    uint8_t* markerType = g_getMarkerType(typeHash, mgr);
    if (!markerType) return nullptr;
    return markerType + 0x1c;
}

const UiText& GetChestSubtypeTitle(uint16_t subtype) {
    static const UiText kTitles[6] = {
        MakeUiText("Gold Chest"),
        MakeUiText("Gold Sarcophagus"),
        MakeUiText("Gold Chest (Locked)"),
        MakeUiText("Silver Chest"),
        MakeUiText("Silver Sarcophagus"),
        MakeUiText("Silver Chest (Locked)")
    };
    return kTitles[subtype < 6 ? subtype : 0];
}

void WriteChestMarkerSlot(uint8_t* m, size_t chestIdx) {
    std::memset(m, 0, 0xA0);
    const ChestSpawn& sp = kChestSpawns[chestIdx];
    bool isGold = (chestIdx < kGoldChestCount);
    uint32_t typeHash = isGold ? kGoldChestTypeHash : kSilverChestTypeHash;

    *reinterpret_cast<uint32_t*>(m + 0x00) = kChestMarkerUidPrefix | static_cast<uint32_t>(chestIdx + 1);
    *reinterpret_cast<uint32_t*>(m + 0x08) = 0u;
    *reinterpret_cast<uint64_t*>(m + 0x10) = (static_cast<uint64_t>(sp.px1) << 32) | (static_cast<uint64_t>(sp.px0) << 16);
    *reinterpret_cast<uint64_t*>(m + 0x18) = (static_cast<uint64_t>(sp.py1) << 32) | (static_cast<uint64_t>(sp.py0) << 16);
    *reinterpret_cast<uint64_t*>(m + 0x20) = (static_cast<uint64_t>(sp.pz1) << 32) | (static_cast<uint64_t>(sp.pz0) << 16);
    // IMPORTANT: Never set m+0x30 to 0 in arr1/arr2 because 0x140d8f833 does not null-check
    // MapMarkerType* before 0x140c24bf0 dereferences [r15 + 0x42]. Instead, omit disabled chests from arr1/arr2.
    *reinterpret_cast<uint32_t*>(m + 0x30) = typeHash;
    m[0x34] = 0;
    *reinterpret_cast<uint32_t*>(m + 0x3c) = typeHash;
    *reinterpret_cast<UiText*>(m + 0x68) = GetChestSubtypeTitle(sp.subtype);
    m[0x80] = isGold ? 4 : 0; // 4 = Yellow/Gold, 0 = White/Silver
}

void SyncChestsInUiArrays(uint8_t* client, bool& dirty) {
    if (!client) return;

    // We distribute only the currently enabled chest markers across:
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

    size_t activeCount = (g_goldChestEnabled ? kGoldChestCount : 0) + (g_silverChestEnabled ? kSilverChestCount : 0);

    size_t avail0 = (caps[0] > vanillaCounts[0]) ? static_cast<size_t>(caps[0] - vanillaCounts[0]) : 0;
    size_t avail1 = (caps[1] > vanillaCounts[1]) ? static_cast<size_t>(caps[1] - vanillaCounts[1]) : 0;
    size_t assign0 = std::min(activeCount, avail0);
    size_t assign1 = std::min(activeCount - assign0, avail1);

    size_t assigns[2] = { assign0, assign1 };
    size_t curLogicalOffset = 0;

    auto mapLogicalToChestIdx = [&](size_t logicalIdx) -> size_t {
        if (g_goldChestEnabled) {
            return logicalIdx; // 0..447 gold, 448..1479 silver (if silver also enabled)
        }
        return kGoldChestCount + logicalIdx; // only silver enabled
    };

    for (int a = 0; a < 2; ++a) {
        uint64_t desiredTotal = vanillaCounts[a] + assigns[a];
        bool needsRewrite = (counts[a] != desiredTotal);
        if (!needsRewrite && assigns[a] > 0) {
            uint8_t* firstM = bases[a] + vanillaCounts[a] * 0xA0;
            size_t firstChestIdx = mapLogicalToChestIdx(curLogicalOffset);
            uint32_t expectedFirstUid = kChestMarkerUidPrefix | static_cast<uint32_t>(firstChestIdx + 1);
            if (*reinterpret_cast<uint32_t*>(firstM + 0x00) != expectedFirstUid) {
                needsRewrite = true;
            }
        }

        if (needsRewrite) {
            for (size_t k = 0; k < assigns[a]; ++k) {
                size_t chestIdx = mapLogicalToChestIdx(curLogicalOffset + k);
                WriteChestMarkerSlot(bases[a] + (vanillaCounts[a] + k) * 0xA0, chestIdx);
            }
            *reinterpret_cast<uint64_t*>(client + kOffsets[a] + 8) = desiredTotal;
            dirty = true;
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
    // UiClient is embedded at GameClient + 0x1f0; GameClient + 0x47d4e0 (client + 0x47d2f0) holds clientWorld (r15)
    uint8_t* clientWorld = *reinterpret_cast<uint8_t**>(client + 0x47d2f0);
    if (!clientWorld) return true;
    // Verify back-pointer [clientWorld + 0x3d98] == client
    if (*reinterpret_cast<uint8_t**>(clientWorld + 0x3d98) != client) return true;
    uint8_t* fow = *reinterpret_cast<uint8_t**>(clientWorld + 0x3db0);
    if (!fow) return true;

    float worldZ = *reinterpret_cast<float*>(fow + 0x04);
    uint32_t chunksX = *reinterpret_cast<uint32_t*>(fow + 0x08);
    uint32_t chunksZ = *reinterpret_cast<uint32_t*>(fow + 0x0c);
    uint8_t* bitmap = *reinterpret_cast<uint8_t**>(fow + 0x18);
    uint64_t totalChunks = *reinterpret_cast<uint64_t*>(fow + 0x20);
    if (!bitmap || chunksX == 0 || chunksZ == 0 || chunksX > 256 || chunksZ > 256 ||
        totalChunks != static_cast<uint64_t>(chunksX) * chunksZ) {
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
    return bitmap[offset] >= 200u;
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

uint32_t GetMarkerOrigTypeHash(const uint8_t* m) {
    uint32_t savedOrig = *reinterpret_cast<const uint32_t*>(m + 0x3c);
    if (savedOrig != 0u && savedOrig != kLockedFastTravelTypeHash) {
        return savedOrig;
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
    return savedOrig;
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

    uint8_t* arrBase = *reinterpret_cast<uint8_t**>(client + 0x36ea00);
    uint64_t arrCount = *reinterpret_cast<uint64_t*>(client + 0x36ea08);

    struct SpirePos2D { int32_t x; int32_t z; };
    SpirePos2D discoveredSpires[32]{};
    size_t numDiscoveredSpires = 0;

    static bool s_initialRefresh = true;
    bool dirty = s_initialRefresh;
    s_initialRefresh = false;
    SyncChestsInUiArrays(client, dirty);

    struct SavedMarkerMeta {
        uint64_t key;
        uint32_t origHash;
        uint32_t savedId08;
    };
    static SavedMarkerMeta s_savedMeta[4096]{};

    auto computeMarkerKey = [](const uint8_t* m) -> uint64_t {
        // Note: m + 0x00 (uid) increments on every replicate_map_markers_for_ui rebuild, so exclude it!
        // Use invariant fields: entityId (m + 0x04), 3D fixed-point position (m + 0x10..0x27), tag38 (m + 0x38), and b48 (m + 0x48).
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

    auto findOrInsertMeta = [&](uint64_t key) -> SavedMarkerMeta& {
        size_t idx = static_cast<size_t>((key * 0x9e3779b97f4a7c15ULL) >> 52) & 4095u;
        for (size_t probe = 0; probe < 4096u; ++probe) {
            size_t slot = (idx + probe) & 4095u;
            if (s_savedMeta[slot].key == key || s_savedMeta[slot].key == 0ULL) {
                s_savedMeta[slot].key = key;
                return s_savedMeta[slot];
            }
        }
        s_savedMeta[idx].key = key;
        s_savedMeta[idx].origHash = 0u;
        s_savedMeta[idx].savedId08 = 0u;
        return s_savedMeta[idx];
    };

    auto resolveMarkerMeta = [&](uint8_t* m, uint32_t& outOrigHash, uint32_t& outSavedId08) {
        uint64_t key = computeMarkerKey(m);
        SavedMarkerMeta& meta = findOrInsertMeta(key);
        uint32_t prevOrig = *reinterpret_cast<uint32_t*>(m + 0x3c);
        uint32_t prevId08 = *reinterpret_cast<uint32_t*>(m + 0x4c);
        if (meta.origHash == 0u && prevOrig != 0u && prevOrig != kLockedFastTravelTypeHash) {
            meta.origHash = prevOrig;
        }
        if (meta.savedId08 == 0u && prevId08 != 0u) {
            meta.savedId08 = prevId08;
        }
        uint32_t tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
        uint32_t curId08 = *reinterpret_cast<uint32_t*>(m + 0x08);
        if (tHash != 0u && tHash != kLockedFastTravelTypeHash) {
            // Do not overwrite a known POI origHash when m+0x30 was temporarily switched to 0xafce0c5f for a quest subpin
            if (meta.origHash == 0u || tHash != 0xafce0c5fu) {
                meta.origHash = tHash;
            }
        }
        if (curId08 != 0u) {
            meta.savedId08 = curId08;
        }
        if (meta.origHash == 0u && m[0x48] != 0u) {
            uint32_t tag38 = *reinterpret_cast<uint32_t*>(m + 0x38);
            if (tag38 == 0xe3a3e3c1u || tag38 == 0xe623c0fbu) {
                meta.origHash = 0x1095684eu;
            } else if (tag38 == 0x5e74cd45u || tag38 == 0x4f301a2au ||
                       tag38 == 0x06e3fedau || tag38 == 0x7312df52u ||
                       tag38 == 0x8235997bu || tag38 == 0xb487f1bbu ||
                       tag38 == 0x69adaed5u || tag38 == 0xa15c18ebu) {
                meta.origHash = 0xa17072beu;
            }
        }
        *reinterpret_cast<uint32_t*>(m + 0x3c) = meta.origHash;
        *reinterpret_cast<uint32_t*>(m + 0x4c) = meta.savedId08;
        outOrigHash = meta.origHash;
        outSavedId08 = meta.savedId08;
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

            uint32_t subpinQuestHash = (origHash != 0xafce0c5fu && origHash != 0x454e24f2u)
                                           ? GetQuestFilterHashForMarkerId(savedId08, true)
                                           : 0u;
            bool subpinQuestOn = (subpinQuestHash == 0xa7cadc08u)   ? mainQuestFilterOn
                               : (subpinQuestHash == 0xc52e92e5u) ? sideQuestFilterOn
                                                                  : false;
            bool allowSubpinId = (subpinQuestHash == 0u) || subpinQuestOn;

            if (b48 != 0 && origHash == 0xa17072beu) {
                bool unlockedForChar = (lastChg != 0);
                applyMarkerVisibility(m, 0xa17072beu, savedId08, spireFilterOn && unlockedForChar,
                                      unlockedForChar && subpinQuestOn, allowSubpinId);
                tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
                if (unlockedForChar && numDiscoveredSpires < 32) {
                    discoveredSpires[numDiscoveredSpires].x = *reinterpret_cast<int32_t*>(m + 0x14);
                    discoveredSpires[numDiscoveredSpires].z = *reinterpret_cast<int32_t*>(m + 0x24);
                    ++numDiscoveredSpires;
                }
            } else if (origHash == 0x1095684eu) {
                bool unlockedForChar = (b48 == 0) || (lastChg != 0);
                uint32_t desiredHash = fastTravelFilterOn
                                           ? (unlockedForChar ? 0x1095684eu : kLockedFastTravelTypeHash)
                                           : (subpinQuestOn ? 0xafce0c5fu : 0u);
                uint32_t desiredId08 = (fastTravelFilterOn ? allowSubpinId : subpinQuestOn) ? savedId08 : 0u;
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

            uint32_t subpinQuestHash = (origHash != 0xafce0c5fu && origHash != 0x454e24f2u)
                                           ? GetQuestFilterHashForMarkerId(savedId08, true)
                                           : 0u;
            bool subpinQuestOn = (subpinQuestHash == 0xa7cadc08u)   ? mainQuestFilterOn
                               : (subpinQuestHash == 0xc52e92e5u) ? sideQuestFilterOn
                                                                  : false;
            bool allowSubpinId = (subpinQuestHash == 0u) || subpinQuestOn;

            if (origHash == 0xa17072beu) {
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (b48 == 0 || lastChg != 0) {
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
                applyMarkerVisibility(m, 0x98b2d65du, savedId08, spireFilterOn && !alreadyUnlockedHere,
                                      !alreadyUnlockedHere && subpinQuestOn, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                if (!alreadyUnlockedHere) {
                    addCountForHash(0xa17072beu);
                    addCountForHash(subpinQuestHash);
                }
                continue;
            }

            if (origHash == 0x1095684eu || origHash == kLockedFastTravelTypeHash) {
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                addCountForHash(0x1095684eu);
                addCountForHash(subpinQuestHash);
                continue;
            }

            // Manage non-filterable (filt == 0) marker types so HIDE ALL and category filters control them
            if (origHash == 0xafce0c5fu) {
                uint32_t qHash = GetQuestFilterHashForMarkerId(savedId08, false);
                uint32_t effHash = (qHash != 0u) ? qHash : 0x8ba7c122u;
                bool showPin = (effHash == 0xa7cadc08u)   ? mainQuestFilterOn
                             : (effHash == 0xc52e92e5u) ? sideQuestFilterOn
                                                        : rumorFilterOn;
                applyMarkerVisibility(m, origHash, savedId08, showPin);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                addCountForHash(effHash);
                continue;
            }
            if (origHash == 0x454e24f2u) {
                applyMarkerVisibility(m, origHash, savedId08, mainQuestFilterOn);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                addCountForHash(0xa7cadc08u);
                continue;
            }
            if (origHash == 0x813080bcu) {
                applyMarkerVisibility(m, origHash, savedId08, fastTravelFilterOn, subpinQuestOn, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                addCountForHash(0x1095684eu);
                addCountForHash(subpinQuestHash);
                continue;
            }
            if (origHash == 0x7b87ef66u || origHash == 0xd11531d2u || origHash == 0x111adadau) {
                applyMarkerVisibility(m, origHash, savedId08, townFilterOn, subpinQuestOn, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                addCountForHash(subpinQuestHash);
                continue;
            }
            if (origHash == 0x9786a9f3u) {
                applyMarkerVisibility(m, origHash, savedId08, passageFilterOn, subpinQuestOn, allowSubpinId);
                arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ *reinterpret_cast<uint32_t*>(m + 0x30)) * 0x100000001b3ULL;
                addCountForHash(subpinQuestHash);
                continue;
            }

            if (origHash != 0u) {
                uint32_t poiFilterHash = (origHash == 0x6a56c5b3u) ? 0xa447cba3u : origHash;
                bool poiFilterOn = IsFilterEnabled(filterEntries, filterCount, poiFilterHash);
                if (!poiFilterOn && subpinQuestOn) {
                    applyMarkerVisibility(m, origHash, savedId08, false, true, true);
                } else {
                    applyMarkerVisibility(m, origHash, savedId08, true, false, allowSubpinId);
                }
                tHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            }

            arr0Sig = (arr0Sig ^ computeMarkerKey(m) ^ tHash ^ *reinterpret_cast<uint32_t*>(m + 0x08)) * 0x100000001b3ULL;
            if (origHash == 0u) continue;
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

    for (size_t q = 0; q < numQueries; ++q) {
        if (queryHashes[q] == kGoldChestTypeHash) {
            outCounts[q] = static_cast<int>(kGoldChestCount);
        } else if (queryHashes[q] == kSilverChestTypeHash) {
            outCounts[q] = static_cast<int>(kSilverChestCount);
        } else if (queryHashes[q] == kOtherPlayerTypeHash) {
            outCounts[q] = otherPlayerCount;
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
float g_lastRootX = 0.0f;
float g_lastRootY = 0.0f;
float g_lastRootW = 1920.0f;
float g_lastRootH = 1080.0f;
float g_lastPanelX = 0.0f;
float g_lastPanelY = 0.0f;
float g_lastPanelW = 0.0f;
float g_lastPanelH = 0.0f;
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
    *reinterpret_cast<UiText*>(cancelBtn + 0x78) = MakeRuntimeUiText("Cancel");

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

void CloseNativeFindDialog() {
    if (g_findCloseDialog) {
        g_findCloseDialog();
    } else if (auto* g = static_cast<uint8_t*>(GetUiG())) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(g + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                d0[0x2fe70] = 0;
            }
        }
    }
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

        std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf),
                      "Type to search  |  F2: Filters [%s]",
                      g_findRespectFilters ? "ON" : "OFF");
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

    if (g_findRespectFilters) {
        if (g_findFilteredOutCount > 0) {
            std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf),
                          "%zu match%s (%d hidden)  |  F2: Filters [ON]",
                          g_findMatchedPlaces.size(),
                          (g_findMatchedPlaces.size() == 1) ? "" : "es",
                          g_findFilteredOutCount);
        } else {
            std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf),
                          "%zu match%s  |  F2: Filters [ON]",
                          g_findMatchedPlaces.size(),
                          (g_findMatchedPlaces.size() == 1) ? "" : "es");
        }
    } else {
        std::snprintf(g_hintLineBuf, sizeof(g_hintLineBuf),
                      "%zu match%s  |  F2: Filters [OFF]",
                      g_findMatchedPlaces.size(),
                      (g_findMatchedPlaces.size() == 1) ? "" : "es");
    }
    *reinterpret_cast<UiText*>(g_mapFindDialog + 0x878) = MakeRuntimeUiText(g_hintLineBuf);
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
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
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (grp.rows[r].typeHash == filterHash) {
                return grp.rows[r].label;
            }
        }
    }
    for (size_t p = 0; p < 10; ++p) {
        if (kCustomPinHashes[p] == filterHash) {
            return "Custom Pin";
        }
    }
    return "Map Location";
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
    if (!rawUtf8 || rawLen == 0) return fallbackLabel ? fallbackLabel : "Map Location";
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
    return s.empty() ? (fallbackLabel ? fallbackLabel : "Map Location") : s;
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
        std::string typeLower = ToLowerAscii(typeLabel ? typeLabel : "");
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
        entry.typeLabel = typeLabel ? typeLabel : "Map Location";
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

            uint32_t savedId08 = *reinterpret_cast<uint32_t*>(m + 0x4c);
            if (savedId08 == 0u) savedId08 = *reinterpret_cast<uint32_t*>(m + 0x08);

            uint32_t filterHash = (origHash == 0xafce0c5fu)
                                      ? GetQuestFilterHashForMarkerId(savedId08, false)
                                      : NormalizeFilterTypeHash(origHash);
            bool isFilterOn = IsFilterEnabled(filterEntries, filterCount, filterHash);
            if (!isFilterOn && origHash != 0xafce0c5fu) {
                uint32_t subpinQuestHash = GetQuestFilterHashForMarkerId(savedId08, true);
                if (subpinQuestHash != 0u && IsFilterEnabled(filterEntries, filterCount, subpinQuestHash)) {
                    isFilterOn = true;
                }
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
        const ChestSpawn& sp = kChestSpawns[c];
        bool isGold = (c < kGoldChestCount);
        uint32_t chestHash = isGold ? kGoldChestTypeHash : kSilverChestTypeHash;
        bool isFilterOn = isGold ? g_goldChestEnabled : g_silverChestEnabled;
        const char* typeLabel = isGold ? "Gold Chest" : "Silver Chest";
        const UiText& subTitle = GetChestSubtypeTitle(sp.subtype);
        float wx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
        float wz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
        processCandidate(wx, wz, chestHash, chestHash, isFilterOn, false,
                          std::string(subTitle.utf8, subTitle.len), typeLabel);
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
                    : "Other Player";
                processCandidate(ox, oz, kOtherPlayerTypeHash, kOtherPlayerTypeHash,
                                  g_otherPlayersEnabled, false, playerName, "Other Players");
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
        std::snprintf(buf, sizeof(buf), "All Matching Places (%zu) - Zoom to fit", g_findMatchedPlaces.size());
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
            std::string shortTitle = topTitle;
            if (shortTitle.size() > 24) {
                shortTitle = shortTitle.substr(0, 22) + "..";
            }
            std::snprintf(buf, sizeof(buf), "All \"%s\" (%zu) - Zoom to fit",
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
        const char* lockTag = p.isLocked ? " [Locked]" : "";
        const char* filtTag = !p.isFilterOn ? " [Hidden]" : "";

        char distBuf[32]{};
        std::snprintf(distBuf, sizeof(distBuf), " (%.0fm)", p.distMeters);
        std::string distStr = distBuf;

        std::string body = p.title + lockTag + filtTag;
        if (p.title != p.typeLabel && !p.typeLabel.empty()) {
            body += " - " + p.typeLabel;
        }
        if (body.size() + distStr.size() > kMaxSingleLineChars) {
            size_t maxBody = (kMaxSingleLineChars > distStr.size() + 2)
                ? (kMaxSingleLineChars - distStr.size() - 2)
                : 12;
            if (body.size() > maxBody) {
                body.resize(maxBody);
                while (!body.empty() && (body.back() == ' ' || body.back() == '-')) {
                    body.pop_back();
                }
                body += "..";
            }
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
        std::snprintf(buf, sizeof(buf), "0 visible (%d hidden) - F2/Enter to show",
                      g_findFilteredOutCount);
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

void CenterMapViewOnPlayer(uint8_t* client, uint8_t* mapState) {
    if (!client || !mapState) return;
    float playerX = 5120.0f, playerZ = 5120.0f;
    if (!GetHudPlayerWorldPos(client, playerX, playerZ)) return;

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

void DrawMapKeyOverlay(void* mapStateVoid) {
    auto* client = GetUiClient();
    auto* mapState = static_cast<uint8_t*>(mapStateVoid);
    if (!client || !mapState) return;

    LoadMinimapConfig();

    // Detect World Map opening so we always center on the player's actual location when opening the map
    uint64_t nowMapTick = GetTickCount64();
    if (!g_worldMapWasOpen || (nowMapTick - g_lastWorldMapFrameTick) > 250ULL) {
        g_worldMapWasOpen = true;
        g_worldMapOpenCenterFrames = 2;
    }
    g_lastWorldMapFrameTick = nowMapTick;
    if (g_worldMapOpenCenterFrames > 0) {
        --g_worldMapOpenCenterFrames;
        CenterMapViewOnPlayer(client, mapState);
    }

    uint32_t allHashes[32]{};
    size_t numHashes = 0;
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            allHashes[numHashes++] = grp.rows[r].typeHash;
        }
    }
    int hashCounts[32]{};
    SyncSpiresAndCountMarkers(client, mapState, allHashes, numHashes, hashCounts);

    HWND foreground = GetForegroundWindow();
    DWORD fgPid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
    bool focused = (fgPid == GetCurrentProcessId());

    auto openFindDialog = [&]() {
        EnsureFindDialogResolved();
        if (!g_mapFindDialog) {
            if (g_findCreateDialog) {
                g_mapFindDialog = g_findCreateDialog("Find", "Find");
            } else {
                g_mapFindDialog = CreateNativeFindDialogFallback("Find", "Find");
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

        constexpr float kPanelW = 432.0f;
        constexpr float kTitleBarH = 30.0f;
        constexpr float kActionBarH = 25.0f;
        constexpr float kFogBarH = 25.0f;
        constexpr float kCatRowH = 23.0f;
        constexpr float kItemRowH = 20.0f;
        constexpr float kPinRowH = 28.0f;
        const float panelX = rootRect[0] + rootW - kPanelW - 24.0f;
        const float panelY = rootRect[1] + 104.0f;

        float totalH = kTitleBarH + 6.0f;
        if (!g_panelCollapsed) {
            totalH += kActionBarH + kFogBarH + 12.0f;
            for (size_t c = 0; c < 5; ++c) {
                totalH += kCatRowH + 2.0f;
                if (!g_categoryCollapsed[c]) {
                    if (kCategoryGroups[c].category == 4) {
                        totalH += kPinRowH + 4.0f;
                    } else {
                        totalH += kCategoryGroups[c].rowCount * kItemRowH + 4.0f;
                    }
                }
            }
            totalH += 6.0f;
        }

        // Block map & map marker hover/click interaction behind the panel or when Find dialog is open
        const bool panelHovered = !g_findDialogOpen && isHovered(panelX, panelY, kPanelW, totalH);
        const bool rightDown = focused && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        if (panelHovered && (leftDown || rightDown || clicked)) {
            g_panelClickLatch = true;
            g_findClosingFrames = std::max(g_findClosingFrames, 8);
        }
        g_panelCurrentlyHovered = panelHovered;
        g_lastRootX = rootRect[0];
        g_lastRootY = rootRect[1];
        g_lastRootW = rootW;
        g_lastRootH = rootH;
        g_lastPanelX = panelX;
        g_lastPanelY = panelY;
        g_lastPanelW = kPanelW;
        g_lastPanelH = totalH;

        if (!g_findDialogOpen && g_findClosingFrames > 0) {
            --g_findClosingFrames;
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

            // Row 1: Title + [FIND (Shift+F)] Button + Collapse Button [ - / + ]
            DrawLabelAbs(panelX + 12.0f, panelY, kPanelW - 196.0f, kTitleBarH,
                "MAP KEY & FILTERS", kTitle, 1.0f, TextAlignH::Left, TextAlignV::Center);

            // Wide enough for "FIND  Shift+F" at the game's text size
            const float btnFindX = kPanelW - 200.0f, btnFindY = 4.0f, btnFindW = 150.0f, btnFindH = 22.0f;
            bool hovFindBtn = !g_findDialogOpen && isHovered(panelX + btnFindX, panelY + btnFindY, btnFindW, btnFindH);
            DrawKeyButtonAbs(anchor.raw, panelX + btnFindX, panelY + btnFindY, btnFindW, btnFindH, hovFindBtn, g_findDialogOpen);
            {
                // "FIND" in the button's colour, then the shortcut in a quieter gold (one frame, no
                // keycap box inside it)
                const bool lit = g_findDialogOpen || hovFindBtn;
                const float bx = panelX + btnFindX, by = panelY + btnFindY;
                // At the game's text size "FIND" is ~47 units wide and "Shift+F" ~72: one at each end
                DrawLabelAbs(bx + 10.0f, by, 56.0f, btnFindH,
                    "FIND", lit ? kActiveText : kText, 0.96f, TextAlignH::Left, TextAlignV::Center);
                DrawLabelAbs(bx + 66.0f, by, btnFindW - 76.0f, btnFindH,
                    "Shift+F", lit ? kTitle : 0xff6fa9c2u, lit ? 0.98f : 0.9f, TextAlignH::Right, TextAlignV::Center); // #c2a96f
            }
            if (clicked && hovFindBtn) {
                openFindDialog();
            }

            const float btnColX = kPanelW - 44.0f, btnColY = 4.0f, btnColW = 36.0f, btnColH = 22.0f;
            bool hovCol = !g_findDialogOpen && isHovered(panelX + btnColX, panelY + btnColY, btnColW, btnColH);
            DrawKeyButtonAbs(anchor.raw, panelX + btnColX, panelY + btnColY, btnColW, btnColH, hovCol, false);
            DrawLabelAbs(panelX + btnColX, panelY + btnColY, btnColW, btnColH,
                g_panelCollapsed ? "+" : "-", hovCol ? kTitle : kText, hovCol ? 1.0f : 0.9f,
                TextAlignH::Center, TextAlignV::Center);
            if (clicked && hovCol) {
                g_panelCollapsed = !g_panelCollapsed;
            }

            if (!g_panelCollapsed) {
                // Row 2: Dedicated Quick Action Bar ([SHOW ALL] / [HIDE ALL])
                const float actionY = kTitleBarH + 5.0f;
                const float actionBtnH = kActionBarH - 2.0f;
                const float btnW = (kPanelW - 22.0f) * 0.5f;

                const float btnOnX = 8.0f;
                bool hovAllOn = isHovered(panelX + btnOnX, panelY + actionY, btnW, actionBtnH);
                DrawKeyButtonAbs(anchor.raw, panelX + btnOnX, panelY + actionY, btnW, actionBtnH, hovAllOn, false);
                DrawLabelAbs(panelX + btnOnX, panelY + actionY, btnW, actionBtnH,
                    "SHOW ALL", hovAllOn ? kActiveText : kText, hovAllOn ? 1.0f : 0.92f,
                    TextAlignH::Center, TextAlignV::Center);
                if (clicked && hovAllOn) {
                    SetAllFiltersState(client, mapState, true);
                }

                const float btnOffX = 14.0f + btnW;
                bool hovAllOff = isHovered(panelX + btnOffX, panelY + actionY, btnW, actionBtnH);
                DrawKeyButtonAbs(anchor.raw, panelX + btnOffX, panelY + actionY, btnW, actionBtnH, hovAllOff, false);
                DrawLabelAbs(panelX + btnOffX, panelY + actionY, btnW, actionBtnH,
                    "HIDE ALL", hovAllOff ? kActiveText : kText, hovAllOff ? 1.0f : 0.92f,
                    TextAlignH::Center, TextAlignV::Center);
                if (clicked && hovAllOff) {
                    SetAllFiltersState(client, mapState, false);
                }

                // Row 2b: Fog of War / Map Reveal Mode Selector (EXPLORED | HYBRID | REVEAL ALL)
                const float fogY = actionY + kActionBarH + 2.0f;
                const float fogBtnH = kFogBarH - 2.0f;
                DrawLabelAbs(panelX + 10.0f, panelY + fogY, 50.0f, fogBtnH,
                    "FOG:", kTextMuted, 0.95f, TextAlignH::Left, TextAlignV::Center);

                // Each button as wide as its label needs at the game's text size ("FOG:" ~49 units,
                // EXPLORED ~107, HYBRID ~79, REVEAL ALL ~123), with the spare room shared out.
                struct FogModeButton {
                    FogRevealMode mode;
                    const char* label;
                    float width;
                };
                constexpr FogModeButton kFogBtns[3] = {
                    { FogRevealMode::ExploredOnly, "EXPLORED",   121.0f },
                    { FogRevealMode::Hybrid,       "HYBRID",      93.0f },
                    { FogRevealMode::RevealAll,    "REVEAL ALL", 136.0f },
                };
                const float fogBtnStartX = 66.0f;
                const float fogBtnGap = 4.0f;
                float fbx = fogBtnStartX;
                for (int fm = 0; fm < 3; ++fm) {
                    const float fogBtnW = kFogBtns[fm].width;
                    bool active = (g_fogRevealMode == kFogBtns[fm].mode);
                    bool hovFm = isHovered(panelX + fbx, panelY + fogY, fogBtnW, fogBtnH);
                    DrawKeyButtonAbs(anchor.raw, panelX + fbx, panelY + fogY, fogBtnW, fogBtnH, hovFm, active);
                    uint32_t txtCol = active ? kActiveText : (hovFm ? kText : kTextMuted);
                    DrawLabelAbs(panelX + fbx, panelY + fogY, fogBtnW, fogBtnH - 1.0f,
                        kFogBtns[fm].label, txtCol, active ? 1.0f : 0.85f,
                        TextAlignH::Center, TextAlignV::Center);
                    if (clicked && hovFm) {
                        ApplyFogRevealMode(kFogBtns[fm].mode);
                    }
                    fbx += fogBtnW + fogBtnGap;
                }

                auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
                size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);

                float curY = kTitleBarH + kActionBarH + kFogBarH + 12.0f;
                size_t hashIdx = 0;

                for (size_t c = 0; c < 5; ++c) {
                    const auto& grp = kCategoryGroups[c];
                    bool catOn = IsCategoryEnabled(filterEntries, filterCount, grp.category);

                    bool hovCatCollapse = isHovered(panelX + 8.0f, panelY + curY, 34.0f, kCatRowH);
                    bool hovCatToggle = isHovered(panelX + 42.0f, panelY + curY, kPanelW - 50.0f, kCatRowH);

                    const bool hovCat = hovCatCollapse || hovCatToggle;
                    DrawFramedBoxAbs(anchor.raw, panelX + 8.0f, panelY + curY, kPanelW - 16.0f, kCatRowH,
                        hovCat ? kHoverFill : kCategoryFill, 0.96f, hovCat ? kGold : kCategoryFrame, hovCat ? 0.8f : 0.95f);

                    // Collapse marker (+ / -) in gold, then the group's title
                    DrawLabelAbs(panelX + 12.0f, panelY + curY, 22.0f, kCatRowH,
                        g_categoryCollapsed[c] ? "+" : "-", hovCatCollapse ? kActiveText : kGold, 1.0f,
                        TextAlignH::Center, TextAlignV::Center);
                    DrawLabelAbs(panelX + 36.0f, panelY + curY, kPanelW - 114.0f, kCatRowH,
                        grp.title, catOn ? kTitle : 0xff948c88u, 1.0f,
                        TextAlignH::Left, TextAlignV::Center);

                    DrawOnOffBadgeAbs(anchor.raw, panelX + kPanelW - 14.0f, panelY + curY, kCatRowH, catOn);

                    if (clicked && hovCatCollapse) {
                        g_categoryCollapsed[c] = !g_categoryCollapsed[c];
                    } else if (clicked && hovCatToggle) {
                        SetCategoryFilterState(client, mapState, grp.category, !catOn);
                        filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
                    }

                    curY += kCatRowH + 2.0f;

                    if (g_categoryCollapsed[c]) {
                        hashIdx += grp.rowCount;
                        continue;
                    }

                    if (grp.category == 4) {
                        const float stripX = 8.0f;
                        const float pinGap = 3.0f;
                        const float slotW = (kPanelW - 16.0f - pinGap * 9.0f) / 10.0f;
                        const float slotH = kPinRowH - 2.0f;
                        for (size_t p = 0; p < 10; ++p) {
                            uint32_t pinHash = kCustomPinHashes[p];
                            bool pinOn = IsFilterEnabled(filterEntries, filterCount, pinHash);
                            float px = stripX + p * (slotW + pinGap);
                            bool hovPin = isHovered(panelX + px, panelY + curY, slotW, slotH);

                            DrawFramedBoxAbs(anchor.raw, panelX + px, panelY + curY, slotW, slotH,
                                hovPin ? kHoverFill : (pinOn ? kButtonFill : kPanelFill), 0.95f,
                                hovPin ? kGold : (pinOn ? kButtonFrame : kOffFrame), hovPin ? 0.9f : 0.85f);

                            if (const void* iconRef = GetIconRefForType(client, pinHash)) {
                                DrawIconRectAbs(anchor.raw, iconRef,
                                    panelX + px + (slotW - 18.0f) * 0.5f,
                                    panelY + curY + (slotH - 18.0f) * 0.5f, 18.0f, 18.0f,
                                    pinOn ? 0xffffffffu : 0xff666666u, pinOn ? 1.0f : 0.38f, pinOn ? 1.15f : 0.7f);
                            }
                            if (clicked && hovPin) {
                                SetSingleFilterState(client, mapState, pinHash, !pinOn);
                            }
                        }
                        curY += kPinRowH + 4.0f;
                    } else {
                        for (size_t r = 0; r < grp.rowCount; ++r) {
                            const auto& rowSpec = grp.rows[r];
                            int count = hashCounts[hashIdx++];
                            bool rowOn = IsFilterEnabled(filterEntries, filterCount, rowSpec.typeHash);
                            bool hovRow = isHovered(panelX + 8.0f, panelY + curY, kPanelW - 16.0f, kItemRowH);

                            if (hovRow) {
                                // The game's gold glow box around the row, as Find lights up chests
                                DrawBoxAbs(anchor.raw, kGlowBoxSmallPrimitive, panelX + 5.0f, panelY + curY - 3.0f,
                                    kPanelW - 10.0f, kItemRowH + 6.0f, kGold, 0.35f, 8.0f);
                                DrawFramedBoxAbs(anchor.raw, panelX + 8.0f, panelY + curY, kPanelW - 16.0f, kItemRowH,
                                    kHoverFill, 0.92f, kGold, 0.85f, 4.0f);
                            }

                            // Status dot with a soft halo while on
                            const float dotCx = panelX + 18.5f, dotCy = panelY + curY + kItemRowH * 0.5f;
                            if (rowOn) {
                                DrawAssetRectAbs(anchor.raw, kSoftGlowPrimitive, dotCx - 7.0f, dotCy - 7.0f, 14.0f, 14.0f,
                                    kDotOn, 0.45f);
                            }
                            DrawAssetRectAbs(anchor.raw, kCirclePrimitive, dotCx - 3.5f, dotCy - 3.5f, 7.0f, 7.0f,
                                rowOn ? kDotOn : kDotOff, rowOn ? 0.98f : 0.8f);

                            // Round badge behind the marker icon: dark disc in a parchment ring
                            {
                                const float bx = panelX + 27.0f, by = panelY + curY + (kItemRowH - 20.0f) * 0.5f;
                                DrawAssetRectAbs(anchor.raw, kCirclePrimitive, bx, by, 20.0f, 20.0f,
                                    kBadgeFill, rowOn ? 0.95f : 0.45f);
                                DrawAssetRectAbs(anchor.raw, kCircleOutlinePrimitive, bx, by, 20.0f, 20.0f,
                                    kBadgeRing, rowOn ? 0.9f : 0.35f);
                            }

                            if (const void* iconRef = GetIconRefForType(client, rowSpec.typeHash)) {
                                uint32_t iconTint = rowOn ? 0xffffffffu : 0xff707070u;
                                if (rowSpec.typeHash == kGoldChestTypeHash) {
                                    iconTint = rowOn ? 0xff38c8ffu : 0xff406880u;
                                } else if (rowSpec.typeHash == kSilverChestTypeHash) {
                                    iconTint = rowOn ? 0xffe8e4dcu : 0xff707070u;
                                }
                                DrawIconRectAbs(anchor.raw, iconRef,
                                    panelX + 30.0f, panelY + curY + (kItemRowH - 14.0f) * 0.5f, 14.0f, 14.0f,
                                    iconTint, rowOn ? 1.0f : 0.40f, rowOn ? 1.15f : 0.7f);
                            }

                            char rowText[96]{};
                            if (count > 0) {
                                std::snprintf(rowText, sizeof(rowText), "%s (%d)", rowSpec.label, count);
                            } else {
                                std::snprintf(rowText, sizeof(rowText), "%s", rowSpec.label);
                            }
                            uint32_t textCol = rowOn ? 0xffd6e6eeu : kOffText; // #eee6d6 parchment
                            if (rowOn && rowSpec.typeHash == kGoldChestTypeHash) {
                                textCol = 0xff78dcffu;
                            }
                            DrawLabelAbs(panelX + 52.0f, panelY + curY, kPanelW - 110.0f, kItemRowH,
                                rowText, textCol, rowOn ? 0.96f : 0.75f,
                                TextAlignH::Left, TextAlignV::Center);

                            DrawOnOffBadgeAbs(anchor.raw, panelX + kPanelW - 14.0f, panelY + curY, kItemRowH, rowOn,
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
            bool tabEdge = keyPressedEdge(VK_TAB);
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
        if (focused && g_lastPanelW > 0.0f && g_lastPanelH > 0.0f) {
            POINT pt{};
            RECT cr{};
            if (GetCursorPos(&pt) && ScreenToClient(foreground, &pt) &&
                GetClientRect(foreground, &cr) && cr.right > 0 && cr.bottom > 0) {
                float mx = g_lastRootX + pt.x * g_lastRootW / static_cast<float>(cr.right);
                float my = g_lastRootY + pt.y * g_lastRootH / static_cast<float>(cr.bottom);
                cursorInPanel = (mx >= g_lastPanelX && mx <= g_lastPanelX + g_lastPanelW &&
                                 my >= g_lastPanelY && my <= g_lastPanelY + g_lastPanelH);
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
    if (g_minimapConfigLoaded) return;
    g_minimapConfigLoaded = true;
    std::string cfg = GetMinimapConfigPath();
    int customPos = GetPrivateProfileIntA("MinimapV2", "CustomPos", 0, cfg.c_str());
    int posX = GetPrivateProfileIntA("MinimapV2", "PosX", -1, cfg.c_str());
    int posY = GetPrivateProfileIntA("MinimapV2", "PosY", -1, cfg.c_str());
    int sizePx = GetPrivateProfileIntA("MinimapV2", "Size", 236, cfg.c_str());
    int zoomIdx = GetPrivateProfileIntA("MinimapV2", "ZoomIdx", 2, cfg.c_str());
    int otherPlayers = GetPrivateProfileIntA("MinimapV2", "OtherPlayers", 1, cfg.c_str());

    g_minimapCustomPos = (customPos != 0 && posX > 10 && posY > 10);
    if (g_minimapCustomPos) {
        g_minimapPosX = static_cast<float>(posX);
        g_minimapPosY = static_cast<float>(posY);
    }
    g_minimapSize = std::max(150.0f, std::min(440.0f, static_cast<float>(sizePx)));
    g_minimapZoomIdx = std::max(0, std::min(kMinimapZoomCount - 1, zoomIdx));
    g_otherPlayersEnabled = (otherPlayers != 0);
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
}

bool GetHudPlayerWorldPos(uint8_t* client, float& outX, float& outZ) {
    if (!client) return false;
    // 1. Primary live player world position from clientWorld (+0x47d2f0)
    if (uint8_t* clientWorld = *reinterpret_cast<uint8_t**>(client + 0x47d2f0)) {
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

void FormatInGameTimeOfDay(uint8_t* client, char* outText, size_t outCap, bool& outIsDay, int& outPhase) {
    outIsDay = true;
    outPhase = 1; // 0 = Dawn, 1 = Day, 2 = Dusk, 3 = Night
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
    const char* phaseStr = "Day";
    if (std::abs(curNs - sunriseNs) <= kTwilightHalfWindowNs) {
        phaseStr = "Dawn";
        outIsDay = true;
        outPhase = 0;
    } else if (std::abs(curNs - sunsetNs) <= kTwilightHalfWindowNs) {
        phaseStr = "Dusk";
        outIsDay = true;
        outPhase = 2;
    } else if ((curNs >= sunriseNs && curNs < sunsetNs) || isDayFlag != 0) {
        if (curNs >= sunriseNs && curNs < sunsetNs) {
            phaseStr = "Day";
            outIsDay = true;
            outPhase = 1;
        } else {
            phaseStr = "Night";
            outIsDay = false;
            outPhase = 3;
        }
    } else {
        phaseStr = "Night";
        outIsDay = false;
        outPhase = 3;
    }

    std::snprintf(outText, outCap, "%d:%02d %s  |  %s", hour12, minute, isPm ? "PM" : "AM", phaseStr);
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
};

void DrawHudMinimapOverlay() {
    uint8_t* client = GetUiClient();
    if (!client) {
        SetMinimapMouseControl(false, nullptr);
        return;
    }

    // Screen state check:
    // 1 = Playing (normal HUD), 5 = World Map, 2 = Pause/System Menu, 9 = Inventory/Journal
    // Hide minimap when World Map (5) or System Menu (2) or Main Menu (0) is open.
    uint8_t screen = client[0x1cd8];
    if (screen != 5) {
        g_worldMapWasOpen = false;
    }
    if (screen == 0 || screen == 2 || screen == 5) {
        g_minimapDraggingMove = false;
        g_minimapDraggingResize = false;
        SetMinimapMouseControl(false, nullptr);
        return;
    }

    float playerX = 5120.0f, playerZ = 5120.0f;
    if (!GetHudPlayerWorldPos(client, playerX, playerZ)) {
        SetMinimapMouseControl(false, nullptr);
        return;
    }

    LoadMinimapConfig();

    static const UiText kLayerName = MakeUiText("minimapHudLayer");

    // IMPORTANT: UiLayer is ~0x2e0 bytes; allocate 0x400 bytes to match DrawMapKeyOverlay!
    alignas(16) uint8_t layer[0x400]{};
    g_beginLayer(layer, &kLayerName, 1, 2, -1, 0);

    Widget anchor{};
    g_namedWidget(anchor.raw, &kLayerName);
    g_opaque(anchor.raw); // Required so the root widget receives the layer's full-screen rect at +0x114!

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

    float frameSize = std::max(150.0f, std::min(440.0f, g_minimapSize));
    float frameX = g_minimapPosX;
    float frameY = g_minimapPosY;

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
    frameY = std::max(rootY - frameSize * 0.05f, std::min(rootY + rootH - frameSize * 1.15f, frameY));

    // Inner map viewport matching the golden frame opening:
    const float vx = frameX + frameSize * 0.120f;
    const float vy = frameY + frameSize * 0.138f;
    const float vw = frameSize * 0.760f;
    const float vh = frameSize * 0.712f;
    const float vcx = vx + vw * 0.5f;
    const float vcy = vy + vh * 0.5f;

    const float btnScale = std::max(0.82f, frameSize / 236.0f);

    // Bottom-right corner resize grip (over the bottom-right golden corner medallion)
    const float resizeGripX0 = frameX + frameSize * 0.76f;
    const float resizeGripY0 = frameY + frameSize * 0.73f;
    const float resizeGripX1 = frameX + frameSize * 0.96f;
    const float resizeGripY1 = frameY + frameSize * 0.93f;

    // 1. Time of Day plaque cleanly below the bottom golden finial of the frame
    const float plaqueW = std::max(186.0f, frameSize * 0.82f);
    const float plaqueH = std::max(25.0f, std::round(26.0f * btnScale));
    const float plaqueX = frameX + (frameSize - plaqueW) * 0.5f;
    const float plaqueY = frameY + frameSize * 0.955f;

    // 2. External Zoom Control Bar directly below the Time of Day plaque:  [ - ]   Zoom: 1.0x   [ + ]
    const float btnSz = std::max(22.0f, std::round(23.0f * btnScale));
    const float zoomBarGap = 5.0f;
    const float zoomBadgeW = std::max(96.0f, plaqueW - 2.0f * (btnSz + zoomBarGap));
    const float zoomBarTotalW = btnSz + zoomBarGap + zoomBadgeW + zoomBarGap + btnSz;
    const float minusBtnX = frameX + (frameSize - zoomBarTotalW) * 0.5f;
    const float minusBtnY = plaqueY + plaqueH + 5.0f;
    const float zoomBadgeX = minusBtnX + btnSz + zoomBarGap;
    const float zoomBadgeY = minusBtnY;
    const float plusBtnX = zoomBadgeX + zoomBadgeW + zoomBarGap;
    const float plusBtnY = minusBtnY;

    // Mouse & Keyboard interaction
    HWND foreground = GetForegroundWindow();
    DWORD fgPid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &fgPid);
    const bool appFocused = (fgPid == GetCurrentProcessId());

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

    // Holding Shift + Alt unlocks the mouse cursor and locks camera rotation/attacks during normal play
    const bool shiftAltHeld = appFocused &&
                              ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) &&
                              ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    const bool lockCameraForMouse = appFocused && (shiftAltHeld || g_minimapDraggingMove || g_minimapDraggingResize);
    SetMinimapMouseControl(lockCameraForMouse, foreground);

    // Handle mouse wheel zoom while Shift+Alt is held
    LONG wheelDelta = InterlockedExchange(&g_minimapWheelDelta, 0);
    if (lockCameraForMouse && wheelDelta != 0) {
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
        if (screen != 1 || shiftAltHeld || g_minimapDraggingMove || g_minimapDraggingResize) {
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

    const bool leftDown = appFocused && cursorVisible && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    const bool leftPressed = leftDown && !g_minimapMouseWasDown;
    const bool leftReleased = !leftDown && g_minimapMouseWasDown;
    g_minimapMouseWasDown = leftDown;

    auto inRect = [&](float rx, float ry, float rw, float rh) -> bool {
        return mouseX >= rx && mouseX <= (rx + rw) && mouseY >= ry && mouseY <= (ry + rh);
    };

    const bool hoverMinus = cursorVisible && inRect(minusBtnX, minusBtnY, btnSz, btnSz);
    const bool hoverPlus  = cursorVisible && inRect(plusBtnX, plusBtnY, btnSz, btnSz);
    const bool hoverResize = cursorVisible && !hoverPlus && !hoverMinus &&
                             (mouseX >= resizeGripX0 && mouseX <= resizeGripX1 &&
                              mouseY >= resizeGripY0 && mouseY <= resizeGripY1);
    const bool hoverWholeWidget = cursorVisible && !hoverMinus && !hoverPlus &&
                                  (inRect(frameX + frameSize * 0.06f, frameY + frameSize * 0.04f,
                                          frameSize * 0.88f, frameSize * 0.90f) ||
                                   inRect(plaqueX, plaqueY, plaqueW, plaqueH) ||
                                   inRect(zoomBadgeX, zoomBadgeY, zoomBadgeW, btnSz));

    if (leftPressed) {
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
        } else if (hoverResize) {
            g_minimapDraggingResize = true;
            g_minimapDragStartMouseX = mouseX;
            g_minimapDragStartMouseY = mouseY;
            g_minimapDragStartSize = frameSize;
            g_minimapDragStartPosX = frameX;
            g_minimapDragStartPosY = frameY;
        } else if (hoverWholeWidget) {
            uint64_t nowTick = GetTickCount64();
            // Double-clicking the top compass star resets to default position under Journal [J]
            if (inRect(frameX + frameSize * 0.35f, frameY, frameSize * 0.30f, frameSize * 0.20f) &&
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
    }

    if (g_minimapDraggingResize) {
        if (leftDown) {
            float delta = std::max(mouseX - g_minimapDragStartMouseX, mouseY - g_minimapDragStartMouseY);
            g_minimapSize = std::max(150.0f, std::min(440.0f, g_minimapDragStartSize + delta));
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

    // ------------------------------------------------------------------------
    // 1. Draw Inner Map Viewport Background, Grid & Range Rings
    // ------------------------------------------------------------------------
    DrawSolidRectAbs(anchor.raw, vx - 2.0f, vy - 2.0f, vw + 4.0f, vh + 4.0f, 0xFF141816u, 0.94f);
    DrawSolidRectAbs(anchor.raw, vx, vy, vw, vh, 0xFF1C211Eu, 0.92f);

    const float radiusMeters = kMinimapZoomLevels[g_minimapZoomIdx].radiusMeters;
    const float pxPerMeterX = (vw * 0.5f) / radiusMeters;
    const float pxPerMeterY = (vh * 0.5f) / radiusMeters;

    // Subtle scrolling terrain grid (100m world grid lines)
    const float gridStepWorld = (radiusMeters <= 150.0f) ? 50.0f : 100.0f;
    const float firstGridX = std::floor((playerX - radiusMeters) / gridStepWorld) * gridStepWorld;
    for (float gx = firstGridX; gx <= playerX + radiusMeters; gx += gridStepWorld) {
        float sx = vcx + (gx - playerX) * pxPerMeterX;
        if (sx >= vx + 2.0f && sx <= vx + vw - 2.0f) {
            DrawSolidRectAbs(anchor.raw, sx, vy + 2.0f, 1.0f, vh - 4.0f, 0xFF2D3530u, 0.45f);
        }
    }
    const float firstGridZ = std::floor((playerZ - radiusMeters) / gridStepWorld) * gridStepWorld;
    for (float gz = firstGridZ; gz <= playerZ + radiusMeters; gz += gridStepWorld) {
        float sy = vcy - (gz - playerZ) * pxPerMeterY;
        if (sy >= vy + 2.0f && sy <= vy + vh - 2.0f) {
            DrawSolidRectAbs(anchor.raw, vx + 2.0f, sy, vw - 4.0f, 1.0f, 0xFF2D3530u, 0.45f);
        }
    }

    // Center crosshair axes & radar range circles
    DrawSolidRectAbs(anchor.raw, vcx, vy + 4.0f, 1.0f, vh - 8.0f, 0xFF3A443Du, 0.40f);
    DrawSolidRectAbs(anchor.raw, vx + 4.0f, vcy, vw - 8.0f, 1.0f, 0xFF3A443Du, 0.40f);
    const float ring1D = std::min(vw, vh) * 0.44f;
    const float ring2D = std::min(vw, vh) * 0.80f;
    DrawAssetRectAbs(anchor.raw, kCirclePrimitive, vcx - ring1D * 0.5f, vcy - ring1D * 0.5f, ring1D, ring1D, 0xFF455249u, 0.16f);
    DrawAssetRectAbs(anchor.raw, kCirclePrimitive, vcx - ring2D * 0.5f, vcy - ring2D * 0.5f, ring2D, ring2D, 0xFF455249u, 0.10f);

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

            uint32_t savedId08 = *reinterpret_cast<uint32_t*>(m + 0x4c);
            if (savedId08 == 0u) savedId08 = *reinterpret_cast<uint32_t*>(m + 0x08);

            uint32_t normHash = (origHash == 0xafce0c5fu)
                                    ? GetQuestFilterHashForMarkerId(savedId08, false)
                                    : NormalizeFilterTypeHash(origHash);
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

            const bool isCleared = (m[0x34] == 1);
            if (hideClearedOnBigMap && isCleared) continue;

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

            uint32_t displayHash = normHash;
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
                alpha = 0.78f;
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

            drawList.push_back(MinimapMarkerDraw{sx, sy, iconSize, iconRef, tint, alpha, sortCat, isPulsing});
        }
    }

    // 2b. Gold & Silver Chests directly from kChestSpawns (100% flicker-free!)
    if (g_goldChestEnabled || g_silverChestEnabled) {
        const void* goldIconRef = g_goldChestEnabled ? GetIconRefForType(client, kGoldChestTypeHash) : nullptr;
        const void* silverIconRef = g_silverChestEnabled ? GetIconRefForType(client, kSilverChestTypeHash) : nullptr;

        for (size_t c = 0; c < kTotalChestCount; ++c) {
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

            float sx = vcx + dxMeters * pxPerMeterX;
            float sy = vcy - dzMeters * pxPerMeterY;
            if (sx < vx + iconPad * 0.5f || sx > vx + vw - iconPad * 0.5f ||
                sy < vy + iconPad * 0.5f || sy > vy + vh - iconPad * 0.5f) {
                continue;
            }

            float iconSize = (isGold ? 16.5f : 15.5f) * btnScale;
            uint32_t tint = isGold ? 0xFF38C8FFu : 0xFFE8E4DCu;

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

            drawList.push_back(MinimapMarkerDraw{sx, sy, iconSize, iconRef, tint, 0.96f, 3, isPulsing});
        }
    }

    // Sort so POIs/Chests (cat 3) render below Vaults (cat 2), Fast Travel/Spires (cat 1), Quests (cat 0), Custom Pins (cat 4)
    auto catPriority = [](uint8_t cat) -> int {
        switch (cat) {
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
    }

    // Draw other players in multiplayer from client + 0x4642c0 (count at +0x4642c8, stride 0x240)
    if (g_otherPlayersEnabled) {
        uint8_t* playerArray = *reinterpret_cast<uint8_t**>(client + 0x4642c0);
        uint64_t playerCount = *reinterpret_cast<uint64_t*>(client + 0x4642c8);
        uint32_t localPlayerId = *reinterpret_cast<uint32_t*>(client + 0x18);
        const void* otherPlayerIcon = GetIconRefForType(client, 0x26d18a06u);

        if (playerArray && playerCount > 0 && playerCount <= 64) {
            const float clampLimit = radiusMeters * 0.91f;
            for (uint64_t i = 0; i < playerCount; ++i) {
                uint8_t* p = playerArray + i * 0x240;
                uint32_t pid = *reinterpret_cast<uint32_t*>(p + 0x00);
                if (pid == 0 || pid == localPlayerId) continue;
                // p[0x207] != 0 means player is disconnected / hidden (0x140d1369a)
                if (p[0x207] != 0) continue;

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

                // Dark contrast halo + warm sky-blue ring so other players stand out clearly on any terrain
                float haloSz = pSz + 3.0f * btnScale;
                DrawAssetRectAbs(anchor.raw, kCirclePrimitive,
                                 osx - haloSz * 0.5f, osy - haloSz * 0.5f,
                                 haloSz, haloSz, 0xFF0A0E12u, 0.78f * pAlpha, 1.0f);
                if (otherPlayerIcon) {
                    DrawIconRectAbs(anchor.raw, otherPlayerIcon,
                                    osx - pSz * 0.5f, osy - pSz * 0.5f,
                                    pSz, pSz, 0xFFFFFFFFu, pAlpha, 1.12f);
                } else {
                    float dotSz = pSz * 0.65f;
                    DrawAssetRectAbs(anchor.raw, kCirclePrimitive,
                                     osx - dotSz * 0.5f, osy - dotSz * 0.5f,
                                     dotSz, dotSz, 0xFFE8C868u, pAlpha, 1.2f);
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

    // Convert world (X=East, Z=North) to screen 2D (X=Right, Y=Down):
    const float faceFx = faceDirX;
    const float faceFy = -faceDirZ;
    const float facePx = -faceFy;
    const float facePy = faceFx;

    // Compact directional triangle / pointer centered on (vcx, vcy):
    // Dark outline pass (pass 0) followed by crisp warm antique-gold / bright-gold fill pass (pass 1)
    for (int pass = 0; pass < 2; ++pass) {
        const bool isOutline = (pass == 0);
        const float pad = isOutline ? (1.25f * btnScale) : 0.0f;
        const uint32_t wingCol = isOutline ? 0xFF0A0E10u : 0xFF63A1B8u;
        const uint32_t coreCol = isOutline ? 0xFF0A0E10u : 0xFF8CC4DAu;
        const float alpha = isOutline ? 0.95f : 1.0f;
        const float bright = isOutline ? 1.0f : 1.15f;

        for (int i = 0; i <= 10; ++i) {
            float t = static_cast<float>(i) / 10.0f; // 0.0 = rear base (-5.0px), 1.0 = sharp tip (+7.0px)
            float dist = (-5.0f + t * 12.0f) * btnScale;
            float halfW = (1.0f - t) * 4.4f * btnScale;
            float dotSz = ((2.2f - t * 0.7f) * btnScale) + pad * 2.0f;

            // Left & right triangle edges
            for (int side = -1; side <= 1; side += 2) {
                float wx = vcx + faceFx * dist + facePx * (static_cast<float>(side) * halfW);
                float wy = vcy + faceFy * dist + facePy * (static_cast<float>(side) * halfW);
                DrawAssetRectAbs(anchor.raw, kCirclePrimitive,
                                 wx - dotSz * 0.5f, wy - dotSz * 0.5f,
                                 dotSz, dotSz, wingCol, alpha, bright);
            }
            // Solid interior fill & notched rear base (i >= 2 fills from slight chevron notch to tip)
            if (i >= 2) {
                float innerW = halfW * 0.48f;
                for (int side = -1; side <= 1; side += 2) {
                    float ix = vcx + faceFx * dist + facePx * (static_cast<float>(side) * innerW);
                    float iy = vcy + faceFy * dist + facePy * (static_cast<float>(side) * innerW);
                    DrawAssetRectAbs(anchor.raw, kCirclePrimitive,
                                     ix - dotSz * 0.5f, iy - dotSz * 0.5f,
                                     dotSz, dotSz, coreCol, alpha, bright);
                }
                float cx = vcx + faceFx * dist;
                float cy = vcy + faceFy * dist;
                DrawAssetRectAbs(anchor.raw, kCirclePrimitive,
                                 cx - dotSz * 0.5f, cy - dotSz * 0.5f,
                                 dotSz, dotSz, coreCol, alpha, bright);
            }
        }
    }

    // ------------------------------------------------------------------------
    // 3. Draw the Custom Ornate Golden Compass-Rose Frame Over the Viewport
    // ------------------------------------------------------------------------
    const float cellScale = frameSize / kMinimapFrameGrid;
    for (const auto& r : kMinimapFrameRects) {
        float rx = frameX + static_cast<float>(r.x) * cellScale;
        float ry = frameY + static_cast<float>(r.y) * cellScale;
        float rw = static_cast<float>(r.w) * cellScale + 0.16f;
        float rh = static_cast<float>(r.h) * cellScale + 0.16f;
        float rAlpha = static_cast<float>((r.color >> 24) & 0xFFu) / 255.0f;
        DrawSolidRectAbs(anchor.raw, rx, ry, rw, rh, r.color | 0xFF000000u, rAlpha);
    }

    // ------------------------------------------------------------------------
    // 4. Draw Cardinal Directions (N, E, S, W) Inside Viewport
    // ------------------------------------------------------------------------
    DrawLabelAbs(vcx - 12.0f, vy + 1.0f, 24.0f, 16.0f, "N", 0xFFA2EBEFu, 0.98f, TextAlignH::Center, TextAlignV::Center);
    DrawLabelAbs(vcx - 12.0f, vy + vh - 16.0f, 24.0f, 15.0f, "S", 0xFFC8D6DFu, 0.82f, TextAlignH::Center, TextAlignV::Center);
    DrawLabelAbs(vx + 3.0f, vcy - 8.0f, 16.0f, 16.0f, "W", 0xFFC8D6DFu, 0.82f, TextAlignH::Center, TextAlignV::Center);
    DrawLabelAbs(vx + vw - 18.0f, vcy - 8.0f, 16.0f, 16.0f, "E", 0xFFC8D6DFu, 0.82f, TextAlignH::Center, TextAlignV::Center);

    // Highlight resize grip when hovered or actively resizing
    if (hoverResize || g_minimapDraggingResize || shiftAltHeld) {
        const uint32_t gripCol = (hoverResize || g_minimapDraggingResize) ? 0xFFA2EBEFu : 0xFF7EBEC2u;
        DrawSolidRectAbs(anchor.raw, resizeGripX0 + 6.0f, resizeGripY1 - 4.0f, 12.0f, 2.0f, gripCol, 0.92f);
        DrawSolidRectAbs(anchor.raw, resizeGripX1 - 4.0f, resizeGripY0 + 6.0f, 2.0f, 12.0f, gripCol, 0.92f);
    }

    // ------------------------------------------------------------------------
    // 5. Draw In-Game Time of Day Plaque Below the Minimap
    // ------------------------------------------------------------------------
    char timeBuf[96]{};
    bool isDay = true;
    int phase = 1;
    FormatInGameTimeOfDay(client, timeBuf, sizeof(timeBuf), isDay, phase);

    // Outer pale champagne-gold border matching HUD Flame icon & dark slate interior
    DrawSolidRectAbs(anchor.raw, plaqueX - 1.0f, plaqueY - 1.0f, plaqueW + 2.0f, plaqueH + 2.0f, 0xFF7EBEC2u, 0.92f);
    DrawSolidRectAbs(anchor.raw, plaqueX, plaqueY, plaqueW, plaqueH, 0xFF14181Bu, 0.94f);
    DrawSolidRectAbs(anchor.raw, plaqueX + 2.0f, plaqueY + 2.0f, plaqueW - 4.0f, 1.0f, 0xFFA2EBEFu, 0.55f);
    DrawSolidRectAbs(anchor.raw, plaqueX + 2.0f, plaqueY + plaqueH - 3.0f, plaqueW - 4.0f, 1.0f, 0xFF5D9295u, 0.55f);

    // Sun / Moon icon in dedicated left slot (plaqueX + 14), separated from text box (plaqueX + 28)
    const float sunX = plaqueX + 14.0f;
    const float sunY = plaqueY + plaqueH * 0.5f;
    if (isDay) {
        uint32_t sunCol = (phase == 0 || phase == 2) ? 0xFF78C8F0u : 0xFF91D6DAu;
        DrawAssetRectAbs(anchor.raw, kCirclePrimitive, sunX - 6.5f, sunY - 6.5f, 13.0f, 13.0f, sunCol, 0.35f, 1.4f);
        DrawAssetRectAbs(anchor.raw, kCirclePrimitive, sunX - 4.5f, sunY - 4.5f, 9.0f, 9.0f, sunCol, 0.98f, 1.3f);
    } else {
        // Crescent Moon
        DrawAssetRectAbs(anchor.raw, kCirclePrimitive, sunX - 5.0f, sunY - 5.0f, 10.0f, 10.0f, 0xFFFFD296u, 0.95f, 1.2f);
        DrawAssetRectAbs(anchor.raw, kCirclePrimitive, sunX - 2.0f, sunY - 6.0f, 9.0f, 9.0f, 0xFF14181Bu, 0.98f);
    }
    // Subtle vertical divider between Sun/Moon icon and Time text
    DrawSolidRectAbs(anchor.raw, plaqueX + 25.0f, plaqueY + 5.0f, 1.0f, plaqueH - 10.0f, 0xFF7EBEC2u, 0.45f);

    DrawLabelAbs(plaqueX + 28.0f, plaqueY, plaqueW - 32.0f, plaqueH, timeBuf,
                 isDay ? 0xFFC8F6FAu : 0xFFFFE4C4u, 0.96f, TextAlignH::Center, TextAlignV::Center);

    // ------------------------------------------------------------------------
    // 6. Draw External Zoom Controls Below the Time of Day Plaque
    // ------------------------------------------------------------------------
    // [-] Zoom Out button
    const uint32_t minusBorderCol = hoverMinus ? 0xFFA2EBEFu : 0xFF7EBEC2u;
    DrawSolidRectAbs(anchor.raw, minusBtnX, minusBtnY, btnSz, btnSz, hoverMinus ? 0xFF283842u : 0xFF14181Bu, 0.94f);
    DrawSolidRectAbs(anchor.raw, minusBtnX, minusBtnY, btnSz, 1.0f, minusBorderCol, 0.90f);
    DrawSolidRectAbs(anchor.raw, minusBtnX, minusBtnY + btnSz - 1.0f, btnSz, 1.0f, minusBorderCol, 0.90f);
    DrawSolidRectAbs(anchor.raw, minusBtnX, minusBtnY, 1.0f, btnSz, minusBorderCol, 0.90f);
    DrawSolidRectAbs(anchor.raw, minusBtnX + btnSz - 1.0f, minusBtnY, 1.0f, btnSz, minusBorderCol, 0.90f);
    DrawLabelAbs(minusBtnX, minusBtnY, btnSz, btnSz, "-",
                 (g_minimapZoomIdx > 0) ? 0xFFEAF4F8u : 0xFF68747Au, 0.98f, TextAlignH::Center, TextAlignV::Center);

    // Center Zoom Scale Badge ("Zoom: 1.0x")
    char zoomLabelBuf[48]{};
    std::snprintf(zoomLabelBuf, sizeof(zoomLabelBuf), "Zoom: %s", kMinimapZoomLevels[g_minimapZoomIdx].label);
    DrawSolidRectAbs(anchor.raw, zoomBadgeX, zoomBadgeY, zoomBadgeW, btnSz, 0xFF14181Bu, 0.92f);
    DrawSolidRectAbs(anchor.raw, zoomBadgeX, zoomBadgeY, zoomBadgeW, 1.0f, 0xFF7EBEC2u, 0.78f);
    DrawSolidRectAbs(anchor.raw, zoomBadgeX, zoomBadgeY + btnSz - 1.0f, zoomBadgeW, 1.0f, 0xFF7EBEC2u, 0.78f);
    DrawSolidRectAbs(anchor.raw, zoomBadgeX, zoomBadgeY, 1.0f, btnSz, 0xFF7EBEC2u, 0.78f);
    DrawSolidRectAbs(anchor.raw, zoomBadgeX + zoomBadgeW - 1.0f, zoomBadgeY, 1.0f, btnSz, 0xFF7EBEC2u, 0.78f);
    DrawLabelAbs(zoomBadgeX, zoomBadgeY, zoomBadgeW, btnSz, zoomLabelBuf,
                 0xFFD8E6ECu, 0.95f, TextAlignH::Center, TextAlignV::Center);

    // [+] Zoom In button
    const uint32_t plusBorderCol = hoverPlus ? 0xFFA2EBEFu : 0xFF7EBEC2u;
    DrawSolidRectAbs(anchor.raw, plusBtnX, plusBtnY, btnSz, btnSz, hoverPlus ? 0xFF283842u : 0xFF14181Bu, 0.94f);
    DrawSolidRectAbs(anchor.raw, plusBtnX, plusBtnY, btnSz, 1.0f, plusBorderCol, 0.90f);
    DrawSolidRectAbs(anchor.raw, plusBtnX, plusBtnY + btnSz - 1.0f, btnSz, 1.0f, plusBorderCol, 0.90f);
    DrawSolidRectAbs(anchor.raw, plusBtnX, plusBtnY, 1.0f, btnSz, plusBorderCol, 0.90f);
    DrawSolidRectAbs(anchor.raw, plusBtnX + btnSz - 1.0f, plusBtnY, 1.0f, btnSz, plusBorderCol, 0.90f);
    DrawLabelAbs(plusBtnX, plusBtnY, btnSz, btnSz, "+",
                 (g_minimapZoomIdx + 1 < kMinimapZoomCount) ? 0xFFEAF4F8u : 0xFF68747Au, 0.98f, TextAlignH::Center, TextAlignV::Center);

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

} // namespace

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
    g_fogColorSite = FindUnique("66 48 0F 7E D0 F3 0F 10 55 30 66 0F 73 D8 08 C7 00 ?? ?? ?? ?? C7 40 04 ?? ?? ?? ?? C7 40 08 ?? ?? ?? ?? F3 0F 11 50 0C");
    g_fogAlphaSite = FindUnique("C7 85 10 17 00 00 ?? ?? ?? ?? 48 8D 55 60 C7 85 14 17 00 00 ?? ?? ?? ?? 44 8B C0 4C 8B 8D 10 17 00 00 48 8B CE");
    uint8_t* fnPlayStateUiTail = FindUnique("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00 48 8B 01 48 85 C0 0F 84 ?? ?? ?? ?? 4D 89 73 C8 4C 8B 70 38");
    uint8_t* fnBuildWorldMapPlayersTail = FindUnique("48 81 EC 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B F9 4C 8B A0 D0 00 00 00 4C 89 A5 90 00 00 00");

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

    // buildWorldMapPlayers: "push rbp/rsi/rdi/r12/r13/r15; lea rbp, [rsp-48h]" (15 bytes).
    if (fnBuildWorldMapPlayersTail) {
        uint8_t* fn = fnBuildWorldMapPlayersTail - sizeof(kCanonicalBuildWorldMapPlayersPrologue);
        if (StartsAsExpected(fn, kCanonicalBuildWorldMapPlayersPrologue, sizeof(kCanonicalBuildWorldMapPlayersPrologue))) {
            g_origBuildWorldMapPlayers = reinterpret_cast<BuildWorldMapPlayersFn>(
                InstallHook(fn, reinterpret_cast<void*>(&BuildWorldMapPlayersHook), sizeof(kCanonicalBuildWorldMapPlayersPrologue)));
            if (g_origBuildWorldMapPlayers) g_buildWorldMapPlayersHookTarget = fn;
        }
    }

    g_hookInstalled = true;
    if (g_log) {
        g_log("MapKeyPanel: hooks installed%s: drawMapFilterMenu=%p worldMapInput=%p updateViewport=%p getMarkerType=%p playStateUi=%p",
            greeneye::Host() ? " through the Greeneye Loader" : "",
            g_hookTarget, g_worldMapInputHookTarget, g_updateMapViewport, g_getMarkerTypeHookTarget,
            g_playStateHookTarget);
    }
    return true;
}

void UseKeptState(KeptState* kept) {
    g_kept = kept;
}

void PrepareMapKeyPanelForReload() {
    SetMinimapMouseControl(false, nullptr);
    if (g_findDialogOpen) {
        CloseNativeFindDialog();
    }
}

} // namespace unlock_map_markers

// Uses the game's hint-bar layout so these entries stay grouped with its prompts.
struct MapHintSpan { uint8_t* data; uint64_t count; };
using MapHintsFn = void (*)(MapHintSpan*, void*);
using MapHintFn = uint64_t (*)(uint8_t*, void*);
MapHintsFn g_originalMapHints = nullptr;
MapHintFn g_originalMapHint = nullptr;
uint8_t* g_mapHintEntries = nullptr;
constexpr size_t kMapHintSize = 0x48;
constexpr size_t kMapBindingSize = 0xc4;

uint8_t* MapHintRecord(uint8_t action) {
    auto* g = static_cast<uint8_t*>(GetUiG());
    auto* ui = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    return ui ? ui + 0xf27c + action * kMapBindingSize : nullptr;
}

// Embervale Teleport's Travel binding if that mod is installed, read from its own ini as it does (padCount and keyCount 0
// otherwise), so the tip follows a rebind within two seconds.
const inp::Binding& EmbervaleTravelBinding() {
    static inp::Binding travel;
    static inp::FileWatch iniWatch;
    static uint64_t checkedAt = 0;
    static bool installed = false;
    const std::string dir = greeneye::DataPath("..\\map-teleport\\");
    const std::string ini = dir + "map-teleport.ini";
    const uint64_t now = GetTickCount64();
    if (!checkedAt || now - checkedAt >= 2000) {
        checkedAt = now;
        const bool was = installed;
        installed = GetFileAttributesA((dir + "map-teleport.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesA((dir + "mod.json").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!installed) travel = inp::Binding{};
        if (installed && !was) iniWatch = inp::FileWatch{};
    }
    if (installed && iniWatch.Changed(ini.c_str())) {
        // As Embervale: LT+RT unless [Controller] Button picks another (Y was its old default, Off none)
        inp::Binding fallback = inp::MakeBinding("Shift+Mouse1, Pad:LT+RT");
        char button[32]{};
        GetPrivateProfileStringA("Controller", "Button", "Y", button, sizeof(button), ini.c_str());
        if (_stricmp(button, "Y") != 0) {
            inp::Binding picked;
            if (_stricmp(button, "Off") == 0) fallback.padCount = 0;
            else if (inp::ParseBinding(std::string("Pad:") + button, fallback, picked)) fallback = picked;
        }
        char text[128]{};
        GetPrivateProfileStringA("Keys", "Travel", "", text, sizeof(text), ini.c_str());
        if (!inp::ParseBinding(text, fallback, travel)) travel = fallback;
    }
    return travel;
}

// Kinds: 0 mark a chest (click), 1 Find, 2 the panel, 3 copy the position, 4 hide a row (click), 5 undo, 6 redo;
// while the controller steers the Map Key, 7 move, 8 select, 9 back; 10 Embervale Teleport's hold; 11 the dungeon /
// overworld map switch (in a hall, vault or spire).
// What each shows: its current keys, and its controller button if it has one.
inp::Binding MapHintBinding(int kind) {
    inp::Binding b;
    auto withClick = [&](const inp::Binding& modifier) {
        b = inp::MakeBinding("Mouse1");
        if (modifier.keyCount) {
            b.keys[0].ctrl = modifier.keys[0].ctrl;
            b.keys[0].shift = modifier.keys[0].shift;
            b.keys[0].alt = modifier.keys[0].alt;
        }
    };
    switch (kind) {
    case 0:
        withClick(g_keys.chestClick);
        b.pads[0] = g_keys.padMarkChest.pads[0];
        b.padCount = g_keys.padMarkChest.padCount ? 1 : 0;
        break;
    case 4:
        withClick(g_keys.hideRowClick);
        b.pads[0] = g_keys.padHideRow.pads[0];
        b.padCount = g_keys.padHideRow.padCount ? 1 : 0;
        break;
    case 1: b = g_keys.findOpen; break;
    case 2:
        b = g_keys.panelToggle;
        if (!b.padCount) { b.pads[0] = g_keys.padMapKey.pads[0]; b.padCount = g_keys.padMapKey.padCount ? 1 : 0; }
        break;
    case 3: b = g_keys.copyPosition; break;
    case 5: b = g_keys.undo; break;
    case 6: b = g_keys.redo; break;
    case 7: b = g_keys.padDown; break;
    case 8: b = g_keys.padClick; break;
    case 9: b = g_keys.padBack; break;
    case 10: b = EmbervaleTravelBinding(); break;
    case 11: b = g_keys.dungeonToggle; break;
    }
    return b;
}
// The map's hints for the device the game is showing: Find, the panel and the rest with a binding on it.
// Undo and redo while there is a change to take back; the copy with Settings > Advanced > Position on
int MapHintKinds(int out[10]) {
    int n = 0;
    const bool pad = greeneye::hints::PadInUse() || g_padKeyFocus;
    auto add = [&](int kind) {
        if (greeneye::hints::ShowsOn(MapHintBinding(kind), pad)) out[n++] = kind;
    };
    if (g_padKeyFocus) { // the Map Key has the controller: its buttons alone
        add(7);
        add(8);
        add(9);
        return n;
    }
    add(0);
    add(4);
    if (!(halls::MapActive() && !halls::g_mapOverworld)) add(1); // Find: not on a dungeon's own map
    if (halls::g_mapDungeon >= 0) add(11); // the Overworld Map / Dungeon Map button's key
    if (!g_filterUndo.empty() || !g_markUndo.empty()) add(5); // a filter change or a Ctrl + click mark
    if (!g_filterRedo.empty() || !g_markRedo.empty()) add(6);
    add(2);
    add(10);
    if (g_minimapShowPosition) add(3);
    return n;
}
int MapHintCount() {
    int kinds[10];
    return MapHintKinds(kinds);
}
int MapHintKind(int i) {
    int kinds[10];
    return i < MapHintKinds(kinds) ? kinds[i] : 3;
}

uint64_t DrawMapHintWithBinding(uint8_t* entry, void* params, int index) {
    uint8_t* binding = MapHintRecord(entry[0]);
    if (!binding) return g_originalMapHint(entry, params);
    uint8_t saved[kMapBindingSize];
    std::memcpy(saved, binding, sizeof(saved));
    greeneye::hints::WriteRecord(binding, MapHintBinding(MapHintKind(index)));
    uint64_t result = 0;
    __try { result = g_originalMapHint(entry, params); }
    __finally { std::memcpy(binding, saved, sizeof(saved)); }
    return result;
}

uint64_t MapHintHook(uint8_t* entry, void* params) {
    greeneye::Lease lease;
    if (lease && g_mapHintEntries) {
        for (int i = 0; i < MapHintCount(); ++i)
            if (entry == g_mapHintEntries + i * kMapHintSize)
                return DrawMapHintWithBinding(entry, params, i);
    }
    return g_originalMapHint(entry, params);
}

// Move, Select, Back: the words of this language's "D-pad: Move   A: Select   B: Back"
const char* MapKeyNavLabel(int i) {
    static std::vector<std::string> nav;
    static int navLanguage = -1;
    if (const int language = static_cast<int>(umm_lang::Current()); language != navLanguage) {
        navLanguage = language;
        nav = PadStripLabels(umm_lang::T(umm_lang::Str::PadKeyNav));
    }
    return nav.size() == 3 ? nav[i].c_str() : "";
}

void DrawCombinedMapHints(MapHintSpan* span, void* options) {
    alignas(16) uint8_t entries[24 * kMapHintSize]{}; // the game's (up to 16) and ours (up to 8)
    const uint8_t* model = nullptr;
    for (uint64_t i = 0; i < span->count; ++i) {
        const uint8_t* e = span->data + i * kMapHintSize;
        if ((e[0x1a] & 7) == 7) { model = e; break; }
    }
    if (!model) { g_originalMapHints(span, options); return; }
    const umm_lang::Str labels[] = {umm_lang::Str::HintClearChest, umm_lang::Str::HintFind, umm_lang::Str::HintToggleKey,
                                    umm_lang::Str::HintCopyPosition, umm_lang::Str::HintHideFilter,
                                    umm_lang::Str::HintUndoFilter, umm_lang::Str::HintRedoFilter};
    const int count = MapHintCount();
    for (int i = 0; i < count; ++i) {
        uint8_t* e = entries + i * kMapHintSize;
        std::memcpy(e, model, kMapHintSize);
        const int kind = MapHintKind(i);
        UiText text = MakeUiText(kind == 11 ? umm_lang::T(halls::g_mapOverworld ? umm_lang::Str::DungeonMap : umm_lang::Str::OverworldMap)
                                 : kind == 10 ? umm_lang::T(umm_lang::Str::HintTeleport)
                                 : kind >= 7 ? MapKeyNavLabel(kind - 7) : umm_lang::T(labels[kind]));
        std::memcpy(e + 0x20, &text, sizeof(text));
        e[0x1b] = 1;
    }
    std::memcpy(entries + count * kMapHintSize, span->data, span->count * kMapHintSize);
    MapHintSpan combined{entries, span->count + count};
    uint8_t* previous = g_mapHintEntries;
    g_mapHintEntries = entries;
    __try { g_originalMapHints(&combined, options); }
    __finally { g_mapHintEntries = previous; }
}

// The screen stays 5 on the menu's other tabs (Skills, Backpack...): the map's hints only while the World Map itself
// draws (its filter menu, DrawMapFilterMenuHook, within the last few frames)
// (or the Map Key's panel: a dungeon's own map draws no filter menu, and its bar got none of the mod's hints, 2026-11-14)
bool WorldMapTabShowing() {
    return GetTickCount64() - g_mapMenuDrawnTick < 200 || (halls::MapActive() && GetTickCount64() - g_mapKeyLastFrameTick < 200);
}

void MapHintsHook(MapHintSpan* span, void* options) {
    greeneye::Lease lease;
    auto* client = lease && g_worldMapModded ? GetUiClient() : nullptr;
    if (client && MapScreen(client[0x1cd8]) && WorldMapTabShowing() && !g_findDialogOpen && span && span->data && span->count &&
        span->count <= 16 && g_originalMapHint) {
        if (g_deskMapState) { // the map desk first (desk.inl), so the hints' key caps draw over it
            SafeDeskDraw(g_deskMapState);
            g_deskHintsTick = GetTickCount64();
        }
        DrawCombinedMapHints(span, options);
    }
    else g_originalMapHints(span, options);
}

bool MapHintBarWorks() {
    return g_originalMapHint && g_originalMapHints;
}

void InstallNativeMapHints() {
    auto* bar = FindUnique("48 89 5C 24 20 55 56 57 41 54 41 55 48 8D 6C 24 C9 48 81 EC B0 00 00 00");
    auto* one = FindUnique("48 8B C4 55 56 57 41 56 48 8D A8 D8 FD FF FF 48 81 EC 08 03 00 00 F6 41 1A 04");
    if (!bar || !one) { if (g_log) g_log("Map hints: native signatures unavailable"); return; }
    g_originalMapHint = reinterpret_cast<MapHintFn>(InstallHook(one, reinterpret_cast<void*>(&MapHintHook), 15));
    if (g_originalMapHint)
        g_originalMapHints = reinterpret_cast<MapHintsFn>(InstallHook(bar, reinterpret_cast<void*>(&MapHintsHook), 12));
}

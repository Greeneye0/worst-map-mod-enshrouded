// Uses the game's hint-bar layout so these entries stay grouped with its prompts.
struct MapHintSpan { uint8_t* data; uint64_t count; };
using MapHintsFn = void (*)(MapHintSpan*, void*);
using MapHintFn = uint64_t (*)(uint8_t*, void*);
MapHintsFn g_originalMapHints = nullptr;
MapHintFn g_originalMapHint = nullptr;
uint8_t* g_mapHintEntries = nullptr;
constexpr size_t kMapHintSize = 0x48;
constexpr size_t kMapBindingSize = 0xc4;

uint8_t* MapHintBinding(uint8_t action) {
    auto* g = static_cast<uint8_t*>(GetUiG());
    auto* ui = g ? *reinterpret_cast<uint8_t**>(g + 0xd0) : nullptr;
    return ui ? ui + 0xf27c + action * kMapBindingSize : nullptr;
}

// Kinds: 0 Ctrl+click, 1 Shift+F, 2 the panel key, 3 middle click, 4 Alt+click, 5 Ctrl+Z, 6 Ctrl+Y.
// Ctrl+click, Alt+click and Shift+F; Ctrl+Z / Ctrl+Y while there is a filter change to undo / redo; the panel key
// unless it is set to None; middle click (copy X, Z) with Settings > Advanced > Position on
int MapHintKinds(int out[8]) {
    int n = 0;
    out[n++] = 0;
    out[n++] = 4;
    out[n++] = 1;
    if (!g_filterUndo.empty()) out[n++] = 5;
    if (!g_filterRedo.empty()) out[n++] = 6;
    if (g_panelToggleVk) out[n++] = 2;
    if (g_minimapShowPosition) out[n++] = 3;
    return n;
}
int MapHintCount() {
    int kinds[8];
    return MapHintKinds(kinds);
}
int MapHintKind(int i) {
    int kinds[8];
    return i < MapHintKinds(kinds) ? kinds[i] : 3;
}

uint64_t DrawMapHintWithBinding(uint8_t* entry, void* params, int index) {
    uint8_t* binding = MapHintBinding(entry[0]);
    if (!binding) return g_originalMapHint(entry, params);
    uint8_t saved[kMapBindingSize];
    std::memcpy(saved, binding, sizeof(saved));
    // The first 16 bytes describe the keyboard/mouse input and its modifier.
    std::memset(binding, 0, 16);
    auto* words = reinterpret_cast<uint16_t*>(binding);
    index = MapHintKind(index);
    words[0] = (index == 0 || index == 3 || index == 4) ? 2 : 1; // mouse or keyboard
    // F, the panel key (Tab), Z, Y (USB HID key codes)
    words[1] = index == 1 ? 0x09 : index == 2 ? g_panelToggleHid : index == 5 ? 0x1d : index == 6 ? 0x1c : 0;
    if (index == 0 || index == 4) *reinterpret_cast<uint32_t*>(binding + 4) = 1; // left mouse
    if (index == 3) *reinterpret_cast<uint32_t*>(binding + 4) = 3; // middle mouse
    if (index < 2 || index >= 4) {
        words[6] = 1;
        words[7] = index == 1 ? 0xe1 : index == 4 ? 0xe2 : 0xe0; // Shift, Alt, Ctrl
    }
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

void DrawCombinedMapHints(MapHintSpan* span, void* options) {
    alignas(16) uint8_t entries[24 * kMapHintSize]{}; // the game's (up to 16) and ours (up to 7)
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
        UiText text = MakeUiText(umm_lang::T(labels[MapHintKind(i)]));
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

void MapHintsHook(MapHintSpan* span, void* options) {
    greeneye::Lease lease;
    auto* client = lease ? GetUiClient() : nullptr;
    if (client && client[0x1cd8] == 5 && !g_findDialogOpen && span && span->data && span->count && span->count <= 16 && g_originalMapHint)
        DrawCombinedMapHints(span, options);
    else g_originalMapHints(span, options);
}

void InstallNativeMapHints() {
    auto* bar = FindUnique("48 89 5C 24 20 55 56 57 41 54 41 55 48 8D 6C 24 C9 48 81 EC B0 00 00 00");
    auto* one = FindUnique("48 8B C4 55 56 57 41 56 48 8D A8 D8 FD FF FF 48 81 EC 08 03 00 00 F6 41 1A 04");
    if (!bar || !one) { if (g_log) g_log("Map hints: native signatures unavailable"); return; }
    g_originalMapHint = reinterpret_cast<MapHintFn>(InstallHook(one, reinterpret_cast<void*>(&MapHintHook), 15));
    if (g_originalMapHint)
        g_originalMapHints = reinterpret_cast<MapHintsFn>(InstallHook(bar, reinterpret_cast<void*>(&MapHintsHook), 12));
}

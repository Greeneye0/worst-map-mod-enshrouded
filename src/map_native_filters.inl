// Map Key filters for the local development bridge (dev builds only: build-dev.bat defines
// UMM_DEV_BRIDGE). Included at the end of map_key_panel.cpp, after MapNativeControl.
#ifdef UMM_DEV_BRIDGE
namespace {

// Stable names for kCategoryGroups, in order (the panel titles follow the game's language).
constexpr const char* kCategorySlugs[] = {
    "fast_travel", "shroud", "world", "mining", "materials", "plants", "bait", "trees", "quests", "custom_pins",
};
static_assert(sizeof(kCategorySlugs) / sizeof(kCategorySlugs[0]) == kCategoryGroupCount, "one slug per Map Key category");

bool EqualsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return *a == *b;
}

struct JsonOut {
    char* buf;
    size_t cap;
    size_t len = 0;
    void Raw(const char* s) {
        for (; *s; ++s) Char(*s);
    }
    void Char(char c) {
        if (len + 1 < cap) buf[len] = c;
        ++len;
    }
    void Str(const char* s) {
        Char('"');
        for (; *s; ++s) {
            if (*s == '"' || *s == '\\') Char('\\');
            if (static_cast<unsigned char>(*s) >= 0x20) Char(*s);
        }
        Char('"');
    }
    void Bool(bool b) { Raw(b ? "true" : "false"); }
    void Hex(uint32_t v) {
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "\"0x%08x\"", v);
        Raw(tmp);
    }
};

void WriteFiltersJson(uint8_t* client, JsonOut& j) {
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t count = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    j.Raw("{\"panelExpanded\":");
    j.Bool(!g_panelCollapsed);
    j.Raw(",\"categories\":[");
    for (size_t c = 0; c < kCategoryGroupCount; ++c) {
        const auto& grp = kCategoryGroups[c];
        if (c) j.Char(',');
        j.Raw("{\"id\":");
        j.Str(kCategorySlugs[c]);
        j.Raw(",\"title\":");
        j.Str(umm_lang::T(grp.title));
        j.Raw(",\"enabled\":");
        j.Bool(IsCategoryEnabled(entries, count, grp.category));
        j.Raw(",\"collapsed\":");
        j.Bool(g_categoryCollapsed[c]);
        j.Raw(",\"rows\":[");
        if (grp.category == 4) {
            for (size_t p = 0; p < 10; ++p) {
                char name[16];
                std::snprintf(name, sizeof(name), "pin%zu", p + 1);
                if (p) j.Char(',');
                j.Raw("{\"name\":");
                j.Str(name);
                j.Raw(",\"hash\":");
                j.Hex(kCustomPinHashes[p]);
                j.Raw(",\"enabled\":");
                j.Bool(IsFilterEnabled(entries, count, kCustomPinHashes[p]));
                j.Char('}');
            }
        }
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if (r) j.Char(',');
            j.Raw("{\"name\":");
            j.Str(grp.rows[r].label);
            j.Raw(",\"hash\":");
            j.Hex(grp.rows[r].typeHash);
            j.Raw(",\"enabled\":");
            j.Bool(IsFilterEnabled(entries, count, grp.rows[r].typeHash));
            j.Char('}');
        }
        j.Raw("]}");
    }
    j.Raw("]}");
}

// A row: its English label, "pin1".."pin10" or "0x<typeHash>". Sets rowHash (+ pairedHash, pairedHash2).
bool FindFilterRow(const char* target, uint32_t& rowHash, uint32_t& pairedHash, uint32_t& pairedHash2) {
    uint32_t hash = 0;
    if ((target[0] == '0') && (target[1] == 'x' || target[1] == 'X')) hash = std::strtoul(target, nullptr, 16);
    for (size_t p = 0; p < 10; ++p) {
        char name[16];
        std::snprintf(name, sizeof(name), "pin%zu", p + 1);
        if (EqualsNoCase(target, name) || hash == kCustomPinHashes[p]) {
            rowHash = kCustomPinHashes[p];
            pairedHash = 0;
            pairedHash2 = 0;
            return true;
        }
    }
    for (const auto& grp : kCategoryGroups) {
        for (size_t r = 0; r < grp.rowCount; ++r) {
            if ((hash && grp.rows[r].typeHash == hash) || EqualsNoCase(target, grp.rows[r].label)) {
                rowHash = grp.rows[r].typeHash;
                pairedHash = grp.rows[r].pairedHash;
                pairedHash2 = grp.rows[r].pairedHash2;
                return true;
            }
        }
    }
    return false;
}

} // namespace

// Called on the game thread by the authenticated local bridge, like MapNativeControl.
// action: list | on | off (target: all, a category id, or a row) | expand | collapse (target:
// all, panel, or a category id). Writes the Map Key state as JSON to out after any change.
// Returns 1 ok, 0 inactive hot-reload generation, -1 bad action, -2 not on the HUD or world map,
// -3 unknown target, -4 out too small.
extern "C" __declspec(dllexport) int MapNativeFilters(const char* action, const char* target, void* uiClient,
                                                      char* out, uint32_t outCap) {
    greeneye::Lease lease;
    if (!lease) return 0;
    if (!action || !target || !out || outCap == 0) return -1;
    auto* client = static_cast<uint8_t*>(uiClient);
    if (!client || (client[0x1cd8] != 1 && client[0x1cd8] != 5)) return -2;
    LoadMinimapConfig();
    const bool on = std::strcmp(action, "on") == 0, off = std::strcmp(action, "off") == 0;
    const bool expand = std::strcmp(action, "expand") == 0, collapse = std::strcmp(action, "collapse") == 0;
    if (!on && !off && !expand && !collapse && std::strcmp(action, "list") != 0) return -1;
    if (on || off || expand || collapse) {
        size_t cat = kCategoryGroupCount;
        for (size_t c = 0; c < kCategoryGroupCount; ++c) {
            if (EqualsNoCase(target, kCategorySlugs[c])) cat = c;
        }
        const bool all = EqualsNoCase(target, "all");
        if (expand || collapse) {
            if (EqualsNoCase(target, "panel")) g_panelCollapsed = collapse;
            else if (all) for (bool& c : g_categoryCollapsed) c = collapse;
            else if (cat < kCategoryGroupCount) g_categoryCollapsed[cat] = collapse;
            else return -3;
        } else {
            uint32_t rowHash = 0, pairedHash = 0, pairedHash2 = 0;
            if (all) SetAllFiltersState(client, nullptr, on);
            else if (cat < kCategoryGroupCount) SetCategoryFilterState(client, nullptr, kCategoryGroups[cat].category, on);
            else if (FindFilterRow(target, rowHash, pairedHash, pairedHash2)) {
                SetSingleFilterState(client, nullptr, rowHash, on);
                if (pairedHash) SetSingleFilterState(client, nullptr, pairedHash, on);
                if (pairedHash2) SetSingleFilterState(client, nullptr, pairedHash2, on);
            } else return -3;
            g_bridgeRefreshMap = true;
        }
    }
    JsonOut j{out, outCap};
    WriteFiltersJson(client, j);
    if (j.len >= outCap) {
        out[outCap - 1] = 0;
        return -4;
    }
    out[j.len] = 0;
    return 1;
}
#endif

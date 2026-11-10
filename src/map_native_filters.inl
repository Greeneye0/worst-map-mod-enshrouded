// Map Key filters for the local development bridge (dev builds only: build-dev.bat defines
// UMM_DEV_BRIDGE). Included at the end of map_key_panel.cpp, after MapNativeControl.
#ifdef UMM_DEV_BRIDGE
namespace {

// Stable names for kCategoryGroups, in order (the panel titles follow the game's language).
constexpr const char* kCategorySlugs[] = {
    "fast_travel", "shroud", "world", "mining", "materials", "plants", "bait", "trees", "quests", "custom_pins", "creatures", "ground",
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

// Dev: every marker slot of the 4 UI arrays, one line each with the visibility inputs, to markers_dump.txt
// next to the mod log (too big for the bridge buffer). Returns the line count.
struct MarkerDumpRow {
    uint32_t orig, norm, questQ, qid;
    int posExpl, mkExpl, filterOn, stage;
    const uint8_t* quest; // by marker id (started or not)
    const uint8_t* active; // active for this marker
    uint8_t questHead[0x60];
};

void FillMarkerDumpRowInner(uint8_t* client, uint8_t* m, FilterEntry* entries, size_t count, MarkerDumpRow& r) {
    r.stage = 1;
    r.orig = GetMarkerOrigTypeHash(m);
    r.qid = IsSlotSaved(m) ? *reinterpret_cast<uint32_t*>(m + 0x4c) : 0u;
    if (r.qid == 0u) r.qid = *reinterpret_cast<uint32_t*>(m + 0x08);
    r.stage = 2;
    r.norm = r.orig == 0xafce0c5fu ? ClassifyQuestOrRumorMarker(r.qid, m[0x35]) : NormalizeFilterTypeHash(r.orig);
    r.stage = 3;
    r.questQ = GetQuestFilterHashForMarkerId(r.qid, true);
    r.stage = 4;
    const int32_t x = *reinterpret_cast<int32_t*>(m + 0x14), z = *reinterpret_cast<int32_t*>(m + 0x24);
    r.posExpl = IsPositionExploredByCharacter(client, x, z) ? 1 : 0;
    r.stage = 5;
    r.mkExpl = IsMarkerExplored(client, m, r.orig) ? 1 : 0;
    r.stage = 6;
    r.filterOn = IsFilterEnabled(entries, count, r.norm) ? 1 : 0;
    r.stage = 7;
    r.quest = g_getQuestByMarkerId && r.qid ? static_cast<const uint8_t*>(g_getQuestByMarkerId(r.qid)) : nullptr;
    r.active = g_getActiveQuestForMarkerId && r.qid ? static_cast<const uint8_t*>(g_getActiveQuestForMarkerId(r.qid)) : nullptr;
    const uint8_t* q = r.quest ? r.quest : r.active;
    if (q) std::memcpy(r.questHead, q, sizeof(r.questHead));
    r.stage = 0;
}

void FillMarkerDumpRow(uint8_t* client, uint8_t* m, FilterEntry* entries, size_t count, MarkerDumpRow& r) {
    __try {
        FillMarkerDumpRowInner(client, m, entries, count, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int DumpMarkersToFile(uint8_t* client) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, greeneye::DataPath("markers_dump.txt"), "w") != 0 || !fp) return -1;
    auto* filterEntries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    std::fprintf(fp, "arr\ti\tuid\tid08\tid4c\tt30\torig\tnorm\tquestQ\tb34\tb35\tb48\tchg\tx\tz\tposExpl\tmkExpl\tfilterOn\tfault\tname\ttypeName\tquest\tactive\tqname\tqhead\traw\n");
    constexpr size_t kArrays[4] = {0x36ea00, 0x396a28, 0x3bea50, 0x3e6a78};
    for (size_t a = 0; a < 4; ++a) {
        if (g_log) g_log("Dev: marker array %zu count %llu cap %llu", a,
                         static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(client + kArrays[a] + 8)),
                         static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(client + kArrays[a] + 16)));
    }
    int lines = 0;
    for (size_t a = 0; a < 4; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(client + kArrays[a]);
        const uint64_t count = *reinterpret_cast<uint64_t*>(client + kArrays[a] + 8);
        if (!base || count == 0 || count > 10000) continue;
        for (uint64_t i = 0; i < count; ++i) {
            uint8_t* m = base + i * 0xA0;
            MarkerDumpRow r{};
            FillMarkerDumpRow(client, m, filterEntries, filterCount, r);
            char name[160] = "", typeName[96] = "";
            if (r.qid && r.stage == 0) ReadLocaText(r.qid, name, sizeof(name));
            if (r.orig && r.stage == 0) ReadGameMarkerTypeName(r.orig, typeName, sizeof(typeName));
            std::fprintf(fp, "%zu\t%llu\t%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t%08x\t%u\t%02x\t%u\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t",
                         a, static_cast<unsigned long long>(i), *reinterpret_cast<uint32_t*>(m),
                         *reinterpret_cast<uint32_t*>(m + 0x08), *reinterpret_cast<uint32_t*>(m + 0x4c),
                         *reinterpret_cast<uint32_t*>(m + 0x30), r.orig, r.norm, r.questQ, m[0x34], m[0x35], m[0x48],
                         *reinterpret_cast<uint64_t*>(m + 0x40) != 0 ? 1 : 0, *reinterpret_cast<int32_t*>(m + 0x14),
                         *reinterpret_cast<int32_t*>(m + 0x24), r.posExpl, r.mkExpl, r.filterOn, r.stage, name, typeName);
            char questName[160] = "";
            if (r.stage == 0 && (r.quest || r.active)) ReadLocaText(GuidToLocaId(r.questHead), questName, sizeof(questName));
            std::fprintf(fp, "%p\t%p\t%s\t", r.quest, r.active, questName);
            for (int b = 0; b < 0x60; ++b) std::fprintf(fp, "%02x", (r.quest || r.active) ? r.questHead[b] : 0);
            std::fputc('\t', fp);
            for (int b = 0; b < 0xA0; ++b) std::fprintf(fp, "%02x", m[b]);
            std::fputc('\n', fp);
            ++lines;
        }
    }
    std::fclose(fp);
    return lines;
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
    if (std::strcmp(action, "hdstatus") == 0 || std::strcmp(action, "hdtest") == 0)
        return hdpool::Control(action, target, out, outCap);
    auto* client = static_cast<uint8_t*>(uiClient);
    if (!client || (client[0x1cd8] != 1 && !MapScreen(client[0x1cd8]))) return -2;
    LoadMinimapConfig();
    // A click's effect without the mouse, target "x,z,r": ctrlclicklore does what Ctrl + click does to the nearest lore
    // note (the notes are this player's own, nothing goes to other players); altclick what Alt + click hides.
    if (std::strcmp(action, "ctrlclicklore") == 0 || std::strcmp(action, "altclick") == 0) {
        float x = 0.0f, z = 0.0f, r = 0.0f;
        if (sscanf_s(target, "%f,%f,%f", &x, &z, &r) != 3) return -3;
        bool done = false;
        if (action[0] == 'c') {
            done = ToggleLoreReadNear(x, z, r, 0.0f, 0.0f);
        } else {
            HideRowTarget row;
            if (HideRowTargetAt(client, static_cast<int32_t>(x), static_cast<int32_t>(z), r, 0u, row)) {
                const FilterSnapshot before = TakeFilterSnapshot(client);
                for (uint32_t h : row.hashes) {
                    if (h != 0u) SetSingleFilterState(client, nullptr, h, false);
                }
                AnnounceFilterChange(before, TakeFilterSnapshot(client));
                done = true;
            }
        }
        const int n = std::snprintf(out, outCap, "{\"done\":%s}", done ? "true" : "false");
        return n > 0 && static_cast<uint32_t>(n) < outCap ? 1 : -4;
    }
    // Filter presets: target "apply:N", "save:N", "name:N:text" (empty text: the default's name) or "status"; N is 1-5
    if (std::strcmp(action, "preset") == 0) {
        LoadFilterPresets();
        int p = -1;
        if (std::strncmp(target, "apply:", 6) == 0) {
            p = std::atoi(target + 6) - 1;
            if (p < 0 || p >= kPresetCount) return -3;
            ApplyFilterPreset(client, nullptr, p);
        } else if (std::strncmp(target, "save:", 5) == 0) {
            p = std::atoi(target + 5) - 1;
            if (p < 0 || p >= kPresetCount) return -3;
            StoreFilterPreset(client, p);
        } else if (std::strncmp(target, "name:", 5) == 0) {
            p = std::atoi(target + 5) - 1;
            const char* colon = std::strchr(target + 5, ':');
            if (p < 0 || p >= kPresetCount || !colon) return -3;
            g_presets[p].name = colon + 1;
            SaveFilterPreset(p);
        } else if (std::strncmp(target, "clear:", 6) == 0) { // back to the built-in default
            p = std::atoi(target + 6) - 1;
            if (p < 0 || p >= kPresetCount) return -3;
            g_presets[p] = FilterPreset{};
            SaveFilterPreset(p);
        } else if (std::strcmp(target, "status") != 0) {
            return -3;
        }
        std::string json = "{\"match\":" + std::to_string(MatchingFilterPreset(client) + 1) + ",\"slots\":[";
        for (int i = 0; i < kPresetCount; ++i) {
            json += (i ? "," : "") + std::string("{\"name\":\"") + PresetName(i) + "\",\"saved\":" +
                    std::to_string(g_presets[i].states.size()) + "}";
        }
        json += "],\"notice\":\"" + g_filterNotice + "\"}";
        const int n = std::snprintf(out, outCap, "%s", json.c_str());
        return n > 0 && static_cast<uint32_t>(n) < outCap ? 1 : -4;
    }
    if (std::strcmp(action, "markers") == 0) {
        g_dumpMarkersRequested = true; // in the world map's marker pass, where the quest lookups work
        const int n = std::snprintf(out, outCap, "{\"file\":\"markers_dump.txt\",\"requested\":true}");
        return n > 0 && static_cast<uint32_t>(n) < outCap ? 1 : -4;
    }
    if (std::strcmp(action, "find") == 0 || std::strcmp(action, "findhidden") == 0) { // needs the world map open; findresult answers next
        g_devFindHidden = std::strcmp(action, "findhidden") == 0;
        g_devFindQuery = target;
        g_devFindResult = "{\"pending\":true}";
        g_devFindPending = true;
        const int n = std::snprintf(out, outCap, "{\"queued\":true}");
        return n > 0 && static_cast<uint32_t>(n) < outCap ? 1 : -4;
    }
    if (std::strcmp(action, "findresult") == 0) {
        const int n = std::snprintf(out, outCap, "%s", g_devFindResult.c_str());
        return n > 0 && static_cast<uint32_t>(n) < outCap ? 1 : -4;
    }
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

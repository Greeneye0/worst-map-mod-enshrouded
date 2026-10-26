// Region focus: the world's regions (regions.h, from the game's biome map) and the World Map showing one of them.
// Pins outside the focused region are dimmed or hidden; the region's border is drawn on the map. Included by
// map_key_panel.cpp.

// Map Key > Region: -1 all regions, else a region (kRegionCount order: by level)
int g_regionFocus = -1;
bool g_regionOutsideHide = false; // false: dim pins outside the region, true: hide them

// The region at world (x, z) metres, or -1 off the map
int RegionAt(float x, float z) {
    if (!(x >= 0.0f && z >= 0.0f)) return -1;
    const int cx = static_cast<int>(x) / kRegionCellMeters, cz = static_cast<int>(z) / kRegionCellMeters;
    if (cx >= kRegionGridSize || cz >= kRegionGridSize) return -1;
    int at = 0;
    for (uint32_t r = kRegionRowStart[cz]; r < kRegionRowStart[cz + 1]; ++r) {
        at += kRegionRuns[r].cells;
        if (cx < at) return kRegionRuns[r].region;
    }
    return -1;
}

// The region the World Map shows this frame (-1: all)
int g_focusedRegion = -1;
void UpdateFocusedRegion(uint8_t*) {
    g_focusedRegion = g_regionFilterEnabled && g_regionFocus >= 0 && g_regionFocus < static_cast<int>(kRegionCount)
                          ? g_regionFocus
                          : -1;
}

// Settings > Advanced > Region Colors: each region tinted in its own colour under the World Map's markers. Sampled on a
// grid of 8 px screen cells and drawn as one strip per run of a region in a row (a few hundred draws), like the
// zoomed-out forests.
constexpr uint32_t kRegionTint[kRegionCount] = {
    0xff5ac87au, // Springlands: green
    0xff3a8a3au, // Revelwood: forest green
    0xffa0508cu, // Blackmire: purple
    0xff4aa0c8u, // Nomad Highlands: ochre
    0xff2a9ae8u, // Kindlewastes: orange
    0xfff0e0c8u, // Albaneve Summits: ice blue
    0xffb4a03cu, // Veilwater Basin: teal
    0xff4646d2u, // Cinder's Cradle: red
};

void DrawRegionColorsOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!g_regionColors || !widgetRaw || !mapState) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1e0) + *reinterpret_cast<float*>(mapView + 0x17c) -
                          *reinterpret_cast<float*>(mapView + 0x1e8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1e4) + *reinterpret_cast<float*>(mapView + 0x180) -
                          *reinterpret_cast<float*>(mapView + 0x1ec);
    const float left = rootRect[0], top = rootRect[1] + 114.0f; // below the menu banner
    const float right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    constexpr float kCell = 8.0f;
    for (float y = top; y < bottom; y += kCell) {
        const float wz = mapWorldH - (y + kCell * 0.5f - originY) / zoom;
        int runRegion = -1;
        float runX = left;
        for (float x = left; x <= right; x += kCell) {
            const int r = x < right ? RegionAt((x + kCell * 0.5f - originX) / zoom, wz) : -1;
            if (r == runRegion) continue;
            if (runRegion >= 0)
                DrawSolidRectAbs(widgetRaw, runX, y, x - runX, kCell, kRegionTint[runRegion],
                                 g_focusedRegion >= 0 && runRegion != g_focusedRegion ? 0.08f : 0.2f);
            runRegion = r;
            runX = x;
        }
    }
}

// Outside the focused region: dimmed or hidden
bool OutsideFocusedRegion(float x, float z) {
    return g_focusedRegion >= 0 && RegionAt(x, z) != g_focusedRegion;
}

// Outside it and hidden (Hide, not Dim)
bool RegionHidesAt(float x, float z) { return g_regionOutsideHide && OutsideFocusedRegion(x, z); }

// A region's name: the game's (the journal's region heading) in its language, else English
const char* RegionName(int region) {
    static char names[kRegionCount][64]{};
    static uint8_t state[kRegionCount]{}; // 0 not read yet, 1 the game's, 2 English
    static int lang = -1;
    if (region < 0 || region >= static_cast<int>(kRegionCount)) return "";
    const int nowLang = static_cast<int>(umm_lang::Current());
    if (lang != nowLang) {
        std::memset(state, 0, sizeof(state));
        lang = nowLang;
    }
    if (!state[region]) {
        const bool game = kRegionNameLoca[region] && umm_lang::GameTextInCurrentLanguage() &&
                          ReadLocaText(kRegionNameLoca[region], names[region], sizeof(names[region]));
        state[region] = game ? 1 : 2;
    }
    return state[region] == 1 ? names[region] : kRegionNameEnglish[region];
}

// One of the game's World Map markers outside the focused region (your own pins and quest pins always show)
bool MarkerOutsideFocusedRegion(const uint8_t* m) {
    if (g_focusedRegion < 0 || !m) return false;
    const uint32_t typeHash = *reinterpret_cast<const uint32_t*>(m + 0x30);
    const uint32_t origHash = GetMarkerOrigTypeHash(m);
    for (uint32_t ph : kCustomPinHashes) {
        if (typeHash == ph || origHash == ph) return false;
    }
    if (origHash == 0xafce0c5fu || origHash == 0x454e24f2u) return false; // quest pin / quest area
    return OutsideFocusedRegion(static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)),
                                static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24)));
}

// The focused region's border on the World Map: gold beads evenly spaced on screen (one draw each: the game's UI
// renderer drops the map's own tiles past a few thousand draws a frame, shared with the deposits)
void DrawRegionBorderOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (g_focusedRegion < 0 || !widgetRaw || !mapState) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1e0) + *reinterpret_cast<float*>(mapView + 0x17c) -
                          *reinterpret_cast<float*>(mapView + 0x1e8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1e4) + *reinterpret_cast<float*>(mapView + 0x180) -
                          *reinterpret_cast<float*>(mapView + 0x1ec);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    constexpr float kSpacing = 9.0f, kBead = 4.2f;
    constexpr int kMaxBeads = 700;
    int beads = 0;
    for (size_t o = 0; o < kRegionOutlineCount && beads < kMaxBeads; ++o) {
        const RegionOutline& line = kRegionOutlines[o];
        if (line.region != g_focusedRegion) continue;
        float carry = 0.0f; // screen distance walked since the last bead
        for (uint32_t p = line.first; p + 1 < line.first + line.count && beads < kMaxBeads; ++p) {
            const float ax = originX + kRegionOutlinePoints[p][0] * zoom;
            const float ay = originY + (mapWorldH - kRegionOutlinePoints[p][1]) * zoom;
            const float bx = originX + kRegionOutlinePoints[p + 1][0] * zoom;
            const float by = originY + (mapWorldH - kRegionOutlinePoints[p + 1][1]) * zoom;
            const float len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            if (len <= 0.0f) continue;
            // Off screen: walk on without drawing
            if (std::max(ax, bx) < left - 8.0f || std::min(ax, bx) > right + 8.0f || std::max(ay, by) < top - 8.0f ||
                std::min(ay, by) > bottom + 8.0f) {
                carry = std::fmod(carry + len, kSpacing);
                continue;
            }
            for (float t = kSpacing - carry; t <= len && beads < kMaxBeads; t += kSpacing) {
                const float x = ax + (bx - ax) * t / len, y = ay + (by - ay) * t / len;
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, x - kBead * 0.5f, y - kBead * 0.5f, kBead, kBead,
                                 0xFF9AD3E9u, 0.95f);
                ++beads;
            }
            carry = std::fmod(carry + len, kSpacing);
        }
    }
}

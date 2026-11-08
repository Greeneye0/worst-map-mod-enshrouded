// Region focus: the world's regions (regions.h, from the game's biome map) and the World Map showing one of them.
// Pins outside the focused region are dimmed or hidden; the region's border is drawn on the map. Included by
// map_key_panel.cpp.

// Map Key > Regions: the regions shown, a bit each (kRegionCount order: by level); all of them: no region filter
constexpr uint32_t kAllRegionsMask = (1u << kRegionCount) - 1u;
uint32_t g_regionMask = kAllRegionsMask;
bool g_regionDrawerOpen = true;   // the Regions drawer under the Map Key's Region row is unfolded
bool g_regionOutsideHide = false; // false: dim pins outside the regions, true: hide them

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

// The regions the World Map shows this frame; g_regionFiltering: not all of them (Settings > Advanced > Region Filter)
bool g_regionFiltering = false;
uint32_t g_focusedRegions = kAllRegionsMask;
void UpdateFocusedRegion(uint8_t*) {
    g_regionMask &= kAllRegionsMask;
    g_regionFiltering = g_regionFilterEnabled && g_regionMask != kAllRegionsMask;
    g_focusedRegions = g_regionFiltering ? g_regionMask : kAllRegionsMask;
}
bool RegionShown(int region) {
    return !g_regionFiltering || (region >= 0 && region < static_cast<int>(kRegionCount) && (g_focusedRegions >> region) & 1u);
}

// Each region's colour: its swatch in the Map Key's Region rows
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

// Outside the regions shown: dimmed or hidden
bool OutsideFocusedRegion(float x, float z) {
    return g_regionFiltering && !RegionShown(RegionAt(x, z));
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

// The UI renderer's current opacity (the top of its opacity stack; ui_mesh.h reads it the same way): the map's
// place-name style ignores a label's colour and alpha, so a faint one is drawn with this lowered
float* UiOpacityTop(void* widget) {
    auto handle = ui_mesh::Read<void*>(widget, 8);
    auto state = handle ? ui_mesh::Read<void*>(handle, 0x50) : nullptr;
    state = state ? ui_mesh::Read<void*>(state, 0x18) : nullptr;
    if (!state || ui_mesh::Read<uint32_t>(state, 8) != 4) return nullptr;
    auto renderer = ui_mesh::Read<uint8_t*>(state, 0x6ce0);
    if (!renderer) return nullptr;
    const auto count = ui_mesh::Read<uint64_t>(renderer, 0xc0);
    auto stack = ui_mesh::Read<float*>(renderer, 0xb8);
    return count && count < 64 && stack ? stack + count - 1 : nullptr;
}

// A place-name label at `faint` of the current opacity, put back even if drawing it faults (a lowered opacity left
// behind would fade the whole UI)
void DrawFaintPlaceName(void* widget, float x, float y, float w, float h, const char* name, float scale, float faint) {
    float* opacity = UiOpacityTop(widget);
    const float was = opacity ? *opacity : 1.0f;
    if (opacity) *opacity = was * faint;
    __try {
        DrawLabelAbs(x, y, w, h, name, 0xffffffffu, 1.0f, TextAlignH::Center, TextAlignV::Center,
                     GetUiAssets() ? GetUiAssets() + kMapLocationNameStyleOffset : nullptr, scale);
    } __finally {
        if (opacity) *opacity = was;
    }
}

// Settings > Advanced > Region Names: each region's name on the World Map as a faint watermark (the map's own spaced,
// outlined place-name capitals, mostly see-through, under the markers), at the middle of the region's land (its
// grid also covers the sea around it, which would pull the Springlands' name out over the water): its cells' centre, or the region's cell nearest it when that
// falls outside (a curved region)
int LevelAreaAt(float x, float z); // levels.inl: -1 off the land
void DrawRegionNamesOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    static float centre[kRegionCount][2]{};
    static bool built = false;
    if (!built) {
        built = true;
        double sx[kRegionCount]{}, sz[kRegionCount]{}, n[kRegionCount]{};
        for (int cz = 0; cz < kRegionGridSize; ++cz) {
            int at = 0;
            for (uint32_t r = kRegionRowStart[cz]; r < kRegionRowStart[cz + 1]; ++r) {
                const int reg = kRegionRuns[r].region, cells = kRegionRuns[r].cells;
                if (reg >= 0 && reg < static_cast<int>(kRegionCount))
                    for (int cx = at; cx < at + cells; ++cx) {
                        if (LevelAreaAt((cx + 0.5f) * kRegionCellMeters, (cz + 0.5f) * kRegionCellMeters) < 0) continue;
                        sx[reg] += cx + 0.5;
                        sz[reg] += cz + 0.5;
                        n[reg] += 1;
                    }
                at += cells;
            }
        }
        for (size_t reg = 0; reg < kRegionCount; ++reg) {
            if (!n[reg]) continue;
            float x = static_cast<float>(sx[reg] / n[reg] * kRegionCellMeters), z = static_cast<float>(sz[reg] / n[reg] * kRegionCellMeters);
            if (RegionAt(x, z) != static_cast<int>(reg)) {
                float best = 1e30f;
                for (int cz = 0; cz < kRegionGridSize; ++cz)
                    for (int cx = 0; cx < kRegionGridSize; ++cx) {
                        const float wx = (cx + 0.5f) * kRegionCellMeters, wz = (cz + 0.5f) * kRegionCellMeters;
                        const float d = (wx - x) * (wx - x) + (wz - z) * (wz - z);
                        if (d < best && RegionAt(wx, wz) == static_cast<int>(reg) && LevelAreaAt(wx, wz) >= 0)
                            best = d, centre[reg][0] = wx, centre[reg][1] = wz;
                    }
            } else {
                centre[reg][0] = x;
                centre[reg][1] = z;
            }
        }
    }
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!widgetRaw || !mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1f0) + *reinterpret_cast<float*>(mapView + 0x18c) -
                          *reinterpret_cast<float*>(mapView + 0x1f8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1f4) + *reinterpret_cast<float*>(mapView + 0x190) -
                          *reinterpret_cast<float*>(mapView + 0x1fc);
    const float left = rootRect[0], top = rootRect[1] + 114.0f;
    const float right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    // Painted on the map: about 300 m of world tall, so they grow and shrink with the terrain as you zoom
    // Zoomed in further than that (a name over 20x the text size) you are well inside one region: none, and the
    // game's text faults on far bigger sizes (that froze the whole map's overlay)
    const float kScale = 300.0f * zoom / 28.0f;
    if (!(kScale > 0.2f && kScale <= 20.0f)) return;
    for (size_t reg = 0; reg < kRegionCount; ++reg) {
        if (centre[reg][0] == 0.0f && centre[reg][1] == 0.0f) continue;
        const bool shown = RegionShown(static_cast<int>(reg));
        if (!shown && g_regionOutsideHide) continue;
        const float cx = originX + centre[reg][0] * zoom, cy = originY + (mapWorldH - centre[reg][1]) * zoom;
        if (cx < left || cx > right || cy < top || cy > bottom) continue;
        const char* name = RegionName(static_cast<int>(reg));
        const float w = umm_lang::TextWidth(name) * 1.6f * kScale + 32.0f, h = 28.0f * kScale;
        DrawFaintPlaceName(widgetRaw, cx - w * 0.5f, cy - h * 0.5f, w, h, name, kScale, shown ? 0.28f : 0.1f);
    }
}

namespace halls {
bool MapHides(float x, float z); // halls.inl: another dungeon's marker while the map shows a Hollow Hall
bool MapHidesChest(size_t chest);
}

// A marker in another Hollow Hall than the one the map shows. The mod's chest markers go by their hall: gold and silver
// chests in another hall can be right over or under this one's floors.
bool MarkerInOtherHall(const uint8_t* m) {
    const uint32_t uid = *reinterpret_cast<const uint32_t*>(m);
    if ((uid & 0xFFFF0000u) == kChestMarkerUidPrefix) {
        const size_t chest = (uid & 0xFFFFu) - 1u;
        if (chest < kTotalChestCount) return halls::MapHidesChest(chest);
    }
    return halls::MapHides(static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14)),
                           static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24)));
}

// One of the game's World Map markers outside the focused region, or in another Hollow Hall than the one the map
// shows (your own pins and quest pins always show)
bool MarkerOutsideFocusedRegion(const uint8_t* m) {
    if (!m) return false;
    const float mx = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x14));
    const float mz = static_cast<float>(*reinterpret_cast<const int32_t*>(m + 0x24));
    if (MarkerInOtherHall(m)) return true; // the dungeon map shows that dungeon only, your pins too
    if (!g_regionFiltering) return false;
    const uint32_t typeHash = *reinterpret_cast<const uint32_t*>(m + 0x30);
    const uint32_t origHash = GetMarkerOrigTypeHash(m);
    for (uint32_t ph : kCustomPinHashes) {
        if (typeHash == ph || origHash == ph) return false;
    }
    if (origHash == 0xafce0c5fu || origHash == 0x454e24f2u) return false; // quest pin / quest area
    return OutsideFocusedRegion(mx, mz);
}

// Not drawn at all: outside the focused region with Hide, or in another Hollow Hall
bool MarkerHiddenOnMap(const uint8_t* m) {
    if (!MarkerOutsideFocusedRegion(m)) return false;
    return g_regionOutsideHide || MarkerInOtherHall(m);
}

// The shown regions' borders on the World Map: gold beads evenly spaced on screen (one draw each: the game's UI
// renderer drops the map's own tiles past a few thousand draws a frame, shared with the deposits)
void DrawRegionBorderOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!g_regionFiltering || g_focusedRegions == 0u || !widgetRaw || !mapState) return;
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
    constexpr float kSpacing = 9.0f, kBead = 4.2f;
    constexpr int kMaxBeads = 700;
    int beads = 0;
    for (size_t o = 0; o < kRegionOutlineCount && beads < kMaxBeads; ++o) {
        const RegionOutline& line = kRegionOutlines[o];
        if (!RegionShown(line.region)) continue;
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

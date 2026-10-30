// Settings > Advanced > Level Areas: the World Map shows the areas of the enemies' levels (levels.h, from the levels
// the game gives its placed objects), each in a colour from green (low) through orange and red to purple and blue
// (high), with dark borders and its level in a box. Drawn over the map (it took the place of Region Colors).
// Included by map_key_panel.cpp after regions.inl.

// The level area at world (x, z) metres, or -1 off the land
int LevelAreaAt(float x, float z) {
    if (!(x >= 0.0f && z >= 0.0f)) return -1;
    const int cx = static_cast<int>(x) / kLevelCellMeters, cz = static_cast<int>(z) / kLevelCellMeters;
    if (cx >= kLevelGridSize || cz >= kLevelGridSize) return -1;
    int at = 0;
    for (uint32_t r = kLevelRowStart[cz]; r < kLevelRowStart[cz + 1]; ++r) {
        at += kLevelRuns[r].cells;
        if (cx < at) return static_cast<int>(kLevelRuns[r].area) - 1;
    }
    return -1;
}

// A level's colour (0xAABBGGRR): blended between stops
uint32_t LevelAreaColor(int area) {
    struct Stop {
        int level;
        uint8_t r, g, b;
    };
    static constexpr Stop kStops[] = {{1, 0x8f, 0xbf, 0x6a},  {9, 0xc8, 0xc0, 0x5a},  {15, 0xd8, 0xa8, 0x48},
                                      {21, 0xe0, 0x88, 0x48}, {27, 0xc8, 0x60, 0x48}, {32, 0xd0, 0x68, 0x7c},
                                      {38, 0xa8, 0x70, 0xb8}, {44, 0x80, 0x78, 0xc8}, {48, 0x68, 0xa8, 0xd0}};
    if (area < 0 || area >= static_cast<int>(kLevelAreaCount)) return 0xff808080u;
    const int level = kLevelAreaLevels[area];
    const Stop* hi = kStops;
    while (hi + 1 < std::end(kStops) && hi->level < level) ++hi;
    const Stop* lo = hi > kStops ? hi - 1 : hi;
    const float t = hi->level > lo->level ? static_cast<float>(level - lo->level) / (hi->level - lo->level) : 0.0f;
    auto mix = [&](uint8_t a, uint8_t b) { return static_cast<uint32_t>(a + (b - a) * std::min(1.0f, std::max(0.0f, t))); };
    return 0xff000000u | mix(lo->b, hi->b) << 16 | mix(lo->g, hi->g) << 8 | mix(lo->r, hi->r);
}

// The World Map's view: zoom (px per metre) and where world (0, mapWorldH) sits on screen
bool LevelMapView(uint8_t* mapState, float& zoom, float& originX, float& originY, float& mapWorldH) {
    if (!mapState) return false;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return false;
    zoom = *reinterpret_cast<float*>(mapView + 0x10);
    mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    originX = *reinterpret_cast<float*>(mapView + 0x1e0) + *reinterpret_cast<float*>(mapView + 0x17c) -
              *reinterpret_cast<float*>(mapView + 0x1e8);
    originY = *reinterpret_cast<float*>(mapView + 0x1e4) + *reinterpret_cast<float*>(mapView + 0x180) -
              *reinterpret_cast<float*>(mapView + 0x1ec);
    return zoom > 0.0f;
}

// The tint: sampled on a grid of screen cells (8 px, up to a level cell's size zoomed in: the UI renderer drops draws
// past a few thousand a frame), one strip per run of an area in a row
void DrawLevelAreasOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!g_levelAreas || !widgetRaw) return;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH)) return;
    const float left = rootRect[0], top = rootRect[1] + 114.0f; // below the menu banner
    const float right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    const float kCell = std::max(8.0f, std::min(24.0f, kLevelCellMeters * zoom));
    for (float y = top; y < bottom; y += kCell) {
        const float wz = mapWorldH - (y + kCell * 0.5f - originY) / zoom;
        int runArea = -1;
        float runX = left;
        for (float x = left; x <= right; x += kCell) {
            const int a = x < right ? LevelAreaAt((x + kCell * 0.5f - originX) / zoom, wz) : -1;
            if (a == runArea) continue;
            if (runArea >= 0) DrawSolidRectAbs(widgetRaw, runX, y, x - runX, kCell, LevelAreaColor(runArea), 0.5f);
            runArea = a;
            runX = x;
        }
    }
}

// The borders (dark beads evenly spaced on screen) and each area's level in a light box
void DrawLevelBordersOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!g_levelAreas || !widgetRaw) return;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH)) return;
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    constexpr float kBead = 3.4f;
    constexpr int kMaxBeads = 900; // the UI renderer's budget, shared with the deposits
    // A segment on screen (below the menu banner), in screen pixels
    auto segment = [&](uint32_t p, float& ax, float& ay, float& bx, float& by) {
        ax = originX + kLevelOutlinePoints[p][0] * zoom;
        ay = originY + (mapWorldH - kLevelOutlinePoints[p][1]) * zoom;
        bx = originX + kLevelOutlinePoints[p + 1][0] * zoom;
        by = originY + (mapWorldH - kLevelOutlinePoints[p + 1][1]) * zoom;
        return !(std::max(ax, bx) < left - 8.0f || std::min(ax, bx) > right + 8.0f || std::max(ay, by) < top + 114.0f ||
                 std::min(ay, by) > bottom + 8.0f);
    };
    // Every border on screen gets its beads: 7 px apart, wider when that would run over the budget
    float shown = 0.0f;
    for (size_t o = 0; o < kLevelOutlineCount; ++o) {
        const LevelOutline& line = kLevelOutlines[o];
        for (uint32_t p = line.first; p + 1 < line.first + line.count; ++p) {
            float ax, ay, bx, by;
            if (segment(p, ax, ay, bx, by)) shown += std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        }
    }
    const float spacing = std::max(7.0f, shown / kMaxBeads);
    int beads = 0;
    for (size_t o = 0; o < kLevelOutlineCount && beads < kMaxBeads; ++o) {
        const LevelOutline& line = kLevelOutlines[o];
        float carry = 0.0f;
        for (uint32_t p = line.first; p + 1 < line.first + line.count && beads < kMaxBeads; ++p) {
            float ax, ay, bx, by;
            const bool onScreen = segment(p, ax, ay, bx, by);
            const float len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            if (len <= 0.0f) continue;
            if (onScreen) {
                for (float t = spacing - carry; t <= len && beads < kMaxBeads; t += spacing) {
                    const float x = ax + (bx - ax) * t / len, y = ay + (by - ay) * t / len;
                    if (y < top + 114.0f) continue;
                    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, x - kBead * 0.5f, y - kBead * 0.5f, kBead, kBead,
                                     0xff202020u, 0.75f);
                    ++beads;
                }
            }
            carry = std::fmod(carry + len, spacing);
        }
    }
    // The levels: a dark badge per area piece, on its point deepest inside, its frame in the area's colour
    for (size_t i = 0; i < kLevelLabelCount; ++i) {
        const LevelLabel& l = kLevelLabels[i];
        const float x = originX + l.x * zoom, y = originY + (mapWorldH - l.z) * zoom;
        constexpr float kW = 48.0f, kH = 34.0f;
        if (x < left + kW || x > right - kW || y < top + 114.0f + kH || y > bottom - kH) continue;
        char text[8];
        std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(kLevelAreaLevels[l.area]));
        DrawFramedBoxAbs(widgetRaw, x - kW * 0.5f, y - kH * 0.5f, kW, kH, 0xff141414u, 0.88f, LevelAreaColor(l.area), 1.0f,
                         5.0f);
        DrawLabelAbs(x - kW * 0.5f, y - kH * 0.5f, kW, kH, text, 0xffffffffu, 1.0f, TextAlignH::Center, TextAlignV::Center,
                     nullptr, 1.5f);
    }
}

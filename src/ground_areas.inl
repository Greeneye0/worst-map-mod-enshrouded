// Included in map_key_panel.cpp's private namespace, after hd_pool.inl.
// Area layers on the World Map: the forests (Trees rows, kTreeShapeRects) and the Ground rows, the regional rock and
// sand (Granite, Basalt, Shell Limestone...). data/ground.bin (tools/resources/make_resources.py) holds each Ground row's share of the ground in 16 m
// cells. The enabled rows are painted into images of the whole world (4 x 4 tiles of 256 px, 10 m a pixel) and drawn
// as 16 images through the GPU pool, so they cost the UI renderer a handful of draws however much ground they cover
// (the forests were thousands of rectangles; DrawTreeAreasOnMap still draws their names).
namespace ground {
constexpr int kTiles = 4, kTilePx = 256, kWorldPx = kTiles * kTilePx;
constexpr float kMetresPerPx = 10240.0f / kWorldPx;

int g_cells = 0, g_cellMetres = 16;
std::vector<std::vector<uint8_t>> g_cover;  // per Ground row (kGroundFirst + i): cells [z * g_cells + x], z rising north
std::vector<std::vector<uint8_t>> g_forest; // per Trees row (kTreesFirst + i): its trees per cell, the same way
bool g_tried = false;
uint64_t g_dataHash = 0; // ground.bin's contents: the images' keys change with the data (the pool keeps images across reloads)

bool Load() {
    if (g_tried) return g_cells != 0;
    g_tried = true;
    const std::string path = greeneye::DataPath("ground.bin");
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (g_log) g_log("Ground: %s not found; no ground areas", path.c_str());
        return false;
    }
    std::vector<uint8_t> d;
    LARGE_INTEGER size{};
    DWORD got = 0;
    const bool read = GetFileSizeEx(f, &size) && size.QuadPart > 10 && size.QuadPart < (16ll << 20) &&
                      (d.resize(static_cast<size_t>(size.QuadPart)), true) &&
                      ReadFile(f, d.data(), static_cast<DWORD>(d.size()), &got, nullptr) && got == d.size();
    CloseHandle(f);
    // "GND1", u16 cells across, u16 cell metres, u16 rows; per row u16 Ore index, u32 byte count, runs {u8 count, u8 value}
    if (!read || std::memcmp(d.data(), "GND1", 4) != 0) {
        if (g_log) g_log("Ground: ground.bin unreadable (reinstall the mod)");
        return false;
    }
    uint16_t head[3]{};
    std::memcpy(head, d.data() + 4, sizeof(head));
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const uint8_t b : d) hash = (hash ^ b) * 0x100000001b3ull;
    const size_t cells = static_cast<size_t>(head[0]) * head[0];
    std::vector<std::vector<uint8_t>> cover(kGroundCount), forest(kTreesCount);
    size_t p = 10;
    for (int r = 0; r < head[2]; ++r) {
        if (p + 6 > d.size()) return false;
        uint16_t ore = 0;
        uint32_t bytes = 0;
        std::memcpy(&ore, d.data() + p, 2);
        std::memcpy(&bytes, d.data() + p + 2, 4);
        p += 6;
        if (p + bytes > d.size()) return false;
        std::vector<uint8_t>* into = ore >= kGroundFirst && ore < kGroundFirst + kGroundCount ? &cover[ore - kGroundFirst]
                                     : ore >= kTreesFirst && ore < kTreesFirst + kTreesCount  ? &forest[ore - kTreesFirst]
                                                                                              : nullptr;
        if (into) {
            auto& c = *into;
            c.reserve(cells);
            for (size_t i = 0; i + 1 < bytes && c.size() < cells; i += 2) c.insert(c.end(), d[p + i], d[p + i + 1]);
            if (c.size() != cells) c.clear();
        }
        p += bytes;
    }
    g_dataHash = hash;
    g_cover = std::move(cover);
    g_forest = std::move(forest);
    g_cells = head[0];
    g_cellMetres = head[1] ? head[1] : 16;
    if (g_log) g_log("Ground: %d rows of %dx%d cells loaded", head[2], g_cells, g_cells);
    return true;
}

// A layer's share of the ground at world (x, z), 0..1, between the centres of its cells (n x n of `metres`)
float CoverAt(const uint8_t* c, int n, float metres, float x, float z) {
    const float fx = x / metres - 0.5f, fz = z / metres - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, n - 1), z0 = std::clamp(static_cast<int>(std::floor(fz)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1), z1 = std::min(z0 + 1, n - 1);
    const float tx = std::clamp(fx - x0, 0.0f, 1.0f), tz = std::clamp(fz - z0, 0.0f, 1.0f);
    const float a = c[static_cast<size_t>(z0) * n + x0] * (1 - tx) + c[static_cast<size_t>(z0) * n + x1] * tx;
    const float b = c[static_cast<size_t>(z1) * n + x0] * (1 - tx) + c[static_cast<size_t>(z1) * n + x1] * tx;
    return (a * (1 - tz) + b * tz) / 255.0f;
}

// The forests as cells of kTreeShapeCellMeters (255 inside a forest of that row), from their rectangles: only when
// ground.bin has no heat map for them
constexpr int kTreeCells = 10240 / kTreeShapeCellMeters;
std::vector<std::vector<uint8_t>> g_trees;
void BuildTrees() {
    if (!g_trees.empty()) return;
    g_trees.assign(kTreesCount, std::vector<uint8_t>(static_cast<size_t>(kTreeCells) * kTreeCells, 0));
    for (size_t i = 0; i < kTreeShapeRectCount; ++i) {
        const TreeShapeRect& t = kTreeShapeRects[i];
        if (t.ore < kTreesFirst || t.ore >= kTreesFirst + kTreesCount) continue;
        auto& m = g_trees[t.ore - kTreesFirst];
        for (int z = t.z0; z <= t.z1 && z < kTreeCells; ++z)
            for (int x = t.x0; x <= t.x1 && x < kTreeCells; ++x) m[static_cast<size_t>(z) * kTreeCells + x] = 255;
    }
}

struct TileImage {
    uint64_t key = 0;
    std::vector<uint32_t> rgba;
};
TileImage g_tile[kTiles][kTiles];

// What the images show: the enabled rows and the Map Key's region filter
uint64_t ViewKey() {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
    for (size_t i = 0; i < kGroundCount; ++i) mix(g_oreEnabled[kGroundFirst + i] ? 1 : 0);
    for (size_t i = 0; i < kTreesCount; ++i) mix(g_oreEnabled[kTreesFirst + i] ? 2 : 0);
    mix(g_regionFiltering ? g_focusedRegions : 0xffffffffu);
    mix(g_regionOutsideHide);
    mix(g_dataHash);
    mix(7); // version of the look
    return h;
}

// Paints one tile: each pixel the colour of its enabled rows by their share, as strong as the strongest (a Ground row
// by its share, a forest as solid as its old rectangles); outside the regions shown, dimmed or left out
void Compose(int tx, int tz, uint64_t key) {
    TileImage& t = g_tile[tz][tx];
    t.rgba.assign(static_cast<size_t>(kTilePx) * kTilePx, 0);
    t.key = key;
    struct Layer { const uint8_t* cells; int n; float metres; uint32_t colour; int kind; }; // 0 Ground, 1 forest heat, 2 forest shape
    Layer on[kGroundCount + kTreesCount]{};
    size_t n = 0;
    for (size_t i = 0; i < kGroundCount && i < g_cover.size(); ++i)
        if (g_oreEnabled[kGroundFirst + i] && !g_cover[i].empty())
            on[n++] = {g_cover[i].data(), g_cells, static_cast<float>(g_cellMetres), kOreAreaColors[kGroundFirst + i], 0};
    for (size_t i = 0; i < kTreesCount; ++i) {
        if (!g_oreEnabled[kTreesFirst + i]) continue;
        if (i < g_forest.size() && !g_forest[i].empty())
            on[n++] = {g_forest[i].data(), g_cells, static_cast<float>(g_cellMetres), kOreAreaColors[kTreesFirst + i], 1};
        else on[n++] = {g_trees[i].data(), kTreeCells, static_cast<float>(kTreeShapeCellMeters), kOreAreaColors[kTreesFirst + i], 2};
    }
    if (!n) return;
    for (int v = 0; v < kTilePx; ++v) {
        const float z = 10240.0f - (tz * kTilePx + v + 0.5f) * kMetresPerPx; // the image's top is north
        for (int u = 0; u < kTilePx; ++u) {
            const float x = (tx * kTilePx + u + 0.5f) * kMetresPerPx;
            if (halls::InCorner(x, z)) continue; // the Hollow Halls' rock: nothing of the overworld
            float sum = 0, r = 0, g = 0, b = 0, alpha = 0;
            for (size_t k = 0; k < n; ++k) {
                const Layer& l = on[k];
                const float w = CoverAt(l.cells, l.n, l.metres, x, z);
                if (w <= 0.02f) continue;
                // light enough that the markers stay readable
                const float a = l.kind == 2 ? std::min(1.0f, w * 2.0f) * 0.34f : l.kind == 1 ? std::sqrt(w) * 0.32f : std::min(1.0f, w * 1.25f) * 0.4f;
                sum += a;
                r += a * (l.colour & 0xff);
                g += a * ((l.colour >> 8) & 0xff);
                b += a * ((l.colour >> 16) & 0xff);
                alpha = std::max(alpha, a);
            }
            if (sum <= 0) continue;
            if (OutsideFocusedRegion(x, z)) {
                if (g_regionOutsideHide) continue;
                alpha *= 0.25f;
            }
            // The UI blends images as premultiplied colour (straight colour drew every faint edge at full strength)
            const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
            const float k = alpha / sum;
            t.rgba[static_cast<size_t>(v) * kTilePx + u] = (a << 24) | (static_cast<uint32_t>(b * k) << 16) |
                                                            (static_cast<uint32_t>(g * k) << 8) | static_cast<uint32_t>(r * k);
        }
    }
}

bool AnyGround() {
    for (size_t i = 0; i < kGroundCount; ++i)
        if (g_oreEnabled[kGroundFirst + i]) return true;
    return false;
}
bool AnyForest() {
    for (size_t i = 0; i < kTreesCount; ++i)
        if (g_oreEnabled[kTreesFirst + i]) return true;
    return false;
}

// Draws the enabled Ground rows on the World Map (same view maths as DrawTreeAreasOnMap). Up to four tiles are painted
// a frame; a tile shows nothing until it is painted for the rows now enabled and its image is up.
void DrawOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    g_treesAsImages = false;
    if (!widgetRaw || !mapState || !hdpool::Available()) return; // the forests as rectangles, no Ground
    const bool forest = AnyForest(), ground = (AnyGround() || forest) && Load();
    if (!ground && !forest) return;
    if (forest) BuildTrees();
    g_treesAsImages = forest;
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
    const float left = rootRect[0], top = rootRect[1] + 114.0f; // below the menu banner
    const float right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    const uint64_t view = ViewKey();
    const uint64_t colour = MakePackedColor(0xffffffffu, 1.0f, 1.0f);
    const float tileMetres = kTilePx * kMetresPerPx;
    int painted = 0;
    for (int tz = 0; tz < kTiles; ++tz)
        for (int tx = 0; tx < kTiles; ++tx) {
            const float sx0 = originX + tx * tileMetres * zoom, sx1 = sx0 + tileMetres * zoom;
            const float sy0 = originY + (mapWorldH - (10240.0f - tz * tileMetres)) * zoom, sy1 = sy0 + tileMetres * zoom;
            const float cx0 = std::max(sx0, left), cx1 = std::min(sx1, right), cy0 = std::max(sy0, top), cy1 = std::min(sy1, bottom);
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            TileImage& t = g_tile[tz][tx];
            const uint64_t key = (view ^ (static_cast<uint64_t>(tz * kTiles + tx + 1) * 0x9e3779b97f4a7c15ull)) | 1;
            if (t.key != key && painted < 4) {
                Compose(tx, tz, key);
                ++painted;
            }
            // A tile painted for other rows never shows (switching rows would flash the old ones)
            if (t.key != key || t.rgba.empty()) continue;
            const uint32_t id = hdpool::Request(widgetRaw, t.rgba.data(), t.key);
            if (!id) continue;
            const float fu0 = (cx0 - sx0) / (sx1 - sx0), fu1 = (cx1 - sx0) / (sx1 - sx0);
            const float fv0 = (cy0 - sy0) / (sy1 - sy0), fv1 = (cy1 - sy0) / (sy1 - sy0);
            float rect[4] = {cx0, cy0, cx1 - cx0, cy1 - cy0};
            if (fu0 == 0.0f && fv0 == 0.0f && fu1 == 1.0f && fv1 == 1.0f) g_drawImage(widgetRaw, rect, &id, &colour);
            else if (g_drawImageSubUv) g_drawImageSubUv(widgetRaw, rect, id, PackXY(fu0, fv0), PackXY(fu1, fv1), colour);
        }
}
} // namespace ground

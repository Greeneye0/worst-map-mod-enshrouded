// Included in map_key_panel.cpp's private namespace, after ground_areas.inl.
// The Surface rows (Ground category, ore_deposits.h kSurfaceFirst): Roads and Surface Water, on the World Map and the
// minimap. data/roads.bin and data/water.bin (tools/resources/make_resources.py, make_water.py) hold the island in
// 2 m cells: a road where a column's top voxel is a road material (Dirt, Smooth Stone, Limestone, Ancient Road...),
// water where the world's starting water (its WaterChunkResources) shows from above. Both are a few metres across, too
// fine for the Ground rows' 10 m pixels, so they are painted at three levels of detail (10, 5 and 2.5 m a pixel: 4 x 4,
// 8 x 8 and 16 x 16 tiles of 256 px over the world), water under roads in one image per tile, and the level drawn is the
// finest that is worth it whose tiles in view fit the image pool. Only the tiles in view are painted (four a frame) and
// kept, the latest 64.
namespace roads {
constexpr int kLevels = 3, kTilePx = 256;
constexpr int kTilesAt[kLevels] = {4, 8, 16};
constexpr size_t kRoads = 0, kWater = 1, kLayers = 2;
static_assert(kSurfaceCount == kLayers, "Roads and Surface Water rows");

struct Layer {
    const char* file;
    int cells = 0, cellMetres = 2;
    std::vector<uint8_t> kind; // [z * cells + x], z rising north; 0 none
    bool tried = false;
    uint64_t hash = 0;
};
Layer g_layer[kLayers] = {{"roads.bin"}, {"water.bin"}};

bool Load(Layer& l) {
    if (l.tried) return l.cells != 0;
    l.tried = true;
    const std::string path = greeneye::DataPath(l.file);
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (g_log) g_log("Surface: %s not found", path.c_str());
        return false;
    }
    std::vector<uint8_t> d;
    LARGE_INTEGER size{};
    DWORD got = 0;
    const bool read = GetFileSizeEx(f, &size) && size.QuadPart > 8 && size.QuadPart < (16ll << 20) &&
                      (d.resize(static_cast<size_t>(size.QuadPart)), true) &&
                      ReadFile(f, d.data(), static_cast<DWORD>(d.size()), &got, nullptr) && got == d.size();
    CloseHandle(f);
    // "RDS1", u16 cells across, u16 cell metres, then runs {u8 count, u8 kind}
    if (!read || std::memcmp(d.data(), "RDS1", 4) != 0) {
        if (g_log) g_log("Surface: %s unreadable (reinstall the mod)", l.file);
        return false;
    }
    uint16_t head[2]{};
    std::memcpy(head, d.data() + 4, sizeof(head));
    if (head[0] == 0 || head[0] > 8192 || head[1] == 0) return false;
    const size_t cells = static_cast<size_t>(head[0]) * head[0];
    std::vector<uint8_t> kind;
    kind.reserve(cells);
    for (size_t i = 8; i + 1 < d.size() && kind.size() < cells; i += 2) kind.insert(kind.end(), d[i], d[i + 1]);
    if (kind.size() != cells) {
        if (g_log) g_log("Surface: %s holds %zu of %zu cells (reinstall the mod)", l.file, kind.size(), cells);
        return false;
    }
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const uint8_t b : d) hash = (hash ^ b) * 0x100000001b3ull;
    l.hash = hash;
    l.kind = std::move(kind);
    l.cells = head[0];
    l.cellMetres = head[1];
    if (g_log) g_log("Surface: %s, %dx%d cells of %d m", l.file, l.cells, l.cells, l.cellMetres);
    return true;
}

bool On(size_t i) { return g_oreEnabled[kSurfaceFirst + i]; }

struct Tile {
    uint64_t key = 0, lastUse = 0;
    std::vector<uint32_t> rgba;
};
std::vector<Tile> g_tiles; // the latest painted, up to kKeep
constexpr size_t kKeep = 64;
uint64_t g_use = 0;

uint64_t ViewKey() {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
    for (size_t i = 0; i < kLayers; ++i) {
        mix(On(i) ? g_layer[i].hash : 0);
        mix(kOreAreaColors[kSurfaceFirst + i]);
    }
    mix(g_regionFiltering ? g_focusedRegions : 0xffffffffu);
    mix(g_regionOutsideHide);
    mix(4); // version of the look
    return h;
}

// How much of a pixel's footprint (cells cx0..cx1, cz0..cz1) a layer covers, and whether it lies beside one (the
// footprint widened by a cell, so the thinnest path still makes a line)
void Cover(const Layer& l, int cx0, int cx1, int cz0, int cz1, int& in, int& beside, int& cells) {
    in = beside = cells = 0;
    for (int z = cz0 - 1; z <= cz1 + 1; ++z) {
        if (z < 0 || z >= l.cells) continue;
        const uint8_t* row = l.kind.data() + static_cast<size_t>(z) * l.cells;
        for (int x = cx0 - 1; x <= cx1 + 1; ++x) {
            if (x < 0 || x >= l.cells) continue;
            const bool inside = z >= cz0 && z <= cz1 && x >= cx0 && x <= cx1;
            cells += inside;
            if (!row[x]) continue;
            if (inside) ++in;
            else ++beside;
        }
    }
}

// Paints one tile of a level: water as a fill (stronger the more of a pixel it covers), roads over it as lines (a road
// crossing a coarse pixel strong, a corner of one fainter, so lines step less)
void Compose(Tile& t, int level, int tx, int tz) {
    const int n = kTilesAt[level];
    const float mpp = 10240.0f / static_cast<float>(n * kTilePx);
    t.rgba.assign(static_cast<size_t>(kTilePx) * kTilePx, 0);
    const bool roadsOn = On(kRoads) && g_layer[kRoads].cells, waterOn = On(kWater) && g_layer[kWater].cells;
    const uint32_t rc = kOreAreaColors[kSurfaceFirst + kRoads], wc = kOreAreaColors[kSurfaceFirst + kWater];
    for (int v = 0; v < kTilePx; ++v) {
        const float zTop = 10240.0f - (tz * kTilePx + v) * mpp; // the image's top is north
        for (int u = 0; u < kTilePx; ++u) {
            const float xLeft = (tx * kTilePx + u) * mpp;
            const float xm = xLeft + mpp * 0.5f, zm = zTop - mpp * 0.5f;
            if (halls::InCorner(xm, zm)) continue;
            float ra = 0.0f, wa = 0.0f;
            int in = 0, beside = 0, cells = 0;
            if (waterOn) {
                const Layer& l = g_layer[kWater];
                const float m = static_cast<float>(l.cellMetres);
                Cover(l, static_cast<int>(xLeft / m), static_cast<int>((xLeft + mpp) / m), static_cast<int>((zTop - mpp) / m),
                      static_cast<int>(zTop / m), in, beside, cells);
                if (in) wa = std::min(0.62f, 0.25f + 0.45f * static_cast<float>(in) / static_cast<float>(cells));
            }
            if (roadsOn) {
                const Layer& l = g_layer[kRoads];
                const float m = static_cast<float>(l.cellMetres);
                Cover(l, static_cast<int>(xLeft / m), static_cast<int>((xLeft + mpp) / m), static_cast<int>((zTop - mpp) / m),
                      static_cast<int>(zTop / m), in, beside, cells);
                const float share = cells ? static_cast<float>(in) / static_cast<float>(cells) : 0.0f;
                ra = in ? std::min(0.9f, 0.45f + share * 1.6f) : (beside && mpp / m >= 1.0f ? 0.22f : 0.0f);
            }
            if (ra <= 0.0f && wa <= 0.0f) continue;
            float k = 1.0f;
            if (OutsideFocusedRegion(xm, zm)) {
                if (g_regionOutsideHide) continue;
                k = 0.25f;
            }
            // road over water, premultiplied (the UI blends images that way)
            ra *= k;
            wa *= k;
            const float a = ra + wa * (1.0f - ra);
            auto ch = [&](int s) {
                return static_cast<uint32_t>(std::min(255.0f, ((rc >> s) & 0xff) * ra + ((wc >> s) & 0xff) * wa * (1.0f - ra)));
            };
            t.rgba[static_cast<size_t>(v) * kTilePx + u] =
                (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
    }
}

Tile* Find(uint64_t key) {
    for (Tile& t : g_tiles)
        if (t.key == key) return &t;
    return nullptr;
}
Tile& Slot() {
    if (g_tiles.size() < kKeep) return g_tiles.emplace_back();
    Tile* oldest = &g_tiles[0];
    for (Tile& t : g_tiles)
        if (t.lastUse < oldest->lastUse) oldest = &t;
    return *oldest;
}

// Draws the enabled Surface rows into the screen rect left..right, top..bottom: world x = 0 at screen x0, world
// z = 10240 (north) at screen yNorth, ppm screen pixels a metre
void Draw(void* widgetRaw, float x0, float yNorth, float ppm, float left, float top, float right, float bottom) {
    if (!widgetRaw || !(ppm > 0.0f) || right <= left || bottom <= top || !hdpool::Available()) return;
    bool any = false;
    for (size_t i = 0; i < kLayers; ++i) any = (On(i) && Load(g_layer[i])) || any;
    if (!any) return;
    // The finest level that is still worth it (its pixels at least 0.8 screen pixels) and whose tiles in view fit
    // the image pool beside the Ground rows' (40 at most: a 5120 px wide map shows many)
    int level = 0;
    for (int l = kLevels - 1; l > 0; --l) {
        const float tm = 10240.0f / kTilesAt[l] * ppm; // a tile's size on screen
        const float across = std::ceil((right - left) / tm) + 1.0f, down = std::ceil((bottom - top) / tm) + 1.0f;
        if ((10240.0f / (kTilesAt[l] * kTilePx)) * ppm >= 0.8f && across * down <= 40.0f) {
            level = l;
            break;
        }
    }
    const int n = kTilesAt[level];
    const float tileMetres = 10240.0f / n, tileScreen = tileMetres * ppm;
    const uint64_t view = ViewKey();
    const uint64_t colour = MakePackedColor(0xffffffffu, 1.0f, 1.0f);
    ++g_use;
    int painted = 0;
    const int tx0 = std::max(0, static_cast<int>(std::floor((left - x0) / tileScreen)));
    const int tx1 = std::min(n - 1, static_cast<int>(std::floor((right - x0) / tileScreen)));
    const int tz0 = std::max(0, static_cast<int>(std::floor((top - yNorth) / tileScreen)));
    const int tz1 = std::min(n - 1, static_cast<int>(std::floor((bottom - yNorth) / tileScreen)));
    for (int tz = tz0; tz <= tz1; ++tz)
        for (int tx = tx0; tx <= tx1; ++tx) {
            const float sx0 = x0 + tx * tileScreen, sx1 = sx0 + tileScreen;
            const float sy0 = yNorth + tz * tileScreen, sy1 = sy0 + tileScreen;
            const float cx0 = std::max(sx0, left), cx1 = std::min(sx1, right), cy0 = std::max(sy0, top), cy1 = std::min(sy1, bottom);
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            const uint64_t key = (view ^ (static_cast<uint64_t>((level * 64 + tz) * 64 + tx + 1) * 0x9e3779b97f4a7c15ull)) | 1;
            Tile* t = Find(key);
            if (!t) {
                if (painted >= 4) continue;
                Tile& s = Slot();
                s.key = key;
                Compose(s, level, tx, tz);
                ++painted;
                t = &s;
            }
            t->lastUse = g_use;
            const uint32_t id = hdpool::Request(widgetRaw, t->rgba.data(), t->key);
            if (!id) continue;
            const float fu0 = (cx0 - sx0) / tileScreen, fu1 = (cx1 - sx0) / tileScreen;
            const float fv0 = (cy0 - sy0) / tileScreen, fv1 = (cy1 - sy0) / tileScreen;
            float rect[4] = {cx0, cy0, cx1 - cx0, cy1 - cy0};
            if (fu0 == 0.0f && fv0 == 0.0f && fu1 == 1.0f && fv1 == 1.0f) g_drawImage(widgetRaw, rect, &id, &colour);
            else if (g_drawImageSubUv) g_drawImageSubUv(widgetRaw, rect, id, PackXY(fu0, fv0), PackXY(fu1, fv1), colour);
        }
}

// On the World Map, over the Ground rows and forests, under the markers (same view maths as ground::DrawOnMap)
void DrawOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!widgetRaw || !mapState) return;
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
    Draw(widgetRaw, originX, originY + (mapWorldH - 10240.0f) * zoom, zoom, left, top, right, bottom);
}

// On the minimap, over its terrain: (vcx, vcy) is the player at (px, pz)
void DrawOnMinimap(void* widgetRaw, float vx, float vy, float vw, float vh, float vcx, float vcy, float px, float pz, float ppm) {
    Draw(widgetRaw, vcx - px * ppm, vcy - (10240.0f - pz) * ppm, ppm, vx, vy, vx + vw, vy + vh);
}
} // namespace roads

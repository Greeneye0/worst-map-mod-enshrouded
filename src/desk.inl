// Included in map_key_panel.cpp's private namespace, after roads.inl.
// The map desk (Settings > Map Desk): the World Map lies on a cartographer's desk that pans and zooms with it.
// data/desk/ (tools/resources/make_desk.py), two layers, each whole at 1x (a 512 px map square) and at 3x in 256 px
// tiles (hi/) for zooming in:
//   under (desk.png, hi/R_C): the desk, its frame's opening clear. Drawn in the game's own map layer from the
//     filter-menu hook at sub-layer 0x80: over the screen's dark backdrop, under the map, the left panel, the banner
//     and the key hints (their key caps too).
//   over (over.png, hi/oR_C): only the frame where it lies on the map's edges (the art looks at the desk a little from
//     its near side, so the opening narrows toward the top and the frame cuts across the map's sides there), the
//     corner ornaments and the top lip with its shade. At sub-layer 0x90, over the map and the game's own thin line along
//     its edge; kept off the left panel and the key hints, which are under it (behind the hints it is drawn at 0x80).
// desk.txt: where the map square is. The 1x images are decoded on a thread of their own the first time the map shows
// and cut into the image pool's 256 px tiles; the hi tiles in view are read on that kind of thread as needed (the latest
// 96 kept). Zoomed out the 1x tiles are drawn; zoomed in the hi ones, and a 1x tile only where its hi ones aren't read.
bool g_mapDesk = true;
float g_deskDim = 0.5f; // the desk art's brightness (1 as painted; the user's trial: half) (dev op map deskdim)

namespace desk {
constexpr int kTile = 256;
constexpr uint32_t kBackdrop = 0xff262626u; // the map screen's dark grey (R,G,B,A bytes)
constexpr int kUnder = 0, kOver = 1;

std::atomic<int> g_state{0}; // 0 not loaded, 1 loading, 2 ready, 3 failed
int g_cols = 0, g_rows = 0, g_hiScale = 3, g_hiCols = 0, g_hiRows = 0;
float g_frame[4]{};          // the map square in desk.png: L T R B
std::vector<std::vector<uint32_t>> g_tiles[2]; // the 1x layers in tiles: [row * g_cols + col] (empty: all clear)
uint64_t g_keyBase = 0;
std::string g_dir;
std::wstring Wide(const std::string& s);
std::string g_langDir; // desk\lang\<Steam language>\ (desk_langs.py): the survivor sheets in the mod's language, or empty

// The language's own desk where its sheets lie: lo.png over desk.png (olo.png over over.png) at lo.txt's "at X Y"
// (before premultiplying)
void PasteLanguage(const std::string& dir, const char* name, std::vector<uint32_t>& under, int w, int h) {
    FILE* f = nullptr;
    int x0 = 0, y0 = 0;
    char line[64]{};
    if (fopen_s(&f, (dir + name + ".txt").c_str(), "r") != 0 || !f) return;
    const bool ok = std::fgets(line, sizeof(line), f) && sscanf_s(line, "at %d %d", &x0, &y0) == 2;
    std::fclose(f);
    std::vector<uint32_t> px;
    int pw = 0, ph = 0;
    if (!ok || !halls::DecodePng(Wide(dir + name + ".png"), px, pw, ph)) return;
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x)
            if (x0 + x >= 0 && y0 + y >= 0 && x0 + x < w && y0 + y < h)
                under[static_cast<size_t>(y0 + y) * w + x0 + x] = px[static_cast<size_t>(y) * pw + x];
}

void Premultiply(std::vector<uint32_t>& px) { // as the UI blends images
    for (uint32_t& p : px) {
        const uint32_t a = p >> 24;
        if (a == 255) continue;
        p = (a << 24) | (((p >> 16 & 0xff) * a / 255) << 16) | (((p >> 8 & 0xff) * a / 255) << 8) | ((p & 0xff) * a / 255);
    }
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// A 1x layer cut into tiles; past the desk's edge the backdrop (under) or clear (over); all-clear tiles left empty
std::vector<std::vector<uint32_t>> Cut(const std::vector<uint32_t>& img, int w, int h, uint32_t pad) {
    const int cols = (w + kTile - 1) / kTile, rows = (h + kTile - 1) / kTile;
    std::vector<std::vector<uint32_t>> tiles(static_cast<size_t>(cols) * rows);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            std::vector<uint32_t> t(static_cast<size_t>(kTile) * kTile, pad);
            bool any = pad != 0;
            for (int y = 0; y < kTile && r * kTile + y < h; ++y) {
                const int n = std::min(kTile, w - c * kTile);
                const uint32_t* row = &img[static_cast<size_t>(r * kTile + y) * w + c * kTile];
                std::memcpy(&t[static_cast<size_t>(y) * kTile], row, n * 4u);
                for (int x = 0; x < n && !any; ++x) any = (row[x] >> 24) != 0;
            }
            if (any) tiles[static_cast<size_t>(r) * cols + c] = std::move(t);
        }
    return tiles;
}

void LoadProps(const std::string& dir); // desk_props.inl
void LoadNow() {
    const std::string dir = greeneye::DataPath("desk\\");
    float L = 0, T = 0, R = 0, B = 0;
    int hi = 3, hiCols = 0, hiRows = 0;
    char line[160]{};
    FILE* f = nullptr;
    bool ok = fopen_s(&f, (dir + "desk.txt").c_str(), "r") == 0 && f;
    if (ok) {
        ok = std::fgets(line, sizeof(line), f) &&
             sscanf_s(line, "frame %f %f %f %f hi %d cols %d rows %d", &L, &T, &R, &B, &hi, &hiCols, &hiRows) == 7 && R > L && B > T &&
             hi > 0;
        std::fclose(f);
    }
    std::vector<uint32_t> under, over;
    int w = 0, h = 0, ow = 0, oh = 0;
    ok = ok && halls::DecodePng(Wide(dir + "desk.png"), under, w, h) && halls::DecodePng(Wide(dir + "over.png"), over, ow, oh) &&
         ow == w && oh == h;
    if (!ok) {
        if (g_log) g_log("Map desk: data\\desk unreadable or missing (reinstall the mod)");
        g_state = 3;
        return;
    }
    std::string langDir;
    if (umm_lang::Current() != umm_lang::Lang::English) {
        langDir = dir + "lang\\" + umm_lang::SteamCode(umm_lang::Current()) + "\\";
        if (GetFileAttributesA(langDir.c_str()) == INVALID_FILE_ATTRIBUTES) langDir.clear();
        else PasteLanguage(langDir, "lo", under, w, h), PasteLanguage(langDir, "olo", over, w, h);
    }
    Premultiply(under);
    Premultiply(over);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < under.size(); i += 97) hash = (hash ^ under[i] ^ over[i]) * 0x100000001b3ull;
    g_tiles[kUnder] = Cut(under, w, h, kBackdrop);
    g_tiles[kOver] = Cut(over, w, h, 0);
    g_cols = (w + kTile - 1) / kTile;
    g_rows = (h + kTile - 1) / kTile;
    g_hiScale = hi;
    g_hiCols = hiCols;
    g_hiRows = hiRows;
    g_frame[0] = L, g_frame[1] = T, g_frame[2] = R, g_frame[3] = B;
    g_keyBase = hash | 1;
    g_dir = dir;
    g_langDir = langDir;
    LoadProps(dir);
    if (g_log) g_log("Map desk: %dx%d, %dx%d hi tiles, map square %.1f,%.1f-%.1f,%.1f", w, h, hiCols, hiRows, L, T, R, B);
    if (g_log && !langDir.empty()) g_log("Map desk: survivor sheets from %s", langDir.c_str());
    g_state = 2;
}

bool Ready() {
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1)) std::thread(LoadNow).detach();
    return g_state == 2;
}

// ---- hi tiles: read on demand, a thread at a time
struct HiTile {
    std::vector<uint32_t> px; // empty while reading, or for a tile with no file (all clear)
    uint64_t lastUse = 0;
    bool ready = false;
};
std::mutex g_hiLock;
std::unordered_map<int, HiTile> g_hi; // by level << 24 | layer << 20 | row << 10 | col (level 0: mid/ 2x, 1: hi/ 3x)
constexpr int kLevelScale[2] = {2, 3};
const char* const kLevelDir[2] = {"mid", "hi"};
std::vector<int> g_hiQueue;
int g_hiWorkers = 0; // reading threads running (up to 3)
constexpr size_t kHiKeep = 160;

void HiWorker() {
    for (;;) {
        int id = -1;
        {
            std::lock_guard lock(g_hiLock);
            if (g_hiQueue.empty()) {
                --g_hiWorkers;
                return;
            }
            id = g_hiQueue.back(); // the latest asked first: what is in view now
            g_hiQueue.pop_back();
        }
        const char* prefix = ((id >> 20) & 15) == kOver ? "o" : "";
        const char* dir = kLevelDir[(id >> 24) & 1];
        const int r = (id >> 10) & 1023, c = id & 1023;
        std::vector<uint32_t> px;
        int w = 0, h = 0;
        bool ok = false;
        // the language's own tile first (where its sheets lie), else the desk's
        bool found = false;
        for (const std::string& root : {g_langDir, g_dir}) {
            if (root.empty() || found) continue;
            for (const char* ext : {"jpg", "png"}) {
                char name[40];
                std::snprintf(name, sizeof(name), "%s\\%s%d_%d.%s", dir, prefix, r, c, ext);
                if (GetFileAttributesA((root + name).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                ok = halls::DecodePng(Wide(root + name), px, w, h);
                found = true;
                break;
            }
        }
        if (!ok || w != kTile || h != kTile) px.clear(); // no file: an all-clear tile
        else Premultiply(px);
        std::lock_guard lock(g_hiLock);
        auto it = g_hi.find(id);
        if (it != g_hi.end()) {
            it->second.px = std::move(px);
            it->second.ready = true;
        }
    }
}

// The hi tile's pixels if read (nullptr: not yet; an empty one is all clear); asks for it otherwise
const std::vector<uint32_t>* HiTileAt(int level, int layer, int r, int c, uint64_t use) {
    const int id = level << 24 | layer << 20 | r << 10 | c;
    std::lock_guard lock(g_hiLock);
    auto it = g_hi.find(id);
    if (it == g_hi.end()) {
        if (g_hi.size() >= kHiKeep) { // the longest unused goes (not one being read)
            auto oldest = g_hi.end();
            for (auto i = g_hi.begin(); i != g_hi.end(); ++i)
                if (i->second.ready && (oldest == g_hi.end() || i->second.lastUse < oldest->second.lastUse)) oldest = i;
            if (oldest != g_hi.end()) g_hi.erase(oldest);
        }
        it = g_hi.emplace(id, HiTile{}).first;
        g_hiQueue.push_back(id);
        if (g_hiWorkers < 3) {
            ++g_hiWorkers;
            std::thread(HiWorker).detach();
        }
    }
    it->second.lastUse = use;
    return it->second.ready ? &it->second.px : nullptr;
}

// The map square on screen: world x 0 at x0, world north edge at y0, side metres * zoom
bool MapSquare(uint8_t* mapState, float& x0, float& y0, float& side) {
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView) return false;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return false;
    x0 = *reinterpret_cast<float*>(mapView + 0x1f0) + *reinterpret_cast<float*>(mapView + 0x18c) - *reinterpret_cast<float*>(mapView + 0x1f8);
    y0 = *reinterpret_cast<float*>(mapView + 0x1f4) + *reinterpret_cast<float*>(mapView + 0x190) - *reinterpret_cast<float*>(mapView + 0x1fc) +
         (mapWorldH - 10240.0f) * zoom;
    side = 10240.0f * zoom;
    return true;
}

bool Shown() { return g_mapDesk && g_worldMapModded && !halls::MapActive(); }

// The game's own edge round the map (FUN_140d08cf0 in the 11-04 build, from the map canvas: a thin border at
// sub-layer 0x86 and strips outside it) would show over the desk's frame: its sizes are two style floats (the UI
// assets, *(G + 200): +0xee1c the border's thickness, +0xedf8 the strips' width, each times the zoom), set to 0 while
// the desk shows and put back when it doesn't (and before a reload)
struct EdgeStyle {
    uint32_t offset;
    float gameDefault; // the 11-04 build's value: put back if the one read is already 0 (a generation that went
                       // before left it so)
    float original;
    bool held;
};
EdgeStyle g_edge[2] = {{0xee1c, 20.0f, 0.0f, false}, {0xedf8, 80.0f, 0.0f, false}};
bool g_edgeStopping = false; // a reload is on its way: the edge stays as the game has it
bool g_edgeWritable = false, g_edgeChecked = false;
void HideGameEdge(bool hide) {
    uint8_t* assets = GetUiAssets();
    if (!assets) return;
    if (!g_edgeChecked) { // the style data may be a read-only resource: made writable once, or left alone
        g_edgeChecked = true;
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* p = assets + 0xedf8;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
            const DWORD rw = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
            DWORD old = 0;
            g_edgeWritable = (mbi.Protect & rw) != 0 || VirtualProtect(p, 0x30, PAGE_READWRITE, &old);
            if (g_log) g_log("Map desk: style data at %p, protection %lx, %s", p, mbi.Protect, g_edgeWritable ? "writable" : "not writable");
        }
    }
    if (!g_edgeWritable) return;
    for (EdgeStyle& e : g_edge) {
        float& v = *reinterpret_cast<float*>(assets + e.offset);
        if (hide && !e.held) {
            if (!(v >= 0.0f && v < 256.0f)) { // not the value expected there: left alone
                static int logged = 0;
                if (g_log && logged++ < 2) g_log("Map desk: style +0x%x is %f; left alone", e.offset, v);
                continue;
            }
            e.original = v > 0.0f ? v : e.gameDefault;
            e.held = true;
            v = 0.0f;
            if (g_log) g_log("Map desk: the game's map edge off (style +0x%x was %.3f)", e.offset, e.original);
        } else if (!hide && e.held) {
            v = e.original;
            e.held = false;
        } else if (hide && e.held) {
            v = 0.0f;
        }
    }
}

uint64_t g_use = 0;
int g_deskBandKey = 0x83; // the frame's sub-layer behind the key hints: over the map, under the hints


// One image (a tile of tilePx desk.png pixels at (px0, py0) of the desk) at its place on screen, clipped
void DrawTile(void* widget, const uint32_t* px, uint64_t key, float dx, float dy, float s, float px0, float py0, float tilePx,
              const float* clip) {
    const float sx0 = dx + px0 * s, sy0 = dy + py0 * s, ts = tilePx * s;
    const float cx0 = std::max(sx0, clip[0]), cx1 = std::min(sx0 + ts, clip[2]), cy0 = std::max(sy0, clip[1]), cy1 = std::min(sy0 + ts, clip[3]);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    const uint32_t id = hdpool::Request(widget, px, key);
    if (!id) return;
    const uint64_t colour = MakePackedColor(0xffffffffu, 1.0f, g_deskDim); // (the whole desk at g_deskDim brightness)
    float rect[4] = {cx0, cy0, cx1 - cx0, cy1 - cy0};
    const float fu0 = (cx0 - sx0) / ts, fu1 = (cx1 - sx0) / ts, fv0 = (cy0 - sy0) / ts, fv1 = (cy1 - sy0) / ts;
    if (fu0 == 0.0f && fv0 == 0.0f && fu1 == 1.0f && fv1 == 1.0f) g_drawImage(widget, rect, &id, &colour);
    else if (g_drawImageSubUv) g_drawImageSubUv(widget, rect, id, PackXY(fu0, fv0), PackXY(fu1, fv1), colour);
}

void SetSubLayer(void* uiCtx, void* drawList, uint16_t base, uint8_t sub) {
    if (!uiCtx) return;
    const uint16_t key = static_cast<uint16_t>((base & 0xFF00u) | sub);
    *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(uiCtx) + 0x6ce8) = key;
    if (drawList) *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(drawList) + 0x35214) = key;
}

#include "desk_props.inl"

#ifdef UMM_DEV_BRIDGE
// dev (map op "meshtest" 1 / 2): a desk tile as a spinning quad through ui_mesh, the texture named by its image id (1)
// or its slot in the registry's image table (2): which one the renderer's texture setter takes
int g_meshTest = 0;
void MeshTest(void* widget, const float* screen) {
    if (!g_meshTest || g_tiles[kUnder].empty()) return;
    size_t t = 0;
    while (t < g_tiles[kUnder].size() && g_tiles[kUnder][t].empty()) ++t;
    if (t == g_tiles[kUnder].size()) return;
    const uint64_t key = (g_keyBase ^ 0x5151515151515151ull) | 1;
    const uint32_t id = hdpool::Request(widget, g_tiles[kUnder][t].data(), key);
    if (!id) return;
    uint32_t texture = id;
    if (g_meshTest == 2) {
        auto* registry = static_cast<uint8_t*>(hdpool::Registry(widget));
        texture = registry ? *reinterpret_cast<uint32_t*>(registry + 0x35300) + (id & 0xfffff) : 0;
    }
    const float a = GetTickCount64() % 6000 / 6000.0f * 6.2831853f, cx = screen[0] + 400.0f, cy = screen[1] + 400.0f;
    smooth_highlight::Batch batch;
    const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (auto& c : corner) {
        const float x = c[0] * 150.0f, y = c[1] * 150.0f * (0.6f + 0.4f * std::cos(a * 0.5f)); // tilting
        smooth_highlight::Vertex v{cx + x * std::cos(a) - y * std::sin(a), cy + x * std::sin(a) + y * std::cos(a), 0xffffffffu,
                                   (c[0] + 1) * 0.5f, (c[1] + 1) * 0.5f, 1.0f};
        batch.vertices.push_back(v);
    }
    batch.indices = {0, 1, 2, 0, 2, 3};
    ui_mesh::Draw(widget, batch, texture);
}
#endif

// The desk: under the map, then its frame over it (called from the filter-menu hook, in the game's map layer)
void DrawUnder(uint8_t* mapState, const float* screen) {
    const bool show = Shown() && hdpool::Available() && Ready() && screen[2] >= 100.0f;
    HideGameEdge(show && !g_edgeStopping);
    if (!show) return;
    float x0 = 0, y0 = 0, side = 0;
    if (!MapSquare(mapState, x0, y0, side)) return;
    static const UiText kName = MakeUiText("mapDesk");
    Widget w;
    g_namedWidget(w.raw, &kName);
    g_setSize(w.Handle(), 0);
    void* uiCtx = *reinterpret_cast<void**>(w.raw);
    void* drawList = uiCtx ? g_getUiDrawList(uiCtx) : nullptr;
    const uint16_t saved = uiCtx ? *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(uiCtx) + 0x6ce8) : 0;
    const uint16_t savedDl = drawList ? *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(drawList) + 0x35214) : 0;
    ++g_use;
    const float s = side / (g_frame[2] - g_frame[0]); // screen units a desk.png pixel
    const float dx = x0 - g_frame[0] * s, dy = y0 - g_frame[1] * s; // the desk's top left on screen
    const float clip[4] = {screen[0], screen[1], screen[0] + screen[2], screen[1] + screen[3]};
    // Zoomed in, each 1x tile is drawn from a finer level read from disk: 2x (mid/) once a 1x pixel shows enlarged, 3x
    // (hi/) once a 2x one would; the tiles nearest the screen's middle first, as many as the image pool allows beside the
    // other layers (a budget of tiles per layer). Where the finer one isn't read yet or is over budget, the next coarser.
    const bool zoomedIn = s > 1.0f;
    const int bestLevel = s > 1.6f ? 1 : 0;
    const float ts = kTile * s;
    const int c0 = std::max(0, static_cast<int>(std::floor((clip[0] - dx) / ts))), c1 = std::min(g_cols - 1, static_cast<int>(std::floor((clip[2] - dx) / ts)));
    const int r0 = std::max(0, static_cast<int>(std::floor((clip[1] - dy) / ts))), r1 = std::min(g_rows - 1, static_cast<int>(std::floor((clip[3] - dy) / ts)));
    const float mx = (clip[0] + clip[2]) * 0.5f, my = (clip[1] + clip[3]) * 0.5f;
    struct Fine {
        int level, r, c;
        const std::vector<uint32_t>* px;
    };
    auto drawLayer = [&](int layer, const float* at, int budget) {
        std::vector<uint8_t> needLo(static_cast<size_t>(g_cols) * g_rows, 1);
        std::vector<Fine> fine;
        if (zoomedIn) {
            std::vector<std::pair<float, int>> order; // the 1x tiles in view with something drawn, nearest the middle first
            for (int r = r0; r <= r1; ++r)
                for (int c = c0; c <= c1; ++c) {
                    if (g_tiles[layer][static_cast<size_t>(r) * g_cols + c].empty()) continue;
                    const float tx = dx + (c + 0.5f) * ts - mx, ty = dy + (r + 0.5f) * ts - my;
                    order.emplace_back(tx * tx + ty * ty, r * g_cols + c);
                }
            std::sort(order.begin(), order.end());
            int used = 0;
            for (const auto& [d2, idx] : order) {
                const int lr = idx / g_cols, lc = idx % g_cols;
                for (int level = bestLevel; level >= 0; --level) {
                    const int k = kLevelScale[level];
                    const float fts = ts / k;
                    std::vector<std::pair<int, int>> cells;
                    for (int r = lr * k; r < (lr + 1) * k; ++r)
                        for (int c = lc * k; c < (lc + 1) * k; ++c) {
                            const float sx0 = dx + c * fts, sy0 = dy + r * fts;
                            if (sx0 + fts <= at[0] || sx0 >= at[2] || sy0 + fts <= at[1] || sy0 >= at[3]) continue;
                            cells.emplace_back(r, c);
                        }
                    if (used + static_cast<int>(cells.size()) > budget) continue; // over budget: a coarser level
                    used += static_cast<int>(cells.size());
                    std::vector<Fine> got;
                    for (auto [r, c] : cells)
                        if (const std::vector<uint32_t>* px = HiTileAt(level, layer, r, c, g_use)) got.push_back({level, r, c, px});
                    if (got.size() == cells.size()) { // all read: drawn from them, over the 1x tile (which shows until
                        // they are on the GPU, so there is never a hole)
                        fine.insert(fine.end(), got.begin(), got.end());
                        break;
                    }
                }
            }
        }
        int drawn = 0;
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1 && drawn < 48; ++c) {
                const auto& t = g_tiles[layer][static_cast<size_t>(r) * g_cols + c];
                if (!needLo[static_cast<size_t>(r) * g_cols + c] || t.empty()) continue;
                const uint64_t key = (g_keyBase ^ (static_cast<uint64_t>(layer * 4096 + r * 64 + c + 1) * 0xd6e8feb86659fd93ull)) | 1;
                DrawTile(w.raw, t.data(), key, dx, dy, s, static_cast<float>(c * kTile), static_cast<float>(r * kTile), static_cast<float>(kTile), at);
                ++drawn;
            }
        for (const Fine& f : fine) {
            if (f.px->empty()) continue; // all clear
            const float tilePx = static_cast<float>(kTile) / kLevelScale[f.level]; // in desk.png pixels
            const uint64_t key = (g_keyBase ^ 0x9e3779b97f4a7c15ull ^
                                  (static_cast<uint64_t>(f.level << 24 | layer << 20 | f.r << 10 | f.c) + 1) * 0xc2b2ae3d27d4eb4full) | 1;
            DrawTile(w.raw, f.px->data(), key, dx, dy, s, f.c * tilePx, f.r * tilePx, tilePx, at);
        }
    };
    // Under the map (sub-layer 0x80, after the game's dark backdrop: under the map and under the key hints, their key
    // caps too, so the desk runs on beneath them)
    SetSubLayer(uiCtx, drawList, saved, 0x80);
    drawLayer(kUnder, clip, 36); // (the image pool holds 96: the 1x tiles, roads, ground and these share it)
    DrawProps(w.raw, dx, dy, s, clip); // the lantern and the compass, on the desk (desk_props.inl)
    // Over the map (0x90): the frame along its edges, the corners, the lip; not over the left panel or the key hints
    // (under this sub-layer; the banner is over it). Behind the key hints (up to two rows of them) the frame goes on at
    // 0x81: over the map, under the hints (the desk is drawn from the hint hook, just before them, so their key caps
    // come after it).
    // Over the map, at 0x81 (drawn from the key-hint hook, just before the hints): over the map, under the hints, the
    // left panel and the banner, all of it at once (the game's own edge, which drew over it, is off)
    SetSubLayer(uiCtx, drawList, saved, static_cast<uint8_t>(g_deskBandKey));
    drawLayer(kOver, clip, 12);
    DrawUnderRing(w.raw); // the mod's icons from over the player's ring, here under the game's markers (map_key_panel.cpp)
#ifdef UMM_DEV_BRIDGE
    MeshTest(w.raw, screen);
#endif
    if (uiCtx) {
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(uiCtx) + 0x6ce8) = saved;
        if (drawList) *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(drawList) + 0x35214) = savedDl;
    }
    g_endWidget(w.raw);
}

} // namespace desk

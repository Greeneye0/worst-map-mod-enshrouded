// The Hollow Halls on the minimap. The six dungeons are carved in the rock of the world's north-west corner, off the
// island, where the minimap's terrain is meaningless: inside one it shows the dungeon's floor plan instead (halls.bin,
// next to the DLL: tools/resources/scan_halls.py + make_halls.py, every 1 m floor cell with rock overhead (within 96 m:
// the tallest halls are over 50 m), as runs of
// one floor height along x), and the player's footprints.
// Floors stack (halls above halls), so in each column the plan shows the floor you'd walk to from where you stand
// (see Flood). What you stand on tells the way: the dungeon's built paths (its blocks, materials 128 and up) bright,
// natural ground (stone, dirt) dimmer, Toxic Slime, Tar and Shroud Slime in a green warning tint.
// Included by map_key_panel.cpp.

bool LocalPlayerY(uint8_t* client, float& y); // map_key_panel.cpp, below
void DrawMapHoverTitle(float x, float y, const char* name);

namespace halls {

struct Run {
    uint16_t x0, length;
    uint16_t yKind; // the height you stand at (m) | kind << 12
    int Y() const { return yKind & 0x0fff; }
    int Kind() const { return yKind >> 12; } // 1 natural, 2 built path, 3 hazard
};
static_assert(sizeof(Run) == 6, "halls.bin runs are 6 bytes");

// Each dungeon: its floors' bounds, the overworld entrance that leads into it (the Fallen Crypt teleport platform,
// 0 0 if none is known) and its Return Beacons, in the order you come to them walking in from the arrival point
struct Dungeon {
    float minX, minZ, maxX, maxZ;
    float entX, entZ;
    uint32_t firstBeacon, beaconCount;
};
struct Beacon {
    float x, y, z;
};
static_assert(sizeof(Dungeon) == 32 && sizeof(Beacon) == 12, "halls.bin layout");

std::vector<uint8_t> g_data;
uint32_t g_z0 = 0, g_rows = 0, g_dungeonCount = 0;
const uint32_t* g_rowStart = nullptr;
const Run* g_runs = nullptr;
const uint8_t* g_runDungeon = nullptr; // per run: its dungeon (255 none)
const uint8_t* g_runMaterial = nullptr; // per run: the voxel material you stand on
const uint32_t* g_materialColour = nullptr; // [256] what each material looks like from above (0xAABBGGRR)
const Dungeon* g_dungeons = nullptr;
const Beacon* g_beacons = nullptr;
bool g_tried = false;

// The dungeons' corner of the world (the island stays east and south of it)
constexpr float kCornerMaxX = 2700.0f, kCornerMinZ = 7900.0f;
// Which dungeons have floors in each 16 m cell of the corner (a bit each): the World Map shows one dungeon's markers
constexpr int kOwnCell = 16, kOwnW = 2816 / kOwnCell, kOwnZ0 = 7808, kOwnH = (10240 - kOwnZ0) / kOwnCell;
uint8_t g_own[kOwnH][kOwnW]{};

bool Load() {
    if (g_tried) return g_runs != nullptr;
    g_tried = true;
    const std::string path = greeneye::DataPath("halls.bin");
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (g_log) g_log("Hollow Halls: %s not found; the minimap shows no floor plan", path.c_str());
        return false;
    }
    LARGE_INTEGER size{};
    DWORD got = 0;
    const bool read = GetFileSizeEx(f, &size) && size.QuadPart > 24 && size.QuadPart < (16ll << 20) &&
                      (g_data.resize(static_cast<size_t>(size.QuadPart)), true) &&
                      ReadFile(f, g_data.data(), static_cast<DWORD>(g_data.size()), &got, nullptr) && got == g_data.size();
    CloseHandle(f);
    // "HAL4", z0, rows, runs, dungeons, beacons; colour[256], the dungeons, the beacons, rowStart[rows + 1], the runs,
    // their dungeons, their materials
    uint32_t head[5]{};
    if (read && std::memcmp(g_data.data(), "HAL4", 4) == 0) std::memcpy(head, g_data.data() + 4, sizeof(head));
    g_z0 = head[0];
    g_rows = head[1];
    const uint32_t runs = head[2];
    g_dungeonCount = head[3];
    const size_t coloursAt = 24, dungeonsAt = coloursAt + 256 * 4, beaconsAt = dungeonsAt + static_cast<size_t>(g_dungeonCount) * sizeof(Dungeon);
    const size_t rowsAt = beaconsAt + static_cast<size_t>(head[4]) * sizeof(Beacon);
    const size_t runsAt = rowsAt + (static_cast<size_t>(g_rows) + 1) * 4;
    const size_t need = runsAt + static_cast<size_t>(runs) * (sizeof(Run) + 2);
    if (!read || g_rows == 0 || g_rows > 4096 || g_dungeonCount > 8 || need != g_data.size()) {
        g_data.clear();
        if (g_log) g_log("Hollow Halls: halls.bin unreadable (reinstall the mod)");
        return false;
    }
    g_dungeons = reinterpret_cast<const Dungeon*>(g_data.data() + dungeonsAt);
    g_beacons = reinterpret_cast<const Beacon*>(g_data.data() + beaconsAt);
    g_rowStart = reinterpret_cast<const uint32_t*>(g_data.data() + rowsAt);
    g_runs = reinterpret_cast<const Run*>(g_data.data() + runsAt);
    g_runDungeon = g_data.data() + runsAt + static_cast<size_t>(runs) * sizeof(Run);
    g_runMaterial = g_runDungeon + runs;
    g_materialColour = reinterpret_cast<const uint32_t*>(g_data.data() + coloursAt);
    for (uint32_t r = 0; r < g_rows; ++r) {
        const int oz = (static_cast<int>(g_z0 + r) - kOwnZ0) / kOwnCell;
        if (oz < 0 || oz >= kOwnH) continue;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            if (g_runDungeon[i] >= g_dungeonCount) continue;
            for (int ox = g_runs[i].x0 / kOwnCell; ox <= (g_runs[i].x0 + g_runs[i].length - 1) / kOwnCell && ox < kOwnW; ++ox)
                g_own[oz][ox] |= static_cast<uint8_t>(1u << g_runDungeon[i]);
        }
    }
    if (g_log) g_log("Hollow Halls: %u floor runs, %u dungeons, %u beacons loaded", runs, g_dungeonCount, head[4]);
    return true;
}

// The floor run nearest your feet: within 8 m around and 30 m up or down (some stairs and walkways are placed pieces,
// not carved rock, and stand well over the floor the scan found under them); -1 outside the dungeons
// dungeonOnly: only a dungeon's own floors, not the unwalkable rock make_halls.py set apart (wall tops, ledges: you
// can still stand on one, in the hall), and further up or down
int NearestRun(float px, float py, float pz, bool dungeonOnly = false, int reachColumns = 8) {
    if (!(px < kCornerMaxX && pz > kCornerMinZ) || !Load()) return -1;
    const int zc = static_cast<int>(std::floor(pz)), xc = static_cast<int>(std::floor(px));
    int best = -1;
    // A dungeon's own floor up to 80 m off: high on rock over it (a wall top, a ledge, 40 m up in Veilwater) you're
    // still in that hall; the halls stacked over each other are hundreds of metres apart
    float bestD = dungeonOnly ? 80.0f : 30.0f;
    for (int z = zc - reachColumns; z <= zc + reachColumns; ++z) {
        if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) continue;
        const uint32_t r = static_cast<uint32_t>(z) - g_z0;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            const Run& run = g_runs[i];
            if (xc + reachColumns < run.x0 || xc - reachColumns >= run.x0 + run.length) continue;
            if (dungeonOnly && g_runDungeon[i] >= g_dungeonCount) continue;
            const float d = std::fabs(run.Y() - py);
            if (d <= bestD) {
                bestD = d;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

// Inside a dungeon: in its corner (no island there), with one of its floors nearby
bool InHall(float px, float py, float pz) { return NearestRun(px, py, pz) >= 0; }

// ---- footprints: a step every 1.6 m, left and right in turn; each shows while it's under 10 minutes old or within the
// last 100 m walked, fading out toward the later of the two; kept per world
struct Step {
    float x, y, z;
    int64_t time; // seconds since 1970
    uint8_t side; // 0 left, 1 right
};
std::vector<Step> g_steps;
std::string g_stepsWorld;
bool g_stepsDirty = false;
uint64_t g_stepsSavedTick = 0;
constexpr size_t kMaxSteps = 4000;
constexpr float kStepMeters = 1.6f;
constexpr double kFadeSeconds = 10.0 * 60.0;
constexpr float kTrailMeters = 100.0f;

// How strongly each step shows (0: gone), newest last as in g_steps
const std::vector<float>& StepAlphas() {
    static std::vector<float> alphas;
    alphas.assign(g_steps.size(), 0.0f);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    float trail = 0.0f;
    for (size_t i = g_steps.size(); i-- > 0;) {
        const Step& s = g_steps[i];
        if (i + 1 < g_steps.size() && g_steps[i + 1].side <= 1) { // walked from here to the next (not a teleport)
            const Step& n = g_steps[i + 1];
            trail += std::sqrt((n.x - s.x) * (n.x - s.x) + (n.z - s.z) * (n.z - s.z));
        }
        const double byAge = 1.0 - static_cast<double>(now - s.time) / kFadeSeconds;
        const float f = std::max(static_cast<float>(byAge), 1.0f - trail / kTrailMeters);
        if (f <= 0.0f && trail > kTrailMeters) break; // older ones are older and further still
        if (f > 0.0f) alphas[i] = (0.25f + 0.7f * std::min(1.0f, f));
    }
    return alphas;
}

std::string StepsPath(const std::string& world) { return greeneye::DataPath(("hall_steps_" + world + ".bin").c_str()); }

void SaveSteps() {
    g_stepsDirty = false;
    g_stepsSavedTick = GetTickCount64();
    if (g_stepsWorld.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, StepsPath(g_stepsWorld).c_str(), "wb") != 0 || !f) return;
    const uint32_t n = static_cast<uint32_t>(g_steps.size());
    std::fwrite("STP1", 1, 4, f);
    std::fwrite(&n, 4, 1, f);
    if (n) std::fwrite(g_steps.data(), sizeof(Step), n, f);
    std::fclose(f);
}

void LoadSteps(const std::string& world) {
    if (world == g_stepsWorld) return;
    if (g_stepsDirty) SaveSteps();
    g_steps.clear();
    g_stepsWorld = world;
    if (world.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, StepsPath(world).c_str(), "rb") != 0 || !f) return;
    char magic[4]{};
    uint32_t n = 0;
    if (std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "STP1", 4) == 0 && std::fread(&n, 4, 1, f) == 1 &&
        n <= kMaxSteps) {
        g_steps.resize(n);
        if (std::fread(g_steps.data(), sizeof(Step), n, f) != n) g_steps.clear();
    }
    std::fclose(f);
}

void NoteStep(float px, float py, float pz) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    uint8_t side = 0;
    if (!g_steps.empty()) {
        const Step& last = g_steps.back();
        const float dx = px - last.x, dy = py - last.y, dz = pz - last.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < kStepMeters * kStepMeters) return;
        side = last.side == 0 ? 1 : 0;
        if (d2 > 40.0f * 40.0f) g_steps.push_back({px, py, pz, now, 2}); // a teleport or a respawn: a break, no trail
    }
    g_steps.push_back({px, py, pz, now, side});
    if (g_steps.size() > kMaxSteps) g_steps.erase(g_steps.begin(), g_steps.begin() + (g_steps.size() - kMaxSteps));
    g_stepsDirty = true;
}

// ---- levels by walking: from the floor you stand on, flood out over floor cells you can step between (up to 2 m
// up or down; a ledge you could only drop off doesn't connect). In each 1 m column the floor reached first, the one
// you'd walk to, is the one drawn: stairs and ramps stay one path, and a hall stacked over or under it shows only where
// it's the nearer way. Nothing here follows your height from step to step, so the plan holds still as you move;
// floors you can't walk to from here (a pit's floor, a hall reached another way) are drawn dimmer.
constexpr int kLayers = 6; // floors kept per column
constexpr int kStepUp = 2; // m between neighbouring cells that still connect
struct Cell {
    uint32_t run[kLayers];
    uint8_t n, reached; // reached: a bit per layer
    int8_t first;       // the layer reached first (-1: none)
};
std::vector<Cell> g_grid;
uint32_t g_floodCount = 0; // grids flooded so far (the HD minimap renders anew after each)
int g_gx0 = 0, g_gz0 = 0, g_gSize = 0;           // the grid's corner (world m) and side
float g_gox = 1e9f, g_goy = 1e9f, g_goz = 1e9f; // where it was last flooded from

Cell* GridCell(int x, int z) {
    const int gx = x - g_gx0, gz = z - g_gz0;
    if (gx < 0 || gz < 0 || gx >= g_gSize || gz >= g_gSize) return nullptr;
    return &g_grid[static_cast<size_t>(gz) * g_gSize + gx];
}

// The layer of a cell nearest height y, at most `within` m off (-1: none)
int LayerAt(const Cell& c, float y, float within) {
    int best = -1;
    float bestD = within;
    for (int l = 0; l < c.n; ++l) {
        const float d = std::fabs(g_runs[c.run[l]].Y() - y);
        if (d <= bestD) {
            bestD = d;
            best = l;
        }
    }
    return best;
}

// Re-flood once you've moved 3 m or the view outgrew the grid; nothing underfoot (mid-jump) keeps the last flood
// The hall you're in (-1 not known yet): the minimap shows its floors only, not another hall's beside or under it
int g_curDungeon = -1;

// A spot within 16 m of one of dungeon d's floors (the ownership cells)
bool NearDungeon(float x, float z, int d) {
    const int ox = static_cast<int>(x) / kOwnCell, oz = (static_cast<int>(z) - kOwnZ0) / kOwnCell;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = ox + dx, cz = oz + dz;
            if (cx >= 0 && cz >= 0 && cx < kOwnW && cz < kOwnH && (g_own[cz][cx] & (1u << d))) return true;
        }
    return false;
}

void Flood(float px, float py, float pz, int half) {
    const float dx = px - g_gox, dy = py - g_goy, dz = pz - g_goz;
    if (dx * dx + dy * dy + dz * dz < 9.0f && half * 2 + 1 <= g_gSize) return;
    const int xc = static_cast<int>(std::floor(px)), zc = static_cast<int>(std::floor(pz));
    // Is there a floor at your feet (or within 2 m, on a cell's edge), or under them (on placed stairs, up to 12 m)?
    auto floorAt = [&](int x, int z) {
        if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) return false;
        const uint32_t r = static_cast<uint32_t>(z) - g_z0;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            const Run& run = g_runs[i];
            if (x >= run.x0 && x < run.x0 + run.length && run.Y() - py <= 3.0f && py - run.Y() <= 12.0f) return true;
        }
        return false;
    };
    bool underfoot = false;
    for (int z = zc - 2; z <= zc + 2 && !underfoot; ++z)
        for (int x = xc - 2; x <= xc + 2 && !underfoot; ++x) underfoot = floorAt(x, z);
    if (!underfoot && half * 2 + 1 <= g_gSize) return;
    // Flooded into a new grid, kept only if it reaches a real stretch of floor: standing on a lone ledge (a block's
    // top, a stair's edge) keeps the last plan rather than dimming everything
    const int size = half * 2 + 1, gx0 = xc - half, gz0 = zc - half;
    static std::vector<Cell> grid;
    grid.assign(static_cast<size_t>(size) * size, Cell{{}, 0, 0, -1});
    for (int z = gz0; z < gz0 + size; ++z) {
        if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) continue;
        const uint32_t r = static_cast<uint32_t>(z) - g_z0;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            // Another hall's floors are left out; the rock set apart from the floors (wall tops, ledges) stays, as
            // you can climb up on it (the plan shows it only where the flood reaches it)
            if (g_curDungeon >= 0 && g_runDungeon[i] != g_curDungeon && g_runDungeon[i] < g_dungeonCount) continue;
            const Run& run = g_runs[i];
            const int x0 = std::max(static_cast<int>(run.x0), gx0);
            const int x1 = std::min(static_cast<int>(run.x0) + run.length, gx0 + size);
            for (int x = x0; x < x1; ++x) {
                Cell& c = grid[static_cast<size_t>(z - gz0) * size + (x - gx0)];
                if (c.n < kLayers) c.run[c.n++] = i;
            }
        }
    }
    // Start from every floor within 3 m of your feet's height, 2 m around; none (on placed stairs): the highest floor
    // under you, up to 12 m down
    static std::vector<uint32_t> queue;
    queue.clear();
    for (int pass = 0; pass < 2 && queue.empty(); ++pass)
        for (int z = zc - 2; z <= zc + 2; ++z)
            for (int x = xc - 2; x <= xc + 2; ++x) {
                const uint32_t idx = static_cast<uint32_t>(z - gz0) * size + (x - gx0);
                Cell& c = grid[idx];
                int under = -1;
                for (int l = 0; l < c.n; ++l) {
                    const float dy = g_runs[c.run[l]].Y() - py;
                    if (pass == 1) {
                        if (dy < 0.0f && dy >= -12.0f && (under < 0 || g_runs[c.run[l]].Y() > g_runs[c.run[under]].Y()))
                            under = l;
                        continue;
                    }
                    if (std::fabs(dy) > 3.0f) continue;
                    c.reached |= static_cast<uint8_t>(1u << l);
                    if (c.first < 0) c.first = static_cast<int8_t>(l);
                    queue.push_back(idx * 8 + l);
                }
                if (under >= 0) {
                    c.reached |= static_cast<uint8_t>(1u << under);
                    if (c.first < 0) c.first = static_cast<int8_t>(under);
                    queue.push_back(idx * 8 + under);
                }
            }
    static const int kDir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (size_t q = 0; q < queue.size(); ++q) {
        const uint32_t idx = queue[q] / 8, layer = queue[q] % 8;
        const int gx = static_cast<int>(idx % size), gz = static_cast<int>(idx / size);
        const int y = g_runs[grid[idx].run[layer]].Y();
        for (const auto& d : kDir) {
            const int nx = gx + d[0], nz = gz + d[1];
            if (nx < 0 || nz < 0 || nx >= size || nz >= size) continue;
            const uint32_t nIdx = static_cast<uint32_t>(nz) * size + nx;
            Cell& n = grid[nIdx];
            for (int l = 0; l < n.n; ++l) {
                if ((n.reached & (1u << l)) || std::abs(g_runs[n.run[l]].Y() - y) > kStepUp) continue;
                n.reached |= static_cast<uint8_t>(1u << l);
                if (n.first < 0) n.first = static_cast<int8_t>(l);
                queue.push_back(nIdx * 8 + l);
            }
        }
    }
    constexpr size_t kMinReach = 400; // cells
    g_gox = px; // tried from here either way: the next try is 3 m on
    g_goy = py;
    g_goz = pz;
    if (queue.size() < kMinReach && half * 2 + 1 <= g_gSize) return;
    g_grid.swap(grid);
    ++g_floodCount;
    g_gSize = size;
    g_gx0 = gx0;
    g_gz0 = gz0;
}

// ---- drawing
// What you stand on: {colour, alpha}
void KindLook(int kind, uint32_t& colour, float& alpha) {
    if (kind == 2) {
        colour = 0xffc4d0d2u; // built path: bright stone
        alpha = 0.92f;
    } else if (kind == 3) {
        colour = 0xff3cb45au; // Toxic Slime, Tar, Shroud Slime: green warning
        alpha = 0.6f;
    } else {
        colour = 0xff707a7cu; // natural ground: dim
        alpha = 0.55f;
    }
}

// The floor plan and the footprints inside the minimap's viewport (vx, vy, vw, vh) centred on the player.
bool DrawHdMinimap(void* widgetRaw, float vx, float vy, float vw, float vh, float vcx, float vcy, float px, float pz,
                   float pxPerMeterX, float pxPerMeterY, float py); // below

void Draw(void* widgetRaw, float vx, float vy, float vw, float vh, float vcx, float vcy, float px, float py, float pz,
          float pxPerMeterX, float pxPerMeterY) {
    if (!g_runs) return;
    // Cells sampled every `step` m so a drawn one is at least 1.5 px (far out, the plan is coarser)
    const int step = std::max(1, static_cast<int>(std::ceil(1.5f / std::max(0.01f, pxPerMeterY))));
    const float halfW = vw * 0.5f / pxPerMeterX, halfH = vh * 0.5f / pxPerMeterY;
    Flood(px, py, pz, std::max(96, std::min(320, static_cast<int>(std::ceil(std::max(halfW, halfH))) + 8)));
    if (g_gSize == 0) return;
    // The HD view (the game's textures, halls.inl's renderer), else the plain plan below
    const bool hd = DrawHdMinimap(widgetRaw, vx, vy, vw, vh, vcx, vcy, px, pz, pxPerMeterX, pxPerMeterY, py);
    const int zTop = static_cast<int>(std::ceil(pz + halfH)), zBottom = static_cast<int>(std::floor(pz - halfH));
    const float xLeft = px - halfW, xRight = px + halfW;
    const int cx0 = static_cast<int>(std::floor(xLeft)), cx1 = static_cast<int>(std::ceil(xRight));
    for (int z = zBottom - (zBottom % step + step) % step; !hd && z <= zTop; z += step) {
        float top = vcy - (z + step - pz) * pxPerMeterY, height = step * pxPerMeterY;
        if (top < vy) {
            height -= vy - top;
            top = vy;
        }
        if (top + height > vy + vh) height = vy + vh - top;
        if (height <= 0.0f) continue;
        // Along the row, cells of one look merged into a strip
        uint32_t stripColour = 0;
        float stripAlpha = 0.0f, stripX0 = 0.0f;
        auto flush = [&](float x1) {
            const float wx0 = std::max(xLeft, stripX0), wx1 = std::min(xRight, x1);
            if (stripAlpha > 0.0f && wx1 > wx0)
                DrawSolidRectAbs(widgetRaw, vcx + (wx0 - px) * pxPerMeterX, top, (wx1 - wx0) * pxPerMeterX, height,
                                 stripColour, stripAlpha);
        };
        for (int x = cx0 - (cx0 % step + step) % step; x <= cx1 + step; x += step) {
            uint32_t colour = 0;
            float alpha = 0.0f;
            const Cell* c = x <= cx1 ? GridCell(x, z) : nullptr;
            if (c && c->n) {
                // The floor you'd walk to; one you can't reach from here: the one nearest your height, dimmer
                const bool reached = c->first >= 0;
                KindLook(g_runs[c->run[reached ? c->first : LayerAt(*c, py, 1e9f)]].Kind(), colour, alpha);
                if (!reached) alpha *= 0.45f;
            }
            if (colour != stripColour || alpha != stripAlpha) {
                flush(static_cast<float>(x));
                stripColour = colour;
                stripAlpha = alpha;
                stripX0 = static_cast<float>(x);
            }
        }
    }
    // Footprints: a small oval each, a little left or right of the path; brighter the newer, faint where they're on
    // a floor stacked under or over the one drawn there
    const std::vector<float>& stepAlpha = StepAlphas();
    const float foot = std::max(4.5f, std::min(7.0f, 0.65f * pxPerMeterX));
    for (size_t i = 0; i < g_steps.size(); ++i) {
        const Step& s = g_steps[i];
        if (s.side > 1 || stepAlpha[i] <= 0.0f || (g_curDungeon >= 0 && !NearDungeon(s.x, s.z, g_curDungeon))) continue;
        // Across the path: perpendicular to the way to the next step (or from the last one)
        const Step& a = i > 0 && g_steps[i - 1].side <= 1 ? g_steps[i - 1] : s;
        const Step& b = i + 1 < g_steps.size() && g_steps[i + 1].side <= 1 ? g_steps[i + 1] : s;
        float dx = b.x - a.x, dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 0.01f) {
            dx /= len;
            dz /= len;
        }
        const float off = (s.side ? 0.3f : -0.3f);
        const float wx = s.x - dz * off, wz = s.z + dx * off;
        const float sx = vcx + (wx - px) * pxPerMeterX, sy = vcy - (wz - pz) * pxPerMeterY;
        if (sx < vx + 2.0f || sx > vx + vw - 2.0f || sy < vy + 2.0f || sy > vy + vh - 2.0f) continue;
        float alpha = stepAlpha[i];
        if (const Cell* c = GridCell(static_cast<int>(std::floor(s.x)), static_cast<int>(std::floor(s.z)))) {
            const int l = LayerAt(*c, s.y, 3.0f);
            if (l >= 0 && c->first >= 0 && l != c->first) alpha *= 0.25f;
        }
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - foot * 0.5f, sy - foot * 0.5f, foot, foot, 0xff1a6cf0u, alpha); // deep amber: stands out on pale stone, goo and rock
    }
}

// Once a frame, while the minimap draws: in a hall? (steps noted there; saved every 30 s)
bool Update(uint8_t* client, float px, float pz, const std::string& world, float& py) {
    const int run = LocalPlayerY(client, py) ? NearestRun(px, py, pz) : -1;
    const int own = run >= 0 ? NearestRun(px, py, pz, true) : -1; // the dungeon's own floor, for which hall it is
    if (run < 0) {
        if (g_stepsDirty && GetTickCount64() - g_stepsSavedTick > 30000ULL) SaveSteps();
        return false;
    }
    // Into another hall: the plan is flooded again with its floors
    if (own >= 0 && g_runDungeon[own] != g_curDungeon) {
        g_curDungeon = g_runDungeon[own];
        g_gSize = 0;
    }
    LoadSteps(world);
    NoteStep(px, py, pz);
    if (g_stepsDirty && GetTickCount64() - g_stepsSavedTick > 30000ULL) SaveSteps();
    return true;
}

// ---- the World Map inside a hall: this dungeon's whole floor plan on a dark ground (the corner of the world it is
// carved in shows nothing useful), its Return Beacons numbered in the order you reach them, the other dungeons'
// markers hidden; a button switches to the overworld, centred on the hall's entrance, and back
int g_mapDungeon = -1; // the dungeon the open map is about (-1: not in one)
bool g_mapOverworld = false;
float g_mapPx = 0.0f, g_mapPy = 0.0f, g_mapPz = 0.0f; // where you stood when the map opened

// Your level: the highest scanned surface under your feet, up to 16 m down (on a placed wall or staircase you
// stand over it: they aren't in the scan), else your height
float GroundUnder(float x, float y, float z) {
    const int zc = static_cast<int>(std::floor(z)), xc = static_cast<int>(std::floor(x));
    int best = -100000;
    for (int zz = zc - 1; zz <= zc + 1; ++zz) {
        if (zz < static_cast<int>(g_z0) || zz >= static_cast<int>(g_z0 + g_rows)) continue;
        const uint32_t r = static_cast<uint32_t>(zz) - g_z0;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            const Run& run = g_runs[i];
            if (xc + 1 < run.x0 || xc - 1 >= run.x0 + run.length) continue;
            if (run.Y() <= y + 2.0f && run.Y() >= y - 16.0f) best = std::max(best, run.Y());
        }
    }
    return best > -100000 ? static_cast<float>(best) : y;
}

// When the World Map opens
void OnMapOpen(uint8_t* client) {
    g_mapDungeon = -1;
    g_mapOverworld = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!client || !GetHudPlayerWorldPos(client, x, z) || !LocalPlayerY(client, y)) return;
    // The dungeon's floor nearest you; on a wall top or a ledge with none in reach, the hall you were last seen in
    const int run = NearestRun(x, y, z, true);
    if (run >= 0) g_mapDungeon = g_runDungeon[run];
    else if (InHall(x, y, z) && g_curDungeon >= 0) g_mapDungeon = g_curDungeon;
    else return;
    g_mapPx = x;
    g_mapPy = GroundUnder(x, y, z);
    g_mapPz = z;
}

// Where the map centres when it opens or switches: the dungeon's middle, or its entrance in the overworld
bool MapFocus(float& x, float& z) {
    if (g_mapDungeon < 0) return false;
    const Dungeon& d = g_dungeons[g_mapDungeon];
    if (g_mapOverworld) {
        if (d.entX == 0.0f && d.entZ == 0.0f) return false;
        x = d.entX;
        z = d.entZ;
    } else {
        x = (d.minX + d.maxX) * 0.5f;
        z = (d.minZ + d.maxZ) * 0.5f;
    }
    return true;
}

bool MapActive() { return g_mapDungeon >= 0 && !g_mapOverworld; }
int g_mapFitStep = -1; // the zoom step (kMapZoomSteps index) that shows the whole dungeon: the map zooms out no further

// The dungeon map's box (world m) while it shows: the dungeon's floors and a margin round them, framed like the
// overworld map's edge
bool MapBounds(float& minX, float& minZ, float& maxX, float& maxZ) {
    if (!MapActive()) return false;
    const Dungeon& d = g_dungeons[g_mapDungeon];
    const float pad = std::max(24.0f, 0.06f * std::max(d.maxX - d.minX, d.maxZ - d.minZ));
    minX = d.minX - pad, minZ = d.minZ - pad, maxX = d.maxX + pad, maxZ = d.maxZ + pad;
    return true;
}

// The dungeon's size (m) while its map shows
bool MapExtent(float& w, float& h) {
    if (!MapActive()) return false;
    const Dungeon& d = g_dungeons[g_mapDungeon];
    w = d.maxX - d.minX;
    h = d.maxZ - d.minZ;
    return true;
}

// While the dungeon map shows it is that dungeon's own map: anything elsewhere (another dungeon, the corner's empty
// rock, the overworld) is hidden
// On the overworld map (and its view from inside a hall) the world's north-west corner shows nothing: the Hollow
// Halls are carved in its rock and there is no island there (no level area), so all it holds is the halls' places
// and the rock around them; the dungeon map shows them
bool InCorner(float x, float z) { return x < kCornerMaxX && z > kCornerMinZ; }
bool MapHides(float x, float z) {
    const bool corner = InCorner(x, z);
    if (!MapActive()) return corner;
    return !corner || !NearDungeon(x, z, g_mapDungeon);
}

// A chest by its hall (chest_halls.h), not by where it is on the map: the halls are stacked over each other
constexpr float kNoHallChestHeight = 30.0f; // a chest of no hall (1, far from them all) shows only this near your height

bool MapHidesChest(size_t chest) {
    const ChestSpawn& sp = kChestSpawns[chest];
    const float x = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
    const float z = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
    if (!MapActive()) return InCorner(x, z);
    const uint8_t hall = kChestHall[chest];
    if (hall == kChestNotInHalls) return true;
    if (hall != kChestNoHall) return hall != g_mapDungeon;
    const float y = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
    return !NearDungeon(x, z, g_mapDungeon) || std::fabs(y - g_mapPy) > kNoHallChestHeight;
}

// On the minimap: only the hall you're in has its chests there (py: your height)
bool MinimapHidesChest(size_t chest, float px, float py, float pz) {
    const uint8_t hall = kChestHall[chest];
    if (hall == kChestNotInHalls) return false;
    if (!InCorner(px, pz)) return true;
    if (hall == kChestNoHall) {
        const ChestSpawn& sp = kChestSpawns[chest];
        const float y = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
        return std::fabs(y - py) > kNoHallChestHeight;
    }
    return g_curDungeon >= 0 && hall != g_curDungeon;
}
// wide: a lore note, boss or unique item, which can lie a room's width from any floor (Tome of Agony, 11 m from the
// Kindle Waste hall's floor, over another hall's): the floor nearest it is looked for further out. Only these few: it
// is too slow for every ore in the rock.
bool MapHidesAt(float x, float y, float z, bool wide = false) {
    if (MapHides(x, z)) return true;
    if (!MapActive()) return false;
    int run = NearestRun(x, y, z, true);
    if (run < 0 && wide) run = NearestRun(x, y, z, true, 48);
    return run >= 0 && g_runDungeon[run] != g_mapDungeon;
}

// HD images use hdpool: fixed native allocations, updated in place before BeginFrame.
// No calls to the screenshot loader, no refunds, and no per-content native allocation.

// ---- HD maps, rendered by the mod: each dungeon seen from above with the game's own textures, at whatever detail the
// zoom needs. The textures ship as hd/mat_<id>.png with hd/materials.txt (tools/resources/make_halls_hd.py textures:
// "<material> <repeats a metre> <png>"; decoded with Windows' image decoder). When the map opens, every 1 m column of
// the dungeon gets the floor nearest the height you stand at (as the plan) and a light from the north-west by those
// heights; tiles of 256 px are rendered from that on a thread of their own, only the ones on screen, at 1 to 16 px a
// metre (the zoom's power of two at or below it), floors not on your level dimmed. Coarser tiles stand in while finer
// ones render. Native GPU slots are reused as tile contents change.
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

bool DecodePng(const std::wstring& path, std::vector<uint32_t>& rgba, int& w, int& h) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        conv->GetSize(&uw, &uh);
        if (uw && uh && uw <= 8192 && uh <= 8192) {
            w = static_cast<int>(uw);
            h = static_cast<int>(uh);
            rgba.resize(static_cast<size_t>(w) * h);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, uw * uh * 4, reinterpret_cast<BYTE*>(rgba.data())));
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

// The materials' textures
struct MatTex {
    int w = 0, h = 0;
    float tiling = 0.0f; // repeats a metre
    std::vector<uint32_t> rgba;
};
MatTex g_matTex[256];
std::atomic<int> g_texState{0}; // 0 not asked, 1 loading, 2 ready, 3 failed

void LoadTextures() {
    FILE* f = nullptr;
    int loaded = 0;
    if (fopen_s(&f, greeneye::DataPath("hd\\materials.txt"), "r") == 0 && f) {
        int mat = 0;
        float tiling = 0.0f;
        char file[64]{};
        while (std::fscanf(f, "%d %f %63s", &mat, &tiling, file) == 3) {
            if (mat < 0 || mat > 255 || !(tiling > 0.0f)) continue;
            char rel[96];
            std::snprintf(rel, sizeof(rel), "hd\\%s", file);
            const std::string path = greeneye::DataPath(rel);
            MatTex& t = g_matTex[mat];
            if (DecodePng(std::wstring(path.begin(), path.end()), t.rgba, t.w, t.h)) {
                t.tiling = tiling;
                ++loaded;
            }
        }
        std::fclose(f);
    }
    if (g_log) g_log("Hollow Halls: %d floor textures loaded for the HD maps", loaded);
    g_texState = loaded ? 2 : 3;
}

// The columns a map shows: per 1 m, the floor drawn (its height and material), its light and whether it's dimmed.
// The dungeon map's: the floor nearest the height you stood at when it opened, dimmed off your level; the minimap's:
// the floor you'd walk to (Flood), dimmed where you can't walk to it from here.
struct LevelGrid {
    uint32_t gen = 0;
    int kind = 0;                      // 0 the dungeon map's, 1 the minimap's
    int x0 = 0, z1 = 0, w = 0, h = 0;  // world: x0 west edge, z1 north edge (exclusive); row 0 north
    std::vector<int16_t> y;            // -1: no floor
    std::vector<uint8_t> mat;
    std::vector<uint8_t> shade;        // light x 128
    std::vector<uint8_t> dim;          // 1: another level
    int At(int x, int z) const {
        const int c = x - x0, r = z1 - 1 - z;
        return c < 0 || r < 0 || c >= w || r >= h ? -1 : r * w + c;
    }
};
std::atomic<uint32_t> g_levelGen{0};
constexpr float kLevelBandM = 8.0f; // up or down: still your level
constexpr float kRockReachM = 20.0f; // bare rock on the dungeon map: this near where you stand

std::shared_ptr<LevelGrid> NewLevelGrid(int kind, int x0, int z1, int w, int h) {
    auto g = std::make_shared<LevelGrid>();
    g->gen = ++g_levelGen;
    g->kind = kind;
    g->x0 = x0;
    g->z1 = z1;
    g->w = w;
    g->h = h;
    const size_t n = static_cast<size_t>(w) * h;
    g->y.assign(n, -1);
    g->mat.assign(n, 0);
    g->shade.assign(n, 128);
    g->dim.assign(n, 0);
    return g;
}

// Light from the north-west by the heights: a rise toward it darkens, a fall brightens (missing neighbours: level)
void ShadeLevelGrid(LevelGrid& g) {
    for (int r = 0; r < g.h; ++r)
        for (int c = 0; c < g.w; ++c) {
            const size_t i = static_cast<size_t>(r) * g.w + c;
            if (g.y[i] < 0) continue;
            auto hAt = [&](int rr, int cc) {
                if (rr < 0 || cc < 0 || rr >= g.h || cc >= g.w) return static_cast<int>(g.y[i]);
                const int v = g.y[static_cast<size_t>(rr) * g.w + cc];
                return v < 0 ? static_cast<int>(g.y[i]) : v;
            };
            const float gx = (hAt(r, c + 1) - hAt(r, c - 1)) * 0.5f, gz = (hAt(r + 1, c) - hAt(r - 1, c)) * 0.5f;
            const float light = std::max(0.55f, std::min(1.3f, 1.0f + 0.12f * (-gx - gz)));
            g.shade[i] = static_cast<uint8_t>(std::min(255L, std::lround(light * 128.0f)));
        }
}

// Per 1 m column: the floor nearest your level. Off your level (dimmed), a roof gives way to the floor under it: built
// blocks with a floor 4 m or more below and none over them (the walking test climbs a building's walls, so its roof
// counts as a floor; from far above it would hide the room under it)
std::shared_ptr<const LevelGrid> BuildMapGrid(int d, float py) {
    const Dungeon& dg = g_dungeons[d];
    auto g = NewLevelGrid(0, static_cast<int>(dg.minX), static_cast<int>(dg.maxZ), static_cast<int>(dg.maxX - dg.minX),
                          static_cast<int>(dg.maxZ - dg.minZ));
    constexpr int kMaxCand = 8;
    struct Cand {
        int16_t y;
        uint8_t mat;
        bool rock;
    };
    std::vector<Cand> cand(static_cast<size_t>(g->w) * kMaxCand);
    std::vector<uint8_t> count(g->w);
    for (int z = static_cast<int>(dg.minZ); z < g->z1; ++z) {
        if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) continue;
        const uint32_t r = static_cast<uint32_t>(z) - g_z0;
        std::fill(count.begin(), count.end(), static_cast<uint8_t>(0));
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            const Run& run = g_runs[i];
            const float dist = std::fabs(run.Y() - py);
            // The dungeon's floors, and the rock set apart from them (wall tops, ledges) only right around you at
            // your level: you're up on it (elsewhere it's slivers of wall)
            const bool rock = g_runDungeon[i] >= g_dungeonCount;
            if (g_runDungeon[i] != d && !(rock && dist <= kLevelBandM)) continue;
            for (int x = std::max(static_cast<int>(run.x0), g->x0); x < std::min(run.x0 + run.length, g->x0 + g->w); ++x) {
                if (rock && (x - g_mapPx) * (x - g_mapPx) + (z - g_mapPz) * (z - g_mapPz) > kRockReachM * kRockReachM)
                    continue;
                const int cx = x - g->x0;
                if (count[cx] < kMaxCand)
                    cand[static_cast<size_t>(cx) * kMaxCand + count[cx]++] = {static_cast<int16_t>(run.Y()),
                                                                              g_runMaterial[i], rock};
            }
        }
        for (int cx = 0; cx < g->w; ++cx) {
            const Cand* cs = &cand[static_cast<size_t>(cx) * kMaxCand];
            const int n = count[cx];
            int best = -1;
            float bestD = 1e9f;
            for (int k = 0; k < n; ++k) {
                const float dist = std::fabs(cs[k].y - py);
                if (dist > kLevelBandM && cs[k].mat >= 128 && !cs[k].rock) {
                    bool below = false, over = false;
                    for (int m = 0; m < n; ++m) {
                        below |= !cs[m].rock && cs[m].y <= cs[k].y - 4;
                        over |= !cs[m].rock && cs[m].y >= cs[k].y + 4 && cs[m].y <= cs[k].y + 30;
                    }
                    if (below && !over) continue; // a roof
                }
                if (dist < bestD) {
                    bestD = dist;
                    best = k;
                }
            }
            if (best < 0) continue;
            const int c = g->At(g->x0 + cx, z);
            if (c < 0) continue;
            g->y[c] = cs[best].y;
            g->mat[c] = cs[best].mat;
            g->dim[c] = bestD > kLevelBandM ? 1 : 0;
        }
    }
    ShadeLevelGrid(*g);
    return g;
}

// The minimap's, from the walking flood (g_grid): the floor reached first, else (not reachable) the nearest, dimmed
std::shared_ptr<const LevelGrid> BuildMiniGrid(float py) {
    auto g = NewLevelGrid(1, g_gx0, g_gz0 + g_gSize, g_gSize, g_gSize);
    for (int gz = 0; gz < g_gSize; ++gz)
        for (int gx = 0; gx < g_gSize; ++gx) {
            const Cell& cell = g_grid[static_cast<size_t>(gz) * g_gSize + gx];
            if (!cell.n) continue;
            const bool reached = cell.first >= 0;
            int l = cell.first;
            if (!reached) { // not reachable: the dungeon's own floor nearest your height (no bare rock), dimmed
                float bestD = 1e9f;
                for (int k = 0; k < cell.n; ++k) {
                    const float dd = std::fabs(g_runs[cell.run[k]].Y() - py);
                    if (g_runDungeon[cell.run[k]] < g_dungeonCount && dd < bestD) {
                        bestD = dd;
                        l = k;
                    }
                }
            }
            if (l < 0) continue;
            const int c = g->At(g_gx0 + gx, g_gz0 + gz);
            if (c < 0) continue;
            g->y[c] = static_cast<int16_t>(g_runs[cell.run[l]].Y());
            g->mat[c] = g_runMaterial[cell.run[l]];
            g->dim[c] = reached ? 0 : 1;
        }
    ShadeLevelGrid(*g);
    return g;
}

// ---- tiles: 256 px, on the world's grid (tile tx covers x tx*span .. (tx+1)*span, tz covers z tz*span ..), so a
// tile of the next grid (the minimap's after a few steps) lies where the last one's did and stands in until it's ready
constexpr int kTilePx = 256, kMaxTileLevel = 4; // 2^level px a metre
struct TileKey {
    uint32_t gen;
    int kind, level, tx, tz;
    uint64_t Pack() const {
        return (static_cast<uint64_t>(gen) << 40) | (static_cast<uint64_t>(level) << 36) |
               (static_cast<uint64_t>(tx & 0x3ffff) << 18) | static_cast<uint64_t>(tz & 0x3ffff);
    }
    uint64_t Spot() const { // the same tile in any grid of this kind
        return (static_cast<uint64_t>(kind) << 40) | (static_cast<uint64_t>(level) << 36) |
               (static_cast<uint64_t>(tx & 0x3ffff) << 18) | static_cast<uint64_t>(tz & 0x3ffff);
    }
};
struct Tile {
    std::vector<uint32_t> rgba; // empty: no floor at all in it
    uint64_t imageKey = 0;      // the game's image for it: its spot and pixels (a new grid that draws it the same keeps it)
    uint64_t lastUse = 0;
    TileKey key{};
};
struct TileJob {
    uint64_t packed;
    TileKey key;
    std::shared_ptr<const LevelGrid> grid;
};
std::mutex g_tileLock;
std::unordered_map<uint64_t, std::shared_ptr<Tile>> g_tiles;
std::unordered_map<uint64_t, uint64_t> g_latestAtSpot; // Spot() -> the newest ready tile there
std::shared_ptr<const LevelGrid> g_liveGrid[2];        // the grids drawn now: [0] the dungeon map's, [1] the minimap's
std::vector<TileJob> g_tileQueue;                       // newest last
std::unordered_set<uint64_t> g_tileQueued;
bool g_tileWorker = false;
constexpr size_t kMaxTiles = 200; // ~50 MiB CPU cache; GPU residency is bounded separately by hdpool

void RenderTile(const LevelGrid& g, const TileKey& k, std::vector<uint32_t>& out) {
    const float ppm = static_cast<float>(1 << k.level), span = kTilePx / ppm;
    const float wx0 = k.tx * span, wzTop = (k.tz + 1) * span;
    out.assign(static_cast<size_t>(kTilePx) * kTilePx, 0u);
    bool any = false;
    for (int j = 0; j < kTilePx; ++j) {
        const float wz = wzTop - (j + 0.5f) / ppm;
        const int cz = static_cast<int>(std::floor(wz));
        for (int i = 0; i < kTilePx; ++i) {
            const float wx = wx0 + (i + 0.5f) / ppm;
            const int c = g.At(static_cast<int>(std::floor(wx)), cz);
            if (c < 0 || g.y[c] < 0) continue;
            const uint8_t m = g.mat[c];
            uint32_t px;
            const MatTex& t = g_matTex[m];
            if (t.w) {
                const float u = wx * t.tiling, v = -wz * t.tiling;
                const int tu = static_cast<int>((u - std::floor(u)) * t.w) % t.w;
                const int tv = static_cast<int>((v - std::floor(v)) * t.h) % t.h;
                px = t.rgba[static_cast<size_t>(tv) * t.w + tu];
            } else {
                px = g_materialColour ? g_materialColour[m] : 0xff707070u; // 0xAABBGGRR, as RGBA bytes
            }
            // Light (bilinear between the columns' centres) and another level, dimmed
            const float fx = wx - 0.5f, fz = wz - 0.5f;
            const int ix = static_cast<int>(std::floor(fx)), iz = static_cast<int>(std::floor(fz));
            const float ax = fx - ix, az = fz - iz;
            auto sh = [&](int x, int z) {
                const int cc = g.At(x, z);
                return cc < 0 || g.y[cc] < 0 ? g.shade[c] / 128.0f : g.shade[cc] / 128.0f;
            };
            float light = (sh(ix, iz) * (1 - ax) + sh(ix + 1, iz) * ax) * (1 - az) +
                          (sh(ix, iz + 1) * (1 - ax) + sh(ix + 1, iz + 1) * ax) * az;
            if (g.dim[c]) light *= 0.6f;
            const int r = std::min(255, static_cast<int>((px & 0xff) * light));
            const int gg = std::min(255, static_cast<int>(((px >> 8) & 0xff) * light));
            const int b = std::min(255, static_cast<int>(((px >> 16) & 0xff) * light));
            out[static_cast<size_t>(j) * kTilePx + i] = 0xff000000u | (b << 16) | (gg << 8) | r;
            any = true;
        }
    }
    if (!any) out.clear();
}

bool GridLive(uint32_t gen) {
    return (g_liveGrid[0] && g_liveGrid[0]->gen == gen) || (g_liveGrid[1] && g_liveGrid[1]->gen == gen);
}

void TileWorker() {
    greeneye::Lease lease; // a hot reload waits for the tile in hand
    for (;;) {
        TileJob job;
        {
            std::lock_guard<std::mutex> lock(g_tileLock);
            // Only the live grids' tiles; the newest asked first
            while (!g_tileQueue.empty() && !GridLive(g_tileQueue.back().key.gen)) {
                g_tileQueued.erase(g_tileQueue.back().packed);
                g_tileQueue.pop_back();
            }
            if (!lease || g_tileQueue.empty()) {
                g_tileWorker = false;
                return;
            }
            job = std::move(g_tileQueue.back());
            g_tileQueue.pop_back();
        }
        Tile t;
        t.key = job.key;
        RenderTile(*job.grid, job.key, t.rgba);
        uint64_t h = 0xcbf29ce484222325ull ^ job.key.Spot();
        for (const uint32_t px : t.rgba) h = (h ^ px) * 0x100000001b3ull;
        t.imageKey = h;
        std::lock_guard<std::mutex> lock(g_tileLock);
        g_tileQueued.erase(job.packed);
        t.lastUse = GetTickCount64();
        if (g_tiles.size() >= kMaxTiles) {
            auto oldest = g_tiles.begin();
            for (auto it = g_tiles.begin(); it != g_tiles.end(); ++it)
                if (it->second->lastUse < oldest->second->lastUse) oldest = it;
            if (g_latestAtSpot[oldest->second->key.Spot()] == oldest->first) g_latestAtSpot.erase(oldest->second->key.Spot());
            g_tiles.erase(oldest);
        }
        g_latestAtSpot[job.key.Spot()] = job.packed;
        g_tiles[job.packed] = std::make_shared<Tile>(std::move(t));
    }
}

// The tile to draw for a spot: this grid's when ready (else asked for), or the newest one of an earlier grid there
std::shared_ptr<const Tile> TileFor(const TileKey& k, const std::shared_ptr<const LevelGrid>& grid) {
    const uint64_t key = k.Pack();
    std::lock_guard<std::mutex> lock(g_tileLock);
    auto it = g_tiles.find(key);
    if (it != g_tiles.end()) {
        it->second->lastUse = GetTickCount64();
        return it->second;
    }
    if (g_tileQueued.insert(key).second) g_tileQueue.push_back({key, k, grid});
    if (!g_tileWorker) {
        g_tileWorker = true;
        std::thread(TileWorker).detach();
    }
    auto latest = g_latestAtSpot.find(k.Spot());
    if (latest != g_latestAtSpot.end()) {
        auto old = g_tiles.find(latest->second);
        if (old != g_tiles.end()) return old->second;
    }
    return nullptr;
}

// The floor textures (loaded once, on a thread of their own): ready?
bool HdTexturesReady() {
    int expected = 0;
    if (g_texState.compare_exchange_strong(expected, 1)) {
        std::thread([] {
            greeneye::Lease lease;
            if (lease) LoadTextures();
            else g_texState = 0;
        }).detach();
    }
    return g_texState == 2;
}

// Draws a grid's HD tiles: screen x = ax + world x * kx, screen y = ay - world z * kz, cut to (left, top, right,
// bottom). Tiles at 2^level px a metre, the power of two at or below the screen's (minus `coarser` levels).
void DrawHdTiles(void* widgetRaw, const std::shared_ptr<const LevelGrid>& grid, float ax, float kx, float ay, float kz,
                 float left, float top, float right, float bottom, int coarser = 0) {
    const LevelGrid& g = *grid;
    int level = 0;
    while (level < kMaxTileLevel && (1 << (level + 1)) <= kx) ++level;
    level = std::max(0, level - coarser);
    const float span = kTilePx / static_cast<float>(1 << level);
    const float wxL = std::max(static_cast<float>(g.x0), (left - ax) / kx);
    const float wxR = std::min(static_cast<float>(g.x0 + g.w), (right - ax) / kx);
    const float wzT = std::min(static_cast<float>(g.z1), (ay - top) / kz);
    const float wzB = std::max(static_cast<float>(g.z1 - g.h), (ay - bottom) / kz);
    if (wxR <= wxL || wzT <= wzB) return;
    const uint64_t colour = MakePackedColor(0xffffffffu, 1.0f, 1.0f);
    // The pool uploads a few tiles a frame: until a spot's new image is up, the one it showed last stays (it would
    // flash to the plain plan otherwise). Per spot, the image last drawn there.
    static std::unordered_map<uint64_t, uint64_t> s_shown;
    if (s_shown.size() > 4096) s_shown.clear();
    // False: nothing drawn (the image not up yet, and none before it)
    auto drawTile = [&](const Tile& t, uint64_t spot, float u0, float v0, float u1, float v1, float sx0, float sy0,
                        float sx1, float sy1) {
        if (t.rgba.empty()) return false;
        const float cx0 = std::max(sx0, left), cx1 = std::min(sx1, right), cy0 = std::max(sy0, top), cy1 = std::min(sy1, bottom);
        if (cx1 <= cx0 || cy1 <= cy0) return true;
        uint32_t id = hdpool::Request(widgetRaw, t.rgba.data(), t.imageKey);
        auto& shown = s_shown[spot];
        if (id) shown = t.imageKey;
        else if (shown && shown != t.imageKey) id = hdpool::Request(widgetRaw, nullptr, shown);
        if (!id) return false;
        const float fu0 = u0 + (u1 - u0) * (cx0 - sx0) / (sx1 - sx0), fu1 = u0 + (u1 - u0) * (cx1 - sx0) / (sx1 - sx0);
        const float fv0 = v0 + (v1 - v0) * (cy0 - sy0) / (sy1 - sy0), fv1 = v0 + (v1 - v0) * (cy1 - sy0) / (sy1 - sy0);
        float rect[4] = {cx0, cy0, cx1 - cx0, cy1 - cy0};
        if (fu0 == 0.0f && fv0 == 0.0f && fu1 == 1.0f && fv1 == 1.0f) g_drawImage(widgetRaw, rect, &id, &colour);
        else if (g_drawImageSubUv) g_drawImageSubUv(widgetRaw, rect, id, PackXY(fu0, fv0), PackXY(fu1, fv1), colour);
        return true;
    };
    for (int tz = static_cast<int>(std::floor(wzT / span)); tz >= static_cast<int>(std::floor(wzB / span)); --tz)
        for (int tx = static_cast<int>(std::floor(wxL / span)); tx <= static_cast<int>(std::floor(wxR / span)); ++tx) {
            const TileKey k{g.gen, g.kind, level, tx, tz};
            const float sx0 = ax + tx * span * kx, sx1 = sx0 + span * kx;
            const float sy0 = ay - (tz + 1) * span * kz, sy1 = sy0 + span * kz;
            if (auto t = TileFor(k, grid))
                if (drawTile(*t, k.Spot(), 0.0f, 0.0f, 1.0f, 1.0f, sx0, sy0, sx1, sy1)) continue;
            // Nothing yet (or its image not up yet): the part of a coarser tile over it, if there is one
            for (int up = 1; up <= level; ++up) {
                const TileKey ck{g.gen, g.kind, level - up, tx >> up, tz >> up};
                std::lock_guard<std::mutex> lock(g_tileLock);
                auto at = g_latestAtSpot.find(ck.Spot());
                if (at == g_latestAtSpot.end()) continue;
                auto it = g_tiles.find(at->second);
                if (it == g_tiles.end()) continue;
                const int mask = (1 << up) - 1;
                const float part = 1.0f / (1 << up);
                const float u0 = (tx & mask) * part, v0 = (mask - (tz & mask)) * part;
                if (drawTile(*it->second, ck.Spot(), u0, v0, u0 + part, v0 + part, sx0, sy0, sx1, sy1)) break;
            }
        }
}

// The dungeon map's HD view. False: no textures (the plan's plain look instead).
bool DrawHdMap(void* widgetRaw, int d, float zoom, float originX, float originY, float mapWorldH, float left, float top,
               float right, float bottom) {
    if (!hdpool::Available()) return false;
    if (!HdTexturesReady() || d < 0) return g_texState == 1; // loading: the dark ground only
    static int s_d = -1;
    static float s_py = 0.0f, s_px = 0.0f, s_pz = 0.0f;
    if (!g_liveGrid[0] || s_d != d || s_py != g_mapPy || s_px != g_mapPx || s_pz != g_mapPz) {
        auto grid = BuildMapGrid(d, g_mapPy);
        s_d = d;
        s_py = g_mapPy;
        s_px = g_mapPx;
        s_pz = g_mapPz;
        std::lock_guard<std::mutex> lock(g_tileLock);
        g_liveGrid[0] = grid;
    }
    DrawHdTiles(widgetRaw, g_liveGrid[0], originX, zoom, originY + mapWorldH * zoom, zoom, left, top, right, bottom);
    return true;
}

// The minimap's HD view, from the walking flood (a new grid each time it floods again). False: not available.
uint32_t g_miniFloodSeen = 0;
bool DrawHdMinimap(void* widgetRaw, float vx, float vy, float vw, float vh, float vcx, float vcy, float px, float pz,
                   float pxPerMeterX, float pxPerMeterY, float py) {
    if (!hdpool::Available() || !HdTexturesReady() || g_gSize == 0) return false;
    if (!g_liveGrid[1] || g_miniFloodSeen != g_floodCount) {
        auto grid = BuildMiniGrid(py);
        g_miniFloodSeen = g_floodCount;
        std::lock_guard<std::mutex> lock(g_tileLock);
        g_liveGrid[1] = grid;
    }
    DrawHdTiles(widgetRaw, g_liveGrid[1], vcx - px * pxPerMeterX, pxPerMeterX, vcy + pz * pxPerMeterY, pxPerMeterY, vx, vy,
                vx + vw, vy + vh);
    return true;
}

// Under the markers: the dark ground, the dungeon seen from above and the footprints
void DrawMapPlan(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!MapActive() || !widgetRaw) return;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH)) return;
    const float left = rootRect[0], top = rootRect[1] + 114.0f; // below the menu banner
    const float right = rootRect[0] + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f) - 64.0f; // above the key hints
    // Like the overworld map: the screen around it in the menu's grey (from the banner's gold line, 88 down, to the key
    // hints), the hall's box dark, framed in the world map's edge line (126, 117, 101) with its soft shadow outside
    const float areaTop = rootRect[1] + 88.0f;
    DrawSolidRectAbs(widgetRaw, left, areaTop, right - left, bottom - areaTop, 0xff2b2b2bu, 1.0f);
    float bx0, bz0, bx1, bz1;
    if (MapBounds(bx0, bz0, bx1, bz1)) {
        const float sx0 = originX + bx0 * zoom, sx1 = originX + bx1 * zoom;
        const float sy0 = originY + (mapWorldH - bz1) * zoom, sy1 = originY + (mapWorldH - bz0) * zoom;
        auto box = [&](float x0, float y0, float x1, float y1, uint32_t colour, float alpha) {
            x0 = std::max(x0, left), x1 = std::min(x1, right), y0 = std::max(y0, areaTop), y1 = std::min(y1, bottom);
            if (x1 > x0 && y1 > y0) DrawSolidRectAbs(widgetRaw, x0, y0, x1 - x0, y1 - y0, colour, alpha);
        };
        for (int i = 3; i >= 1; --i) box(sx0 - i, sy0 - i, sx1 + i, sy1 + i, 0xff393c3fu, 0.25f); // shadow
        box(sx0 - 1.0f, sy0 - 1.0f, sx1 + 1.0f, sy1 + 1.0f, 0xff65757eu, 1.0f);                // the frame line
        box(sx0, sy0, sx1, sy1, 0xff0a0c0bu, 1.0f);                                              // the map
    }
    // The HD map (each column's top floor, the game's textures); over it only what differs: a floor under it you're on,
    // drawn plain, and the top floors not on your level, dimmed
    const bool hd = DrawHdMap(widgetRaw, g_mapDungeon, zoom, originX, originY, mapWorldH, left, top, right, bottom);
    // A top-down view: in each column the floor nearest the height you stood at, lit from the north-west (higher ground
    // lighter, the low side of a drop in its shadow, so walls, steps and pits show), and dimmed when it is not on your
    // level. Sampled on screen cells (the UI renderer drops draws past a few thousand a frame), cells of one look merged
    // into strips per row.
    // Cells of whole metres on the world's grid (not the screen's): panning moves them with the map, it doesn't change
    // what they show
    const int cellM = std::max(1, static_cast<int>(std::ceil(5.0f / zoom)));
    const float cell = cellM * zoom;
    const int gx0 = static_cast<int>(std::floor((left - originX) / zoom / cellM)) * cellM;            // west edge of cell 0
    const int gzTop = static_cast<int>(std::ceil((mapWorldH - (top - originY) / zoom) / cellM)) * cellM; // north edge of row 0
    const int cells = static_cast<int>((right - left) / cell) + 3;
    static std::vector<float> bestD;
    static std::vector<int> bestY, aboveY, topY; // aboveY: the row before (north), -1 none; topY: the column's top floor
    static std::vector<uint8_t> bestKind, bestMat;
    aboveY.assign(static_cast<size_t>(cells), -1);
    constexpr float kLevelBand = 8.0f; // up or down: still your level
    constexpr int kHdVeil = 0x1000;    // a look: the HD map's floor, dimmed
    for (int row = 0; !hd; ++row) { // the HD map shows your level itself; plain cells without it
        const int zNorth = gzTop - row * cellM, z = zNorth - cellM; // the row's southmost metre is sampled
        const float rowTop = originY + (mapWorldH - zNorth) * zoom;
        if (rowTop >= bottom) break;
        const float y = std::max(top, rowTop), rowH = std::min(bottom, rowTop + cell) - y;
        if (rowH <= 0.0f) {
            aboveY.assign(static_cast<size_t>(cells), -1);
            continue;
        }
        bestD.assign(static_cast<size_t>(cells), 1e9f);
        bestY.assign(static_cast<size_t>(cells), -1);
        topY.assign(static_cast<size_t>(cells), -1);
        bestKind.assign(static_cast<size_t>(cells), 0);
        bestMat.assign(static_cast<size_t>(cells), 0);
        if (z >= static_cast<int>(g_z0) && z < static_cast<int>(g_z0 + g_rows)) {
            const uint32_t r = static_cast<uint32_t>(z) - g_z0;
            for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
                if (g_runDungeon[i] != g_mapDungeon) continue;
                const Run& run = g_runs[i];
                int c0 = static_cast<int>(std::floor(static_cast<float>(run.x0 - gx0) / cellM));
                int c1 = static_cast<int>(std::floor(static_cast<float>(run.x0 + run.length - 1 - gx0) / cellM));
                if (c1 < 0 || c0 >= cells) continue;
                c0 = std::max(0, c0);
                c1 = std::min(cells - 1, c1);
                const float d = std::fabs(run.Y() - g_mapPy);
                for (int c = c0; c <= c1; ++c) {
                    topY[c] = std::max(topY[c], run.Y());
                    if (d < bestD[c]) {
                        bestD[c] = d;
                        bestY[c] = run.Y();
                        bestKind[c] = static_cast<uint8_t>(run.Kind());
                        bestMat[c] = g_runMaterial[i];
                    }
                }
            }
        }
        // Each cell's look: kind, light (quantized to tenths) and whether it is on your level
        // look: the material + 1 (0 no floor)
        auto look = [&](int c, int& kind, int& light, bool& onLevel) {
            kind = c < cells && bestKind[c] ? bestMat[c] + 1 : 0;
            if (hd && kind && bestY[c] == topY[c]) {
                // The HD map shows this floor: nothing over it on your level, a dark veil on another
                kind = std::fabs(bestY[c] - g_mapPy) <= kLevelBand ? 0 : kHdVeil;
                light = 10;
                onLevel = true;
                return;
            }
            light = 10;
            onLevel = true;
            if (!kind) return;
            const int h = bestY[c];
            float f = 1.0f + std::max(-0.3f, std::min(0.2f, (h - g_mapPy) / 60.0f)); // higher lighter, lower darker
            const int north = aboveY[c], west = c > 0 ? bestY[c - 1] : -1;
            if ((north >= 0 && north - h >= 2) || (west >= 0 && west - h >= 2)) f *= 0.62f;      // under a drop: shadow
            else if ((north >= 0 && h - north >= 2) || (west >= 0 && h - west >= 2)) f *= 1.12f; // a rise's lit edge
            light = static_cast<int>(std::lround(f * 10.0f));
            onLevel = std::fabs(h - g_mapPy) <= kLevelBand;
        };
        int stripKind = 0, stripLight = 10, stripC = 0;
        bool stripOnLevel = true;
        for (int c = 0; c <= cells; ++c) {
            int k = 0, light = 10;
            bool onLevel = true;
            look(c, k, light, onLevel);
            if (k == stripKind && (!k || (light == stripLight && onLevel == stripOnLevel))) continue;
            const float x0 = std::max(left, originX + (gx0 + stripC * cellM) * zoom);
            const float x1 = std::min(right, originX + (gx0 + c * cellM) * zoom);
            if (stripKind == kHdVeil && x1 > x0) {
                DrawSolidRectAbs(widgetRaw, x0, y, x1 - x0, rowH, 0xff0a0c0bu, 0.6f);
            } else if (stripKind && x1 > x0) {
                // What the floor is made of, as the game colours it; another level shows through faintly
                const uint32_t colour = g_materialColour[stripKind - 1];
                const float alpha = stripOnLevel ? 1.0f : 0.4f;
                DrawSolidRectAbs(widgetRaw, x0, y, x1 - x0, rowH, colour, alpha, stripLight / 10.0f);
            }
            stripKind = k;
            stripLight = light;
            stripOnLevel = onLevel;
            stripC = c;
        }
        aboveY.swap(bestY);
    }
    // Footprints in this dungeon: at most 600 (the draw budget), the newest
    const std::vector<float>& stepAlpha = StepAlphas();
    const float foot = std::max(3.0f, std::min(6.0f, 0.65f * zoom));
    size_t shown = 0;
    for (size_t i = g_steps.size(); i-- > 0 && shown < 600;) {
        const Step& s = g_steps[i];
        if (s.side > 1 || stepAlpha[i] <= 0.0f || !(s.x < kCornerMaxX && s.z > kCornerMinZ) || MapHides(s.x, s.z)) continue;
        const float sx = originX + s.x * zoom, sy = originY + (mapWorldH - s.z) * zoom;
        if (sx < left || sx > right || sy < top || sy > bottom) continue;
        const float alpha = stepAlpha[i];
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - foot * 0.5f, sy - foot * 0.5f, foot, foot, 0xff1a6cf0u, alpha);
        ++shown;
    }
}

#ifdef UMM_DEV_BRIDGE
// Real dungeon data, worker and draw path, without moving the player. The probe
// cycles beacon floors and zoom levels and is bounded by hdpool's 60-second timer.
void DrawPoolTest(void* widget,const float* rect) {
    if(!hdpool::g_testHalls || GetTickCount64()>=hdpool::g_testUntil || !Load() || !HdTexturesReady() || !g_dungeonCount)return;
    static uint64_t phaseSeen=UINT64_MAX;
    static Beacon centre{};
    const uint64_t phase=GetTickCount64()/5000;
    if(phaseSeen!=phase){
        phaseSeen=phase;
        const unsigned d=unsigned(phase%g_dungeonCount);const auto& dungeon=g_dungeons[d];
        if(!dungeon.beaconCount)return;
        centre=g_beacons[dungeon.firstBeacon+(phase/g_dungeonCount)%dungeon.beaconCount];
        const float oldX=g_mapPx,oldZ=g_mapPz;g_mapPx=centre.x;g_mapPz=centre.z;
        auto grid=BuildMapGrid(d,centre.y);g_mapPx=oldX;g_mapPz=oldZ;
        std::lock_guard lock(g_tileLock);g_liveGrid[0]=grid;
    }
    if(!g_liveGrid[0])return;
    const float left=rect[0]+180,top=rect[1]+180,w=900,h=600,zoom=(phase%2)?4.0f:8.0f;
    DrawSolidRectAbs(widget,left,top,w,h,0xff0a0c0bu,1.0f);
    DrawHdTiles(widget,g_liveGrid[0],left+w/2-centre.x*zoom,zoom,top+h/2+centre.z*zoom,zoom,left,top,left+w,top+h);
}
#endif

// Hollow Halls Progression (another mod in the same game process, optional): it publishes the beacons its host has on
// record for you in a small shared block, Local\\HollowHallsProgression.<pid>, and takes a journey request there. Its
// halls and beacon numbers are the ones in halls.bin.
struct HhpLink {          // 20 bytes, as Hollow Halls Progression lays it out
    uint32_t magic;       // 'HHL1'
    volatile LONG known;  // 1 while masks hold the host's record
    uint8_t masks[8];     // per hall: bit n = beacon n + 1 reached
    volatile LONG request; // 0 none, else 0x80000000 | hall << 8 | beacon (from 0)
};
static_assert(sizeof(HhpLink) == 20, "Hollow Halls Progression link layout");

HhpLink* HhpBlock() {
    static HhpLink* block = nullptr;
    static uint64_t nextTry = 0;
    if (block) return block;
    const uint64_t now = GetTickCount64();
    if (now < nextTry) return nullptr;
    nextTry = now + 2000;
    wchar_t name[64];
    swprintf_s(name, L"Local\\HollowHallsProgression.%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return nullptr;
    auto* view = static_cast<HhpLink*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HhpLink)));
    CloseHandle(mapping); // the view keeps it
    if (view && view->magic != 0x314C4848u) {
        UnmapViewOfFile(view);
        view = nullptr;
    }
    block = view;
    return block;
}

// Over the markers: each Return Beacon's number on a badge beside it (in Hollow Halls Progression's order), and you.
// With Hollow Halls Progression a beacon you've reached is bright and a click travels there; one you haven't is grey.
// Returns true while a badge is under the mouse.
bool DrawMapBeacons(void* widgetRaw, const float* rootRect, uint8_t* mapState, bool haveCursor, float mx, float my,
                    bool clicked) {
    if (!MapActive() || !widgetRaw) return false;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH)) return false;
    const float top = rootRect[1] + 114.0f;
    const Dungeon& d = g_dungeons[g_mapDungeon];
    HhpLink* hhp = HhpBlock();
    const bool travel = hhp && hhp->known == 1;
    constexpr float kBadge = 26.0f;
    bool anyHovered = false;
    for (uint32_t i = 0; i < d.beaconCount; ++i) {
        const Beacon& b = g_beacons[d.firstBeacon + i];
        const float sx = originX + b.x * zoom + 12.0f, sy = originY + (mapWorldH - b.z) * zoom - 14.0f;
        if (sy - kBadge * 0.5f < top) continue;
        const bool reached = travel && i < 8 && (hhp->masks[g_mapDungeon] >> i & 1);
        // The badge or the beacon's icon under it (one title for both)
        const float ix = sx - 12.0f, iy = sy + 14.0f;
        const bool hovered = haveCursor && ((std::fabs(mx - sx) <= kBadge * 0.5f + 2.0f && std::fabs(my - sy) <= kBadge * 0.5f + 2.0f) ||
                                            (std::fabs(mx - ix) <= 12.0f && std::fabs(my - iy) <= 12.0f));
        const float ring = hovered ? kBadge + 10.0f : kBadge + 4.0f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring,
                         travel && !reached ? 0xff8c9296u : hovered && reached ? 0xff7ae6ffu : 0xff3cc8f8u, 0.95f); // amber, grey
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - kBadge * 0.5f, sy - kBadge * 0.5f, kBadge, kBadge, 0xff0a0e10u, 0.92f);
        char num[8];
        std::snprintf(num, sizeof(num), "%u", i + 1);
        DrawLabelAbs(sx - kBadge * 0.5f, sy - kBadge * 0.5f, kBadge, kBadge, num, travel && !reached ? 0xffb4b4b4u : 0xffffffffu,
                     travel && !reached ? 0.7f : 1.0f, TextAlignH::Center, TextAlignV::Center);
        if (hovered) {
            anyHovered = true;
            char title[128];
            std::snprintf(title, sizeof(title), "%s %u", OreName(kBeaconsFirst), i + 1);
            DrawMapHoverTitle(sx + kBadge * 0.5f + 10.0f, sy, title);
            if (clicked && reached) InterlockedExchange(&hhp->request, static_cast<LONG>(0x80000000u | g_mapDungeon << 8 | i));
        }
    }
    return anyHovered; // you and the other players: DrawDungeonMapPlayers (map_key_panel.cpp)
}

} // namespace halls

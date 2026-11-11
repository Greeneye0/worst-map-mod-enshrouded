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
void DrawMapHoverTitle(float x, float y, const char* name, bool gameTitle = false);

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
    // beacons | paired teleport pads << 8 | exit pads << 16: a vault's pads follow its beacons, the pairs side by side
    // (pad 2k takes you to 2k + 1 and back), then the pads that take you out to its Ancient Lands entrance; halls have
    // none. The links are the server's own (tools/resources/vault_pads_server.py).
    uint32_t firstBeacon, beaconCount;
    uint32_t Beacons() const { return beaconCount & 0xffu; }
    uint32_t Pads() const { return beaconCount >> 8 & 0xffu; }
    uint32_t Exits() const { return beaconCount >> 16 & 0xffu; }
    // Its floors, broken up by its teleporters (tools/resources/floors.py), after the exits: {lowest m, highest m, 0}
    uint32_t Floors() const { return beaconCount >> 24 & 0xffu; }
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
uint16_t g_own[kOwnH][kOwnW]{};
// Dungeons 0-5 are the Hollow Halls, in the corner; from 6 the Ancient Vaults (ancientLands_beacon_dungeon_01-06: Engine
// Chambers and the others), out over the island, above or under its ground: one is told by its box and its floors'
// heights, not by a corner (make_halls.py, from scan_vault.py + trim_vault.py)
constexpr uint32_t kHallCount = 6, kMaxDungeons = 32;
float g_minY[kMaxDungeons]{}, g_maxY[kMaxDungeons]{}; // each dungeon's floor heights
bool IsVault(int d) { return d >= static_cast<int>(kHallCount) && d < static_cast<int>(g_dungeonCount); }
int FloorCount(int d);
int FloorAt(int d, float y); // a vault's or spire's floors (below)
constexpr uint32_t kVaultCount = 6; // dungeons 6-11 the Ancient Vaults, from 12 the Ancient Spires (make_halls.py)
int VaultAt(float x, float y, float z);
// In an Ancient Spire: the minimap zooms on its own, closer scale (a spire is some 70 m across)
bool InSpire(float x, float y, float z) {
    return VaultAt(x, y, z) >= static_cast<int>(kHallCount + kVaultCount); // none while halls.bin isn't loaded
}
// The vault whose box holds this spot: 12 m round its floors, 20 m under the lowest and 25 m over the highest; -1 none
int VaultAt(float x, float y, float z) {
    for (uint32_t d = kHallCount; d < g_dungeonCount; ++d) {
        const Dungeon& v = g_dungeons[d];
        if (x >= v.minX - 12.0f && x <= v.maxX + 12.0f && z >= v.minZ - 12.0f && z <= v.maxZ + 12.0f &&
            y >= g_minY[d] - 20.0f && y <= g_maxY[d] + 25.0f)
            return static_cast<int>(d);
    }
    return -1;
}

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
    if (!read || g_rows == 0 || g_rows > 12288 || g_dungeonCount > kMaxDungeons || need != g_data.size()) {
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
    for (uint32_t d = 0; d < kMaxDungeons; ++d) g_minY[d] = 1e9f, g_maxY[d] = -1e9f;
    for (uint32_t r = 0; r < g_rows; ++r) {
        const int oz = (static_cast<int>(g_z0 + r) - kOwnZ0) / kOwnCell;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            const uint8_t d = g_runDungeon[i];
            if (d >= g_dungeonCount) continue;
            g_minY[d] = std::min(g_minY[d], static_cast<float>(g_runs[i].Y()));
            g_maxY[d] = std::max(g_maxY[d], static_cast<float>(g_runs[i].Y()));
            if (oz < 0 || oz >= kOwnH || d >= kHallCount) continue;
            for (int ox = g_runs[i].x0 / kOwnCell; ox <= (g_runs[i].x0 + g_runs[i].length - 1) / kOwnCell && ox < kOwnW; ++ox)
                g_own[oz][ox] |= static_cast<uint16_t>(1u << d);
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
    if (!Load() || (!(px < kCornerMaxX && pz > kCornerMinZ) && VaultAt(px, py, pz) < 0)) return -1;
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
void DrawFootprint(void* w, float sx, float sy, float fx, float fy, float size, uint8_t side, uint32_t colour, float alpha); // below
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
    if (IsVault(d)) {
        const Dungeon& v = g_dungeons[d];
        return x >= v.minX - 16.0f && x <= v.maxX + 16.0f && z >= v.minZ - 16.0f && z <= v.maxZ + 16.0f;
    }
    const int ox = static_cast<int>(x) / kOwnCell, oz = (static_cast<int>(z) - kOwnZ0) / kOwnCell;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = ox + dx, cz = oz + dz;
            if (cx >= 0 && cz >= 0 && cx < kOwnW && cz < kOwnH && (g_own[cz][cx] & (1u << d))) return true;
        }
    return false;
}

// Pins: Explored inside a Hollow Hall: the game's fog of war never clears in the halls' corner, so a spot there counts
// as explored when it belongs to the hall you're in (or were last in: the World Map shows a hall only from inside it)
bool ExploredInHall(float x, float z, bool& inCorner) {
    inCorner = x < kCornerMaxX && z > kCornerMinZ && Load();
    return inCorner && g_curDungeon >= 0 && g_curDungeon < static_cast<int>(kHallCount) && NearDungeon(x, z, g_curDungeon);
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
                if (c.n < kLayers) {
                    c.run[c.n++] = i;
                    continue;
                }
                // Full (a spire's columns hold up to 12 floors): the ones nearest your height stay, not the row's
                // first few, which left your own floor out at a spire's pillar rows (2026-11-09)
                int worst = 0;
                for (int k = 1; k < kLayers; ++k)
                    if (std::fabs(g_runs[c.run[k]].Y() - py) > std::fabs(g_runs[c.run[worst]].Y() - py)) worst = k;
                if (std::fabs(run.Y() - py) < std::fabs(g_runs[c.run[worst]].Y() - py)) c.run[worst] = i;
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

// ---- traps in the vaults and spires (data/traps.bin, tools/resources/make_traps.py): spike plates (4 x 4 m), fireball
// launchers with the lane their balls roll down, electric and beam traps across a corridor, and trap rails
struct Trap {
    uint8_t kind, pad[3]; // 1 spikes, 2 fireballs, 3 electric, 4 rail; pad[0] bit 0: a door a switch opens
    float x, y, z, ex, ez; // ex, ez: a lane's / beam's far end
};
static_assert(sizeof(Trap) == 24, "traps.bin records");
std::vector<Trap> g_traps;
bool g_trapsTried = false;
unsigned g_mapTrapKinds = 0; // the kinds the dungeon map drew this frame (its legend)
// Per spike plate: which of its 4 x 4 one-metre cells have floor at its height (bit row * 4 + column, row 0 north):
// a plate against a wall drew over the wall (2026-11-09)
std::vector<uint16_t> g_trapMask;
// A switch with no floor round it at its own height (a target high on a wall over a room: 2026-11-09, a shootable 13.5 m
// over a spire's floor 2, nothing of floor 3 near it) also belongs to the floor it hangs over: that floor's height (NAN:
// none, it's on its own level)
std::vector<float> g_trapAltY;
// The switches that turn spike plates off (traps.bin "SRC1", from the scene's signal wiring) and each trap's (up to 2)
struct SwitchPos { float x, y, z; };
std::vector<SwitchPos> g_switchPos;
std::vector<std::array<uint16_t, 2>> g_trapSrc;
// The 1 m cells the barred windows (kind 7) stand in, and the floor height they stand on: no floor is drawn there (on
// that level), so a window reads as the wall it is, the same as the stone round it
// One entry per window: a spire's floors stack windows in the same cell, and with one height a cell the last floor's
// took the others' place, so those drew as open floor (2026-11-09: 501 of them)
std::unordered_multimap<int64_t, int16_t> g_windowCells;
// Per door: the doorway it stands in, wall to wall (a .. b m along x, axis 1, or along z, axis 2; 0: none found), from
// the floor plan: a shut door is drawn as a bar across it. The walls came out so clean that a door's icon alone in a
// gap read as a way through (2026-11-09).
struct DoorSpan {
    float a, b;
    uint8_t axis;
};
std::vector<DoorSpan> g_doorSpan;
int64_t WindowKey(int x, int z) { return static_cast<int64_t>(x) << 32 ^ static_cast<uint32_t>(z); }
bool WindowAt(int x, int z, int y) {
    const auto range = g_windowCells.equal_range(WindowKey(x, z));
    for (auto it = range.first; it != range.second; ++it)
        if (std::abs(it->second - y) <= 2) return true;
    return false;
}
bool FloorRunAt(int x, int z, float y, float tol) {
    if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) return false;
    const uint32_t r = static_cast<uint32_t>(z) - g_z0;
    for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
        const Run& run = g_runs[i];
        if (x >= run.x0 && x < run.x0 + run.length && std::fabs(run.Y() - y) <= tol) return true;
    }
    return false;
}

void LoadTraps() {
    if (g_trapsTried) return;
    g_trapsTried = true;
    std::vector<uint8_t> b;
    const std::string trapsPath = greeneye::DataPath("traps.bin");
    HANDLE f = CreateFileA(trapsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart >= 8 && size.QuadPart < (4 << 20)) {
        b.resize(static_cast<size_t>(size.QuadPart));
        if (!ReadFile(f, b.data(), static_cast<DWORD>(b.size()), &got, nullptr) || got != b.size()) b.clear();
    }
    CloseHandle(f);
    uint32_t n = 0;
    if (b.size() >= 8 && std::memcmp(b.data(), "TRP1", 4) == 0) std::memcpy(&n, b.data() + 4, 4);
    const size_t recEnd = 8 + static_cast<size_t>(n) * sizeof(Trap);
    if (!n || b.size() < recEnd) return;
    g_traps.resize(n);
    std::memcpy(g_traps.data(), b.data() + 8, recEnd - 8);
    // "SRC1": the switches that turn spike plates off, then per trap two switch indices (make_traps.py)
    g_trapSrc.assign(n, {0xffffu, 0xffffu});
    g_switchPos.clear();
    uint32_t ns = 0;
    if (b.size() >= recEnd + 8 && std::memcmp(b.data() + recEnd, "SRC1", 4) == 0) std::memcpy(&ns, b.data() + recEnd + 4, 4);
    if (ns && ns < 65535 && b.size() == recEnd + 8 + ns * 12ull + n * 4ull) {
        g_switchPos.resize(ns);
        std::memcpy(g_switchPos.data(), b.data() + recEnd + 8, ns * 12ull);
        std::memcpy(g_trapSrc.data(), b.data() + recEnd + 8 + ns * 12ull, n * 4ull);
    }
    g_trapMask.assign(n, 0xffffu);
    g_windowCells.clear();
    for (uint32_t i = 0; i < n; ++i) {
        const Trap& t = g_traps[i];
        if (t.kind != 7) continue;
        const float hx = t.ex - t.x, hz = t.ez - t.z; // half its length along its way
        const int16_t wy = static_cast<int16_t>(std::lround(t.y));
        for (float s = -1.0f; s <= 1.0f; s += 0.1f) {
            const int64_t key = WindowKey(static_cast<int>(std::floor(t.x + hx * s * 0.95f)), static_cast<int>(std::floor(t.z + hz * s * 0.95f)));
            bool have = false;
            const auto range = g_windowCells.equal_range(key);
            for (auto it = range.first; it != range.second; ++it) have |= it->second == wy;
            if (!have) g_windowCells.emplace(key, wy);
        }
    }
    if (Load()) {
        for (uint32_t i = 0; i < n; ++i) {
            const Trap& t = g_traps[i];
            if (t.kind != 1) continue;
            uint16_t m = 0;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    if (FloorRunAt(static_cast<int>(std::floor(t.x - 2.0f + c + 0.5f)), static_cast<int>(std::floor(t.z + 2.0f - r - 0.5f)), t.y, 2.5f))
                        m |= static_cast<uint16_t>(1u << (r * 4 + c));
            g_trapMask[i] = m;
        }
        g_doorSpan.assign(n, DoorSpan{0.0f, 0.0f, 0});
        for (uint32_t i = 0; i < n; ++i) {
            const Trap& t = g_traps[i];
            if (t.kind != 5) continue;
            // From the door's cell out both ways to the first cell without floor at its height, up to 6 m: the
            // narrower way that ends in wall on both sides is across the doorway
            const int cx = static_cast<int>(std::floor(t.x)), cz = static_cast<int>(std::floor(t.z));
            int best = 99;
            for (int axis = 1; axis <= 2; ++axis) {
                int end[2] = {-1, -1};
                for (int side = 0; side < 2; ++side)
                    for (int k = 0; k <= 6 && end[side] < 0; ++k) {
                        const int o = side ? k : -k;
                        if (!FloorRunAt(axis == 1 ? cx + o : cx, axis == 1 ? cz : cz + o, t.y, 2.0f)) end[side] = k;
                    }
                const int width = end[0] + end[1] - 1;
                if (end[0] < 0 || end[1] < 0 || width < 1 || width >= best) continue;
                best = width;
                const int c0 = axis == 1 ? cx : cz;
                g_doorSpan[i] = {static_cast<float>(c0 - end[0] + 1), static_cast<float>(c0 + end[1]), static_cast<uint8_t>(axis)};
            }
        }
        g_trapAltY.assign(n, NAN);
        for (uint32_t i = 0; i < n; ++i) {
            const Trap& t = g_traps[i];
            if (t.kind < 10) continue;
            bool own = false;
            float under = -1e9f;
            for (int z = static_cast<int>(std::floor(t.z)) - 3; z <= static_cast<int>(std::floor(t.z)) + 3; ++z) {
                if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) continue;
                const uint32_t r = static_cast<uint32_t>(z) - g_z0;
                for (uint32_t k = g_rowStart[r]; k < g_rowStart[r + 1]; ++k) {
                    const Run& run = g_runs[k];
                    if (g_runDungeon[k] >= g_dungeonCount) continue; // bare rock, wall tops
                    if (t.x + 3.0f < run.x0 || t.x - 3.0f >= run.x0 + run.length) continue;
                    const float y = static_cast<float>(run.Y());
                    if (y >= t.y - 3.5f && y <= t.y + 1.0f) own = true;
                    else if (y < t.y - 3.5f && y > t.y - 40.0f) under = std::max(under, y);
                }
            }
            if (!own && under > -1e9f) g_trapAltY[i] = under;
        }
    }
    if (g_log) g_log("Traps: %u in the vaults and spires, %zu switches that turn spikes off", n, g_switchPos.size());
}

// ---- live state (2026-11-09, seen by pressing a switch in Nomad Highlands' spire): a door, a switch or a spike plate's
// entity on the client carries LogicGraphNodeComponentClient (1 byte: 1 its circuit is on), ClientInteractionOffer
// (12 bytes: a verb; 0x4e82e309 on a door that waits for its switch: locked) and, on a button, ClientToggleState (16
// bytes: first u32 1 once pressed). Spikes: from the switches they're wired to (below). A door: that verb = locked, else unlocked (its logic flag is
// only whether it stands open now: it opens as you come near). Component type numbers change per session: looked up by name in the registry's type table (*(*reg + 8): a
// record per type 0x100 apart, {name, length} first).
std::vector<uint8_t> g_trapLive; // per trap: 0 unknown; spikes 1 armed 2 disabled; doors 1 locked 2 closed 3 open;
                                 // switches 1 ready 2 used
constexpr uint32_t kLockedDoorVerb = 0x4e82e309u;
int EcsTypeByName(uint8_t* reg, const char* name) {
    __try {
        const uint8_t* p0 = *reinterpret_cast<uint8_t**>(reg);
        const uint8_t* tbl = p0 ? *reinterpret_cast<uint8_t* const*>(p0 + 8) : nullptr;
        if (!tbl) return -1;
        const size_t want = std::strlen(name);
        for (int i = 0; i < kEcsTypeBits; ++i) {
            const char* s = *reinterpret_cast<const char* const*>(tbl + static_cast<size_t>(i) * 0x100);
            const size_t n = *reinterpret_cast<const size_t*>(tbl + static_cast<size_t>(i) * 0x100 + 8);
            if (!s || n == 0 || n > 96) return -1;
            if (n == want && std::memcmp(s, name, n) == 0) return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}
struct LiveTypes {
    void* reg = nullptr;
    int pos = -1, logic = -1, offer = -1, toggle = -1;
};
LiveTypes g_liveTypes;

void UpdateTrapLiveInner(uint8_t* reg, int site) {
    if (g_liveTypes.reg != reg) {
        g_liveTypes = LiveTypes{reg, EcsTypeByName(reg, "CurrentTransform"), EcsTypeByName(reg, "LogicGraphNodeComponentClient"),
                                EcsTypeByName(reg, "ClientInteractionOffer"), EcsTypeByName(reg, "ClientToggleState")};
        if (g_log) g_log("Traps: live state types position %d logic %d offer %d toggle %d", g_liveTypes.pos, g_liveTypes.logic,
                         g_liveTypes.offer, g_liveTypes.toggle);
    }
    const LiveTypes& t = g_liveTypes;
    if (t.pos < 0 || t.logic < 0) return;
    // this site's doors, switches and spike plates by 2 m cell
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;
    auto key = [](int cx, int cz) { return static_cast<int64_t>(cx) << 32 ^ static_cast<uint32_t>(cz); };
    for (uint32_t i = 0; i < g_traps.size(); ++i) {
        const Trap& tr = g_traps[i];
        if (!(tr.kind == 1 || tr.kind == 5 || tr.kind >= 10) || !NearDungeon(tr.x, tr.z, site)) continue;
        cells[key(static_cast<int>(std::floor(tr.x / 2.0f)), static_cast<int>(std::floor(tr.z / 2.0f)))].push_back(i);
    }
    if (cells.empty()) return;
    // this site's switches that drive spike plates: pressed (ClientToggleState first u32 1) or not, as read below
    std::vector<int> swNear;
    std::vector<uint8_t> swPressed(g_switchPos.size(), 0);
    for (uint32_t i = 0; i < g_switchPos.size(); ++i)
        if (NearDungeon(g_switchPos[i].x, g_switchPos[i].z, site)) swNear.push_back(static_cast<int>(i));
    ForEachEcsEntity(reg, [&](uint8_t* rec) {
        uint16_t sz = 0;
        const uint8_t* logic = EcsComponent(rec, static_cast<uint16_t>(t.logic), &sz);
        if (logic && sz != 1) logic = nullptr;
        const uint8_t* offer = t.offer >= 0 ? EcsComponent(rec, static_cast<uint16_t>(t.offer), &sz) : nullptr;
        if (offer && sz != 12) offer = nullptr;
        const uint8_t* toggle = t.toggle >= 0 ? EcsComponent(rec, static_cast<uint16_t>(t.toggle), &sz) : nullptr;
        if (toggle && sz != 16) toggle = nullptr;
        if (!logic && !offer && !toggle) return;
        float p[3];
        if (!EcsPosition(rec, static_cast<uint16_t>(t.pos), p)) return;
        if (toggle && *reinterpret_cast<const uint32_t*>(toggle) == 1)
            for (int i : swNear) {
                const SwitchPos& s = g_switchPos[i];
                const float dx = s.x - p[0], dy = s.y - p[1], dz = s.z - p[2];
                if (dx * dx + dz * dz < 1.6f * 1.6f && std::fabs(dy) < 2.5f) swPressed[i] = 1;
            }
        const int cx = static_cast<int>(std::floor(p[0] / 2.0f)), cz = static_cast<int>(std::floor(p[2] / 2.0f));
        int best = -1;
        float bestD = 1.6f * 1.6f;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                auto it = cells.find(key(cx + dx, cz + dz));
                if (it == cells.end()) continue;
                for (uint32_t i : it->second) {
                    const Trap& tr = g_traps[i];
                    const float ex = tr.x - p[0], ez = tr.z - p[2];
                    if (std::fabs(tr.y - p[1]) > 2.5f || ex * ex + ez * ez >= bestD) continue;
                    bestD = ex * ex + ez * ez;
                    best = static_cast<int>(i);
                }
            }
        if (best < 0) return;
        const Trap& tr = g_traps[best];
        const bool toggled = toggle && *reinterpret_cast<const uint32_t*>(toggle) == 1;
        uint8_t s = 0;
        // Spikes: set below from their switches (the plate's own logic flag is only "a player is within ~14 m":
        // 2026-11-09, the 1s moved with the player, switched-off and armed plates alike)
        if (tr.kind == 1) s = 0;
        // A door: locked while it carries the locked verb; else unlocked (an Ancient door slides open as you come near:
        // its logic flag is only that, it went green and amber as you walked about)
        // That verb is the door's own "Open" offer (every closed door has it, 2026-11-09): a door a switch opens is
        // locked while it has it; a plain one is closed (opens by hand)
        else if (tr.kind == 5)
            s = offer && *reinterpret_cast<const uint32_t*>(offer) == kLockedDoorVerb ? (tr.pad[0] & 1 ? 1 : 2) : 3;
        else s = toggled ? 2 : 1;
        if (s) g_trapLive[best] = s;
    });
    // A spike plate is off once a switch it's wired to is pressed (the server's ServerToggleState goes 0 then: it never
    // reaches the client, the switch's state does); armed otherwise, and always when it's wired to none
    if (g_trapSrc.size() != g_traps.size()) return;
    for (const auto& cell : cells)
        for (uint32_t i : cell.second) {
            if (g_traps[i].kind != 1) continue;
            bool off = false;
            for (uint16_t k : g_trapSrc[i])
                if (k < swPressed.size() && swPressed[k]) off = true;
            g_trapLive[i] = off ? 2 : 1;
        }
}

// Once a second in a vault or spire: its doors', switches' and spike plates' state as the client has it
void UpdateTrapLive(float px, float py, float pz) {
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 1000ULL) return;
    last = now;
    LoadTraps();
    const int site = VaultAt(px, py, pz);
    if (site < 0 || g_traps.empty()) return;
    if (g_trapLive.size() != g_traps.size()) g_trapLive.assign(g_traps.size(), 0);
    auto* reg = static_cast<uint8_t*>(g_waterRegistry.load(std::memory_order_relaxed));
    for (int r = 0; !reg && r < kMaxWaterRegistries; ++r) reg = static_cast<uint8_t*>(g_waterRegistries[r].load(std::memory_order_relaxed));
    if (!reg) return;
    __try {
        UpdateTrapLiveInner(reg, site);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged && g_log) g_log("Traps: reading the doors' and switches' state failed");
        logged = true;
    }
}

// ---- the fireballs themselves (2026-11-09, Nomad Highlands spire): each rolling magma ball is a client entity with a
// position, UsedItem (0x75f12caa), a VFX and a collider; ~5.5 m/s down its launcher's lane, gone at the lane's end.
// Read ~15 times a second; drawn each frame where it is now (its last position plus its speed since), so the map's
// balls roll with the real ones.
struct Ball {
    float x, y, z, vx, vz;
    uint8_t kind; // 2 a fireball, 3 a spark (an electric trap's; 2026-11-09: projectiles like the fireballs, ~6 m across)
};
std::vector<Ball> g_balls;
uint64_t g_ballsTick = 0; // GetTickCount64() of the last read
int g_ballsUsedItem = -1, g_ballsPos = -1, g_ballsNet = -1, g_ballsLogic = -1;
// Each spike plate's firing: NetworkActor +8 (u64) is the game time it last fired, in ns (2026-11-09, Nomad Highlands
// spire floor 2: steps of exactly 3.0 s, the plates in groups 1 s apart, a wave). When it changes the plate just fired:
// the tick we saw it, and its period from the step.
struct SpikeTime {
    uint64_t fired = 0;   // the game's firing time last read
    uint64_t tick = 0;    // GetTickCount64() when it was seen to change (0: not yet)
    uint32_t periodMs = 0;
};
std::vector<SpikeTime> g_spikeTime;
void* g_ballsReg = nullptr;
void UpdateFireballsInner(uint8_t* reg, int site) {
    if (g_ballsReg != reg) {
        g_ballsReg = reg;
        g_ballsUsedItem = EcsTypeByName(reg, "UsedItem");
        g_ballsPos = EcsTypeByName(reg, "CurrentTransform");
        g_ballsNet = EcsTypeByName(reg, "NetworkActor");
        g_ballsLogic = EcsTypeByName(reg, "LogicGraphNodeComponentClient");
        if (g_log) g_log("Traps: fireball types position %d used item %d", g_ballsPos, g_ballsUsedItem);
    }
    const int posType = g_ballsPos;
    if (posType < 0 || g_ballsUsedItem < 0) return;
    // this site's fire lanes
    struct Lane { float x, y, z, dx, dz, len; uint8_t kind; };
    std::vector<Lane> lanes;
    for (const Trap& tr : g_traps) {
        // fire lanes and the spark-shooting electric traps (not the steady beams: pad[0] bit 1)
        if (!(tr.kind == 2 || (tr.kind == 3 && !(tr.pad[0] & 2))) || !NearDungeon(tr.x, tr.z, site)) continue;
        const float dx = tr.ex - tr.x, dz = tr.ez - tr.z, len = std::hypot(dx, dz);
        if (len > 0.5f) lanes.push_back({tr.x, tr.y, tr.z, dx / len, dz / len, len, tr.kind});
    }
    // this site's armed spike plates by 2 m cell, for their firing times
    std::unordered_map<int64_t, std::vector<uint32_t>> plateCells;
    auto pkey = [](int cx, int cz) { return static_cast<int64_t>(cx) << 32 ^ static_cast<uint32_t>(cz); };
    if (g_spikeTime.size() != g_traps.size()) g_spikeTime.assign(g_traps.size(), SpikeTime{});
    if (g_ballsNet >= 0 && g_ballsLogic >= 0)
        for (uint32_t i = 0; i < g_traps.size(); ++i) {
            const Trap& tr = g_traps[i];
            if (tr.kind == 1 && NearDungeon(tr.x, tr.z, site))
                plateCells[pkey(static_cast<int>(std::floor(tr.x / 2.0f)), static_cast<int>(std::floor(tr.z / 2.0f)))].push_back(i);
        }
    if (lanes.empty() && plateCells.empty()) { g_balls.clear(); return; }
    const uint64_t now = GetTickCount64();
    const float dt = g_ballsTick ? std::min(0.5f, (now - g_ballsTick) / 1000.0f) : 0.0f;
    std::vector<Ball> prev;
    prev.swap(g_balls);
    ForEachEcsEntity(reg, [&](uint8_t* rec) {
        uint16_t sz = 0;
        // a spike plate (it has the logic flag and a network actor): its firing time
        if (!plateCells.empty() && EcsComponent(rec, static_cast<uint16_t>(g_ballsLogic), &sz) && sz == 1) {
            const uint8_t* na = EcsComponent(rec, static_cast<uint16_t>(g_ballsNet), &sz);
            float q[3];
            if (na && sz >= 16 && EcsPosition(rec, static_cast<uint16_t>(posType), q)) {
                auto it = plateCells.find(pkey(static_cast<int>(std::floor(q[0] / 2.0f)), static_cast<int>(std::floor(q[2] / 2.0f))));
                if (it != plateCells.end())
                    for (uint32_t i : it->second) {
                        const Trap& tr = g_traps[i];
                        if (std::fabs(tr.x - q[0]) > 0.6f || std::fabs(tr.z - q[2]) > 0.6f || std::fabs(tr.y - q[1]) > 1.5f) continue;
                        SpikeTime& st = g_spikeTime[i];
                        const uint64_t fired = *reinterpret_cast<const uint64_t*>(na + 8) & 0xffffffffffull;
                        if (st.fired && fired != st.fired) {
                            const uint64_t step = fired > st.fired ? (fired - st.fired) / 1000000ull : 0;
                            if (step >= 500 && step <= 20000) {
                                // a step of several periods (missed reads): the period is the smallest seen
                                st.periodMs = st.periodMs ? static_cast<uint32_t>(std::min<uint64_t>(st.periodMs, step)) : static_cast<uint32_t>(step);
                                st.tick = now;
                            }
                        }
                        st.fired = fired;
                        break;
                    }
            }
            return;
        }
        if (lanes.empty() || g_balls.size() >= 64) return;
        if (!EcsComponent(rec, static_cast<uint16_t>(g_ballsUsedItem), &sz) || sz != 4) return;
        float p[3];
        if (!EcsPosition(rec, static_cast<uint16_t>(posType), p)) return;
        uint8_t laneKind = 0;
        for (const Lane& l : lanes) {
            const float rx = p[0] - l.x, rz = p[2] - l.z, along = rx * l.dx + rz * l.dz;
            if (along < -1.0f || along > l.len + 2.0f || std::fabs(p[1] - l.y) > 4.0f) continue;
            if (std::fabs(rx * l.dz - rz * l.dx) < 1.5f) { laneKind = l.kind; break; }
        }
        if (!laneKind) return;
        // its speed: from the same ball last time (the nearest after moving that one on by its speed)
        Ball b{p[0], p[1], p[2], 0.0f, 0.0f, laneKind};
        float best = 2.5f * 2.5f; // (a spark goes ~1.5 m between reads)
        for (const Ball& o : prev) {
            const float ex = o.x + o.vx * dt - p[0], ez = o.z + o.vz * dt - p[2];
            if (ex * ex + ez * ez >= best || std::fabs(o.y - p[1]) > 2.0f) continue;
            best = ex * ex + ez * ez;
            if (dt > 0.01f) {
                b.vx = (p[0] - o.x) / dt;
                b.vz = (p[2] - o.z) / dt;
                if (std::hypot(b.vx, b.vz) > 40.0f) b.vx = b.vz = 0.0f;
            }
        }
        g_balls.push_back(b);
    });
    g_ballsTick = now;
}
void UpdateFireballs(float px, float py, float pz) {
    const uint64_t now = GetTickCount64();
    if (now - g_ballsTick < 66ULL) return;
    LoadTraps();
    const int site = VaultAt(px, py, pz);
    if (site < 0 || g_traps.empty()) {
        g_balls.clear();
        g_ballsTick = now;
        return;
    }
    auto* reg = static_cast<uint8_t*>(g_waterRegistry.load(std::memory_order_relaxed));
    for (int r = 0; !reg && r < kMaxWaterRegistries; ++r) reg = static_cast<uint8_t*>(g_waterRegistries[r].load(std::memory_order_relaxed));
    if (!reg) return;
    __try {
        UpdateFireballsInner(reg, site);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_balls.clear();
        g_ballsTick = now;
    }
}

// Whether a trap is on a level the band in() takes: its own height, or (a switch hanging over a floor) that floor's
template <class InBand>
bool TrapOnLevel(const Trap& tr, InBand inBand) {
    if (inBand(tr.y)) return true;
    const size_t i = static_cast<size_t>(&tr - g_traps.data());
    return &tr >= g_traps.data() && i < g_traps.size() && g_trapAltY.size() == g_traps.size() && !std::isnan(g_trapAltY[i]) &&
           inBand(g_trapAltY[i]);
}

// The traps visible(trap) picks, toScreen(x, z, sx, sy) placing them, ppm screen px a metre, inside the clip box.
// Which kinds were drawn: a bit each (1 << kind), for the legend
template <class ToScreen, class Visible>
unsigned DrawTraps(void* w, ToScreen toScreen, float ppm, Visible visible, float l, float t, float r, float bt) {
    LoadTraps();
    unsigned kinds = 0;
    // An icon m px across is drawn only when it fits inside the clip box (it spilled over the minimap's edge,
    // 2026-11-09); squares (spike plates, rails) are cut at the box's edge instead
    auto in = [&](float sx, float sy, float m) {
        return sx - m * 0.5f >= l && sx + m * 0.5f <= r && sy - m * 0.5f >= t && sy + m * 0.5f <= bt;
    };
    auto overlaps = [&](float sx, float sy, float half) { return sx + half > l && sx - half < r && sy + half > t && sy - half < bt; };
    auto rect = [&](float x, float y, float rw, float rh, uint32_t c, float al) {
        const float x0 = std::max(x, l), y0 = std::max(y, t), x1 = std::min(x + rw, r), y1 = std::min(y + rh, bt);
        if (x1 > x0 && y1 > y0) DrawSolidRectAbs(w, x0, y0, x1 - x0, y1 - y0, c, al);
    };
    for (const Trap& tr : g_traps) {
        if (!visible(tr)) continue;
        float sx, sy;
        toScreen(tr.x, tr.z, sx, sy);
        const uint8_t live = g_trapLive.size() == g_traps.size() ? g_trapLive[&tr - g_traps.data()] : 0;
        if (tr.kind == 5) { // a door, by its live state: locked red, closed amber, open green (not known: grey)
            const float s = std::max(10.0f, std::min(22.0f, 2.4f * ppm));
            if (!in(sx, sy, s)) continue;
            const uint32_t c = live == 1 ? 0xff3a3ae0u : live == 2 ? 0xff20b0f0u : live == 3 ? 0xff50c860u : 0xff909090u;
            // Shut (or not known): a bar across its doorway, wall to wall, the wall's colour with the door's own down
            // its middle; open: the gap stays clear
            const size_t di = static_cast<size_t>(&tr - g_traps.data());
            if (live != 3 && di < g_doorSpan.size() && g_doorSpan[di].axis) {
                const DoorSpan& d = g_doorSpan[di];
                float ax, ay, bx, by;
                toScreen(d.axis == 1 ? d.a : tr.x, d.axis == 1 ? tr.z : d.a, ax, ay);
                toScreen(d.axis == 1 ? d.b : tr.x, d.axis == 1 ? tr.z : d.b, bx, by);
                const float th = std::max(5.0f, 1.0f * ppm), in2 = th * 0.5f;
                const float x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
                if (d.axis == 1) {
                    rect(x0, y0 - th * 0.5f, x1 - x0, th, 0xff100f0du, 1.0f);
                    rect(x0, y0 - in2 * 0.5f, x1 - x0, in2, c, 0.95f);
                } else {
                    rect(x0 - th * 0.5f, y0, th, y1 - y0, 0xff100f0du, 1.0f);
                    rect(x0 - in2 * 0.5f, y0, in2, y1 - y0, c, 0.95f);
                }
            }
            DrawSolidRectAbs(w, sx - s * 0.5f, sy - s * 0.5f, s, s, 0xff0a0e10u, 0.85f);
            DrawSolidRectAbs(w, sx - s * 0.4f, sy - s * 0.4f, s * 0.8f, s * 0.8f, c, 0.95f);
            DrawSolidRectAbs(w, sx - s * 0.15f, sy - s * 0.3f, s * 0.3f, s * 0.6f, 0xff0a0e10u, live == 3 ? 0.2f : 0.75f);
            kinds |= 1u << (20 + live);
            continue;
        }
        if (tr.kind == 8) continue; // a one-way pad's destination: only data (DrawMapBeacons)
        if (tr.kind == 7) continue; // a barred window: cut out of the floor plan (WindowAt), a wall like the others
        if (tr.kind == 6) { // a magical barrier: purple
            const float s = std::max(8.0f, std::min(18.0f, 2.0f * ppm));
            if (in(sx, sy, s)) DrawSolidRectAbs(w, sx - s * 0.5f, sy - s * 0.5f, s, s, 0xffd060b0u, 0.7f);
            kinds |= 1u << 6;
            continue;
        }
        if (tr.kind >= 10) { // a switch: a plate's outline, a button's dot, a target's ring; faint once used
            const float s = std::max(10.0f, std::min(20.0f, 1.8f * ppm));
            if (!in(sx, sy, s)) continue;
            const uint32_t c = 0xff40e0f0u;
            const float a = live == 2 ? 0.4f : 0.95f;
            DrawAssetRectAbs(w, kCirclePrimitive, sx - s * 0.6f, sy - s * 0.6f, s * 1.2f, s * 1.2f, 0xff0a0e10u, 0.7f);
            if (tr.kind == 10) {
                const float b = std::max(1.5f, s * 0.12f);
                DrawSolidRectAbs(w, sx - s * 0.4f, sy - s * 0.4f, s * 0.8f, b, c, a);
                DrawSolidRectAbs(w, sx - s * 0.4f, sy + s * 0.4f - b, s * 0.8f, b, c, a);
                DrawSolidRectAbs(w, sx - s * 0.4f, sy - s * 0.4f, b, s * 0.8f, c, a);
                DrawSolidRectAbs(w, sx + s * 0.4f - b, sy - s * 0.4f, b, s * 0.8f, c, a);
            } else if (tr.kind == 11) {
                DrawAssetRectAbs(w, kCirclePrimitive, sx - s * 0.35f, sy - s * 0.35f, s * 0.7f, s * 0.7f, c, a);
            } else {
                DrawAssetRectAbs(w, kCircleOutlinePrimitive, sx - s * 0.45f, sy - s * 0.45f, s * 0.9f, s * 0.9f, c, a);
                DrawAssetRectAbs(w, kCirclePrimitive, sx - s * 0.12f, sy - s * 0.12f, s * 0.24f, s * 0.24f, c, a);
            }
            kinds |= 1u << tr.kind;
            continue;
        }
        if (tr.kind == 1) { // a spike plate: its 4 m square in red, only over its floor (not the wall it's against)
            const float h = 2.0f * ppm;
            if (!overlaps(sx, sy, h)) continue;
            const uint16_t m = g_trapMask.size() == g_traps.size() ? g_trapMask[&tr - g_traps.data()] : 0xffffu;
            if (!m) continue;
            const float fade = live == 2 ? 0.3f : 1.0f; // switched off: faint
            // An armed plate through its cycle, as the trap's own script runs it from the firing time (behaviour
            // resource Trap_GroundSpikes_Dungeon, 2026-11-09: each step's time in ns at its +0): 0 s the warning
            // pose, 1.0 s spikes out (0.1 s snap), 1.067 s the damage (SpawnImpact), 1.267 s back down (0.9 s
            // retract). The map: the warning brightening toward 1.0 s, full red with raised spikes while out,
            // fading as they retract, dim while down (no firing seen yet: steady)
            float pulse = 1.0f, spikeGrow = 1.0f;
            if (live != 2 && g_spikeTime.size() == g_traps.size()) {
                const SpikeTime& st = g_spikeTime[&tr - g_traps.data()];
                if (st.tick && st.periodMs) {
                    constexpr float kOutMs = 1000.0f, kDownMs = 1267.0f, kRetractMs = 900.0f;
                    const float since = static_cast<float>((GetTickCount64() - st.tick) % st.periodMs);
                    if (since < kOutMs) { // the warning
                        pulse = 0.7f + 0.5f * (since / kOutMs);
                    } else if (since < kDownMs) { // out: the damage
                        pulse = 1.8f;
                        spikeGrow = 1.9f;
                    } else if (since < kDownMs + kRetractMs) { // going back down
                        const float f = (since - kDownMs) / kRetractMs;
                        pulse = 1.8f - 1.1f * f;
                        spikeGrow = 1.9f - 0.9f * f;
                    } else {
                        pulse = 0.7f;
                    }
                }
            }
            const float d = std::max(1.5f, 0.35f * ppm * spikeGrow);
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4;) { // a row's floor cells merged into strips
                    if (!(m >> (r * 4 + c) & 1)) { ++c; continue; }
                    int e = c;
                    while (e < 4 && (m >> (r * 4 + e) & 1)) ++e;
                    rect(sx - h + c * ppm, sy - h + r * ppm, (e - c) * ppm, ppm, 0xff3a3ad8u,
                                     std::min(0.85f, 0.34f * fade * pulse));
                    c = e;
                }
                for (int c = 0; c < 4; ++c) // a spike dot in each floor cell
                    if (m >> (r * 4 + c) & 1)
                        rect(sx - h + (c + 0.5f) * ppm - d * 0.5f, sy - h + (r + 0.5f) * ppm - d * 0.5f, d, d,
                                         spikeGrow > 1.05f ? 0xff6060ffu : 0xff2020b0u, std::min(1.0f, 0.8f * fade * pulse));
            }
        } else if (tr.kind == 4) { // a piece of trap rail: a dark red block
            const float s = std::max(2.0f, 1.4f * ppm);
            if (!overlaps(sx, sy, s * 0.5f)) continue;
            rect(sx - s * 0.5f, sy - s * 0.5f, s, s, 0xff1c1c90u, 0.7f);
        } else { // a fireball or spark launcher (its live balls show its lane), or a steady electric beam (a line)
            const uint32_t c = tr.kind == 2 ? 0xff1a8cffu : 0xffffe08cu;
            const float len = std::hypot(tr.ex - tr.x, tr.ez - tr.z);
            const float dot = std::max(2.0f, std::min(6.0f, 0.6f * ppm));
            // a fire lane's dots looked like footprints: its live balls show the lane now (2026-11-09)
            // a spark-shooting electric trap too (its sparks show); a steady beam: a solid line (dots every metre)
            const bool steady = tr.kind == 3 && (tr.pad[0] & 2);
            for (float m = 0.0f; steady && m <= len; m += std::max(0.5f, 1.5f / std::max(0.1f, ppm))) {
                float px, py;
                toScreen(tr.x + (tr.ex - tr.x) * m / std::max(0.01f, len), tr.z + (tr.ez - tr.z) * m / std::max(0.01f, len), px, py);
                if (in(px, py, dot)) DrawSolidRectAbs(w, px - dot * 0.5f, py - dot * 0.5f, dot, dot, c, 0.75f);
            }
            const float rr = std::max(8.0f, std::min(16.0f, 1.6f * ppm));
            if (in(sx, sy, rr)) DrawAssetRectAbs(w, kCirclePrimitive, sx - rr * 0.5f, sy - rr * 0.5f, rr, rr, c, 0.95f);
        }
        kinds |= 1u << tr.kind;
    }
    // The fireballs rolling now: a glowing orange ball where each is this frame (moved on by its speed since it was read)
    const float since = std::min(0.25f, (GetTickCount64() - g_ballsTick) / 1000.0f);
    const float br = std::max(8.0f, std::min(30.0f, 2.8f * ppm));
    for (const Ball& b : g_balls) {
        Trap at{};
        at.kind = 2;
        at.x = b.x + b.vx * since;
        at.y = b.y;
        at.z = b.z + b.vz * since;
        if (!visible(at)) continue;
        float sx, sy;
        toScreen(at.x, at.z, sx, sy);
        if (!in(sx, sy, br)) continue;
        if (b.kind == 3) { // a spark: smaller, electric blue with a white core
            const float r = br * 0.7f;
            DrawAssetRectAbs(w, kCirclePrimitive, sx - r * 0.85f, sy - r * 0.85f, r * 1.7f, r * 1.7f, 0xffff9a3cu, 0.3f);
            DrawAssetRectAbs(w, kCirclePrimitive, sx - r * 0.5f, sy - r * 0.5f, r, r, 0xffffc070u, 0.95f);
            DrawAssetRectAbs(w, kCirclePrimitive, sx - r * 0.25f, sy - r * 0.25f, r * 0.5f, r * 0.5f, 0xfffff8f0u, 0.95f);
            kinds |= 1u << 3;
            continue;
        }
        DrawAssetRectAbs(w, kCirclePrimitive, sx - br * 0.75f, sy - br * 0.75f, br * 1.5f, br * 1.5f, 0xff0a60ffu, 0.3f); // glow
        DrawAssetRectAbs(w, kCirclePrimitive, sx - br * 0.5f, sy - br * 0.5f, br, br, 0xff1a8cffu, 0.95f);
        DrawAssetRectAbs(w, kCirclePrimitive, sx - br * 0.28f, sy - br * 0.34f, br * 0.5f, br * 0.5f, 0xff60e0ffu, 0.9f); // hot core
        kinds |= 1u << 2;
    }
    return kinds;
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
    // In a vault or spire: its traps round your own level (your floor, 4 m under you to 5 m over)
    {
        const int site = VaultAt(px, py, pz);
        if (site >= 0) {
            const int myF = FloorCount(site) > 1 ? FloorAt(site, py) : -1;
            DrawTraps(widgetRaw,
                      [&](float x, float z, float& sx, float& sy) { sx = vcx + (x - px) * pxPerMeterX; sy = vcy - (z - pz) * pxPerMeterY; },
                      pxPerMeterY,
                      [&](const Trap& tr) {
                          return NearDungeon(tr.x, tr.z, site) && TrapOnLevel(tr, [&](float y) {
                                     // 6 m under you (not 4): a spire's spark traps 4 m under its floor's walkway
                                     // flickered as your height crossed the edge (2026-11-09); still your floor only
                                     return y >= py - 6.0f && y <= py + 5.0f && (myF < 0 || FloorAt(site, y) == myF);
                                 });
                      },
                      vx, vy, vx + vw, vy + vh);
            // Its teleport pads on your floor, as on the dungeon map (a teal ring with the pair's letter; a one-way pad
            // an amber double ring), smaller; only where the whole ring fits in the minimap
            const Dungeon& dg = g_dungeons[site];
            const float ring = 20.0f;
            // your floor and your level (6 m under you to 5 m over, as the traps)
            auto onMine = [&](float y) { return (myF < 0 || FloorAt(site, y) == myF) && y >= py - 6.0f && y <= py + 5.0f; };
            auto place = [&](const Beacon& b, float& sx, float& sy) {
                sx = vcx + (b.x - px) * pxPerMeterX;
                sy = vcy - (b.z - pz) * pxPerMeterY;
                return sx - ring * 0.5f >= vx && sx + ring * 0.5f <= vx + vw && sy - ring * 0.5f >= vy && sy + ring * 0.5f <= vy + vh;
            };
            for (uint32_t i = 0; i < dg.Pads(); ++i) {
                const Beacon& b = g_beacons[dg.firstBeacon + dg.Beacons() + i];
                float sx, sy;
                if (!onMine(b.y) || !place(b, sx, sy)) continue;
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring, 0xff0a0e10u, 0.85f);
                DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring, 0xffd8c85au, 0.95f);
                const char letter[2] = {static_cast<char>('A' + (i / 2) % 26), 0};
                DrawLabelAbs(sx - ring * 0.5f, sy - ring * 0.5f + 1.0f, ring, ring, letter, 0xffd8c85au, 0.95f, TextAlignH::Center,
                             TextAlignV::Center, nullptr, 0.6f);
            }
            for (uint32_t i = 0; i < dg.Exits(); ++i) {
                const Beacon& b = g_beacons[dg.firstBeacon + dg.Beacons() + dg.Pads() + i];
                float sx, sy;
                if (!onMine(b.y) || !place(b, sx, sy)) continue;
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring, 0xff0a0e10u, 0.85f);
                DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring, 0xff3cc8f8u, 0.95f);
                DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, sx - ring * 0.27f, sy - ring * 0.27f, ring * 0.54f, ring * 0.54f,
                                 0xff3cc8f8u, 0.95f);
            }
        }
    }
    // Footprints: a foot each, a little left or right of the path, pointing the way walked; brighter the newer, faint
    // where they're on a floor stacked under or over the one drawn there
    const std::vector<float>& stepAlpha = StepAlphas();
    const float foot = std::max(4.5f, std::min(8.0f, 0.7f * pxPerMeterX));
    // In a vault or spire: only the steps on the floor you're on
    int myFloor = -1, floorCount = 0;
    const int vaultHere = VaultAt(px, py, pz);
    if (FloorCount(vaultHere) > 1) {
        myFloor = FloorAt(vaultHere, py);
        floorCount = FloorCount(vaultHere);
    }
    for (size_t i = 0; i < g_steps.size(); ++i) {
        const Step& s = g_steps[i];
        if (s.side > 1 || stepAlpha[i] <= 0.0f || (g_curDungeon >= 0 && !NearDungeon(s.x, s.z, g_curDungeon))) continue;
        if (floorCount && FloorAt(vaultHere, s.y) != myFloor) continue;
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
        // deep amber: stands out on pale stone, goo and rock; screen y runs south
        DrawFootprint(widgetRaw, sx, sy, len > 0.01f ? dx : 0.0f, len > 0.01f ? -dz : -1.0f, foot, s.side, 0xff1a6cf0u, alpha);
    }
}

// A footprint at (sx, sy) on screen, pointing (fx, fy) (a unit screen direction: the way walked), size px wide: a heel
// and the ball of the foot along it, toes in front, the big toe on the inner side (side 0 a right foot, 1 a left);
// small, just the heel and the ball ("more like foot steps that also show the direction traveled", 2026-11-09)
void DrawFootprint(void* w, float sx, float sy, float fx, float fy, float size, uint8_t side, uint32_t colour, float alpha) {
    const float qx = -fy, qy = fx;               // across the foot, to its right
    // the big toe's side, toward the path: side 0 is drawn right of the path (a right foot, big toe on its left),
    // side 1 left of it (seen mirrored the first time, 2026-11-09)
    const float inner = side ? 1.0f : -1.0f;
    const float len = size * 1.7f;               // heel to toe
    auto dot = [&](float along, float across, float d) {
        const float x = sx + fx * along * len + qx * across * size, y = sy + fy * along * len + qy * across * size;
        DrawAssetRectAbs(w, kCirclePrimitive, x - d * 0.5f, y - d * 0.5f, d, d, colour, alpha);
    };
    dot(-0.32f, 0.0f, size * 0.62f);             // heel
    dot(0.12f, -0.05f * inner, size * 0.78f);    // ball of the foot
    if (size < 5.0f) return;
    dot(-0.1f, -0.12f * inner, size * 0.5f);     // the arch's outer edge (the foot's waist)
    dot(0.42f, 0.22f * inner, size * 0.3f);      // big toe
    dot(0.40f, -0.02f * inner, size * 0.22f);
    dot(0.33f, -0.24f * inner, size * 0.2f);     // little toes
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
    UpdateTrapLive(px, py, pz); // doors, switches, spikes (once a second)
    UpdateFireballs(px, py, pz); // the rolling fireballs (~15 times a second)
    if (g_stepsDirty && GetTickCount64() - g_stepsSavedTick > 30000ULL) SaveSteps();
    return true;
}

// ---- the World Map inside a hall: this dungeon's whole floor plan on a dark ground (the corner of the world it is
// carved in shows nothing useful), its Return Beacons numbered in the order you reach them, the other dungeons'
// markers hidden; a button switches to the overworld, centred on the hall's entrance, and back
int g_mapDungeon = -1; // the dungeon the open map is about (-1: not in one)
bool g_mapOverworld = false;
float g_mapPx = 0.0f, g_mapPy = 0.0f, g_mapPz = 0.0f; // where you stood when the map opened
// The floor the map shows (a vault's or spire's; -1: none, a Hollow Hall) and how far up or down from g_mapPy is still
// on it: the floor's own span, or 8 m
int g_mapFloor = -1;
float g_mapBand = 8.0f;
// The heights on the map's level: g_mapPy +- 8 m, or a floor's own span (4 m under its lowest pad, 5 m over its
// highest; never into the floors round it). A level under it showing through a gap in it (a
// spire's 956 m room under its 965 m floor) is then dimmed, not drawn as part of it.
float g_mapLo = 0.0f, g_mapHi = 0.0f;
bool OnMapLevel(float y) { return y >= g_mapLo && y <= g_mapHi; }
int FloorCount(int d);
int FloorAt(int d, float y);
float FloorTop(int d, int i);
void SelectFloor(int i); // below
const Beacon& FloorRec(int d, int i);

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
    // A vault or spire opens on the floor you're on
    g_mapFloor = -1;
    g_mapBand = 8.0f;
    g_mapLo = g_mapPy - 8.0f;
    g_mapHi = g_mapPy + 8.0f;
    if (FloorCount(g_mapDungeon) > 1) {
        // Your own floor round where you stand, not its pads: a floor holds levels a wall's height apart (a spire's
        // 973 m hallway over its 965 m floor, no teleporter between): from below the hallway is a wall, from on it the
        // floor below is
        const float ground = g_mapPy;
        SelectFloor(FloorAt(g_mapDungeon, ground));
        const int i = g_mapFloor, n = FloorCount(g_mapDungeon);
        const Beacon& f = FloorRec(g_mapDungeon, i);
        const float floorLo = i > 0 ? FloorTop(g_mapDungeon, i - 1) : -1e9f;
        const float floorHi = i + 1 < n ? FloorTop(g_mapDungeon, i) : 1e9f;
        g_mapPy = ground;
        g_mapLo = std::max(floorLo, ground - 4.0f);
        g_mapHi = std::min(floorHi, ground + 5.0f);
        g_mapBand = (g_mapHi - g_mapLo) * 0.5f + 1000.0f; // the grid's cache key (apart from a pick by the buttons)
    }
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

// ---- floors (a vault's or spire's): floor i is {lowest, highest} m; it reaches halfway to the floors round it
int FloorCount(int d) { return d >= 0 && d < static_cast<int>(g_dungeonCount) ? static_cast<int>(g_dungeons[d].Floors()) : 0; }
const Beacon& FloorRec(int d, int i) {
    const Dungeon& g = g_dungeons[d];
    return g_beacons[g.firstBeacon + g.Beacons() + g.Pads() + g.Exits() + static_cast<uint32_t>(i)];
}
// The floor a height is on (0 the bottom); -1 without floors
// Where floor i ends and floor i + 1 begins: just under the next floor's lowest pad (1.5 m: the pad stands on its
// floor). A floor's pads are its bottom and its levels rise from them (the user, 2026-11-09: a spire's floor 4 has
// pads at 834 and its third level at 850, under floor 5's pad at 862; halfway between the pads called it floor 5).
// Walking links can't settle it: placed stairs aren't in the scan.
float FloorTop(int d, int i) {
    return std::max(FloorRec(d, i).y + 1.0f, FloorRec(d, i + 1).x - 1.5f);
}
int FloorAt(int d, float y) {
    const int n = FloorCount(d);
    for (int i = 0; i < n - 1; ++i) {
        if (y < FloorTop(d, i)) return i;
    }
    return n - 1;
}
// Shows floor i on the map: its middle as your level, its span (and 6 m) as the band still on it
void SelectFloor(int i) {
    const int n = FloorCount(g_mapDungeon);
    if (n <= 0) return;
    i = std::max(0, std::min(n - 1, i));
    const Beacon& f = FloorRec(g_mapDungeon, i);
    g_mapFloor = i;
    g_mapPy = (f.x + f.y) * 0.5f;
    g_mapLo = f.x - 4.0f;
    g_mapHi = f.y + 5.0f; // not 10: a 2 m thick wall's top 9 m up (a spire's) read as floor
    if (i > 0) g_mapLo = std::max(g_mapLo, FloorTop(g_mapDungeon, i - 1));
    if (i + 1 < n) g_mapHi = std::min(g_mapHi, FloorTop(g_mapDungeon, i));
    g_mapBand = (g_mapHi - g_mapLo) * 0.5f; // the grid's cache key
}
// The map's floor one up (+1) or down (-1); true when it changed
bool StepFloor(int dir) {
    if (!MapActive() || g_mapFloor < 0) return false;
    const int before = g_mapFloor;
    SelectFloor(g_mapFloor + dir);
    return g_mapFloor != before;
}
// The minimap's: your floor and the count in a vault or spire (false elsewhere or with one floor)
bool MinimapFloor(float px, float py, float pz, int& floor, int& count) {
    const int d = g_dungeonCount ? VaultAt(px, py, pz) : -1;
    count = FloorCount(d);
    if (count < 2) return false;
    floor = FloorAt(d, py);
    return true;
}
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
    if (IsVault(g_mapDungeon)) return !NearDungeon(x, z, g_mapDungeon); // a vault's map: only its own box
    return !corner || !NearDungeon(x, z, g_mapDungeon);
}
// A vault's map: what lies far above or below its floors (the island's ground under it, a Night Sanctum beneath) is
// not in it
bool VaultMapHidesHeight(float y) {
    return MapActive() && IsVault(g_mapDungeon) && (y < g_minY[g_mapDungeon] - 20.0f || y > g_maxY[g_mapDungeon] + 25.0f);
}

// On one of dungeon d's own floors: one within 4 m around and 6 m up or down (a chest stands on its floor)
bool OnDungeonFloor(float x, float y, float z, int d) {
    if (!Load()) return false;
    const int xc = static_cast<int>(std::floor(x)), zc = static_cast<int>(std::floor(z));
    for (int zz = zc - 4; zz <= zc + 4; ++zz) {
        if (zz < static_cast<int>(g_z0) || zz >= static_cast<int>(g_z0 + g_rows)) continue;
        const uint32_t r = static_cast<uint32_t>(zz) - g_z0;
        for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
            const Run& run = g_runs[i];
            if (g_runDungeon[i] != d || xc + 4 < run.x0 || xc - 4 >= run.x0 + run.length) continue;
            if (std::fabs(run.Y() - y) <= 6.0f) return true;
        }
    }
    return false;
}

// A chest by its hall (chest_halls.h), not by where it is on the map: the halls are stacked over each other
constexpr float kNoHallChestHeight = 30.0f; // a chest of no hall (1, far from them all) shows only this near your height

bool MapHidesChest(size_t chest) {
    const ChestSpawn& sp = kChestSpawns[chest];
    const float x = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f;
    const float z = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f;
    if (!MapActive()) return InCorner(x, z);
    const uint8_t hall = kChestHall[chest];
    if (IsVault(g_mapDungeon)) { // a vault's or spire's: the island's chests standing on its own floors (no hall's)
        const float y = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
        return hall != kChestNotInHalls || !OnDungeonFloor(x, y, z, g_mapDungeon) ||
               (g_mapFloor >= 0 && FloorAt(g_mapDungeon, y) != g_mapFloor); // only the floor shown's
    }
    if (hall == kChestNotInHalls) return true;
    if (hall != kChestNoHall) return hall != g_mapDungeon;
    const float y = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
    return !NearDungeon(x, z, g_mapDungeon) || std::fabs(y - g_mapPy) > kNoHallChestHeight;
}

// On the minimap: only the hall you're in has its chests there (py: your height)
bool MinimapHidesChest(size_t chest, float px, float py, float pz) {
    const uint8_t hall = kChestHall[chest];
    // In a vault or spire (its plan on the minimap): only the chests on its own floors, not the ground's round it
    const int vault = Load() ? VaultAt(px, py, pz) : -1;
    if (vault >= 0 && hall == kChestNotInHalls) {
        const ChestSpawn& sp = kChestSpawns[chest];
        const float cy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
        if (FloorCount(vault) > 1 && FloorAt(vault, cy) != FloorAt(vault, py)) return true; // your floor's only
        return !OnDungeonFloor(static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f, cy,
                               static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f, vault);
    }
    if (hall == kChestNotInHalls) return false;
    if (!InCorner(px, pz)) return true;
    if (hall == kChestNoHall) {
        const ChestSpawn& sp = kChestSpawns[chest];
        const float y = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f;
        return std::fabs(y - py) > kNoHallChestHeight;
    }
    return g_curDungeon >= 0 && hall != g_curDungeon;
}
// The minimap in a vault or spire with floors: a marker at height y on another floor than yours (lore notes,
// bosses, deposits: as the chests and pads)
bool MinimapOtherFloor(float x, float y, float z, float px, float py, float pz) {
    const int site = Load() ? VaultAt(px, py, pz) : -1;
    if (site < 0 || FloorCount(site) <= 1 || !NearDungeon(x, z, site)) return false;
    return FloorAt(site, y) != FloorAt(site, py);
}
// wide: a lore note, boss or unique item, which can lie a room's width from any floor (Tome of Agony, 11 m from the
// Kindle Waste hall's floor, over another hall's): the floor nearest it is looked for further out. Only these few: it
// is too slow for every ore in the rock.
bool MapHidesAt(float x, float y, float z, bool wide = false) {
    if (MapHides(x, z)) return true;
    if (!MapActive()) return false;
    // a vault's or spire's: only what's on the floor shown (a lore note on floor 1 showed on all five, 2026-11-09)
    if (IsVault(g_mapDungeon))
        return VaultMapHidesHeight(y) || (g_mapFloor >= 0 && FloorAt(g_mapDungeon, y) != g_mapFloor);
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
    std::vector<uint8_t> dim;          // 1: another level; 2: far below (a drop: an open balcony over a spire's base)
    int At(int x, int z) const {
        const int c = x - x0, r = z1 - 1 - z;
        return c < 0 || r < 0 || c >= w || r >= h ? -1 : r * w + c;
    }
};
std::atomic<uint32_t> g_levelGen{0};
constexpr float kLevelBandM = 8.0f; // up or down: still your level
constexpr float kRockReachM = 20.0f; // bare rock on the dungeon map: this near where you stand
// A floor this far under your level is a drop, not a room beside you: drawn far darker (dim 2). Off a spire's open
// balcony the next floor down is its base plaza, 100 m below; at the ordinary dim it read as a hall.
constexpr float kDropM = 30.0f;

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
            // Only slopes on this cell's own level (steps, ramps): a neighbour on another level or more than 3 m up or
            // down (a wall's column holds some other floor) counts as level ground. It shaded both sides of every wall
            // and pillar row hard dark / bright, soft bars that read as walls across open floor (2026-11-09).
            auto hAt = [&](int rr, int cc) {
                if (rr < 0 || cc < 0 || rr >= g.h || cc >= g.w) return static_cast<int>(g.y[i]);
                const size_t n = static_cast<size_t>(rr) * g.w + cc;
                const int v = g.y[n];
                return v < 0 || g.dim[n] != g.dim[i] || std::abs(v - g.y[i]) > 3 ? static_cast<int>(g.y[i]) : v;
            };
            const float gx = (hAt(r, c + 1) - hAt(r, c - 1)) * 0.5f, gz = (hAt(r + 1, c) - hAt(r - 1, c)) * 0.5f;
            const float light = std::max(0.55f, std::min(1.3f, 1.0f + 0.12f * (-gx - gz)));
            g.shade[i] = static_cast<uint8_t>(std::min(255L, std::lround(light * 128.0f)));
        }
}

// Per 1 m column: the floor nearest your level. Off your level (dimmed), a roof gives way to the floor under it: built
// blocks with a floor 4 m or more below and none over them (the walking test climbs a building's walls, so its roof
// counts as a floor; from far above it would hide the room under it)
// Per 1 m column of the box (x0 .. x0 + w, z1 - h .. z1) of dungeon d: its floor in the band lo .. hi nearest py, else
// the nearest other one, dimmed; bare rock only within kRockReachM of (rx, rz) and in the band. The dungeon map and,
// in a vault or spire, the minimap (2026-11-09: built from the walk's few layers per column it was coarser) share it.
std::shared_ptr<LevelGrid> BuildBandGrid(int kind, int x0, int z1, int w, int h, int d, float py, float lo, float hi,
                                         float rx, float rz) {
    LoadTraps();
    auto g = NewLevelGrid(kind, x0, z1, w, h);
    auto onLevel = [&](float y) { return y >= lo && y <= hi; };
    struct Cand {
        int16_t y;
        uint8_t mat;
        bool rock;
    };
    // Every floor of a column, however many: the first 8 of a row's runs were kept, and a spire's columns hold up to
    // 12 (its floors, their vaults' tops, the roofs). Where a row's runs break at pillars your own floor's came 9th or
    // later and was dropped, so the column fell back to another level's floor, dimmed: dark bars across an open
    // corridor at every pillar row (2026-11-09). Per row: counted, then filled (start[cx] .. start[cx + 1]).
    std::vector<Cand> cand;
    std::vector<uint32_t> start(static_cast<size_t>(g->w) + 1), fillAt(g->w);
    for (int z = z1 - h; z < z1; ++z) {
        if (z < static_cast<int>(g_z0) || z >= static_cast<int>(g_z0 + g_rows)) continue;
        const uint32_t r = static_cast<uint32_t>(z) - g_z0;
        auto eachFloor = [&](auto&& fn) {
            for (uint32_t i = g_rowStart[r]; i < g_rowStart[r + 1]; ++i) {
                const Run& run = g_runs[i];
                if (run.x0 >= x0 + w || run.x0 + run.length <= x0) continue;
                // The dungeon's floors, and the rock set apart from them (wall tops, ledges) only right around you at
                // your level: you're up on it (elsewhere it's slivers of wall)
                const bool rock = g_runDungeon[i] >= g_dungeonCount;
                if (g_runDungeon[i] != d && !(rock && onLevel(static_cast<float>(run.Y())))) continue;
                for (int x = std::max(static_cast<int>(run.x0), x0); x < std::min(run.x0 + run.length, x0 + w); ++x) {
                    if (rock && (x - rx) * (x - rx) + (z - rz) * (z - rz) > kRockReachM * kRockReachM) continue;
                    fn(x - x0, Cand{static_cast<int16_t>(run.Y()), g_runMaterial[i], rock});
                }
            }
        };
        std::fill(start.begin(), start.end(), 0u);
        eachFloor([&](int cx, const Cand&) { ++start[static_cast<size_t>(cx) + 1]; });
        for (int cx = 0; cx < g->w; ++cx) start[static_cast<size_t>(cx) + 1] += start[cx];
        cand.resize(start[g->w]);
        std::copy(start.begin(), start.end() - 1, fillAt.begin());
        eachFloor([&](int cx, const Cand& c) { cand[fillAt[cx]++] = c; });
        for (int cx = 0; cx < g->w; ++cx) {
            const Cand* cs = cand.data() + start[cx];
            const int n = static_cast<int>(start[static_cast<size_t>(cx) + 1] - start[cx]);
            int best = -1;
            float bestD = 1e9f;
            for (int k = 0; k < n; ++k) {
                const float dist = std::fabs(cs[k].y - py);
                if (!onLevel(cs[k].y) && cs[k].mat >= 128 && !cs[k].rock) {
                    bool below = false, over = false;
                    for (int m = 0; m < n; ++m) {
                        below |= !cs[m].rock && cs[m].y <= cs[k].y - 4;
                        over |= !cs[m].rock && cs[m].y >= cs[k].y + 4 && cs[m].y <= cs[k].y + 30;
                    }
                    if (below && !over) continue; // a roof
                }
                const float rank = dist + (onLevel(cs[k].y) ? 0.0f : 1000.0f); // the level's own first
                if (rank < bestD) {
                    bestD = rank;
                    best = k;
                }
            }
            if (best < 0) continue;
            if (onLevel(cs[best].y) && WindowAt(x0 + cx, z, cs[best].y)) continue; // a barred window: wall
            const int c = g->At(x0 + cx, z);
            if (c < 0) continue;
            g->y[c] = cs[best].y;
            g->mat[c] = cs[best].mat;
            g->dim[c] = onLevel(cs[best].y) ? 0 : cs[best].y < lo - kDropM ? 2 : 1;
        }
    }
    // A 1 m bit of "wall" with floor of this level on three or four sides is a pillar's base or a post poking into a
    // hall, not a wall: floor (a spire's corridor read as a maze of stubs, 2026-11-09). One pass, so a long wall only
    // loses its last metre at a doorway; a barred window stays.
    {
        auto on = [&](int x, int z) {
            const int c = g->At(x, z);
            return c >= 0 && g->y[c] >= 0 && g->dim[c] == 0 ? c : -1;
        };
        std::vector<std::pair<int, int>> fill; // cell, the neighbour it takes its floor from
        for (int z = z1 - h; z < z1; ++z)
            for (int x = x0; x < x0 + w; ++x) {
                const int c = g->At(x, z);
                if (c < 0 || (g->y[c] >= 0 && g->dim[c] == 0)) continue;
                const int n[4] = {on(x - 1, z), on(x + 1, z), on(x, z - 1), on(x, z + 1)};
                int count = 0, from = -1;
                for (int k = 0; k < 4; ++k)
                    if (n[k] >= 0) ++count, from = n[k];
                if (count >= 3 && !WindowAt(x, z, g->y[from])) fill.push_back({c, from});
            }
        for (auto [c, from] : fill) {
            g->y[c] = g->y[from];
            g->mat[c] = g->mat[from];
            g->dim[c] = 0;
        }
    }
    ShadeLevelGrid(*g);
    return g;
}
std::shared_ptr<const LevelGrid> BuildMapGrid(int d, float py) {
    const Dungeon& dg = g_dungeons[d];
    return BuildBandGrid(0, static_cast<int>(dg.minX), static_cast<int>(dg.maxZ), static_cast<int>(dg.maxX - dg.minX),
                         static_cast<int>(dg.maxZ - dg.minZ), d, py, g_mapLo, g_mapHi, g_mapPx, g_mapPz);
}

// The minimap's, from the walking flood (g_grid): the floor reached first, else (not reachable) the nearest, dimmed.
// In a vault or spire with floors: as the dungeon map (BuildMapGrid), so the two agree (2026-11-09: the minimap went by
// what the walk reached and drew a hall behind barred windows a level apart, and wall tops as floor): per column the
// dungeon's floor in the band round your feet nearest your height (4 m under, 5 m over, never past halfway to the floors
// round it), else the nearest other one dimmed (a roof giving way to the floor under it); bare rock only near you.
std::shared_ptr<const LevelGrid> BuildMiniGrid(float py) {
    LoadTraps();
    auto g = NewLevelGrid(1, g_gx0, g_gz0 + g_gSize, g_gSize, g_gSize);
    const float cxw = static_cast<float>(g_gx0 + g_gSize / 2), czw = static_cast<float>(g_gz0 + g_gSize / 2);
    const int site = VaultAt(cxw, py, czw);
    const int myFloor = FloorCount(site) > 1 ? FloorAt(site, py) : -1;
    float lo = -1e9f, hi = 1e9f;
    if (myFloor >= 0) {
        const int n = FloorCount(site);
        const Beacon& f = FloorRec(site, myFloor);
        lo = py - 4.0f;
        hi = py + 5.0f;
        if (myFloor > 0) lo = std::max(lo, FloorTop(site, myFloor - 1));
        if (myFloor + 1 < n) hi = std::min(hi, FloorTop(site, myFloor));
    }
    auto inBand = [&](int y) { return y >= lo && y <= hi; };
    if (myFloor >= 0) return BuildBandGrid(1, g_gx0, g_gz0 + g_gSize, g_gSize, g_gSize, site, py, lo, hi, cxw, czw);
    for (int gz = 0; gz < g_gSize; ++gz)
        for (int gx = 0; gx < g_gSize; ++gx) {
            const Cell& cell = g_grid[static_cast<size_t>(gz) * g_gSize + gx];
            if (!cell.n) continue;
            const int wx = g_gx0 + gx, wz = g_gz0 + gz;
            int l = -1;
            bool dimmed = true;
            if (myFloor >= 0) {
                const bool nearYou = (wx - cxw) * (wx - cxw) + (wz - czw) * (wz - czw) <= kRockReachM * kRockReachM;
                float bestD = 1e9f;
                for (int k = 0; k < cell.n; ++k) {
                    const uint32_t ri = cell.run[k];
                    const int y = g_runs[ri].Y();
                    const bool rock = g_runDungeon[ri] >= g_dungeonCount;
                    if (rock && !(nearYou && inBand(y))) continue;
                    if (!inBand(y) && g_runMaterial[ri] >= 128 && !rock) { // a roof off your level: the floor under it
                        bool below = false, over = false;
                        for (int m = 0; m < cell.n; ++m) {
                            const uint32_t rm = cell.run[m];
                            if (g_runDungeon[rm] >= g_dungeonCount) continue;
                            below |= g_runs[rm].Y() <= y - 4;
                            over |= g_runs[rm].Y() >= y + 4 && g_runs[rm].Y() <= y + 30;
                        }
                        if (below && !over) continue;
                    }
                    const float rank = std::fabs(y - py) + (inBand(y) ? 0.0f : 1000.0f);
                    if (rank < bestD) {
                        bestD = rank;
                        l = k;
                    }
                }
                dimmed = bestD >= 1000.0f;
            } else {
                const bool reached = cell.first >= 0;
                l = cell.first;
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
                dimmed = !reached;
            }
            if (l < 0) continue;
            const int y = g_runs[cell.run[l]].Y();
            if (!dimmed && WindowAt(wx, wz, y)) continue; // a barred window: wall
            const int c = g->At(wx, wz);
            if (c < 0) continue;
            g->y[c] = static_cast<int16_t>(y);
            g->mat[c] = g_runMaterial[cell.run[l]];
            g->dim[c] = !dimmed ? 0 : g->y[c] < py - kDropM ? 2 : 1;
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
            const int cx = static_cast<int>(std::floor(wx));
            const int c = g.At(cx, cz);
            auto floorAt = [&](int x, int z) {
                const int cc = g.At(x, z);
                return cc >= 0 && g.y[cc] >= 0;
            };
            if (c < 0 || g.y[c] < 0) {
                // A wall cell beside floor: solid wall colour, not left clear (a 1 m wall's clear pixels blurred into
                // the floor round it when the image was scaled: thin walls read as faint lines, 2026-11-09)
                if (floorAt(cx - 1, cz) || floorAt(cx + 1, cz) || floorAt(cx, cz - 1) || floorAt(cx, cz + 1)) {
                    out[static_cast<size_t>(j) * kTilePx + i] = 0xff0d0f10u;
                    any = true;
                }
                continue;
            }
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
            if (g.dim[c]) light *= g.dim[c] == 2 ? 0.22f : 0.6f;
            // A sharp edge where the floor meets a wall: its outer 0.15 m (at least a pixel) darker
            {
                const float edge = std::max(1.0f / ppm, 0.15f);
                const float fxIn = wx - cx, fzIn = wz - cz;
                if ((fxIn < edge && !floorAt(cx - 1, cz)) || (fxIn > 1.0f - edge && !floorAt(cx + 1, cz)) ||
                    (fzIn < edge && !floorAt(cx, cz - 1)) || (fzIn > 1.0f - edge && !floorAt(cx, cz + 1)))
                    light *= 0.55f;
            }
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
        float fu0 = u0 + (u1 - u0) * (cx0 - sx0) / (sx1 - sx0), fu1 = u0 + (u1 - u0) * (cx1 - sx0) / (sx1 - sx0);
        float fv0 = v0 + (v1 - v0) * (cy0 - sy0) / (sy1 - sy0), fv1 = v0 + (v1 - v0) * (cy1 - sy0) / (sy1 - sy0);
        // No seams between tiles: sampled half a texel inside the image's edge (filtering blended the edge pixels
        // with what's past it) and drawn half a pixel over its neighbours where it isn't cut by the view (thin dark
        // lines in a grid across the floor looked like walls, 2026-11-09)
        constexpr float kHalfTexel = 0.5f / kTilePx;
        fu0 = std::max(fu0, kHalfTexel);
        fv0 = std::max(fv0, kHalfTexel);
        fu1 = std::min(fu1, 1.0f - kHalfTexel);
        fv1 = std::min(fv1, 1.0f - kHalfTexel);
        const float ex0 = cx0 > left ? cx0 - 0.5f : cx0, ex1 = cx1 < right ? cx1 + 0.5f : cx1;
        const float ey0 = cy0 > top ? cy0 - 0.5f : cy0, ey1 = cy1 < bottom ? cy1 + 0.5f : cy1;
        float rect[4] = {ex0, ey0, ex1 - ex0, ey1 - ey0};
        if (g_drawImageSubUv) g_drawImageSubUv(widgetRaw, rect, id, PackXY(fu0, fv0), PackXY(fu1, fv1), colour);
        else g_drawImage(widgetRaw, rect, &id, &colour);
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
    static float s_py = 0.0f, s_px = 0.0f, s_pz = 0.0f, s_band = 0.0f;
    if (!g_liveGrid[0] || s_d != d || s_py != g_mapPy || s_px != g_mapPx || s_pz != g_mapPz || s_band != g_mapBand) {
        auto grid = BuildMapGrid(d, g_mapPy);
        s_d = d;
        s_py = g_mapPy;
        s_band = g_mapBand;
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
    // (again when you go 1.5 m up or down: its band follows your feet)
    static float s_miniPy = -1e9f;
    if (!g_liveGrid[1] || g_miniFloodSeen != g_floodCount || std::fabs(py - s_miniPy) > 1.5f) {
        auto grid = BuildMiniGrid(py);
        g_miniFloodSeen = g_floodCount;
        s_miniPy = py;
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
    // on your level: OnMapLevel (the floor shown)
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
                kind = OnMapLevel(static_cast<float>(bestY[c])) ? 0 : kHdVeil;
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
            onLevel = OnMapLevel(h);
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
    // A vault's or spire's traps on the level shown
    if (IsVault(g_mapDungeon)) {
        UpdateTrapLive(g_mapPx, g_mapPy, g_mapPz);
        UpdateFireballs(g_mapPx, g_mapPy, g_mapPz);
        g_mapTrapKinds = DrawTraps(widgetRaw,
                  [&](float x, float z, float& sx, float& sy) { sx = originX + x * zoom; sy = originY + (mapWorldH - z) * zoom; },
                  zoom, [&](const Trap& tr) { return NearDungeon(tr.x, tr.z, g_mapDungeon) && TrapOnLevel(tr, OnMapLevel); },
                  left, top, right, bottom);
    }
    // Footprints in this dungeon: at most 600 (the draw budget), the newest
    const std::vector<float>& stepAlpha = StepAlphas();
    const float foot = std::max(3.0f, std::min(7.0f, 0.7f * zoom));
    size_t shown = 0;
    // (each foot up to 6 dots: 400 of them stay within the draw budget the 600 single dots had)
    for (size_t i = g_steps.size(); i-- > 0 && shown < 400;) {
        const Step& s = g_steps[i];
        if (s.side > 1 || stepAlpha[i] <= 0.0f || (!(s.x < kCornerMaxX && s.z > kCornerMinZ) && VaultAt(s.x, s.y, s.z) < 0) ||
            MapHides(s.x, s.z) || VaultMapHidesHeight(s.y))
            continue;
        // A vault's or spire's: only the steps on the floor shown
        if (g_mapFloor >= 0 && FloorAt(g_mapDungeon, s.y) != g_mapFloor) continue;
        // the way walked: from the step before to the one after (not across a teleport)
        const Step& a = i > 0 && g_steps[i - 1].side <= 1 ? g_steps[i - 1] : s;
        const Step& b = i + 1 < g_steps.size() && g_steps[i + 1].side <= 1 ? g_steps[i + 1] : s;
        float dx = b.x - a.x, dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 0.01f) {
            dx /= len;
            dz /= len;
        } else {
            dx = 0.0f;
            dz = 1.0f;
        }
        const float off = s.side ? 0.3f : -0.3f; // a little left or right of the path
        const float wx = s.x - dz * off, wz = s.z + dx * off;
        const float sx = originX + wx * zoom, sy = originY + (mapWorldH - wz) * zoom;
        if (sx < left || sx > right || sy < top || sy > bottom) continue;
        DrawFootprint(widgetRaw, sx, sy, dx, -dz, foot, s.side, 0xff1a6cf0u, stepAlpha[i]);
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
        if(!dungeon.Beacons())return;
        centre=g_beacons[dungeon.firstBeacon+(phase/g_dungeonCount)%dungeon.Beacons()];
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
// The way your character faces, from its own entity's CurrentTransform (a player entity, id under 0x400, standing where
// you are): forward = +z turned by its rotation quaternion {x, y, z, w} at +0x18. The entity's slot is remembered;
// looked for again at most twice a second.
bool PlayerFacingInner(uint8_t* reg, int posType, float px, float pz, float& fx, float& fz) {
    static uint64_t s_slot = ~0ull, s_lastSearch = 0;
    static void* s_reg = nullptr;
    uint8_t* map = reg + 0x148;
    const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
    auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
    auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
    auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
    if (!used || !ids || !records || capacity > (1u << 22)) return false;
    auto read = [&](uint64_t i) -> bool {
        if (i >= capacity || !(used[i >> 6] >> (i & 63) & 1) || ids[i] == 0 || ids[i] >= 0x400 || !records[i]) return false;
        uint16_t sz = 0;
        const uint8_t* ct = EcsComponent(records[i], static_cast<uint16_t>(posType), &sz);
        if (!ct || sz != 0x38) return false;
        const double ex = reinterpret_cast<const int64_t*>(ct)[0] / 4294967296.0 - px;
        const double ez = reinterpret_cast<const int64_t*>(ct)[2] / 4294967296.0 - pz;
        if (ex * ex + ez * ez > 1.5 * 1.5) return false;
        const float* q = reinterpret_cast<const float*>(ct + 0x18);
        const float x = 2.0f * (q[0] * q[2] + q[3] * q[1]), z = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
        const float len = std::sqrt(x * x + z * z);
        if (!std::isfinite(len) || len < 0.2f) return false;
        fx = x / len;
        fz = z / len;
        return true;
    };
    if (s_reg == reg && read(s_slot)) return true;
    const uint64_t now = GetTickCount64();
    if (now - s_lastSearch < 500ULL) return false;
    s_lastSearch = now;
    s_reg = reg;
    for (uint64_t i = 0; i < capacity; ++i)
        if (used[i >> 6] >> (i & 63) & 1 && ids[i] < 0x400 && read(i)) {
            s_slot = i;
            return true;
        }
    return false;
}
bool PlayerFacing(float px, float pz, float& fx, float& fz) {
    auto* reg = static_cast<uint8_t*>(g_waterRegistry.load(std::memory_order_relaxed));
    for (int r = 0; !reg && r < kMaxWaterRegistries; ++r) reg = static_cast<uint8_t*>(g_waterRegistries[r].load(std::memory_order_relaxed));
    if (!reg) return false;
    static void* s_typesReg = nullptr;
    static int s_pos = -1;
    __try {
        if (s_typesReg != reg) {
            s_typesReg = reg;
            s_pos = EcsTypeByName(reg, "CurrentTransform");
        }
        return s_pos >= 0 && PlayerFacingInner(reg, s_pos, px, pz, fx, fz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whether you stand on the floor the dungeon map shows (true when it shows no single floor or your height isn't known):
// off it, your arrow is a dotted outline
bool PlayerOnShownFloor(uint8_t* client) {
    float y = 0.0f;
    if (g_mapFloor < 0 || g_mapDungeon < 0 || !client || !LocalPlayerY(client, y)) return true;
    return FloorAt(g_mapDungeon, y) == g_mapFloor;
}

bool DrawMapBeacons(void* widgetRaw, const float* rootRect, uint8_t* mapState, bool haveCursor, float mx, float my,
                    bool clicked) {
    if (!MapActive() || !widgetRaw) return false;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH)) return false;
    const float top = rootRect[1] + 114.0f;
    const Dungeon& d = g_dungeons[g_mapDungeon];
    HhpLink* hhp = HhpBlock();
    const bool travel = hhp && hhp->known == 1 && !IsVault(g_mapDungeon); // Hollow Halls Progression knows the halls only
    constexpr float kBadge = 26.0f;
    bool anyHovered = false;
    int shownPads = 0, shownExits = 0, shownBeacons = 0; // on the floor shown, for the legend
    // A pad that takes you to another floor, while hovered: a chip to the ring's right, a smooth arrow up or down and
    // the floor it takes you to (only on hover, no text title: 2026-11-09)
    auto floorArrow = [&](float px, float py, float y, float ty, float level) {
        const int fp = FloorCount(g_mapDungeon) > 1 ? FloorAt(g_mapDungeon, y) : -1;
        const int fq = FloorCount(g_mapDungeon) > 1 ? FloorAt(g_mapDungeon, ty) : -1;
        if (!(fp >= 0 && fq >= 0 ? fp != fq : std::fabs(ty - y) > 6.0f)) return;
        const bool up = ty > y;
        const uint32_t ink = 0xffd8c85au;
        const float h = 22.0f, w = fq >= 0 ? 40.0f : 24.0f, x0 = px + 18.0f, y0 = py - h * 0.5f;
        // the chip: a dark pill with a thin teal edge
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, x0 - 1.0f, y0 - 1.0f, h + 2.0f, h + 2.0f, ink, 0.55f * level);
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, x0 + w - h - 1.0f, y0 - 1.0f, h + 2.0f, h + 2.0f, ink, 0.55f * level);
        DrawSolidRectAbs(widgetRaw, x0 + h * 0.5f, y0 - 1.0f, w - h, h + 2.0f, ink, 0.55f * level);
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, x0, y0, h, h, 0xff0a0e10u, 0.92f * level);
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, x0 + w - h, y0, h, h, 0xff0a0e10u, 0.92f * level);
        DrawSolidRectAbs(widgetRaw, x0 + h * 0.5f, y0, w - h, h, 0xff0a0e10u, 0.92f * level);
        // the arrow: a filled triangle (1 px rows) over a stem, 12 px tall
        const float cx = x0 + 12.0f, top = y0 + 4.0f, triH = 7.0f, stemH = 5.0f;
        for (int k = 0; k < static_cast<int>(triH); ++k) {
            const float f = (k + 0.5f) / triH, half = 0.5f + 5.0f * f; // narrow at the tip
            const float ry = up ? top + k : top + stemH + triH - 1.0f - k;
            DrawSolidRectAbs(widgetRaw, cx - half, ry, half * 2.0f, 1.0f, ink, 0.95f * level);
        }
        DrawSolidRectAbs(widgetRaw, cx - 1.5f, up ? top + triH : top, 3.0f, stemH, ink, 0.95f * level);
        if (fq >= 0) {
            char num[8];
            std::snprintf(num, sizeof(num), "%d", fq + 1);
            DrawLabelAbs(x0 + 20.0f, y0, w - 22.0f, h, num, 0xfff0ecdcu, 0.95f * level, TextAlignH::Center, TextAlignV::Center,
                         nullptr, 0.85f);
        }
    };
    // A vault's teleport pads (they link its sections): a teal ring with its pair's letter, the two pads that take you to
    // each other alike (A and A, B and B: make_halls.py stores pad 2k beside its partner 2k + 1), dimmed off your floor
    // like the beacons, under them
    for (uint32_t i = 0; i < d.Pads(); ++i) {
        const Beacon& p = g_beacons[d.firstBeacon + d.Beacons() + i];
        const float px = originX + p.x * zoom, py = originY + (mapWorldH - p.z) * zoom;
        if (py - 15.0f < top) continue;
        if (g_mapFloor >= 0 && FloorAt(g_mapDungeon, p.y) != g_mapFloor) continue; // only the floor shown's pads
        // ... and on the level shown: a floor can hold levels a room's height apart (a pad 12 m under the ledge you
        // stood on showed dimmed, "there isn't one", 2026-11-09)
        if (!OnMapLevel(p.y)) continue;
        ++shownPads;
        const float level = !OnMapLevel(p.y) ? 0.4f : 1.0f;
        // 30 px with the letter at 0.75: at 22 px a C filled its ring, at 28 px / 0.85 a B still did ("cramped",
        // 2026-11-09); kPadLetterDy nudges the letter to the ring's optical middle
        constexpr float kPadRing = 30.0f, kPadLetterDy = 1.0f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, px - kPadRing * 0.5f, py - kPadRing * 0.5f, kPadRing, kPadRing, 0xff0a0e10u, 0.85f * level);
        DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, px - kPadRing * 0.5f, py - kPadRing * 0.5f, kPadRing, kPadRing, 0xffd8c85au,
                         0.95f * level);
        const char letter[2] = {static_cast<char>('A' + (i / 2) % 26), 0};
        DrawLabelAbs(px - kPadRing * 0.5f, py - kPadRing * 0.5f + kPadLetterDy, kPadRing, kPadRing, letter, 0xffd8c85au, 0.95f * level,
                     TextAlignH::Center, TextAlignV::Center, nullptr, 0.75f);
        // Hovered, with its partner on another floor: where it takes you (up or down, which floor)
        if (const uint32_t j = i ^ 1u; j < d.Pads() && haveCursor && std::hypot(mx - px, my - py) <= kPadRing * 0.5f + 2.0f) {
            floorArrow(px, py, p.y, g_beacons[d.firstBeacon + d.Beacons() + j].y, level);
            anyHovered = true;
        }
    }
    // The pads that take you out of the vault (to its entrance in the Ancient Lands): an amber double ring
    for (uint32_t i = 0; i < d.Exits(); ++i) {
        const Beacon& p = g_beacons[d.firstBeacon + d.Beacons() + d.Pads() + i];
        const float px = originX + p.x * zoom, py = originY + (mapWorldH - p.z) * zoom;
        if (py - 11.0f < top) continue;
        if (g_mapFloor >= 0 && FloorAt(g_mapDungeon, p.y) != g_mapFloor) continue; // only the floor shown's pads
        // ... and on the level shown: a floor can hold levels a room's height apart (a pad 12 m under the ledge you
        // stood on showed dimmed, "there isn't one", 2026-11-09)
        if (!OnMapLevel(p.y)) continue;
        ++shownExits;
        const float level = !OnMapLevel(p.y) ? 0.4f : 1.0f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, px - 11.0f, py - 11.0f, 22.0f, 22.0f, 0xff0a0e10u, 0.85f * level);
        DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, px - 11.0f, py - 11.0f, 22.0f, 22.0f, 0xff3cc8f8u, 0.95f * level);
        DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, px - 6.0f, py - 6.0f, 12.0f, 12.0f, 0xff3cc8f8u, 0.95f * level);
        // where it takes you, while hovered (traps.bin kind 8: its destination's height)
        LoadTraps();
        for (const Trap& t : g_traps)
            if (t.kind == 8 && std::fabs(t.x - p.x) < 1.0f && std::fabs(t.z - p.z) < 1.0f && std::fabs(t.y - p.y) < 2.0f) {
                if (haveCursor && std::hypot(mx - px, my - py) <= 13.0f) { // hovered: where it takes you
                    floorArrow(px - 3.0f, py, p.y, t.ex, level);
                    anyHovered = true;
                }
                break;
            }
    }
    for (uint32_t i = 0; i < d.Beacons(); ++i) {
        const Beacon& b = g_beacons[d.firstBeacon + i];
        const float sx = originX + b.x * zoom + 12.0f, sy = originY + (mapWorldH - b.z) * zoom - 14.0f;
        if (sy - kBadge * 0.5f < top) continue;
        if (g_mapFloor >= 0 && FloorAt(g_mapDungeon, b.y) != g_mapFloor) continue; // only the floor shown's beacons
        ++shownBeacons;
        const bool reached = travel && i < 8 && (hhp->masks[g_mapDungeon] >> i & 1);
        // A beacon on another floor than yours (more than kLevelBandM up or down, as the floors the map dims) is dimmed:
        // in a vault the levels lie over each other
        const float level = !OnMapLevel(b.y) ? 0.4f : 1.0f;
        // The badge or the beacon's icon under it (one title for both)
        const float ix = sx - 12.0f, iy = sy + 14.0f;
        const bool hovered = haveCursor && ((std::fabs(mx - sx) <= kBadge * 0.5f + 2.0f && std::fabs(my - sy) <= kBadge * 0.5f + 2.0f) ||
                                            (std::fabs(mx - ix) <= 12.0f && std::fabs(my - iy) <= 12.0f));
        const float ring = hovered ? kBadge + 10.0f : kBadge + 4.0f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring,
                         travel && !reached ? 0xff8c9296u : hovered && reached ? 0xff7ae6ffu : 0xff3cc8f8u,
                         hovered ? 0.95f : 0.95f * level); // amber, grey
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - kBadge * 0.5f, sy - kBadge * 0.5f, kBadge, kBadge, 0xff0a0e10u,
                         hovered ? 0.92f : 0.92f * level);
        char num[8];
        std::snprintf(num, sizeof(num), "%u", i + 1);
        DrawLabelAbs(sx - kBadge * 0.5f, sy - kBadge * 0.5f, kBadge, kBadge, num, travel && !reached ? 0xffb4b4b4u : 0xffffffffu,
                     (travel && !reached ? 0.7f : 1.0f) * (hovered ? 1.0f : level), TextAlignH::Center, TextAlignV::Center);
        if (hovered) {
            anyHovered = true;
            char title[128];
            std::snprintf(title, sizeof(title), "%s %u", OreName(kBeaconsFirst), i + 1);
            DrawMapHoverTitle(sx + kBadge * 0.5f + 10.0f, sy, title);
            if (clicked && reached) InterlockedExchange(&hhp->request, static_cast<LONG>(0x80000000u | g_mapDungeon << 8 | i));
        }
    }
    // A vault's or spire's legend, lower left: the symbols it uses, each as the map draws it
    if (IsVault(g_mapDungeon)) {
        struct Row { int kind; const char* text; };
        Row rows[16];
        int n = 0;
        // only what the floor shown has (a beacon two floors up listed here confused, 2026-11-09)
        if (shownBeacons) rows[n++] = {0, OreName(kBeaconsFirst)};
        if (shownPads) rows[n++] = {1, umm_lang::T(umm_lang::Str::TeleportPair)};
        if (shownExits) rows[n++] = {2, umm_lang::T(umm_lang::Str::OneWayTeleport)};
        // the traps on the floor shown (DrawMapPlan: g_mapTrapKinds)
        if (g_mapTrapKinds & (1u << 1)) rows[n++] = {3, umm_lang::T(umm_lang::Str::SpikeTrap)};
        if (g_mapTrapKinds & (1u << 2)) rows[n++] = {4, umm_lang::T(umm_lang::Str::FireTrap)};
        if (g_mapTrapKinds & (1u << 3)) rows[n++] = {5, umm_lang::T(umm_lang::Str::ElectricTrap)};
        if (g_mapTrapKinds & (1u << 4)) rows[n++] = {6, umm_lang::T(umm_lang::Str::TrapRail)};
        if (g_mapTrapKinds & (1u << 21)) rows[n++] = {7, umm_lang::T(umm_lang::Str::LockedDoor)};
        if (g_mapTrapKinds & (1u << 22)) rows[n++] = {8, umm_lang::T(umm_lang::Str::ClosedDoor)}; // a plain door, shut (opens by hand)
        if (g_mapTrapKinds & (1u << 23)) rows[n++] = {9, umm_lang::T(umm_lang::Str::OpenDoor)};
        if (g_mapTrapKinds & (1u << 6)) rows[n++] = {13, umm_lang::T(umm_lang::Str::MagicBarrier)};
        if (g_mapTrapKinds & (1u << 10)) rows[n++] = {10, umm_lang::T(umm_lang::Str::PressurePlate)};
        if (g_mapTrapKinds & (1u << 11)) rows[n++] = {11, umm_lang::T(umm_lang::Str::ButtonLever)};
        if (g_mapTrapKinds & (1u << 12)) rows[n++] = {12, umm_lang::T(umm_lang::Str::ShootSwitch)};
        float textW = 0.0f;
        for (int i = 0; i < n; ++i) textW = std::max(textW, umm_lang::TextWidth(rows[i].text) * 1.5f + 6.0f);
        const float rowH = 30.0f, pad = 10.0f, iconW = 30.0f;
        const float boxW = pad + iconW + 8.0f + textW + pad, boxH = pad * 2.0f + rowH * n;
        const float boxX = rootRect[0] + 24.0f;
        const float boxY = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f) - 64.0f - 16.0f - boxH;
        if (n) DrawSolidRectAbs(widgetRaw, boxX, boxY, boxW, boxH, 0xff0a0e10u, 0.75f);
        for (int i = 0; i < n; ++i) {
            const float cx = boxX + pad + iconW * 0.5f, cy = boxY + pad + rowH * (i + 0.5f);
            if (rows[i].kind == 0) {
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, 0xff3cc8f8u, 0.95f);
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f, 0xff0a0e10u, 0.92f);
                DrawLabelAbs(cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, "1", 0xffffffffu, 1.0f, TextAlignH::Center, TextAlignV::Center);
            } else if (rows[i].kind == 1) {
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, 0xff0a0e10u, 0.85f);
                DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, 0xffd8c85au, 0.95f);
                DrawLabelAbs(cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, "A", 0xffd8c85au, 0.95f, TextAlignH::Center, TextAlignV::Center);
            } else if (rows[i].kind == 2) {
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, 0xff0a0e10u, 0.85f);
                DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f, 0xff3cc8f8u, 0.95f);
                DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 0xff3cc8f8u, 0.95f);
            } else if (rows[i].kind == 3) { // a spike plate
                DrawSolidRectAbs(widgetRaw, cx - 10.0f, cy - 10.0f, 20.0f, 20.0f, 0xff3a3ad8u, 0.5f);
                for (int a = -1; a <= 1; a += 2)
                    for (int c2 = -1; c2 <= 1; c2 += 2)
                        DrawSolidRectAbs(widgetRaw, cx + a * 5.0f - 2.0f, cy + c2 * 5.0f - 2.0f, 4.0f, 4.0f, 0xff2020b0u, 0.9f);
            } else if (rows[i].kind >= 7 && rows[i].kind <= 9) { // a door: locked, closed, open
                const uint32_t c = rows[i].kind == 7 ? 0xff3a3ae0u : rows[i].kind == 8 ? 0xff20b0f0u : 0xff50c860u;
                DrawSolidRectAbs(widgetRaw, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f, c, 0.95f);
                DrawSolidRectAbs(widgetRaw, cx - 3.0f, cy - 6.0f, 6.0f, 12.0f, 0xff0a0e10u, rows[i].kind == 9 ? 0.2f : 0.75f);
            } else if (rows[i].kind == 13) { // a barrier
                DrawSolidRectAbs(widgetRaw, cx - 8.0f, cy - 8.0f, 16.0f, 16.0f, 0xffd060b0u, 0.75f);
            } else if (rows[i].kind >= 10 && rows[i].kind <= 12) { // a switch
                const uint32_t c = 0xff40e0f0u;
                if (rows[i].kind == 10) {
                    DrawSolidRectAbs(widgetRaw, cx - 8.0f, cy - 8.0f, 16.0f, 2.0f, c, 0.95f);
                    DrawSolidRectAbs(widgetRaw, cx - 8.0f, cy + 6.0f, 16.0f, 2.0f, c, 0.95f);
                    DrawSolidRectAbs(widgetRaw, cx - 8.0f, cy - 8.0f, 2.0f, 16.0f, c, 0.95f);
                    DrawSolidRectAbs(widgetRaw, cx + 6.0f, cy - 8.0f, 2.0f, 16.0f, c, 0.95f);
                } else if (rows[i].kind == 11) {
                    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f, c, 0.95f);
                } else {
                    DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f, c, 0.95f);
                    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 2.5f, cy - 2.5f, 5.0f, 5.0f, c, 0.95f);
                }
            } else if (rows[i].kind == 6) { // a rail
                for (int a = -1; a <= 1; ++a) DrawSolidRectAbs(widgetRaw, cx + a * 7.0f - 3.0f, cy - 3.0f, 6.0f, 6.0f, 0xff1c1c90u, 0.85f);
            } else { // a fireball lane or an electric beam: its launcher and dots
                const uint32_t c = rows[i].kind == 4 ? 0xff1a8cffu : 0xffffe08cu;
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - 11.0f, cy - 6.0f, 12.0f, 12.0f, c, 0.95f);
                for (int a = 0; a < 3; ++a) DrawSolidRectAbs(widgetRaw, cx + 3.0f + a * 4.5f, cy - 1.5f, 3.0f, 3.0f, c, 0.85f);
            }
            DrawLabelAbs(boxX + pad + iconW + 8.0f, cy - rowH * 0.5f, textW, rowH, rows[i].text, 0xffe8e4dcu, 0.95f,
                         TextAlignH::Left, TextAlignV::Center);
        }
    }
    return anyHovered; // you and the other players: DrawDungeonMapPlayers (map_key_panel.cpp)
}

} // namespace halls

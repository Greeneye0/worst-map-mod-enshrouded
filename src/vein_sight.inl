// GG's vein sight: with GG, the ore and Materials veins around you (50 m) whose Mining / Materials rows are on show
// through the ground for a while, each outlined in its ore's colour (the edge its 4 m chunks make on screen); near you
// (12 m) its edges glow on the rock itself (the game's build-zone borders), following you as you dig; each deposit's
// icon marks its middle. The voxels come from the game's world files (tools/resources/make_veins.py:
// data/veins.bin, next to the DLL, unpacked on the first GG). Included by map_key_panel.cpp before chest_glow.inl,
// whose GG and projection it shares.

bool LocalPlayerY(uint8_t* client, float& y); // map_key_panel.cpp, below

namespace vein_sight {

constexpr float kRangeMeters = 50.0f;
constexpr uint32_t kShowMs = 30000;  // long enough to dig toward it
constexpr int kTileMeters = 128;
constexpr float kDepositReach = 30.0f; // a deposit's ore: within this of its marker
constexpr unsigned kMinVoxels = 50;  // as the map's deposits: smaller veins are left out
constexpr unsigned kMinChunkVoxels = 24; // and chunks with less ore (grains in the soil, which would join a field into one)
constexpr size_t kMaxChunks = 1600;
constexpr int kMaxBeads = 3600;      // outline draws a frame: the UI renderer's budget, shared with the map
constexpr float kCloseMeters = 12.0f; // near you: the ore's 1 m cells themselves, where they sit in the stone
constexpr size_t kMaxCloseCells = 1800;

// Each row's colour, 0xAABBGGRR (kMiningFirst ..., then kMaterialsFirst ...)
constexpr uint32_t kRowColour[kMiningCount + kMaterialsCount] = {
    0xffb0b0b8u, 0xfff4f4f4u, 0xff3c82e8u, 0xff4a78a8u, 0xff20a8ffu, 0xffc8c0b0u, 0xffb4d8e8u, // flint..limestone
    0xffc8e8f0u, 0xff30e8f0u, 0xff3c5cc8u, 0xffe0703cu, 0xff606060u, 0xffe8e8e8u, 0xffe05ab4u, // fossil..amethyst
    0xff8c3c6cu, 0xff28c8ffu, 0xffe0e070u, 0xffe0c8f0u, 0xff4040e0u, 0xfffff0d0u, 0xfff0c040u, // obsidian..celesthium
    0xff3c5a78u, 0xff2c4a64u, 0xff6aa0c8u, 0xff60a0c0u, 0xffd8e8f0u, 0xff303030u, 0xff40e070u, // soils..toxic slime
    0xffd0e050u, 0xfff0a040u, 0xfffff0d0u, 0xff28c8ffu, 0xfff070c0u,                           // growths..essence
};

struct Point {
    uint8_t x, z;
    uint16_t y;
    uint8_t row, amount;
};
static_assert(sizeof(Point) == 6, "veins.bin points are 6 bytes");

std::vector<uint8_t> g_data; // unpacked veins.bin
const uint32_t* g_first = nullptr;
const Point* g_points = nullptr;
const uint8_t* g_masks = nullptr; // each point's u64 cell mask (version 2)
uint32_t g_tiles = 0;
bool g_tried = false;

bool Load() {
    if (g_tried) return g_points != nullptr;
    g_tried = true;
    const std::string path = greeneye::DataPath("veins.bin");
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (g_log) g_log("Vein sight: %s not found; GG lights no veins", path.c_str());
        return false;
    }
    LARGE_INTEGER size{};
    std::vector<uint8_t> packed;
    DWORD got = 0;
    const bool read = GetFileSizeEx(f, &size) && size.QuadPart > 12 && size.QuadPart < (64ll << 20) &&
                      (packed.resize(static_cast<size_t>(size.QuadPart)), true) &&
                      ReadFile(f, packed.data(), static_cast<DWORD>(packed.size()), &got, nullptr) && got == packed.size();
    CloseHandle(f);
    uint32_t rawSize = 0, packedSize = 0;
    if (!read || std::memcmp(packed.data(), "VNZ1", 4) != 0) return false;
    std::memcpy(&rawSize, packed.data() + 4, 4);
    std::memcpy(&packedSize, packed.data() + 8, 4);
    if (packedSize + 12 > packed.size() || rawSize > (64u << 20)) return false;
    using DecompressFn = LONG(NTAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);
    using WorkSizeFn = LONG(NTAPI*)(USHORT, PULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto decompress = reinterpret_cast<DecompressFn>(GetProcAddress(ntdll, "RtlDecompressBufferEx"));
    auto workSize = reinterpret_cast<WorkSizeFn>(GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize"));
    constexpr USHORT kXpressHuff = 4;
    ULONG ws = 0, fws = 0;
    if (!decompress || !workSize || workSize(kXpressHuff, &ws, &fws) != 0) return false;
    std::vector<uint8_t> work(ws);
    g_data.resize(rawSize);
    ULONG out = 0;
    if (decompress(kXpressHuff, g_data.data(), rawSize, packed.data() + 12, packedSize, &out, work.data()) != 0 ||
        out != rawSize || rawSize < 16 || std::memcmp(g_data.data(), "VEIN", 4) != 0) {
        g_data.clear();
        if (g_log) g_log("Vein sight: veins.bin unreadable");
        return false;
    }
    uint32_t version = 0, count = 0;
    std::memcpy(&version, g_data.data() + 4, 4);
    std::memcpy(&g_tiles, g_data.data() + 8, 4);
    std::memcpy(&count, g_data.data() + 12, 4);
    const size_t headerEnd = 16 + (static_cast<size_t>(g_tiles) * g_tiles + 1) * 4;
    if (version != 2 || g_tiles == 0 || g_tiles > 256 || headerEnd + static_cast<size_t>(count) * (sizeof(Point) + 8) != rawSize) {
        g_data.clear();
        return false;
    }
    g_first = reinterpret_cast<const uint32_t*>(g_data.data() + 16);
    g_points = reinterpret_cast<const Point*>(g_data.data() + headerEnd);
    g_masks = g_data.data() + headerEnd + static_cast<size_t>(count) * sizeof(Point);
    if (g_log) g_log("Vein sight: %u voxel chunks loaded", count);
    return true;
}

// A vein: neighbouring chunks of one row (in the same or a touching 4 m cell), drawn as one outline
struct Chunk {
    float x, y, z, half; // its voxels' centre, and half the side of a cube as big as its ore
};
struct Vein {
    uint8_t row;
    std::vector<Chunk> chunks;
    float cx, cy, cz;
};
std::vector<Vein> g_veins;      // what GG lit (UI thread)
std::vector<size_t> g_deposits; // the deposits among them, for their icons
uint64_t g_start = 0, g_until = 0;
uint64_t g_cellsAt = 0; // when the cells near you were last gathered

bool RowOn(size_t row) { return row < kMiningCount + kMaterialsCount && g_oreEnabled[kMiningFirst + row]; }

// A chunk of a row within reach of one of the deposits of that row the map shows around you
bool NearShownDeposit(uint8_t row, float x, float z) {
    for (size_t d : g_deposits) {
        const OreDeposit& dep = kOreDeposits[d];
        if (dep.ore != kMiningFirst + row) continue;
        if ((dep.x - x) * (dep.x - x) + (dep.z - z) * (dep.z - z) <= kDepositReach * kDepositReach) return true;
    }
    return false;
}

// GG: the veins of the rows that are on, nearest first
void LightNearby(uint8_t* client) {
    g_veins.clear();
    g_deposits.clear();
    bool any = false;
    for (size_t r = 0; r < kMiningCount + kMaterialsCount && !any; ++r) any = RowOn(r);
    float px = 0.0f, pz = 0.0f;
    if (!any || !GetHudPlayerWorldPos(client, px, pz) || !Load()) return;
    // The deposits the map shows around you (their rows on): only their ore lights up, not every voxel of the row (a
    // cliff of limestone, grains of flint in the soil)
    for (size_t d = 0; d < kOreDepositCount; ++d) {
        const OreDeposit& dep = kOreDeposits[d];
        if (dep.ore < kMiningFirst || dep.ore >= kMiningFirst + kMiningCount + kMaterialsCount || !g_oreEnabled[dep.ore]) continue;
        const float reach = kRangeMeters + kDepositReach;
        if ((dep.x - px) * (dep.x - px) + (dep.z - pz) * (dep.z - pz) <= reach * reach) g_deposits.push_back(d);
    }
    if (g_deposits.empty()) return;
    struct Near {
        float x, y, z;
        uint8_t row, amount;
    };
    std::vector<Near> pts;
    const int t0x = std::max(0, static_cast<int>((px - kRangeMeters) / kTileMeters));
    const int t1x = std::min(static_cast<int>(g_tiles) - 1, static_cast<int>((px + kRangeMeters) / kTileMeters));
    const int t0z = std::max(0, static_cast<int>((pz - kRangeMeters) / kTileMeters));
    const int t1z = std::min(static_cast<int>(g_tiles) - 1, static_cast<int>((pz + kRangeMeters) / kTileMeters));
    for (int tz = t0z; tz <= t1z; ++tz) {
        for (int tx = t0x; tx <= t1x; ++tx) {
            const uint32_t t = static_cast<uint32_t>(tz) * g_tiles + static_cast<uint32_t>(tx);
            for (uint32_t i = g_first[t]; i < g_first[t + 1]; ++i) {
                const Point& p = g_points[i];
                if (!RowOn(p.row) || p.amount * 2u < kMinChunkVoxels) continue;
                const float x = tx * kTileMeters + p.x * 0.5f, z = tz * kTileMeters + p.z * 0.5f;
                if ((x - px) * (x - px) + (z - pz) * (z - pz) > kRangeMeters * kRangeMeters) continue;
                if (!NearShownDeposit(p.row, x, z)) continue;
                pts.push_back({x, p.y * 0.5f, z, p.row, p.amount});
            }
        }
    }
    // Join neighbours of the same row (union-find over 4 m cells)
    std::vector<int> parent(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) parent[i] = static_cast<int>(i);
    auto find = [&](int i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    auto key = [](int gx, int gy, int gz, int row) {
        return (static_cast<uint64_t>(gx & 0xffff) << 40) | (static_cast<uint64_t>(gy & 0xffff) << 24) |
               (static_cast<uint64_t>(gz & 0xffff) << 8) | static_cast<uint64_t>(row);
    };
    std::unordered_map<uint64_t, std::vector<int>> grid;
    for (size_t i = 0; i < pts.size(); ++i)
        grid[key(int(pts[i].x / 4), int(pts[i].y / 4), int(pts[i].z / 4), pts[i].row)].push_back(static_cast<int>(i));
    for (size_t i = 0; i < pts.size(); ++i) {
        const int gx = int(pts[i].x / 4), gy = int(pts[i].y / 4), gz = int(pts[i].z / 4);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find(key(gx + dx, gy + dy, gz + dz, pts[i].row));
                    if (it == grid.end()) continue;
                    for (int j : it->second) parent[find(j)] = find(static_cast<int>(i));
                }
    }
    std::vector<Vein> veins;
    std::vector<unsigned> voxels;
    std::unordered_map<int, size_t> veinOf;
    for (size_t i = 0; i < pts.size(); ++i) {
        auto [it, added] = veinOf.emplace(find(static_cast<int>(i)), veins.size());
        if (added) {
            veins.push_back({pts[i].row, {}, 0.0f, 0.0f, 0.0f});
            voxels.push_back(0);
        }
        Vein& v = veins[it->second];
        const float amount = pts[i].amount;
        v.chunks.push_back({pts[i].x, pts[i].y, pts[i].z, std::max(0.5f, 2.0f * std::cbrt(amount / 256.0f))});
        v.cx += pts[i].x * amount;
        v.cy += pts[i].y * amount;
        v.cz += pts[i].z * amount;
        voxels[it->second] += pts[i].amount * 2u;
    }
    for (size_t v = 0; v < veins.size(); ++v) {
        if (voxels[v] < kMinVoxels) continue;
        const float w = voxels[v] * 0.5f;
        veins[v].cx /= w;
        veins[v].cy /= w;
        veins[v].cz /= w;
        g_veins.push_back(std::move(veins[v]));
    }
    std::sort(g_veins.begin(), g_veins.end(), [&](const Vein& a, const Vein& b) {
        return (a.cx - px) * (a.cx - px) + (a.cz - pz) * (a.cz - pz) < (b.cx - px) * (b.cx - px) + (b.cz - pz) * (b.cz - pz);
    });
    size_t chunks = 0, n = 0;
    while (n < g_veins.size() && chunks + g_veins[n].chunks.size() <= kMaxChunks) chunks += g_veins[n++].chunks.size();
    g_veins.resize(n);
    g_start = GetTickCount64();
    g_until = g_start + kShowMs;
    g_cellsAt = 0; // gathered on the next frame
    if (g_log) {
        // Which ores: "Flintstone 3/2" (veins lit / deposits near)
        std::string rows;
        for (size_t r = 0; r < kMiningCount + kMaterialsCount; ++r) {
            size_t v = 0, d = 0;
            for (const Vein& vein : g_veins) v += vein.row == r ? 1 : 0;
            for (size_t dep : g_deposits) d += kOreDeposits[dep].ore == kMiningFirst + r ? 1 : 0;
            if (!v && !d) continue;
            char part[64];
            std::snprintf(part, sizeof(part), "%s%s %zu/%zu", rows.empty() ? "" : ", ", kOreLabels[kMiningFirst + r], v, d);
            rows += part;
        }
        g_log("Vein sight: %zu veins (%zu chunks), %zu deposits within %.0f m: %s", g_veins.size(), chunks,
              g_deposits.size(), kRangeMeters, rows.c_str());
    }
}

// On screen through the ground: each vein's outline (the edge of its chunks as the camera sees them, dents and all) in
// its ore's colour on a dark edge, a faint glow inside, breathing with GG's outline; the deposits' icons on a dark disc.
// Projected like chest_glow's markers.
// On the rock itself: the game's zone visualization (the glowing border build zones draw where they meet terrain)
// takes up to 32 boxes a frame (ZoneVisualizationSdfShaderStorageData: a bounding sphere {centre, radius^2} and an
// element {half size, type} each). Its filler (0x212d00 in the Oct 2026 build) is hooked: after the game's own zones
// the ore's boxes are added, one per 4 m chunk near you, tight around its ore cells, and the borders are switched on,
// so the ore's edges glow on the stone where it meets the surface, hidden by rock in front like the game's own.
struct ZoneBox {
    float centre[3], half[3];
    uint32_t type; // 0 BuildZone, 1 NoBuildZone: its border colour
};
constexpr unsigned kZoneSlots = 32, kMaxZoneBoxes = 28; // a few slots stay the game's (its build zones come first)
SRWLOCK g_zoneLock = SRWLOCK_INIT;
ZoneBox g_zoneBoxes[kMaxZoneBoxes];
unsigned g_zoneBoxCount = 0;

void PublishZoneBoxes(const ZoneBox* boxes, unsigned count) {
    AcquireSRWLockExclusive(&g_zoneLock);
    g_zoneBoxCount = std::min(count, kMaxZoneBoxes);
    std::memcpy(g_zoneBoxes, boxes, g_zoneBoxCount * sizeof(ZoneBox));
    ReleaseSRWLockExclusive(&g_zoneLock);
}

using ZoneFillFn = void (*)(uint32_t* header, uint8_t* storage, uint8_t* frame, uint8_t* module);
ZoneFillFn g_origZoneFill = nullptr;

void AddZoneBoxes(uint32_t* header, uint8_t* storage, uint8_t* frame, uint8_t* module) {
    if (!*reinterpret_cast<void**>(module + 0x38)) return; // no settings: nothing draws this frame
    AcquireSRWLockShared(&g_zoneLock);
    unsigned n = header[0];
    for (unsigned i = 0; i < g_zoneBoxCount && n < kZoneSlots; ++i, ++n) {
        const ZoneBox& b = g_zoneBoxes[i];
        float* sphere = reinterpret_cast<float*>(storage + n * 16);
        float* element = reinterpret_cast<float*>(storage + 0x200 + n * 16);
        sphere[0] = b.centre[0];
        sphere[1] = b.centre[1];
        sphere[2] = b.centre[2];
        sphere[3] = b.half[0] * b.half[0] + b.half[1] * b.half[1] + b.half[2] * b.half[2];
        element[0] = b.half[0];
        element[1] = b.half[1];
        element[2] = b.half[2];
        reinterpret_cast<uint32_t*>(element)[3] = b.type; // its border colour
    }
    const bool any = g_zoneBoxCount > 0;
    ReleaseSRWLockShared(&g_zoneLock);
    header[0] = n;
    if (any) frame[0x90] |= 2; // DrawBuildZoneBorder
}

void SafeAddZoneBoxes(uint32_t* header, uint8_t* storage, uint8_t* frame, uint8_t* module) {
    __try {
        AddZoneBoxes(header, storage, frame, module);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ZoneFillHook(uint32_t* header, uint8_t* storage, uint8_t* frame, uint8_t* module) {
    g_origZoneFill(header, storage, frame, module);
    greeneye::Lease lease;
    if (lease && header && storage && frame && module) SafeAddZoneBoxes(header, storage, frame, module);
}

void InstallZoneHook() {
    uint8_t* fn = FindUnique("48 89 5C 24 18 55 56 57 41 54 41 55 48 8D AC 24 A0 FC FF FF 48 81 EC 60 04 00 00 0F 57 C0 "
                             "49 8B F0 66 0F 7F 01 48 8B FA 4C 8B E9 33 D2 48 8D 4C 24 60 41 B8 00 04 00 00 49 8B D9");
    if (fn) g_origZoneFill = reinterpret_cast<ZoneFillFn>(InstallHook(fn, reinterpret_cast<void*>(&ZoneFillHook), 12));
    if (g_log) g_log(g_origZoneFill ? "Vein sight: ore outlines on the rock ready" : "Vein sight: zone visualization not found");
}

// Near you (12 m): each 1 m cell of the rows that are on, nearest first; refreshed as you move while vein sight shows
struct Cell {
    float x, y, z;
    uint8_t row;
};
std::vector<Cell> g_cells;

void GatherCells(uint8_t* client) {
    g_cells.clear();
    float px = 0.0f, pz = 0.0f, py = 0.0f;
    if (!g_masks || !GetHudPlayerWorldPos(client, px, pz) || !LocalPlayerY(client, py)) {
        PublishZoneBoxes(nullptr, 0);
        return;
    }
    struct Candidate {
        ZoneBox box;
        float dist2;
    };
    std::vector<Candidate> candidates;
    const int t0x = std::max(0, static_cast<int>((px - kCloseMeters) / kTileMeters));
    const int t1x = std::min(static_cast<int>(g_tiles) - 1, static_cast<int>((px + kCloseMeters) / kTileMeters));
    const int t0z = std::max(0, static_cast<int>((pz - kCloseMeters) / kTileMeters));
    const int t1z = std::min(static_cast<int>(g_tiles) - 1, static_cast<int>((pz + kCloseMeters) / kTileMeters));
    const float r2 = kCloseMeters * kCloseMeters;
    for (int tz = t0z; tz <= t1z; ++tz) {
        for (int tx = t0x; tx <= t1x; ++tx) {
            const uint32_t t = static_cast<uint32_t>(tz) * g_tiles + static_cast<uint32_t>(tx);
            for (uint32_t i = g_first[t]; i < g_first[t + 1]; ++i) {
                const Point& p = g_points[i];
                if (!RowOn(p.row) || !NearShownDeposit(p.row, tx * kTileMeters + p.x * 0.5f, tz * kTileMeters + p.z * 0.5f))
                    continue;
                // The chunk: the 4 m cell holding the voxels' centre
                const float ox = std::floor((tx * kTileMeters + p.x * 0.5f) / 4.0f) * 4.0f;
                const float oz = std::floor((tz * kTileMeters + p.z * 0.5f) / 4.0f) * 4.0f;
                const float oy = std::floor(p.y * 0.5f / 4.0f) * 4.0f;
                const float dx = std::max(0.0f, std::max(ox - px, px - ox - 4.0f));
                const float dy = std::max(0.0f, std::max(oy - py, py - oy - 4.0f));
                const float dz = std::max(0.0f, std::max(oz - pz, pz - oz - 4.0f));
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                uint64_t mask = 0;
                std::memcpy(&mask, g_masks + static_cast<size_t>(i) * 8, 8);
                if (__popcnt64(mask) >= 4) {
                    // The chunk's box, tight around its ore cells
                    int lo[3] = {4, 4, 4}, hi[3] = {-1, -1, -1};
                    for (uint64_t m = mask; m; m &= m - 1) {
                        unsigned long b = 0;
                        _BitScanForward64(&b, m);
                        const int c[3] = {static_cast<int>(b & 3), static_cast<int>((b >> 2) & 3), static_cast<int>(b >> 4)};
                        for (int k = 0; k < 3; ++k) {
                            lo[k] = std::min(lo[k], c[k]);
                            hi[k] = std::max(hi[k], c[k]);
                        }
                    }
                    const float o[3] = {ox, oy, oz};
                    ZoneBox box{};
                    for (int k = 0; k < 3; ++k) {
                        box.half[k] = (hi[k] - lo[k] + 1) * 0.5f;
                        box.centre[k] = o[k] + lo[k] + box.half[k];
                    }
                    const float ddx = box.centre[0] - px, ddy = box.centre[1] - py, ddz = box.centre[2] - pz;
                    const float dist2 = ddx * ddx + ddy * ddy + ddz * ddz;
                    box.type = 0;
                    candidates.push_back({box, dist2});
                }
                for (; mask; mask &= mask - 1) {
                    unsigned long b = 0;
                    _BitScanForward64(&b, mask);
                    const float cx = ox + (b & 3) + 0.5f, cy = oy + ((b >> 2) & 3) + 0.5f, cz = oz + (b >> 4) + 0.5f;
                    if ((cx - px) * (cx - px) + (cy - py) * (cy - py) + (cz - pz) * (cz - pz) <= r2)
                        g_cells.push_back({cx, cy, cz, p.row});
                }
            }
        }
    }
    // The nearest chunks' boxes to the zone visualization
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.dist2 < b.dist2; });
    ZoneBox boxes[kMaxZoneBoxes];
    unsigned count = 0;
    for (const Candidate& c : candidates) {
        if (count == kMaxZoneBoxes) break;
        boxes[count++] = c.box;
    }
    PublishZoneBoxes(boxes, count);
    // Only the ore body's surface: a cell with a side not against more of the same ore (a solid cliff of it is a shell,
    // not a fog)
    auto cellKey = [](float x, float y, float z, uint8_t row) {
        return (static_cast<uint64_t>(static_cast<int>(x) & 0xffff) << 40) |
               (static_cast<uint64_t>(static_cast<int>(y) & 0xffff) << 24) |
               (static_cast<uint64_t>(static_cast<int>(z) & 0xffff) << 8) | row;
    };
    std::unordered_set<uint64_t> filled;
    filled.reserve(g_cells.size() * 2);
    for (const Cell& c : g_cells) filled.insert(cellKey(c.x, c.y, c.z, c.row));
    const float kSides[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<Cell> surface;
    for (const Cell& c : g_cells) {
        for (const auto& d : kSides) {
            if (!filled.count(cellKey(c.x + d[0], c.y + d[1], c.z + d[2], c.row))) {
                surface.push_back(c);
                break;
            }
        }
    }
    g_cells.swap(surface);
    if (g_cells.size() > kMaxCloseCells) {
        std::nth_element(g_cells.begin(), g_cells.begin() + kMaxCloseCells, g_cells.end(), [&](const Cell& a, const Cell& b) {
            return (a.x - px) * (a.x - px) + (a.y - py) * (a.y - py) + (a.z - pz) * (a.z - pz) <
                   (b.x - px) * (b.x - px) + (b.y - py) * (b.y - py) + (b.z - pz) * (b.z - pz);
        });
        g_cells.resize(kMaxCloseCells);
    }
}

void Draw(uint8_t* client, void* widgetRaw, const float* view, const float* proj, const float* viewport) {
    const uint64_t now = GetTickCount64();
    if (g_veins.empty() && g_deposits.empty()) return;
    if (now >= g_until) {
        g_veins.clear();
        g_deposits.clear();
        g_cells.clear();
        PublishZoneBoxes(nullptr, 0);
        return;
    }
    float px = 0.0f, pz = 0.0f;
    GetHudPlayerWorldPos(client, px, pz);
    const float elapsed = static_cast<float>(now - g_start) * 0.001f;
    const float breathe = 0.5f - 0.5f * std::cos(std::fmod(elapsed, 1.6f) / 1.6f * 6.2831853f);
    const float fade = std::min(1.0f, static_cast<float>(g_until - now) / 2000.0f) * std::min(1.0f, elapsed / 0.4f);
    auto project = [&](float x, float y, float z, float& sx, float& sy) {
        const float depth = x * view[2] + y * view[6] + z * view[10] + view[14];
        if (!std::isfinite(depth) || depth <= 0.3f) return false;
        const float inv = 1.0f / depth;
        const float vx = inv * (x * view[0] + y * view[4] + z * view[8] + view[12]);
        const float vy = inv * (x * view[1] + y * view[5] + z * view[9] + view[13]);
        sx = ((vy * proj[4] + vx * proj[0] + proj[8] + proj[12]) * 0.5f + 0.5f) * viewport[2] + viewport[0];
        sy = (0.5f - (vy * proj[5] + vx * proj[1] + proj[9] + proj[13]) * 0.5f) * viewport[3] + viewport[1];
        return std::isfinite(sx) && std::isfinite(sy) && std::fabs(sx) < 1e5f && std::fabs(sy) < 1e5f;
    };
    // Each vein's chunks as discs on a grid of 4 px cells; the cells on its edge get a bead (touching: a line)
    constexpr float kCell = 4.0f;
    constexpr int kMaxCells = 500 * 380;
    std::vector<uint8_t> cells;
    int beads = 0;
    for (const Vein& v : g_veins) {
        struct Disc {
            float x, y, r;
        };
        std::vector<Disc> discs;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (const Chunk& c : v.chunks) {
            float cx = 0.0f, cy = 0.0f;
            if (!project(c.x, c.y, c.z, cx, cy)) continue; // behind the camera (standing on or in the vein)
            // Its cube's reach on screen: the farthest projected corner in front of the camera
            float r = 0.0f;
            for (int k = 0; k < 8; ++k) {
                float sx = 0.0f, sy = 0.0f;
                if (project(c.x + ((k & 1) ? c.half : -c.half), c.y + ((k & 2) ? c.half : -c.half),
                            c.z + ((k & 4) ? c.half : -c.half), sx, sy))
                    r = std::max(r, std::sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)));
            }
            r = std::min(r * 0.8f, viewport[2]); // a cube's corners reach past its body
            discs.push_back({cx, cy, r});
            minX = std::min(minX, cx - r);
            maxX = std::max(maxX, cx + r);
            minY = std::min(minY, cy - r);
            maxY = std::max(maxY, cy + r);
        }
        if (discs.empty()) continue;
        // On screen only (plus a cell around it)
        minX = std::max(minX, viewport[0] - kCell);
        minY = std::max(minY, viewport[1] - kCell);
        maxX = std::min(maxX, viewport[0] + viewport[2] + kCell);
        maxY = std::min(maxY, viewport[1] + viewport[3] + kCell);
        if (minX >= maxX || minY >= maxY) continue;
        const int w = static_cast<int>((maxX - minX) / kCell) + 3, h = static_cast<int>((maxY - minY) / kCell) + 3;
        if (w * h > kMaxCells) continue;
        const float ox = minX - kCell, oy = minY - kCell;
        cells.assign(static_cast<size_t>(w) * h, 0);
        for (const Disc& d : discs) {
            const int x0 = std::max(0, static_cast<int>((d.x - d.r - ox) / kCell));
            const int x1 = std::min(w - 1, static_cast<int>((d.x + d.r - ox) / kCell));
            const int y0 = std::max(0, static_cast<int>((d.y - d.r - oy) / kCell));
            const int y1 = std::min(h - 1, static_cast<int>((d.y + d.r - oy) / kCell));
            for (int y = y0; y <= y1; ++y) {
                const float cy = oy + (y + 0.5f) * kCell - d.y;
                for (int x = x0; x <= x1; ++x) {
                    const float cx = ox + (x + 0.5f) * kCell - d.x;
                    if (cx * cx + cy * cy <= d.r * d.r) cells[static_cast<size_t>(y) * w + x] = 1;
                }
            }
        }
        // Pinholes (an empty cell between four full ones) are inside, not edge
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (!cells[i] && cells[i - 1] && cells[i + 1] && cells[i - w] && cells[i + w]) cells[i] = 2;
            }
        const uint32_t colour = kRowColour[v.row];
        // The inside: one faint glow over the vein's box
        const float gw = (maxX - minX), gh = (maxY - minY);
        DrawAssetRectAbs(widgetRaw, kSoftGlowPrimitive, minX, minY, gw, gh, colour, (0.10f + 0.06f * breathe) * fade);
        const float bead = 3.4f + 0.5f * breathe, edge = bead + 2.2f;
        for (int y = 1; y + 1 < h && beads < kMaxBeads; ++y) {
            for (int x = 1; x + 1 < w && beads < kMaxBeads; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (!cells[i] || (cells[i - 1] && cells[i + 1] && cells[i - w] && cells[i + w])) continue;
                const float sx = ox + (x + 0.5f) * kCell, sy = oy + (y + 0.5f) * kCell;
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - edge * 0.5f, sy - edge * 0.5f, edge, edge, 0xff0a0e10u,
                                 0.45f * fade);
                DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - bead * 0.5f, sy - bead * 0.5f, bead, bead, colour,
                                 (0.8f + 0.2f * breathe) * fade);
                beads += 2;
            }
        }
    }
    // Near you: the ore's 1 m cells, for how deep it goes (they are not drawn: a cloud of them reads as noise)
    if (now - g_cellsAt >= 400) {
        GatherCells(client);
        g_cellsAt = now;
    }
    for (size_t d : g_deposits) {
        const OreDeposit& dep = kOreDeposits[d];
        float sx = 0.0f, sy = 0.0f;
        if (!project(float(dep.x), float(dep.topY), float(dep.z), sx, sy)) continue;
        const float dist = std::sqrt((dep.x - px) * (dep.x - px) + (dep.z - pz) * (dep.z - pz));
        const float size = std::max(18.0f, std::min(34.0f, 34.0f * 12.0f / std::max(12.0f, dist)));
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - size * 0.6f, sy - size * 0.6f, size * 1.2f, size * 1.2f,
                         0xff0a0e10u, 0.65f * fade);
        DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, sx - size * 0.65f, sy - size * 0.65f, size * 1.3f, size * 1.3f,
                         kRowColour[dep.ore - kMiningFirst], 0.9f * fade);
        DrawIconRectAbs(widgetRaw, kOreIcons[dep.ore], sx - size * 0.42f, sy - size * 0.42f, size * 0.84f, size * 0.84f,
                        0xffffffffu, fade);
    }
}

bool Active() { return !g_veins.empty() || !g_deposits.empty(); }

} // namespace vein_sight

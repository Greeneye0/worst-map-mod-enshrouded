// GG's vein surfaces: for a few seconds, the ore within 10 m of you (the Mining rows that are on) shows through the rock
// as shaded surfaces, beside vein_sight's dotted outlines. The terrain is never changed: when the game next reads the
// voxels of the area around you (marked dirty here) the ore's isosurface is built from them, and a colour-only pass with
// its own pipeline draws it on the finished image, after the upscaler. Included by vein_sight.inl.
// Pinned to one game build: on any other it stays off, and GG still has its outlines.

namespace vein_surface {

using ore_overlay::V;

constexpr uint32_t kGameStamp = 0x6aeb144c, kGameImageSize = 0x2a3f000; // the game build of 2026-11-03
constexpr float kRadius = 10.0f;
constexpr uint64_t kShowMs = 5000, kFadeFromMs = 4000;
constexpr size_t kMaxTriangles = 60000, kMaxChunks = 64;
// What a frame can draw: the upload ring (16 MB, half kept for the game) holds a few frames of it at 96 bytes a triangle.
// A big deposit is merged down to this once captured; over it the ring runs out and frames go without, a flicker.
constexpr size_t kDrawBudget = 12000;
constexpr uint64_t kRebuildMs = 200;
constexpr float kFaceAlpha = 0.24f, kEdgeAlpha = 0.82f; // a surface facing you is glass, seen edge on it is solid
constexpr float kColourScale = 0.60f;                   // the rows' colours, to the scene's brightness
// The Mining rows' voxel materials, kMiningFirst on (tools/resources/make_resources.py ORES)
constexpr uint8_t kRowMaterials[] = {19, 17, 26, 25, 18, 27, 69, 38, 30, 29, 31, 6, 63, 24, 7, 89, 96, 97, 60, 113, 115};
static_assert(std::size(kRowMaterials) == kMiningCount);

constexpr size_t kRvaUploadRange = 0x4e960, kRvaAddPass = 0x6c340, kRvaBufferHandle = 0x1030830;
constexpr size_t kRvaNewCommandBlock = 0xfc6190, kRvaVertexLayout = 0xfe5490, kRvaUpscaled = 0x11a810;
constexpr size_t kRvaUpscaleCall = 0x13d5ab, kRvaUpscale = 0x11a870;
constexpr size_t kRvaMeshReadCall = 0x1d7fc6, kRvaMeshRead = 0x10b20e0;
constexpr size_t kRvaProcessCall = 0x1e67b2, kRvaProcess = 0x1e07c0, kRvaMarkDirty = 0x1e2280;

struct State {
    V centre{};
    bool enabled = false;
    uint64_t revision = 0;
    uint32_t rows = 0; // bit per Mining row
};

// The mesher's voxels of one region: x fastest, four mip levels
struct MeshRegion {
    uint16_t* data;
    uint64_t count;
    uint32_t size[3];
    uint32_t level;
    int32_t first[3];
    uint32_t padding;
};
static_assert(sizeof(MeshRegion) == 48);

struct OreMesh {
    uint64_t epoch;
    std::vector<ore_overlay::Triangle> triangles;
    std::vector<ore_overlay::VertexNormals> normals;
};

std::atomic<bool> g_installed{false};
std::atomic<uint64_t> g_lastUi{0};
SRWLOCK g_stateLock = SRWLOCK_INIT;
State g_state{};
uint64_t g_started = 0;
V g_player{}; // live, for the character's cut-out in the surfaces
bool g_havePlayer = false;

std::mutex g_meshMutex; // g_mesh only, never held for long: the render thread takes it every frame
std::shared_ptr<const OreMesh> g_mesh;
std::mutex g_chunkMutex;
bool g_chunksDirty = false;
uint64_t g_lastRebuild = 0;
std::map<std::array<int32_t, 3>, std::vector<ore_overlay::Triangle>> g_chunks;
uint64_t g_chunkEpoch = 0;

// Checked reads: the renderer's structures are native and this runs on the game's threads
template <class T>
T Read(const void* ptr) {
    T value{};
    SIZE_T n = 0;
    if (!ptr || !ReadProcessMemory(GetCurrentProcess(), ptr, &value, sizeof(value), &n) || n != sizeof(value)) return {};
    return value;
}
template <class T>
T& At(void* p, size_t off) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off);
}
template <class F>
F GameFn(size_t rva) {
    return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + rva);
}

State Snapshot() {
    AcquireSRWLockShared(&g_stateLock);
    State s = g_state;
    const uint64_t started = g_started;
    ReleaseSRWLockShared(&g_stateLock);
    if (GetTickCount64() - started >= kShowMs) s.enabled = false;
    return s;
}

void Stop() {
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.enabled = false;
    ReleaseSRWLockExclusive(&g_stateLock);
}

void RebuildIfDue(const State& s);

// Every UI frame: off the play screen (a menu, the map, a loading screen) the surfaces go
void Tick(uint8_t* client) {
    g_lastUi.store(GetTickCount64(), std::memory_order_release);
    if (!client || client[0x1cd8] != 1) {
        Stop();
        return;
    }
    if (!Snapshot().enabled) return;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    const bool have = GetHudPlayerWorldPos(client, x, z) && LocalPlayerY(client, y);
    AcquireSRWLockExclusive(&g_stateLock);
    g_player = {x, y, z};
    g_havePlayer = have;
    ReleaseSRWLockExclusive(&g_stateLock);
    RebuildIfDue(Snapshot()); // the last regions read, when no more come to do it
}

// The render and mesher threads go on while the UI stops (loading): the surfaces stop with it
void CheckUiFresh() {
    const uint64_t stamp = g_lastUi.load(std::memory_order_acquire);
    if (!stamp || GetTickCount64() - stamp > 500) Stop();
}

// GG, at the player's feet
void Start(uint8_t* client) {
    if (!g_installed.load()) return;
    uint32_t rows = 0;
    for (size_t r = 0; r < kMiningCount; ++r) {
        if (g_oreEnabled[kMiningFirst + r]) rows |= 1u << r;
    }
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!rows || !GetHudPlayerWorldPos(client, x, z) || !LocalPlayerY(client, y)) return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.centre = {x, y, z};
    g_state.rows = rows;
    g_state.enabled = true;
    ++g_state.revision;
    g_started = GetTickCount64();
    ReleaseSRWLockExclusive(&g_stateLock);
    if (g_log) g_log("Vein surfaces: GG at %.1f %.1f %.1f, rows %06x", x, y, z, rows);
}


uint32_t RowColour(size_t miningRow); // vein_sight.inl

// The ore of the rows that are on in one region, each material's surface on its own
bool ExtractRows(const ore_overlay::Grid& grid, const State& s, std::vector<ore_overlay::Triangle>& output, bool* overBudget) {
    if (overBudget) *overBudget = false;
    if (!grid.samples || !std::isfinite(grid.step) || grid.step <= 0.0f || grid.step > 1.0f) return false;
    size_t count = 1;
    for (unsigned n : grid.n) {
        if (n < 2 || n > 128) return false;
        count *= n;
    }
    if (count != grid.count) return false;
    bool present[256]{};
    for (size_t i = 0; i < count; ++i) {
        const uint16_t v = grid.samples[i];
        if ((v >> 8) >= 128) present[v & 255] = true;
    }
    std::vector<ore_overlay::Triangle> combined;
    for (size_t r = 0; r < kMiningCount; ++r) {
        if (!(s.rows & (1u << r)) || !present[kRowMaterials[r]]) continue;
        std::vector<ore_overlay::Triangle> part;
        if (!ore_overlay::Extract(grid, kRowMaterials[r], s.centre, kRadius, part, kMaxTriangles - combined.size(), overBudget)) {
            return false;
        }
        const uint32_t colour = RowColour(r);
        for (auto& t : part) t.colour = colour;
        combined.insert(combined.end(), part.begin(), part.end());
    }
    output.swap(combined);
    return true;
}

// Merges the corners in each cell of a grid into one, coarser until the mesh fits a frame's upload. One grid over the
// world, so chunks still meet; an ore's corners only with its own, so thin veins next to a big one stay.
void MergeToBudget(std::vector<ore_overlay::Triangle>& triangles) {
    using ore_overlay::Triangle;
    using CellKey = std::array<int32_t, 4>;
    for (float cell = 1.0f; triangles.size() > kDrawBudget && cell <= 4.0f; cell += 0.5f) {
        struct Sum {
            V sum{};
            int count = 0;
            uint32_t id = 0;
        };
        std::map<CellKey, Sum> cells;
        auto keyOf = [&](V p, uint32_t colour) {
            return CellKey{static_cast<int32_t>(std::floor(p.x / cell)), static_cast<int32_t>(std::floor(p.y / cell)),
                           static_cast<int32_t>(std::floor(p.z / cell)), static_cast<int32_t>(colour)};
        };
        for (const Triangle& t : triangles) {
            for (const V& p : t.p) {
                Sum& c = cells[keyOf(p, t.colour)];
                c.sum = c.sum + p;
                ++c.count;
            }
        }
        uint32_t nextId = 0;
        for (auto& c : cells) c.second.id = nextId++;
        std::vector<std::array<uint32_t, 3>> seen;
        seen.reserve(triangles.size());
        std::vector<Triangle> merged;
        merged.reserve(triangles.size() / 2);
        for (const Triangle& t : triangles) {
            const Sum* corner[3];
            for (int i = 0; i < 3; ++i) corner[i] = &cells[keyOf(t.p[i], t.colour)];
            if (corner[0] == corner[1] || corner[1] == corner[2] || corner[2] == corner[0]) continue;
            Triangle out{};
            for (int i = 0; i < 3; ++i) out.p[i] = corner[i]->sum * (1.0f / static_cast<float>(corner[i]->count));
            V normal = ore_overlay::Cross(out.p[1] - out.p[0], out.p[2] - out.p[0]);
            const float length = std::sqrt(ore_overlay::Dot(normal, normal));
            if (length < 1e-6f) continue;
            normal = normal * (1.0f / length);
            if (ore_overlay::Dot(normal, t.normal) < 0.0f) {
                std::swap(out.p[1], out.p[2]);
                normal = normal * -1.0f;
            }
            out.normal = normal;
            out.colour = t.colour;
            std::array<uint32_t, 3> ids{corner[0]->id, corner[1]->id, corner[2]->id};
            std::sort(ids.begin(), ids.end());
            seen.push_back(ids);
            merged.push_back(out);
        }
        // The same three cells twice: a fold flattened, one is enough
        std::vector<uint32_t> byIds(merged.size());
        for (uint32_t i = 0; i < byIds.size(); ++i) byIds[i] = i;
        std::sort(byIds.begin(), byIds.end(), [&](uint32_t a, uint32_t b) { return seen[a] < seen[b]; });
        std::vector<Triangle> unique;
        unique.reserve(merged.size());
        for (size_t i = 0; i < byIds.size(); ++i) {
            if (i == 0 || seen[byIds[i]] != seen[byIds[i - 1]]) unique.push_back(merged[byIds[i]]);
        }
        triangles.swap(unique);
    }
}

// Whole chunks, nearest first, up to the capture budget, merged to what a frame can draw. On the mesher's or the UI's
// thread, at most every kRebuildMs: regions come in dozens, and each rebuild is milliseconds of that thread.
void RebuildMesh(const State& s) {
    auto mesh = std::make_shared<OreMesh>();
    mesh->epoch = s.revision;
    struct Candidate {
        const std::vector<ore_overlay::Triangle>* triangles;
        float distance;
    };
    std::vector<Candidate> candidates;
    for (const auto& c : g_chunks) {
        float distance = std::numeric_limits<float>::max();
        for (const auto& t : c.second) {
            for (const V& p : t.p) {
                const V d = p - s.centre;
                distance = std::min(distance, ore_overlay::Dot(d, d));
            }
        }
        candidates.push_back({&c.second, distance});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    for (const auto& c : candidates) {
        if (c.triangles->size() > kMaxTriangles - mesh->triangles.size()) continue;
        mesh->triangles.insert(mesh->triangles.end(), c.triangles->begin(), c.triangles->end());
    }
    MergeToBudget(mesh->triangles);
    mesh->normals = ore_overlay::SmoothNormals(mesh->triangles);
    std::lock_guard<std::mutex> guard(g_meshMutex);
    g_mesh = mesh;
}

void RebuildIfDue(const State& s) {
    std::unique_lock<std::mutex> guard(g_chunkMutex, std::try_to_lock);
    if (!guard.owns_lock() || !g_chunksDirty || g_chunkEpoch != s.revision || !s.enabled) return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastRebuild < kRebuildMs) return;
    g_lastRebuild = now;
    g_chunksDirty = false;
    RebuildMesh(s);
}

// --- Capture: the mesher's reads of the voxels (read only) ---

struct RegionSpan {
    MeshRegion* data;
    uint64_t count;
};
using MeshReadFn = uint8_t (*)(RegionSpan*, void*);
MeshReadFn g_origMeshRead = nullptr;
using ProcessFn = void (*)(uint8_t*);
ProcessFn g_origProcess = nullptr;
using MarkDirtyFn = void (*)(uint8_t*, uint8_t*, void*);
MarkDirtyFn g_markDirty = nullptr;

void* g_source = nullptr; // the world's voxel source, the one at the origin
std::mutex g_sourceMutex;
uint64_t g_markedEpoch = 0;

uint8_t MeshReadHook(RegionSpan* regions, void* source) {
    const uint8_t result = g_origMeshRead(regions, source);
    greeneye::Lease lease;
    if (!lease || !result) return result;
    const State s = Snapshot();
    if (!s.enabled) return result;
    {
        std::lock_guard<std::mutex> guard(g_sourceMutex);
        if (source != g_source) return result;
    }
    if (!regions || !regions->data || regions->count > 4) return result;
    for (uint64_t i = 0; i < regions->count; ++i) {
        const MeshRegion& r = regions->data[i];
        if (r.level != 6) continue; // the finest, half a metre: coarser ones would stack surfaces
        ore_overlay::Grid grid{r.data,
                               static_cast<size_t>(r.count),
                               {r.size[0], r.size[1], r.size[2]},
                               {r.first[0] * 0.5f, r.first[1] * 0.5f, r.first[2] * 0.5f},
                               0.5f};
        std::vector<ore_overlay::Triangle> triangles;
        bool overBudget = false;
        if (!ExtractRows(grid, s, triangles, &overBudget)) {
            if (g_log) g_log("Vein surfaces: region left out (%s)", overBudget ? "too many triangles" : "not a voxel grid");
            continue;
        }
        std::unique_lock<std::mutex> guard(g_chunkMutex);
        const State now = Snapshot();
        if (now.revision != s.revision || !now.enabled) continue;
        if (g_chunkEpoch != s.revision) {
            g_chunks.clear();
            g_chunkEpoch = s.revision;
        }
        const std::array<int32_t, 3> key{r.first[0], r.first[1], r.first[2]};
        if (triangles.empty()) {
            if (!g_chunks.erase(key)) continue; // an empty one only removes a chunk mined out since
        } else {
            if (g_chunks.size() >= kMaxChunks && g_chunks.find(key) == g_chunks.end()) continue;
            g_chunks[key] = std::move(triangles);
        }
        g_chunksDirty = true;
        guard.unlock();
        RebuildIfDue(s);
    }
    return result;
}

void ProcessHook(uint8_t* owner) {
    g_origProcess(owner);
    greeneye::Lease lease;
    if (!lease || g_modOff) return;
    auto* source = Read<uint8_t*>(owner + 0x3c8);
    if (!source) return;
    const auto origin = Read<std::array<float, 3>>(source + 0x30);
    if (origin[0] != 0.0f || origin[1] != 0.0f || origin[2] != 0.0f) return;
    {
        std::lock_guard<std::mutex> guard(g_sourceMutex);
        if (source != g_source) {
            g_source = source;
            g_markedEpoch = 0;
        }
    }
    const State s = Snapshot();
    if (!s.enabled) return;
    CheckUiFresh();
    if (g_markedEpoch == s.revision) return;
    // Once per GG: the area around you is read again, and MeshReadHook sees its voxels
    struct alignas(16) Box {
        float lo[4], hi[4];
        uint8_t valid, pad[15];
    };
    const float reach = kRadius + 2.0f;
    Box box{{s.centre.x - reach, s.centre.y - reach, s.centre.z - reach, 0.0f},
            {s.centre.x + reach, s.centre.y + reach, s.centre.z + reach, 0.0f},
            1,
            {}};
    auto* nodes = Read<uint8_t*>(owner + 0x390);
    const auto count = Read<uint64_t>(owner + 0x398);
    if (!nodes || count > 65536) return;
    for (uint64_t i = 0; i < count; ++i) g_markDirty(owner, nodes + i * 0x68, &box);
    g_markedEpoch = s.revision;
}

// --- The colour-only pass: no terrain, visibility or depth writes ---

struct Image {
    uint64_t context, id;
};
struct Span {
    const void* data;
    uint64_t count;
};

void* g_graphics = nullptr;
void* g_pipeline = nullptr;
std::atomic<bool> g_failed{false};

using UpscaleFn = void (*)(void*, uint8_t*, uint8_t*);
UpscaleFn g_origUpscale = nullptr;

template <class F>
F Method(void* p, size_t offset) {
    return At<F>(At<void*>(p, 0), offset);
}

// First use, on the render thread. One pipeline a generation, on one graphics device (a new device: until a restart).
bool CreatePipeline(const Image& image) {
    auto* g = Read<void*>(reinterpret_cast<void*>(image.context + 8));
    if (!g) return false;
    if (g_pipeline && g_graphics == g) return true;
    if (g_graphics && g_graphics != g) return false;
    g_graphics = g;
    auto* device = Read<void*>(static_cast<uint8_t*>(g) + 0x278);
    if (!device) return false;
    alignas(8) uint8_t layoutDesc[0x30]{};
    void* layout = Method<void* (*)(void*, const void*)>(device, 0x38)(device, layoutDesc);
    if (!layout) return false;
    // Two vec4s: clip position; position from the centre with the packed normal and opacity
    const uint8_t attributes[] = {0, 3, 1, 3};
    const Span span{attributes, 2};
    void* vertices = GameFn<void* (*)(void*, const Span*)>(kRvaVertexLayout)(g, &span);
    if (!vertices || Read<uint8_t>(static_cast<uint8_t*>(vertices) + 0x22) != 32) return false;
    alignas(8) uint8_t desc[0x118]{};
    At<const void*>(desc, 0x08) = ore_overlay::vert;
    At<uint64_t>(desc, 0x10) = sizeof(ore_overlay::vert);
    At<const void*>(desc, 0x48) = ore_overlay::frag;
    At<uint64_t>(desc, 0x50) = sizeof(ore_overlay::frag);
    At<void*>(desc, 0x58) = layout;
    At<void*>(desc, 0x60) = vertices;
    At<uint32_t>(desc, 0x68) = 0x01000000; // triangle list
    desc[0x6d] = 0x1f;
    for (int i = 0; i < 7; ++i) desc[0x79 + i * 8] = 0xf;
    desc[0x78] = 0x61;
    const uint8_t blend[] = {1, 1, 5, 1, 1, 5}; // premultiplied alpha over
    std::memcpy(desc + 0x7a, blend, sizeof(blend));
    At<uint32_t>(desc, 0xb0) = 0xff00;
    At<uint32_t>(desc, 0xb4) = 7;
    At<uint32_t>(desc, 0xb8) = 0x070000ff;
    At<uint32_t>(desc, 0xbc) = 0x01000000;
    At<uint32_t>(desc, 0xc0) = 0x00070000;
    const char* names[] = {"main", "tcs_main", "tes_main", "ms_main", "main"};
    for (int i = 0; i < 5; ++i) {
        At<const char*>(desc, 0xc8 + i * 16) = names[i];
        At<uint64_t>(desc, 0xd0 + i * 16) = std::strlen(names[i]);
    }
    g_pipeline = Method<void* (*)(void*, const void*)>(device, 0x40)(device, desc);
    return g_pipeline != nullptr;
}

uint8_t* QueueCommand(void* pass, uint8_t op, uint16_t size) {
    auto* commands = At<uint8_t*>(pass, 0x10);
    auto* block = At<uint8_t*>(commands, 0x28);
    if (At<uint64_t>(block, 0x10) - At<uint64_t>(block, 8) < size) {
        auto* fresh = GameFn<uint8_t* (*)(void*)>(kRvaNewCommandBlock)(At<void*>(commands, 0));
        At<void*>(block, 0) = fresh;
        At<void*>(commands, 0x28) = fresh;
        block = fresh;
    }
    auto* out = At<uint8_t*>(block, 8);
    At<uint8_t*>(block, 8) += size;
    ++At<uint64_t>(block, 0x18);
    std::memset(out, 0, size);
    At<uint16_t>(out, 0) = size;
    out[2] = op;
    return out;
}

// The upload ring shared with the game's later passes: enough left for this after a reserve for them, or this frame
// goes without (an allocation failing in there is a crash in the game's renderer)
bool UploadRoom(void* scope, uint64_t amount, bool wholeFrame) {
    auto* a = Read<uint8_t*>(static_cast<uint8_t*>(scope) + 0x40);
    if (!a) return false;
    const auto index = Read<uint64_t>(a + 0xd8), count = Read<uint64_t>(a + 0x20);
    auto* rings = Read<uint8_t*>(a + 0x18);
    if (!rings || index >= count || count > 1024) return false;
    auto* ring = rings + index * 0x98;
    const uint64_t alignment = std::max<uint64_t>(16, std::max(Read<uint64_t>(a + 0x168), Read<uint64_t>(a + 0xf0 + 6 * 8)));
    const uint64_t capacity = Read<uint64_t>(ring + 0x70), head = Read<uint64_t>(ring + 0x60), live = Read<uint64_t>(ring + 0x68);
    constexpr uint64_t kReserve = 8ull * 1024 * 1024;
    return wholeFrame ? ore_upload::HasBatchRoom(capacity, head, live, amount, alignment, kReserve)
                      : ore_upload::HasRoom(capacity, head, live, amount, alignment, kReserve);
}

// World to clip coordinates, the way the pass draws them
void ToClip(const float* view, const float* proj, V p, float* out) {
    const double v[] = {p.x * static_cast<double>(view[0]) + p.y * static_cast<double>(view[4]) + p.z * static_cast<double>(view[8]) + view[12],
                        p.x * static_cast<double>(view[1]) + p.y * static_cast<double>(view[5]) + p.z * static_cast<double>(view[9]) + view[13],
                        p.x * static_cast<double>(view[2]) + p.y * static_cast<double>(view[6]) + p.z * static_cast<double>(view[10]) + view[14],
                        1.0};
    for (int r = 0; r < 4; ++r) {
        double c = 0.0;
        for (int k = 0; k < 4; ++k) c += proj[k * 4 + r] * v[k];
        out[r] = static_cast<float>(c);
    }
}

// The character's cut-out: the pass has no depth to hide behind (it has to show through rock), so the surfaces leave out
// a capsule around the player, on screen, whether the ore is nearer than the character or not. Off in first person.
constexpr float kMaskFeet = -0.15f, kMaskHead = 1.95f, kMaskRadius = 0.42f, kMaskSoft = 0.45f, kMaskMinCamera = 1.2f;
constexpr float kTriangleReach = 0.7f; // a triangle's corners from its middle at most, in metres (half metre voxels)
constexpr float kMaskFinePixels = 16.0f; // the opacity runs across a piece: no need for fine ones
constexpr int kMaskMaxSplits = 3;

struct PlayerMask {
    bool on = false;
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f, radius = 0.0f, soft = 0.0f;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    float halfW = 0.0f, halfH = 0.0f;

    float Distance(float x, float y) const {
        const float dx = bx - ax, dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        const float u = len2 > 0.0f ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
        const float ex = ax + dx * u - x, ey = ay + dy * u - y;
        return std::sqrt(ex * ex + ey * ey);
    }
    bool Inside(float x, float y) const { return Distance(x, y) <= radius; }
    // 0 in the capsule, 1 outside its soft edge
    float Opacity(float x, float y) const {
        const float u = std::clamp((Distance(x, y) - radius) / soft, 0.0f, 1.0f);
        return u * u * (3.0f - 2.0f * u);
    }
};

PlayerMask MakeMask(const float* view, const float* proj, V player, uint32_t width, uint32_t height) {
    PlayerMask m;
    m.halfW = width * 0.5f;
    m.halfH = height * 0.5f;
    float feet[4], head[4];
    ToClip(view, proj, {player.x, player.y + kMaskFeet, player.z}, feet);
    ToClip(view, proj, {player.x, player.y + kMaskHead, player.z}, head);
    if (feet[3] < kMaskMinCamera || head[3] < kMaskMinCamera) return m;
    m.ax = feet[0] / feet[3] * m.halfW;
    m.ay = feet[1] / feet[3] * m.halfH;
    m.bx = head[0] / head[3] * m.halfW;
    m.by = head[1] / head[3] * m.halfH;
    m.radius = kMaskRadius * std::fabs(proj[0]) / std::min(feet[3], head[3]) * m.halfW;
    m.soft = m.radius * (kMaskSoft / kMaskRadius);
    const float reach = m.radius + m.soft;
    m.minX = std::min(m.ax, m.bx) - reach;
    m.maxX = std::max(m.ax, m.bx) + reach;
    m.minY = std::min(m.ay, m.by) - reach;
    m.maxY = std::max(m.ay, m.by) + reach;
    m.on = true;
    return m;
}

struct Piece {
    V p[3], n[3];
    uint32_t colour;
};

struct Corner {
    V p, n;
    float x, y;
    bool front;
};

Corner MakeCorner(const float* view, const float* proj, const PlayerMask& m, V p, V n) {
    float c[4];
    ToClip(view, proj, p, c);
    Corner out{p, n, 0.0f, 0.0f, c[3] > 0.05f};
    if (out.front) {
        out.x = c[0] / c[3] * m.halfW;
        out.y = c[1] / c[3] * m.halfH;
    }
    return out;
}

// What is left of a triangle outside the cut-out, split finer along its edge
void AddOutsideMask(const float* view, const float* proj, const PlayerMask& m, const Corner& a, const Corner& b, const Corner& c,
                    int splits, uint32_t colour, std::vector<Piece>& out) {
    auto keep = [&] { out.push_back({{a.p, b.p, c.p}, {a.n, b.n, c.n}, colour}); };
    if (!a.front || !b.front || !c.front) return keep(); // reaches behind the camera: the character isn't in it
    if (std::max({a.x, b.x, c.x}) < m.minX || std::min({a.x, b.x, c.x}) > m.maxX ||
        std::max({a.y, b.y, c.y}) < m.minY || std::min({a.y, b.y, c.y}) > m.maxY) {
        return keep();
    }
    if (m.Inside(a.x, a.y) && m.Inside(b.x, b.y) && m.Inside(c.x, c.y)) return; // the capsule is convex: all of it inside
    const float spanX = std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x});
    const float spanY = std::max({a.y, b.y, c.y}) - std::min({a.y, b.y, c.y});
    if (splits == 0 || std::max(spanX, spanY) <= kMaskFinePixels) {
        if (!m.Inside((a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f)) keep();
        return;
    }
    auto middle = [&](const Corner& u, const Corner& v) {
        return MakeCorner(view, proj, m, (u.p + v.p) * 0.5f, (u.n + v.n) * 0.5f);
    };
    const Corner ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
    AddOutsideMask(view, proj, m, a, ab, ca, splits - 1, colour, out);
    AddOutsideMask(view, proj, m, ab, b, bc, splits - 1, colour, out);
    AddOutsideMask(view, proj, m, ca, bc, c, splits - 1, colour, out);
    AddOutsideMask(view, proj, m, ab, bc, ca, splits - 1, colour, out);
}

void DrawPass(void* scope, uint8_t* state) {
    const State s = Snapshot();
    if (!s.enabled) return;
    // Like the game's own caller: the finished upscaled image, or the scene colour with upscaling off. Never into history.
    const bool upscaled = GameFn<bool (*)(void*)>(kRvaUpscaled)(state + 0xf38);
    const auto image = Read<Image>(state + (upscaled ? 0x180 : 0x140));
    if (!image.context || !image.id) return;
    auto* table = Read<uint8_t*>(reinterpret_cast<void*>(image.context + 0x190));
    if (!table) return;
    auto* imageDesc = table + (image.id & 0xffff) * 0x30;
    const auto width = Read<uint32_t>(imageDesc + 4), height = Read<uint32_t>(imageDesc + 8);
    if (Read<uint8_t>(imageDesc + 0x15) != 0x61 || !width || !height || width > 16384 || height > 16384) return;
    if (!g_pipeline) {
        if (!CreatePipeline(image)) {
            g_failed = true;
            if (g_log) g_log("Vein surfaces: no pipeline, off until a restart");
        }
        return;
    }
    std::shared_ptr<const OreMesh> mesh;
    {
        std::lock_guard<std::mutex> guard(g_meshMutex);
        mesh = g_mesh;
    }
    if (!mesh || mesh->epoch != s.revision || mesh->triangles.empty() || mesh->normals.size() != mesh->triangles.size()) return;
    float view[16], proj[16];
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), state + 0x1000, view, sizeof(view), &copied) || copied != sizeof(view) ||
        !ReadProcessMemory(GetCurrentProcess(), state + 0x10c0, proj, sizeof(proj), &copied) || copied != sizeof(proj)) {
        return;
    }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(view[i]) || !std::isfinite(proj[i])) return;
    }
    ore_overlay::StableProjection(proj);
    // Far to near; the camera moves every frame, the mesh doesn't
    struct Sort {
        size_t index;
        float z;
    };
    std::vector<Sort> order;
    order.reserve(mesh->triangles.size());
    auto viewZ = [&](V p) { return p.x * view[2] + p.y * view[6] + p.z * view[10] + view[14]; };
    for (size_t i = 0; i < mesh->triangles.size(); ++i) {
        const auto& t = mesh->triangles[i];
        if (!ore_overlay::WithinReveal(t, s.centre, kRadius) || !ore_overlay::FrontFacing(t, view)) continue;
        order.push_back({i, viewZ((t.p[0] + t.p[1] + t.p[2]) * (1.0f / 3.0f))});
    }
    std::stable_sort(order.begin(), order.end(), [](const Sort& a, const Sort& b) { return a.z > b.z; });
    if (order.empty()) return;

    AcquireSRWLockShared(&g_stateLock);
    const uint64_t age = GetTickCount64() - g_started;
    const V player = g_player;
    const bool havePlayer = g_havePlayer;
    ReleaseSRWLockShared(&g_stateLock);

    // In draw order: a whole triangle of the mesh, or the pieces of one around the character
    constexpr uint32_t kWhole = 0xffffffffu;
    struct Item {
        uint32_t index, piece;
    };
    std::vector<Item> items;
    std::vector<Piece> pieces;
    items.reserve(order.size());
    PlayerMask mask;
    if (havePlayer) mask = MakeMask(view, proj, player, width, height);
    const float pixelsPerMetre = std::fabs(proj[0]) * width * 0.5f;
    for (const Sort& sorted : order) {
        const uint32_t index = static_cast<uint32_t>(sorted.index);
        const auto& t = mesh->triangles[sorted.index];
        bool closeBy = false;
        if (mask.on) {
            float c[4];
            ToClip(view, proj, (t.p[0] + t.p[1] + t.p[2]) * (1.0f / 3.0f), c);
            if (c[3] <= 0.05f) {
                closeBy = false;
            } else {
                const float x = c[0] / c[3] * mask.halfW, y = c[1] / c[3] * mask.halfH;
                const float margin = pixelsPerMetre / c[3] * kTriangleReach;
                closeBy = x >= mask.minX - margin && x <= mask.maxX + margin && y >= mask.minY - margin && y <= mask.maxY + margin;
            }
        }
        if (!closeBy) {
            items.push_back({index, kWhole});
            continue;
        }
        const auto& n = mesh->normals[sorted.index];
        const size_t first = pieces.size();
        AddOutsideMask(view, proj, mask, MakeCorner(view, proj, mask, t.p[0], n[0]), MakeCorner(view, proj, mask, t.p[1], n[1]),
                       MakeCorner(view, proj, mask, t.p[2], n[2]), kMaskMaxSplits, t.colour, pieces);
        for (size_t i = first; i < pieces.size(); ++i) items.push_back({index, static_cast<uint32_t>(i)});
    }
    if (items.empty() || !UploadRoom(scope, items.size(), true)) return;

    float fade = 1.0f;
    if (age > kFadeFromMs) {
        fade = std::clamp(static_cast<float>(kShowMs - std::min(age, kShowMs)) / static_cast<float>(kShowMs - kFadeFromMs), 0.0f, 1.0f);
    }

    const uint8_t attachment[] = {1, 1, 0};
    GameFn<void (*)(void*, const void*, const void*, const char*)>(kRvaAddPass)(scope, &image, attachment, "ore surfaces");
    At<void*>(QueueCommand(scope, 5, 0x10), 8) = g_pipeline;
    auto* allocator = Read<uint8_t*>(static_cast<uint8_t*>(scope) + 0x40);
    const uint64_t chunkBytes = allocator ? Read<uint64_t>(allocator + 0x10) : 0;
    const V camera{-(view[0] * view[12] + view[1] * view[13] + view[2] * view[14]),
                   -(view[4] * view[12] + view[5] * view[13] + view[6] * view[14]),
                   -(view[8] * view[12] + view[9] * view[13] + view[10] * view[14])};
    ore_upload::NativeRange range{};
    const size_t drawn = ore_upload::Batches(
        items.size(), chunkBytes,
        [&](size_t count) -> float* {
            if (!UploadRoom(scope, count * 96, false)) return nullptr;
            // Headroom in the handle table the game's later passes share
            auto* graphOwner = Read<uint8_t*>(scope);
            auto* graph = graphOwner ? Read<uint8_t*>(graphOwner) : nullptr;
            if (!graph) return nullptr;
            const auto used = Read<uint64_t>(graph + 0x2e0), capacity = Read<uint64_t>(graph + 0x2e8);
            if (used >= capacity || capacity - used < 128) return nullptr;
            alignas(8) uint32_t options[4]{};
            range = {};
            GameFn<void* (*)(void*, void*, uint8_t, uint64_t, const void*)>(kRvaUploadRange)(scope, &range, 6, count * 3 * 32, options);
            return ore_upload::Mapped(range);
        },
        [&](float* mapped, size_t begin, size_t count) {
            auto pack = [](float f) {
                return static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(std::lround(std::clamp(f, -1.0f, 1.0f) * 127.0f))));
            };
            for (size_t i = begin; i < begin + count; ++i) {
                const Item& item = items[i];
                const bool whole = item.piece == kWhole;
                const V* corners = whole ? mesh->triangles[item.index].p : pieces[item.piece].p;
                const V* normals = whole ? mesh->normals[item.index].data() : pieces[item.piece].n;
                const uint32_t abgr = whole ? mesh->triangles[item.index].colour : pieces[item.piece].colour;
                // 0x3f up top: the bits are never a float's NaN on their way through
                uint32_t rgb = 0x3f000000u;
                for (int channel = 0; channel < 3; ++channel) {
                    const float c = static_cast<float>((abgr >> (channel * 8)) & 255u) / 255.0f;
                    rgb |= static_cast<uint32_t>(std::lround(std::clamp(c * c * kColourScale, 0.0f, 1.0f) * 255.0f)) << (channel * 8);
                }
                for (int vertex = 0; vertex < 3; ++vertex) {
                    const V p = corners[vertex];
                    const V normal = normals[vertex];
                    float clip[4]; // not read back from mapped: upload memory is very slow to read
                    ToClip(view, proj, p, clip);
                    // Glass facing you, solid edge on: the vein's shape, not a skin over the rock
                    V toCamera = camera - p;
                    const float distance = std::sqrt(ore_overlay::Dot(toCamera, toCamera));
                    const float facing = distance > 0.001f ? std::fabs(ore_overlay::Dot(normal, toCamera)) / distance : 1.0f;
                    float alpha = fade * (kFaceAlpha + (kEdgeAlpha - kFaceAlpha) * (1.0f - facing) * (1.0f - facing));
                    if (mask.on && clip[3] > 0.05f) {
                        alpha *= mask.Opacity(clip[0] / clip[3] * mask.halfW, clip[1] / clip[3] * mask.halfH);
                    }
                    std::memcpy(&clip[2], &rgb, 4);
                    std::memcpy(mapped, clip, sizeof(clip));
                    mapped[4] = p.x - s.centre.x;
                    mapped[5] = p.y - s.centre.y;
                    mapped[6] = p.z - s.centre.z;
                    const uint32_t packed = pack(normal.x) | (pack(normal.y) << 8) | (pack(normal.z) << 16) | (pack(alpha) << 24);
                    std::memcpy(mapped + 7, &packed, 4);
                    mapped += 8;
                }
            }
        },
        [&](size_t count) {
            auto* bind = QueueCommand(scope, 9, 0x20);
            At<uint64_t>(bind, 8) = GameFn<uint64_t (*)(const void*)>(kRvaBufferHandle)(&range);
            At<uint64_t>(bind, 0x10) = range.offset;
            At<uint64_t>(bind, 0x18) = range.bytes;
            auto* draw = QueueCommand(scope, 0xd, 0x18);
            At<uint32_t>(draw, 4) = static_cast<uint32_t>(count * 3);
            At<uint32_t>(draw, 8) = 1;
        });
    static uint64_t loggedRevision = 0;
    if (loggedRevision != s.revision) {
        loggedRevision = s.revision;
        if (g_log) g_log("Vein surfaces: drawing %llu of %llu triangles", static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(items.size()));
    }
}

void UpscaleHook(void* scope, uint8_t* upscalers, uint8_t* inputs) {
    g_origUpscale(scope, upscalers, inputs);
    greeneye::Lease lease;
    if (!lease || g_modOff || g_failed.load()) return;
    CheckUiFresh();
    DrawPass(scope, upscalers - 0xf38);
}

bool CallTargets(const GreeneyeHost* h, uint8_t* base, size_t rva, size_t target) {
    uint8_t call[5]{};
    h->read(base + rva, call, sizeof(call));
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    return call[0] == 0xe8 && static_cast<int64_t>(rva) + 5 + rel == static_cast<int64_t>(target);
}

// Each hook goes through the loader, so Town Portal can share the upscale call
void Install() {
    if (!GetPrivateProfileIntA("General", "GGVeinSurfaces", 1, greeneye::DataPath("minimap_config.ini"))) return;
    const GreeneyeHost* h = greeneye::Host();
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (!h || !h->read || !h->redirectCall || nt->FileHeader.TimeDateStamp != kGameStamp || nt->OptionalHeader.SizeOfImage != kGameImageSize) {
        if (g_log) g_log("Vein surfaces: not for this game build; GG keeps its outlines");
        return;
    }
    struct Guard {
        size_t rva;
        uint8_t bytes[16];
    };
    static const Guard kGuards[] = {
        {kRvaUpscaled, {0x80, 0x39, 0x00, 0x74, 0x37, 0x48, 0x83, 0x79, 0x08, 0x00, 0x74, 0x06, 0x80, 0x79, 0x28, 0x00}},
        {kRvaUploadRange, {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x53, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x84}},
        {kRvaBufferHandle, {0x48, 0x8b, 0x01, 0x48, 0x85, 0xc0, 0x74, 0x10, 0x0f, 0xb7, 0x49, 0x08, 0x48, 0x8b, 0x80, 0x68}},
        {kRvaAddPass, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x55}},
        {kRvaNewCommandBlock, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x99, 0x10, 0x07, 0x00}},
        {kRvaVertexLayout, {0x48, 0x8b, 0xc4, 0x53, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x4a, 0x08, 0x48}},
    };
    for (const Guard& g : kGuards) {
        uint8_t bytes[16];
        h->read(base + g.rva, bytes, sizeof(bytes));
        if (std::memcmp(bytes, g.bytes, sizeof(bytes)) != 0) {
            if (g_log) g_log("Vein surfaces: the game's code at %llx differs; off", static_cast<unsigned long long>(g.rva));
            return;
        }
    }
    if (!CallTargets(h, base, kRvaMeshReadCall, kRvaMeshRead) || !CallTargets(h, base, kRvaProcessCall, kRvaProcess) ||
        !CallTargets(h, base, kRvaUpscaleCall, kRvaUpscale)) {
        if (g_log) g_log("Vein surfaces: a call site differs; off");
        return;
    }
    g_markDirty = reinterpret_cast<MarkDirtyFn>(base + kRvaMarkDirty);
    g_origMeshRead = reinterpret_cast<MeshReadFn>(h->redirectCall(base + kRvaMeshReadCall, reinterpret_cast<void*>(&MeshReadHook)));
    g_origProcess = reinterpret_cast<ProcessFn>(h->redirectCall(base + kRvaProcessCall, reinterpret_cast<void*>(&ProcessHook)));
    g_origUpscale = reinterpret_cast<UpscaleFn>(h->redirectCall(base + kRvaUpscaleCall, reinterpret_cast<void*>(&UpscaleHook)));
    g_installed = g_origMeshRead && g_origProcess && g_origUpscale;
    if (g_log) g_log("Vein surfaces: %s", g_installed.load() ? "ready" : "hooks failed; off");
}

} // namespace vein_surface


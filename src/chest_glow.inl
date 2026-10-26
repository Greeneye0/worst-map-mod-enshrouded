// GG: everything near the player that gets the minimap's height arrow (75 m) and is not done yet - gold and silver
// chests not looted, lore notes not read, bosses not defeated, each while its row is on - lights up for a while: the
// game's outline highlight where it is in view, its silhouette on screen through walls and ground (Find's,
// chest_silhouette.inl; a ring with its icon where it has no model), and pulses on the map and minimap. Silver chests
// glow silver; the rest in Find's gold.
//
// The glow is the game's own outline highlight, the way Find lights the chests it finds and Quick Deposit the chests
// it filled (QuickDeposit/src/glow.cpp, trimmed from FindMod/src/nativehighlight.cpp): the render systems
// "dynamic_render_model" and "static_render_model" walk every model with the ECS iterator 'next'; their calls to it
// are redirected, and when an iteration ends the chests to light get highlight group 5 on their render instance
// through the renderer's own setter (renderer, span of model handles, span of colour bytes), and chests whose time is
// up get their old group back, unless something else (the game's interaction highlight, Quick Deposit's group 6) has
// set another since. Group 7 (Find's; groups 4 and 5 draw no outline): its colour is Find's gold while Find lights
// something, else the game's orange. The outline is hidden by walls like the chest, hence the screen markers
// (DrawMarkers: the client's view / projection matrices at +0x1d20 / +0x1d60 and the UI window's viewport, as Find's
// projection).

namespace chest_glow {

constexpr unsigned kMax = 64;
constexpr uint8_t kGroup = 7; // HighlightColorIndex Group4: the one of the free groups the outline draws
constexpr uint32_t kGlowMs = 14000;  // as long as the map's pulses
constexpr uint32_t kFindGold = 0xff6ecdffu, kSilver = 0xffe8e4dcu; // 0xAABBGGRR
constexpr float kNearMeters = 75.0f; // the chests' height-arrow range

struct Span {
    const void* data;
    uint64_t count;
};
struct Field {
    const char* text;
    uint64_t length;
};
struct System {
    uint8_t* fn;
    unsigned reads, writes, modelSlot;
};
struct Target {
    uint32_t entity;
    float position[3];
    uint64_t until;
    uint32_t colour;
};
struct Applied {
    void* renderer;
    void* registry;
    uint8_t* instance;
    uint32_t handle;
    uint8_t before;
    bool haveSnapshot;
    chest_silhouette::Snapshot snapshot; // its model, for the silhouette
};

using NextFn = bool (*)(void* iter, void* view, uint32_t size);
using SetFn = void (*)(void* renderer, const Span* handles, const Span* colours);
using InstanceFn = uint8_t* (*)(void* renderer, uint32_t handle, bool valid);
using EntityComponentFn = void** (*)(void* out16, void* registry, void* typeIndex, uint32_t entityId);

NextFn g_next = nullptr;
SetFn g_set = nullptr;
InstanceFn g_instance = nullptr;
EntityComponentFn g_entityComponent = nullptr;
System g_systems[2] = {};

SRWLOCK g_lock = SRWLOCK_INIT;
Target g_targets[kMax];
unsigned g_count = 0;
Applied g_applied[kMax];
unsigned g_appliedCount = 0;
volatile LONG g_pulsing = 0;

template <class T>
T Read(const void* p, size_t off = 0) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(p) + off, sizeof(value));
    return value;
}

bool InImage(const void* p, size_t length) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto at = reinterpret_cast<uintptr_t>(p);
    return at >= base && length <= nt->OptionalHeader.SizeOfImage && at - base <= nt->OptionalHeader.SizeOfImage - length;
}

bool Named(const Field& f, const char* text) {
    const size_t n = std::strlen(text);
    return f.length == n && InImage(f.text, n) && std::memcmp(f.text, text, n) == 0;
}

// The system's definition in .rdata: {name, length}, its function at +0x10, then four spans of fields at +0x38 (two
// read, two written); ModelComponent is among the written ones.
bool DiscoverSystem(const char* name, System& out) {
    out = {};
    out.fn = SystemFunction(name);
    if (!out.fn) return false;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (std::memcmp(sec->Name, ".rdata", 6) != 0) continue;
        for (size_t off = 0; off + 0x80 <= sec->Misc.VirtualSize; off += 8) {
            uint8_t* p = base + sec->VirtualAddress + off;
            if (!Named(Read<Field>(p), name) || Read<uint8_t*>(p, 0x10) != out.fn) continue;
            unsigned slot = 0;
            bool model = false;
            for (unsigned list = 0; list < 4; ++list) {
                auto span = Read<Span>(p, 0x38 + 0x10 * list);
                if (span.count > 32 || (span.count && !InImage(span.data, span.count * sizeof(Field)))) return false;
                for (unsigned i = 0; i < span.count; ++i, ++slot) {
                    if (Named(Read<Field>(span.data, i * sizeof(Field)), "ModelComponent")) {
                        if (model || list != 2) return false;
                        model = true;
                        out.modelSlot = slot;
                    }
                }
                (list < 2 ? out.reads : out.writes) += static_cast<unsigned>(span.count);
            }
            return model;
        }
    }
    return false;
}

// A component of an entity by id (the game's GetEntityComponent), or nullptr.
const void* Data(void* registry, uint16_t type, uint32_t entity) {
    if (!g_entityComponent || !registry) return nullptr;
    void* out[2] = {};
    return *g_entityComponent(out, registry, &type, entity);
}

void Set(void* renderer, uint32_t handle, uint8_t group) {
    Span handles{&handle, 1}, groups{&group, 1};
    g_set(renderer, &handles, &groups);
}

void Restore(unsigned i) {
    const Applied& a = g_applied[i];
    uint8_t* current = g_instance(a.renderer, a.handle, true);
    // A group another mod set (Quick Deposit's 6, Find's 7) is not put back: that mod restores its own, and once both
    // had lit the chest it stayed lit with neither.
    if (current == a.instance && current && current[0x78] == kGroup) Set(a.renderer, a.handle, a.before == 6 || a.before == 7 ? 0 : a.before);
    g_applied[i] = g_applied[--g_appliedCount];
}

bool SamePlace(void* registry, uint32_t entity, const float* position) {
    const auto* pos = static_cast<const int64_t*>(Data(registry, g_waterTypes.position, entity));
    if (!pos) return false;
    for (int j = 0; j < 3; ++j) {
        if (std::fabs(static_cast<float>(pos[j] / 4294967296.0) - position[j]) >= 0.15f) return false;
    }
    return true;
}

// At the end of a render system's iteration, on its own thread.
void Apply(void* iter, void* view, const System& system) {
    auto* state = Read<uint8_t*>(iter);
    if (!state || Read<uint16_t>(state, 0x4b8) != system.reads || Read<uint16_t>(state, 0x4ba) != system.writes) return;

    void* registry = Read<void*>(state, 0x30);
    uint8_t* record = state + 0x1c8 + system.modelSlot * 0x10;
    const uint16_t type = Read<uint16_t>(record, 0xa);
    if (type >= 1024 || Read<uint16_t>(record, 8) != 4) return;
    // Global resources come last in the view; these two layouts are 0xb8 and 0x58 bytes.
    const size_t viewSize = system.reads == 6 && system.writes == 2 ? 0xb8 : system.reads == 2 && system.writes == 1 ? 0x58 : 0;
    if (!viewSize) return;
    void* scene = Read<void*>(view, viewSize - 8);
    void* renderer = scene ? Read<void*>(scene, 0x98) : nullptr;
    if (!renderer) return;

    AcquireSRWLockExclusive(&g_lock);
    __try {
        const uint64_t now = GetTickCount64();
        uint32_t handles[kMax], colours[kMax];
        unsigned count = 0;
        bool active = false;
        for (unsigned i = 0; i < g_count; ++i) active |= g_targets[i].until > now;
        // Render jobs may run on views without a registry: the one whose entities sit where the chests are is the
        // render registry.
        for (int r = 0; active && !registry && r < kMaxWaterRegistries; ++r) {
            void* cand = g_waterRegistries[r].load(std::memory_order_relaxed);
            for (unsigned i = 0; cand && i < g_count && !registry; ++i) {
                const auto* model = static_cast<const uint32_t*>(Data(cand, type, g_targets[i].entity));
                if (model && *model && g_instance(renderer, *model, true) &&
                    SamePlace(cand, g_targets[i].entity, g_targets[i].position)) {
                    registry = cand;
                }
            }
        }
        for (unsigned i = 0; active && registry && i < g_count; ++i) {
            const Target& t = g_targets[i];
            if (t.until <= now) continue;
            const auto* model = static_cast<const uint32_t*>(Data(registry, type, t.entity));
            if (!model || !*model || !SamePlace(registry, t.entity, t.position) || !g_instance(renderer, *model, true)) continue;
            if (std::find(handles, handles + count, *model) == handles + count) {
                colours[count] = t.colour;
                handles[count++] = *model;
            }
        }
        for (unsigned i = 0; i < g_appliedCount;) {
            const Applied& a = g_applied[i];
            if (a.renderer == renderer && (!registry || a.registry == registry) &&
                std::find(handles, handles + count, a.handle) == handles + count) {
                Restore(i);
            } else {
                ++i;
            }
        }
        for (unsigned i = 0; i < count; ++i) {
            uint8_t* instance = g_instance(renderer, handles[i], true);
            if (!instance) continue;
            unsigned j = 0;
            while (j < g_appliedCount && !(g_applied[j].renderer == renderer && g_applied[j].handle == handles[i])) ++j;
            if (j == g_appliedCount) {
                if (j == kMax) continue;
                g_applied[g_appliedCount] = {renderer, registry, instance, handles[i], instance[0x78], false, {}};
                ++g_appliedCount;
            }
            if (g_applied[j].instance != instance) continue;
            // The game's interaction highlight, Quick Deposit's and Find's go first.
            if (instance[0x78] != 0 && instance[0x78] != kGroup) continue;
            // Silver chests get no outline highlight (group 7 is orange-gold): only their silver silhouette
            if (colours[i] != kSilver) Set(renderer, handles[i], kGroup);
            // Its model for the silhouette: resource (+0x108) -> asset (+0x38), mesh table, transform (+0x38)
            Applied& a = g_applied[j];
            a.haveSnapshot = false;
            auto* resource = Read<uint8_t*>(instance, 0x108);
            auto* asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
            if (asset && Read<uint32_t>(asset, 0x70) > 0 && Read<uint32_t>(asset, 0x70) < 256) {
                auto* mesh = asset + 0x6c + Read<int32_t>(asset, 0x6c);
                std::memcpy(a.snapshot.asset, asset, sizeof(a.snapshot.asset));
                std::memcpy(a.snapshot.mesh, mesh, sizeof(a.snapshot.mesh));
                std::memcpy(&a.snapshot.transform, instance + 0x38, sizeof(a.snapshot.transform));
                a.snapshot.colour = colours[i];
                a.haveSnapshot = true;
            }

        }
        if (!active) g_count = 0;
        InterlockedExchange(&g_pulsing, g_appliedCount ? 1 : 0);
    } __finally {
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void SafeApply(void* iter, void* view, const System& system) {
    __try {
        Apply(iter, view, system);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_pulsing, 0);
        static volatile LONG logged = 0;
        if (!InterlockedExchange(&logged, 1) && g_log) g_log("Chest glow: render layout check failed; chests are not lit");
    }
}

// Which render system is iterating is told by the iterator (Apply checks its read / write counts, 6/2 and 2/1), not
// by the return address: Quick Deposit redirects the same calls, and when the loader chains the two detours the second
// one's return address is the first detour.
bool NextHook(void* iter, void* view, uint32_t size) {
    greeneye::Lease lease; // an older generation after a hot reload only calls on
    const bool next = g_next(iter, view, size);
    if (!lease || next || g_modOff) return next;
    for (const System& system : g_systems) SafeApply(iter, view, system);
    return next;
}

void Install() {
    uint8_t* next = FindUnique("40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 30 08 00 00");
    uint8_t* setter = FindUnique("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 DB 4D 8B F0 48 8B FA "
                                 "48 8B F1 48 39 5A 08 76 46 66 66 0F 1F 84 00 00 00 00 00 48 8B 07 41 B0 01 48 8B CE 8B "
                                 "14 98 E8 ?? ?? ?? ?? 48 85 C0 74 1D 49 8B 0E 0F B6 14 0B 38 50 78");
    uint8_t* instance = FindUnique("85 D2 74 29 48 8B 81 C8 01 00 00 44 8B C2 41 81 E0 FF FF FF 00 C1 EA 18");
    uint8_t* entityComponent = FindUnique("40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC "
                                          "CC CC CC CC CC CC 4C 8B D1");
    if (!next || !setter || !instance || !entityComponent ||
        !DiscoverSystem("dynamic_render_model", g_systems[0]) || !DiscoverSystem("static_render_model", g_systems[1])) {
        if (g_log) g_log("Chest glow: game code not found (next %p, set %p, instance %p, component %p); GG outlines no chests",
                         next, setter, instance, entityComponent);
        return;
    }
    g_set = reinterpret_cast<SetFn>(setter);
    g_instance = reinterpret_cast<InstanceFn>(instance);
    g_entityComponent = reinterpret_cast<EntityComponentFn>(entityComponent);
    static const char* const kSystems[] = {"dynamic_render_model", "static_render_model"};
    int calls = 0, found = 0;
    g_next = reinterpret_cast<NextFn>(RedirectSystemCalls(next, kSystems, 2, reinterpret_cast<void*>(&NextHook), calls, found));
    const bool ok = g_next != nullptr;
    if (g_log) g_log(ok ? "Chest glow: ready (%d iterator calls in %d render systems)" : "Chest glow: redirect failed (%d calls, %d systems)",
                     calls, found);
    if (!ok) g_next = nullptr;
}

struct NearChest {
    float x, y, z;
    size_t chest;      // a chest (kChestSpawns), or kTotalChestCount
    size_t deposit;    // a lore note / boss (kOreDeposits), or kOreDepositCount
    uint32_t colour;
};
std::vector<NearChest> g_markers; // what GG lit (UI thread)
uint64_t g_markersStart = 0, g_markersUntil = 0;

bool TryDetectTypes(uint8_t* reg) {
    __try {
        return reg && DetectWaterTypesInner(reg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Targets: the client registry's entities standing where the chests stand.
void FillTargets(uint8_t* reg, const NearChest* chests, size_t chestCount, uint64_t until) {
    AcquireSRWLockExclusive(&g_lock);
    __try {
        g_count = 0;
        const uint16_t posType = g_waterTypes.position;
        uint8_t* map = reg + 0x148;
        const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
        auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
        auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
        auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
        for (uint64_t i = 0; used && ids && records && capacity <= (1u << 22) && i < capacity && g_count < kMax; ++i) {
            if (!(used[i >> 6] >> (i & 63) & 1) || ids[i] < 0x400 || !records[i]) continue;
            float p[3];
            if (!EcsPosition(records[i], posType, p)) continue;
            for (size_t c = 0; c < chestCount; ++c) {
                const NearChest& n = chests[c];
                const float tol = n.chest < kTotalChestCount ? 0.6f : 1.2f; // notes / bosses: whole metres
                if (std::fabs(p[0] - n.x) < tol && std::fabs(p[2] - n.z) < tol && std::fabs(p[1] - n.y) < tol + 0.6f) {
                    g_targets[g_count++] = {ids[i], {p[0], p[1], p[2]}, until, n.colour};
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_count = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// The nearby unlooted chests' entities (client registry, by their position) and positions; lights them.
void LightNearbyChests(uint8_t* client) {
    float px = 0.0f, pz = 0.0f;
    if (!GetHudPlayerWorldPos(client, px, pz)) return;
    // The nearby unlooted chests from the chest list
    std::vector<NearChest> nearChests;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        const bool gold = c < kGoldChestCount;
        if (IsChestCurrentlyCleared(c) || !(gold ? g_goldChestEnabled : g_silverChestEnabled)) continue;
        const ChestSpawn& sp = kChestSpawns[c];
        const float x = sp.px1 + sp.px0 / 65536.0f, y = sp.py1 + sp.py0 / 65536.0f, z = sp.pz1 + sp.pz0 / 65536.0f;
        if ((x - px) * (x - px) + (z - pz) * (z - pz) > kNearMeters * kNearMeters) continue;
        nearChests.push_back({x, y, z, c, kOreDepositCount, gold ? kFindGold : kSilver});
    }
    // Lore notes not read and bosses not defeated, while their rows are on
    for (size_t i = 0; i < kKnowledgeSpotCount; ++i) {
        const size_t d = kKnowledgeFirstDeposit + i;
        const OreDeposit& dep = kOreDeposits[d];
        if (!g_oreEnabled[dep.ore] || KnowledgeDone(d)) continue;
        if ((dep.x - px) * (dep.x - px) + (dep.z - pz) * (dep.z - pz) > kNearMeters * kNearMeters) continue;
        nearChests.push_back({float(dep.x), float(dep.topY), float(dep.z), kTotalChestCount, d, kFindGold});
    }
    g_markers = nearChests;
    g_markersStart = GetTickCount64();
    g_markersUntil = g_markersStart + kGlowMs;
    // The map and minimap pulse them (like Find's results)
    g_pulsingPlaces.clear();
    for (const auto& n : nearChests) g_pulsingPlaces.push_back({n.x, n.z});
    g_pulseStartTick = GetTickCount64();
    if (g_log) g_log("Chest glow: %zu chests, notes and bosses within %.0f m", nearChests.size(), kNearMeters);
    if (!g_next || nearChests.empty()) return;
    // Their entities: an entity of the client registry standing where a chest stands
    auto* reg = static_cast<uint8_t*>(g_waterRegistry.load(std::memory_order_relaxed));
    if (!reg || !g_waterTypes.position) {
        reg = nullptr;
        for (auto& slot : g_waterRegistries) {
            auto* cand = static_cast<uint8_t*>(slot.load(std::memory_order_relaxed));
            if (TryDetectTypes(cand) || (cand && g_waterTypes.registry == cand && g_waterTypes.position)) {
                reg = cand;
                break;
            }
        }
    }
    if (!reg || !g_waterTypes.position) return;
    FillTargets(reg, nearChests.data(), nearChests.size(), GetTickCount64() + kGlowMs);
    if (g_log) g_log("Chest glow: %u lit", g_count);
}

// G twice within half a second while playing (not in a menu, where G sorts the inventory).
void UpdateKey(uint8_t* client, bool appFocused) {
    static bool wasDown = false;
    static uint64_t firstTick = 0;

    const bool down = appFocused && (GetAsyncKeyState('G') & 0x8000) != 0;
    const bool edge = down && !wasDown;
    wasDown = down;
    if (!edge || !client || client[0x1cd8] != 1) return;
    const uint64_t now = GetTickCount64();
    if (firstTick && now - firstTick <= 500ULL) {
        firstTick = 0;
        LightNearbyChests(client);
        vein_sight::LightNearby(client); // and the veins of the Mining / Materials rows that are on
    } else {
        firstTick = now;
    }
}

// The lit chests on screen, through walls: a soft gold glow and a ring pulsing with the outline, the chest's icon in
// the middle, smaller with distance. Projected the way Find projects (the client's view / projection matrices and
// the UI window's viewport); behind the camera or looted since: not drawn.
void DrawMarkersInner(uint8_t* client, void* widgetRaw) {
    const uint64_t now = GetTickCount64();
    if (now >= g_markersUntil) g_markers.clear();
    auto* root = static_cast<uint8_t*>(GetUiG());
    uint8_t* window = root ? *reinterpret_cast<uint8_t**>(root + 0x10) : nullptr;
    if (!window) return;
    float view[16], proj[16], viewport[4];
    std::memcpy(view, client + 0x1d20, sizeof(view));
    std::memcpy(proj, client + 0x1d60, sizeof(proj));
    std::memcpy(viewport, window + 0x18, sizeof(viewport));
    if (viewport[2] < 1.0f || viewport[3] < 1.0f) return;
    vein_sight::Draw(client, widgetRaw, view, proj, viewport); // under the chests' rings
    if (g_markers.empty()) return;
    float px = 0.0f, pz = 0.0f;
    GetHudPlayerWorldPos(client, px, pz);
    const float elapsed = static_cast<float>(now - g_markersStart) * 0.001f;
    const float breathe = 0.5f - 0.5f * std::cos(std::fmod(elapsed, 1.2f) / 1.2f * 6.2831853f);
    const float fadeOut = std::min(1.0f, static_cast<float>(g_markersUntil - now) / 1500.0f);
    // Silhouettes of the lit models (snapshots taken by the render hook)
    chest_silhouette::Snapshot snaps[kMax];
    unsigned snapCount = 0;
    AcquireSRWLockShared(&g_lock);
    for (unsigned i = 0; i < g_appliedCount && snapCount < kMax; ++i) {
        if (g_applied[i].haveSnapshot) snaps[snapCount++] = g_applied[i].snapshot;
    }
    ReleaseSRWLockShared(&g_lock);
    chest_silhouette::Camera camera;
    std::memcpy(camera.view, view, sizeof(view));
    std::memcpy(camera.projection, proj, sizeof(proj));
    std::memcpy(camera.viewport, viewport, sizeof(viewport));
    bool drawn[kMax]{};
    chest_silhouette::Draw(widgetRaw, camera, snaps, snapCount, fadeOut, drawn);
    // A ring with its icon for what has no silhouette here (no model near it, or none decodable)
    auto hasSilhouette = [&](const NearChest& c) {
        for (unsigned i = 0; i < snapCount; ++i) {
            if (!drawn[i]) continue;
            const auto& t = snaps[i].transform;
            const float sx = float(t.position[0] / 4294967296.0), sz = float(t.position[2] / 4294967296.0);
            if (std::fabs(sx - c.x) < 1.5f && std::fabs(sz - c.z) < 1.5f) return true;
        }
        return false;
    };
    for (const NearChest& c : g_markers) {
        if (c.chest < kTotalChestCount ? IsChestCurrentlyCleared(c.chest) : KnowledgeDone(c.deposit)) continue;
        if (hasSilhouette(c)) continue;
        const float world[3] = {c.x, c.y + 0.6f, c.z};
        const float depth = world[0] * view[2] + world[1] * view[6] + world[2] * view[10] + view[14];
        if (!std::isfinite(depth) || depth <= 0.01f) continue;
        const float inv = 1.0f / depth;
        const float vx = inv * (world[0] * view[0] + world[1] * view[4] + world[2] * view[8] + view[12]);
        const float vy = inv * (world[0] * view[1] + world[1] * view[5] + world[2] * view[9] + view[13]);
        const float sx = ((vy * proj[4] + vx * proj[0] + proj[8] + proj[12]) * 0.5f + 0.5f) * viewport[2] + viewport[0];
        const float sy = (0.5f - (vy * proj[5] + vx * proj[1] + proj[9] + proj[13]) * 0.5f) * viewport[3] + viewport[1];
        if (!std::isfinite(sx) || !std::isfinite(sy)) continue;
        const float dist = std::sqrt((c.x - px) * (c.x - px) + (c.z - pz) * (c.z - pz));
        const float size = std::max(18.0f, std::min(40.0f, 40.0f * 12.0f / std::max(12.0f, dist)));
        const float glow = size * (1.8f + 0.5f * breathe), ring = size * (1.15f + 0.25f * breathe);
        const bool gold = c.chest < kGoldChestCount;
        const uint32_t tint = c.chest < kTotalChestCount ? (gold ? 0xff3cc8ffu : 0xffe8e4dcu) : 0xffffffffu;
        DrawAssetRectAbs(widgetRaw, kSoftGlowPrimitive, sx - glow * 0.5f, sy - glow * 0.5f, glow, glow, c.colour,
                         (0.35f + 0.35f * breathe) * fadeOut);
        DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring, 0xff5ca4c9u,
                         (0.55f + 0.45f * breathe) * fadeOut);
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - size * 0.5f, sy - size * 0.5f, size, size, 0xff0a0e10u,
                         0.6f * fadeOut);
        const void* icon = c.chest < kTotalChestCount ? GetIconRefForType(client, gold ? kGoldChestTypeHash : kSilverChestTypeHash)
                                                      : DepositIcon(c.deposit);
        if (icon) {
            DrawIconRectAbs(widgetRaw, icon, sx - size * 0.4f, sy - size * 0.4f, size * 0.8f, size * 0.8f, tint, fadeOut);
        }
    }
}

bool SafeDrawMarkersInner(uint8_t* client, void* widgetRaw) {
    __try {
        DrawMarkersInner(client, widgetRaw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Its own HUD layer while playing (the minimap's only exists while the minimap shows).
void DrawMarkers(uint8_t* client) {
    if ((g_markers.empty() && !vein_sight::Active()) || !client || client[0x1cd8] != 1) return;
    static const UiText kLayerName = MakeUiText("chestGlowLayer");
    alignas(16) uint8_t layer[0x400]{};
    g_beginLayer(layer, &kLayerName, 1, 2, -1, 0);
    Widget anchor{};
    g_namedWidget(anchor.raw, &kLayerName);
    g_opaque(anchor.raw);
    if (!SafeDrawMarkersInner(client, anchor.raw)) g_markers.clear();
    g_endWidget(anchor.raw);
    g_endLayer(layer);
}

} // namespace chest_glow

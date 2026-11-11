// Live creatures (Settings > Nearby creatures only): the creature entities of the client registries within the
// height-arrow range, each with its Map Key row. A client creature entity holds no template, name or resource GUID of
// its own, but its render model's asset in memory is the RenderModel resource blob as loaded: the FNV-1a 64 of its first
// 0x80 bytes names the model, and creature_templates.h the creatures with that model. The model handle only resolves on
// a render thread (chest_glow's render hook, ResolvePending there): the game thread queues the entities it has not seen
// yet, the render hook sets what each one is.

namespace creature_live {

struct LiveCreature {
    uint32_t entity;
    int registry;   // g_waterRegistries index
    float x, y, z;
    int16_t model;  // kCreatureModels index of the model's first entry (its other rows follow it)
    uint32_t slot;  // its slot in the registry's entity map (to read where it is now)
    uint8_t state = 0, awareness = 0; // an enemy's NetworkEnemy: keen::enemy01::State / AwarenessState (0: none)
};

// An enemy's NetworkEnemy component (8 bytes, enemies only, not wildlife): {State state u8, AwarenessState
// awarenessState u8, pad, float awareness 0..1}. State: 2 Idle, 5 Investigate, 6 Combat, 7 ReturnHome, 10 ReactToNoise,
// 11 Patrol, 14 ControlledByAttack, 19 Hit, 23 BlockProjectile, 26 Circling... (29 values); AwarenessState: 0 None,
// 1 Low, 2 High. Its type number is found by its data like the others: the 8-byte type all of whose values on the live
// creatures read as one, held by the most of them.
uint16_t g_enemyType[kMaxWaterRegistries] = {};
bool LooksLikeNetworkEnemy(const uint8_t* c) {
    float awareness;
    std::memcpy(&awareness, c + 4, 4);
    return c[0] >= 1 && c[0] <= 29 && c[1] <= 2 && c[2] == 0 && c[3] == 0 && awareness >= 0.0f && awareness <= 1.0f;
}

constexpr int16_t kNotCreature = -1, kUnresolved = -2;
struct CacheEntry {
    int16_t model = kUnresolved;
    uint16_t tries = 0;
    uint64_t seen = 0;
};
struct Pending {
    void* registry;
    uint32_t entity;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, CacheEntry> g_cache; // (registry index << 32 | entity) -> what it is; under g_lock
std::vector<Pending> g_pending;                    // under g_lock
std::atomic<bool> g_havePending{false};

uint64_t Key(int ri, uint32_t entity) { return static_cast<uint64_t>(ri) << 32 | entity; }

// The first kCreatureModels entry of a model, or kNotCreature.
int16_t FindModel(uint64_t asset) {
    size_t lo = 0, hi = sizeof(kCreatureModels) / sizeof(kCreatureModels[0]);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kCreatureModels[mid].asset < asset) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(kCreatureModels) / sizeof(kCreatureModels[0]) && kCreatureModels[lo].asset == asset
               ? static_cast<int16_t>(lo)
               : kNotCreature;
}

uint64_t Fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// The model's rows: the entries from `model` on with the same asset.
template <class F>
void ForEachRow(int16_t model, F&& f) {
    constexpr size_t n = sizeof(kCreatureModels) / sizeof(kCreatureModels[0]);
    for (size_t i = static_cast<size_t>(model); model >= 0 && i < n && kCreatureModels[i].asset == kCreatureModels[model].asset; ++i)
        f(kCreatureModels[i].row);
}

// Calls f(id, record) for every non-player entity of the registry.
template <class F>
void ForEachEntity(uint8_t* reg, F&& f) {
    uint8_t* map = reg + 0x148;
    const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
    auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
    auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
    auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
    if (!used || !ids || !records || capacity > (1u << 22)) return;
    for (uint64_t i = 0; i < capacity; ++i) {
        if (!(used[i >> 6] >> (i & 63) & 1) || ids[i] < 0x400 || !records[i]) continue;
        f(ids[i], records[i]);
    }
}

// The registry's NetworkEnemy type, from the live creatures found so far (at least three holding it, every value a
// plausible one).
void DetectEnemyType(int ri, uint8_t** records, std::vector<LiveCreature>& out) {
    static int has[kEcsTypeBits], ok[kEcsTypeBits];
    std::memset(has, 0, sizeof(has));
    std::memset(ok, 0, sizeof(ok));
    for (const LiveCreature& c : out) {
        if (c.registry != ri) continue;
        uint8_t* rec = records[c.slot];
        const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
        for (int ty = 1; ty < kEcsTypeBits; ++ty) {
            if (!(bits[ty >> 6] >> (ty & 63) & 1)) continue;
            uint16_t size = 0;
            const uint8_t* d = EcsComponent(rec, static_cast<uint16_t>(ty), &size);
            if (!d || size != 8) continue;
            ++has[ty];
            if (LooksLikeNetworkEnemy(d)) ++ok[ty];
        }
    }
    int best = 0;
    for (int ty = 1; ty < kEcsTypeBits; ++ty)
        if (has[ty] >= 3 && ok[ty] == has[ty] && ok[ty] > ok[best]) best = ty;
    if (!best) return;
    g_enemyType[ri] = static_cast<uint16_t>(best);
    if (g_log) g_log("Live creatures: registry %d enemy state type %d (%d creatures)", ri, best, ok[best]);
}

// Game thread, g_lock held: the known creatures within range; the entities not seen yet are queued for the render hook.
void ScanInner(std::vector<LiveCreature>& out, float px, float pz, float range, uint64_t now) {
    for (int ri = 0; ri < kMaxWaterRegistries; ++ri) {
        auto* reg = static_cast<uint8_t*>(g_waterRegistries[ri].load(std::memory_order_relaxed));
        if (!reg) continue;
        boss_live::RegistryTypes& bt = boss_live::g_types[ri];
        if (!bt.position) boss_live::DetectTypes(reg, bt, px, pz);
        if (!bt.position) continue;
        uint8_t* map = reg + 0x148;
        const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
        auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
        auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
        auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
        if (!used || !ids || !records || capacity > (1u << 22)) continue;
        for (uint64_t i = 0; i < capacity; ++i) {
            if (!(used[i >> 6] >> (i & 63) & 1) || ids[i] < 0x400 || !records[i]) continue;
            float p[3];
            if (!EcsPosition(records[i], bt.position, p) || (p[0] - px) * (p[0] - px) + (p[2] - pz) * (p[2] - pz) > range * range)
                continue;
            CacheEntry& e = g_cache[Key(ri, ids[i])];
            e.seen = now;
            if (e.model >= 0) {
                if (out.size() >= 256) continue;
                LiveCreature c{ids[i], ri, p[0], p[1], p[2], e.model, static_cast<uint32_t>(i)};
                uint16_t size = 0;
                const uint8_t* enemy = g_enemyType[ri] ? EcsComponent(records[i], g_enemyType[ri], &size) : nullptr;
                if (enemy && size == 8) c.state = enemy[0], c.awareness = enemy[1];
                out.push_back(c);
            } else if (e.model == kUnresolved) {
                // not drawn yet (a model handle without an instance): asked again for a while, then given up on
                if (++e.tries > 600) e.model = kNotCreature;
                else if (g_pending.size() < 2048) g_pending.push_back({reg, ids[i]});
            }
        }
        if (!g_enemyType[ri]) DetectEnemyType(ri, records, out);
    }
    // what left the range a while ago is forgotten (its id may come back as another entity)
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (now - it->second.seen > 20000ULL) it = g_cache.erase(it);
        else ++it;
    }
}

bool ScanSafe(std::vector<LiveCreature>& out, float px, float pz, float range, uint64_t now) {
    __try {
        ScanInner(out, px, pz, range, now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The live creatures within range of (px, pz), each with its model (rows). Game thread.
bool Scan(std::vector<LiveCreature>& out, float px, float pz, float range) {
    const uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    g_pending.clear();
    const bool ok = ScanSafe(out, px, pz, range, now);
    if (!ok) out.clear();
    g_havePending.store(!g_pending.empty(), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

// Where the creature is now: false once it is gone (its slot freed or another entity's).
bool PositionInner(const LiveCreature& c, float out[3]) {
    auto* reg = static_cast<uint8_t*>(g_waterRegistries[c.registry].load(std::memory_order_relaxed));
    const uint16_t posType = boss_live::g_types[c.registry].position;
    if (!reg || !posType) return false;
    uint8_t* map = reg + 0x148;
    const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
    auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
    auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
    auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
    if (!used || !ids || !records || c.slot >= capacity || !(used[c.slot >> 6] >> (c.slot & 63) & 1) ||
        ids[c.slot] != c.entity || !records[c.slot])
        return false;
    return EcsPosition(records[c.slot], posType, out) && PlausibleWorldPos(out);
}
bool Position(const LiveCreature& c, float out[3]) {
    __try {
        return PositionInner(c, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The first of the creature's rows that is on (Map Key), or -1.
int ShownRow(int16_t model) {
    int shown = -1;
    ForEachRow(model, [&](uint16_t r) {
        if (shown < 0 && g_oreEnabled[r]) shown = r;
    });
    return shown;
}

// The live creatures near the player for the minimap (game thread): looked for four times a second, where they are
// read every frame.
std::vector<LiveCreature> g_near;
#ifdef UMM_DEV_BRIDGE
void DevWatch(const std::vector<LiveCreature>& live, float px, float pz);
#endif
const std::vector<LiveCreature>& Nearby(float px, float pz, float range) {
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last >= 250ULL) {
        last = now;
        g_near.clear();
        Scan(g_near, px, pz, range);
#ifdef UMM_DEV_BRIDGE
        DevWatch(g_near, px, pz);
#endif
    } else {
        size_t kept = 0;
        for (const LiveCreature& c : g_near) {
            float p[3];
            if (!Position(c, p)) continue;
            g_near[kept] = c;
            g_near[kept].x = p[0], g_near[kept].y = p[1], g_near[kept].z = p[2];
            ++kept;
        }
        g_near.resize(kept);
    }
    return g_near;
}

// How much the creature is onto you: 2 fighting (Combat, Hit, Circling, ControlledByAttack, BlockProjectile), 1 alerted
// (Investigate, ReactToNoise, or awareness High), 0 not (or not an enemy). In multiplayer the fight may be with another
// player: the client is not told whom it is after.
int Aggro(const LiveCreature& c) {
    switch (c.state) {
        case 6: case 14: case 19: case 23: case 26: return 2;
        case 5: case 10: return 1;
        default: return c.awareness == 2 && c.state != 3 ? 1 : 0;
    }
}

} // namespace creature_live

#ifdef UMM_DEV_BRIDGE
namespace creature_live {
// dev (bridge op map creatures value "watch" / "unwatch"): with the watch on, every rescan logs the changes in the live
// creatures' components of up to 24 bytes within 30 m (a type that changed more than 60 times is muted: positions,
// timers); for finding the one that says a creature is after you.
bool g_watch = true; // on in dev builds, so a hot reload keeps it
void DevWatchInner(const std::vector<LiveCreature>& live, float px, float pz) {
    static std::unordered_map<uint64_t, std::vector<uint8_t>> last; // (entity << 16 | type) -> bytes
    static int changes[kEcsTypeBits];
    for (const LiveCreature& c : live) {
        if ((c.x - px) * (c.x - px) + (c.z - pz) * (c.z - pz) > 30.0f * 30.0f) continue;
        auto* reg = static_cast<uint8_t*>(g_waterRegistries[c.registry].load(std::memory_order_relaxed));
        auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
        uint8_t* rec = records[c.slot];
        const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
        for (int ty = 0; ty < kEcsTypeBits; ++ty) {
            if (!(bits[ty >> 6] >> (ty & 63) & 1) || ty == boss_live::g_types[c.registry].position) continue;
            uint16_t size = 0;
            const uint8_t* d = EcsComponent(rec, static_cast<uint16_t>(ty), &size);
            if (!d || !size || size > 24) continue;
            auto& prev = last[static_cast<uint64_t>(c.entity) << 16 | static_cast<uint64_t>(ty)];
            if (prev.size() == size && std::memcmp(prev.data(), d, size) == 0) continue;
            if (!prev.empty() && changes[ty]++ < 60 && g_log) {
                char a[64] = {}, b[64] = {};
                for (uint16_t k = 0; k < size; ++k) std::snprintf(a + 2 * k, 3, "%02x", prev[k]), std::snprintf(b + 2 * k, 3, "%02x", d[k]);
                g_log("Watch %x %s (%.0f m): type %d %s -> %s", c.entity, OreName(kCreatureModels[c.model].row),
                      std::hypot(c.x - px, c.z - pz), ty, a, b);
            }
            prev.assign(d, d + size);
        }
    }
}
void DevWatch(const std::vector<LiveCreature>& live, float px, float pz) {
    if (!g_watch) return;
    __try {
        DevWatchInner(live, px, pz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
} // namespace creature_live
#endif
